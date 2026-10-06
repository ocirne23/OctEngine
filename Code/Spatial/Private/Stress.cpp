module Spatial;

import Core;
import Core.glm;
import Core.Frustum;
import Settings;

void SpatialStressTest::update(const glm::dvec3& cameraPos, const Frustum& frustum)
{
    SpatialStressSettings& s = m_settings;
    if (s.clearRequested)
    {
        s.clearRequested = false;
        clearEntries();
    }
    if (s.spawnRequested)
    {
        s.spawnRequested = false;
        clearEntries();
        spawnEntries();
    }
    if (s.churnPercent > 0.0f && !m_handles.empty())
        churn();

    if (s.runSphereQuery)
    {
        const auto start = Clock::now();
        s.queryHits = int(Globals::spatialIndex.querySphere(cameraPos, s.queryRadius, SpatialLayer_Stress, m_queryResults));
        s.queryMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
        if (s.verifyBruteForce)
        {
            // double-precision reference; tiny deltas are float rounding on the sphere boundary
            int expected = 0;
            for (size_t i = 0; i < m_positions.size(); ++i)
            {
                const double r = double(s.queryRadius) + double(m_radii[i]);
                const glm::dvec3 d = m_positions[i] - cameraPos;
                expected += glm::dot(d, d) <= r * r ? 1 : 0;
            }
            s.verifyDelta = glm::abs(expected - s.queryHits);
        }
    }
    else
    {
        s.queryHits = 0;
        s.queryMs = 0.0f;
    }

    if (s.runFrustumQuery)
    {
        const auto start = Clock::now();
        s.frustumHits = int(Globals::spatialIndex.queryFrustum(rebaseFrustum(frustum, cameraPos), cameraPos,
            s.frustumMaxDist, SpatialLayer_Stress, m_queryResults));
        s.frustumMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    }
    else
    {
        s.frustumHits = 0;
        s.frustumMs = 0.0f;
    }
}

void SpatialStressTest::spawnEntries()
{
    const int count = m_settings.count;
    m_handles.reserve(uint32(count));
    m_positions.reserve(uint32(count));
    m_radii.reserve(uint32(count));
    for (int i = 0; i < count; ++i)
    {
        const glm::dvec3 pos = glm::dvec3(randomSym(), randomSym(), randomSym()) * double(m_settings.extent);
        const float radius = 0.25f * exp2f(randomUnit() * 6.0f); // log-uniform 0.25m .. 16m
        m_positions.push_back(pos);
        m_radii.push_back(radius);
        m_handles.push_back(Globals::spatialIndex.registerEntry(pos, radius, uint64(i), SpatialLayer_Stress));
    }
}

void SpatialStressTest::clearEntries()
{
    for (const SpatialHandle& handle : m_handles)
        Globals::spatialIndex.unregisterEntry(handle);
    m_handles.clear();
    m_positions.clear();
    m_radii.clear();
    m_churnCursor = 0;
}

void SpatialStressTest::churn()
{
    const uint32 total = uint32(m_handles.size());
    uint32 numToMove = uint32(double(total) * double(m_settings.churnPercent) * 0.01);
    if (numToMove > total)
        numToMove = total;
    const double bound = double(m_settings.extent);
    for (uint32 n = 0; n < numToMove; ++n)
    {
        const uint32 i = m_churnCursor++ % total;
        glm::dvec3& pos = m_positions[i];
        pos += glm::dvec3(randomSym(), randomSym(), randomSym()) * 0.5;
        pos = glm::clamp(pos, glm::dvec3(-bound), glm::dvec3(bound));
        Globals::spatialIndex.updateEntry(m_handles[i], pos, m_radii[i]);
    }
}
