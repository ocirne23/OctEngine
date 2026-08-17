module Script;

import Core;
import :DSL;
import :ScriptBindings;
import :ScriptLang; // dslOperatorText -- the one shared operator spelling
import :Transpiler;

namespace
{
	using ST = DSLSymbol::SymbolType;

	// The registry owns this mapping -- it builds the array emit templates against it too, and those template
	// arguments must agree with what's declared here or a `T[]` wouldn't compile (see ScriptBindings::build).
	const char* cppTypeName(DSLType type)
	{
		return Globals::scriptBindings.cppTypeName(type);
	}

	// The type-appropriate default an unresolved value falls back to -- a committed document never actually
	// holds one (the editor's no-placeholder rule refuses to confirm), but Placeholder/field-initializer emit
	// both want the SAME rule if one ever slips through, so it's one switch, not two.
	oc::string defaultValueText(DSLType type, const ScriptBindings& bindings)
	{
		if (dslIsArrayType(type))
			return "0"; // no array yet -- the first push creates one and writes its handle back (see OcArray)
		if (const BindingStruct* structDef = bindings.structFor(type); structDef != nullptr)
			return oc::string(structDef->cppName) + "()";
		switch (type)
		{
		case DSLType::Float:  return "0.0f";
		case DSLType::Bool:   return "false";
		case DSLType::String: return "\"\"";
		case DSLType::Entity: return "nullptr";
		default:              return "0";
		}
	}

	const DSLSymbol::VariableDeclaration& declOf(const DSLSymbol* varDecl)
	{
		return oc::get<DSLSymbol::VariableDeclaration>(varDecl->data);
	}

	DSLType declaredType(const DSLSymbol* varDecl)
	{
		return oc::get<DSLSymbol::TypeDeclaration>(declOf(varDecl).typeSymbol->data).type;
	}

	struct Emitter
	{
		const DSL& document;
		const ScriptBindings& bindings;
		oc::string out;
		int indent = 0;

		void emitLine(const oc::string& text)
		{
			if (!text.empty())
				out.append(static_cast<size_t>(indent), '\t');
			out += text;
			out += '\n';
		}

		// ---- expressions ----

		// A Float constant needs a real C++ float literal: "1" -> "1.0f", "0.5" -> "0.5f", "1." -> "1.0f".
		static oc::string floatLiteral(const oc::string& value)
		{
			oc::string text = value;
			if (text.find('.') == oc::string::npos)
				text += ".0";
			else if (text.back() == '.')
				text += '0';
			return text + "f";
		}

		static oc::string constantText(const DSLSymbol::Constant& c)
		{
			switch (c.type)
			{
			case DSLType::Float:  return floatLiteral(c.value);
			// Interned rather than emitted as a bare literal: a literal lives in the script DLL's .rdata and
			// dangles once that DLL is unloaded, so anything outliving the call (a persistent ScriptData
			// string, an array element) would break on the next hot-reload. See internString in ScriptAPI.h.
			case DSLType::String: return "ctx->internString(\"" + c.value + "\")"; // content stored unquoted, no escapes in the DSL
			// An Entity has no literal form in the DSL -- this is only the default an `ifexist` binding is
			// declared with before its lookup runs.
			case DSLType::Entity: return "nullptr";
			default:              return c.value;               // Int/Bool exactly as authored
			}
		}

		// EVERY placeholder an emit template can use, in one place -- the vocabulary itself, so adding one is a
		// field here plus a case below rather than a fourth near-identical scanner. Unused fields simply never
		// match: a call's template has no "$i", a sequence's `at` has no "$1", and neither has "$v".
		struct EmitArgs
		{
			oc::string receiver;             // "$r" -- the receiver's own emitted expression
			oc::vector<oc::string> args;    // "$1".."$9" -- by CALLEE-parameter position (a lookup's key is $1)
			oc::vector<oc::string> varargs; // "$*" -- a variadic callee's tail, each preceded by ", "
			oc::string index;                // "$i" -- a sequence walk's own index local
			oc::string boundVar;             // "$v" -- the variable a lookup writes through / reads back
		};

		static oc::string substituteEmit(const char* emitTemplate, const EmitArgs& a)
		{
			oc::string result;
			for (const char* p = emitTemplate; *p != '\0'; ++p)
			{
				if (*p != '$' || p[1] == '\0')
				{
					result += *p;
					continue;
				}
				const char code = p[1];
				if (code == 'r')      result += a.receiver;
				else if (code == 'i') result += a.index;
				else if (code == 'v') result += a.boundVar;
				else if (code == '*')
				{
					for (const oc::string& extra : a.varargs)
						result += ", " + extra;
				}
				else if (code >= '1' && code <= '9')
				{
					const size_t index = static_cast<size_t>(code - '1');
					if (index < a.args.size())
						result += a.args[index];
				}
				else
				{
					result += *p; // not a placeholder at all -- a literal '$' in the emitted C++
					continue;
				}
				++p;
			}
			return result;
		}

