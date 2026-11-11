#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// EPipelineIndex::LitTreeVoxel: THE VOXEL TIER of the procedural trees (Procedural TreeSystem, "Trees/Voxel impostors").
// The main pass draws it wherever a tree's whole billboard would draw (tree_cull.inc.glsl treeCullMainFar); shadows, GI
// and RT keep the billboard. The mesh is the variant's grid box (Procedural treeVoxelBoxMesh), front faces only; this
// shader marches the tree's PREFILTERED voxel grid (two atlases, Procedural TreeImpostor.ixx "The VOXEL tier") from the
// face along the view ray at the level whose voxel is about a pixel, stops where the ray's coverage passes this pixel's
// threshold (stochastic coverage: TAA / DLSS resolve it), writes that point's depth and lights it with the foliage
// cards' terms - the leaf transmission, the N.V bend, the sky wrap, the interior as AO - and a real self-shadow: the sun
// marched through the tree's own voxels. The cascades' shadow is looked up where that sun path LEAVES the box, so the
// tree's own billboard (its shadow caster) does not shadow it twice.
// Drawn from its own command list (StaticMeshGraphicsPipeline::record): it writes FragDepth, which no pipeline of the
// DGC execution set does. The VS is instanced_indirect.vs.glsl with TREE_VOXEL.

#include "shared.inc.glsl"

layout (location = 0) in vec4 in_posU;                 // xyz = the box face's world position (with the trunk bend)
layout (location = 3) in flat uint in_meshIdxMaterialIdx;
layout (location = 4) in vec3 in_prevWorldDelta;       // the wind's motion (instanced_indirect.vs.glsl)
layout (location = 5) in flat vec3 in_voxelCam;        // the camera in grid space
layout (location = 6) in vec3 in_voxelGrid;            // the face point in grid space
layout (location = 7) in flat vec4 in_voxelQuat;       // grid (tree) axes -> world
layout (location = 8) in flat vec3 in_voxelWorldSize;  // the box in world metres
layout (location = 9) in flat vec3 in_instanceOrigin;  // the tree's origin: the distance fade
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target
// Reversed-Z: the hit lies at or behind the box face, so the written depth never exceeds the rasterized one - early depth
// still rejects a box the scene hides.
layout (depth_less) out float gl_FragDepth;

#define MOTION_WORLD_DELTA in_prevWorldDelta
#define SUN_SHADOW_FIRST    // instanced_indirect_lit.inc.glsl: the sun's shadow before the material
#define SHADOW_FOLIAGE_BIAS // shadows.inc.glsl: the foliage's constant depth bias
#define FOLIAGE_NO_RTAO     // instanced_indirect_lit.inc.glsl: g_noRtao (the depth is a point inside a crown)
#define LEAF_SKY_WRAP
#define LIT_EXTRA_LIGHT     // the leaves' transmitted sun, added inside computeLitColor
#include "instanced_indirect_lit.inc.glsl"
#include "tree_fade.inc.glsl"
#include "leaf_look.inc.glsl"
#define FACTION_MAP_BINDING 24
#include "terrain_faction.inc.glsl" // the game's faction layer: the far trees take the faction look of the land they stand on

#include "tree_voxel.inc.glsl"

