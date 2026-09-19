export module Entity:PhysicsComponent;

import :Entity;
import :SceneComponent;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Physics;
import Spatial;

// A rigid body simulated by the Physics library. The body owns the pose: it is placed once at spawn from
// the entity's composed world transform, and is only ever moved again by PhysicsWorld::teleportBody.
// Dynamic bodies write their simulated pose into the entity's local transform each update, interpolated
// between fixed steps. The entity's world scale is baked into the collision shape at spawn.
export struct PhysicsComponent
{
    static constexpr EComponentID getId() { return EComponentID_Physics; }

    PhysicsBody body;
    SpatialOccluder occluder; // static mesh colliders feed the CPU occlusion buffer (registered at spawn;
                              // the triangles behind it stay in SpawnInfo::occluders)
    // Fired by dispatchPhysicsContactEvents for begin/end contact and sensor overlaps involving this
    // body (the shape must set ContactEvents true, or be a Sensor). C++ gameplay hook; scripts get
    // the same events through the "On Physics Event" node. A plain function pointer, not an
    // oc::function (32 bytes in every body): per-entity state comes from `self`.
    void (*onContact)(Entity& self, Entity& other, bool begin) = nullptr;
    // NO STORED POSE: the body is the pose. update() shows the body pose moved back along the body
    // velocity by the part of the step that is not shown yet (see update).
    float buoyancyVolume = 0.0f; // the shape's exact displaced volume (box3d mass / density) at spawn; 0 = never floats
    // The LOW 16 BITS of the step count the last buoyancy application was read after; the gap to the
    // current step (modulo 65536) is what the next application owes (see applyBuoyancy). A body that
    // was out of the loop for N steps with N mod 65536 in 1..4 (54 min at 20 Hz, 0.006 % of the
    // returns) gets one catch-up force of that many steps.
    uint16 buoyancyStep = 0;
    // THE FLAGS ARE TWO BYTES BY WHO WRITES THEM - a bit-field write rewrites its whole byte:
    //  1. SPAWN CONSTANTS: written in spawn() only. Other workers read them (unpark's probe).
    EPhysicsBodyType bodyType : 2 = EPhysicsBodyType::Dynamic;
    bool lockRotation : 1 = false; // locked bodies write back only their position: the body never
                                   // rotates, so entity.rot stays free for script-driven facing
    uint8 _unused : 5; // ends the spawn byte: the run-time bits below must not share it

    //  2. RUN-TIME: written by the entity's own job or by main outside the pass. `suspended` is
    //     also READ by other workers (unpark's probe) while the owner rewrites this byte for
    //     another bit. A rewrite keeps the `suspended` bit, so the read is right on x86; it is a
    //     data race by the letter of the standard, accepted like the probe's schedTier read.
    bool enabled : 1 = true;    // only spawn() writes it today; it sits here so a later setter is safe
    bool suspended : 1 = false; // body removed from the simulation (entity disabled via EEntityFlag_Enabled)
    bool buoyant : 1 = false; // BUOYANCY GATE, written by World::simLodDelta on every visit (like the force
                              // bubble gate): true while the entity's distance tier is <= SimLodConfig::
                              // buoyancyMaxTier, always outside the LOD. A never-visited far body stays off.
    bool poseHeld : 1 = false; // set by snapPose: the next update() leaves the entity pose as it is
	uint8 _unused2 : 4; // ends the run-time byte

    struct SpawnInfo
    {
        EPhysicsBodyType bodyType = EPhysicsBodyType::Dynamic;
        PhysicsShape shape;                    // filter bits / geometry resolved from the fields below at parse time
        oc::string layer;                     // named collision layer (category), empty = Default
        oc::vector<oc::string> collidesWith; // named layers this body collides with ("All"/"None" allowed), empty = all
        oc::shared_ptr<PhysicsMesh> mesh;     // Shape Mesh: keeps the shared collision BVH alive (shape.mesh points at it)
        oc::shared_ptr<const OccluderData> occluders; // Shape Mesh + Static: occlusion-culling occluder triangles
        bool lockRotation = false;             // dynamic body never rotates (upright character capsules)
        bool enabled = true;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, const Transform& parentWorld);