		oc::string callText(const DSLSymbol* callSymbol)
		{
			const DSLSymbol::FunctionCall& call = oc::get<DSLSymbol::FunctionCall>(callSymbol->data);
			const DSLSymbol::FunctionDeclaration& callee = oc::get<DSLSymbol::FunctionDeclaration>(call.functionSymbol->data);

			// Argument expressions in the CALLEE's parameter order -- named arguments reorder to their
			// parameter's position, positional ones keep their index (the DSL's order-independent named
			// matching becomes plain positional C++ here).
			oc::vector<oc::string> args(oc::max(call.arguments.size(), callee.parameterVarDeclarations.size()));
			for (size_t i = 0; i < call.arguments.size(); ++i)
			{
				size_t index = i;
				if (call.arguments[i].parameter != nullptr)
					for (size_t p = 0; p < callee.parameterVarDeclarations.size(); ++p)
						if (callee.parameterVarDeclarations[p] == call.arguments[i].parameter)
						{
							index = p;
							break;
						}
				if (index < args.size())
					args[index] = expressionText(call.arguments[i].value);
			}

			if (const char* emitTemplate = bindings.emitFor(call.functionSymbol); emitTemplate != nullptr)
			{
				// The receiver is its own emitted EXPRESSION (a handle-fetch call like
				// "ctx->entityGetPhysicsComponent(self)", or a chained member access like "self.pos"), so emit
				// templates compose textually.
				const oc::string receiverName = (call.receiver != nullptr) ? expressionText(call.receiver) : oc::string();
				// A variadic callee's extras sit past its declared parameters (always positional, so `args` holds
				// them at their own indices) and reach the template through "$*" instead of "$1".."$9".
				oc::vector<oc::string> varargs;
				if (callee.isVariadic && args.size() > callee.parameterVarDeclarations.size())
					varargs.assign(args.begin() + callee.parameterVarDeclarations.size(), args.end());
				return substituteEmit(emitTemplate, { receiverName, args, varargs });
			}

			// A user function: ctx/self/scriptData are auto-threaded through exactly like the callee's own
			// signature (see emitFunction) -- invisible to the DSL author at both the declaration and every
			// call site.
			oc::string text = callee.name + "(ctx, self, scriptData";
			for (const oc::string& arg : args)
				text += ", " + arg;
			return text + ")";
		}

		oc::string memberText(const DSLSymbol* symbol)
		{
			const DSLSymbol::MemberAccess& m = oc::get<DSLSymbol::MemberAccess>(symbol->data);
			const DSLType receiverType = dslValueType(m.receiver);
			// self.events.<name> IS its index, a compile-time constant -- "self.events" itself is never even
			// rendered (no receiverText needed): the name->index mapping is this document's OWN (DSL::eventNames),
			// the same one ScriptEventName's switch emits from, so the two can never disagree.
			if (receiverType == DSLType::ScriptEvents)
			{
				const int index = dslFindEventIndex(document.eventNames, m.memberName);
				return oc::to_string(index >= 0 ? index : 0); // defensive -- authored/loaded names always exist
			}
			const oc::string receiverText = expressionText(m.receiver); // recursion makes chains compose ("self.pos" -> ".x")
			// self.data's own fields are this DOCUMENT's, not the static registry (DSLType::ScriptData) -- "data"
			// itself dereferences to a VALUE ("(*(ScriptData*)scriptData)", see ScriptBindings' self.data emit),
			// so every field hop composes with "." exactly like any other member chain.
			if (receiverType == DSLType::ScriptData)
				return receiverText + "." + m.memberName;
			if (const BindingMember* member = bindings.findMember(receiverType, m.memberName); member != nullptr)
				return substituteEmit(member->emit.c_str(), { receiverText });
			return receiverText + "." + m.memberName; // defensive -- authored/loaded members always exist in the registry
		}

		oc::string expressionText(const DSLSymbol* symbol)
		{
			if (symbol == nullptr)
				return "0";
			switch (symbol->type)
			{
			case ST::Constant:
				return constantText(oc::get<DSLSymbol::Constant>(symbol->data));
			case ST::VariableReference:
				return declOf(oc::get<DSLSymbol::VariableReference>(symbol->data).declaration).name;
			case ST::FunctionCall:
				return callText(symbol);
			case ST::MemberAccess:
				return memberText(symbol);
			case ST::Expression:
			{
				// FLAT, exactly as authored: C++'s own precedence supplies the "*, / over +, -" the DSL defers
				// to emit time (DSL.ixx); parens appear only where the author grouped.
				const DSLSymbol::Expression& e = oc::get<DSLSymbol::Expression>(symbol->data);
				oc::string text;
				for (size_t i = 0; i < e.operands.size(); ++i)
				{
					if (i > 0)
					{
						text += ' ';
						text += dslOperatorText(e.operators[i - 1]);
						text += ' ';
					}
					text += expressionText(e.operands[i]);
				}
				return e.grouped ? "(" + text + ")" : text;
			}
			case ST::Placeholder:
				// Committed documents never hold value placeholders (the editor's no-placeholder rule) -- emit
				// a type default anyway rather than broken text if one ever slips through.
				return defaultValueText(oc::get<DSLSymbol::Placeholder>(symbol->data).expectedType, bindings);
			default:
				return "0"; // no other symbol kind is a value
			}
		}

