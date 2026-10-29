export module Physics:PhysicsTypes;

import Core;
import Core.glm;
import Core.Transform;

export class PhysicsWorld; // declared before the handles' friend lines

// ---- Layers ----

// Named collision layers. A name maps to one of 64 category bits, allocated on first use;
// "Default" is bit 0 (the categoryBits/maskBits defaults below). Two shapes collide when each one's
// mask contains the other's category (unless a non-zero matching groupIndex overrides: negative =
// never collide, positive = always).
export namespace PhysicsLayers
{
    uint64 bit(oc::string_view name);
    constexpr uint64 All = ~0ull;
}

// ---- Handles ----

// A shared triangle-mesh collision BVH (box3d b3MeshData). Mesh shapes reference it, so it must
// outlive every body using it. Movable RAII, like PhysicsBody.
export class PhysicsMesh final
{
public:

    PhysicsMesh() = default;
    PhysicsMesh(PhysicsMesh&& other) noexcept : m_data(other.m_data) { other.m_data = 0; }
    PhysicsMesh& operator=(PhysicsMesh&& other) noexcept;
    ~PhysicsMesh() { destroy(); }

    bool isValid() const { return m_data != 0; }
    void destroy();

    PhysicsMesh(const PhysicsMesh&) = delete;
    PhysicsMesh& operator=(const PhysicsMesh&) = delete;

private:

    friend class PhysicsWorld;
    uint64 m_data = 0; // b3MeshData*
};

// RAII handle to a joint. Destroying an attached body destroys the joint too, so the handle
// validates before destroying.
export class PhysicsJoint final
{
public:

    PhysicsJoint() = default;
    PhysicsJoint(PhysicsJoint&& other) noexcept : m_handle(other.m_handle) { other.m_handle = 0; }
    PhysicsJoint& operator=(PhysicsJoint&& other) noexcept;
    ~PhysicsJoint() { destroy(); }

    bool isValid() const { return m_handle != 0; }
    void destroy();

    PhysicsJoint(const PhysicsJoint&) = delete;
    PhysicsJoint& operator=(const PhysicsJoint&) = delete;

private:

    friend class PhysicsWorld;
    explicit PhysicsJoint(uint64 handle) : m_handle(handle) {}

    uint64 m_handle = 0; // b3JointId bits, 0 = invalid
};

// ---- Bodies and shapes ----

export enum class EPhysicsBodyType : uint8
{
    Static,
    Kinematic,
    Dynamic,
};

export enum class EPhysicsShapeType : uint8
{
    Box,
    Sphere,
    Capsule,
    Hull, // convex hull reduced from a point cloud (hullPoints)
    Mesh, // triangle-mesh BVH (mesh), static bodies only
};

export struct PhysicsShape
{
    EPhysicsShapeType type = EPhysicsShapeType::Box;
    glm::vec3 halfExtents = glm::vec3(0.5f); // Box
    float radius = 0.5f;                     // Sphere / Capsule
    float halfHeight = 0.5f;                 // Capsule: center to hemisphere center, along local Y
    glm::vec3 offset = glm::vec3(0.0f);      // placement within the body's local space
    float density = 1000.0f;                 // kg/m^3
    float friction = 0.6f;
    float restitution = 0.0f;
    uint64 categoryBits = 1;                 // the layer(s) this shape is on (bit 0 = "Default")
    uint64 maskBits = PhysicsLayers::All;    // the layers this shape collides with
    int groupIndex = 0;                      // box3d collision group override, 0 = none

    oc::vector<glm::vec3> hullPoints;       // Hull: point cloud, reduced to a convex hull at body creation
    int maxHullVertices = 32;                // Hull: vertex budget for the reduction (clamped to [4, 64])
    const PhysicsMesh* mesh = nullptr;       // Mesh: shared triangle BVH, must outlive the body

    bool isSensor = false;                   // overlap events instead of collision response
    bool contactEvents = false;              // report begin/end touch events for this shape
};

export struct PhysicsBodyDesc
{
    EPhysicsBodyType type = EPhysicsBodyType::Dynamic;
    Transform transform;                          // world placement; transform.scale is baked into the shape dimensions
    glm::vec3 linearVelocity = glm::vec3(0.0f);
    void* userData = nullptr;                     // reported back through ContactEvent (the engine stores Entity* here)
    bool lockRotation = false;                    // solver-level lock on all angular axes: the body never rotates
                                                  // (upright character capsules); contacts apply no torque
};

// ---- The body-command queue (PhysicsWorld::queueBodyCommand, spelled PhysicsWorld::EBodyCommand) ----

// `a` is the velocity/impulse/force/torque, `b` the world-space point for the AtPoint variants.
export enum class EPhysicsBodyCommand : uint8
{
    Teleport, SetLinearVelocity, SetAngularVelocity, ApplyImpulse, ApplyImpulseAtPoint,
    ApplyAngularImpulse, ApplyForce, ApplyForceAtPoint, ApplyTorque,
    SetGravityScale, SetLinearDamping, SetAngularDamping, SetAwake, // scalar in a.x
    SetEnabled, // scalar in a.x: 0 = b3Body_Disable (out of the broadphase + solver entirely,
                // pose kept), else b3Body_Enable. The pass-safe form of PhysicsBody::setEnabled
    SetWorldGravity,
    // Bounded velocity steering toward a target (network correction "push"): a = target linear
    // velocity, b = target angular velocity, c.x/c.y = max linear/angular velocity change applied
    // THIS drain. The linear half lands as a center impulse of at most mass*c.x (composes with the
    // sim like any hit); the angular half is the exact result the ideal angular impulse I*dw would
    // produce, applied as a capped velocity add (the inertia tensor cancels out of the math).
    NudgeVelocity,
};

// ---- Inline bodies ----

inline PhysicsMesh& PhysicsMesh::operator=(PhysicsMesh&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_data = other.m_data;
        other.m_data = 0;
    }
    return *this;
}

inline PhysicsJoint& PhysicsJoint::operator=(PhysicsJoint&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_handle = other.m_handle;
        other.m_handle = 0;
    }
    return *this;
}
