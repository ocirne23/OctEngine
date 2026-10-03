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
layout (location = 4) in vec3 in_prevWorldDelta; // the motion vectors (instanced_indirect.vs.glsl)
#ifdef FOLIAGE
layout (location = 5) in flat vec3 in_instanceOrigin; // a point on a FOLIAGE card's axis (foliageCrownNormal)
#endif
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;
#ifndef NO_MOTION_OUTPUT // the transparent variant: its DGC set's fragment interface is location 0 only
layout (location = 1) out vec4 out_motion; // the scene's motion target
#endif

// The lit core's AO read follows this point to where it was last frame (read where it is used, so the
// interpolant is not live across the sun's shadow search).
#define MOTION_WORLD_DELTA in_prevWorldDelta
// The sun's shadow at the top of main, before the material is read (see instanced_indirect_lit.inc.glsl).
#define SUN_SHADOW_FIRST
// VARIANTS: ALPHA_MASK = LitMasked (the alpha + distance-fade discards, the leaf transmission); with FOLIAGE too =
// LitFoliage, the tree billboard cards (MATERIAL_FLAG_BILLBOARD) only: their card frame, edge fade, shadow, crown
// normal and interior. Split so the card code no longer sets the register count of every alpha-tested mesh.
#ifdef FOLIAGE
#define SHADOW_FOLIAGE_BIAS // shadows.inc.glsl: the foliage cards' own bias (sunShadowFirstFoliage)
#define FOLIAGE_NO_RTAO     // instanced_indirect_lit.inc.glsl: g_noRtao
#endif
#include "instanced_indirect_lit.inc.glsl"

// THE SHADOW FIRST - the register peak - while only the position and the geometric normal are live. The geometric
// normal also carries the shadow's normal offset (a normal map must not bend the bias). A surface facing away from
// the sun gets none; doSunLight's facing test on the shading normal still applies.
// Measured 2026-09-28: lit FS 72/16 -> 64/48 (regs / spill B); game mode (64 units) Static meshes 0.281/0.284 ->
// 0.257/0.263 ms, GPU frame 1.553/1.562 -> 1.508/1.515.
void sunShadowFirst(vec3 pos)
{
	const vec3 geoN = normalize(in_normalV.xyz);
	g_sunShadowFirst = dot(geoN, u_sunDirection.xyz) > 0.0 ? sunShadowVisibility(pos, geoN) : 0.0;
}

#ifdef ALPHA_MASK
// LEAVES (MATERIAL_FLAG_LEAF, the meshes): thin and two-sided, so a leaf facing AWAY from the sun is still lit
// through - its transmission needs its real shadow. sunShadowFirst's facing reject gave every such leaf 0, the
// very leaves the transmission's back term lights, so "Foliage transmission shadow" scaled them all as one. The
// lookup is taken from the leaf's sun side (the bias along the geometric normal flipped toward the sun);
// doSunLight's facing test on the shading normal still keeps the front light off a back-facing leaf.
// Any other masked surface keeps sunShadowFirst's facing reject. ONE call of sunShadowVisibility for both: each
// call site inlines the whole shadow search (two cost LitMasked ~20 KB of code).
void sunShadowFirstMasked(vec3 pos, bool leaf)
{
	const vec3 geoN = normalize(in_normalV.xyz);
	const bool facing = dot(geoN, u_sunDirection.xyz) > 0.0;
	g_sunShadowFirst = leaf || facing ? sunShadowVisibility(pos, facing ? geoN : -geoN) : 0.0;
}
#endif