		// "float applied = 0.0f" (no trailing ';') -- local declarations and the for-loop's first clause.
		oc::string declarationText(const DSLSymbol* declSymbol)
		{
			const DSLSymbol::VariableDeclaration& v = declOf(declSymbol);
			oc::string text = oc::string(cppTypeName(declaredType(declSymbol))) + " " + v.name;
			if (v.initialValue != nullptr)
				text += " = " + expressionText(v.initialValue);
			return text;
		}

		// ---- statements / blocks ----

		// One currently-open nested block. `trailing` is a statement emitted just before the closing brace --
		// the write-back of a `ref` element binding, which has to be the LAST thing in the block whichever way
		// the block ends up closed (a sibling at the same level, an else, or the function's end).
		struct OpenScope
		{
			int scopeLevel;
			oc::string trailing;
		};

		// Names the hoisted source locals (see the ForEach/IfExist cases). Monotonic per FUNCTION rather than
		// keyed on nesting depth: two sibling loops at the same depth would otherwise both declare `ocSrc1`, and
		// unlike the loop index -- which lives in the for-init and is scoped to its own loop -- these sit in the
		// enclosing block, where a repeated name is a redefinition.
		int sourceLocalCounter = 0;

		// Whether the block opened at `headerIndex` assigns to `boundVar` anywhere -- directly (`p = ...`) or
		// through a member (`p.x = ...`). What decides if a `ref` binding needs its write-back at all: a
		// read-only body pays nothing. Every symbol on a line is a peer in its flat `symbols` list (see DSL.ixx's
		// ownership model), so a scan finds nested assignments without a tree walk.
		bool blockAssignsTo(const DSLSymbol* boundVar, int headerIndex) const
		{
			const int blockEnd = dslBlockEnd(document.file, headerIndex);
			for (int i = headerIndex + 1; i < blockEnd; ++i)
				for (const oc::unique_ptr<DSLSymbol>& s : document.file.lines[i]->symbols)
				{
					if (s->type != ST::Expression)
						continue;
					const DSLSymbol::Expression& e = oc::get<DSLSymbol::Expression>(s->data);
					if (e.operators.empty() || !dslIsAssignOperator(e.operators[0]) || e.operands.empty())
						continue;
					// Walk the target back to its root declaration -- "p" and "p.x" both root at boundVar.
					const DSLSymbol* target = e.operands[0];
					while (target != nullptr && target->type == ST::MemberAccess)
						target = oc::get<DSLSymbol::MemberAccess>(target->data).receiver;
					if (target != nullptr && target->type == ST::VariableReference
						&& oc::get<DSLSymbol::VariableReference>(target->data).declaration == boundVar)
						return true;
				}
			return false;
		}

		// The write-back statement for a `ref` element binding, or "" when none is needed: the binding isn't
		// `ref`, the body never assigns to it, the container declared its elements read-only, or the element is
		// HANDLE-typed (an Entity is already a reference -- writes reach the real object through it, so there is
		// nothing to copy back). `receiverText` is the HOISTED source local, never the source expression itself
		// -- re-evaluating that here would run it a second time; `keyText` is the loop index for a foreach, the
		// `at` key for an ifexist.
		// iterText: the loop's hoisted local (ForEach only, empty elsewhere). A container that resolved its
		// storage there writes back through it; everything else re-resolves from the receiver.
		oc::string elementWriteBack(const DSLCodeLine& line, const DSLSymbol::FlowControl* flow,
			const DSLSymbol::VariableDeclaration& bound, const oc::string& receiverText, const oc::string& keyText,
			const oc::string& iterText = {})
		{
			if (!bound.isRef || flow->condition == nullptr)
				return {};
			const DSLType elemType = oc::get<DSLSymbol::TypeDeclaration>(bound.typeSymbol->data).type;
			if (dslIsEngineObjectType(elemType))
				return {}; // a handle, not a copy
			const BindingObject* container = bindings.objectFor(dslValueType(flow->condition));
			if (container == nullptr || !container->elementWritable)
				return {};
			const int headerIndex = dslLineIndex(document.file, &line);
			if (headerIndex < 0 || !blockAssignsTo(flow->forLoopVar, headerIndex))
				return {};
			if (!iterText.empty() && container->sequenceBeginEmit != nullptr && container->sequenceElementSetEmit != nullptr)
				return substituteEmit(container->sequenceElementSetEmit, { iterText, { keyText }, {}, {}, bound.name }) + ";";
			if (container->elementSetEmit == nullptr)
				return {};
			return substituteEmit(container->elementSetEmit, { receiverText, { keyText }, {}, {}, bound.name }) + ";";
		}

