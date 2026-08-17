module Script;

import Core;
import :DSL;
import :ScriptBindings;

namespace
{
	using ST = DSLSymbol::SymbolType;

	DSLSymbol* addSymbol(oc::vector<oc::unique_ptr<DSLSymbol>>& container, ST type, DSLSymbol::Data data)
	{
		auto symbol = oc::make_unique<DSLSymbol>();
		symbol->type = type;
		symbol->data = oc::move(data);
		DSLSymbol* ptr = symbol.get();
		container.push_back(oc::move(symbol));
		return ptr;
	}
}

DSLType ScriptBindings::registerStruct(BindingStruct def)
{
	const DSLType type = dslStructType(static_cast<int>(m_structDefs.size()));
	m_structDefs.push_back(oc::move(def));
	return type;
}

void ScriptBindings::registerObject(BindingObject def)
{
	m_objectDefs.push_back(oc::move(def));
}

DSLType ScriptBindings::registerComponentType(const char* memberName, const char* typeName, int componentBit, const char* fetchEmit)
{
	const DSLType type = dslComponentType(static_cast<int>(m_componentTypeNames.size()));
	m_componentTypeNames.push_back(typeName);
	m_componentBits.push_back(componentBit);
	m_componentFetchEmits.push_back(fetchEmit);
	// self must already be registered by now (guaranteed by the caller -- see Entity's registerScriptDslBindings)
	// -- appended to IN PLACE, not re-registered, so every OTHER caller's earlier self.<member>s survive. The
	// member's own gate IS `type` itself (see DSL::requiredComponents) -- there's nothing else it could sensibly
	// be. Its emit is the host-cached pointer ("scriptData->" + the member's own name), valid precisely because
	// that gate guarantees the component exists; reaching ANOTHER entity's component goes through `ifexist`
	// and componentFetchEmit instead, which is where the null case is handled.
	for (BindingObject& object : m_objectDefs)
		if (object.name != nullptr && oc::string_view(object.name) == "self")
		{
			object.members.push_back({ memberName, type, "scriptData->" + oc::string(memberName),
				/*writable*/ false, /*selfOnly*/ true, /*requiredComponent*/ type });
			break;
		}
	return type;
}

// The three DERIVED access types (sequence / optional / namespace) all take a name from the same range and,
// except for a namespace, a BindingObject carrying only templates -- which half is filled in is the whole
// difference between them, and is what dslTypeName reads back to spell each one. This allocates the type and
// the object; each caller then fills its own half.
DSLType ScriptBindings::allocateDerivedType(const char* name, BindingObject& outObject)
{
	const DSLType type = dslSequenceType(static_cast<int>(m_sequenceTypeNames.size()));
	m_sequenceTypeNames.push_back(name);
	outObject.name = name;
	outObject.type = type;
	// Never a sidebar root of its own: a derived type is only ever reached through whatever produces one.
	outObject.sidebarTopLevel = false;
	return type;
}

DSLType ScriptBindings::registerSequenceType(const char* name, DSLType elementType, const char* countEmit,
	const char* atEmit, const char* lookupEmit, const char* elementSetEmit)
{
	// A BindingObject like any other, so objectFor/findMember/the editor/the transpiler need no new case -- it
	// just happens to carry only the sequence templates.
	BindingObject object;
	const DSLType type = allocateDerivedType(name, object);
	object.sequenceElementType = elementType;
	object.sequenceCountEmit = countEmit;
	object.sequenceAtEmit = atEmit;
	if (lookupEmit != nullptr)
	{
		// Indexable from `ifexist` too.
		object.lookupKeyType = DSLType::Int;
		object.lookupValueType = elementType;
		object.lookupEmit = lookupEmit;
	}
	// Without a write-back the elements stay READ-ONLY (a view over an engine collection, not storage the
	// script owns) and `ref` is refused; with one, `foreach ref`/`ifexist ref` bind a copy that the block's
	// end applies back through it.
	if (elementSetEmit != nullptr)
	{
		object.elementWritable = true;
		object.elementSetEmit = elementSetEmit;
	}
	m_objectDefs.push_back(oc::move(object));
	return type;
}

DSLType ScriptBindings::registerOptionalType(const char* name, DSLType valueType, const char* lookupEmit)
{
	// Fills only the lookup half, and takes no key: sequence* is what makes a type iterable, lookup* is what
	// makes it ifexist-able, and an optional is only ever the latter.
	BindingObject object;
	const DSLType type = allocateDerivedType(name, object);
	object.lookupKeyType = DSLType::Void;
	object.lookupValueType = valueType;
	object.lookupEmit = lookupEmit;
	m_objectDefs.push_back(oc::move(object));
	return type;
}

