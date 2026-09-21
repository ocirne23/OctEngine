module RendererVK;

import Core;
import :FrameSubmission;
import :Layout;

void FrameSubmission::initialize(oc::function<void()> onGpuIdle, oc::function<void()> onInvalidate)
{
    m_onGpuIdle = oc::move(onGpuIdle);
    m_onInvalidate = oc::move(onInvalidate);
    for (FrameSlot& s : m_slots)
    {
        s.lightInfos.initialize(sizeof(RendererVKLayout::LightInfo) * RendererVKLayout::MAX_LIGHTS,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightInfos", BufferHostAccess::eSequentialWrite);
        s.mappedLightInfos = s.lightInfos.mapMemory<RendererVKLayout::LightInfo>();

        s.fogVolumes.initialize(sizeof(RendererVKLayout::FogVolumes),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "FogVolumes", BufferHostAccess::eSequentialWrite);
        s.mappedFogVolumes = s.fogVolumes.mapMemory<RendererVKLayout::FogVolumes>();
        s.mappedFogVolumes.data()->count = 0;
    }
    createLightGridBuffers();
}

void FrameSubmission::createLightGridBuffers()
{
    for (FrameSlot& s : m_slots)
    {
        s.lightGrids.initialize(m_lightGridBufferSize,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LightGrids");

        // Device-local + host-visible (ReBAR) so GPU writes are fast; the CPU reads the header in getStats,
        // hence the default random (cached-where-possible) access.
        s.lightTable.initialize(3 * sizeof(uint32) + sizeof(uint32) * m_lightTableEntries,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible, false, "LightTable");

        // Zero the readback header so the capacity check / stats never see uninitialized counters
        // before the first recorded frame fills them in.
        oc::span<uint32> header = s.lightTable.mapMemory<uint32>(0, 3 * sizeof(uint32));
        memset(header.data(), 0, 3 * sizeof(uint32));
        s.lightTable.flushMappedMemory(3 * sizeof(uint32));
        s.lightTable.unmapMemory();
    }
}

uint32 FrameSubmission::addLight(uint32 frameIdx, const RendererVKLayout::LightInfo& light)
{
    // lock-free bump; claims beyond MAX_LIGHTS are dropped (present clamps the flush count)
    const uint32 idx = oc::atomic_ref<uint32>(m_lightCount).fetch_add(1);
    if (idx >= RendererVKLayout::MAX_LIGHTS)
        return UINT32_MAX;
    m_slots[frameIdx].mappedLightInfos[idx] = light;
    return idx;
}

uint32 FrameSubmission::addFogVolume(uint32 frameIdx, const RendererVKLayout::FogVolumeInfo& fogVolume)
{
    const uint32 idx = oc::atomic_ref<uint32>(m_fogVolumeCount).fetch_add(1);
    if (idx >= RendererVKLayout::MAX_FOG_VOLUMES)
        return UINT32_MAX;
    m_slots[frameIdx].mappedFogVolumes.data()->volumes[idx] = fogVolume;
    return idx;
}

uint32 FrameSubmission::claimDecal()
{
    const uint32 idx = oc::atomic_ref<uint32>(m_decalCount).fetch_add(1);
    return idx < RendererVKLayout::MAX_DECALS ? idx : UINT32_MAX;
}

void FrameSubmission::settleCounts()
{
    m_lightCount = oc::min(m_lightCount, uint32(RendererVKLayout::MAX_LIGHTS));
    m_fogVolumeCount = oc::min(m_fogVolumeCount, uint32(RendererVKLayout::MAX_FOG_VOLUMES));
    m_decalCount = oc::min(m_decalCount, uint32(RendererVKLayout::MAX_DECALS));
}

void FrameSubmission::flushFrame(uint32 frameIdx)
{
    FrameSlot& s = m_slots[frameIdx];
    s.lightInfos.flushMappedMemory(m_lightCount * sizeof(RendererVKLayout::LightInfo));
    s.mappedFogVolumes.data()->count = m_fogVolumeCount;
    s.fogVolumes.flushMappedMemory(RendererVKLayout::FOG_VOLUME_HEADER_SIZE + m_fogVolumeCount * sizeof(RendererVKLayout::FogVolumeInfo));
}

oc::span<const RendererVKLayout::LightInfo> FrameSubmission::getLights(uint32 frameIdx) const
{
    return oc::span<const RendererVKLayout::LightInfo>(m_slots[frameIdx].mappedLightInfos.data(), m_lightCount);
}

bool FrameSubmission::lightGridNeedsGrow(const LightGridComputePipeline::Demand& demand, const LightGridComputePipeline& pipeline) const
{
    // hostBuffersFit covers the table: its entries are 4x the pipeline's grid capacity (the claim table IS the GPU table)
    return demand.gridDataBytes > m_lightGridBufferSize || !pipeline.hostBuffersFit(demand);
}

void FrameSubmission::growLightGrid(const LightGridComputePipeline::Demand& demand, LightGridComputePipeline& pipeline)
{
    const size_t neededGridBytes = demand.gridDataBytes + demand.gridDataBytes / 2;
    while (m_lightGridBufferSize < neededGridBytes)
        m_lightGridBufferSize *= 2;
    m_onGpuIdle();
    pipeline.resizeHostBuffers(demand); // doubles the grid capacity to fit; rehashes the claim table
    m_lightTableEntries = pipeline.getTableEntries(); // the GPU table IS the claim table: same size, a power of 2
    createLightGridBuffers(); // per-frame GPU scratch, rewritten every frame: nothing to preserve
    m_onInvalidate();
    printf("Renderer: grew light grid buffers to %zu bytes / %u table entries (%u grids, %u workgroups)\n",
        m_lightGridBufferSize, m_lightTableEntries, demand.numGrids, demand.numWorkgroups);
}