#ifdef FOLIAGE
// FOLIAGE (MATERIAL_FLAG_BILLBOARD - the tree billboard cards): one flat geometric normal stands for a whole
// clump, and rejecting by it darkened the entire card whenever the sun was behind it. Shadow it from the card's
// sun side instead (the bias still follows the geometric normal, just flipped toward the sun) and let
// doSunLight's facing test on the normal-mapped normal decide. The lookup stays on the card plane, as the flat
// caster (shadow_depth.fs.glsl writes no depth).
// Returns the leaf's offset off the card to the texel's baked depth (the normal map's alpha, signed along the card's
// FRONT normal, in units of the card's u length; world, unscaled): the interior term reads the leaf's real 3D point.
// `cardDu` / `cardDv` = d(pos)/du, d(pos)/dv (pos and uv are affine over the card); front normal = cross(dv, du) on
// both faces (billboardViews).
vec3 sunShadowFirstFoliage(vec3 pos, float depth01, vec3 cardDu, vec3 cardDv)
{
	const vec3 geoN = normalize(in_normalV.xyz);
	const vec3 frontN = cross(cardDv, cardDu);
	const float frontLen = length(frontN);
	const vec3 leafOffset = frontLen > 0.0 ? frontN * ((depth01 * 2.0 - 1.0) * length(cardDu) / frontLen) : vec3(0.0);
	g_shadowFoliage = true; // the constant depth bias only (shadows.inc.glsl)
	g_sunShadowFirst = sunShadowVisibility(pos, dot(geoN, u_sunDirection.xyz) >= 0.0 ? geoN : -geoN);
	g_shadowFoliage = false;
	return leafOffset;
}

// The CROWN normal of a foliage card: where the VIEW RAY hits a sphere around the card's centre. Each crossed card's
// baked normals shade the crown side its own bake view saw, so where card A gives way to card B on screen (their
// crossing axis) the shading split hard; this normal depends on the ray only, so both cards agree. The sphere is
// foliageCrownFrame's (below). Outside the sphere (leaf tips) the normal turns to
// the rim. w = the pixel's distance from the axis / the radius (the blend reaches 1 at the axis, where the cards
// cross and their baked normals disagree most).
// INTERIOR: the leaf's real 3D point (the card point + its baked depth, `leafOffset`) against the same sphere - a
// leaf seen through the gaps deep inside the crown sits near the centre, an outer one near the surface. `interior`
// = the darkening, 1 = none: down to 1 - "Trees/Foliage interior shadow" (u_foliageParams.w) at the centre. An
// AO-like term (no sun direction - the transmitted sun shadow is the directional part), so a fully lit crown is
// not flat.
// The crown frame both use: the axis, the sphere centre on it and the radius. A vertical card (or a module's): the
// axis from the instance origin along +u, the centre at the card's u centre. The whole tree's HORIZONTAL card
// (TreeImpostor billboardHorizontalView): it lies across the axis at mid height, so the axis is its normal and the
// centre its own point on it. Its u spans the tree's height too, so the radius - half the u length - is the vertical
// cards'. False on a degenerate card frame.
// The horizontal card: a MATERIAL_FLAG_BILLBOARD_TOP_CARD material's card whose normal points up (whole trees stand
// upright - yaw only - so their vertical cards' normals are horizontal). Found by geometry, not by its strip.
bool foliageTopCard(vec3 cardDu, vec3 cardDv, uint flags)
{
	const vec3 n = cross(cardDv, cardDu);
	return (flags & MATERIAL_FLAG_BILLBOARD_TOP_CARD) != 0u && n.y * n.y > 0.5 * dot(n, n);
}

bool foliageCrownFrame(vec3 pos, vec3 cardDu, vec3 cardDv, vec2 uv, uint flags, out vec3 axis, out vec3 centre, out float radius)
{
	const float uLen = length(cardDu);
	radius = 0.5 * uLen;
	axis = vec3(0.0, 1.0, 0.0);
	centre = pos;
	if (uLen <= 0.0)
		return false;
	if (foliageTopCard(cardDu, cardDv, flags))
	{
		axis = normalize(cross(cardDv, cardDu));
		centre = in_instanceOrigin + axis * dot(pos - in_instanceOrigin, axis);
		return true;
	}
	axis = cardDu / uLen;
	// The MERGED branch cards (one mesh of many modules' cards per tree): the instance origin is the tree's, not the
	// card's module, so the card carries its axis instead - |tangent.w| = 3 + the texture v of its module's axis line
	// (Procedural billboardMesh, RenderMeshData). The crown centre is the card's u centre moved along v onto that line.
	const float axisCode = abs(in_tangent.w);
	if (axisCode >= 1.5)
		centre = pos + cardDu * (0.5 - uv.x) + cardDv * (axisCode - 3.0 - uv.y);
	else
		centre = in_instanceOrigin + axis * dot(pos + cardDu * (0.5 - uv.x) - in_instanceOrigin, axis);
	return true;
}

