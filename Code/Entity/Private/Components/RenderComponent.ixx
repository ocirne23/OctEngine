export module Entity:RenderComponent;

import :Entity;
import Core;
import Core.Transform;
import File;
import Spatial;
import RendererVK;

export struct RenderComponent
{
    static constexpr EComponentID getId() { return EComponentID_Render; }

    ~RenderComponent() {}

    RenderNode node; // the entity's SpatialEntry (Entity::spatialEntry) takes its bounds from here
    Transform localTransform;
    bool showBounds = false;

    struct SpawnInfo
    {
        ObjectContainer* container = nullptr;       // null = nothing to spawn
        oc::string containerName;                  // ObjectContainer reference name, kept for re-serialization
        oc::string nodePath;                       // For debug/display. nodeIdx is used at runtime for spawning.
        NodeSpawnIdx nodeIdx = NodeSpawnIdx_ROOT;
        Transform localTransform;                   // applied on top of the spawn base transform
        glm::vec3 color{ -1.0f };                   // >= 0: solid TINT — the node's materials override
                                                    // to a flat color (authored as `Color r g b`)
        bool skinned = false;                       // spawn a skinned node (GPU skinning) instead of a static one
        oc::string rigType;                        // skinned only: "Humanoid" / "Generic" (empty = unspecified; informational, not yet consumed)
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    // Requires a valid node: places it, refreshes the entity's spatial entry from its bounds and
    // submits it under the cull pass mask. renderNode is lock-free (the parallel entity pass).
    void update(Entity& entity, Renderer& renderer, const Transform& world);
};

export const RenderComponent::SpawnInfo* getRenderSpawnInfo(const Entity* entity);

// Serializes a render spawn recipe into a "Component Render" node; mirror of World::buildRenderSpawnInfo.
export void writeRenderSpawnInfo(const RenderComponent::SpawnInfo& info, AssetNode& out);
