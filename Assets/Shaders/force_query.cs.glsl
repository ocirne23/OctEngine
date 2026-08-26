#version 460

// Gameplay point queries: one thread per registered query slot, evaluating the per-team fields at
// the query position ("which team's bubble, after deformation, contains this point?") PLUS the
// gradient of the strongest field OPPOSING the query's own team — shield-less swarm units read it
// back as their push direction (they carry no emitter, so the per-emitter force integral path does
// not exist for them). Results are written slot-indexed straight into the host-visible readback
// buffer, stamped with the frame index so the CPU can tell a live result from a never-evaluated
// slot; the CPU reads them ~2 frames later.

layout (local_size_x = FORCE_SIM_GROUP_SIZE) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

// Matches RendererVKLayout::ForceQueriesGpu / ForceQueryResult.
struct ForceQueryData { vec4 posActive; }; // xyz = world position, w = 0 inactive / 1 + queryTeam
struct ForceQueryResult
{
    uint owningTeam;        // MAX_FORCE_TEAMS = outside every bubble
    float ownField;
    float bestOpposingField;
    uint frameStamp;
    vec4 opposingGrad;      // xyz = gradient of the strongest field opposing the query's team,
                            // w = that field's VALUE at the point (the local pressure analog)
};

layout (binding = 5, std430) readonly buffer Queries
{
    uint q_count; uint q_pad0; uint q_pad1; uint q_pad2;
    ForceQueryData q_queries[];
};
layout (binding = 6, std430) writeonly buffer OutResults { ForceQueryResult out_results[]; };

// The strongest field of any team EXCEPT `team` at p (what presses on a body of that team).
float forceOpposingTo(vec3 p, uint team)
{
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(p, phi);
    float opposing = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
    return opposing;
}

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= q_count)
        return;
    const ForceQueryData q = q_queries[i];
    if (q.posActive.w < 0.5)
        return; // inactive slot: leave the last result in place
    const uint queryTeam = min(uint(q.posActive.w + 0.5) - 1u, MAX_FORCE_TEAMS - 1u);

    // ONE center accumulation serves every scalar output (the old forceSampleField, inlined so the
    // per-query-team opposing level shares it too).
    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(q.posActive.xyz, phi);
    uint bestTeam = 0u;
    float bestPhi = phi[0];
    for (uint t = 1u; t < MAX_FORCE_TEAMS; ++t)
        if (phi[t] > bestPhi) { bestPhi = phi[t]; bestTeam = t; }
    float secondPhi = 0.0;
    float opposing = 0.0; // vs the QUERY's team, not vs the strongest
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
    {
        if (t != bestTeam)
            secondPhi = max(secondPhi, phi[t]);
        if (t != queryTeam)
            opposing = max(opposing, phi[t]);
    }
    const float F = bestPhi - forceOpposingBound(u_forceParams0.x, secondPhi);

    // Opposing-field gradient: 4-tap tetrahedral finite differences (the forceSurfaceNormal
    // pattern), only where an opposing field exists at all. h is body-scale — queries ride units.
    vec3 grad = vec3(0.0);
    if (opposing > 1e-4)
    {
        const float h = 0.35;
        const vec2 k = vec2(1.0, -1.0);
        grad = (k.xyy * forceOpposingTo(q.posActive.xyz + k.xyy * h, queryTeam)
              + k.yyx * forceOpposingTo(q.posActive.xyz + k.yyx * h, queryTeam)
              + k.yxy * forceOpposingTo(q.posActive.xyz + k.yxy * h, queryTeam)
              + k.xxx * forceOpposingTo(q.posActive.xyz + k.xxx * h, queryTeam)) / (4.0 * h);
    }

    ForceQueryResult result;
    result.owningTeam = F > 0.0 ? bestTeam : MAX_FORCE_TEAMS;
    result.ownField = bestPhi;
    result.bestOpposingField = secondPhi;
    result.frameStamp = u_frameIndex;
    result.opposingGrad = vec4(grad, opposing);
    out_results[i] = result;
}
