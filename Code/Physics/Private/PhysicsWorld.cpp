module;

#include <box3d/box3d.h>

module Physics;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;

import :PhysicsWorld;
import :Body;
import :Joint;
import :Mesh;
import :Types;
import :Layers;
import :Convert;
import :TaskScheduler;

// box3d hands DrawShapeFcn a *user* shape handle, created on demand the first time each shape is
// drawn and destroyed when that shape is modified or removed. Without these callbacks there is no
// handle to pass and shapes are never drawn at all. The handle is a copy of box3d's b3DebugShape:
// its union points at box3d-owned geometry that stays valid for as long as the handle does, which
// is exactly the lifetime destroyDebugShape delimits.
namespace
{
oc::vector<oc::unique_ptr<b3DebugShape>> g_debugShapes;

void* createDebugShapeFcn(const b3DebugShape* debugShape, void*)
{
    return g_debugShapes.emplace_back(oc::make_unique<b3DebugShape>(*debugShape)).get();
}

void destroyDebugShapeFcn(void* userShape, void*)
{
    oc::erase_if(g_debugShapes, [userShape](const oc::unique_ptr<b3DebugShape>& shape) { return shape.get() == userShape; });
}
} // namespace

bool PhysicsWorld::initialize()
{
    if (m_initialized)
        return true;

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = toB3(m_gravity);
    def.createDebugShape = createDebugShapeFcn;
    def.destroyDebugShape = destroyDebugShapeFcn;
    // Solver fork/join rides the engine job system instead of the private thread pool box3d spawns
    // for itself when workerCount > 1 without task callbacks -- see :TaskScheduler.
    m_workerCount = PhysicsTaskScheduler::defaultWorkerCount();
    def.workerCount = uint32(m_workerCount);
    def.enqueueTask = PhysicsTaskScheduler::enqueue;
    def.finishTask = PhysicsTaskScheduler::finish;
    def.userTaskContext = &m_taskScheduler;
    const b3WorldId world = b3CreateWorld(&def);
    if (B3_IS_NULL(world))
    {
        Log::warning("Physics: failed to create box3d world");
        return false;
    }
    m_worldHandle = std::bit_cast<uint32>(world);
    m_initialized = true;

    PhysicsBodyDesc staticDesc;
    staticDesc.type = EPhysicsBodyType::Static;
    m_staticBody = createBody(staticDesc, {});

    Tweak::float3("Physics/World", "Gravity", &m_gravity, 0.05f, [this] { setGravity(m_gravity); });
    Tweak::boolean("Physics/World", "Paused", &m_paused);
    Tweak::boolean("Physics/World", "Interpolate", &m_interpolate);
    Tweak::floatVar("Physics/World", "Time Scale", &m_timeScale, 0.0f, 4.0f);
    Tweak::intVar("Physics/World", "Sub Steps", &m_subSteps, 1, 16);
    Tweak::intVar("Physics/World", "Step Hz", &m_stepHz, 5, 120);
    // Live: box3d re-slices the step from this on the next b3World_Step. 1 = single threaded, which
    // is also the A/B toggle for measuring what the fan-out actually buys on a given scene.
    Tweak::intVar("Physics/World", "Worker count", &m_workerCount, 1, B3_MAX_WORKERS, 1.0f,
        [this] { b3World_SetWorkerCount(std::bit_cast<b3WorldId>(m_worldHandle), m_workerCount); });

    Tweak::floatVar("Physics/Buoyancy", "Density (kg/m3)", &m_waterDensity, 0.0f, 3000.0f, 10.0f);
    Tweak::floatVar("Physics/Buoyancy", "Linear drag", &m_waterLinearDrag, 0.0f, 20.0f, 0.1f);

    Tweak::boolean("Physics/Debug", "Draw colliders", &m_debugDrawEnabled);
    Tweak::boolean("Physics/Debug", "Draw joints", &m_debugDrawJoints);
    Tweak::boolean("Physics/Debug", "Draw contacts", &m_debugDrawContacts);
    Tweak::boolean("Physics/Debug", "Draw bounds", &m_debugDrawBounds);
    Tweak::floatVar("Physics/Debug", "Range", &m_debugDrawRange, 4.0f, 1024.0f);
    return true;
}

void PhysicsWorld::shutdown()
{
    if (!m_initialized)
        return;
    m_staticBody.destroy();
    b3DestroyWorld(std::bit_cast<b3WorldId>(m_worldHandle));
    m_worldHandle = 0;
    m_initialized = false;
}

