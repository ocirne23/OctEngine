module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Physics;
import Spatial;
import Threading;

// Buoyancy catch-up bound: the scheduling below owes at most two steps, so a bigger gap means the
// component was out of the loop entirely (spawned, parked, suspended, or gated off by the SIM LOD
// tier) and starts fresh instead of firing one huge force.
constexpr uint32 c_maxBuoyancyCatchUpSteps = 4;

void PhysicsComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    enabled = info.enabled;
    bodyType = info.bodyType;
    lockRotation = info.lockRotation;
    buoyancyStep = uint16(Globals::physics.getStepCount());

    // `base` is parent-local for a prefab child; the ancestor chain is already positioned by now.
    Transform world = base;
    for (const Entity* p = entity.parent; p; p = p->parent)
        world = composeTransform(Transform(p->pos, p->scale, p->rot), world);

    PhysicsBodyDesc desc;
    desc.type = info.bodyType;
    desc.transform = world;
    desc.userData = &entity;
    desc.lockRotation = info.lockRotation;
    body = Globals::physics.createBody(desc, oc::span(&info.shape, 1));
    // The exact displaced volume from box3d's mass (the world scale is baked into the shape).
    if (info.bodyType == EPhysicsBodyType::Dynamic && !info.shape.isSensor && info.shape.density > 0.0f && body.isValid())
        buoyancyVolume = body.getMass() / info.shape.density;

    if (info.bodyType == EPhysicsBodyType::Static && info.occluders)
        occluder = SpatialOccluder(Globals::occlusionBuffer.addOccluder(info.occluders, world));
}

void PhysicsComponent::destroy(Entity& entity, const SpawnInfo& info)
{
    body.destroy(); // removes the collider from the box3d world (shapes die with the body)
    occluder.reset();
}

void PhysicsComponent::suspendBody()
{
    if (!body.isValid() || suspended)
        return;
    body.setEnabled(false);
    suspended = true;
    occluder.reset(); // an invisible entity must not occlude either
}

void PhysicsComponent::park(bool disable)
{
    if (!body.isValid())
        return;
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetLinearVelocity, glm::vec3(0.0f));
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetAngularVelocity, glm::vec3(0.0f));
    Globals::physics.queueBodyCommand(body,
        disable ? PhysicsWorld::EBodyCommand::SetEnabled : PhysicsWorld::EBodyCommand::SetAwake, glm::vec3(0.0f));
}

void PhysicsComponent::unpark(Entity& entity, const glm::vec3& velocity)
{
    if (!body.isValid() || suspended)
        return;
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetLinearVelocity, velocity);
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetAngularVelocity, glm::vec3(0.0f));
    // OVERLAP LIFT: a far-ticked body teleports through everything, so it may wake inside another
    // dynamic body. Any live one within a metre (or one woken earlier this pass: schedTier left 3,
    // a racy read that at least one of a waking pair sees) lifts this body 2 m so it lands on top
    // instead of the solver blasting the two apart.
    if (bodyType == EPhysicsBodyType::Dynamic)
    {
        const glm::vec3 pos = body.getPosition();
        bool occupied = false;
        Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), 1.0f, SpatialLayer_Entity, [&](uint64 user)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            const PhysicsComponent* opc = other != &entity ? getComponent<PhysicsComponent>(other) : nullptr;
            occupied |= opc && opc->bodyType == EPhysicsBodyType::Dynamic && opc->body.isValid() && !opc->suspended
                && (opc->body.isEnabled() || other->schedTier != 3)
                && glm::distance(opc->body.getPosition(), pos) < 1.0f;
        });
        if (occupied)
        {
            const glm::vec3 lifted = pos + glm::vec3(0.0f, 2.0f, 0.0f);
            Globals::physics.teleportBody(body, lifted, body.getRotation());
            snapPose(entity, lifted);
        }
    }
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(1.0f));
}

