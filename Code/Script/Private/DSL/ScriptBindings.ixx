export module Script:ScriptBindings;

import Core;
import :DSL;

// The engine-exposure registry for the DSL: the ONE table describing what scripts can touch. Exposing an
// engine function that already exists on the ScriptContext ABI (Code/Script/Public/ScriptAPI.h) costs exactly
// one BindingFunc row here; exposing brand-new C++ additionally costs the ABI's own minimal trio (X-macro row +
// thunk + ctor init line in Code/Entity/Private/ScriptContext.cpp).
//
// The transpilation model (Transpiler.cpp): each DSL function becomes its own free `SCRIPT_EXPORT` function,
// like the node system's generated .scr code -- no wrapper class, no per-component member. `ctx`/`self` are
// auto-injected as every generated function's first two parameters (invisible to the DSL author), so a
// binding's `emit` template can reference them directly ("$r" = the receiver's own emitted expression, "$1..$n"
// = arguments): self's own functions/members call straight through the real ctx-> thunks (e.g.
// "ctx->entitySetEnabled($r, $1)"), while a component member (self.physics) resolves to "scriptData-><name>" --
// a pointer the HOST caches once, engine-side, right after allocating ScriptData (see ScriptRequiredComponentsFn
// in ScriptAPI.h and ScriptComponent::spawn), not a per-access ctx-> fetch. A script declares which components
// it requires (DSL::requiredComponents); assignment-time validation (future work) guarantees required members
// are never null.
//
// EXTENSIBLE: registerStruct/registerObject/registerEntryPoint are public, so any library can expose its own
// script bindings -- this registry has NO built-in content of its own (not even vec2/3/4 or self): every DSLType
// it knows about, structs and components alike, was registered by SOME caller through this exact same path.
// registerStruct returns the DSLType it just assigned immediately, usable right away in a LATER registration
// call's own parameter/return types; typeByName is the same lookup for an EXISTING struct (e.g. vec3) a new
// registration wants to reference without needing that earlier call's return value handed to it directly.
// Registration order matters: nothing here happens automatically or lazily, so the ENGINE (Entity's
// registerScriptDslBindings, called once from main before anything touches this registry -- see its own
// comment) is what guarantees the base vocabulary (vec2/3/4, self, ...) exists before either the DSL editor or
// any OTHER library's own registration call runs, same as any other cross-library Globals dependency in this
// codebase (see ScriptEventManager::initialize's comment).
//
// Editor-side, the registry replaces the hardcoded builtins: build() constructs the SAME two DSLSymbol vectors
// every consumer (AutoCompleteRules/ScriptLoader/editor) already reads -- sidebar VariableDeclarations for the
// binding objects, builtin FunctionDeclarations for every function (engine free functions plus
// requiresReceiver=true entries for object functions) -- from every struct/object registered SO FAR; a
// registration made AFTER build() runs exists in the registry (queryable via typeByName/structs/objects) but
// never gets symbols built for it, so every registration -- built-in or external -- needs to happen before the
// one build() call (ScriptEditor's, currently). ALL bindings are built regardless of the require set -- stable
// symbol identity forever; DSL::requiredComponents only gates what autocomplete OFFERS and what the loader/
// panel accept.

export struct BindingParam
{
	const char* name;
	DSLType type;
	bool isRef = false;
};

export struct BindingFunc
{
	const char* name;
	DSLType returnType;
	oc::vector<BindingParam> params;
	const char* emit;               // "$r" = the receiver's own emitted expression (empty for a free/Engine call),
	                                 // "$1..$9" = args, "$*" = the variadic tail (see isVariadic)
	bool isPositionalCall = false;  // terse constructors (vec2/3/4): call sites render positionally
	// C varargs (printf): the declared params, then zero or more extra arguments of any value type. "$*" in
	// `emit` expands to those extras, each preceded by ", " (empty when there are none), so a template reads
	// "ctx->logf($1$*)" -- valid with or without a tail. See DSLSymbol::FunctionDeclaration::isVariadic for
	// what it means downstream.
	bool isVariadic = false;
	// This function MUTATES the container it's called on (an array's push/clear): calling it while a
	// foreach/ifexist is iterating that same container is refused, in the editor and the loader alike. A
	// diagnostic for a logic mistake, not a safety mechanism -- element bindings are copies with a
	// write-back, so a missed case can't corrupt anything (see the ref/write-back design).
	bool mutatesContainer = false;
};