// Appends the step's buffered contact/sensor begin/end events (box3d clears them on the next step,
// so they are drained after every step, not once per frame). End events may reference destroyed
// shapes and are validated first.
static void dispatchContactEvents(b3WorldId world, oc::function<void(const PhysicsWorld::ContactEvent&)> contactCallback)
{
    auto userDataOf = [](b3ShapeId shape) { return b3Body_GetUserData(b3Shape_GetBody(shape)); };

    const b3ContactEvents contacts = b3World_GetContactEvents(world);
    for (int i = 0; i < contacts.beginCount; ++i)
    {
        const b3ContactBeginTouchEvent& e = contacts.beginEvents[i];
        contactCallback({ userDataOf(e.shapeIdA), userDataOf(e.shapeIdB), true, false, packContactId(e.contactId) });
    }
    for (int i = 0; i < contacts.endCount; ++i)
    {
        const b3ContactEndTouchEvent& e = contacts.endEvents[i];
        if (!b3Shape_IsValid(e.shapeIdA) || !b3Shape_IsValid(e.shapeIdB))
            continue;
        contactCallback({ userDataOf(e.shapeIdA), userDataOf(e.shapeIdB), false, false, packContactId(e.contactId) });
    }

    const b3SensorEvents sensors = b3World_GetSensorEvents(world);
    for (int i = 0; i < sensors.beginCount; ++i)
    {
        const b3SensorBeginTouchEvent& e = sensors.beginEvents[i];
        contactCallback({ userDataOf(e.sensorShapeId), userDataOf(e.visitorShapeId), true, true, 0 });
    }
    for (int i = 0; i < sensors.endCount; ++i)
    {
        const b3SensorEndTouchEvent& e = sensors.endEvents[i];
        if (!b3Shape_IsValid(e.sensorShapeId) || !b3Shape_IsValid(e.visitorShapeId))
            continue;
        contactCallback({ userDataOf(e.sensorShapeId), userDataOf(e.visitorShapeId), false, true, 0 });
    }
}

void PhysicsWorld::update(double deltaSec, oc::function<void(const ContactEvent&)> contactCallback)
{
    if (!m_initialized)
        return;
    ProfileScope updateScope("Physics", EProfileCategory::Physics);

    applyQueuedCommands(); // before the paused check: placing a body / setting its state is authoring, not simulation

    if (!m_paused)
        stepSimulation(deltaSec, contactCallback);

    // After the steps, so the wireframes match the poses the entities will render from this frame -- and
    // deliberately NOT gated on paused, since inspecting colliders with the simulation stopped is the point.
    if (m_debugDrawEnabled && m_debugLine && m_debugViewPos)
    {
        ProfileScope profileScope("Collider debug draw", EProfileCategory::Physics);
        debugDraw(m_debugViewPos(), m_debugLine);
    }
}

void PhysicsWorld::stepSimulation(double deltaSec, const oc::function<void(const ContactEvent&)>& contactCallback)
{
    m_accumulator += float(deltaSec) * m_timeScale;
    const float step = 1.0f / float(m_stepHz);
    const b3WorldId world = std::bit_cast<b3WorldId>(m_worldHandle);

    constexpr int maxCatchUpSteps = 4;
    int steps = 0;
    while (m_accumulator >= step && steps < maxCatchUpSteps)
    {
        ProfileScope profileScope("Physics step", EProfileCategory::Physics);
        if (m_waterSurface && (!m_waterActive || m_waterActive()))
        {
            ProfileScope buoyancyScope("Buoyancy", EProfileCategory::Physics);
            applyBuoyancy(); // box3d clears applied forces every step, so this runs per step
        }
        b3World_Step(world, step, m_subSteps);
        ++m_stepCount;
        dispatchContactEvents(world, contactCallback);
        m_accumulator -= step;
        ++steps;
    }
    if (m_accumulator >= step)
        m_accumulator = 0.0f; // drop the remainder after a hitch instead of spiraling
}

void PhysicsWorld::teleportBody(const PhysicsBody& body, const glm::vec3& pos, const glm::quat& rot)
{
    if (!body.isValid())
        return;
    const std::lock_guard<std::mutex> lock(m_commandMutex);
    m_commands.push_back(BodyCommand{ .bodyHandle = body.m_handle, .type = EBodyCommand::Teleport, .a = pos, .rot = glm::normalize(rot) });
}

void PhysicsWorld::queueBodyCommand(const PhysicsBody& body, EBodyCommand type, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    if (!body.isValid())
        return;
    const std::lock_guard<std::mutex> lock(m_commandMutex);
    m_commands.push_back(BodyCommand{ .bodyHandle = body.m_handle, .type = type, .a = a, .b = b, .c = c });
}

void PhysicsWorld::queueSetGravity(const glm::vec3& gravity)
{
    const std::lock_guard<std::mutex> lock(m_commandMutex);
    m_commands.push_back(BodyCommand{ .type = EBodyCommand::SetWorldGravity, .a = gravity });
}

