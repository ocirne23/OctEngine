export module Script:DSL;

import Core;

// ---------------------------------------------------------------------------
// Grammar overview (see ScriptLang.ixx's example program for the full spec).
//
// OWNERSHIP MODEL: a DSLCodeLine's `symbols` vector owns EVERY symbol that
// appears anywhere in that logical line/statement, flattened -- not just its
// top-level symbol(s). E.g. `physics.getMass() * force` on one line produces
// several peer entries in that line's `symbols` (the VariableReference to
// `physics`, the FunctionCall to `getMass`, the VariableReference to `force`,
// and the outer Expression tying them together via `*`) rather than a tree of
// unique_ptrs nested inside each other. Structural relationships (which
// symbol is whose operand/argument/receiver) are expressed purely through
// the raw DSLSymbol* cross-reference fields below, which may point at a peer
// earlier in the SAME line, or (for VariableReference::declaration and
// FunctionCall::functionSymbol) at a symbol owned by an EARLIER line, the
// DSL::sidebar list, or a static builtin registry (vec3/print/component
// methods -- not user-authored, owned by ScriptLang/Transpiler, not by any
// DSLCodeLine). This is deliberate: every sub-expression is its own flat,
// independently addressable entry, which is exactly what the editor needs --
// "select just the `getMass` call out of a larger expression" is "select one
// entry in this line's symbols", not a tree-navigation problem.
//
// Storage order within a line's `symbols` is POST-ORDER: every symbol a
// statement depends on (its operands/arguments/nested calls) is appended
// before the statement's own head symbol, so `symbols.back()` is always the
// line's primary/defining symbol (the FunctionDeclaration on a function
// header line, the top-level FunctionCall or Expression on a statement
// line). This is a construction/bookkeeping convention only -- it has no
// bearing on reading/render order, which the Syntax formatter derives
// by walking the pointer relationships, not by iterating this vector in order.
// Builtin function declarations (vec3/print/physics.applyForce/...) are NOT
// owned by any line at all -- they live in a static registry (ScriptLang /
// Transpiler), so a FunctionCall's `functionSymbol` for a builtin points
// outside the document entirely.
//
// Block nesting (if/while/for/function bodies) is expressed ENTIRELY through
// DSLCodeLine::scopeLevel, never through a stored "end" symbol/line -- the
// closing `end` shown in the example syntax is a SYNTHETIC render artifact
// the formatter (Syntax, in ScriptLang.ixx) inserts wherever scopeLevel steps
// back down. There is nothing to desync (only one source of truth), and
// "delete this if" is a single well-defined operation on its header line,
// never "delete two independently-existing lines that happen to pair up".
// Blank lines used for visual spacing in the example are likewise not stored
// -- purely a formatter choice, not part of the model.
//
// Per-line-kind symbol sequences (DSLCodeLine::symbols), the working convention:
//   variable declaration (with or without an initializer) : [VariableDeclaration]
//     (VariableDeclaration::initialValue holds the initializer symbol directly --
//     no separate "assign" symbol; the value's own dependent symbols are still
//     owned as earlier peers on this line, VariableDeclaration itself stays head)
//   bare assignment/compound-assign : [Expression]
//   call statement                  : [FunctionCall, ...its nested sub-expressions as peers]
//   if / elseif / while             : [FlowControl{control, condition}, ...condition's nested peers]
//   else                             : [FlowControl{control=Else, condition=nullptr}]
//   for                              : [..., VariableDeclaration(loop var) [own initialValue = init clause],
//                                       ..., Expression(condition), ..., Expression(increment),
//                                       FlowControl{control=For, forLoopVar, forCondition, forIncrement}]
//     (the three comma-separated clauses are referenced directly by FlowControl's
//     dedicated for* fields -- not discovered by scanning the line -- so the loop
//     var/condition/increment symbols just need to be SOME earlier peers on this
//     line, in whatever order their own dependencies require)
//   return <value>                  : [FlowControl{control=Return, condition=returnValueExpr}]
//   return (bare) / break            : [FlowControl{control, condition=nullptr}]
//   function declaration header     : [FunctionDeclaration]
//     (parameters live on FunctionDeclaration::parameterVarDeclarations --
//     symbols owned by this same header line, referenced directly by that field --
//     not discovered by scanning the line, same convention as `for`'s clauses above)
//
// `ref` rendering: there is no standalone "ref" symbol/keyword token. A
// parameter's VariableDeclaration::isRef marks it as by-reference; the
// formatter renders the `ref` keyword wherever that parameter is declared, at
// a call site that targets it, or (checking VariableReference::declaration)
// wherever it's assigned into (`ref appliedForce = toApply`).
//
// Comparison operators: DSLOperator has exactly one canonical form per
// operator, rendered the same symbolic way ("<=", not "lesseq") in both
// compact and expanded view.
// ---------------------------------------------------------------------------

