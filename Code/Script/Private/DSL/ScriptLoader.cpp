module Script;

import Core;
import Core.Log;
import File; // all disk access goes through FileSystem
import :DSL;
import :ScriptBindings;
import :ScriptLang;
import :ScriptLoader;

// The .dsl parser: the exact inverse of ScriptLang.cpp's renderSymbol/Syntax::format. Every statement shape it
// accepts is one the renderer emits (expanded view); the grammar mirrors are kept side by side per construct so
// a renderer change has one obvious place to land here too. Expression structure is rebuilt the way the editor
// authors it (see DSL.ixx): operators split by CLASS level -- logical (&&/||) lowest, then at most ONE
// comparison, then one FLAT arithmetic chain -- with explicit parentheses as the only nesting, preserved
// verbatim as `grouped` chains (a lone "(a)" and doubled "((x))" both round-trip). Constants adopt the type of
// the slot they sit in (a declaration's declared type, a parameter's type, the comparison's other side),
// matching what the editor's compose flows would have produced in the same position.

namespace
{
	using ST = DSLSymbol::SymbolType;

	// Every DSL line is prefixed "//@" (not a bare "//") so ordinary C++ comments in the future transpiled
	// output can never be mistaken for DSL content; the block markers double the '@' ("//@@dsl"/"//@@end")
	// since a DSL `end` line itself serializes as "//@end".
	constexpr const char* kBlockEnd = "//@@end";

	// ---- tokens ----

	struct Token
	{
		enum class Kind { Identifier, Number, String, Symbol };
		Kind kind;
		std::string text; // String: the literal's CONTENT, quotes already stripped
	};

