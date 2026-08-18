export module Physics:PhysicsWorld;

import Core;
import Core.glm;

import :Body;
import :Joint;
import :Mesh;
import :Types;
import :Layers;
import :TaskScheduler;

export class PhysicsWorld final
{
public:

    ~PhysicsWorld() { shutdown(); }

    bool initialize();
    void shutdown();

    // Contact/sensor begin/end events fired during update() (this frame's steps), userData is Entity*
	// Do not store the returned ContactEvent references.
    struct ContactEvent
    {
        void* userDataA = nullptr;
        void* userDataB = nullptr;
        bool begin = false;   // begin or end touch
        bool sensor = false;  // sensor overlap instead of a solid contact
        int64 contactId = 0;  // opaque box3d contact handle (0 for sensor events, which have no manifold);
                               // pass to getContactPoint() before the next update() call, after which it may
                               // reference a recycled contact
    };

    // Applies queued body commands (teleports, velocities, impulses, ...), then runs the fixed-step
    // accumulator: steps at stepHz with a bounded number of catch-up steps per frame. Also drains
    // contact/sensor events (see getContactEvents) and advances the step counter. Also emits the
    // collider wireframes, see setDebugDrawCallback.
    void update(double deltaSec, oc::function<void(const ContactEvent&)> contactCallback);

    // Thread-safe absolute repositioning: queues the pose, applied at the start of the next update()
    // before any step. The only way to move a body. Requests apply in call order (last one for a body
    // wins) and are discarded if the body died before the drain.
    void teleportBody(const PhysicsBody& body, const glm::vec3& pos, const glm::quat& rot);

    // Thread-safe DEFERRED body writes, same contract as teleportBody: queued, applied in call order
    // at the start of the next update(), dropped if the body died. Every box3d body write can WAKE
    // the body, which mutates the world's shared solver sets - never thread-safe off the main
    // thread - so anything that can run on a job (the script thunks during the parallel entity
    // pass) must go through here. Reads meanwhile still see the pre-write values (one frame of
    // latency, like teleport). `a` is the velocity/impulse/force/torque, `b` the world-space point
    // for the AtPoint variants.
    enum class EBodyCommand : uint8
    {
        Teleport, SetLinearVelocity, SetAngularVelocity, ApplyImpulse, ApplyImpulseAtPoint,
        ApplyAngularImpulse, ApplyForce, ApplyForceAtPoint, ApplyTorque,
        SetGravityScale, SetLinearDamping, SetAngularDamping, SetAwake, // scalar in a.x
        SetWorldGravity,
        // Bounded velocity steering toward a target (network correction "push"): a = target linear
        // velocity, b = target angular velocity, c.x/c.y = max linear/angular velocity change applied
        // THIS drain. The linear half lands as a center impulse of at most mass*c.x (composes with the
        // sim like any hit); the angular half is the exact result the ideal angular impulse I*dw would
        // produce, applied as a capped velocity add (the inertia tensor cancels out of the math).
        NudgeVelocity,
    };
    void queueBodyCommand(const PhysicsBody& body, EBodyCommand type, const glm::vec3& a, const glm::vec3& b = glm::vec3(0.0f), const glm::vec3& c = glm::vec3(0.0f));
    void queueSetGravity(const glm::vec3& gravity); // world gravity via the same queue (no body)

    PhysicsBody createBody(const PhysicsBodyDesc& desc, oc::span<const PhysicsShape> shapes);

    // Builds a shared triangle-mesh BVH from render-style geometry (positions + triangle indices).
    // Standalone data: does not require the world and may be built before initialize().
    PhysicsMesh createCollisionMesh(oc::span<const glm::vec3> vertices, oc::span<const uint32> indices);

    // A shapeless static body owned by the world, for anchoring joints to the world itself.
    PhysicsBody& staticBody() { return m_staticBody; }

    // Joints. Anchors/axes are world space; local frames are derived from the bodies' current poses,
    // so create joints after the bodies are at their intended placement.
    PhysicsJoint createDistanceJoint(const PhysicsBody& a, const PhysicsBody& b,
        const glm::vec3& anchorA, const glm::vec3& anchorB, float minLength, float maxLength);
    PhysicsJoint createRevoluteJoint(const PhysicsBody& a, const PhysicsBody& b,
        const glm::vec3& pivot, const glm::vec3& axis, float lowerDeg = -FLT_MAX, float upperDeg = FLT_MAX);
    PhysicsJoint createSphericalJoint(const PhysicsBody& a, const PhysicsBody& b,
        const glm::vec3& pivot, float coneAngleDeg = 180.0f);
    PhysicsJoint createWeldJoint(const PhysicsBody& a, const PhysicsBody& b, const glm::vec3& anchor);

    struct RayHit
    {
        bool hit = false;
        float fraction = 0.0f;
        glm::vec3 point = glm::vec3(0.0f);
        glm::vec3 normal = glm::vec3(0.0f);
    };
    // ignoreBody skips every shape of that body — needed when the ray travels between two poses of
    // the body itself (network claim validation): box3d's closest-cast only ignores overlap AT the
    // origin, so the body's own surface a step ahead would count as a hit. staticOnly restricts hits
    // to static bodies (same caller: a DYNAMIC body in a movement claim's path is something the
    // claimant can legitimately push, never a wall to validate against).
    RayHit castRayClosest(const glm::vec3& origin, const glm::vec3& translation, uint64 maskBits = PhysicsLayers::All,
        const PhysicsBody* ignoreBody = nullptr, bool staticOnly = false) const;

