export module RendererVK:TreeRecordPool;

import Core;
import Core.glm;

import :Buffer;
import :Layout;
import :MeshLodRegistry; // IndexRangeFreeList

// WORLD TREE RECORDS on the GPU (Procedural TreeWorld; Docs/TreeRenderingPlan.md 3.5, W2): every terrain chunk's 4-byte
// tree records in ONE fixed-size device-local pool, plus a TABLE of the resident chunks per frame slot. The far-tree
// volume's bake reads them (W4: EVERY tree of the volume comes from the records - tree_volume_splat.cs in detail near
// the camera, tree_volume_records.cs / tree_volume_far.cs as mass per column beyond).
//
// * The pool never grows (a growth would copy or drain): its size is a setting, and a chunk that does not fit is
//   refused. reset() with a new size drains the GPU - a user action, never a streaming one.
// * A chunk's range is written ONCE through the staging ring, into blocks no frame reads: a removed chunk's blocks are
//   reused only NUM_FRAMES_IN_FLIGHT frames after its removal - by then every frame whose table still listed it is done.
// * The table: host-visible per frame slot, rewritten for the CURRENT slot (after beginFrame's fence wait) when it
//   changed since that slot was last written.
//
// Main thread only.
export struct TreeRecordChunkGpu // MIRRORED in tree_volume_splat.cs.glsl / tree_volume_records.cs.glsl
{
    glm::ivec2 coord{ 0 };
    uint32 first = 0;  // the first record in the pool
    uint32 count = 0;
};
static_assert(sizeof(TreeRecordChunkGpu) == 16);

// One record TYPE (a species, by the record's type) as the far volume sees it (MIRRORED in tree_volume_splat.cs.glsl,
// tree_volume_records.cs.glsl, tree_volume_far.cs.glsl): how a record EXPANDS - exactly as Procedural TreeSystem's
// expandChunk (variant, scale, yaw from the record's seed; the tree's bushes) into the world set's volume types - and,
// for the columns far out, the species' extinction integrated over the ground per height: P(h) = mass x shape(h), shape
// over [0, height] in TREE_RECORD_PROFILE_BINS bins integrating to 1, at its mean size. mass 0 = not in the volume.
export constexpr uint32 TREE_RECORD_PROFILE_BINS = 32;
export constexpr uint32 TREE_RECORD_MAX_VARIANTS = 8;
export constexpr uint32 TREE_RECORD_MAX_BUSHES = 8;
export struct TreeRecordTypeGpu
{
    glm::vec4 albedo{ 0.0f };
    float mass = 0.0f;          // m^2: the extinction's integral over the tree, at the mean size (the profile's)
    float height = 0.0f;        // m: the profile's top
    float radius = 0.0f;        // m: the crown's horizontal radius at scale 1 (the mass splat's footprint)
    float sizeVariation = 0.0f; // the scale: mix(scale.x, scale.y, u) x 2^(+-this)
    glm::vec2 scale{ 1.0f };
    uint32 numVariants = 0;
    uint32 numBushes = 0;       // trees: their bush types
    float bushesPerTree = 0.0f;
    float bushRadius = 1.5f;
    float pad0 = 0.0f, pad1 = 0.0f;
    uint32 variantType[TREE_RECORD_MAX_VARIANTS] = {}; // the world set's volume type of each variant
    float variantMass[TREE_RECORD_MAX_VARIANTS] = {};  // m^2 at scale 1
    uint32 bushTypes[TREE_RECORD_MAX_BUSHES] = {};     // RECORD types
    float shape[TREE_RECORD_PROFILE_BINS] = {};        // 1/m
};
static_assert(sizeof(TreeRecordTypeGpu) == 64 + TREE_RECORD_MAX_VARIANTS * 8 + TREE_RECORD_MAX_BUSHES * 4 + TREE_RECORD_PROFILE_BINS * 4);

// A chunk's GROUND (the far volume's: the records carry no height - a tree stands on it): TREE_RECORD_HEIGHT_RES^2
// heights over the chunk, corners included (16 m at 256 m chunks), stored in the pool right BEFORE the chunk's records:
// word 0 = the minimum (float bits), word 1 = the step per unit (float bits), then 16-bit units, two per word (low half
// first). MIRRORED in tree_record.inc.glsl (treeRecordGround). Procedural TreeWorld fills it from the generator's own
// field: the camera-centred height map's far cascade averages ~100 m texels, which buried the trees on every peak.
export constexpr uint32 TREE_RECORD_HEIGHT_RES = 17;
export constexpr uint32 TREE_RECORD_HEIGHT_WORDS = 2 + (TREE_RECORD_HEIGHT_RES * TREE_RECORD_HEIGHT_RES + 1) / 2;

