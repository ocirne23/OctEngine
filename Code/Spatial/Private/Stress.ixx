export module Spatial:Stress;

import Core;
import Core.glm;
import Core.Frustum;
import :Types;

// Synthetic load harness for the SpatialIndex, driven entirely from the Spatial/Stress
// tweaks: spawn/clear N entries with log-uniform radii, random-walk a fraction of them per frame
// (exercising the same-cell fast path vs cross-cell moves), and time a sphere query around the
// camera. Entries live on layer bit 1 so gameplay/render queries (bit 0) never see them.
export class SpatialStressTest final
{
public:

    void initialize(); // registers the Spatial/Stress tweaks
    void update(const glm::dvec3& cameraPos, const Frustum& frustum);

private:

    void spawnEntries();
    void clearEntries();
    void churn();

    uint64 nextRandom()
    {
        uint64 x = m_rngState;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        return m_rngState = x;
    }
    float randomUnit() { return float(nextRandom() >> 40) * (1.0f / 16777216.0f); }
    double randomSym() { return double(nextRandom() >> 11) * (2.0 / 4503599627370496.0) - 1.0; }

    oc::vector<SpatialHandle> m_handles;
    oc::vector<glm::dvec3> m_positions;
    oc::vector<float> m_radii;
    oc::vector<uint64> m_queryResults;
    uint64 m_rngState = 0x9e37'79b9'7f4a'7c15ull;
    int m_count = 100000;
    float m_extent = 2000.0f;
    float m_churnPercent = 0.0f;
    float m_queryRadius = 100.0f;
    float m_queryMs = 0.0f;
    float m_frustumMaxDist = 2000.0f;
    float m_frustumMs = 0.0f;
    int m_queryHits = 0;
    int m_frustumHits = 0;
    int m_verifyDelta = 0;
    uint32 m_churnCursor = 0;
    bool m_spawnRequested = false;
    bool m_clearRequested = false;
    bool m_runSphereQuery = false;
    bool m_runFrustumQuery = false;
    bool m_verifyBruteForce = false;
};

export namespace Globals
{
    SpatialStressTest spatialStress;
}