export class DSLCodeLine;

// A type is either one of the fixed builtin kinds below, or a DYNAMIC engine-defined struct or COMPONENT type:
// value `FirstStruct + N` names entry N of the ScriptBindings struct registry (vec2/vec3/vec4 are the first
// three table rows), `FirstComponentType + N` entry N of the component-type registry (e.g. PhysicsComponent/
// AudioComponent/ForceComponent -- none hardcoded here; whichever library owns that engine component registers
// its own type by name, see ScriptBindings::registerComponentType) -- adding a new one of either is one
// registry call, no enum edit. Struct types carry members, a positional constructor, and callable member
// functions; component types carry members/functions too, but no constructor/positional-call shape (a
// component handle is FETCHED off its owning entity, e.g. self.physics, never constructed).
export enum class DSLType : uint16
{
	Void,               // no value (a function with no return type)
	Int,
	Float,
	String,
	Bool,
	Function,           // reserved: a symbol whose value IS a function reference; unused by the current example
	World,              // -- sidebar-bindable engine object kinds (never user-declarable; see
	Entity,             //    dslIsEngineObjectType below and the ScriptBindings registry) --
	// The document's OWN persistent per-instance data (self.data.*, see DSL::dataFields) -- a binding like the
	// registered component types above (dotted into, never a value/declarable type -- dslIsEngineObjectType
	// covers it too), but PER-DOCUMENT rather than a registry row: self.data's own member ("data") is a real static
	// BindingMember (ScriptBindings.cpp), while ITS members (the fields) are looked up against DSL::dataFields
	// directly wherever a receiver of this type is resolved (ScriptLang.cpp's receiverCandidates, ScriptEditor's
	// chain-building, ScriptLoader's dot-chain parser, Transpiler's memberText).
	ScriptData,
	// The document's OWN named On Event entries (self.events.<name>, see DSL::eventNames) -- same per-document
	// binding pattern as ScriptData: self.events' own member ("events") is a real static BindingMember, while
	// its members (one read-only Int per name, the entry's index) are looked up against DSL::eventNames
	// directly. The index IS the name's position in that list -- also what Transpiler emits ScriptEventName's
	// switch from, so the two always agree.
	ScriptEvents,
	// Build-time-only sentinel: inside ScriptBindings.cpp's struct table, a BindingFunc's return/param type of
	// ThisBinding means "whichever struct this entry's OWN row belongs to" -- e.g. vec3's "normalized" returns
	// ThisBinding instead of hardcoding vec3's own DSLType, so the row reads identically regardless of which
	// struct it's for. ScriptBindings::build() resolves it to the real dslStructType() index while constructing
	// that struct's FunctionDeclaration symbols; no DSLSymbol ever actually holds this value, so nothing else
	// (dslTypeName, the transpiler, the editor) needs to know about it.
	ThisBinding,
	FirstComponentType = 128, // + registered component-type index (see dslComponentType/dslComponentTypeIndex)
	FirstStruct = 256,        // + struct registry index (see dslStructType/dslStructIndex)
	// + registered SEQUENCE index (see dslSequenceType/registerSequenceType). A read-only, iterate-only view
	// over an engine collection -- what "self.scene.getChildren()" returns. Distinct from an array because
	// there's no storage and no handle behind it: the call emits its RECEIVER, and the sequence's own count/at
	// templates run against that, so iterating one copies nothing. Placed above the struct range, which it
	// therefore caps (still 16k structs).
	FirstSequence = 0x4000,
	// ARRAY of the type in the low bits ("int[]" = ArrayFlag | Int). A composed type rather than a range of its
	// own, so it stacks on top of everything above -- including the two DYNAMIC ranges, which is why it's a high
	// bit and not another FirstX constant. Deliberately NOT nestable: one bit means one level, and "int[][]"
	// isn't representable (nothing offers it either). An array's storage is engine-owned and reached through a
	// handle (see OcArray in ScriptAPI.h); the DSL sees it as a chainable type carrying count/push/clear.
	ArrayFlag = 0x8000,
};

export constexpr bool dslIsComponentType(DSLType type) { return type >= DSLType::FirstComponentType && type < DSLType::FirstStruct; }
export constexpr int dslComponentTypeIndex(DSLType type) { return static_cast<int>(type) - static_cast<int>(DSLType::FirstComponentType); }
export constexpr DSLType dslComponentType(int index) { return static_cast<DSLType>(static_cast<int>(DSLType::FirstComponentType) + index); }

