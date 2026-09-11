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
    // The spatial registration is the ENTITY's (Entity::create, after every component spawned).
}

void RenderComponent::destroy(Entity& entity, const SpawnInfo& info)
{

}

void RenderComponent::update(Entity& entity, Renderer& renderer, const Transform& world)
{
    node.setTransform(composeTransform(world, localTransform));
    SpatialIndex& spatialIndex = Globals::spatialIndex;
    const SpatialCullingConfig& culling = spatialIndex.getCullingConfig();
    const bool hasEntry = entity.spatialEntry.isValid();
    if (hasEntry)
    {
        const Sphere bounds = node.getWorldBounds();
        const float radius = node.isSkinned() ? bounds.radius * culling.skinnedRadiusScale : bounds.radius;
        spatialIndex.updateEntry(entity.spatialEntry.handle(), glm::dvec3(bounds.pos), radius);
    }
    if (culling.mode >= int(ESpatialCullMode::Cull) && hasEntry)
    {
        const uint32 spatialMask = spatialIndex.getPassMask(entity.spatialEntry.handle());
        uint32 passMask = 0;
        if (spatialMask & SpatialPassBit_Main)
            passMask = RendererVKLayout::PASS_ALL;
        else if ((spatialMask & SpatialPassBit_Near) && culling.mode != int(ESpatialCullMode::MainOnly))
            passMask = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI; // off-screen but shadow/RT relevant
        else if ((spatialMask & SpatialPassBit_Shadow) && culling.mode != int(ESpatialCullMode::MainOnly))
            passMask = RendererVKLayout::PASS_SHADOW; // off-screen, up-sun of the view
        if (passMask != 0)
            renderer.renderNode(node, passMask);
    }
    else
        renderer.renderNode(node);
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
