#version 460

// Cloud Beer shadow map: one invocation per texel of ONE TILE of one snapshot (RendererState::buildUboClouds: a
// cascade's map is rendered a tile per frame in its back layer and shown when it is complete). A texel is a point of
// a HORIZONTAL grid at the main layer's middle; its line runs through that point along the snapshot's sun direction,
// from where it leaves the main layer's top toward the sun down through the band (both intervals; the ground ends
// it) in a fixed step count. The layout and the lookup are in cloud_shadow.inc.glsl.
// THE SNAPSHOT'S OWN FRAME comes in the push block, not from the UBO (that holds the FRONT snapshot's lookup, and
// today's sun). The grid is fixed to the CLOUDS (noise space) and the push block says where it lies now, so tiles
// rendered many frames apart show the same cloud field.
// A TILE is a block of neighbouring texels, so a workgroup's threads agree: over clear sky all of them leave the
// density after its coverage test (most of the map at a low coverage), and none waits for a neighbour's noise fetches.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"
#define CLOUD_NOISE_BINDING 2
#include "clouds.inc.glsl"

layout (binding = 1, rgba32f) uniform writeonly image2DArray u_outShadow;

layout (push_constant) uniform CloudShadowPC
{
    vec4 u_centre; // xyz = the map's middle as it lies now, relative to the centre view's camera (m), w = the texel size (m)
    vec4 u_sun;    // xyz = the snapshot's light direction
    uint u_layer;
    uint u_cascade;
    uvec2 u_tile;  // the tile's first texel
};

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy + u_tile);
    const ivec3 dst = ivec3(px, int(u_layer));
    const vec4 noCloud = vec4(-1e30, 0.0, 0.0, 0.0);

    const vec3 L = u_sun.xyz;
    const float texel = u_centre.w;
    const vec2 local = (vec2(px) + 0.5 - 0.5 * float(imageSize(u_outShadow).x)) * texel;
    const vec3 origin = u_centre.xyz + vec3(local.x, 0.0, local.y); // the texel's point at the layer's middle
    const float camAlt = u_viewPos.y; // centre view (g_viewIndex 0)

    // THE MAIN LAYER ONLY: the upper layer (thin, low-density sheets) casts no map shadow, so all steps go to the main
    // band. Start where the line leaves the main band's top toward the sun (at the plane when the plane is above it).
    const vec2 tTop = cloudRaySphere(cloudAltitude(origin, camAlt), cloudRayB(origin, L, camAlt), cloudMainTop());
    const float aStart = tTop.y > tTop.x ? max(tTop.y, 0.0) : 0.0;
    const vec3 start = origin + L * aStart;
    vec2 seg0, seg1;
    cloudMainIntervals(cloudAltitude(start, camAlt), cloudRayB(start, -L, camAlt), 1e7, seg0, seg1);
    const float len0 = max(seg0.y - seg0.x, 0.0);
    const float len1 = max(seg1.y - seg1.x, 0.0);
    const float total = len0 + len1;
    if (total <= 0.0)
    {
        imageStore(u_outShadow, dst, noCloud);
        return;
    }

    // THE DETAIL: the near cascade fetches it (its texels show the lumps; the clouds' own self-shadow reads them). The
    // far cascade erodes by the detail's MEAN, no fetches: at its texel size the detail mip is near that mean. With no
    // erosion at all it shadows far darker, and its border shows as a box.
    const bool nearCascade = u_cascade == 0u;
    const int steps = int(nearCascade ? u_clouds_shadowNearSteps : u_clouds_shadowFarSteps);
    const float detail = nearCascade ? 1.0 : CLOUD_DETAIL_FETCH_MIN;
    const float dt = total / float(steps);
    const float odOpaque = 10.0 / u_clouds_extinction; // in density x metres: the march ends at a transmittance of e^-10
    const vec2 noiseOffset = cloudNoiseOffset();
    const float lodBase = max(log2(texel * u_clouds_baseFrequency * CLOUD_BASE_RES), 0.0);
    const float lodDetail = max(log2(texel * u_clouds_detailFrequency * CLOUD_DETAIL_RES), 0.0);
    const float seg1Shift = seg1.x - len0; // u past the first interval maps to seg1.x + (u - len0)

    float od = 0.0;
    float aFront = -1e30;
    float aBack = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const float u = (float(i) + 0.5) * dt;
        const float t = (u < len0 ? seg0.x : seg1Shift) + u;
        const vec3 p = start - L * t;
        const float dens = cloudMainDensity(p.xz + noiseOffset, cloudAltitude(p, camAlt), 1e30, detail, lodBase, lodDetail);
        if (dens <= 0.0)
            continue;
        od += dens * dt;
        const float a = aStart - t; // the distance along L from the texel's point: p = origin + L * a
        if (aFront == -1e30)
            aFront = a + 0.5 * dt;
        aBack = a - 0.5 * dt;
        if (od > odOpaque) // nothing below changes a receiver's light (a tile's threads end together in a cloud's core)
            break;
    }

    od *= u_clouds_extinction;
    if (od <= 0.0)
    {
        imageStore(u_outShadow, dst, noCloud);
        return;
    }
    imageStore(u_outShadow, dst, vec4(aFront, od / max(aFront - aBack, 1.0), od, 0.0));
}
