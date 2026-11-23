module RendererVK;

import Core;
import Core.glm;
import :SnowPack;
import :Layout;

float SnowPack::binTemperature(uint32 t)
{
    using namespace RendererVKLayout;
    return SNOW_TABLE_TEMP_MIN + (float)t * (SNOW_TABLE_TEMP_MAX - SNOW_TABLE_TEMP_MIN) / (float)(TEMP_BINS - 1);
}

float SnowPack::binCatch(uint32 c)
{
    return (float)c * RendererVKLayout::SNOW_TABLE_CATCH_MAX / (float)(CATCH_BINS - 1);
}

float SnowPack::snowFraction(float airC, float snowBelow, float rainAbove)
{
    return 1.0f - glm::smoothstep(snowBelow, glm::max(rainAbove, snowBelow + 0.01f), airC);
}

// The ice's absorption per channel (m^-1, from its imaginary index: gamma = 4 pi k / lambda; good to ~2x), the grime's at
// full grime (a look value: dust absorbs blue most), and the absorption path per grain diameter. snow.inc.glsl has the
// same three (SNOW_ICE_ABS, SNOW_GRIME_ABS, SNOW_ABS_PATH).
static const glm::vec3 ICE_ABS = glm::vec3(0.31f, 0.046f, 0.016f);
static const glm::vec3 GRIME_ABS = glm::vec3(6.0f, 8.0f, 11.0f);
static constexpr float ABS_PATH = 13.0f;

glm::vec3 SnowPack::surfaceAlbedo(float grainMm, float liquid, float grime)
{
    const float dEff = grainMm * 1e-3f * (1.0f + 2.0f * liquid); // a water film joins the grains into larger optical ones
    return glm::exp(-glm::sqrt((ICE_ABS + grime * GRIME_ABS) * (ABS_PATH * dEff)));
}

// Per cell, over the step: + the snowfall x the catch, then the settling, then the melt (degree-hours over 0 C plus the
// sun's share, which fades out in cold air: none below -8 C), clamped to [0, max depth]. In doubles: a slow snowfall
// adds micrometres per frame.
// THE STATE (in doubles too: its per-frame increments are as small): new snow renews the surface (over "Surface layer":
// fresh grains, no grime, dry; a bare cell that gets snow again starts fresh); the melt (and the rain on the pack) wets it
// - in sub-zero air the sun's melt refreezes in the surface (none stays liquid from -4 C) - and the meltwater drains (above
// 0 C) or freezes (below), stepped exactly (no step-length bias); dry grains grow slowly (slower in deep cold), wet ones
// fast (corn snow); the grime settles, and the melt concentrates it. A melted-out cell KEEPS its state (the melting edge's
// bilinear neighbours see old snow, not fresh), and its meltwater keeps decaying: the ground stays wet as it melts out.
void SnowPack::advance(const Step& s)
{
    if (s.gameHours <= 0.0)
        return;
    const double hours = s.gameHours;
    const double settle = std::exp(-(double)glm::max(s.settlingPerHour, 0.0f) * hours);
    const double maxDepth = (double)glm::max(s.maxDepth, 0.0f);
    const double freshGrain = (double)glm::max(s.freshGrainMm, 0.01f);
    const double surfaceLayer = (double)glm::max(s.surfaceLayer, 1e-4f);
    const double meltHold = (double)glm::max(s.meltHold, 1e-5f);
    const double wetGrowth = (double)glm::max(s.wetGrainGrowth, 0.0f);
    const double meltGrime = (double)glm::max(s.meltGrime, 0.0f);
    float deepest = 0.0f;
    for (uint32 t = 0; t < TEMP_BINS; ++t)
    {
        const float air = binTemperature(t) + s.airOffset;
        const float snowShare = snowFraction(air, s.snowBelow, s.rainAbove);
        const double fall = (double)(s.snowfallRate * s.precipitation * snowShare) * hours;
        const float meltPerHour = s.meltRate * glm::max(air, 0.0f) + s.sunMelt * glm::max(s.sunHeight, 0.0f) * glm::smoothstep(-8.0f, 0.0f, air);
        const double melt = (double)meltPerHour * hours;
        const double meltLiquid = (double)glm::smoothstep(-4.0f, 0.5f, air); // the share of the melt that stays liquid
        const double rainWetRate = (double)((1.0f - snowShare) * s.precipitation) * 2.0; // rain soaks the surface in ~0.5 h
        const double drainHours = (double)glm::max(air >= 0.0f ? s.drainHours : s.refreezeHours, 0.01f);
        const double drain = std::exp(-hours / drainHours);
        const double dryGrowth = (double)glm::max(s.grainGrowth, 0.0f) * std::exp(0.07 * (double)glm::min(air, 0.0f)) * hours / 24.0;
        const double grimeAdd = (double)glm::max(s.grimePerDay, 0.0f) * hours / 24.0;
        for (uint32 c = 0; c < CATCH_BINS; ++c)
        {
            const uint32 i = c * TEMP_BINS + t;
            const double before = m_depth[i];
            const double fallen = fall * (double)binCatch(c);
            const double settled = (before + fallen) * settle;
            const double d = glm::clamp(settled - melt, 0.0, maxDepth);
            m_depth[i] = d;
            m_depthOut[i] = (float)d;
            deepest = glm::max(deepest, (float)d);

            StateD& st = m_stateD[i];
            if (before <= 0.0 && fallen > 0.0)
                st = StateD{ freshGrain, 0.0, 0.0 }; // new snow on bare ground
            const double meltApplied = glm::min(melt, glm::max(settled, 0.0));
            const double renew = 1.0 - std::exp(-fallen / surfaceLayer);
            st.grainMm += (freshGrain - st.grainMm) * renew;
            st.grime *= 1.0 - renew;
            st.liquid *= 1.0 - renew;
            // d(liquid)/dt = rate - liquid / drainHours, exact over the step (the melt only while snow lies).
            const double rate = d > 0.0 ? meltApplied * meltLiquid / (meltHold * hours) + rainWetRate : 0.0;
            const double liquidBefore = st.liquid;
            st.liquid = glm::clamp(st.liquid * drain + rate * drainHours * (1.0 - drain), 0.0, 1.0);
            const double liquidMean = 0.5 * (liquidBefore + st.liquid);
            if (liquidMean < 0.05)
                st.grainMm = std::sqrt(st.grainMm * st.grainMm + dryGrowth);
            else
                st.grainMm = std::cbrt(st.grainMm * st.grainMm * st.grainMm + wetGrowth * liquidMean * hours);
            st.grime = glm::clamp(st.grime + (grimeAdd + meltGrime * meltApplied / surfaceLayer) * (1.0 - st.grime), 0.0, 1.0);
            st.grainMm = glm::clamp(st.grainMm, 0.5 * freshGrain, 5.0);

            m_state[i] = CellState{ .grainMm = (float)st.grainMm, .liquid = (float)st.liquid, .grime = (float)st.grime };
        }
    }
    m_deepest = deepest;
}

void SnowPack::clear(float freshGrainMm)
{
    const float fresh = glm::max(freshGrainMm, 0.01f);
    m_depth.fill(0.0);
    m_depthOut.fill(0.0f);
    m_state.fill(CellState{ .grainMm = fresh });
    m_stateD.fill(StateD{ (double)fresh, 0.0, 0.0 });
    m_deepest = 0.0f;
}
