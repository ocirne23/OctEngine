export module Physics:Body;

import Core;
import Core.glm;
import :PhysicsTypes;

// RAII handle to a rigid body (movable, like RenderNode). Destroying the handle destroys the body. The direct setters are main-thread only; anything that
// can run on a job writes through PhysicsWorld::queueBodyCommand.
export class PhysicsBody final
{
public:

    PhysicsBody() = default;
    PhysicsBody(PhysicsBody&& other) noexcept : m_handle(other.m_handle) { other.m_handle = 0; }
    PhysicsBody& operator=(PhysicsBody&& other) noexcept;
    ~PhysicsBody() { destroy(); }

    bool isValid() const { return m_handle != 0; }
    void destroy();

    // Removes/re-adds the body from the simulation entirely (no collision, no queries, no events).
    // Expensive in box3d; callers should track state and avoid redundant toggles.
    void setEnabled(bool enabled);
    bool isEnabled() const;

    glm::vec3 getPosition() const;
    glm::quat getRotation() const;
    // No setTransform: repositioning goes through PhysicsWorld::teleportBody.

    glm::vec3 getLinearVelocity() const;
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;            // radians/second about each world axis
    void setAngularVelocity(const glm::vec3& radiansPerSecond);
    // Instantaneous (a hit, a jump), at the center of mass; wakes the body.
    void applyImpulse(const glm::vec3& impulse);

    float getMass() const;                  // 0 for a static/kinematic body
    glm::vec3 getCenterOfMass() const;      // world space
    void getAABB(glm::vec3& outLower, glm::vec3& outUpper) const; // world AABB over every shape (the body origin if none)
    glm::vec3 getPointVelocity(const glm::vec3& worldPoint) const; // includes the spin contribution

    float getGravityScale() const; // per-body multiplier on world gravity (0 = floats, 1 = normal, negative = falls upward)
    float getLinearDamping() const;
    float getAngularDamping() const;
    bool isAwake() const;

    PhysicsBody(const PhysicsBody&) = delete;
    PhysicsBody& operator=(const PhysicsBody&) = delete;

private:

    friend class PhysicsWorld;
    explicit PhysicsBody(uint64 handle) : m_handle(handle) {}

    uint64 m_handle = 0; // b3BodyId bits, 0 = invalid
};

// ---- Inline bodies ----

inline PhysicsBody& PhysicsBody::operator=(PhysicsBody&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_handle = other.m_handle;
        other.m_handle = 0;
    }
    return *this;
}
