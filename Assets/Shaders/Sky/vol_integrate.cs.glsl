#version 450

// Volumetric fog integration: one invocation per froxel column, marching front to back along Z.
// Each slice's in-scatter (rgb, per meter) and extinction (a, 1/m) from vol_scatter.cs.glsl are
// integrated analytically over the slice's depth, accumulating transmittance-weighted in-scatter.
// Every slice stores the accumulated result so the apply pass can sample fog at any scene depth:
// rgb = total in-scattered radiance camera..slice, a = transmittance camera..slice.

#include "shared.inc.glsl"
#include "vol_fog.inc.glsl"

layout (binding = 1) uniform sampler3D u_scatter;
layout (binding = 2, rgba16f) uniform writeonly image3D u_outIntegrated;

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// 3x3 XY tent filter over the scatter grid (weights 1-2-1, separable). The scatter pass's jittered,
// few-ray lighting is noisy per froxel; one cheap spatial pass at froxel resolution removes the
// remaining blotchiness the temporal blend doesn't catch, at well below pixel frequency.
vec4 filteredScatter(ivec2 xy, int z)
{
    vec4 sum = vec4(0.0);
    float wSum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 p = clamp(xy + ivec2(dx, dy), ivec2(0), ivec2(VOL_FROXEL_X - 1, VOL_FROXEL_Y - 1));
            const float w = float((2 - abs(dx)) * (2 - abs(dy)));
            sum += texelFetch(u_scatter, ivec3(p, z), 0) * w;
            wSum += w;
        }
    }
    return sum / wSum;
}

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(VOL_FROXEL_X, VOL_FROXEL_Y))))
        return;

    const bool spatialFilter = u_fog_spatialFilter > 0.5;
    vec3 accum = vec3(0.0);
    float transmittance = 1.0;
    float zNear = VOL_FOG_NEAR;
    for (int z = 0; z < VOL_FROXEL_Z; ++z)
    {
        const vec4 s = spatialFilter ? filteredScatter(xy, z) : texelFetch(u_scatter, ivec3(xy, z), 0);
        const float zFar = volSliceToViewZ(float(z + 1) / float(VOL_FROXEL_Z));
        const float dz = zFar - zNear;
        zNear = zFar;

        const float extinction = max(s.a, 1e-5);
        const float sliceTrans = exp(-extinction * dz);
        // Analytic in-scatter over the slice (constant medium): S/ext * (1 - e^(-ext*dz)).
        accum += (s.rgb / extinction) * (1.0 - sliceTrans) * transmittance;
        transmittance *= sliceTrans;

        imageStore(u_outIntegrated, ivec3(xy, z), vec4(accum, transmittance));
    }
}