void PhysicsWorld::applyQueuedCommands()
{
    m_commandScratch.clear();
    {
        const std::lock_guard<std::mutex> lock(m_commandMutex);
        m_commandScratch.swap(m_commands); // box3d runs outside the lock; producers keep queueing meanwhile
    }

    for (const BodyCommand& command : m_commandScratch)
    {
        if (command.type == EBodyCommand::SetWorldGravity)
        {
            setGravity(command.a);
            continue;
        }
        const b3BodyId id = toBodyId(command.bodyHandle);
        if (!b3Body_IsValid(id)) // the body may have died since the queue call
            continue;
        switch (command.type)
        {
        case EBodyCommand::Teleport:            b3Body_SetTransform(id, toB3(command.a), toB3(command.rot)); break;
        case EBodyCommand::SetLinearVelocity:   b3Body_SetLinearVelocity(id, toB3(command.a)); break;
        case EBodyCommand::SetAngularVelocity:  b3Body_SetAngularVelocity(id, toB3(command.a)); break;
        case EBodyCommand::ApplyImpulse:        b3Body_ApplyLinearImpulseToCenter(id, toB3(command.a), true); break;
        case EBodyCommand::ApplyImpulseAtPoint: b3Body_ApplyLinearImpulse(id, toB3(command.a), toB3(command.b), true); break;
        case EBodyCommand::ApplyAngularImpulse: b3Body_ApplyAngularImpulse(id, toB3(command.a), true); break;
        case EBodyCommand::ApplyForce:          b3Body_ApplyForceToCenter(id, toB3(command.a), true); break;
        case EBodyCommand::ApplyForceAtPoint:   b3Body_ApplyForce(id, toB3(command.a), toB3(command.b), true); break;
        case EBodyCommand::ApplyTorque:         b3Body_ApplyTorque(id, toB3(command.a), true); break;
        case EBodyCommand::SetGravityScale:     b3Body_SetGravityScale(id, command.a.x); break;
        case EBodyCommand::SetLinearDamping:    b3Body_SetLinearDamping(id, command.a.x); break;
        case EBodyCommand::SetAngularDamping:   b3Body_SetAngularDamping(id, command.a.x); break;
        case EBodyCommand::SetAwake:            b3Body_SetAwake(id, command.a.x != 0.0f); break;
        case EBodyCommand::NudgeVelocity:
        {
            const auto capped = [](const glm::vec3& delta, float maxLen)
            {
                const float len = glm::length(delta);
                return (len > maxLen && len > 1e-6f) ? delta * (maxLen / len) : delta;
            };
            const glm::vec3 dv = capped(command.a - toGlm(b3Body_GetLinearVelocity(id)), command.c.x);
            b3Body_ApplyLinearImpulseToCenter(id, toB3(dv * b3Body_GetMass(id)), true);
            const glm::vec3 angVel = toGlm(b3Body_GetAngularVelocity(id));
            b3Body_SetAngularVelocity(id, toB3(angVel + capped(command.b - angVel, command.c.y)));
            break;
        }
        case EBodyCommand::SetWorldGravity:     break; // handled above
        }
    }
}

void PhysicsWorld::applyBuoyancy()
{
    // Probe-based buoyancy: every dynamic shape near the water splits its world AABB into a 2x2x2 grid
    // of probes; each probe carries its share of the shape's EXACT volume (box3d mass data / density),
    // scaled by how deep it sits, as an anti-gravity force at the probe point — Archimedes, and since
    // the force applies off-center, righting torque and tumbling damping emerge for free — plus a drag
    // force against the probe's point velocity (which damps rotation the same way). Broadphase does the
    // body iteration (box3d has no body-list API): one whole-world AABB overlap, dynamics filtered, and
    // a one-sample early out per shape keeps dry bodies at a single water query.
    const b3WorldId world = std::bit_cast<b3WorldId>(m_worldHandle);

    m_buoyancyShapes.clear();
    constexpr float B = 1e9f;
    const b3AABB everything{ { -B, -B, -B }, { B, B, B } };
    b3World_OverlapAABB(world, everything, b3DefaultQueryFilter(),
        [](b3ShapeId shape, void* context) {
            static_cast<oc::vector<uint64>*>(context)->push_back(b3StoreShapeId(shape));
            return true;
        }, &m_buoyancyShapes);

    for (const uint64 shapeBits : m_buoyancyShapes)
    {
        const b3ShapeId shape = b3LoadShapeId(shapeBits);
        const b3BodyId body = b3Shape_GetBody(shape);
        if (b3Body_GetType(body) != b3_dynamicBody || b3Shape_IsSensor(shape))
            continue;
        const float density = b3Shape_GetDensity(shape);
        if (density <= 0.0f)
            continue;

        const b3AABB aabb = b3Shape_GetAABB(shape);
        // Early out: whole shape above the local surface (1m margin covers the wave slope across the AABB).
        const float waterAtCenter = m_waterSurface(
            (aabb.lowerBound.x + aabb.upperBound.x) * 0.5f, (aabb.lowerBound.z + aabb.upperBound.z) * 0.5f);
        if (aabb.lowerBound.y > waterAtCenter + 1.0f)
            continue;

        const float volume = b3Shape_ComputeMassData(shape).mass / density;
        const float cellVolume = volume * (1.0f / 8.0f);
        const float cellHeight = glm::max((aabb.upperBound.y - aabb.lowerBound.y) * 0.5f, 0.01f);
        const glm::vec3 lo = toGlm(aabb.lowerBound), hi = toGlm(aabb.upperBound);
        for (uint32 i = 0; i < 8; ++i)
        {
            const glm::vec3 probe = glm::mix(lo, hi,
                glm::vec3(0.25f) + 0.5f * glm::vec3(float(i & 1u), float((i >> 1) & 1u), float((i >> 2) & 1u)));
            const float waterY = m_waterSurface(probe.x, probe.z);
            const float submerged = glm::clamp((waterY - probe.y) / cellHeight + 0.5f, 0.0f, 1.0f);
            if (submerged <= 0.0f)
                continue;
            const float displacedMass = m_waterDensity * cellVolume * submerged;
            glm::vec3 force = -m_gravity * displacedMass; // Archimedes: weight of the displaced water, upward
            const glm::vec3 pointVel = toGlm(b3Body_GetWorldPointVelocity(body, toB3(probe)));
            force -= pointVel * (displacedMass * m_waterLinearDrag);
            b3Body_ApplyForce(body, toB3(force), toB3(probe), true);
        }
    }
}

