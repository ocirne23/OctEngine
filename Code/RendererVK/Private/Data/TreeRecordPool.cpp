module RendererVK;

import Core;
import Core.glm;

import :TreeRecordPool;
import :Buffer;
import :Device;
import :Layout;

void TreeRecordPool::reset(uint64 bytes, uint64 frame, uint32 ringRadius)
{
    // The chunk map: a power of two over the ring's diameter plus TreeWorld's eviction hysteresis.
    m_mapSize = 1;
    while (m_mapSize < 2 * (ringRadius + 2) + 1)
        m_mapSize *= 2;
    m_mapMirror.assign((size_t)m_mapSize * m_mapSize, TreeRecordMapGpu{});
    const uint32 numBlocks = (uint32)oc::min<uint64>(bytes / (BLOCK_RECORDS * sizeof(uint32)), UINT32_MAX / BLOCK_RECORDS);
    if (numBlocks != m_numBlocks)
    {
        if (m_records.getBuffer())
        {
            const vk::Result waitResult = Globals::device.graphicsQueueWaitIdle();
            assert(waitResult == vk::Result::eSuccess);
            (void)waitResult;
        }
        m_records.destroy();
        m_numBlocks = numBlocks;
        if (numBlocks > 0)
            m_records.initialize((vk::DeviceSize)numBlocks * BLOCK_RECORDS * sizeof(uint32),
                vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal, false, "TreeRecords");
        m_topBlock = 0;
        m_freeBlocks = {};
        m_pending.clear();
    }
    else
    {
        // Same pool: the live chunks' blocks go through the deferred free like any removal.
        for (uint32 handle : m_dense)
            m_pending.push_back({ m_chunks[handle].firstBlock, m_chunks[handle].blocks, frame + RendererVKLayout::NUM_FRAMES_IN_FLIGHT, frame });
    }
    m_chunks.clear();
    m_freeHandles.clear();
    m_dense.clear();
    m_numRecords = 0;
    m_usedBlocks = 0;
    m_refused = 0;
    m_tableDirty.fill(true);
}

uint32 TreeRecordPool::add(glm::ivec2 coord, oc::span<const uint32> ground, oc::span<const uint32> records)
{
    assert(ground.size() == TREE_RECORD_HEIGHT_WORDS);
    if (m_numBlocks == 0 || ground.size() != TREE_RECORD_HEIGHT_WORDS)
        return UINT32_MAX;
    const uint32 blocks = ((uint32)(TREE_RECORD_HEIGHT_WORDS + records.size()) + BLOCK_RECORDS - 1) / BLOCK_RECORDS;
    uint32 firstBlock = m_freeBlocks.allocate(blocks);
    if (firstBlock == UINT32_MAX)
    {
        if (m_topBlock + blocks > m_numBlocks)
        {
            ++m_refused;
            return UINT32_MAX;
        }
        firstBlock = m_topBlock;
        m_topBlock += blocks;
    }
    // The ground first, the records right after it.
    const uint32 base = firstBlock * BLOCK_RECORDS;
    m_records.upload(ground.size_bytes(), ground.data(), (vk::DeviceSize)base * sizeof(uint32));
    if (!records.empty())
        m_records.upload(records.size_bytes(), records.data(), (vk::DeviceSize)(base + TREE_RECORD_HEIGHT_WORDS) * sizeof(uint32));

    uint32 handle;
    if (!m_freeHandles.empty())
    {
        handle = m_freeHandles.back();
        m_freeHandles.pop_back();
    }
    else
    {
        handle = (uint32)m_chunks.size();
        m_chunks.emplace_back();
    }
    Chunk& chunk = m_chunks[handle];
    chunk.gpu = { coord, base + TREE_RECORD_HEIGHT_WORDS, (uint32)records.size() };
    chunk.firstBlock = firstBlock;
    chunk.blocks = blocks;
    m_mapMirror[mapIndex(coord)] = TreeRecordMapGpu{ coord, base };
    chunk.dense = (uint32)m_dense.size();
    m_dense.push_back(handle);
    m_numRecords += records.size();
    m_usedBlocks += blocks;
    m_tableDirty.fill(true);
    return handle;
}

