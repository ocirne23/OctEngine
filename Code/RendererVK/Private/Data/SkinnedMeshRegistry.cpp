module RendererVK;

import Core;
import Core.glm;
import :SkinnedMeshRegistry;
import :Layout;

namespace
{
    uint32 growSkinnedCapacity(uint32 current, uint32 needed)
    {
        uint64 capacity = current;
        while (capacity < needed)
            capacity *= 2;
        return (uint32)capacity;
    }
}

uint32 SkinnedMeshRegistry::addSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources)
{
    if (const uint32 reusedBase = m_freeSourceSlots.allocate((uint32)sources.size()); reusedBase != UINT32_MAX)
    {
        oc::copy(sources.begin(), sources.end(), m_sources.begin() + reusedBase);
        return reusedBase;
    }
    const uint32 baseIdx = (uint32)m_sources.size();
    m_sources.insert(m_sources.end(), sources.begin(), sources.end());
    return baseIdx;
}

uint32 SkinnedMeshRegistry::allocatePalette(uint32 boneCount)
{
    // The palette store is a bump allocator; freed regions (destroyed containers) are recycled on an
    // exact boneCount match - same-skeleton respawns, the common case - instead of tracking sub-ranges.
    for (size_t i = 0; i < m_freePaletteHandles.size(); ++i)
    {
        const uint32 handle = m_freePaletteHandles[i];
        const PaletteRegion& region = m_paletteRegions[handle];
        if (region.boneCount == boneCount)
        {
            m_freePaletteHandles[i] = m_freePaletteHandles.back();
            m_freePaletteHandles.pop_back();
            oc::fill_n(m_palettes.begin() + region.offset, boneCount, glm::mat4(1.0f));
            return handle;
        }
    }
    const uint32 handle = (uint32)m_paletteRegions.size();
    const uint32 offset = (uint32)m_palettes.size();
    m_paletteRegions.push_back({ offset, boneCount });
    m_palettes.resize(offset + boneCount, glm::mat4(1.0f)); // identity until the first setPalette
    if ((uint32)m_palettes.size() > m_maxPaletteEntries)
    {
        m_maxPaletteEntries = growSkinnedCapacity(m_maxPaletteEntries, (uint32)m_palettes.size());
        m_onPaletteGrown(m_maxPaletteEntries);
    }
    return handle;
}

void SkinnedMeshRegistry::setPalette(uint32 bundleHandle, oc::span<const glm::mat4> palette)
{
    const uint32 paletteHandle = m_bundles[bundleHandle].paletteHandle;
    assert(paletteHandle < m_paletteRegions.size());
    const PaletteRegion& region = m_paletteRegions[paletteHandle];
    const uint32 count = oc::min((uint32)palette.size(), region.boneCount);
    if (count > 0)
        memcpy(m_palettes.data() + region.offset, palette.data(), count * sizeof(glm::mat4));
}

uint32 SkinnedMeshRegistry::allocateJobRange(uint32 count)
{
    uint32 firstJob = m_freeJobSlots.allocate(count);
    if (firstJob == UINT32_MAX)
    {
        firstJob = (uint32)m_jobs.size();
        // Value-initialized: vertexCount/indexCount 0 keeps a slot inert until setInstance fills it.
        m_jobs.resize(firstJob + count);
        m_blasBuilds.resize(firstJob + count);
        if ((uint32)m_jobs.size() > m_maxJobs)
        {
            m_maxJobs = growSkinnedCapacity(m_maxJobs, (uint32)m_jobs.size());
            m_onJobsGrown(m_maxJobs);
        }
    }
    return firstJob;
}

void SkinnedMeshRegistry::setInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset,
    uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount)
{
    assert(paletteHandle < m_paletteRegions.size());
    m_jobs[jobIdx] = RendererVKLayout::SkinningJob{
        .baseVertexOffset = baseVertexOffset,
        .skinVertexOffset = skinVertexOffset,
        .outVertexOffset = outVertexOffset,
        .vertexCount = vertexCount,
        .paletteOffset = m_paletteRegions[paletteHandle].offset,
    };
    m_blasBuilds[jobIdx] = AccelerationStructure::SkinnedBlasBuild{
        .meshIdx = meshIdx, .vertexOffset = outVertexOffset, .vertexCount = vertexCount, .firstIndex = firstIndex, .indexCount = indexCount };
    // No re-record needed: the jobs are uploaded per frame and dispatched indirectly; the skinned BLAS
    // list is consumed by recordGlobalIllum, which re-records every frame.
}

uint32 SkinnedMeshRegistry::registerBundle(const Bundle& bundle)
{
    if (!m_freeBundleSlots.empty())
    {
        const uint32 handle = m_freeBundleSlots.back();
        m_freeBundleSlots.pop_back();
        m_bundles[handle] = bundle;
        return handle;
    }
    const uint32 handle = (uint32)m_bundles.size();
    m_bundles.push_back(bundle);
    return handle;
}

void SkinnedMeshRegistry::parkBundle(uint32 bundleHandle)
{
    const Bundle& bundle = m_bundles[bundleHandle];
    // Park in place: a zero vertexCount makes the skinning dispatch skip the job, a zero indexCount makes
    // recordBuildSkinnedBlas skip the rebuild. The entries must keep their positions (see Bundle).
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        m_jobs[bundle.firstJob + k].vertexCount = 0;
        m_blasBuilds[bundle.firstJob + k].indexCount = 0;
    }
    m_parkedBundles[bundle.sourceKey].push_back(bundleHandle);
}

uint32 SkinnedMeshRegistry::acquireBundle(uint32 sourceKey)
{
    const auto it = m_parkedBundles.find(sourceKey);
    if (it == m_parkedBundles.end() || it->second.empty())
        return UINT32_MAX;
    const uint32 bundleHandle = it->second.back();
    it->second.pop_back();

    const Bundle& bundle = m_bundles[bundleHandle];
    const PaletteRegion& region = m_paletteRegions[bundle.paletteHandle];
    oc::fill_n(m_palettes.begin() + region.offset, region.boneCount, glm::mat4(1.0f)); // bind pose until the first setPalette
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        const RendererVKLayout::SkinnedMeshSource& src = m_sources[bundle.sourceKey + k];
        m_jobs[bundle.firstJob + k].vertexCount = src.vertexCount;
        // The bundle's recorded count, NOT src.indexCount: the skinned BLAS was created (and its buffer
        // sized) for the chain's RT level, which may be coarser than level 0.
        m_blasBuilds[bundle.firstJob + k].indexCount = bundle.blasIndexCounts[k];
    }
    return bundleHandle;
}

oc::span<const uint32> SkinnedMeshRegistry::getParkedBundles(uint32 sourceKey) const
{
    const auto it = m_parkedBundles.find(sourceKey);
    return it == m_parkedBundles.end() ? oc::span<const uint32>() : oc::span<const uint32>(it->second.data(), it->second.size());
}

void SkinnedMeshRegistry::recycleBundle(uint32 bundleHandle)
{
    Bundle& bundle = m_bundles[bundleHandle];
    m_freeJobSlots.release(bundle.firstJob, bundle.numMeshes);
    m_freePaletteHandles.push_back(bundle.paletteHandle);
    bundle = Bundle{ .sourceKey = UINT32_MAX, .baseMeshIdx = 0, .paletteHandle = 0, .firstJob = 0, .numMeshes = 0 };
    m_freeBundleSlots.push_back(bundleHandle);
}