export struct BindingMember
{
	const char* name;
	DSLType type;
	oc::string emit;     // e.g. "$r->pos" (a real field) or "scriptData->physics" (a host-cached pointer, see
	                       // registerComponentType) -- "$r" is the receiver's own emitted expression; a
	                       // oc::string (not a literal-only const char*) since registerComponentType computes
	                       // this one from the member name rather than taking it from the caller
	bool writable = true; // false = read-only in the DSL (no `x.member = ...` statements)
	// This member belongs to the SCRIPT, not to the entity it's read through: its emit ignores "$r" (self.data
	// is "(*scriptData)") or resolves against host-cached state (self.physics is "scriptData->physics"), so
	// reaching it off any other entity would silently read this script's own. Legal only directly on `self`,
	// with no member path walked -- enforced in receiverCandidates, ScriptLoader's checkMemberUsable, and the
	// sidebar tree alike. Another entity's components go through `ifexist`; its script data isn't reachable
	// at all, which is correct -- a script's data is private to its own instance.
	bool selfOnly = false;
	// Void = always available; else this member (e.g. self.physics) is only offered/legal while the script
	// requires this component -- always the member's OWN type for a registerComponentType member (see there),
	// but held separately since not every BindingMember is one (e.g. self.pos is never gated). Implies
	// selfOnly, which registerComponentType sets alongside it.
	DSLType requiredComponent = DSLType::Void;
};

// One engine-defined script STRUCT (vec2/vec3/vec4, and whatever the engine exposes later): a value type with
// members, a positional constructor, and callable member functions. Its DSLType is dslStructType(<table row>).
export struct BindingStruct
{
	const char* name;    // DSL spelling ("vec3") -- also the constructor's callable name
	const char* cppName; // transpiled type ("glm::vec3")
	oc::vector<BindingParam> constructorParams;
	const char* constructorEmit;        // "glm::vec3($1, $2, $3)"
	oc::vector<BindingMember> members; // { "x", Float, "$r.x" }
	oc::vector<BindingFunc> functions; // emit may wrap free functions: { "length", Float, {}, "glm::length($r)" }
};

export struct EntryPointParam
{
	const char* name;
	DSLType type;
};

// One ScriptAPI.h entry point (ScriptOnSpawnFn, ScriptUpdateFn, ...) the sidebar can toggle a matching DSL
// function on/off for. `dslParams` are its DSL-visible, LOCKED parameters (deltaSeconds, eventIdx, ...) -- what
// ScriptEditor seeds the function with on toggle-on, and refuses to let the author add to/remove/retype/rename
// afterward. `cppSuffix` is the REST of the real exported signature, verbatim, after "const ScriptContext* ctx,
// Entity* self" -- raw text (not DSLType-driven) because the real ABI has parameters no DSLType can represent
// yet: `scriptData` (opaque per-instance memory, never useful without a DSL-side ScriptData feature) on every
// kind, and OnPhysicsEvent's `other` (an Entity VALUE -- Entity is a binding-only type today, see
// dslIsEngineObjectType) and `contactId` (int64 -- DSLType has no 64-bit integer). Those stay invisible/
// unreadable from DSL body code until the type system grows to cover them; `dslParams`' names/types MUST match
// `cppSuffix`'s leading parameters exactly (same names, same order), since generated code and what the editor
// shows the author must agree. `registerMacro` is the ScriptAPI.h REGISTER_*() call Transpiler emits after the
// function -- a no-op in the DLL build, what makes the STATIC/cooked build find it by kind.
export struct EntryPointDef
{
	const char* name; // the exported C symbol AND the DSL function's own name, e.g. "Update"
	oc::vector<EntryPointParam> dslParams;
	const char* cppSuffix;
	const char* registerMacro;
};