DSLType ScriptBindings::registerNamespace(const char* name)
{
	// Fills NEITHER half -- which is exactly how dslTypeName tells the three apart (see there). The object it
	// allocates is discarded: the caller registers its own, with the functions the namespace groups.
	BindingObject unused;
	return allocateDerivedType(name, unused);
}

void ScriptBindings::registerEntryPoint(EntryPointDef def)
{
	m_entryPointDefs.push_back(oc::move(def));
}

DSLType ScriptBindings::typeByName(const oc::string& name) const
{
	for (size_t i = 0; i < m_structDefs.size(); ++i)
		if (name == m_structDefs[i].name)
			return dslStructType(static_cast<int>(i));
	return componentTypeByName(name);
}

DSLType ScriptBindings::componentTypeByName(const oc::string& name) const
{
	for (size_t i = 0; i < m_componentTypeNames.size(); ++i)
		if (name == m_componentTypeNames[i])
			return dslComponentType(static_cast<int>(i));
	return DSLType::Void;
}

void ScriptBindings::build(oc::vector<oc::unique_ptr<DSLSymbol>>& sidebarOut, oc::vector<oc::unique_ptr<DSLSymbol>>& builtinsOut)
{
	if (!m_emits.empty())
	{
		assert(false && "ScriptBindings::build() called more than once");
		return;
	}

	// One FunctionDeclaration builtin per exposed function -- what the editor/loader resolve against. Shared
	// by binding-object functions, struct member functions (requiresReceiver=true), and struct constructors
	// (free positional calls named like the struct itself). `selfType` (struct rows only) resolves any
	// DSLType::ThisBinding in `returnType`/a param's type to the struct's own concrete type -- Void everywhere
	// else, where no BindingFunc ever spells ThisBinding.
	auto buildFunction = [&](const char* name, DSLType returnType, const oc::vector<BindingParam>& params,
		const char* emit, bool requiresReceiver, bool isPositionalCall, DSLType selfType = DSLType::Void,
		bool isVariadic = false, bool mutatesContainer = false) -> DSLSymbol*
	{
		const auto resolveSelf = [selfType](DSLType t) { return t == DSLType::ThisBinding ? selfType : t; };
		oc::vector<DSLSymbol*> built;
		for (const BindingParam& param : params)
		{
			DSLSymbol* paramType = addSymbol(builtinsOut, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ resolveSelf(param.type) });
			built.push_back(addSymbol(builtinsOut, ST::VariableDeclaration,
				DSLSymbol::VariableDeclaration{ param.name, paramType, nullptr, param.isRef }));
		}
		DSLSymbol* funcSymbol = addSymbol(builtinsOut, ST::FunctionDeclaration, DSLSymbol::FunctionDeclaration{
			name, oc::move(built), resolveSelf(returnType), requiresReceiver, isPositionalCall, isVariadic,
			mutatesContainer });
		m_emits.emplace_back(funcSymbol, emit);
		return funcSymbol;
	};

	// ARRAY types (T[]) get a synthetic BindingObject each, generated here rather than registered by anyone:
	// they're derived from the element types, so the set is only knowable once every registration is in. Doing
	// it this way means arrays need no special-casing downstream at all -- objectFor/findMember/functionSymbols/
	// emitFor and the whole editor + transpiler treat "int[]" exactly like any other chainable binding.
	// Appended to m_objectDefs BEFORE the build loop below, so that loop picks them up with everything else
	// (and nothing holds a pointer into the vector across the append).
	{
		oc::vector<DSLType> elementTypes{ DSLType::Int, DSLType::Float, DSLType::Bool, DSLType::String, DSLType::Entity };
		for (size_t i = 0; i < m_structDefs.size(); ++i)
			elementTypes.push_back(dslStructType(static_cast<int>(i)));

		for (DSLType elementType : elementTypes)
		{
			// Every array function routes through the same generic ABI (see OcArray in ScriptAPI.h): "$r" is the
			// handle field itself, so push takes its ADDRESS to create the array on first use and write the id
			// back. The generated helpers (see Transpiler's preamble) do the null-check and the cast, which is
			// what keeps a stale handle or an out-of-range index from ever touching memory.
			// The C++ spelling, NOT the DSL one: the template argument has to be the type the ABI actually
			// writes through the void* (Entity* / const char* / the value type), and for a struct it has to be
			// a name that exists in the generated TU ("glm::vec3", not "vec3").
			const oc::string elementName = cppTypeName(elementType);
			m_arrayEmits.push_back(oc::make_unique<oc::string>(
				"ocArrPush<" + elementName + ">(ctx, self, &$r, " + oc::to_string(static_cast<int>(elementType)) + ", $1)"));
			const char* pushEmit = m_arrayEmits.back()->c_str();

			BindingObject arrayObject;
			arrayObject.name = "array";   // never shown: sidebarTopLevel is false, so it's only ever reached by
			arrayObject.type = dslArrayOf(elementType); // dotting into an array-typed value
			arrayObject.sidebarTopLevel = false;
			arrayObject.functions = {
				{ "push",  DSLType::Void, { { "value", elementType } }, pushEmit,
					/*isPositionalCall*/ false, /*isVariadic*/ false, /*mutatesContainer*/ true },
				{ "clear", DSLType::Void, {},                           "ctx->arrayClear($r)",
					/*isPositionalCall*/ false, /*isVariadic*/ false, /*mutatesContainer*/ true },
				// The only way to drop ONE element. Out of range is a no-op, matching every other indexed write
				// -- `ifexist` is still the only way to READ an element, so nothing here needs a success flag.
				{ "removeAt", DSLType::Void, { { "index", DSLType::Int } }, "ctx->arrayRemoveAt($r, $1)",
					/*isPositionalCall*/ false, /*isVariadic*/ false, /*mutatesContainer*/ true },
			};
			arrayObject.members = {
				{ "count", DSLType::Int, "ctx->arrayCount($r)", /*writable*/ false },
			};
			// An array is iterable by construction -- this is what "foreach int v in self.data.scores" walks.
			// The loop resolves the container ONCE (sequenceBeginEmit) and the count/at emits below read that
			// local. A POD element type resolves to the raw storage and indexes it directly, so an element costs
			// a load and the whole loop costs one ABI call; Entity and String elements have no raw element to
			// point at (refcounted handles / engine-owned std::strings) and keep the checked per-element read.
			arrayObject.sequenceElementType = elementType;
			if (elementType == DSLType::Entity || elementType == DSLType::String)
			{
				arrayObject.sequenceBeginEmit = "ctx->arrayResolve($r)";
				arrayObject.sequenceCountEmit = "ctx->arrayViewCount($r)";
				m_arrayEmits.push_back(oc::make_unique<oc::string>("ocArrViewGet<" + elementName + ">(ctx, $r, $i)"));
				arrayObject.sequenceAtEmit = m_arrayEmits.back()->c_str();
				// A `ref` write-back goes through the resolved view too. Without this it would fall back to the
				// handle form, whose receiver is a source local that a begin-emit container no longer declares.
				m_arrayEmits.push_back(oc::make_unique<oc::string>("ocArrViewSet<" + elementName + ">(ctx, $r, $1, $v)"));
				arrayObject.sequenceElementSetEmit = m_arrayEmits.back()->c_str();
			}
			else
			{
				m_arrayEmits.push_back(oc::make_unique<oc::string>(
					"ctx->arraySpan($r, (int)sizeof(" + elementName + "))"));
				arrayObject.sequenceBeginEmit = m_arrayEmits.back()->c_str();
				arrayObject.sequenceCountEmit = "$r.count";
				m_arrayEmits.push_back(oc::make_unique<oc::string>(
					"static_cast<const " + elementName + "*>($r.data)[$i]"));
				arrayObject.sequenceAtEmit = m_arrayEmits.back()->c_str();
				m_arrayEmits.push_back(oc::make_unique<oc::string>(
					"static_cast<" + elementName + "*>($r.data)[$1] = $v"));
				arrayObject.sequenceElementSetEmit = m_arrayEmits.back()->c_str();
				m_arrayEmits.push_back(oc::make_unique<oc::string>(
					"static_cast<" + elementName + "*>($r.data)[$i]"));
				arrayObject.sequenceElementRefEmit = m_arrayEmits.back()->c_str();
			}

			// ...and indexable, but ONLY through `ifexist`: arrayGet's own return value IS the bounds check
			// (1 on success, outValue untouched otherwise -- see ScriptAPI.h), which is exactly the flag the
			// construct branches on. There is deliberately no unchecked read anywhere in the DSL.
			// A POD element resolves to its own ADDRESS and the read is a typed load off it (the pointer IS the
			// success flag); Entity and String elements have no raw element, so they keep the copying accessors.
			arrayObject.lookupKeyType = DSLType::Int;
			arrayObject.lookupValueType = elementType;
			const bool rawElements = elementType != DSLType::Entity && elementType != DSLType::String;
			arrayObject.lookupEmit = rawElements
				? "ocArrLoad(ctx, $r, $1, $v)"
				: "ctx->arrayGet($r, $1, (int)sizeof($v), &$v)";
			if (rawElements)
			{
				m_arrayEmits.push_back(oc::make_unique<oc::string>("static_cast<" + elementName
					+ "*>(ctx->arrayElem($r, $1, (int)sizeof(" + elementName + ")))"));
				arrayObject.lookupRefEmit = m_arrayEmits.back()->c_str();
			}
			// Elements are writable via a `ref` binding; the write-back re-resolves the handle and range-checks
			// at write time, so a body that pushed or cleared drops the write instead of corrupting anything.
			arrayObject.elementWritable = true;
			m_arrayEmits.push_back(oc::make_unique<oc::string>(rawElements
				? "ocArrStore(ctx, $r, $1, $v)"
				: "ocArrSet<" + elementName + ">(ctx, $r, $1, $v)"));
			arrayObject.elementSetEmit = m_arrayEmits.back()->c_str();
			m_objectDefs.push_back(oc::move(arrayObject));
		}
	}

	for (size_t i = 0; i < m_structDefs.size(); ++i)
	{
		const BindingStruct& def = m_structDefs[i];
		const DSLType selfType = dslStructType(static_cast<int>(i));
		BuiltStruct& built = m_builtStructs.emplace_back();
		built.def = &def;
		built.constructorFunc = buildFunction(def.name, selfType, def.constructorParams,
			def.constructorEmit, /*requiresReceiver*/ false, /*isPositionalCall*/ true, selfType);
		for (const BindingFunc& func : def.functions)
			built.functionSymbols.push_back(buildFunction(func.name, func.returnType, func.params, func.emit,
				/*requiresReceiver*/ true, func.isPositionalCall, selfType, func.isVariadic, func.mutatesContainer));
	}

	for (const BindingObject& object : m_objectDefs)
	{
		BuiltObject& built = m_built.emplace_back();
		built.def = &object;
		if (object.name != nullptr && object.sidebarTopLevel)
		{
			DSLSymbol* typeSymbol = addSymbol(sidebarOut, ST::TypeDeclaration, DSLSymbol::TypeDeclaration{ object.type });
			built.decl = addSymbol(sidebarOut, ST::VariableDeclaration, DSLSymbol::VariableDeclaration{ object.name, typeSymbol });
		}
		for (const BindingFunc& func : object.functions)
			built.functionSymbols.push_back(buildFunction(func.name, func.returnType, func.params, func.emit,
				/*requiresReceiver*/ object.name != nullptr, func.isPositionalCall, DSLType::Void, func.isVariadic,
				func.mutatesContainer));
	}
}

