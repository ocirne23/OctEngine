#version 460

#extension GL_GOOGLE_include_directive : enable

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_depth;   // full-res hardware depth
layout (binding = 3) uniform sampler2D u_accumAO; // temporally-accumulated AO (rgb = bent normal, a = AO, half-res)
layout (binding = 4, rgba16f) uniform restrict writeonly image2D u_aoOut;

layout (push_constant) uniform PC
{
    uint  aoWidth;
    uint  aoHeight;
    uint  viewIndex;  // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
} pc;

// The kernel half-extent (texels): a constant while "Ray tracing" is locked, so the loops unroll.
#define AO_BLUR_RADIUS int(u_rt_aoBlurRadius)

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.aoWidth) || px.y >= int(pc.aoHeight))
        return;

    const vec2 texel = 1.0 / vec2(pc.aoWidth, pc.aoHeight);
    const vec2 uv = (vec2(px) + 0.5) * texel;

    const float depth = texture(u_depth, uv).r;
    const vec4 centerAO = texture(u_accumAO, uv);
    // A NO-AO hole (a < 0: grass, clutter - rtao.cs.glsl) has no AO of its own and its depth-derived normal is noise:
    // filled like the background, from the traced surfaces around, so the ground between the blades takes the ground's AO.
    if (depth <= 0.0 || centerAO.a < 0.0) // background (reversed-Z: far/cleared = 0), or a hole
    {
        // Dilate geometry AO into background texels: the forward pass upsamples this image with plain bilinear, so the
        // trace's background value (bentN = +Z, ao = 1) would bleed into silhouette pixels as a bright rim.
        vec4 sum = vec4(0.0);
        float wsum = 0.0;
        for (int dy = -AO_BLUR_RADIUS; dy <= AO_BLUR_RADIUS; ++dy)
        for (int dx = -AO_BLUR_RADIUS; dx <= AO_BLUR_RADIUS; ++dx)
        {
            const vec2 nuv = uv + vec2(dx, dy) * texel;
            if (texture(u_depth, nuv).r <= 0.0) continue;
            const vec4 tap = texture(u_accumAO, nuv);
            if (tap.a < 0.0) continue;
            const float ws = exp(-float(dx * dx + dy * dy) / 8.0);
            sum  += tap * ws;
            wsum += ws;
        }

        vec4 bg;
        if (wsum > 1e-4)
        {
            bg = sum / wsum;
            bg.xyz = (dot(bg.xyz, bg.xyz) > 1e-8) ? normalize(bg.xyz) : vec3(0.0);
        }
        else
            bg = vec4(0.0, 0.0, 0.0, 1.0); // nothing traced nearby: no occlusion, no bent normal
        imageStore(u_aoOut, px, bg);
        return;
    }

    // The crease edge-stop is the neighbour's distance off the CENTRE's tangent plane: one depth-derived normal per pixel
    // (there is no normal target). Camera-relative positions throughout - see viewRelFromDepth.
    const vec3 centerPos = viewRelFromDepth(uv, depth);
    const vec3 centerN   = normalFromDepth(u_depth, ivec2(uv * vec2(textureSize(u_depth, 0))), depth, u_taaJitter.xy);
    const float viewDist = length(centerPos);
    const float sigmaZ   = max(0.05 * viewDist, 0.02);
    // One AO texel's world footprint: a wall k texels past a crease sits ~k footprints off the plane.
    const float sigmaP   = max(length(viewRelFromDepth(uv + texel, depth) - centerPos), 1e-4);

    vec4 sum = vec4(0.0); // .a = AO, .xyz = bent normal
    float wsum = 0.0;
    for (int dy = -AO_BLUR_RADIUS; dy <= AO_BLUR_RADIUS; ++dy)
    for (int dx = -AO_BLUR_RADIUS; dx <= AO_BLUR_RADIUS; ++dx)
    {
        const vec2 nuv = uv + vec2(dx, dy) * texel;
        const float nDepth = texture(u_depth, nuv).r;
        if (nDepth <= 0.0) continue;
        const vec4 tap = texture(u_accumAO, nuv);
        if (tap.a < 0.0) continue; // a NO-AO hole

        const vec3 dPos = viewRelFromDepth(nuv, nDepth) - centerPos;
        const float dp = dot(dPos, centerN);
        const float wz = exp(-dot(dPos, dPos) / (2.0 * sigmaZ * sigmaZ));
        const float wn = exp(-(dp * dp) / (2.0 * sigmaP * sigmaP));
        const float ws = exp(-float(dx * dx + dy * dy) / 8.0);
        const float w  = wz * wn * ws;

        sum  += tap * w;
        wsum += w;
    }

    vec4 ao = (wsum > 1e-4) ? sum / wsum : centerAO;
    ao.xyz = (dot(ao.xyz, ao.xyz) > 1e-8) ? normalize(ao.xyz) : vec3(0.0);
    imageStore(u_aoOut, px, ao);
}
