export module Physics:PhysicsWorldState;

import Core;
import Core.glm;
import Settings;

import :PhysicsTypes;
import :Body;
import :TaskScheduler;

// Everything of PhysicsWorld but its public API (PhysicsWorld.ixx): the box3d world, the step, the body-command queue, the solver's task scheduler, the
// buoyancy sampler and the debug-draw sinks. No helper here calls the API, so there is no self().
export class PhysicsWorldState
{
protected:

    ~PhysicsWorldState() { shutdown(); }

    void shutdown();
    void applyQueuedCommands(); // main thread, start of update
    void stepSimulation(double deltaSec);
    void debugDraw(const glm::vec3& viewPos, const oc::function<void(const glm::vec3&, const glm::vec3&, uint32)>& line); // driven by update()
    void applyContactTuning(); // Globals::settings.physics' contact tuning -> box3d
    void setGravity(const glm::vec3& gravity);

    // ---- The body-command queue ----

    struct BodyCommand
    {
        uint64 bodyHandle = 0; // b3BodyId bits (unused for SetWorldGravity)
        EPhysicsBodyCommand type = EPhysicsBodyCommand::Teleport;
        glm::vec3 a = glm::vec3(0.0f);   // pos / velocity / impulse / force / torque / gravity
        glm::vec3 b = glm::vec3(0.0f);   // world point for the AtPoint variants / NudgeVelocity target angular velocity
        glm::vec3 c = glm::vec3(0.0f);   // NudgeVelocity caps (max dV, max dW)
        glm::quat rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Teleport only
    };
    std::mutex m_commandMutex;
    oc::vector<BodyCommand> m_commands;
    oc::vector<BodyCommand> m_commandScratch; // swapped with m_commands at drain, so neither reallocates

    // ---- The world ----

    uint32 m_worldHandle = 0; // b3WorldId bits
    bool m_initialized = false;
    float m_accumulator = 0.0f;
    // Handed to box3d as b3WorldDef::userTaskContext and returned through both task callbacks, so
    // the solver's fork/join state is owned by this world rather than by file statics.
    PhysicsTaskScheduler m_taskScheduler;
    uint32 m_stepCount = 0;
    uint32 m_lastDispatchedStep = 0; // dispatchContactEvents' double-fire guard
    PhysicsBody m_staticBody;

    // ---- Buoyancy (see setWaterSurface) ----

    oc::function<float(float x, float z)> m_waterSurface;
    oc::function<bool()> m_waterActive; // global gate; empty = always on

    // ---- Debug draw (the Physics/Debug tweaks) ----

    oc::function<void(const glm::vec3&, const glm::vec3&, uint32)> m_debugLine; // registered by the App, see setDebugDrawCallback
    oc::function<glm::vec3()> m_debugViewPos; // ditto: where the wireframes are centred/range-culled
};
