module Spatial;

import Core;
import Core.glm;
import Core.Tweaks;

static constexpr oc::string_view levelCellNames[Morton::MaxLevels] = {
    "L0 cells", "L1 cells", "L2 cells", "L3 cells", "L4 cells", "L5 cells",
    "L6 cells", "L7 cells", "L8 cells", "L9 cells", "L10 cells",
};
static constexpr oc::string_view levelEntryNames[Morton::MaxLevels] = {
    "L0 entries", "L1 entries", "L2 entries", "L3 entries", "L4 entries", "L5 entries",
    "L6 entries", "L7 entries", "L8 entries", "L9 entries", "L10 entries",
};

void SpatialEntry::reset()
{
    if (m_handle.isValid())
    {
        Globals::spatialIndex.unregisterEntry(m_handle);
        m_handle = {};
    }
}

// Lock-free max for the clamped-oversize radius (updateEntry runs concurrently on jobs).
static void atomicFloatMax(oc::atomic<float>& target, float value)
{
    float current = target.load(oc::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, oc::memory_order_relaxed)) {}
}

void SpatialIndex::initialize(const SpatialIndexDesc& desc)
{
    assert(!m_initialized);
    ProfileScope scope("SpatialIndex::initialize", EProfileCategory::Spatial);
    m_numLevels = glm::clamp(desc.numLevels, 2u, Morton::MaxLevels);
    for (uint32 i = 0; i < m_numLevels; ++i)
        m_levels[i].initialize(desc.initialCellCapacity);
    m_pool.initialize(desc.initialEntryCapacity);
    m_pendingOps.initialize(); // requires the JobSystem (sized by scheduler context count)
    m_pendingOps.forEach([](oc::vector<PendingOp>& ops) { ops.reserve(1024); });
    m_initialized = true;

    Tweak::intVar("Spatial/Stats", "Entries", &m_stats.numEntries, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells", &m_stats.numCells, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells tested", &m_stats.cellsTested, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells fully inside", &m_stats.cellsFullyInside, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Entity tests", &m_stats.entityTests, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Visible main", &m_stats.visiblePerPass[uint32(ESpatialPass::Main)], 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Visible near", &m_stats.visiblePerPass[uint32(ESpatialPass::Near)], 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Visible shadow", &m_stats.visiblePerPass[uint32(ESpatialPass::Shadow)], 0, INT32_MAX);
    Tweak::floatVar("Spatial/Stats", "Commit ms", &m_stats.commitMs, 0.0f, FLT_MAX, 0.001f);
    Tweak::floatVar("Spatial/Stats", "Mark visible ms", &m_stats.markVisibleMs, 0.0f, FLT_MAX, 0.001f);
    for (uint32 i = 0; i < m_numLevels; ++i)
    {
        Tweak::intVar("Spatial/Stats/Levels", levelCellNames[i], &m_stats.perLevelCells[i], 0, INT32_MAX);
        Tweak::intVar("Spatial/Stats/Levels", levelEntryNames[i], &m_stats.perLevelEntities[i], 0, INT32_MAX);
    }

    Tweak::intVar("Spatial/Stats", "Blocks", &m_stats.numBlocks, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cell moves", &m_stats.cellMoves, 0, INT32_MAX);

    static constexpr oc::string_view cullModeNames[] = { "Off", "Stats only", "Cull", "Main only (debug)" };
    Tweak::enumVar("Spatial/Culling", "Mode", &m_culling.mode, cullModeNames);
    Tweak::boolean("Spatial/Culling", "Freeze", &m_culling.freeze);
    Tweak::floatVar("Spatial/Culling", "Margin", &m_culling.margin, 0.0f, 64.0f, 0.1f);
    Tweak::floatVar("Spatial/Culling", "Near radius", &m_culling.nearRadius, 0.0f, 4096.0f, 1.0f);
    Tweak::floatVar("Spatial/Culling", "Shadow reach (m)", &m_culling.shadowReach, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Spatial/Culling", "Near requery slack", &m_culling.nearSlack, 0.0f, 128.0f, 0.5f);
    // Max dist is not tweaked: it's driven every frame from the render camera's far plane (setCullMaxDist).
    Tweak::floatVar("Spatial/Culling", "Skinned radius scale", &m_culling.skinnedRadiusScale, 1.0f, 4.0f, 0.01f);
}

uint32 SpatialIndex::entryLevel(float radius)
{
    uint32 level = Morton::levelForRadius(radius);
    if (level >= m_numLevels)
        level = m_numLevels - 1;
    if (float(Morton::cellSize(level)) < radius * 2.0f) // clamped oversize, widen top-level tests
        atomicFloatMax(m_topLevelMaxRadius, radius);
    return level;
}

SpatialHandle SpatialIndex::registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask, bool spawnVisible)
{
    assert(m_initialized && radius >= 0.0f);
    // All the pure math runs before the lock; the exclusive section is only the slot acquire (pool
    // growth reallocates the SoA a concurrent shared-locked traversal reads) + the SoA writes.
    const uint32 level = entryLevel(radius);
    const uint64 key = Morton::keyAtLevel(Morton::fineKey(pos), level);
    const glm::vec3 rel = glm::vec3(pos - Morton::cellMinWorld(key, level));
    uint32 idx, gen;
    {
        const std::unique_lock lock(m_registerMutex);
        idx = m_pool.acquire();
        m_pool.posX[idx] = rel.x;
        m_pool.posY[idx] = rel.y;
        m_pool.posZ[idx] = rel.z;
        m_pool.radius[idx] = radius;
        m_pool.cellKey[idx] = key;
        m_pool.userData[idx] = userData;
        m_pool.layerMask[idx] = uint8(layerMask);
        for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
            m_pool.lastVisible[p][idx] = 0; // never stamped: reports visible until the first query, unless NoSpawnGuard
        m_pool.storeIdx[idx] = UINT32_MAX;
        m_pool.level[idx] = uint8(level);
        m_pool.flags[idx] = uint8(RecordFlag_Alive | RecordFlag_Unlinked | (spawnVisible ? 0 : RecordFlag_NoSpawnGuard));
        gen = m_pool.gen[idx]; // captured under the lock: a later growth may move the array
    }
    // Staged per worker - no shared state, so the (possibly allocating) push stays outside the lock.
    m_pendingOps.local().push_back({ .newKey = key, .newRelPos = rel, .newRadius = radius,
                                     .idx = idx, .gen = gen, .type = PendingOp::Link, .newLevel = uint8(level) });
    return { idx, gen };
}

void SpatialIndex::unregisterEntry(SpatialHandle handle)
{
    {
        const std::unique_lock lock(m_registerMutex); // pairs with registerEntry (parallel spawn/despawn)
        if (!m_pool.isValidAlive(handle))
            return;
        const uint32 idx = handle.idx;
        if (!(m_pool.flags[idx] & RecordFlag_Unlinked))
        {
            // tombstone the lane (queries read it lock-free); the counters and the block bookkeeping
            // wait for the Unlink op at commit (retireLane)
            const uint32 slot = m_pool.storeIdx[idx];
            m_blocks[m_pool.level[idx]].blocks[BlockStore::blockOf(slot)].radius[BlockStore::laneOf(slot)] = CellBlock::Tombstone;
        }
        m_pool.flags[idx] = uint8((m_pool.flags[idx] & ~RecordFlag_Alive) | RecordFlag_PendingFree);
        m_pool.radius[idx] = -1e30f; // getRadius reads the pool copy; the lane above is what queries test
    }
    // Per-worker staging: outside the lock (only the handle's own gen is needed).
    m_pendingOps.local().push_back({ .newKey = 0, .newRelPos = glm::vec3(0.0f), .newRadius = 0.0f,
                                     .idx = handle.idx, .gen = handle.gen, .type = PendingOp::Unlink, .newLevel = 0 });
}

void SpatialIndex::updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    const uint32 level = entryLevel(radius);
    const uint64 key = Morton::keyAtLevel(Morton::fineKey(pos), level);
    const glm::vec3 rel = glm::vec3(pos - Morton::cellMinWorld(key, level));
    const uint8 flags = m_pool.flags[idx];
    if (key == m_pool.cellKey[idx] && level == m_pool.level[idx] && !(flags & RecordFlag_PendingMove))
    {
        if (rel.x == m_pool.posX[idx] && rel.y == m_pool.posY[idx] && rel.z == m_pool.posZ[idx] && radius == m_pool.radius[idx])
            return; // untouched
        // Same cell: in place, the pool copy and the lane queries read (no lane yet while Unlinked:
        // the Link op inserts from the pool copy).
        m_pool.posX[idx] = rel.x;
        m_pool.posY[idx] = rel.y;
        m_pool.posZ[idx] = rel.z;
        m_pool.radius[idx] = radius;
        if (!(flags & RecordFlag_Unlinked))
        {
            const uint32 slot = m_pool.storeIdx[idx];
            CellBlock& block = m_blocks[level].blocks[BlockStore::blockOf(slot)];
            const uint32 lane = BlockStore::laneOf(slot);
            block.posX[lane] = rel.x;
            block.posY[lane] = rel.y;
            block.posZ[lane] = rel.z;
            block.radius[lane] = radius;
        }
        return;
    }
    m_pool.flags[idx] = uint8(flags | RecordFlag_PendingMove);
    m_pendingOps.local().push_back({ .newKey = key, .newRelPos = rel, .newRadius = radius,
                                     .idx = idx, .gen = handle.gen, .type = PendingOp::Move, .newLevel = uint8(level) });
}

void SpatialIndex::setLayerMask(SpatialHandle handle, uint32 layerMask)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    m_pool.layerMask[idx] = uint8(layerMask);
    if (!(m_pool.flags[idx] & RecordFlag_Unlinked))
    {
        const uint32 slot = m_pool.storeIdx[idx];
        m_blocks[m_pool.level[idx]].blocks[BlockStore::blockOf(slot)].layer[BlockStore::laneOf(slot)] = layerMask;
    }
}

