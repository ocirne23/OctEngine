export module Settings.Nav;

import Core;

// "Nav": the flow-field service (Nav::NavSystem). The behaviour of each value is documented at its use in
// Code/Nav (System.cpp) and in the Tweaks table of Code/Nav/CONTEXT.md.
export struct NavSettings
{
    bool enabled = true;
    float fieldRadius = 400.0f; // covers a whole arena (the 600 m co-op map corner to corner from the Base); a unit outside it falls back to the local search
    float rebuildInterval = 0.25f;
    float buildSpread = 0.25f; // seconds a field build is spread over (= the rebuild interval: the next build is due as it lands)
    int clearanceCost = 1;     // 2x on wall-adjacent cells: nudge off walls, no wide detours
    int keepFrames = 120;
    // Fade rates are HALF-LIVES in seconds (frame-rate independent), not per-frame factors.
    float flowHalfLife = 5.0f;
    float pressureDiffusion = 0.1f; // Jacobi step weight at 60 Hz (dt-scaled, clamped to 0.25)
    float pressureFloor = 1.6f;     // magnitude a neighbour needs before it diffuses (0 = off)
    float pressureHalfLife = 1.0f;  // seconds for pressure to halve - the seeded TROUGH has
                                    // to outlive the walk it was planned for, and jams stay
                                    // felt after the crowd that made them moved on
    float flowMaxSpeed = 20.0f;     // per-cell magnitude cap (splats SUM - see FlowField::update)
    float seedArea = 10.0f;         // metres: requests from/to the same area are ONE lane
    float seedCooldown = 3.0f;      // seconds that area pair stays suppressed
    int seedMaxPerFrame = 2;        // hard cap on plan JOBS queued per frame
    float seedTrough = 20.0f;       // NEGATIVE pressure a seeded lane carves (0 = flow only)
    float seedSqueeze = 10.0f;      // extra trough depth per blocked neighbour of a lane cell
    float seedRange = 20.0f;        // metres of the plan actually written (0 = all of it)
    // LOG-SCALED: the slider is the EXPONENT, the gain is 10^x - one slider covers
    // 0.01 .. 1000 m/s of flow per unit of pressure gradient, with fine control at the low end
    // (a linear 0..20 range could neither reach "pressure dominates" nor resolve small values).
    float pressureFlowGainExp = 0.3f;
    int debugMode = 0;
    int debugTeam = 0;
    float debugRadius = 60.0f;
    float debugFlowMin = 0.1f; // hide flow arrows below this (the haze buries the lanes)
};

export namespace Settings
{
    void registerNav(NavSettings& s);
}
