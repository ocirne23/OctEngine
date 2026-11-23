#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// THE SNOW CAPS' surface (snow_cap.vs.glsl): the lit core with the snow's material and its sun BRDF - the object layer's
// (the same relief tap, decode, projection, state and glint lattice as the layer the cap's rim sinks into), its relief
// turned onto the cap's own normal (the rounded rim shades). Static: no motion (the camera reprojects), alpha 1. RTAO
// stays (the contact shade against a wall).

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in float in_top;

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // write-masked (static)

#define SUN_SHADOW_FIRST
#define LIT_DIELECTRIC
#define LIT_NO_UNDERWATER
#define LIT_SNOW
#include "instanced_indirect_lit.inc.glsl"
#define SNOW_OBJECTS
#include "snow.inc.glsl"

void main()
{
	out_motion = vec4(0.0);
	const vec3 pos = in_pos;
	const vec3 geoN = normalize(in_normal);
	// The shadow first (the register peak), from at least 2 cm over the top: the sunk rim's rays would start inside it.
	const vec3 shadowPos = vec3(pos.x, max(pos.y, in_top + 0.02), pos.z);
	g_sunShadowFirst = dot(geoN, u_sunDirection.xyz) > 0.0 ? sunShadowVisibility(shadowPos, geoN) : 0.0;

	const vec2 dx = dFdx(pos.xz), dz = dFdy(pos.xz);
	// The state: the point lies over the top, so the catch is its column's pass - as the roof's object layer reads it.
	const float tempC = terrainTemperatureAt(terrainClimateAt(pos.xz), pos.y);
	const SnowState st = snowStateAt(tempC, snowSiteCatch(pos));
	const f16vec3 geoNh = f16vec3(geoN);
	const f16vec3 N = snowNormalOn(geoNh, snowObjectRelief(pos.xz, dx, dz));
	const f16vec3 albedo = f16vec3(snowAlbedo(st));
	const float16_t rough = float16_t(snowAlpha(st));

	const vec3 V = normalize(u_viewPos - pos);
	snowLitSetup(pos.xz, N, geoNh, V, float16_t(1.0), rough, snowGlintDensity(st), snowFootprint(dx, dz));
	out_color = vec4(computeLitColor(pos, V, N, albedo, rough, float16_t(0.0), float16_t(1.0)), 1.0);
}
