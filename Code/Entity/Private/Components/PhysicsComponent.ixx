export module Entity:PhysicsComponent;

import :Entity;
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
    SpatialOccluder occluder; // static mesh colliders feed the CPU occlusion buffer (registered at spawn)
    std::shared_ptr<const OccluderData> occluderData; // shared object-space triangles behind `occluder`
    glm::vec3 prevPos, currPos; // body pose at the previous/current physics step (dynamic interpolation)
    glm::quat prevRot, currRot;
    float shapeScale = 1.0f;  // world scale baked into the shape at spawn
    uint32 lastStep = 0;
    EPhysicsBodyType bodyType = EPhysicsBodyType::Dynamic;
    bool lockRotation = false; // locked bodies write back only their position: the body never
                               // rotates, so entity.rot stays free for script-driven facing
    bool enabled = true;
    bool suspended = false;   // body removed from the simulation (entity disabled via EEntityFlag_Enabled)
	PhysicsWorld::ContactEvent* pContactEventList = nullptr; // linked list of contact events collected this frame

    // Fired by dispatchPhysicsContactEvents for begin/end contact and sensor overlaps involving this
    // body (the shape must set ContactEvents true, or be a Sensor). C++ gameplay hook; scripts get
    // the same events through the "On Physics Event" node.
    std::function<void(Entity& other, bool begin)> onContact;

    struct SpawnInfo
    {
        EPhysicsBodyType bodyType = EPhysicsBodyType::Dynamic;
        PhysicsShape shape;                    // filter bits / geometry resolved from the fields below at parse time
        std::string layer;                     // named collision layer (category), empty = Default
        std::vector<std::string> collidesWith; // named layers this body collides with ("All"/"None" allowed), empty = all
        std::shared_ptr<PhysicsMesh> mesh;     // Shape Mesh: keeps the shared collision BVH alive (shape.mesh points at it)
        std::shared_ptr<const OccluderData> occluders; // Shape Mesh + Static: occlusion-culling occluder triangles
        bool lockRotation = false;             // dynamic body never rotates (upright character capsules)
        bool enabled = true;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, const Transform& parentWorld);

    // Pulls the body out of the simulation (and drops its occluder) while the entity is disabled
    // (EEntityFlag_Enabled); the next update() after re-enable re-adds and resyncs it. See updateTree.
    void suspendBody();
};

// Suspends every PhysicsComponent body in this entity's subtree (used when the entity is disabled —
// updateTree stops reaching it, so the bodies would otherwise keep colliding invisibly).
export void suspendPhysicsTree(Entity& entity);

export const PhysicsComponent::SpawnInfo* getPhysicsSpawnInfo(const Entity* entity);

// Serializes a physics spawn recipe into a "Component Physics" node.
export void writePhysicsSpawnInfo(const PhysicsComponent::SpawnInfo& info, AssetNode& out);
