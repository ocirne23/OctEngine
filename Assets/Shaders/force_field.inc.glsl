// Forcefield bubble field math — THE single definition, consumed by the shell draw
// (force_shell.vs/fs.glsl) and the grid/force/query compute passes. Keep the lobe in sync with the
// CPU mirror in Code/Force/Private/System.cpp (forceDistributionGain + the debug rings' closed-form
// iso profile) — the debug rings and gameplay reason
// about the same field the pixels shade.
//
// Emitter model: the bubble spans EXACTLY the "output line" pos -> pos + dir * Reach, whatever the
// shape. In normalized coords (X = axial position mapped to [-1, 1], Y = lateral / (Reach/2)):
//   u^2 = X^2 + Y^2 * ((1 - X) / (1 + X))^m,   m = 1 - 2*focus
//   w = Output * (1 - u^2)^2 * distGain(t)     for u < 1, else 0 (COMPACT SUPPORT, C1 falloff)
// focus 0.5 (m = 0) is an EXACT sphere spanning the line; focus 0 an exact cone with its point at
// the emitter; focus 1 the cone pointed at the target — one "pinch" slider, reach never changes.
// distGain is a smooth bump at t = Distribution along the line (0 = density at the emitter end,
// 1 = at the target), with its budget normalization pre-folded into Output on the CPU
// (System.cpp forceDistributionMean) so total emitted field is conserved exactly.
// A team's field phi[t] is the SUM of its emitters' w (merging). A point is inside team t where
// phi[t] > iso AND phi[t] beats every other team; the drawn surface is
//   F = phi[best] - max(iso, phi[secondBest]) = 0
// — the equal-field pressure equilibrium, so squish and focused-cone pierce need no simulation.
//
// This include declares the compacted emitter buffer itself; the consumer sets
// FORCE_EMITTERS_BINDING before including (default 1). Candidate enumeration: with FORCE_GRID
// defined, evaluation gathers the point's hash-grid cell + the globally-scanned big-emitter list
// (force_grid.inc.glsl); without it, every evaluation scans all fe_count emitters (small scenes /
// A-B correctness toggle).

#ifndef FORCE_FIELD_INC_GLSL
#define FORCE_FIELD_INC_GLSL

#ifndef FORCE_EMITTERS_BINDING
#define FORCE_EMITTERS_BINDING 1
#endif

struct ForceEmitterData
{
    vec4 posReach;     // xyz = world position, w = Reach (m): bubble spans pos .. pos + dir * Reach
    vec4 dirFocus;     // xyz = normalized direction, w = focus [0,1] (0.5 = sphere, 0/1 = cones)
    vec4 outputParams; // x = Output (distribution budget fold pre-applied), y = shell alpha mult,
                       // z = distribution [0,1] (axial density bump position),
                       // w = width (lateral scale: 1 = round, < 1 pinches the shape narrower)
    uvec4 teamFlags;   // x = team [0, MAX_FORCE_TEAMS), y = FORCE_FLAG_* bits, z = source slot (readback), w unused
};

// Matches RendererVKLayout::ForceEmittersGpu (header + big-emitter index list + compacted live emitters).
layout (binding = FORCE_EMITTERS_BINDING, std430) readonly buffer ForceEmitters
{
    uint fe_count; uint fe_bigCount; uint fe_pad0; uint fe_pad1;
    uint fe_bigIndices[MAX_FORCE_BIG_EMITTERS];
    ForceEmitterData fe_emitters[];
};

#ifdef FORCE_GRID
#include "force_grid.inc.glsl"
#endif

// The axial density bump: smooth gaussian at t = Distribution along the output line, with a floor
// so no part of the bubble goes dead. The budget fold (1 / field-weighted mean of this) is
// pre-applied to Output on the CPU.
float forceDistributionGain(float t, float D)
{
    const float b = (t - D) * (1.0 / 0.45);
    return 0.15 + exp(-b * b);
}