		void openBlock()
		{
			emitLine("{");
			++indent;
		}

		void closeBlock()
		{
			--indent;
			emitLine("}");
		}

		// A function's full C++ signature (return type, name, ctx/self/scriptData + declared params, or the exact
		// real ABI shape for a recognized entry point) with no trailing ';' or '{' -- shared by the forward
		// declaration pass (transpile) and emitFunction's own header line, so the two can never drift apart.
		//
		// A recognized ScriptAPI entry point (ScriptEditor's EXPORTS toggles, see EntryPointDef) transpiles
		// to its EXACT real exported signature -- `cppSuffix` spells out everything after "ctx, self"
		// verbatim, including parameters no DSLType can represent yet (scriptData always; OnPhysicsEvent's
		// other/contactId), which the DSL body simply never references, and is marked SCRIPT_EXPORT: the host
		// resolves it by exact name (GetProcAddress in the DLL build; the REGISTER_*() macro in the cooked
		// build), so it must keep C linkage/dllexport. Every OTHER function is a plain internal helper --
		// `static`, never SCRIPT_EXPORT -- exactly like the node editor's own generated helper functions
		// (Scene.cpp's emitFunctions): nothing outside this file ever calls one by name, so it has no ABI
		// surface to keep, and `static` avoids needlessly exporting it from the DLL (or, in the cooked build's
		// per-script namespace, needlessly widening its linkage beyond that namespace).
		//
		// `ctx`/`self` are auto-injected as the first two parameters either way, invisible to the DSL author,
		// so self.thing/thunk-function calls resolve without one (see the ScriptBindings emit templates, all
		// written in terms of these two names) and so user-function calls (callText) can thread them straight
		// through too. Non-entry functions ALSO get `scriptData` as their 3rd (self.data needs it in scope
		// wherever it's dotted into, not just from an entry point) -- an entry point already has one,
		// positioned wherever the real ABI puts it (see cppSuffix).
		oc::string functionSignature(int headerIndex)
		{
			const DSLSymbol::FunctionDeclaration& func =
				oc::get<DSLSymbol::FunctionDeclaration>(document.file.lines[headerIndex]->head()->data);
			const EntryPointDef* entry = bindings.entryPointFor(func.name);

			oc::string signature = oc::string(entry != nullptr ? "SCRIPT_EXPORT " : "static ")
				+ cppTypeName(func.returnType) + " " + func.name + "(const ScriptContext* ctx, Entity* self";
			if (entry != nullptr)
			{
				signature += entry->cppSuffix;
			}
			else
			{
				signature += ", ScriptData* scriptData";
				for (size_t i = 0; i < func.parameterVarDeclarations.size(); ++i)
				{
					signature += ", ";
					const DSLSymbol::VariableDeclaration& param = declOf(func.parameterVarDeclarations[i]);
					signature += cppTypeName(declaredType(func.parameterVarDeclarations[i]));
					signature += param.isRef ? "& " : " "; // `ref` out-parameters become C++ references
					signature += param.name;
				}
			}
			signature += ")";
			return signature;
		}

