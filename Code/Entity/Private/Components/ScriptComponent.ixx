export module Entity:ScriptComponent;

import :Entity;
import Core;
import Core.Transform;
import Script;

export struct ScriptComponent;
// Frees every engine-owned script array the component created (see ScriptComponent::ownedArrays). Defined in
// ScriptContext.cpp next to the registry itself; called when the component dies and whenever its ScriptData
// block is discarded, which is the moment the handles inside it stop being reachable.
void releaseScriptArrays(ScriptComponent& owner);

// SEH-guarded entry-point invokers (defined in ScriptComponent.cpp): run the script function and catch
// hardware faults (integer divide by zero, a stale pointer) so a broken script disables itself instead of
// killing the engine. On a fault they mark the module `faulted` -- which requirementsMet folds in, so every
// entry point everywhere skips it until a successful recompile clears the flag -- and return false.
bool invokeScriptOnSpawn(const ScriptModule* module, Entity& entity, void* scriptData);
bool invokeScriptOnDestroy(const ScriptModule* module, Entity& entity, void* scriptData);
bool invokeScriptUpdate(const ScriptModule* module, Entity& entity, float deltaSeconds, void* scriptData);
bool invokeScriptOnEvent(const ScriptModule* module, Entity& entity, int eventIdx, void* scriptData);
bool invokeScriptOnPhysicsEvent(const ScriptModule* module, Entity& entity, Entity* other, int begin, int sensor, int64 contactId, void* scriptData);

// References a visual script (.scr) the entity runs each frame. The Script library compiles the file on
// demand and ticks it with this entity as `self`, so the script's Get/Set Entity nodes read and write
// this entity's fields. Holds no execution state itself (Entity must not depend on the Script library).
export struct ScriptComponent
{
    static constexpr EComponentID getId() { return EComponentID_Script; }

    const ScriptModule* scriptModule = nullptr;
    oc::unique_ptr<uint8[]> scriptData;
    uint32 scriptDataSize = 0;
    uint32 scriptDataLayoutId = 0; // what the block was allocated against -- a reload with a different layout
                                    // can't reuse it (see ScriptDataLayoutIdFn in ScriptAPI.h)
    bool enabled = true;

    // Every engine-owned array (the DSL's `T[]`) this script created, by handle -- the ScriptData block only
    // holds the ids, so this is what actually owns the storage and frees it when the component dies or its
    // data block is discarded. See the array registry in ScriptContext.cpp.
    oc::vector<uint32> ownedArrays;

    ~ScriptComponent();

    // Both are re-checked at EVERY entry point rather than latched at spawn: a hot-reload mutates the bound
    // ScriptModule in place and never re-runs spawn, so anything latched there outlives the edit that fixes it.
    bool requirementsMet(const Entity& entity) const;
    // Reallocates ScriptData on a layout change (refilling the //@@require slots, clearing onSpawnRan);
    // returns whether it did. The Live variant also re-registers the event listener, whose stored ScriptData
    // pointer the reallocation would dangle -- use it from anything running after spawn.
    bool syncScriptData(Entity& entity);
    void syncScriptDataLive(Entity& entity);

    // This script has BEGUN on this entity: set the first time it satisfies the //@@require set, at spawn or
    // later (update() constructs late). OnDestroy pairs with THIS, never with the live requirement check.
    bool onSpawnRan = false;

    // AUTHORED initial values for the script's exposed ScriptData fields (the DSL's "//@@data private int num"),
    // by field name, as TEXT -- the .pre's own form, and the only one available before the script has compiled
    // and said what type each field is. Applied at spawn and again after any reallocation (applyInitialValues).
    // Editing a field LIVE in the Properties panel does not come through here: that writes the block directly
    // and is deliberately never serialized.
    struct InitialFieldValue
    {
        oc::string name;
        oc::string value;
    };

    struct SpawnInfo
    {
        oc::string scriptPath;
        bool enabled = true;
        oc::vector<InitialFieldValue> initialValues;
    };

    // A COPY of the SpawnInfo's initial values, so they survive template retirement and -- the reason this
    // exists -- a hot-reload: syncScriptData reallocates and ZEROES the block whenever the layout changes, which
    // would otherwise silently reset every authored value to zero.
    oc::vector<InitialFieldValue> initialValues;

    // Writes the authored values into the (freshly allocated) block, matching names against the module's
    // exposed-field table and parsing each text by that field's type. Unknown names and unparseable text are
    // skipped -- a field renamed in the script stops being applied rather than landing on a neighbour.
    void applyInitialValues();

    void spawn(Entity&, const SpawnInfo& info, const Transform&);
    void destroy(Entity&, const SpawnInfo&);

    // Compiles (on demand) and runs the referenced script with `entity` as its `self`. Defined in
    // ScriptRuntime.cpp, which owns the ScriptContext + thunks.
    void update(Entity& entity, float deltaSeconds);

    // Fires a named On Event entry (e.g. an animation notify). No-ops if the script declares no such entry.
    void fireEvent(Entity& entity, const oc::string& eventName);
    void fireEvent(Entity& entity, uint32 eventKey); // Globals::scriptEvents.getEventKeyForName

    // Fires the On Physics Event entry point (see dispatchPhysicsContactEvents). No-ops if the script
    // declares no such entry. contactId is only valid for the frame it was collected (see PhysicsWorld::getContactPoint).
    void firePhysicsEvent(Entity& entity, Entity* other, bool begin, bool sensor, int64 contactId);
};

export const ScriptComponent::SpawnInfo* getScriptSpawnInfo(const Entity* entity);