vec4 foliageCrownNormal(vec3 pos, vec3 V, vec3 cardDu, vec3 cardDv, vec2 uv, uint flags, vec3 leafOffset, out float interior)
{
	interior = 1.0;
	vec3 axis, centre;
	float radius;
	if (!foliageCrownFrame(pos, cardDu, cardDv, uv, flags, axis, centre, radius))
		return vec4(V, 1.0);
	const vec3 D = -V;
	const vec3 w = (centre - pos) + D * distance(u_viewPos, pos); // centre - camera, kept small-valued
	const vec3 offset = D * dot(w, D) - w;                       // the ray's closest approach, from the centre
	const float r2 = dot(offset, offset) / (radius * radius);
	const vec3 fromAxis = (pos - centre) - axis * dot(pos - centre, axis);
	const float leafR = length(pos + leafOffset - centre) / radius;
	interior = mix(1.0 - u_foliageParams.w, 1.0, smoothstep(u_foliageParams2.w, max(u_foliageParams3.x, u_foliageParams2.w + 1e-3), leafR));
	// On the horizontal card ^ "Foliage interior shadow top card scale" (u_foliageParams3.z): an EXPONENT, so > 1
	// darkens it at any strength (a multiplier on the strength saturated at 1).
	// x |V.y|, the view's steepness: from BELOW V.y is negative - a negative factor on the AO and the sun turned the
	// card black (from level: 0).
	if (foliageTopCard(cardDu, cardDv, flags))
		interior = pow(max(interior, 0.0), max(u_foliageParams3.z, 0.01)) * abs(V.y); // pow(0, 0) is undefined
	return vec4(normalize(offset / radius - D * sqrt(max(1.0 - r2, 0.0))), length(fromAxis) / radius);
}

// The pixel's distance from a foliage card's axis / the crown radius (the sphere of foliageCrownNormal).
float foliageAxisDistance(vec3 pos, vec3 cardDu, vec3 cardDv, vec2 uv, uint flags)
{
	vec3 axis, centre;
	float radius;
	if (!foliageCrownFrame(pos, cardDu, cardDv, uv, flags, axis, centre, radius))
		return 1.0;
	const vec3 toPos = pos - centre;
	return length(toPos - axis * dot(toPos, axis)) / radius;
}
#endif

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex); // per-eye reconstruction (AO upsample) + view pos
#endif
	const vec3 pos = in_posU.xyz;
#ifndef ALPHA_MASK
	sunShadowFirst(pos);
#endif
	const vec3 V = normalize(u_viewPos - pos);

	const uint16_t materialIdx   = uint16_t((in_meshIdxMaterialIdx & 0xFFFF0000) >> 16);
	const MaterialInfo material  = in_materialInfos[materialIdx];
	const uint16_t diffuseTexIdx = uint16_t(material.diffuseNormalTexIdx & 0x0000FFFF);
	const uint16_t normalTexIdx  = uint16_t((material.diffuseNormalTexIdx & 0xFFFF0000) >> 16);
	const uint16_t metalRoughnessTexIdx = uint16_t(material.metalRoughnessTexIdxAlphaMode & 0x0000FFFF);
	const vec2 uv = vec2(in_posU.w, in_normalV.w);

	const vec4 diffuseSample  = texture(u_textures[diffuseTexIdx], uv);
#ifdef FOLIAGE
	// The card frame from the screen derivatives (taken before any discard): d(pos)/du and d(pos)/dv.
	const vec2 uvDx = dFdx(uv), uvDy = dFdy(uv);
	const float uvDet = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
	vec3 cardDu = vec3(0.0), cardDv = vec3(0.0);
	if (abs(uvDet) > 1e-20)
	{
		const vec3 pdx = dFdx(pos), pdy = dFdy(pos);
		cardDu = (pdx * uvDy.y - pdy * uvDx.y) / uvDet;
		cardDv = (pdy * uvDx.x - pdx * uvDy.x) / uvDet;
	}
