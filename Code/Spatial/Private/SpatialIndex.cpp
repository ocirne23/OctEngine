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
    Tweak::intVar("Spatial/Static", "Scan budget", &m_staticScanBudget, 1024, 1'000'000);
    Tweak::intVar("Spatial/Static", "Rebuild batch", &m_staticRebuildBatch, 64, 1'000'000);
    Tweak::intVar("Spatial/Stats", "Static entries", &m_stats.staticEntries, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Static rebuilds", &m_stats.staticRebuilds, 0, INT32_MAX);
    Tweak::floatVar("Spatial/Stats", "Rebuild ms", &m_stats.rebuildMs, 0.0f, FLT_MAX, 0.001f);

    static constexpr oc::string_view cullModeNames[] = { "Off", "Stats only", "Cull", "Main only (debug)" };
    Tweak::enumVar("Spatial/Culling", "Mode", &m_culling.mode, cullModeNames);
    Tweak::boolean("Spatial/Culling", "Freeze", &m_culling.freeze);
    Tweak::floatVar("Spatial/Culling", "Margin", &m_culling.margin, 0.0f, 64.0f, 0.1f);
    Tweak::floatVar("Spatial/Culling", "Near radius", &m_culling.nearRadius, 0.0f, 4096.0f, 1.0f);
    Tweak::floatVar("Spatial/Culling", "Near requery slack", &m_culling.nearSlack, 0.0f, 128.0f, 0.5f);
    // Max dist is not tweaked: it's driven every frame from the render camera's far plane (setCullMaxDist).
    Tweak::floatVar("Spatial/Culling", "Skinned radius scale", &m_culling.skinnedRadiusScale, 1.0f, 4.0f, 0.01f);
}

SpatialHandle SpatialIndex::registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask, bool spawnVisible)
{
    assert(m_initialized && radius >= 0.0f);
    const uint32 idx = m_pool.acquire();
    uint32 level = Morton::levelForRadius(radius);
    if (level >= m_numLevels)
        level = m_numLevels - 1;
    if (float(Morton::cellSize(level)) < radius * 2.0f) // clamped oversize, widen top-level tests
        atomicFloatMax(m_topLevelMaxRadius, radius);
    const uint64 key = Morton::keyAtLevel(Morton::fineKey(pos), level);
    const glm::vec3 rel = glm::vec3(pos - Morton::cellMinWorld(key, level));
    m_pool.posX[idx] = rel.x;
    m_pool.posY[idx] = rel.y;
    m_pool.posZ[idx] = rel.z;
    m_pool.radius[idx] = radius;
    m_pool.cellKey[idx] = key;
    m_pool.userData[idx] = userData;
    m_pool.next[idx] = UINT32_MAX;
    m_pool.prev[idx] = UINT32_MAX;
    m_pool.layerMask[idx] = layerMask;
    for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
        m_pool.lastVisible[p][idx] = 0; // never stamped: reports visible until the first query, unless NoSpawnGuard
    m_pool.lastMoveFrame[idx] = m_frameId;
    m_pool.storeIdx[idx] = UINT32_MAX;
    m_pool.level[idx] = uint8(level);
    m_pool.flags[idx] = uint8(RecordFlag_Alive | RecordFlag_Unlinked | (spawnVisible ? 0 : RecordFlag_NoSpawnGuard));
    m_pendingOps.local().push_back({ .newKey = key, .newRelPos = rel, .newRadius = radius,
                                     .idx = idx, .gen = m_pool.gen[idx], .type = PendingOp::Link, .newLevel = uint8(level) });
    return { idx, m_pool.gen[idx] };
}

void SpatialIndex::unregisterEntry(SpatialHandle handle)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    if ((m_pool.flags[idx] & RecordFlag_StaticTier) && m_pool.storeIdx[idx] != UINT32_MAX)
    {
        StaticStore& store = m_static[m_pool.level[idx]];
        store.radius[m_pool.storeIdx[idx]] = -1e30f; // tombstone the stored copy too
        ++store.numTombstones;
    }
    m_pool.flags[idx] = uint8((m_pool.flags[idx] & ~RecordFlag_Alive) | RecordFlag_PendingFree);
    m_pool.radius[idx] = -1e30f; // queries this frame can no longer return the dying entry
    m_pendingOps.local().push_back({ .newKey = 0, .newRelPos = glm::vec3(0.0f), .newRadius = 0.0f,
                                     .idx = idx, .gen = handle.gen, .type = PendingOp::Unlink, .newLevel = 0 });
}

