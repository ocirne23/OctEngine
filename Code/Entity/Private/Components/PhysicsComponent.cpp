module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Physics;
import Spatial;
import Threading;

void PhysicsComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    enabled = info.enabled;
    bodyType = info.bodyType;
    lockRotation = info.lockRotation;
    lastStep = Globals::physics.getStepCount();

    // `base` is parent-local for a prefab child; the ancestor chain is already positioned by now.
    Transform world = base;
    for (const Entity* p = entity.parent; p; p = p->parent)
        world = composeTransform(Transform(p->pos, p->scale, p->rot), world);

    shapeScale = world.scale;
    prevPos = currPos = world.pos;
    prevRot = currRot = world.quat;

    PhysicsBodyDesc desc;
    desc.type = info.bodyType;
    desc.transform = world;
    desc.userData = &entity;
    desc.lockRotation = info.lockRotation;
    body = Globals::physics.createBody(desc, oc::span(&info.shape, 1));
    // The exact displaced volume from box3d's mass (the world scale is baked into the shape).
    if (info.bodyType == EPhysicsBodyType::Dynamic && !info.shape.isSensor && info.shape.density > 0.0f && body.isValid())
        buoyancyVolume = body.getMass() / info.shape.density;

    if (info.bodyType == EPhysicsBodyType::Static)
    {
        occluderData = info.occluders;
        if (occluderData)
            occluder = SpatialOccluder(Globals::occlusionBuffer.addOccluder(occluderData, world));
    }
}

void PhysicsComponent::destroy(Entity& entity, const SpawnInfo& info)
{
    body.destroy(); // removes the collider from the box3d world (shapes die with the body)
    occluder.reset();
    occluderData.reset();
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
            prevPos = currPos = lifted;
            lastStep = Globals::physics.getStepCount();
        }
    }
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(1.0f));
}

void PhysicsComponent::applyBuoyancy()
{
    const PhysicsWorld& physics = Globals::physics;
    glm::vec3 lo, hi;
    body.getAABB(lo, hi);
    // Early out: whole body above the local surface (1m margin covers the wave slope across the AABB).
    const float waterAtCenter = physics.sampleWaterHeight((lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f);
    if (lo.y > waterAtCenter + 1.0f)
        return;

    const float waterDensity = physics.getWaterDensity();
    const float drag = physics.getWaterLinearDrag();
    const glm::vec3 gravity = physics.getGravity();

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
        Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyForce, force);
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
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyForce, force);
    Globals::physics.queueBodyCommand(body, PhysicsWorld::EBodyCommand::ApplyTorque, torque);
}

void PhysicsComponent::update(Entity& entity, const Transform& parentWorld)
{
    if (!body.isValid())
        return;

    if (suspended)
    {
        body.setEnabled(true);
        suspended = false;
        if (occluderData)
            occluder = SpatialOccluder(Globals::occlusionBuffer.addOccluder(occluderData,
                Transform(body.getPosition(), shapeScale, body.getRotation())));
    }

    if (!enabled)
        return;

    if (bodyType == EPhysicsBodyType::Dynamic)
    {
        // Track the pose per physics step so rendering can interpolate between fixed steps.
        const uint32 stepCount = Globals::physics.getStepCount();
        if (stepCount != lastStep)
        {
            prevPos = currPos;
            prevRot = currRot;
            currPos = body.getPosition();
            currRot = body.getRotation();
            lastStep = stepCount;
        }
        const float alpha = Globals::physics.getInterpolationAlpha();
        const glm::vec3 pos = glm::mix(prevPos, currPos, alpha);
        const glm::quat rot = glm::slerp(prevRot, currRot, alpha);
        const Transform local = parentWorld.inverse() * Transform(pos, parentWorld.scale * entity.scale, rot);
        entity.pos = local.pos;
        if (!lockRotation)
            entity.rot = local.quat; // a locked body's rot is frozen at spawn - writing it back
                                     // would stomp script-driven facing (see the player capsule)

        // Once per step interval, on a frame that did not step (the step frame is the busy one)
        // unless every frame steps: the queued force lands at the next drain and box3d holds it
        // until the step. The pass runs after this frame's physics.update, so `stepCount` is the
        // step the read follows.
        if (buoyant && buoyancyVolume > 0.0f && buoyancyStep != stepCount
            && (!Globals::jobSystem.frameHasPhysicsStep() || Globals::jobSystem.prevFrameHadPhysicsStep())
            && Globals::physics.isWaterActive())
        {
            buoyancyStep = stepCount;
            applyBuoyancy();
        }
    }
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
