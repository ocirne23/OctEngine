#pragma once

// ABI shared between the engine host and runtime-compiled visual-script DLLs.
// Only ABI compatible types; glm vectors, PODs, floats and raw pointers cross this boundary no STL, no engine types. That keeps

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstdarg>
#include <cstddef> // offsetof -- the generated ScriptDataFields table locates each exposed field with it

#include "ScriptCtxMacros.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SCRIPT_BUILD) || defined(SCRIPT_STATIC_BUILD)
struct Entity
{
    glm::vec3 pos;    // local position
    float     scale;
    glm::quat rot;
    Entity*   parent; // null for a root entity
};
#else
class Entity;
#endif
struct Camera; // engine render camera; only the host-only update() below touches it (scripts never see it)

// Opaque component handles: forward-declared only, never dereferenced by generated script code (always passed
// through to the matching ctx->component*() thunks, which take void* -- an object pointer converts to void*
// implicitly, so no cast is needed at the call site). Lets ScriptData (see ScriptDataInitFn) cache a typed
// pointer per required component instead of a bare void*, with no layout to keep in sync on either side.
class PhysicsComponent;
class AudioComponent;
class ForceComponent;
class SceneComponent;
class LightComponent;
class AnimatorComponent;

// Handle to an engine-owned dynamic array (the DSL's `T[]`). A plain POD id so it can live in a script's
// persistent ScriptData block: the ELEMENTS live engine-side, keyed by this id, which means a hot-reload that
// keeps the block also keeps the contents with nothing to copy, and the script never owns heap memory.
// 0 = no array yet -- a zeroed ScriptData block is therefore a set of empty arrays, and the first push creates
// one lazily. The id is generation-tagged engine-side, so a STALE or garbage value (a reinterpreted field after
// a layout change) fails lookup and every operation degrades to a no-op / default rather than touching memory.
typedef unsigned int OcArray;

// A POD array's element storage, resolved ONCE for a foreach so the loop indexes memory directly instead of
// calling per element. Only POD element kinds get one -- Entity elements are refcounted handles and String
// elements are engine-owned std::strings, neither of which has a raw element to point at.
// `data` is null and `count` 0 for a stale/empty/non-POD array or an elemSize mismatch, so a loop bounded by
// `count` never dereferences null. VALID ONLY WHILE THE ARRAY DOES NOT GROW: the DSL refuses push/clear on a
// container an enclosing foreach is reading, which is what the generated code relies on.
struct OcArraySpan
{
    void* data;
    int   count;
};

// One light of a LightComponent, crossed BY VALUE as a whole (the DSL's `Light` struct): lightGetAt copies
// one out, lightSetAt applies one back (with the host clamping/normalizing, so a script can't store a shape
// the renderer degenerates on). The light's TYPE is not here -- it decides which shape fields mean anything
// and stays authoring-only; writing a field the type ignores is harmlessly stored. Angles in DEGREES.
struct OcLight
{
    glm::vec3 color;
    float     intensity;
    float     range;        // world units, NOT scaled by the entity
    glm::vec3 offset;       // entity-space position
    glm::vec3 direction;    // entity-space aim axis (Spot/Area/Tube)
    float     coneAngle;    // Spot: cone HALF-angle
    float     edgeSoftness; // Spot
    float     width;        // Area width / Tube radius
    float     height;       // Area
    float     length;       // Tube
    float     rotation;     // Area: extra roll about the aim axis
    bool      enabled;
};

// One raycast result, returned BY VALUE (a POD of ABI-legal members, like every other type crossing here).
// `hit` is what the DSL branches on: the whole struct is exposed as a `RayHit?`, so a miss takes the else
// branch of an `ifexist` and the other fields are only reachable once a hit is proven.
struct OcRayHit
{
    int       hit;      // 0 = nothing within range; every other field is then meaningless
    glm::vec3 point;    // world-space contact position
    glm::vec3 normal;   // world-space surface normal at `point`
    float     distance; // along the ray from its origin
};

// ---- exposed ScriptData fields ----
// A script's ScriptData block is opaque to the engine: it knows the block's SIZE and a layout HASH, nothing
// about what is in it. This table is how a script describes the fields it chose to expose (the DSL's
// "//@@data private int num"), so the editor can show and set them by name: one row per non-Hidden field,
// carrying the offset to reach it and the type to interpret it as.
//
// Hidden fields -- the default -- are absent entirely. Arrays never appear: the field would be a OcArray handle
// whose storage lives engine-side, with nothing for an inspector to show.
enum OcScriptFieldType
{
    OC_FIELD_INT = 0,
    OC_FIELD_FLOAT,
    OC_FIELD_BOOL,   // stored as a C++ bool in the block
    OC_FIELD_STRING, // stored as a const char* into ENGINE-interned text (see internString) -- never freed
    OC_FIELD_VEC2,
    OC_FIELD_VEC3,
    OC_FIELD_VEC4,
    OC_FIELD_QUAT,
};

enum OcScriptFieldVisibility
{
    OC_FIELD_PRIVATE = 1, // editor-visible: the Properties panel edits it live, the Entity Editor authors it
    OC_FIELD_PUBLIC  = 2, // ...and, in future, readable from another entity's script
};

struct OcScriptField
{
    const char* name;
    int         type;       // OcScriptFieldType
    int         offset;     // byte offset into the ScriptData block
    int         visibility; // OcScriptFieldVisibility
};

#pragma warning(push)
#pragma warning(disable: 4190) // has C-linkage specified, but returns 'glm::vec<3,float,glm::packed_highp>' which is incompatible with..

