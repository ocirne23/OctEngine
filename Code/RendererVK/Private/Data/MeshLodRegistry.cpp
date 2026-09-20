module RendererVK;

import Core;
import :MeshLodRegistry;
import :StagingManager;
import :Layout;

namespace
{
    uint32 growLodCapacity(uint32 current, uint32 needed)
    {
        uint64 capacity = current;
        while (capacity < needed)
            capacity *= 2;
        return (uint32)capacity;
    }
}

void MeshLodRegistry::uploadShared(Buffer& buffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset)
{
    Globals::stagingManager.ensureDrainedForSharedWrite();
    buffer.upload(dataSize, data, dstOffset);
}

uint32 MeshLodRegistry::addGroup(const MeshLodGroup& group)
{
    uint32 groupIdx;
    if (!m_freeGroups.empty())
    {
        groupIdx = m_freeGroups.back();
        m_freeGroups.pop_back();
        m_groups[groupIdx] = group;
    }
    else
    {
        m_groups.push_back(group);
        groupIdx = (uint32)m_groups.size() - 1;
        if ((uint32)m_groups.size() > m_maxGroups)
            growGroupCapacity((uint32)m_groups.size());
    }
    uploadGroup(groupIdx);
    for (uint8 k = 0; k < group.numLods; ++k)
        setGroupIdxForMesh(group.meshIdx[k], groupIdx);
    return groupIdx;
}

void MeshLodRegistry::freeGroup(uint32 groupIdx)
{
    const MeshLodGroup& group = m_groups[groupIdx];
    for (uint8 k = 0; k < group.numLods; ++k)
        setGroupIdxForMesh(group.meshIdx[k], UINT32_MAX);
    m_freeGroups.push_back(groupIdx);
}

void MeshLodRegistry::uploadGroup(uint32 groupIdx)
{
    const MeshLodGroup& group = m_groups[groupIdx];
    RendererVKLayout::GpuMeshLodGroup gpu{};
    gpu.numLods = group.numLods;
    gpu.mesh01 = (uint32)group.meshIdx[0] | ((uint32)group.meshIdx[1] << 16);
    gpu.mesh23 = (uint32)group.meshIdx[2] | ((uint32)group.meshIdx[3] << 16);
    gpu.mesh4 = (uint32)group.meshIdx[4];
    for (uint32 k = 1; k < RendererVKLayout::MAX_MESH_LODS; ++k)
        gpu.errors1_4[k - 1] = group.errors[k];
    uploadShared(m_groupsBuffer, sizeof(gpu), &gpu, (size_t)groupIdx * sizeof(gpu));
}

void MeshLodRegistry::setGroupIdxForMesh(uint16 meshIdx, uint32 groupIdx)
{
    m_meshToGroup[meshIdx] = groupIdx;
    uploadShared(m_groupIdxBuffer, sizeof(uint32), &m_meshToGroup[meshIdx], (size_t)meshIdx * sizeof(uint32));
}

void MeshLodRegistry::uploadMeshMapping(uint32 baseMeshInfoIdx, uint32 count)
{
    uploadShared(m_groupIdxBuffer, count * sizeof(uint32),
        m_meshToGroup.data() + baseMeshInfoIdx, (size_t)baseMeshInfoIdx * sizeof(uint32));
}

uint32 MeshLodRegistry::allocateStateRange(uint32 count)
{
    if (const uint32 reusedBase = m_freeStateSlots.allocate(count); reusedBase != UINT32_MAX)
        return reusedBase;
    const uint32 base = m_stateCounter;
    m_stateCounter += count;
    if (m_stateCounter > m_maxStateSlots)
        growStateCapacity(m_stateCounter);
    return base;
}

void MeshLodRegistry::onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes)
{
    m_groupIdxBuffer.initialize(maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroupIdx");
    oc::vector<uint32> mapping(maxUniqueMeshes, UINT32_MAX);
    if (!m_meshToGroup.empty())
        memcpy(mapping.data(), m_meshToGroup.data(), m_meshToGroup.size() * sizeof(uint32));
    uploadShared(m_groupIdxBuffer, mapping.size() * sizeof(uint32), mapping.data(), 0);
}

void MeshLodRegistry::growStateCapacity(uint32 needed)
{
    m_maxStateSlots = growLodCapacity(m_maxStateSlots, needed);
    m_onGpuIdle();
    // Contents are advisory hysteresis history (clamped into a valid band on read), so the old
    // buffer's state doesn't need preserving.
    m_stateBuffer.initialize(m_maxStateSlots * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LodLevelState");
    m_onInvalidate();
    printf("Renderer: grew LOD state capacity to %u\n", m_maxStateSlots);
}

void MeshLodRegistry::growGroupCapacity(uint32 needed)
{
    m_maxGroups = growLodCapacity(m_maxGroups, needed);
    m_onGpuIdle();
    m_groupsBuffer.initialize(m_maxGroups * sizeof(RendererVKLayout::GpuMeshLodGroup),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroups");
    for (uint32 i = 0; i < (uint32)m_groups.size(); ++i)
        uploadGroup(i); // fresh buffer: re-publish every registered group
    m_onInvalidate();
    printf("Renderer: grew mesh LOD group capacity to %u\n", m_maxGroups);
}
