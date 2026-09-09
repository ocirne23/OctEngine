#version 450

// GI probe debug visualization: instanced cubes, one per live probe cell. Instance index maps to a
// (grid, cell) via the probe work list; the cube is placed at the cell center and sized with the cell.
// Color = directional irradiance (mode 0) or cellSize/LOD band (mode 1). Invalid instances collapse
// off-screen. Procedural geometry (no vertex buffers): 36 verts = 12 triangles of a unit cube.

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

layout (push_constant) uniform PC
{
    float  u_radius; // cube radius as a fraction of probe spacing
    uint   u_mode;   // 0 = irradiance, 1 = cascade/LOD color
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
    if (pc.u_mode == 1u)
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
