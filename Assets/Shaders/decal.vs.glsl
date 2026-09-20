#version 460

// Projected decal boxes: 36 vertices per instance (a unit cube, outward CCW winding), instanced from
// the per-frame decal buffer. The pipeline culls FRONT faces and skips the depth test, so the box's
// far/inside faces rasterize even when the camera sits inside the volume; the fragment shader does the
// actual surface projection from the scene depth.

#include "shared.inc.glsl"
#include "decal.inc.glsl"

layout (binding = 1, std430) readonly buffer Decals { Decal d_decals[]; };

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) out flat uint v_decalIdx;

// Outward-CCW cube triangles over corners indexed bit0 = +X, bit1 = +Y, bit2 = +Z.
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
    const Decal decal = d_decals[gl_InstanceIndex];
    const uint corner = c_cubeIndices[gl_VertexIndex];
    const vec3 local = vec3(float(corner & 1u), float((corner >> 1u) & 1u), float(corner >> 2u)) * 2.0 - 1.0;
    const vec3 world = decal.posOpacity.xyz + decalQuatRotate(decal.rotation, local * decal.halfExtentsAngleFade.xyz);
    gl_Position = u_mvp * vec4(world, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;
    v_decalIdx = uint(gl_InstanceIndex);
}