// One binding object ("self", "physics", ...) or the Engine section (objectName == nullptr: its functions are
// FREE calls in the DSL, `ctx.*` in C++). Every object's functions/members are reachable through
// ScriptBindings::objectFor(type) regardless of `sidebarTopLevel` -- physics/audio/force stay full binding
// objects (findReceiverFunction et al. work on them exactly like self does), just not their OWN sidebar root:
// they're reached only by dotting through self's matching member (self.physics), never as a bare identifier.
export struct BindingObject
{
	const char* name;                // the DSL identifier; nullptr = the Engine (free-function) section
	DSLType type;                    // the object's engine-object DSLType (Void for the Engine section)
	bool sidebarTopLevel = true;     // false = no root-level sidebar VariableDeclaration/candidate of its own
	                                  // (see ScriptBindings::build) -- reachable only via another binding's member
	oc::vector<BindingFunc> functions;
	oc::vector<BindingMember> members;
	// Makes this object ITERABLE: "foreach Entity child in self.scene" walks it without the author ever seeing
	// an index, which is what makes an out-of-range access unauthorable. Void elementType = not a sequence.
	// The two emits are ordinary templates ("$r" = the receiver's expression, plus "$i" for the index in
	// `atEmit`) so an engine collection becomes iterable for the cost of one row plus whatever count/at pair
	// the ABI already has -- nothing is copied into script memory. `atEmit` MUST be range-safe on its own
	// terms (sceneGetChildAt returns null past the end); the loop only ever passes indices below countEmit.
	DSLType sequenceElementType = DSLType::Void;
	const char* sequenceCountEmit = nullptr; // "ctx->sceneGetChildCount($r)"
	const char* sequenceAtEmit = nullptr;    // "ctx->sceneGetChildAt($r, $i)"
	// Optional: resolves the receiver ONCE per loop into whatever the count/at pair would otherwise re-resolve
	// per element ("$r" = the receiver). When set, the foreach hoists it into a local and "$r" in the count/at
	// emits above is that local instead of the receiver expression. Only worth declaring when the per-element
	// path does real lookup work (an array unpacks a generation-tagged handle every access); a collection whose
	// at-emit is already a direct call leaves this null and nothing changes.
	const char* sequenceBeginEmit = nullptr; // "ctx->arrayResolve($r)"
	// Optional `ref` write-back through the hoisted local rather than the receiver, for containers whose begin
	// emit already resolved the storage ("$r" = the hoisted local, "$1" = the index, "$v" = the binding). Only
	// consulted alongside sequenceBeginEmit; without it a `ref` element writes through elementSetEmit below,
	// which re-resolves from the receiver.
	const char* sequenceElementSetEmit = nullptr; // "static_cast<T*>($r.data)[$1] = $v"
	// Optional: the element as a writable LVALUE ("$r" = the hoisted local, "$i" = the index). When a container
	// declares one, a `ref` binding becomes a C++ reference straight into the storage instead of a copy plus a
	// write-back at block end -- no load, no store, and no window in which the two can disagree. Needs the same
	// validity as the hoisted local itself, so it is only meaningful alongside sequenceBeginEmit.
	const char* sequenceElementRefEmit = nullptr; // "static_cast<T*>($r.data)[$i]"

	// Optional: the `ifexist` twin of sequenceElementRefEmit -- a POINTER to the looked-up element over "$r"
	// (the receiver) and "$1" (the key), null on a miss. The pointer IS the condition, so a `ref` binding
	// becomes a reference into the storage with no default-initialized copy and no store at block end.
	const char* lookupRefEmit = nullptr; // "static_cast<T*>(ctx->arrayElem($r, $1, (int)sizeof(T)))"

	// Makes this object LOOKUP-ABLE by `ifexist`: "ifexist float item in <this> at <key>" binds one element
	// only when the lookup succeeds, so a failed/out-of-range access can't be written at all. `lookupEmit` is a
	// BOOLEAN expression over "$r" (the receiver), "$1" (the key) and "$v" (the bound variable, already declared
	// and default-initialized by the generated code) -- an array's is a range-checked read
	// ("ctx->arrayGet($r, $1, (int)sizeof($v), &$v)"), a nullable getter's would be an assign-and-test
	// ("($v = ctx->find($r, $1)) != nullptr"). Void valueType = not lookup-able.
	//
	// Deliberately NOT synthesized from the sequence pair above: a bounds check derived from `sequenceCountEmit`
	// would evaluate the receiver twice, and would be the WRONG check for any container whose lookup can fail
	// for reasons other than range (a map's key miss). Each container states its own.
	DSLType lookupKeyType = DSLType::Void;
	DSLType lookupValueType = DSLType::Void;
	const char* lookupEmit = nullptr;
	// Whether a bound element may be WRITTEN (`ifexist ref`/`foreach ref`) -- the container's own call, the same
	// way BindingMember::writable is the member's. False = the binding is read-only however it's reached, and
	// `ref` on it is refused outright ("this container's elements can't be modified") rather than silently
	// dropping the write. Only meaningful alongside a sequence or lookup registration.
	bool elementWritable = false;
	// The element write-back for a `ref` binding of a BY-VALUE element: a statement expression over "$r", "$1"
	// (the index/key) and "$v". Required when elementWritable and the element type is by-value; a handle-typed
	// element (Entity) needs none -- writes go straight through the handle. See the ref/write-back design: a
	// copy back beats an interior pointer, which push/clear would dangle.
	const char* elementSetEmit = nullptr;
};

