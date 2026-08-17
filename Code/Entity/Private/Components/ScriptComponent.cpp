module;

// 1 (default): script entry points run under the SEH guards below -- a hardware fault in script code disables
// the script instead of crashing the engine. 0: the invokers call the entry points raw (a script fault then
// crashes like any other engine fault -- e.g. to get a clean crash dump at the faulting instruction).
#define OC_SCRIPT_FAULT_CONTAINMENT 1

#if OC_SCRIPT_FAULT_CONTAINMENT
#include <excpt.h>
#endif
#include "ScriptAPI.h"

module Entity;

import Core;
import Core.Log;
import Core.Transform;
import :Entity;
import :ScriptContext;
import :ScriptEventManager;
import Script;

// ---- SEH fault containment -------------------------------------------------------------------------------
// A script is generated C++, but it runs author logic: an integer divide by zero, INT_MIN / -1, or a stale
// pointer held across frames raises a hardware exception that would kill the whole engine. Every entry-point
// call goes through the invokers below instead: the fault is caught with STRUCTURED exception handling (no
// C++ exceptions involved -- works with /EH off, and on x64 the __try is table-based, zero cost until a fault
// actually fires), the module is marked `faulted`, and requirementsMet then skips it at every entry point --
// including OnDestroy, since the script's own state may be half-written at the fault -- until a successful
// recompile clears the flag. The __try helpers must stay free of objects with destructors (C2712), so they
// only return the exception code and the plain C++ wrappers below them do the logging.
namespace
{
#if OC_SCRIPT_FAULT_CONTAINMENT
    int scriptFaultFilter(unsigned long code)
    {
        switch (code)
        {
        case 0xC0000005: // access violation (a stale Entity* / component pointer)
        case 0xC0000006: // in-page error
        case 0xC000001D: // illegal instruction
        case 0xC000008C: // array bounds exceeded
        case 0xC000008D: // float denormal operand   -- the six float faults only exist if someone unmasks
        case 0xC000008E: // float divide by zero        the FP control word; masked (the default, and what
        case 0xC000008F: // float inexact result        the engine wants) float math produces inf/NaN and
        case 0xC0000090: // float invalid operation     never raises
        case 0xC0000091: // float overflow
        case 0xC0000092: // float stack check
        case 0xC0000093: // float underflow
        case 0xC0000094: // integer divide by zero -- the common one
        case 0xC0000095: // integer overflow (INT_MIN / -1)
        case 0xC0000096: // privileged instruction
            return EXCEPTION_EXECUTE_HANDLER;
        }
        // Everything else keeps its normal path: breakpoints/single-step stay with the debugger, C++
        // exceptions (0xE06D7363) stay fatal as they are today, and stack overflow stays fatal because the
        // guard page is spent -- continuing after it would fault unrecoverably anyway.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // OnSpawn and OnDestroy share one shape, so one invoker serves both.
    unsigned long sehInvoke(ScriptOnSpawnFn fn, Entity* entity, void* data)
    {
        __try { fn(&Globals::scriptContext, entity, data); }
        __except (scriptFaultFilter(_exception_code())) { return _exception_code(); }
        return 0;
    }
    unsigned long sehInvoke(ScriptUpdateFn fn, Entity* entity, float deltaSeconds, void* data)
    {
        __try { fn(&Globals::scriptContext, entity, deltaSeconds, data); }
        __except (scriptFaultFilter(_exception_code())) { return _exception_code(); }
        return 0;
    }
    unsigned long sehInvoke(ScriptOnEventFn fn, Entity* entity, int eventIdx, void* data)
    {
        __try { fn(&Globals::scriptContext, entity, eventIdx, data); }
        __except (scriptFaultFilter(_exception_code())) { return _exception_code(); }
        return 0;
    }
    unsigned long sehInvoke(ScriptOnPhysicsEventFn fn, Entity* entity, Entity* other, int begin, int sensor, long long contactId, void* data)
    {
        __try { fn(&Globals::scriptContext, entity, other, begin, sensor, contactId, data); }
        __except (scriptFaultFilter(_exception_code())) { return _exception_code(); }
        return 0;
    }
#else
    // Containment off: raw calls, same signatures so the wrappers below don't change. Never returns nonzero,
    // so reportScriptFault and the `faulted` gate are dead paths (the flag then simply never sets).
    unsigned long sehInvoke(ScriptOnSpawnFn fn, Entity* entity, void* data)                    { fn(&Globals::scriptContext, entity, data); return 0; }
    unsigned long sehInvoke(ScriptUpdateFn fn, Entity* entity, float deltaSeconds, void* data) { fn(&Globals::scriptContext, entity, deltaSeconds, data); return 0; }
    unsigned long sehInvoke(ScriptOnEventFn fn, Entity* entity, int eventIdx, void* data)      { fn(&Globals::scriptContext, entity, eventIdx, data); return 0; }
    unsigned long sehInvoke(ScriptOnPhysicsEventFn fn, Entity* entity, Entity* other, int begin, int sensor, long long contactId, void* data)
    {
        fn(&Globals::scriptContext, entity, other, begin, sensor, contactId, data); return 0;
    }
#endif

    void reportScriptFault(const ScriptModule* module, Entity& entity, const char* entry, unsigned long code)
    {
        module->faulted = true;
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08lX", code);
        const char* name = entity.getName();
        Log::error("Script '" + module->scriptPath + "' faulted (" + hex + ") in " + entry + " on entity '"
            + (name ? name : "<unnamed>") + "' -- disabled until it is recompiled (F6 / Compile & Run)");
    }
}

bool invokeScriptOnSpawn(const ScriptModule* module, Entity& entity, void* scriptData)
{
    const unsigned long code = sehInvoke(reinterpret_cast<ScriptOnSpawnFn>(module->onSpawn), &entity, scriptData);
    if (code == 0)
        return true;
    reportScriptFault(module, entity, "OnSpawn", code);
    return false;
}

bool invokeScriptOnDestroy(const ScriptModule* module, Entity& entity, void* scriptData)
{
    const unsigned long code = sehInvoke(reinterpret_cast<ScriptOnSpawnFn>(module->onDestroy), &entity, scriptData);
    if (code == 0)
        return true;
    reportScriptFault(module, entity, "OnDestroy", code);
    return false;
}

bool invokeScriptUpdate(const ScriptModule* module, Entity& entity, float deltaSeconds, void* scriptData)
{
    const unsigned long code = sehInvoke(reinterpret_cast<ScriptUpdateFn>(module->update), &entity, deltaSeconds, scriptData);
    if (code == 0)
        return true;
    reportScriptFault(module, entity, "Update", code);
    return false;
}

bool invokeScriptOnEvent(const ScriptModule* module, Entity& entity, int eventIdx, void* scriptData)
{
    const unsigned long code = sehInvoke(reinterpret_cast<ScriptOnEventFn>(module->onEvent), &entity, eventIdx, scriptData);
    if (code == 0)
        return true;
    reportScriptFault(module, entity, "OnEvent", code);
    return false;
}

bool invokeScriptOnPhysicsEvent(const ScriptModule* module, Entity& entity, Entity* other, int begin, int sensor, int64 contactId, void* scriptData)
{
    const unsigned long code = sehInvoke(reinterpret_cast<ScriptOnPhysicsEventFn>(module->onPhysicsEvent), &entity, other, begin, sensor, contactId, scriptData);
    if (code == 0)
        return true;
    reportScriptFault(module, entity, "OnPhysicsEvent", code);
    return false;
}

void ScriptComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
	enabled = info.enabled;
	initialValues = info.initialValues; // kept for re-application after any later reallocation

