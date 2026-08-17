module Script;

import Core;
import :DSL;
import :ScriptLang;

const char* dslTypeName(DSLType type)
{
	// An array renders as its element type plus "[]". Composed on demand into a per-element cache so the
	// return stays a stable `const char*` like every other branch here (callers hold it as one).
	if (dslIsArrayType(type))
	{
		static oc::unordered_map<uint16, oc::string> names;
		auto it = names.find(static_cast<uint16>(type));
		if (it == names.end())
			it = names.emplace(static_cast<uint16>(type), oc::string(dslTypeName(dslArrayElementType(type))) + "[]").first;
		return it->second.c_str();
	}
	// Struct types are dynamic -- their spelling lives in the registry (see DSLType::FirstStruct).
	if (const BindingStruct* structDef = Globals::scriptBindings.structFor(type); structDef != nullptr)
		return structDef->name;
	// So are registered component types (e.g. PhysicsComponent/AudioComponent/ForceComponent, none hardcoded
	// here -- see DSLType::FirstComponentType).
	if (const char* componentName = Globals::scriptBindings.componentTypeName(type); componentName != nullptr)
		return componentName;
	// A registered SEQUENCE renders exactly like an array of its element type ("Entity[]"), because that's what
	// it is to the author: an indexable, iterable collection. Whether the storage is a script-owned handle or a
	// live engine collection is an implementation detail of the emits, not something the DSL spells differently.
	if (dslIsSequenceType(type))
	{
		static oc::unordered_map<uint16, oc::string> names;
		auto it = names.find(static_cast<uint16>(type));
		if (it == names.end())
		{
			// Which half the registration filled in decides how it reads: a collection ("Entity[]"), a value
			// that may not be there ("Entity?"), or -- neither -- a plain NAMESPACE, which reads as its own
			// registered name ("math"). See registerSequenceType / registerOptionalType / registerNamespace.
			const BindingObject* object = Globals::scriptBindings.objectFor(type);
			const char* registered = Globals::scriptBindings.sequenceTypeName(type);
			oc::string name = (registered != nullptr) ? registered : "?";
			if (object != nullptr && object->sequenceElementType != DSLType::Void)
				name = oc::string(dslTypeName(object->sequenceElementType)) + "[]";
			else if (object != nullptr && object->lookupValueType != DSLType::Void)
				name = oc::string(dslTypeName(object->lookupValueType)) + "?";
			it = names.emplace(static_cast<uint16>(type), oc::move(name)).first;
		}
		return it->second.c_str();
	}
	switch (type)
	{
	case DSLType::Void:         return "void";
	case DSLType::Int:          return "int";
	case DSLType::Float:        return "float";
	case DSLType::String:      return "string";
	case DSLType::Bool:         return "bool";
	case DSLType::Function:     return "function";
	case DSLType::World:        return "World";
	case DSLType::Entity:       return "Entity";
	case DSLType::ScriptData:   return "ScriptData";
	case DSLType::ScriptEvents: return "ScriptEvents";
	default:                    return "?";
	}
}

namespace
{
	using ST = DSLSymbol::SymbolType;

	const char* flowControlKeyword(DSLFlowControl control)
	{
		switch (control)
		{
		case DSLFlowControl::If:     return "if";
		case DSLFlowControl::ElseIf: return "elseif";
		case DSLFlowControl::Else:   return "else";
		case DSLFlowControl::While:  return "while";
		case DSLFlowControl::For:    return "for";
		case DSLFlowControl::ForEach:return "foreach";
		case DSLFlowControl::IfExist: return "ifexist";
		case DSLFlowControl::Return: return "return";
		case DSLFlowControl::Break:  return "break";
		default:                     return "?";
		}
	}

	const char* operatorText(DSLOperator op)
	{
		switch (op)
		{
		case DSLOperator::Assign:             return "=";
		case DSLOperator::AssignAdd:          return "+=";
		case DSLOperator::AssignSubtract:     return "-=";
		case DSLOperator::AssignMultiply:     return "*=";
		case DSLOperator::AssignDivide:       return "/=";
		case DSLOperator::AssignModulus:      return "%=";
		case DSLOperator::Add:                return "+";
		case DSLOperator::Subtract:           return "-";
		case DSLOperator::Multiply:           return "*";
		case DSLOperator::Divide:             return "/";
		case DSLOperator::Modulus:             return "%";
		case DSLOperator::Equal:              return "==";
		case DSLOperator::NotEqual:           return "!=";
		case DSLOperator::LessThan:           return "<";
		case DSLOperator::GreaterThan:        return ">";
		case DSLOperator::LessThanOrEqual:    return "<=";
		case DSLOperator::GreaterThanOrEqual: return ">=";
		case DSLOperator::And:                return "&&";
		case DSLOperator::Or:                 return "||";
		default:                              return "?";
		}
	}

	void appendSpan(DSLSymbol* symbol, const oc::string& text, size_t startPos, oc::vector<SyntaxSpan>& outSpans, SlotRef slot = {}, int operatorIndex = -1)
	{
		outSpans.push_back(SyntaxSpan{ symbol, static_cast<int>(startPos), static_cast<int>(text.size()), slot, operatorIndex });
	}

