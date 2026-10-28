export module RendererVK:ObjectContainer;

import Core;
import Core.fwd;
import Animation;

import File.fwd;

import :Layout;
import :ObjectContainerState; // the load steps and the bookkeeping

export class RenderNode;

export enum NodeSpawnIdx : uint16
{
    NodeSpawnIdx_ROOT = 0,
    NodeSpawnIdx_INVALID = UINT16_MAX
};

// When passed to ObjectContainer::initialize(), every material is forced onto the given pipeline, and (unless useSceneTextures is set) uses these texture
// indices instead of its own scene textures. ObjectContainer::MaterialOverrides outside.
export struct ContainerMaterialOverrides
{
    uint16 diffuseTexIdx = RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX;
    uint16 normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
    uint16 metalRoughnessTexIdx = UINT16_MAX;
    bool excludeFromRayTracing = false;
    bool useSceneTextures = true; // if false use the above texture indices instead of the material's own scene textures
    // Skip meshopt LOD-chain generation for this container's meshes. For containers that ARE a LOD scheme already (terrain chunks are re-generated per ring
    // distance, the ocean grid is a clipmap) a generated chain on top only hurts: raster picks a level per frame while ray tracing shares ONE BLAS at "RT/BLAS
    // LOD level", so refraction/reflection rays hit different geometry than the rasterizer draws (visible mismatch at the waterline), and simplifying the
    // ocean's FLAT grid has ~zero geometric error while destroying its per-vertex morph texcoords.
    bool disableGeneratedLods = false;
    RendererVKLayout::EPipelineIndex pipelineIdx = RendererVKLayout::EPipelineIndex::UnlitOpaque;
};

// A loaded model: its meshes, materials and node tree in the renderer's tables, spawned as RenderNodes. The state is in ObjectContainerState.ixx.
export class ObjectContainer final : public ObjectContainerState
{
public:

    using MaterialOverrides = ContainerMaterialOverrides;

    // ---- Lifetime ----

    ObjectContainer() = default;
    ~ObjectContainer();

    bool initialize(const ISceneData& sceneData, const MaterialOverrides* pOverrides = nullptr);
    bool isValid() const { return !m_nodeInfos.empty(); }
    const oc::string& getFilePath() const { return m_filePath; }

    // ---- Nodes ----

    NodeSpawnIdx getSpawnIdxForPath(const oc::string& nodePath) const;
    // All node paths this container knows about - for editor tooling (Entity Editor) to offer a picker scoped to a single container instead of every
    // spawnable registered engine-wide.
    oc::vector<oc::string> getNodePaths() const;
    void getRootTransformForIdx(NodeSpawnIdx idx, Transform& transform);

    // ---- Spawning ----

    RenderNode spawnNodeForPath(const oc::string& nodePath, const Transform& transform);
    RenderNode spawnRootNode(const Transform& transform);
    RenderNode spawnNodeForIdx(NodeSpawnIdx idx, const Transform& transform);

    // ---- Skinning ----

    // True when this container has skeletal/skinned meshes. Spawn such a model with spawnSkinnedNode and drive it with an AnimationPlayer
    // (setSkinningPalette via the returned RenderNode's palette handle).
    bool isSkinned() const { return m_isSkinned; }
    uint32 getNumSkeletonBones() const { return m_numSkeletonBones; }
    RenderNode spawnSkinnedNode(const Transform& transform);

    // The container's own copy of the source skeleton (null if not skinned). Animators retarget their clips against this by bone name.
    const Skeleton* getSkeleton() const { return m_skeleton.get(); }

    // ---- Not copyable ----

    ObjectContainer(const ObjectContainer&) = delete;
    ObjectContainer(const ObjectContainer&&) = delete;
    ObjectContainer& operator=(const ObjectContainer&) = delete;
    ObjectContainer& operator=(const ObjectContainer&&) = delete;
};