	if (info.scriptPath.empty())
		return;
    {
        const ScriptModule* loaded = Globals::scriptHost.getOrLoad(info.scriptPath);
        if (!loaded)
            return;

        // Logged once here; the refusal itself is requirementsMet(), re-checked at every entry point. Latching
        // it would outlive the edit that fixes it -- a hot-reload never re-runs spawn.
        if (const uint32 missing = loaded->requiredComponents & ~uint32(entity.typeBits))
        {
            oc::string names;
            for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
                if (missing & (1u << i))
                    names += (names.empty() ? "" : ", ") + oc::string(componentTypeName(EComponentID(i)));
            Log::error("Script '" + info.scriptPath + "' requires " + names + " -- entity '"
                + oc::string(entity.getName()) + "' has none of that; it will not run until the entity gains"
                " those components or the script stops requiring them");
        }
        scriptModule = loaded;
    }

    syncScriptData(entity);

    if (scriptModule->onEvent)
        Globals::scriptEvents.registerListener(scriptModule, &entity, scriptData.get());

	// OnSpawn runs even for a frozen entity: it is the script's constructor, not a tick. Unmet //@@require
	// suppresses it -- a constructor is exactly where a script reaches for its components -- and update()
	// then runs it late, the first frame the entity does qualify.
	if (requirementsMet(entity))
	{
		onSpawnRan = true; // set even without an OnSpawn entry: it marks "this script has begun on this entity"
		if (scriptModule->onSpawn)
			invokeScriptOnSpawn(scriptModule, entity, scriptData.get());
	}
}

