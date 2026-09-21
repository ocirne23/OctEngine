export module RendererVK:RayTracingScene;

import Core;
import :Layout;
import :AccelerationStructure;

// The CPU bookkeeping around the acceleration structures: which meshes still owe a BLAS, what each one
// needs to build it, and how big this frame's TLAS instance buffers have to be. It owns the
// AccelerationStructure itself, so the whole RT side of the scene is one member of the Renderer.
//
// The build watermark is the load-bearing part. A static BLAS is built ONCE per mesh, so
// takeBuildList() scans forward from `m_blasBuiltCount` to the live MeshInfo count and moves the
// watermark - which means anything that needs a build BELOW the watermark has to be queued explicitly:
//  * a RE-STREAMED mesh (its data moved in the mega-buffer), and
//  * a REUSED slot a destroyed container freed, if it sits under the watermark.
// SKINNED OUTPUT regions never build a static BLAS at all: their vertices are uninitialized until the
// skinning compute runs, and their address entries are owned per frame slot by the skinned rebuild, so
// a static build entering compaction would clobber them with a garbage BLAS.
export class RayTracingScene final
{
public:
    // onGpuIdle runs before the TLAS instance buffers are re-created, onTlasGrown afterwards (resize
    // the GI pipeline's buffers and re-record: the cached GI secondary bakes them and the dispatch size).
    void initialize(uint32 maxUniqueMeshes, oc::function<void()> onGpuIdle, oc::function<void(uint32)> onTlasGrown)
    {
        m_onGpuIdle = oc::move(onGpuIdle);
        m_onTlasGrown = oc::move(onTlasGrown);
        m_accel.initialize(maxUniqueMeshes);
    }

    AccelerationStructure& accel() { return m_accel; }
    const AccelerationStructure& accel() const { return m_accel; }

    // ---- MeshInfo lifetime ----
    // Called with the slots addMeshInfos just claimed. `reused` distinguishes a recycled range (below
    // the watermark, so its builds are queued) from a fresh append (the watermark scan catches it).
    void onMeshInfosAdded(uint32 base, uint32 count, oc::span<const uint32> vertexCounts, bool skinnedOutputs, bool reused)
    {
        if (!reused)
        {
            m_vertexCounts.resize(base + count);
            m_isSkinnedOutput.resize(base + count);
        }
        for (uint32 i = 0; i < count; ++i)
        {
            m_vertexCounts[base + i] = vertexCounts[i];
            m_isSkinnedOutput[base + i] = skinnedOutputs ? 1 : 0;
            if (reused && !skinnedOutputs && base + i < m_blasBuiltCount)
                m_pendingRebuilds.push_back(base + i);
        }
    }
    // Neutralizes a freed range: its BLASes/aliases retire and any queued rebuild for it is dropped.
    void onMeshInfoRangeFreed(uint32 base, uint32 count)
    {
        for (uint32 i = base; i < base + count; ++i)
        {
            m_vertexCounts[i] = 0;
            m_isSkinnedOutput[i] = 0;
        }
        m_accel.onMeshRangeFreed(base, count);
        oc::erase_if(m_pendingRebuilds, [&](uint32 meshIdx) { return meshIdx >= base && meshIdx < base + count; });
    }
    // The BLAS goes with the mesh data (rebuilt on re-stream); safe because eviction requires the set
    // to have been unreferenced for far longer than any in-flight TLAS.
    void onMeshEvicted(uint16 meshInfoIdx) { m_accel.onMeshEvicted(meshInfoIdx); }
    void onMeshStreamedIn(uint16 meshInfoIdx)
    {
        if (m_accel.getMeshAlias(meshInfoIdx) == meshInfoIdx) // an aliased LOD level builds nothing of its own
            m_pendingRebuilds.push_back(meshInfoIdx);
    }

    // ---- The one-time static builds ----
    bool hasPendingBuilds(uint32 meshInfoCount) const { return m_blasBuiltCount < meshInfoCount || !m_pendingRebuilds.empty(); }
    // The queued rebuilds plus every never-built mesh up to `meshInfoCount`; moves the watermark.
    oc::vector<uint32> takeBuildList(uint32 meshInfoCount)
    {
        oc::vector<uint32> buildList = oc::move(m_pendingRebuilds);
        m_pendingRebuilds.clear();
        for (uint32 meshIdx = m_blasBuiltCount; meshIdx < meshInfoCount; ++meshIdx)
            if (!m_isSkinnedOutput[meshIdx])
                buildList.push_back(meshIdx);
        m_blasBuiltCount = meshInfoCount;
        return buildList;
    }
    const uint32* getVertexCounts() const { return m_vertexCounts.data(); } // BLAS maxVertex, one per MeshInfo

    // ---- The GI TLAS instance buffers ----
    uint32 getMaxTlasInstances() const { return m_maxTlasInstances; }
    // Doubles until `count` fits; a no-op when it already does. Called before this frame writes any
    // instances, from last frame's (still live) instance count.
    void growTlasInstancesFor(uint32 count)
    {
        if (count <= m_maxTlasInstances)
            return;
        while (m_maxTlasInstances < count)
            m_maxTlasInstances *= 2;
        m_onGpuIdle();
        m_onTlasGrown(m_maxTlasInstances);
        printf("Renderer: grew GI TLAS instance capacity to %u\n", m_maxTlasInstances);
    }

private:
    AccelerationStructure m_accel;
    oc::function<void()> m_onGpuIdle;
    oc::function<void(uint32)> m_onTlasGrown;

    oc::vector<uint32> m_vertexCounts;    // exact per-MeshInfo vertex count (BLAS maxVertex)
    oc::vector<uint8> m_isSkinnedOutput;  // per MeshInfo: skinned output region (no static BLAS build)
    oc::vector<uint32> m_pendingRebuilds; // re-streamed / recycled meshes awaiting a build
    uint32 m_blasBuiltCount = 0;          // the one-time build watermark
    uint32 m_maxTlasInstances = RendererVKLayout::GI_INITIAL_TLAS_INSTANCES;
};