void SpatialIndex::commitFrame()
{
    ProfileScope profileScope("Spatial commit", EProfileCategory::Spatial);

    assert(m_initialized);
    const auto start = Clock::now();
    int cellMoves = 0;
    // Per-slot FIFO is the only ordering guarantee (one visitor per entity per pass keeps a frame's
    // Moves for one entry in one slot). The lone cross-slot case - Link (main) + first Move (worker)
    // in the same frame - self-heals: a Move that lands before its Link skips on RecordFlag_Unlinked
    // and leaves PendingMove set, which forces the next updateEntry to re-queue it.
    m_pendingOps.forEach([&](oc::vector<PendingOp>& ops)
    {
    for (const PendingOp& op : ops)
    {
        if (op.idx >= m_pool.capacity() || m_pool.gen[op.idx] != op.gen)
            continue;
        const uint8 opFlags = m_pool.flags[op.idx];
        switch (op.type)
        {
        case PendingOp::Link:
            if ((opFlags & (RecordFlag_Alive | RecordFlag_Unlinked)) == (RecordFlag_Alive | RecordFlag_Unlinked))
            {
                insertLane(op.idx);
                m_pool.flags[op.idx] = uint8(opFlags & ~RecordFlag_Unlinked);
                // Counts as visible for the current stamp generation; the next markVisibleSet (which
                // runs with the entry actually in the index) decides for real. NOT for NoSpawnGuard
                // entries: with the culling FROZEN no markVisibleSet ever re-decides, so this write
                // would leak every newly linked (off-screen-streamed) entry into the frozen main set.
                // RENDER passes only. The SIM LOD tiers are stamped by the World's PERIODIC
                // selection job: a "current" tier stamp made here would read as a real tier
                // (tier 0!) for frames, and the spawn-guard 0 as "in every tier" for ever - they
                // get the LINKED sentinel instead (hasStamp false: the World derives the tier from
                // the distance until the job stamps it). The root-dedupe passes stay 0: a fresh
                // root must not read as held.
                if (!(opFlags & RecordFlag_NoSpawnGuard))
                    for (uint32 p = 0; p < uint32(ESpatialPass::UpdateTier0); ++p)
                        m_pool.lastVisible[p][op.idx] = m_visibleQueryId[p];
                for (uint32 p = uint32(ESpatialPass::UpdateTier0); p <= uint32(ESpatialPass::UpdateTier2); ++p)
                    m_pool.lastVisible[p][op.idx] = SpatialStamp_Linked;
            }
            break;
        case PendingOp::Move:
            if ((opFlags & RecordFlag_Alive) && !(opFlags & RecordFlag_Unlinked))
            {
                retireLane(op.idx);
                m_pool.cellKey[op.idx] = op.newKey;
                m_pool.posX[op.idx] = op.newRelPos.x;
                m_pool.posY[op.idx] = op.newRelPos.y;
                m_pool.posZ[op.idx] = op.newRelPos.z;
                m_pool.radius[op.idx] = op.newRadius;
                m_pool.level[op.idx] = op.newLevel;
                m_pool.flags[op.idx] = uint8(m_pool.flags[op.idx] & ~RecordFlag_PendingMove);
                insertLane(op.idx);
                ++cellMoves;
            }
            break;
        case PendingOp::Unlink:
            if (opFlags & RecordFlag_PendingFree)
            {
                if (!(opFlags & RecordFlag_Unlinked))
                    retireLane(op.idx); // the lane was tombstoned at unregister; this settles the counters/blocks
                m_pool.release(op.idx);
            }
            break;
        }
    }
    ops.clear();
    });
    sweepEmptyCells();

    m_stats.numEntries = int(m_pool.numAlive());
    m_stats.cellMoves = cellMoves;
    int totalCells = 0;
    int totalBlocks = 0;
    for (uint32 i = 0; i < m_numLevels; ++i)
    {
        m_stats.perLevelCells[i] = int(m_levels[i].size());
        m_stats.perLevelEntities[i] = int(m_levelEntityCount[i]);
        totalCells += int(m_levels[i].size());
        totalBlocks += int(m_blocks[i].numBlocksInUse);
    }
    m_stats.numCells = totalCells;
    m_stats.numBlocks = totalBlocks;
    m_stats.cellsTested = 0;
    m_stats.cellsFullyInside = 0;
    m_stats.entityTests = 0;
    m_stats.markVisibleMs = 0.0f; // markVisible* calls accumulate into it after this commit
    m_stats.commitMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    ++m_frameId;
}