export class ScriptBindings
{
public:
	// Registers one struct type (a value type with members/functions/positional constructor, e.g. vec2/3/4) and
	// returns the DSLType it's assigned -- callable from ANY library, at any time (see the class comment). The
	// returned type is immediately usable as a parameter/return type of a LATER registerStruct/registerObject
	// call from anyone; typeByName is the same lookup for an EXISTING struct this call didn't just register.
	DSLType registerStruct(BindingStruct def);
	// Registers one binding object (or the Engine free-function section, when def.name == nullptr) -- same
	// call-from-anywhere/any-time guarantee as registerStruct.
	void registerObject(BindingObject def);
	// Registers one COMPONENT TYPE (e.g. "PhysicsComponent") -- returns the DSLType it's assigned AND, in the
	// SAME call, exposes it as self.<memberName>, emit "scriptData-><memberName>" (writable=false always -- a
	// component handle is a HOST-cached pointer, never assigned; gated by the returned type itself, see
	// DSL::requiredComponents -- a component member is only offered/legal while the script requires it, so the
	// gate and the member's own type are always the same value, never a separate parameter to keep in sync).
	// `componentBit` is the engine's own EComponentID bit for this component (Script has no visibility into
	// that enum -- Entity's registerScriptDslBindings passes it as a plain int); it's what
	// ScriptRequiredComponentsFn's bitmask is built from and what orders ScriptData's cached-pointer fields
	// (see componentBit() below and Transpiler.cpp) -- MUST match the bit the engine itself scans in
	// ScriptComponent::spawn, since neither side can see the other's ordering otherwise. Everything reachable
	// off self/Entity is a component by construction: there is no separate step that patches self's own member
	// list, so an external library registering its own component type (e.g. Particle exposing self.particle)
	// never touches -- or even needs to know about -- self's registration, which the caller (Entity's
	// registerScriptDslBindings) already guaranteed ran before this call. The returned type is also usable as
	// a BindingObject's own `type` (see registerObject) -- e.g. "physics"'s own applyImpulse/getVelocity/...
	// functions, reachable only by dotting through self.physics, matching the sidebarTopLevel=false pattern --
	// with no positional-call shape to it (unlike registerStruct), since a component is fetched, never
	// constructed.
	// `fetchEmit` reaches the component off an ARBITRARY entity ("ctx->entityGetPhysicsComponent($r)"), which
	// can come back null -- what `ifexist` emits for a component type (see componentFetchEmit). The self.<memberName> member
	// registered here is the OTHER path: a pointer the host cached for this script, non-null by construction
	// because //@@require guarantees the component exists, and offered only on a `self` receiver.
	DSLType registerComponentType(const char* memberName, const char* typeName, int componentBit, const char* fetchEmit);
	// Registers one SEQUENCE type -- a read-only view over an engine collection, exposed by the member/accessor
	// that names it ("self.scene.children"). It SPELLS as "<element>[]" (see dslTypeName): to the author it's an
	// indexable collection like any other, and only the emits differ. Nothing is copied -- the exposing member's
	// emit is just "$r", so the value IS its receiver's expression, and these templates run against it:
	// `countEmit` with "$r", `atEmit` with "$r" plus "$i" for the index, and `lookupEmit` (optional) with "$r",
	// "$1" for the key and "$v" for the bound variable, which is what makes it usable from `ifexist` as well as
	// `foreach`. `atEmit` must be range-safe on its own terms; `lookupEmit` must REPORT the miss.
	// Distinct from an ARRAY (T[]), which is storage the script owns a handle to and can write through.
	// `elementSetEmit` (optional) makes the elements WRITABLE: a statement over "$r", "$1" (the index) and "$v"
	// that applies the bound element back to the engine ("ctx->lightSetAt($r, $1, &$v)") -- what a `ref`
	// binding's block-end write-back emits. Without it the view stays read-only and `ref` is refused.
	DSLType registerSequenceType(const char* name, DSLType elementType, const char* countEmit, const char* atEmit,
		const char* lookupEmit = nullptr, const char* elementSetEmit = nullptr);
	// Registers one OPTIONAL type -- a value that may not be there ("self.parent" at the root). Spelled
	// "<value>?" (see dslTypeName) and reachable ONLY through `ifexist`, which is the point: it is not a value
	// of its own, so it cannot be read, passed, or dotted into without the existence check having succeeded,
	// and there is no way to forget one. Same lookup contract as a keyed container minus the key -- `lookupEmit`
	// is a boolean expression over "$r" (the exposing member's receiver) and "$v" (the bound variable), e.g.
	// "($v = $r->parent) != nullptr". The exposing member's own emit is just "$r", so nothing is fetched twice.
	DSLType registerOptionalType(const char* name, DSLType valueType, const char* lookupEmit);
	// Registers one NAMESPACE type -- a binding object with no runtime value at all, just a name to group
	// functions under ("math.sin(x)"). Its functions require a receiver like any other object's, which is what
	// makes them reachable ONLY through the prefix; their emits simply never mention "$r", so nothing about the
	// receiver reaches the generated C++ (which calls glm/std directly).
	DSLType registerNamespace(const char* name);

private:
	// Allocates a name + DSLType from the derived-type range and fills in the parts every derived kind shares.
	// Which half of `outObject` the caller then fills is what distinguishes sequence / optional / namespace.
	DSLType allocateDerivedType(const char* name, BindingObject& outObject);

public:
	// Registers one toggleable ScriptAPI entry point (see EntryPointDef) -- same guarantee.
	void registerEntryPoint(EntryPointDef def);

