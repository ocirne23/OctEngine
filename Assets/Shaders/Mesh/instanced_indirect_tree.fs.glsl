#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// EPipelineIndex::LitTree: the procedural tree MESHES (Procedural TreeSystem) - instanced_indirect.fs.glsl's LitMasked
// path plus the tree-only terms: the DISTANCE FADE (tree_fade.inc.glsl) and the LEAF terms (MATERIAL_FLAG_LEAF: the
// sun-side shadow, the edge-on N.V bend, the transmission). Its own shader so the tree code never sets the register
// count of the other lit meshes. The VS is instanced_indirect.vs.glsl (ALPHA_MASK).

#include "shared.inc.glsl"

layout (location = 0) in vec4 in_posU;    // xyz = world position, w = uv.x
layout (location = 1) in vec4 in_normalV; // xyz = normal, w = uv.y
layout (location = 2) in vec4 in_tangent; // xyz = tangent, w = bitangent sign
layout (location = 3) in flat uint in_meshIdxMaterialIdx;
layout (location = 4) in vec3 in_prevWorldDelta; // the motion vectors (instanced_indirect.vs.glsl)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target

// The lit core's AO read follows this point to where it was last frame (read where it is used, so the interpolant is
// not live across the sun's shadow search).
#define MOTION_WORLD_DELTA in_prevWorldDelta
// The sun's shadow at the top of main, before the material is read (see instanced_indirect_lit.inc.glsl).
#define SUN_SHADOW_FIRST
#define SHADOW_LEAF_TRANSMIT // shadows.inc.glsl: the leaves' shadow fades in with the gap to the blocker
#define LEAF_SKY_WRAP        // instanced_indirect_lit.inc.glsl: a downward leaf's ambient mirrored toward the sky
#define LEAF_SPEC_NORMAL     // instanced_indirect_lit.inc.glsl: a leaf's sun specular from a more curved normal
#include "instanced_indirect_lit.inc.glsl"
#include "tree_fade.inc.glsl"
#include "leaf_look.inc.glsl"

