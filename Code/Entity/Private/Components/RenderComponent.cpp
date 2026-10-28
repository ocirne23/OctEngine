module Entity;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import :Entity;
import RendererVK;
import Spatial;

void RenderComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    if (!info.container)
        return;
    localTransform = info.localTransform;
    const Transform world = composeTransform(base, info.localTransform);
    if (info.skinned && info.container->isSkinned())
        node = info.container->spawnSkinnedNode(world);
    else
        node = info.container->spawnNodeForIdx(info.nodeIdx, world);
    if (node.isValid() && info.color.x >= 0.0f) // authored tint: flat solid-color material override
        node.setMaterialOverride(RenderMaterial::getOrCreateSolidColor(info.color));
    // The spatial registration is the ENTITY's (Entity::create, after every component spawned).
}

void RenderComponent::destroy(Entity& entity, const SpawnInfo& info)
{

}

void RenderComponent::place(const Transform& world)
{
    node.setTransform(composeTransform(world, localTransform));
}

const RenderComponent::SpawnInfo* getRenderSpawnInfo(const Entity* entity)
{
    return getSpawnInfo<RenderComponent>(entity);
}
