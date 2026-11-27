module;

#include <immintrin.h>

export module Spatial:BlockStore;

import Core;
import :Morton;

// Per-level storage of the indexed entries: 8-wide SoA blocks chained per cell (see "Cell blocks" in Spatial CONTEXT.md).

// ---- The lane encoding ----
// Fixed point per level L: a position is uint16 quanta of cellSize / 65536 from the cell's min corner (it decodes to the quantum's
// centre), a radius int16 quanta of twice that, rounded up. Below the top level an entry's radius is <= cellSize / 2 (the level
// rule) = 16384 radius quanta; the top level saturates at LaneRadiusMax, and its tests take the clamped-oversize radius instead.
// With 8 m fine cells the position quantum is 2^(2L - 13) m: 0.12 mm at level 0, 128 m at level 10.

constexpr int16 LaneRadiusMax = 32767;

struct LaneScale
{
    double posScale;     // metres -> position quanta
    float posQuantum;    // position quanta -> metres
    float radiusScale;   // metres -> radius quanta
    float radiusQuantum; // radius quanta -> metres
};
struct LaneScales
{
    LaneScale level[Morton::MaxLevels];
};
constexpr LaneScales makeLaneScales()
{
    LaneScales s{};
    for (uint32 l = 0; l < Morton::MaxLevels; ++l)
    {
        const double quantum = Morton::cellSize(l) / 65536.0;
        s.level[l] = { 1.0 / quantum, float(quantum), float(1.0 / (2.0 * quantum)), float(2.0 * quantum) };
    }
    return s;
}
inline constexpr LaneScales g_laneScales = makeLaneScales();

// One lane's encoded position and radius: what a pending Index / Move op carries.
struct LaneCode
{
    uint16 x, y, z;
    int16 radius;
};
static_assert(sizeof(LaneCode) == 8);

// 192 bytes, three cache lines: the geometry of all 8 lanes (what test8 reads) | poolIdx + layer + the header (the chain walk, the
// layer test, the stamp) | userData (a query hit).
struct alignas(64) CellBlock
{
    static constexpr uint32 Lanes = 8;
    static constexpr int16 Tombstone = -1;

    uint16 posX[Lanes], posY[Lanes], posZ[Lanes]; // the lane encoding, cell-relative; an indexed entry's only copy
    int16 radius[Lanes];                          // the lane encoding; negative = tombstone or unused lane
    uint32 poolIdx[Lanes];                        // owning RecordPool slot (visibility stamps, gen)
    uint8 layer[Lanes];                           // SpatialLayer_* bits (all below 256, see SpatialTypes.ixx)
    uint32 next;                                  // next block in the cell's chain, UINT32_MAX = end
    uint32 count;                                 // lanes handed out (owned + retired)
    uint32 live;                                  // lanes the pool still owns
    uint32 chainBlocks;                           // head block only: blocks in the cell's chain
    alignas(64) uint64 userData[Lanes];           // a copy of the pool row (it never changes): a query hit reads no pool row
};
static_assert(sizeof(CellBlock) == 192);

struct BlockStore
{
    oc::vector<CellBlock> blocks; // 64-aligned: the engine allocator serves over-aligned EASTL requests
    uint32 freeHead = UINT32_MAX; // freed blocks, chained through `next`
    uint32 numBlocksInUse = 0;

    static uint32 slotOf(uint32 block, uint32 lane) { return block * CellBlock::Lanes + lane; }
    static uint32 blockOf(uint32 slot) { return slot / CellBlock::Lanes; }
    static uint32 laneOf(uint32 slot) { return slot % CellBlock::Lanes; }

    // A block's first two lines into L1: what a test reads (the geometry, the layer + the chain's next).
    void prefetch(uint32 b) const
    {
        const char* p = reinterpret_cast<const char*>(&blocks[b]);
        _mm_prefetch(p, _MM_HINT_T0);
        _mm_prefetch(p + 64, _MM_HINT_T0);
    }

    uint32 allocBlock(uint32 next)
    {
        uint32 b;
        if (freeHead != UINT32_MAX)
        {
            b = freeHead;
            freeHead = blocks[b].next;
        }
        else
        {
            b = uint32(blocks.size());
            blocks.emplace_back();
        }
        CellBlock& block = blocks[b];
        for (uint32 lane = 0; lane < CellBlock::Lanes; ++lane)
        {
            block.radius[lane] = CellBlock::Tombstone; // unused lanes fail every test8
            block.layer[lane] = 0;
        }
        block.next = next;
        block.count = 0;
        block.live = 0;
        ++numBlocksInUse;
        return b;
    }

    void freeBlock(uint32 b)
    {
        blocks[b].next = freeHead;
        freeHead = b;
        --numBlocksInUse;
    }

    // Appends into the chain at `head` (a new head block when the head is full or absent) and
    // returns the slot; `head` is updated in place.
    uint32 insert(uint32& head, const LaneCode& code, uint32 layerMask, uint64 userData, uint32 poolIdx)
    {
        if (head == UINT32_MAX || blocks[head].count == CellBlock::Lanes)
        {
            const uint32 chainBlocks = head == UINT32_MAX ? 0 : blocks[head].chainBlocks; // before allocBlock may grow the vector
            head = allocBlock(head);
            blocks[head].chainBlocks = chainBlocks + 1;
        }
        CellBlock& block = blocks[head];
        const uint32 lane = block.count++;
        block.posX[lane] = code.x; block.posY[lane] = code.y; block.posZ[lane] = code.z;
        block.radius[lane] = code.radius;
        block.userData[lane] = userData;
        block.layer[lane] = uint8(layerMask);
        block.poolIdx[lane] = poolIdx;
        ++block.live;
        return slotOf(head, lane);
    }
};
