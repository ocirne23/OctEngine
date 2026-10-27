export module Spatial:BlockStore;

import Core;

// Per-level storage of the linked entries: 8-wide SoA blocks chained per cell (see "Cell blocks" in
// Spatial CONTEXT.md). 256 bytes, cache-line aligned: no 32-byte lane row splits a line.
struct alignas(64) CellBlock
{
    static constexpr uint32 Lanes = 8;
    static constexpr float Tombstone = -1e30f;

    float posX[Lanes], posY[Lanes], posZ[Lanes]; // cell-relative, like the pool
    float radius[Lanes];                          // negative = tombstone or unused lane
    uint32 layer[Lanes];
    uint32 poolIdx[Lanes];                        // owning RecordPool slot (visibility stamps, userData)
    uint32 next;                                  // next block in the cell's chain, UINT32_MAX = end
    uint32 count;                                 // lanes handed out (owned + retired)
    uint32 live;                                  // lanes the pool still owns
    uint32 chainBlocks;                           // head block only: blocks in the cell's chain
};
static_assert(sizeof(CellBlock) == 256);

struct BlockStore
{
    oc::vector<CellBlock> blocks; // 64-aligned: the engine allocator serves over-aligned EASTL requests
    uint32 freeHead = UINT32_MAX; // freed blocks, chained through `next`
    uint32 numBlocksInUse = 0;

    static uint32 slotOf(uint32 block, uint32 lane) { return block * CellBlock::Lanes + lane; }
    static uint32 blockOf(uint32 slot) { return slot / CellBlock::Lanes; }
    static uint32 laneOf(uint32 slot) { return slot % CellBlock::Lanes; }

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
    uint32 insert(uint32& head, float x, float y, float z, float r, uint32 layerMask, uint32 poolIdx)
    {
        if (head == UINT32_MAX || blocks[head].count == CellBlock::Lanes)
        {
            const uint32 chainBlocks = head == UINT32_MAX ? 0 : blocks[head].chainBlocks; // before allocBlock may grow the vector
            head = allocBlock(head);
            blocks[head].chainBlocks = chainBlocks + 1;
        }
        CellBlock& block = blocks[head];
        const uint32 lane = block.count++;
        block.posX[lane] = x; block.posY[lane] = y; block.posZ[lane] = z;
        block.radius[lane] = r;
        block.layer[lane] = layerMask;
        block.poolIdx[lane] = poolIdx;
        ++block.live;
        return slotOf(head, lane);
    }
};
