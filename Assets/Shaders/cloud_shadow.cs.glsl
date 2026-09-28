#version 460

// Cloud Beer shadow map: one invocation per texel of one cascade (push constant). The texel's line along the
// sun direction L starts where it leaves the shell top toward the sun and marches down through the shell
// (both intervals; the ground ends it) with a fixed step count. Writes the front's along-light coordinate,
// the mean extinction between the first and the last cloud, and the whole optical depth - the layout and
// the lookup are in cloud_shadow.inc.glsl.
//
// PROGRESSIVE (Renderer::buildUboClouds): a frame renders one texel of every 2x2 (split 1), 4x4 (split 2) or 8x8
// (split 3) block - the phase's - one thread per rendered texel. The phases rotate in an order that spreads each
// frame's texels evenly (the 2x2 order (0,0) (1,1) (1,0) (0,1), applied at every level of the block).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"
#define CLOUD_NOISE_BINDING 2
#include "clouds.inc.glsl"

layout (binding = 1, rgba32f) uniform writeonly image2DArray u_outShadow;

layout (push_constant) uniform CloudShadowPC
{
    uint u_cascade;
    uint u_resolution;
    uint u_split; // 0 = every texel, 1 = one of each 2x2, 2 = one of each 4x4, 3 = one of each 8x8
    uint u_phase;
};

ivec2 phaseOffset2(uint p) { return ivec2(int((p & 1u) ^ ((p >> 1u) & 1u)), int(p & 1u)); }

ivec2 shadowTexel()
{
    const ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (u_split == 0u)
        return id;
    if (u_split == 1u)
        return id * 2 + phaseOffset2(u_phase);
    if (u_split == 2u)
        return id * 4 + phaseOffset2(u_phase & 3u) * 2 + phaseOffset2(u_phase >> 2u);
    return id * 8 + phaseOffset2(u_phase & 3u) * 4 + phaseOffset2((u_phase >> 2u) & 3u) * 2 + phaseOffset2(u_phase >> 4u);
}

void main()
{
    const ivec2 px = shadowTexel();
    if (px.x >= int(u_resolution) || px.y >= int(u_resolution))
        return;
    const ivec3 dst = ivec3(px, int(u_cascade));
    const vec4 noCloud = vec4(-1e30, 0.0, 0.0, 0.0);

    const vec4 cascade = u_cascade == 0u ? u_cloudShadow0 : u_cloudShadow1;
    const vec3 L = u_sunDirection;
    const float texel = 1.0 / (cascade.w * float(u_resolution));
    const vec2 local = (vec2(px) + 0.5 - 0.5 * float(u_resolution)) * texel;
    const vec3 origin = cascade.xyz + u_cloudShadow2.xyz * local.x + u_cloudShadow3.xyz * local.y; // on the plane through the centre
    const float camAlt = u_viewPos.y; // centre view (g_viewIndex 0)

    // Start where the line leaves the shell top toward the sun (at the plane when the plane is above it).
    const vec2 tTop = cloudRaySphere(cloudAltitude(origin, camAlt), cloudRayB(origin, L, camAlt), u_cloudShape0.y);
    const float aStart = tTop.y > tTop.x ? max(tTop.y, 0.0) : 0.0;
    const vec3 start = origin + L * aStart;
    vec2 seg0, seg1;
    cloudShellIntervals(cloudAltitude(start, camAlt), cloudRayB(start, -L, camAlt), 1e7, seg0, seg1);
    const float len0 = max(seg0.y - seg0.x, 0.0);
    const float len1 = max(seg1.y - seg1.x, 0.0);
    const float total = len0 + len1;
    if (total <= 0.0)
    {
        imageStore(u_outShadow, dst, noCloud);
        return;
    }

    const int steps = max(int(u_cascade == 0u ? u_cloudShadow4.y : u_cloudShadow4.z), 1); // per cascade: the far one updates less often
    const float dt = total / float(steps);
    const vec2 noiseOffset = cloudNoiseOffset();
    const float lodBase = max(log2(texel * u_cloudShape1.y * CLOUD_BASE_RES), 0.0);
    const float lodDetail = max(log2(texel * u_cloudShape1.z * CLOUD_DETAIL_RES), 0.0);
    float od = 0.0;
    float aFront = -1e30;
    float aBack = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const float u = (float(i) + 0.5) * dt;
        const float t = u < len0 ? seg0.x + u : seg1.x + (u - len0);
        const vec3 p = start - L * t;
        // The detail erosion in BOTH cascades: it removes much of the base density, so a base-only far
        // cascade shadows far darker and its border shows as a box. At the far texel size the detail's mip
        // is close to its mean, which is what the far cascade needs.
        const float dens = cloudDensity(p.xz + noiseOffset, cloudAltitude(p, camAlt), 1e30, 1.0, lodBase, lodDetail);
        if (dens <= 0.0)
            continue;
        od += dens * u_cloudShape1.w * dt;
        const float a = aStart - t; // = dot(p - centre, L): origin - centre is perpendicular to L
        if (aFront == -1e30)
            aFront = a + 0.5 * dt;
        aBack = a - 0.5 * dt;
    }
    if (od <= 0.0)
    {
        imageStore(u_outShadow, dst, noCloud);
        return;
    }
    imageStore(u_outShadow, dst, vec4(aFront, od / max(aFront - aBack, 1.0), od, 0.0));
}