// The block is REUSED while size and layout both match -- that is what makes a reload free for arrays: the
// handles survive, so the engine-side contents do. A mismatch releases the old arrays first (once the block
// goes nothing can reach those handles again) and clears onSpawnRan, so the script reconstructs into the
// fresh one. Runs at spawn AND live, since a hot-reload mutates the ScriptModule in place, never re-spawning.
bool ScriptComponent::syncScriptData(Entity& entity)
{
    if (!scriptModule || (scriptModule->dataSize == scriptDataSize && scriptModule->dataLayoutId == scriptDataLayoutId))
        return false;

    releaseScriptArrays(*this);
    scriptDataSize = scriptModule->dataSize;
    scriptDataLayoutId = scriptModule->dataLayoutId;
    scriptData = scriptDataSize ? oc::make_unique<uint8[]>(scriptDataSize) : nullptr;
    onSpawnRan = false;

    // Required-component pointers (//@@require) occupy the FRONT of ScriptData, one 8-byte slot per set bit in
    // ascending EComponentID order, so "scriptData-><name>" reads an already-resolved pointer. EVERY id the
    // mask can carry needs a branch: a missing one shifts every later component into the wrong slot.
    if (scriptData && scriptModule->requiredComponents)
    {
        void** slot = reinterpret_cast<void**>(scriptData.get());
        if (scriptModule->requiredComponents & (1u << EComponentID_Scene))    *slot++ = getComponent<SceneComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Render))   *slot++ = getComponent<RenderComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Animator)) *slot++ = getComponent<AnimatorComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Physics))  *slot++ = getComponent<PhysicsComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Audio))    *slot++ = getComponent<AudioComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Particle)) *slot++ = getComponent<ParticleComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Force))    *slot++ = getComponent<ForceComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Light))    *slot++ = getComponent<LightComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_Network))  *slot++ = getComponent<NetworkComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_GameUnit)) *slot++ = getComponent<GameUnitComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_GameStructure)) *slot++ = getComponent<GameStructureComponent>(&entity);
        if (scriptModule->requiredComponents & (1u << EComponentID_GameProjectile)) *slot++ = getComponent<GameProjectileComponent>(&entity);
    }

    // The block was just zeroed, so the authored values have to go back in -- otherwise a hot-reload that
    // touched //@@data would silently reset every one of them. AFTER the require slots, which occupy the front
    // of the block and are never fields.
    applyInitialValues();
    return true;
}

// Text -> bytes, by each field's own type. Deliberately tolerant: a name that no longer exists in the script, or
// text that doesn't parse, is skipped rather than guessed at -- an authored value for a since-renamed field
// should quietly stop applying, never land on whatever field now occupies that offset.
void ScriptComponent::applyInitialValues()
{
    if (initialValues.empty() || scriptData == nullptr || scriptModule == nullptr
        || scriptModule->dataFields == nullptr || scriptModule->numDataFields <= 0)
        return;
    // ScriptModule carries the table as void* (it doesn't depend on the script ABI) -- typed once, here.
    const OcScriptField* fields = static_cast<const OcScriptField*>(scriptModule->dataFields);

    // Comma-separated floats for the vector/quat kinds, so an authored value reads the way every other vector
    // in the .pre format does ("1, 2, 3"). A short or unparseable list leaves the field alone.
    const auto parseFloats = [](const oc::string& text, float* out, int count)
    {
        int parsed = 0;
        const char* p = text.c_str();
        while (parsed < count && *p != '\0')
        {
            char* end = nullptr;
            const float value = std::strtof(p, &end);
            if (end == p)
                break;
            out[parsed++] = value;
            p = end;
            while (*p == ',' || *p == ' ' || *p == '\t')
                ++p;
        }
        return parsed == count;
    };

    for (const InitialFieldValue& authored : initialValues)
    {
        const OcScriptField* field = nullptr;
        for (int i = 0; i < scriptModule->numDataFields && field == nullptr; ++i)
            if (authored.name == fields[i].name)
                field = &fields[i];
        if (field == nullptr || field->offset < 0 || uint32(field->offset) >= scriptDataSize)
            continue;
        uint8* slot = scriptData.get() + field->offset;

        switch (field->type)
        {
        case OC_FIELD_INT:   *reinterpret_cast<int*>(slot) = std::atoi(authored.value.c_str()); break;
        case OC_FIELD_FLOAT: *reinterpret_cast<float*>(slot) = std::strtof(authored.value.c_str(), nullptr); break;
        case OC_FIELD_BOOL:  *reinterpret_cast<bool*>(slot) = (authored.value == "true" || authored.value == "1"); break;
        // The block holds a const char* into ENGINE-interned storage, never into this component's own string --
        // the block outlives any particular InitialFieldValue (and a reload replaces the whole vector).
        case OC_FIELD_STRING:
            *reinterpret_cast<const char**>(slot) = Globals::scriptContext.internString(authored.value.c_str());
            break;
        case OC_FIELD_VEC2:  parseFloats(authored.value, reinterpret_cast<float*>(slot), 2); break;
        case OC_FIELD_VEC3:  parseFloats(authored.value, reinterpret_cast<float*>(slot), 3); break;
        case OC_FIELD_VEC4:
        case OC_FIELD_QUAT:  parseFloats(authored.value, reinterpret_cast<float*>(slot), 4); break;
        default: break;
        }
    }
}