void SpatialIndex::insertLane(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    const uint64 key = m_pool.cellKey[idx];
    CellRecord& rec = m_levels[level].getOrCreate(key);
    m_pool.storeIdx[idx] = m_blocks[level].insert(rec.head, m_pool.posX[idx], m_pool.posY[idx], m_pool.posZ[idx],
                                                  m_pool.radius[idx], m_pool.layerMask[idx], idx);
    ++rec.count;
    ++m_levelEntityCount[level];
    setOccupancyBits(level, key);
}

void SpatialIndex::setOccupancyBits(uint32 level, uint64 key)
{
    // walk up the ancestors, stopping at the first already-set bit (each bit is set once per cell lifetime)
    for (uint32 lev = level + 1; lev < m_numLevels; ++lev)
    {
        const uint32 bit = Morton::childBit(key);
        key = Morton::parentKey(key);
        CellRecord& parent = m_levels[lev].getOrCreate(key);
        if (parent.childMask & (1ull << bit))
            return;
        parent.childMask |= 1ull << bit;
    }
}

void SpatialIndex::retireLane(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    BlockStore& store = m_blocks[level];
    const uint32 slot = m_pool.storeIdx[idx];
    const uint32 b = BlockStore::blockOf(slot);
    CellBlock& block = store.blocks[b];
    block.radius[BlockStore::laneOf(slot)] = CellBlock::Tombstone; // idempotent over unregisterEntry's early tombstone
    m_pool.storeIdx[idx] = UINT32_MAX;
    --block.live;
    --m_levelEntityCount[level];
    CellRecord* rec = m_levels[level].find(m_pool.cellKey[idx]);
    assert(rec && rec->count);
    --rec->count;
    if (rec->count == 0)
    {
        // the whole chain is dead: free it, and let the sweep judge the cell
        for (uint32 cur = rec->head; cur != UINT32_MAX;)
        {
            const uint32 next = store.blocks[cur].next;
            store.freeBlock(cur);
            cur = next;
        }
        rec->head = UINT32_MAX;
        if (rec->childMask == 0)
            m_emptyCandidates.push_back({ m_pool.cellKey[idx], level });
        return;
    }
    if (block.live == 0)
    {
        // unlink the emptied block from the cell's chain
        if (rec->head == b)
            rec->head = block.next;
        else
        {
            uint32 prev = rec->head;
            while (store.blocks[prev].next != b)
                prev = store.blocks[prev].next;
            store.blocks[prev].next = block.next;
        }
        store.freeBlock(b);
    }
    // compact once the dead lanes would fill a whole block (frees at least one block per pass, so
    // the cost stays one cell walk per 8 retirements)
    uint32 numBlocks = 0;
    for (uint32 cur = rec->head; cur != UINT32_MAX; cur = store.blocks[cur].next)
        ++numBlocks;
    if (numBlocks >= 2 && numBlocks * CellBlock::Lanes - rec->count >= CellBlock::Lanes)
        compactCell(level, *rec);
}