oc::span<const BindingObject> ScriptBindings::objects() const
{
	return m_objectDefs;
}

const BindingObject* ScriptBindings::objectFor(DSLType type) const
{
	for (const BuiltObject& built : m_built)
		if (built.def->name != nullptr && built.def->type == type)
			return built.def;
	return nullptr;
}

DSLSymbol* ScriptBindings::objectDecl(const BindingObject& object) const
{
	for (const BuiltObject& built : m_built)
		if (built.def == &object)
			return built.decl;
	return nullptr;
}

const BindingObject* ScriptBindings::objectForDecl(const DSLSymbol* sidebarDecl) const
{
	for (const BuiltObject& built : m_built)
		if (built.decl != nullptr && built.decl == sidebarDecl)
			return built.def;
	return nullptr;
}

oc::span<DSLSymbol* const> ScriptBindings::functionSymbols(const BindingObject& object) const
{
	for (const BuiltObject& built : m_built)
		if (built.def == &object)
			return built.functionSymbols;
	return {};
}

const BindingMember* ScriptBindings::findMember(DSLType receiverType, const oc::string& name) const
{
	const oc::vector<BindingMember>* members = nullptr;
	if (const BindingObject* object = objectFor(receiverType); object != nullptr)
		members = &object->members;
	else if (const BindingStruct* structDef = structFor(receiverType); structDef != nullptr)
		members = &structDef->members;
	if (members == nullptr)
		return nullptr;
	for (const BindingMember& member : *members)
		if (name == member.name)
			return &member;
	return nullptr;
}

