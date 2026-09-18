#version 450

// GI probe debug visualization: instanced cubes, one per live probe cell. Instance index maps to a
// (grid, cell) via the probe work list; the cube is placed at the cell center and sized with the cell.
// Color = directional irradiance (mode 0), cellSize/LOD band (mode 1), update priority (mode 2) or
// relocation / backface state (mode 3). Invalid instances collapse
// off-screen. Procedural geometry (no vertex buffers): 36 verts = 12 triangles of a unit cube.

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

layout (push_constant) uniform PC
{
    float  u_radius; // cube radius as a fraction of probe spacing
    uint   u_mode;   // 0 = irradiance, 1 = cascade/LOD color, 2 = update priority, 3 = relocation / backface state
} pc;

#define GI_GRID_DATA_NAME  gi_gridData
#include "gi_probe.inc.glsl"

layout (location = 0) out vec3 v_color;
layout (location = 1) out vec3 v_normal;

const vec3 CORNERS[8] = vec3[](
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5),
    vec3(-0.5, -0.5,  0.5), vec3(0.5, -0.5,  0.5), vec3(0.5, 0.5,  0.5), vec3(-0.5, 0.5,  0.5));
const int IDX[36] = int[](
    0,1,2, 0,2,3,   4,5,6, 4,6,7,   0,4,5, 0,5,1,
    2,6,7, 2,7,3,   0,3,7, 0,7,4,   1,5,6, 1,6,2);

void main()
{
    uint inst = uint(gl_InstanceIndex);
    int  cascade = int(inst / uint(GI_CASCADE_PROBES));
    uint local   = inst - uint(cascade) * uint(GI_CASCADE_PROBES);
    uint DX      = uint(GI_PROBE_DIM_X), DY = uint(GI_PROBE_DIM_Y);
    ivec3 oc     = ivec3(int(local % DX), int((local / DX) % DY), int(local / (DX * DY)));

    int   spacing = giCascadeSpacing(cascade);
    ivec3 lc      = giCascadeOrigin(cascade, u_sceneFocus.xyz) + oc;
    uint  cellBase = giProbeBase(cascade, lc);
    vec3  center  = vec3(lc) * float(spacing) + giProbeOffset(cellBase);

    vec3 corner = CORNERS[IDX[gl_VertexIndex]];
    vec3 world  = center + corner * (pc.u_radius * sqrt(float(spacing)));
    gl_Position = u_mvp * vec4(world, 1.0);

    v_normal = normalize(corner);
    if (pc.u_mode == 3u)
    {
        // Relocation / backface state (the misc vec4): red = how far the lookup has faded the probe out as
        // backface-dead, blue = relocation offset as a fraction of its clamp, YELLOW = escaped on its last
        // visit (the trace pins the stored fraction to exactly DEAD_MAX there). A probe that keeps
        // returning to yellow is escaping again and again - its lookup weight and its irradiance jump at
        // the visit rate.
        const vec4  misc   = gi_gridData[cellBase + GI_MISC_V4];
        const float dead   = smoothstep(GI_BACKFACE_DEAD_MIN, GI_BACKFACE_DEAD_MAX, misc.x);
        const float offset = clamp(length(misc.yzw) / (0.45 * float(spacing)), 0.0, 1.0);
        v_color = vec3(0.12) + vec3(dead, 0.0, offset);
        if (abs(misc.x - GI_BACKFACE_DEAD_MAX) < 1e-5)
            v_color = vec3(1.0, 1.0, 0.0);
    }
    else if (pc.u_mode == 2u)
    {
        // Update rate: the wave's ACTUAL interval in frames (giWaveUpdateInterval, what the trace uses).
        // MAGENTA = every frame (the maximum rate) - a HUE the ramp never produces, not white: these cubes
        // go through the scene's exposure and bloom, where the ramp's bright yellow can clip to white.
        // The rest is a LOG ramp, each doubling an equal step: blue (2 frames) -> green (~22) ->
        // yellow (~76) -> red (256 or more). Backface-dead probes (GI_DEAD_INTERVAL on top) are dimmed.
        const uint interval = giWaveUpdateInterval(giWaveMin(lc, giCascadeOrigin(cascade, u_sceneFocus.xyz)), spacing);
        if (interval <= 1u)
            v_color = vec3(1.0, 0.0, 1.0);
        else
        {
            const float t = clamp((log2(float(interval)) - 1.0) / 7.0, 0.0, 1.0);
            v_color = t < 0.5 ? mix(vec3(0.05, 0.25, 1.0), vec3(0.05, 1.0, 0.1), t * 2.0)
                              : vec3(min((t - 0.5) * 4.0, 1.0), min((1.0 - t) * 4.0, 1.0), 0.05);
        }
        if (giProbeBackfaceFrac(cellBase) > GI_BACKFACE_DEAD_MAX)
            v_color *= 0.2;
    }
    else if (pc.u_mode == 1u)
    {
        if      (cascade == 0) v_color = vec3(1.0, 0.2, 0.2);
        else if (cascade == 1) v_color = vec3(0.2, 1.0, 0.2);
        else if (cascade == 2) v_color = vec3(0.3, 0.5, 1.0);
        else                   v_color = vec3(1.0, 1.0, 0.2);
    }
    else
    {
        v_color = giEvalCell(cellBase, v_normal) / PI;
    }
}
