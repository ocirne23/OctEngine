export module Settings.Force;

import Core;
import Settings.Render; // ForceFieldParams

// The "Force/Merge" settings (ForceSystem's merge pass).
export struct ForceMergeSettings
{
    bool enabled = true;
    float joinDistance = 0.5f;   // join when |ci - cj| < joinDistance * (ri + rj)
    float leaveDistance = 0.85f; // leave when no member is closer than leaveDistance * (ri + rj)
                                 // (< 1: the own bubble reappears while still overlapping the group)
    // Cover of one member from the group centre = spreadScale * |c - centre| + radiusScale * r;
    // the group radius = coverScale * max over members + coverMargin. 1/1/1 covers every member
    // bubble exactly; below 1 the group sphere hugs the crowd more tightly at the price of
    // members' own bubble rims sticking out of it near the edge (the tuned default: 1/1/0.85).
    float spreadScale = 1.0f;
    float radiusScale = 1.0f;
    float coverScale = 0.85f;
    float coverMargin = 0.2f;    // metres added around the members' cover
    float maxRadius = 8.0f;      // a group whose cover would exceed this refuses the member
    int maxMembers = 255;
    int minMembers = 2;          // smaller groups dissolve
    float sumFraction = 0.2f;    // group output = max(largest member, sum * fraction)
    bool memberReadback = true;  // members stay on the GPU as PASSIVE for their own force/pressure
    float smoothTime = 0.3f;     // group sphere easing time constant (s)
    float blendTime = 0.5f;      // member join/leave transition duration (s)
    float leaveFromGroup = 0.5f; // where a Leaving sphere starts: 0 = the own bubble (instant own
                                 // field, no ghost), 1 = a full copy of the group sphere shrinking
                                 // onto the unit (reads as an empty bubble left behind)
};

// ForceSystem's CPU-side settings (the renderer-facing ones are ForceFieldParams).
export struct ForceSystemSettings
{
    bool bakeEnabled = true;
    float bakeSampleHeight = 1.0f;     // world y the baked field is evaluated at (where bodies live)
    float activateRamp = 0.6f;         // "Activate ramp (s)": an activated bubble grows in over this
    float visibleBoundsIsoFrac = 1.0f; // draw-box shrink (packVisibleBounds); 0 = full boxes
    bool bubbleLight = true;
    float bubbleLightIntensity = 2.0f; // x radius^2
    float bubbleLightRange = 2.0f;     // x radius
    float bubbleLightHeight = 0.8f;    // x radius: lift above the bubble centre (out of a Centered structure's mesh)
    float bubbleLightFade = 0.5f;      // seconds, in and out
    float bubbleLightWhite = 0.2f;     // team colour -> white mix
    ForceMergeSettings merge;
    bool debugDraw = false;
    bool debugDrawGroups = false;
    bool debugDrawQueries = false;
    // Read-only stats, written by ForceSystem.
    int statBakeChunks = 0;
    int statEmitters = 0;
    int statSlots = 0; // only ACTIVE emitters hold one
    int statGroups = 0;
    int statMerged = 0;
};

// "Force": the bubble field (ForceFieldParams, pushed to the renderer by ForceSystem every frame) and
// ForceSystem's own settings - registered together, in one interleaved order.
export namespace Settings
{
    void registerForce(ForceFieldParams& params, ForceSystemSettings& s);
}