	// Recursive per-symbol renderer -- used both for a line's head symbol and for every nested sub-expression
	// (conditions, call arguments, operands, receivers). `isFunctionParamContext` is true only while rendering
	// a VariableDeclaration that is one of a FunctionDeclaration's own parameters (or a call's matching
	// argument type annotation), which is the one place `compact` hides a type. `slot` describes how to
	// replace THIS symbol wholesale (see SlotRef) -- it's attached only to the symbol's own top-level
	// identifying span; when recursing into a child, either a fresh slot is built (the three tracked
	// positions: FlowControl::condition, CallArgument::value, VariableDeclaration::initialValue) or `{}` is
	// passed (everywhere else -- not independently replaceable).
	void renderSymbol(DSLSymbol* symbol, bool compact, bool isFunctionParamContext, const SlotRef& slot, oc::string& outText, oc::vector<SyntaxSpan>& outSpans)
	{
		if (symbol == nullptr)
			return;

		switch (symbol->type)
		{
		case ST::Constant:
		{
			const DSLSymbol::Constant& c = oc::get<DSLSymbol::Constant>(symbol->data);
			const size_t start = outText.size();
			outText += (c.type == DSLType::String) ? ("\"" + c.value + "\"") : c.value;
			appendSpan(symbol, outText, start, outSpans, slot);
			break;
		}
		case ST::TypeDeclaration:
		{
			const DSLSymbol::TypeDeclaration& t = oc::get<DSLSymbol::TypeDeclaration>(symbol->data);
			const size_t start = outText.size();
			outText += dslTypeName(t.type);
			appendSpan(symbol, outText, start, outSpans, slot);
			break;
		}
		case ST::VariableReference:
		{
			const DSLSymbol::VariableReference& r = oc::get<DSLSymbol::VariableReference>(symbol->data);
			const DSLSymbol::VariableDeclaration& decl = oc::get<DSLSymbol::VariableDeclaration>(r.declaration->data);
			const size_t start = outText.size();
			outText += decl.name;
			appendSpan(symbol, outText, start, outSpans, slot); // span -> the reference itself, not the declaration
			break;
		}
		case ST::VariableDeclaration:
		{
			const DSLSymbol::VariableDeclaration& v = oc::get<DSLSymbol::VariableDeclaration>(symbol->data);
			if (v.isRef)
				outText += "ref ";
			if (!(compact && isFunctionParamContext))
			{
				renderSymbol(v.typeSymbol, compact, false, {}, outText, outSpans);
				outText += " ";
			}
			const size_t start = outText.size();
			outText += v.name;
			appendSpan(symbol, outText, start, outSpans, slot);
			if (v.initialValue != nullptr)
			{
				outText += " = ";
				renderSymbol(v.initialValue, compact, false,
					SlotRef{ SlotRef::Kind::VariableDeclarationInitialValue, symbol, symbol->line }, outText, outSpans);
			}
			break;
		}
		case ST::FunctionDeclaration:
		{
			const DSLSymbol::FunctionDeclaration& f = oc::get<DSLSymbol::FunctionDeclaration>(symbol->data);
			outText += "function ";
			size_t start = outText.size();
			outText += f.name;
			appendSpan(symbol, outText, start, outSpans, slot);
			outText += "(";
			for (size_t i = 0; i < f.parameterVarDeclarations.size(); ++i)
			{
				if (i > 0) outText += ", ";
				renderSymbol(f.parameterVarDeclarations[i], compact, /*isFunctionParamContext*/ true, {}, outText, outSpans);
			}
			outText += ")";
			if (!compact)
			{
				// Always a selectable span (Kind::FunctionReturnType), even when Void -- rendered blank in that
				// case, exactly like a blank statement placeholder, so a return type can be added (or changed)
				// at any time, on a brand-new function or a pre-existing one alike (there's no dedicated
				// return-type symbol; this always resolves back to the SAME FunctionDeclaration).
				const SlotRef returnSlot{ SlotRef::Kind::FunctionReturnType, symbol, symbol->line };
				if (f.returnType != DSLType::Void)
					outText += " -> ";
				start = outText.size();
				if (f.returnType != DSLType::Void)
					outText += dslTypeName(f.returnType);
				appendSpan(symbol, outText, start, outSpans, returnSlot);
			}
			break;
		}
		case ST::FunctionCall:
		{
			const DSLSymbol::FunctionCall& call = oc::get<DSLSymbol::FunctionCall>(symbol->data);
			if (call.receiver != nullptr)
			{
				renderSymbol(call.receiver, compact, false, {}, outText, outSpans);
				outText += ".";
			}
			const DSLSymbol::FunctionDeclaration& callee = oc::get<DSLSymbol::FunctionDeclaration>(call.functionSymbol->data);
			size_t start = outText.size();
			outText += callee.name;
			appendSpan(symbol, outText, start, outSpans, slot); // span -> the call site, not the callee's own declaration
			outText += "(";
			for (size_t i = 0; i < call.arguments.size(); ++i)
			{
				if (i > 0) outText += ", ";
				const DSLSymbol::CallArgument& arg = call.arguments[i];
				if (arg.parameter != nullptr)
				{
					const DSLSymbol::VariableDeclaration& param = oc::get<DSLSymbol::VariableDeclaration>(arg.parameter->data);
					if (param.isRef)
						outText += "ref ";
					if (!compact)
					{
						renderSymbol(param.typeSymbol, compact, false, {}, outText, outSpans);
						outText += " ";
					}
					// The parameter NAME is a real span onto the callee's own parameter declaration -- so
					// occurrence highlighting connects call sites with the declaration and every body usage,
					// and renaming here renames the parameter itself (same symbol; user-declared callees only,
					// see ScriptEditor::beginCompose's builtin guard). The " = " stays glue.
					const size_t nameStart = outText.size();
					outText += param.name;
					appendSpan(arg.parameter, outText, nameStart, outSpans);
					outText += " = ";
				}
				renderSymbol(arg.value, compact, false,
					SlotRef{ SlotRef::Kind::CallArgumentValue, symbol, symbol->line, static_cast<int>(i) }, outText, outSpans);
			}
			outText += ")";
			break;
		}
		case ST::FlowControl:
		{
			const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(symbol->data);
			if (fc.control == DSLFlowControl::For)
			{
				// Unlike every other FlowControl kind, "for" previously had no span of its own at all (only its
				// three clauses were selectable) -- now spanned like the rest, so it's a legitimate cursor stop
				// (needed so Backspace can target the for's own keyword, see ScriptEditor::handleKeyEvent).
				const size_t start = outText.size();
				outText += "for";
				appendSpan(symbol, outText, start, outSpans, slot);
				outText += " ";
				renderSymbol(fc.forLoopVar, compact, false, {}, outText, outSpans); // e.g. "int counter = 0"
				outText += ", ";
				renderSymbol(fc.forCondition, compact, false, {}, outText, outSpans);
				outText += ", ";
				renderSymbol(fc.forIncrement, compact, false, {}, outText, outSpans);
				break;
			}
			if (fc.control == DSLFlowControl::ForEach || fc.control == DSLFlowControl::IfExist)
			{
				// "foreach <type> <name> in <sequence>" / "ifexist <type> <name> in <source> [at <key>]" --
				// one shape: the bound declaration renders
				// like a function parameter ([ref] type + name, never an initial value -- the construct supplies
				// it), and the source is a plain value slot so it can be re-edited/replaced like any other.
				const size_t start = outText.size();
				outText += flowControlKeyword(fc.control);
				appendSpan(symbol, outText, start, outSpans, slot);
				outText += " ";
				renderSymbol(fc.forLoopVar, compact, /*isFunctionParamContext*/ true, {}, outText, outSpans);
				outText += " in ";
				renderSymbol(fc.condition, compact, false,
					SlotRef{ SlotRef::Kind::FlowControlCondition, symbol, symbol->line }, outText, outSpans);
				// The key rides in forCondition, in its own slot so it re-edits independently of the container
				// (absent for a container whose lookup takes none).
				if (fc.forCondition != nullptr)
				{
					outText += " at ";
					renderSymbol(fc.forCondition, compact, false,
						SlotRef{ SlotRef::Kind::IfExistKey, symbol, symbol->line }, outText, outSpans);
				}
				break;
			}
			const size_t start = outText.size();
			outText += flowControlKeyword(fc.control);
			appendSpan(symbol, outText, start, outSpans, slot);
			if (fc.condition != nullptr)
			{
				outText += " ";
				renderSymbol(fc.condition, compact, false,
					SlotRef{ SlotRef::Kind::FlowControlCondition, symbol, symbol->line }, outText, outSpans);
			}
			break;
		}
		case ST::Expression:
		{
			const DSLSymbol::Expression& e = oc::get<DSLSymbol::Expression>(symbol->data);

			// "ref" prefixes an assignment into a ref-parameter's reference (e.g. `ref appliedForce = toApply`
			// inside the function that owns it) -- distinct from a `ref` argument at a CALL site, handled above.
			if (!e.operators.empty() && dslIsAssignOperator(e.operators[0])
				&& !e.operands.empty() && e.operands[0]->type == ST::VariableReference)
			{
				const DSLSymbol::VariableReference& leftRef = oc::get<DSLSymbol::VariableReference>(e.operands[0]->data);
				if (oc::get<DSLSymbol::VariableDeclaration>(leftRef.declaration->data).isRef)
					outText += "ref ";
			}

			if (e.grouped)
				outText += "("; // unspanned glue -- grouping edits happen by deleting terms, never on the parens themselves
			for (size_t i = 0; i < e.operands.size(); ++i)
			{
				if (i > 0)
				{
					outText += " ";
					const size_t start = outText.size();
					outText += operatorText(e.operators[i - 1]);
					// The INCOMING slot (how this whole chain is replaced/deleted as a unit -- LineHead for an
					// assignment statement, FlowControlCondition for an if/while condition, ...) rides on the
					// FIRST operator span only, preserving the statement-level select/delete behaviors keyed on
					// it; operatorIndex additionally marks every operator span as an in-place replace target.
					appendSpan(symbol, outText, start, outSpans, (i == 1) ? slot : SlotRef{}, static_cast<int>(i - 1));
					outText += " ";
				}
				// The assignment TARGET (operand 0 under an assign-class operator) is not a value slot -- it
				// can't be swapped for a literal/call; changing what an assignment targets means deleting the
				// statement and re-authoring it (the staged Reassign flow).
				const bool assignTarget = (i == 0 && !e.operators.empty() && dslIsAssignOperator(e.operators[0]));
				renderSymbol(e.operands[i], compact, false,
					assignTarget ? SlotRef{} : SlotRef{ SlotRef::Kind::ExpressionOperand, symbol, symbol->line, static_cast<int>(i) },
					outText, outSpans);
			}
			if (e.grouped)
			{
				// The closing paren is its own cursor stop, selecting the GROUP as one unit (see
				// SyntaxSpan::groupClose) -- typing ')' on the group's last operand steps the selection out
				// here, ready to continue the chain past the parens (ScriptEditor::handleKeyEvent).
				const size_t start = outText.size();
				outText += ")";
				outSpans.push_back(SyntaxSpan{ symbol, static_cast<int>(start), static_cast<int>(outText.size()), slot, -1, true });
			}
			break;
		}
		case ST::MemberAccess:
		{
			const DSLSymbol::MemberAccess& m = oc::get<DSLSymbol::MemberAccess>(symbol->data);
			renderSymbol(m.receiver, compact, false, {}, outText, outSpans);
			outText += ".";
			const size_t start = outText.size();
			outText += m.memberName;
			appendSpan(symbol, outText, start, outSpans, slot);
			break;
		}
		case ST::Comment:
		{
			const size_t start = outText.size();
			outText += "# " + oc::get<DSLSymbol::Comment>(symbol->data).text;
			appendSpan(symbol, outText, start, outSpans, slot);
			break;
		}
		case ST::Placeholder:
		{
			const DSLSymbol::Placeholder& p = oc::get<DSLSymbol::Placeholder>(symbol->data);
			const size_t start = outText.size();
			// A statement placeholder renders as an empty span -- a blank line, like an empty line in any text
			// editor (Enter, not an ever-visible marker, is what creates one -- see ScriptEditor). A VALUE
			// placeholder still needs a visible marker since it sits inside an otherwise-real line of code.
			if (p.expectedType != DSLType::Void)
				outText += "<" + oc::string(dslTypeName(p.expectedType)) + ">";
			appendSpan(symbol, outText, start, outSpans, slot);
			break;
		}
		}
	}