void SpatialIndex::compactCell(uint32 level, CellRecord& rec)
{
    BlockStore& store = m_blocks[level];
    oc::vector<CompactEntry>& live = m_compactScratch;
    live.clear();
    // Keep every lane the pool still OWNS, tombstoned or not: an entry unregistered this frame has
    // its lane tombstoned already but its Unlink op (retireLane, which settles the counters through
    // storeIdx) may still be queued behind this op - dropping the lane here would strand that slot.
    // Ownership = a live or pending-free record whose storeIdx names this lane (a released slot has
    // flags 0; a recycled one is Unlinked with storeIdx reset).
    for (uint32 cur = rec.head; cur != UINT32_MAX; cur = store.blocks[cur].next)
    {
        const CellBlock& block = store.blocks[cur];
        for (uint32 lane = 0; lane < block.count; ++lane)
        {
            const uint32 idx = block.poolIdx[lane];
            if ((m_pool.flags[idx] & (RecordFlag_Alive | RecordFlag_PendingFree)) && m_pool.storeIdx[idx] == BlockStore::slotOf(cur, lane))
                live.push_back({ block.posX[lane], block.posY[lane], block.posZ[lane], block.radius[lane], block.layer[lane], idx });
        }
    }
    assert(uint32(live.size()) == rec.count);
    // refill the chain's blocks from the head, then free the surplus tail
    uint32 cur = rec.head;
    uint32 prev = UINT32_MAX;
    for (uint32 i = 0; i < uint32(live.size()); cur = store.blocks[cur].next)
    {
        CellBlock& block = store.blocks[cur];
        const uint32 n = glm::min(CellBlock::Lanes, uint32(live.size()) - i);
        for (uint32 lane = 0; lane < n; ++lane, ++i)
        {
            const CompactEntry& e = live[i];
            block.posX[lane] = e.x; block.posY[lane] = e.y; block.posZ[lane] = e.z;
            block.radius[lane] = e.radius;
            block.layer[lane] = e.layer;
            block.poolIdx[lane] = e.poolIdx;
            m_pool.storeIdx[e.poolIdx] = BlockStore::slotOf(cur, lane);
        }
        for (uint32 lane = n; lane < CellBlock::Lanes; ++lane)
            block.radius[lane] = CellBlock::Tombstone;
        block.count = n;
        block.live = n;
        prev = cur;
    }
    if (prev == UINT32_MAX)
        rec.head = UINT32_MAX;
    else
        store.blocks[prev].next = UINT32_MAX;
    while (cur != UINT32_MAX)
    {
        const uint32 next = store.blocks[cur].next;
        store.freeBlock(cur);
        cur = next;
    }
}

