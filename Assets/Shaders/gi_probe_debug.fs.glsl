#version 450

// GI probe debug visualization fragment. The quad is a sphere impostor: intersect the view ray with the
// sphere and write the hit's depth, so the spheres sort against the scene and each other like real geometry.
// Irradiance mode evaluates the probe's SH-L1 irradiance along the TRUE sphere normal per pixel, scaled by
// "GI/Strength" like the scene's own lookup, and unshaded - the ball reads as the probe's directional
// lighting. The flat-colour modes get a simple directional shade off the sphere normal for depth perception.

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

#define GI_GRID_DATA_NAME  gi_gridData
#include "gi_probe.inc.glsl"

layout (location = 0) flat in vec3 v_color;
layout (location = 1) in vec3 v_world;
layout (location = 2) flat in vec4 v_sphere;
layout (location = 3) flat in uint v_cellBase;
layout (location = 4) flat in uint v_mode;

layout (location = 0) out vec4 out_color;

void main()
{
    const vec3  dir  = normalize(v_world - u_viewPos);
    const vec3  oc   = u_viewPos - v_sphere.xyz;
    const float b    = dot(oc, dir);
    const float disc = b * b - (dot(oc, oc) - v_sphere.w * v_sphere.w);
    if (disc < 0.0)
        discard;
    const vec3 hit = u_viewPos + dir * (-b - sqrt(disc));
    const vec3 n   = (hit - v_sphere.xyz) / v_sphere.w;

    if (v_mode == 0u)
        out_color = vec4(giEvalCell(v_cellBase, n) / PI * u_aoParams.y, 1.0); // u_aoParams.y = GI strength (0 while GI is off)
    else
        out_color = vec4(v_color * (max(dot(n, normalize(vec3(0.4, 0.8, 0.5))), 0.0) * 0.7 + 0.3), 1.0);

    const vec4 clip = u_mvp * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
