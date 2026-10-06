module Settings.Force;

import Core;
import Core.glm;
import Settings.Render;
import Settings.Tweaks;

void Settings::registerForce(ForceFieldParams& params, ForceSystemSettings& s)
{
    Tweak::boolean("Force", "Enabled", &params.enabled);
    Tweak::floatVar("Force", "Iso threshold", &params.isoThreshold, 0.01f, 2.0f);
    Tweak::intVar("Force", "March steps", &params.marchSteps, 8, 128);
    Tweak::boolean("Force", "Use grid", &params.useGrid); // off = brute force (A-B correctness check)
    Tweak::boolean("Force/Bake", "Enabled", &s.bakeEnabled);
    Tweak::floatVar("Force/Bake", "Sample height", &s.bakeSampleHeight, 0.0f, 10.0f, 0.1f);
    Tweak::intVar("Force/Bake", "Chunks (stat)", &s.statBakeChunks, 0, 100000);
    Tweak::floatVar("Force", "Force gain", &params.forceGain, 0.0f, 10.0f);
    Tweak::floatVar("Force", "Activate ramp (s)", &s.activateRamp, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Force/Shell", "Alpha", &params.shellAlpha, 0.0f, 1.0f);
    // Draw culling/LOD (the field/readbacks of a culled shell stay live; desktop only):
    Tweak::floatVar("Force/Shell", "Min screen radius (px)", &params.minShellPixels, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Force/Shell", "Full-detail radius (px)", &params.shellFullResPixels, 8.0f, 1024.0f, 4.0f);
    Tweak::floatVar("Force/Shell", "Sampled tier radius (m)", &params.sampledShellRadius, 0.0f, 100.0f, 1.0f);
    Tweak::boolean("Force/Shell", "Union march", &params.unionMarch);
    Tweak::boolean("Force/Shell", "Union half res", &params.unionHalfRes); // rebuild-class (device idle)
    Tweak::boolean("Force/Shell", "Union jitter", &params.unionJitter);    // rebuild-class (shader define)
    Tweak::floatVar("Force/Shell", "Union step (m)", &params.unionStepSize, 0.05f, 4.0f, 0.05f);
    Tweak::intVar("Force/Shell", "Union max steps", &params.unionMaxSteps, 8, 512, 8);
    // The sampled-tier volume's fit is clipped to the camera's view footprint + this (0 = the
    // unbounded union of every large bubble's support box).
    Tweak::floatVar("Force/Shell", "Volume view margin (m)", &params.shellVolumeViewMargin, 0.0f, 200.0f, 1.0f);
    // The draw-box shrink's iso reduction (see packVisibleBounds): 0 = full support boxes.
    Tweak::floatVar("Force/Shell", "Visible bounds iso frac", &s.visibleBoundsIsoFrac, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Force/Shell", "Interior alpha", &params.interiorAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Backface alpha", &params.backfaceAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Rim power", &params.rimPower, 0.5f, 8.0f);
    Tweak::floatVar("Force/Shell", "Rim intensity", &params.rimIntensity, 0.0f, 8.0f);
    Tweak::floatVar("Force/Glow", "Contact intensity", &params.contactGlowIntensity, 0.0f, 16.0f);
    Tweak::floatVar("Force/Glow", "Contact width", &params.contactGlowWidth, 0.01f, 1.0f);
    Tweak::floatVar("Force/Glow", "Contact wall alpha", &params.contactWallAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Junction smoothing", &params.junctionSmoothing, 0.0f, 2.0f);
    Tweak::floatVar("Force/Glow", "Geometry distance (m)", &params.geoGlowDistance, 0.0f, 4.0f);
    Tweak::boolean("Force/Glow", "Bubble light", &s.bubbleLight);
    Tweak::floatVar("Force/Glow", "Bubble light intensity", &s.bubbleLightIntensity, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Force/Glow", "Bubble light range (x radius)", &s.bubbleLightRange, 0.5f, 4.0f, 0.05f);
    Tweak::floatVar("Force/Glow", "Bubble light height (x radius)", &s.bubbleLightHeight, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Force/Glow", "Bubble light fade (s)", &s.bubbleLightFade, 0.02f, 3.0f, 0.02f);
    Tweak::floatVar("Force/Glow", "Bubble light white mix", &s.bubbleLightWhite, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Force/Pattern", "Scale (1/m)", &params.patternScale, 0.01f, 8.0f);
    Tweak::floatVar("Force/Pattern", "Scroll speed", &params.patternSpeed, 0.0f, 4.0f);
    Tweak::floatVar("Force/Pattern", "Intensity", &params.patternIntensity, 0.0f, 4.0f);
    static constexpr const char* teamNames[8] = { "Team 0", "Team 1", "Team 2", "Team 3", "Team 4", "Team 5", "Team 6", "Team 7" };
    static_assert(sizeof(params.teamColors) / sizeof(params.teamColors[0]) == 8);
    for (uint32 i = 0; i < 8; ++i)
        Tweak::color3("Force/Teams", teamNames[i], &params.teamColors[i]);
    Tweak::intVar("Force", "Emitters (stat)", &s.statEmitters, 0, 1000000);
    Tweak::intVar("Force", "GPU slots (stat)", &s.statSlots, 0, 1000000); // only ACTIVE emitters hold one
    Tweak::boolean("Force/Merge", "Enabled", &s.merge.enabled);
    Tweak::floatVar("Force/Merge", "Join distance (x radii)", &s.merge.joinDistance, 0.0f, 1.5f, 0.01f);
    Tweak::floatVar("Force/Merge", "Leave distance (x radii)", &s.merge.leaveDistance, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover spread scale", &s.merge.spreadScale, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover radius scale", &s.merge.radiusScale, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover scale", &s.merge.coverScale, 0.1f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover margin (m)", &s.merge.coverMargin, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Force/Merge", "Max group radius (m)", &s.merge.maxRadius, 1.0f, 256.0f, 0.5f);
    Tweak::intVar("Force/Merge", "Max members", &s.merge.maxMembers, 2, 1024);
    Tweak::intVar("Force/Merge", "Min members", &s.merge.minMembers, 2, 64);
    Tweak::floatVar("Force/Merge", "Summed output fraction", &s.merge.sumFraction, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Force/Merge", "Member readback", &s.merge.memberReadback);
    Tweak::floatVar("Force/Merge", "Smooth time (s)", &s.merge.smoothTime, 0.0f, 3.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Blend time (s)", &s.merge.blendTime, 0.01f, 3.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Leave from group sphere", &s.merge.leaveFromGroup, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Force/Merge", "Groups (stat)", &s.statGroups, 0, 100000);
    Tweak::intVar("Force/Merge", "Merged emitters (stat)", &s.statMerged, 0, 100000);
    Tweak::boolean("Force/Debug", "Draw emitters", &s.debugDraw);
    Tweak::boolean("Force/Debug", "Draw merge groups", &s.debugDrawGroups);
    Tweak::boolean("Force/Debug", "Draw queries", &s.debugDrawQueries);
    Tweak::boolean("Force/Debug", "Density view", &params.densityView);
    Tweak::boolean("Force/Debug", "Log tier classification", &params.logTierDebug);
    Tweak::floatVar("Force/Debug", "Density range", &params.densityRange, 0.1f, 10.0f, 0.05f);
}