#define SCRIPT_CTX_FUNCS(X) \
    X(int,         isKeyDown,              (const char*, keyName)) /* e.g. "Space", "A", "Left Shift" */ \
    X(void,        spawnPointLight,        (glm::vec3, position), (float, range), (glm::vec3, color), (float, intensity)) \
    X(void,        setSun,                 (glm::vec3, direction), (glm::vec3, color), (float, intensity)) \
    X(Entity*,     spawnEntity,            (const char*, assetPath), (glm::vec3, position)) \
    X(void,        destroyEntity,          (Entity*, entity)) /* queues the entity (e.g. self) for removal */ \
    /* ---- entity reads ---- */ \
    X(const char*, entityGetName,          (Entity*, entity)) \
    X(int,         entityGetEnabled,       (Entity*, entity)) \
    X(int,         entityGetChildCount,    (Entity*, entity)) /* used by the Get Entity node's Children output; child hierarchy nodes go through the SceneComponent handle (below) */ \
    X(float,       entityGetBoundsRadius,  (Entity*, entity)) /* world-space render bounds radius (0 if no mesh) */ \
    /* ---- entity writes ---- */ \
    X(void,        entitySetEnabled,       (Entity*, entity), (int, enabled)) \
    X(void,        entitySetAnimFloat,     (Entity*, entity), (const char*, param), (float, value)) \
    X(void,        entitySetAnimBool,      (Entity*, entity), (const char*, param), (int, value)) \
    X(void,        entitySetAnimTrigger,   (Entity*, entity), (const char*, param)) \
    /* ---- physics (global) ---- */ \
    X(void,        physicsSetGravity,      (glm::vec3, gravity)) \
    X(int,         physicsRayCast,         (glm::vec3, origin), (glm::vec3, translation), (glm::vec3*, outPoint), (glm::vec3*, outNormal), (float*, outFraction)) /* ray = origin + translation, 1 on hit */ \
    X(float,       physicsRayCastDistance, (glm::vec3, origin), (glm::vec3, dir), (float, maxDist)) /* dir need not be pre-normalized; distance to the closest hit within maxDist, or maxDist itself on a miss (never a sentinel outside the query range) */ \
    /* ---- events ---- */ \
    X(void,        sendEvent,              (const char*, eventName)) \
    X(void,        sendEventToEntity,      (Entity*, entity), (const char*, eventName)) \
    /* ---- physics contact queries ---- */ \
    X(int,         physicsContactGetPoint, (long long, contactId), (glm::vec3*, outPoint), (glm::vec3*, outNormal)) \
    /* ---- spatial queries ---- */ \
    X(int,         spatialQueryRadius,     (glm::vec3, position), (float, radius), (Entity**, outEntities), (int, maxOut)) \
    X(Entity*,     spatialGetNearestEntity,(glm::vec3, position), (float, maxRadius), (Entity*, exclude)) \
    /* ---- force field ----  */ \
    X(void*,       entityGetForceComponent,(Entity*, entity)) \
    X(float,       forceGetOutput,         (void*, forceComponent)) \
    X(float,       forceGetReach,          (void*, forceComponent)) \
    X(float,       forceGetFocus,          (void*, forceComponent)) \
    X(float,       forceGetDistribution,   (void*, forceComponent)) \
    X(float,       forceGetWidth,          (void*, forceComponent)) \
    X(int,         forceGetTeam,           (void*, forceComponent)) \
    X(glm::vec3,   forceGetAppliedForce,   (void*, forceComponent)) \
    X(float,       forceGetPressure,       (void*, forceComponent)) \
    X(glm::vec3,   forceGetLocalDirection, (void*, forceComponent)) \
    X(glm::vec3,   forceGetLocalOffset,    (void*, forceComponent)) \
    X(int,         forceGetCentered,       (void*, forceComponent)) \
    X(void,        forceSetOutput,         (void*, forceComponent), (float, output)) \
    X(void,        forceSetReach,          (void*, forceComponent), (float, reach)) \
    X(void,        forceSetFocus,          (void*, forceComponent), (float, focus)) \
    X(void,        forceSetDistribution,   (void*, forceComponent), (float, distribution)) \
    X(void,        forceSetWidth,          (void*, forceComponent), (float, width)) \
    X(void,        forceSetTeam,           (void*, forceComponent), (int, team)) \
    X(void,        forceSetLocalDirection, (void*, forceComponent), (glm::vec3, direction)) \
    X(void,        forceSetLocalOffset,    (void*, forceComponent), (glm::vec3, offset)) \
    X(void,        forceSetCentered,       (void*, forceComponent), (int, centered)) \
    /* ---- scene component ---- */ \
    X(void*,       entityGetSceneComponent,(Entity*, entity)) \
    X(int,         sceneGetChildCount,     (void*, sceneComponent)) \
    X(Entity*,     sceneFindChild,         (void*, sceneComponent), (const char*, name)) /* direct child by name, null if none match */ \
    X(Entity*,     sceneGetChildAt,        (void*, sceneComponent), (int, index)) /* direct child by index, null if out of range */ \
    X(void,        sceneAddChild,          (void*, parentSceneComponent), (Entity*, child)) /* reparents child under the component's owner (no-op on a cycle / wrong current parent) */ \
    X(void,        sceneRemoveChild,       (void*, parentSceneComponent), (Entity*, child)) /* detaches child, becoming a root entity (no-op if not the component owner's child) */ \
    X(void,        sceneRemoveChildAt,     (void*, parentSceneComponent), (int, index)) /* same as sceneRemoveChild, by child index */ \
    /* ---- physics component ---- */ \
    X(void*,       entityGetPhysicsComponent,(Entity*, entity)) \
    X(int,         physicsIsAwake,         (void*, physicsComponent)) \
    X(glm::vec3,   physicsGetVelocity,     (void*, physicsComponent)) \
    X(void,        physicsSetVelocity,     (void*, physicsComponent), (glm::vec3, velocity)) \
    X(void,        physicsApplyImpulse,    (void*, physicsComponent), (glm::vec3, impulse)) /* world space, at the center of mass, wakes the body */ \
    X(void,        physicsTeleport,        (void*, physicsComponent), (glm::vec3, position), (glm::vec3, eulerDeg)) /* queued; applied before the next physics step */ \
    /* ---- audio component ---- */ \
    X(void*,       entityGetAudioComponent,(Entity*, entity)) \
    X(void,        audioTrigger,           (void*, audioComponent), (Entity*, entity), (const char*, alias), (int, overrideMask), (glm::vec3, position), (float, volume), (float, pitch)) \
    X(void,        audioStop,              (void*, audioComponent), (const char*, alias)) \
    /* Every DSL string LITERAL is emitted as a call to this rather than as a bare C++ literal: a literal lives
       in the script DLL's .rdata, which is unmapped on hot-reload, so anything that outlives one call (a
       persistent ScriptData field, an array element) would be left pointing at freed pages. The engine interns
       the text permanently and hands back a pointer that outlives any DLL. The intern set only ever holds the
       distinct literals scripts contain -- a small, finite set -- so it never needs freeing. Strings from
       ELSEWHERE (entityGetName and friends) are not interned and still only live as long as their owner. */ \
    X(const char*, internString,           (const char*, text)) \
    /* ---- engine-owned dynamic arrays (see OcArray) ---- generic over the element type: the caller passes the
       element's size and a pointer to/for the value, so one set of functions serves every T the DSL can hold.
       `elemKind` is DSLType's own value for the element -- the engine needs it because two kinds aren't stored
       as the caller's bytes at all: Entity elements become refcounted EntityPtrs (a destroyed entity then reads
       back null instead of dangling) and string elements are COPIED engine-side (a script DLL's .rdata is
       unmapped on reload). Values are copied in/out rather than exposing an interior pointer, so no caller can
       hold or corrupt the storage. */ \
    X(void,        arrayPush,              (Entity*, owner), (OcArray*, handle), (int, elemKind), (int, elemSize), (const void*, value)) /* creates on *handle == 0 and writes the new id back through `handle` */ \
    X(int,         arrayGet,               (OcArray, handle), (int, index), (int, elemSize), (void*, outValue)) /* ENTITY/STRING elements only (POD goes through arrayElem): 1 on success, 0 (outValue untouched) for a stale handle, an out-of-range index, a dead entity, or a POD array */ \
    X(void,        arraySet,               (OcArray, handle), (int, index), (int, elemSize), (const void*, value)) /* Entity/String only; no-op when out of range */ \
    X(int,         arrayCount,             (OcArray, handle)) /* 0 for a stale/empty handle */ \
    X(void,        arrayClear,             (OcArray, handle)) \
    /* ---- AnimatorComponent ---- the state machine's parameters ARE its gameplay interface (see
       Animation:StateMachine): a script sets floats/bools/triggers and the graph's own transitions decide what
       plays, rather than a script naming clips directly. The older entitySetAnim* trio does the same thing off
       an Entity and is kept for the visual-script nodes built against it; these take the component handle, so
       the DSL reaches them through self.animator / `ifexist` like every other component. A null handle is a
       no-op (or a zero/empty read) throughout, same contract as the physics/audio/force blocks. */ \
    X(void*,       entityGetAnimatorComponent,(Entity*, entity)) \
    X(void,        animatorSetFloat,       (void*, animator), (const char*, name), (float, value)) \
    X(void,        animatorSetBool,        (void*, animator), (const char*, name), (int, value)) \
    X(void,        animatorSetTrigger,     (void*, animator), (const char*, name)) /* consumed by the next transition that tests it */ \
    X(float,       animatorGetFloat,       (void*, animator), (const char*, name)) /* 0 when never set */ \
    X(int,         animatorGetBool,        (void*, animator), (const char*, name)) \
    X(const char*, animatorGetStateName,   (void*, animator)) /* the state currently playing; "" with no graph */ \
    X(float,       animatorGetTimeInState, (void*, animator)) /* seconds since the current state was entered */ \
    X(float,       animatorGetSpeed,       (void*, animator)) /* the resolved playback rate for that state */ \
    X(int,         animatorGetEnabled,     (void*, animator)) \
    X(void,        animatorSetEnabled,     (void*, animator), (int, enabled))     /* ---- randomness ---- ENGINE-side on purpose: a generator living in the script DLL would reset its state
       on every hot-reload, so a script's "random" sequence would restart mid-run. One shared stream. */     X(float,       randomFloat,            (float, minValue), (float, maxValue))     X(int,         randomInt,              (int, minValue), (int, maxValue)) /* both ends INCLUSIVE */ \
    /* ---- world ---- the ambient state a script acts on rather than owns. physicsRayCast (above) reports its
       hit through out-pointers, which the DSL has no spelling for; this returns the same information by value
       so it can be exposed as a `RayHit?` and read only after the hit is proven. */ \
    X(OcRayHit,    worldRayCast,           (glm::vec3, origin), (glm::vec3, direction), (float, maxDistance)) /* direction need not be normalized */ \
    X(glm::vec3,   worldGetSunDirection,   (int, unused)) /* the arity macros have no zero-parameter form; pass 0 */ \
    /* One world-space debug line for this frame only (the renderer's per-frame list, cleared every present) --
       drawn unlit and depth-tested. `color` is 0xAABBGGRR. */ \
    X(void,        worldDrawLine,          (glm::vec3, from), (glm::vec3, to), (glm::vec3, color)) \
    /* First ROOT entity with this display name, or null. Roots only -- a whole-scene search would be a
       different (and far more expensive) operation, and the name is not unique either way. */ \
    X(Entity*,     worldFindRootEntity,    (const char*, displayName)) \
    /* ---- radius query ---- three calls, because the DSL has no way to hand back a list: the first RUNS the
       query and returns a HANDLE to its results, the other two read that handle. The results live in a
       THREAD-LOCAL ring of buffers, so concurrent callers never share one, and the handle is generation-tagged
       like OcArray -- a nested query takes its own slot, and a handle whose slot has since been recycled reads
       as empty rather than as someone else's results. 0 is never a valid handle. */ \
    X(int,         worldQueryRadius,       (glm::vec3, position), (float, radius)) \
    X(int,         worldQueryCount,        (int, queryHandle)) \
    X(Entity*,     worldQueryResultAt,     (int, queryHandle), (int, index)) /* null out of range or stale */ \
    /* ---- physics component (rigid body) ---- every one of these is a no-op / zero read on a null handle or a
       component whose body isn't valid, so gameplay code never has to guard. Angular quantities are RADIANS
       here (box3d's own unit); the DSL bindings convert, since the DSL speaks degrees throughout. */ \
    X(glm::vec3,   physicsGetAngularVelocity,(void*, physicsComponent)) \
    X(void,        physicsSetAngularVelocity,(void*, physicsComponent), (glm::vec3, radiansPerSecond)) \
    /* Impulses are instantaneous; forces accumulate over the step and box3d clears them every step, so a
       continuous push must be re-applied each frame. The AtPoint forms take a WORLD point and impart spin. */ \
    X(void,        physicsApplyImpulseAtPoint,(void*, physicsComponent), (glm::vec3, impulse), (glm::vec3, worldPoint)) \
    X(void,        physicsApplyAngularImpulse,(void*, physicsComponent), (glm::vec3, impulse)) \
    X(void,        physicsApplyForce,      (void*, physicsComponent), (glm::vec3, force)) \
    X(void,        physicsApplyForceAtPoint,(void*, physicsComponent), (glm::vec3, force), (glm::vec3, worldPoint)) \
    X(void,        physicsApplyTorque,     (void*, physicsComponent), (glm::vec3, torque)) \
    X(float,       physicsGetMass,         (void*, physicsComponent)) /* 0 for a static/kinematic body */ \
    X(glm::vec3,   physicsGetCenterOfMass, (void*, physicsComponent)) /* world space */ \
    X(glm::vec3,   physicsGetPointVelocity,(void*, physicsComponent), (glm::vec3, worldPoint)) /* includes spin */ \
    X(glm::vec3,   physicsGetPosition,     (void*, physicsComponent)) /* the SIMULATED pose, not the entity's */ \
    X(float,       physicsGetGravityScale, (void*, physicsComponent)) \
    X(void,        physicsSetGravityScale, (void*, physicsComponent), (float, scale)) \
    X(float,       physicsGetLinearDamping,(void*, physicsComponent)) \
    X(void,        physicsSetLinearDamping,(void*, physicsComponent), (float, damping)) \
    X(float,       physicsGetAngularDamping,(void*, physicsComponent)) \
    X(void,        physicsSetAngularDamping,(void*, physicsComponent), (float, damping)) \
    X(void,        physicsSetAwake,        (void*, physicsComponent), (int, awake)) \
    /* ---- particle component ---- the effect follows its entity on its own (ParticleComponent::update), so
       what a script controls is whether it emits and when it bursts, not where it is. */ \
    X(void*,       entityGetParticleComponent,(Entity*, entity)) \
    X(void,        particleBurst,          (void*, particleComponent)) /* every emitter's Burst count, once */ \
    X(int,         particleGetEmitting,    (void*, particleComponent)) \
    X(void,        particleSetEmitting,    (void*, particleComponent), (int, emitting)) /* continuous rates only; bursts still fire */ \
    /* ---- render component ---- READS only, plus the local offset. The node's own transform is recomposed
       from the entity's world transform every frame (Entity::updateTree), so there is nothing here to write it
       with: `offset` is the per-instance nudge that survives, and moving the entity is the other way. */ \
    X(void*,       entityGetRenderComponent,(Entity*, entity)) \
    X(float,       renderGetBoundsRadius,  (void*, renderComponent)) /* world space; 0 with no mesh */ \
    X(glm::vec3,   renderGetBoundsCenter,  (void*, renderComponent)) /* world space */ \
    X(int,         renderIsVisible,        (void*, renderComponent)) /* as of this frame's spatial pass */ \
    X(int,         renderIsSkinned,        (void*, renderComponent)) \
    X(glm::vec3,   renderGetOffsetPos,     (void*, renderComponent)) \
    X(float,       renderGetOffsetScale,   (void*, renderComponent)) \
    X(glm::quat,   renderGetOffsetRot,     (void*, renderComponent)) \
    X(void,        renderSetOffsetPos,     (void*, renderComponent), (glm::vec3, position)) \
    X(void,        renderSetOffsetScale,   (void*, renderComponent), (float, scale)) \
    X(void,        renderSetOffsetRot,     (void*, renderComponent), (glm::quat, rotation)) \
    /* ---- array iteration ---- arrayResolve turns a handle into the array itself ONCE, so a foreach pays the
       handle unpack + generation check per LOOP instead of per element (the registry's storage is chunked and
       never moves, which is what makes the pointer safe to hold). The view calls re-read the element storage
       every access, so a body that pushes or clears still reads correct data -- what is hoisted is the lookup,
       not the storage. Null view / out of range behave exactly like the handle forms. */ \
    X(void,        arrayRemoveAt,          (OcArray, handle), (int, index)) /* shifts the tail down; no-op when out of range */ \
    /* The ELEMENT's address, for POD arrays only -- lets the typed caller load/store it directly instead of
       handing a void* to a type-erased memcpy. Null for a stale handle, an out-of-range index, an elemSize
       mismatch, or an Entity/String array (no raw element exists there). Valid until the array grows. */ \
    X(void*,       arrayElem,              (OcArray, handle), (int, index), (int, elemSize)) \
    X(void*,       arrayViewElem,          (void*, view), (int, index), (int, elemSize)) \
    X(OcArraySpan, arraySpan,              (OcArray, handle), (int, elemSize)) /* POD element storage; {null,0} otherwise */ \
    X(void*,       arrayResolve,           (OcArray, handle)) /* null for a stale/empty handle */ \
    X(int,         arrayViewCount,         (void*, view)) \
    X(int,         arrayViewGet,           (void*, view), (int, index), (int, elemSize), (void*, outValue)) /* Entity/String only, as arrayGet */ \
    X(void,        arrayViewSet,           (void*, view), (int, index), (int, elemSize), (const void*, value)) /* Entity/String only */ \
    /* ---- light component ---- a LIST of lights on one entity, addressed by index [0, lightGetCount).
       Whole-light access only, through the OcLight mirror (see there): lightGetAt's return value IS the
       bounds check (1 on success, outLight untouched otherwise), which is what ifexist branches on;
       lightSetAt clamps/normalizes and no-ops out of range, so scripts need no bounds proof. Writes land
       on the live component (lights re-upload every frame). Node/DSL emits that touch single fields go
       copy-out / edit / copy-in through the same pair. */ \
    X(void*,       entityGetLightComponent,(Entity*, entity)) \
    X(int,         lightGetCount,          (void*, lightComponent)) \
    X(int,         lightGetAt,             (void*, lightComponent), (int, index), (OcLight*, outLight)) \
    X(void,        lightSetAt,             (void*, lightComponent), (int, index), (const OcLight*, light)) \
    /* ---- network ---- fires the named event locally AND across the network (server -> all clients,
       client -> server which relays to the other clients); in single player it degrades to sendEvent. */ \
    X(void,        sendNetworkEvent,       (const char*, eventName)) \
    /* As above, but attributes the event to the entity firing it (a script's `self`). The netId
       travels with the event so the receiving server can hand the entity to its event filter. */ \
    X(void,        sendNetworkEventFrom,   (Entity*, sender), (const char*, eventName)) \
    /* ---- game HUD ---- the in-game viewport overlay (Core.GameHud): a 10-slot hotbar (keys 1..9,0 ->
       slots 0..9; a press fires the global "Hotbar Select" script event) plus bars and numeric counters
       stacked from the top left. GLOBAL state -- one screen, one HUD -- so nothing here takes an entity.
       Bars/counters are keyed by their display name; insertion order is display order. Colors are linear
       0..1 RGB. Thread-safe (mutex-guarded state); harmless headless (state is written, nothing draws). */ \
    X(void,        hudSetSlot,             (int, slotIndex), (const char*, label), (int, count)) /* assigns the slot; count > 0 draws a stack number */ \
    X(void,        hudSetSlotCount,        (int, slotIndex), (int, count)) /* no-op on an unassigned slot */ \
    X(void,        hudClearSlot,           (int, slotIndex)) \
    X(void,        hudSelectSlot,          (int, slotIndex)) /* clamped to 0..9 */ \
    X(int,         hudGetSelectedSlot,     (int, unused)) /* the arity macros have no zero-parameter form; pass 0 */ \
    X(void,        hudSetHotbarVisible,    (int, visible)) /* explicit off-switch; the hotbar only shows while visible AND any slot is assigned */ \
    X(void,        hudSetBar,              (const char*, name), (float, value), (float, maxValue), (glm::vec3, color)) /* creates or updates; value clamps to 0..maxValue */ \
    X(void,        hudRemoveBar,           (const char*, name)) \
    X(void,        hudSetCounter,          (const char*, name), (float, value), (int, decimals), (glm::vec3, color)) /* decimals 0 = integer display */ \
    X(void,        hudRemoveCounter,       (const char*, name)) \
    X(void,        hudClear,               (int, unused)) /* removes every slot, bar and counter */ \
    /* ---- game components (GameUnit/GameStructure/GameProjectile — the shared C++ sim; the DSL is
       the orders/config tier: read state, re-tune stats, set/clear a unit's walk target) ---- */ \
    X(void*,       entityGetGameUnitComponent,(Entity*, entity)) \
    X(float,       gameUnitGetHealth,      (void*, gameUnit)) \
    X(void,        gameUnitSetHealth,      (void*, gameUnit), (float, health)) \
    X(float,       gameUnitGetHealthMax,   (void*, gameUnit)) \
    X(float,       gameUnitGetEnergy,      (void*, gameUnit)) \
    X(void,        gameUnitSetEnergy,      (void*, gameUnit), (float, energy)) \
    X(float,       gameUnitGetEnergyMax,   (void*, gameUnit)) \
    X(int,         gameUnitGetTeam,        (void*, gameUnit)) \
    X(int,         gameUnitIsCollapsed,    (void*, gameUnit)) \
    X(void,        gameUnitDamage,         (void*, gameUnit), (float, amount)) \
    X(void,        gameUnitSetTarget,      (void*, gameUnit), (glm::vec3, position)) /* LOCKS the walk target until cleared */ \
    X(void,        gameUnitClearTarget,    (void*, gameUnit)) /* back to automatic targeting */ \
    X(glm::vec3,   gameUnitGetTarget,      (void*, gameUnit)) \
    X(int,         gameUnitHasTarget,      (void*, gameUnit)) \
    X(void,        gameUnitSetMoveSpeed,   (void*, gameUnit), (float, speed)) \
    X(float,       gameUnitGetMoveSpeed,   (void*, gameUnit)) \
    X(void*,       entityGetGameStructureComponent,(Entity*, entity)) \
    X(float,       gameStructureGetHealth, (void*, gameStructure)) \
    X(void,        gameStructureSetHealth, (void*, gameStructure), (float, health)) \
    X(float,       gameStructureGetHealthMax,(void*, gameStructure)) \
    X(int,         gameStructureGetTeam,   (void*, gameStructure)) \
    X(int,         gameStructureIsBlueprint,(void*, gameStructure)) \
    X(void,        gameStructureDamage,    (void*, gameStructure), (float, amount)) /* respects invulnerable */ \
    X(void*,       entityGetGameProjectileComponent,(Entity*, entity)) \
    X(int,         gameProjectileGetTeam,  (void*, gameProjectile)) \
    X(float,       gameProjectileGetAge,   (void*, gameProjectile))

