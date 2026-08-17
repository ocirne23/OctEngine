export module Script:ScriptLang;

import Core;
import :DSL;
import :ScriptBindings;

// Human-readable name for a DSLType, as used in rendered code ("float", "vec3", "PhysicsComponent") and in the
// editor's compose UI (e.g. building up "float " before a variable name is typed). One canonical spelling
// shared by rendering and editing so they can't drift apart.
export const char* dslTypeName(DSLType type);

// Same for a DSLOperator ("+", "<=", "&&", "+=") -- the ONE spelling rendering, candidate labels, and every
// compose-prefix builder share.
export const char* dslOperatorText(DSLOperator op);

// Identifies a MUTABLE pointer location a span's symbol occupies, so an edit knows what to overwrite when
// replacing it -- a whole statement, a flow-control condition, a call argument's value, a declaration's
// initializer, or one operand of an Expression chain. Replacing a slot swaps its ENTIRE occupant; chain-level
// editing (inserting/removing individual operands and operators) goes through Kind::ExpressionOperand plus
// SyntaxSpan::operatorIndex below. `line` is always the symbol's owning line (see DSL.ixx's ownership model)
// -- where a freshly-built replacement gets pushed.
export struct SlotRef
{
	enum class Kind
	{
		None,                            // not editable (e.g. an assignment's target operand, glue text)
		LineHead,                        // this line's own top-level statement symbol
		FlowControlCondition,            // -> parent is the FlowControl symbol
		CallArgumentValue,               // -> parent is the FunctionCall symbol; argIndex selects which argument
		VariableDeclarationInitialValue, // -> parent is the VariableDeclaration symbol
		FunctionReturnType,              // -> parent is the FunctionDeclaration symbol; always present/selectable
		                                 // in expanded view (rendered blank when returnType == Void), so a
		                                 // function's return type can be set or changed at any time
		IfExistKey,                      // -> parent is the IfExist FlowControl symbol: its `at <key>` value,
		                                 // separate from FlowControlCondition (the container) so the two
		                                 // re-edit independently
		ExpressionOperand,               // -> parent is the Expression (operator chain) symbol; argIndex selects
		                                 // which operand -- what in-place term replace/insert/delete acts on
	};
	Kind kind = Kind::None;
	DSLSymbol* parent = nullptr; // unused for LineHead
	DSLCodeLine* line = nullptr;
	int argIndex = -1; // CallArgumentValue / ExpressionOperand only
};

// One contiguous, independently-selectable range of a rendered line's text, mapped back to the DSLSymbol it
// represents (see DSL.ixx's ownership-model comment: a span always identifies a real symbol, never a bare tree
// position). Ranges are left-to-right and non-overlapping within a line. `slot` is set (Kind != None) only when
// this span sits in one of the editable positions above -- that's what autocomplete-driven replacement acts on.
export struct SyntaxSpan
{
	DSLSymbol* symbol = nullptr;
	int startCol = 0;
	int endCol = 0; // exclusive
	SlotRef slot;
	int operatorIndex = -1;  // >= 0: this span IS operator #N of `symbol`'s Expression chain (the target of
	                         // in-place operator replacement and operator+operand deletion, see ScriptEditor)
	bool groupClose = false; // this span is a parenthesized group's own closing ')' -- selects the WHOLE group
	                         // as one unit ("after the parens"): typing an operator there chains a new term
	                         // after the group, Backspace deletes the group as a single chain term. `slot` is
	                         // the group's own position (same slot its enclosing context passed in).
};

// One rendered visual line. `sourceLine` is null only for a synthetic `end` line (see DSL.ixx: blocks close via
// DSLCodeLine::scopeLevel transitions, never a stored symbol) -- `endOfSymbol` then names the block-header
// symbol (If/While/For/FunctionDeclaration) that this `end` closes, so it's still a legitimate cursor stop.
// Every line produced by Syntax::format has at least one span (there is always a primary/head symbol to select).
export struct SyntaxLine
{
	DSLCodeLine* sourceLine = nullptr;
	DSLSymbol*   endOfSymbol = nullptr;
	int scopeLevel = 0;
	oc::string text;
	oc::vector<SyntaxSpan> spans; // left-to-right, non-overlapping, covers every selectable substring of text
};

