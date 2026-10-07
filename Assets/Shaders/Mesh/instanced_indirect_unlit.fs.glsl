#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"
#include "motion_vector.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // the per-eye view (1=left, 2=right) in VR: the motion vector's matrices
#endif

struct MaterialInfo
{
	uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
	uint metalRoughnessTexIdxAlphaMode;
};

layout (binding = 2, std430) readonly buffer InMaterialInfos
{
    MaterialInfo in_materialInfos[];
};

layout (binding = 23) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count

layout (location = 0) in vec4 in_posU;    // xyz = world position, w = uv.x (instanced_indirect.vs.glsl)
layout (location = 1) in vec4 in_normalV; // xyz = normal, w = uv.y
layout (location = 3) in flat uint in_meshIdxMaterialIdx;
layout (location = 4) in vec3 in_prevWorldDelta; // the motion vectors (instanced_indirect.vs.glsl)

layout (location = 0) out vec4 out_color;
#ifndef NO_MOTION_OUTPUT // the transparent variant: its DGC set's fragment interface is location 0 only
layout (location = 1) out vec4 out_motion; // the scene's motion target (write-masked except in UnlitOpaque)
#endif

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const uint16_t materialIdx = uint16_t((in_meshIdxMaterialIdx & 0xFFFF0000) >> 16);
    const MaterialInfo material = in_materialInfos[materialIdx];
    const uint16_t diffuseTexIdx = uint16_t(material.diffuseNormalTexIdx & 0x0000FFFF);
    const uint16_t metalRoughnessTexIdx = uint16_t(material.metalRoughnessTexIdxAlphaMode & 0x0000FFFF);
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);

    const vec4 diffuseSample = texture(u_textures[diffuseTexIdx], vec2(in_posU.w, in_normalV.w));
    //const vec4 diffuseSample = vec4(0.5, 0.5, 0.5, 1.0);

    // Alpha mask (alphaMode 1): discard fragments below the cutoff (stored in material.opacity).
    if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
        discard;
	out_color = vec4(diffuseSample.xyz, min(diffuseSample.a, material.opacity));
#ifndef NO_MOTION_OUTPUT
	out_motion = motionVector(in_prevWorldDelta);
#endif
}
