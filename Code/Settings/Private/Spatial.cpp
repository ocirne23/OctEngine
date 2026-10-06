module Settings.Spatial;

import Core;
import Settings.Tweaks;

static constexpr oc::string_view levelCellNames[SpatialStatsMaxLevels] = {
    "L0 cells", "L1 cells", "L2 cells", "L3 cells", "L4 cells", "L5 cells",
    "L6 cells", "L7 cells", "L8 cells", "L9 cells", "L10 cells",
};
static constexpr oc::string_view levelEntryNames[SpatialStatsMaxLevels] = {
    "L0 entries", "L1 entries", "L2 entries", "L3 entries", "L4 entries", "L5 entries",
    "L6 entries", "L7 entries", "L8 entries", "L9 entries", "L10 entries",
};

void Settings::registerSpatial(SpatialSettings& s)
{
    SpatialStats& stats = s.stats;
    Tweak::intVar("Spatial/Stats", "Entries", &stats.numEntries, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells", &stats.numCells, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells tested", &stats.cellsTested, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cells fully inside", &stats.cellsFullyInside, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Entity tests", &stats.entityTests, 0, INT32_MAX);
    // ESpatialPass::Main / Near / Shadow
    Tweak::intVar("Spatial/Stats", "Visible main", &stats.visiblePerPass[0], 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Visible near", &stats.visiblePerPass[1], 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Visible shadow", &stats.visiblePerPass[2], 0, INT32_MAX);
    Tweak::floatVar("Spatial/Stats", "Commit ms", &stats.commitMs, 0.0f, FLT_MAX, 0.001f);
    Tweak::floatVar("Spatial/Stats", "Mark visible ms", &stats.markVisibleMs, 0.0f, FLT_MAX, 0.001f);
    for (uint32 i = 0; i < SpatialStatsMaxLevels; ++i)
    {
        Tweak::intVar("Spatial/Stats/Levels", levelCellNames[i], &stats.perLevelCells[i], 0, INT32_MAX);
        Tweak::intVar("Spatial/Stats/Levels", levelEntryNames[i], &stats.perLevelEntities[i], 0, INT32_MAX);
    }

    Tweak::intVar("Spatial/Stats", "Blocks", &stats.numBlocks, 0, INT32_MAX);
    Tweak::intVar("Spatial/Stats", "Cell moves", &stats.cellMoves, 0, INT32_MAX);

    SpatialCullingConfig& culling = s.culling;
    static constexpr oc::string_view cullModeNames[] = { "Off", "Stats only", "Cull", "Main only (debug)" };
    Tweak::enumVar("Spatial/Culling", "Mode", &culling.mode, cullModeNames);
    Tweak::boolean("Spatial/Culling", "Freeze", &culling.freeze);
    Tweak::floatVar("Spatial/Culling", "Margin", &culling.margin, 0.0f, 64.0f, 0.1f);
    Tweak::floatVar("Spatial/Culling", "Near radius", &culling.nearRadius, 0.0f, 4096.0f, 1.0f);
    Tweak::floatVar("Spatial/Culling", "Shadow reach (m)", &culling.shadowReach, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Spatial/Culling", "Near requery slack", &culling.nearSlack, 0.0f, 128.0f, 0.5f);
    // Max dist is not tweaked: it's driven every frame from the render camera's far plane (setCullMaxDist).
    Tweak::floatVar("Spatial/Culling", "Skinned radius scale", &culling.skinnedRadiusScale, 1.0f, 4.0f, 0.01f);

    SpatialOcclusionSettings& occlusion = s.occlusion;
    Tweak::boolean("Spatial/Occlusion", "Enabled", &occlusion.enabled);
    Tweak::floatVar("Spatial/Occlusion", "Depth bias", &occlusion.depthBias, 0.0f, 0.05f, 0.0001f);
    Tweak::intVar("Spatial/Occlusion", "Max occluder tris", &occlusion.maxTriangles, 64, 16384);
    Tweak::intVar("Spatial/Occlusion", "Triangles", &occlusion.statTriangles, 0, INT32_MAX);
    Tweak::intVar("Spatial/Occlusion", "Hidden cells", &occlusion.statHiddenCells, 0, INT32_MAX);
    Tweak::floatVar("Spatial/Occlusion", "Raster ms", &occlusion.statRasterMs, 0.0f, FLT_MAX, 0.001f);

    SpatialStressSettings& stress = s.stress;
    Tweak::intVar("Spatial/Stress", "Count", &stress.count, 0, 10'000'000);
    Tweak::floatVar("Spatial/Stress", "Extent", &stress.extent, 1.0f, 1'000'000.0f, 10.0f);
    Tweak::boolean("Spatial/Stress", "Spawn", &stress.spawnRequested);
    Tweak::boolean("Spatial/Stress", "Clear", &stress.clearRequested);
    Tweak::floatVar("Spatial/Stress", "Churn %", &stress.churnPercent, 0.0f, 100.0f, 0.1f);
    Tweak::boolean("Spatial/Stress", "Sphere query", &stress.runSphereQuery);
    Tweak::floatVar("Spatial/Stress", "Query radius", &stress.queryRadius, 1.0f, 100'000.0f, 1.0f);
    Tweak::intVar("Spatial/Stress", "Query hits", &stress.queryHits, 0, INT32_MAX);
    Tweak::floatVar("Spatial/Stress", "Query ms", &stress.queryMs, 0.0f, FLT_MAX, 0.001f);
    Tweak::boolean("Spatial/Stress", "Verify brute force", &stress.verifyBruteForce);
    Tweak::intVar("Spatial/Stress", "Verify delta", &stress.verifyDelta, 0, INT32_MAX);
    Tweak::boolean("Spatial/Stress", "Frustum query", &stress.runFrustumQuery);
    Tweak::floatVar("Spatial/Stress", "Frustum max dist", &stress.frustumMaxDist, 1.0f, 1'000'000.0f, 10.0f);
    Tweak::intVar("Spatial/Stress", "Frustum hits", &stress.frustumHits, 0, INT32_MAX);
    Tweak::floatVar("Spatial/Stress", "Frustum ms", &stress.frustumMs, 0.0f, FLT_MAX, 0.001f);
}
