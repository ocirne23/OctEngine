module;

#include "ScriptAPI.h"

export module Entity:ScriptContext;

import Core.Camera;

// ScriptContext comes from the ABI header in the global module fragment, so it isn't reachable through
// `import Entity` on its own -- without this an importer can hold `Globals::scriptContext` but can't
// name its type.
export using ::ScriptContext;

// The engine side of the ABI struct: ScriptAPI.h declares no member functions (it cannot name engine types such
// as Camera), so the constructor that binds the thunks and the per-frame update live here. Single inheritance,
// no virtuals: &Globals::scriptContext is the same address as the ScriptContext* the scripts receive.
export class EngineScriptContext : public ScriptContext
{
public:
    EngineScriptContext();

    void update(const Camera& camera, float newDeltaSeconds, float newElapsedSeconds);
};

export namespace Globals
{
    EngineScriptContext scriptContext;
}

// Registers the DSL's base vocabulary (vec2/vec3/vec4/quat, self/physics/audio/force/Engine, the 5 ScriptAPI
// entry points) against Globals::scriptBindings -- Script's own registry has no built-in content of its own
// (see ScriptBindings.ixx), so THIS call is what guarantees it exists. Must run once from main, before anything
// touches the registry (ScriptEditor's ScriptBindings::build() first, in practice) -- same explicit-call
// convention as ScriptEventManager::initialize().
export void registerScriptDslBindings();
