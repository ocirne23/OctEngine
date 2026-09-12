#version 460

#extension GL_GOOGLE_include_directive : enable

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_curDepth;   // this frame's hardware depth (full-res gbuffer)
layout (binding = 2) uniform sampler2D u_prevDepth;  // previous frame's hardware depth
layout (binding = 3) uniform sampler2D u_rawAO;      // this frame's raw AO (rgb = bent normal, a = AO, half-res)
layout (binding = 4) uniform sampler2D u_historyAO;  // previous frame's accumulated AO (rgb = bent normal, a = AO)
layout (binding = 5, rgba16f) uniform restrict writeonly image2D u_accumOut;

layout (push_constant) uniform PC
{
    uint  aoWidth;
    uint  aoHeight;
    float maxHistory;   // blend weight for valid history (e.g. 0.92)
    uint  viewIndex;    // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
} pc;

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.aoWidth) || px.y >= int(pc.aoHeight))
        return;

    const vec2 uv = (vec2(px) + 0.5) / vec2(pc.aoWidth, pc.aoHeight);
    const vec4 raw = texture(u_rawAO, uv); // rgb = bent normal, a = AO
    const float depth = texture(u_curDepth, uv).r;
    if (depth <= 0.0) { imageStore(u_accumOut, px, raw); return; } // background (reversed-Z far = 0)

    // The depth image is jittered (exact depth-prepass reuse); compensate geometric uses - see taaJitterUv.
    const vec2 uvUnjit = uv - taaJitterUv(u_taaJitter.xy);
    const vec3 worldPos = worldPosFromDepth(uvUnjit, depth); // disocclusion test only

    float clipW;
    // Clip-space reprojection: precision independent of the camera's world position (see shared.inc.glsl).
    const vec2 prevUv = prevScreenUVClip(uvUnjit, depth, clipW);

    // Bilateral 2x2 history fetch: validate each bilinear tap against its own reprojected position
    // instead of fetching with plain bilinear after a single-point disocclusion test. At silhouettes the
    // history mixes foreground, far background, and sky texels; with sub-texel reprojection (camera
    // motion) a plain fetch bleeds those into the edge history and leaves a bright trailing rim.
    float histWeight = 0.0;
    vec4 hist = raw;
    if (clipW > 0.0 && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0))))
    {
        // Distance-scaled disocclusion threshold: tolerate more at range (depth precision falls off).
        const float viewDist = length(worldPos - u_viewPos);
        const float thresh = 0.02 + 0.01 * viewDist;

        const vec2 res   = vec2(pc.aoWidth, pc.aoHeight);
        const vec2 st    = prevUv * res - 0.5;
        const vec2 base  = (floor(st) + 0.5) / res;
        const vec2 f     = fract(st);
        const float bw[4] = float[]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
        const vec2 offs[4] = vec2[](vec2(0.0), vec2(1.0 / res.x, 0.0), vec2(0.0, 1.0 / res.y), 1.0 / res);

        vec4 sum = vec4(0.0);
        float wsum = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const vec2 tapUv = base + offs[i];
            // Last frame's depth is jittered by LAST frame's jitter: the surface at unjittered tapUv
            // lives at tapUv + prevJitter - fetch there, reconstruct at tapUv.
            const float prevDepth = texture(u_prevDepth, tapUv + taaJitterUv(u_taaJitter.zw)).r;
            if (prevDepth <= 0.0)
                continue;
            const vec3 prevWorld = worldPosFromDepthMat(tapUv, prevDepth, u_prevInvMvp);
            if (length(prevWorld - worldPos) >= thresh)
                continue;
            sum  += texture(u_historyAO, tapUv) * bw[i];
            wsum += bw[i];
        }
        if (wsum > 1e-4)
        {
            hist = sum / wsum;
            // Scale trust by surviving coverage so a mostly-disoccluded fetch leans on the fresh sample.
            histWeight = pc.maxHistory * wsum;
        }
    }

    vec4 accum = mix(raw, hist, histWeight); // AO in .a; bent normal in .xyz blends as a vector
    accum.xyz = (dot(accum.xyz, accum.xyz) > 1e-8) ? normalize(accum.xyz) : raw.xyz;
    imageStore(u_accumOut, px, accum);
}
