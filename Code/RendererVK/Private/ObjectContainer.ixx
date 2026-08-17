export module RendererVK:ObjectContainer;

import Core;
import Core.glm;
import Core.Sphere;
import Core.fwd;
import Animation;

import File.fwd;

import :Layout;
import :Buffer;
import :Texture;

export class RenderNode;

export enum NodeSpawnIdx : uint16
{
    NodeSpawnIdx_ROOT = 0,
    NodeSpawnIdx_INVALID = UINT16_MAX
};

export class ObjectContainer final
{
public:

    friend class Renderer; // removeObjectContainer tears the bookkeeping below down
    friend class RenderNode;
    friend class IndirectCullComputePipeline;

    ObjectContainer() = default;
    ~ObjectContainer();
    ObjectContainer(const ObjectContainer&) = delete;
    ObjectContainer(const ObjectContainer&&) = delete;
    ObjectContainer& operator=(const ObjectContainer&) = delete;
    ObjectContainer& operator=(const ObjectContainer&&) = delete;

    // When passed to initialize(), every material is forced onto the given pipeline, and (unless
    // useSceneTextures is set) uses these texture indices instead of its own scene textures.
    struct MaterialOverrides
    {
        uint16 diffuseTexIdx = RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX;
        uint16 normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
        uint16 metalRoughnessTexIdx = UINT16_MAX;
        bool excludeFromRayTracing = false;
        bool useSceneTextures = true; // if false use the above texture indices instead of the material's own scene textures
        // Skip meshopt LOD-chain generation for this container's meshes. For containers that ARE a LOD
        // scheme already (terrain chunks are re-generated per ring distance, the ocean grid is a
        // clipmap) a generated chain on top only hurts: raster picks a level per frame while ray
        // tracing shares ONE BLAS at "RT/BLAS LOD level", so refraction/reflection rays hit different
        // geometry than the rasterizer draws (visible mismatch at the waterline), and simplifying the
        // ocean's FLAT grid has ~zero geometric error while destroying its per-vertex morph texcoords.
        bool disableGeneratedLods = false;
        RendererVKLayout::EPipelineIndex pipelineIdx = RendererVKLayout::EPipelineIndex::UnlitOpaque;
    };

    bool initialize(const ISceneData& sceneData, const MaterialOverrides* pOverrides = nullptr);
    bool isValid() const { return !m_nodeInfos.empty(); }

    NodeSpawnIdx getSpawnIdxForPath(const oc::string& nodePath) const;

    // All node paths this container knows about — for editor tooling (Entity Editor) to offer a picker
    // scoped to a single container instead of every spawnable registered engine-wide.
    oc::vector<oc::string> getNodePaths() const;
    RenderNode spawnNodeForPath(const oc::string& nodePath, const Transform& transform);
    RenderNode spawnRootNode(const Transform& transform);
    RenderNode spawnNodeForIdx(NodeSpawnIdx idx, const Transform& transform);
    void getRootTransformForIdx(NodeSpawnIdx idx, Transform& transform);

    // True when this container has skeletal/skinned meshes. Spawn such a model with spawnSkinnedNode and
    // drive it with an AnimationPlayer (setSkinningPalette via the returned RenderNode's palette handle).
    bool isSkinned() const { return m_isSkinned; }
    uint32 getNumSkeletonBones() const { return m_numSkeletonBones; }
    RenderNode spawnSkinnedNode(const Transform& transform);

    // The container's own copy of the source skeleton (null if not skinned). Animators retarget their
    // clips against this by bone name.
    const Skeleton* getSkeleton() const { return m_skeleton.get(); }

	const oc::string& getFilePath() const { return m_filePath; }

private:

    struct TempInitData;
    void initializeMeshes(const ISceneData& sceneData, TempInitData& temp, const MaterialOverrides* pOverrides);
    void initializeMaterials(const ISceneData& sceneData, TempInitData& temp, const MaterialOverrides* pOverrides);
    void initializeNodes(const ISceneData& sceneData, TempInitData& temp);

private:

    struct NodeInfo
    {
        uint16 meshInfoIdx = UINT16_MAX;
        uint16 materialInfoIdx = UINT16_MAX;
        uint16 pipelineIdx = 0;
        uint16 alphaMode = 0;
        uint32 lodGroupIdx = UINT32_MAX; // Renderer mesh-LOD-group index (UINT32_MAX = no LOD chain)
    };

    struct NodeMeshRange
    {
        uint16 startIdx = UINT16_MAX;
        uint16 numNodes = UINT16_MAX;
    };
    oc::string m_filePath;

    oc::vector<NodeInfo> m_nodeInfos;
    oc::vector<NodeMeshRange> m_nodeMeshRanges;
    oc::unordered_map<oc::string, uint16> m_nodePathIdxLookup;

    oc::vector<RendererVKLayout::MeshInstanceOffset> m_meshInstanceOffsets;
    oc::vector<Sphere> m_nodeBounds;
    oc::vector<Sphere> m_boundsForMeshIdx;
    // Each node's own world transform relative to the container root, indexed by NodeSpawnIdx. Used by
    // spawnNodeForIdx to rebase a sub-node's baked (root-relative) offsets into the node's own space.
    oc::vector<Transform> m_nodeRootTransforms;
    // Instance-offset base per NodeSpawnIdx for rebased sub-node spawns (UINT32_MAX = not yet uploaded).
    // The rebased offsets are immutable and independent of the spawn transform (placement comes from the
    // per-node transform at cull time), so they're uploaded once and shared by every instance of the idx.
    oc::vector<uint32> m_rebasedOffsetBaseForIdx;

    uint32 m_baseMeshInstanceOffsetsIdx = 0;
    uint16 m_baseMeshInfoIdx = 0;
    uint16 m_baseMaterialInfoIdx = 0;
    uint32 m_numMeshInfos = 0; // total MeshInfo slots (source meshes + generated LOD levels)

    // Teardown bookkeeping, consumed by Renderer::removeObjectContainer (~ObjectContainer).
    enum class EOwnedRange : uint8 { Vertex, Index, Skinning };
    struct OwnedDataRange
    {
        uint64 offset;
        uint64 size;
        EOwnedRange kind;
    };
    // Mega-buffer uploads this container owns — everything EXCEPT ranges owned by a registered mesh
    // stream set (re-streams relocate those; the MeshStreamer frees their current ranges at
    // unregisterSets) and skinned per-instance output regions (owned by the skinned bundles).
    oc::vector<OwnedDataRange> m_ownedDataRanges;
    oc::vector<uint16> m_ownedTextures;  // texture slots uploaded by initializeMaterials
    oc::vector<uint32> m_ownedLodGroups; // static-mesh LOD groups (bundle groups are freed with their bundle)

    // Skinned-mesh source data is captured in initializeMeshes() but owned by the Renderer (like MeshInfo);
    // this container just keeps the base index + count of its entries there. spawnSkinnedNode() reads them
    // back and turns each into a unique per-instance output region + MeshInfo.
    uint32 m_baseSkinnedMeshIdx = UINT32_MAX;
    uint32 m_numSkinnedMeshes = 0;
    bool m_isSkinned = false;
    uint32 m_numSkeletonBones = 0;
    oc::unique_ptr<Skeleton> m_skeleton; // copy of the source skeleton (retained for animator retargeting)
    uint32 m_skinnedIdentityOffsetIdx = UINT32_MAX; // shared identity per-mesh offset for skinned instances

    oc::vector<oc::string> m_meshNames;
    oc::vector<oc::string> m_materialNames;
};