export constexpr bool dslIsArrayType(DSLType type) { return (static_cast<int>(type) & static_cast<int>(DSLType::ArrayFlag)) != 0; }
export constexpr DSLType dslArrayElementType(DSLType type) { return static_cast<DSLType>(static_cast<int>(type) & ~static_cast<int>(DSLType::ArrayFlag)); }
// Arrays don't nest -- an already-array type is returned unchanged rather than gaining a second level.
export constexpr DSLType dslArrayOf(DSLType elementType) { return static_cast<DSLType>(static_cast<int>(elementType) | static_cast<int>(DSLType::ArrayFlag)); }

export constexpr bool dslIsSequenceType(DSLType type) { return type >= DSLType::FirstSequence && !dslIsArrayType(type); }
export constexpr int dslSequenceIndex(DSLType type) { return static_cast<int>(type) - static_cast<int>(DSLType::FirstSequence); }
export constexpr DSLType dslSequenceType(int index) { return static_cast<DSLType>(static_cast<int>(DSLType::FirstSequence) + index); }

export constexpr bool dslIsStructType(DSLType type)
{
	return type >= DSLType::FirstStruct && type < DSLType::FirstSequence && !dslIsArrayType(type);
}
export constexpr int dslStructIndex(DSLType type) { return static_cast<int>(type) - static_cast<int>(DSLType::FirstStruct); }
export constexpr DSLType dslStructType(int index) { return static_cast<DSLType>(static_cast<int>(DSLType::FirstStruct) + index); }

// Engine-object kinds are BINDINGS (dotted into for their members/functions), never values: excluded from
// variable/reassign candidates, literal slots, and declarable types alike. World/Entity are the two ALWAYS
// present; ScriptData/ScriptEvents (self.data/self.events) get the same binding-only treatment despite being
// per-document rather than a ScriptBindings::objectFor() row of their own (every objectFor() call on one of
// them already handles "no match" gracefully -- skip/continue -- which is exactly right for a type resolved
// elsewhere); every DYNAMICALLY registered component type (dslIsComponentType -- e.g. PhysicsComponent/
// AudioComponent/ForceComponent, none hardcoded here, see registerComponentType) is one too.
export inline bool dslIsEngineObjectType(DSLType type)
{
	return type == DSLType::World || type == DSLType::Entity || type == DSLType::ScriptData
		|| type == DSLType::ScriptEvents || dslIsComponentType(type) || dslIsSequenceType(type);
}

// A type with further members/functions reachable by dotting into it -- an engine-defined STRUCT value
// (vec2/3/4), a bound engine-object kind (self.physics and friends), or an ARRAY (count/push/clear, built per
// element type by ScriptBindings::build). What decides whether a member that doesn't itself match a value slot
// is still offered as a dot-into WAYPOINT rather than excluded outright.
export inline bool dslIsChainableType(DSLType type)
{
	return dslIsStructType(type) || dslIsEngineObjectType(type) || dslIsArrayType(type);
}

export enum class DSLFlowControl
{
	If,
	ElseIf,
	Else,
	While,
	For,
	ForEach, // "foreach <type> <name> in <sequence>" -- see FlowControl's comment
	// "ifexist <Type> <name> in <source> [at <key>]" -- binds ONE thing that may not be there and enters only
	// when it was: an array element, a value behind any registered lookup, an optional like self.parent, or an
	// entity's COMPONENT (`ifexist PhysicsComponent p in e`). The DSL's ONLY such read -- there is no unchecked
	// form to author -- so an out-of-range index, an absent value and a missing component are one construct
	// with one answer. `at` applies only to a source whose lookup takes a key.
	IfExist,
	Return,
	Break,
};
export enum class DSLOperator
{
	Assign,
	AssignAdd,
	AssignSubtract,
	AssignMultiply,
	AssignDivide,
	AssignModulus,
	Add,
	Subtract,
	Multiply,
	Divide,
	Modulus,
	Equal,
	NotEqual,
	LessThan,
	GreaterThan,
	LessThanOrEqual,
	GreaterThanOrEqual,
	And, // && -- logical, joining BOOL-valued operands (comparisons, bool values, grouped chains)
	Or,  // ||
};

// The four operator classes (assign / arithmetic / comparison / logical) -- every Expression's operators are
// uniformly one class (see DSLSymbol::Expression below), and which class decides both what an operator span
// offers as replacements and whether a chain operand/operator pair may be deleted (chain operators --
// arithmetic and logical -- only; the two sides of a comparison or assignment aren't individually removable).
// Range checks lean on the enum's declaration order.
export inline bool dslIsAssignOperator(DSLOperator op)     { return op >= DSLOperator::Assign && op <= DSLOperator::AssignModulus; }
export inline bool dslIsArithmeticOperator(DSLOperator op) { return op >= DSLOperator::Add && op <= DSLOperator::Modulus; }
export inline bool dslIsComparisonOperator(DSLOperator op) { return op >= DSLOperator::Equal && op <= DSLOperator::GreaterThanOrEqual; }
export inline bool dslIsLogicalOperator(DSLOperator op)    { return op == DSLOperator::And || op == DSLOperator::Or; }
export inline bool dslIsChainOperator(DSLOperator op)      { return dslIsArithmeticOperator(op) || dslIsLogicalOperator(op); }

