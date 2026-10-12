#version 450

// GI probe debug visualization fragment. The quad is a sphere impostor: intersect the view ray with the sphere and write
// the hit's depth, so the spheres sort against the scene like real geometry. Irradiance mode evaluates the probe's SH
// along the TRUE sphere normal, scaled by "GI/Strength" like the scene's own lookup, unshaded. Visibility mode evaluates
// the depth moments the same way. The flat-colour modes get a directional shade off the normal for depth perception.

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
        out_color = vec4(giEvalCell(v_cellBase, n) / PI * u_rt_giStrength, 1.0); // u_rt_giStrength: 0 while GI is off
    else if (v_mode == 4u)
    {
        // Visibility: what giSampleCascade's Chebyshev test sees for a surface in direction n FROM the probe. Grey =
        // mean distance over the cap (black = an occluder at the probe, white = open to the cap); ORANGE tint = the
        // deviation above the variance floor, i.e. how soft the occlusion edge is there. MAGENTA = no depth data yet.
        vec4 dsh, d2sh;
        giReadDepthSH(v_cellBase, dsh, d2sh);
        const float cap = GI_DEPTH_CAP_SPACING * v_color.x;
        float mean, variance;
        giVisMoments(dsh, d2sh, n, int(v_color.x), mean, variance);
        vec3 col = vec3(1.0, 0.0, 1.0);
        if (d2sh.x > 1e-4) // the DC term: > 0 as soon as the probe has traced (see giVisibilityWeight)
        {
            const float minDev = u_rt_giVisVarianceFloor * v_color.x;
            // The tint MULTIPLIES the grey (black stays black). ORANGE, not blue: blue carries ~7% of the luminance, so
            // a blue tint reads as plain darkening. Linear to cap / 2, the deviation's maximum (half the rays at 0, half
            // at the cap).
            const float t = clamp((sqrt(variance) - minDev) / (0.5 * cap), 0.0, 1.0);
            col = (mean / cap) * mix(vec3(1.0), vec3(1.0, 0.4, 0.0), t);
        }
        out_color = vec4(col * v_color.y, 1.0);
    }
    else
        out_color = vec4(v_color * (max(dot(n, normalize(vec3(0.4, 0.8, 0.5))), 0.0) * 0.7 + 0.3), 1.0);

    const vec4 clip = u_mvp * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
