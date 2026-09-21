module RendererVK;

import Core;
import :InstanceStream;
import :Layout;

namespace
{
    uint32 growInstanceCapacity(uint32 current, uint32 needed)
    {
        uint64 capacity = current;
        while (capacity < needed)
            capacity *= 2;
        return (uint32)capacity;
    }
}

void InstanceStream::initialize(uint32 maxUniqueMeshes, oc::function<void()> onGpuIdle, oc::function<void()> onInvalidate,
    oc::function<void(uint32)> onInstanceCapacityGrown)
{
    m_maxUniqueMeshes = maxUniqueMeshes;
    m_onGpuIdle = oc::move(onGpuIdle);
    m_onInvalidate = oc::move(onInvalidate);
    m_onInstanceCapacityGrown = oc::move(onInstanceCapacityGrown);
    for (FrameSlot& s : m_slots)
    {
        createNodeBuffers(s);
        createInstanceBuffer(s);
        createFirstInstanceBuffer(s);
        // Indirect usage: consumed by DGC (sequenceCountAddress requires an INDIRECT_BUFFER).
        s.meshCount.initialize(sizeof(uint32),
            vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "MeshCount", BufferHostAccess::eSequentialWrite);
        s.mappedMeshCount = s.meshCount.mapMemory<uint32>();
        s.mappedMeshCount[0] = 0;
        s.meshCount.flushMappedMemory(sizeof(uint32));
    }
}

// The three per-render-node buffers. No contents to preserve on a re-create: the generation bump makes
// every node re-upload its transform at the next push, and masks/biases are rewritten by every push.
void InstanceStream::createNodeBuffers(FrameSlot& s)
{
    s.transforms.initialize(m_maxRenderNodes * sizeof(RendererVKLayout::RenderNodeTransform),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "RenderNodeTransforms", BufferHostAccess::eSequentialWrite);
    s.mappedTransforms = s.transforms.mapMemory<RendererVKLayout::RenderNodeTransform>();

    s.passMasks.initialize(m_maxRenderNodes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "NodePassMasks", BufferHostAccess::eSequentialWrite);
    s.mappedPassMasks = s.passMasks.mapMemory<uint32>();

    s.lodStateBias.initialize(m_maxRenderNodes * sizeof(int32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "NodeLodStateBias", BufferHostAccess::eSequentialWrite);
    s.mappedLodStateBias = s.lodStateBias.mapMemory<int32>();
}

void InstanceStream::createInstanceBuffer(FrameSlot& s)
{
    // Kept cached/random: growInstances reads the existing mapping to preserve in-flight instances
    // across a resize, so this buffer must stay CPU-readable.
    s.meshInstances.initialize(m_maxInstances * sizeof(RendererVKLayout::InMeshInstance),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "MeshInstances");
    s.mappedMeshInstances = s.meshInstances.mapMemory<RendererVKLayout::InMeshInstance>();
}

void InstanceStream::createFirstInstanceBuffer(FrameSlot& s)
{
    // No contents to preserve: first-instance offsets are rewritten every present().
    s.firstInstances.initialize(m_maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "FirstInstances", BufferHostAccess::eSequentialWrite);
    s.mappedFirstInstances = s.firstInstances.mapMemory<uint32>();
}

uint32 InstanceStream::claimInstances(uint32 count)
{
    const uint32 startIdx = oc::atomic_ref<uint32>(m_instanceCounter).fetch_add(count);
    if (startIdx + count <= m_maxInstances)
        return startIdx;
    // Doesn't fit this frame. NO rollback - un-bumping a non-top claim corrupts the cursor (later
    // claims would land above the final counter or leave unwritten gaps below it). The cursor is
    // monotonic, so no claim after this one can fit either: successful claims stay one contiguous
    // prefix, and present() clamps the counter to the smallest failed claim recorded here.
    oc::atomic_ref<uint32> overflow(m_instanceOverflowStart);
    uint32 curOverflow = overflow.load(oc::memory_order_relaxed);
    while (startIdx < curOverflow && !overflow.compare_exchange_weak(curOverflow, startIdx)) {}
    oc::atomic_ref<uint32> pending(m_pendingMaxInstances);
    uint32 cur = pending.load(oc::memory_order_relaxed);
    while (cur < startIdx + count && !pending.compare_exchange_weak(cur, startIdx + count)) {}
    return UINT32_MAX;
}

uint32 InstanceStream::allocateTransform(const Transform& transform)
{
    if (!m_freeTransformSlots.empty())
    {
        const uint32 idx = m_freeTransformSlots.back();
        m_freeTransformSlots.pop_back();
        m_transforms[idx] = transform;
        return idx; // a fresh RenderNode starts all-dirty, so the reused slot uploads at its first push
    }
    const uint32 idx = (uint32)m_transforms.size();
    m_transforms.emplace_back(transform);
    if ((uint32)m_transforms.size() > m_maxRenderNodes)
        growRenderNodes((uint32)m_transforms.size());
    return idx;
}

void InstanceStream::beginFrame()
{
    m_instanceCounter = 0;
    m_instanceOverflowStart = UINT32_MAX;
    memset(m_numInstancesPerMesh.data(), 0, m_numInstancesPerMesh.size() * sizeof(m_numInstancesPerMesh[0]));
}

void InstanceStream::growToPendingDemand(uint32 currentFrameIdx)
{
    if (m_pendingMaxInstances > m_maxInstances)
        growInstances(m_pendingMaxInstances, currentFrameIdx);
}

void InstanceStream::growRenderNodes(uint32 needed)
{
    m_maxRenderNodes = growInstanceCapacity(m_maxRenderNodes, needed);
    m_onGpuIdle();
    for (FrameSlot& s : m_slots)
        createNodeBuffers(s);
    ++m_bufferGeneration; // fresh (empty) GPU buffers: every node uploads again at its next push
    m_onInvalidate();
    printf("Renderer: grew render node capacity to %u\n", m_maxRenderNodes);
}

void InstanceStream::growInstances(uint32 needed, uint32 currentFrameIdx)
{
    m_maxInstances = growInstanceCapacity(m_maxInstances, needed);
    m_onGpuIdle();

    // renderNode() grows inline mid-frame, so the instances already written to the current frame's
    // mapped buffer this frame must survive the reallocation. (At the beginFrame call site the counter
    // holds last frame's count and the copy is redundant but harmless.)
    FrameSlot& current = m_slots[currentFrameIdx];
    oc::vector<RendererVKLayout::InMeshInstance> written(current.mappedMeshInstances.begin(),
        current.mappedMeshInstances.begin() + m_instanceCounter);
    for (FrameSlot& s : m_slots)
        createInstanceBuffer(s);
    memcpy(current.mappedMeshInstances.data(), written.data(), written.size() * sizeof(RendererVKLayout::InMeshInstance));

    m_onInstanceCapacityGrown(m_maxInstances); // the three cull pipelines' instance buffers
    m_onInvalidate();
    printf("Renderer: grew mesh instance capacity to %u\n", m_maxInstances);
}

void InstanceStream::onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes)
{
    m_maxUniqueMeshes = maxUniqueMeshes;
    for (FrameSlot& s : m_slots)
        createFirstInstanceBuffer(s);
}