float PhysicsWorld::getInterpolationAlpha() const
{
    if (!m_interpolate)
        return 1.0f;
    return glm::clamp(m_accumulator * float(m_stepHz), 0.0f, 1.0f);
}

PhysicsBody PhysicsWorld::createBody(const PhysicsBodyDesc& desc, oc::span<const PhysicsShape> shapes)
{
    assert(m_initialized);

    b3BodyDef bodyDef = b3DefaultBodyDef();
    switch (desc.type)
    {
    case EPhysicsBodyType::Static:    bodyDef.type = b3_staticBody;    break;
    case EPhysicsBodyType::Kinematic: bodyDef.type = b3_kinematicBody; break;
    case EPhysicsBodyType::Dynamic:   bodyDef.type = b3_dynamicBody;   break;
    }
    bodyDef.position = toB3(desc.transform.pos);
    bodyDef.rotation = toB3(glm::normalize(desc.transform.quat));
    bodyDef.linearVelocity = toB3(desc.linearVelocity);
    bodyDef.userData = desc.userData;
    bodyDef.motionLocks.angularX = desc.lockRotation;
    bodyDef.motionLocks.angularY = desc.lockRotation;
    bodyDef.motionLocks.angularZ = desc.lockRotation;

    const b3BodyId body = b3CreateBody(std::bit_cast<b3WorldId>(m_worldHandle), &bodyDef);
    if (B3_IS_NULL(body))
        return PhysicsBody();

    const float scale = desc.transform.scale;
    for (const PhysicsShape& shape : shapes)
    {
        b3ShapeDef shapeDef = b3DefaultShapeDef();
        shapeDef.density = shape.density;
        shapeDef.baseMaterial.friction = shape.friction;
        shapeDef.baseMaterial.restitution = shape.restitution;
        shapeDef.filter.categoryBits = shape.categoryBits;
        shapeDef.filter.maskBits = shape.maskBits;
        shapeDef.filter.groupIndex = shape.groupIndex;
        shapeDef.isSensor = shape.isSensor;
        shapeDef.enableSensorEvents = true; // visitors must opt in for sensors to see them; always on so sensors just work
        shapeDef.enableContactEvents = shape.contactEvents;

        const glm::vec3 offset = shape.offset * scale;
        switch (shape.type)
        {
        case EPhysicsShapeType::Box:
        {
            const glm::vec3 he = shape.halfExtents * scale;
            const b3BoxHull hull = b3MakeOffsetBoxHull(he.x, he.y, he.z, toB3(offset));
            b3CreateHullShape(body, &shapeDef, &hull.base);
            break;
        }
        case EPhysicsShapeType::Sphere:
        {
            const b3Sphere sphere{ toB3(offset), shape.radius * scale };
            b3CreateSphereShape(body, &shapeDef, &sphere);
            break;
        }
        case EPhysicsShapeType::Capsule:
        {
            const glm::vec3 axis(0.0f, shape.halfHeight * scale, 0.0f);
            const b3Capsule capsule{ toB3(offset - axis), toB3(offset + axis), shape.radius * scale };
            b3CreateCapsuleShape(body, &shapeDef, &capsule);
            break;
        }
        case EPhysicsShapeType::Hull:
        {
            if (shape.hullPoints.size() < 4)
            {
                Log::warning("Physics: hull shape needs at least 4 points");
                break;
            }
            oc::vector<b3Vec3> points;
            points.reserve(shape.hullPoints.size());
            for (const glm::vec3& p : shape.hullPoints)
                points.push_back(toB3(p * scale + offset));
            if (b3HullData* hull = b3CreateHull(points.data(), int(points.size()), glm::clamp(shape.maxHullVertices, 4, 64)))
            {
                b3CreateHullShape(body, &shapeDef, hull); // the shape clones the hull
                b3DestroyHull(hull);
            }
            else
                Log::warning("Physics: convex hull creation failed (degenerate point cloud?)");
            break;
        }
        case EPhysicsShapeType::Mesh:
        {
            if (!shape.mesh || !shape.mesh->isValid())
            {
                Log::warning("Physics: mesh shape without collision mesh data");
                break;
            }
            if (desc.type != EPhysicsBodyType::Static)
                Log::warning("Physics: mesh colliders only generate contacts on static bodies");
            const b3MeshData* meshData = reinterpret_cast<const b3MeshData*>(shape.mesh->m_data);
            b3CreateMeshShape(body, &shapeDef, meshData, b3Vec3{ scale, scale, scale });
            break;
        }
        }
    }
    return PhysicsBody(std::bit_cast<uint64>(body));
}