// The live-path wrapper: the event manager holds the ScriptData pointer it was registered with, so a
// reallocation has to re-register or every fired event writes into freed memory.
void ScriptComponent::syncScriptDataLive(Entity& entity)
{
    if (syncScriptData(entity) && scriptModule->onEvent)
    {
        Globals::scriptEvents.unregisterListener(scriptModule, &entity);
        Globals::scriptEvents.registerListener(scriptModule, &entity, scriptData.get());
    }
}

bool ScriptComponent::requirementsMet(const Entity& entity) const
{
    // Also the fault gate: a module that hardware-faulted (see the SEH invokers above) stops qualifying at
    // every entry point at once -- OnDestroy included, since its data may be half-written -- until a
    // successful recompile clears the flag.
    return scriptModule == nullptr
        || (!scriptModule->faulted && (scriptModule->requiredComponents & ~uint32(entity.typeBits)) == 0);
}

void ScriptComponent::update(Entity& entity, float deltaSeconds)
{
    if (!enabled || !scriptModule)
        return;
    syncScriptDataLive(entity); // the module may have been recompiled under us since spawn
    if (!requirementsMet(entity))
        return;

    // Late construction: an entity that didn't satisfy the //@@require set at spawn -- or whose script has
    // since dropped the requirement it was missing -- runs OnSpawn the first frame it qualifies. Without this
    // a script fixed mid-session stays half-initialized on everything already spawned until it is respawned.
    if (!onSpawnRan)
    {
        onSpawnRan = true;
        if (scriptModule->onSpawn && !invokeScriptOnSpawn(scriptModule, entity, scriptData.get()))
            return; // faulted in its constructor -- don't run the first Update on half-built state
    }

    if (!scriptModule->update || entity.isFrozen())
        return;
    invokeScriptUpdate(scriptModule, entity, deltaSeconds, scriptData.get());
}

void ScriptComponent::fireEvent(Entity& entity, const oc::string& eventName)
{
    if (!enabled || !scriptModule)
        return;
    syncScriptDataLive(entity); // the module may have been recompiled under us since spawn
    if (!scriptModule->onEvent || entity.isFrozen() || !requirementsMet(entity))
        return;
    auto it = scriptModule->eventKeyToIndex.find(Globals::scriptEvents.findEventKey(eventName));
    if (it != scriptModule->eventKeyToIndex.end())
    {
        invokeScriptOnEvent(scriptModule, entity, it->second, scriptData.get());
    }
}

void ScriptComponent::fireEvent(Entity& entity, uint32 eventKey)
{
    if (!enabled || !scriptModule)
        return;
    syncScriptDataLive(entity); // the module may have been recompiled under us since spawn
    if (!scriptModule->onEvent || entity.isFrozen() || !requirementsMet(entity))
        return;
    auto it = scriptModule->eventKeyToIndex.find(eventKey);
    if (it != scriptModule->eventKeyToIndex.end())
    {
        invokeScriptOnEvent(scriptModule, entity, it->second, scriptData.get());
    }
}

void ScriptComponent::firePhysicsEvent(Entity& entity, Entity* other, bool begin, bool sensor, int64 contactId)
{
    if (!enabled || !scriptModule)
        return;
    syncScriptDataLive(entity); // the module may have been recompiled under us since spawn
    if (!scriptModule->onPhysicsEvent || entity.isFrozen() || !requirementsMet(entity))
        return;
    invokeScriptOnPhysicsEvent(scriptModule, entity, other, begin ? 1 : 0, sensor ? 1 : 0, contactId, scriptData.get());
}

void ScriptComponent::destroy(Entity& entity, const SpawnInfo& info)
{
    if (!scriptModule)
        return;

    // Paired with OnSpawn HAVING RUN, or a script edited mid-life tears down what it never built. The
    // requirement is checked on top: a skipped teardown beats reaching for a component it was refused
    // (the script's arrays are released below either way).
    if (scriptModule->onDestroy && onSpawnRan && requirementsMet(entity))
        invokeScriptOnDestroy(scriptModule, entity, scriptData.get());

    if (scriptModule->onEvent)
        Globals::scriptEvents.unregisterListener(scriptModule, &entity);
}

// Arrays are released here rather than in destroy(): destroy() is the SCRIPT's teardown hook (it runs
// OnDestroy, which may still read them) and isn't reached on every path that frees a component.
ScriptComponent::~ScriptComponent()
{
    releaseScriptArrays(*this);
}

const ScriptComponent::SpawnInfo* getScriptSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<ScriptComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Script); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const ScriptComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}
