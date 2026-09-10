#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"

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

layout (binding = 21) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count

layout (location = 0) in vec3 in_pos;
layout (location = 1) in mat3 in_tbn;
layout (location = 4) in vec2 in_uv;
layout (location = 5) in flat uint in_meshIdxMaterialIdx;

layout (location = 0) out vec4 out_color;

void main()
{
    const uint16_t materialIdx = uint16_t((in_meshIdxMaterialIdx & 0xFFFF0000) >> 16);
    const MaterialInfo material = in_materialInfos[materialIdx];
    const uint16_t diffuseTexIdx = uint16_t(material.diffuseNormalTexIdx & 0x0000FFFF);
    const uint16_t metalRoughnessTexIdx = uint16_t(material.metalRoughnessTexIdxAlphaMode & 0x0000FFFF);
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);

    const vec4 diffuseSample = texture(u_textures[diffuseTexIdx], in_uv);
    //const vec4 diffuseSample = vec4(0.5, 0.5, 0.5, 1.0);

    // Alpha mask (alphaMode 1): discard fragments below the cutoff (stored in material.opacity).
    if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
        discard;
	out_color = vec4(diffuseSample.xyz, min(diffuseSample.a, material.opacity));
}