PhysicsMesh PhysicsWorld::createCollisionMesh(oc::span<const glm::vec3> vertices, oc::span<const uint32> indices)
{
    PhysicsMesh outMesh;
    if (vertices.size() < 3 || indices.size() < 3)
        return outMesh;

    b3MeshDef def{}; // the input arrays are only read during creation
    def.vertices = reinterpret_cast<b3Vec3*>(const_cast<glm::vec3*>(vertices.data()));
    def.indices = reinterpret_cast<int32_t*>(const_cast<uint32*>(indices.data()));
    def.vertexCount = int(vertices.size());
    def.triangleCount = int(indices.size() / 3);
    def.weldVertices = true;
    def.weldTolerance = 0.001f;
    def.identifyEdges = true; // smooth contacts across internal edges

    b3MeshData* data = b3CreateMesh(&def, nullptr, 0);
    if (!data)
    {
        Log::warning("Physics: collision mesh creation failed");
        return outMesh;
    }
    outMesh.m_data = reinterpret_cast<uint64>(data);
    return outMesh;
}

// Quaternion rotating the +Z axis (box3d's hinge/cone axis convention) onto `axis`.
static glm::quat quatFromZAxis(glm::vec3 axis)
{
    axis = glm::normalize(axis);
    const glm::vec3 z(0.0f, 0.0f, 1.0f);
    const float d = glm::dot(z, axis);
    if (d > 0.9999f)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.9999f)
        return glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 c = glm::cross(z, axis);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// World-space joint frame expressed in the body's local space.
static b3Transform localJointFrame(const PhysicsBody& body, const glm::vec3& worldAnchor, const glm::quat& worldRot)
{
    const glm::quat invRot = glm::conjugate(body.getRotation());
    return b3Transform{ toB3(invRot * (worldAnchor - body.getPosition())), toB3(glm::normalize(invRot * worldRot)) };
}

static void fillJointBase(b3JointDef& base, const PhysicsBody& a, const PhysicsBody& b, uint64 handleA, uint64 handleB,
    const glm::vec3& anchorA, const glm::vec3& anchorB, const glm::quat& worldRot)
{
    base.bodyIdA = toBodyId(handleA);
    base.bodyIdB = toBodyId(handleB);
    base.localFrameA = localJointFrame(a, anchorA, worldRot);
    base.localFrameB = localJointFrame(b, anchorB, worldRot);
    base.collideConnected = false;
}