void PhysicsComponent::applyBuoyancy(uint32 steps)
{
    const PhysicsWorld& physics = Globals::physics;
    glm::vec3 lo, hi;
    body.getAABB(lo, hi);
    // Early out: whole body above the local surface (1m margin covers the wave slope across the AABB).
    const float waterAtCenter = physics.sampleWaterHeight((lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f);
    if (lo.y > waterAtCenter + 1.0f)
        return;

    // One step consumes the queued force, so the steps this application owes are its scale: the
    // impulse per second of sim time is then the same whatever the frame rate schedules.
    const float scale = float(steps);
    const float coveredSec = scale / float(physics.getStepHz());
    const float waterDensity = physics.getWaterDensity();
    const glm::vec3 gravity = physics.getGravity();
    // Drag is EXPLICIT damping: over the covered time it must not reverse the velocity, which a
    // body far lighter than the water it displaces otherwise does. The cap is against the FULLY
    // submerged displaced mass, so it binds only where the step would have been unstable anyway.
    const float drag = glm::min(physics.getWaterLinearDrag(),
        body.getMass() / glm::max(waterDensity * buoyancyVolume * coveredSec, 1e-6f));

    if (lockRotation)
    {
        // A locked body cannot use a torque, so ONE probe at the centre carrying the whole volume
        // gives the same net lift and drag as the grid for a fraction of the sampling and queuing.
        const glm::vec3 probe = (lo + hi) * 0.5f;
        const float submerged = glm::clamp((waterAtCenter - probe.y) / glm::max(hi.y - lo.y, 0.01f) + 0.5f, 0.0f, 1.0f);
        if (submerged <= 0.0f)
            return;
        const float displacedMass = waterDensity * buoyancyVolume * submerged;
        glm::vec3 force = -gravity * displacedMass; // Archimedes: weight of the displaced water, upward
        force -= body.getLinearVelocity() * (displacedMass * drag);
        Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyForce, force * scale);
        return;
    }

    // Free rotation: a 2x2x2 probe grid, each cell its share of the volume, so the off-centre
    // forces give righting torque and tumbling damping.
    const float cellVolume = buoyancyVolume * (1.0f / 8.0f);
    const float cellHeight = glm::max((hi.y - lo.y) * 0.5f, 0.01f);
    const glm::vec3 centerOfMass = body.getCenterOfMass();
    glm::vec3 force(0.0f), torque(0.0f);
    for (uint32 i = 0; i < 8; ++i)
    {
        const glm::vec3 probe = glm::mix(lo, hi,
            glm::vec3(0.25f) + 0.5f * glm::vec3(float(i & 1u), float((i >> 1) & 1u), float((i >> 2) & 1u)));
        const float waterY = physics.sampleWaterHeight(probe.x, probe.z);
        const float submerged = glm::clamp((waterY - probe.y) / cellHeight + 0.5f, 0.0f, 1.0f);
        if (submerged <= 0.0f)
            continue;
        const float displacedMass = waterDensity * cellVolume * submerged;
        glm::vec3 probeForce = -gravity * displacedMass; // Archimedes: weight of the displaced water, upward
        probeForce -= body.getPointVelocity(probe) * (displacedMass * drag);
        force += probeForce;
        torque += glm::cross(probe - centerOfMass, probeForce);
    }
    if (force == glm::vec3(0.0f))
        return;
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyForce, force * scale);
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyTorque, torque * scale);
}

void PhysicsComponent::update(Entity& entity, const Transform& parentWorld)
{
    if (!body.isValid())
        return;

    if (suspended)
    {
        body.setEnabled(true);
        suspended = false;
        // Re-bake the occluder at the body's pose. The triangles live in the spawn recipe and the
        // scale is the composed world scale, the same value spawn() baked into the shape.
        const SpawnInfo* info = bodyType == EPhysicsBodyType::Static ? getPhysicsSpawnInfo(&entity) : nullptr;
        if (info && info->occluders)
            occluder = SpatialOccluder(Globals::occlusionBuffer.addOccluder(info->occluders,
                Transform(body.getPosition(), parentWorld.scale * entity.scale, body.getRotation())));
    }

    if (!enabled)
        return;
    //ProfileScope profileScope("PhysicsComponent::update", EProfileCategory::Entity);

    if (bodyType == EPhysicsBodyType::Dynamic)
    {
        const uint16 stepCount = uint16(Globals::physics.getStepCount()); // the stamp is modulo 65536
        if (poseHeld)
            poseHeld = false; // a snap wrote the entity pose; the queued teleport may not have moved the body yet
        else
        {
            // THE POSE BETWEEN TWO STEPS, with no stored pose: box3d integrates x1 = x0 + v1 * h,
            // so the pose `backSec` before the body pose is the body pose moved back along its
            // velocity. Not exact with sub-steps and contact correction (free fall at 20 Hz: about
            // 9 mm per step), and a velocity written between two steps moves the shown pose at once.
            const float backSec = (1.0f - Globals::physics.getInterpolationAlpha()) / float(Globals::physics.getStepHz());
            const glm::vec3 pos = body.getPosition() - body.getLinearVelocity() * backSec;
            glm::quat rot = body.getRotation();
            if (!lockRotation)
            {
                const glm::vec3 angVel = body.getAngularVelocity();
                const float speed = glm::length(angVel);
                if (speed * backSec > 1e-5f)
                    rot = glm::normalize(glm::angleAxis(-speed * backSec, angVel / speed) * rot);
            }
            const Transform local = parentWorld.inverse() * Transform(pos, parentWorld.scale * entity.scale, rot);
            entity.pos = local.pos;
            if (!lockRotation)
                entity.rot = local.quat; // a locked body's rot is frozen at spawn - writing it back
                                         // would stomp script-driven facing (see the player capsule)
        }

        // BUOYANCY, on a frame that did not step (the step frame is the busy one) unless every
        // frame steps: the queued force lands at the next drain and box3d holds it until the step
        // consumes it. The pass runs after this frame's physics.update, so `stepCount` is the step
        // the read follows. That deferral is NOT one application per step — near the step rate a
        // step frame whose predecessor did not step is skipped, and at 25 fps against the 20 Hz
        // default that loses one step in four — so the application carries the steps it OWES and
        // the sim-time impulse comes out the same at any frame rate.
        if (buoyancyVolume > 0.0f)
        {
            const uint16 owed = uint16(stepCount - buoyancyStep);
            if (!buoyant || owed > c_maxBuoyancyCatchUpSteps)
                buoyancyStep = stepCount; // gated off by the tier, or back from a gap: no catch-up
            else if (owed != 0
                && (!Globals::jobSystem.frameHasPhysicsStep() || Globals::jobSystem.prevFrameHadPhysicsStep()))
            {
                buoyancyStep = stepCount;
                if (Globals::physics.isWaterActive()) // once per step, never per frame: it calls the ocean
                    applyBuoyancy(owed);
            }
        }
    }
}

