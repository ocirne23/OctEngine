module;

#include <cstdarg>
#include <cstddef>
#include "ScriptAPI.h"

module Entity;

import Core;
import Core.glm;
import Core.Log;
import Core.Time;
import Core.SDL;
import Core.GameHud;
import Core.Sphere;
import Core.Transform;
import Core.Camera;
import RendererVK;
import Animation;
import Physics;
import Force;
import Spatial;
import Script;
import :Entity;
import :Component;
import :ScriptComponent; // releaseScriptArrays (module-linkage, declared next to the component)
import :World;

void registerScriptDslBindings()
{
    ScriptBindings& bindings = Globals::scriptBindings;
    using T = DSLType;

    // The vector/quaternion surface. Every emit is a plain glm call, so the generated C++ stays exactly what
    // the same expression would be written by hand -- and everything here resolves out of the two headers
    // ScriptAPI.h already includes (<glm/glm.hpp> and <glm/gtc/quaternion.hpp>), so script DLLs need nothing
    // extra. "$r" (the receiver) is parenthesized wherever it lands inside a larger expression, since it can be
    // an arbitrary composed chain. T::ThisBinding means "this struct's own type", so the shared rows below read
    // identically for vec2/3/4 (see DSLType::ThisBinding).
    //
    // Registration ORDER carries the cross-type dependencies: a function's parameter/return types must already
    // exist, so vec2 comes first (vec3.xy needs it), then vec3 (vec4.xyz, and every quat function that speaks
    // vec3), then vec4, then quat. That's also why rotating a vector lives on quat as rotate(vec3) rather than
    // on vec3 as rotatedBy(quat) -- the latter would need quat before quat exists.
    const DSLType vec2 = bindings.registerStruct({ "vec2", "glm::vec2", { { "x", T::Float }, { "y", T::Float } }, "glm::vec2($1, $2)",
        {
            { "x", T::Float, "$r.x" },
            { "y", T::Float, "$r.y" },
        },
        {
            { "length",          T::Float,       {},                                                   "glm::length($r)" },
            // Comparing or sorting by distance rarely needs the square root -- glm::length2 lives in an
            // extension header that isn't included, so the dot product with itself stands in for it.
            { "lengthSquared",   T::Float,       {},                                                   "glm::dot($r, $r)" },
            { "normalized",      T::ThisBinding, {},                                                   "glm::normalize($r)" },
            { "dot",             T::Float,       { { "other", T::ThisBinding } },                      "glm::dot($r, $1)" },
            { "distance",        T::Float,       { { "other", T::ThisBinding } },                      "glm::distance($r, $1)" },
            { "distanceSquared", T::Float,       { { "other", T::ThisBinding } },                      "glm::dot(($r) - ($1), ($r) - ($1))" },
            // Scaling by a scalar can't be written as an operator: a chain constrains every term to one type.
            { "scaled",          T::ThisBinding, { { "factor", T::Float } },                           "(($r) * ($1))" },
            { "negated",         T::ThisBinding, {},                                                   "(-($r))" },
            { "lerp",            T::ThisBinding, { { "target", T::ThisBinding }, { "t", T::Float } },  "glm::mix($r, $1, $2)" },
            { "min",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::min($r, $1)" },
            { "max",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::max($r, $1)" },
            { "clamp",           T::ThisBinding, { { "low", T::ThisBinding }, { "high", T::ThisBinding } }, "glm::clamp($r, $1, $2)" },
            { "abs",             T::ThisBinding, {},                                                   "glm::abs($r)" },
            { "floor",           T::ThisBinding, {},                                                   "glm::floor($r)" },
            { "ceil",            T::ThisBinding, {},                                                   "glm::ceil($r)" },
            { "round",           T::ThisBinding, {},                                                   "glm::round($r)" },
            // 2D only: the heading of the vector, and the 90-degree turn of it.
            { "angleDeg",        T::Float,       {},                                                   "glm::degrees(glm::atan(($r).y, ($r).x))" },
            { "perpendicular",   T::ThisBinding, {},                                                   "glm::vec2(-($r).y, ($r).x)" },
        } });
    const DSLType vec3 = bindings.registerStruct({ "vec3", "glm::vec3", { { "x", T::Float }, { "y", T::Float }, { "z", T::Float } }, "glm::vec3($1, $2, $3)",
        {
            { "x", T::Float, "$r.x" },
            { "y", T::Float, "$r.y" },
            { "z", T::Float, "$r.z" },
        },
        {
            { "length",          T::Float,       {},                                                   "glm::length($r)" },
            { "lengthSquared",   T::Float,       {},                                                   "glm::dot($r, $r)" },
            { "normalized",      T::ThisBinding, {},                                                   "glm::normalize($r)" },
            { "dot",             T::Float,       { { "other", T::ThisBinding } },                      "glm::dot($r, $1)" },
            { "cross",           T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::cross($r, $1)" },
            { "distance",        T::Float,       { { "other", T::ThisBinding } },                      "glm::distance($r, $1)" },
            { "distanceSquared", T::Float,       { { "other", T::ThisBinding } },                      "glm::dot(($r) - ($1), ($r) - ($1))" },
            { "scaled",          T::ThisBinding, { { "factor", T::Float } },                           "(($r) * ($1))" },
            { "negated",         T::ThisBinding, {},                                                   "(-($r))" },
            { "lerp",            T::ThisBinding, { { "target", T::ThisBinding }, { "t", T::Float } },  "glm::mix($r, $1, $2)" },
            { "min",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::min($r, $1)" },
            { "max",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::max($r, $1)" },
            { "clamp",           T::ThisBinding, { { "low", T::ThisBinding }, { "high", T::ThisBinding } }, "glm::clamp($r, $1, $2)" },
            { "abs",             T::ThisBinding, {},                                                   "glm::abs($r)" },
            { "floor",           T::ThisBinding, {},                                                   "glm::floor($r)" },
            { "ceil",            T::ThisBinding, {},                                                   "glm::ceil($r)" },
            { "round",           T::ThisBinding, {},                                                   "glm::round($r)" },
            // `normal` is expected normalized, matching glm's own contract for these two.
            { "reflect",         T::ThisBinding, { { "normal", T::ThisBinding } },                     "glm::reflect($r, $1)" },
            { "refract",         T::ThisBinding, { { "normal", T::ThisBinding }, { "eta", T::Float } },"glm::refract($r, $1, $2)" },
            { "xy",              vec2,           {},                                                   "glm::vec2(($r).x, ($r).y)" },
        } });
    const DSLType vec4 = bindings.registerStruct({ "vec4", "glm::vec4", { { "x", T::Float }, { "y", T::Float }, { "z", T::Float }, { "w", T::Float } }, "glm::vec4($1, $2, $3, $4)",
        {
            { "x", T::Float, "$r.x" },
            { "y", T::Float, "$r.y" },
            { "z", T::Float, "$r.z" },
            { "w", T::Float, "$r.w" },
        },
        {
            { "length",          T::Float,       {},                                                   "glm::length($r)" },
            { "lengthSquared",   T::Float,       {},                                                   "glm::dot($r, $r)" },
            { "normalized",      T::ThisBinding, {},                                                   "glm::normalize($r)" },
            { "dot",             T::Float,       { { "other", T::ThisBinding } },                      "glm::dot($r, $1)" },
            { "distance",        T::Float,       { { "other", T::ThisBinding } },                      "glm::distance($r, $1)" },
            { "distanceSquared", T::Float,       { { "other", T::ThisBinding } },                      "glm::dot(($r) - ($1), ($r) - ($1))" },
            { "scaled",          T::ThisBinding, { { "factor", T::Float } },                           "(($r) * ($1))" },
            { "negated",         T::ThisBinding, {},                                                   "(-($r))" },
            { "lerp",            T::ThisBinding, { { "target", T::ThisBinding }, { "t", T::Float } },  "glm::mix($r, $1, $2)" },
            { "min",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::min($r, $1)" },
            { "max",             T::ThisBinding, { { "other", T::ThisBinding } },                      "glm::max($r, $1)" },
            { "clamp",           T::ThisBinding, { { "low", T::ThisBinding }, { "high", T::ThisBinding } }, "glm::clamp($r, $1, $2)" },
            { "abs",             T::ThisBinding, {},                                                   "glm::abs($r)" },
            { "floor",           T::ThisBinding, {},                                                   "glm::floor($r)" },
            { "ceil",            T::ThisBinding, {},                                                   "glm::ceil($r)" },
            { "round",           T::ThisBinding, {},                                                   "glm::round($r)" },
            { "xy",              vec2,           {},                                                   "glm::vec2(($r).x, ($r).y)" },
            { "xyz",             vec3,           {},                                                   "glm::vec3(($r).x, ($r).y, ($r).z)" },
        } });
    (void)(vec4);
    // glm::quat's own 4-scalar constructor takes (w, x, y, z) -- opposite of vec4's (x, y, z, w) -- so the
    // positional args reorder in the emit while the DSL's own constructorParams/members stay x, y, z, w for
    // consistency. That raw form is rarely what an author wants, though: quatFromEuler/quatFromAxisAngle in the
    // Engine section below are the readable ways to build one.
    const DSLType quat = bindings.registerStruct({ "quat", "glm::quat", { { "x", T::Float }, { "y", T::Float }, { "z", T::Float }, { "w", T::Float } }, "glm::quat($4, $1, $2, $3)",
        {
            { "x", T::Float, "$r.x" },
            { "y", T::Float, "$r.y" },
            { "z", T::Float, "$r.z" },
            { "w", T::Float, "$r.w" },
        },
        {
            { "length",     T::Float,       {},                                                   "glm::length($r)" },
            { "normalized", T::ThisBinding, {},                                                   "glm::normalize($r)" },
            { "dot",        T::Float,       { { "other", T::ThisBinding } },                      "glm::dot($r, $1)" },
            // For a UNIT quaternion the conjugate IS the inverse and is cheaper; inverse is correct regardless.
            { "inverse",    T::ThisBinding, {},                                                   "glm::inverse($r)" },
            { "conjugate",  T::ThisBinding, {},                                                   "glm::conjugate($r)" },
            // Composition is not commutative: a.then(b) applies a first, then b.
            { "then",       T::ThisBinding, { { "next", T::ThisBinding } },                       "(($1) * ($r))" },
            { "slerp",      T::ThisBinding, { { "target", T::ThisBinding }, { "t", T::Float } },  "glm::slerp($r, $1, $2)" },
            { "rotate",     vec3,           { { "v", vec3 } },                                    "(($r) * ($1))" },
            { "euler",      vec3,           {},                                                   "glm::degrees(glm::eulerAngles($r))" },
            { "axis",       vec3,           {},                                                   "glm::axis($r)" },
            { "angleDeg",   T::Float,       {},                                                   "glm::degrees(glm::angle($r))" },
        } });
    // An Entity that may not be there ("self.parent" at the root, a spawn that failed, a query that found
    // nothing). NOT an Entity value: the only way in is "ifexist Entity e in <the Entity? expression>", which
    // makes the null check impossible to forget and leaves nothing in the DSL needing a manual one.
    //
    // The lookup is GENERIC -- "$r" is whatever expression produced the optional, so one type serves a member
    // (self.parent) and a function return (world.spawn / world.nearestEntity) with no per-source variant. Each
    // of those supplies its own emit; this only tests it, once.
    const DSLType optionalEntityType = bindings.registerOptionalType("Entity?", T::Entity, "($v = $r) != nullptr");

    bindings.registerObject({ "self", T::Entity, /*sidebarTopLevel*/ true,
        {
            { "setEnabled",     T::Void,  { { "enabled", T::Bool } }, "ctx->entitySetEnabled($r, $1)" },
        },
        {
            { "pos",     vec3,        "$r->pos" }, // self is Entity* -- a real field of the ABI mirror struct
            { "scale",   T::Float,    "$r->scale" },
            { "rot",     quat,        "$r->rot" },
            { "parent",  optionalEntityType, "$r->parent", /*writable*/ false },
            // Both belong to the SCRIPT, not to the entity: `data`'s emit ignores the receiver entirely and
            // `events` resolves against this document's own list, so neither means anything on another entity.
            { "data",    T::ScriptData,   "(*scriptData)", /*writable*/ false, /*selfOnly*/ true },
            { "events",  T::ScriptEvents, "$r",            /*writable*/ false, /*selfOnly*/ true }, // special case in transpiler
        } });

    // The child list as its own collection type, so walking it reads as "foreach Entity e in
    // self.scene.children" -- a component isn't a collection, and shouldn't be iterable as one. It SPELLS as
    // "Entity[]" (see dslTypeName): to a script it's an indexable collection like any other. Nothing is copied
    // to produce it -- the `children` member's emit is just "$r", so the value IS the SceneComponent handle and
    // these templates run straight against it. sceneGetChildAt is null-safe past the end on its own, which is
    // what keeps the loop sound if the child list changes mid-iteration AND what lets `ifexist` test the miss.
    const DSLType entityListType = bindings.registerSequenceType("EntityList", T::Entity,
        "ctx->sceneGetChildCount($r)", "ctx->sceneGetChildAt($r, $i)",
        "($v = ctx->sceneGetChildAt($r, $1)) != nullptr");

    const DSLType sceneType = bindings.registerComponentType("scene", "SceneComponent", EComponentID_Scene,
        "ctx->entityGetSceneComponent($r)");
    bindings.registerObject({ "scene", sceneType, /*sidebarTopLevel*/ false,
        {
            { "getNumChildren", T::Int,    {},                             "ctx->sceneGetChildCount($r)"},
            // First DIRECT child with this display name -- an Entity?, since there may be none. Not recursive:
            // "my child called X" is a different question from "anything under me called X".
            { "findChild",      optionalEntityType, {{ "name", T::String }},    "ctx->sceneFindChild($r, $1)"},
            { "getChild",       T::Entity, {{ "index", T::Int }},          "ctx->sceneGetChildAt($r, $1)"},
            { "removeChildIdx", T::Void,   {{ "index", T::Int }},          "ctx->sceneRemoveChildAt($r, $1)"},
            { "removeChild",    T::Void,   {{ "entity", T::Entity }},      "ctx->sceneRemoveChild($r, $1)"},
            { "addChild",       T::Void,   {{ "entity", T::Entity }},      "ctx->sceneAddChild($r, $1)"},
        },
        {
            // A MEMBER, not an accessor call: "self.scene.children" is the collection itself. Read-only -- the
            // list belongs to the engine, and add/removeChild are how it changes.
            { "children", entityListType, "$r", /*writable*/ false },
        } });

    // Animation is the ANIMATOR's surface, not the entity's: a script drives the state machine's parameters and
    // the graph's own transitions decide what plays (see Animation:StateMachine). Nothing here names a clip --
    // that's the .apl author's job, and going around it would desync the graph from what's actually playing.
    const DSLType animatorType = bindings.registerComponentType("animator", "AnimatorComponent", EComponentID_Animator,
        "ctx->entityGetAnimatorComponent($r)");
    bindings.registerObject({ "animator", animatorType, /*sidebarTopLevel*/ false,
        {
            { "setFloat",   T::Void,  { { "name", T::String }, { "value", T::Float } }, "ctx->animatorSetFloat($r, $1, $2)" },
            { "setBool",    T::Void,  { { "name", T::String }, { "value", T::Bool } },  "ctx->animatorSetBool($r, $1, $2)" },
            // Consumed by the next transition that tests it -- fire-and-forget, so there is no getter.
            { "setTrigger", T::Void,  { { "name", T::String } },                        "ctx->animatorSetTrigger($r, $1)" },
            { "getFloat",   T::Float, { { "name", T::String } },                        "ctx->animatorGetFloat($r, $1)" },
            { "getBool",    T::Bool,  { { "name", T::String } },                        "(ctx->animatorGetBool($r, $1) != 0)" },
            // Pausing the animator without touching its parameters. A FUNCTION, not a writable member: a
            // member's emit is used verbatim as an assignment TARGET, so only a real lvalue ("$r->pos") can be
            // writable -- a getter call can't sit on the left of an `=`.
            { "setEnabled", T::Void,  { { "enabled", T::Bool } },                       "ctx->animatorSetEnabled($r, $1)" },
        },
        {
            { "state",       T::String, "ctx->animatorGetStateName($r)",   /*writable*/ false },
            { "timeInState", T::Float,  "ctx->animatorGetTimeInState($r)", /*writable*/ false },
            { "speed",       T::Float,  "ctx->animatorGetSpeed($r)",       /*writable*/ false },
            { "enabled",     T::Bool,   "(ctx->animatorGetEnabled($r) != 0)", /*writable*/ false },
        } });

    const DSLType physicsType = bindings.registerComponentType("physics", "PhysicsComponent", EComponentID_Physics,
        "ctx->entityGetPhysicsComponent($r)");
    bindings.registerObject({ "physics", physicsType, /*sidebarTopLevel*/ false,
        {
            // READS are members (below), WRITES are functions -- the split every binding here uses, and the
            // only one that works: a member's emit doubles as an assignment target, so anything backed by a
            // getter CALL has to be read-only, which leaves a setter function as the way to write it.

            // -- velocity -- angular is DEGREES per second here, radians across the ABI (box3d's own unit)
            { "setVelocity",        T::Void, { { "velocity", vec3 } },          "ctx->physicsSetVelocity($r, $1)" },
            { "setAngularVelocity", T::Void, { { "degreesPerSecond", vec3 } },  "ctx->physicsSetAngularVelocity($r, glm::radians($1))" },
            // A settled body ignores a changed velocity until it wakes. Every apply* below wakes it for you, so
            // this is only for the cases that don't: a direct velocity write, or forcing a body to sleep.
            { "setAwake",           T::Void, { { "awake", T::Bool } },          "ctx->physicsSetAwake($r, $1)" },

            // -- impulses -- INSTANTANEOUS: a hit, a jump. At the centre of mass unless a world point is given,
            // in which case the offset also imparts spin.
            { "applyImpulse",        T::Void, { { "impulse", vec3 } },                                  "ctx->physicsApplyImpulse($r, $1)" },
            { "applyImpulseAtPoint", T::Void, { { "impulse", vec3 }, { "worldPoint", vec3 } },          "ctx->physicsApplyImpulseAtPoint($r, $1, $2)" },
            { "applyAngularImpulse", T::Void, { { "impulse", vec3 } },                                  "ctx->physicsApplyAngularImpulse($r, $1)" },

            // -- forces -- CONTINUOUS: box3d clears accumulated forces every step, so a sustained push (thrust,
            // wind, a magnet) has to be re-applied every Update. One call is a single step's worth.
            { "applyForce",        T::Void, { { "force", vec3 } },                                      "ctx->physicsApplyForce($r, $1)" },
            { "applyForceAtPoint", T::Void, { { "force", vec3 }, { "worldPoint", vec3 } },              "ctx->physicsApplyForceAtPoint($r, $1, $2)" },
            { "applyTorque",       T::Void, { { "torque", vec3 } },                                     "ctx->physicsApplyTorque($r, $1)" },

            // -- per-body tuning -- gravityScale 0 floats, 1 is normal, negative falls upward
            { "setGravityScale",   T::Void, { { "scale", T::Float } },      "ctx->physicsSetGravityScale($r, $1)" },
            { "setLinearDamping",  T::Void, { { "damping", T::Float } },    "ctx->physicsSetLinearDamping($r, $1)" },
            { "setAngularDamping", T::Void, { { "damping", T::Float } },    "ctx->physicsSetAngularDamping($r, $1)" },

            // Queued: the pose lands at the next physics update, so the reads below still report the old
            // one for the rest of the frame. The only way to move a body, of any type.
            { "teleport",          T::Void, { { "position", vec3 }, { "eulerDeg", vec3 } }, "ctx->physicsTeleport($r, $1, $2)" },
            // Takes an argument, so it stays a function rather than a member. The velocity AT a world point
            // includes the spin contribution, which is what a contact response or surface drag actually wants.
            { "getPointVelocity",  vec3,    { { "worldPoint", vec3 } },     "ctx->physicsGetPointVelocity($r, $1)" },
        },
        {
            { "velocity",        vec3,     "ctx->physicsGetVelocity($r)",        /*writable*/ false },
            // Degrees per second, matching setAngularVelocity and the DSL's convention everywhere else.
            { "angularVelocity", vec3,     "glm::degrees(ctx->physicsGetAngularVelocity($r))", /*writable*/ false },
            { "mass",            T::Float, "ctx->physicsGetMass($r)",            /*writable*/ false }, // 0 if static/kinematic
            { "centerOfMass",    vec3,     "ctx->physicsGetCenterOfMass($r)",    /*writable*/ false }, // world space
            // The SIMULATED pose. Distinct from self.pos, which a dynamic body only catches up to at the end of
            // the entity update -- reading it mid-frame is how you see where the body actually is right now.
            { "position",        vec3,     "ctx->physicsGetPosition($r)",        /*writable*/ false },
            { "awake",           T::Bool,  "(ctx->physicsIsAwake($r) != 0)",     /*writable*/ false },
            { "gravityScale",    T::Float, "ctx->physicsGetGravityScale($r)",    /*writable*/ false },
            { "linearDamping",   T::Float, "ctx->physicsGetLinearDamping($r)",   /*writable*/ false },
            { "angularDamping",  T::Float, "ctx->physicsGetAngularDamping($r)",  /*writable*/ false },
        } });

    // What a script can know about the mesh, plus the one transform it can meaningfully write. The node's own
    // transform is NOT here: updateTree recomposes it from the entity's world transform every frame, so a write
    // would vanish within the same frame. `offset*` is the per-instance nudge that survives that (updateTree
    // composes world x localTransform), and moving the entity is the other way.
    const DSLType renderType = bindings.registerComponentType("render", "RenderComponent", EComponentID_Render,
        "ctx->entityGetRenderComponent($r)");
    bindings.registerObject({ "render", renderType, /*sidebarTopLevel*/ false,
        {
            // The offset the mesh sits at RELATIVE to its entity -- a held item in a hand, a recoil kick, a
            // breathing scale. Persistent: it is re-composed onto the world transform every update.
            { "setOffsetPos",   T::Void, { { "position", vec3 } }, "ctx->renderSetOffsetPos($r, $1)" },
            { "setOffsetScale", T::Void, { { "scale", T::Float } }, "ctx->renderSetOffsetScale($r, $1)" },
            { "setOffsetRot",   T::Void, { { "rotation", quat } },  "ctx->renderSetOffsetRot($r, $1)" },
        },
        {
            // World-space render bounds -- what "how big is this thing, and where is its middle" asks.
            { "boundsRadius", T::Float, "ctx->renderGetBoundsRadius($r)",     /*writable*/ false },
            { "boundsCenter", vec3,     "ctx->renderGetBoundsCenter($r)",     /*writable*/ false },
            // As of THIS frame's spatial pass: the cheap way to skip work for something off-screen. False when
            // culling is disabled outright (nothing stamps visibility then), so treat it as a hint, not a fact.
            { "visible",      T::Bool,  "(ctx->renderIsVisible($r) != 0)",    /*writable*/ false },
            { "skinned",      T::Bool,  "(ctx->renderIsSkinned($r) != 0)",    /*writable*/ false },
            { "offsetPos",    vec3,     "ctx->renderGetOffsetPos($r)",        /*writable*/ false },
            { "offsetScale",  T::Float, "ctx->renderGetOffsetScale($r)",      /*writable*/ false },
            { "offsetRot",    quat,     "ctx->renderGetOffsetRot($r)",        /*writable*/ false },
        } });

    // A particle effect follows its entity on its own (ParticleComponent::update keeps the emitters on the
    // world transform and feeds them its finite-difference velocity), so there is deliberately no position or
    // velocity here -- move the entity. What is left is the part a script actually decides: emit or not, and
    // when to fire a one-shot.
    const DSLType particleType = bindings.registerComponentType("particle", "ParticleComponent", EComponentID_Particle,
        "ctx->entityGetParticleComponent($r)");
    bindings.registerObject({ "particle", particleType, /*sidebarTopLevel*/ false,
        {
            // One shot of every emitter's authored Burst count. Independent of `emitting`: a burst fires even
            // while continuous emission is paused, which is what makes it usable as a pure event effect.
            { "burst",       T::Void, {},                          "ctx->particleBurst($r)" },
            // Pauses/resumes the CONTINUOUS rates only. Already-live particles keep simulating out.
            { "setEmitting", T::Void, { { "emitting", T::Bool } }, "ctx->particleSetEmitting($r, $1)" },
        },
        {
            { "emitting", T::Bool, "(ctx->particleGetEmitting($r) != 0)", /*writable*/ false },
        } });

    const DSLType audioType = bindings.registerComponentType("audio", "AudioComponent", EComponentID_Audio,
        "ctx->entityGetAudioComponent($r)");
    bindings.registerObject({ "audio", audioType, /*sidebarTopLevel*/ false,
        {
            { "trigger", T::Void, { { "alias", T::String } }, "ctx->audioTrigger($r, self, $1, 0, glm::vec3(0.0f), 1.0f, 1.0f)" },
            { "stop",    T::Void, { { "alias", T::String } }, "ctx->audioStop($r, $1)" },
        },
        {} });

    const DSLType forceType = bindings.registerComponentType("force", "ForceComponent", EComponentID_Force,
        "ctx->entityGetForceComponent($r)");
    // The whole authored shape of a forcefield bubble, plus what the GPU measured about it. Same reads-are-
    // members / writes-are-functions split as every other component here.
    //
    // The bubble spans the "output line" from the emitter to `pos + direction * reach`; every shape control
    // below redistributes the SAME total output along it rather than adding or removing power (the reference is
    // a plain sphere, and the fold is pre-applied engine-side), so pinching or narrowing DENSIFIES the field.
    // `reach` is the one that genuinely scales the total.
    //
    // There is no on/off: Force has no such concept, so silencing an emitter is setOutput(0).
    bindings.registerObject({ "force", forceType, /*sidebarTopLevel*/ false,
        {
            { "setOutput",       T::Void, { { "output", T::Float } },      "ctx->forceSetOutput($r, $1)" },
            // World units, and NOT scaled by the entity -- the span length, so bigger means more total power.
            { "setReach",        T::Void, { { "reach", T::Float } },       "ctx->forceSetReach($r, $1)" },
            // 0.5 is an exact sphere spanning the line; 0 and 1 are exact cones pointed at the emitter and at
            // the target respectively. One "pinch" slider -- reach never changes with it.
            { "setFocus",        T::Void, { { "focus", T::Float } },       "ctx->forceSetFocus($r, $1)" },
            // Where along the line the density sits: 0 = at the emitter end, 1 = at the target end.
            { "setDistribution", T::Void, { { "distribution", T::Float } }, "ctx->forceSetDistribution($r, $1)" },
            // Pure lateral scale: 1 = round, below 1 pinches cones sharper / spheres prolate.
            { "setWidth",        T::Void, { { "width", T::Float } },       "ctx->forceSetWidth($r, $1)" },
            // 0-7. Same-team fields SUM (metaball merging); different teams contest, and the drawn surface is
            // where they balance.
            { "setTeam",         T::Void, { { "team", T::Int } },          "ctx->forceSetTeam($r, $1)" },
            // Both LOCAL to the entity. The emitter sits at the START of the span (offset .. offset + dir*reach).
            { "setDirection",    T::Void, { { "direction", vec3 } },       "ctx->forceSetLocalDirection($r, $1)" },
            { "setOffset",       T::Void, { { "offset", vec3 } },          "ctx->forceSetLocalOffset($r, $1)" },
            // On (the default) pulls the emitter back reach/2 along the axis, so a zero offset centres the
            // bubble on the entity; off means the span STARTS at the entity.
            { "setCentered",     T::Void, { { "centered", T::Bool } },     "ctx->forceSetCentered($r, $1)" },
        },
        {
            // Authored state -- these read back live, exactly what was last set.
            { "output",       T::Float, "ctx->forceGetOutput($r)",              /*writable*/ false },
            { "reach",        T::Float, "ctx->forceGetReach($r)",               /*writable*/ false },
            { "focus",        T::Float, "ctx->forceGetFocus($r)",               /*writable*/ false },
            { "distribution", T::Float, "ctx->forceGetDistribution($r)",        /*writable*/ false },
            { "width",        T::Float, "ctx->forceGetWidth($r)",               /*writable*/ false },
            { "team",         T::Int,   "ctx->forceGetTeam($r)",                /*writable*/ false },
            { "direction",    vec3,     "ctx->forceGetLocalDirection($r)",      /*writable*/ false },
            { "offset",       vec3,     "ctx->forceGetLocalOffset($r)",         /*writable*/ false },
            { "centered",     T::Bool,  "(ctx->forceGetCentered($r) != 0)",     /*writable*/ false },

            // MEASURED state, latched from a GPU readback and therefore about two frames old -- fine for
            // reacting to (knockback, territory), wrong for anything that has to agree with this frame exactly.
            { "appliedForce", vec3,     "ctx->forceGetAppliedForce($r)",        /*writable*/ false },
            { "pressure",     T::Float, "ctx->forceGetPressure($r)",            /*writable*/ false },
        } });

    // One light as a VALUE the DSL can hold, iterate and edit whole: `Light` mirrors OcLight (ScriptAPI.h).
    // Every member is writable ON THE COPY -- what makes an edit land is writing it back through a `ref`
    // binding (below), where the host clamps/normalizes (lightSetAt), so no per-member validation lives here.
    // Angles in degrees; range/width/height/length are world units, NOT scaled by the entity; offset/direction
    // are entity-space. Shape params are each read by ONE type (Spot: coneAngle/edgeSoftness; Area: width/
    // height/rotation; Tube: width = radius, length) and harmlessly stored for the others; the TYPE itself
    // stays authoring-only.
    const DSLType lightStructType = bindings.registerStruct({ "Light", "OcLight", {}, "OcLight()",
        {
            { "enabled",      T::Bool,  "$r.enabled" },
            { "color",        vec3,     "$r.color" },
            { "intensity",    T::Float, "$r.intensity" },
            { "range",        T::Float, "$r.range" },
            { "offset",       vec3,     "$r.offset" },
            { "direction",    vec3,     "$r.direction" },
            { "coneAngle",    T::Float, "$r.coneAngle" },
            { "edgeSoftness", T::Float, "$r.edgeSoftness" },
            { "width",        T::Float, "$r.width" },
            { "height",       T::Float, "$r.height" },
            { "length",       T::Float, "$r.length" },
            { "rotation",     T::Float, "$r.rotation" },
        },
        {
            // Intent-named shape setters: the raw fields are OVERLOADED per light type (width is an area's
            // width but a tube's radius), so these spell out which shape is being authored and set its fields
            // together. They mutate the receiver (the bound copy -- a `ref` binding is what makes it land).
            { "setSpotCone",  T::Void, { { "coneAngleDeg", T::Float }, { "edgeSoftness", T::Float } },
                "(void)($r.coneAngle = $1, $r.edgeSoftness = $2)" },
            { "setAreaSize",  T::Void, { { "width", T::Float }, { "height", T::Float } },
                "(void)($r.width = $1, $r.height = $2)" },
            { "setTubeShape", T::Void, { { "radius", T::Float }, { "length", T::Float } },
                "(void)($r.width = $1, $r.length = $2)" }, // radius rides the width field
            // The tube reading of the overloaded width field, so a script never has to know the aliasing.
            { "tubeRadius",   T::Float, {}, "$r.width" },
        } });

    // The component's light list as a WRITABLE sequence: `foreach [ref] Light l in self.light.lights` and
    // `ifexist [ref] Light l in self.light.lights at <i>` are the whole access surface -- the list never
    // grows or shrinks from a script, so there is no push/clear and iteration is always safe. lightGetAt's
    // return value is the existence check; a non-ref binding is a copy, a `ref` one writes back through
    // lightSetAt at block end.
    const DSLType lightListType = bindings.registerSequenceType("LightList", lightStructType,
        "ctx->lightGetCount($r)", "ocLightAt(ctx, $r, $i)",
        "(ctx->lightGetAt($r, $1, &$v) != 0)", "ctx->lightSetAt($r, $1, &$v)");

    const DSLType lightType = bindings.registerComponentType("light", "LightComponent", EComponentID_Light,
        "ctx->entityGetLightComponent($r)");
    bindings.registerObject({ "light", lightType, /*sidebarTopLevel*/ false,
        {},
        {
            // The collection itself, scene.children-style ("self.light.lights"); the member's value IS the
            // component handle, so the sequence emits run straight against it.
            { "lights", lightListType, "$r", /*writable*/ false },
            { "count",  T::Int, "ctx->lightGetCount($r)", /*writable*/ false },
        } });
    // free functions
    // The result of a radius query, as a collection so it reads like any other ("foreach Entity e in
    // world.entitiesInRadius(...)"). The exposing FUNCTION's own emit RUNS the query and yields a handle to its
    // results, so "$r" here is that handle -- and because `foreach` hoists its source into a local (see the
    // Transpiler), the query runs exactly once per loop while `at` only indexes what it found.
    const DSLType entityQueryType = bindings.registerSequenceType("EntityQuery", T::Entity,
        "ctx->worldQueryCount($r)", "ctx->worldQueryResultAt($r, $i)");

    // A raycast result. Registered as a plain struct so its fields read normally once a hit is proven, and
    // wrapped in an optional so proving it is the only way in: "ifexist RayHit h in world.rayCast(...)". The
    // constructor takes no arguments -- a RayHit is something the engine hands back, never something a script
    // builds -- and every field is read-only for the same reason.
    const DSLType rayHitType = bindings.registerStruct({ "RayHit", "OcRayHit", {}, "OcRayHit()",
        {
            { "point",    vec3,     "$r.point",    /*writable*/ false },
            { "normal",   vec3,     "$r.normal",   /*writable*/ false },
            { "distance", T::Float, "$r.distance", /*writable*/ false },
        },
        {} });
    const DSLType optionalRayHitType = bindings.registerOptionalType("OptionalRayHit", rayHitType,
        "(($v = $r).hit != 0)");

    // Everything mathematical lives under one prefix, so "math." is the single thing to type to see what is
    // available. A NAMESPACE, not an object with state: its functions require a receiver like any binding
    // object's (which is what makes `sin(x)` unauthorable and `math.sin(x)` the only spelling), but none of
    // their emits mention "$r", so the generated C++ is a direct glm/std call with no receiver in sight.
    //
    // ANGLES ARE DEGREES across the whole DSL surface (quatFromEuler, quat.angleDeg, and these) -- the
    // conversion lives in the emit, so a script never handles radians unless it asks for them by name.
    const DSLType mathType = bindings.registerNamespace("math");
    bindings.registerObject({ "math", mathType, /*sidebarTopLevel*/ true,
        {
            // -- basics --
            { "abs",        T::Float, { { "x", T::Float } },                                        "glm::abs($1)" },
            { "sign",       T::Float, { { "x", T::Float } },                                        "glm::sign($1)" },
            { "min",        T::Float, { { "a", T::Float }, { "b", T::Float } },                     "glm::min($1, $2)" },
            { "max",        T::Float, { { "a", T::Float }, { "b", T::Float } },                     "glm::max($1, $2)" },
            { "clamp",      T::Float, { { "x", T::Float }, { "low", T::Float }, { "high", T::Float } }, "glm::clamp($1, $2, $3)" },
            { "saturate",   T::Float, { { "x", T::Float } },                                        "glm::clamp($1, 0.0f, 1.0f)" },
            { "sqrt",       T::Float, { { "x", T::Float } },                                        "glm::sqrt($1)" },
            { "pow",        T::Float, { { "base", T::Float }, { "exponent", T::Float } },           "glm::pow($1, $2)" },
            { "exp",        T::Float, { { "x", T::Float } },                                        "glm::exp($1)" },
            { "log",        T::Float, { { "x", T::Float } },                                        "glm::log($1)" },
            { "log2",       T::Float, { { "x", T::Float } },                                        "glm::log2($1)" },
            // Sign follows the DIVIDEND (C fmod semantics), so math.mod(-1, 4) is -1, not 3.
            { "mod",        T::Float, { { "x", T::Float }, { "y", T::Float } },                     "std::fmod($1, $2)" },

            // -- rounding --
            { "floor",      T::Float, { { "x", T::Float } },                                        "glm::floor($1)" },
            { "ceil",       T::Float, { { "x", T::Float } },                                        "glm::ceil($1)" },
            { "round",      T::Float, { { "x", T::Float } },                                        "glm::round($1)" },
            { "trunc",      T::Float, { { "x", T::Float } },                                        "glm::trunc($1)" },
            { "fract",      T::Float, { { "x", T::Float } },                                        "glm::fract($1)" },
            { "toInt",      T::Int,   { { "x", T::Float } },                                        "((int)($1))" }, // truncates, like C++
            { "toFloat",    T::Float, { { "i", T::Int } },                                          "((float)($1))" },

            // -- interpolation / ranges --
            { "lerp",         T::Float, { { "a", T::Float }, { "b", T::Float }, { "t", T::Float } }, "glm::mix($1, $2, $3)" },
            // 0 when the input range is empty, rather than a division by zero.
            { "inverseLerp",  T::Float, { { "a", T::Float }, { "b", T::Float }, { "value", T::Float } }, "ocInverseLerp($1, $2, $3)" },
            { "remap",        T::Float, { { "value", T::Float }, { "inMin", T::Float }, { "inMax", T::Float }, { "outMin", T::Float }, { "outMax", T::Float } }, "ocRemap($1, $2, $3, $4, $5)" },
            { "step",         T::Float, { { "edge", T::Float }, { "x", T::Float } },                 "glm::step($1, $2)" },
            { "smoothstep",   T::Float, { { "edge0", T::Float }, { "edge1", T::Float }, { "x", T::Float } }, "glm::smoothstep($1, $2, $3)" },
            // Frame-rate independent approach: never overshoots the target.
            { "moveTowards",  T::Float, { { "current", T::Float }, { "target", T::Float }, { "maxDelta", T::Float } }, "ocMoveTowards($1, $2, $3)" },

            // -- trigonometry (degrees in, degrees out) --
            { "sin",        T::Float, { { "angleDeg", T::Float } },                                 "glm::sin(glm::radians($1))" },
            { "cos",        T::Float, { { "angleDeg", T::Float } },                                 "glm::cos(glm::radians($1))" },
            { "tan",        T::Float, { { "angleDeg", T::Float } },                                 "glm::tan(glm::radians($1))" },
            { "asin",       T::Float, { { "x", T::Float } },                                        "glm::degrees(glm::asin(glm::clamp($1, -1.0f, 1.0f)))" },
            { "acos",       T::Float, { { "x", T::Float } },                                        "glm::degrees(glm::acos(glm::clamp($1, -1.0f, 1.0f)))" },
            { "atan",       T::Float, { { "x", T::Float } },                                        "glm::degrees(glm::atan($1))" },
            { "atan2",      T::Float, { { "y", T::Float }, { "x", T::Float } },                     "glm::degrees(glm::atan($1, $2))" },
            // For the rare case a raw radian value is wanted (feeding a shader constant, say).
            { "toRadians",  T::Float, { { "degrees", T::Float } },                                  "glm::radians($1)" },
            { "toDegrees",  T::Float, { { "radians", T::Float } },                                  "glm::degrees($1)" },

            // -- angles -- the wrap-around cases hand-rolled arithmetic gets wrong
            { "wrapAngle",        T::Float, { { "angleDeg", T::Float } },                           "ocWrapAngle($1)" },      // -> [-180, 180]
            { "deltaAngle",       T::Float, { { "fromDeg", T::Float }, { "toDeg", T::Float } },     "ocDeltaAngle($1, $2)" }, // the short way around
            { "lerpAngle",        T::Float, { { "a", T::Float }, { "b", T::Float }, { "t", T::Float } }, "ocLerpAngle($1, $2, $3)" },
            { "moveTowardsAngle", T::Float, { { "currentDeg", T::Float }, { "targetDeg", T::Float }, { "maxDelta", T::Float } }, "ocMoveTowardsAngle($1, $2, $3)" },

            // -- randomness -- one engine-side stream, so a hot-reload does not restart the sequence
            { "randomFloat",      T::Float, { { "min", T::Float }, { "max", T::Float } },           "ctx->randomFloat($1, $2)" },
            { "randomInt",        T::Int,   { { "min", T::Int }, { "max", T::Int } },               "ctx->randomInt($1, $2)" }, // both ends inclusive
            { "randomUnitVector", vec3,     {},                                                     "ocRandomUnitVector(ctx)" },

            // -- rotations -- the constructors that used to sit at top level
            { "quatFromEuler",     quat,   { { "eulerDeg", vec3 } },                                "glm::quat(glm::radians($1))" },
            { "quatFromAxisAngle", quat,   { { "axis", vec3 }, { "angleDeg", T::Float } },          "glm::angleAxis(glm::radians($2), glm::normalize($1))" },
            { "quatIdentity",      quat,   {},                                                      "glm::quat(1.0f, 0.0f, 0.0f, 0.0f)" },
            // The rotation whose -Z faces `forward` (the engine forward axis) with +Y as close to `up` as possible.
            { "quatLookRotation",  quat,   { { "forward", vec3 }, { "up", vec3 } },                 "glm::quatLookAt(glm::normalize($1), glm::normalize($2))" },
        },
        {
            { "pi",      T::Float, "3.14159265358979f", /*writable*/ false },
            { "tau",     T::Float, "6.28318530717959f", /*writable*/ false },
            { "epsilon", T::Float, "1.0e-6f",           /*writable*/ false },
        } });

    // The render camera, as a read-only view onto the per-frame fields ScriptContext already carries (see its
    // `Per-frame data` block -- ScriptContext::update refreshes them once a frame). Every emit here reads a
    // FIELD, not a call, so none of them mention "$r": the object exists purely to group them under a name.
    const DSLType cameraType = bindings.registerNamespace("Camera");
    bindings.registerObject({ "Camera", cameraType, /*sidebarTopLevel*/ false,
        {},
        {
            { "position",  vec3,     "ctx->cameraPosition",  /*writable*/ false },
            { "direction", vec3,     "ctx->cameraDirection", /*writable*/ false }, // normalized, the way it looks
            { "up",        vec3,     "ctx->cameraUp",        /*writable*/ false },
            { "right",     vec3,     "glm::normalize(glm::cross(ctx->cameraDirection, ctx->cameraUp))", /*writable*/ false },
            { "fovDeg",    T::Float, "ctx->cameraFovDeg",    /*writable*/ false },
            { "nearPlane", T::Float, "ctx->cameraNear",      /*writable*/ false },
            { "farPlane",  T::Float, "ctx->cameraFar",       /*writable*/ false },
        } });

    // The ambient state a script ACTS ON rather than owns: spawning, the physics world, the sky, global events.
    // Everything an entity owns stays on self/its components -- the split is "whose state is this", which is
    // also why nothing here takes an entity as a receiver.
    bindings.registerObject({ "world", T::World, /*sidebarTopLevel*/ true,
        {
            // -- entities -- spawn returns an Entity?: a bad path or a failed spawn has no entity to give back,
            // and the DSL has no null, so the caller proves it with `ifexist` like any other optional.
            { "spawn",         optionalEntityType, { { "assetPath", T::String }, { "position", vec3 } }, "ctx->spawnEntity($1, $2)" },
            { "destroy",       T::Void,  { { "entity", T::Entity } },                                    "ctx->destroyEntity($1)" },
            // By display name, ROOTS only -- names are not unique, so this is "the first one", and a
            // whole-scene search would be a different and far more expensive operation.
            { "findRootEntity", optionalEntityType, { { "displayName", T::String } },              "ctx->worldFindRootEntity($1)" },
            // Every entity within the radius, walkable with `foreach`. Nesting is fine -- each query takes its
            // own slot in a thread-local ring (see worldQueryRadius) -- though a handle held across more than
            // that ring's worth of further queries reads as empty. Not offered to `ifexist`: indexing one
            // result of a query you ran for that purpose is what nearestEntity is for.
            { "entitiesInRadius", entityQueryType, { { "position", vec3 }, { "radius", T::Float } }, "ctx->worldQueryRadius($1, $2)" },
            // Nearest OTHER entity within the radius (pass self as `exclude` to skip yourself).
            { "nearestEntity", optionalEntityType, { { "position", vec3 }, { "maxRadius", T::Float }, { "exclude", T::Entity } }, "ctx->spatialGetNearestEntity($1, $2, $3)" },

            // -- raycasts -- the full hit is an optional, so point/normal/distance can only be read once the
            // hit is proven; rayCastDistance is the cheap form when only "how far to the wall" matters (it
            // reports maxDistance itself on a miss, never a sentinel outside the query range).
            { "rayCast",         optionalRayHitType, { { "origin", vec3 }, { "direction", vec3 }, { "maxDistance", T::Float } }, "ctx->worldRayCast($1, $2, $3)" },
            { "rayCastDistance", T::Float, { { "origin", vec3 }, { "direction", vec3 }, { "maxDistance", T::Float } },           "ctx->physicsRayCastDistance($1, $2, $3)" },

            // -- events -- global, or delivered to one entity's own script
            { "sendEvent",   T::Void, { { "eventName", T::String } },                              "ctx->sendEvent($1)" },
            { "sendEventTo", T::Void, { { "entity", T::Entity }, { "eventName", T::String } },     "ctx->sendEventToEntity($1, $2)" },
            // fires locally AND across the network (server -> clients / client -> server + relay); single player = sendEvent
            // `self` rides along so the server's event filter sees which entity fired it
            { "sendNetworkEvent", T::Void, { { "eventName", T::String } },                         "ctx->sendNetworkEventFrom(self, $1)" },
            // who fired the event being handled (0 = server); server-stamped, unforgeable
            { "networkEventSender", T::Int, { },                                                   "int(ctx->networkEventSender())" },

            // -- sky / lighting --
            { "setSun",          T::Void, { { "direction", vec3 }, { "color", vec3 }, { "intensity", T::Float } },                      "ctx->setSun($1, $2, $3)" },
            { "spawnPointLight", T::Void, { { "position", vec3 }, { "range", T::Float }, { "color", vec3 }, { "intensity", T::Float } }, "ctx->spawnPointLight($1, $2, $3, $4)" },

            // -- physics globals --
            { "setGravity",  T::Void, { { "gravity", vec3 } },                                     "ctx->physicsSetGravity($1)" },

            // -- debug drawing -- this frame only, so calling it every Update is how a line stays on screen
            { "drawLine",    T::Void, { { "from", vec3 }, { "to", vec3 }, { "color", vec3 } },     "ctx->worldDrawLine($1, $2, $3)" },
        },
        {
            // Normalized, pointing the way the sunlight TRAVELS (not toward the sun).
            { "sunDirection", vec3,     "ctx->worldGetSunDirection(0)", /*writable*/ false },
            // Seconds since startup -- the same clock deltaSeconds comes from, already on the context.
            { "time",         T::Float, "ctx->elapsedSeconds",          /*writable*/ false },
            { "camera",       cameraType, "ctx",                        /*writable*/ false }, // "$r" unused by its members
        } });

    // The in-game HUD overlay (Core.GameHud): a hotbar the number keys drive plus bars/counters at the
    // top left. GLOBAL state, deliberately not a component -- one screen, one HUD -- so like `world` it
    // lives under its own top-level name. Keys 1..9,0 select slots 0..9 while the hotbar is active
    // (visible AND any slot assigned) and fire the global "Hotbar Select" script event, so a script
    // reacts by subscribing to that event and reading hud.selectedSlot. Bars/counters are keyed by
    // their display name; insertion order is display order; colors are linear 0..1 RGB.
    const DSLType hudType = bindings.registerNamespace("hud");
    bindings.registerObject({ "hud", hudType, /*sidebarTopLevel*/ true,
        {
            // count > 0 draws a Minecraft-style stack number in the slot corner.
            { "setSlot",          T::Void, { { "slot", T::Int }, { "label", T::String }, { "count", T::Int } }, "ctx->hudSetSlot($1, $2, $3)" },
            { "setSlotCount",     T::Void, { { "slot", T::Int }, { "count", T::Int } },                        "ctx->hudSetSlotCount($1, $2)" },
            { "clearSlot",        T::Void, { { "slot", T::Int } },                                             "ctx->hudClearSlot($1)" },
            { "selectSlot",       T::Void, { { "slot", T::Int } },                                             "ctx->hudSelectSlot($1)" },
            // Explicit off-switch: the hotbar only draws (and the number keys only select) while visible
            // AND any slot is assigned, so an empty HUD leaves the testbed number keys untouched.
            { "setHotbarVisible", T::Void, { { "visible", T::Bool } },                                         "ctx->hudSetHotbarVisible($1)" },
            // Creates or updates the named bar; value clamps to 0..maxValue.
            { "setBar",           T::Void, { { "name", T::String }, { "value", T::Float }, { "maxValue", T::Float }, { "color", vec3 } }, "ctx->hudSetBar($1, $2, $3, $4)" },
            { "removeBar",        T::Void, { { "name", T::String } },                                          "ctx->hudRemoveBar($1)" },
            // decimals 0 = integer display.
            { "setCounter",       T::Void, { { "name", T::String }, { "value", T::Float }, { "decimals", T::Int }, { "color", vec3 } }, "ctx->hudSetCounter($1, $2, $3, $4)" },
            { "removeCounter",    T::Void, { { "name", T::String } },                                          "ctx->hudRemoveCounter($1)" },
            // Removes every slot, bar and counter -- what an OnDestroy typically calls.
            { "clear",            T::Void, {},                                                                 "ctx->hudClear(0)" },
        },
        {
            { "selectedSlot", T::Int, "ctx->hudGetSelectedSlot(0)", /*writable*/ false },
        } });

    bindings.registerObject({ nullptr, T::Void, /*sidebarTopLevel*/ false,
        {
            // printf-style formatting: the format string, then any number of values ("$*" expands the tail,
            // comma-prefixed, so a zero-vararg call still emits valid C++). The DSL doesn't check the format
            // against the arguments -- same "author's responsibility" stance the rest of the ABI takes.
            { "printf",          T::Void,  { { "format", T::String } },                                     "ctx->logf($1$*)", /*isPositionalCall*/ false, /*isVariadic*/ true },
            { "isKeyDown",       T::Bool,  { { "keyName", T::String } },                                     "(ctx->isKeyDown($1) != 0)" },
        },
        {} });

    // cppSuffix's trailing "ScriptData* scriptData" is the GENERATED file's own concrete type (declared earlier
    // in that same file) -- the real ABI typedefs (ScriptOnSpawnFn etc., ScriptAPI.h) still take "void*", since
    // they're shared across every script and ScriptData's layout differs per script. The host only ever calls
    // these through a reinterpret_cast to those typedefs, so the two need not textually match -- both are
    // simple pointer-sized parameters, identical at the ABI/calling-convention level.
    bindings.registerEntryPoint({ "OnSpawn",         {},                                            ", ScriptData* scriptData", "REGISTER_ON_SPAWN()" });
    bindings.registerEntryPoint({ "OnDestroy",       {},                                            ", ScriptData* scriptData", "REGISTER_ON_DESTROY()" });
    bindings.registerEntryPoint({ "Update",          { { "deltaSeconds", T::Float } },              ", float deltaSeconds, ScriptData* scriptData", "REGISTER_UPDATE()" });
    bindings.registerEntryPoint({ "OnEvent",         { { "eventIdx", T::Int } },                    ", int eventIdx, ScriptData* scriptData", "REGISTER_ON_EVENT()" });
    bindings.registerEntryPoint({ "OnPhysicsEvent",  { { "begin", T::Int }, { "sensor", T::Int } }, ", Entity* other, int begin, int sensor, long long contactId, ScriptData* scriptData", "REGISTER_ON_PHYSICS_EVENT()" });
}

// ---- engine-owned script arrays (the DSL's `T[]`, see OcArray in ScriptAPI.h) ----------------------------
// Storage lives HERE, not in the script's ScriptData block, which holds only the handle. That's what makes a
// hot-reload free: the block survives (same layout id), so the handles survive, so the contents survive with
// nothing to copy. It's also what keeps a script from ever owning heap memory across a DLL swap.
//
// A handle is {index, generation}: freeing bumps the slot's generation, so a stale or garbage id -- e.g. a
// ScriptData field reinterpreted after a layout change slipped through -- fails lookup and every operation
// degrades to a no-op or a default. That, plus each accessor's own range check, is the memory-safety guarantee: no
// value a script can put in a handle field can make the engine touch memory it doesn't own.
namespace
{
    struct ScriptArray
    {
        std::vector<uint8> bytes;        // elements packed raw; elemSize is fixed at first push
        std::vector<EntityPtr> entities; // Entity elements instead: refcounted, so a destroyed entity reads back
                                          // as null rather than dangling (bytes stays empty for these)
        std::vector<std::string> strings; // string elements: copied engine-side, so they don't point into a
                                           // script DLL's .rdata and dangle when it unloads (bytes stays empty)
        int elemSize = 0;
        int elemKind = 0;                // the DSLType value -- decides which of the three stores is in use
        uint16 generation = 1;           // bumped on free; 0 is never a live generation, so handle 0 is always invalid
        bool live = false;
        ScriptComponent* owner = nullptr;
    };

    // Storage is CHUNKED so it never reallocates: a ScriptArray* returned by resolveScriptArray stays valid
    // while other threads create arrays (a std::vector's growth would dangle every outstanding one). A handle
    // packs its index into 16 bits, so 64 chunks of 1024 cover the entire address space and the chunk table is
    // a fixed, never-moving array. Only allocation and the free list take the lock, both cold (an array is
    // created once per script field, freed when the component dies); the read path is lock-free.
    // Per-array CONTENTS need no synchronisation at all: a handle lives in its owner's ScriptData and the DSL
    // only reaches arrays through `self`, so one array is only ever touched by one entity's script.
    constexpr uint32 kArrayChunkShift = 10;
    constexpr uint32 kArrayChunkSize  = 1u << kArrayChunkShift;
    constexpr uint32 kArrayChunkMask  = kArrayChunkSize - 1;
    constexpr uint32 kMaxScriptArrays = 1u << 16; // the handle's index field
    constexpr uint32 kArrayChunkCount = kMaxScriptArrays / kArrayChunkSize;

    std::atomic<ScriptArray*> g_arrayChunks[kArrayChunkCount] = {};
    std::unique_ptr<ScriptArray[]> g_arrayChunkOwners[kArrayChunkCount]; // ownership only; reads go through the atomics
    std::vector<uint32> g_freeScriptArrays; // guarded by g_scriptArrayMutex
    uint32 g_scriptArrayCount = 0;          // guarded by g_scriptArrayMutex
    std::mutex g_scriptArrayMutex;

    constexpr int kEntityElemKind = static_cast<int>(DSLType::Entity);
    constexpr int kStringElemKind = static_cast<int>(DSLType::String);

    constexpr uint32 makeArrayHandle(uint32 index, uint16 generation) { return (index + 1u) | (uint32(generation) << 16); }
    constexpr uint32 arrayHandleIndex(uint32 handle) { return (handle & 0xFFFFu) - 1u; }
    constexpr uint16 arrayHandleGeneration(uint32 handle) { return uint16(handle >> 16); }

    // The ONLY way a handle becomes a ScriptArray -- everything below goes through it, so a bad id can never
    // reach the stores.
    ScriptArray* resolveScriptArray(uint32 handle)
    {
        if (handle == 0)
            return nullptr;
        const uint32 index = arrayHandleIndex(handle);
        if (index >= kMaxScriptArrays)
            return nullptr; // low 16 bits of 0 underflow the -1; a compare against a constant, not a load
        ScriptArray* chunk = g_arrayChunks[index >> kArrayChunkShift].load(std::memory_order_acquire);
        if (chunk == nullptr)
            return nullptr; // unallocated chunk: the same check the old size() bound was doing
        ScriptArray& array = chunk[index & kArrayChunkMask];
        if (!array.live || array.generation != arrayHandleGeneration(handle))
            return nullptr;
        return &array;
    }

    int scriptArrayCount(const ScriptArray& array)
    {
        if (array.elemKind == kEntityElemKind)
            return static_cast<int>(array.entities.size());
        if (array.elemKind == kStringElemKind)
            return static_cast<int>(array.strings.size());
        return array.elemSize > 0 ? static_cast<int>(array.bytes.size() / array.elemSize) : 0;
    }
}

// Frees every array `owner` created and forgets their handles -- called when the component dies, and when its
// data block is discarded (a layout change), which is the moment those handles stop being reachable.
void releaseScriptArrays(ScriptComponent& owner)
{
    for (uint32 handle : owner.ownedArrays)
        if (ScriptArray* array = resolveScriptArray(handle); array != nullptr)
        {
            array->live = false;
            ++array->generation; // any surviving copy of this handle is now stale, and resolves to nothing
            array->bytes.clear();
            array->bytes.shrink_to_fit();
            array->entities.clear();
            array->strings.clear();
            array->owner = nullptr;
            const std::lock_guard<std::mutex> lock(g_scriptArrayMutex);
            g_freeScriptArrays.push_back(arrayHandleIndex(handle));
        }
    owner.ownedArrays.clear();
}

#pragma warning(push)
#pragma warning(disable: 4190) // for glm types
extern "C" // The thunks have C linkage (external) so the cooked App-Scripts can call them
{
    // ---- string literals ----
    const char* thunk_internString(const char* text)
    {
        if (text == nullptr)
            return "";
        // unordered_set is node-based, so an element's address is stable for the life of the set even across
        // rehashing -- which is exactly what makes the returned pointer safe to keep forever. Never erased:
        // the set only ever grows to the number of distinct literals across all loaded scripts.
        // Scripts tick in parallel and this is emitted for every literal USE, so the hit path (all but the
        // first use of each literal) takes the shared lock and the insert re-checks under the exclusive one.
        static std::unordered_set<std::string> interned;
        static std::shared_mutex mutex;
        {
            const std::shared_lock<std::shared_mutex> read(mutex);
            if (const auto it = interned.find(text); it != interned.end())
                return it->c_str();
        }
        const std::lock_guard<std::shared_mutex> write(mutex);
        return interned.emplace(text).first->c_str();
    }

    // ---- script arrays ----
    void thunk_arrayPush(Entity* owner, OcArray* handle, int elemKind, int elemSize, const void* value)
    {
        if (handle == nullptr || owner == nullptr || elemSize <= 0 || value == nullptr)
            return;
        ScriptArray* array = resolveScriptArray(*handle);
        if (array == nullptr)
        {
            // Either the first push (handle 0, from a zeroed data block) or a handle that no longer resolves;
            // both mean "this field has no array", so make one and write its id back through the field.
            ScriptComponent* component = getComponent<ScriptComponent>(owner);
            if (component == nullptr)
                return;
            uint32 index;
            {
                const std::lock_guard<std::mutex> lock(g_scriptArrayMutex);
                if (!g_freeScriptArrays.empty())
                {
                    index = g_freeScriptArrays.back();
                    g_freeScriptArrays.pop_back();
                }
                else
                {
                    if (g_scriptArrayCount >= kMaxScriptArrays)
                        return; // registry full: the field keeps handle 0 and every access reads as empty
                    index = g_scriptArrayCount++;
                }
                std::atomic<ScriptArray*>& slot = g_arrayChunks[index >> kArrayChunkShift];
                if (slot.load(std::memory_order_relaxed) == nullptr)
                {
                    g_arrayChunkOwners[index >> kArrayChunkShift] = std::make_unique<ScriptArray[]>(kArrayChunkSize);
                    slot.store(g_arrayChunkOwners[index >> kArrayChunkShift].get(), std::memory_order_release);
                }
            }
            array = &g_arrayChunks[index >> kArrayChunkShift].load(std::memory_order_acquire)[index & kArrayChunkMask];
            array->live = true;
            array->elemSize = elemSize;
            array->elemKind = elemKind;
            array->owner = component;
            *handle = makeArrayHandle(index, array->generation);
            component->ownedArrays.push_back(*handle);
        }
        // The caller's bytes are its OWN representation (Entity* / const char* / a POD value); the two non-POD
        // kinds convert into engine-owned storage here rather than being stored as given.
        if (array->elemKind == kEntityElemKind)
            array->entities.emplace_back(*static_cast<Entity* const*>(value));
        else if (array->elemKind == kStringElemKind)
        {
            const char* text = *static_cast<const char* const*>(value);
            array->strings.emplace_back(text != nullptr ? text : "");
        }
        else
        {
            array->bytes.resize(array->bytes.size() + array->elemSize);
            std::memcpy(array->bytes.data() + array->bytes.size() - array->elemSize, value, array->elemSize);
        }
    }

    // A POD element's own address. Entity and String arrays have no raw element (refcounted EntityPtr /
    // engine-owned std::string), so they resolve to null and keep the copying accessors below.
    static void* scriptArrayElemAt(ScriptArray* array, int index, int elemSize)
    {
        if (array == nullptr || index < 0 || elemSize <= 0 || array->elemSize != elemSize
            || array->elemKind == kEntityElemKind || array->elemKind == kStringElemKind
            || index >= scriptArrayCount(*array))
            return nullptr;
        return array->bytes.data() + static_cast<size_t>(index) * elemSize;
    }

    // ENTITY AND STRING elements only -- neither has a raw element the caller could load through (a refcounted
    // EntityPtr, an engine-owned std::string), so these hand out the derived Entity*/const char* instead. POD
    // elements go through arrayElem and are loaded/stored directly by the caller, which is why there is no
    // type-erased copy left in here; a POD array reaching this reads as a miss.
    int thunk_arrayViewGet(void* view, int index, int elemSize, void* outValue)
    {
        ScriptArray* array = static_cast<ScriptArray*>(view);
        if (array == nullptr || outValue == nullptr || index < 0 || index >= scriptArrayCount(*array))
            return 0;
        if (array->elemKind == kEntityElemKind)
        {
            // A refcounted element whose entity has since died reports a MISS, exactly like an out-of-range
            // index -- the DSL has no null Entity to hand back, and `ifexist` is the only way to read one, so
            // "the slot is there but its entity is gone" and "the slot isn't there" are the same answer.
            Entity* entity = array->entities[index].get();
            if (entity == nullptr)
                return 0;
            *static_cast<Entity**>(outValue) = entity;
            return 1;
        }
        if (array->elemKind == kStringElemKind)
        {
            // Points into engine-owned storage, valid until this array is next mutated -- the same contract
            // entityGetName already has.
            *static_cast<const char**>(outValue) = array->strings[index].c_str();
            return 1;
        }
        return 0;
    }

    void thunk_arrayViewSet(void* view, int index, int elemSize, const void* value)
    {
        ScriptArray* array = static_cast<ScriptArray*>(view);
        if (array == nullptr || value == nullptr || index < 0 || index >= scriptArrayCount(*array))
            return;
        if (array->elemKind == kEntityElemKind)
            array->entities[index] = EntityPtr(*static_cast<Entity* const*>(value));
        else if (array->elemKind == kStringElemKind)
        {
            const char* text = *static_cast<const char* const*>(value);
            array->strings[index] = text != nullptr ? text : "";
        }
    }

    // The handle forms are the view forms plus a resolve -- one implementation, not two.
    int thunk_arrayGet(OcArray handle, int index, int elemSize, void* outValue)
    {
        return thunk_arrayViewGet(resolveScriptArray(handle), index, elemSize, outValue);
    }

    void thunk_arraySet(OcArray handle, int index, int elemSize, const void* value)
    {
        thunk_arrayViewSet(resolveScriptArray(handle), index, elemSize, value);
    }

    // POD element storage, for a foreach to index directly. Refuses anything whose elements are not raw bytes
    // of exactly this size, so a mismatched T reads as an empty array rather than reinterpreting the storage.
    OcArraySpan thunk_arraySpan(OcArray handle, int elemSize)
    {
        const ScriptArray* array = resolveScriptArray(handle);
        if (array == nullptr || array->elemKind == kEntityElemKind || array->elemKind == kStringElemKind
            || array->elemSize != elemSize || elemSize <= 0 || array->bytes.empty())
            return OcArraySpan{ nullptr, 0 };
        return OcArraySpan{ const_cast<uint8*>(array->bytes.data()), static_cast<int>(array->bytes.size() / elemSize) };
    }

    void* thunk_arrayElem(OcArray handle, int index, int elemSize)
    {
        return scriptArrayElemAt(resolveScriptArray(handle), index, elemSize);
    }

    void* thunk_arrayViewElem(void* view, int index, int elemSize)
    {
        return scriptArrayElemAt(static_cast<ScriptArray*>(view), index, elemSize);
    }

    // The array itself, resolved once for a loop. Safe to hold across the body: the registry's chunked storage
    // never moves a ScriptArray, and a freed slot stays valid memory that reads back empty.
    void* thunk_arrayResolve(OcArray handle) { return resolveScriptArray(handle); }

    int thunk_arrayViewCount(void* view)
    {
        const ScriptArray* array = static_cast<const ScriptArray*>(view);
        return array != nullptr ? scriptArrayCount(*array) : 0;
    }

    void thunk_arrayRemoveAt(OcArray handle, int index)
    {
        ScriptArray* array = resolveScriptArray(handle);
        if (array == nullptr || index < 0 || index >= scriptArrayCount(*array))
            return;
        if (array->elemKind == kEntityElemKind)
            array->entities.erase(array->entities.begin() + index);
        else if (array->elemKind == kStringElemKind)
            array->strings.erase(array->strings.begin() + index);
        else
        {
            const auto first = array->bytes.begin() + static_cast<size_t>(index) * array->elemSize;
            array->bytes.erase(first, first + array->elemSize);
        }
    }

    int thunk_arrayCount(OcArray handle)
    {
        const ScriptArray* array = resolveScriptArray(handle);
        return array != nullptr ? scriptArrayCount(*array) : 0;
    }

    void thunk_arrayClear(OcArray handle)
    {
        if (ScriptArray* array = resolveScriptArray(handle); array != nullptr)
        {
            array->bytes.clear();
            array->entities.clear();
            array->strings.clear();
        }
    }

    // ---- context thunks -----------------------------------------------------
    void thunk_log(const char* message) { Log::info(message ? message : ""); }
    void thunk_logf(const char* message, ...) 
    {
        va_list args;
        va_start(args, message);
        int size_s = std::snprintf(nullptr, 0, message, args) + 1;
		if (size_s <= 0) { va_end(args); return; }
		size_t size = static_cast<size_t>(size_s);
		std::string formattedString(size, '\0');
		std::vsnprintf(&formattedString[0], size, message, args);
        Log::info(formattedString);
        va_end(args);
    }

    void thunk_vlogf(const char* fmt, va_list ap)
    {
        if (!fmt) return;
        va_list ap2; va_copy(ap2, ap);
        const int n = std::vsnprintf(nullptr, 0, fmt, ap2); va_end(ap2);
        if (n < 0) return;
        std::string s(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(&s[0], s.size(), fmt, ap);
        s.resize(static_cast<size_t>(n));
        Log::info(s);
    }

    int thunk_isKeyDown(const char* keyName)
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        return (keys && keyName) ? (keys[scancodeFromName(keyName)] ? 1 : 0) : 0;
    }

    void thunk_spawnPointLight(glm::vec3 position, float range, glm::vec3 color, float intensity)
    {
        if (!Globals::rendererVK.isInitialized()) // headless server: no mapped light buffers to write
            return;
        Globals::rendererVK.addPointLight(PointLight(position, range, color, intensity));
    }

    void thunk_setSun(glm::vec3 direction, glm::vec3 color, float intensity)
    {
        if (!Globals::rendererVK.isInitialized())
            return;
        Globals::rendererVK.setSunLight(direction, color, intensity);
    }

    // Queued for the main thread, so there is no entity to hand back yet -- the DSL already types this as
    // `Entity?` and a script reads it through `ifexist`, which simply takes the miss branch.
    Entity* thunk_spawnEntity(const char* assetPath, glm::vec3 position)
    {
        if (!assetPath) return nullptr;
        Globals::scriptEvents.addSpawnRequest(assetPath, position);
        return nullptr;
    }

    void thunk_destroyEntity(Entity* e)
    {
        Globals::scriptEvents.addDestroyRequest(EntityPtr(e));
    }

    const char* thunk_entityGetName(Entity* e) { return e->getName(); }

    int thunk_entityGetEnabled(Entity* en) { return en->isEnabled() ? 1 : 0; }
    int thunk_entityGetChildCount(Entity* en)
    {
        if (SceneComponent* sc = getComponent<SceneComponent>(en))
			return (int)sc->children.size();
        return 0;
    }
    float thunk_entityGetBoundsRadius(Entity* en)
    {
		if (RenderComponent* rc = getComponent<RenderComponent>(en))
			return rc->node.getWorldBounds().radius;
        return 0.0f;
    }

    void thunk_entitySetEnabled(Entity* en, int enabled)              { en->setEnabled(enabled != 0); }
    void thunk_entitySetAnimFloat(Entity* en, const char* p, float v) { if (AnimatorComponent* ac = getComponent<AnimatorComponent>(en)) ac->stateMachine.setFloat(p, v); }
    void thunk_entitySetAnimBool(Entity * en, const char* p, int v)   { if (AnimatorComponent* ac = getComponent<AnimatorComponent>(en)) ac->stateMachine.setBool(p, v != 0); }
    void thunk_entitySetAnimTrigger(Entity* en, const char* p)        { if (AnimatorComponent* ac = getComponent<AnimatorComponent>(en)) ac->stateMachine.setTrigger(p); }

    // ---- physics thunks ----
    PhysicsComponent* physicsOf(Entity* en)
    {
        if (!en)
            return nullptr;
        PhysicsComponent* pc = getComponent<PhysicsComponent>(en);
        return (pc->body.isValid()) ? pc : nullptr;
    }

    // Scripts tick on WORKER threads during the parallel entity pass, and every box3d write can
    // wake a body (mutating the world's shared solver sets - main-thread-only). So ALL box3d
    // writes below queue through PhysicsWorld's command queue, applied at the start of the next
    // physics.update() - one frame of latency, like teleport; reads meanwhile see pre-write values.
    void thunk_physicsSetGravity(glm::vec3 gravity) { Globals::physics.queueSetGravity(gravity); }

    int thunk_physicsRayCast(glm::vec3 origin, glm::vec3 translation, glm::vec3* outPoint, glm::vec3* outNormal, float* outFraction)
    {
        const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(origin, translation);
        if (outPoint)    *outPoint = hit.point;
        if (outNormal)   *outNormal = hit.normal;
        if (outFraction) *outFraction = hit.fraction;
        return hit.hit ? 1 : 0;
    }

    float thunk_physicsRayCastDistance(glm::vec3 origin, glm::vec3 dir, float maxDist)
    {
        const glm::vec3 translation = glm::normalize(dir) * maxDist;
        const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(origin, translation);
        return hit.hit ? hit.fraction * maxDist : maxDist;
    }

    int thunk_physicsContactGetPoint(long long contactId, glm::vec3* outPoint, glm::vec3* outNormal)
    {
        glm::vec3 point(0.0f), normal(0.0f);
        if (!Globals::physics.getContactPoint(contactId, point, normal))
            return 0;
        if (outPoint)  *outPoint = point;
        if (outNormal) *outNormal = normal;
        return 1;
    }

    int thunk_spatialQueryRadius(glm::vec3 position, float radius, Entity** outEntities, int maxOut)
    {
        if (!outEntities || maxOut <= 0)
            return 0;
        thread_local std::vector<uint64> results;
        Globals::spatialIndex.querySphere(glm::dvec3(position), radius, SpatialLayer_Render, results);
        const int count = glm::min(int(results.size()), maxOut);
        for (int i = 0; i < count; ++i)
            outEntities[i] = reinterpret_cast<Entity*>(results[i]);
        return count;
    }

    // ---- radius query results ----
    // A THREAD-LOCAL ring of result buffers, handed out by generation-tagged handle (the OcArray discipline).
    // Thread-local because a query's results belong to whoever asked -- scripts run on the main thread today,
    // but nothing here needs to know that. A ring because queries NEST: a foreach over one may run another in
    // its body, and each needs its own live buffer. Recycling a slot bumps its generation, so a handle held
    // across more than kQueryRingSize further queries reads as EMPTY rather than as the results that replaced
    // its own -- the loop ends early instead of walking someone else's entities.
    constexpr uint32 kQueryRingSize = 8;

    struct ScriptQueryFrame
    {
        std::vector<uint64> results;
        uint32 generation = 0;
    };

    // One ring per thread. Handles pack slot + generation into a positive int: `generation * ringSize + slot`,
    // with generations starting at 1, so the first handle any slot hands out is >= ringSize and 0 is never valid
    // (a zeroed/absent handle therefore reads as empty).
    ScriptQueryFrame* queryRing()
    {
        thread_local ScriptQueryFrame frames[kQueryRingSize];
        return frames;
    }

    ScriptQueryFrame* queryFrameFor(int handle)
    {
        if (handle <= 0)
            return nullptr;
        ScriptQueryFrame& frame = queryRing()[static_cast<uint32>(handle) % kQueryRingSize];
        return (frame.generation == static_cast<uint32>(handle) / kQueryRingSize) ? &frame : nullptr;
    }

    int thunk_worldQueryRadius(glm::vec3 position, float radius)
    {
        thread_local uint32 nextSlot = 0;
        const uint32 slot = nextSlot++ % kQueryRingSize;
        ScriptQueryFrame& frame = queryRing()[slot];
        ++frame.generation; // retires any handle still pointing at this slot
        frame.results.clear();
        if (radius > 0.0f)
            Globals::spatialIndex.querySphere(glm::dvec3(position), radius, SpatialLayer_Render, frame.results);
        return static_cast<int>(frame.generation * kQueryRingSize + slot);
    }

    int thunk_worldQueryCount(int handle)
    {
        const ScriptQueryFrame* frame = queryFrameFor(handle);
        return frame != nullptr ? static_cast<int>(frame->results.size()) : 0;
    }

    Entity* thunk_worldQueryResultAt(int handle, int index)
    {
        const ScriptQueryFrame* frame = queryFrameFor(handle);
        if (frame == nullptr || index < 0 || index >= static_cast<int>(frame->results.size()))
            return nullptr;
        return reinterpret_cast<Entity*>(frame->results[index]);
    }

    Entity* thunk_spatialGetNearestEntity(glm::vec3 position, float maxRadius, Entity* exclude)
    {
        return reinterpret_cast<Entity*>(Globals::spatialIndex.queryNearest(
            glm::dvec3(position), maxRadius, SpatialLayer_Render, reinterpret_cast<uint64>(exclude)));
    }

    void thunk_sendEvent(const char* eventName)
    {
        if (eventName)
            Globals::scriptEvents.fireEvent(eventName); // broadcast to every listener of this event
    }
    void thunk_sendNetworkEvent(const char* eventName)
    {
        // worker-safe: the local fire matches thunk_sendEvent, the network half only queues (mutex-guarded,
        // drained by NetworkManager::send on the main thread — NetHost itself is single-threaded)
        if (eventName)
            Globals::networkManager.fireNetworkEvent(eventName);
    }
    void thunk_sendNetworkEventFrom(Entity* sender, const char* eventName)
    {
        if (eventName)
            Globals::networkManager.fireNetworkEvent(eventName, {}, sender);
    }
    unsigned int thunk_networkEventSender()
    {
        return Globals::networkManager.currentEventSender();
    }
    void thunk_sendEventToEntity(Entity* en, const char* eventName)
    {
        if (!en || !eventName)
            return;
        if (ScriptComponent* sc = getComponent<ScriptComponent>(en))
            sc->fireEvent(*en, eventName); // targeted: only this entity's script
    }

    // ---- force field ----
    void* thunk_entityGetForceComponent(Entity* en) { return en ? getComponent<ForceComponent>(en) : nullptr; }
    ForceComponent* asForce(void* p) { return static_cast<ForceComponent*>(p); }

    float thunk_forceGetOutput(void* p)       { return asForce(p)->emitter.getOutput(); }
    float thunk_forceGetReach(void* p)        { return asForce(p)->emitter.getReach(); }
    float thunk_forceGetFocus(void* p)        { return asForce(p)->emitter.getFocus(); }
    float thunk_forceGetDistribution(void* p) { return asForce(p)->emitter.getDistribution(); }
    float thunk_forceGetWidth(void* p)        { return asForce(p)->emitter.getWidth(); }
    int   thunk_forceGetTeam(void* p)         { return int(asForce(p)->emitter.getTeam()); }
    glm::vec3 thunk_forceGetAppliedForce(void* p) { return asForce(p)->emitter.getAppliedForce(); }
    float thunk_forceGetPressure(void* p)     { return asForce(p)->emitter.getPressure(); }
    glm::vec3 thunk_forceGetLocalDirection(void* p) { return asForce(p)->localDirection; }
    glm::vec3 thunk_forceGetLocalOffset(void* p)    { return asForce(p)->localOffset; }
    int   thunk_forceGetCentered(void* p)           { return asForce(p)->centered ? 1 : 0; }

    void thunk_forceSetOutput(void* p, float v)       { asForce(p)->emitter.setOutput(v); }
    void thunk_forceSetReach(void* p, float v)        { asForce(p)->emitter.setReach(v); }
    void thunk_forceSetFocus(void* p, float v)        { asForce(p)->emitter.setFocus(v); }
    void thunk_forceSetDistribution(void* p, float v) { asForce(p)->emitter.setDistribution(v); }
    void thunk_forceSetWidth(void* p, float v)        { asForce(p)->emitter.setWidth(v); }
    void thunk_forceSetTeam(void* p, int v)           { asForce(p)->emitter.setTeam(uint32(glm::max(v, 0))); }
    void thunk_forceSetLocalOffset(void* p, glm::vec3 v) { asForce(p)->localOffset = v; }
    void thunk_forceSetCentered(void* p, int v)       { asForce(p)->centered = (v != 0); }

    void thunk_forceSetLocalDirection(void* p, glm::vec3 v)
    {
        // Match spawn: store a unit axis so the per-frame follow doesn't scale the field direction.
        const float len2 = glm::dot(v, v);
        asForce(p)->localDirection = len2 > 1e-12f ? v * glm::inversesqrt(len2) : glm::vec3(0.0f, 0.0f, -1.0f);
    }

    // ---- light component ----
    // Per-index access into the component's light list; find() bounds-checks, so a bad index reads type
    // defaults and writes nothing. Writes go straight onto the live descs -- the whole list re-uploads to
    // the renderer every update, so there is nothing else to poke.
    void* thunk_entityGetLightComponent(Entity* en) { return en ? getComponent<LightComponent>(en) : nullptr; }
    LightComponent::LightDesc* asLightDesc(void* p, int index)
    {
        LightComponent* lc = static_cast<LightComponent*>(p);
        return index >= 0 ? lc->find(size_t(index)) : nullptr;
    }

    int thunk_lightGetCount(void* p) { return int(static_cast<LightComponent*>(p)->lights.size()); }

    // lightSetAt is the ONE write path (the DSL's `ref` write-back, the Set Light node's copy-in), so every
    // clamp lives there and nowhere else.
    int thunk_lightGetAt(void* p, int i, OcLight* out)
    {
        LightComponent::LightDesc* d = asLightDesc(p, i);
        if (!d || !out)
            return 0;
        out->color = d->color;
        out->intensity = d->intensity;
        out->range = d->range;
        out->offset = d->offset;
        out->direction = d->direction;
        out->coneAngle = d->coneAngle;
        out->edgeSoftness = d->edgeSoftness;
        out->width = d->width;
        out->height = d->height;
        out->length = d->length;
        out->rotation = d->rotation;
        out->enabled = d->enabled;
        return 1;
    }
    void thunk_lightSetAt(void* p, int i, const OcLight* v)
    {
        LightComponent::LightDesc* d = asLightDesc(p, i);
        if (!d || !v)
            return;
        d->color = glm::max(v->color, glm::vec3(0.0f));
        d->intensity = glm::max(v->intensity, 0.0f);
        d->range = glm::max(v->range, 0.0f);
        d->offset = v->offset;
        const float len2 = glm::dot(v->direction, v->direction);
        d->direction = len2 > 1e-12f ? v->direction * glm::inversesqrt(len2) : glm::vec3(0.0f, 0.0f, -1.0f);
        d->coneAngle = glm::clamp(v->coneAngle, 0.0f, 89.0f);
        d->edgeSoftness = glm::clamp(v->edgeSoftness, 0.001f, 1.0f);
        d->width = glm::max(v->width, 0.001f);
        d->height = glm::max(v->height, 0.001f);
        d->length = glm::max(v->length, 0.001f);
        d->rotation = v->rotation;
        d->enabled = v->enabled;
    }

    // ---- scene component ----
    SceneComponent* asScene(void* p) { return static_cast<SceneComponent*>(p); }
    void* thunk_entityGetSceneComponent(Entity* en) { return en ? getComponent<SceneComponent>(en) : nullptr; }

    int thunk_sceneGetChildCount(void* p) { return (int)asScene(p)->children.size(); }
    Entity* thunk_sceneFindChild(void* p, const char* name)
    {
        SceneComponent* sc = asScene(p);
        if (!name) return nullptr;
        for (const EntityPtr& child : sc->children)
            if (std::string_view(child->getName()) == name)
                return child.get();
        return nullptr;
    }
    Entity* thunk_sceneGetChildAt(void* p, int index)
    {
        SceneComponent* sc = asScene(p);
        if (index >= 0 && index < (int)sc->children.size())
            return sc->children[index].get();
        return nullptr;
    }

    void thunk_sceneAddChild(void* p, Entity* child)
    {
        SceneComponent* sc = asScene(p);
        if (!child) return;
        Entity* parent = sc->getEntity();
        if (child->parent != parent) return;
        Globals::scriptEvents.addReparentRequest(EntityPtr(child), EntityPtr(parent));
    }

    void thunk_sceneRemoveChild(void* p, Entity* child)
    {
        SceneComponent* sc = asScene(p);
        if (!child) return;
        Entity* parent = sc->getEntity();
        if (child->parent != parent) return;
        Globals::scriptEvents.addReparentRequest(EntityPtr(child), EntityPtr(nullptr));
    }

    void thunk_sceneRemoveChildAt(void* p, int index)
    {
        thunk_sceneRemoveChild(p, thunk_sceneGetChildAt(p, index));
    }

    // ---- physics component ----
    void* thunk_entityGetPhysicsComponent(Entity* en) { return physicsOf(en); }

    int thunk_physicsIsAwake(void* p) 
    { 
        return static_cast<PhysicsComponent*>(p)->body.isAwake() ? 1 : 0;
    }

    glm::vec3 thunk_physicsGetVelocity(void* p) 
    { 
        return static_cast<PhysicsComponent*>(p)->body.getLinearVelocity(); 
    }

    void thunk_physicsSetVelocity(void* p, glm::vec3 v)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, v);
    }

    void thunk_physicsApplyImpulse(void* p, glm::vec3 v)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse, v);
    }

    void thunk_physicsTeleport(void* p, glm::vec3 position, glm::vec3 eulerDeg)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        Globals::physics.teleportBody(pc->body, position, glm::quat(glm::radians(eulerDeg)));
    }

    // Every physics-component thunk below guards a body that was never created or has already been destroyed.
    // Either way the call degrades to a no-op or a zero, so gameplay code never has to check -- the same
    // contract the rest of the ABI keeps.
    glm::vec3 thunk_physicsGetAngularVelocity(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getAngularVelocity() : glm::vec3(0.0f);
    }

    void thunk_physicsSetAngularVelocity(void* p, glm::vec3 radiansPerSecond)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, radiansPerSecond);
    }

    void thunk_physicsApplyImpulseAtPoint(void* p, glm::vec3 impulse, glm::vec3 worldPoint)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulseAtPoint, impulse, worldPoint);
    }

    void thunk_physicsApplyAngularImpulse(void* p, glm::vec3 impulse)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyAngularImpulse, impulse);
    }

    void thunk_physicsApplyForce(void* p, glm::vec3 force)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyForce, force);
    }

    void thunk_physicsApplyForceAtPoint(void* p, glm::vec3 force, glm::vec3 worldPoint)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyForceAtPoint, force, worldPoint);
    }

    void thunk_physicsApplyTorque(void* p, glm::vec3 torque)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyTorque, torque);
    }

    float thunk_physicsGetMass(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getMass() : 0.0f;
    }

    glm::vec3 thunk_physicsGetCenterOfMass(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getCenterOfMass() : glm::vec3(0.0f);
    }

    glm::vec3 thunk_physicsGetPointVelocity(void* p, glm::vec3 worldPoint)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getPointVelocity(worldPoint) : glm::vec3(0.0f);
    }

    glm::vec3 thunk_physicsGetPosition(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getPosition() : glm::vec3(0.0f);
    }

    float thunk_physicsGetGravityScale(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getGravityScale() : 0.0f;
    }

    void thunk_physicsSetGravityScale(void* p, float scale)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetGravityScale, glm::vec3(scale, 0.0f, 0.0f));
    }

    float thunk_physicsGetLinearDamping(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getLinearDamping() : 0.0f;
    }

    void thunk_physicsSetLinearDamping(void* p, float damping)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearDamping, glm::vec3(damping, 0.0f, 0.0f));
    }

    float thunk_physicsGetAngularDamping(void* p)
    {
        PhysicsComponent* pc = static_cast<PhysicsComponent*>(p);
        return (pc->body.isValid()) ? pc->body.getAngularDamping() : 0.0f;
    }

    void thunk_physicsSetAngularDamping(void* p, float damping)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetAngularDamping, glm::vec3(damping, 0.0f, 0.0f));
    }

    void thunk_physicsSetAwake(void* p, int awake)
    {
        if (PhysicsComponent* pc = static_cast<PhysicsComponent*>(p); pc->body.isValid())
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetAwake, glm::vec3(awake != 0 ? 1.0f : 0.0f, 0.0f, 0.0f));
    }

    // ---- render component ----
    // Reads go through the RenderNode; the offset is the component's own localTransform, which updateTree
    // composes onto the entity's world transform every frame -- so writing it here is picked up next update
    // rather than being overwritten, which is exactly why the node transform itself isn't exposed.
    void* thunk_entityGetRenderComponent(Entity* en) { return en ? getComponent<RenderComponent>(en) : nullptr; }

    float thunk_renderGetBoundsRadius(void* p)
    {
        const RenderComponent* rc = static_cast<const RenderComponent*>(p);
        return rc->node.isValid() ? rc->node.getWorldBounds().radius : 0.0f;
    }

    glm::vec3 thunk_renderGetBoundsCenter(void* p)
    {
        const RenderComponent* rc = static_cast<const RenderComponent*>(p);
        return rc->node.isValid() ? rc->node.getWorldBounds().pos : glm::vec3(0.0f);
    }

    int thunk_renderIsVisible(void* p)
    {
        const RenderComponent* rc = static_cast<const RenderComponent*>(p);
        if (!rc->spatialEntry.isValid())
            return 0;
        return Globals::spatialIndex.isVisible(rc->spatialEntry.handle()) ? 1 : 0;
    }

    int thunk_renderIsSkinned(void* p)
    {
        const RenderComponent* rc = static_cast<const RenderComponent*>(p);
        return (rc->node.isValid() && rc->node.isSkinned()) ? 1 : 0;
    }

    glm::vec3 thunk_renderGetOffsetPos(void* p)
    {
        return static_cast<const RenderComponent*>(p)->localTransform.pos;
    }

    float thunk_renderGetOffsetScale(void* p)
    {
        return static_cast<const RenderComponent*>(p)->localTransform.scale;
    }

    glm::quat thunk_renderGetOffsetRot(void* p)
    {
        return static_cast<const RenderComponent*>(p)->localTransform.quat;
    }

    void thunk_renderSetOffsetPos(void* p, glm::vec3 position)
    {
        static_cast<RenderComponent*>(p)->localTransform.pos = position;
    }

    void thunk_renderSetOffsetScale(void* p, float scale)
    {
        static_cast<RenderComponent*>(p)->localTransform.scale = scale;
    }

    void thunk_renderSetOffsetRot(void* p, glm::quat rotation)
    {
        static_cast<RenderComponent*>(p)->localTransform.quat = glm::normalize(rotation);
    }

    // ---- particle component ----
    void* thunk_entityGetParticleComponent(Entity* en) { return en ? getComponent<ParticleComponent>(en) : nullptr; }

    void thunk_particleBurst(void* p)
    {
        if (ParticleComponent* pc = static_cast<ParticleComponent*>(p); pc->effect.isValid())
            pc->effect.burst();
    }

    int thunk_particleGetEmitting(void* p)
    {
        return static_cast<const ParticleComponent*>(p)->effect.isEmitting() ? 1 : 0;
    }

    void thunk_particleSetEmitting(void* p, int emitting)
    {
        if (ParticleComponent* pc = static_cast<ParticleComponent*>(p); pc->effect.isValid())
            pc->effect.setEmitting(emitting != 0);
    }

    // ---- audio component ----
    void* thunk_entityGetAudioComponent(Entity* en) { return en ? getComponent<AudioComponent>(en) : nullptr; }

    void thunk_audioTrigger(void* p, Entity* en, const char* alias, int overrideMask, glm::vec3 position, float volume, float pitch)
    {
        AudioComponent* ac = static_cast<AudioComponent*>(p);
        if (!en || !alias)
            return;
        AudioComponent::TriggerOverrides overrides;
        if (overrideMask & 1) overrides.position = position;
        if (overrideMask & 2) overrides.volume = volume;
        if (overrideMask & 4) overrides.pitch = pitch;
        ac->trigger(*en, alias, overrides);
    }

    void thunk_audioStop(void* p, const char* alias)
    {
        static_cast<AudioComponent*>(p)->stopSound(alias ? alias : "");
    }

    // ---- animator component ----
    // Everything routes through the state machine, which is the animator's gameplay interface: a script sets
    // parameters and the graph decides what plays. Reads of a never-set parameter return the type's zero (the
    // state machine's own contract), so there is no "does this parameter exist" question to answer.
    AnimStateMachine* animatorMachineOf(void* p)
    {
        AnimatorComponent* ac = static_cast<AnimatorComponent*>(p);
        return ac->hasStateMachine ? &ac->stateMachine : nullptr;
    }

    void* thunk_entityGetAnimatorComponent(Entity* en) { return en ? getComponent<AnimatorComponent>(en) : nullptr; }

    void thunk_animatorSetFloat(void* p, const char* name, float value)
    {
        if (AnimStateMachine* sm = animatorMachineOf(p); sm != nullptr && name != nullptr)
            sm->setFloat(name, value);
    }

    void thunk_animatorSetBool(void* p, const char* name, int value)
    {
        if (AnimStateMachine* sm = animatorMachineOf(p); sm != nullptr && name != nullptr)
            sm->setBool(name, value != 0);
    }

    void thunk_animatorSetTrigger(void* p, const char* name)
    {
        if (AnimStateMachine* sm = animatorMachineOf(p); sm != nullptr && name != nullptr)
            sm->setTrigger(name);
    }

    float thunk_animatorGetFloat(void* p, const char* name)
    {
        AnimStateMachine* sm = animatorMachineOf(p);
        return (sm != nullptr && name != nullptr) ? sm->getFloat(name) : 0.0f;
    }

    int thunk_animatorGetBool(void* p, const char* name)
    {
        AnimStateMachine* sm = animatorMachineOf(p);
        return (sm != nullptr && name != nullptr && sm->getBool(name)) ? 1 : 0;
    }

    const char* thunk_animatorGetStateName(void* p)
    {
        // Points into the state machine's own storage -- valid until the graph is rebuilt, the same lifetime
        // entityGetName's result has (see internString's comment in ScriptAPI.h: engine strings aren't interned).
        AnimStateMachine* sm = animatorMachineOf(p);
        return sm != nullptr ? sm->getCurrentStateName().c_str() : "";
    }

    float thunk_animatorGetTimeInState(void* p)
    {
        AnimStateMachine* sm = animatorMachineOf(p);
        return sm != nullptr ? sm->getTimeInState() : 0.0f;
    }

    float thunk_animatorGetSpeed(void* p)
    {
        return static_cast<const AnimatorComponent*>(p)->resolvePlaybackSpeed();
    }

    int thunk_animatorGetEnabled(void* p)
    {
        return static_cast<const AnimatorComponent*>(p)->enabled ? 1 : 0;
    }

    void thunk_animatorSetEnabled(void* p, int enabled)
    {
        static_cast<AnimatorComponent*>(p)->enabled = enabled != 0;
    }

    // ---- randomness ----
    // Lives HERE rather than in a script DLL: a DLL-local generator would reset its state on each hot-reload,
    // restarting the sequence mid-run. ONE GENERATOR PER THREAD -- scripts tick in parallel, and a shared
    // std::mt19937 is a data race. Seeds come off a fixed base so a run is still reproducible per thread;
    // the interleaving across threads is not, which parallel script scheduling had already given up.
    std::mt19937& scriptRng()
    {
        static std::atomic<uint32> nextSeed{ 0x5eed5eedu };
        thread_local std::mt19937 rng(nextSeed.fetch_add(0x9e3779b9u, std::memory_order_relaxed));
        return rng;
    }

    float thunk_randomFloat(float minValue, float maxValue)
    {
        if (!(minValue < maxValue))
            return minValue; // empty or inverted range -- one deterministic answer beats an assert in gameplay code
        return std::uniform_real_distribution<float>(minValue, maxValue)(scriptRng());
    }

    // ---- world ----
    OcRayHit thunk_worldRayCast(glm::vec3 origin, glm::vec3 direction, float maxDistance)
    {
        OcRayHit out{};
        const float len = glm::length(direction);
        if (len <= 0.0f || maxDistance <= 0.0f)
            return out; // a degenerate ray misses rather than asserting -- gameplay code shouldn't have to guard
        const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(origin, (direction / len) * maxDistance);
        if (!hit.hit)
            return out;
        out.hit = 1;
        out.point = hit.point;
        out.normal = hit.normal;
        out.distance = hit.fraction * maxDistance;
        return out;
    }

    glm::vec3 thunk_worldGetSunDirection(int) { return Globals::rendererVK.getSkyParams().sunDirection; }

    Entity* thunk_worldFindRootEntity(const char* displayName)
    {
        if (!displayName) return nullptr;
        // Linear over the root list: it is short (a scene's top level), and there is no name index to keep in
        // sync. Unnamed entities have a null name, which never matches.
        for (const EntityPtr& root : Globals::world.rootEntities())
            if (const char* name = root->getName(); name != nullptr && std::string_view(name) == displayName)
                return root.get();
        return nullptr;
    }

    void thunk_worldDrawLine(glm::vec3 from, glm::vec3 to, glm::vec3 color)
    {
        if (!Globals::rendererVK.isInitialized()) // headless server: the debug-line PerWorker staging doesn't exist
            return;
        // The DSL speaks in linear 0..1 colours like everything else here; the renderer wants packed 0xAABBGGRR.
        const auto channel = [](float v) { return static_cast<uint32>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        Globals::rendererVK.addDebugLine(from, to,
            0xFF000000u | (channel(color.z) << 16) | (channel(color.y) << 8) | channel(color.x));
    }

    int thunk_randomInt(int minValue, int maxValue)
    {
        if (minValue > maxValue)
            return minValue;
        return std::uniform_int_distribution<int>(minValue, maxValue)(scriptRng()); // both ends inclusive
    }

    // ---- game HUD ----
    // Straight passthroughs to Core.GameHud (mutex-guarded, so worker-thread script ticks are fine; the
    // labels copy into engine-owned std::strings, so a script DLL's .rdata never outlives its call here).
    void thunk_hudSetSlot(int slotIndex, const char* label, int count) { Globals::gameHud.setSlot(slotIndex, label != nullptr ? label : "", count); }
    void thunk_hudSetSlotCount(int slotIndex, int count)               { Globals::gameHud.setSlotCount(slotIndex, count); }
    void thunk_hudClearSlot(int slotIndex)                             { Globals::gameHud.clearSlot(slotIndex); }
    void thunk_hudSelectSlot(int slotIndex)                            { Globals::gameHud.selectSlot(slotIndex); }
    int  thunk_hudGetSelectedSlot(int)                                 { return Globals::gameHud.getSelectedSlot(); }
    void thunk_hudSetHotbarVisible(int visible)                        { Globals::gameHud.setHotbarVisible(visible != 0); }
    void thunk_hudSetBar(const char* name, float value, float maxValue, glm::vec3 color)
    {
        if (name != nullptr)
            Globals::gameHud.setBar(name, value, maxValue, color);
    }
    void thunk_hudRemoveBar(const char* name)
    {
        if (name != nullptr)
            Globals::gameHud.removeBar(name);
    }
    void thunk_hudSetCounter(const char* name, float value, int decimals, glm::vec3 color)
    {
        if (name != nullptr)
            Globals::gameHud.setCounter(name, value, decimals, color);
    }
    void thunk_hudRemoveCounter(const char* name)
    {
        if (name != nullptr)
            Globals::gameHud.removeCounter(name);
    }
    void thunk_hudClear(int) { Globals::gameHud.clearAll(); }

}
#pragma warning(pop)