export class DSLSymbol
{
public:

	~DSLSymbol() {}

	struct Constant
	{
		DSLType type;
		oc::string value;
	};
	struct TypeDeclaration
	{
		DSLType type;
	};
	// A use of a variable (or sidebar binding) as a value somewhere in an
	// expression -- e.g. the `counter` in `thing += counter`, or the `physics`
	// being dotted into in `physics.getMass()`.
	struct VariableReference
	{
		DSLSymbol* declaration = nullptr; // -> VariableDeclaration
	};
	struct VariableDeclaration
	{
		oc::string name;
		DSLSymbol* typeSymbol = nullptr;   // -> TypeDeclaration
		DSLSymbol* initialValue = nullptr; // optional initializer value; nullptr = no initializer (also unused on parameters)
		bool isRef = false; // by-reference output parameter (only meaningful on a function's parameterVarDeclarations)
		int numReferences = 0; // maintained by the editor: live VariableReference count, guards rename/delete
	};
	// One call argument. `parameter == nullptr` means positional matching
	// (used by terse builtin constructors like vec3(0,1,0)); non-null points
	// directly at the callee's matching parameter VariableDeclaration for
	// named/keyword matching, order-independent (ordinary calls, e.g. `force = 1.0`).
	// For a `ref` parameter, `value` is a VariableReference to the caller's
	// local that receives the output (`ref appliedForce = applied`).
	struct CallArgument
	{
		DSLSymbol* parameter = nullptr; // -> VariableDeclaration (callee's param), or nullptr for positional
		DSLSymbol* value = nullptr;
	};
	struct FunctionCall
	{
		DSLSymbol* functionSymbol = nullptr; // -> FunctionDeclaration (user-authored or a static builtin, e.g. vec3/print/applyForce)
		DSLSymbol* receiver = nullptr;       // -> VariableReference; null = free call (print(...)), non-null = dot-call (physics.getMass())
		oc::vector<CallArgument> arguments;
	};
	struct FunctionDeclaration
	{
		oc::string name;
		oc::vector<DSLSymbol*> parameterVarDeclarations; // -> VariableDeclaration, owned by this decl's header line
		DSLType returnType = DSLType::Void;
		bool requiresReceiver = false; // builtin component methods (physics.getVelocity(), world.rayCast(...)) --
		                                // AutoCompleteRules excludes these from plain call-statement/value
		                                // candidates; they're only reachable by dotting into their receiver's
		                                // BindingObject (ScriptEditor's MemberSelect flow, AutoCompleteRules::
		                                // receiverCandidates)
		bool isPositionalCall = false;  // terse builtin constructors (vec3(0,1,0)) -- editor-inserted calls get
		                                 // positional CallArgument::parameter=nullptr slots instead of named
		                                 // `x = ...` ones, even though parameterVarDeclarations is still populated
		                                 // (it's what tells the editor how many/what-typed placeholders to create)
		// Builtin C varargs (printf): after the declared parameters the call takes ZERO OR MORE extra arguments
		// of any value type. Those extras are POSITIONAL (CallArgument::parameter = nullptr, like a positional
		// call's) and live past parameterVarDeclarations' end, so nothing indexes a declaration for them --
		// every parameterVarDeclarations[argIndex] lookup has to bound-check once this is set. An argument count
		// no longer says when the list is done either: only ')' / Enter closes one (see ScriptEditor's
		// CallArgValue staging). User-declared DSL functions are never variadic -- there's no way to author it.
		bool isVariadic = false;
		// Builtin only: this call MUTATES the container it's made on (an array's push/clear). Calling it while
		// a foreach/ifexist in an enclosing block is reading that same container is refused, DIRECTLY or through
		// any chain of user function calls (ScriptLoader closes the call graph after parsing; a push through a
		// parameter poisons that function for every caller). A SAFETY rule, not just a diagnostic: a POD foreach
		// resolves the element storage once and indexes it raw, so growth mid-loop would dangle the span.
		// Mirrors BindingFunc::mutatesContainer, carried here so consumers read it off the symbol they already
		// have instead of looking the registration back up.
		bool mutatesContainer = false;
		int numReferences = 0; // maintained by the editor: live FunctionCall count, guards rename/delete
	};
	// If/ElseIf/While carry their condition in `condition`; Return carries its
	// value (nullptr for a bare `return`); Else/Break leave everything null.
	// For ignores `condition` and uses its own three dedicated fields instead
	// (a 3-ary node in spirit, without inventing a generic N-ary symbol shape
	// just for this one construct): forLoopVar's own VariableDeclaration
	// carries the init clause via its `initialValue` field.
	// ForEach reuses forLoopVar for its ELEMENT declaration (no initialValue --
	// the loop supplies each value) and `condition` for the SEQUENCE being
	// walked: an array-typed value, or a member/receiver registered as a
	// BindingSequence. No index is ever author-visible, which is what makes it
	// impossible to author an out-of-range access through one.
	// IfExist has that shape plus a key: forLoopVar is the bound declaration,
	// `condition` the SOURCE it comes from (a container, an optional, or the
	// entity a component is fetched off), and forCondition the `at` key (null
	// when the source's lookup takes none -- a component fetch never has one).
	// The bound declaration's `isRef` is what asks for a write-back -- see the
	// Transpiler.
	struct FlowControl
	{
		DSLFlowControl control;
		DSLSymbol* condition = nullptr;
		DSLSymbol* forLoopVar = nullptr;   // For/ForEach/IfExist: -> VariableDeclaration
		DSLSymbol* forCondition = nullptr; // For: -> Expression; IfExist: the `at` key value
		DSLSymbol* forIncrement = nullptr; // For only: -> Expression
	};
	// An operator CHAIN, stored FLAT and exactly as authored: operators.size() == operands.size() - 1,
	// operators[i] sitting between operands[i] and operands[i+1]. operands.size() >= 2, with ONE exception: a
	// `grouped` chain may hold a single operand and no operators at all -- authored parens around a lone value
	// ("(a)") are kept verbatim, deliberate edit anchors rather than redundancy to strip.
	// No precedence is encoded here -- `1 + 2 * 3` is one flat chain -- the transpiler applies ordinary
	// arithmetic precedence (*, /, % over +, -; left-associative) when emitting C++. Explicit parentheses are
	// the ONLY structural nesting: an operand that is itself an Expression with `grouped` set renders in
	// parens and always evaluates as one unit. Grouping is stored user intent, never derived from precedence,
	// which is what lets the editor replace any single operator in place without restructuring anything.
	// Structural statements reuse the same shape as exactly-binary chains: an assignment is [target, value]
	// around an assign-class operator, a comparison [left, right] around a comparison operator -- and by
	// construction those never grow more operands (arithmetic editing wraps their SIDES in nested ungrouped
	// sub-chains instead, so every Expression's operators are uniformly one class -- see ScriptEditor's
	// in-place expression editing).
	struct Expression
	{
		oc::vector<DSLSymbol*> operands;
		oc::vector<DSLOperator> operators;
		bool grouped = false; // explicit parentheses around this whole chain (only meaningful as another chain's operand)
	};
	// Field-style access on a receiver, e.g. `self.pos` or `self.pos.x` -- NOT a call (see
	// FunctionCall::receiver for method-style dot-calls like physics.getVelocity()).
	// `type` is stamped from the ScriptBindings member registry when the access is
	// authored/loaded, so dslValueType stays registry-free.
	struct MemberAccess
	{
		DSLSymbol* receiver = nullptr; // -> VariableReference, or a nested MemberAccess (chained access)
		oc::string memberName;
		DSLType type = DSLType::Void;
	};
	// A whole-line comment ("# ...") -- pure annotation: no cross-references, never a value, freely deletable.
	// The Transpiler emits it as a C++ comment.
	struct Comment
	{
		oc::string text;
	};
	// A yet-unfilled slot -- what keeps the document "always compilable" even mid-edit. `expectedType` names
	// what belongs here: DSLType::Void means a whole STATEMENT slot (AutoCompleteRules offers if/while/return/
	// break/declare-variable/call-statement candidates); any other DSLType means a VALUE slot of that type
	// (offers matching constants/variables/functions). Confirming a candidate REPLACES the placeholder
	// outright -- it never partially resolves. The editor renders one distinctly (e.g. "<float>") and
	// Transpiler emits a type-appropriate default for any left unfilled, so a document with placeholders
	// still compiles.
	struct Placeholder
	{
		DSLType expectedType;
	};

