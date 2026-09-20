#ifndef SHARED_CONSTANTS_INC_GLSL
#define SHARED_CONSTANTS_INC_GLSL

// ALPHA_MODE_* (RendererVKLayout::EAlphaMode) and MATERIAL_FLAG_* (RendererVKLayout::MATERIAL_FLAG_*)
// are injected by the engine from Layout.ixx.
// MATERIAL_FLAG_NO_RAYTRACING: debug/gizmo geometry that never blocks light - excluded from the TLAS
// (mask 0) and from the sun cascade caster cull.

const uint EMPTY_ENTRY        = 0xFFFFFFFFu;
const uint INITIALIZING_ENTRY = 0xEFFFFFFFu;

const float PI = 3.14159265359;
const float INV_PI = 0.31830988618; // multiply by this instead of dividing by PI (a division is not folded by every compiler)

#include "ubo.inc.glsl"

vec3 randomColor(uint seed) 
{
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    vec3 bits = vec3(float(seed & 255u), float((seed >> 8) & 255u), float((seed >> 16) & 255u));
    return bits / 255.0;
}
vec3 randomColor(ivec3 seed) 
{
    uvec3 u = uvec3(seed);
    uint hash = u.x * 1597334673u + u.y * 3812015801u + u.z * 2798796415u;
	return randomColor(hash);
}
vec3 cascadeDebugColor(int cascade)
{
	if (cascade == 0) return vec3(1.0, 0.0, 0.0);
	if (cascade == 1) return vec3(0.0, 1.0, 0.0);
	if (cascade == 2) return vec3(0.0, 0.0, 1.0);
    if (cascade == 3) return vec3(1.0, 0.0, 1.0);
    if (cascade == 4) return vec3(0.0, 1.0, 1.0);
	return vec3(1.0, 1.0, 0.0);
}

// Reconstruct world-space position from a hardware depth sample and a full-frame screen UV (origin top-left),
// given an inverse view-proj. The scene renders through u_viewportRect (a sub-rect of the render target, e.g.
// the editor viewport panel), so the full-frame UV is first remapped into viewport-local [0,1], then to NDC.
// The viewport is y-flipped (negative height), so viewport-local v=0 (top) maps to ndc.y=+1.
vec3 worldPosFromDepthMat(vec2 uv, float depth, mat4 invM)
{
    vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    vec4 clip = vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0);
    vec4 world = invM * clip;
    return world.xyz / world.w;
}
// Reconstruction for the current view (u_invMvp = u_views[g_viewIndex].invMvp). Shared passes leave
// g_viewIndex at VIEW_CENTER (centre view); per-eye passes set it to the eye they process.
vec3 worldPosFromDepth(vec2 uv, float depth) { return worldPosFromDepthMat(uv, depth, u_invMvp); }

// ALL raster passes apply the TAA sub-pixel jitter in clip space - the scene pass writes THE depth
// every screen-space pass reads (there is no prepass) - so image content at pixel uv is the surface at
// uv - taaJitterUv(u_taaJitter.xy). Geometric consumers of sampled depth (reprojection, world-pos
// reconstruction: TAA, AO temporal, RTAO) subtract it for exact positions - mvp/invMvp/prevMvp stay
// unjittered. Pass u_taaJitter.zw (LAST frame's jitter) when interpreting the previous depth image.
vec2 taaJitterUv(vec2 jitterNdc) { return vec2(jitterNdc.x, -jitterNdc.y) * 0.5 * u_viewportRect.zw; }

// CAMERA-RELATIVE position from depth (current view). worldPosFromDepth rounds at the scale of the
// world coordinate (~0.3 mm at 5 km from the origin), which is the size of a pixel footprint near the
// camera - fine for a position, noise for a DIFFERENCE of neighbouring positions. For a perspective
// projection invMvp = T(eye) * R^-1 * P^-1: columns 0/1 are pure directions, column 2 is eye * w, so
// the eye term cancels analytically and only the per-frame constant carries a (smooth) rounding bias.
vec3 viewRelFromDepth(vec2 uv, float depth)
{
    const vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    const mat4 invM = u_invMvp;
    const vec3 dir = invM[0].xyz * (vpUv.x * 2.0 - 1.0) + invM[1].xyz * (1.0 - vpUv.y * 2.0) + (invM[3].xyz - u_viewPos * invM[3].w);
    return dir / (invM[2].w * depth + invM[3].w);
}