		// One function: header line + every body line up to (not including) `bodyEnd`. Blocks mirror
		// Syntax::format's scopeLevel derivation -- an elseif/else continues its chain's open block.
		void emitFunction(int headerIndex, int bodyEnd)
		{
			const DSLSymbol::FunctionDeclaration& func =
				oc::get<DSLSymbol::FunctionDeclaration>(document.file.lines[headerIndex]->head()->data);
			const EntryPointDef* entry = bindings.entryPointFor(func.name);

			emitLine(functionSignature(headerIndex));
			openBlock();

			// One entry per currently-open nested block: its header's scopeLevel, plus a statement (if any) to
			// emit just BEFORE the closing brace -- what a `ref` binding's write-back rides on (see IfExist/
			// ForEach), so the copy back lands at the end of the block whichever way the block is closed.
			oc::vector<OpenScope> openScopes;
			sourceLocalCounter = 0;
			for (int i = headerIndex + 1; i < bodyEnd; ++i)
			{
				const DSLCodeLine& line = *document.file.lines[i];
				const DSLSymbol* head = line.head();
				const DSLSymbol::FlowControl* flow = (head != nullptr && head->type == ST::FlowControl)
					? &oc::get<DSLSymbol::FlowControl>(head->data) : nullptr;
				const bool continuation = flow != nullptr
					&& (flow->control == DSLFlowControl::ElseIf || flow->control == DSLFlowControl::Else);

				while (!openScopes.empty() && openScopes.back().scopeLevel >= line.scopeLevel)
				{
					if (continuation && openScopes.back().scopeLevel == line.scopeLevel)
						break;
					if (!openScopes.back().trailing.empty())
						emitLine(openScopes.back().trailing);
					closeBlock();
					openScopes.pop_back();
				}

				emitStatement(line, head, flow, openScopes);
			}
			// Blocks still open at the end of the body close here -- `end` lines aren't stored (the formatter
			// re-derives them), so a block that is the LAST statement in a function is never closed by the
			// per-line path above. It has to emit the trailing statement too, or a `ref` element's write-back
			// is silently dropped for exactly the loops that end a function.
			while (!openScopes.empty())
			{
				if (!openScopes.back().trailing.empty())
					emitLine(openScopes.back().trailing);
				closeBlock();
				openScopes.pop_back();
			}

			closeBlock();

			if (entry != nullptr)
			{
				// OnEvent's REGISTER_ON_EVENT() also registers ScriptEventCount/ScriptEventName (see
				// ScriptAPI.h) -- driven by this document's OWN named entries (DSL::eventNames, authored via the
				// EVENTS sidebar section), the SAME list self.events.<name> indexes into (memberText), so the
				// host's name->index resolution always agrees with what the body compares eventIdx against.
				if (func.name == "OnEvent")
				{
					emitLine("");
					emitLine("SCRIPT_EXPORT int ScriptEventCount(void) { return " + oc::to_string(document.eventNames.size()) + "; }");
					emitLine("SCRIPT_EXPORT const char* ScriptEventName(int eventIdx)");
					openBlock();
					emitLine("switch (eventIdx)");
					openBlock();
					for (size_t i = 0; i < document.eventNames.size(); ++i)
						emitLine("case " + oc::to_string(i) + ": return \"" + document.eventNames[i] + "\";");
					emitLine("default: return \"\";");
					closeBlock();
					closeBlock();
				}
				emitLine("");
				emitLine(entry->registerMacro);
			}
		}

