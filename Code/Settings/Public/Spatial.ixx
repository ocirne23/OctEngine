export module Settings.Spatial;

import Core;

// "Spatial/...": the SpatialIndex (culling + stats), the CPU occlusion buffer and the stress harness.
// Spatial re-exports this module, so its importers see these types.

export enum class ESpatialCullMode : int
{
    Off = 0,   // no queries, every entity is pushed for all render passes
    StatsOnly, // the queries run for their stats, nothing is gated
    Cull,      // main set renders fully; near-only entities are pushed for shadows/ray tracing only
    MainOnly,  // debug: only the main-frustum set is pushed at all (visibly breaks off-screen shadows/GI)
};

// Live-tweakable culling settings (Spatial/Culling): the App drives the markVisible* queries from
// these and Entity::updateTree pushes per-pass masks when mode >= Cull.
export struct SpatialCullingConfig
{
    int mode = int(ESpatialCullMode::Cull);
    bool freeze = false;             // stop re-stamping, fly around to inspect the culled set
    float margin = 4.0f;             // frustum inflation masking the one-frame stamp latency
    float nearRadius = 0.0f;       // shadow-caster + ray-tracing relevance range around the camera
    float shadowReach = 60.0f;     // Shadow pass: how far (m) the view frustum is swept toward the sun on the
                                   // horizontal plane - off-screen casters up to this far up-sun of the
                                   // visible ground keep their shadow pass. 0 = off (Main + Near only).
    float nearSlack = 16.0f;         // Near ball inflation; requery only after the camera moves this far (0 = every frame)
    float maxDist = 5000.0f; // main-pass cull distance; NOT a tweak: overwritten each frame with the camera far plane (setCullMaxDist)
    float skinnedRadiusScale = 1.5f; // animation can exceed the bind-pose bounds sphere
};

// Array bounds of the stats: Morton::MaxLevels and ESpatialPass::Count (static_asserted in Spatial).
export constexpr uint32 SpatialStatsMaxLevels = 11;
export constexpr uint32 SpatialStatsMaxPasses = 8;

// Live readouts (Spatial/Stats), written by the index.
export struct SpatialStats
{
    int numEntries = 0;
    int numCells = 0;
    int cellsTested = 0;      // per-frame query counters, reset each commitFrame
    int cellsFullyInside = 0;
    int entityTests = 0;
    int visiblePerPass[SpatialStatsMaxPasses] = {};
    int numBlocks = 0;  // 8-lane cell blocks in use across all levels (numEntries / numBlocks = lane fill)
    int cellMoves = 0;  // entries that changed cell in the last commit (Move ops applied)
    float commitMs = 0.0f;
    float markVisibleMs = 0.0f;
    int perLevelCells[SpatialStatsMaxLevels] = {};
    int perLevelEntities[SpatialStatsMaxLevels] = {};
};

// Spatial/Occlusion: the CPU software occlusion (default off) plus its readouts.
export struct SpatialOcclusionSettings
{
    bool enabled = false;
    float depthBias = 0.001f;
    int maxTriangles = 2048;
    int statTriangles = 0;   // readouts, written by the occlusion buffer
    int statHiddenCells = 0;
    float statRasterMs = 0.0f;
};

// Spatial/Stress: the SpatialStressTest harness - controls plus its readouts.
export struct SpatialStressSettings
{
    int count = 100000;
    float extent = 2000.0f;
    bool spawnRequested = false;
    bool clearRequested = false;
    float churnPercent = 0.0f;
    bool runSphereQuery = false;
    float queryRadius = 100.0f;
    int queryHits = 0;
    float queryMs = 0.0f;
    bool verifyBruteForce = false;
    int verifyDelta = 0;
    bool runFrustumQuery = false;
    float frustumMaxDist = 2000.0f;
    int frustumHits = 0;
    float frustumMs = 0.0f;
};

export struct SpatialSettings
{
    SpatialStats stats;
    SpatialCullingConfig culling;
    SpatialOcclusionSettings occlusion;
    SpatialStressSettings stress;
};

export namespace Settings
{
    void registerSpatial(SpatialSettings& s);
}