// Turns a DSLScriptFile into rendered text + a symbol-tagged span list per line -- the one place that knows how
// a DSLSymbol tree reads as text. Deliberately independent from Transpiler (per the DSL editor's architecture:
// "what this looks like" and "how this becomes C++" are separately maintained systems, even though both walk
// the same symbol shapes) and from AutoCompleteRules ("what can be typed here" is a third, also-separate concern).
//
// `compact` hides ONLY function-parameter type annotations (both at a function's own declaration and at each of
// its call sites) and a function's return-type arrow -- NOT local variable declarations, which always show
// their type in both views. (Worked from the example program's compact/expanded comparison: `int thing = 1`
// and the for-loop header are byte-identical between views; only `doForce(...)`'s call-site types and
// `function update(...)`/`canJump() -> bool`'s own signatures differ.)
export class Syntax
{
public:
	// Takes a non-const DSLScriptFile so the returned spans/endOfSymbol carry mutable DSLSymbol* -- Syntax
	// itself never mutates anything, but ScriptEditor's editing operations act directly through these same pointers.
	static oc::vector<SyntaxLine> format(DSLScriptFile& file, bool compact);

	// True if `head` (a line's own top-level symbol) opens a nested block -- i.e. the FOLLOWING lines at
	// scopeLevel+1 are its body, closed by a synthetic `end`. Shared between format()'s end-insertion algorithm
	// and ScriptEditor's Enter-makes-a-new-line handling (which needs the same answer to decide whether the new
	// line belongs one level deeper or as a plain sibling).
	static bool isBlockOpener(const DSLSymbol* head);
};

export class TabGroup
{
public:
	TabGroup* parent = nullptr;
	oc::vector<TabGroup> children;
};

// One offered completion. `refSymbol` names the existing declaration a Variable/Function/Reassign candidate
// resolves to; `declareType` is only meaningful for DeclareType (picking a type keyword to start a new local
// variable -- AutoCompleteRules doesn't invent the variable's NAME, that's a free-typed second step the editor
// drives); `op` is only meaningful for Comparator/AssignOperator (one of the comparison/compound-assign
// operators offered while building an if/while/for-loop clause). Reassign is a STATEMENT candidate (offered in
// the Void branch, like Function) naming an existing in-scope variable -- confirming it starts building a
// `name = value` assignment statement targeting it (see ScriptEditor's ReassignValue/ValueRight flow), distinct
// from Variable (a use of that same variable as a VALUE somewhere else).
export struct Candidate
{
	enum class Kind
	{
		KeywordIf, KeywordWhile, KeywordReturn, KeywordBreak, KeywordTrue, KeywordFalse,
		Variable, Function, DeclareType, Literal, Comparator, DeclareFunction, KeywordFor, KeywordForEach,
		KeywordIfExist,     // "ifexist" -- stages a type, a name, the source, and (if it takes one) the key;
		                    // the DSL's only read of something that may not be there, check included
		KeywordRef,    // "ref" -- a MODIFIER, not a pick: offered alongside the type keywords wherever a
		               // by-reference binding is allowed (a function parameter today), confirming it sets the
		               // flag and returns to the SAME stage with "ref " in the prefix, awaiting the type
		AssignOperator, Reassign,
		ArithmeticOperator, LogicalOperator,
		KeywordElseIf, KeywordElse, // offered only on a statement slot INSIDE an if/elseif branch -- confirming
		                            // grows the chain with a new branch after the enclosing one (the elseif
		                            // stages its condition first, like any if; else needs the chain else-less)
		BindingObject, // a sidebar engine-object binding ("physics", "self") -- consumable only by dotting into
		               // it: '.' (or a confirm) opens its member/function list (see receiverCandidates)
		Member,        // one MEMBER of a binding object ("pos"): refSymbol = the RECEIVER's declaration,
		               // declareType = the member's value type -- resolves to a MemberAccess
	};
	oc::string label;
	Kind kind = Kind::Variable;
	DSLSymbol* refSymbol = nullptr;
	DSLType declareType = DSLType::Void;
	DSLOperator op = DSLOperator::Equal;
	DSLSymbol* receiver = nullptr; // Function candidates from receiverCandidates: the receiver chain's ROOT
	                               // declaration (a sidebar binding or a struct-typed variable; null = free call)
	oc::string receiverPath;      // the dotted member path between the root and the call, "" for a direct
	                               // dot-call -- "pos" in `self.pos.length()`. For Kind::Member the PATH rides
	                               // in `label` instead ("pos.x"), always relative to refSymbol as the root
	bool memberWritable = true;    // Kind::Member only: the underlying BindingMember::writable -- a chainable-
	                               // only member (self.physics) refuses to resolve as a bare assignment target
	                               // in statement context (confirmCompose), it must keep dotting further in
};