	bool isIdentChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
	}

	// One source line (indentation already stripped, never a '#' comment) -> tokens. False = a character that
	// can't appear in rendered DSL text, with `error` set.
	bool tokenizeLine(const std::string& text, std::vector<Token>& out, std::string& error)
	{
		static const char* const kMultiChar[] = { "->", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=" };

		size_t i = 0;
		while (i < text.size())
		{
			const char c = text[i];
			if (c == ' ' || c == '\t')
			{
				++i;
				continue;
			}
			if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_')
			{
				size_t j = i + 1;
				while (j < text.size() && isIdentChar(text[j]))
					++j;
				// A trailing "[]" belongs to the identifier: an ARRAY type reads as one word ("int[]"), so
				// typeFromKeyword's suffix check is the only place that knows about brackets and nothing else
				// in the grammar has to tokenize them. They appear nowhere else -- there's no index syntax.
				if (j + 1 < text.size() && text[j] == '[' && text[j + 1] == ']')
					j += 2;
				out.push_back({ Token::Kind::Identifier, text.substr(i, j - i) });
				i = j;
				continue;
			}
			// A '.' starts a number only in value position -- never right after a name or ')', where it's
			// member access ("self.pos" vs a bare ".5" literal).
			const bool dotStartsNumber = c == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0
				&& (out.empty() || (out.back().kind == Token::Kind::Symbol && out.back().text != ")"));
			if (std::isdigit(static_cast<unsigned char>(c)) != 0 || dotStartsNumber)
			{
				size_t j = i + 1;
				while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) != 0 || text[j] == '.'))
					++j;
				out.push_back({ Token::Kind::Number, text.substr(i, j - i) });
				i = j;
				continue;
			}
			if (c == '"')
			{
				const size_t close = text.find('"', i + 1);
				if (close == std::string::npos)
				{
					error = "unterminated string literal";
					return false;
				}
				out.push_back({ Token::Kind::String, text.substr(i + 1, close - i - 1) });
				i = close + 1;
				continue;
			}
			bool matchedMulti = false;
			for (const char* multi : kMultiChar)
				if (text.compare(i, 2, multi) == 0)
				{
					out.push_back({ Token::Kind::Symbol, multi });
					i += 2;
					matchedMulti = true;
					break;
				}
			if (matchedMulti)
				continue;
			if (std::string_view("()=<>+-*/%,.").find(c) != std::string_view::npos)
			{
				out.push_back({ Token::Kind::Symbol, std::string(1, c) });
				++i;
				continue;
			}
			error = std::string("unexpected character '") + c + "'";
			return false;
		}
		return true;
	}

	// ---- vocabulary lookups (single source of truth: dslTypeName / dslOperatorText) ----

	bool typeFromKeyword(const std::string& word, DSLType& out)
	{
		// "int[]" and friends: an ARRAY of whatever precedes the brackets. Written as one word rather than
		// three tokens so nothing else in the grammar has to know about brackets (they aren't tokenized at all).
		if (word.size() > 2 && word.compare(word.size() - 2, 2, "[]") == 0)
		{
			DSLType elementType = DSLType::Void;
			if (!typeFromKeyword(word.substr(0, word.size() - 2), elementType) || dslIsArrayType(elementType))
				return false; // unknown element, or an attempt to nest -- arrays are one level only
			out = dslArrayOf(elementType);
			return true;
		}
		for (DSLType t : { DSLType::Int, DSLType::Float, DSLType::Bool, DSLType::String })
			if (word == dslTypeName(t))
			{
				out = t;
				return true;
			}
		// Entity is not a DECLARABLE type on its own, but it is a valid array element and a foreach element.
		if (word == dslTypeName(DSLType::Entity))
		{
			out = DSLType::Entity;
			return true;
		}
		if (const DSLType structType = Globals::scriptBindings.typeByName(word); structType != DSLType::Void)
		{
			out = structType;
			return true;
		}
		return false;
	}

	bool operatorFromText(const std::string& text, DSLOperator& out)
	{
		for (DSLOperator op : { DSLOperator::Assign, DSLOperator::AssignAdd, DSLOperator::AssignSubtract, DSLOperator::AssignMultiply,
			DSLOperator::AssignDivide, DSLOperator::AssignModulus, DSLOperator::Add, DSLOperator::Subtract, DSLOperator::Multiply,
			DSLOperator::Divide, DSLOperator::Modulus, DSLOperator::Equal, DSLOperator::NotEqual, DSLOperator::LessThan,
			DSLOperator::GreaterThan, DSLOperator::LessThanOrEqual, DSLOperator::GreaterThanOrEqual, DSLOperator::And, DSLOperator::Or })
			if (text == dslOperatorText(op))
			{
				out = op;
				return true;
			}
		return false;
	}

	// ---- token-span helpers ----

	// Whether `tok` can END a term -- what makes a following +/- a binary operator instead of a literal's sign
	// ("a - 1" splits, the "-1" in "vec3(0, -1, 0)" doesn't).
	bool endsTerm(const Token& tok)
	{
		return tok.kind != Token::Kind::Symbol || tok.text == ")";
	}

	// Index of the ')' matching the '(' at `open`, or -1.
	int matchParen(std::span<const Token> t, int open)
	{
		int depth = 0;
		for (int i = open; i < static_cast<int>(t.size()); ++i)
		{
			if (t[i].kind != Token::Kind::Symbol)
				continue;
			if (t[i].text == "(")
				++depth;
			else if (t[i].text == ")" && --depth == 0)
				return i;
		}
		return -1;
	}

	// Depth-0 positions of operator tokens matching `accept`, in BINARY position (a term ended just before).
	std::vector<int> operatorSplits(std::span<const Token> t, auto&& accept)
	{
		std::vector<int> out;
		int depth = 0;
		for (int i = 0; i < static_cast<int>(t.size()); ++i)
		{
			if (t[i].kind != Token::Kind::Symbol)
				continue;
			if (t[i].text == "(") { ++depth; continue; }
			if (t[i].text == ")") { --depth; continue; }
			if (depth == 0 && i > 0 && endsTerm(t[i - 1]) && accept(t[i].text))
				out.push_back(i);
		}
		return out;
	}

	// Depth-0 comma splits -- call-argument and for-clause separators. Always yields at least one (possibly
	// empty) part; callers that mean "no parts at all" check for an empty input span first.
	std::vector<std::span<const Token>> splitAtCommas(std::span<const Token> t)
	{
		std::vector<std::span<const Token>> out;
		int depth = 0;
		size_t start = 0;
		for (size_t i = 0; i < t.size(); ++i)
		{
			if (t[i].kind != Token::Kind::Symbol)
				continue;
			if (t[i].text == "(")
				++depth;
			else if (t[i].text == ")")
				--depth;
			else if (t[i].text == "," && depth == 0)
			{
				out.push_back(t.subspan(start, i - start));
				start = i + 1;
			}
		}
		out.push_back(t.subspan(start));
		return out;
	}

	// ---- the parser ----

	struct Parser
	{
		const std::vector<std::unique_ptr<DSLSymbol>>& sidebar;
		const std::vector<std::unique_ptr<DSLSymbol>>& builtins;
		const ScriptBindings& bindings;
		const std::vector<DSLType>& requiredComponents; // the FILE's "//@@require" set, parsed before any line
		const std::vector<DSLDataField>& dataFields;             // the FILE's "//@@data" fields, parsed before any line
		const std::vector<std::string>& eventNames;              // the FILE's "//@@event" names, parsed before any line

		std::vector<std::unique_ptr<DSLCodeLine>> outLines;
		std::vector<DSLSymbol*> userFunctions;  // pass-1 FunctionDeclaration heads, document order
		std::vector<DSLSymbol*> scopeVars;      // the current function's parameters + locals so far (reset per function)
		// Names bound by an enclosing `ifexist` that are no longer READABLE (an else branch pruned them
		// from scopeVars) but must still not be redeclared: the generated C++ scopes the condition
		// declaration across the whole if/else, so a redeclaration there would silently shadow it.
		std::vector<std::string> reservedNames;
		// The open block headers enclosing the line being parsed -- owned here (not by the load driver) so
		// statement parsers can ask what they're nested inside; see checkContainerNotIterated.
		std::vector<DSLSymbol*> openBlocks;

		DSLType currentReturnType = DSLType::Void;
		std::string error;                      // first failure -- everything bails out through fail()/failValue()

		bool fail(std::string what)
		{
			if (error.empty())
				error = std::move(what);
			return false;
		}

		DSLSymbol* failValue(std::string what)
		{
			fail(std::move(what));
			return nullptr;
		}

		// Same construction as ScriptEditor::pushSymbol: the symbol is owned by `line`, appended in creation
		// order -- children before parents, which is exactly the post-order convention (see DSL.ixx).
		DSLSymbol* push(DSLCodeLine& line, ST type, DSLSymbol::Data data)
		{
			auto symbol = std::make_unique<DSLSymbol>();
			symbol->type = type;
			symbol->data = std::move(data);
			symbol->line = &line;
			DSLSymbol* ptr = symbol.get();
			line.symbols.push_back(std::move(symbol));
			return ptr;
		}

		bool isReservedInChain(const std::string& name) const
		{
			for (const std::string& reserved : reservedNames)
				if (reserved == name)
					return true;
			return false;
		}

		DSLSymbol* findVariable(const std::string& name) const
		{
			for (auto it = scopeVars.rbegin(); it != scopeVars.rend(); ++it)
				if (std::get<DSLSymbol::VariableDeclaration>((*it)->data).name == name)
					return *it;
			for (const std::unique_ptr<DSLSymbol>& s : sidebar)
				if (s->type == ST::VariableDeclaration && std::get<DSLSymbol::VariableDeclaration>(s->data).name == name)
					return s.get();
			return nullptr;
		}

		const DSLDataField* findDataField(const std::string& name) const { return dslFindDataField(dataFields, name); }
		int findEventIndex(const std::string& name) const { return dslFindEventIndex(eventNames, name); }

		DSLSymbol* findFunction(const std::string& name, bool requiresReceiver) const
		{
			if (!requiresReceiver) // user functions are never receiver-based
				for (DSLSymbol* func : userFunctions)
					if (std::get<DSLSymbol::FunctionDeclaration>(func->data).name == name)
						return func;
			for (const std::unique_ptr<DSLSymbol>& s : builtins)
				if (s->type == ST::FunctionDeclaration
					&& std::get<DSLSymbol::FunctionDeclaration>(s->data).requiresReceiver == requiresReceiver
					&& std::get<DSLSymbol::FunctionDeclaration>(s->data).name == name)
					return s.get();
			return nullptr;
		}

		static DSLType declaredType(const DSLSymbol* varDecl)
		{
			const DSLSymbol::VariableDeclaration& decl = std::get<DSLSymbol::VariableDeclaration>(varDecl->data);
			return std::get<DSLSymbol::TypeDeclaration>(decl.typeSymbol->data).type;
		}

		// A dotted call's callee, resolved against the RECEIVER's own registry side -- a binding object's
		// functions or a struct type's member functions -- never a global name scan.
		DSLSymbol* findReceiverFunction(DSLType receiverType, const std::string& name) const
		{
			if (const BindingObject* object = bindings.objectFor(receiverType); object != nullptr)
			{
				const std::span<DSLSymbol* const> symbols = bindings.functionSymbols(*object);
				for (size_t i = 0; i < object->functions.size(); ++i)
					if (name == object->functions[i].name)
						return symbols[i];
			}
			if (const BindingStruct* structDef = bindings.structFor(receiverType); structDef != nullptr)
			{
				const std::span<DSLSymbol* const> symbols = bindings.structFunctionSymbols(receiverType);
				for (size_t i = 0; i < structDef->functions.size(); ++i)
					if (name == structDef->functions[i].name)
						return symbols[i];
			}
			return nullptr;
		}

		// A reference to a component-bound member (self.physics and friends) is the HOST-CACHED pointer for this
		// script's own entity, so it's legal only directly on `self` (a sidebar binding, reached with no member
		// path walked -- `self.parent.physics` and `e.physics` are other entities and would silently read self's
		// component instead), and only when the file's "//@@require" set names it. The loader-side twin of
		// receiverCandidates' editor gating; `ifexist` is how another entity's component is reached.
		bool checkMemberUsable(const std::string& ownerName, const std::string& memberName, const BindingMember& member,
			const DSLSymbol* rootDecl, bool receiverIsRoot)
		{
			// A self-only member (see BindingMember::selfOnly) belongs to the SCRIPT, not to whichever entity it
			// was read through -- its emit ignores the receiver, so reaching it off another entity would
			// silently resolve to this script's own. Only `self` itself, with no member path walked.
			if (member.selfOnly && (rootDecl == nullptr || rootDecl->line != nullptr || !receiverIsRoot))
				return fail("'" + ownerName + "." + memberName + "' belongs to this script's own entity"
					+ (member.requiredComponent != DSLType::Void
						? std::string(" -- use 'ifexist' to reach another entity's") : std::string()));
			if (member.requiredComponent != DSLType::Void
				&& std::find(requiredComponents.begin(), requiredComponents.end(), member.requiredComponent) == requiredComponents.end())
				return fail("'" + ownerName + "." + memberName + "' is used but its component isn't in the //@@require line");
			return true;
		}

		// A numeric literal adopts the slot's expected type when it IS numeric -- the same Constant the editor's
		// compose flow would have produced there ("force = 1" makes a Float "1") -- else its own shape decides.
		DSLSymbol* buildNumberConstant(const std::string& text, DSLType expected, DSLCodeLine& line)
		{
			const DSLType type = (expected == DSLType::Int || expected == DSLType::Float)
				? expected
				: (text.find('.') != std::string::npos ? DSLType::Float : DSLType::Int);
			if (!AutoCompleteRules::isValidLiteralText(type, text))
				return failValue("'" + text + "' is not a valid " + dslTypeName(type) + " literal");
			return push(line, ST::Constant, DSLSymbol::Constant{ type, text });
		}

		// Expression grammar, split by operator CLASS (mirroring how the editor authors chains -- see the file
		// comment): parseExpression = logical level, parseComparison = at most one comparison, parseArithmetic =
		// one flat chain, parseTerm = a single value (literal/variable/call/member/parenthesized group).

		DSLSymbol* parseExpression(std::span<const Token> t, DSLCodeLine& line, DSLType expected)
		{
			if (t.empty())
				return failValue("expected a value");
			const std::vector<int> splits = operatorSplits(t, [](const std::string& s) { return s == "&&" || s == "||"; });
			if (splits.empty())
				return parseComparison(t, line, expected);

			std::vector<DSLSymbol*> operands;
			std::vector<DSLOperator> ops;
			int start = 0;
			for (size_t i = 0; i <= splits.size(); ++i)
			{
				const int end = (i < splits.size()) ? splits[i] : static_cast<int>(t.size());
				DSLSymbol* operand = parseComparison(t.subspan(start, end - start), line, DSLType::Bool);
				if (operand == nullptr)
					return nullptr;
				operands.push_back(operand);
				if (i < splits.size())
				{
					ops.push_back(t[splits[i]].text == "&&" ? DSLOperator::And : DSLOperator::Or);
					start = splits[i] + 1;
				}
			}
			return push(line, ST::Expression, DSLSymbol::Expression{ std::move(operands), std::move(ops) });
		}

		DSLSymbol* parseComparison(std::span<const Token> t, DSLCodeLine& line, DSLType expected)
		{
			const std::vector<int> splits = operatorSplits(t, [](const std::string& s)
				{ return s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" || s == ">="; });
			if (splits.empty())
				return parseArithmetic(t, line, expected);
			if (splits.size() > 1)
				return failValue("chained comparisons need parentheses");

			// The left side fixes the type the right side composes against -- same order the editor's
			// ConditionLeft/ConditionRight staging resolves them in.
			DSLSymbol* left = parseArithmetic(t.subspan(0, splits[0]), line, DSLType::Void);
			if (left == nullptr)
				return nullptr;
			const DSLType leftType = dslValueType(left);
			DSLSymbol* right = parseArithmetic(t.subspan(splits[0] + 1), line, leftType);
			if (right == nullptr)
				return nullptr;
			DSLOperator op = DSLOperator::Equal;
			operatorFromText(t[splits[0]].text, op);
			// Engine objects compare by identity / against null only -- the loader-side twin of the editor
			// narrowing its comparison-operator list (AutoCompleteRules::isEqualityOnlyType).
			if (AutoCompleteRules::isEqualityOnlyType(leftType) && op != DSLOperator::Equal && op != DSLOperator::NotEqual)
				return failValue("'" + std::string(dslOperatorText(op)) + "' can't compare " + std::string(dslTypeName(leftType)) + " values");
			return push(line, ST::Expression, DSLSymbol::Expression{ { left, right }, { op } });
		}

		DSLSymbol* parseArithmetic(std::span<const Token> t, DSLCodeLine& line, DSLType expected)
		{
			const std::vector<int> splits = operatorSplits(t, [](const std::string& s)
				{ return s == "+" || s == "-" || s == "*" || s == "/" || s == "%"; });
			if (splits.empty())
				return parseTerm(t, line, expected);

			std::vector<DSLSymbol*> operands;
			std::vector<DSLOperator> ops;
			DSLType elementType = expected;
			int start = 0;
			for (size_t i = 0; i <= splits.size(); ++i)
			{
				const int end = (i < splits.size()) ? splits[i] : static_cast<int>(t.size());
				DSLSymbol* operand = parseTerm(t.subspan(start, end - start), line, elementType);
				if (operand == nullptr)
					return nullptr;
				operands.push_back(operand);
				if (elementType == DSLType::Void)
					elementType = dslValueType(operand); // a chain shares one element type -- later literals adopt it
				if (i < splits.size())
				{
					DSLOperator op = DSLOperator::Add;
					operatorFromText(t[splits[i]].text, op);
					ops.push_back(op);
					start = splits[i] + 1;
				}
			}
			return push(line, ST::Expression, DSLSymbol::Expression{ std::move(operands), std::move(ops) });
		}

		DSLSymbol* parseTerm(std::span<const Token> t, DSLCodeLine& line, DSLType expected)
		{
			if (t.empty())
				return failValue("expected a value");

			const Token& first = t[0];

			// "(...)" covering the whole span: an authored group, preserved VERBATIM as a `grouped` chain -- a
			// lone "(a)" wraps into a one-operand group and "((x))" into two nested ones, never stripped as
			// redundant (they're the edit anchors later chain edits hang off, see DSL.ixx).
			if (first.kind == Token::Kind::Symbol && first.text == "(" && matchParen(t, 0) == static_cast<int>(t.size()) - 1)
			{
				DSLSymbol* inner = parseExpression(t.subspan(1, t.size() - 2), line, expected);
				if (inner == nullptr)
					return nullptr;
				if (inner->type == ST::Expression && !std::get<DSLSymbol::Expression>(inner->data).grouped)
				{
					std::get<DSLSymbol::Expression>(inner->data).grouped = true;
					return inner;
				}
				return push(line, ST::Expression, DSLSymbol::Expression{ { inner }, {}, /*grouped*/ true });
			}

			// Signed numeric literal -- the sign character is part of the constant's stored value ("-1").
			if (first.kind == Token::Kind::Symbol && (first.text == "-" || first.text == "+")
				&& t.size() == 2 && t[1].kind == Token::Kind::Number)
				return buildNumberConstant(first.text + t[1].text, expected, line);

			if (t.size() == 1)
			{
				if (first.kind == Token::Kind::Number)
					return buildNumberConstant(first.text, expected, line);
				if (first.kind == Token::Kind::String)
					return push(line, ST::Constant, DSLSymbol::Constant{ DSLType::String, first.text });
				if (first.kind == Token::Kind::Identifier)
				{
					if (first.text == "true" || first.text == "false")
						return push(line, ST::Constant, DSLSymbol::Constant{ DSLType::Bool, first.text });
					DSLSymbol* decl = findVariable(first.text);
					if (decl == nullptr)
						return failValue("unknown identifier '" + first.text + "'");
					// A SIDEBAR binding (physics/audio/...) has no bare value form -- only dotting into it. Two
					// things do have one: any LOCAL of an engine-object type (a foreach's Entity element, an
					// ifexist binding), and `self`, which is a genuine Entity value. Told apart by the
					// owning line + the type, same as the editor's candidate lists.
					if (dslIsEngineObjectType(declaredType(decl)) && decl->line == nullptr
						&& declaredType(decl) != DSLType::Entity)
						return failValue("'" + first.text + "' is a binding object -- it can only be dotted into");
					return push(line, ST::VariableReference, DSLSymbol::VariableReference{ decl });
				}
				return failValue("expected a value, got '" + first.text + "'");
			}

			if (first.kind != Token::Kind::Identifier)
				return failValue("expected a value, got '" + first.text + "'");

			// receiver.member(.member...)  /  receiver(.member...).method(...)
			if (t[1].kind == Token::Kind::Symbol && t[1].text == ".")
			{
				DSLSymbol* receiverDecl = findVariable(first.text);
				if (receiverDecl == nullptr)
					return failValue("unknown identifier '" + first.text + "'");
				DSLType receiverType = declaredType(receiverDecl);
				DSLSymbol* receiver = push(line, ST::VariableReference, DSLSymbol::VariableReference{ receiverDecl });
				std::string ownerName = first.text;
				size_t i = 1; // at each loop head t[i] is the '.'
				while (true)
				{
					if (i + 1 >= t.size() || t[i + 1].kind != Token::Kind::Identifier)
						return failValue("expected a member name after '" + ownerName + ".'");
					const std::string memberName = t[i + 1].text;
					// "name(" terminates the chain as a method call on everything walked so far.
					if (i + 2 < t.size() && t[i + 2].kind == Token::Kind::Symbol && t[i + 2].text == "(")
					{
						if (matchParen(t, static_cast<int>(i + 2)) != static_cast<int>(t.size()) - 1)
							return failValue("unexpected tokens after '" + ownerName + "." + memberName + "(...)'");
						DSLSymbol* func = findReceiverFunction(receiverType, memberName);
						if (func == nullptr)
							return failValue("'" + ownerName + "' has no function '" + memberName + "'");
						return parseCall(t.subspan(i + 3, t.size() - (i + 3) - 1), line, func, receiver);
					}
					// A member hop -- its value type stamps from the registry (see MemberAccess in DSL.ixx), or
					// from this document's OWN fields/events when dotting through self.data/self.events
					// (DSLType::ScriptData/ScriptEvents).
					DSLType memberType;
					if (receiverType == DSLType::ScriptData)
					{
						const DSLDataField* field = findDataField(memberName);
						if (field == nullptr)
							return failValue("'" + ownerName + "' has no member '" + memberName + "'");
						memberType = field->type;
					}
					else if (receiverType == DSLType::ScriptEvents)
					{
						if (findEventIndex(memberName) < 0)
							return failValue("'" + ownerName + "' has no member '" + memberName + "'");
						memberType = DSLType::Int;
					}
					else
					{
						const BindingMember* member = bindings.findMember(receiverType, memberName);
						if (member == nullptr)
							return failValue("'" + ownerName + "' has no member '" + memberName + "'");
						// i == 1 is the FIRST hop, i.e. the receiver is still the bare root declaration.
						if (!checkMemberUsable(ownerName, memberName, *member, receiverDecl, i == 1))
							return nullptr;
						memberType = member->type;
					}
					receiver = push(line, ST::MemberAccess, DSLSymbol::MemberAccess{ receiver, memberName, memberType });
					receiverType = memberType;
					ownerName += "." + memberName;
					i += 2;
					if (i >= t.size())
						return receiver; // the chain itself is the value
					if (!(t[i].kind == Token::Kind::Symbol && t[i].text == "."))
						return failValue("unexpected tokens after '" + ownerName + "'");
				}
			}

			// name(...)
			if (t[1].kind == Token::Kind::Symbol && t[1].text == "(" && matchParen(t, 1) == static_cast<int>(t.size()) - 1)
			{
				DSLSymbol* func = findFunction(first.text, /*requiresReceiver*/ false);
				if (func == nullptr)
					return failValue("unknown function '" + first.text + "'");
				return parseCall(t.subspan(2, t.size() - 3), line, func, nullptr);
			}

			return failValue("unexpected tokens after '" + first.text + "'");
		}

		// `args` is the token span BETWEEN the call's parens. Each argument is positional ("vec3(0, 1, 0)") or
		// named ("[ref] [type] name = value" -- the expanded view always writes the type; the callee's matching
		// parameter is resolved by NAME, order-independent, exactly like DSLSymbol::CallArgument documents).
		DSLSymbol* parseCall(std::span<const Token> args, DSLCodeLine& line, DSLSymbol* funcSymbol, DSLSymbol* receiverRef)
		{
			const DSLSymbol::FunctionDeclaration& callee = std::get<DSLSymbol::FunctionDeclaration>(funcSymbol->data);
			std::vector<DSLSymbol::CallArgument> built;

			if (!args.empty())
			{
				const std::vector<std::span<const Token>> parts = splitAtCommas(args);
				for (size_t argIndex = 0; argIndex < parts.size(); ++argIndex)
				{
					std::span<const Token> arg = parts[argIndex];
					if (arg.empty())
						return failValue("empty argument in call to '" + callee.name + "'");

					// Optional "ref" lead-in -- the callee's parameter carries the actual flag.
					if (arg[0].kind == Token::Kind::Identifier && arg[0].text == "ref" && arg.size() > 1)
						arg = arg.subspan(1);

					// Named form. A positional value can never false-match: "name =" only reads as an
					// assignment, which is not a value ("==" is a different token).
					size_t nameAt = 0;
					DSLType annotatedType = DSLType::Void;
					if (arg.size() >= 3 && arg[0].kind == Token::Kind::Identifier && typeFromKeyword(arg[0].text, annotatedType)
						&& arg[1].kind == Token::Kind::Identifier && arg[2].kind == Token::Kind::Symbol && arg[2].text == "=")
						nameAt = 1;

					DSLSymbol* param = nullptr;
					std::span<const Token> valueSpan = arg;
					if (arg.size() >= nameAt + 2 && arg[nameAt].kind == Token::Kind::Identifier
						&& arg[nameAt + 1].kind == Token::Kind::Symbol && arg[nameAt + 1].text == "=")
					{
						for (DSLSymbol* p : callee.parameterVarDeclarations)
							if (std::get<DSLSymbol::VariableDeclaration>(p->data).name == arg[nameAt].text)
								param = p;
						if (param == nullptr)
							return failValue("'" + callee.name + "' has no parameter named '" + arg[nameAt].text + "'");
						valueSpan = arg.subspan(nameAt + 2);
					}

					// Expected value type: the named parameter's, or the callee's same-index parameter for a
					// positional call -- an argument past the callee's declared count (shouldn't occur via the
					// editor, which never authors more than it declares) infers per value instead.
					const DSLSymbol* typeSource = param;
					if (typeSource == nullptr && argIndex < callee.parameterVarDeclarations.size())
						typeSource = callee.parameterVarDeclarations[argIndex];
					DSLType expected = DSLType::Void;
					if (typeSource != nullptr)
						expected = std::get<DSLSymbol::TypeDeclaration>(
							std::get<DSLSymbol::VariableDeclaration>(typeSource->data).typeSymbol->data).type;

					DSLSymbol* value = parseExpression(valueSpan, line, expected);
					if (value == nullptr)
						return nullptr;
					// The loader-side twin of the editor's ref-argument gate (see ScriptEditor's CallArgValue
					// confirm): a `ref` parameter takes the callee's OUTPUT, so only a bare existing variable
					// can stand there. Without this a hand-edited file reaches the transpiler and fails in cl
					// with a reference-binding error instead of a DSL diagnostic.
					if (typeSource != nullptr && std::get<DSLSymbol::VariableDeclaration>(typeSource->data).isRef
						&& value->type != ST::VariableReference)
						return failValue("argument " + std::to_string(argIndex + 1) + " of '" + callee.name
							+ "' is a 'ref' parameter -- it needs a variable, not an expression");
					built.push_back(DSLSymbol::CallArgument{ param, value });
				}
			}

			if (!checkContainerNotIterated(funcSymbol, receiverRef))
				return nullptr;
			return push(line, ST::FunctionCall, DSLSymbol::FunctionCall{ funcSymbol, receiverRef, std::move(built) });
		}

		// Refuses an array push/clear authored directly inside a foreach/ifexist over that same container --
		// the loader twin of the editor filtering it out of the candidate list, reported at the offending line.
		// The same rule ACROSS user function calls is ScriptLoader::checkContainerMutations, which runs over the
		// whole document on both save and load (a callee body may not be parsed yet when its call is).
		bool checkContainerNotIterated(const DSLSymbol* funcSymbol, const DSLSymbol* receiver)
		{
			if (funcSymbol == nullptr || receiver == nullptr)
				return true;
			const DSLSymbol::FunctionDeclaration& callee = std::get<DSLSymbol::FunctionDeclaration>(funcSymbol->data);
			if (!callee.mutatesContainer)
				return true;
			for (const DSLSymbol* open : openBlocks)
			{
				if (open == nullptr || open->type != ST::FlowControl)
					continue;
				const DSLSymbol::FlowControl& fc = std::get<DSLSymbol::FlowControl>(open->data);
				if (fc.control != DSLFlowControl::ForEach && fc.control != DSLFlowControl::IfExist)
					continue;
				if (dslSameStorage(fc.condition, receiver))
					return fail("'" + callee.name + "' changes this container while a "
						+ (fc.control == DSLFlowControl::ForEach ? "'foreach'" : "'ifexist'") + " is reading it");
			}
			return true;
		}

		// "type name [= value]" -> a VariableDeclaration head, registered into scopeVars. Locals and the
		// for-loop's first clause alike.
		DSLSymbol* parseDeclaration(std::span<const Token> t, DSLCodeLine& line)
		{
			DSLType type = DSLType::Void;
			if (!(t[0].kind == Token::Kind::Identifier && typeFromKeyword(t[0].text, type)))
				return failValue("expected a type keyword");
			if (t.size() < 2 || t[1].kind != Token::Kind::Identifier)
				return failValue("expected a variable name after '" + t[0].text + "'");
			const std::string& name = t[1].text;
			// The loader-side twin of the editor's isNameInScope gate -- keywords/"ctx" are reserved exactly
			// like an actual collision (see AutoCompleteRules::isReservedWord).
			if (AutoCompleteRules::isReservedWord(name))
				return failValue("'" + name + "' is reserved");
			for (DSLSymbol* var : scopeVars)
				if (std::get<DSLSymbol::VariableDeclaration>(var->data).name == name)
					return failValue("'" + name + "' is already declared in this function");
			if (isReservedInChain(name))
				return failValue("'" + name + "' is bound by the enclosing 'ifexist'");
			for (const std::unique_ptr<DSLSymbol>& s : sidebar)
				if (s->type == ST::VariableDeclaration && std::get<DSLSymbol::VariableDeclaration>(s->data).name == name)
					return failValue("'" + name + "' is a sidebar binding");
			// A variable can't shadow a function name either (see isNameInScope's own comment for why this is
			// more than a style rule -- it'd make the function uncallable in the transpiled C++). Receiver-based
			// builtins are exempt: `dot`/`length`/`push` are only reachable through a receiver, so nothing about
			// them is shadowable (see isFunctionNameTaken).
			for (const std::unique_ptr<DSLSymbol>& s : builtins)
				if (s->type == ST::FunctionDeclaration && !std::get<DSLSymbol::FunctionDeclaration>(s->data).requiresReceiver
					&& std::get<DSLSymbol::FunctionDeclaration>(s->data).name == name)
					return failValue("'" + name + "' is already a function name");
			for (DSLSymbol* func : userFunctions)
				if (std::get<DSLSymbol::FunctionDeclaration>(func->data).name == name)
					return failValue("'" + name + "' is already a function name");

			DSLSymbol* typeSymbol = push(line, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ type });
			DSLSymbol* initValue = nullptr;
			if (t.size() > 2)
			{
				if (!(t[2].kind == Token::Kind::Symbol && t[2].text == "="))
					return failValue("expected '=' after '" + name + "'");
				initValue = parseExpression(t.subspan(3), line, type);
				if (initValue == nullptr)
					return nullptr;
			}
			DSLSymbol* decl = push(line, ST::VariableDeclaration, DSLSymbol::VariableDeclaration{ name, typeSymbol, initValue });
			scopeVars.push_back(decl);
			return decl;
		}

		// Index of the assignment operator following a (possibly dotted) target at the statement's start
		// ("self.pos.x += ..." -> the '+=' index), or 0 when the line doesn't read as an assignment.
		static size_t assignmentOpPosition(std::span<const Token> t)
		{
			size_t i = (t.size() > 1 && t[0].kind == Token::Kind::Identifier && t[0].text == "ref") ? 1 : 0;
			if (i >= t.size() || t[i].kind != Token::Kind::Identifier)
				return 0;
			++i;
			while (i + 1 < t.size() && t[i].kind == Token::Kind::Symbol && t[i].text == "."
				&& t[i + 1].kind == Token::Kind::Identifier)
				i += 2;
			DSLOperator op = DSLOperator::Assign;
			if (i < t.size() && t[i].kind == Token::Kind::Symbol && operatorFromText(t[i].text, op) && dslIsAssignOperator(op))
				return i;
			return 0;
		}

		// "[ref] name(.member)* <assign-op> value" -> the binary assign-class Expression head (see DSL.ixx:
		// assignments stay exactly [target, value]); a dotted target is a member-assign statement, every hop
		// registry-validated and the written member required writable.
		DSLSymbol* parseAssignment(std::span<const Token> t, DSLCodeLine& line)
		{
			if (t[0].kind == Token::Kind::Identifier && t[0].text == "ref" && t.size() > 1)
				t = t.subspan(1); // rendering artifact of assigning into a ref parameter -- the target's decl carries the flag
			const size_t opAt = assignmentOpPosition(t);
			DSLOperator op = DSLOperator::Assign;
			if (opAt == 0 || t.size() < opAt + 2 || !operatorFromText(t[opAt].text, op))
				return failValue("expected an assignment");
			DSLSymbol* rootDecl = findVariable(t[0].text);
			if (rootDecl == nullptr)
				return failValue("unknown identifier '" + t[0].text + "'");
			DSLType targetType = declaredType(rootDecl);
			if (opAt == 1 && dslIsEngineObjectType(targetType))
				return failValue("'" + t[0].text + "' is a binding object -- it can't be assigned to");
			// The loader-side twin of addVariableCandidates' element-binding rule: a non-`ref` foreach/ifexist
			// element is a COPY, so a write to it (or to one of its members) would be silently discarded at the
			// end of the block. Entity elements are handles, not copies, so they stay writable.
			if (!dslIsEngineObjectType(targetType)
				&& !std::get<DSLSymbol::VariableDeclaration>(rootDecl->data).isRef
				&& dslIsElementBinding(rootDecl))
				return failValue("'" + t[0].text + "' binds a copy -- declare it 'ref' to write through it");

			DSLSymbol* target = push(line, ST::VariableReference, DSLSymbol::VariableReference{ rootDecl });
			std::string ownerName = t[0].text;
			for (size_t i = 2; i < opAt; i += 2) // member hops sit at 2, 4, ... opAt-1
			{
				const std::string& memberName = t[i].text;
				DSLType memberType;
				if (targetType == DSLType::ScriptData)
				{
					// self.data's own fields are always writable (persistent data is meant to be read/write).
					const DSLDataField* field = findDataField(memberName);
					if (field == nullptr)
						return failValue("no member '" + memberName + "' to assign to");
					memberType = field->type;
				}
				else if (targetType == DSLType::ScriptEvents)
				{
					// self.events's own entries are always read-only (a compile-time index, see memberText).
					if (findEventIndex(memberName) < 0)
						return failValue("no member '" + memberName + "' to assign to");
					return failValue("member '" + memberName + "' is read-only");
				}
				else
				{
					const BindingMember* member = bindings.findMember(targetType, memberName);
					if (member == nullptr)
						return failValue("no member '" + memberName + "' to assign to");
					// Every hop must be writable, not just the last: everything reachable through a read-only
					// member is read-only too ("self.parent.pos = ..." would mutate an object the script only
					// has read access to). The `data` member is the one exception (see the editor's twin in
					// AutoCompleteRules): it is read-only because the BLOCK can't be replaced wholesale, not
					// because its contents are -- so a read-only ScriptData hop passes when another hop follows
					// (the field branch above then owns the write), while "self.data = ..." itself still refuses.
					if (!member->writable && !(member->type == DSLType::ScriptData && i < opAt - 1))
						return failValue("member '" + memberName + "' is read-only");
					// i == 2 is the FIRST hop here (the target's own name is at 0, its '.' at 1).
					if (!checkMemberUsable(ownerName, memberName, *member, rootDecl, i == 2))
						return nullptr;
					memberType = member->type;
				}
				target = push(line, ST::MemberAccess, DSLSymbol::MemberAccess{ target, memberName, memberType });
				targetType = memberType;
				ownerName += "." + memberName;
			}

			DSLSymbol* value = parseExpression(t.subspan(opAt + 1), line, targetType);
			if (value == nullptr)
				return nullptr;
			return push(line, ST::Expression, DSLSymbol::Expression{ { target, value }, { op } });
		}

		// "for" already consumed; `t` holds the three comma-separated clauses. The loop variable registers into
		// scope BEFORE the condition/increment parse, since both reference it on the same line.
		DSLSymbol* parseFor(std::span<const Token> t, DSLCodeLine& line)
		{
			const std::vector<std::span<const Token>> clauses = splitAtCommas(t);
			if (clauses.size() != 3)
				return failValue("'for' takes exactly three comma-separated clauses");
			DSLSymbol* loopVar = parseDeclaration(clauses[0], line);
			if (loopVar == nullptr)
				return nullptr;
			DSLSymbol* condition = parseExpression(clauses[1], line, DSLType::Bool);
			if (condition == nullptr)
				return nullptr;
			DSLSymbol* increment = parseAssignment(clauses[2], line);
			if (increment == nullptr)
				return nullptr;
			return push(line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::For, nullptr, loopVar, condition, increment });
		}

		// "foreach" already consumed; `t` holds "<type> <name> in <sequence>". The element declaration registers
		// into scope for the body but is NOT a value slot of its own -- the loop supplies each value, so unlike
		// a for-loop's counter it carries no initializer.
		DSLSymbol* parseForEach(std::span<const Token> t, DSLCodeLine& line)
		{
			bool isRef = false;
			if (!t.empty() && t[0].kind == Token::Kind::Identifier && t[0].text == "ref")
			{
				isRef = true;
				t = t.subspan(1);
			}
			DSLType elementType = DSLType::Void;
			if (t.size() < 4 || t[0].kind != Token::Kind::Identifier || !typeFromKeyword(t[0].text, elementType)
				|| t[1].kind != Token::Kind::Identifier)
				return failValue("expected \"foreach [ref] <type> <name> in <sequence>\"");
			if (t[2].kind != Token::Kind::Identifier || t[2].text != "in")
				return failValue("expected 'in' after the foreach element name");
			const std::string elementName = t[1].text;
			if (AutoCompleteRules::isReservedWord(elementName))
				return failValue("'" + elementName + "' is reserved");
			for (DSLSymbol* var : scopeVars)
				if (std::get<DSLSymbol::VariableDeclaration>(var->data).name == elementName)
					return failValue("'" + elementName + "' is already declared in this function");
			if (isReservedInChain(elementName))
				return failValue("'" + elementName + "' is bound by the enclosing 'ifexist'");

			// The sequence resolves FIRST, so it can't see the element the loop is about to declare.
			DSLSymbol* sequence = parseExpression(t.subspan(3), line, DSLType::Void);
			if (sequence == nullptr)
				return nullptr;
			const DSLType sequenceType = dslValueType(sequence);
			const BindingObject* object = bindings.objectFor(sequenceType);
			if (object == nullptr || object->sequenceElementType == DSLType::Void)
				return failValue("'" + std::string(dslTypeName(sequenceType)) + "' isn't iterable");
			if (object->sequenceElementType != elementType)
				return failValue("'" + std::string(dslTypeName(sequenceType)) + "' yields "
					+ dslTypeName(object->sequenceElementType) + ", not " + dslTypeName(elementType));

			if (!checkRefBinding(isRef, elementType, object, "foreach"))
				return nullptr;

			DSLSymbol* typeSymbol = push(line, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ elementType });
			DSLSymbol* elementVar = push(line, ST::VariableDeclaration,
				DSLSymbol::VariableDeclaration{ elementName, typeSymbol, nullptr, isRef });
			scopeVars.push_back(elementVar);
			return push(line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::ForEach, sequence, elementVar });
		}

		// "ifexist" already consumed; `t` holds "[ref] <type> <name> in <container> [at <key>]". The container's
		// own registration decides everything: whether it can be looked up at all, what type the key is, what
		// the element yields, and whether `ref` is allowed -- see BindingObject's lookup fields.
		DSLSymbol* parseIfExist(std::span<const Token> t, DSLCodeLine& line)
		{
			bool isRef = false;
			if (!t.empty() && t[0].kind == Token::Kind::Identifier && t[0].text == "ref")
			{
				isRef = true;
				t = t.subspan(1);
			}
			// The bound type is a value type, or a COMPONENT type -- the latter is fetched off an entity rather
			// than looked up in a container, which is the only thing that differs from here on.
			DSLType elementType = DSLType::Void;
			if (t.size() >= 1 && t[0].kind == Token::Kind::Identifier && !typeFromKeyword(t[0].text, elementType))
				elementType = Globals::scriptBindings.componentTypeByName(t[0].text);
			if (t.size() < 4 || elementType == DSLType::Void || t[1].kind != Token::Kind::Identifier)
				return failValue("expected \"ifexist [ref] <type> <name> in <source> [at <key>]\"");
			if (t[2].kind != Token::Kind::Identifier || t[2].text != "in")
				return failValue("expected 'in' after the ifexist variable name");
			const std::string boundName = t[1].text;
			if (AutoCompleteRules::isReservedWord(boundName))
				return failValue("'" + boundName + "' is reserved");
			for (DSLSymbol* var : scopeVars)
				if (std::get<DSLSymbol::VariableDeclaration>(var->data).name == boundName)
					return failValue("'" + boundName + "' is already declared in this function");
			if (isReservedInChain(boundName))
				return failValue("'" + boundName + "' is bound by the enclosing 'ifexist'");

			// Split the container from the optional "at <key>" -- the LAST top-level `at`, so a key expression
			// containing one of its own can't confuse the split (nothing today can, but the rule is cheap).
			size_t atPos = t.size();
			for (size_t i = 3, depth = 0; i < t.size(); ++i)
			{
				if (t[i].kind == Token::Kind::Symbol && t[i].text == "(") ++depth;
				else if (t[i].kind == Token::Kind::Symbol && t[i].text == ")") { if (depth > 0) --depth; }
				else if (depth == 0 && t[i].kind == Token::Kind::Identifier && t[i].text == "at") atPos = i;
			}
			if (atPos == 3)
				return failValue("expected a source before 'at'");

			// A component's source is an ENTITY (the fetch call is the whole check, see componentFetchEmit);
			// anything else is a registered lookup and must declare one. The source resolves FIRST either way,
			// so it can't see the name the statement is about to bind.
			const bool isComponent = dslIsComponentType(elementType);
			DSLSymbol* container = parseExpression(t.subspan(3, atPos - 3), line,
				isComponent ? DSLType::Entity : DSLType::Void);
			if (container == nullptr)
				return nullptr;
			const DSLType containerType = dslValueType(container);
			const BindingObject* object = isComponent ? nullptr : Globals::scriptBindings.objectFor(containerType);
			if (isComponent)
			{
				if (containerType != DSLType::Entity)
					return failValue("a component needs an Entity after 'in'");
			}
			else if (object == nullptr || object->lookupValueType == DSLType::Void || object->lookupEmit == nullptr)
			{
				return failValue("'" + std::string(dslTypeName(containerType)) + "' can't be looked up");
			}
			else if (object->lookupValueType != elementType)
			{
				return failValue("'" + std::string(dslTypeName(containerType)) + "' holds "
					+ dslTypeName(object->lookupValueType) + ", not " + dslTypeName(elementType));
			}

			DSLSymbol* key = nullptr;
			const DSLType keyType = (object != nullptr) ? object->lookupKeyType : DSLType::Void;
			if (keyType != DSLType::Void)
			{
				if (atPos >= t.size())
					return failValue("'" + std::string(dslTypeName(containerType)) + "' needs an 'at <key>'");
				key = parseExpression(t.subspan(atPos + 1), line, keyType);
				if (key == nullptr)
					return nullptr;
				if (dslValueType(key) != keyType)
					return failValue("the 'at' key must be " + std::string(dslTypeName(keyType)));
			}
			else if (atPos < t.size())
			{
				return failValue("'" + std::string(dslTypeName(containerType)) + "' takes no 'at <key>'");
			}

			// A component handle is an engine object, so checkRefBinding refuses `ref` on it regardless -- but
			// there is no container registration to consult for one, hence the null-object form.
			if (isRef && !checkRefBinding(isRef, elementType, object, "ifexist"))
				return nullptr;

			DSLSymbol* typeSymbol = push(line, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ elementType });
			DSLSymbol* boundVar = push(line, ST::VariableDeclaration,
				DSLSymbol::VariableDeclaration{ boundName, typeSymbol, nullptr, isRef });
			scopeVars.push_back(boundVar);
			return push(line, ST::FlowControl,
				DSLSymbol::FlowControl{ DSLFlowControl::IfExist, container, boundVar, key });
		}

		// The loader-side twin of the editor's `ref`-binding rules, shared by ifexist and foreach: a container
		// that declares its elements read-only refuses `ref` outright (clearer than an assignment-to-read-only
		// complaint at the use site), and a HANDLE-typed element has nothing to copy back -- writes already
		// reach the real object through it, so `ref` there would be a no-op pretending to mean something.
		bool checkRefBinding(bool isRef, DSLType elementType, const BindingObject* container, const char* keyword)
		{
			if (!isRef)
				return true;
			if (dslIsEngineObjectType(elementType))
				return fail(std::string("'") + keyword + " ref' isn't allowed on " + dslTypeName(elementType)
					+ " -- it's already a handle, writes through it reach the real object");
			if (container == nullptr || !container->elementWritable)
				return fail("this container's elements can't be modified");
			return true;
		}

		// One body/statement line. Never a function header, else/elseif, end, comment, or blank -- the load
		// driver handles those (they need block-stack context). Returns the line's new head symbol, or null.
		DSLSymbol* parseStatement(std::span<const Token> t, DSLCodeLine& line)
		{
			const Token& first = t[0];
			if (first.kind != Token::Kind::Identifier)
				return failValue("expected a statement, got '" + first.text + "'");

			if (first.text == "return")
			{
				DSLSymbol* value = nullptr;
				if (t.size() > 1)
				{
					value = parseExpression(t.subspan(1), line, currentReturnType);
					if (value == nullptr)
						return nullptr;
				}
				return push(line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::Return, value });
			}
			if (first.text == "break" && t.size() == 1)
				return push(line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::Break, nullptr });
			if (first.text == "if" || first.text == "while")
			{
				DSLSymbol* condition = parseExpression(t.subspan(1), line, DSLType::Bool);
				if (condition == nullptr)
					return nullptr;
				return push(line, ST::FlowControl,
					DSLSymbol::FlowControl{ first.text == "if" ? DSLFlowControl::If : DSLFlowControl::While, condition });
			}
			if (first.text == "for")
				return parseFor(t.subspan(1), line);
			if (first.text == "foreach")
				return parseForEach(t.subspan(1), line);
			if (first.text == "ifexist")
				return parseIfExist(t.subspan(1), line);

			DSLType declType = DSLType::Void;
			if (typeFromKeyword(first.text, declType))
				return parseDeclaration(t, line);

			// "[ref] name(.member)* <assign-op> ..." -> assignment; anything else must be a call statement.
			if (assignmentOpPosition(t) != 0)
				return parseAssignment(t, line);

			DSLSymbol* head = parseTerm(t, line, DSLType::Void);
			if (head == nullptr)
				return nullptr;
			if (head->type != ST::FunctionCall)
				return failValue("expected a statement");
			return head;
		}

		// Pass 1: "function name([ref] type name, ...) [-> type]" -> a complete scope-0 header line, its
		// FunctionDeclaration registered for pass 2's call resolution (which is what makes forward calls work).
		std::unique_ptr<DSLCodeLine> parseFunctionHeader(std::span<const Token> t)
		{
			auto line = std::make_unique<DSLCodeLine>();
			line->scopeLevel = 0;

			if (t.size() < 4 || t[1].kind != Token::Kind::Identifier
				|| !(t[2].kind == Token::Kind::Symbol && t[2].text == "("))
			{
				fail("malformed function declaration");
				return nullptr;
			}
			const std::string& name = t[1].text;
			// The loader-side twin of the editor's isFunctionNameTaken gate -- keywords/"ctx" are reserved
			// exactly like an actual collision (see AutoCompleteRules::isReservedWord).
			if (AutoCompleteRules::isReservedWord(name))
			{
				fail("'" + name + "' is reserved");
				return nullptr;
			}
			for (DSLSymbol* func : userFunctions)
				if (std::get<DSLSymbol::FunctionDeclaration>(func->data).name == name)
				{
					fail("function '" + name + "' is declared twice");
					return nullptr;
				}
			// Receiver-based builtins are exempt, same as for variable names above (see isFunctionNameTaken).
			for (const std::unique_ptr<DSLSymbol>& s : builtins)
				if (s->type == ST::FunctionDeclaration && !std::get<DSLSymbol::FunctionDeclaration>(s->data).requiresReceiver
					&& std::get<DSLSymbol::FunctionDeclaration>(s->data).name == name)
				{
					fail("'" + name + "' is a builtin function");
					return nullptr;
				}

			const int close = matchParen(t, 2);
			if (close < 0)
			{
				fail("missing ')' on 'function " + name + "'");
				return nullptr;
			}

			std::vector<DSLSymbol*> params;
			if (close > 3)
			{
				for (std::span<const Token> part : splitAtCommas(t.subspan(3, close - 3)))
				{
					bool isRef = false;
					if (!part.empty() && part[0].kind == Token::Kind::Identifier && part[0].text == "ref")
					{
						isRef = true;
						part = part.subspan(1);
					}
					DSLType paramType = DSLType::Void;
					if (part.size() != 2 || part[0].kind != Token::Kind::Identifier || !typeFromKeyword(part[0].text, paramType)
						|| part[1].kind != Token::Kind::Identifier)
					{
						fail("malformed parameter in 'function " + name + "' (expected \"[ref] type name\")");
						return nullptr;
					}
					if (AutoCompleteRules::isReservedWord(part[1].text))
					{
						fail("'function " + name + "' can't declare a parameter named '" + part[1].text + "' -- it's reserved");
						return nullptr;
					}
					for (DSLSymbol* prev : params)
						if (std::get<DSLSymbol::VariableDeclaration>(prev->data).name == part[1].text)
						{
							fail("'function " + name + "' declares parameter '" + part[1].text + "' twice");
							return nullptr;
						}
					DSLSymbol* typeSymbol = push(*line, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ paramType });
					params.push_back(push(*line, ST::VariableDeclaration,
						DSLSymbol::VariableDeclaration{ part[1].text, typeSymbol, nullptr, isRef }));
				}
			}

			DSLType returnType = DSLType::Void;
			if (close + 1 < static_cast<int>(t.size()))
			{
				if (!(t[close + 1].kind == Token::Kind::Symbol && t[close + 1].text == "->"
					&& close + 3 == static_cast<int>(t.size()) && t[close + 2].kind == Token::Kind::Identifier
					&& typeFromKeyword(t[close + 2].text, returnType)))
				{
					fail("malformed return type on 'function " + name + "'");
					return nullptr;
				}
			}

			DSLSymbol* head = push(*line, ST::FunctionDeclaration,
				DSLSymbol::FunctionDeclaration{ name, std::move(params), returnType });
			userFunctions.push_back(head);
			return line;
		}
	};

	// One line of the file's DSL block: comment prefix stripped, indentation intact, classified for the driver.
	struct BlockLine
	{
		enum class Kind { Blank, Comment, Code };
		Kind kind = Kind::Blank;
		int fileLineNo = 0;      // 1-based line in the .dsl file, for error messages
		std::string text;        // the whole line as saved (minus "//") -- what the round-trip check compares against
		std::string commentText; // Comment only
		std::vector<Token> tokens; // Code only
	};
}

