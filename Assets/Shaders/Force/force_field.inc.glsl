// Forcefield bubble field math - THE single definition, used by the shell draw and the grid / force / query compute
// passes. Keep the lobe in sync with the CPU mirror in Code/Force/Private/System.cpp (forceDistributionGain + the debug
// rings' closed-form iso profile): the debug rings and gameplay reason about the same field the pixels shade.
//
// Emitter model: the bubble spans EXACTLY the "output line" pos -> pos + dir * Reach, whatever the shape. In normalized
// coords (X = axial position mapped to [-1, 1], Y = lateral / (Reach/2)):
//   u^2 = X^2 + Y^2 * ((1 - X) / (1 + X))^m,   m = 1 - 2*focus
//   w = Output * (1 - u^2)^2 * distGain(t)     for u < 1, else 0 (COMPACT SUPPORT, C1 falloff)
// focus 0.5 (m = 0) is an EXACT sphere spanning the line; focus 0 a cone with its point at the emitter; focus 1 the cone
// pointed at the target. distGain is a smooth bump at t = Distribution along the line (0 = emitter end, 1 = target); its
// budget normalization is pre-folded into Output on the CPU (forceDistributionMean), so total field is conserved.
// A team's field phi[t] is the SUM of its emitters' w. A point is inside team t where phi[t] > iso AND phi[t] beats
// every other team; the drawn surface is F = phi[best] - max(iso, phi[secondBest]) = 0 - the equal-field pressure
// equilibrium, so squish and focused-cone pierce need no simulation.
//
// The includer sets FORCE_EMITTERS_BINDING (default 1). With FORCE_GRID, evaluation gathers the point's hash-grid cell +
// the big-emitter list (force_grid.inc.glsl); without it, every evaluation scans all fe_count emitters.

#ifndef FORCE_FIELD_INC_GLSL
#define FORCE_FIELD_INC_GLSL

// The LIVE team count (2..MAX_FORCE_TEAMS), injected per pipeline from the game mode (co-op = 2): every per-team loop
// and phi array is sized by it. MAX_FORCE_TEAMS stays the CAP - the UBO color array's size and the "outside every
// bubble" sentinel (never an index).
#ifndef NUM_FORCE_TEAMS
#define NUM_FORCE_TEAMS 8 // = MAX_FORCE_TEAMS, as a PLAIN literal (this macro is used in #if)
#endif

#ifndef FORCE_EMITTERS_BINDING
#define FORCE_EMITTERS_BINDING 1
#endif

struct ForceEmitterData
{
    vec4 posReach;     // xyz = world position, w = Reach (m): bubble spans pos .. pos + dir * Reach
    vec4 dirFocus;     // xyz = normalized direction, w = focus [0,1] (0.5 = sphere, 0/1 = cones)
    vec4 outputParams; // x = Output (distribution budget fold pre-applied), y = shell alpha mult,
                       // z = distribution [0,1] (axial density bump position), w = width (lateral scale, < 1 = narrower)
    uvec4 teamFlags;   // x = team [0, MAX_FORCE_TEAMS), y = FORCE_FLAG_* bits, z = source slot (readback), w = visible bounds
};

// Matches RendererVKLayout::ForceEmittersGpu (header + compacted live emitters).
layout (binding = FORCE_EMITTERS_BINDING, std430) readonly buffer ForceEmitters
{
    // fe_count = field-contributing emitters; fe_evalCount = fe_count + the FORCE_FLAG_PASSIVE tail (merge-group members
    // that only want their own force / pressure readback - only force_emitter.cs evaluates them).
    uint fe_count; uint fe_evalCount; uint fe_pad0; uint fe_pad1;
    ForceEmitterData fe_emitters[];
};

#ifdef FORCE_GRID
#include "force_grid.inc.glsl"
#endif

// The axial density bump: a gaussian at t = Distribution, with a floor so no part of the bubble goes dead.
float forceDistributionGain(float t, float D)
{
    const float b = (t - D) * (1.0 / 0.45);
    return 0.15 + exp(-b * b);
}