// World-space GEOMETRIC normal from a hardware depth image (reversed-Z, jittered by jitterNdc), facing
// the camera. p = a texel of depthTex holding geometry (depth > 0). Each axis differences toward the
// neighbour whose depth is closer to the centre's, so a silhouette never bends the normal.
vec3 normalFromDepth(sampler2D depthTex, ivec2 p, float depth, vec2 jitterNdc)
{
    const ivec2 last = textureSize(depthTex, 0) - 1;
    const vec2 texel = 1.0 / vec2(last + 1);
    const vec2 uv = (vec2(p) + 0.5) * texel - taaJitterUv(jitterNdc);
    const float dl = texelFetch(depthTex, ivec2(max(p.x - 1, 0), p.y), 0).r;
    const float dr = texelFetch(depthTex, ivec2(min(p.x + 1, last.x), p.y), 0).r;
    const float du = texelFetch(depthTex, ivec2(p.x, max(p.y - 1, 0)), 0).r;
    const float dd = texelFetch(depthTex, ivec2(p.x, min(p.y + 1, last.y)), 0).r;
    const vec3 c = viewRelFromDepth(uv, depth);
    // A background neighbour (depth 0) that still wins the pick reads as the centre's depth.
    const vec3 dx = abs(dl - depth) < abs(dr - depth)
        ? c - viewRelFromDepth(uv - vec2(texel.x, 0.0), dl > 0.0 ? dl : depth)
        : viewRelFromDepth(uv + vec2(texel.x, 0.0), dr > 0.0 ? dr : depth) - c;
    const vec3 dy = abs(du - depth) < abs(dd - depth)
        ? c - viewRelFromDepth(uv - vec2(0.0, texel.y), du > 0.0 ? du : depth)
        : viewRelFromDepth(uv + vec2(0.0, texel.y), dd > 0.0 ? dd : depth) - c;
    const vec3 n = cross(dx, dy);
    const float len2 = dot(n, n);
    if (len2 < 1e-30)
        return normalize(-c);
    return n * (dot(n, c) > 0.0 ? -inversesqrt(len2) : inversesqrt(len2));
}

// Project a world position to a previous-frame full-frame screen UV (inverse of the mapping above): NDC ->
// viewport-local UV -> full-frame UV through u_viewportRect. Uses the current view's previous matrix.
vec2 prevScreenUVMat(vec3 worldPos, mat4 prevM, out float clipW)
{
    vec4 p = prevM * vec4(worldPos, 1.0);
    clipW = p.w;
    vec3 ndc = p.xyz / p.w;
    vec2 vpUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return u_viewportRect.xy + vpUv * u_viewportRect.zw;
}
vec2 prevScreenUV(vec3 worldPos, out float clipW) { return prevScreenUVMat(worldPos, u_prevMvp, clipW); }

// Reproject a full-frame screen UV + hardware depth to last frame's screen UV entirely in clip space via
// u_reprojClip (prevMvp * inverse(mvp), fused in double precision on the CPU). Temporal passes must use
// this instead of worldPosFromDepth + prevScreenUV: that world-space round trip loses precision with the
// camera's distance from the world origin (pixel-scale history misses by ~500 units = temporal jitter).
// clipW is the previous clip w scaled by 1/currentW - only its sign is meaningful (> 0 = in front).
vec2 prevScreenUVClip(vec2 uv, float depth, out float clipW)
{
    vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    vec4 prevClip = u_reprojClip * vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0);
    clipW = prevClip.w;
    vec2 vpPrev = vec2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
    return u_viewportRect.xy + vpPrev * u_viewportRect.zw;
}

// Atmosphere scattering + skyRadiance() for GI miss rays / fog ambient / surface fallback: the same
// Rayleigh+Mie model the sky renders with, so indirect sky light follows the atmosphere settings.
#include "atmosphere.inc.glsl"

#endif