	enum class SymbolType
	{
		Constant,
		TypeDeclaration,
		VariableReference,
		VariableDeclaration,
		FunctionCall,
		FunctionDeclaration,
		FlowControl,
		Expression,
		MemberAccess,
		Comment,
		Placeholder,
	};
	// Named so code building a symbol from one of the alternatives above (e.g. ScriptEditor's pushSymbol helper)
	// can take it as a plain, non-template parameter -- any alternative converts to this implicitly.
	using Data = oc::variant<Constant, TypeDeclaration, VariableReference, VariableDeclaration, FunctionCall, FunctionDeclaration, FlowControl, Expression, MemberAccess, Comment, Placeholder>;

	SymbolType type;
	Data data;
	DSLCodeLine* line = nullptr;
};

export class DSLCodeLine
{
public:
	int scopeLevel = 0;
	int scopeId = 0;

	oc::vector<oc::unique_ptr<DSLSymbol>> symbols;

	// The line's primary/defining symbol -- symbols.back(), per the post-order ownership convention above.
	// Null only for a line with no symbols at all (a transient state mid-edit, never a resting one).
	DSLSymbol* head() const { return symbols.empty() ? nullptr : symbols.back().get(); }
};

export class DSLScriptFile
{
public:
	oc::vector<oc::unique_ptr<DSLCodeLine>> lines;
};