// The CHUNK MAP: a toroidal grid of mapSize^2 entries (a power of two over the ring's diameter), indexed by the chunk
// coordinate & (mapSize - 1): the coordinate (to check) and where its ground lies in the pool. A position's chunk ->
// its ground, whichever chunk's tree stands there (a bush past its chunk's edge, a column next to the trees).
export struct TreeRecordMapGpu // MIRRORED in tree_record.inc.glsl
{
    glm::ivec2 coord{ INT32_MIN };
    uint32 ground = 0; // the pool word of its ground
};
static_assert(sizeof(TreeRecordMapGpu) == 12);

export struct TreeRecordStats
{
    uint32 chunks = 0;
    uint64 records = 0;
    uint64 usedBytes = 0;    // in blocks, incl. the round-up
    uint64 poolBytes = 0;
    uint64 pendingBytes = 0; // removed, not reusable yet
    uint32 refused = 0;      // add() calls the pool had no room for (since the last reset)
};

export class TreeRecordPool final
{
public:
    static constexpr uint32 BLOCK_RECORDS = 64; // the allocation unit (256 bytes)

    // (Re)creates the pool at `bytes` - every chunk is dropped. A size change drains the GPU (the old pool may be read).
    // ringRadius (chunks) sizes the chunk map.
    void reset(uint64 bytes, uint64 frame, uint32 ringRadius);
    // Uploads a chunk's ground (TREE_RECORD_HEIGHT_WORDS) and its records (any number, 0 too: its ground still serves
    // its neighbours' trees); the handle, or UINT32_MAX when the pool has no room.
    uint32 add(glm::ivec2 coord, oc::span<const uint32> ground, oc::span<const uint32> records);
    void remove(uint32 handle, uint64 frame);
    // The record types' far view (TreeSystem, at a world spawn; empty = none). A change drains the GPU (rare).
    void setTypes(oc::span<const TreeRecordTypeGpu> types);
    Buffer& types() { return m_types; }
    uint32 numTypes() const { return m_numTypes; }
    // Once per frame after beginFrame: the deferred frees whose frames are done, and the current slot's table.
    // holdSince: a far-volume bake that started on that frame is still running over several frames (UINT64_MAX: none) -
    // it works from a snapshot of the table, so a chunk removed since then keeps its blocks until NUM_FRAMES_IN_FLIGHT
    // frames after the bake ended.
    void update(uint32 frameSlot, uint64 frame, uint64 holdSince);
    TreeRecordStats stats() const;

    Buffer& records() { return m_records; }
    Buffer& table(uint32 frameSlot) { return m_tables[frameSlot]; }
    uint32 tableCount(uint32 frameSlot) const { return m_tableCounts[frameSlot]; }
    // The CPU copy of what the slot's table holds (the far volume picks its detail chunks from it).
    oc::span<const TreeRecordChunkGpu> tableCpu(uint32 frameSlot) const { return m_tableCpu[frameSlot]; }
    Buffer& map(uint32 frameSlot) { return m_maps[frameSlot]; }
    oc::span<const TreeRecordMapGpu> mapCpu() const { return m_mapMirror; } // the chunk map as of now
    uint32 mapSize() const { return m_mapSize; }

private:
    struct Chunk
    {
        TreeRecordChunkGpu gpu;   // first = its records (its ground lies TREE_RECORD_HEIGHT_WORDS before)
        uint32 firstBlock = 0;
        uint32 blocks = 0;
        uint32 dense = UINT32_MAX; // its index in m_dense (UINT32_MAX = a free handle)
    };
    struct PendingFree
    {
        uint32 firstBlock = 0;
        uint32 blocks = 0;
        uint64 readyFrame = 0;
        uint64 removedFrame = 0;
    };
    uint64 m_holdSince = UINT64_MAX; // the running bake's start (update's holdSince)

    Buffer m_records;
    uint32 m_numBlocks = 0;
    uint32 m_topBlock = 0;              // blocks below it were handed out at least once
    IndexRangeFreeList m_freeBlocks;    // below m_topBlock
    oc::vector<PendingFree> m_pending;
    oc::vector<Chunk> m_chunks;         // by handle
    oc::vector<uint32> m_freeHandles;
    oc::vector<uint32> m_dense;         // the live handles, packed (the table's order)
    uint64 m_numRecords = 0;
    uint64 m_usedBlocks = 0;
    uint32 m_refused = 0;

    Buffer m_types;
    uint32 m_numTypes = 0;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tables;
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tableCounts{};
    oc::array<bool, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tableDirty{};
    oc::array<oc::vector<TreeRecordChunkGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tableCpu;
    // The chunk map: its CPU mirror, written whole into the current slot's buffer with the table.
    uint32 m_mapSize = 0;
    oc::vector<TreeRecordMapGpu> m_mapMirror;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_maps;
    uint32 mapIndex(glm::ivec2 coord) const { return ((uint32)coord.x & (m_mapSize - 1)) + ((uint32)coord.y & (m_mapSize - 1)) * m_mapSize; }
};
