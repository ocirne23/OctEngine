module;

#include <box3d/box3d.h>

module Physics;

import :Joint;

void PhysicsJoint::destroy()
{
    if (m_handle == 0)
        return;
    const b3JointId id = oc::bitCast<b3JointId>(m_handle);
    if (b3Joint_IsValid(id)) // destroying an attached body already destroyed the joint
        b3DestroyJoint(id, true);
    m_handle = 0;
}
