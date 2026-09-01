export module Physics:Body;

import Core;
import Core.glm;

// Parallel entity spawning: box3d's body create/destroy mutate shared world arrays (body pools,
// broadphase), so concurrent spawn/despawn jobs serialize on this ONE module-wide mutex —
// PhysicsWorld::createBody and PhysicsBody::destroy both take it. Everything else that writes
// box3d stays main-thread-only (or rides the body-command queue) per the standing contract, so
// no other box3d call needs it. Namespace scope per the /Zc:threadSafeInit- rule; a std::mutex
// is constant-initialized, so static init is safe.
std::mutex g_bodyLifecycleMutex; // module linkage: visible to Physics units importing :Body, never exported

// RAII handle to a rigid body (movable, like RenderNode). Destroying the handle destroys the body.
export class PhysicsBody final
{
public:

    PhysicsBody() = default;
    PhysicsBody(const PhysicsBody&) = delete;
    PhysicsBody& operator=(const PhysicsBody&) = delete;
    PhysicsBody(PhysicsBody&& other) noexcept : m_handle(other.m_handle) { other.m_handle = 0; }
    PhysicsBody& operator=(PhysicsBody&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_handle = other.m_handle;
            other.m_handle = 0;
        }
        return *this;
    }
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

    // IMPULSES are instantaneous (a hit, a jump); FORCES accumulate over the step and are cleared by box3d
    // every step, so a continuous push has to be re-applied each frame. Both wake the body. The "AtPoint"
    // variants take a WORLD-space point and therefore also impart spin -- applying at the center of mass does
    // not, which is what the plain forms do.
    void applyImpulse(const glm::vec3& impulse);
    void applyImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& worldPoint);
    void applyAngularImpulse(const glm::vec3& impulse);
    void applyForce(const glm::vec3& force);
    void applyForceAtPoint(const glm::vec3& force, const glm::vec3& worldPoint);
    void applyTorque(const glm::vec3& torque);

    float getMass() const;                  // 0 for a static/kinematic body
    glm::vec3 getCenterOfMass() const;      // world space
    glm::vec3 getPointVelocity(const glm::vec3& worldPoint) const; // includes the spin contribution

    // Per-body multiplier on world gravity (0 = floats, 1 = normal, negative = falls upward).
    float getGravityScale() const;
    void setGravityScale(float scale);
    float getLinearDamping() const;
    void setLinearDamping(float damping);
    float getAngularDamping() const;
    void setAngularDamping(float damping);

    bool isAwake() const;
    void setAwake(bool awake); // waking is what makes a settled body react to a changed velocity/force

private:

    friend class PhysicsWorld;
    explicit PhysicsBody(uint64 handle) : m_handle(handle) {}

    uint64 m_handle = 0; // b3BodyId bits, 0 = invalid
};
