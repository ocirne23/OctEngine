// MOTION VECTORS, the fragment side (the scene's motion target: see shared.inc.glsl prevScreenUVMotion for
// the value, SceneColor for the attachment). The vertex shader of a surface that can move passes the
// WORLD offset of the shaded point from where it was last frame (prevWorld - world: the instance's previous
// transform, a skinned vertex's previous position; instanced_indirect.vs.glsl). A world offset, not a
// clip position: exactly 0 on a surface that did not move, and small - so the camera's part stays in
// u_reprojClip (current NDC + depth -> last frame's clip, fused in double precision on the CPU), whose
// precision does not depend on the distance from the world origin, and the object's part goes through
// prevMvp's LINEAR part only (no translation).
// Needs shared.inc.glsl (the UBO) first; fragment shaders only (gl_FragCoord).
#ifndef MOTION_VECTOR_INC_GLSL
#define MOTION_VECTOR_INC_GLSL

// This fragment's surface in LAST frame's clip space, divided by this frame's clip w (the prevScreenUVClip
// convention). gl_FragCoord is jittered: the surface at the pixel sits at uv - this frame's jitter.
// The camera's part alone (a surface that cannot move):
vec4 fragPrevClipCamera()
{
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    const vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
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

#endif