const char* ScriptBindings::componentTypeName(DSLType type) const
{
	if (!dslIsComponentType(type))
		return nullptr;
	const int index = dslComponentTypeIndex(type);
	if (index < 0 || index >= static_cast<int>(m_componentTypeNames.size()))
		return nullptr;
	return m_componentTypeNames[index];
}

const char* ScriptBindings::componentFetchEmit(DSLType type) const
{
	if (!dslIsComponentType(type))
		return nullptr;
	const int index = dslComponentTypeIndex(type);
	if (index < 0 || index >= static_cast<int>(m_componentFetchEmits.size()))
		return nullptr;
	return m_componentFetchEmits[index];
}

const char* ScriptBindings::sequenceTypeName(DSLType type) const
{
	if (!dslIsSequenceType(type))
		return nullptr;
	const int index = dslSequenceIndex(type);
	if (index < 0 || index >= static_cast<int>(m_sequenceTypeNames.size()))
		return nullptr;
	return m_sequenceTypeNames[index];
}

int ScriptBindings::componentBit(DSLType type) const
{
	if (!dslIsComponentType(type))
		return -1;
	const int index = dslComponentTypeIndex(type);
	if (index < 0 || index >= static_cast<int>(m_componentBits.size()))
		return -1;
	return m_componentBits[index];
}