// One emitter's field contribution at x (see the header comment for the model).
float forceContribution(vec3 x, ForceEmitterData e)
{
    const vec3 d = x - e.posReach.xyz;
    const float R = e.posReach.w;
    const float z = dot(d, e.dirFocus.xyz);
    if (z <= 0.0 || z >= R)
        return 0.0; // the bubble spans exactly the output line: nothing behind pos or past target
    const float lat2 = max(dot(d, d) - z * z, 0.0);
    const float X = z * (2.0 / R) - 1.0;        // axial [-1 at emitter .. 1 at target]
    const float invW = 1.0 / e.outputParams.w;  // width: pure lateral scale, reach untouched
    const float Y2 = lat2 * (4.0 / (R * R)) * (invW * invW); // lateral, in (width*R/2)^2 units
    const float m = 1.0 - 2.0 * e.dirFocus.w;   // focus 0.5 -> 0 (sphere); 0/1 -> +-1 (cones)
    const float q = clamp((1.0 - X) / (1.0 + X), 1e-4, 1e4);
    const float u2 = X * X + Y2 * pow(q, m);
    if (u2 >= 1.0)
        return 0.0;
    const float qq = 1.0 - u2;
    return e.outputParams.x * qq * qq * forceDistributionGain(z / R, e.outputParams.z);
}

// Conservative local bounds of an emitter's field support, shared by the proxy VS, the FS ray
// interval, and the grid insert so they always agree. Local frame: +Z = emitter direction. The
// support spans exactly [0, Reach] axially; max lateral half-width is R/2 for the sphere growing
// to R at the full cones — bounded by (R/2)*(1 + |m|). Small slack covers normal-tap offsets.
void forceEmitterBounds(ForceEmitterData e, out float side, out float forward, out float back)
{
    const float R = e.posReach.w;
    const float m = abs(1.0 - 2.0 * e.dirFocus.w);
    side = 0.5 * R * (1.0 + m) * e.outputParams.w * 1.03;
    forward = R * 1.02;
    back = R * 0.02;
}

// Orthonormal RIGHT-HANDED frame with +Z = dir (right x up == dir). Handedness is load-bearing: a
// mirrored basis flips the proxy cube's winding, so front-face culling keeps the NEAR faces instead
// of the far ones and the shell stops rasterizing entirely with the camera inside the box.
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
uint forceCandidateCell(vec3 x) { return forceFindCell(getGridPos(x)); }
uint forceNumCandidates(uint cell)
{
    return fe_bigCount + (cell != FORCE_INVALID_CELL ? forceCellCount(cell) : 0u);
}
uint forceCandidateIdx(uint cell, uint k)
{
    return k < fe_bigCount ? fe_bigIndices[k] : forceCellEmitter(cell, k - fe_bigCount);
}
#else
uint forceCandidateCell(vec3 x) { return 0u; }
uint forceNumCandidates(uint cell) { return fe_count; }
uint forceCandidateIdx(uint cell, uint k) { return k; }
#endif

// GLOBAL AMBIENT FIELD (u_forceParams4.zw + u_forceParams5): an analytic distance-based term added
// to one team everywhere — zero within the safe radius of the planar center, growing per metre,
// capped. No emitter, no proxy, never drawn (the visible-field shading path sees it at zero
// visibility); it deforms bubbles and feeds every readback like any other field. slope <= 0 = off.
float forceAmbientField(vec3 x)
{
    const float slope = u_forceParams4.z;
    if (slope <= 0.0)
        return 0.0;
    const float d = distance(x.xz, u_forceParams5.xy);
    return clamp((d - u_forceParams5.z) * slope, 0.0, u_forceParams5.w);
}

// Per-team field accumulation at x. MAX_FORCE_TEAMS is injected from Layout.ixx (= 8).
void forceAccumulate(vec3 x, out float phi[MAX_FORCE_TEAMS])
{
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        phi[t] = 0.0;
    const uint cell = forceCandidateCell(x);
    const uint n = forceNumCandidates(cell);
    for (uint k = 0u; k < n; ++k)
    {
        const ForceEmitterData e = fe_emitters[forceCandidateIdx(cell, k)];
        phi[e.teamFlags.x] += forceContribution(x, e);
    }
    phi[uint(u_forceParams4.w)] += forceAmbientField(x);
}