void SpatialIndex::updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    uint32 level = m_pool.level[idx];
    if (radius != m_pool.radius[idx])
    {
        level = Morton::levelForRadius(radius);
        if (level >= m_numLevels)
            level = m_numLevels - 1;
        if (float(Morton::cellSize(level)) < radius * 2.0f)
            atomicFloatMax(m_topLevelMaxRadius, radius);
    }
    const uint64 key = Morton::keyAtLevel(Morton::fineKey(pos), level);
    const glm::vec3 rel = glm::vec3(pos - Morton::cellMinWorld(key, level));
    if (key == m_pool.cellKey[idx] && level == m_pool.level[idx] && !(m_pool.flags[idx] & RecordFlag_PendingMove))
    {
        if (rel.x == m_pool.posX[idx] && rel.y == m_pool.posY[idx] && rel.z == m_pool.posZ[idx] && radius == m_pool.radius[idx])
            return; // untouched, keeps lastMoveFrame aging for the static tier
        if (!(m_pool.flags[idx] & RecordFlag_StaticTier)) // any change to a static entry demotes it via Move
        {
            m_pool.posX[idx] = rel.x;
            m_pool.posY[idx] = rel.y;
            m_pool.posZ[idx] = rel.z;
            m_pool.radius[idx] = radius;
            m_pool.lastMoveFrame[idx] = m_frameId;
            return;
        }
    }
    m_pool.flags[idx] = uint8(m_pool.flags[idx] | RecordFlag_PendingMove);
    m_pool.lastMoveFrame[idx] = m_frameId;
    m_pendingOps.local().push_back({ .newKey = key, .newRelPos = rel, .newRadius = radius,
                                     .idx = idx, .gen = handle.gen, .type = PendingOp::Move, .newLevel = uint8(level) });
}

void SpatialIndex::setLayerMask(SpatialHandle handle, uint32 layerMask)
{
    if (!m_pool.isValidAlive(handle))
        return;
    const uint32 idx = handle.idx;
    m_pool.layerMask[idx] = layerMask;
    if ((m_pool.flags[idx] & RecordFlag_StaticTier) && m_pool.storeIdx[idx] != UINT32_MAX)
        m_static[m_pool.level[idx]].layer[m_pool.storeIdx[idx]] = layerMask;
}

void SpatialIndex::commitFrame()
{
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
                if (!(opFlags & RecordFlag_NoSpawnGuard))
                    for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
                        m_pool.lastVisible[p][op.idx] = m_visibleQueryId[p];
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
                {
                    if (m_pool.storeIdx[op.idx] == UINT32_MAX)
                        unlinkFromCell(op.idx); // pending promotion: still in its dynamic list
                    // stored entries were tombstoned at unregister and just wait for a rebuild
                }
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
        promotionScan();
    sweepEmptyCells();
    if (m_staticEnabled)
    {
        for (uint32 level = 0; level < m_numLevels; ++level) // at most one level rebuild per frame
        {
            StaticStore& store = m_static[level];
            const uint32 pending = uint32(store.pendingPromotions.size());
            const uint32 size = store.size();
            if ((pending && (pending >= uint32(m_staticRebuildBatch) || pending >= glm::max(1u, size / 8)))
                || store.numTombstones * 4 > size)
            {
                rebuildStaticLevel(level);
                break;
            }
        }
    }

    m_stats.numEntries = int(m_pool.numAlive());
    int totalCells = 0;
    int totalStatic = 0;
    for (uint32 i = 0; i < m_numLevels; ++i)
    {
        m_stats.perLevelCells[i] = int(m_levels[i].size());
        m_stats.perLevelEntities[i] = int(m_levelEntityCount[i]);
        totalCells += int(m_levels[i].size());
        totalStatic += int(m_static[i].size() - m_static[i].numTombstones);
    }
    m_stats.numCells = totalCells;
    m_stats.staticEntries = totalStatic;
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
    const uint8 flags = m_pool.flags[idx];
    if (flags & RecordFlag_StaticTier)
    {
        const uint32 slot = m_pool.storeIdx[idx];
        if (slot != UINT32_MAX)
        {
            StaticStore& store = m_static[m_pool.level[idx]]; // stored: tombstone, not in a dynamic list
            if (store.radius[slot] >= 0.0f)
            {
                store.radius[slot] = -1e30f;
                ++store.numTombstones;
            }
            m_pool.storeIdx[idx] = UINT32_MAX;
        }
        else
            unlinkFromCell(idx); // pending promotion: still linked
        m_pool.flags[idx] = uint8(flags & ~RecordFlag_StaticTier);
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
        constexpr uint8 exclude = RecordFlag_Unlinked | RecordFlag_PendingFree | RecordFlag_PendingMove
                                | RecordFlag_StaticTier | RecordFlag_PendingStatic;
        if ((m_pool.flags[idx] & (RecordFlag_Alive | exclude)) != RecordFlag_Alive)
            continue;
        if (m_frameId - m_pool.lastMoveFrame[idx] < uint32(m_promoteAfterFrames))
            continue;
        // selected: stays linked and queryable until a rebuild folds it into the sorted store
        m_pool.flags[idx] = uint8(m_pool.flags[idx] | RecordFlag_StaticTier | RecordFlag_PendingStatic);
        m_pool.storeIdx[idx] = UINT32_MAX;
        m_static[m_pool.level[idx]].pendingPromotions.push_back({ idx, m_pool.gen[idx] });
    }
}

