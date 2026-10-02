#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"

// Procedural tree piece impostor (EPipelineIndex::TreeImpostor). One quad per instance, picked from an
// octahedral atlas of N x N frames baked on the CPU (Code/Procedural/Private/TreeImpostor.cpp - the frame
// mapping, the frame basis and the atlas layout below are MIRRORED there; keep them in step).
//
// The quad mesh carries no real geometry, only per-piece constants (all 4 vertices share them but the corner):
//   in_posU.xyz    = the piece's bounding-sphere centre (piece-local)
//   in_normalV.xyz = (corner x, corner y in -1/+1, sphere radius)
//   in_tangent.x   = N (frames per atlas side)
// The frame is chosen per instance from the camera direction in piece space, and the quad is built in THAT
// frame's basis, so it matches the baked orthographic view. The frame basis goes to the fragment shader as
// the TBN (normal = the frame direction, tangent = its right axis): the LitMasked FS's alpha discard and
// normal mapping then shade the baked frame unchanged.

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;
    uint meshIdxMaterialIdx;
    uint prevVertexDelta;
    uvec2 prevQuat;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec4 in_posU;
layout (location = 1) in vec4 in_normalV;
layout (location = 2) in vec4 in_tangent;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec4 out_posU;
layout (location = 1) out vec4 out_normalV;
layout (location = 2) out vec4 out_tangent;
layout (location = 3) out flat uint out_meshIdxMaterialIdx;
layout (location = 4) out vec3 out_prevWorldDelta;
layout (location = 5) out flat vec3 out_instanceOrigin; // the LitMasked FS's input (unused: no FOLIAGE flag here)

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec2 signNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral mapping of the full sphere, +Y as the pole axis.
vec2 octEncode(vec3 d)
{
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = d.xz;
    if (d.y < 0.0)
        p = (1.0 - abs(p.yx)) * signNotZero(p);
    return p;
}

vec3 octDecode(vec2 p)
{
    vec3 d = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (d.y < 0.0)
        d.xz = (1.0 - abs(d.zx)) * signNotZero(d.xz);
    return normalize(d);
}

// The frame's orthographic camera basis: `dir` points from the piece toward the viewer.
void frameBasis(vec3 dir, out vec3 right, out vec3 up)
{
    const vec3 ref = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    right = normalize(cross(ref, dir));
    up = cross(dir, right);
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];
    const vec4 quat = inst.quat;
    const float scale = inst.posScale.w;
    out_meshIdxMaterialIdx = inst.meshIdxMaterialIdx;

    const vec3 centreLocal = in_posU.xyz;
    const vec2 corner = in_normalV.xy;
    const float radius = in_normalV.z;
    const float frames = in_tangent.x;

    const vec3 centreWorld = quat_transform(centreLocal * scale, quat) + inst.posScale.xyz;
    const vec4 invQuat = vec4(-quat.xyz, quat.w);
    const vec3 toViewLocal = quat_transform(normalize(u_viewPos - centreWorld), invQuat);

    const vec2 cell = clamp(floor((octEncode(toViewLocal) * 0.5 + 0.5) * frames), vec2(0.0), vec2(frames - 1.0));
    const vec3 dirLocal = octDecode((cell + 0.5) / frames * 2.0 - 1.0);
    vec3 rightLocal, upLocal;
    frameBasis(dirLocal, rightLocal, upLocal);

    const vec3 dirWorld = quat_transform(dirLocal, quat);
    const vec3 rightWorld = quat_transform(rightLocal, quat);
    const vec3 upWorld = quat_transform(upLocal, quat);

    const vec3 pos = centreWorld + (rightWorld * corner.x + upWorld * corner.y) * (radius * scale);
    // Atlas: frame (cell.x, cell.y), its top row (corner.y = +1) at the frame's smallest v.
    const vec2 uv = (cell + vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5)) / frames;

    out_posU = vec4(pos, uv.x);
    out_normalV = vec4(dirWorld, uv.y);
    out_tangent = vec4(rightWorld, 1.0); // FS bitangent = cross(N, T) = the frame's up
    out_prevWorldDelta = vec3(0.0);      // static trees; a frame switch is a pop either way
    out_instanceOrigin = inst.posScale.xyz;

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;
}
