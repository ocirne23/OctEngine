module Entity;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import :Entity;
import File;
import Animation;
import RendererVK;
import Spatial;

void SceneAnimatorComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    rig = info.rig.get();
    enabled = info.enabled;
    if (!rig)
        return;
    state.initialize(rig->anim);

    const uint32 numBones = (uint32)rig->bones.size();
    bones.resize(numBones);
    for (uint32 i = 0; i < numBones; ++i)
    {
        const SceneAnimRig::Bone& src = rig->bones[i];
        Bone& bone = bones[i];
        bone.local = src.bind;
        bone.world = composeTransform(src.parent < 0 ? base : bones[src.parent].world, src.bind);
        const RenderComponent::SpawnInfo* render = src.render.get();
        if (!render || !render->container)
            continue;
        // Same recipe as RenderComponent::spawn (static nodes only: a bone IS the rigid part).
        bone.node = render->container->spawnNodeForIdx(render->nodeIdx, composeTransform(bone.world, render->localTransform));
        if (bone.node.isValid() && render->color.x >= 0.0f)
            bone.node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(render->color));
    }
}

void SceneAnimatorComponent::destroy(Entity& entity, const SpawnInfo& info)
{
}

void SceneAnimatorComponent::update(Entity& entity, float deltaSeconds)
{
    if (!enabled || !rig)
        return;
    if (drivenDistance >= 0.0f && drivenDelta <= 0.0f)
        return; // driven, and the parent did not tick this frame (SIM LOD)
    //ProfileScope profileScope("SceneAnimatorComponent::update", EProfileCategory::Entity);

    float distance;
    if (drivenDistance >= 0.0f)
    {
        distance = drivenDistance;
        deltaSeconds = drivenDelta; // the parent's tick, catch-up included
        drivenDistance = 0.0f;
        drivenDelta = 0.0f;
    }
    else
    {
        // Planar distance in the parent's space: the world, for a root entity.
        const glm::vec3 pos = entity.pos;
        distance = hasLastPos ? glm::length(glm::vec2(pos.x - lastPos.x, pos.z - lastPos.z)) : 0.0f;
        lastPos = pos;
        hasLastPos = true;
    }
    // Stride / FullSpeed are in this entity's LOCAL units, so a scaled model keeps its gait.
    if (!advanceParts(rig->anim, state, deltaSeconds, distance / glm::max(entity.scale, 1e-4f)))
        return;

    const SceneAnimation& anim = rig->anim;
    const uint32 numParts = (uint32)anim.parts.size();
    for (uint32 i = 0; i < numParts; ++i)
    {
        const uint16 boneIdx = rig->partBones[i];
        if (boneIdx == SceneAnimRig::InvalidBone)
            continue;
        const Part& part = anim.parts[i];
        glm::vec3 partPos;
        glm::quat partRot;
        evaluatePart(anim, part, state, partPos, partRot);
        Transform& local = bones[boneIdx].local;
        if (part.rotates)    local.quat = partRot;
        if (part.translates) local.pos = partPos;
    }
    placeDirty = 1;
}

void SceneAnimatorComponent::place(Renderer& renderer, const Transform& world, uint32 passMask)
{
    const uint32 numBones = (uint32)bones.size();
    const bool moved = world.pos != placedWorld.pos || world.quat != placedWorld.quat || world.scale != placedWorld.scale;
    if (placeDirty || moved)
    {
        placedWorld = world;
        placeDirty = 0;
        for (uint32 i = 0; i < numBones; ++i)
        {
            const SceneAnimRig::Bone& src = rig->bones[i];
            Bone& bone = bones[i];
            bone.world = composeTransform(src.parent < 0 ? world : bones[src.parent].world, bone.local);
            if (bone.node.isValid())
                bone.node.setTransform(composeTransform(bone.world, src.render->localTransform));
        }
    }
    if (passMask == 0)
        return;
    for (uint32 i = 0; i < numBones; ++i)
        if (bones[i].node.isValid())
            renderer.renderNode(bones[i].node, passMask); // lock-free (the parallel entity pass)
}

void SceneAnimatorComponent::addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const
{
    if (!rig)
        return;
    oc::vector<Transform> rest(rig->bones.size());
    for (size_t i = 0; i < rig->bones.size(); ++i)
    {
        const SceneAnimRig::Bone& src = rig->bones[i];
        rest[i] = composeTransform(src.parent < 0 ? toEntity : rest[src.parent], src.bind);
        if (i >= bones.size() || !bones[i].node.isValid())
            continue;
        const Sphere& local = bones[i].node.getLocalBounds();
        if (!(local.radius >= 0.0f) || !std::isfinite(local.radius) || glm::any(glm::isnan(local.pos)) || glm::any(glm::isinf(local.pos)))
            continue;
        const Transform t = composeTransform(rest[i], src.render->localTransform);
        Sphere placed(t.quat * (local.pos * t.scale) + t.pos, local.radius * t.scale);
        if (any)
            bounds.combineSphere(placed);
        else
            bounds = placed;
        any = true;
    }
}

const SceneAnimatorComponent::SpawnInfo* getSceneAnimatorSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<SceneAnimatorComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_SceneAnimator); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const SceneAnimatorComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeSceneAnimatorSpawnInfo(const SceneAnimatorComponent::SpawnInfo& info, AssetNode& out)
{
    for (const AssetNode& child : info.source.children)
        out.children.push_back(child);
}