PhysicsJoint PhysicsWorld::createDistanceJoint(const PhysicsBody& a, const PhysicsBody& b,
    const glm::vec3& anchorA, const glm::vec3& anchorB, float minLength, float maxLength)
{
    assert(m_initialized && a.isValid() && b.isValid());
    b3DistanceJointDef def = b3DefaultDistanceJointDef();
    fillJointBase(def.base, a, b, a.m_handle, b.m_handle, anchorA, anchorB, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    def.length = glm::distance(anchorA, anchorB);
    def.enableLimit = true;
    def.minLength = glm::min(minLength, maxLength);
    def.maxLength = glm::max(minLength, maxLength);
    return PhysicsJoint(std::bit_cast<uint64>(b3CreateDistanceJoint(std::bit_cast<b3WorldId>(m_worldHandle), &def)));
}

PhysicsJoint PhysicsWorld::createRevoluteJoint(const PhysicsBody& a, const PhysicsBody& b,
    const glm::vec3& pivot, const glm::vec3& axis, float lowerDeg, float upperDeg)
{
    assert(m_initialized && a.isValid() && b.isValid());
    b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
    fillJointBase(def.base, a, b, a.m_handle, b.m_handle, pivot, pivot, quatFromZAxis(axis));
    if (lowerDeg > -FLT_MAX * 0.5f || upperDeg < FLT_MAX * 0.5f)
    {
        def.enableLimit = true;
        def.lowerAngle = glm::radians(glm::max(lowerDeg, -178.0f));
        def.upperAngle = glm::radians(glm::min(upperDeg, 178.0f));
    }
    return PhysicsJoint(std::bit_cast<uint64>(b3CreateRevoluteJoint(std::bit_cast<b3WorldId>(m_worldHandle), &def)));
}

PhysicsJoint PhysicsWorld::createSphericalJoint(const PhysicsBody& a, const PhysicsBody& b,
    const glm::vec3& pivot, float coneAngleDeg)
{
    assert(m_initialized && a.isValid() && b.isValid());
    b3SphericalJointDef def = b3DefaultSphericalJointDef();
    fillJointBase(def.base, a, b, a.m_handle, b.m_handle, pivot, pivot, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    if (coneAngleDeg < 180.0f)
    {
        def.enableConeLimit = true;
        def.coneAngle = glm::radians(glm::clamp(coneAngleDeg, 0.0f, 180.0f));
    }
    return PhysicsJoint(std::bit_cast<uint64>(b3CreateSphericalJoint(std::bit_cast<b3WorldId>(m_worldHandle), &def)));
}

PhysicsJoint PhysicsWorld::createWeldJoint(const PhysicsBody& a, const PhysicsBody& b, const glm::vec3& anchor)
{
    assert(m_initialized && a.isValid() && b.isValid());
    b3WeldJointDef def = b3DefaultWeldJointDef();
    fillJointBase(def.base, a, b, a.m_handle, b.m_handle, anchor, anchor, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    return PhysicsJoint(std::bit_cast<uint64>(b3CreateWeldJoint(std::bit_cast<b3WorldId>(m_worldHandle), &def)));
}

namespace
{
    struct FilteredRayContext
    {
        uint64 ignoreBodyBits = 0;
        bool staticOnly = false;
        PhysicsWorld::RayHit hit;
    };
    // closest-hit semantics per the b3CastResultFcn contract: returning the fraction clips the ray,
    // so later callbacks only fire for strictly closer hits; -1 filters a shape out
    float filteredClosestRayFcn(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context)
    {
        FilteredRayContext* ctx = static_cast<FilteredRayContext*>(context);
        const b3BodyId body = b3Shape_GetBody(shapeId);
        if (std::bit_cast<uint64>(body) == ctx->ignoreBodyBits)
            return -1.0f;
        if (ctx->staticOnly && b3Body_GetType(body) != b3_staticBody)
            return -1.0f;
        ctx->hit.hit = true;
        ctx->hit.fraction = fraction;
        ctx->hit.point = toGlm(point);
        ctx->hit.normal = toGlm(normal);
        return fraction;
    }
}

PhysicsWorld::RayHit PhysicsWorld::castRayClosest(const glm::vec3& origin, const glm::vec3& translation, uint64 maskBits,
    const PhysicsBody* ignoreBody, bool staticOnly) const
{
    RayHit outHit;
    if (!m_initialized)
        return outHit;
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = PhysicsLayers::All; // pass every shape's own mask; maskBits alone decides what the ray sees
    filter.maskBits = maskBits;
    if ((ignoreBody != nullptr && ignoreBody->isValid()) || staticOnly)
    {
        FilteredRayContext context{ .ignoreBodyBits = ignoreBody && ignoreBody->isValid() ? ignoreBody->m_handle : 0, .staticOnly = staticOnly };
        b3World_CastRay(std::bit_cast<b3WorldId>(m_worldHandle), toB3(origin), toB3(translation), filter,
            &filteredClosestRayFcn, &context);
        return context.hit;
    }
    const b3RayResult result = b3World_CastRayClosest(std::bit_cast<b3WorldId>(m_worldHandle),
        toB3(origin), toB3(translation), filter);
    outHit.hit = result.hit;
    outHit.fraction = result.fraction;
    outHit.point = toGlm(result.point);
    outHit.normal = toGlm(result.normal);
    return outHit;
}

bool PhysicsWorld::getContactPoint(int64 contactId, glm::vec3& outPoint, glm::vec3& outNormal) const
{
    if (!m_initialized || contactId == 0)
        return false;

    b3ContactId id{};
    id.index1 = (int32)((uint64)contactId >> 32);
    id.world0 = std::bit_cast<b3WorldId>(m_worldHandle).index1;
    id.padding = 0;
    id.generation = (uint32)((uint64)contactId & 0xffffffffu);
    if (!b3Contact_IsValid(id))
        return false;

    const b3ContactData data = b3Contact_GetData(id);
    if (data.manifoldCount == 0 || data.manifolds[0].pointCount == 0)
        return false;

    const b3Manifold& manifold = data.manifolds[0];
    const glm::vec3 comA = toGlm(b3Body_GetWorldCenterOfMass(b3Shape_GetBody(data.shapeIdA)));
    outPoint = comA + toGlm(manifold.points[0].anchorA);
    outNormal = toGlm(manifold.normal);
    return true;
}

// ---- Collider wireframe debug draw ----------------------------------------------------------------
// Everything below decomposes box3d's debug-draw callbacks into world-space line segments pushed
// through the caller's DebugLineFn, so no renderer types leak into Physics.

namespace
{
struct DebugDrawContext
{
    const PhysicsWorld::DebugLineFn* line;
    glm::vec3 viewPos;
    float rangeSq;
};

uint32 toRGBA(b3HexColor color) // 0xRRGGBB (material preset in the high byte, ignored) -> RGBA8, R low byte
{
    const uint32 c = (uint32)color;
    return ((c >> 16) & 0xffu) | (((c >> 8) & 0xffu) << 8) | ((c & 0xffu) << 16) | 0xff000000u;
}

void emit(const DebugDrawContext& ctx, const glm::vec3& a, const glm::vec3& b, uint32 rgba)
{
    (*ctx.line)(a, b, rgba);
}

// Circle of N segments around `axis` at `center`; u/v span its plane.
void wireCircle(const DebugDrawContext& ctx, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v, float radius, uint32 rgba)
{
    constexpr int N = 16;
    glm::vec3 prev = center + u * radius;
    for (int i = 1; i <= N; ++i)
    {
        const float a = float(i) * (2.0f * 3.14159265f / float(N));
        const glm::vec3 p = center + (u * cosf(a) + v * sinf(a)) * radius;
        emit(ctx, prev, p, rgba);
        prev = p;
    }
}

void wireSphere(const DebugDrawContext& ctx, const glm::vec3& center, const glm::quat& rot, float radius, uint32 rgba)
{
    const glm::vec3 x = rot * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 y = rot * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 z = rot * glm::vec3(0.0f, 0.0f, 1.0f);
    wireCircle(ctx, center, y, z, radius, rgba);
    wireCircle(ctx, center, z, x, radius, rgba);
    wireCircle(ctx, center, x, y, radius, rgba);
}

void wireCapsule(const DebugDrawContext& ctx, const glm::vec3& p1, const glm::vec3& p2, float radius, uint32 rgba)
{
    const glm::vec3 d = p2 - p1;
    const float len = glm::length(d);
    if (len < 1e-6f)
    {
        wireSphere(ctx, p1, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), radius, rgba);
        return;
    }
    const glm::vec3 axis = d / len;
    const glm::vec3 ref = fabsf(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 u = glm::normalize(glm::cross(axis, ref));
    const glm::vec3 v = glm::cross(axis, u);
    wireCircle(ctx, p1, u, v, radius, rgba);
    wireCircle(ctx, p2, u, v, radius, rgba);
    for (int i = 0; i < 4; ++i)
    {
        const float a = float(i) * (3.14159265f * 0.5f);
        const glm::vec3 side = (u * cosf(a) + v * sinf(a)) * radius;
        emit(ctx, p1 + side, p2 + side, rgba);
    }
    // Hemisphere cap arcs in the two planes containing the axis: each arc runs side -> -side,
    // bulging along +axis at p2 and -axis at p1.
    constexpr int N = 8;
    for (int half = 0; half < 2; ++half)
    {
        const glm::vec3& side = half == 0 ? u : v;
        glm::vec3 prevTop = p2 + side * radius;
        glm::vec3 prevBot = p1 + side * radius;
        for (int i = 1; i <= N; ++i)
        {
            const float a = float(i) * (3.14159265f / float(N));
            const glm::vec3 c = side * (cosf(a) * radius);
            const glm::vec3 s = axis * (sinf(a) * radius);
            const glm::vec3 top = p2 + c + s;
            const glm::vec3 bot = p1 + c - s;
            emit(ctx, prevTop, top, rgba);
            emit(ctx, prevBot, bot, rgba);
            prevTop = top;
            prevBot = bot;
        }
    }
}

void wireBounds(const DebugDrawContext& ctx, const glm::vec3& lo, const glm::vec3& hi, uint32 rgba)
{
    const glm::vec3 c[8] = {
        { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
        { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    constexpr int e[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
    for (auto& [a, b] : e)
        emit(ctx, c[a], c[b], rgba);
}

glm::vec3 xfPoint(const b3WorldTransform& xf, const glm::quat& rot, const glm::vec3& p)
{
    return toGlm(xf.p) + rot * p;
}

void drawHull(const DebugDrawContext& ctx, const b3HullData* hull, const b3WorldTransform& xf, uint32 rgba)
{
    const b3Vec3* points = b3GetHullPoints(hull);
    const b3HullHalfEdge* edges = b3GetHullEdges(hull);
    if (!points || !edges)
        return;
    const glm::quat rot = toGlm(xf.q);
    for (int i = 0; i < hull->edgeCount; ++i)
    {
        const b3HullHalfEdge& e = edges[i];
        if (i > (int)e.twin)
            continue; // each edge pair once
        emit(ctx, xfPoint(xf, rot, toGlm(points[e.origin])), xfPoint(xf, rot, toGlm(points[edges[e.twin].origin])), rgba);
    }
}

void drawMesh(const DebugDrawContext& ctx, const b3Mesh& mesh, const b3WorldTransform& xf, uint32 rgba)
{
    const b3Vec3* verts = b3GetMeshVertices(mesh.data);
    const b3MeshTriangle* tris = b3GetMeshTriangles(mesh.data);
    if (!verts || !tris)
        return;
    const glm::quat rot = toGlm(xf.q);
    const glm::vec3 scale = toGlm(mesh.scale);
    for (int t = 0; t < mesh.data->triangleCount; ++t)
    {
        const glm::vec3 a = xfPoint(xf, rot, toGlm(verts[tris[t].index1]) * scale);
        const glm::vec3 b = xfPoint(xf, rot, toGlm(verts[tris[t].index2]) * scale);
        const glm::vec3 c = xfPoint(xf, rot, toGlm(verts[tris[t].index3]) * scale);
        // A single mesh shape can be huge (Sponza): cull per triangle so only wireframe near the
        // camera is emitted (the world-level drawingBounds only culls whole shapes).
        auto distSq = [&ctx](const glm::vec3& p) { const glm::vec3 d = p - ctx.viewPos; return glm::dot(d, d); };
        if (glm::min(glm::min(distSq(a), distSq(b)), distSq(c)) > ctx.rangeSq)
            continue;
        emit(ctx, a, b, rgba);
        emit(ctx, b, c, rgba);
        emit(ctx, c, a, rgba);
    }
}

bool drawShapeFcn(void* userShape, b3WorldTransform transform, b3HexColor color, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    const b3DebugShape& shape = *static_cast<const b3DebugShape*>(userShape);
    const uint32 rgba = toRGBA(color);
    const glm::quat rot = toGlm(transform.q);
    switch (shape.type)
    {
    case b3_sphereShape:
        wireSphere(ctx, xfPoint(transform, rot, toGlm(shape.sphere->center)), rot, shape.sphere->radius, rgba);
        break;
    case b3_capsuleShape:
        wireCapsule(ctx, xfPoint(transform, rot, toGlm(shape.capsule->center1)), xfPoint(transform, rot, toGlm(shape.capsule->center2)), shape.capsule->radius, rgba);
        break;
    case b3_hullShape:
        drawHull(ctx, shape.hull, transform, rgba);
        break;
    case b3_meshShape:
        drawMesh(ctx, *shape.mesh, transform, rgba);
        break;
    default: // compound/height-field: never created by this engine's PhysicsShape set
        break;
    }
    return false; // fully drawn here; skip box3d's own drawing of this shape
}

void drawSegmentFcn(b3Pos p1, b3Pos p2, b3HexColor color, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    emit(ctx, toGlm(p1), toGlm(p2), toRGBA(color));
}

void drawTransformFcn(b3WorldTransform transform, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    const glm::vec3 p = toGlm(transform.p);
    const glm::quat rot = toGlm(transform.q);
    constexpr float axisLen = 0.25f;
    emit(ctx, p, p + rot * glm::vec3(axisLen, 0.0f, 0.0f), 0xff0000ffu);
    emit(ctx, p, p + rot * glm::vec3(0.0f, axisLen, 0.0f), 0xff00ff00u);
    emit(ctx, p, p + rot * glm::vec3(0.0f, 0.0f, axisLen), 0xffff0000u);
}

void drawPointFcn(b3Pos p, float size, b3HexColor color, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    const glm::vec3 c = toGlm(p);
    const float h = glm::max(size, 0.02f) * 0.5f;
    const uint32 rgba = toRGBA(color);
    emit(ctx, c - glm::vec3(h, 0.0f, 0.0f), c + glm::vec3(h, 0.0f, 0.0f), rgba);
    emit(ctx, c - glm::vec3(0.0f, h, 0.0f), c + glm::vec3(0.0f, h, 0.0f), rgba);
    emit(ctx, c - glm::vec3(0.0f, 0.0f, h), c + glm::vec3(0.0f, 0.0f, h), rgba);
}

void drawSphereFcn(b3Pos p, float radius, b3HexColor color, float, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    wireSphere(ctx, toGlm(p), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), radius, toRGBA(color));
}

void drawCapsuleFcn(b3Pos p1, b3Pos p2, float radius, b3HexColor color, float, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    wireCapsule(ctx, toGlm(p1), toGlm(p2), radius, toRGBA(color));
}

void drawBoundsFcn(b3AABB aabb, b3HexColor color, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    wireBounds(ctx, toGlm(aabb.lowerBound), toGlm(aabb.upperBound), toRGBA(color));
}

void drawBoxFcn(b3Vec3 extents, b3WorldTransform transform, b3HexColor color, void* context)
{
    const DebugDrawContext& ctx = *static_cast<const DebugDrawContext*>(context);
    const glm::quat rot = toGlm(transform.q);
    const glm::vec3 e = toGlm(extents);
    const uint32 rgba = toRGBA(color);
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = xfPoint(transform, rot, glm::vec3(i & 1 ? e.x : -e.x, i & 2 ? e.y : -e.y, i & 4 ? e.z : -e.z));
    constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
    for (auto& [a, b] : edges)
        emit(ctx, c[a], c[b], rgba);
}

void drawStringFcn(b3Pos, const char*, b3HexColor, void*) {}
} // namespace

void PhysicsWorld::debugDraw(const glm::vec3& viewPos, const DebugLineFn& line)
{
    if (!m_initialized || !m_debugDrawEnabled)
        return;

    DebugDrawContext ctx{ .line = &line, .viewPos = viewPos, .rangeSq = m_debugDrawRange * m_debugDrawRange };

    b3DebugDraw draw = b3DefaultDebugDraw();
    draw.DrawShapeFcn = drawShapeFcn;
    draw.DrawSegmentFcn = drawSegmentFcn;
    draw.DrawTransformFcn = drawTransformFcn;
    draw.DrawPointFcn = drawPointFcn;
    draw.DrawSphereFcn = drawSphereFcn;
    draw.DrawCapsuleFcn = drawCapsuleFcn;
    draw.DrawBoundsFcn = drawBoundsFcn;
    draw.DrawBoxFcn = drawBoxFcn;
    draw.DrawStringFcn = drawStringFcn;
    draw.drawingBounds = b3AABB{ toB3(viewPos - glm::vec3(m_debugDrawRange)), toB3(viewPos + glm::vec3(m_debugDrawRange)) };
    draw.drawShapes = true;
    draw.drawJoints = m_debugDrawJoints;
    draw.drawContacts = m_debugDrawContacts;
    draw.drawContactNormals = m_debugDrawContacts;
    draw.drawBounds = m_debugDrawBounds;
    draw.context = &ctx;

    b3World_Draw(std::bit_cast<b3WorldId>(m_worldHandle), &draw, PhysicsLayers::All);
}

void PhysicsWorld::setGravity(const glm::vec3& gravity)
{
    m_gravity = gravity;
    if (m_initialized)
        b3World_SetGravity(std::bit_cast<b3WorldId>(m_worldHandle), toB3(m_gravity));
}