		void emitStatement(const DSLCodeLine& line, const DSLSymbol* head, const DSLSymbol::FlowControl* flow,
			oc::vector<OpenScope>& openScopes)
		{
			if (head == nullptr || head->type == ST::Placeholder)
			{
				emitLine(""); // a blank statement slot stays a blank line
				return;
			}
			switch (head->type)
			{
			case ST::Comment:
				emitLine("// " + oc::get<DSLSymbol::Comment>(head->data).text);
				return;
			case ST::VariableDeclaration:
				emitLine(declarationText(head) + ";");
				return;
			case ST::Expression: // an assignment/compound-assign statement -- same flat-chain emit as any value
				emitLine(expressionText(head) + ";");
				return;
			case ST::FunctionCall:
				emitLine(callText(head) + ";");
				return;
			case ST::FlowControl:
				break;
			default:
				emitLine("// unsupported statement");
				return;
			}

			switch (flow->control)
			{
			case DSLFlowControl::If:
			case DSLFlowControl::While:
				emitLine(oc::string(flow->control == DSLFlowControl::If ? "if" : "while")
					+ " (" + expressionText(flow->condition) + ")");
				openBlock();
				openScopes.push_back({ line.scopeLevel, {} });
				return;
			case DSLFlowControl::ElseIf:
			case DSLFlowControl::Else:
				// The chain's previous branch closes here; the open-block entry survives -- one `end` (a single
				// scopeLevel step-down) closes the whole chain, exactly like the formatter reads it.
				closeBlock();
				emitLine(flow->control == DSLFlowControl::Else ? "else"
					: "else if (" + expressionText(flow->condition) + ")");
				openBlock();
				return;
			case DSLFlowControl::ForEach:
			{
				// The sequence's count/at emits come from the receiver's own registration (BindingObject's
				// sequence* fields) -- an array walks the generic handle ABI, an engine collection its own
				// count/at pair. The index is a generated local the DSL author can't name, so the only way to
				// index the sequence is the one the loop does, and `at` is range-safe by contract.
				const DSLSymbol::VariableDeclaration& elem = oc::get<DSLSymbol::VariableDeclaration>(flow->forLoopVar->data);
				const DSLType elemType = oc::get<DSLSymbol::TypeDeclaration>(elem.typeSymbol->data).type;

				// The sequence expression is hoisted into a local ONLY when it is named more than once: a
				// source that is a CALL (world.entitiesInRadius(...)) must not run per element. A container
				// with a begin emit names it exactly once -- the begin emit resolves it and count/at read that
				// result -- so there the hoist local never appears at all.
				const auto localId = sourceLocalCounter++;
				const oc::string sourceText = expressionText(flow->condition);
				const oc::string sourceName = "ocSrc" + oc::to_string(localId);
				const BindingObject* object = bindings.objectFor(dslValueType(flow->condition));
				if (object == nullptr || object->sequenceBeginEmit == nullptr)
					emitLine("const auto " + sourceName + " = " + sourceText + ";");

				const oc::string indexName = "ocIdx" + oc::to_string(openScopes.size());
				oc::string countText, atText, iterName, refAtText;
				if (object != nullptr && object->sequenceCountEmit != nullptr && object->sequenceAtEmit != nullptr)
				{
					// The begin emit resolves the container once, turning a per-element lookup (an array's
					// generation-tagged handle unpack) into a per-loop one.
					iterName = sourceName;
					if (object->sequenceBeginEmit != nullptr)
					{
						iterName = "ocIter" + oc::to_string(localId);
						emitLine("const auto " + iterName + " = " + substituteEmit(object->sequenceBeginEmit, { sourceText }) + ";");
					}
					countText = substituteEmit(object->sequenceCountEmit, { iterName });
					atText = substituteEmit(object->sequenceAtEmit, { iterName, {}, {}, indexName });
					// A `ref` element binds straight into the storage when the container names it as an
					// lvalue: no copy in, no write-back out, so the two cannot drift.
					if (elem.isRef && object->sequenceBeginEmit != nullptr && object->sequenceElementRefEmit != nullptr)
						refAtText = substituteEmit(object->sequenceElementRefEmit, { iterName, {}, {}, indexName });
				}
				emitLine("for (int " + indexName + " = 0, " + indexName + "End = " + countText + "; "
					+ indexName + " < " + indexName + "End; ++" + indexName + ")");
				openBlock();
				if (!refAtText.empty())
				{
					emitLine(oc::string(cppTypeName(elemType)) + "& " + elem.name + " = " + refAtText + ";");
					openScopes.push_back({ line.scopeLevel, {} }); // the reference IS the write-back
				}
				else
				{
					emitLine(oc::string(cppTypeName(elemType)) + " " + elem.name + " = " + atText + ";");
					openScopes.push_back({ line.scopeLevel, elementWriteBack(line, flow, elem, sourceName, indexName, iterName) });
				}
				return;
			}
			case DSLFlowControl::IfExist:
			{
				// The lookup's own success flag IS the condition -- there is no separate bounds check to emit,
				// and no way to author the read without it. The element is declared in the if's INIT-statement
				// so the lookup can write through it and the branch can read it; C++ scopes that declaration
				// across an attached `else` too, which is exactly why the DSL reserves the name there (see
				// isChainComponentBinding) even though it isn't readable in that branch.
				const DSLSymbol::VariableDeclaration& bound = oc::get<DSLSymbol::VariableDeclaration>(flow->forLoopVar->data);
				const DSLType elemType = oc::get<DSLSymbol::TypeDeclaration>(bound.typeSymbol->data).type;
				// The source is hoisted into a local ONLY when it is used more than once -- i.e. when a copied
				// `ref` binding needs a write-back at block end, where re-evaluating a source that is a call
				// would run it twice. The component fetch and the pointer lookup below each name it exactly
				// once, so they inline it and the local never appears.
				const oc::string sourceText = expressionText(flow->condition);
				const oc::string sourceName = "ocSrc" + oc::to_string(sourceLocalCounter++);
				const oc::string keyText = (flow->forCondition != nullptr) ? expressionText(flow->forCondition) : oc::string();

				// A COMPONENT is fetched off the entity rather than looked up in a container: the fetch call IS
				// the condition (a null handle is the miss), so it needs no lookup registration of its own --
				// the component type's own fetch emit is the whole story. Everything downstream is identical.
				if (dslIsComponentType(elemType))
				{
					const char* fetchEmit = bindings.componentFetchEmit(elemType);
					const oc::string fetchText = (fetchEmit != nullptr)
						? substituteEmit(fetchEmit, { sourceText }) : oc::string("nullptr");
					emitLine("if (" + oc::string(cppTypeName(elemType)) + " " + bound.name + " = " + fetchText + ")");
					openBlock();
					openScopes.push_back({ line.scopeLevel, {} });
					return;
				}

				const BindingObject* container = bindings.objectFor(dslValueType(flow->condition));

				// A `ref` element binds to the storage itself when the container can hand back a pointer: the
				// pointer IS the miss test, so there is no default-initialized copy to seed and no store to
				// emit at block end. The pointer lives in the if's init-statement (scoped across an attached
				// `else`, like the copy it replaces) and the DSL's name is a reference to it inside the body --
				// which keeps every expression that reads the binding exactly as it was.
				if (bound.isRef && container != nullptr && container->lookupRefEmit != nullptr)
				{
					const oc::string pointerName = "ocPtr" + oc::to_string(sourceLocalCounter++);
					emitLine("if (" + oc::string(cppTypeName(elemType)) + "* " + pointerName + " = "
						+ substituteEmit(container->lookupRefEmit, { sourceText, { keyText } }) + ")");
					openBlock();
					emitLine(oc::string(cppTypeName(elemType)) + "& " + bound.name + " = *" + pointerName + ";");
					openScopes.push_back({ line.scopeLevel, {} }); // the reference IS the write-back
					return;
				}

				// The write-back is resolved BEFORE anything is emitted: it decides whether the source is named
				// twice, and therefore whether the hoist local is needed at all.
				const oc::string writeBack = elementWriteBack(line, flow, bound, sourceName, keyText);
				const oc::string receiver = writeBack.empty() ? sourceText : sourceName;
				if (!writeBack.empty())
					emitLine("const auto " + sourceName + " = " + sourceText + ";");
				const oc::string testText = (container != nullptr && container->lookupEmit != nullptr)
					? substituteEmit(container->lookupEmit, { receiver, { keyText }, {}, {}, bound.name })
					: oc::string("false");
				emitLine("if (" + oc::string(cppTypeName(elemType)) + " " + bound.name + " = "
					+ defaultValueText(elemType, bindings) + "; " + testText + ")");
				openBlock();
				openScopes.push_back({ line.scopeLevel, writeBack });
				return;
			}
			case DSLFlowControl::For:
				emitLine("for (" + declarationText(flow->forLoopVar) + "; "
					+ expressionText(flow->forCondition) + "; " + expressionText(flow->forIncrement) + ")");
				openBlock();
				openScopes.push_back({ line.scopeLevel, {} });
				return;
			case DSLFlowControl::Return:
				emitLine(flow->condition != nullptr ? "return " + expressionText(flow->condition) + ";" : "return;");
				return;
			case DSLFlowControl::Break:
				emitLine("break;");
				return;
			default:
				return;
			}
		}
	};

}

