#ifndef SHARED_CONSTANTS_INC_GLSL
#define SHARED_CONSTANTS_INC_GLSL

// ALPHA_MODE_* and MATERIAL_FLAG_* are injected by the engine from Layout.ixx. MATERIAL_FLAG_NO_RAYTRACING: debug / gizmo
// geometry that never blocks light - excluded from the TLAS (mask 0) and from the sun cascade caster cull.

const uint EMPTY_ENTRY        = 0xFFFFFFFFu;
const uint INITIALIZING_ENTRY = 0xEFFFFFFFu;

const float PI = 3.14159265359;
const float INV_PI = 0.31830988618; // multiply by this instead of dividing by PI (a division is not folded by every compiler)

#include "ubo.inc.glsl"
#include "mathutils.inc.glsl"

vec3 randomColor(uint seed)
{
    seed = hashU(seed);
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

// World position from a hardware depth and a full-frame screen uv (origin top-left). The scene renders through
// u_viewportRect (e.g. the editor viewport panel), so the uv is first remapped into the viewport; the viewport is
// y-flipped, so its v = 0 (top) is ndc.y = +1.
vec3 worldPosFromDepthMat(vec2 uv, float depth, mat4 invM)
{
    vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
    vec4 clip = vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0);
    vec4 world = invM * clip;
    return world.xyz / world.w;
}

vec3 worldPosFromDepth(vec2 uv, float depth) { return worldPosFromDepthMat(uv, depth, u_invMvp); }

// ALL raster passes apply the TAA sub-pixel jitter in clip space (there is no depth prepass), so the image content at
// pixel uv is the surface at uv - taaJitterUv(u_taaJitter.xy). Geometric readers of sampled depth (TAA, AO temporal,
// RTAO) subtract it; mvp / invMvp / prevMvp stay unjittered. Pass u_taaJitter.zw (LAST frame's) for the previous depth.
vec2 taaJitterUv(vec2 jitterNdc) { return vec2(jitterNdc.x, -jitterNdc.y) * 0.5 * u_viewportRect.zw; }

// CAMERA-RELATIVE position from depth (current view). worldPosFromDepth rounds at the scale of the world coordinate
// (~0.3 mm at 5 km from the origin): fine for a position, noise for a DIFFERENCE of neighbouring positions. With
// invMvp = T(eye) * R^-1 * P^-1 the eye term cancels analytically here.
vec3 viewRelFromDepth(vec2 uv, float depth)
{
    const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
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

// A world position -> its previous-frame full-frame screen uv (the inverse of worldPosFromDepthMat).
vec2 prevScreenUVMat(vec3 worldPos, mat4 prevM, out float clipW)
{
    vec4 p = prevM * vec4(worldPos, 1.0);
    clipW = p.w;
    vec3 ndc = p.xyz / p.w;
    vec2 vpUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return u_viewportRect.xy + vpUv * u_viewportRect.zw;
}

vec2 prevScreenUV(vec3 worldPos, out float clipW) { return prevScreenUVMat(worldPos, u_prevMvp, clipW); }

// Last frame's clip position -> its full-frame screen uv.
vec2 prevClipToScreenUV(vec4 prevClip)
{
    vec2 vpPrev = vec2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
    return u_viewportRect.xy + vpPrev * u_viewportRect.zw;
}

// A full-frame screen uv + hardware depth -> last frame's screen uv, all in clip space through u_reprojClip (fused in
// double on the CPU). Temporal passes must use this, not worldPosFromDepth + prevScreenUV: that world-space round trip
// loses precision with the distance from the world origin (temporal jitter). clipW is the previous clip w / the current
// one - only its sign is meaningful (> 0 = in front).
vec2 prevScreenUVClip(vec2 uv, float depth, out float clipW)
{
    vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
    vec4 prevClip = u_reprojClip * vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0);
    clipW = prevClip.w;
    return prevClipToScreenUV(prevClip);
}

// MOTION VECTORS - the scene's motion target (SceneColor::getMotionView; motion_vector.inc.glsl writes it): xy = this
// frame's minus last frame's UNJITTERED full-frame uv, z = the surface's hardware depth LAST frame (<= 0: behind last
// frame's camera), w = 1 where written. w = 0 = no object motion: the pixel reprojects through the camera alone.
// NO-AO pixels (the grass and the clutter: motionVectorNoAo) carry w = 2 (object motion) or -1 (camera alone): RTAO
// traces none of them, and fills them from the surfaces around.
bool motionIsObject(vec4 motion) { return motion.w > 0.5; }
bool motionNoAo(vec4 motion) { return motion.w < -0.5 || motion.w > 1.5; }

// Last frame's UNJITTERED uv of the surface at uvUnjit. valid = false: no history (behind last frame's camera).
vec2 prevScreenUVMotion(vec2 uvUnjit, float depth, vec4 motion, out bool valid)
{
    if (motionIsObject(motion))
    {
        valid = motion.z > 0.0;
        return uvUnjit - motion.xy;
    }

    float clipW;
    const vec2 prevUv = prevScreenUVClip(uvUnjit, depth, clipW);
    valid = clipW > 0.0;
    return prevUv;
}

// skyRadiance() for GI miss rays / fog ambient / surface fallback: the sky's own model, so indirect sky light follows
// the atmosphere settings.
#include "atmosphere.inc.glsl"

#endif
