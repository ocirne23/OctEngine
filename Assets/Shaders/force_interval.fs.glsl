#version 460

// The ANALYTIC tier's interval rasterization: each small-emitter proxy box (same VS as the shell
// draw) writes its ray interval as (tEntry, -tExit) with MIN blending into the RG16F interval
// target — per pixel the target then holds the UNION interval (min entry, -min(-exit) = max exit)
// of every analytic shell covering it, and force_union.fs.glsl marches that ONCE. No marching
// here: just the ray-box intersection the shell FS also starts with. The scene-depth clamp happens
// once in the union pass, not per proxy.

#include "shared.inc.glsl"
#include "force_field.inc.glsl" // declares the emitter buffer at FORCE_EMITTERS_BINDING (1)

#ifndef FORCE_UNION_UV_SCALE
#define FORCE_UNION_UV_SCALE 1.0 // injected 2.0 when the union march runs at half res
#endif

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) in flat uint v_emitterIdx;

layout (location = 0) out vec2 out_interval; // (tEntry, -tExit), MIN-blended

void main()
{
    g_viewIndex = int(u_viewIndex);
    const ForceEmitterData e = fe_emitters[v_emitterIdx];

    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw * FORCE_UNION_UV_SCALE; // see force_union.fs
    const vec3 rayOrigin = u_viewPos;
    const vec3 rayDir = normalize(worldPosFromDepth(uv, 0.0) - rayOrigin);

    float side, forward, back;
    forceVisibleBounds(e, side, forward, back); // matches the VS: the shrunk visible-extent box
    const mat3 basis = forceEmitterBasis(e.dirFocus.xyz);
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * (forward - back) * 0.5;
    const vec3 halfExtents = vec3(side, side, (forward + back) * 0.5);
    const vec3 localOrigin = transpose(basis) * (rayOrigin - center);
    const vec3 localDir = transpose(basis) * rayDir;
    const vec3 invDir = 1.0 / (localDir + vec3(equal(localDir, vec3(0.0))) * 1e-8);
    const vec3 tA = (-halfExtents - localOrigin) * invDir;
    const vec3 tB = ( halfExtents - localOrigin) * invDir;
    float t0 = max(max(min(tA.x, tB.x), min(tA.y, tB.y)), min(tA.z, tB.z));
    float t1 = min(min(max(tA.x, tB.x), max(tA.y, tB.y)), max(tA.z, tB.z));
    t0 = max(t0, 0.0);
    if (t1 <= t0)
        discard;
    out_interval = vec2(t0, -t1);
}