// ---------------------------------------------------------------------------
// Structural queries every layer above (editor ops, autocomplete scoping,
// formatting) keeps needing -- defined once here, next to the model they walk,
// instead of each caller hand-rolling the same index/scan loops.
// ---------------------------------------------------------------------------

// The DSLType `symbol` evaluates to when used as a VALUE -- what constrains candidate lists when editing one
// operand of an existing expression in place. An Expression resolves through its operands: for an assignment
// that's the target's own type, for a comparison/arithmetic chain any resolvable operand (all operands of a
// chain share one type by construction). Void = unresolvable (a statement-only construct).
export inline DSLType dslValueType(const DSLSymbol* symbol)
{
	if (symbol == nullptr)
		return DSLType::Void;
	switch (symbol->type)
	{
	case DSLSymbol::SymbolType::Constant:
		return oc::get<DSLSymbol::Constant>(symbol->data).type;
	case DSLSymbol::SymbolType::VariableReference:
	{
		const DSLSymbol* decl = oc::get<DSLSymbol::VariableReference>(symbol->data).declaration;
		if (decl == nullptr)
			return DSLType::Void;
		const DSLSymbol* typeSymbol = oc::get<DSLSymbol::VariableDeclaration>(decl->data).typeSymbol;
		return typeSymbol != nullptr ? oc::get<DSLSymbol::TypeDeclaration>(typeSymbol->data).type : DSLType::Void;
	}
	case DSLSymbol::SymbolType::FunctionCall:
	{
		const DSLSymbol* callee = oc::get<DSLSymbol::FunctionCall>(symbol->data).functionSymbol;
		return callee != nullptr ? oc::get<DSLSymbol::FunctionDeclaration>(callee->data).returnType : DSLType::Void;
	}
	case DSLSymbol::SymbolType::Placeholder:
		return oc::get<DSLSymbol::Placeholder>(symbol->data).expectedType;
	case DSLSymbol::SymbolType::MemberAccess:
		return oc::get<DSLSymbol::MemberAccess>(symbol->data).type; // stamped from the member registry at author/load time
	case DSLSymbol::SymbolType::Expression:
	{
		const DSLSymbol::Expression& e = oc::get<DSLSymbol::Expression>(symbol->data);
		// A comparison or logical chain EVALUATES to Bool regardless of what its operands are.
		if (!e.operators.empty() && (dslIsComparisonOperator(e.operators[0]) || dslIsLogicalOperator(e.operators[0])))
			return DSLType::Bool;
		for (const DSLSymbol* operand : e.operands)
			if (const DSLType t = dslValueType(operand); t != DSLType::Void)
				return t;
		return DSLType::Void;
	}
	default:
		return DSLType::Void;
	}
}

// Index of `line` within file.lines, or -1 -- the raw-pointer lookup every structural edit/query starts from.
export inline int dslLineIndex(const DSLScriptFile& file, const DSLCodeLine* line)
{
	for (int i = 0; i < static_cast<int>(file.lines.size()); ++i)
		if (file.lines[i].get() == line)
			return i;
	return -1;
}

// Decomposes a value expression into the DECLARATION it roots at plus the dotted member path walked from
// there ("self.data.scores" -> {self, "data.scores"}). False for anything that isn't a plain variable/member
// chain -- a call, a literal, an operator chain. Together the pair IDENTIFIES the storage a value names, which
// is what "is this the same container?" comparisons need (see the mutate-while-iterating rule).
export inline bool dslChainToRoot(const DSLSymbol* value, const DSLSymbol*& outRoot, oc::string& outPath)
{
	outPath.clear();
	while (value != nullptr && value->type == DSLSymbol::SymbolType::MemberAccess)
	{
		const DSLSymbol::MemberAccess& access = oc::get<DSLSymbol::MemberAccess>(value->data);
		outPath = outPath.empty() ? access.memberName : access.memberName + "." + outPath;
		value = access.receiver;
	}
	if (value == nullptr || value->type != DSLSymbol::SymbolType::VariableReference)
		return false;
	outRoot = oc::get<DSLSymbol::VariableReference>(value->data).declaration;
	return outRoot != nullptr;
}

