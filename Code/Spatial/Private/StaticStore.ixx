export module Spatial:StaticStore;

import Core;

// Module-internal per-level storage for settled entries: 8-wide SoA BLOCKS chained per cell
// (CellRecord.staticHead), so the 8-wide SIMD testers get transpose-free loads straight from a
// block and every mutation is local to one cell. A promotion appends into the cell's head block
// (a new head when it is full), a demotion tombstones its lane in place (negative radius - fails
// every test, including the unused lanes past `count`), a block whose live count reaches zero is
// freed at once, and a cell whose dead lanes would fill a whole block compacts itself. There is
// no level-wide rebuild: the cost of a change is the size of its cell, never the level.
struct StaticBlock
{
    static constexpr uint32 Lanes = 8;
    static constexpr float Tombstone = -1e30f;

    float posX[Lanes], posY[Lanes], posZ[Lanes]; // cell-relative, like the pool
    float radius[Lanes];                          // negative = tombstone or unused lane
    uint32 layer[Lanes];
    uint32 poolIdx[Lanes];                        // owning RecordPool slot (visibility stamps, userData)
    uint32 next;                                  // next block in the cell's chain, UINT32_MAX = end
    uint32 count;                                 // lanes handed out (owned + retired)
    uint32 live;                                  // lanes the pool still OWNS (retireStatic settles them; a
                                                  // tombstone from unregisterEntry alone keeps its lane owned)
};

struct StaticStore
{
    oc::vector<StaticBlock> blocks;
    uint32 freeHead = UINT32_MAX; // freed blocks, chained through `next`
    uint32 numBlocksInUse = 0;
    uint32 numLive = 0;

    static uint32 slotOf(uint32 block, uint32 lane) { return block * StaticBlock::Lanes + lane; }
    static uint32 blockOf(uint32 slot) { return slot / StaticBlock::Lanes; }
    static uint32 laneOf(uint32 slot) { return slot % StaticBlock::Lanes; }

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
        StaticBlock& block = blocks[b];
        for (uint32 lane = 0; lane < StaticBlock::Lanes; ++lane)
        {
            block.radius[lane] = StaticBlock::Tombstone; // unused lanes fail every test8
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
        if (head == UINT32_MAX || blocks[head].count == StaticBlock::Lanes)
            head = allocBlock(head);
        StaticBlock& block = blocks[head];
        const uint32 lane = block.count++;
        block.posX[lane] = x; block.posY[lane] = y; block.posZ[lane] = z;
        block.radius[lane] = r;
        block.layer[lane] = layerMask;
        block.poolIdx[lane] = poolIdx;
        ++block.live;
        ++numLive;
        return slotOf(head, lane);
    }
};