    // THE TELEPORT CONTRACT: call with every teleportBody. The teleport is queued and lands at the
    // next physics.update, so until then the body still holds the old pose. The snap writes the
    // WORLD pose into the entity at once (rotation only for a body that owns entity.rot) and the
    // next update() leaves it there. A body that is snapped every frame (network playback)
    // therefore always shows exactly the snap pose.
    void snapPose(Entity& entity, const glm::vec3& pos, const glm::quat& rot);
    void snapPose(Entity& entity, const glm::vec3& pos); // keeps the rotation
    // The world position the entity shows: what update() or a snap wrote last. Read before this
    // frame's entity pass (the player camera) it is the pose the mesh showed last frame.
    static glm::vec3 getShownPosition(const Entity& entity);

    // Pulls the body out of the simulation (and drops its occluder) while the entity is disabled
    // (EEntityFlag_Enabled); the next update() after re-enable re-adds and resyncs it. See updateTree.
    void suspendBody();
    // PARK / UNPARK - for a body whose entity the World stops simulating (SIM LOD dormancy).
    // Both ride the body-command queue (pass-safe, applied before the next step). park: zero the
    // velocities, then DISABLE (out of the broadphase + solver, pose kept - nothing can wake or
    // push it) or merely put it to sleep. unpark: zero the velocities (a body parked inside a
    // crowd may still hold a contact push-out) and enable - a no-op on an enabled body; skipped
    // while `suspended` (an Enabled-off subtree owns its own disable). `velocity` = the linear
    // velocity to wake with (a marching unit's walk - GameUnitComponent::wakeVelocity; zero at
    // rest). A dynamic body waking within a metre of another live one is first LIFTED 2 m (far-
    // ticked units teleport through each other; landing on a body beats exploding out of it).
    void park(bool disable);
    void unpark(Entity& entity, const glm::vec3& velocity = glm::vec3(0.0f));
    // BUOYANCY, per component on the entity pass (workers). The queued force is consumed by ONE
    // step, so `steps` - the steps that elapsed since the last application - is the force scale
    // that makes the impulse per second of SIM TIME the same at any frame rate. It is not always
    // 1: the caller keeps the work off the step frame (already the busy one) where it can, and
    // near the step rate that deferral lets a step slip past.
    // A `lockRotation` body uses ONE probe at its AABB centre with the whole volume (no torque to
    // gain, one force queued); a free one splits its world AABB into 2x2x2 probes, each carrying
    // its share of the volume scaled by depth as an Archimedes force plus a drag against the
    // probe's point velocity, and queues the sum as ONE force + ONE torque about the centre of
    // mass (the off-centre probes give righting torque and tumbling damping for free). Reads
    // only; the writes ride the body-command queue and land at the next drain, before the step.
    void applyBuoyancy(uint32 steps);
};
static_assert(sizeof(PhysicsComponent) <= 32, "PhysicsComponent is inline in every physics entity: keep it in two 16-byte slots");

// Suspends every PhysicsComponent body in this entity's subtree (used when the entity is disabled -
// updateTree stops reaching it, so the bodies would otherwise keep colliding invisibly).
export void suspendPhysicsTree(Entity& entity, SceneComponent* sc);

export const PhysicsComponent::SpawnInfo* getPhysicsSpawnInfo(const Entity* entity);

// The composed world scale of the entity: what spawn() baked into the collision shape (the shape
// does not follow a later scale change). Walks the parent chain - not for per-frame use in bulk.
export float getPhysicsShapeScale(const Entity* entity);

// Serializes a physics spawn recipe into a "Component Physics" node.
export void writePhysicsSpawnInfo(const PhysicsComponent::SpawnInfo& info, AssetNode& out);
