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
    Tweak::floatVar("Spatial/Stats", "Commit ms", &m_stats.commitMs, 0.0f, FLT_MAX, 0.001f);
    Tweak::floatVar("Spatial/Stats", "Mark visible ms", &m_stats.markVisibleMs, 0.0f, FLT_MAX, 0.001f);
    for (uint32 i = 0; i < m_numLevels; ++i)
    {
        Tweak::intVar("Spatial/Stats/Levels", levelCellNames[i], &m_stats.perLevelCells[i], 0, INT32_MAX);
        Tweak::intVar("Spatial/Stats/Levels", levelEntryNames[i], &m_stats.perLevelEntities[i], 0, INT32_MAX);
    }

    Tweak::boolean("Spatial/Static", "Enabled", &m_staticEnabled);
    Tweak::intVar("Spatial/Static", "Promote after frames", &m_promoteAfterFrames, 1, 1000);
    Tweak::intVar("Spatial/Static", "Scan budget", &m_staticScanBudget, 64, 1'000'000);
    Tweak::floatVar("Spatial/Static", "Radius tolerance", &m_staticRadiusTolerance, 0.0f, 0.5f, 0.005f);
    Tweak::floatVar("Spatial/Static", "Position tolerance", &m_staticPositionTolerance, 0.0f, 10.0f, 0.01f);
    Tweak::intVar("Spatial/Stats", "Static entries", &m_stats.staticEntries, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Static blocks", &m_stats.staticBlocks, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Static promotions", &m_stats.staticPromotions, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Static demotions", &m_stats.staticDemotions, 0, INT32_MAX);

    static constexpr oc::string_view cullModeNames[] = { "Off", "Stats only", "Cull", "Main only (debug)" };
    Tweak::enumVar("Spatial/Culling", "Mode", &m_culling.mode, cullModeNames);
    Tweak::boolean("Spatial/Culling", "Freeze", &m_culling.freeze);
    Tweak::floatVar("Spatial/Culling", "Margin", &m_culling.margin, 0.0f, 64.0f, 0.1f);
    Tweak::floatVar("Spatial/Culling", "Near radius", &m_culling.nearRadius, 0.0f, 4096.0f, 1.0f);
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
        m_pool.next[idx] = UINT32_MAX;
        m_pool.prev[idx] = UINT32_MAX;
        m_pool.layerMask[idx] = uint8(layerMask);
        for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
            m_pool.lastVisible[p][idx] = 0; // never stamped: reports visible until the first query, unless NoSpawnGuard
        m_pool.lastMoveFrame[idx] = uint16(m_frameId);
        m_pool.storeIdx[idx] = UINT32_MAX;
        m_pool.level[idx] = uint8(level);
        m_pool.flags[idx] = uint8(RecordFlag_Alive | RecordFlag_Unlinked | (spawnVisible ? 0 : RecordFlag_NoSpawnGuard));
        gen = m_pool.gen[idx]; // captured under the lock: a later growth may move the array
    }
    // Staged per worker — no shared state, so the (possibly allocating) push stays outside the lock.
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
        if (m_pool.flags[idx] & RecordFlag_StaticTier)
        {
            // tombstone the stored lane too (queries read it lock-free); the counters and the block
            // bookkeeping wait for the Unlink op at commit (retireStatic)
            const uint32 slot = m_pool.storeIdx[idx];
            m_static[m_pool.level[idx]].blocks[StaticStore::blockOf(slot)].radius[StaticStore::laneOf(slot)] = StaticBlock::Tombstone;
        }
        m_pool.flags[idx] = uint8((m_pool.flags[idx] & ~RecordFlag_Alive) | RecordFlag_PendingFree);
        m_pool.radius[idx] = -1e30f; // queries this frame can no longer return the dying entry
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
    const bool isStatic = (m_pool.flags[idx] & (RecordFlag_StaticTier | RecordFlag_PendingMove)) == RecordFlag_StaticTier;
    if (isStatic && radius != m_pool.radius[idx]
        && glm::abs(radius - m_pool.radius[idx]) <= m_pool.radius[idx] * m_staticRadiusTolerance)
        radius = m_pool.radius[idx]; // drift inside the band: keep the stored radius, so the level cannot flip either
    const uint32 level = entryLevel(radius);
    const uint64 key = Morton::keyAtLevel(Morton::fineKey(pos), level);
    const glm::vec3 rel = glm::vec3(pos - Morton::cellMinWorld(key, level));
    if (key == m_pool.cellKey[idx] && level == m_pool.level[idx] && !(m_pool.flags[idx] & RecordFlag_PendingMove))
    {
        if (rel.x == m_pool.posX[idx] && rel.y == m_pool.posY[idx] && rel.z == m_pool.posZ[idx] && radius == m_pool.radius[idx])
            return; // untouched, keeps lastMoveFrame aging for the static tier
        if (isStatic && radius == m_pool.radius[idx])
        {
            const glm::vec3 delta = rel - glm::vec3(m_pool.posX[idx], m_pool.posY[idx], m_pool.posZ[idx]);
            if (glm::dot(delta, delta) <= m_staticPositionTolerance * m_staticPositionTolerance)
                return; // drift inside the band: the stored copy stays, lastMoveFrame keeps aging
        }
        if (!(m_pool.flags[idx] & RecordFlag_StaticTier)) // any change to a static entry demotes it via Move
        {
            m_pool.posX[idx] = rel.x;
            m_pool.posY[idx] = rel.y;
            m_pool.posZ[idx] = rel.z;
            m_pool.radius[idx] = radius;
            m_pool.lastMoveFrame[idx] = uint16(m_frameId);
            return;
        }
    }
    m_pool.flags[idx] = uint8(m_pool.flags[idx] | RecordFlag_PendingMove);
    m_pool.lastMoveFrame[idx] = uint16(m_frameId);
    m_pendingOps.local().push_back({ .newKey = key, .newRelPos = rel, .newRadius = radius,
                                     .idx = idx, .gen = handle.gen, .type = PendingOp::Move, .newLevel = uint8(level) });
}

