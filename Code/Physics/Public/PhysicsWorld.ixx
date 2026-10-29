export module Physics:PhysicsWorld;

import Core;
import Core.glm;
import Settings;

import :PhysicsTypes;
import :Body;
import :PhysicsWorldState; // the box3d world, the step, the command queue, the task scheduler, the sinks

// THE PHYSICS WORLD (Globals::physics): box3d behind integer handles. The state and the private helpers are in PhysicsWorldState.ixx.
export class PhysicsWorld final : public PhysicsWorldState
{
public:

    bool initialize();

    // ---- The frame ----

    // Contact/sensor begin/end events from the frame's step, fired by dispatchContactEvents(),
    // userData is Entity*. Do not store the returned ContactEvent references.
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
    // accumulator - AT MOST ONE step per update (deliberate: box3d buffers each step's contact/
    // sensor events until the NEXT step, so a single step keeps them valid for the deferred
    // dispatchContactEvents below; under stepHz the sim runs slower than real time instead of
    // catching up). Contact events are NOT fired here - nothing in update() touches the spatial
    // index or renderer state, so the main loop overlaps it with the spatial-cull/begin-frame
    // jobs. Also emits the collider wireframes, see setDebugDrawCallback.
    void update(double deltaSec);
    // Fires the step's contact/sensor events (reads box3d's buffers - no copy). Main thread, after
    // update() and before the next one; no-op unless a step ran since the last dispatch (the
    // buffers still hold the OLD events until the next step - refiring would duplicate). Deferred
    // out of update() because contact scripts query the spatial index and can touch renderer state
    // (light/sun thunks): the main loop calls this AFTER joining the spatial/begin-frame jobs.
    void dispatchContactEvents(const oc::function<void(const ContactEvent&)>& contactCallback);

    // Whether update(deltaSec) will take a step this frame (the accumulator reaches a step and the
    // sim is live): main publishes it to the JobSystem at the frame top (setFrameHasPhysicsStep),
    // so work that can wait a frame keeps off the step's worker load.
    bool willStep(double deltaSec) const;
    // Fixed-step render interpolation: total steps taken, and the fraction [0,1] of the current frame
    // into the next step (1 when interpolation is disabled via the Tweak).
    uint32 getStepCount() const { return m_stepCount; }
    float getInterpolationAlpha() const;
    int getStepHz() const { return Globals::settings.physics.stepHz; } // for code that must pace itself in whole steps
    bool isInitialized() const { return m_initialized; }

    // ---- The body-command queue ----

    // Thread-safe absolute repositioning: queues the pose, applied at the start of the next update()
    // before any step. The only way to move a body. Requests apply in call order (last one for a body
    // wins) and are discarded if the body died before the drain.
    void teleportBody(const PhysicsBody& body, const glm::vec3& pos, const glm::quat& rot);

    // Thread-safe DEFERRED body writes, same contract as teleportBody: queued, applied in call order
    // at the start of the next update(), dropped if the body died. Every box3d body write can WAKE
    // the body, which mutates the world's shared solver sets - never thread-safe off the main
    // thread - so anything that can run on a job (the script thunks during the parallel entity
    // pass) must go through here. Reads meanwhile still see the pre-write values (one frame of
    // latency, like teleport). The commands are in PhysicsTypes.ixx.
    using EBodyCommand = EPhysicsBodyCommand;
    void queueBodyCommand(const PhysicsBody& body, EBodyCommand type, const glm::vec3& a, const glm::vec3& b = glm::vec3(0.0f), const glm::vec3& c = glm::vec3(0.0f));
    void queueSetGravity(const glm::vec3& gravity); // world gravity via the same queue (no body)

    // ---- Creation ----

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

    // ---- Queries ----

    struct RayHit
    {
        bool hit = false;
        float fraction = 0.0f;
        glm::vec3 point = glm::vec3(0.0f);
        glm::vec3 normal = glm::vec3(0.0f);
    };
    // ignoreBody skips every shape of that body - needed when the ray travels between two poses of
    // the body itself (network claim validation): box3d's closest-cast only ignores overlap AT the
    // origin, so the body's own surface a step ahead would count as a hit. staticOnly restricts hits
    // to static bodies (same caller: a DYNAMIC body in a movement claim's path is something the
    // claimant can legitimately push, never a wall to validate against).
    RayHit castRayClosest(const glm::vec3& origin, const glm::vec3& translation, uint64 maskBits = PhysicsLayers::All,
        const PhysicsBody* ignoreBody = nullptr, bool staticOnly = false) const;

    // Resolves a ContactEvent::contactId (from THIS frame, before the next update()) to its first manifold
    // point in world space. Returns false (leaving the outputs untouched) if the contact is stale/gone or
    // has no manifold point (e.g. purely speculative contact).
    bool getContactPoint(int64 contactId, glm::vec3& outPoint, glm::vec3& outNormal) const;

    // ---- Buoyancy ----

    // Water surface for buoyancy (the App wires the ocean's CPU height field). fn(x, z) returns
    // the water surface world Y at that column, or -FLT_MAX where there is no water. `active` is
    // the cheap global gate (no water anywhere yet); empty = always active. THE PHYSICS WORLD
    // APPLIES NO BUOYANCY ITSELF: Entity's PhysicsComponent computes the probe forces for its own
    // body on the entity pass (workers) through the read-only queries below, and queues them.
    // Density/drag are Tweaks under Physics/Buoyancy (Globals::settings.physics).
    using WaterSurfaceFn = oc::function<float(float x, float z)>;
    using WaterActiveFn = oc::function<bool()>;
    void setWaterSurface(WaterSurfaceFn fn, WaterActiveFn active = {})
    {
        m_waterSurface = oc::move(fn);
        m_waterActive = oc::move(active);
    }
    // Thread-safe reads for the component's buoyancy (the sampler is a const read of a CPU tile
    // that only main refreshes, outside the entity pass).
    bool isWaterActive() const { return m_waterSurface && (!m_waterActive || m_waterActive()); }
    float sampleWaterHeight(float x, float z) const { return m_waterSurface(x, z); } // isWaterActive first
    float getWaterDensity() const { return Globals::settings.physics.waterDensity; }
    float getWaterLinearDrag() const { return Globals::settings.physics.waterLinearDrag; }
    const glm::vec3& getGravity() const { return Globals::settings.physics.gravity; }

    // ---- Debug draw ----

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
};

// OC_SEG_PHYSICS: ~PhysicsWorld (b3DestroyWorld) fans its teardown tasks onto the job system, so it
// must destruct BEFORE ~JobSystem (XCU5) - and after ~World (XCU8), whose entities free their bodies
// into this still-live world. This is what lets the task scheduler assume the job system is always up.
OC_INIT_SEG(OC_SEG_PHYSICS)
export namespace Globals
{
    PhysicsWorld physics;
}

// ---- Inline bodies ----

inline bool PhysicsWorld::willStep(double deltaSec) const
{
    const PhysicsSettings& settings = Globals::settings.physics;
    return m_initialized && !settings.paused && m_accumulator + float(deltaSec) * settings.timeScale >= 1.0f / float(settings.stepHz);
}
