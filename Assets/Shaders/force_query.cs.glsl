#version 460

// Gameplay point queries: one thread per registered query slot, evaluating the per-team fields at
// the query position ("which team's bubble, after deformation, contains this point?"). Results are
// written slot-indexed straight into the host-visible readback buffer, stamped with the frame index
// so the CPU can tell a live result from a never-evaluated slot; the CPU reads them ~2 frames later.
// (Field FORCE for shield-less bodies does not live here any more - that is the baked pressure
// field, force_bake.cs.glsl.)

layout (local_size_x = FORCE_SIM_GROUP_SIZE) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

// Matches RendererVKLayout::ForceQueriesGpu / ForceQueryResult.
struct ForceQueryData { vec4 posActive; }; // xyz = world position, w = 1 active / 0 inactive
struct ForceQueryResult
{
    uint owningTeam;        // MAX_FORCE_TEAMS = outside every bubble
    float ownField;
    float bestOpposingField;
    uint frameStamp;
};

layout (binding = 5, std430) readonly buffer Queries
{
    uint q_count; uint q_pad0; uint q_pad1; uint q_pad2;
    ForceQueryData q_queries[];
};
layout (binding = 6, std430) writeonly buffer OutResults { ForceQueryResult out_results[]; };

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= q_count)
        return;
    const ForceQueryData q = q_queries[i];
    if (q.posActive.w < 0.5)
        return; // inactive slot: leave the last result in place

    uint bestTeam;
    float bestPhi, secondPhi, F;
    forceSampleField(q.posActive.xyz, u_forceParams0.x, bestTeam, bestPhi, secondPhi, F);
    ForceQueryResult result;
    result.owningTeam = F > 0.0 ? bestTeam : MAX_FORCE_TEAMS;
    result.ownField = bestPhi;
    result.bestOpposingField = secondPhi;
    result.frameStamp = u_frameIndex;
    out_results[i] = result;
}
