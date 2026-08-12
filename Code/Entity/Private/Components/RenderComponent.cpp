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
        node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(info.color));
    if (node.isValid())
    {
        const Sphere bounds = node.getWorldBounds();
        const float radius = node.isSkinned()
            ? bounds.radius * Globals::spatialIndex.getCullingConfig().skinnedRadiusScale
            : bounds.radius;
        spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
            glm::dvec3(bounds.pos), radius, reinterpret_cast<uint64>(&entity), SpatialLayer_Render));
    }
}

void RenderComponent::destroy(Entity& entity, const SpawnInfo& info)
{

}

const RenderComponent::SpawnInfo* getRenderSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<RenderComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Render); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const RenderComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}