// Mutate-while-iterating, over a whole DOCUMENT rather than during parsing -- so it runs on the editor's save
// path (which never parses) as well as on load. Refuses a container mutated while a foreach/ifexist reads it,
// directly OR through any chain of user function calls: each function's own mutations and callees are
// collected, the call graph is closed, then every call made from inside a loop is re-checked. A mutation
// through a PARAMETER makes the array the caller's choice, so it poisons that function for every caller
// instead of being matched argument-by-argument.
bool ScriptLoader::checkContainerMutations(const DSL& document, std::string& outError, int& outLineIndex)
{
	struct Key
	{
		const DSLSymbol* root = nullptr;
		std::string path;
		bool operator==(const Key& other) const { return root == other.root && path == other.path; }
	};
	struct Mutation
	{
		std::vector<Key> paths;
		std::vector<const DSLSymbol*> calls;
		bool unknown = false;
	};
	struct Site
	{
		const DSLSymbol* callee = nullptr;
		std::vector<Key> iterated;
		std::string calleeName;
		std::string blockWord;
		int lineIndex = 0;
	};

	const auto storageOf = [](const DSLSymbol* value, Key& out)
		{
			const DSLSymbol* root = nullptr;
			std::string path;
			if (!dslChainToRoot(value, root, path))
				return false;
			out.root = root;
			out.path = std::move(path);
			return true;
		};

	const std::vector<std::unique_ptr<DSLCodeLine>>& lines = document.file.lines;
	std::unordered_map<const DSLSymbol*, Mutation> mutations;
	std::vector<Site> sites;
	const DSLSymbol* currentFunction = nullptr;

	for (int i = 0; i < static_cast<int>(lines.size()); ++i)
	{
		const DSLSymbol* head = lines[i]->head();
		if (head != nullptr && head->type == DSLSymbol::SymbolType::FunctionDeclaration)
		{
			currentFunction = head;
			mutations[head]; // present even with nothing recorded, so a caller's lookup succeeds
			continue;
		}

		// Storage every enclosing foreach/ifexist is reading, innermost first.
		std::vector<Key> iterated;
		std::string blockWord;
		for (int enclosing = dslEnclosingBlockHeader(document.file, i); enclosing >= 0;
			enclosing = dslEnclosingBlockHeader(document.file, enclosing))
		{
			const DSLSymbol* openHead = lines[enclosing]->head();
			if (openHead == nullptr || openHead->type != DSLSymbol::SymbolType::FlowControl)
				continue;
			const DSLSymbol::FlowControl& fc = std::get<DSLSymbol::FlowControl>(openHead->data);
			if (fc.control != DSLFlowControl::ForEach && fc.control != DSLFlowControl::IfExist)
				continue;
			if (Key key; storageOf(fc.condition, key))
			{
				iterated.push_back(std::move(key));
				if (blockWord.empty())
					blockWord = fc.control == DSLFlowControl::ForEach ? "foreach" : "ifexist";
			}
		}

		for (const std::unique_ptr<DSLSymbol>& symbol : lines[i]->symbols)
		{
			if (symbol->type != DSLSymbol::SymbolType::FunctionCall)
				continue;
			const DSLSymbol::FunctionCall& call = std::get<DSLSymbol::FunctionCall>(symbol->data);
			if (call.functionSymbol == nullptr)
				continue;
			const DSLSymbol::FunctionDeclaration& callee = std::get<DSLSymbol::FunctionDeclaration>(call.functionSymbol->data);

			if (callee.mutatesContainer && call.receiver != nullptr)
			{
				Key key;
				const bool resolved = storageOf(call.receiver, key);
				if (resolved)
					for (const Key& open : iterated)
						if (open == key)
						{
							outError = "'" + callee.name + "' changes this container while a '"
								+ (blockWord.empty() ? std::string("foreach") : blockWord) + "' is reading it";
							outLineIndex = i;
							return false;
						}
				if (currentFunction != nullptr)
				{
					Mutation& mutation = mutations[currentFunction];
					bool throughParameter = false;
					if (resolved)
						for (const DSLSymbol* param :
							std::get<DSLSymbol::FunctionDeclaration>(currentFunction->data).parameterVarDeclarations)
							throughParameter = throughParameter || (param == key.root);
					if (!resolved || throughParameter)
						mutation.unknown = true;
					else if (std::find(mutation.paths.begin(), mutation.paths.end(), key) == mutation.paths.end())
						mutation.paths.push_back(std::move(key));
				}
			}
			else if (!callee.requiresReceiver && call.receiver == nullptr)
			{
				// A bare-name call: a user function if this document declares it (builtins are receiver-based
				// or resolve to no declaration line of their own).
				bool isUser = false;
				for (const std::unique_ptr<DSLCodeLine>& other : lines)
					isUser = isUser || (other->head() == call.functionSymbol);
				if (!isUser)
					continue;
				if (currentFunction != nullptr)
				{
					Mutation& mutation = mutations[currentFunction];
					if (std::find(mutation.calls.begin(), mutation.calls.end(), call.functionSymbol) == mutation.calls.end())
						mutation.calls.push_back(call.functionSymbol);
				}
				if (!iterated.empty())
					sites.push_back({ call.functionSymbol, iterated, callee.name, blockWord, i });
			}
		}
	}

	for (bool changed = true; changed; )
	{
		changed = false;
		for (auto& [function, mutation] : mutations)
		{
			const std::vector<const DSLSymbol*> callees = mutation.calls;
			for (const DSLSymbol* callee : callees)
			{
				const auto calleeIt = mutations.find(callee);
				if (calleeIt == mutations.end())
					continue;
				const Mutation inherited = calleeIt->second;
				Mutation& caller = mutations[function];
				if (inherited.unknown && !caller.unknown)
				{
					caller.unknown = true;
					changed = true;
				}
				for (const Key& key : inherited.paths)
					if (std::find(caller.paths.begin(), caller.paths.end(), key) == caller.paths.end())
					{
						caller.paths.push_back(key);
						changed = true;
					}
				for (const DSLSymbol* nested : inherited.calls)
					if (std::find(caller.calls.begin(), caller.calls.end(), nested) == caller.calls.end())
					{
						caller.calls.push_back(nested);
						changed = true;
					}
			}
		}
	}

	for (const Site& site : sites)
	{
		const auto it = mutations.find(site.callee);
		if (it == mutations.end())
			continue;
		const std::string word = site.blockWord.empty() ? std::string("foreach") : site.blockWord;
		if (it->second.unknown)
		{
			outError = "'" + site.calleeName + "' changes a container it is given while a '" + word + "' is reading one";
			outLineIndex = site.lineIndex;
			return false;
		}
		for (const Key& mutated : it->second.paths)
			if (std::find(site.iterated.begin(), site.iterated.end(), mutated) != site.iterated.end())
			{
				outError = "'" + site.calleeName + "' changes this container while a '" + word + "' is reading it";
				outLineIndex = site.lineIndex;
				return false;
			}
	}
	return true;
}