void SpatialIndex::setLayerMask(SpatialHandle handle, uint32 layerMask)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    m_pool.layerMask[idx] = uint8(layerMask);
    if (m_pool.flags[idx] & RecordFlag_StaticTier)
    {
        const uint32 slot = m_pool.storeIdx[idx];
        m_static[m_pool.level[idx]].blocks[StaticStore::blockOf(slot)].layer[StaticStore::laneOf(slot)] = layerMask;
    }
}

void SpatialIndex::commitFrame()
{
    ProfileScope profileScope("Spatial commit", EProfileCategory::Spatial);

    assert(m_initialized);
    const auto start = Clock::now();
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
                linkIntoCell(op.idx);
                m_pool.flags[op.idx] = uint8(opFlags & ~RecordFlag_Unlinked);
                // Counts as visible for the current stamp generation; the next markVisibleSet (which
                // runs with the entry actually in the index) decides for real. NOT for NoSpawnGuard
                // entries: with the culling FROZEN no markVisibleSet ever re-decides, so this write
                // would leak every newly linked (off-screen-streamed) entry into the frozen main set.
                // RENDER passes only. The SIM LOD tiers are stamped by the World's PERIODIC
                // selection job: a "current" tier stamp made here would read as a real tier
                // (tier 0!) for frames, and the spawn-guard 0 as "in every tier" for ever — they
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
                demoteOrUnlink(op.idx);
                m_pool.cellKey[op.idx] = op.newKey;
                m_pool.posX[op.idx] = op.newRelPos.x;
                m_pool.posY[op.idx] = op.newRelPos.y;
                m_pool.posZ[op.idx] = op.newRelPos.z;
                m_pool.radius[op.idx] = op.newRadius;
                m_pool.level[op.idx] = op.newLevel;
                m_pool.flags[op.idx] = uint8(m_pool.flags[op.idx] & ~RecordFlag_PendingMove);
                linkIntoCell(op.idx);
            }
            break;
        case PendingOp::Unlink:
            if (opFlags & RecordFlag_PendingFree)
            {
                if (opFlags & RecordFlag_StaticTier)
                    retireStatic(op.idx); // the lane was tombstoned at unregister; this settles the counters/blocks
                else if (!(opFlags & RecordFlag_Unlinked))
                    unlinkFromCell(op.idx);
                m_pool.release(op.idx);
            }
            break;
        }
    }
    ops.clear();
    });
    if (m_staticEnabled)
        promotionScan(); // promotes in place: no rebuild phase follows
    sweepEmptyCells();

    m_stats.numEntries = int(m_pool.numAlive());
    int totalCells = 0;
    int totalStatic = 0;
    int totalBlocks = 0;
    for (uint32 i = 0; i < m_numLevels; ++i)
    {
        m_stats.perLevelCells[i] = int(m_levels[i].size());
        m_stats.perLevelEntities[i] = int(m_levelEntityCount[i]);
        totalCells += int(m_levels[i].size());
        totalStatic += int(m_static[i].numLive);
        totalBlocks += int(m_static[i].numBlocksInUse);
    }
    m_stats.numCells = totalCells;
    m_stats.staticEntries = totalStatic;
    m_stats.staticBlocks = totalBlocks;
    m_stats.cellsTested = 0;
    m_stats.cellsFullyInside = 0;
    m_stats.entityTests = 0;
    m_stats.markVisibleMs = 0.0f; // markVisible* calls accumulate into it after this commit
    m_stats.commitMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    ++m_frameId;
}

