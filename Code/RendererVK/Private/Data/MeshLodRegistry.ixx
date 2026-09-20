export module RendererVK:MeshLodRegistry;

import Core;
import Core.glm;
import :Buffer;
import :Layout;

// Recycles contiguous slot ranges freed by destroyed ObjectContainers (mesh infos, materials, instance
// offsets, skinning jobs, ...). Ranges stay sorted and coalesced; allocation is best-fit so small
// requests don't shred the large holes. All quantities are element counts, not bytes.
export struct IndexRangeFreeList
{
    struct Range { uint32 base; uint32 count; };

    uint32 allocate(uint32 count)
    {
        int32 best = -1;
        for (int32 i = 0; i < (int32)m_ranges.size(); ++i)
            if (m_ranges[i].count >= count && (best < 0 || m_ranges[i].count < m_ranges[best].count))
                best = i;
        if (best < 0)
            return UINT32_MAX;
        const uint32 base = m_ranges[best].base;
        m_ranges[best].base += count;
        m_ranges[best].count -= count;
        if (m_ranges[best].count == 0)
            m_ranges.erase(m_ranges.begin() + best);
        return base;
    }

    void release(uint32 base, uint32 count)
    {
        if (count == 0)
            return;
        auto it = oc::lower_bound(m_ranges.begin(), m_ranges.end(), base,
            [](const Range& range, uint32 b) { return range.base < b; });
        it = m_ranges.insert(it, Range{ base, count });
        if (auto next = it + 1; next != m_ranges.end() && it->base + it->count == next->base)
        {
            it->count += next->count;
            m_ranges.erase(next);
        }
        if (it != m_ranges.begin() && (it - 1)->base + (it - 1)->count == it->base)
        {
            (it - 1)->count += it->count;
            m_ranges.erase(it);
        }
    }

private:
    oc::vector<Range> m_ranges; // sorted by base, no two adjacent
};

// One mesh LOD chain: global mesh indices per level ([0] = full resolution) sharing one set of local
// bounds. Registered by ObjectContainer at load, referenced by RenderNode::m_lodInstances; selection
// happens per instance on the GPU (mesh_lod.inc.glsl in the cull shaders), fed by the GpuMeshLodGroup
// mirror MeshLodRegistry keeps.
export struct MeshLodGroup
{
    uint16 meshIdx[RendererVKLayout::MAX_MESH_LODS] = {};
    uint8 numLods = 0;
    glm::vec3 center = glm::vec3(0.0f); // LOD0 local bounds
    float radius = 0.0f;
    // Geometric deviation of each level from LOD0 in mesh-local units (meshopt simplify error x mesh
    // extents), 0 for level 0. Drives screen-space-error selection: a level is usable when its error
    // projects below "LOD/Max error (px)". All-zero (authored chains without error data) falls back to
    // the projected-size metric.
    float errors[RendererVKLayout::MAX_MESH_LODS] = {};
    uint32 lastUseFrame = UINT32_MAX; // frame stamp for the once-per-frame chain-warmth noteUse
};

// THE CPU side of GPU LOD selection, owned by the Renderer: the registered chains, the per-MeshInfo
// chain mapping and the per-instance hysteresis slots, plus the three device-local buffers the cull
// shaders read (mesh_lod.inc.glsl). Nothing here touches per-frame state - the Renderer hands it the
// two frame-wide effects a capacity growth needs: onGpuIdle (drain + flush staging before a buffer is
// destroyed) and onInvalidate (re-record the cached secondaries that baked a handle).
//
// The hysteresis state buffer is advisory: the shader clamps whatever it reads into the frame's valid
// band, so a grow may drop its contents and cross-frame races are benign.
export class MeshLodRegistry final
{
public:
    void initialize(uint32 maxUniqueMeshes, oc::function<void()> onGpuIdle, oc::function<void()> onInvalidate)
    {
        m_onGpuIdle = oc::move(onGpuIdle);
        m_onInvalidate = oc::move(onInvalidate);
        m_groupIdxBuffer.initialize(maxUniqueMeshes * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroupIdx");
        m_groupsBuffer.initialize(m_maxGroups * sizeof(RendererVKLayout::GpuMeshLodGroup),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroups");
        m_stateBuffer.initialize(m_maxStateSlots * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LodLevelState");
    }

    // Registers a chain and points every member mesh at it. The caller still owns the RT-alias side
    // (AccelerationStructure), which is why this does not do it.
    uint32 addGroup(const MeshLodGroup& group);
    // Detaches a chain's member meshes from GPU selection, then recycles the slot.
    void freeGroup(uint32 groupIdx);
    void setGroupIdxForMesh(uint16 meshIdx, uint32 groupIdx);
    void uploadGroup(uint32 groupIdx);

    // Per-instance LOD hysteresis state slots, one contiguous range per RenderNode with LOD chains.
    // NOT internally locked - the caller holds the spawn mutex (parallel entity spawning).
    uint32 allocateStateRange(uint32 count);
    void releaseStateRange(uint32 base, uint32 count) { m_freeStateSlots.release(base, count); }

    // The per-MeshInfo mapping mirrors m_groupIdxBuffer; the Renderer grows it with the MeshInfo array
    // and re-uploads the slice a new container claimed.
    void resizeMeshMapping(uint32 meshInfoCount) { m_meshToGroup.resize(meshInfoCount, UINT32_MAX); }
    void uploadMeshMapping(uint32 baseMeshInfoIdx, uint32 count);
    // A unique-mesh capacity growth re-creates the (unmirrored) index buffer, so the whole mapping is
    // re-uploaded padded to the new capacity - future slots must read as "no chain".
    void onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes);

    uint32 getGroupIdxForMesh(uint16 meshIdx) const { return m_meshToGroup[meshIdx]; }
    MeshLodGroup& getGroup(uint32 groupIdx) { return m_groups[groupIdx]; }
    const MeshLodGroup& getGroup(uint32 groupIdx) const { return m_groups[groupIdx]; }
    uint32 getNumGroups() const { return (uint32)m_groups.size(); }

    Buffer& getGroupIdxBuffer() { return m_groupIdxBuffer; }
    Buffer& getGroupsBuffer() { return m_groupsBuffer; }
    Buffer& getStateBuffer() { return m_stateBuffer; }

private:
    void growGroupCapacity(uint32 needed);
    void growStateCapacity(uint32 needed);
    void uploadShared(Buffer& buffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset);

    oc::function<void()> m_onGpuIdle;
    oc::function<void()> m_onInvalidate;

    oc::vector<MeshLodGroup> m_groups;
    oc::vector<uint32> m_freeGroups;
    oc::vector<uint32> m_meshToGroup; // per MeshInfo: owning group (UINT32_MAX = no chain)

    Buffer m_groupIdxBuffer;
    Buffer m_groupsBuffer;
    Buffer m_stateBuffer;
    IndexRangeFreeList m_freeStateSlots;
    uint32 m_stateCounter = 0;
    uint32 m_maxStateSlots = RendererVKLayout::INITIAL_LOD_STATE_SLOTS;
    uint32 m_maxGroups = RendererVKLayout::INITIAL_MESH_LOD_GROUPS;
};