// "What can be typed here" -- kept independent from Syntax/Transpiler (see the DSL editor's architecture: three
// separately-maintained concerns that happen to walk the same symbol shapes). `expectedType` (read directly off
// the selected Placeholder, or DSLType::Void for a statement slot -- see DSLSymbol::Placeholder) decides the
// candidate shape: Void offers control-flow keywords + "declare a new variable of type X" per known type +
// in-scope function names (free call statements); any other type offers matching literals/keywords (true/false
// for Bool), in-scope variables of that type, and functions returning that type. `atLine` anchors the in-scope
// variable scan: sidebar + this function's own parameters + locals declared earlier in the SAME function
// (walking file.lines backward from atLine to its enclosing FunctionDeclaration header) -- cruder than real
// block scoping (doesn't distinguish sibling if/else branches), but good enough in practice. `typedPrefix` filters by
// case-insensitive substring-from-start, same convention the old word-list autocomplete used. `excludeVariable`
// (optional) drops one specific VariableDeclaration from the in-scope-variables candidates -- used when
// re-editing an EXISTING declaration's own initializer, so e.g. `float test = ...` can't offer `test` itself
// as its own value (self-referential, meaningless before the declaration completes).
export class AutoCompleteRules
{
public:
	// `offerComparisonLeads` (Bool slots only) additionally offers NUMERIC variables/functions as a
	// comparison's left operand ("bool b = i < 5") -- passed only by value flows that support the comparator
	// continuation (the editor refuses such a lead as a direct value; only a typed comparator consumes it).
	static oc::vector<Candidate> candidatesFor(DSLType expectedType, const DSLCodeLine& atLine, const DSLScriptFile& file,
		const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
		const oc::string& typedPrefix, DSLSymbol* excludeVariable = nullptr, bool offerComparisonLeads = false);

	// For an if/while condition's FIRST operand, whose type isn't known yet (it's what FIXES the type the
	// comparator/second operand must then match) -- every in-scope variable and every non-Void-returning
	// function, regardless of type. No free-typed literal option here: a bare literal's type would be
	// ambiguous (Int? Float? String?) with nothing yet to disambiguate it against -- comparisons in this DSL
	// always lead with a variable/call (`height <= 0.1`, `counter < 5`), matching the example program.
	// `excludeVariable` (optional) drops one declaration -- a declaration's own initializer (or a `&&`/`||`
	// chain grown from it) must never be able to reference the not-yet-existing variable it's building.
	// `offerLiterals` lifts the no-literal rule above for the one slot where nothing CAN disambiguate a
	// literal's type and that's fine: a VARIADIC callee's extra arguments (printf's values), which have no
	// declared parameter to match against. The typed text picks its own type -- quoted = String, digits = Int,
	// digits with a '.' = Float -- plus true/false for Bool, exactly what a C vararg can carry.
	static oc::vector<Candidate> candidatesForAnyValue(const DSLCodeLine& atLine, const DSLScriptFile& file,
		const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
		const oc::string& typedPrefix, DSLSymbol* excludeVariable = nullptr, bool offerLiterals = false);

