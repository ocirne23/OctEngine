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
// count of the other lit meshes. The VS is instanced_indirect.vs.glsl (ALPHA_MASK, TREE_ORIGIN).

#include "shared.inc.glsl"

layout (location = 0) in vec4 in_posU;    // xyz = world position, w = uv.x
layout (location = 1) in vec4 in_normalV; // xyz = normal, w = uv.y
layout (location = 2) in vec4 in_tangent; // xyz = tangent, w = bitangent sign
layout (location = 3) in flat uint in_meshIdxMaterialIdx;
layout (location = 4) in vec3 in_prevWorldDelta; // the motion vectors (instanced_indirect.vs.glsl)
layout (location = 5) in flat uint in_treeInstance;  // the tree's draw slot (the VS's TREE_ORIGIN): its instance record, in_drawInstances
layout (location = 6) in vec3 in_treeGrid;           // the point in its tree's voxel grid (0..1; tree_leaf_sun.inc.glsl)
layout (location = 10) in vec3 in_windSway;          // the wind's sway of this point without the trunk's bend (its branch + its leaf flutter)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target

// THE TREE'S INSTANCE RECORD (the main cull's OutMeshInstance, 64 B, at the draw slot): what is the same for every pixel of
// a tree is read HERE, where it is used, and not handed over as flat interpolants (7 plane equations a triangle, on the
// smallest triangles of the scene) - the origin (the distance fade, the height up the tree), the rotation and the scale
// (the sun in the tree's voxel grid). `wind` / `rest`: the tree's wind and its mesh | material (instanced_indirect.vs.glsl).
// The tree's TYPE (the leaves' voxel grid, tree_type.inc.glsl) = the low 16 bits of in_meshIdxMaterialIdx.
struct TreeDrawInstance
{
	vec4 posScale;
	vec4 quat;
	vec4 wind;
	uvec4 rest;
};
layout (binding = 1, std430) readonly buffer InDrawInstances { TreeDrawInstance in_drawInstances[]; };

// The lit core's AO read follows this point to where it was last frame (read where it is used, so the interpolant is
// not live across the sun's shadow search).
#define MOTION_WORLD_DELTA in_prevWorldDelta
// The sun's shadow at the top of main, before the material is read (see instanced_indirect_lit.inc.glsl).
#define SUN_SHADOW_FIRST
#define LEAF_SKY_WRAP        // instanced_indirect_lit.inc.glsl: a downward leaf's ambient mirrored toward the sky
#define LEAF_SPEC_NORMAL     // instanced_indirect_lit.inc.glsl: a leaf's sun specular from a more curved normal
#define LIT_EXTRA_LIGHT      // instanced_indirect_lit.inc.glsl: the leaves' transmitted sun, added inside computeLitColor
#define LIT_NO_UNDERWATER    // instanced_indirect_lit.inc.glsl: a tree's leaves and bark are never under the water
#define LIT_DIELECTRIC       // instanced_indirect_lit.inc.glsl: a tree material is never metal (no metal-roughness texture)
float16_t g_specularF0 = float16_t(0.04);
#define LIT_SPECULAR_F0 g_specularF0 // instanced_indirect_lit.inc.glsl: the leaves' own F0 ("Trees/Leaf specular")
// THE LEAVES READ NO RTAO (instanced_indirect_lit.inc.glsl LIT_RTAO_SKIP): they are not in the TLAS and they move in the
// wind, so a trace from their depth is noise (as the grass's). Their fragments write the NO-AO flag, so the trace leaves a
// hole and spends no ray on them. The BARK reads the RTAO: its mesh stands in the TLAS for those rays.
bool g_leafNoRtao = false;
#define LIT_RTAO_SKIP g_leafNoRtao
// instanced_indirect_lit.inc.glsl: the canopy map's occlusion above this height up the tree - the point's height over the
// tree's origin (computeLitColor's worldPos), the origin read there.
#define LIT_CANOPY_HEIGHT (worldPos.y - in_drawInstances[in_treeInstance].posScale.y)
#include "instanced_indirect_lit.inc.glsl"
#include "tree_fade.inc.glsl"
#include "leaf_look.inc.glsl"
#define VOXEL_BRICK_ORIGIN // tree_voxel.inc.glsl: the march does not set this shader's register count
#include "tree_voxel.inc.glsl"
#define TREE_TYPES_BINDING 3
#define TREE_LEAF_SUN_MARCH
#include "tree_leaf_sun.inc.glsl" // the leaves' sun self-shadow through the tree's own voxel grid
#define FACTION_MAP_BINDING 24
#include "terrain_faction.inc.glsl" // the game's faction layer: the tree takes the faction look of the land it stands on

