#version 460

// Volumetric cloud march: one invocation per HALF-RES pixel, every pixel every frame (a fly-through has
// parallax at every depth, so an amortized 1-in-N update would smear). Marches the view ray through the
// cloud shell (clouds.inc.glsl; the march itself is cloud_raymarch.inc.glsl) up to the farthest scene
// surface of the pixel's 2x2 block, and writes
//   out color = (in-scatter incl. the aerial perspective in front of the cloud, transmittance)
//   out depth = log2 distances: x = first cloud hit, y = transmittance-weighted cloud distance (the
//               temporal reprojection), z = the march limit (the scene surface / max distance), w = steps
// cloud_temporal.cs.glsl accumulates it; cloud_apply.fs.glsl / vol_apply.fs.glsl upsample it over the scene.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"
#define CLOUD_NOISE_BINDING 5
#include "clouds.inc.glsl"
#define CLOUD_SHADOW_BINDING 9
#include "cloud_shadow.inc.glsl"

layout (binding = 1) uniform sampler2D u_depth;
layout (binding = 2) uniform sampler2DArray u_skyMap; // GI's per-frame sky bake (atmosphere.inc.glsl)
layout (binding = 3, rgba16f) uniform writeonly image2D u_outColor;
layout (binding = 4, rgba16f) uniform writeonly image2D u_outDepth;

#include "cloud_raymarch.inc.glsl"

layout (push_constant) uniform CloudPC
{
    uint u_viewIndex;
    uint u_width;
    uint u_height;
    uint u_pad;
};

float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

vec3 stepHeat(float x)
{
    return clamp(vec3(x * 3.0 - 1.0, x < 0.5 ? x * 2.0 : 2.0 - x * 2.0, 1.0 - x * 3.0), 0.0, 1.0);
}

void writeEmpty(ivec2 px, float logLimit)
{
    imageStore(u_outColor, px, vec4(0.0, 0.0, 0.0, 1.0));
    imageStore(u_outDepth, px, vec4(logLimit, logLimit, logLimit, 0.0));
}

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(u_width) || px.y >= int(u_height))
        return;
    g_viewIndex = int(u_viewIndex);

    const float maxDist = u_cloudMarch0.y;
    const float logMax = log2(maxDist);
    const ivec2 full = px * 2;
    const vec2 uv = (vec2(full) + 1.0) * u_screenSize.zw; // the 2x2 block's centre, full-target UV
    const vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    if (u_cloudShape0.w < 0.5 || any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
    {
        writeEmpty(px, logMax);
        return;
    }

    // The FARTHEST surface of the block (reversed-Z: the smallest depth, 0 = sky): the march reaches every
    // full-res pixel's surface, and the apply drops the clouds a nearer pixel has in front of it.
    const ivec2 last = textureSize(u_depth, 0) - 1;
    const float depth = min(min(texelFetch(u_depth, min(full, last), 0).r, texelFetch(u_depth, min(full + ivec2(1, 0), last), 0).r),
                            min(texelFetch(u_depth, min(full + ivec2(0, 1), last), 0).r, texelFetch(u_depth, min(full + ivec2(1, 1), last), 0).r));
    const vec2 uvJ = uv - taaJitterUv(u_taaJitter.xy);
    const float limit = depth > 0.0 ? min(length(viewRelFromDepth(uvJ, depth)), maxDist) : maxDist;
    const float logLimit = log2(max(limit, 1.0));
    const vec3 dir = normalize(viewRelFromDepth(uvJ, 1.0));
    // Angular size of one half-res pixel: the mip level from the footprint at each distance.
    const float pixelAngle = length(normalize(viewRelFromDepth(uvJ + vec2(2.0 * u_screenSize.z, 0.0), 1.0)) - dir);

    const float camAlt = u_viewPos.y;
    vec2 seg0, seg1;
    cloudShellIntervals(camAlt, cloudRayB(vec3(0.0), dir, camAlt), limit, seg0, seg1);
    if (seg0.y <= seg0.x && seg1.y <= seg1.x)
    {
        writeEmpty(px, logLimit);
        return;
    }

    const int maxSteps = int(u_cloudMarch0.x);
    const float jitter = ign(vec2(px) + float(u_frameIndex % 64u) * 5.588238);
    const CloudMarchResult r = cloudRaymarch(vec3(0.0), dir, seg0, seg1, maxSteps, jitter, pixelAngle, u_cloudMarch1.w == 2.0);
    if (r.front < 0.0)
    {
        writeEmpty(px, logLimit);
        return;
    }

    vec4 color = vec4(r.inScatter, r.transmittance);
    if (u_cloudMarch1.w == 1.0)
        color = vec4(stepHeat(float(r.steps) / float(maxSteps)), 0.0);
    imageStore(u_outColor, px, color);
    imageStore(u_outDepth, px, vec4(log2(max(r.front, 1.0)), log2(max(r.weighted, 1.0)), logLimit, float(r.steps)));
}