	// Functions/members reachable by dotting into a receiver of `receiverType` -- a binding object (self) or any
	// engine-defined STRUCT value (vec3 and friends). `receiverDecl` is the chain's ROOT declaration, carried
	// into every candidate (`receiver` for functions, `refSymbol` for members) -- for a chained access
	// (`self.pos.x`, `self.physics.applyImpulse(...)`) the type is the PATH END's, while the root stays the
	// decl. expectedType Void + !anyValue = statement context (every function -- a call statement may ignore its
	// result -- plus WRITABLE members, the lead-in of a member assignment, plus chainable-but-unwritable members
	// like self.physics as dot-into waypoints); anyValue = condition-lead style (non-Void functions + every
	// member); otherwise a value slot of that type (chainable members always pass, so a chain can continue
	// through them toward a matching leaf). `document` gates component-bound members (self.physics/audio/force)
	// by DSL::requiredComponents, the receiver-side twin of appendBindingObjects' top-level gating.
	// `receiverWritable` is false once ANY member already walked to reach this receiver is read-only -- being
	// read-only propagates down the whole chain (assigning into `self.parent.pos` would mutate an object the
	// script only has read access to), so no member is offered as an assignment lead-in under such a path;
	// chainable ones still are, since dotting further toward a call stays legal.
	// `receiverIsRoot` = the receiver expression is the root declaration ITSELF, with no member path walked yet.
	// Component members need it: they're self's HOST-CACHED pointers, so `self.physics` is valid but
	// `self.parent.physics` is not -- both are Entity-typed and share the same root, and only the path length
	// tells them apart (the other entity's component is reached with `ifexist` instead).
	// `atLine`/`file`/`receiverPath` locate the receiver in the document, which is what the mutate-while-
	// iterating refusal needs: a container-mutating function (push/clear) isn't offered while a foreach/ifexist
	// in an enclosing block is reading that same storage.
	static oc::vector<Candidate> receiverCandidates(const ScriptBindings& bindings, const DSL& document, DSLSymbol* receiverDecl,
		DSLType receiverType, DSLType expectedType, bool anyValue, bool receiverWritable, bool receiverIsRoot,
		const DSLCodeLine* atLine, const DSLScriptFile& file, const oc::string& receiverPath,
		const oc::string& typedPrefix);

	// The six comparison operators (==, !=, <, >, <=, >=) offered while building an if/while condition's middle
	// term, filtered by typedPrefix same as any other candidate list (typing "<" narrows to "<" and "<=").
	// `equalityOnly` drops the four ORDERING operators, leaving == and != -- what an Entity comparison gets
	// (identity/null tests are meaningful, "one entity is less than another" isn't).
	static oc::vector<Candidate> comparisonOperatorCandidates(const oc::string& typedPrefix, bool equalityOnly = false);
	// Whether comparing `type` should be restricted to == / != (see comparisonOperatorCandidates).
	static bool isEqualityOnlyType(DSLType type);

	// The five compound-assign operators (+=, -=, *=, /=, %=) offered while building a for-loop's increment
	// clause (`counter += 1`) -- a plain `=` isn't offered here, matching the example program's convention that
	// a for-loop's increment always compounds the loop variable rather than replacing it outright.
	static oc::vector<Candidate> compoundAssignOperatorCandidates(const oc::string& typedPrefix);

	// The five arithmetic operators (+, -, *, /, %) -- offered when REPLACING an arithmetic operator inside an
	// existing expression chain in place (see ScriptEditor's ReplaceOperator mode).
	static oc::vector<Candidate> arithmeticOperatorCandidates(const oc::string& typedPrefix);

	// The two logical operators (&&, ||) -- offered when replacing one inside an existing logical chain.
	static oc::vector<Candidate> logicalOperatorCandidates(const oc::string& typedPrefix);

	// All six assign-class operators (=, +=, -=, *=, /=, %=) -- offered when replacing an assignment
	// statement's (or a for-increment's) own operator in place. Superset of compoundAssignOperatorCandidates:
	// unlike a for-loop being AUTHORED, an existing statement's operator may become a plain `=` too.
	static oc::vector<Candidate> assignOperatorCandidates(const oc::string& typedPrefix);

	// What DSLType belongs at `slot` (DSLType::Void = a statement slot). Derived from the slot's STRUCTURAL
	// position (which field of which parent it is), not from whatever symbol currently occupies it -- so
	// replacing an already-filled real value offers the same, structurally-correct candidates a placeholder
	// in the same spot would.
	static DSLType expectedTypeForSlot(const SlotRef& slot, const DSLScriptFile& file);

	// Walks `file.lines` backward from `atLine` to its enclosing FunctionDeclaration header and returns its
	// declared return type (DSLType::Void if none is found).
	static DSLType enclosingFunctionReturnType(const DSLCodeLine& atLine, const DSLScriptFile& file);

	// Whether `name` is a word the GRAMMAR itself gives meaning to -- a flow-control/declaration keyword (if,
	// elseif, else, while, for, return, break, function, end), a boolean literal (true, false), a type keyword
	// (int/float/bool/string plus every registered struct name, the same set typeKeywordCandidates offers), or
	// "ctx" (the Transpiler's auto-injected context parameter, see Transpiler::emitFunction). A variable/
	// function/parameter named one of these would silently misparse as the keyword when the document round-
	// trips through ScriptLoader's plain text-based parser (which matches these words literally) -- the ONE
	// place this list lives, so isNameInScope/isFunctionNameTaken/the editor's parameter check/the loader's own
	// checks can't drift apart.
	static bool isReservedWord(const oc::string& name);