#endif
#ifdef ALPHA_MASK
	// Only the LitMasked / LitFoliage variants discard: a discard anywhere in the shader costs the pipeline its early
	// depth write. The alpha-mode test stays, because a material override can put an opaque material here.
	const uint16_t alphaMode = uint16_t((material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000) >> 16);
	if (alphaMode == ALPHA_MODE_MASK && diffuseSample.a < material.opacity)
		discard;
	// DISTANCE FADE (Layout.ixx makeDistanceFadeFlags): a dithered fade over a camera-distance band. A fade-out
	// surface keeps the pixels with dither < 1 - f, a fade-in one those with dither >= 1 - f, so an out/in pair
	// over the same band shares no pixel (the tree mesh -> billboard crossfade). Interleaved gradient noise,
	// shifted per frame so TAA resolves the dither into a smooth blend.
	if ((material.flags & MATERIAL_FLAG_DISTANCE_FADE) != 0u)
	{
		const float fadeStart = float(material.flags & 0xFFFu);
		const float fadeWidth = max(float((material.flags >> 12) & 0x3FFu), 1.0);
		const float f = clamp((distance(u_viewPos, pos) - fadeStart) / fadeWidth, 0.0, 1.0);
		const vec2 pixel = gl_FragCoord.xy + 5.588238 * float(u_frameIndex & 7u);
		const float dither = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
		if (((material.flags & MATERIAL_FLAG_FADE_IN) != 0u) ? dither < 1.0 - f : dither >= 1.0 - f)
			discard;
	}
	// FOLIAGE cards (the tree billboards) fade out as they turn EDGE-ON: a near-grazing card smears its texture
	// into bright streaks, and the crossed card faces the view right then. A dither of its own (a decorrelated
	// pattern), so it composes with the distance fade instead of cancelling it. Over |N.V| from "Trees/Foliage edge
	// fade start" to "end" (u_foliageParams2.xy), both x "Foliage edge fade centre scale" (z) at the crossing axis, back to x1
	// at half the crown radius; on a whole tree's horizontal card also x "Foliage edge fade top card scale"
	// (u_foliageParams3.y). Not on MATERIAL_FLAG_NO_EDGE_FADE (the mid tier's branch cards).
#ifdef FOLIAGE
	if ((material.flags & MATERIAL_FLAG_NO_EDGE_FADE) == 0u)
	{
		const float facing = abs(dot(normalize(in_normalV.xyz), V));
		const bool topCard = foliageTopCard(cardDu, cardDv, material.flags);
		const float centreScale = mix(u_foliageParams2.z, 1.0, smoothstep(0.0, 0.5, foliageAxisDistance(pos, cardDu, cardDv, uv, material.flags)))
			* (topCard ? u_foliageParams3.y : 1.0);
		const float fadeStart = u_foliageParams2.x * centreScale;
		// Not from BELOW: looking up into a crown, the edge-on cards are what fills it - fading them left it see-through.
		// The fade blends out as the view turns upward, over the first ~11 degrees below the pixel (V.y = 0 .. -0.2).
		const float fromBelow = smoothstep(0.0, 0.2, -V.y);
		const float visibility = mix(smoothstep(fadeStart, max(u_foliageParams2.y * centreScale, fadeStart + 1e-3), facing), 1.0, fromBelow);
		if (visibility < 1.0)
		{
			const vec2 pixel = gl_FragCoord.xy + vec2(17.0, 31.0) + 5.588238 * float((u_frameIndex + 3u) & 7u);
			const float dither = fract(52.9829189 * fract(dot(pixel, vec2(0.00583715, 0.06711056))));
			if (dither >= visibility)
				discard;
		}
	}
	// After the discard: a cut-out pixel pays no shadow.
	const vec3 leafOffset = sunShadowFirstFoliage(pos, texture(u_textures[normalTexIdx], uv).a, cardDu, cardDv);
#else
	// After the discard: a cut-out pixel pays no shadow.
	sunShadowFirstMasked(pos, (material.flags & MATERIAL_FLAG_LEAF) != 0u);
#endif
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
	f16vec3 N = normalize(T * tangentNormal.x + B * tangentNormal.y + geoN * tangentNormal.z);
	float16_t surfaceAO = float16_t(1.0);