// THE SHADOW FIRST (instanced_indirect.fs.glsl's sunShadowFirst), with the LEAVES looked up from their sun side: a thin
// leaf facing AWAY from the sun is still lit through, and its transmission needs its real shadow (the facing reject
// gave it 0). The bias follows the geometric normal flipped toward the sun; doSunLight's facing test on the shading
// normal still keeps the front light off a back-facing leaf. ONE call of sunShadowVisibility for both cases: each call
// site inlines the whole shadow search (two cost ~20 KB of code).
// selfExit > 0 (a leaf with the SELF-SHADOW: tree_leaf_sun.inc.glsl): the cascades are looked up where the sun path
// LEAVES the tree's box, past its own caster there (the shadow billboard, whose depth never passes the box; with
// "Trees/Voxel shadows" the voxel grid itself) - the march darkens by the tree's own leaves already, and its caster
// would shade it a second time. As the voxel tier's lookup.
// EVERY LOOKUP IS AT THE POSITION BEFORE THE WIND'S SWAY (in_windSway): the casters bend by the trunk alone, so a swaying
// branch or leaf would move through its own tree's shadow.
void sunShadowFirstTree(vec3 pos, bool leaf, float selfExit)
{
	const vec3 geoN = normalize(in_normalV.xyz);
	const bool facing = dot(geoN, u_sunDirection.xyz) > 0.0;
	const vec3 restPos = pos - in_windSway;
	const vec3 shadowPos = selfExit > 0.0 ? restPos + u_sunDirection.xyz * (selfExit + 0.5) : restPos;
	g_sunShadowFirst = (leaf || facing) && u_sunLit > 0.5 ? sunShadowVisibility(shadowPos, facing ? geoN : -geoN) : 0.0; // no search at night
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
	const vec2 texUv                    = vec2(in_posU.w, in_normalV.w);

	const vec4 diffuseSample = texture(u_textures[diffuseTexIdx], texUv);
	// The alpha-mode test stays: a material override can put an opaque material here.
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);
	if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
		discard;
	// The crossfades, per tree (tree_fade.inc.glsl): by the camera's distance to the tree's ORIGIN, read only on a material
	// that fades.
	if ((material.flags & MATERIAL_FLAG_DISTANCE_FADE) != 0u
		&& treeDistanceFaded(material.flags, distance(u_viewPos, in_drawInstances[in_treeInstance].posScale.xyz)))
		discard;

	// After the discards: a cut-out pixel pays no shadow.
	const bool leaf = (material.flags & MATERIAL_FLAG_LEAF) != 0u;
	// THE LEAVES' SUN SELF-SHADOW (tree_leaf_sun.inc.glsl): the sun through the tree's own voxel grid, as the voxel tier
	// marches it. Its box's exit FIRST (no texture: the cascades are looked up there), the march after the search and only
	// for a pixel the cascades leave lit. Its inputs are the grid interpolant and two loads (the tree's record, its type),
	// taken again at the march: nothing is held across the search. The result is ONE scalar, the log of the transmittance
	// x "Voxel self shadow" (0 = none). A type without a grid takes neither.
	const bool selfShadow = leaf && u_foliage_voxelSelfShadow > 0.0 && u_sunLit > 0.5;
	const uint typeIdx = in_meshIdxMaterialIdx & 0xFFFFu;
	sunShadowFirstTree(pos, leaf, selfShadow ? treeLeafSunExit(in_treeGrid, treeLeafSun(in_treeInstance, typeIdx)) : 0.0);
	float sunLog = 0.0;
	if (selfShadow && g_sunShadowFirst > 0.0)
		sunLog = log2(max(treeLeafSunTransmit(in_treeGrid, treeLeafSun(in_treeInstance, typeIdx), treeLeafGridVoxel(typeIdx),
			distance(u_viewPos, pos)), 1e-4)) * u_foliage_voxelSelfShadow;

	// The surface is HALF from the texture taps on (computeLitColor takes it half): colour, roughness and the normal-map
	// decode + TBN. Leaves: "Trees/Leaf roughness" (a waxy sheen) and the crown's colour PATCHES. A tree material has NO
	// metal-roughness texture (Procedural createTreeMaterial): the metalness is the constant 0, so the lit core's
	// specular colour is the one F0 scalar.
	const float16_t roughness = float16_t(leaf ? u_foliage_leafRoughness : 0.65);

	const f16vec3 materialColor = terrainFactionAlbedo(f16vec3(leaf ? leafPatchTint(diffuseSample.xyz, pos) : diffuseSample.xyz), pos.xz,
		leaf ? 1.0 : u_terrain_factionSolidScale); // bark: the weaker tint, as the near bark's (instanced_indirect.fs.glsl)

	// THE NORMAL MAP: Z rebuilt from X / Y for BOTH layouts, no branch on the format - the bark's two-channel BC5 has no Z
	// (.z reads 0), and the leaves' (and the uncompressed bark's) RGB normals are unit with Z > 0, so the rebuilt Z is
	// their stored one.
	const f16vec4 normalTap = f16vec4(texture(u_textures[normalTexIdx], texUv));
	const f16vec2 normalXY = normalTap.xy * float16_t(2.0) - float16_t(1.0);
	const f16vec3 rawTangentNormal = f16vec3(normalXY, sqrt(max(float16_t(1.0) - dot(normalXY, normalXY), float16_t(0.0))));
	// Leaves: the normal map's slope x "Trees/Leaf normal strength" (0 = flat cards; the cupped, tilted leaves of the
	// cluster texture are subtle at 1; at exactly 1 - a constant while its row is locked - nothing to do). Its A is the
	// leaf's RIM (0 at the midrib .. 1 at the edge, Procedural TreeLeafTexture): a leaf is thicker in its centre, so less
	// sun gets through there. (From the normal's tilt it was wrong: each leaf's own random tilt cancels the cup on one
	// half, which then read as all centre.)
	const float leafRim = float(normalTap.w);
	f16vec3 tangentNormal = rawTangentNormal;
	if (leaf && u_foliage_leafNormalStrength != 1.0) // not unit after it: the TBN's normalize below takes any length
		tangentNormal.xy *= float16_t(u_foliage_leafNormalStrength);

	const f16vec3 geoN = f16vec3(in_normalV.xyz);
	const f16vec3 T = f16vec3(in_tangent.xyz);
	const f16vec3 B = cross(geoN, T) * float16_t(in_tangent.w < 0.0 ? -1.0 : 1.0);
	f16vec3 N = normalize(T * tangentNormal.x + B * tangentNormal.y + geoN * tangentNormal.z);

	// The LEAF terms (MATERIAL_FLAG_LEAF). Formed BEFORE computeLitColor: its light loop is the shader's register peak.
	// The transmitted sun goes in as g_litExtraLight, added to computeLitColor's sum before that loop (LIT_EXTRA_LIGHT):
	// a transmission colour kept live across the loop and added after it cost 16 B of spill.
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
#ifdef LEAF_SPEC_NORMAL
		if (u_foliage_leafSpecCurve > 0.0)
		{
			const f16vec2 st = rawTangentNormal.xy * float16_t(u_foliage_leafSpecCurve); // not unit: the normalize below takes any length
			vec3 sn = normalize(vec3(T * st.x + B * st.y + geoN * rawTangentNormal.z));
			const float snoV = dot(sn, V);
			if (snoV < u_foliage_minNoV)
				sn = normalize(sn + V * (u_foliage_minNoV - snoV));
			g_leafSpecN = f16vec3(sn);
			g_leafSpecOn = true;
		}