#if defined(SCRIPT_STATIC_BUILD)
// Cooked build: the engine thunks the inline ctx methods forward to (defined extern "C" in ScriptContext.cpp,
extern "C" {
    void thunk_log(const char* message);       // log/logf are outside SCRIPT_CTX_FUNCS (see there); declared by hand
    void thunk_vlogf(const char* fmt, va_list ap);
    unsigned int thunk_networkEventSender(void); // zero-arg, so also outside the table
#define SCRIPT_CTX_DECL(ret, name, ...) ret thunk_##name (SCRIPT_CTX_PARAMS(__VA_ARGS__));
    SCRIPT_CTX_FUNCS(SCRIPT_CTX_DECL)
#undef SCRIPT_CTX_DECL
}
#endif

// Services the host exposes to scripts. The host fills these in; scripts only call them.
typedef struct ScriptContext
{
    // Per-frame data
    float deltaSeconds;
    float elapsedSeconds;
    glm::vec3 cameraPosition;
    glm::vec3 cameraDirection;
    glm::vec3 cameraUp;
    float     cameraFovDeg;
    float     cameraNear;
	float     cameraFar;

#if defined(SCRIPT_STATIC_BUILD)
    // Cooked build: `ctx->foo(x)` is an inline forwarder to `thunk_foo(x)` (the engine thunk), so LTCG inlines it
    void log(const char* message) const { thunk_log(message); }
    void logf(const char* fmt, ...) const { va_list ap; va_start(ap, fmt); thunk_vlogf(fmt, ap); va_end(ap); }
#define SCRIPT_CTX_METHOD(ret, name, ...) ret name (SCRIPT_CTX_PARAMS(__VA_ARGS__)) const { return thunk_##name (SCRIPT_CTX_ARGS(__VA_ARGS__)); }
    SCRIPT_CTX_FUNCS(SCRIPT_CTX_METHOD)
#undef SCRIPT_CTX_METHOD
    unsigned int networkEventSender() const { return thunk_networkEventSender(); }
#else
    void  (* const log)(const char* message);
    void  (* const logf)(const char* message, ...);
    // The rest of the const function-pointer table, generated from SCRIPT_CTX_FUNCS (docs live there).
#define SCRIPT_CTX_PTR(ret, name, ...) ret (* const name) (SCRIPT_CTX_PARAMS(__VA_ARGS__));
    SCRIPT_CTX_FUNCS(SCRIPT_CTX_PTR)
#undef SCRIPT_CTX_PTR
    // Zero-arg, so it cannot live in the table above (see ScriptCtxMacros.h). APPENDED here, after
    // the generated pointers, so existing cached DLLs keep their offsets.
    // Who fired the event being handled: 0 = the server (or a local single-player fire), else the
    // originating client. Server-stamped from the receiving connection, so it cannot be forged.
    unsigned int (* const networkEventSender)(void);

#ifdef __cplusplus
#ifndef SCRIPT_BUILD
    ScriptContext(); // the engine binds all the function pointers here; scripts never construct one

    void update(const Camera& camera, float newDeltaSeconds, float newElapsedSeconds);
#endif // SCRIPT_BUILD
#endif // __cplusplus
#endif // SCRIPT_STATIC_BUILD (inline forwarders) vs. the pointer table
} ScriptContext;

