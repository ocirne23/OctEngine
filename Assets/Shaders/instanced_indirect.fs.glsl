#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"

layout (location = 0) in vec4 in_posU;    // xyz = world position, w = uv.x
layout (location = 1) in vec4 in_normalV; // xyz = normal, w = uv.y
layout (location = 2) in vec4 in_tangent; // xyz = tangent, w = bitangent sign
layout (location = 3) in flat uint in_meshIdxMaterialIdx;
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;

#include "instanced_indirect_lit.inc.glsl"

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex); // per-eye reconstruction (AO upsample) + view pos
#endif
	const vec3 pos = in_posU.xyz;
	const vec3 V = normalize(u_viewPos - pos);

	const uint16_t materialIdx   = uint16_t((in_meshIdxMaterialIdx & 0xFFFF0000) >> 16);
	const MaterialInfo material  = in_materialInfos[materialIdx];
	const uint16_t diffuseTexIdx = uint16_t(material.diffuseNormalTexIdx & 0x0000FFFF);
	const uint16_t normalTexIdx  = uint16_t((material.diffuseNormalTexIdx & 0xFFFF0000) >> 16);
	const uint16_t metalRoughnessTexIdx = uint16_t(material.metalRoughnessTexIdxAlphaMode & 0x0000FFFF);
	const vec2 uv = vec2(in_posU.w, in_normalV.w);

	const vec4 diffuseSample  = texture(u_textures[diffuseTexIdx], uv);
#ifdef ALPHA_MASK
	// Only the LitMasked variant discards: a discard anywhere in the shader costs the pipeline its early
	// depth write. The alpha-mode test stays, because a material override can put an opaque material here.
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);
	if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
		discard;
#endif

	// The surface is HALF from the texture taps on (computeLitColor takes it half): colour, roughness,
	// metalness and the normal-map decode + TBN.
	float16_t roughness = float16_t(0.65);
	float16_t metalness = float16_t(0.0);
	if (metalRoughnessTexIdx != uint16_t(0xFFFF))
	{
		const f16vec2 metalRoughness = f16vec2(texture(u_textures[metalRoughnessTexIdx], uv).bg);
		metalness = metalRoughness.x;
		roughness = max(metalRoughness.y, float16_t(0.01));
	}

	const f16vec3 materialColor = f16vec3(diffuseSample.xyz);
	// Two-channel BC5 normal maps store only X/Y (red/green), so .z reads 0 and would flip the normal
	// into the surface - reconstruct Z from X/Y. Full RGB(A) normal maps keep their stored Z.
	const f16vec3 normalSample = f16vec3(texture(u_textures[normalTexIdx], uv).xyz);
	f16vec3 tangentNormal;
	if ((material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u)
	{
		const f16vec2 normalXY = normalSample.xy * float16_t(2.0) - float16_t(1.0);
		tangentNormal = f16vec3(normalXY, sqrt(max(float16_t(1.0) - dot(normalXY, normalXY), float16_t(0.0))));
	}
	else
	{
		tangentNormal = normalize(normalSample * float16_t(2.0) - float16_t(1.0));
	}
	const f16vec3 geoN = f16vec3(in_normalV.xyz);
	const f16vec3 T = f16vec3(in_tangent.xyz);
	const f16vec3 B = cross(geoN, T) * float16_t(in_tangent.w < 0.0 ? -1.0 : 1.0);
	const f16vec3 N = normalize(T * tangentNormal.x + B * tangentNormal.y + geoN * tangentNormal.z);

	const vec3 color = computeLitColor(pos, V, N, materialColor, roughness, metalness, float16_t(1.0));
	out_color = vec4(color, min(diffuseSample.a, material.opacity));
}