// Whether `varDecl` is the binding a foreach/ifexist HEADER declares, as opposed to an ordinary local or a
// for-loop counter (which own their storage outright). Those two are the only bindings that stand in for
// something living somewhere else, which is what makes `ref` meaningful on them and nowhere else -- and what
// makes a non-ref one refuse to be an assignment target. A predicate about the model, so it lives here rather
// than being restated by each consumer (ScriptLang's candidate rules and ScriptLoader's parse both ask it).
export inline bool dslIsElementBinding(const DSLSymbol* varDecl)
{
	if (varDecl == nullptr || varDecl->line == nullptr)
		return false;
	const DSLSymbol* head = varDecl->line->head();
	if (head == nullptr || head->type != DSLSymbol::SymbolType::FlowControl)
		return false;
	const DSLSymbol::FlowControl& fc = oc::get<DSLSymbol::FlowControl>(head->data);
	return fc.forLoopVar == varDecl
		&& (fc.control == DSLFlowControl::ForEach || fc.control == DSLFlowControl::IfExist);
}

// Whether two value expressions name the SAME storage -- same root declaration, same member path.
export inline bool dslSameStorage(const DSLSymbol* a, const DSLSymbol* b)
{
	const DSLSymbol* rootA = nullptr;
	const DSLSymbol* rootB = nullptr;
	oc::string pathA, pathB;
	return dslChainToRoot(a, rootA, pathA) && dslChainToRoot(b, rootB, pathB) && rootA == rootB && pathA == pathB;
}

// First index past the block opened at `headerIndex` -- every following line with a DEEPER scopeLevel belongs
// to the block's body (see the scopeLevel-only nesting model above), so this is headerIndex + 1 when the body
// is empty, and also correct (trivially) for a non-block line.
export inline int dslBlockEnd(const DSLScriptFile& file, int headerIndex)
{
	const int headerScopeLevel = file.lines[headerIndex]->scopeLevel;
	int i = headerIndex + 1;
	while (i < static_cast<int>(file.lines.size()) && file.lines[i]->scopeLevel > headerScopeLevel)
		++i;
	return i;
}

// Index of the block-header line whose body directly contains `lineIndex` -- the nearest EARLIER line one (or
// more) scope levels shallower -- or -1 at top level. What "am I inside an if branch?" style queries start from.
export inline int dslEnclosingBlockHeader(const DSLScriptFile& file, int lineIndex)
{
	const int scope = file.lines[lineIndex]->scopeLevel;
	for (int i = lineIndex - 1; i >= 0; --i)
		if (file.lines[i]->scopeLevel < scope)
			return i;
	return -1;
}

// Index of the FunctionDeclaration header line enclosing `fromIndex` (walking backward; `fromIndex` itself
// counts if it IS a header -- its parameters belong to it), or -1 when outside any function.
export inline int dslEnclosingFunctionHeader(const DSLScriptFile& file, int fromIndex)
{
	for (int i = fromIndex; i >= 0; --i)
	{
		const DSLSymbol* head = file.lines[i]->head();
		if (head != nullptr && head->type == DSLSymbol::SymbolType::FunctionDeclaration)
			return i;
	}
	return -1;
}

// One field of the document's own persistent per-instance data (self.data.<name>, see DSL::dataFields) --
// authored via the SCRIPT DATA sidebar section's add/remove controls, serialized as one .dsl "//@@data <type>
// <name>" line each. `type` is never an engine-object kind (self.data.self makes no sense) -- only
// Int/Float/Bool/String, an engine-defined struct (vec2/3/4), or an ARRAY of any of those (dslIsArrayType --
// the field then stores a OcArray handle, and the elements live engine-side; see ScriptAPI.h).
//
// ENTITY IS NOT STORABLE, in either form. As a bare field it would dangle the moment the entity died. As an
// array element the engine could hold a refcounted EntityPtr instead -- memory-safe, but then a script silently
// keeps entities ALIVE for as long as it remembers them, which turns a forgotten push into an entity leak with
// nothing to point at. Entities are for iterating and acting on within a frame (foreach over self.scene, a
// spatial query); remembering one across frames needs a generation-checked handle the engine can invalidate,
// which doesn't exist yet.
// Whether a ScriptData field is visible OUTSIDE the script. Hidden (the default) means the field is purely the
// script's own state -- nothing else can see it. Private exposes it to the EDITOR (the Properties panel shows it
// live, the Entity Editor authors its initial value); Public additionally means another entity's script may read
// it, which nothing implements yet -- it is declared here so the authored data survives until it does.
//
// Only SCALAR and STRUCT fields can be anything but Hidden: an array is a OcArray handle whose storage lives
// engine-side, and there is nothing meaningful for an inspector to show or set. Enforced in the SCRIPT DATA
// panel and, as always, again in the loader.
export enum class DSLFieldVisibility : uint8
{
	Hidden,
	Private,
	Public,
};