#pragma warning(pop)

typedef void (*ScriptOnSpawnFn)(const ScriptContext*, Entity* self, void* scriptData);
typedef void (*ScriptOnDestroyFn)(const ScriptContext*, Entity* self, void* scriptData);
typedef void (*ScriptUpdateFn)(const ScriptContext*, Entity* self, float deltaSeconds, void* scriptData);
typedef void (*ScriptOnEventFn)(const ScriptContext*, Entity* self, int eventIdx, void* scriptData);
typedef int          (*ScriptEventCountFn)(void);
typedef const char*  (*ScriptEventNameFn)(int eventIdx);
typedef void         (*ScriptOnPhysicsEventFn)(const ScriptContext*, Entity* self, Entity* other, int begin, int sensor, long long contactId, void* scriptData);
typedef unsigned int (*ScriptDataSizeFn)(void);

// Optional export: which components the script requires (see //@@require), as an EComponentID bitmask
// (Code/Entity/Private/EntityDef.ixx -- Script has no visibility into that enum, so this is just a plain
// uint32 here). The host allocates one 8-byte pointer slot per set bit at the FRONT of ScriptData, in
// ascending bit order, and fills each straight off getComponent<T>() right after allocating the block, before
// OnSpawn runs -- so self.physics/audio/force (their emit is "scriptData-><name>", see
// ScriptBindings::registerComponentType) always read an already-resolved pointer instead of re-fetching the
// handle on every access. Absent (or 0) means the script requires no components.
typedef unsigned int (*ScriptRequiredComponentsFn)(void);