// One emitter's field contribution at x (see the header for the model).
float forceContribution(vec3 x, ForceEmitterData e)
{
    const vec3 d = x - e.posReach.xyz;
    const float R = e.posReach.w;
    const float z = dot(d, e.dirFocus.xyz);
    if (z <= 0.0 || z >= R)
        return 0.0;

    const float lat2 = dot(d, d) - z * z;                    // dir is unit; a rounding -0 only feeds the polynomial
    const float X = z * (2.0 / R) - 1.0;                     // axial [-1 at emitter .. 1 at target]
    const float invW = 1.0 / e.outputParams.w;               // width: pure lateral scale, reach untouched
    const float Y2 = lat2 * (4.0 / (R * R)) * (invW * invW); // lateral, in (width*R/2)^2 units
    const float m = 1.0 - 2.0 * e.dirFocus.w;                // focus 0.5 -> 0 (sphere); 0/1 -> +-1 (cones)
    const float q = clamp((1.0 - X) / (1.0 + X), 1e-4, 1e4);
    // m exactly 0 (every merge-group sphere and most shields) skips the pow.
    const float u2 = X * X + Y2 * (m == 0.0 ? 1.0 : pow(q, m));
    if (u2 >= 1.0)
        return 0.0;

    const float qq = 1.0 - u2;
    return e.outputParams.x * qq * qq * forceDistributionGain(z / R, e.outputParams.z);
}

// Conservative local bounds of an emitter's field support, shared by the proxy VS, the FS ray interval and the grid
// insert so they always agree. Local frame: +Z = emitter direction. Axially exactly [0, Reach]; the max lateral
// half-width is R/2 for the sphere, growing to R at the full cones: (R/2)*(1 + |m|). The slack covers normal-tap offsets.
void forceEmitterBounds(ForceEmitterData e, out float side, out float forward, out float back)
{
    const float R = e.posReach.w;
    const float m = abs(1.0 - 2.0 * e.dirFocus.w);
    side = 0.5 * R * (1.0 + m) * e.outputParams.w * 1.03;
    forward = R * 1.02;
    back = R * 0.02;
}

// The DRAWN box: forceEmitterBounds shrunk to the emitter's packed visible-surface extent (teamFlags.w - CPU
// packVisibleBounds in Force/System.cpp: axial lo / hi in R units as two unorm8, lateral fraction of `side` as unorm16;
// 0 = feature off or nothing above the reduced iso, keep the full box). The pack is the OWN-iso extent at iso x "Visible
// bounds iso frac" (lowering it adds merge slack: two sub-iso fields can SUM to a surface outside either's own extent).
// Used by the proxy VS, the interval FS and the shell FS's march interval only - the grid insert, the bake fits and every
// CPU mirror keep the FULL support box (the FIELD is unchanged, only the draw shrinks).
void forceVisibleBounds(ForceEmitterData e, out float side, out float forward, out float back)
{
    forceEmitterBounds(e, side, forward, back);
    const uint p = e.teamFlags.w;
    if (p == 0u)
        return;

    const float R = e.posReach.w;
    const float lo = float(p & 0xFFu) * (1.0 / 255.0);
    const float hi = float((p >> 8u) & 0xFFu) * (1.0 / 255.0);
    side *= float(p >> 16u) * (1.0 / 65535.0);
    forward = R * (hi + 0.02);
    back = R * (0.02 - lo); // lo > 0.02: negative back - the box starts in FRONT of the emitter
}

// The emitter's VISIBLE size: the half-extent of its drawn box - the actual bubble radius, not the authored Reach (a
// merged group sphere's reach is ~2x its bubble). THE sampled-tier metric: the shell FS and the union ownership test
// compare it against u_force_bakeThreshold; MIRRORED on the CPU in RendererVKLayout::forceEmitterVisibleRadius.
float forceVisibleRadius(ForceEmitterData e)
{
    float side, forward, back;
    forceVisibleBounds(e, side, forward, back);
    return max(side, (forward + back) * 0.5);
}

// Orthonormal RIGHT-HANDED frame with +Z = dir. Handedness is load-bearing: a mirrored basis flips the proxy cube's
// winding, so front-face culling keeps the NEAR faces and the shell stops rasterizing with the camera inside the box.
mat3 forceEmitterBasis(vec3 dir)
{
    const vec3 ref = abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 right = normalize(cross(dir, ref));
    const vec3 up = cross(dir, right);
    return mat3(right, up, dir);
}

// ---- candidate enumeration: every field evaluation loops the same way ----
//   const uint cell = forceCandidateCell(x);
//   const uint n = forceNumCandidates(cell);
//   for (uint k = 0u; k < n; ++k) { e = fe_emitters[forceCandidateIdx(cell, k)]; ... }