bool ScriptLoader::save(DSL& document, const std::string& path, const std::string& generatedCode)
{
	// Nothing that would produce dangling-span C++ gets written. The editor never parses on this path, so this
	// is the only place the rule can be enforced for an authored document.
	std::string error;
	int lineIndex = 0;
	if (!checkContainerMutations(document, error, lineIndex))
	{
		Log::error("ScriptLoader: refusing to save '" + path + "' -- line " + std::to_string(lineIndex + 1) + ": " + error);
		return false;
	}

	std::ostringstream file; // built in memory, written through FileSystem below

	if (!generatedCode.empty())
	{
		file << generatedCode;
		if (generatedCode.back() != '\n')
			file << '\n';
		file << '\n';
	}
	file << "//@@dsl 1\n";
	file << directiveText(document);
	for (const SyntaxLine& line : Syntax::format(document.file, /*compact*/ false))
	{
		file << "//@";
		for (int i = 0; i < line.scopeLevel; ++i)
			file << '\t';
		file << line.text << '\n';
	}
	file << kBlockEnd << '\n';
	// Saving is an explicit editor action on the main thread.
	return FileSystem::writeFileStr(path, file.str(), /*allowMainThread*/ true);
}

std::string ScriptLoader::directiveText(const DSL& document)
{
	std::ostringstream out;
	if (!document.requiredComponents.empty())
	{
		out << "//@@require ";
		for (size_t i = 0; i < document.requiredComponents.size(); ++i)
		{
			const char* name = Globals::scriptBindings.componentTypeName(document.requiredComponents[i]);
			out << (i > 0 ? ", " : "") << (name != nullptr ? name : "?");
		}
		out << '\n';
	}
	// "//@@data [private|public] <type> <name>" -- the keyword is OMITTED for Hidden, which is the default, so
	// every field authored before visibility existed round-trips byte-identically.
	for (const DSLDataField& field : document.dataFields)
	{
		out << "//@@data ";
		if (field.visibility != DSLFieldVisibility::Hidden)
			out << dslFieldVisibilityName(field.visibility) << ' ';
		out << dslTypeName(field.type) << ' ' << field.name << '\n';
	}
	for (const std::string& eventName : document.eventNames)
		out << "//@@event " << eventName << '\n';
	return out.str();
}

