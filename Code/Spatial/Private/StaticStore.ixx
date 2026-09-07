export module Spatial:StaticStore;

import Core;

// Module-internal per-level storage for settled entries: SoA arrays sorted by cell Morton key
// into contiguous per-cell ranges (CellRecord.staticStart/staticCount), so queries iterate them
// linearly and the 8-wide SIMD testers get transpose-free loads. Entries promote in after not
// changing for N frames (they stay in the dynamic lists until a rebuild consumes them) and
// demote by tombstoning (negative radius - fails every test in place); budgeted per-level
// rebuilds drop tombstones and merge pending promotions back into sorted order.
struct StaticStore
{
    struct Pending
    {
        uint32 idx; // RecordPool slot flagged StaticTier, still linked in its dynamic list
        uint32 gen;
    };

    oc::vector<float> posX, posY, posZ; // cell-relative, like the pool
    oc::vector<float> radius;           // negative = tombstone, dropped on rebuild
    oc::vector<uint32> layer;
    oc::vector<uint32> poolIdx;         // owning RecordPool slot (visibility stamps, userData)
    oc::vector<uint64> cellKey;         // key at this level; kept sorted, drives rebuild merges
    oc::vector<Pending> pendingPromotions;
    uint32 numTombstones = 0;

    // The rebuild's target arrays, DOUBLE-BUFFERED with the live ones: rebuildStaticLevel merges
    // into these (capacity kept from the last rebuild), then swaps them with the live set, so a
    // rebuild allocates nothing once both sets have reached the level's peak size.
    struct Build
    {
        oc::vector<float> posX, posY, posZ, radius;
        oc::vector<uint32> layer, poolIdx;
        oc::vector<uint64> cellKey;
        void clearAndReserve(uint32 n)
        {
            posX.clear(); posY.clear(); posZ.clear(); radius.clear(); layer.clear(); poolIdx.clear(); cellKey.clear();
            posX.reserve(n); posY.reserve(n); posZ.reserve(n); radius.reserve(n); layer.reserve(n); poolIdx.reserve(n); cellKey.reserve(n);
        }
    };
    Build build;

    uint32 size() const { return uint32(poolIdx.size()); }
};