void SpatialIndex::rebuildStaticLevel(uint32 level)
{
    const auto start = Clock::now();
    StaticStore& store = m_static[level];

    struct Add { uint64 key; uint32 poolIdx; };
    oc::vector<Add> adds;
    adds.reserve(store.pendingPromotions.size());
    for (const StaticStore::Pending& pending : store.pendingPromotions)
    {
        if (pending.idx >= m_pool.capacity() || m_pool.gen[pending.idx] != pending.gen)
            continue; // released (and possibly recycled) since it was selected
        m_pool.flags[pending.idx] = uint8(m_pool.flags[pending.idx] & ~RecordFlag_PendingStatic);
        constexpr uint8 exclude = RecordFlag_PendingFree | RecordFlag_PendingMove;
        if ((m_pool.flags[pending.idx] & (RecordFlag_Alive | RecordFlag_StaticTier | exclude))
            != (RecordFlag_Alive | RecordFlag_StaticTier))
            continue; // moved or died while pending (Move commit already demoted it)
        if (m_pool.storeIdx[pending.idx] != UINT32_MAX || m_pool.level[pending.idx] != level)
            continue;
        unlinkFromCell(pending.idx);
        adds.push_back({ m_pool.cellKey[pending.idx], pending.idx });
    }
    store.pendingPromotions.clear();
    oc::sort(adds.begin(), adds.end(), [](const Add& a, const Add& b) { return a.key < b.key; });

    // merge the (sorted) surviving store with the sorted additions into fresh arrays
    const uint32 oldSize = store.size();
    const uint32 newCapacity = oldSize - store.numTombstones + uint32(adds.size());
    oc::vector<float> posX, posY, posZ, radius;
    oc::vector<uint32> layer, poolIdx;
    oc::vector<uint64> cellKey;
    posX.reserve(newCapacity); posY.reserve(newCapacity); posZ.reserve(newCapacity);
    radius.reserve(newCapacity); layer.reserve(newCapacity); poolIdx.reserve(newCapacity);
    cellKey.reserve(newCapacity);
    uint32 o = 0, a = 0;
    while (o < oldSize || a < uint32(adds.size()))
    {
        if (o < oldSize && (a >= uint32(adds.size()) || store.cellKey[o] <= adds[a].key))
        {
            if (store.radius[o] >= 0.0f) // drop tombstones
            {
                m_pool.storeIdx[store.poolIdx[o]] = uint32(poolIdx.size());
                posX.push_back(store.posX[o]); posY.push_back(store.posY[o]); posZ.push_back(store.posZ[o]);
                radius.push_back(store.radius[o]);
                layer.push_back(store.layer[o]);
                poolIdx.push_back(store.poolIdx[o]);
                cellKey.push_back(store.cellKey[o]);
            }
            ++o;
        }
        else
        {
            const uint32 idx = adds[a].poolIdx;
            m_pool.storeIdx[idx] = uint32(poolIdx.size());
            posX.push_back(m_pool.posX[idx]); posY.push_back(m_pool.posY[idx]); posZ.push_back(m_pool.posZ[idx]);
            radius.push_back(m_pool.radius[idx]);
            layer.push_back(m_pool.layerMask[idx]);
            poolIdx.push_back(idx);
            cellKey.push_back(adds[a].key);
            ++a;
        }
    }

    // retire the old ranges, then write the new ones (with occupancy, since emptied cells may
    // have been swept while their entries were pending)
    m_levels[level].forEachCellMutable([&](uint64 key, CellRecord& rec)
    {
        if (rec.staticCount)
        {
            rec.staticStart = 0;
            rec.staticCount = 0;
            if (rec.dynCount == 0 && rec.childMask == 0)
                m_emptyCandidates.push_back({ key, level }); // rechecked by next frame's sweep
        }
    });
    store.posX.swap(posX); store.posY.swap(posY); store.posZ.swap(posZ);
    store.radius.swap(radius);
    store.layer.swap(layer);
    store.poolIdx.swap(poolIdx);
    store.cellKey.swap(cellKey);
    store.numTombstones = 0;
    const uint32 count = store.size();
    for (uint32 i = 0; i < count;)
    {
        const uint64 key = store.cellKey[i];
        uint32 rangeEnd = i + 1;
        while (rangeEnd < count && store.cellKey[rangeEnd] == key)
            ++rangeEnd;
        CellRecord& rec = m_levels[level].getOrCreate(key);
        rec.staticStart = i;
        rec.staticCount = rangeEnd - i;
        setOccupancyBits(level, key);
        i = rangeEnd;
    }
    ++m_stats.staticRebuilds;
    m_stats.rebuildMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
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