	// Whether `name` collides with any variable a declaration AT atLine would share scope with (sidebar bindings
	// always count too), OR with any FUNCTION name (builtin or user-declared, same set isFunctionNameTaken
	// checks) -- used to block declaring or renaming a variable to an already-taken name. The function check
	// matters beyond naming clarity: a local variable shadowing a function of the same name makes that function
	// uncallable for the rest of its scope in the transpiled C++ (a local hides a free function of the same
	// name, same as plain C++), so this isn't just a style rule.
	// Two-directional, both block-scope-aware (same rule inScopeVariables uses): BACKWARD catches anything
	// currently reachable from atLine (a name already used earlier in an enclosing/still-open block); FORWARD
	// catches anything declared from atLine through the close of ITS OWN enclosing block, at any nesting depth
	// (nested blocks always see outward, so a name chosen here would collide with one a LATER nested branch
	// already uses, even though that inner name isn't reachable FROM atLine yet -- e.g. renaming an outer
	// variable to match one a later nested if-branch declares). A name from an ALREADY-CLOSED sibling block (a
	// finished loop's own counter, a past if-branch's own local) collides with neither direction, so it's free
	// to reuse.
	// `excludeVariable` (optional) ignores one declaration -- e.g. a rename's own prior name shouldn't count
	// as a collision against itself.
	static bool isNameInScope(const oc::string& name, const DSLCodeLine& atLine, const DSLScriptFile& file,
		const oc::vector<oc::unique_ptr<DSLSymbol>>& sidebar, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
		DSLSymbol* excludeVariable = nullptr);

	// Whether `text` reads as a valid literal of `type` (String: always; Int/Float: must look like a real
	// number -- see candidatesFor's literal-candidate check). Exported so ScriptEditor can apply the SAME rule
	// when validating vec2/3/4's comma-separated component list, so both places agree on "a valid number".
	static bool isValidLiteralText(DSLType type, const oc::string& text);

	// The same fixed list of primitive type keywords (int/float/bool/string/vec2/vec3/vec4) offered wherever a
	// TYPE is being picked rather than a value -- declaring a local variable (candidatesFor's Void branch),
	// declaring a new function's parameter, or setting a function's return type. One shared list so all three
	// stay in sync; each Candidate carries Kind::DeclareType (picking a type is picking a type, regardless of
	// what it's for -- the caller decides what to do with `declareType`).
	static oc::vector<Candidate> typeKeywordCandidates(const oc::string& typedPrefix);

	// Whether `name` is already used by ANY function -- a builtin (vec2/3/4, print, component methods) or a
	// user-declared one -- so a newly-declared function can't collide with or shadow an existing name. Unlike
	// isNameInScope, this is global (function names aren't scoped to where they're declared).
	// `excludeFunction` (optional) ignores one user declaration -- a re-authored function's own current name
	// shouldn't count as a collision against itself.
	static bool isFunctionNameTaken(const oc::string& name, const DSLScriptFile& file, const oc::vector<oc::unique_ptr<DSLSymbol>>& builtins,
		DSLSymbol* excludeFunction = nullptr);

	// Whether `varDecl` (a local variable's own VariableDeclaration) is used by any VariableReference anywhere
	// in its enclosing function -- guards deleting the line that declares it (see ScriptEditor::handleKeyEvent's
	// Backspace handling): a still-used declaration can't be removed out from under its uses.
	static bool isVariableReferenced(const DSLSymbol* varDecl, const DSLScriptFile& file);

	// Whether `funcDecl` is called anywhere in the document (any FunctionCall::functionSymbol pointing at it) --
	// guards deleting an (empty) function declaration the same way isVariableReferenced guards a local's. Global,
	// not scoped to one function, since a function can be called from anywhere (see knownFunctions).
	static bool isFunctionReferenced(const DSLSymbol* funcDecl, const DSLScriptFile& file);
	// The finer sibling: whether any call site CONSUMES `funcDecl`'s return value (a call anywhere but a line's
	// own head). What return-type edits guard on -- a call STATEMENT ignores the result, so a statement-only-
	// called function can change or drop its return type without breaking a single call site, while parameter
	// changes (which every call shape carries typed arguments for) still need isFunctionReferenced.
	static bool isFunctionResultUsed(const DSLSymbol* funcDecl, const DSLScriptFile& file);
};
