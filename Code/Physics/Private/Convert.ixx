module;

#include <box3d/box3d.h>

export module Physics:Convert;

import Core;
import Core.glm;

// box3d ABI assumptions the handle bit-casts below rely on.
static_assert(sizeof(b3BodyId) == sizeof(uint64));
static_assert(sizeof(b3JointId) == sizeof(uint64));
static_assert(sizeof(b3WorldId) == sizeof(uint32));
static_assert(sizeof(b3Vec3) == sizeof(glm::vec3));

export b3Vec3 toB3(const glm::vec3& v) { return b3Vec3{ v.x, v.y, v.z }; }
export b3Quat toB3(const glm::quat& q) { return b3Quat{ { q.x, q.y, q.z }, q.w }; }
export glm::vec3 toGlm(const b3Vec3& v) { return glm::vec3(v.x, v.y, v.z); }
export glm::quat toGlm(const b3Quat& q) { return glm::quat(q.s, q.v.x, q.v.y, q.v.z); }

export b3BodyId toBodyId(uint64 handle) { return oc::bitCast<b3BodyId>(handle); }

// b3ContactId is 3 fields (index1/world0/generation) and doesn't fit a single bit_cast-able integer; world0
// is always this process's one physics world, so it's dropped here and re-supplied from the world handle when
// unpacking (see PhysicsWorld::getContactPoint). Packing is lossless for index1/generation, so this is valid
// for as long as box3d itself guarantees the id (until the next world step recycles it).
export int64 packContactId(b3ContactId id)
{
    return (int64)(((uint64)(uint32)id.index1 << 32) | (uint64)id.generation);
}
