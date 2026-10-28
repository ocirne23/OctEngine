export module RendererVK:ObjectContainerState;

import Core;
import Core.glm;
import Core.Sphere;
import Core.fwd;
import Animation;

import File.fwd;

import :Layout;

export class RenderNode;                  // declared before the friend line below, which would otherwise introduce the name (RendererVK.fwd has an extern "C++" one)
export struct ContainerMaterialOverrides; // ObjectContainer.ixx

// Everything of ObjectContainer but its public API (ObjectContainer.ixx): the load steps and the bookkeeping.
export class ObjectContainerState
{
protected:

    friend class RendererState; // removeObjectContainer tears the bookkeeping below down
    friend class RenderNode;
    friend class IndirectCullComputePipeline;

    struct TempInitData;
    void initializeMeshes(const ISceneData& sceneData, TempInitData& temp, const ContainerMaterialOverrides* pOverrides);
    void initializeMaterials(const ISceneData& sceneData, TempInitData& temp, const ContainerMaterialOverrides* pOverrides);
    void initializeNodes(const ISceneData& sceneData, TempInitData& temp);

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
    // Each node's own world transform relative to the container root, indexed by NodeSpawnIdx. Used by spawnNodeForIdx to rebase a sub-node's baked
    // (root-relative) offsets into the node's own space.
    oc::vector<Transform> m_nodeRootTransforms;
    // Instance-offset base per NodeSpawnIdx for rebased sub-node spawns (UINT32_MAX = not yet uploaded). The rebased offsets are immutable and independent of
    // the spawn transform (placement comes from the per-node transform at cull time), so they're uploaded once and shared by every instance of the idx.
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
    // Mega-buffer uploads this container owns - everything EXCEPT ranges owned by a registered mesh stream set (re-streams relocate those; the MeshStreamer
    // frees their current ranges at unregisterSets) and skinned per-instance output regions (owned by the skinned bundles).
    oc::vector<OwnedDataRange> m_ownedDataRanges;
    oc::vector<uint16> m_ownedTextures;  // texture slots uploaded by initializeMaterials
    oc::vector<uint32> m_ownedLodGroups; // static-mesh LOD groups (bundle groups are freed with their bundle)

    // Skinned-mesh source data is captured in initializeMeshes() but owned by the Renderer (like MeshInfo); this container just keeps the base index + count
    // of its entries there. spawnSkinnedNode() reads them back and turns each into a unique per-instance output region + MeshInfo.
    uint32 m_baseSkinnedMeshIdx = UINT32_MAX;
    uint32 m_numSkinnedMeshes = 0;
    bool m_isSkinned = false;
    uint32 m_numSkeletonBones = 0;
    oc::unique_ptr<Skeleton> m_skeleton; // copy of the source skeleton (retained for animator retargeting)
    uint32 m_skinnedIdentityOffsetIdx = UINT32_MAX; // shared identity per-mesh offset for skinned instances

    oc::vector<oc::string> m_meshNames;
    oc::vector<oc::string> m_materialNames;
};