#endif

		// TRANSMISSION: thin leaves let the sun through, tinted by their colour - a diffuse back term saturate(-N.L)
		// plus a forward GLOW saturate(V.-L)^focus x glow (the backlit rim), x "Trees/Foliage transmission". It leans on the
		// sun shadow only by "Foliage transmission shadow": a leaf seen from the shaded side sits in its own crown's shadow,
		// which would leave the backlit view no glow.
		const vec3 L = u_sunDirection.xyz;
		// The FRONT sun only on the card face that faces it, whatever the normal map says: the seen face of a leaf lit from
		// behind had half of each leaf tilted into the sun by the map (x "Leaf specular curve" for the highlight), and
		// looking toward a low sun Fresnel -> 1 turned that half white. The back side gets the transmission below.
		g_sunVisMaterial = float16_t(smoothstep(0.0, 0.1, dot(vec3(geoN), L) * inversesqrt(max(dot(vec3(geoN), vec3(geoN)), 1e-8))));
		const float back = max(-dot(vec3(N), L), 0.0);
		const float glow = pow(max(-dot(V, L), 0.0), u_foliage_transmissionFocus) * u_foliage_transmissionGlow;
		// x the leaf's THICKNESS: 1 - "Leaf transmission centre" at the midrib, 1 at the rim (leafRim).
		// ... and the self-shadow at "Foliage transmission shadow"'s share too (the voxel tier's rule: light scattered forward
		// through the leaves loses less).
		const float visibility = mix(1.0, g_sunShadowFirst, u_foliage_transmissionShadow) * mix(1.0 - u_foliage_leafTransmissionCentre, 1.0, leafRim)
			* exp2(sunLog * u_foliage_transmissionShadow);
		const float through = (back + glow) * visibility; // 0 for a sun-facing leaf seen with the sun behind
		if (through > 0.0)
			g_litExtraLight = f16vec3(min(vec3(materialColor) * (through * u_foliage_transmission * INV_PI)
				* (u_sunTransmittance * u_sunColor.rgb), vec3(MEDIUMP_FLT_MAX)));
	}

#ifdef LEAF_SKY_WRAP
	g_leafSkyWrap = float16_t(leaf ? u_foliage_leafSkyWrap : 0.0);
#endif
	g_specularF0 = float16_t(leaf ? u_foliage_leafSpecular : 0.04); // "Trees/Leaf specular": a waxy leaf's white highlight
	g_leafNoRtao = leaf;
	g_sunShadowFirst *= exp2(sunLog); // the direct sun takes the whole self-shadow (after the transmission took its share)
	const vec3 color = computeLitColor(pos, V, N, materialColor, roughness, float16_t(0.0), float16_t(1.0));
	// Alpha 1: the pipeline does not blend (alpha-tested, opaque), and the texture's alpha and the material's opacity were
	// two values live across the whole lit core for it.
	out_color = vec4(color, 1.0);
	out_motion = leaf ? motionVectorNoAo(in_prevWorldDelta) : motionVector(in_prevWorldDelta);
}
