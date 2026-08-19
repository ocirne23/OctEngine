module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;

void ForceComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    entity.setProfiled(); // field emitters carry a per-entity profile scope
    const float dirLen2 = glm::dot(info.direction, info.direction);
    localDirection = dirLen2 > 1e-12f ? info.direction * glm::inversesqrt(dirLen2) : glm::vec3(0.0f, 0.0f, -1.0f);
    localOffset = info.offset;
    centered = info.centered;
    const glm::vec3 worldDir = base.quat * localDirection;
    glm::vec3 pos = base.pos + base.quat * (localOffset * base.scale);
    if (centered)
        pos -= worldDir * (info.reach * 0.5f); // half a reach back so a zero offset sits the bubble on the entity
    emitter = Globals::forceSystem.createEmitter(info.team, pos, worldDir,
        info.output, info.reach, info.focus, info.distribution, info.width);
}

void ForceComponent::destroy(Entity& entity, const SpawnInfo&)
{
    emitter.destroy();
}

void ForceComponent::update(Entity& entity, const Transform& world)
{
    if (!emitter.isValid())
        return;
    const glm::vec3 worldDir = world.quat * localDirection;
    glm::vec3 pos = world.pos + world.quat * (localOffset * world.scale);
    if (centered)
        pos -= worldDir * (emitter.getReach() * 0.5f); // reach is world units, so the centering isn't entity-scaled
    emitter.setTransform(pos, worldDir);
}

const ForceComponent::SpawnInfo* getForceSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<ForceComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Force); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const ForceComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeForceSpawnInfo(const ForceComponent::SpawnInfo& info, AssetNode& out)
{
    const ForceComponent::SpawnInfo defaults;
    out.set("Output", info.output);
    out.set("Reach", info.reach);
    if (info.team != defaults.team)                 out.addChild("Team").values.emplace_back(oc::to_string(info.team));
    if (info.direction != defaults.direction)       out.set("Direction", info.direction);
    if (info.offset != defaults.offset)             out.set("Offset", info.offset);
    if (info.focus != defaults.focus)               out.set("Focus", info.focus);
    if (info.distribution != defaults.distribution) out.set("Distribution", info.distribution);
    if (info.width != defaults.width)               out.set("Width", info.width);
    if (info.centered != defaults.centered)         out.set("Centered", info.centered);
}