#ifdef FORCE_GRID
uint forceCandidateCell(vec3 x) { return forceFindCell(forceGridPos(x)); }
uint forceNumCandidates(uint cell) { return cell != FORCE_INVALID_CELL ? forceCellCount(cell) : 0u; }
uint forceCandidateIdx(uint cell, uint k) { return forceCellEmitter(cell, k); }
#else
uint forceCandidateCell(vec3 x) { return 0u; }
uint forceNumCandidates(uint cell) { return fe_count; }
uint forceCandidateIdx(uint cell, uint k) { return k; }
#endif

// Per-team field accumulation at x. Team indices are CPU-clamped below the live count at upload. The Cell variant takes
// the grid cell PRE-RESOLVED: a march whose samples stay in one 16 m cell (the union march) pays the hash probe once.
void forceAccumulateCell(vec3 x, uint cell, out float phi[NUM_FORCE_TEAMS])
{
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        phi[t] = 0.0;

    const uint n = forceNumCandidates(cell);
    for (uint k = 0u; k < n; ++k)
    {
        const ForceEmitterData e = fe_emitters[forceCandidateIdx(cell, k)];
        const float c = forceContribution(x, e);
        // NEVER a dynamic-index store (phi[e.teamFlags.x] += c): the NVIDIA compiler miscompiles that on a 2-element
        // private array (the add lands in BOTH elements for index 1 and NEITHER for index 0). The predicated adds of
        // the unrolled loop are correct at every team count.
        for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
            phi[t] += e.teamFlags.x == t ? c : 0.0;
    }
}

void forceAccumulate(vec3 x, out float phi[NUM_FORCE_TEAMS])
{
    forceAccumulateCell(x, forceCandidateCell(x), phi);
}

// A per-team array READ at a run-time team index, as an unrolled select. `phi[team]` is correct, but a dynamic index
// into a private array lets the compiler put the WHOLE array in local memory (an L1TEX round trip per access); the
// select keeps it in registers. Use it when the index is not a loop induction variable.
// RTX 4090: force_union.fs 176 B local -> 48 B. force_shell.fs keeps the plain index (FORCE_PHI_DYNAMIC_READ): the
// select there gave no clear win (+17 % code).
float forcePhiAt(float phi[NUM_FORCE_TEAMS], uint team)
{
#ifdef FORCE_PHI_DYNAMIC_READ
    return phi[team];
#else
    float r = phi[0];
    for (uint t = 1u; t < NUM_FORCE_TEAMS; ++t)
        r = team == t ? phi[t] : r;
    return r;
#endif
}

// forceAccumulate plus a SHELL-VISIBLE field per team: each contribution also scaled by its emitter's shell alpha
// (outputParams.y). Shading weighs team colors and contact glow by the VISIBLE fields, so an invisible emitter (alpha
// 0, e.g. a map-scale gameplay field) still deforms the geometry but neither tints the junction nor lights the rim.
void forceAccumulateVisible(vec3 x, out float phi[NUM_FORCE_TEAMS], out float phiVis[NUM_FORCE_TEAMS])
{
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
    {
        phi[t] = 0.0;
        phiVis[t] = 0.0;
    }

    const uint cell = forceCandidateCell(x);
    const uint n = forceNumCandidates(cell);
    for (uint k = 0u; k < n; ++k)
    {
        const ForceEmitterData e = fe_emitters[forceCandidateIdx(cell, k)];
        const float c = forceContribution(x, e);
        for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t) // predicated adds, never a dynamic-index store (see forceAccumulate)
        {
            phi[t] += e.teamFlags.x == t ? c : 0.0;
            phiVis[t] += e.teamFlags.x == t ? c * e.outputParams.y : 0.0;
        }
    }
}

// The bound a team's field must beat to be "inside": iso or the strongest opposing field, blended by a C1 smooth max
// (k = u_force_junctionK, 0 = hard max) so the shells meet the equilibrium wall in a rounded fillet - a hard crease's
// discontinuous normals printed march-step-sized jaggies along the junction. The query compute uses the same function,
// so gameplay "inside" matches the drawn surface exactly.
float forceOpposingBound(float iso, float opposing)
{
    const float k = u_force_junctionK;
    const float h = max(k - abs(iso - opposing), 0.0) * u_force_junctionInvK;
    return max(iso, opposing) + h * h * k * 0.25;
}

