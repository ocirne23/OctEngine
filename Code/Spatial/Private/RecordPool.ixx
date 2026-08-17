export module Spatial:RecordPool;

import Core;
import Core.BitRangeAllocator;
import :Types;

constexpr uint32 NumSpatialPasses = uint32(ESpatialPass::Count);

// Module-internal SoA storage for every registered entry. Slots come from a BitRangeAllocator so
// indices stay low and dense; positions are stored relative to the owning cell's min corner
// (bounded by the cell size, so float keeps full precision at planet-scale coordinates) split
// into per-axis arrays for future 8-wide SIMD tests. Nothing here is exported.

enum ERecordFlag : uint8
{
    RecordFlag_Alive       = 1,
    RecordFlag_Unlinked    = 2, // registered but not yet linked into its cell (pre-commit)
    RecordFlag_PendingFree   = 4,  // unregistered, unlink + slot release happen at commit
    RecordFlag_PendingMove   = 8,  // a queued Move op holds newer data than the in-place SoA fields
    RecordFlag_StaticTier    = 16, // selected for / stored in the static tier (storeIdx = which)
    RecordFlag_PendingStatic = 32, // sits in a pendingPromotions list; blocks re-promotion until a
                                   // rebuild consumes that entry (a demote in between would otherwise
                                   // allow a duplicate, and the rebuild would unlink the entry twice)
    RecordFlag_NoSpawnGuard  = 64, // registerEntry(spawnVisible = false): the entry is NEVER treated as
                                   // visible before its first real stamp — streamed terrain wants this
                                   // (chunks materialize off-screen constantly; the guard pinned them in
                                   // the main pass until first entering the frustum, forever with Freeze)
};

class RecordPool final
{
public:

    void initialize(uint32 capacity)
    {
        m_capacity = (capacity + 63) & ~63u;
        m_slots.resize(m_capacity);
        resizeArrays();
    }

    uint32 acquire()
    {
        int idx = m_slots.acquireOne();
        while (idx < 0)
        {
            m_capacity *= 2;
            m_slots.resize(m_capacity);
            resizeArrays();
            idx = m_slots.acquireOne();
        }
        ++m_numAlive;
        return uint32(idx);
    }

    void release(uint32 idx)
    {
        ++gen[idx];
        flags[idx] = 0;
        m_slots.releaseOne(int(idx));
        --m_numAlive;
    }

    bool isValidAlive(SpatialHandle handle) const
    {
        return handle.idx < m_capacity && gen[handle.idx] == handle.gen && (flags[handle.idx] & RecordFlag_Alive);
    }

    uint32 capacity() const { return m_capacity; }
    uint32 numAlive() const { return m_numAlive; }

    oc::vector<float> posX, posY, posZ; // relative to the owning cell's min corner
    oc::vector<float> radius;           // negative = neutralized (pending free), fails every test
    oc::vector<uint64> cellKey;         // current cell Morton key at `level`
    oc::vector<uint64> userData;
    oc::vector<uint32> next, prev;      // intrusive per-cell doubly-linked list
    oc::vector<uint32> layerMask;
    oc::vector<uint32> gen;
    oc::array<oc::vector<uint32>, NumSpatialPasses> lastVisible; // stamp generation per pass
    oc::vector<uint32> lastMoveFrame;
    oc::vector<uint32> storeIdx; // StaticTier: slot in the level's StaticStore, UINT32_MAX = pending
    oc::vector<uint8> level;
    oc::vector<uint8> flags;

private:

    void resizeArrays()
    {
        posX.resize(m_capacity); posY.resize(m_capacity); posZ.resize(m_capacity);
        radius.resize(m_capacity);
        cellKey.resize(m_capacity);
        userData.resize(m_capacity);
        next.resize(m_capacity); prev.resize(m_capacity);
        layerMask.resize(m_capacity);
        gen.resize(m_capacity);
        for (oc::vector<uint32>& pass : lastVisible)
            pass.resize(m_capacity);
        lastMoveFrame.resize(m_capacity);
        storeIdx.resize(m_capacity);
        level.resize(m_capacity);
        flags.resize(m_capacity);
    }

    BitRangeAllocator<false> m_slots{ 64 };
    uint32 m_capacity = 0;
    uint32 m_numAlive = 0;
};