void SpatialIndex::linkIntoCell(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    const uint64 key = m_pool.cellKey[idx];
    CellRecord& rec = m_levels[level].getOrCreate(key);
    const uint32 head = rec.dynHead;
    m_pool.next[idx] = head;
    m_pool.prev[idx] = UINT32_MAX;
    if (head != UINT32_MAX)
        m_pool.prev[head] = idx;
    rec.dynHead = idx;
    ++rec.dynCount;
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

void SpatialIndex::demoteOrUnlink(uint32 idx)
{
    if (m_pool.flags[idx] & RecordFlag_StaticTier)
    {
        retireStatic(idx); // stored, not in a dynamic list
        m_pool.flags[idx] = uint8(m_pool.flags[idx] & ~RecordFlag_StaticTier);
        ++m_stats.staticDemotions;
    }
    else
        unlinkFromCell(idx);
}

void SpatialIndex::unlinkFromCell(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    const uint64 key = m_pool.cellKey[idx];
    CellRecord* rec = m_levels[level].find(key);
    assert(rec);
    const uint32 nxt = m_pool.next[idx];
    const uint32 prv = m_pool.prev[idx];
    if (prv != UINT32_MAX) m_pool.next[prv] = nxt; else rec->dynHead = nxt;
    if (nxt != UINT32_MAX) m_pool.prev[nxt] = prv;
    m_pool.next[idx] = UINT32_MAX;
    m_pool.prev[idx] = UINT32_MAX;
    --rec->dynCount;
    --m_levelEntityCount[level];
    if (rec->dynCount == 0 && rec->childMask == 0 && rec->staticCount == 0)
        m_emptyCandidates.push_back({ key, level });
}

void SpatialIndex::promotionScan()
{
    if (m_promoteAfterFrames <= 0)
        return;
    const uint32 capacity = m_pool.capacity();
    uint32 budget = glm::min(uint32(m_staticScanBudget), capacity);
    while (budget--)
    {
        const uint32 idx = m_promoteCursor;
        m_promoteCursor = m_promoteCursor + 1 < capacity ? m_promoteCursor + 1 : 0;
        constexpr uint8 exclude = RecordFlag_Unlinked | RecordFlag_PendingFree | RecordFlag_PendingMove | RecordFlag_StaticTier;
        if ((m_pool.flags[idx] & (RecordFlag_Alive | exclude)) != RecordFlag_Alive)
            continue;
        if (uint16(uint16(m_frameId) - m_pool.lastMoveFrame[idx]) < uint16(glm::min(m_promoteAfterFrames, 65535)))
            continue; // modular 16-bit age: an entry static for > 65k frames reads young for a few frames — harmless
        promoteEntry(idx);
    }
}

void SpatialIndex::promoteEntry(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    CellRecord* rec = m_levels[level].find(m_pool.cellKey[idx]);
    assert(rec); // linked, so its cell exists
    m_pool.storeIdx[idx] = m_static[level].insert(rec->staticHead, m_pool.posX[idx], m_pool.posY[idx], m_pool.posZ[idx],
                                                  m_pool.radius[idx], m_pool.layerMask[idx], idx);
    ++rec->staticCount; // before the unlink, so an emptied dynamic list does not nominate the cell for the sweep
    m_pool.flags[idx] = uint8(m_pool.flags[idx] | RecordFlag_StaticTier);
    unlinkFromCell(idx);
    ++m_stats.staticPromotions;
}

void SpatialIndex::retireStatic(uint32 idx)
{
    const uint32 level = m_pool.level[idx];
    StaticStore& store = m_static[level];
    const uint32 slot = m_pool.storeIdx[idx];
    const uint32 b = StaticStore::blockOf(slot);
    StaticBlock& block = store.blocks[b];
    block.radius[StaticStore::laneOf(slot)] = StaticBlock::Tombstone; // idempotent over unregisterEntry's early tombstone
    --block.live;
    --store.numLive;
    CellRecord* rec = m_levels[level].find(m_pool.cellKey[idx]);
    assert(rec && rec->staticCount);
    --rec->staticCount;
    if (rec->staticCount == 0)
    {
        // the whole chain is dead: free it, and let the sweep judge the cell
        for (uint32 cur = rec->staticHead; cur != UINT32_MAX;)
        {
            const uint32 next = store.blocks[cur].next;
            store.freeBlock(cur);
            cur = next;
        }
        rec->staticHead = UINT32_MAX;
        if (rec->dynCount == 0 && rec->childMask == 0)
            m_emptyCandidates.push_back({ m_pool.cellKey[idx], level });
        return;
    }
    if (block.live == 0)
    {
        // unlink the emptied block from the cell's chain
        if (rec->staticHead == b)
            rec->staticHead = block.next;
        else
        {
            uint32 prev = rec->staticHead;
            while (store.blocks[prev].next != b)
                prev = store.blocks[prev].next;
            store.blocks[prev].next = block.next;
        }
        store.freeBlock(b);
    }
    // compact once the dead lanes would fill a whole block (frees at least one block per pass, so
    // the cost stays one cell walk per 8 demotions)
    uint32 numBlocks = 0;
    for (uint32 cur = rec->staticHead; cur != UINT32_MAX; cur = store.blocks[cur].next)
        ++numBlocks;
    if (numBlocks >= 2 && numBlocks * StaticBlock::Lanes - rec->staticCount >= StaticBlock::Lanes)
        compactStaticCell(level, *rec);
}

void SpatialIndex::compactStaticCell(uint32 level, CellRecord& rec)
{
    StaticStore& store = m_static[level];
    oc::vector<CompactEntry>& live = m_compactScratch;
    live.clear();
    // Keep every lane the pool still OWNS, tombstoned or not: an entry unregistered this frame has
    // its lane tombstoned already but its Unlink op (retireStatic, which settles the counters through
    // storeIdx) may still be queued behind this op — dropping the lane here would strand that slot.
    for (uint32 cur = rec.staticHead; cur != UINT32_MAX; cur = store.blocks[cur].next)
    {
        const StaticBlock& block = store.blocks[cur];
        for (uint32 lane = 0; lane < block.count; ++lane)
        {
            const uint32 idx = block.poolIdx[lane];
            if ((m_pool.flags[idx] & RecordFlag_StaticTier) && m_pool.storeIdx[idx] == StaticStore::slotOf(cur, lane))
                live.push_back({ block.posX[lane], block.posY[lane], block.posZ[lane], block.radius[lane], block.layer[lane], idx });
        }
    }
    assert(uint32(live.size()) == rec.staticCount);
    // refill the chain's blocks from the head, then free the surplus tail
    uint32 cur = rec.staticHead;
    uint32 prev = UINT32_MAX;
    for (uint32 i = 0; i < uint32(live.size()); cur = store.blocks[cur].next)
    {
        StaticBlock& block = store.blocks[cur];
        const uint32 n = glm::min(StaticBlock::Lanes, uint32(live.size()) - i);
        for (uint32 lane = 0; lane < n; ++lane, ++i)
        {
            const CompactEntry& e = live[i];
            block.posX[lane] = e.x; block.posY[lane] = e.y; block.posZ[lane] = e.z;
            block.radius[lane] = e.radius;
            block.layer[lane] = e.layer;
            block.poolIdx[lane] = e.poolIdx;
            m_pool.storeIdx[e.poolIdx] = StaticStore::slotOf(cur, lane);
        }
        for (uint32 lane = n; lane < StaticBlock::Lanes; ++lane)
            block.radius[lane] = StaticBlock::Tombstone;
        block.count = n;
        block.live = n;
        prev = cur;
    }
    if (prev == UINT32_MAX)
        rec.staticHead = UINT32_MAX;
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
        if (!rec || rec->dynCount != 0 || rec->childMask != 0 || rec->staticCount != 0)
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
        if (parent->dynCount == 0 && parent->childMask == 0)
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