// Optional export: a hash of ScriptData's LAYOUT (every field's type and name, in order). Size alone can't tell
// a layout change from a no-op -- "int a; int b;" and "float a; float b;" are the same size, and reinterpreting
// one as the other silently corrupts every field, OcArray handles included. The host keeps a script's existing
// data block across a hot-reload only while this value matches; otherwise it drops the block (freeing the
// arrays those handles owned) and starts from a zeroed one. Absent = treat every reload as a layout change.
typedef unsigned int (*ScriptDataLayoutIdFn)(void);

// Optional export: the script's EXPOSED ScriptData fields (see OcScriptField). Absent, or a count of 0, means
// the script exposes nothing -- which is every script that never marks a field private/public, so the editor
// simply has nothing to show. The returned pointer is to static storage in the script and lives as long as the
// module does; the engine never holds it across a reload.
typedef const OcScriptField* (*ScriptDataFieldsFn)(int* outCount);

#ifdef __cplusplus
}
#endif

// ---- array access helpers ----
// The typed skin over the type-ERASED array ABI above (void* + elemSize -- one entry point per operation
// covering every element type, since the table itself is C linkage and APPEND-only). T is the type the ABI
// actually writes through the void*: `Entity*` for entity elements, `const char*` for strings, the value type
// otherwise (see ScriptBindings::cppTypeName, which builds the emit templates naming them).
//
// They live HERE, not in the transpiler's output, because this header is force-included (/FI) into every
// script TU -- so generated code carries no preamble, and this is ordinary source instead of emitted strings.
// Outside the extern "C" block: a template can't have C linkage. Nothing about the ABI changes by adding
// them, so a cached DLL built against an older header keeps working (it carried its own copy).
//
// The parameter is what makes these functions rather than inlinable expressions: `&value` needs an lvalue,
// and every push/set call site passes an rvalue (`vec3(1,2,3)`, `a + b`, an interned literal). ocArrGet needs
// one more thing -- storage to write into, in an EXPRESSION position (a `foreach` element initializer) -- and
// its `T v = T()` is what makes a stale handle or out-of-range index read back as a default: the engine
// leaves outValue untouched on failure rather than writing garbage.
#ifdef __cplusplus
template<class T> T ocArrGet(const ScriptContext* ctx, OcArray h, int i)
{ T v = T(); ctx->arrayGet(h, i, (int)sizeof(T), &v); return v; }
template<class T> void ocArrSet(const ScriptContext* ctx, OcArray h, int i, const T& v)
{ ctx->arraySet(h, i, (int)sizeof(T), &v); }
template<class T> void ocArrPush(const ScriptContext* ctx, Entity* self, OcArray* h, int kind, const T& v)
{ ctx->arrayPush(self, h, kind, (int)sizeof(T), &v); }
// POD element access as a typed LOAD/STORE through the element's own address -- no type-erased copy, and the
// success flag is the pointer itself. ocArrLoad is what an `ifexist ... at <key>` branches on; it leaves `out`
// untouched on a miss, exactly like arrayGet did.
template<class T> bool ocArrLoad(const ScriptContext* ctx, OcArray h, int i, T& out)
{
    if (const void* element = ctx->arrayElem(h, i, (int)sizeof(T))) { out = *static_cast<const T*>(element); return true; }
    return false;
}
template<class T> void ocArrStore(const ScriptContext* ctx, OcArray h, int i, const T& v)
{
    if (void* element = ctx->arrayElem(h, i, (int)sizeof(T))) *static_cast<T*>(element) = v;
}