void TreeRecordPool::remove(uint32 handle, uint64 frame)
{
    if (handle >= (uint32)m_chunks.size() || m_chunks[handle].dense == UINT32_MAX)
        return;
    Chunk& chunk = m_chunks[handle];
    m_pending.push_back({ chunk.firstBlock, chunk.blocks, frame + RendererVKLayout::NUM_FRAMES_IN_FLIGHT, frame });
    if (TreeRecordMapGpu& entry = m_mapMirror[mapIndex(chunk.gpu.coord)]; entry.coord == chunk.gpu.coord)
        entry = TreeRecordMapGpu{};
    // Swap-remove from the packed list.
    const uint32 last = m_dense.back();
    m_dense[chunk.dense] = last;
    m_chunks[last].dense = chunk.dense;
    m_dense.pop_back();
    m_numRecords -= chunk.gpu.count;
    m_usedBlocks -= chunk.blocks;
    chunk.dense = UINT32_MAX;
    m_freeHandles.push_back(handle);
    m_tableDirty.fill(true);
}

void TreeRecordPool::setTypes(oc::span<const TreeRecordTypeGpu> types)
{
    if (m_types.getBuffer())
    {
        const vk::Result waitResult = Globals::device.graphicsQueueWaitIdle(); // an in-flight bake may read them
        assert(waitResult == vk::Result::eSuccess);
        (void)waitResult;
    }
    m_types.destroy();
    m_numTypes = (uint32)types.size();
    if (types.empty())
        return;
    m_types.initialize(types.size_bytes(), vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeRecordTypes", BufferHostAccess::eSequentialWrite);
    m_types.upload(types.size_bytes(), types.data());
}

void TreeRecordPool::update(uint32 frameSlot, uint64 frame, uint64 holdSince)
{
    // A bake ended: what it held is reusable once its last frame is done.
    if (m_holdSince != UINT64_MAX && holdSince != m_holdSince)
        for (PendingFree& p : m_pending)
            if (p.removedFrame >= m_holdSince)
                p.readyFrame = oc::max(p.readyFrame, frame + RendererVKLayout::NUM_FRAMES_IN_FLIGHT);
    m_holdSince = holdSince;
    for (size_t i = 0; i < m_pending.size(); )
    {
        if (m_pending[i].readyFrame <= frame && (m_holdSince == UINT64_MAX || m_pending[i].removedFrame < m_holdSince))
        {
            m_freeBlocks.release(m_pending[i].firstBlock, m_pending[i].blocks);
            m_pending[i] = m_pending.back();
            m_pending.pop_back();
        }
        else
            ++i;
    }

    if (!m_tableDirty[frameSlot])
        return;
    m_tableDirty[frameSlot] = false;
    oc::vector<TreeRecordChunkGpu>& tableCpu = m_tableCpu[frameSlot];
    tableCpu.resize(m_dense.size());
    for (size_t i = 0; i < m_dense.size(); ++i)
        tableCpu[i] = m_chunks[m_dense[i]].gpu;
    Buffer& table = m_tables[frameSlot];
    const size_t bytes = tableCpu.size() * sizeof(TreeRecordChunkGpu);
    if (!table.getBuffer() || table.getSize() < bytes)
    {
        // This slot's last frame is done (beginFrame waited its fence): its buffer can be replaced.
        size_t capacity = oc::max<size_t>(table.getSize(), 1024 * sizeof(TreeRecordChunkGpu));
        while (capacity < bytes)
            capacity *= 2;
        table.initialize(capacity, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeRecordTable", BufferHostAccess::eSequentialWrite);
    }
    if (bytes > 0)
        table.upload(bytes, tableCpu.data());
    m_tableCounts[frameSlot] = (uint32)tableCpu.size();

    // The chunk map, whole (its size only changes at a reset).
    Buffer& map = m_maps[frameSlot];
    const size_t mapBytes = m_mapMirror.size() * sizeof(TreeRecordMapGpu);
    if (mapBytes == 0)
        return;
    if (!map.getBuffer() || map.getSize() != mapBytes)
        map.initialize(mapBytes, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeRecordMap", BufferHostAccess::eSequentialWrite);
    map.upload(mapBytes, m_mapMirror.data());
}

TreeRecordStats TreeRecordPool::stats() const
{
    TreeRecordStats s;
    s.chunks = (uint32)m_dense.size();
    s.records = m_numRecords;
    s.usedBytes = m_usedBlocks * BLOCK_RECORDS * sizeof(uint32);
    s.poolBytes = (uint64)m_numBlocks * BLOCK_RECORDS * sizeof(uint32);
    for (const PendingFree& p : m_pending)
        s.pendingBytes += (uint64)p.blocks * BLOCK_RECORDS * sizeof(uint32);
    s.refused = m_refused;
    return s;
}