oc::string Transpiler::transpile(const DSL& document, const ScriptBindings& bindings)
{
	Emitter emitter{ document, bindings };

	emitter.emitLine("// Generated by the DSL transpiler -- do not edit (the source is the //@@dsl block below).");

	// The ocArrGet/ocArrSet/ocArrPush the array emits call live in ScriptAPI.h, which is force-included (/FI)
	// into every script TU -- nothing to emit here.

	// self.data's backing struct, plus one cached pointer field per required component (//@@require) the HOST
	// (not this generated code) fills in straight off getComponent<T>() right after allocating the block
	// (ScriptComponent::spawn), before OnSpawn runs. ALWAYS declared, even empty: every generated function takes
	// a "ScriptData* scriptData" parameter regardless of whether this document uses it (see functionSignature/
	// EntryPointDef::cppSuffix) -- ScriptDataSize()/ScriptRequiredComponents() (and their REGISTER_*() calls)
	// are the only pieces that stay conditional, since an all-default ScriptData needs no host-side allocation
	// or component population.
	//
	// Required-component fields are sorted by EComponentID bit (componentBit) and emitted FIRST, ahead of the
	// document's own data fields: the host has zero visibility into this struct's layout (same as any other
	// ScriptData field), so it can only fill these in by scanning ScriptRequiredComponents' bitmask bit-by-bit
	// and writing sequential pointer-sized slots from the FRONT of the block -- this struct's field order must
	// match that scan order exactly, or the host writes the right pointers into the wrong fields.
	{
		oc::vector<DSLType> requiredSorted = document.requiredComponents;
		oc::sort(requiredSorted.begin(), requiredSorted.end(), [&](DSLType a, DSLType b)
			{ return bindings.componentBit(a) < bindings.componentBit(b); });

		emitter.emitLine("");
		emitter.emitLine("struct ScriptData");
		emitter.emitLine("{");
		++emitter.indent;
		for (DSLType componentType : requiredSorted)
		{
			const char* typeName = bindings.componentTypeName(componentType);
			const BindingObject* object = bindings.objectFor(componentType);
			emitter.emitLine(oc::string(typeName != nullptr ? typeName : "void") + "* " + (object != nullptr ? object->name : "?") + ";");
		}
		for (const DSLDataField& field : document.dataFields)
			emitter.emitLine(oc::string(cppTypeName(field.type)) + " " + field.name + " = " + defaultValueText(field.type, bindings) + ";");
		--emitter.indent;
		emitter.emitLine("};");

		if (!document.dataFields.empty() || !requiredSorted.empty())
		{
			emitter.emitLine("");
			emitter.emitLine("SCRIPT_EXPORT unsigned int ScriptDataSize(void) { return sizeof(ScriptData); }");
			// Hashed from the FIELD LAYOUT, not the size: the host reuses an existing data block across a
			// hot-reload only while this matches, so reinterpreting one layout as another can't happen (see
			// ScriptDataLayoutIdFn). Required components are part of it -- they occupy the block's leading slots.
			{
				uint32 layoutHash = 2166136261u; // FNV-1a over "<type>:<name>;" per field, in layout order
				const auto hashText = [&layoutHash](const oc::string& text)
				{
					for (char ch : text)
					{
						layoutHash ^= static_cast<uint8>(ch);
						layoutHash *= 16777619u;
					}
				};
				for (DSLType componentType : requiredSorted)
					hashText(oc::string(bindings.componentTypeName(componentType) ? bindings.componentTypeName(componentType) : "?") + ":*;");
				for (const DSLDataField& field : document.dataFields)
					hashText(oc::string(dslTypeName(field.type)) + ":" + field.name + ";");
				emitter.emitLine("SCRIPT_EXPORT unsigned int ScriptDataLayoutId(void) { return "
					+ oc::to_string(layoutHash) + "u; }");
			}
			emitter.emitLine("REGISTER_SCRIPT_DATA_SIZE()");

			// The EXPOSED fields (see OcScriptField): what lets the editor show and set them by name, since the
			// engine otherwise knows this block only as a size and a hash. Offsets come from offsetof rather
			// than being computed here, so the table cannot drift from the struct the compiler actually laid
			// out. Hidden fields -- the default -- are absent, so a script that exposes nothing emits nothing.
			{
				oc::vector<const DSLDataField*> exposed;
				for (const DSLDataField& field : document.dataFields)
					if (field.visibility != DSLFieldVisibility::Hidden && dslCanExposeFieldType(field.type))
						exposed.push_back(&field);
				if (!exposed.empty())
				{
					emitter.emitLine("");
					emitter.emitLine("static const OcScriptField kScriptDataFields[] = {");
					for (const DSLDataField* field : exposed)
					{
						const char* typeCode = "OC_FIELD_INT";
						switch (field->type)
						{
						case DSLType::Float:  typeCode = "OC_FIELD_FLOAT"; break;
						case DSLType::Bool:   typeCode = "OC_FIELD_BOOL"; break;
						case DSLType::String: typeCode = "OC_FIELD_STRING"; break;
						case DSLType::Int:    typeCode = "OC_FIELD_INT"; break;
						default:
						{
							// A struct: matched by its registered DSL name, so a new one only needs a code here
							// once it is actually exposable.
							const char* name = dslTypeName(field->type);
							if (oc::string_view(name) == "vec2")      typeCode = "OC_FIELD_VEC2";
							else if (oc::string_view(name) == "vec3") typeCode = "OC_FIELD_VEC3";
							else if (oc::string_view(name) == "vec4") typeCode = "OC_FIELD_VEC4";
							else if (oc::string_view(name) == "quat") typeCode = "OC_FIELD_QUAT";
							else                                      typeCode = nullptr; // not representable -- skipped
							break;
						}
						}
						if (typeCode == nullptr)
							continue;
						emitter.emitLine("\t{ \"" + field->name + "\", " + typeCode
							+ ", (int)offsetof(ScriptData, " + field->name + "), "
							+ (field->visibility == DSLFieldVisibility::Public ? "OC_FIELD_PUBLIC" : "OC_FIELD_PRIVATE") + " },");
					}
					emitter.emitLine("};");
					emitter.emitLine("SCRIPT_EXPORT const OcScriptField* ScriptDataFields(int* outCount)");
					emitter.emitLine("{ *outCount = (int)(sizeof(kScriptDataFields) / sizeof(kScriptDataFields[0])); return kScriptDataFields; }");
					emitter.emitLine("REGISTER_SCRIPT_DATA_FIELDS()");
				}
			}

			if (!requiredSorted.empty())
			{
				oc::string bitmask;
				for (DSLType componentType : requiredSorted)
					bitmask += (bitmask.empty() ? "" : " | ") + oc::string("(1u << ") + oc::to_string(bindings.componentBit(componentType)) + ")";
				emitter.emitLine("");
				emitter.emitLine("SCRIPT_EXPORT unsigned int ScriptRequiredComponents(void) { return " + bitmask + "; }");
				emitter.emitLine("REGISTER_SCRIPT_REQUIRED_COMPONENTS()");
			}
		}
	}

	// Forward-declare every DSL function up front, in document order, so call order in the .dsl never matters --
	// a function can call one authored later in the file (the editor itself has no such restriction: any
	// in-scope function name is a valid call candidate regardless of where it's declared). Repeating the
	// leading qualifier (SCRIPT_EXPORT or static, see functionSignature) on both the declaration and the
	// definition below is standard practice either way; functionSignature is the single source of truth for both.
	bool anyFunctions = false;
	for (int i = 0; i < static_cast<int>(document.file.lines.size()); ++i)
	{
		const DSLSymbol* head = document.file.lines[i]->head();
		if (head == nullptr || head->type != ST::FunctionDeclaration)
			continue;
		if (!anyFunctions)
		{
			emitter.emitLine("");
			anyFunctions = true;
		}
		emitter.emitLine(emitter.functionSignature(i) + ";");
	}

	// One function per DSL function, at file scope -- no wrapper class (see emitFunction/functionSignature).
	for (int i = 0; i < static_cast<int>(document.file.lines.size()); ++i)
	{
		const DSLSymbol* head = document.file.lines[i]->head();
		if (head == nullptr || head->type != ST::FunctionDeclaration)
			continue;
		emitter.emitLine("");
		const int bodyEnd = dslBlockEnd(document.file, i);
		emitter.emitFunction(i, bodyEnd);
		i = bodyEnd - 1;
	}

	return emitter.out;
}
