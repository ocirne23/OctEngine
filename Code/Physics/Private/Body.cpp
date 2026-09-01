module;

#include <box3d/box3d.h>

module Physics;

import Core.glm;

import :Body;
import :Convert;

void PhysicsBody::destroy()
{
    if (m_handle == 0)
        return;
    const std::lock_guard lock(g_bodyLifecycleMutex); // parallel spawn/despawn jobs — see Body.ixx
    const b3BodyId id = toBodyId(m_handle);
    if (b3Body_IsValid(id)) // the world may already be gone at shutdown
        b3DestroyBody(id);
    m_handle = 0;
}

void PhysicsBody::setEnabled(bool enabled)
{
    const b3BodyId id = toBodyId(m_handle);
    if (enabled)
        b3Body_Enable(id);
    else
        b3Body_Disable(id);
}

bool PhysicsBody::isEnabled() const
{
    return b3Body_IsEnabled(toBodyId(m_handle));
}

glm::vec3 PhysicsBody::getPosition() const
{
    return toGlm(b3Body_GetPosition(toBodyId(m_handle)));
}

glm::quat PhysicsBody::getRotation() const
{
    return toGlm(b3Body_GetRotation(toBodyId(m_handle)));
}

glm::vec3 PhysicsBody::getLinearVelocity() const
{
    return toGlm(b3Body_GetLinearVelocity(toBodyId(m_handle)));
}

void PhysicsBody::setLinearVelocity(const glm::vec3& velocity)
{
    b3Body_SetLinearVelocity(toBodyId(m_handle), toB3(velocity));
}

glm::vec3 PhysicsBody::getAngularVelocity() const
{
    return toGlm(b3Body_GetAngularVelocity(toBodyId(m_handle)));
}

void PhysicsBody::setAngularVelocity(const glm::vec3& radiansPerSecond)
{
    b3Body_SetAngularVelocity(toBodyId(m_handle), toB3(radiansPerSecond));
}

// The trailing `true` on each of these is box3d's "wake the body" flag: a settled body would otherwise ignore
// the impulse/force entirely, which is never what a gameplay call means.
void PhysicsBody::applyImpulse(const glm::vec3& impulse)
{
    b3Body_ApplyLinearImpulseToCenter(toBodyId(m_handle), toB3(impulse), true);
}

void PhysicsBody::applyImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& worldPoint)
{
    b3Body_ApplyLinearImpulse(toBodyId(m_handle), toB3(impulse), toB3(worldPoint), true);
}

void PhysicsBody::applyAngularImpulse(const glm::vec3& impulse)
{
    b3Body_ApplyAngularImpulse(toBodyId(m_handle), toB3(impulse), true);
}

void PhysicsBody::applyForce(const glm::vec3& force)
{
    b3Body_ApplyForceToCenter(toBodyId(m_handle), toB3(force), true);
}

void PhysicsBody::applyForceAtPoint(const glm::vec3& force, const glm::vec3& worldPoint)
{
    b3Body_ApplyForce(toBodyId(m_handle), toB3(force), toB3(worldPoint), true);
}

void PhysicsBody::applyTorque(const glm::vec3& torque)
{
    b3Body_ApplyTorque(toBodyId(m_handle), toB3(torque), true);
}

float PhysicsBody::getMass() const
{
    return b3Body_GetMass(toBodyId(m_handle));
}

glm::vec3 PhysicsBody::getCenterOfMass() const
{
    return toGlm(b3Body_GetWorldCenterOfMass(toBodyId(m_handle)));
}

glm::vec3 PhysicsBody::getPointVelocity(const glm::vec3& worldPoint) const
{
    return toGlm(b3Body_GetWorldPointVelocity(toBodyId(m_handle), toB3(worldPoint)));
}

float PhysicsBody::getGravityScale() const
{
    return b3Body_GetGravityScale(toBodyId(m_handle));
}

void PhysicsBody::setGravityScale(float scale)
{
    b3Body_SetGravityScale(toBodyId(m_handle), scale);
}

float PhysicsBody::getLinearDamping() const
{
    return b3Body_GetLinearDamping(toBodyId(m_handle));
}

void PhysicsBody::setLinearDamping(float damping)
{
    b3Body_SetLinearDamping(toBodyId(m_handle), damping);
}

float PhysicsBody::getAngularDamping() const
{
    return b3Body_GetAngularDamping(toBodyId(m_handle));
}

void PhysicsBody::setAngularDamping(float damping)
{
    b3Body_SetAngularDamping(toBodyId(m_handle), damping);
}

bool PhysicsBody::isAwake() const
{
    return b3Body_IsAwake(toBodyId(m_handle));
}

void PhysicsBody::setAwake(bool awake)
{
    b3Body_SetAwake(toBodyId(m_handle), awake);
}
