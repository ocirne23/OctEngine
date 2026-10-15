// MOTION VECTORS, the fragment side (the value: shared.inc.glsl prevScreenUVMotion). The vertex shader of a surface that
// can move passes prevWorld - world of the shaded point (instanced_indirect.vs.glsl). A world OFFSET, not a clip
// position: exactly 0 on a surface that did not move, and small - so the camera's part stays in u_reprojClip (fused in
// double on the CPU, precise at any distance from the world origin) and the offset goes through prevMvp's LINEAR part only.
// Needs shared.inc.glsl first; fragment shaders only (gl_FragCoord).

#ifndef MOTION_VECTOR_INC_GLSL
#define MOTION_VECTOR_INC_GLSL

// This fragment's surface in LAST frame's clip space, divided by this frame's clip w (the prevScreenUVClip
// convention). gl_FragCoord is jittered: the surface at the pixel sits at uv - this frame's jitter.
// The camera's part alone (a surface that cannot move):
vec4 fragPrevClipCamera()
{
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
    return u_reprojClip * vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, gl_FragCoord.z, 1.0);
}

vec4 fragPrevClip(vec3 prevWorldDelta)
{
    return fragPrevClipCamera() + (u_prevMvp * vec4(prevWorldDelta, 0.0)) * gl_FragCoord.w;
}

// The motion target's value (fragment location 1). A point that did not move writes 0: its readers keep the
// camera-only reprojection, exact and without the fp16 rounding of a stored uv.
vec4 motionVector(vec3 prevWorldDelta)
{
    if (all(equal(prevWorldDelta, vec3(0.0))))
        return vec4(0.0);
    const vec4 prevClip = fragPrevClip(prevWorldDelta);
    if (prevClip.w <= 0.0)
        return vec4(0.0, 0.0, -1.0, 1.0); // behind last frame's camera: no history
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    return vec4(uv - prevClipToScreenUV(prevClip), prevClip.z / prevClip.w, 1.0);
}

// The same, flagged NO-AO (shared.inc.glsl motionNoAo): the surfaces in the depth image that are not in the TLAS and read
// no RTAO (the grass, the clutter). A trace from their depth is noise that bleeds into the ground around them.
vec4 motionVectorNoAo(vec3 prevWorldDelta)
{
    vec4 m = motionVector(prevWorldDelta);
    m.w = m.w > 0.5 ? 2.0 : -1.0;
    return m;
}

#endif