// The surface function F for a FIXED team: positive inside its bubble, zero on its surface.
float forceSurfaceForTeam(vec3 x, uint team, float iso)
{
    float phi[NUM_FORCE_TEAMS];
    forceAccumulate(x, phi);
    float opposing = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
    return forcePhiAt(phi, team) - forceOpposingBound(iso, opposing);
}

// A fixed team's own field and its best opposing field at x.
void forceTeamSample(vec3 x, uint team, out float own, out float opposing)
{
    float phi[NUM_FORCE_TEAMS];
    forceAccumulate(x, phi);
    own = forcePhiAt(phi, team);
    opposing = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
}

// Full sample: strongest team, its field, and the best opposing field. F > 0 = inside bestTeam's bubble.
void forceSampleFieldCell(vec3 x, uint cell, float iso, out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    float phi[NUM_FORCE_TEAMS];
    forceAccumulateCell(x, cell, phi);
    bestTeam = 0u;
    bestPhi = phi[0];
    for (uint t = 1u; t < NUM_FORCE_TEAMS; ++t)
        if (phi[t] > bestPhi) { bestPhi = phi[t]; bestTeam = t; }

    secondPhi = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != bestTeam)
            secondPhi = max(secondPhi, phi[t]);
    F = bestPhi - forceOpposingBound(iso, secondPhi);
}

void forceSampleField(vec3 x, float iso, out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    forceSampleFieldCell(x, forceCandidateCell(x), iso, bestTeam, bestPhi, secondPhi, F);
}

// The strongest single contributor of `team` at x - the OWNER of a shell surface point. Overlapping same-team proxies
// would shade a merged surface point once per proxy; only the fragment whose instance IS the dominant contributor keeps
// it (compact support puts the point inside that emitter's own proxy). Ties break to the lowest compact index.
uint forceDominantEmitter(vec3 x, uint team)
{
    uint bestIdx = 0xFFFFFFFFu;
    float bestW = -1.0;
    const uint cell = forceCandidateCell(x);
    const uint n = forceNumCandidates(cell);
    for (uint k = 0u; k < n; ++k)
    {
        const uint i = forceCandidateIdx(cell, k);
        if (fe_emitters[i].teamFlags.x != team)
            continue;
        const float w = forceContribution(x, fe_emitters[i]);
        if (w > bestW || (w == bestW && i < bestIdx))
        {
            bestW = w;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// Signed field difference between two teams: positive on teamA's side, zero on the equilibrium WALL between two pressed
// bubbles. F only dips to 0 there without changing sign (best / second swap), so the wall is refined on THIS function.
float forceTeamDiff(vec3 x, uint teamA, uint teamB)
{
    float phi[NUM_FORCE_TEAMS];
    forceAccumulate(x, phi);
    return forcePhiAt(phi, teamA) - forcePhiAt(phi, teamB);
}

// Normal of the equilibrium wall (gradient of the team difference), pointing toward teamA's side.
vec3 forceWallNormal(vec3 x, uint teamA, uint teamB, float h)
{
    const vec2 k = vec2(1.0, -1.0);
    vec3 grad = k.xyy * forceTeamDiff(x + k.xyy * h, teamA, teamB)
              + k.yyx * forceTeamDiff(x + k.yyx * h, teamA, teamB)
              + k.yxy * forceTeamDiff(x + k.yxy * h, teamA, teamB)
              + k.xxx * forceTeamDiff(x + k.xxx * h, teamA, teamB);
    return normalize(grad + vec3(0.0, 1e-6, 0.0));
}

// Outward surface normal via 4-tap tetrahedral differences of F (team held fixed). F increases inward, so the normal is
// the negated gradient. h scales with the dominant emitter's reach so tiny bubbles and huge domes both resolve.
vec3 forceSurfaceNormal(vec3 x, uint team, float iso, float h)
{
    const vec2 k = vec2(1.0, -1.0);
    vec3 grad = k.xyy * forceSurfaceForTeam(x + k.xyy * h, team, iso)
              + k.yyx * forceSurfaceForTeam(x + k.yyx * h, team, iso)
              + k.yxy * forceSurfaceForTeam(x + k.yxy * h, team, iso)
              + k.xxx * forceSurfaceForTeam(x + k.xxx * h, team, iso);
    return -normalize(grad + vec3(0.0, 1e-6, 0.0));
}

#endif