export const char* dslFieldVisibilityName(DSLFieldVisibility visibility)
{
	switch (visibility)
	{
	case DSLFieldVisibility::Private: return "private";
	case DSLFieldVisibility::Public:  return "public";
	default:                          return "hidden";
	}
}

// Whether `type` may be exposed at all (see DSLFieldVisibility): arrays cannot.
export inline bool dslCanExposeFieldType(DSLType type)
{
	return !dslIsArrayType(type);
}

export struct DSLDataField
{
	oc::string name;
	DSLType type;
	DSLFieldVisibility visibility = DSLFieldVisibility::Hidden;
};

// Top-level document: one script's function bodies plus its sidebar of bound
// engine objects (World/Entity/component instances the body may dot-call
// into -- see ScriptLang.ixx's "sidebar" example section). Kept free of any
// ImGui/editor state on purpose; ScriptEditor owns a DSL instance and layers
// editing/rendering on top of it.
export class DSL
{
public:
	oc::string filePath;
	oc::vector<oc::unique_ptr<DSLSymbol>> sidebar; // VariableDeclaration entries (self + engine-object kinds,
	                                                  // built once by ScriptBindings); line == nullptr. ALL bindings
	                                                  // exist here for stable identity -- requiredComponents below
	                                                  // gates which are offered/legal, never which are built
	DSLScriptFile file;
	// The component TYPES this script REQUIRES on its entity (authored via the sidebar panel's checkboxes,
	// serialized as the .dsl "//@@require" line, matched by name against ScriptBindings::componentTypeName) --
	// always one of the DSLTypes ScriptBindings::registerComponentType assigned, never a struct or anything
	// else. Only required components' bindings appear in autocomplete -- engine-side enforcement (a script only
	// assignable to entities that have every component it required, making every component binding non-null by
	// construction) is separate future work.
	oc::vector<DSLType> requiredComponents;
	// This script's persistent per-instance data fields, reachable in the body as self.data.<name> -- Transpiler
	// emits them as a real "struct ScriptData { ... };" (+ ScriptDataSize()/REGISTER_SCRIPT_DATA_SIZE()) that
	// self.data casts the host's zeroed scriptData block to. Insertion order (append on add, like
	// requiredComponents); names unique within this list (enforced at add time).
	oc::vector<DSLDataField> dataFields;
	// This script's named On Event entries, reachable in the body as self.events.<name> (a read-only Int, the
	// entry's INDEX -- compare against OnEvent's own eventIdx parameter). Insertion order = index order, also
	// what Transpiler's ScriptEventName switch emits from, so the host's name->index resolution agrees with
	// self.events.<name>'s own compile-time value. Names unique within this list (enforced at add time).
	oc::vector<oc::string> eventNames;
};

export inline bool dslIsComponentRequired(const DSL& document, DSLType componentType)
{
	return oc::find(document.requiredComponents.begin(), document.requiredComponents.end(), componentType)
		!= document.requiredComponents.end();
}

// `name`'s field in `fields` (nullptr if none) -- the ONE lookup every self.data.<name> resolution site shares
// (ScriptEditor's chain-building, ScriptLoader's dot-chain parser -- which parses its own file-local dataFields
// before they become DSL::dataFields, hence taking the vector directly rather than a whole DSL -- and
// Transpiler's memberText), so they can't drift apart on what counts as a match.
export inline const DSLDataField* dslFindDataField(const oc::vector<DSLDataField>& fields, const oc::string& name)
{
	for (const DSLDataField& field : fields)
		if (field.name == name)
			return &field;
	return nullptr;
}

// `name`'s index in `names` (-1 if none) -- the ONE lookup every self.events.<name> resolution site shares
// (ScriptEditor's chain-building, ScriptLoader's dot-chain parser, Transpiler's memberText/ScriptEventName), so
// they can't drift apart on what index a name means.
export inline int dslFindEventIndex(const oc::vector<oc::string>& names, const oc::string& name)
{
	for (size_t i = 0; i < names.size(); ++i)
		if (names[i] == name)
			return static_cast<int>(i);
	return -1;
}
