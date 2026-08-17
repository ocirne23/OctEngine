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

    uint32 size() const { return uint32(poolIdx.size()); }
};