#ifdef ALPHA_MASK
	const float sunVisibility = g_sunShadowFirst; // before the interior term (the leaf transmission's shadow)
#endif
#ifdef FOLIAGE
	// FOLIAGE: toward the crown normal - fully at the crossing axis, by "Trees/Foliage crown normal"
	// (u_foliageParams.y) from half the radius out - and the crown INTERIOR darkening on both the sun and the
	// ambient (in place of the RTAO a card does not read).
	{
		float interior;
		const vec4 crown = foliageCrownNormal(pos, V, cardDu, cardDv, uv, material.flags, leafOffset, interior);
		const vec3 blended = mix(vec3(N), crown.xyz, mix(1.0, u_foliageParams.y, smoothstep(0.0, 0.5, crown.w)));
		N = f16vec3(blended * inversesqrt(max(dot(blended, blended), 1e-8))); // opposite normals can cancel
		g_noRtao = true;
		g_sunShadowFirst *= interior;
		surfaceAO = float16_t(interior);
	}
#endif
#ifdef ALPHA_MASK
	// LEAF TRANSMISSION (MATERIAL_FLAG_LEAF): thin leaves let the sun through, tinted by their own colour - a
	// diffuse back term saturate(-N.L) (lit from behind) plus a forward GLOW saturate(V.-L)^focus x glow (looking
	// toward the sun: the backlit rim). x "Trees/Foliage transmission" (u_foliageParams3.w). Its visibility leans
	// on the sun shadow by "Foliage transmission shadow" (u_foliageParams4.z) only - a leaf seen from the shaded
	// side sits in its own crown's shadow, which would leave the backlit view no glow - then x the interior term,
	// so the crown's depths stay dark.
	// Formed BEFORE computeLitColor: its light loop is the shader's register peak, and only this half colour is live
	// across it - not the shadow, the interior term and the surface colour it is made of (LitFoliage 72 registers).
	f16vec3 transmit = f16vec3(0.0);
	if ((material.flags & MATERIAL_FLAG_LEAF) != 0u && u_foliageParams3.w > 0.0)
	{
		const vec3 L = u_sunDirection.xyz;
		const float back = max(-dot(vec3(N), L), 0.0);
		const float glow = pow(max(-dot(V, L), 0.0), u_foliageParams4.x) * u_foliageParams4.y;
		const float visibility = mix(1.0, sunVisibility, u_foliageParams4.z) * float(surfaceAO);
		transmit = materialColor * float16_t(min((back + glow) * visibility * u_foliageParams3.w * INV_PI, MEDIUMP_FLT_MAX));
	}
#endif

	vec3 color = computeLitColor(pos, V, N, materialColor, roughness, metalness, surfaceAO);
#ifdef ALPHA_MASK
	color += vec3(transmit) * (u_sunTransmittance * u_sunColor.rgb);
#endif
#if defined(TREE_DEBUG) && TREE_DEBUG != 0
	// "Trees/Debug view" (baked; StaticMeshGraphicsPipeline): a flat colour per MATERIAL (1) or per MESH (2: the LOD
	// level the cull picked), or the distance FADE side (3: red = fade-out, green = fade-in, white = none) - over the
	// lit brightness, so the shapes still read. A snap is a colour change: which kind tells the cause.
	{
#if TREE_DEBUG == 3
		const vec3 debugColor = (material.flags & MATERIAL_FLAG_DISTANCE_FADE) == 0u ? vec3(1.0)
			: (material.flags & MATERIAL_FLAG_FADE_IN) != 0u ? vec3(0.1, 1.0, 0.1) : vec3(1.0, 0.1, 0.1);
#else
		uint h = TREE_DEBUG == 1 ? uint(materialIdx) * 2654435761u + 7u : (in_meshIdxMaterialIdx & 0xFFFFu) * 2246822519u + 3u;
		h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
		const vec3 debugColor = vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0 * 0.8 + 0.2;
#endif
		const float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
		color = debugColor * (0.35 + 0.65 * clamp(lum * 4.0, 0.0, 1.0));
	}
#endif
	out_color = vec4(color, min(diffuseSample.a, material.opacity));
#ifndef NO_MOTION_OUTPUT
	out_motion = motionVector(in_prevWorldDelta);
#endif
}