// Interleaved gradient noise, shifted per frame (TAA averages it); `offset` decorrelates the uses.
float voxelNoise(vec2 offset)
{
	const vec2 pixel = gl_FragCoord.xy + offset + 5.588238 * float(u_frameIndex & 7u);
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// tree_voxel.inc.glsl's voxelTap on level `lod`: the scene sampler's mip bias (DLSS), which Vulkan adds to the explicit
// level, taken back off.
vec4 voxelTapScene(uint tex, vec3 g, float n, float tilesX, float lod)
{
	return voxelTap(tex, g, n, tilesX, lod - u_foliage_voxelSamplerBias);
}

// motion_vector.inc.glsl's motionVector at the HIT's depth, not the box face's.
vec4 voxelMotionVector(vec3 prevWorldDelta, float depth, float invW)
{
	if (all(equal(prevWorldDelta, vec3(0.0))))
		return vec4(0.0); // still: the readers reproject the written depth with the camera
	const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
	const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
	const vec4 prevClip = u_reprojClip * vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0)
		+ (u_prevMvp * vec4(prevWorldDelta, 0.0)) * invW;
	if (prevClip.w <= 0.0)
		return vec4(0.0, 0.0, -1.0, 1.0);
	return vec4(uv - prevClipToScreenUV(prevClip), prevClip.z / prevClip.w, 1.0);
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	const uint materialIdx = in_meshIdxMaterialIdx >> 16;
	const MaterialInfo material = in_materialInfos[materialIdx];
	// The crossfade with the meshes / branch cards (the billboard's band), per tree: before the march.
	if (treeDistanceFaded(material.flags, in_instanceOrigin))
		discard;

	const uint albedoTex = material.diffuseNormalTexIdx & 0xFFFFu;
	const uint normalTex = material.diffuseNormalTexIdx >> 16;
	// The grid's resolution from the atlas width (res^2 / TREE_VOXEL_TILES_Y) and its last level.
	const float res = voxelGridRes(albedoTex);
	const float tilesX = res / TREE_VOXEL_TILES_Y;
	const float maxLod = float(textureQueryLevels(u_textures[albedoTex]) - 1);

	// THE RAY, in grid space from the face; its parameter is the world distance from the face.
	const vec3 entry = in_posU.xyz;
	const vec3 V = normalize(u_viewPos - entry);
	const vec3 rayG = in_voxelGrid - in_voxelCam;
	const vec3 dirG = rayG / max(length(rayG * in_voxelWorldSize), 1e-6); // grid units per metre

	// THE LEVEL: a voxel about a pixel ("Trees/Voxel LOD bias"), dithered between the two nearest per pixel.
	const float voxel0 = pow(in_voxelWorldSize.x * in_voxelWorldSize.y * in_voxelWorldSize.z, 1.0 / 3.0) / res;
	const float pixelWorld = 2.0 * distance(u_viewPos, entry) / max(u_mipPixelScale, 1e-3);
	const float lodF = log2(max(pixelWorld / voxel0, 1e-6)) + u_foliage_voxelLodBias;
	const float lod = clamp(floor(lodF + voxelNoise(vec2(29.0, 61.0))), 0.0, maxLod);
	const float n = res * exp2(-lod);
	const float stepWorld = voxel0 * exp2(lod); // one crossing: the stored opacity's length

	// THE MARCH: until the ray's coverage passes this pixel's threshold, or it leaves the box (then nothing). Not the
	// distance fade's noise (tree_fade.inc.glsl, offset 0): inside its band only the high thresholds would be kept.
	const float threshold = max(voxelNoise(vec2(47.0, 113.0)), 0.004);
	// Empty space is skipped two levels coarser (voxelSkip), tested only while the last sample was empty.
	const vec3 skipDirG = mix(vec3(1e-6), dirG, greaterThan(abs(dirG), vec3(1e-6)));
	const float skipLod = min(lod + 2.0, maxLod);
	float t = stepWorld * voxelNoise(vec2(83.0, 7.0));
	float transmit = 1.0;
	bool hit = false;
	bool empty = true;
	vec3 hitG = vec3(0.0);
	vec3 albedo = vec3(0.0);
	float hitOpacity = 0.0;
	const int maxSteps = int(n * 1.75) + 2;
	for (int i = 0; i < maxSteps; ++i)
	{
		const vec3 g = in_voxelGrid + dirG * t;
		if (any(lessThan(g, vec3(-1e-3))) || any(greaterThan(g, vec3(1.0 + 1e-3))))
			break;
		if (empty && skipLod > lod)
		{
			const float skip = voxelSkip(albedoTex, g, skipDirG, res, tilesX, skipLod, u_foliage_voxelSamplerBias);
			if (skip > 0.0)
			{
				t += skip;
				continue;
			}
		}
		const vec4 tap = voxelTapScene(albedoTex, g, n, tilesX, lod);
		empty = tap.a <= VOXEL_SKIP_EMPTY;
		transmit *= 1.0 - tap.a;
		if (1.0 - transmit >= threshold)
		{
			hit = true;
			hitG = g;
			albedo = tap.rgb; // linear: the atlas is sRGB
			hitOpacity = tap.a;
			break;
		}
		t += stepWorld;
	}
	if (!hit)
		discard;

	const vec3 pos = entry - V * t;
	const vec4 clip = u_mvp * vec4(pos, 1.0);
	const float depth = min(clip.z / clip.w, gl_FragCoord.z); // never nearer than the face (depth_less)
	gl_FragDepth = depth;

	// THE NORMAL: the voxel's mean leaf normal; where its leaves face every way (a short mean) toward the crown's outward
	// direction ("Trees/Voxel normal hull"). Stored in the tree's axes.
	const vec4 normalTap = voxelTapScene(normalTex, hitG, n, tilesX, lod);
	const vec3 meanN = normalTap.xyz * 2.0 - 1.0;
	// 0 = the voxel's leaves face every way, 1 = all one way. Trusted by the voxel's cover: a sparse voxel (a thin tip) holds
	// a leaf or two, whose mean is long though the tip around it faces every way. "Voxel transmission spread" takes off more.
	const float leafAgree = min(length(meanN), 1.0) * smoothstep(0.05, 0.5, hitOpacity) * (1.0 - u_foliage_voxelTransmissionSpread);
	const vec3 hull = (hitG - 0.5) * in_voxelWorldSize;
	const vec3 hullN = dot(hull, hull) > 1e-8 ? normalize(hull) : vec3(0.0, 1.0, 0.0);
	vec3 localN = meanN + hullN * ((1.0 - min(length(meanN), 1.0)) * u_foliage_voxelNormalHull);
	localN = dot(localN, localN) > 1e-8 ? normalize(localN) : hullN;
	f16vec3 N = f16vec3(quat_transform(localN, in_voxelQuat));

	// THE SUN through the tree's own voxels (one level coarser), to where its path leaves the box.
	const vec3 L = u_sunDirection.xyz;
	vec3 sunG = quat_transform(L, vec4(-in_voxelQuat.xyz, in_voxelQuat.w)) / in_voxelWorldSize; // grid units per metre
	sunG = mix(vec3(1e-6), sunG, greaterThan(abs(sunG), vec3(1e-6)));
	const vec3 toFace = (step(vec3(0.0), sunG) - hitG) / sunG;
	const float exitDist = max(min(min(toFace.x, toFace.y), toFace.z), 0.0);
	float sunTransmit = 1.0;
	if (u_foliage_voxelSelfShadow > 0.0)
	{
		const float sunLod = min(lod + 1.0, maxLod);
		const float sunN = res * exp2(-sunLod);
		const float sunStep = voxel0 * exp2(sunLod);
		// Empty space skipped two levels coarser (voxelSkip), tested only while the last sample was empty: the path out
		// of the crown often crosses the box's empty corners.
		const float sunSkipLod = min(sunLod + 2.0, maxLod);
		const int sunSteps = min(int(exitDist / sunStep) + 1, 64);
		float d = 0.5 * sunStep;
		bool sunEmpty = false; // the hit's own voxel holds cover
		for (int i = 0; i < sunSteps && sunTransmit > 0.01 && d <= exitDist; ++i)
		{
			const vec3 g = hitG + sunG * d;
			if (sunEmpty && sunSkipLod > sunLod)
			{
				const float skip = voxelSkip(albedoTex, g, sunG, res, tilesX, sunSkipLod, u_foliage_voxelSamplerBias);
				if (skip > 0.0)
				{
					d += skip;
					continue;
				}
			}
			const float a = voxelTapScene(albedoTex, g, sunN, tilesX, sunLod).a;
			sunEmpty = a <= VOXEL_SKIP_EMPTY;
			sunTransmit *= 1.0 - a;
			d += sunStep;
		}
	}

	// The cascades' shadow (and the terrain's, the clouds'). With the SELF-SHADOW on ("Voxel self shadow" > 0): where the
	// sun path leaves the box - the march above already darkens by the tree's own leaves, and its own caster (the voxel
	// grid with "Trees/Voxel shadows", else the billboard, shifted toward the sun by "Billboard shadow shift") lies inside
	// the box: it would shadow it twice. With it OFF: at the hit itself, so the tree takes its own caster's shadow from the
	// cascades as a billboard tree does.
	const float casterShift = u_foliage_voxelShadows > 0.5 ? 0.0 : max(u_foliage_billboardShadowShift, 0.0);
	const vec3 shadowPos = u_foliage_voxelSelfShadow > 0.0 ? pos + L * (exitDist + casterShift + 0.5) : pos;
	g_shadowFoliage = true;
	g_sunShadowFirst = sunShadowVisibility(shadowPos, L);
	g_shadowFoliage = false;
	const float sunVisibility = g_sunShadowFirst; // before the self-shadow (the transmission's own share below)
	const float selfSun = pow(max(sunTransmit, 1e-4), u_foliage_voxelSelfShadow);
	const float selfTransmit = pow(max(sunTransmit, 1e-4), u_foliage_voxelSelfShadow * u_foliage_voxelTransmissionSelfShadow);
	g_sunShadowFirst *= selfSun;
	g_noRtao = true;

	// THE INTERIOR (the normal atlas's A) as the billboards': an AO on the ambient, remapped by "Foliage interior depth
	// start / end" x "Foliage interior shadow". The sun takes the real self-shadow instead.
	const float interior = 1.0 - u_foliage_interiorShadow * smoothstep(u_foliage_interiorStart, u_foliage_interiorEnd, normalTap.w);
	const float16_t surfaceAO = float16_t(interior);

	// "Trees/Voxel saturation": about the colour's luminance (linear) - the bake's averages read greyer than the mesh leaves.
	// Then x "Trees/Voxel brightness".
	const float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
	albedo = max(vec3(luma) + (albedo - vec3(luma)) * u_foliage_voxelSaturation, vec3(0.0)) * u_foliage_voxelBrightness;
	const bool leaf = (material.flags & MATERIAL_FLAG_LEAF) != 0u;
	const f16vec3 materialColor = terrainFactionAlbedo(f16vec3(leaf ? leafPatchTint(albedo, pos) : albedo), pos.xz,
		leaf ? 1.0 : u_terrain_factionSolidScale);
	const vec3 Vh = normalize(u_viewPos - pos);

	// LEAVES seen edge-on: N bent toward the viewer until N.V reaches "Foliage min N.V" (the foliage FS's).
	if (leaf)
	{
		const float noV = dot(vec3(N), Vh);
		if (noV < u_foliage_minNoV)
			N = f16vec3(normalize(vec3(N) + Vh * (u_foliage_minNoV - noV)));
	}

	// LEAF TRANSMISSION, as the foliage cards': a diffuse back term + the forward glow, x the sun's shadow by "Foliage
	// transmission shadow" and the self-shadow at "Voxel transmission self shadow"'s share. The "Foliage transmission"
	// strength, glow and focus x the voxel tier's own "Voxel transmission ..." factors.
	// THE BACK TERM by the voxel's leaves: its mean normal points out of the crown, so -N.L alone lit only the crown's
	// underside (under a high sun the top and the sides face it), while a mesh crown's leaves face every way and glow all
	// over. Leaves facing every way (two-sided, the face the viewer sees): on average 0.25 x (1 - V.L) - 0 with the sun
	// behind the viewer, 0.5 straight ahead; aligned leaves: -N.L, N turned to the seen side. Blended by the mean normal's
	// length.
	if (leaf)
	{
		const vec3 seenN = dot(vec3(N), Vh) >= 0.0 ? vec3(N) : -vec3(N);
		const float back = mix(0.25 * (1.0 - dot(Vh, L)), max(-dot(seenN, L), 0.0), leafAgree);
		const float glow = pow(max(-dot(Vh, L), 0.0), u_foliage_transmissionFocus * u_foliage_voxelTransmissionFocus)
			* (u_foliage_transmissionGlow * u_foliage_voxelTransmissionGlow);
		const float visibility = mix(1.0, sunVisibility, u_foliage_transmissionShadow) * selfTransmit;
		const float through = (back + glow) * visibility;
		if (through > 0.0)
			g_litExtraLight = f16vec3(min(leafTransmitColor(vec3(materialColor))
				* (through * u_foliage_transmission * u_foliage_voxelTransmission * INV_PI)
				* (u_sunTransmittance * u_sunColor.rgb), vec3(MEDIUMP_FLT_MAX)));
	}

	g_leafSkyWrap = float16_t(leaf ? u_foliage_leafSkyWrap : 0.0);
	const vec3 color = computeLitColor(pos, Vh, N, materialColor, float16_t(0.65), float16_t(0.0), surfaceAO);
	out_color = vec4(color, 1.0);
	out_motion = voxelMotionVector(in_prevWorldDelta, depth, 1.0 / clip.w);
}