// forceAccumulate plus a SHELL-VISIBLE variant of each team's field: every contribution also
// scaled by its emitter's shell alpha (outputParams.y). The shading path weighs team colors and
// contact glow by the VISIBLE fields, so an invisible emitter (alpha 0 — e.g. a map-scale gameplay
// field) still deforms bubble geometry but neither tints the junction color mix nor lights the
// whole rim as a contested seam.
void forceAccumulateVisible(vec3 x, out float phi[MAX_FORCE_TEAMS], out float phiVis[MAX_FORCE_TEAMS])
{
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
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
        phi[e.teamFlags.x] += c;
        phiVis[e.teamFlags.x] += c * e.outputParams.y;
    }
    phi[uint(u_forceParams4.w)] += forceAmbientField(x); // invisible: shapes, never tints
}

// C1 smooth max (k = blend width; 0 = hard max). Used for the surface's opposing bound so the
// shells meet the equilibrium wall in a rounded fillet instead of a hard crease — the crease's
// discontinuous normals printed march-step-sized classification jaggies along the junction.
float forceSmoothMax(float a, float b, float k)
{
    const float h = max(k - abs(a - b), 0.0) / max(k, 1e-5);
    return max(a, b) + h * h * k * 0.25;
}

// The bound a team's field must beat to be "inside": iso or the strongest opposing field, smoothly
// blended (u_forceParams3.w = junction smoothing as a fraction of iso; scale-free since the rim
// region lives at field values ~iso). The query compute uses the same function, so gameplay
// "inside" matches the drawn surface exactly.
float forceOpposingBound(float iso, float opposing)
{
    return forceSmoothMax(iso, opposing, u_forceParams3.w * iso);
}

// The render surface function F for a FIXED team t (marched/refined/differentiated holding the hit
// team constant): positive inside t's bubble, zero on its surface.
float forceSurfaceForTeam(vec3 x, uint team, float iso)
{
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(x, phi);
    float opposing = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
    return phi[team] - forceOpposingBound(iso, opposing);
}

// A fixed team's own field and its best opposing field at x.
void forceTeamSample(vec3 x, uint team, out float own, out float opposing)
{
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(x, phi);
    own = phi[team];
    opposing = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
}

// Full sample: strongest team, its field, and the best opposing field. F > 0 means x is inside
// bestTeam's bubble.
void forceSampleField(vec3 x, float iso, out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(x, phi);
    bestTeam = 0u;
    bestPhi = phi[0];
    for (uint t = 1u; t < MAX_FORCE_TEAMS; ++t)
        if (phi[t] > bestPhi) { bestPhi = phi[t]; bestTeam = t; }
    secondPhi = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        if (t != bestTeam)
            secondPhi = max(secondPhi, phi[t]);
    F = bestPhi - forceOpposingBound(iso, secondPhi);
}

// The strongest single contributor of `team` at x — the OWNER of a shell surface point. Every proxy
// marches its own box, so overlapping same-team proxies would shade a merged surface point once per
// proxy; only the fragment whose instance IS the dominant contributor keeps it (compact support puts
// the point inside the dominant emitter's own proxy, so exactly one fragment survives). Ties break
// to the lowest compact index.
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

// Signed field difference between two teams: positive on teamA's side, zero exactly on the
// equilibrium WALL between two pressed bubbles. F itself only dips to 0 there without changing
// sign (best/second swap), so the interior wall must be detected and refined on THIS function.
float forceTeamDiff(vec3 x, uint teamA, uint teamB)
{
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(x, phi);
    return phi[teamA] - phi[teamB];
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

// Outward surface normal at a hit via 4-tap tetrahedral finite differences of F (team held fixed).
// F increases inward, so the outward normal is the negated gradient. h scales with the dominant
// emitter's reach so tiny bubbles and huge domes both resolve.
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