	// `name`'s DSLType among every struct OR component type registered so far (Void if neither spells `name`) --
	// how a NEW registerStruct/registerObject/registerComponentType call, from this library or any other,
	// references an EXISTING struct/component type (e.g. "vec3", "PhysicsComponent") as one of its own
	// parameter/return/member types, without needing that earlier call's DSLType handed to it directly.
	DSLType typeByName(const oc::string& name) const;
	// Same lookup, but ONLY among registered component types (Void if `name` isn't one, including when it's a
	// struct name instead) -- what DSL::requiredComponents / the .dsl "//@@require" line resolves a name
	// against, where a struct match would be meaningless.
	DSLType componentTypeByName(const oc::string& name) const;

	// Constructs the sidebar/builtin symbol vectors from every struct/object registered SO FAR -- call ONCE (ScriptEditor currently),
	// after every registration whose symbols should exist (symbol identity must be stable for the lifetime of
	// every document that references them); a registration made after this call exists in the registry but
	// never gets symbols built for it.
	void build(oc::vector<oc::unique_ptr<DSLSymbol>>& sidebarOut, oc::vector<oc::unique_ptr<DSLSymbol>>& builtinsOut);

	// The full table, in registration order (built-ins first; the Engine section last among them).
	oc::span<const BindingObject> objects() const;

	// The BindingObject a receiver's engine-object type belongs to (nullptr for non-object types).
	const BindingObject* objectFor(DSLType type) const;
	// The sidebar VariableDeclaration symbol built for an object (nullptr for the Engine section).
	DSLSymbol* objectDecl(const BindingObject& object) const;
	// The reverse: which object a sidebar declaration is (nullptr if it isn't one of ours).
	const BindingObject* objectForDecl(const DSLSymbol* sidebarDecl) const;

	// The built FunctionDeclaration symbols of one object, parallel to its `functions` vector.
	oc::span<DSLSymbol* const> functionSymbols(const BindingObject& object) const;

	// The member definition behind a receiver-type + name pair (nullptr = no such member) -- binding objects
	// AND struct types alike; what stamps MemberAccess::type at author/load time.
	const BindingMember* findMember(DSLType receiverType, const oc::string& name) const;