	void renderLine(DSLCodeLine& line, bool compact, oc::string& outText, oc::vector<SyntaxSpan>& outSpans)
	{
		if (line.head() != nullptr)
			renderSymbol(line.head(), compact, false, SlotRef{ SlotRef::Kind::LineHead, nullptr, &line }, outText, outSpans);
	}

	bool isElseContinuation(const DSLSymbol* head)
	{
		if (head == nullptr || head->type != ST::FlowControl)
			return false;
		const DSLFlowControl control = oc::get<DSLSymbol::FlowControl>(head->data).control;
		return control == DSLFlowControl::ElseIf || control == DSLFlowControl::Else;
	}

	SyntaxLine makeEndLine(int scopeLevel, DSLSymbol* headerSymbol)
	{
		SyntaxLine endLine;
		endLine.scopeLevel = scopeLevel;
		endLine.endOfSymbol = headerSymbol;
		endLine.text = "end";
		endLine.spans.push_back(SyntaxSpan{ headerSymbol, 0, 3, {} });
		return endLine;
	}

	// ---- AutoCompleteRules helpers ----

	bool matchesPrefix(const oc::string& label, const oc::string& prefix)
	{
		if (prefix.size() > label.size())
			return false;
		for (size_t i = 0; i < prefix.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(label[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
				return false;
		return true;
	}

	void addIfMatches(oc::vector<Candidate>& out, Candidate candidate, const oc::string& typedPrefix)
	{
		if (matchesPrefix(candidate.label, typedPrefix))
			out.push_back(oc::move(candidate));
	}

	bool equalsCaseInsensitive(const oc::string& a, const oc::string& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
				return false;
		return true;
	}

	// Moves a candidate whose label matches `typedPrefix` EXACTLY (not just as a prefix) to the front of the
	// list, preserving every other candidate's relative order -- so finishing a name to an exact match (e.g.
	// typing "test" when both "test" and "test2" are in scope) always surfaces it first, ready to confirm,
	// instead of wherever it happened to fall out of the underlying scan order.
	void sortExactMatchFirst(oc::vector<Candidate>& candidates, const oc::string& typedPrefix)
	{
		if (typedPrefix.empty())
			return;
		const auto it = oc::find_if(candidates.begin(), candidates.end(),
			[&](const Candidate& c) { return equalsCaseInsensitive(c.label, typedPrefix); });
		if (it != candidates.end() && it != candidates.begin())
			oc::rotate(candidates.begin(), it, it + 1);
	}

	// Whether `text` reads as a real Int/Float literal -- so "bla" is never offered (or accepted) as a numeric
	// value just because a variable/function of that name doesn't happen to exist either. An optional leading
	// sign, digits, and (Float only) at most one '.' with at least one digit somewhere.
	bool looksLikeIntLiteral(const oc::string& text)
	{
		if (text.empty())
			return false;
		size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
		if (i >= text.size())
			return false;
		for (; i < text.size(); ++i)
			if (!std::isdigit(static_cast<unsigned char>(text[i])))
				return false;
		return true;
	}

	bool looksLikeFloatLiteral(const oc::string& text)
	{
		if (text.empty())
			return false;
		size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
		bool sawDigit = false, sawDot = false;
		for (; i < text.size(); ++i)
		{
			if (std::isdigit(static_cast<unsigned char>(text[i]))) { sawDigit = true; continue; }
			if (text[i] == '.' && !sawDot) { sawDot = true; continue; }
			return false;
		}
		return sawDigit;
	}

	bool isValidLiteralTextImpl(DSLType type, const oc::string& text)
	{
		// A string literal is QUOTED, exactly once: `"..."` -- an unquoted word in a string slot names a
		// variable/function, never a literal (the quotes are what disambiguate `s2 = s1` from `s2 = "s1"`).
		if (type == DSLType::String)
			return text.size() >= 2 && text.front() == '"' && text.back() == '"'
				&& text.find('"', 1) == text.size() - 1;
		if (type == DSLType::Int)
			return looksLikeIntLiteral(text);
		if (type == DSLType::Float)
			return looksLikeFloatLiteral(text);
		return false;
	}

	// Every VariableDeclaration reachable from `atLine`: sidebar bindings, this function's own parameters, and
	// locals declared earlier in the SAME function (walking file.lines backward to the enclosing
	// FunctionDeclaration header, whose own parameters are picked up there too) -- but only through blocks
	// atLine is CURRENTLY nested inside: a line deeper than atLine's own scopeLevel is a closed/sibling block
	// (a finished loop, an inactive if-branch) once atLine isn't inside it too, so its locals are skipped
	// entirely (jumping straight to just before that block's own header). A for-loop's own counter gets the
	// SAME treatment one level up: it's declared ON the header line itself (at the header's own, shallower
	// scopeLevel -- see commitForStatement), but scoped to just its body, so it's excluded once atLine sits at
	// or past the loop's matching `end`.
	oc::vector<DSLSymbol*> inScopeVariables(const DSLCodeLine& atLine, const DSLScriptFile& file, const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar)
	{
		oc::vector<DSLSymbol*> result;
		for (const oc::unique_ptr<DSLSymbol>& s : sidebar)
			if (s->type == ST::VariableDeclaration)
				result.push_back(s.get());

		const int atIndex = dslLineIndex(file, &atLine);
		for (int i = atIndex; i >= 0; --i)
		{
			const DSLCodeLine& line = *file.lines[i];
			if (line.scopeLevel > atLine.scopeLevel)
			{
				const int headerIndex = dslEnclosingBlockHeader(file, i);
				if (headerIndex < 0)
					break; // shouldn't happen (a deeper line always has an enclosing header) -- bail safely
				if (atIndex >= dslBlockEnd(file, headerIndex))
				{
					i = headerIndex; // the loop's own --i steps to headerIndex - 1 next
					continue;
				}
				// atLine is genuinely nested inside this same block -- its declarations count, fall through.
			}
			// A block header that declares its OWN variable (a for counter, a foreach element, an ifexist
			// binding) declares it on the header line -- at the header's own, shallower scopeLevel -- but scopes
			// it to just the body, so it goes out of scope at the block's end like anything inside it. For
			// ifexist that end is where an `else` begins, which is exactly what keeps the bound value
			// unreachable there (C++ would leave it in scope across both branches; the DSL must not).
			const DSLSymbol* head = line.head();
			if (head != nullptr && head->type == ST::FlowControl && atIndex >= dslBlockEnd(file, i))
			{
				const DSLFlowControl control = oc::get<DSLSymbol::FlowControl>(head->data).control;
				if (control == DSLFlowControl::For || control == DSLFlowControl::ForEach
					|| control == DSLFlowControl::IfExist)
					continue;
			}
			for (const oc::unique_ptr<DSLSymbol>& s : line.symbols)
				if (s->type == ST::VariableDeclaration)
					result.push_back(s.get());
			if (line.head() != nullptr && line.head()->type == ST::FunctionDeclaration)
				break; // reached this function's own header (its params were just picked up above) -- stop
		}
		return result;
	}

	// Free-callable functions only: receiver-based builtins (physics.getVelocity(), world.rayCast(...) --
	// FunctionDeclaration::requiresReceiver) are excluded, since a plain Function candidate always inserts a
	// receiver-less call (see ScriptEditor::applyCandidate) -- offering one here would silently produce a
	// wrong, receiver-less call to a method that needs one. Dot-calls are only reachable by dotting into their
	// receiver's BindingObject (receiverCandidates, below).
	oc::vector<DSLSymbol*> knownFunctions(const DSLScriptFile& file, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins)
	{
		oc::vector<DSLSymbol*> result;
		for (const oc::unique_ptr<DSLSymbol>& s : builtins)
			if (s->type == ST::FunctionDeclaration && !oc::get<DSLSymbol::FunctionDeclaration>(s->data).requiresReceiver)
				result.push_back(s.get());
		for (const oc::unique_ptr<DSLCodeLine>& line : file.lines)
			if (line->head() != nullptr && line->head()->type == ST::FunctionDeclaration)
				result.push_back(line->head());
		return result;
	}

	// The two candidate-list builders every "offer values here" branch shares -- one construction point for
	// Variable/Reassign/Function candidates instead of a copy of these loops per calling context. `accept`
	// filters by the variable's declared type / the function's return type (always-true lambda = "any").
	// Whether a foreach/ifexist in a block ENCLOSING atLine is reading the storage named by (root, path) --
	// what makes a container-mutating call (push/clear) refuse to be offered/authored there. See
	// FunctionDeclaration::mutatesContainer: a diagnostic for a logic mistake, not a safety mechanism.
	bool isContainerIterated(const DSLSymbol* root, const oc::string& path, const DSLCodeLine& atLine,
		const DSLScriptFile& file)
	{
		if (root == nullptr)
			return false;
		const int atIndex = dslLineIndex(file, &atLine);
		if (atIndex < 0)
			return false;
		for (int i = atIndex - 1; i >= 0; --i)
		{
			if (file.lines[i]->scopeLevel >= atLine.scopeLevel)
				continue; // not an enclosing header -- a sibling or something deeper
			const DSLSymbol* head = file.lines[i]->head();
			if (head == nullptr || head->type != ST::FlowControl)
				continue;
			const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(head->data);
			if (fc.control == DSLFlowControl::ForEach || fc.control == DSLFlowControl::IfExist)
			{
				const DSLSymbol* loopRoot = nullptr;
				oc::string loopPath;
				if (dslChainToRoot(fc.condition, loopRoot, loopPath) && loopRoot == root && loopPath == path)
					return true;
			}
		}
		return false;
	}

	void addVariableCandidates(oc::vector<Candidate>& out, const DSLCodeLine& atLine, const DSLScriptFile& file,
		const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::string& typedPrefix,
		Candidate::Kind kind, DSLSymbol* excludeVariable, auto&& accept)
	{
		for (DSLSymbol* var : inScopeVariables(atLine, file, sidebar))
		{
			if (var == excludeVariable)
				continue;
			const DSLSymbol::VariableDeclaration& v = oc::get<DSLSymbol::VariableDeclaration>(var->data);
			const DSLType varType = oc::get<DSLSymbol::TypeDeclaration>(v.typeSymbol->data).type;
			// SIDEBAR bindings (self/physics/...) are never plain values or assignment targets -- they're
			// offered as Kind::BindingObject dot-into candidates instead (ScriptEditor appends those, gated on
			// the script's required-components set). Told apart from a local by having no owning line: a
			// sidebar symbol belongs to the registry, not to any statement.
			//
			// ONE exception, in the VALUE pass only: `self` is a genuine Entity, and Entity is a real value type
			// -- it's what `ifexist ... in self` takes, what an Entity parameter accepts, and what an
			// identity comparison names. It stays un-assignable (never offered for Kind::Reassign), and the
			// component bindings stay dot-into-only since a component isn't a value at all.
			//
			// A LOCAL of an engine-object type is a different thing entirely and IS offered -- a foreach
			// element ("foreach Entity e in ...") is an ordinary variable that happens to be Entity-typed, and
			// excluding it would leave it unusable: unnameable as a value, and undottable for its members.
			if (dslIsEngineObjectType(varType) && var->line == nullptr
				&& !(varType == DSLType::Entity && kind == Candidate::Kind::Variable))
				continue;
			// A foreach/ifexist element binds a COPY unless declared `ref` (see the Transpiler's write-back), so
			// assigning to a non-ref one would compile and then be silently discarded at the end of the block --
			// the exact footgun `ref` exists to make visible. Refuse it as an assignment target; the fix the
			// author wants is to add `ref`. Entity elements are handles, not copies, so they stay writable.
			if (kind == Candidate::Kind::Reassign && !v.isRef && !dslIsEngineObjectType(varType)
				&& dslIsElementBinding(var))
				continue;
			if (!accept(varType))
				continue;
			Candidate c;
			c.label = v.name;
			c.kind = kind;
			c.refSymbol = var;
			addIfMatches(out, c, typedPrefix);
		}
	}

	// `includeParameterized` is false only where a call's arguments can't be staged before it lands -- the
	// editor stages arguments for call STATEMENTS and for call VALUES inside chain composes (ScriptEditor's
	// CallArgValue flow), but not, e.g., for another call's own argument slot (no nested staging), and
	// placeholder arguments never land in the document.
	void addFunctionCandidates(oc::vector<Candidate>& out, const DSLScriptFile& file,
		const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins, const oc::string& typedPrefix,
		bool includeParameterized, auto&& accept)
	{
		for (DSLSymbol* func : knownFunctions(file, builtins))
		{
			const DSLSymbol::FunctionDeclaration& f = oc::get<DSLSymbol::FunctionDeclaration>(func->data);
			if (!accept(f.returnType))
				continue;
			if (!includeParameterized && !f.parameterVarDeclarations.empty())
				continue;
			Candidate c;
			c.label = f.name;
			c.kind = Candidate::Kind::Function;
			c.refSymbol = func;
			addIfMatches(out, c, typedPrefix);
		}
	}
}

const char* dslOperatorText(DSLOperator op)
{
	return operatorText(op);
}

bool Syntax::isBlockOpener(const DSLSymbol* head)
{
	using ST = DSLSymbol::SymbolType;

	if (head == nullptr)
		return false;
	if (head->type == ST::FunctionDeclaration)
		return true;
	if (head->type == ST::FlowControl)
	{
		const DSLFlowControl control = oc::get<DSLSymbol::FlowControl>(head->data).control;
		return control == DSLFlowControl::If || control == DSLFlowControl::ElseIf
			|| control == DSLFlowControl::Else || control == DSLFlowControl::While
			|| control == DSLFlowControl::For || control == DSLFlowControl::ForEach
			|| control == DSLFlowControl::IfExist;
	}
	return false;
}

oc::vector<SyntaxLine> Syntax::format(DSLScriptFile& file, bool compact)
{
	oc::vector<SyntaxLine> out;

	struct OpenBlock { int scopeLevel; DSLSymbol* headerSymbol; };
	oc::vector<OpenBlock> stack;

	for (oc::unique_ptr<DSLCodeLine>& linePtr : file.lines)
	{
		DSLCodeLine& line = *linePtr;
		DSLSymbol* head = line.head();
		const bool continuation = isElseContinuation(head);

		// Close every open block at least as deep as this line, EXCEPT the one this line continues (an
		// ElseIf/Else continues the innermost open If at the SAME scopeLevel rather than closing it).
		while (!stack.empty() && stack.back().scopeLevel >= line.scopeLevel)
		{
			if (continuation && stack.back().scopeLevel == line.scopeLevel)
				break;
			out.push_back(makeEndLine(stack.back().scopeLevel, stack.back().headerSymbol));
			stack.pop_back();
		}

		SyntaxLine syntaxLine;
		syntaxLine.sourceLine = &line;
		syntaxLine.scopeLevel = line.scopeLevel;
		renderLine(line, compact, syntaxLine.text, syntaxLine.spans);
		out.push_back(oc::move(syntaxLine));

		if (isBlockOpener(head) && !continuation)
			stack.push_back({ line.scopeLevel, head });
	}

	while (!stack.empty())
	{
		out.push_back(makeEndLine(stack.back().scopeLevel, stack.back().headerSymbol));
		stack.pop_back();
	}

	return out;
}

oc::vector<Candidate> AutoCompleteRules::candidatesFor(DSLType expectedType, const DSLCodeLine& atLine, const DSLScriptFile& file,
	const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
	const oc::string& typedPrefix, DSLSymbol* excludeVariable, bool offerComparisonLeads)
{
	oc::vector<Candidate> out;

	if (expectedType == DSLType::Void)
	{
		// Statement slot: control-flow keywords, "declare a new variable of type X" per known type, and
		// in-scope function names (a free call statement).
		addIfMatches(out, Candidate{ "if", Candidate::Kind::KeywordIf }, typedPrefix);
		addIfMatches(out, Candidate{ "while", Candidate::Kind::KeywordWhile }, typedPrefix);
		addIfMatches(out, Candidate{ "for", Candidate::Kind::KeywordFor }, typedPrefix);
		addIfMatches(out, Candidate{ "foreach", Candidate::Kind::KeywordForEach }, typedPrefix);
		addIfMatches(out, Candidate{ "ifexist", Candidate::Kind::KeywordIfExist }, typedPrefix);
		addIfMatches(out, Candidate{ "return", Candidate::Kind::KeywordReturn }, typedPrefix);
		addIfMatches(out, Candidate{ "break", Candidate::Kind::KeywordBreak }, typedPrefix);

		// else/elseif: only meaningful on a statement INSIDE an if/elseif branch, and only when it's the LAST
		// line of that branch -- statements below would silently stay in the current branch while the new one
		// opens after them, which never reads as intended. Confirming grows the chain with a new branch (see
		// ScriptEditor's KeywordElse/ElseIf handling); `else` additionally needs the chain to not already end
		// in one (a second else has no place to go).
		const int lineIndex = dslLineIndex(file, &atLine);
		int headerIndex = (lineIndex >= 0 && atLine.scopeLevel > 0) ? dslEnclosingBlockHeader(file, lineIndex) : -1;
		if (headerIndex >= 0 && lineIndex != dslBlockEnd(file, headerIndex) - 1)
			headerIndex = -1; // not the branch's last line -- something below would be stranded
		const DSLSymbol* branchHead = (headerIndex >= 0) ? file.lines[headerIndex]->head() : nullptr;
		if (branchHead != nullptr && branchHead->type == ST::FlowControl)
		{
			const DSLFlowControl branchControl = oc::get<DSLSymbol::FlowControl>(branchHead->data).control;
			// `ifexist` takes an `else` (the thing wasn't there) but no `elseif`: an elseif carries a bool
			// condition, which has nothing to continue from a lookup that either found something or didn't.
			const bool chainable = branchControl == DSLFlowControl::If || branchControl == DSLFlowControl::ElseIf
				|| branchControl == DSLFlowControl::IfExist;
			if (chainable)
			{
				if (branchControl != DSLFlowControl::IfExist)
					addIfMatches(out, Candidate{ "elseif", Candidate::Kind::KeywordElseIf }, typedPrefix);
				bool chainHasElse = false;
				for (int i = headerIndex; !chainHasElse; )
				{
					const DSLFlowControl c = oc::get<DSLSymbol::FlowControl>(file.lines[i]->head()->data).control;
					if (c == DSLFlowControl::Else)
						chainHasElse = true;
					const int next = dslBlockEnd(file, i);
					if (chainHasElse || next >= static_cast<int>(file.lines.size())
						|| file.lines[next]->scopeLevel != file.lines[headerIndex]->scopeLevel)
						break;
					const DSLSymbol* nextHead = file.lines[next]->head();
					if (nextHead == nullptr || nextHead->type != ST::FlowControl)
						break;
					const DSLFlowControl nc = oc::get<DSLSymbol::FlowControl>(nextHead->data).control;
					if (nc != DSLFlowControl::ElseIf && nc != DSLFlowControl::Else)
						break;
					i = next;
				}
				if (!chainHasElse)
					addIfMatches(out, Candidate{ "else", Candidate::Kind::KeywordElse }, typedPrefix);
			}
		}

		// Declaring a new function is only well-formed at the top level (this DSL has no nested functions) --
		// atLine.scopeLevel == 0 is exactly "not inside any function body" (every scope-1+ line lives inside
		// some function's body, per the grammar; see DSL.ixx).
		if (atLine.scopeLevel == 0)
			addIfMatches(out, Candidate{ "function", Candidate::Kind::DeclareFunction }, typedPrefix);

		for (const Candidate& typeCandidate : typeKeywordCandidates(typedPrefix))
			out.push_back(typeCandidate);

		// Reassigning an EXISTING in-scope variable is also a statement candidate -- typing its name offers
		// "assign into it", the same way typing a function's name offers "call it" (any return type: a call
		// statement is free to ignore the result).
		const auto anyType = [](DSLType) { return true; };
		addVariableCandidates(out, atLine, file, sidebar, typedPrefix, Candidate::Kind::Reassign, nullptr, anyType);
		addFunctionCandidates(out, file, builtins, typedPrefix, /*includeParameterized*/ true, anyType);
		sortExactMatchFirst(out, typedPrefix);
		return out;
	}

	// Value slot of type `expectedType`.
	if (expectedType == DSLType::Bool)
	{
		addIfMatches(out, Candidate{ "true", Candidate::Kind::KeywordTrue }, typedPrefix);
		addIfMatches(out, Candidate{ "false", Candidate::Kind::KeywordFalse }, typedPrefix);
		if (offerComparisonLeads)
		{
			// Numeric leads for "bool b = i < 5" -- consumable only by a typed comparator (see the parameter
			// comment); no overlap with the Bool-typed candidates added below.
			const auto numeric = [](DSLType t) { return t == DSLType::Int || t == DSLType::Float; };
			addVariableCandidates(out, atLine, file, sidebar, typedPrefix, Candidate::Kind::Variable, excludeVariable, numeric);
			addFunctionCandidates(out, file, builtins, typedPrefix, /*includeParameterized*/ false, numeric);
		}
	}

	const auto matchesExpected = [&](DSLType type) { return type == expectedType; };
	addVariableCandidates(out, atLine, file, sidebar, typedPrefix, Candidate::Kind::Variable, excludeVariable, matchesExpected);
	addFunctionCandidates(out, file, builtins, typedPrefix, /*includeParameterized*/ true, matchesExpected);

	// Chainable variables that DON'T match the slot are still offered as dot-into WAYPOINTS ("test" in a Float
	// slot, toward "test.x"; a foreach's Entity element toward "e.pos") -- Kind::BindingObject, consumable only
	// by '.'/confirm into MemberSelect, never as the bare value (matching ones already appeared as plain
	// Variable candidates above).
	addVariableCandidates(out, atLine, file, sidebar, typedPrefix, Candidate::Kind::BindingObject, excludeVariable,
		[&](DSLType type) { return dslIsChainableType(type) && type != expectedType; });

	// Free-typed literal entry: only offered once what's typed actually reads as a value of the expected type
	// (any text is valid String content; Int/Float need looksLike*Literal to hold) -- "bla" must never become
	// a confirmable Float/Int constant just because no variable/function happens to be named that either.
	if (!typedPrefix.empty() && isValidLiteralTextImpl(expectedType, typedPrefix))
	{
		Candidate c;
		c.label = typedPrefix;
		c.kind = Candidate::Kind::Literal;
		c.declareType = expectedType;
		out.push_back(c);
	}

	sortExactMatchFirst(out, typedPrefix);
	return out;
}

bool AutoCompleteRules::isValidLiteralText(DSLType type, const oc::string& text)
{
	return isValidLiteralTextImpl(type, text);
}

oc::vector<Candidate> AutoCompleteRules::typeKeywordCandidates(const oc::string& typedPrefix)
{
	oc::vector<Candidate> out;
	auto add = [&](DSLType t)
	{
		Candidate c;
		c.label = dslTypeName(t);
		c.kind = Candidate::Kind::DeclareType;
		c.declareType = t;
		addIfMatches(out, c, typedPrefix);
	};
	for (DSLType t : { DSLType::Int, DSLType::Float, DSLType::Bool, DSLType::String })
		add(t);
	for (size_t i = 0; i < Globals::scriptBindings.structs().size(); ++i)
		add(dslStructType(static_cast<int>(i))); // every engine-defined struct is a declarable type
	sortExactMatchFirst(out, typedPrefix);
	return out;
}

bool AutoCompleteRules::isFunctionNameTaken(const oc::string& name, const DSLScriptFile& file, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
	DSLSymbol* excludeFunction)
{
	using ST = DSLSymbol::SymbolType;

	if (isReservedWord(name))
		return true;

	// Only BARE-callable builtins reserve a global name. A receiver-based one (a struct's `dot`/`length`, an
	// object's `getVelocity`, an array's `push`) is only ever reachable through its receiver -- it's excluded
	// from every plain candidate list (see candidatesForAnyValue) and the loader resolves a bare identifier
	// against requiresReceiver=false only -- so a local or function of the same name is never ambiguous, and its
	// generated C++ is a qualified/member call either way.
	for (const oc::unique_ptr<DSLSymbol>& s : builtins)
		if (s->type == ST::FunctionDeclaration && !oc::get<DSLSymbol::FunctionDeclaration>(s->data).requiresReceiver
			&& oc::get<DSLSymbol::FunctionDeclaration>(s->data).name == name)
			return true;
	for (const oc::unique_ptr<DSLCodeLine>& line : file.lines)
		if (line->head() != nullptr && line->head() != excludeFunction && line->head()->type == ST::FunctionDeclaration
			&& oc::get<DSLSymbol::FunctionDeclaration>(line->head()->data).name == name)
			return true;
	return false;
}

bool AutoCompleteRules::isVariableReferenced(const DSLSymbol* varDecl, const DSLScriptFile& file)
{
	using ST = DSLSymbol::SymbolType;

	if (varDecl == nullptr || varDecl->line == nullptr)
		return false;

	const int headerIndex = dslEnclosingFunctionHeader(file, dslLineIndex(file, varDecl->line));
	if (headerIndex < 0)
		return false;

	// Every symbol anywhere in the function's body is a peer in its owning line's flat `symbols` list (see
	// DSL.ixx's ownership model) -- no recursive tree-walk needed to find nested VariableReferences.
	const int blockEnd = dslBlockEnd(file, headerIndex);
	for (int i = headerIndex + 1; i < blockEnd; ++i)
		for (const oc::unique_ptr<DSLSymbol>& s : file.lines[i]->symbols)
			if (s->type == ST::VariableReference && oc::get<DSLSymbol::VariableReference>(s->data).declaration == varDecl)
				return true;

	return false;
}

bool AutoCompleteRules::isFunctionReferenced(const DSLSymbol* funcDecl, const DSLScriptFile& file)
{
	using ST = DSLSymbol::SymbolType;

	for (const oc::unique_ptr<DSLCodeLine>& line : file.lines)
		for (const oc::unique_ptr<DSLSymbol>& s : line->symbols)
			if (s->type == ST::FunctionCall && oc::get<DSLSymbol::FunctionCall>(s->data).functionSymbol == funcDecl)
				return true;
	return false;
}

bool AutoCompleteRules::isFunctionResultUsed(const DSLSymbol* funcDecl, const DSLScriptFile& file)
{
	using ST = DSLSymbol::SymbolType;

	// A call that is its own line's HEAD is a call statement -- the one shape that ignores the result. Any
	// other position (an initializer, an argument, an operand, a return's value) consumed it at its old type.
	for (const oc::unique_ptr<DSLCodeLine>& line : file.lines)
		for (const oc::unique_ptr<DSLSymbol>& s : line->symbols)
			if (s->type == ST::FunctionCall && oc::get<DSLSymbol::FunctionCall>(s->data).functionSymbol == funcDecl
				&& line->head() != s.get())
				return true;
	return false;
}

oc::vector<Candidate> AutoCompleteRules::candidatesForAnyValue(const DSLCodeLine& atLine, const DSLScriptFile& file,
	const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
	const oc::string& typedPrefix, DSLSymbol* excludeVariable, bool offerLiterals)
{
	oc::vector<Candidate> out;
	if (offerLiterals)
	{
		addIfMatches(out, Candidate{ "true", Candidate::Kind::KeywordTrue }, typedPrefix);
		addIfMatches(out, Candidate{ "false", Candidate::Kind::KeywordFalse }, typedPrefix);
		// The typed text names its own type -- see the declaration for why that's allowed here.
		for (DSLType literalType : { DSLType::String, DSLType::Int, DSLType::Float })
			if (!typedPrefix.empty() && isValidLiteralTextImpl(literalType, typedPrefix))
			{
				Candidate c;
				c.label = typedPrefix;
				c.kind = Candidate::Kind::Literal;
				c.declareType = literalType;
				out.push_back(c);
				break; // the checks are mutually exclusive; first match wins
			}
	}
	addVariableCandidates(out, atLine, file, sidebar, typedPrefix, Candidate::Kind::Variable, excludeVariable, [](DSLType) { return true; });
	// A statement-only (Void-returning) call has nothing to compare, so those are excluded here.
	addFunctionCandidates(out, file, builtins, typedPrefix, /*includeParameterized*/ true, [](DSLType type) { return type != DSLType::Void; });
	sortExactMatchFirst(out, typedPrefix);
	return out;
}

namespace
{
	// The one construction path every fixed operator list shares -- labels always come from operatorText, so
	// candidate labels can never drift from how the operator renders.
	oc::vector<Candidate> operatorCandidates(oc::initializer_list<DSLOperator> operators, Candidate::Kind kind, const oc::string& typedPrefix)
	{
		oc::vector<Candidate> out;
		for (DSLOperator op : operators)
		{
			Candidate c;
			c.label = operatorText(op);
			c.kind = kind;
			c.op = op;
			addIfMatches(out, c, typedPrefix);
		}
		sortExactMatchFirst(out, typedPrefix);
		return out;
	}
}

oc::vector<Candidate> AutoCompleteRules::receiverCandidates(const ScriptBindings& bindings, const DSL& document, DSLSymbol* receiverDecl,
	DSLType receiverType, DSLType expectedType, bool anyValue, bool receiverWritable, bool receiverIsRoot,
	const DSLCodeLine* atLine, const DSLScriptFile& file, const oc::string& receiverPath,
	const oc::string& typedPrefix)
{
	oc::vector<Candidate> out;

	// self.data: no functions, just the document's OWN fields (DSL::dataFields) -- always writable (persistent
	// data is meant to be read/write), type-filtered the same way any other member is. This is the one place a
	// read-only hop does NOT propagate (`receiverWritable` is deliberately ignored here): the `data` member
	// itself is read-only because the block can't be replaced wholesale, not because its contents are off
	// limits -- they're the script's own storage. ScriptLoader's parseStatement keeps the same exception.
	if (receiverType == DSLType::ScriptData)
	{
		const bool statementContext = expectedType == DSLType::Void && !anyValue;
		for (const DSLDataField& field : document.dataFields)
		{
			const bool accepted = statementContext
				|| (anyValue || field.type == expectedType || dslIsChainableType(field.type));
			if (!accepted)
				continue;
			Candidate c;
			c.label = field.name;
			c.kind = Candidate::Kind::Member;
			c.refSymbol = receiverDecl;
			c.declareType = field.type;
			c.memberWritable = true;
			addIfMatches(out, c, typedPrefix);
		}
		sortExactMatchFirst(out, typedPrefix);
		return out;
	}

	// self.events: one read-only Int per document.eventNames entry (its index) -- never offered in statement
	// context (not writable, and Int isn't chainable, so there's no valid bare-statement form at all).
	if (receiverType == DSLType::ScriptEvents)
	{
		const bool statementContext = expectedType == DSLType::Void && !anyValue;
		for (const oc::string& name : document.eventNames)
		{
			const bool accepted = !statementContext && (anyValue || expectedType == DSLType::Int);
			if (!accepted)
				continue;
			Candidate c;
			c.label = name;
			c.kind = Candidate::Kind::Member;
			c.refSymbol = receiverDecl;
			c.declareType = DSLType::Int;
			c.memberWritable = false;
			addIfMatches(out, c, typedPrefix);
		}
		sortExactMatchFirst(out, typedPrefix);
		return out;
	}

	// A receiver is a binding OBJECT (physics/self) or any STRUCT-typed value -- same candidate shapes either
	// way, sourced from the matching registry side.
	const oc::vector<BindingFunc>* functions = nullptr;
	const oc::vector<BindingMember>* members = nullptr;
	oc::span<DSLSymbol* const> functionSymbols;
	if (const BindingObject* object = bindings.objectFor(receiverType); object != nullptr)
	{
		functions = &object->functions;
		members = &object->members;
		functionSymbols = bindings.functionSymbols(*object);
	}
	else if (const BindingStruct* structDef = bindings.structFor(receiverType); structDef != nullptr)
	{
		functions = &structDef->functions;
		members = &structDef->members;
		functionSymbols = bindings.structFunctionSymbols(receiverType);
	}
	else
		return out;

	const bool statementContext = expectedType == DSLType::Void && !anyValue;
	// A call that MUTATES this container is refused while a foreach/ifexist above is reading it (see
	// FunctionDeclaration::mutatesContainer) -- the editor twin of ScriptLoader's checkContainerNotIterated.
	const bool iterated = atLine != nullptr && isContainerIterated(receiverDecl, receiverPath, *atLine, file);
	for (size_t i = 0; i < functions->size(); ++i)
	{
		const DSLType returnType = (*functions)[i].returnType;
		const bool accepted = statementContext
			|| (anyValue ? returnType != DSLType::Void : returnType == expectedType);
		if (!accepted)
			continue;
		if (iterated && (*functions)[i].mutatesContainer)
			continue;
		Candidate c;
		c.label = (*functions)[i].name;
		c.kind = Candidate::Kind::Function;
		c.refSymbol = functionSymbols[i];
		c.receiver = receiverDecl;
		addIfMatches(out, c, typedPrefix);
	}
	// A component member (self.physics) is the host-cached pointer for THIS script's own entity -- valid only
	// on `self` ITSELF, and only while //@@require guarantees the component is there. Any other entity's
	// component is reached with `ifexist`, which handles the "it might not have one" case structurally.
	// Sidebar symbols have no owning line (see addVariableCandidates), and self is the only entity-typed one --
	// but that alone isn't enough: `self.parent` is Entity-typed and rooted at the same sidebar symbol, so the
	// path must be EMPTY too, or `self.parent.physics` would offer self's own component for another entity.
	const bool receiverIsSelf = receiverDecl != nullptr && receiverDecl->line == nullptr && receiverIsRoot;
	for (const BindingMember& member : *members)
	{
		if (member.selfOnly && !receiverIsSelf)
			continue;
		if (member.requiredComponent != DSLType::Void && !dslIsComponentRequired(document, member.requiredComponent))
			continue;
		// A member is writable only if the whole path to it is (see `receiverWritable`) -- everything reachable
		// through a read-only member is read-only too.
		const bool writable = receiverWritable && member.writable;
		// Value contexts type-filter (chainable members always pass, as dot-into waypoints toward a matching
		// leaf); a STATEMENT context offers WRITABLE members (the lead-in of a member-assignment, `v.x = ...`)
		// plus chainable-but-unwritable ones (`self.physics`, a waypoint toward a Void-returning dot-call).
		const bool accepted = statementContext ? (writable || dslIsChainableType(member.type))
			: (anyValue || member.type == expectedType || dslIsChainableType(member.type));
		if (!accepted)
			continue;
		Candidate c;
		c.label = member.name;
		c.kind = Candidate::Kind::Member;
		c.refSymbol = receiverDecl; // the chain's ROOT -- the member itself has no symbol, only registry data
		c.declareType = member.type;
		c.memberWritable = writable;
		addIfMatches(out, c, typedPrefix);
	}

	sortExactMatchFirst(out, typedPrefix);
	return out;
}

bool AutoCompleteRules::isEqualityOnlyType(DSLType type)
{
	// Engine objects are compared by identity (or against null), never ordered. Struct types have no comparison
	// story at all today, so they're left alone here -- nothing offers them as a comparison operand.
	return dslIsEngineObjectType(type);
}

oc::vector<Candidate> AutoCompleteRules::comparisonOperatorCandidates(const oc::string& typedPrefix, bool equalityOnly)
{
	if (equalityOnly)
		return operatorCandidates({ DSLOperator::Equal, DSLOperator::NotEqual }, Candidate::Kind::Comparator, typedPrefix);
	return operatorCandidates({ DSLOperator::Equal, DSLOperator::NotEqual, DSLOperator::LessThanOrEqual,
		DSLOperator::GreaterThanOrEqual, DSLOperator::LessThan, DSLOperator::GreaterThan },
		Candidate::Kind::Comparator, typedPrefix);
}

oc::vector<Candidate> AutoCompleteRules::compoundAssignOperatorCandidates(const oc::string& typedPrefix)
{
	return operatorCandidates({ DSLOperator::AssignAdd, DSLOperator::AssignSubtract, DSLOperator::AssignMultiply,
		DSLOperator::AssignDivide, DSLOperator::AssignModulus },
		Candidate::Kind::AssignOperator, typedPrefix);
}

oc::vector<Candidate> AutoCompleteRules::arithmeticOperatorCandidates(const oc::string& typedPrefix)
{
	return operatorCandidates({ DSLOperator::Add, DSLOperator::Subtract, DSLOperator::Multiply,
		DSLOperator::Divide, DSLOperator::Modulus },
		Candidate::Kind::ArithmeticOperator, typedPrefix);
}

oc::vector<Candidate> AutoCompleteRules::logicalOperatorCandidates(const oc::string& typedPrefix)
{
	return operatorCandidates({ DSLOperator::And, DSLOperator::Or }, Candidate::Kind::LogicalOperator, typedPrefix);
}

oc::vector<Candidate> AutoCompleteRules::assignOperatorCandidates(const oc::string& typedPrefix)
{
	return operatorCandidates({ DSLOperator::Assign, DSLOperator::AssignAdd, DSLOperator::AssignSubtract,
		DSLOperator::AssignMultiply, DSLOperator::AssignDivide, DSLOperator::AssignModulus },
		Candidate::Kind::AssignOperator, typedPrefix);
}

DSLType AutoCompleteRules::enclosingFunctionReturnType(const DSLCodeLine& atLine, const DSLScriptFile& file)
{
	const int headerIndex = dslEnclosingFunctionHeader(file, dslLineIndex(file, &atLine));
	if (headerIndex < 0)
		return DSLType::Void;
	return oc::get<DSLSymbol::FunctionDeclaration>(file.lines[headerIndex]->head()->data).returnType;
}

DSLType AutoCompleteRules::expectedTypeForSlot(const SlotRef& slot, const DSLScriptFile& file)
{
	using ST = DSLSymbol::SymbolType;

	switch (slot.kind)
	{
	case SlotRef::Kind::LineHead:
		return DSLType::Void;

	case SlotRef::Kind::FlowControlCondition:
	{
		const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(slot.parent->data);
		if (fc.control == DSLFlowControl::Return)
			return slot.line != nullptr ? enclosingFunctionReturnType(*slot.line, file) : DSLType::Void;
		if (fc.control == DSLFlowControl::ForEach)
		{
			// The sequence, not a condition: an ARRAY of the element type the loop declares. A registered
			// BindingSequence (self.scene.getChildren()) isn't array-typed, so re-editing that slot in place
			// isn't offered -- the whole header re-authors through the staged flow instead.
			if (fc.forLoopVar == nullptr)
				return DSLType::Void;
			const DSLSymbol::VariableDeclaration& elem = oc::get<DSLSymbol::VariableDeclaration>(fc.forLoopVar->data);
			return dslArrayOf(oc::get<DSLSymbol::TypeDeclaration>(elem.typeSymbol->data).type);
		}
		if (fc.control == DSLFlowControl::IfExist)
		{
			// The SOURCE. A component is fetched off an ENTITY; otherwise, like ForEach's sequence, an array of
			// the bound type is the only shape re-editable in place -- any other registered lookup (an optional,
			// an engine collection) re-authors through the staged flow instead.
			if (fc.forLoopVar == nullptr)
				return DSLType::Void;
			const DSLSymbol::VariableDeclaration& elem = oc::get<DSLSymbol::VariableDeclaration>(fc.forLoopVar->data);
			const DSLType boundType = oc::get<DSLSymbol::TypeDeclaration>(elem.typeSymbol->data).type;
			return dslIsComponentType(boundType) ? DSLType::Entity : dslArrayOf(boundType);
		}
		return DSLType::Bool; // If/ElseIf/While
	}

	case SlotRef::Kind::IfExistKey:
	{
		// The key type is the CONTAINER's, not the element's (Int for an array index, whatever a map declares).
		const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(slot.parent->data);
		const BindingObject* container = (fc.condition != nullptr)
			? Globals::scriptBindings.objectFor(dslValueType(fc.condition)) : nullptr;
		return container != nullptr ? container->lookupKeyType : DSLType::Void;
	}

	case SlotRef::Kind::CallArgumentValue:
	{
		const DSLSymbol::FunctionCall& call = oc::get<DSLSymbol::FunctionCall>(slot.parent->data);
		if (slot.argIndex < 0 || slot.argIndex >= static_cast<int>(call.arguments.size()))
			return DSLType::Void;
		const DSLSymbol* paramSym = call.arguments[slot.argIndex].parameter;
		if (paramSym == nullptr)
		{
			// Positional argument (vec3(0, 1, 0)) -- fall back to the CALLEE's parameter list by index, so e.g.
			// a vector literal's components still edit as the Floats they are.
			const DSLSymbol::FunctionDeclaration& callee = oc::get<DSLSymbol::FunctionDeclaration>(call.functionSymbol->data);
			if (slot.argIndex >= static_cast<int>(callee.parameterVarDeclarations.size()))
				return DSLType::Void; // past the callee's declared count -- shouldn't occur via the editor
			paramSym = callee.parameterVarDeclarations[slot.argIndex];
		}
		const DSLSymbol::VariableDeclaration& param = oc::get<DSLSymbol::VariableDeclaration>(paramSym->data);
		return oc::get<DSLSymbol::TypeDeclaration>(param.typeSymbol->data).type;
	}

	case SlotRef::Kind::VariableDeclarationInitialValue:
	{
		const DSLSymbol::VariableDeclaration& v = oc::get<DSLSymbol::VariableDeclaration>(slot.parent->data);
		return oc::get<DSLSymbol::TypeDeclaration>(v.typeSymbol->data).type;
	}

	case SlotRef::Kind::ExpressionOperand:
	{
		// The chain's element type: for an assignment every operand matches the TARGET's own type; for a
		// comparison or arithmetic chain, whatever any OTHER operand resolves to (all operands of one chain
		// share a type by construction -- skipping the operand being edited keeps its current occupant from
		// deciding its own replacement's type).
		const DSLSymbol::Expression& e = oc::get<DSLSymbol::Expression>(slot.parent->data);
		for (size_t i = 0; i < e.operands.size(); ++i)
		{
			if (static_cast<int>(i) == slot.argIndex)
				continue;
			if (const DSLType t = dslValueType(e.operands[i]); t != DSLType::Void)
				return t;
		}
		return dslValueType(slot.parent); // last resort -- includes the edited operand itself
	}

	default:
		return DSLType::Void;
	}
}

namespace
{
	// First index PAST the whole if/elseif/else chain opened at `headerIndex` -- dslBlockEnd only reaches the
	// end of that one BRANCH, since a sibling `else` sits at the header's own scopeLevel.
	int chainEnd(const DSLScriptFile& file, int headerIndex)
	{
		const int level = file.lines[headerIndex]->scopeLevel;
		for (int i = headerIndex; ; )
		{
			const int next = dslBlockEnd(file, i);
			if (next >= static_cast<int>(file.lines.size()) || file.lines[next]->scopeLevel != level)
				return next;
			const DSLSymbol* head = file.lines[next]->head();
			if (head == nullptr || head->type != ST::FlowControl)
				return next;
			const DSLFlowControl c = oc::get<DSLSymbol::FlowControl>(head->data).control;
			if (c != DSLFlowControl::ElseIf && c != DSLFlowControl::Else)
				return next;
			i = next;
		}
	}

	// Whether `name` is bound by an `ifexist` whose CHAIN encloses `atIndex`. Deliberately wider than what
	// inScopeVariables makes readable: the binding is readable only in the construct's own branch, but it stays
	// RESERVED across the sibling `else` too, because the generated C++ scopes the condition declaration over
	// the entire if/else -- redeclaring the name in the else would silently shadow it there.
	bool isChainBoundName(const oc::string& name, const DSLScriptFile& file, int atIndex, DSLSymbol* excludeVariable)
	{
		for (int i = atIndex; i >= 0; --i)
		{
			const DSLSymbol* head = file.lines[i]->head();
			if (head == nullptr || head->type != ST::FlowControl)
				continue;
			const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(head->data);
			if (fc.control != DSLFlowControl::IfExist || fc.forLoopVar == nullptr || fc.forLoopVar == excludeVariable)
				continue;
			if (atIndex < chainEnd(file, i)
				&& oc::get<DSLSymbol::VariableDeclaration>(fc.forLoopVar->data).name == name)
				return true;
		}
		return false;
	}
}

bool AutoCompleteRules::isReservedWord(const oc::string& name)
{
	// Every fixed word the grammar's statement/expression parsers (ScriptLang.cpp's flowControlKeyword,
	// ScriptLoader.cpp's matching literal checks) match LITERALLY -- a name equal to one of these would silently
	// misparse as the keyword instead of an identifier once the document round-trips through the loader's
	// plain-text parser. "end" is deliberately NOT here: it's never a real stored symbol (DSL.ixx -- purely a
	// synthetic render/load artifact recognized only as a whole line consisting of exactly that one token, see
	// ScriptLoader.cpp), and no legitimate statement is ever just a bare identifier with nothing else (a
	// declaration needs a type before the name, an assignment an operator, a call parens), so a variable/
	// function actually named "end" can never produce that single-token line -- safe to allow.
	static const char* const kKeywords[] = {
		"if", "elseif", "else", "while", "for", "foreach", "ifexist", "in", "at", "ref",
		"return", "break", "function", "true", "false", "null",
		"ctx", "self", "ScriptData", "scriptData" // the Transpiler's auto-injected context parameter (see Transpiler::emitFunction)
	};
	for (const char* keyword : kKeywords)
		if (name == keyword)
			return true;

	// Type keywords -- the exact set typeKeywordCandidates offers (primitives + every registered struct), so
	// this stays in sync automatically as the struct registry grows.
	for (DSLType t : { DSLType::Int, DSLType::Float, DSLType::Bool, DSLType::String })
		if (name == dslTypeName(t))
			return true;
	for (const BindingStruct& structDef : Globals::scriptBindings.structs())
		if (name == structDef.name)
			return true;

	return false;
}

bool AutoCompleteRules::isNameInScope(const oc::string& name, const DSLCodeLine& atLine, const DSLScriptFile& file,
	const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
	DSLSymbol* excludeVariable)
{
	using ST = DSLSymbol::SymbolType;

	if (isReservedWord(name))
		return true;

	// Sidebar bindings are visible everywhere.
	for (const oc::unique_ptr<DSLSymbol>& s : sidebar)
		if (s->type == ST::VariableDeclaration && s.get() != excludeVariable
			&& oc::get<DSLSymbol::VariableDeclaration>(s->data).name == name)
			return true;

	// A variable can't shadow a function name either -- see isFunctionNameTaken for the same check in the
	// other direction (declaring/renaming a FUNCTION to an already-taken name).
	if (isFunctionNameTaken(name, file, builtins))
		return true;

	const int atIndex = dslLineIndex(file, &atLine);
	if (dslEnclosingFunctionHeader(file, atIndex) < 0)
		return false; // outside any function -- nothing else here applies

	// Backward: anything CURRENTLY reachable from atLine, via the SAME block-scoping rule inScopeVariables uses
	// (parameters included -- they're seeded there too, from the function header's own line).
	for (DSLSymbol* var : inScopeVariables(atLine, file, sidebar))
		if (var != excludeVariable && oc::get<DSLSymbol::VariableDeclaration>(var->data).name == name)
			return true;

	// An enclosing ifexist's binding is reserved even where it isn't readable -- see the helper.
	if (isChainBoundName(name, file, atIndex, excludeVariable))
		return true;

	// Forward: a declaration made AT atLine stays visible for the rest of ITS OWN enclosing block, nested
	// blocks included (they can always see outward) -- so anything declared from here through that block's
	// close, AT ANY DEPTH, collides too. This is what still blocks e.g. renaming an outer variable to match one
	// a LATER nested if-branch already declares (that inner name isn't reachable FROM atLine yet, so the
	// backward scan above wouldn't catch it) -- while no longer flagging reuse of a name from an ALREADY-CLOSED
	// sibling block (a finished loop's own counter, a past if-branch's own local), which the old whole-function
	// flat scan used to block too.
	const int enclosingHeader = dslEnclosingBlockHeader(file, atIndex);
	const int scopeEnd = dslBlockEnd(file, enclosingHeader >= 0 ? enclosingHeader : dslEnclosingFunctionHeader(file, atIndex));
	for (int i = atIndex; i < scopeEnd; ++i)
		for (const oc::unique_ptr<DSLSymbol>& s : file.lines[i]->symbols)
			if (s->type == ST::VariableDeclaration && s.get() != excludeVariable
				&& oc::get<DSLSymbol::VariableDeclaration>(s->data).name == name)
				return true;

	return false;
}