    // Buoyancy: with a water surface set (the App wires the ocean's CPU height field), every dynamic
    // body gets probe-based buoyancy + drag before each fixed step — bodies denser than water sink,
    // lighter ones float and bob in the swell, tilted floaters right themselves (per-probe forces
    // torque the body for free). fn(x, z) returns the water surface world Y at that column, or
    // -FLT_MAX where there is no water. Density/drag are Tweaks under Physics/Buoyancy. An empty
    // function disables the pass entirely.
    using WaterSurfaceFn = oc::function<float(float x, float z)>;
    void setWaterSurface(WaterSurfaceFn fn) { m_waterSurface = oc::move(fn); }

    // Resolves a ContactEvent::contactId (from THIS frame, before the next update()) to its first manifold
    // point in world space. Returns false (leaving the outputs untouched) if the contact is stale/gone or
    // has no manifold point (e.g. purely speculative contact).
    bool getContactPoint(int64 contactId, glm::vec3& outPoint, glm::vec3& outNormal) const;

    void setGravity(const glm::vec3& gravity);

    // Collider wireframe debug view. The App registers a line sink ONCE (keeps Physics free of any
    // renderer dependency) plus a source for the view position to draw around -- a callback rather than
    // an update() argument, because the simulation has no notion of a viewer and shouldn't carry one
    // through its signature. update() emits into the sink whenever the "Physics/Debug/Draw colliders"
    // tweak is on; every shape type (box/sphere/capsule/hull/mesh), plus optionally joints/contacts/
    // AABBs, is decomposed into world-space segments. Drawing is bounded to "Range" around the view
    // position (mesh triangles are also CPU-culled per triangle, so a huge static mesh collider doesn't
    // emit its whole wireframe). Color is packed RGBA8 with R in the low byte (matches GLSL unpackUnorm4x8).
    using DebugLineFn = oc::function<void(const glm::vec3& a, const glm::vec3& b, uint32 colorRGBA)>;
    using DebugViewPosFn = oc::function<glm::vec3()>;
    void setDebugDrawCallback(DebugLineFn line, DebugViewPosFn viewPos)
    {
        m_debugLine = oc::move(line);
        m_debugViewPos = oc::move(viewPos);
    }
    bool isDebugDrawEnabled() const { return m_debugDrawEnabled; }

    // Fixed-step render interpolation: total steps taken, and the fraction [0,1] of the current frame
    // into the next step (1 when interpolation is disabled via the Tweak).
    uint32 getStepCount() const { return m_stepCount; }
    float getInterpolationAlpha() const;
    int getStepHz() const { return m_stepHz; } // for code that must pace itself in whole steps

    bool isInitialized() const { return m_initialized; }

private:

    void applyBuoyancy(); // per fixed step, before b3World_Step (box3d clears forces every step)
    void applyQueuedCommands(); // main thread, start of update
    void stepSimulation(double deltaSec, const oc::function<void(const ContactEvent&)>& contactCallback);
    void debugDraw(const glm::vec3& viewPos, const DebugLineFn& line); // driven by update(), see setDebugDrawCallback

    struct BodyCommand
    {
        uint64 bodyHandle = 0; // b3BodyId bits (unused for SetWorldGravity)
        EBodyCommand type = EBodyCommand::Teleport;
        glm::vec3 a = glm::vec3(0.0f);   // pos / velocity / impulse / force / torque / gravity
        glm::vec3 b = glm::vec3(0.0f);   // world point for the AtPoint variants / NudgeVelocity target angular velocity
        glm::vec3 c = glm::vec3(0.0f);   // NudgeVelocity caps (max dV, max dW)
        glm::quat rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Teleport only
    };
    std::mutex m_commandMutex;
    oc::vector<BodyCommand> m_commands;
    oc::vector<BodyCommand> m_commandScratch; // swapped with m_commands at drain, so neither reallocates

    uint32 m_worldHandle = 0; // b3WorldId bits
    bool m_initialized = false;
    bool m_paused = false;
    bool m_interpolate = true;
    float m_accumulator = 0.0f;
    float m_timeScale = 1.0f;
    int m_subSteps = 4;
    int m_stepHz = 20;
    int m_workerCount = 1; // box3d parallelism, driven onto the engine job system (see :TaskScheduler)
    // Handed to box3d as b3WorldDef::userTaskContext and returned through both task callbacks, so
    // the solver's fork/join state is owned by this world rather than by file statics.
    PhysicsTaskScheduler m_taskScheduler;
    uint32 m_stepCount = 0;
    glm::vec3 m_gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    PhysicsBody m_staticBody;

    // Buoyancy (see setWaterSurface)
    WaterSurfaceFn m_waterSurface;
    float m_waterDensity = 1000.0f;  // kg/m^3: fresh water; shapes denser than this sink
    float m_waterLinearDrag = 3.0f;  // 1/s: drag on each submerged probe's point velocity
    oc::vector<uint64> m_buoyancyShapes; // per-step overlap scratch (b3StoreShapeId bits)

    // Debug draw tweaks (Physics/Debug)
    DebugLineFn m_debugLine;       // registered by the App, see setDebugDrawCallback
    DebugViewPosFn m_debugViewPos; // ditto: where the wireframes are centred/range-culled
    bool m_debugDrawEnabled = false;
    bool m_debugDrawJoints = false;
    bool m_debugDrawContacts = false;
    bool m_debugDrawBounds = false;
    float m_debugDrawRange = 64.0f; // draw distance around viewPos (world units)
};

export namespace Globals
{
    PhysicsWorld physics;
}