oc::span<const BindingStruct> ScriptBindings::structs() const
{
	return m_structDefs;
}

// Reads m_structDefs, NOT m_builtStructs (structFor's source): build() needs this while generating the array
// objects, which happens before the struct loop populates m_builtStructs.
const char* ScriptBindings::cppTypeName(DSLType type) const
{
	// An array is a HANDLE on the script side -- the elements live engine-side (see OcArray in ScriptAPI.h),
	// which is what lets one sit in ScriptData and survive a hot-reload without being copied.
	if (dslIsArrayType(type))
		return "OcArray";
	// Engine-defined structs carry their own C++ spelling in the registry.
	if (dslIsStructType(type))
	{
		const int index = dslStructIndex(type);
		if (index >= 0 && index < static_cast<int>(m_structDefs.size()))
			return m_structDefs[index].cppName;
		return "/* unregistered struct */ void*";
	}
	switch (type)
	{
	case DSLType::Void:    return "void";
	case DSLType::Int:     return "int";
	case DSLType::Float:   return "float";
	case DSLType::Bool:    return "bool";
	case DSLType::String:  return "const char*";
	case DSLType::Entity:  return "Entity*"; // the ABI mirror struct (ScriptAPI.h) -- always by pointer, and nullable
	default: break;
	}
	// Component handles cross the ABI as opaque void* (every component function takes one), so an
	// component variable bound by `ifexist` declares as void* and passes straight through with no cast.
	if (dslIsComponentType(type))
		return "void*";
	return "/* unsupported DSLType */ void*"; // the other engine-object kinds never declare values
}

const BindingStruct* ScriptBindings::structFor(DSLType type) const
{
	if (!dslIsStructType(type))
		return nullptr;
	const int index = dslStructIndex(type);
	if (index < 0 || index >= static_cast<int>(m_builtStructs.size()))
		return nullptr;
	return m_builtStructs[index].def;
}

oc::span<DSLSymbol* const> ScriptBindings::structFunctionSymbols(DSLType type) const
{
	if (!dslIsStructType(type))
		return {};
	const int index = dslStructIndex(type);
	if (index < 0 || index >= static_cast<int>(m_builtStructs.size()))
		return {};
	return m_builtStructs[index].functionSymbols;
}

const char* ScriptBindings::emitFor(const DSLSymbol* funcDecl) const
{
	for (const auto& [symbol, emit] : m_emits)
		if (symbol == funcDecl)
			return emit;
	return nullptr;
}

oc::span<const EntryPointDef> ScriptBindings::entryPoints() const
{
	return m_entryPointDefs;
}

const EntryPointDef* ScriptBindings::entryPointFor(const oc::string& name) const
{
	for (const EntryPointDef& def : m_entryPointDefs)
		if (name == def.name)
			return &def;
	return nullptr;
}