// The hoisted-view forms a foreach emits; same defaults-on-failure contract as ocArrGet above.
template<class T> T ocArrViewGet(const ScriptContext* ctx, void* view, int i)
{ T v = T(); ctx->arrayViewGet(view, i, (int)sizeof(T), &v); return v; }
template<class T> void ocArrViewSet(const ScriptContext* ctx, void* view, int i, const T& v)
{ ctx->arrayViewSet(view, i, (int)sizeof(T), &v); }

// One light BY VALUE, for the foreach at-emit over self.light.lights (defaults-on-failure like ocArrGet;
// ifexist goes through ctx->lightGetAt directly, whose return value is the existence check).
inline OcLight ocLightAt(const ScriptContext* ctx, void* lightComponent, int i)
{ OcLight v{}; ctx->lightGetAt(lightComponent, i, &v); return v; }

// ---- math helpers ----
// The `math.*` bindings are otherwise one-to-one glm/std calls emitted inline. These few are here because their
// formula USES AN ARGUMENT MORE THAN ONCE: substituting the argument's expression twice would evaluate it twice,
// so `math.remap(rollDice(), ...)` would roll two different numbers. A function parameter evaluates once.
// Angles are DEGREES throughout the DSL surface, so these are too.
inline float ocInverseLerp(float a, float b, float v) { return (b == a) ? 0.0f : (v - a) / (b - a); }
inline float ocRemap(float v, float inMin, float inMax, float outMin, float outMax)
{ return outMin + ocInverseLerp(inMin, inMax, v) * (outMax - outMin); }
inline float ocMoveTowards(float current, float target, float maxDelta)
{ const float d = target - current; return (d * d <= maxDelta * maxDelta) ? target : current + (d < 0.0f ? -maxDelta : maxDelta); }
// The signed difference from `a` to `b`, always in [-180, 180] -- the short way around.
inline float ocDeltaAngle(float a, float b)
{ float d = std::fmod(b - a, 360.0f); if (d > 180.0f) d -= 360.0f; if (d < -180.0f) d += 360.0f; return d; }
inline float ocWrapAngle(float deg) { return ocDeltaAngle(0.0f, deg); }
inline float ocLerpAngle(float a, float b, float t) { return a + ocDeltaAngle(a, b) * t; }
inline float ocMoveTowardsAngle(float current, float target, float maxDelta)
{ return current + ocMoveTowards(0.0f, ocDeltaAngle(current, target), maxDelta); }
// Uniform on the unit sphere (z picked uniformly, then the ring around it -- equal-area, unlike normalizing
// three uniform components). Built on randomFloat rather than being its own ABI row: the engine owns the one
// generator, and this is pure arithmetic on top of it.
inline glm::vec3 ocRandomUnitVector(const ScriptContext* ctx)
{
    const float z = ctx->randomFloat(-1.0f, 1.0f);
    const float a = ctx->randomFloat(0.0f, 6.28318530718f);
    const float r = std::sqrt(1.0f - z * z);
    return glm::vec3(r * std::cos(a), r * std::sin(a), z);
}
#endif