ScriptContext::ScriptContext()
    : log(&thunk_log)
    , logf(&thunk_logf)
#define SCRIPT_CTX_INIT(ret, name, ...) , name(&thunk_##name)
    SCRIPT_CTX_FUNCS(SCRIPT_CTX_INIT)
#undef SCRIPT_CTX_INIT
    , networkEventSender(&thunk_networkEventSender) // hand-declared (zero-arg), see ScriptAPI.h
{
    deltaSeconds = 0.0f;
    elapsedSeconds = 0.0f;
    cameraPosition = glm::vec3(0.0f);
    cameraDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
    cameraFovDeg = 45.0f;
}

void ScriptContext::update(const Camera& camera, float newDeltaSeconds, float newElapsedSeconds)
{
    ProfileScope profileScope("Script context", EProfileCategory::Script);
    deltaSeconds = newDeltaSeconds;
    elapsedSeconds = newElapsedSeconds;

    // Forward/up are derived from the inverse view matrix (its -Z / Y columns).
    const glm::mat4 camToWorld = glm::inverse(camera.viewMatrix);
    cameraPosition = camera.position;
    cameraDirection = glm::normalize(-glm::vec3(camToWorld[2]));
    cameraUp = glm::normalize(glm::vec3(camToWorld[1]));
    cameraFovDeg = camera.fovDeg;
    cameraNear = camera.near;
    cameraFar = camera.far;
}

static_assert(offsetof(Entity, pos) == 0, "ScriptAPI.h Entity mirror out of sync: pos");
static_assert(offsetof(Entity, scale) == 12, "ScriptAPI.h Entity mirror out of sync: scale");
static_assert(offsetof(Entity, rot) == 16, "ScriptAPI.h Entity mirror out of sync: rot");
static_assert(offsetof(Entity, parent) == 32, "ScriptAPI.h Entity mirror out of sync: parent");