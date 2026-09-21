export module RendererVK:SkinnedMeshRegistry;

import Core;
import Core.glm;
import :Layout;
import :MeshLodRegistry; // IndexRangeFreeList
import :AccelerationStructure;

// THE CPU side of skinning, owned by the Renderer: the per-instance skinning jobs and their parallel
// skinned-BLAS builds, the bone palette store, the per-container source table, and the BUNDLES that
// tie one spawned skinned node's allocations together. All of it is persistent (set up at spawn); only
// the palette CONTENTS change per frame, and present() uploads jobs + palettes wholesale.
//
// Three recycling rules shape the whole thing:
//  * a bundle is PARKED, not freed, when its node dies - its job/BLAS entries keep their positions
//    (the per-frame skinned BLAS slots are positional and sized once) and go inert (vertexCount 0 /
//    indexCount 0), so the next spawn of the same container reuses it with no upload and no re-record;
//  * palette regions recycle on an EXACT boneCount match (same-skeleton respawns, the common case),
//    because the store is a bump allocator with no sub-range tracking;
//  * a job range is one CONTIGUOUS block per bundle, from the free list when one fits.
//
// NOT internally locked: every mutator runs under the Renderer's spawn mutex (parallel entity spawning).
// It knows nothing about the device or the pipelines - a capacity growth calls back out.
export class SkinnedMeshRegistry final
{
public:
    // Everything one spawnSkinnedNode allocated (per-instance MeshInfo range, output vertex regions,
    // palette region, contiguous SkinningJob/SkinnedBlasBuild range), recycled as a unit.
    struct Bundle
    {
        uint32 sourceKey;    // owning container's base SkinnedMeshSource index (free-list key)
        uint32 baseMeshIdx;  // first MeshInfo of the per-instance range (level 0s; LOD levels follow)
        uint32 paletteHandle;
        uint32 firstJob;     // first entry of the contiguous SkinningJob/SkinnedBlasBuild range
        uint32 numMeshes;
        oc::vector<uint32> lodGroupForMesh; // per mesh: MeshLodGroup idx (UINT32_MAX = no chain)
        oc::vector<uint32> blasIndexCounts; // per mesh: the skinned BLAS's index count (its RT level's,
                                            // restored on unpark - src.indexCount would be level 0's)
    };

    // onPaletteGrown / onJobsGrown must wait for the GPU, resize the skinning pipeline's buffer to the
    // new capacity and re-record. They are the only way out of this class.
    void initialize(oc::function<void(uint32)> onPaletteGrown, oc::function<void(uint32)> onJobsGrown)
    {
        m_onPaletteGrown = oc::move(onPaletteGrown);
        m_onJobsGrown = oc::move(onJobsGrown);
    }

    // ---- Sources (per container, CPU-only) ----
    uint32 addSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources);
    const RendererVKLayout::SkinnedMeshSource& getSource(uint32 idx) const { return m_sources[idx]; }
    void releaseSources(uint32 base, uint32 count) { m_freeSourceSlots.release(base, count); }

    // ---- Bone palettes ----
    uint32 allocatePalette(uint32 boneCount);
    // Writes one bundle's palette, clamped to its region (the caller has the node's bundle handle).
    void setPalette(uint32 bundleHandle, oc::span<const glm::mat4> palette);
    uint32 getPaletteOffset(uint32 paletteHandle) const { return m_paletteRegions[paletteHandle].offset; }

    // ---- Job / BLAS ranges ----
    // One contiguous block per bundle - from the free list (destroyed containers) when one fits,
    // appended otherwise - filled per mesh afterwards by setInstance.
    uint32 allocateJobRange(uint32 count);
    void setInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset,
        uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount);
    uint32 getJobOutVertexOffset(uint32 jobIdx) const { return m_jobs[jobIdx].outVertexOffset; }

    // ---- Bundles ----
    uint32 registerBundle(const Bundle& bundle);
    void parkBundle(uint32 bundleHandle);              // node destroyed: entries go inert, handle joins its source's park list
    uint32 acquireBundle(uint32 sourceKey);            // reactivates + returns a parked bundle, UINT32_MAX if none free
    const Bundle& getBundle(uint32 handle) const { return m_bundles[handle]; }
    // The parked bundles of one container, so the Renderer can free each one's MeshInfos/LOD groups
    // before recycleBundle() releases this class's own slots. Empty span = the container had none.
    oc::span<const uint32> getParkedBundles(uint32 sourceKey) const;
    void clearParkedBundles(uint32 sourceKey) { m_parkedBundles.erase(sourceKey); }
    // Releases everything this class holds for a bundle. The caller has already freed the MeshInfo
    // range, the output vertex data, the LOD groups and the RT job slots, which are all its own.
    void recycleBundle(uint32 bundleHandle);

    // ---- Per-frame consumers ----
    oc::span<const glm::mat4> getPalettes() const { return m_palettes; }
    oc::span<const RendererVKLayout::SkinningJob> getJobs() const { return m_jobs; }
    oc::span<const AccelerationStructure::SkinnedBlasBuild> getBlasBuilds() const { return m_blasBuilds; }
    bool hasJobs() const { return !m_jobs.empty(); }
    uint32 getMaxPaletteEntries() const { return m_maxPaletteEntries; }
    uint32 getMaxJobs() const { return m_maxJobs; }

private:
    struct PaletteRegion { uint32 offset; uint32 boneCount; };

    oc::function<void(uint32)> m_onPaletteGrown;
    oc::function<void(uint32)> m_onJobsGrown;

    oc::vector<PaletteRegion> m_paletteRegions;
    oc::vector<glm::mat4> m_palettes;                                 // concatenated per region, uploaded each frame
    oc::vector<RendererVKLayout::SkinningJob> m_jobs;                 // one per skinned mesh instance, uploaded each frame
    oc::vector<AccelerationStructure::SkinnedBlasBuild> m_blasBuilds; // parallel to m_jobs
    oc::vector<RendererVKLayout::SkinnedMeshSource> m_sources;

    oc::vector<Bundle> m_bundles;
    oc::unordered_map<uint32, oc::vector<uint32>> m_parkedBundles; // sourceKey -> parked bundle handles
    oc::vector<uint32> m_freeBundleSlots;
    oc::vector<uint32> m_freePaletteHandles; // regions reused on exact boneCount match
    IndexRangeFreeList m_freeJobSlots;       // freed slots stay inert (vertexCount/indexCount 0)
    IndexRangeFreeList m_freeSourceSlots;

    uint32 m_maxPaletteEntries = RendererVKLayout::INITIAL_SKINNING_PALETTE;
    uint32 m_maxJobs = RendererVKLayout::INITIAL_SKINNING_JOBS;
};
