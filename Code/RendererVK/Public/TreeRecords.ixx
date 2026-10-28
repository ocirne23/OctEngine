export module RendererVK:TreeRecords;

import Core;
import Core.glm;

// The WORLD TREE RECORDS' API types (Renderer::setTreeRecordTypes / addTreeRecordChunk / treeRecordStats; Procedural TreeSystem + TreeWorld). The GPU pool
// itself is TreeRecordPool (Private/Data).

// One record TYPE (a species, by the record's type) as the far volume sees it (MIRRORED in tree_volume_splat.cs.glsl,
// tree_volume_records.cs.glsl, tree_volume_far.cs.glsl): how a record EXPANDS - exactly as Procedural TreeSystem's
// expandChunk (variant, scale, yaw from the record's seed; the tree's bushes) into the world set's volume types - and,
// for the columns far out, the species' extinction integrated over the ground per height: P(h) = mass x shape(h), shape
// over [0, height] in TREE_RECORD_PROFILE_BINS bins integrating to 1, at its mean size. mass 0 = not in the volume.
// albedo.w = 0: a ROCK type (Procedural's world rocks) - a SOLID: its variants' grids are occupancy, the volume gives it
// "Far rock extinction" at any size (variantMass = the occupied volume at scale 1; its mass grows with scale^3), and its
// colour is the climate's bedrock (the resolve), not albedo.rgb. No bushes, no size variation.
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

// A chunk's GROUND (the far volume's: the records carry no height - a tree stands on it): TREE_RECORD_GROUND_RES^2
// heights over the chunk, corners included (16 m at 256 m chunks), stored in the pool right BEFORE the chunk's records:
// word 0 = the minimum (float bits), word 1 = the step per unit (float bits), then 16-bit units, two per word (low half
// first). MIRRORED in tree_record.inc.glsl (treeRecordGround). Procedural TreeWorld fills it from the generator's own
// field: the camera-centred height map's far cascade averages ~100 m texels, which buried the trees on every peak.
export constexpr uint32 TREE_RECORD_GROUND_RES = 17;
export constexpr uint32 TREE_RECORD_GROUND_WORDS = 2 + (TREE_RECORD_GROUND_RES * TREE_RECORD_GROUND_RES + 1) / 2;

export struct TreeRecordStats
{
    uint32 chunks = 0;
    uint64 records = 0;
    uint64 usedBytes = 0;    // in blocks, incl. the round-up
    uint64 poolBytes = 0;
    uint64 pendingBytes = 0; // removed, not reusable yet
    uint32 refused = 0;      // add() calls the pool had no room for (since the last reset)
};
