export module Spatial:Stress;

import Core;
import Core.glm;
import Core.Frustum;
import Settings;
import :Types;

// Synthetic load harness for the SpatialIndex, driven entirely from the Spatial/Stress
// tweaks (Globals::settings.spatial.stress): spawn/clear N entries with log-uniform radii, random-walk
// a fraction of them per frame (exercising the same-cell fast path vs cross-cell moves), and time a
// sphere query around the camera. Entries live on layer bit 1 so gameplay/render queries (bit 0) never see them.
export class SpatialStressTest final
{
public:

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
    uint32 m_churnCursor = 0;
    SpatialStressSettings& m_settings = Globals::settings.spatial.stress;
};

export namespace Globals
{
    SpatialStressTest spatialStress;
}