static Transform parentWorldOf(const Entity& entity)
{
    Transform world;
    for (const Entity* p = entity.parent; p; p = p->parent)
        world = composeTransform(Transform(p->pos, p->scale, p->rot), world);
    return world;
}

void PhysicsComponent::snapPose(Entity& entity, const glm::vec3& pos, const glm::quat& rot)
{
    const Transform parentWorld = parentWorldOf(entity);
    const Transform local = parentWorld.inverse() * Transform(pos, parentWorld.scale * entity.scale, rot);
    entity.pos = local.pos;
    if (!lockRotation)
        entity.rot = local.quat; // a locked body does not own entity.rot (see update)
    poseHeld = true;
}

void PhysicsComponent::snapPose(Entity& entity, const glm::vec3& pos)
{
    const Transform parentWorld = parentWorldOf(entity);
    entity.pos = (parentWorld.inverse() * Transform(pos, parentWorld.scale * entity.scale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))).pos;
    poseHeld = true;
}

glm::vec3 PhysicsComponent::getShownPosition(const Entity& entity)
{
    return composeTransform(parentWorldOf(entity), Transform(entity.pos, entity.scale, entity.rot)).pos;
}

void suspendPhysicsTree(Entity& entity, SceneComponent* sc)
{
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity))
        pc->suspendBody();
    if (sc)
        for (const EntityPtr& child : sc->children)
            suspendPhysicsTree(*child, getComponent<SceneComponent>(child));
}

const PhysicsComponent::SpawnInfo* getPhysicsSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<PhysicsComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Physics); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const PhysicsComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

float getPhysicsShapeScale(const Entity* entity)
{
    float scale = 1.0f;
    for (const Entity* e = entity; e; e = e->parent)
        scale *= e->scale;
    return scale;
}

void writePhysicsSpawnInfo(const PhysicsComponent::SpawnInfo& info, AssetNode& out)
{
    switch (info.bodyType)
    {
    case EPhysicsBodyType::Static:    out.set("Body", "Static");    break;
    case EPhysicsBodyType::Kinematic: out.set("Body", "Kinematic"); break;
    case EPhysicsBodyType::Dynamic:   out.set("Body", "Dynamic");   break;
    }
    const PhysicsShape& shape = info.shape;
    const PhysicsShape defaults;
    switch (shape.type)
    {
    case EPhysicsShapeType::Box:
        out.set("Shape", "Box");
        out.set("HalfExtents", shape.halfExtents);
        break;
    case EPhysicsShapeType::Sphere:
        out.set("Shape", "Sphere");
        out.set("Radius", shape.radius);
        break;
    case EPhysicsShapeType::Capsule:
        out.set("Shape", "Capsule");
        out.set("Radius", shape.radius);
        out.set("HalfHeight", shape.halfHeight);
        break;
    case EPhysicsShapeType::Hull:
        out.set("Shape", "Hull"); // point cloud re-derived from the render mesh on load
        if (shape.maxHullVertices != defaults.maxHullVertices)
            out.addChild("MaxHullVertices").values.emplace_back(oc::to_string(shape.maxHullVertices));
        break;
    case EPhysicsShapeType::Mesh:
        out.set("Shape", "Mesh"); // BVH re-derived from the render mesh on load
        break;
    }
    if (info.lockRotation)   out.set("LockRotation", info.lockRotation);
    if (shape.isSensor)      out.set("Sensor", shape.isSensor);
    if (shape.contactEvents) out.set("ContactEvents", shape.contactEvents);
    if (shape.offset != defaults.offset)           out.set("Offset", shape.offset);
    if (shape.density != defaults.density)         out.set("Density", shape.density);
    if (shape.friction != defaults.friction)       out.set("Friction", shape.friction);
    if (shape.restitution != defaults.restitution) out.set("Restitution", shape.restitution);
    if (!info.layer.empty())                       out.set("Layer", info.layer);
    if (!info.collidesWith.empty())                out.addChild("CollidesWith").values = info.collidesWith;
    if (shape.groupIndex != 0)                     out.addChild("Group").values.emplace_back(oc::to_string(shape.groupIndex));
    if (!info.enabled)                             out.set("Enabled", info.enabled);
}