ScriptLoader::LoadResult ScriptLoader::load(DSL& document, const std::string& path, const std::vector<std::unique_ptr<DSLSymbol>>& builtins,
	const ScriptBindings& bindings)
{
	// Loading a script document is an explicit editor/compile action (main thread by design).
	if (!FileSystem::exists(path, /*allowMainThread*/ true))
	{
		LoadResult result;
		result.error = "cannot open '" + path + "'";
		return result;
	}
	return loadFromText(document, FileSystem::readFileStr(path, /*allowMainThread*/ true), path, builtins, bindings);
}

ScriptLoader::LoadResult ScriptLoader::loadFromText(DSL& document, const std::string& text, const std::string& originName,
	const std::vector<std::unique_ptr<DSLSymbol>>& builtins, const ScriptBindings& bindings)
{
	LoadResult result;
	const auto failAt = [&](int lineNo, const std::string& what) -> LoadResult
	{
		result.error = originName + "(" + std::to_string(lineNo) + "): " + what;
		return result;
	};

	std::istringstream file(text);

	// Extract the "//@@dsl" block: every following "//@"-prefixed line until "//@@end" (or the first
	// unprefixed line/EOF), prefixes stripped; "//@@require" directive lines carry the file's required
	// components. Anything outside the block -- the future transpiled C++, ordinary "//" comments included --
	// is ignored entirely.
	std::vector<BlockLine> blockLines;
	std::vector<DSLType> requiredComponents;
	std::vector<DSLDataField> dataFields;
	std::vector<std::string> eventNames;
	{
		std::string raw;
		int lineNo = 0;
		bool inBlock = false, done = false;
		while (!done && std::getline(file, raw))
		{
			++lineNo;
			if (!raw.empty() && raw.back() == '\r')
				raw.pop_back();
			if (!inBlock)
			{
				if (raw.rfind("//@@dsl", 0) == 0)
				{
					std::string version = raw.substr(7);
					version.erase(0, version.find_first_not_of(" \t"));
					if (version != "1")
						return failAt(lineNo, "unsupported format version '" + version + "' (this build reads version 1)");
					inBlock = true;
				}
				continue;
			}
			if (raw.rfind("//@@require", 0) == 0)
			{
				// Comma-separated component type names, matched against every registered component's own name
				// (ScriptBindings::componentTypeByName) -- never a struct name, even though one might otherwise
				// spell the same word, see its own comment.
				std::string rest = raw.substr(11);
				size_t start = 0;
				while (start <= rest.size())
				{
					size_t comma = rest.find(',', start);
					if (comma == std::string::npos)
						comma = rest.size();
					std::string name = rest.substr(start, comma - start);
					name.erase(0, name.find_first_not_of(" \t"));
					name.erase(name.find_last_not_of(" \t") + 1);
					start = comma + 1;
					if (name.empty())
						continue;
					const DSLType componentType = Globals::scriptBindings.componentTypeByName(name);
					if (componentType == DSLType::Void)
						return failAt(lineNo, "unknown component kind '" + name + "' in //@@require");
					requiredComponents.push_back(componentType);
				}
				continue;
			}
			if (raw.rfind("//@@data", 0) == 0)
			{
				// "//@@data [private|public] <type> <name>" -- one persistent self.data field, the "type name"
				// part in the exact shape a real declaration line uses, so it reuses the same tokenizer and
				// typeFromKeyword machinery. The visibility keyword is optional; absent means Hidden.
				std::string rest = raw.substr(8);
				rest.erase(0, rest.find_first_not_of(" \t"));
				std::vector<Token> tokens;
				std::string tokenError;
				if (!tokenizeLine(rest, tokens, tokenError))
					return failAt(lineNo, tokenError);
				std::span<const Token> fieldTokens(tokens);
				DSLFieldVisibility visibility = DSLFieldVisibility::Hidden;
				if (!fieldTokens.empty() && fieldTokens[0].kind == Token::Kind::Identifier)
				{
					if (fieldTokens[0].text == "private")      visibility = DSLFieldVisibility::Private;
					else if (fieldTokens[0].text == "public")  visibility = DSLFieldVisibility::Public;
					if (visibility != DSLFieldVisibility::Hidden)
						fieldTokens = fieldTokens.subspan(1);
				}
				DSLType type = DSLType::Void;
				if (fieldTokens.size() != 2 || fieldTokens[0].kind != Token::Kind::Identifier
					|| !typeFromKeyword(fieldTokens[0].text, type) || fieldTokens[1].kind != Token::Kind::Identifier)
					return failAt(lineNo, "malformed //@@data line (expected \"//@@data [private|public] <type> <name>\")");
				const std::string& fieldName = fieldTokens[1].text;
				// The loader twin of the SCRIPT DATA panel's rule: an array is a handle into engine-side storage,
				// so there is nothing for an inspector to show or set.
				if (visibility != DSLFieldVisibility::Hidden && !dslCanExposeFieldType(type))
					return failAt(lineNo, "'" + std::string(dslTypeName(type)) + "' can't be "
						+ dslFieldVisibilityName(visibility) + " -- only scalar and struct fields can be exposed");
				// Entity isn't storable in either form -- see DSLDataField. The loader-side twin of the SCRIPT
				// DATA panel simply not offering it.
				if (type == DSLType::Entity || dslArrayElementType(type) == DSLType::Entity)
					return failAt(lineNo, "'" + std::string(dslTypeName(type)) + "' can't be stored in script data");
				if (AutoCompleteRules::isReservedWord(fieldName))
					return failAt(lineNo, "'" + fieldName + "' is reserved");
				for (const DSLDataField& existing : dataFields)
					if (existing.name == fieldName)
						return failAt(lineNo, "'" + fieldName + "' is declared twice in //@@data");
				dataFields.push_back(DSLDataField{ fieldName, type, visibility });
				continue;
			}
			if (raw.rfind("//@@event", 0) == 0)
			{
				// "//@@event <name>" -- one named On Event entry (self.events.<name>), index = position in this list.
				std::string rest = raw.substr(9);
				rest.erase(0, rest.find_first_not_of(" \t"));
				std::vector<Token> tokens;
				std::string tokenError;
				if (!tokenizeLine(rest, tokens, tokenError))
					return failAt(lineNo, tokenError);
				if (tokens.size() != 1 || tokens[0].kind != Token::Kind::Identifier)
					return failAt(lineNo, "malformed //@@event line (expected \"//@@event <name>\")");
				const std::string& eventName = tokens[0].text;
				if (AutoCompleteRules::isReservedWord(eventName))
					return failAt(lineNo, "'" + eventName + "' is reserved");
				for (const std::string& existing : eventNames)
					if (existing == eventName)
						return failAt(lineNo, "'" + eventName + "' is declared twice in //@@event");
				eventNames.push_back(eventName);
				continue;
			}
			if (raw == kBlockEnd || raw.rfind("//@", 0) != 0)
			{
				done = true;
				continue;
			}
			BlockLine& line = blockLines.emplace_back();
			line.fileLineNo = lineNo;
			line.text = raw.substr(3);
		}
		if (!inBlock && blockLines.empty())
		{
			result.error = "'" + originName + "' has no //@@dsl block";
			return result;
		}
	}

	// Classify each line: leading TABS are indentation (informational only -- structure derives from the block
	// keywords; the round-trip check flags any drift), '#' starts a comment, empty is a blank statement slot.
	for (BlockLine& line : blockLines)
	{
		size_t i = 0;
		while (i < line.text.size() && line.text[i] == '\t')
			++i;
		const std::string body = line.text.substr(i);
		if (body.empty())
			continue; // Kind::Blank
		if (body[0] == '#')
		{
			line.kind = BlockLine::Kind::Comment;
			line.commentText = body.substr(body.size() >= 2 && body[1] == ' ' ? 2 : 1);
			continue;
		}
		std::string tokenError;
		if (!tokenizeLine(body, line.tokens, tokenError))
			return failAt(line.fileLineNo, tokenError);
		line.kind = line.tokens.empty() ? BlockLine::Kind::Blank : BlockLine::Kind::Code;
	}

	Parser parser{ document.sidebar, builtins, bindings, requiredComponents, dataFields, eventNames };

	// Pass 1: function headers, so pass 2's body statements resolve forward calls.
	std::vector<std::unique_ptr<DSLCodeLine>> headers(blockLines.size());
	for (size_t i = 0; i < blockLines.size(); ++i)
	{
		const BlockLine& line = blockLines[i];
		if (line.kind == BlockLine::Kind::Code && line.tokens[0].kind == Token::Kind::Identifier && line.tokens[0].text == "function")
		{
			headers[i] = parser.parseFunctionHeader(line.tokens);
			if (headers[i] == nullptr)
				return failAt(line.fileLineNo, parser.error);
		}
	}

	// Pass 2: everything in order. The block-keyword structure (function/if/while/for open, elseif/else
	// continue, end closes) drives every line's scopeLevel -- DSL.ixx's scope-only nesting model rebuilt.
	std::vector<DSLSymbol*>& openBlocks = parser.openBlocks; // each entry = the open block's CURRENT header symbol
	// Parallel to openBlocks: parser.scopeVars.size() from JUST BEFORE that block's own header ran (so a
	// for-loop's counter, declared as part of parsing its header line, is included in what gets pruned) --
	// the editor-side twin of ScriptLang.cpp's inScopeVariables scope-walk, restored on 'end'/branch-switch so
	// a name declared inside a block (the loop counter included) can't resolve once the block's done with.
	std::vector<size_t> openBlockScopeDepth;
	std::vector<size_t> openBlockReservedDepth; // parallel: reservedNames size when that block opened
	for (size_t i = 0; i < blockLines.size(); ++i)
	{
		const BlockLine& src = blockLines[i];
		const int depth = static_cast<int>(openBlocks.size());

		if (src.kind == BlockLine::Kind::Blank || src.kind == BlockLine::Kind::Comment)
		{
			auto line = std::make_unique<DSLCodeLine>();
			line->scopeLevel = depth;
			if (src.kind == BlockLine::Kind::Blank)
				parser.push(*line, ST::Placeholder, DSLSymbol::Placeholder{ DSLType::Void });
			else
				parser.push(*line, ST::Comment, DSLSymbol::Comment{ src.commentText });
			parser.outLines.push_back(std::move(line));
			continue;
		}

		const std::string word = (src.tokens[0].kind == Token::Kind::Identifier) ? src.tokens[0].text : std::string();

		if (word == "end" && src.tokens.size() == 1)
		{
			if (openBlocks.empty())
				return failAt(src.fileLineNo, "'end' with no open block");
			openBlocks.pop_back();
			parser.scopeVars.resize(openBlockScopeDepth.back());
			openBlockScopeDepth.pop_back();
			parser.reservedNames.resize(openBlockReservedDepth.back());
			openBlockReservedDepth.pop_back();
			continue; // synthetic -- the formatter re-derives it from the scopeLevel step-down
		}
		if (word == "function")
		{
			if (!openBlocks.empty())
				return failAt(src.fileLineNo, "functions can't nest -- missing 'end'?");
			DSLSymbol* head = headers[i]->head();
			const DSLSymbol::FunctionDeclaration& func = std::get<DSLSymbol::FunctionDeclaration>(head->data);
			parser.scopeVars = func.parameterVarDeclarations; // fresh function scope: its own parameters
			parser.currentReturnType = func.returnType;
			openBlocks.push_back(head);
			openBlockScopeDepth.push_back(parser.scopeVars.size());
			openBlockReservedDepth.push_back(parser.reservedNames.size());
			parser.outLines.push_back(std::move(headers[i]));
			continue;
		}
		if ((word == "else" && src.tokens.size() == 1) || word == "elseif")
		{
			const DSLSymbol* top = openBlocks.empty() ? nullptr : openBlocks.back();
			const DSLSymbol::FlowControl* topFlow = (top != nullptr && top->type == ST::FlowControl)
				? &std::get<DSLSymbol::FlowControl>(top->data) : nullptr;
			// ifexist chains an else but never an elseif -- nothing bool to continue from a lookup.
			const bool boundHeader = topFlow != nullptr && topFlow->control == DSLFlowControl::IfExist;
			const bool chainable = topFlow != nullptr
				&& (topFlow->control == DSLFlowControl::If || topFlow->control == DSLFlowControl::ElseIf
					|| (boundHeader && word == "else"));
			if (!chainable)
				return failAt(src.fileLineNo, "'" + word + "' without a matching 'if'");
			// The branch just finished goes out of scope before this sibling's own condition/body parses --
			// same depth as when the chain opened (a branch switch doesn't nest deeper), so the snapshot itself
			// doesn't change, only scopeVars gets pruned back to it.
			// An ifexist's binding stops being READABLE here but stays reserved for the rest of
			// the chain -- redeclaring it in the else would shadow the condition declaration in the emitted C++.
			if (boundHeader && topFlow->forLoopVar != nullptr)
				parser.reservedNames.push_back(std::get<DSLSymbol::VariableDeclaration>(topFlow->forLoopVar->data).name);
			parser.scopeVars.resize(openBlockScopeDepth.back());
			auto line = std::make_unique<DSLCodeLine>();
			line->scopeLevel = depth - 1; // continues the chain at its own header's level (see Syntax::format)
			DSLSymbol* head = nullptr;
			if (word == "else")
				head = parser.push(*line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::Else, nullptr });
			else
			{
				DSLSymbol* condition = parser.parseExpression(std::span<const Token>(src.tokens).subspan(1), *line, DSLType::Bool);
				if (condition == nullptr)
					return failAt(src.fileLineNo, parser.error);
				head = parser.push(*line, ST::FlowControl, DSLSymbol::FlowControl{ DSLFlowControl::ElseIf, condition });
			}
			openBlocks.back() = head; // the chain's single 'end' now closes from this branch
			parser.outLines.push_back(std::move(line));
			continue;
		}

		auto line = std::make_unique<DSLCodeLine>();
		line->scopeLevel = depth;
		const size_t scopeDepthBeforeHeader = parser.scopeVars.size(); // captured before parseStatement, so a
		                                                                // block opener's OWN declaration (a for-
		                                                                // loop's counter) is included if pruned
		DSLSymbol* head = parser.parseStatement(src.tokens, *line);
		if (head == nullptr)
			return failAt(src.fileLineNo, parser.error);
		if (Syntax::isBlockOpener(head))
		{
			openBlocks.push_back(head);
			openBlockScopeDepth.push_back(scopeDepthBeforeHeader);
			openBlockReservedDepth.push_back(parser.reservedNames.size());
		}
		parser.outLines.push_back(std::move(line));
	}

	if (!openBlocks.empty())
		return failAt(blockLines.empty() ? 1 : blockLines.back().fileLineNo, "missing 'end' at end of script");


	// Round-trip check: re-render the reconstructed document and compare against what was loaded. A mismatch
	// still loads (the structure is valid; the usual cause is hand-edited formatting, normalized on the next
	// save) but every differing line is logged, so a loader bug can never hide.
	DSLScriptFile rebuilt;
	rebuilt.lines = std::move(parser.outLines);
	const std::vector<SyntaxLine> rendered = Syntax::format(rebuilt, /*compact*/ false);
	if (rendered.size() != blockLines.size())
		Log::warning("ScriptLoader: '" + originName + "' re-renders to " + std::to_string(rendered.size())
			+ " lines where the file has " + std::to_string(blockLines.size()));
	for (size_t i = 0; i < rendered.size() && i < blockLines.size(); ++i)
	{
		std::string expected(static_cast<size_t>(rendered[i].scopeLevel), '\t');
		expected += rendered[i].text;
		if (expected != blockLines[i].text)
			Log::warning("ScriptLoader: '" + originName + "' line " + std::to_string(blockLines[i].fileLineNo)
				+ " re-renders as \"" + expected + "\" (file has \"" + blockLines[i].text + "\")");
	}

	document.file.lines = std::move(rebuilt.lines);
	document.requiredComponents = std::move(requiredComponents);
	document.dataFields = std::move(dataFields);
	document.eventNames = std::move(eventNames);

	// The same rule save() enforces, so a hand-edited file can't smuggle in what the editor won't write.
	std::string mutationError;
	int badLine = 0;
	if (!checkContainerMutations(document, mutationError, badLine))
	{
		document.file.lines.clear();
		return failAt(badLine >= 0 && badLine < static_cast<int>(blockLines.size())
			? blockLines[badLine].fileLineNo : 1, mutationError);
	}

	result.success = true;
	return result;
}