// ---- static/cooked build registry ----
#if defined(SCRIPT_STATIC_BUILD) || defined(SCRIPTS_STATIC)
enum OcScriptEntryKind
{
    OC_SCRIPT_ON_SPAWN = 0,
    OC_SCRIPT_UPDATE,
    OC_SCRIPT_ON_DESTROY,
    OC_SCRIPT_ON_EVENT,
    OC_SCRIPT_ON_PHYSICS_EVENT,
    OC_SCRIPT_DATA_SIZE,
    OC_SCRIPT_DATA_LAYOUT_ID,
    OC_SCRIPT_DATA_FIELDS,
    OC_SCRIPT_REQUIRED_COMPONENTS,
    OC_SCRIPT_EVENT_COUNT,
    OC_SCRIPT_EVENT_NAME,
    OC_SCRIPT_ENTRY_COUNT
};
#ifdef __cplusplus
extern "C" {
#endif
void ocRegisterScriptEntry(const char* scriptPath, int kind, void* fn);
#ifdef __cplusplus
}
#endif
#endif

// Entry-point linkage. DLL build
#if defined(SCRIPT_STATIC_BUILD)
#define SCRIPT_EXPORT
#else
#define SCRIPT_EXPORT extern "C" __declspec(dllexport)
#endif

// Each .scr/.dsl emits a REGISTER_*() after the entry points it defines.
#if defined(SCRIPT_STATIC_BUILD)
#define REGISTER_ON_SPAWN()          static const int _ocRegOnSpawn   = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_ON_SPAWN,         (void*)&OnSpawn), 0);
#define REGISTER_UPDATE()            static const int _ocRegUpdate    = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_UPDATE,          (void*)&Update), 0);
#define REGISTER_ON_DESTROY()        static const int _ocRegOnDestroy = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_ON_DESTROY,      (void*)&OnDestroy), 0);
#define REGISTER_ON_PHYSICS_EVENT()  static const int _ocRegOnPhys    = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_ON_PHYSICS_EVENT,(void*)&OnPhysicsEvent), 0);
#define REGISTER_SCRIPT_DATA_SIZE()  static const int _ocRegDataSize  = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_DATA_SIZE,       (void*)&ScriptDataSize), \
                                                                        ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_DATA_LAYOUT_ID,  (void*)&ScriptDataLayoutId), 0);
#define REGISTER_SCRIPT_REQUIRED_COMPONENTS() static const int _ocRegReqComp = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_REQUIRED_COMPONENTS, (void*)&ScriptRequiredComponents), 0);
#define REGISTER_SCRIPT_DATA_FIELDS() static const int _ocRegDataFields = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_DATA_FIELDS, (void*)&ScriptDataFields), 0);
#define REGISTER_ON_EVENT()          static const int _ocRegOnEvent   = (ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_ON_EVENT,        (void*)&OnEvent), \
                                                                        ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_EVENT_COUNT,     (void*)&ScriptEventCount), \
                                                                        ocRegisterScriptEntry(OC_CURRENT_SCRIPT, OC_SCRIPT_EVENT_NAME,      (void*)&ScriptEventName), 0);
#else
#define REGISTER_ON_SPAWN()
#define REGISTER_UPDATE()
#define REGISTER_ON_DESTROY()
#define REGISTER_ON_PHYSICS_EVENT()
#define REGISTER_SCRIPT_DATA_SIZE()
#define REGISTER_SCRIPT_REQUIRED_COMPONENTS()
#define REGISTER_SCRIPT_DATA_FIELDS()
#define REGISTER_ON_EVENT()
#endif
