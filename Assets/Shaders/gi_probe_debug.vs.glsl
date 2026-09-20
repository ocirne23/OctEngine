#version 450

// GI probe debug visualization: one instance per clipmap probe, placed at the probe (relocation offset
// included) and sized with the cascade. Procedural geometry (no vertex buffers).
// Every probe is a SPHERE IMPOSTOR: 6 verts = a camera-facing quad, and the fragment shader intersects the
// sphere. Mode 0 (irradiance) evaluates the probe's SH per pixel along the true normal there; modes 1
// (cascade / LOD), 2 (update priority) and 3 (relocation / backface state) pass a flat colour, which the
// fragment shader shades by the sphere normal for depth perception. Mode 4 (visibility) is per pixel too.

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

layout (push_constant) uniform PC
{
    float  u_radius; // sphere diameter scale (x sqrt(spacing))
    uint   u_mode;   // 0 = irradiance, 1 = cascade/LOD color, 2 = update priority, 3 = relocation / backface state, 4 = visibility
} pc;

#define GI_GRID_DATA_NAME  gi_gridData
#include "gi_probe.inc.glsl"

layout (location = 0) flat out vec3 v_color;       // flat-colour modes: the probe's colour
layout (location = 1) out vec3 v_world;            // the world point on the impostor quad
layout (location = 2) flat out vec4 v_sphere;      // xyz = centre, w = radius
layout (location = 3) flat out uint v_cellBase;    // irradiance mode: the probe the fragment shader evaluates
layout (location = 4) flat out uint v_mode;

const vec2 QUAD[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

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

    // Sphere impostor: a quad through the centre, facing the camera. The silhouette of a sphere seen from
    // distance d is wider than its radius at the centre plane - r * d / sqrt(d^2 - r^2) - so the quad takes
    // that size and the fragment shader discards what misses. A camera inside (or almost inside) the sphere
    // has no silhouette: collapse the instance.
    const float r     = 0.5 * pc.u_radius * sqrt(float(spacing));
    const vec3  toCam = u_viewPos - center;
    const float d     = length(toCam);
    v_cellBase = cellBase;
    v_mode     = pc.u_mode;
    v_sphere   = vec4(center, r);
    v_color    = vec3(0.0);
    v_world    = center;
    if (d < r * 1.05)
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the clip volume
        return;
    }
    const vec3 fwd   = toCam / d;
    const vec3 right = normalize(cross(abs(fwd.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), fwd));
    const vec3 up    = cross(fwd, right);
    const vec2 q     = QUAD[gl_VertexIndex] * (r * d / sqrt(d * d - r * r));
    v_world     = center + right * q.x + up * q.y;
    gl_Position = u_mvp * vec4(v_world, 1.0);

    if (pc.u_mode == 4u)
    {
        // Visibility: evaluated per pixel by the fragment shader. x = spacing (the depth cap's unit),
        // y = brightness (backface-dead probes, which the lookup rejects, are dimmed).
        v_color = vec3(float(spacing), giProbeBackfaceFrac(cellBase) > GI_BACKFACE_DEAD_MAX ? 0.2 : 1.0, 0.0);
    }
    else if (pc.u_mode == 3u)
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
        // A COVERED wave (giWaveCovered) shows through its interval: a slow block in the fast centre.
        const uint interval = giWaveUpdateInterval(cascade, giWaveMin(lc, giCascadeOrigin(cascade, u_sceneFocus.xyz)), spacing);
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
}