// THE SHADOW FIRST (instanced_indirect.fs.glsl's sunShadowFirst), with the LEAVES looked up from their sun side: a thin
// leaf facing AWAY from the sun is still lit through, and its transmission needs its real shadow (the facing reject
// gave it 0). The bias follows the geometric normal flipped toward the sun; doSunLight's facing test on the shading
// normal still keeps the front light off a back-facing leaf. ONE call of sunShadowVisibility for both cases: each call
// site inlines the whole shadow search (two cost ~20 KB of code).
void sunShadowFirstTree(vec3 pos, bool leaf)
{
	const vec3 geoN = normalize(in_normalV.xyz);
	const bool facing = dot(geoN, u_sunDirection.xyz) > 0.0;
	g_shadowLeafLength = leaf ? u_foliage_leafShadowLength : 0.0; // "Trees/Leaf shadow length (m)"
	g_sunShadowFirst = leaf || facing ? sunShadowVisibility(pos, facing ? geoN : -geoN) : 0.0;
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex); // per-eye reconstruction (AO upsample) + view pos
#endif
	const vec3 pos = in_posU.xyz;
	const vec3 V = normalize(u_viewPos - pos);

	const uint16_t materialIdx          = uint16_t((in_meshIdxMaterialIdx & 0xFFFF0000) >> 16);
	const MaterialInfo material         = in_materialInfos[materialIdx];
	const uint16_t diffuseTexIdx        = uint16_t(material.diffuseNormalTexIdx & 0x0000FFFF);
	const uint16_t normalTexIdx         = uint16_t((material.diffuseNormalTexIdx & 0xFFFF0000) >> 16);
	const uint16_t metalRoughnessTexIdx = uint16_t(material.metalRoughnessTexIdxAlphaMode & 0x0000FFFF);
	const vec2 texUv                    = vec2(in_posU.w, in_normalV.w);

	const vec4 diffuseSample = texture(u_textures[diffuseTexIdx], texUv);
	// The alpha-mode test stays: a material override can put an opaque material here.
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);
	if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
		discard;
	if (treeDistanceFaded(material.flags, pos)) // the crossfades (tree_fade.inc.glsl)
		discard;

	// After the discards: a cut-out pixel pays no shadow.
	const bool leaf = (material.flags & MATERIAL_FLAG_LEAF) != 0u;
	sunShadowFirstTree(pos, leaf);

	// The surface is HALF from the texture taps on (computeLitColor takes it half): colour, roughness, metalness and the
	// normal-map decode + TBN. Leaves: "Trees/Leaf roughness" (a waxy sheen) and the crown's colour PATCHES.
	float16_t roughness = float16_t(leaf ? u_foliage_leafRoughness : 0.65);
	float16_t metalness = float16_t(0.0);
	if (metalRoughnessTexIdx != uint16_t(0xFFFF))
	{
		const f16vec2 metalRoughness = f16vec2(texture(u_textures[metalRoughnessTexIdx], texUv).bg);
		metalness = metalRoughness.x;
		roughness = metalRoughness.y; // the cook / upload floors G >= 3/255
	}

	const f16vec3 materialColor = f16vec3(leaf ? leafPatchTint(diffuseSample.xyz, pos) : diffuseSample.xyz);

	// Two-channel BC5 normal maps store only X/Y (red/green), so .z reads 0 and would flip the normal into the surface -
	// reconstruct Z from X/Y. Full RGB(A) normal maps keep their stored Z.
	const f16vec4 normalTap = f16vec4(texture(u_textures[normalTexIdx], texUv));
	const f16vec3 normalSample = normalTap.xyz;
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
	// Leaves: the normal map's slope x "Trees/Leaf normal strength" (0 = flat cards; the cupped, tilted leaves of the
	// cluster texture are subtle at 1). Its A is the leaf's RIM (0 at the midrib .. 1 at the edge, Procedural
	// TreeLeafTexture): a leaf is thicker in its centre, so less sun gets through there. (From the normal's tilt it was
	// wrong: each leaf's own random tilt cancels the cup on one half, which then read as all centre.)
	const float leafRim = float(normalTap.w);
	const f16vec3 rawTangentNormal = tangentNormal;
	if (leaf)
		tangentNormal = normalize(f16vec3(tangentNormal.xy * float16_t(u_foliage_leafNormalStrength), tangentNormal.z));

	const f16vec3 geoN = f16vec3(in_normalV.xyz);
	const f16vec3 T = f16vec3(in_tangent.xyz);
	const f16vec3 B = cross(geoN, T) * float16_t(in_tangent.w < 0.0 ? -1.0 : 1.0);
	f16vec3 N = normalize(T * tangentNormal.x + B * tangentNormal.y + geoN * tangentNormal.z);

	// The LEAF terms (MATERIAL_FLAG_LEAF). Formed BEFORE computeLitColor: its light loop is the shader's register peak,
	// and only the half transmission colour is live across it - not the shadow and the surface colour it is made of.
	f16vec3 transmit = f16vec3(0.0);
	if (leaf)
	{
		// Seen nearly edge-on: N bent toward the viewer until N.V reaches "Trees/Foliage min N.V". The cards' normals are
		// bent, so N.V can approach 0 while N.L > 0, and the sun's GGX at grazing lit a leaf hundreds of times brighter
		// than its diffuse. Adding V x (min - N.V) gives N.V = min / |N'| (just under min); N.L barely moves.
		const float noV = dot(vec3(N), V);
		if (noV < u_foliage_minNoV)
			N = f16vec3(normalize(vec3(N) + V * (u_foliage_minNoV - noV)));

		// The SPECULAR normal: the normal map's slope x "Trees/Leaf specular curve" (from the raw map, not x the normal
		// strength), the same edge-on bend - the sun's highlight from a more curved leaf (doSunLight, LEAF_SPEC_NORMAL).
		if (u_foliage_leafSpecCurve > 0.0)
		{
			const f16vec3 st = normalize(f16vec3(rawTangentNormal.xy * float16_t(u_foliage_leafSpecCurve), rawTangentNormal.z));
			vec3 sn = normalize(vec3(T * st.x + B * st.y + geoN * st.z));
			const float snoV = dot(sn, V);
			if (snoV < u_foliage_minNoV)
				sn = normalize(sn + V * (u_foliage_minNoV - snoV));
			g_leafSpecN = f16vec3(sn);
			g_leafSpecOn = true;
		}

		// TRANSMISSION: thin leaves let the sun through, tinted by their colour DEEPENED (leafTransmitColor) - a diffuse back term saturate(-N.L)
		// plus a forward GLOW saturate(V.-L)^focus x glow (the backlit rim), x "Trees/Foliage transmission". It leans on the
		// sun shadow only by "Foliage transmission shadow": a leaf seen from the shaded side sits in its own crown's shadow,
		// which would leave the backlit view no glow.
		const vec3 L = u_sunDirection.xyz;
		const float back = max(-dot(vec3(N), L), 0.0);
		const float glow = pow(max(-dot(V, L), 0.0), u_foliage_transmissionFocus) * u_foliage_transmissionGlow;
		// x the leaf's THICKNESS: 1 - "Leaf transmission centre" at the midrib, 1 at the rim (leafRim).
		const float visibility = mix(1.0, g_sunShadowFirst, u_foliage_transmissionShadow) * mix(1.0 - u_foliage_leafTransmissionCentre, 1.0, leafRim);
		transmit = f16vec3(leafTransmitColor(vec3(materialColor)) * ((back + glow) * visibility * u_foliage_transmission * INV_PI)); // <= ~11.5 by the tweak ranges
	}

	g_leafSkyWrap = float16_t(leaf ? u_foliage_leafSkyWrap : 0.0);
	vec3 color = computeLitColor(pos, V, N, materialColor, roughness, metalness, float16_t(1.0));
	color += vec3(transmit) * (u_sunTransmittance * u_sunColor.rgb);
	out_color = vec4(color, min(diffuseSample.a, material.opacity));
	out_motion = motionVector(in_prevWorldDelta);
}
