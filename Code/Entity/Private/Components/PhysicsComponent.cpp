module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Physics;
import Spatial;

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
            entity.rot = local.quat; // a locked body's rot is frozen at spawn — writing it back
                                     // would stomp script-driven facing (see the player capsule)
    }
}

void suspendPhysicsTree(Entity& entity)
{
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity))
        pc->suspendBody();
    if (SceneComponent* sc = getComponent<SceneComponent>(&entity))
        for (const EntityPtr& child : sc->children)
            suspendPhysicsTree(*child);
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