	// `type`'s registered name (nullptr if `type` isn't a registered component type) -- the reverse of
	// registerComponentType/typeByName, what dslTypeName renders a component-type DSLType as.
	const char* componentTypeName(DSLType type) const;
	// Same, for a registered SEQUENCE type (nullptr if `type` isn't one).
	const char* sequenceTypeName(DSLType type) const;
	// `type`'s "fetch it off this entity" emit template, "$r" being the entity (nullptr if `type` isn't a
	// registered component type) -- what `ifexist` binds a component variable from. Distinct from the cached
	// self.<component> member, which needs no fetch at all; see registerComponentType.
	const char* componentFetchEmit(DSLType type) const;
	// `type`'s registered EComponentID bit (-1 if `type` isn't a registered component type) -- what
	// Transpiler.cpp sorts ScriptData's required-component fields by and folds into ScriptRequiredComponents'
	// bitmask; see registerComponentType.
	int componentBit(DSLType type) const;

	// How `type` spells itself in the TRANSPILED C++ -- the single source of truth for that mapping, shared by
	// the transpiler's own declarations and the array emit templates built in build() (whose element type must
	// match what the ABI writes: `Entity*` for entities, `const char*` for strings, the value type otherwise).
	const char* cppTypeName(DSLType type) const;

	// ---- structs ----
	oc::span<const BindingStruct> structs() const; // every struct registered so far, in registration order
	const BindingStruct* structFor(DSLType type) const; // nullptr for non-struct types
	// The struct's own member functions' built symbols, parallel to structFor(type)->functions.
	oc::span<DSLSymbol* const> structFunctionSymbols(DSLType type) const;

	// The emit template a built function symbol was declared with (nullptr if not a registry symbol) -- what
	// Transpiler's callText substitutes against.
	const char* emitFor(const DSLSymbol* funcDecl) const;

	// The toggleable ScriptAPI entry points registered so far (the 5 built-in ones -- OnSpawn/OnDestroy/Update/
	// OnEvent/OnPhysicsEvent -- plus any registerEntryPoint addition), in registration order. Doesn't need
	// `build()`, unlike objects()/structs()' BUILT symbols.
	oc::span<const EntryPointDef> entryPoints() const;
	// `name`'s matching EntryPointDef (nullptr if `name` isn't registered).
	const EntryPointDef* entryPointFor(const oc::string& name) const;

private:
	struct BuiltObject
	{
		const BindingObject* def = nullptr;
		DSLSymbol* decl = nullptr;              // the sidebar VariableDeclaration (null for the Engine section)
		oc::vector<DSLSymbol*> functionSymbols; // parallel to def->functions
	};
	struct BuiltStruct
	{
		const BindingStruct* def = nullptr;
		DSLSymbol* constructorFunc = nullptr;    // the positional constructor builtin ("vec3(...)")
		oc::vector<DSLSymbol*> functionSymbols; // parallel to def->functions
	};

	// Registered defs (registerStruct/registerObject/registerComponentType/registerEntryPoint).
	oc::vector<BindingStruct> m_structDefs;
	oc::vector<BindingObject> m_objectDefs;
	oc::vector<const char*> m_componentTypeNames; // index N = DSLType dslComponentType(N)
	oc::vector<int> m_componentBits;              // parallel to m_componentTypeNames -- the EComponentID bit
	oc::vector<const char*> m_componentFetchEmits; // parallel too -- see componentFetchEmit
	oc::vector<const char*> m_sequenceTypeNames;  // index N = DSLType dslSequenceType(N)
	oc::vector<EntryPointDef> m_entryPointDefs;

	// Symbols built from the above by build() -- called once, well after every registration that should be
	// included.
	oc::vector<BuiltObject> m_built;
	oc::vector<BuiltStruct> m_builtStructs;     // index N = DSLType dslStructType(N)
	oc::vector<oc::pair<const DSLSymbol*, const char*>> m_emits; // function symbol -> emit template
	// Backing storage for the array types' generated emit templates (see build()). Everything else's emits are
	// string literals owned by the caller; these are composed per element type, so they need an owner with a
	// stable address -- unique_ptr, since the vector itself reallocates as more are added.
	oc::vector<oc::unique_ptr<oc::string>> m_arrayEmits;
};

// The one registry instance (house singleton pattern) -- registrations (built-in and external alike) accumulate
// into it from wherever they're called, build() constructs symbols from them once (the Script Editor's own call,
// currently); consulted by dslTypeName (struct names), the loader, and the transpiler too.
export namespace Globals
{
	ScriptBindings scriptBindings;
}
