module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import File;
import Animation;

void SceneAnimatorComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    rig = info.rig.get();
    enabled = info.enabled;
    if (rig)
    {
        state.initialize(rig->anim);
        nodes.assign(rig->nodes.size(), nullptr);
    }
}

void SceneAnimatorComponent::destroy(Entity& entity, const SpawnInfo& info)
{
}

// Walks the rig's slot paths once; after that the parts are reached by pointer until a children list
// on the way changes (unbind).
void SceneAnimatorComponent::bind(Entity& entity)
{
    const uint32 numNodes = (uint32)rig->nodes.size();
    for (uint32 i = 0; i < numNodes; ++i)
    {
        const SceneAnimRig::Node& node = rig->nodes[i];
        Entity* parent = node.parent < 0 ? &entity : nodes[node.parent];
        SceneComponent* sc = parent ? getComponent<SceneComponent>(parent) : nullptr;
        nodes[i] = (sc && node.slot < sc->children.size()) ? sc->children[node.slot].get() : nullptr;
    }
    bound = true;
}

// Parent -> child writes, before this entity emits its children: safe inside the parallel pass.
void SceneAnimatorComponent::update(Entity& entity, float deltaSeconds)
{
    if (!enabled || !rig)
        return;

    // Planar distance in the parent's space: the world, for the root entity a unit is.
    const glm::vec3 pos = entity.pos;
    const glm::vec2 moved = hasLastPos ? glm::vec2(pos.x - lastPos.x, pos.z - lastPos.z) : glm::vec2(0.0f);
    lastPos = pos;
    hasLastPos = true;

    if (!advanceParts(rig->anim, state, deltaSeconds, glm::length(moved)))
        return;

    if (!bound)
        bind(entity);

    const SceneAnimation& anim = rig->anim;
    const uint32 numParts = (uint32)anim.parts.size();
    for (uint32 i = 0; i < numParts; ++i)
    {
        const uint16 nodeIdx = rig->partNodes[i];
        Entity* target = nodeIdx != SceneAnimRig::InvalidNode ? nodes[nodeIdx] : nullptr;
        if (!target)
            continue;
        const Part& part = anim.parts[i];
        glm::vec3 partPos;
        glm::quat partRot;
        evaluatePart(anim, part, state, partPos, partRot);
        if (part.rotates)    target->rot = partRot;
        if (part.translates) target->pos = partPos;
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
