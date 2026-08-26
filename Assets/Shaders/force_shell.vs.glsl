#version 460

// Forcefield shell proxies: 36 vertices per instance (a unit cube, outward CCW winding), one
// instance per compacted live emitter, oriented and sized to the emitter's directional reach bounds
// (force_field.inc.glsl forceEmitterBounds — the FS intersects the same box for its march interval).
// Front faces are culled and the fixed-function depth test is off (the camera can be inside a
// bubble); the fragment shader ray-marches the analytic team field inside the box.

#include "shared.inc.glsl"
#include "force_field.inc.glsl" // declares the emitter buffer at FORCE_EMITTERS_BINDING (1)

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) out flat uint v_emitterIdx;

const uint c_cubeIndices[36] = uint[36](
    1u, 3u, 7u, 1u, 7u, 5u,  // +X
    0u, 6u, 2u, 0u, 4u, 6u,  // -X
    2u, 6u, 7u, 2u, 7u, 3u,  // +Y
    0u, 1u, 5u, 0u, 5u, 4u,  // -Y
    4u, 5u, 7u, 4u, 7u, 6u,  // +Z
    0u, 3u, 1u, 0u, 2u, 3u); // -Z

void main()
{
    g_viewIndex = int(u_viewIndex);
    const ForceEmitterData e = fe_emitters[gl_InstanceIndex];
    const uint corner = c_cubeIndices[gl_VertexIndex];
    const vec3 local = vec3(float(corner & 1u), float((corner >> 1u) & 1u), float(corner >> 2u)) * 2.0 - 1.0;

    float side, forward, back;
    forceVisibleBounds(e, side, forward, back); // the support box shrunk to the visible-surface extent
    const mat3 basis = forceEmitterBasis(e.dirFocus.xyz); // columns: right, up, dir
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * (forward - back) * 0.5;
    const vec3 halfExtents = vec3(side, side, (forward + back) * 0.5);
    const vec3 world = center + basis * (local * halfExtents);

    gl_Position = u_mvp * vec4(world, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;
    v_emitterIdx = uint(gl_InstanceIndex);
}