void SpatialIndex::sweepEmptyCells()
{
    for (uint32 i = 0; i < m_emptyCandidates.size(); ++i) // grows while iterating as parents cascade
    {
        const EmptyCandidate candidate = m_emptyCandidates[i];
        const CellRecord* rec = m_levels[candidate.level].find(candidate.key);
        if (!rec || rec->count != 0 || rec->childMask != 0)
            continue; // resurrected this frame, or already swept
        m_levels[candidate.level].erase(candidate.key);
        if (candidate.level + 1 >= m_numLevels)
            continue;
        const uint32 bit = Morton::childBit(candidate.key);
        const uint64 parentKey = Morton::parentKey(candidate.key);
        CellRecord* parent = m_levels[candidate.level + 1].find(parentKey);
        if (!parent)
            continue;
        parent->childMask &= ~(1ull << bit);
        if (parent->count == 0 && parent->childMask == 0)
            m_emptyCandidates.push_back({ parentKey, candidate.level + 1 });
    }
    m_emptyCandidates.clear();
}

glm::dvec3 SpatialIndex::getPosition(SpatialHandle handle) const
{
    if (!m_pool.isValidAlive(handle))
        return glm::dvec3(0.0);
    const uint32 idx = handle.idx;
    return Morton::cellMinWorld(m_pool.cellKey[idx], m_pool.level[idx])
         + glm::dvec3(m_pool.posX[idx], m_pool.posY[idx], m_pool.posZ[idx]);
}

float SpatialIndex::getRadius(SpatialHandle handle) const
{
    return m_pool.isValidAlive(handle) ? m_pool.radius[handle.idx] : 0.0f;
}
