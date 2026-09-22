// --- Terrain texture splatting (setTerrainSplatMaterials; u_terrainTexParams*/u_terrainSplatClimate) ---
// Four physical layers composited bottom-up - no biome enum, climate selects textures directly:
//   1. GROUND - climate-picked soil/vegetation, world-XZ projection
//   2. BEACH  - shoreline band just above the local waterline (not climate-selected)
//   3. ROCK   - bedrock where too steep / too prominent to hold soil; climate-picked type, triplanar
//   4. SNOW   - cover over ALL of the above (snow falls ON bedrock - as a ground type the rock layer
//               would paint over it and peaks would come out gray)
//
// Shared by the terrain fragment shader (its own pixels) and the ocean shader (the seabed at a
// refraction-ray hit, so the sand seen through the water IS the terrain next to it). The includer
// declares, before including:
//   in_materialInfos[] (MaterialInfo: flags / diffuseNormalTexIdx / metalRoughnessTexIdxAlphaMode),
//   u_textures[] + GL_EXT_nonuniform_qualifier, the UBO (u_terrainTexParams*, u_terrainSplatClimate,
//   u_terrainParams).
// Optional, before including:
//   TERRAIN_SPLAT_TEX(tex, uv)  - the texture fetch. Defaults to texture() (screen derivatives); a ray
//                                 hit has none, so the ocean defines it as textureLod at a ray-cone LOD.
//   TERRAIN_SPLAT_ALBEDO_ONLY   - skip the normal + ARM taps (the seabed only needs colour: the water
//                                 column blurs any detail normal away). TerrainSample.normal is then the
//                                 geometric normal and rough/metal/ao are constants.
//   TERRAIN_SPLAT_RELIEF        - the splat HEIGHT maps (u_terrainSplatHeightTex, u_terrainTexParams6/7):
//                                 the height blend at layer borders plus parallax occlusion mapping near the
//                                 camera. Needs screen derivatives (the terrain FS only); without it every
//                                 layer blend is linear, so the ocean's seabed is the terrain minus relief.
//                                 Writes the relief self-shadow to g_sunVisMaterial (the lit core's).
//   TERRAIN_POM (0/1)           - (with RELIEF) the parallax march + its self-shadow. BAKED by
//                                 StaticMeshGraphicsPipeline from "Terrain/Textures/Parallax"; 0 compiles
//                                 them out (the height blend stays). Defaults to 0.
//   TERRAIN_SPLAT_HEIGHT_ONLY   - (with RELIEF) only the coverages (terrainLayers) and the height composite
//                                 (terrainReliefAt): no material sampling, no screen derivatives - for the
//                                 tessellation evaluation shader, which declares only the UBO + u_textures.
// Requires GL_EXT_shader_explicit_arithmetic_types: the splat is HALF math - the samples, the layer
// coverages and the blend weights (all in [0, 1]) and the normals. Positions, UVs, the climate match and
// the crag fBm (a hash: it needs the 32-bit mantissa) stay 32-bit.

#ifndef TERRAIN_SPLAT_INC_GLSL
#define TERRAIN_SPLAT_INC_GLSL

#ifndef TERRAIN_SPLAT_TEX
#define TERRAIN_SPLAT_TEX(tex, uv) texture(tex, uv)
#endif
#ifndef TERRAIN_POM
#define TERRAIN_POM 0
#endif

struct TerrainFields
{
	float altitude;    // macro altitude (m above sea level)
	float temperature; // Celsius
	float humidity;    // [0,1]
	float waterLevel;  // world Y of the local water surface
};

struct TerrainSample
{
	f16vec3 albedo;
	f16vec3 normal;   // world space
	float16_t rough;  // GGX alpha, >= 0.01 (the fp16 BRDF's floor)
	float16_t metal;
	float16_t ao;
	float16_t height; // relief 0..1, 1 = top (the mesh surface); 0.5 without TERRAIN_SPLAT_RELIEF or a height map
};

#ifdef TERRAIN_SPLAT_RELIEF
uint terrainHeightTexIdx(uint matIdx)
{
	const uint slot = matIdx - uint(u_terrainTexParams0.x);
	return u_terrainSplatHeightTex[slot >> 2][slot & 3u];
}

// The parallax march runs in non-uniform flow: explicit gradients (the pixel's own, scaled to the layer's UV).
float16_t terrainHeightGrad(uint matIdx, vec2 uv, vec2 dx, vec2 dy)
{
	const uint texIdx = terrainHeightTexIdx(matIdx);
	return texIdx != 0xFFFFu ? float16_t(textureGrad(u_textures[nonuniformEXT(texIdx)], uv, dx, dy).r) : float16_t(0.5);
}

#ifndef TERRAIN_SPLAT_HEIGHT_ONLY
float16_t terrainHeightTap(uint matIdx, vec2 uv)
{
	const uint texIdx = terrainHeightTexIdx(matIdx);
	return texIdx != 0xFFFFu ? float16_t(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(texIdx)], uv).r) : float16_t(0.5);
}
#endif

// Height blend: coverage w of a layer standing dh above the one beneath becomes a steeper ramp shifted by
// dh, so the higher texels win first (sand fills the gaps between stones before it covers them). Keeps
// w = 0 -> 0 and w = 1 -> 1 for any |dh| <= 1; contrast 0 = linear.
float16_t terrainHeightWeight(float16_t w, float16_t dh)
{
	const float16_t k = float16_t(u_terrainTexParams6.w);
	return clamp((w - float16_t(0.5)) * (float16_t(1.0) + k) + dh * (k * float16_t(0.5)) + float16_t(0.5), float16_t(0.0), float16_t(1.0));
}
#endif

#ifndef TERRAIN_SPLAT_HEIGHT_ONLY
// One splat material with world-XZ UVs; tangent basis = world X/Z reoriented onto the geometric
// normal. matIdx diverges between neighbouring pixels at climate borders -> nonuniformEXT.
TerrainSample sampleTerrainXZ(uint matIdx, vec2 uv, f16vec3 geoN)
{
	const MaterialInfo material = in_materialInfos[nonuniformEXT(matIdx)];
	const uint diffuseTexIdx = material.diffuseNormalTexIdx & 0xFFFFu;

	TerrainSample s;
	s.albedo = f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uv).rgb);
#ifdef TERRAIN_SPLAT_RELIEF
	s.height = terrainHeightTap(matIdx, uv);
#else
	s.height = float16_t(0.5);
#endif
#ifdef TERRAIN_SPLAT_ALBEDO_ONLY
	s.ao = float16_t(1.0);
	s.rough = float16_t(0.9);
	s.metal = float16_t(0.0);
	s.normal = geoN;
#else
	const uint normalTexIdx = material.diffuseNormalTexIdx >> 16;
	const uint armTexIdx    = material.metalRoughnessTexIdxAlphaMode & 0xFFFFu;
	const f16vec3 arm = armTexIdx != 0xFFFFu ? f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uv).rgb) : f16vec3(1.0, 0.9, 0.0);
	s.ao = arm.r;
	s.rough = max(arm.g, float16_t(0.01));
	s.metal = arm.b;

	const f16vec3 normalSample = f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uv).xyz);
	f16vec3 tn;
	if ((material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u)
	{
		const f16vec2 nxy = normalSample.xy * float16_t(2.0) - float16_t(1.0);
		tn = f16vec3(nxy, sqrt(max(float16_t(1.0) - dot(nxy, nxy), float16_t(0.0))));
	}
	else
		tn = normalize(normalSample * float16_t(2.0) - float16_t(1.0));
	s.normal = normalize(f16vec3(tn.x, float16_t(0.0), tn.y) + tn.z * geoN);
#endif
	return s;
}

// Decode one triplanar normal-map tap into tangent space (BC5 reconstructs Z).
f16vec3 decodeTriplanarNormal(f16vec3 ns, bool bc5)
{
	if (bc5)
	{
		const f16vec2 xy = ns.xy * float16_t(2.0) - float16_t(1.0);
		return f16vec3(xy, sqrt(max(float16_t(1.0) - dot(xy, xy), float16_t(0.0))));
	}
	return normalize(ns * float16_t(2.0) - float16_t(1.0));
}

// Triplanar version for the rock layer (whiteout-style normal blend), so cliff faces don't smear.
// PLANE SKIPPING: the three projection weights sum to 1, so a plane below TRIPLANAR_WMIN contributes
// <~4% and is dropped (its texture taps skipped) - on shallow crag one axis dominates and the other two
// are near-zero, cutting up to 2/3 of the taps. Surviving weights are renormalised so no energy is lost.
// The branch is coherent across a quad except on the exact plane-crossover diagonal, where one pixel may
// get a slightly wrong mip; invisible in practice.
#define TRIPLANAR_WMIN 0.05
TerrainSample sampleTerrainTriplanar(uint matIdx, vec3 worldPos, f16vec3 geoN, float uvScale)
{
	const MaterialInfo material = in_materialInfos[nonuniformEXT(matIdx)];
	const uint diffuseTexIdx = material.diffuseNormalTexIdx & 0xFFFFu;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
	const uint normalTexIdx  = material.diffuseNormalTexIdx >> 16;
	const uint armTexIdx     = material.metalRoughnessTexIdxAlphaMode & 0xFFFFu;
	const bool bc5 = (material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u;
	const bool hasArm = armTexIdx != 0xFFFFu;
#endif

	f16vec3 w = abs(geoN);
	w = w / (w.x + w.y + w.z);
	const bvec3 use = greaterThan(w, f16vec3(TRIPLANAR_WMIN));
	w /= dot(w, f16vec3(use)); // renormalise over the kept planes
	const vec2 uvX = worldPos.zy * uvScale; // plane normal = X
	const vec2 uvY = worldPos.xz * uvScale; // plane normal = Y
	const vec2 uvZ = worldPos.xy * uvScale; // plane normal = Z

	f16vec3 albedo = f16vec3(0.0);
	f16vec3 arm = f16vec3(0.0);
	f16vec3 nrm = f16vec3(0.0);
	if (use.x)
	{
		albedo += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvX).rgb) * w.x;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const f16vec3 tn = decodeTriplanarNormal(f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvX).xyz), bc5);
		nrm += f16vec3(tn.z * sign(geoN.x), tn.y, tn.x) * w.x;
		if (hasArm) arm += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvX).rgb) * w.x;
#endif
	}
	if (use.y)
	{
		albedo += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvY).rgb) * w.y;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const f16vec3 tn = decodeTriplanarNormal(f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvY).xyz), bc5);
		nrm += f16vec3(tn.x, tn.z * sign(geoN.y), tn.y) * w.y;
		if (hasArm) arm += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvY).rgb) * w.y;
#endif
	}
	if (use.z)
	{
		albedo += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvZ).rgb) * w.z;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const f16vec3 tn = decodeTriplanarNormal(f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvZ).xyz), bc5);
		nrm += f16vec3(tn.x, tn.y, tn.z * sign(geoN.z)) * w.z;
		if (hasArm) arm += f16vec3(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvZ).rgb) * w.z;
#endif
	}

	TerrainSample s;
	s.albedo = albedo;
	// The height is the TOP plane's alone (uvY), for every slope: the parallax march works in world XZ, and on
	// the steep faces where the other planes carry the colour the rock is opaque, so no blend reads it.
#ifdef TERRAIN_SPLAT_RELIEF
	s.height = terrainHeightTap(matIdx, uvY);
#else
	s.height = float16_t(0.5);
#endif
#ifdef TERRAIN_SPLAT_ALBEDO_ONLY
	s.ao = float16_t(1.0);
	s.rough = float16_t(0.9);
	s.metal = float16_t(0.0);
	s.normal = geoN;
#else
	if (!hasArm)
		arm = f16vec3(1.0, 0.9, 0.0);
	s.ao = arm.r;
	s.rough = max(arm.g, float16_t(0.01));
	s.metal = arm.b;
	s.normal = normalize(nrm + geoN * float16_t(2.0)); // biased toward geoN: detail, not replacement
#endif
	return s;
}
#endif // !TERRAIN_SPLAT_HEIGHT_ONLY

// Crag wander noise (value fBm over world XZ). Lives HERE and not in the generator: the wander must be
// scaled by local relief, which the generator's coarse path cannot supply (its relief is 0 by
// construction - the cascades would disagree). The shader always has the true mesh height.
float terrainHash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float terrainValueNoise(vec2 p)
{
	const vec2 i = floor(p);
	const vec2 f = p - i;
	const vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(terrainHash12(i),                terrainHash12(i + vec2(1.0, 0.0)), u.x),
	           mix(terrainHash12(i + vec2(0.0, 1.0)), terrainHash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// ~[-1, 1], 3 octaves, amplitude-normalised.
float terrainFbm(vec2 p)
{
	float v = 0.0, a = 0.5, norm = 0.0;
	for (int i = 0; i < 3; ++i)
	{
		v += a * terrainValueNoise(p);
		norm += a;
		p *= 2.0;
		a *= 0.5;
	}
	return (v / norm) * 2.0 - 1.0;
}

// Lay `s` over `acc` with coverage `w` (height-blended under TERRAIN_SPLAT_RELIEF).
void terrainMixInto(inout TerrainSample acc, TerrainSample s, float16_t w)
{
#ifdef TERRAIN_SPLAT_RELIEF
	w = terrainHeightWeight(w, s.height - acc.height);
#endif
	acc.height = mix(acc.height, s.height, w);
	acc.albedo = mix(acc.albedo, s.albedo, w);
	acc.normal = mix(acc.normal, s.normal, w);
	acc.rough  = mix(acc.rough, s.rough, w);
	acc.metal  = mix(acc.metal, s.metal, w);
	acc.ao     = mix(acc.ao, s.ao, w);
}

// Match of `climate` against an entry's climate BOX: 1 inside, Gaussian-decaying outside. A box (not a
// point) lets an entry opt OUT of an axis by leaving that range at full width ("cold at ANY humidity").
float climateBoxWeight(vec2 climate, vec4 box, float invS2)
{
	const vec2 d = max(max(box.xz - climate, climate - box.yw), vec2(0.0));
	return exp(-dot(d, d) * invS2);
}

struct ClimatePick
{
	int i0, i1, i2;    // top three entries (0-based like u_terrainSplatClimate; caller adds baseMat)
	float16_t n1, n2;  // normalized coverage of i1 and i2; the top pick i0 gets the rest (n0 = 1 - n1 - n2)
};

// Top THREE climate entries in [first, first + count) as a partition of unity, each weighted RELATIVE to
// the FOURTH (w - w3): when the #3/#4 ranking swaps both sit at zero contribution, so the third texture
// fades in and out rather than popping (the generalisation of the old top-two's relative-to-third trick).
// Three real entries - not two - is what lets a pixel where three climate boxes overlap show all three
// instead of pinching the loser into a hard sliver.
ClimatePick pickClimate(vec2 climate, int first, int count, float invS2)
{
	int i0 = first, i1 = first, i2 = first;
	float w0 = -1.0, w1 = -1.0, w2 = -1.0, w3 = -1.0;
	for (int i = first; i < first + count; ++i)
	{
		const float w = climateBoxWeight(climate, u_terrainSplatClimate[i], invS2);
		if      (w > w0) { i2 = i1; i1 = i0; i0 = i; w3 = w2; w2 = w1; w1 = w0; w0 = w; }
		else if (w > w1) { i2 = i1; i1 = i;         w3 = w2; w2 = w1; w1 = w; }
		else if (w > w2) { i2 = i;                  w3 = w2; w2 = w; }
		else if (w > w3) {                          w3 = w; }
	}
	w3 = max(w3, 0.0); // count < 4: nothing to subtract
	const float a0 = w0 - w3;            // >= 0: w0 is the maximum
	const float a1 = max(w1 - w3, 0.0);
	const float a2 = max(w2 - w3, 0.0);
	const float inv = 1.0 / max(a0 + a1 + a2, 1e-6); // max: all weights can underflow to 0
	return ClimatePick(i0, i1, i2, float16_t(a1 * inv), float16_t(a2 * inv));
}

// A layer samples the top climate pick, then blends i1/i2 in ONLY when their coverage clears this - so
// the flat interior of a climate box stays one sample, a two-box border costs two, and only a genuine
// three-box junction pays for three. Sequential mix (i1 at n1/(1-n2), then i2 at n2) reproduces the
// weighted sum n0*s0 + n1*s1 + n2*s2 exactly; a skipped small weight just folds into s0.
#define TERRAIN_BLEND_EPS 0.004

// A layer at/above this coverage hides everything beneath it (< 0.3% contribution): the buried layers'
// texture taps are skipped entirely and the covering layer REPLACES the accumulator instead of mixing.
#define TERRAIN_LAYER_OPAQUE 0.997

// The four layer coverages and the climate picks of one pixel: everything the composite (terrainSplat) and
// the parallax march's height composite (terrainReliefAt) decide from, so the two cannot disagree.
struct TerrainLayers
{
	int baseMat, numGround, numRock;
	uint snowMatIdx;
	float16_t snowW, beachW, rockW;
	ClimatePick g, r; // g read only while the ground shows, r only while the rock does
};

// All four layer coverages are computed FIRST (cheap ALU) so fully buried layers never sample: a
// snow-capped peak collapses to the 3 snow taps, a sheer cliff face skips ground + beach.
TerrainLayers terrainLayers(vec3 worldPos, vec3 geoN, TerrainFields f)
{
	TerrainLayers L;
	L.baseMat = int(u_terrainTexParams0.x);
	L.numGround = int(u_terrainTexParams0.y);
	L.numRock = int(u_terrainTexParams0.z);
	L.g = ClimatePick(0, 0, 0, float16_t(0.0), float16_t(0.0));
	L.r = L.g;
	L.beachW = float16_t(0.0);
	L.rockW = float16_t(0.0);
	const int baseMat = L.baseMat;
	const int numGround = L.numGround;
	const int numRock = L.numRock;
	const float slope = 1.0 - clamp(geoN.y, 0.0, 1.0);

	// --- Coverages ---
	// Snow over everything, but it slides off steep faces (the mountain's rock shows through)
	// and needs humidity to fall at all (no white polar deserts). Evaluated first: full snow cover
	// returns before the beach / rock coverages (the rock fBm in particular) are computed.
	float16_t snowW = float16_t(0.0);
	if (u_terrainTexParams3.y > 0.5)
	{
		const float cold  = 1.0 - smoothstep(u_terrainTexParams3.z, u_terrainTexParams3.w, f.temperature);
		const float holds = 1.0 - smoothstep(u_terrainTexParams4.x, u_terrainTexParams4.y, slope);
		const float wet = smoothstep(0.0, max(u_terrainTexParams4.z, 1e-3), f.humidity);
		snowW = float16_t(cold * holds * wet);
	}
	L.snowMatIdx = uint(baseMat + numGround + numRock) + (u_terrainTexParams3.x > 0.5 ? 1u : 0u);
	L.snowW = snowW;

	// Full snow cover: everything beneath is hidden - the whole splat is the snow sample alone.
	if (snowW >= float16_t(TERRAIN_LAYER_OPAQUE))
		return L;

	// The baked temperature already carries the altitude lapse, so elevation enters the selection as
	// the cold it causes - snow line and vegetation cannot disagree.
	const vec2 climate = vec2(clamp((f.temperature + 25.0) / 75.0, 0.0, 1.0), f.humidity);
	const float invS2 = 1.0 / (2.0 * u_terrainTexParams0.w * u_terrainTexParams0.w);

	// Beach: the band just above the local waterline.
	float16_t beachW = float16_t(0.0);
	if (u_terrainTexParams3.x > 0.5)
		beachW = float16_t(1.0 - smoothstep(0.3, max(u_terrainTexParams2.z, 0.31), worldPos.y - f.waterLevel));

	// Rock: too steep OR standing too far above the macro altitude (crag). max(), not a sum -
	// the two coincide on a cliff. On V3 terrain crag is what puts rock on mountains (the 30 m/px field
	// rarely reaches the slope threshold). The relief is wandered by fBm first or the rock boundary is
	// an elevation contour across a whole range; |wander| <= relief keeps flat lowlands untouched.
	float16_t rockW = float16_t(0.0);
	if (numRock > 0)
	{
		float relief = (worldPos.y - u_terrainParams.z) - f.altitude;
		const float wanderAmp = u_terrainTexParams5.x;
		// The wander moves relief by at most +-amp, so only pixels where that can change the crag
		// smoothstep pay for the 12-hash fBm - saturated flatland (crag 0) and sheer crag (1) skip it.
		if (wanderAmp > 0.0 && relief + wanderAmp > u_terrainTexParams2.x && relief - wanderAmp < u_terrainTexParams2.y)
		{
			const float w = terrainFbm(worldPos.xz * u_terrainTexParams5.y) * wanderAmp;
			relief -= w * clamp(relief / wanderAmp, 0.0, 1.0);
		}
		const float crag = smoothstep(u_terrainTexParams2.x, u_terrainTexParams2.y, relief);
		rockW = float16_t(max(smoothstep(u_terrainTexParams1.z, u_terrainTexParams1.w, slope), crag * 0.85));
	}
	L.beachW = beachW;
	L.rockW = rockW;

	const float16_t opaque = float16_t(TERRAIN_LAYER_OPAQUE);
	const float16_t blendEps = float16_t(TERRAIN_BLEND_EPS);
	if (beachW < opaque && rockW < opaque)
		L.g = pickClimate(climate, 0, numGround, invS2);
	if (rockW > blendEps)
		L.r = pickClimate(climate, numGround, numRock, invS2);
	return L;
}

#ifdef TERRAIN_SPLAT_RELIEF
float16_t terrainMixHeight(float16_t acc, float16_t h, float16_t w)
{
	return mix(acc, h, terrainHeightWeight(w, h - acc));
}

// The composite HEIGHT at world xz: terrainSplat's layer chain on the height maps alone (same skips, same
// height-blended mixes), with explicit gradients - dx / dy are the pixel's world-XZ derivatives. Rock
// reads its top plane, as sampleTerrainTriplanar does. Most pixels are one tap: the flat interior of a
// climate box with no overlay.
float16_t terrainReliefAt(TerrainLayers L, vec2 xz, vec2 dx, vec2 dy)
{
	const float16_t opaque = float16_t(TERRAIN_LAYER_OPAQUE);
	const float16_t blendEps = float16_t(TERRAIN_BLEND_EPS);
	const float sG = u_terrainTexParams1.x, sR = u_terrainTexParams1.y, sS = u_terrainTexParams2.w;
	if (L.snowW >= opaque)
		return terrainHeightGrad(L.snowMatIdx, xz * sS, dx * sS, dy * sS);

	float16_t h = float16_t(0.0);
	if (L.beachW < opaque && L.rockW < opaque)
	{
		h = terrainHeightGrad(uint(L.baseMat + L.g.i0), xz * sG, dx * sG, dy * sG);
		if (L.g.n1 > blendEps)
			h = terrainMixHeight(h, terrainHeightGrad(uint(L.baseMat + L.g.i1), xz * sG, dx * sG, dy * sG), L.g.n1 / max(float16_t(1.0) - L.g.n2, float16_t(1e-4)));
		if (L.g.n2 > blendEps)
			h = terrainMixHeight(h, terrainHeightGrad(uint(L.baseMat + L.g.i2), xz * sG, dx * sG, dy * sG), L.g.n2);
	}
	if (L.beachW > blendEps && L.rockW < opaque)
	{
		const float16_t hb = terrainHeightGrad(uint(L.baseMat + L.numGround + L.numRock), xz * sG, dx * sG, dy * sG);
		h = L.beachW >= opaque ? hb : terrainMixHeight(h, hb, L.beachW);
	}
	if (L.rockW > blendEps)
	{
		float16_t hr = terrainHeightGrad(uint(L.baseMat + L.r.i0), xz * sR, dx * sR, dy * sR);
		if (L.r.n1 > blendEps)
			hr = terrainMixHeight(hr, terrainHeightGrad(uint(L.baseMat + L.r.i1), xz * sR, dx * sR, dy * sR), L.r.n1 / max(float16_t(1.0) - L.r.n2, float16_t(1e-4)));
		if (L.r.n2 > blendEps)
			hr = terrainMixHeight(hr, terrainHeightGrad(uint(L.baseMat + L.r.i2), xz * sR, dx * sR, dy * sR), L.r.n2);
		h = L.rockW >= opaque ? hr : terrainMixHeight(h, hr, L.rockW);
	}
	if (L.snowW > blendEps)
		h = terrainMixHeight(h, terrainHeightGrad(L.snowMatIdx, xz * sS, dx * sS, dy * sS), L.snowW);
	return h;
}

// terrainReliefAt at THREE points at once - xz, xz + (e, 0), xz + (0, e): the forward differences of a normal.
// One walk of the layer chain (its branches, the height-texture index lookups, the scales) with three taps per
// visited layer and the height blends vectorized; the same results as three terrainReliefAt calls.
f16vec3 terrainHeightGrad3(uint matIdx, vec2 uv, float e, vec2 dx, vec2 dy)
{
	const uint texIdx = terrainHeightTexIdx(matIdx);
	if (texIdx == 0xFFFFu)
		return f16vec3(0.5);
	return f16vec3(textureGrad(u_textures[nonuniformEXT(texIdx)], uv, dx, dy).r,
	               textureGrad(u_textures[nonuniformEXT(texIdx)], uv + vec2(e, 0.0), dx, dy).r,
	               textureGrad(u_textures[nonuniformEXT(texIdx)], uv + vec2(0.0, e), dx, dy).r);
}

f16vec3 terrainMixHeight3(f16vec3 acc, f16vec3 h, float16_t w)
{
	const float16_t k = float16_t(u_terrainTexParams6.w);
	const f16vec3 wv = clamp((w - float16_t(0.5)) * (float16_t(1.0) + k) + (h - acc) * (k * float16_t(0.5)) + float16_t(0.5), f16vec3(0.0), f16vec3(1.0));
	return mix(acc, h, wv);
}

f16vec3 terrainReliefAt3(TerrainLayers L, vec2 xz, float e, vec2 dx, vec2 dy)
{
	const float16_t opaque = float16_t(TERRAIN_LAYER_OPAQUE);
	const float16_t blendEps = float16_t(TERRAIN_BLEND_EPS);
	const float sG = u_terrainTexParams1.x, sR = u_terrainTexParams1.y, sS = u_terrainTexParams2.w;
	if (L.snowW >= opaque)
		return terrainHeightGrad3(L.snowMatIdx, xz * sS, e * sS, dx * sS, dy * sS);

	f16vec3 h = f16vec3(0.0);
	if (L.beachW < opaque && L.rockW < opaque)
	{
		h = terrainHeightGrad3(uint(L.baseMat + L.g.i0), xz * sG, e * sG, dx * sG, dy * sG);
		if (L.g.n1 > blendEps)
			h = terrainMixHeight3(h, terrainHeightGrad3(uint(L.baseMat + L.g.i1), xz * sG, e * sG, dx * sG, dy * sG), L.g.n1 / max(float16_t(1.0) - L.g.n2, float16_t(1e-4)));
		if (L.g.n2 > blendEps)
			h = terrainMixHeight3(h, terrainHeightGrad3(uint(L.baseMat + L.g.i2), xz * sG, e * sG, dx * sG, dy * sG), L.g.n2);
	}
	if (L.beachW > blendEps && L.rockW < opaque)
	{
		const f16vec3 hb = terrainHeightGrad3(uint(L.baseMat + L.numGround + L.numRock), xz * sG, e * sG, dx * sG, dy * sG);
		h = L.beachW >= opaque ? hb : terrainMixHeight3(h, hb, L.beachW);
	}
	if (L.rockW > blendEps)
	{
		f16vec3 hr = terrainHeightGrad3(uint(L.baseMat + L.r.i0), xz * sR, e * sR, dx * sR, dy * sR);
		if (L.r.n1 > blendEps)
			hr = terrainMixHeight3(hr, terrainHeightGrad3(uint(L.baseMat + L.r.i1), xz * sR, e * sR, dx * sR, dy * sR), L.r.n1 / max(float16_t(1.0) - L.r.n2, float16_t(1e-4)));
		if (L.r.n2 > blendEps)
			hr = terrainMixHeight3(hr, terrainHeightGrad3(uint(L.baseMat + L.r.i2), xz * sR, e * sR, dx * sR, dy * sR), L.r.n2);
		h = L.rockW >= opaque ? hr : terrainMixHeight3(h, hr, L.rockW);
	}
	if (L.snowW > blendEps)
		h = terrainMixHeight3(h, terrainHeightGrad3(L.snowMatIdx, xz * sS, e * sS, dx * sS, dy * sS), L.snowW);
	return h;
}

#ifndef TERRAIN_SPLAT_HEIGHT_ONLY
// Parallax occlusion mapping, displaced along the GEOMETRIC NORMAL (what tessellation would do): the mesh is
// the top of the relief (height 1), height 0 lies "depth" metres below it along -N. A ray point at depth d
// maps back up along N onto the surface point whose texels it sees, so the texture offset is the ray's
// TANGENTIAL part: -(V - N * NoV) / NoV * d. (A march along world Y instead ignores the base slope: over the
// ray's sideways run the slope itself rises or falls by more than the relief, and slopes broke.)
// One 3D offset serves every layer - each with its own UV scale, the triplanar rock planes too - so one march
// covers the whole composite. Faded with the camera distance and on steep ground (the XZ projection
// stretches there), skipped past the fade end. Returns the texture-position offset; lighting and coverages
// stay on the mesh.
//  - Linear search with steps ~ 1 / NoV (a grazing ray crosses 1 / NoV times more relief per unit of depth),
//    then TERRAIN_POM_BISECT bisection steps on the bracket and a secant inside it: the one secant alone
//    left the linear steps visible as slices at grazing angles.
//  - Relief SELF-SHADOW (Tatarchuk 2006): a second march from the hit up toward the sun in the same
//    tangential frame; the texels standing above the light ray darken it, the nearer ones more (soft
//    penumbra). Written to g_sunVisMaterial, which scales the ground's sun in doSunLight only.
//  - SILHOUETTES (TERRAIN_POM_SILHOUETTE; SPOM for a surface with no UV border): the base surface is CURVED
//    (curved relief mapping, Oliveira & Policarpo 2005). Along the ray's tangential run u the surface falls
//    away by 0.5 k u^2 (k = normal curvature in that direction, > 0 on a crest), so in height fraction the
//    ray is rayH(s) = 1 - s + c s^2. With c > 1/4 it turns back up before the bottom of the relief; a ray
//    that climbs back out of the top (s = 1/c) without a hit passed OVER the crest's relief - the pixel is
//    discarded and whatever is behind the crest shows. The fade scales depth, and c with it, so the
//    silhouette fades out with the parallax.
//    COST: a discard in the ground pass defers its depth write for every terrain pixel. Set it to 0 to
//    compile the discard out (then c = 0 and the march is the flat one).
#define TERRAIN_POM_BISECT 4
#define TERRAIN_POM_SHADOW_STEPS 6
#define TERRAIN_POM_SHADOW_SHARPNESS 6.0
// OFF: on 2 m facets with smooth normals a crest has no clean silhouette - measured by eye, it left holes in
// front of the terrain behind and a strip on the sky. Kept for reference; tessellation is the planned fix.
#ifndef TERRAIN_POM_SILHOUETTE
#define TERRAIN_POM_SILHOUETTE 0
#endif

#if TERRAIN_POM_SILHOUETTE
// Normal curvature of the base surface along the unit tangent direction D, from the screen derivatives of
// the position and the geometric normal (taken by the caller in uniform flow): D expressed in the two
// derivative directions (2x2 least squares), then the normal's change along it. > 0 on a crest.
float terrainCurvatureAlong(vec3 D, vec3 dPx, vec3 dPy, vec3 dNx, vec3 dNy)
{
	const float xx = dot(dPx, dPx), xy = dot(dPx, dPy), yy = dot(dPy, dPy);
	const float det = xx * yy - xy * xy;
	if (abs(det) < 1e-12)
		return 0.0;
	const float dx = dot(dPx, D), dy = dot(dPy, D);
	const float a = (yy * dx - xy * dy) / det;
	const float b = (xx * dy - xy * dx) / det;
	return dot(a * dNx + b * dNy, D);
}
#endif

// curv: the base surface's curvature along the view ray's tangential direction (0 = flat march).
// faceN: the rasterized FACET's normal, facing the camera (geoN when the silhouette is off). The march frame
// eases from the interpolated normal to it as the interpolated one turns edge-on: the facets are chords
// UNDER the smooth surface the vertex normals describe, so along a coarse ridge a mesh-sized band has
// NoV <= 0 on the interpolated normal while its facet still faces the camera - often inside the relief
// shell. Discarding that whole band cut mesh-sized holes (the terrain behind showed through); on the facet
// frame those pixels march like any other and only the curvature test discards: a ray nearly edge-on to its
// facet gets a large c and leaves the shell, the rest hit relief.
vec3 terrainParallaxOffset(TerrainLayers L, vec3 worldPos, vec3 geoNInterp, vec3 faceN, vec2 dx, vec2 dy, float curv)
{
	const float fadeEnd = u_terrainTexParams7.x;
	const vec3 toView = u_viewPos - worldPos;
	const float dist = length(toView);
	if (fadeEnd <= 0.0 || dist >= fadeEnd)
		return vec3(0.0);
	const float strength = (1.0 - smoothstep(u_terrainTexParams6.z, fadeEnd, dist)) * smoothstep(0.35, 0.6, geoNInterp.y);
	const float depth = mix(mix(u_terrainTexParams6.x, u_terrainTexParams6.y, float(L.rockW)), u_terrainTexParams6.x, float(L.snowW)) * strength;
	const vec3 V = toView / dist;
	if (depth < 1e-3)
		return vec3(0.0);
	const vec3 geoN = normalize(mix(faceN, geoNInterp, smoothstep(0.0, 0.2, dot(geoNInterp, V))));
	const float NoV = dot(geoN, V);
	if (NoV <= 0.0)
	{
#if TERRAIN_POM_SILHOUETTE
		discard; // edge-on to the facet itself (SPOM's back-facing case): nothing of the shell is in view
#endif
		return vec3(0.0);
	}

	// s = tangential run as a fraction of the flat march's (maxOffset at s = 1); rayH(s) = 1 - s + c s^2.
	// NoV floor 0.05: a higher one underestimated c for grazing rays, and they missed the silhouette test.
	// On a crest the search ends at s = 1 / c anyway; only flat and concave ground runs the full 20x depth.
	const vec3 maxOffset = -(V - geoN * NoV) / max(NoV, 0.05) * depth; // the flat march's shift at the bottom
	const float c = 0.5 * curv * dot(maxOffset, maxOffset) / depth;
	// Where the search ends: the bottom of the relief (rayH = 0), or - on a crest where the ray turns before
	// it - back at the top (rayH = 1).
	float sEnd = 1.0;
	if (c > 0.25)
		sEnd = 1.0 / c;
	else if (abs(c) > 1e-4)
		sEnd = (1.0 - sqrt(1.0 - 4.0 * c)) / (2.0 * c);
	const float maxSteps = u_terrainTexParams7.y;
	const float baseSteps = max(maxSteps * 0.25, 4.0); // looking straight on
	const int steps = int(clamp(baseSteps / max(NoV, 0.05), baseSteps, maxSteps));
	const float ds = sEnd / float(steps);
	float s = 0.0, prevS = 0.0;
	float16_t h = terrainReliefAt(L, worldPos.xz, dx, dy);
	float16_t prevH = h;
	for (int i = 0; i < steps && 1.0 - s + c * s * s > float(h); ++i)
	{
		prevS = s;
		prevH = h;
		s += ds;
		h = terrainReliefAt(L, worldPos.xz + maxOffset.xz * s, dx, dy);
	}
#if TERRAIN_POM_SILHOUETTE
	// Still above the relief at the end: only possible when the ray climbed back out over a crest (at the
	// flat end rayH = 0 <= h). The mesh here is not what this ray sees.
	if (c > 0.25 && 1.0 - s + c * s * s > float(h))
		discard;
#endif
	// Bracket: prevS above the surface, s at or under it. The first tap hitting leaves no bracket.
	if (s > prevS)
	{
		for (int i = 0; i < TERRAIN_POM_BISECT; ++i)
		{
			const float midS = 0.5 * (s + prevS);
			const float16_t hm = terrainReliefAt(L, worldPos.xz + maxOffset.xz * midS, dx, dy);
			if (float(hm) >= 1.0 - midS + c * midS * midS) { s = midS; h = hm; }
			else { prevS = midS; prevH = hm; }
		}
	}
	const float after = float(h) - (1.0 - s + c * s * s);                  // >= 0: at or under the surface
	const float before = float(prevH) - (1.0 - prevS + c * prevS * prevS); // <= 0: above it (0: no bracket)
	const float t = clamp(after / max(after - before, 1e-4), 0.0, 1.0);
	const float sHit = mix(s, prevS, t);
	const float hitH = 1.0 - sHit + c * sHit * sHit;
	const vec3 offset = maxOffset * sHit;

	// Self-shadow: skipped off the sun, at the top of the relief (nothing stands above it) and when off.
	const vec3 Ls = u_sunDirection.xyz;
	const float NoL = dot(geoN, Ls);
	const float shadowStrength = u_terrainTexParams7.z;
	if (shadowStrength > 0.0 && NoL > 0.0 && hitH < 0.98)
	{
		// Per unit of height fraction climbed toward the sun, the texture position moves by this.
		const vec2 lightStep = ((Ls - geoN * NoL) / max(NoL, 0.1) * depth).xz;
		const float climb = (1.0 - hitH) / float(TERRAIN_POM_SHADOW_STEPS);
		float occlusion = 0.0;
		for (int i = 1; i <= TERRAIN_POM_SHADOW_STEPS; ++i)
		{
			const float lh = hitH + climb * float(i);
			const float16_t hs = terrainReliefAt(L, worldPos.xz + offset.xz + lightStep * (lh - hitH), dx, dy);
			occlusion = max(occlusion, (float(hs) - lh) * (1.0 - float(i - 1) / float(TERRAIN_POM_SHADOW_STEPS)));
		}
		g_sunVisMaterial = float16_t(1.0 - clamp(occlusion * TERRAIN_POM_SHADOW_SHARPNESS, 0.0, 1.0) * shadowStrength * strength);
	}
	return offset;
}
#endif // !TERRAIN_SPLAT_HEIGHT_ONLY
#endif // TERRAIN_SPLAT_RELIEF

#ifndef TERRAIN_SPLAT_HEIGHT_ONLY
// The full splatted terrain surface from precomputed coverages (terrainLayers) - for a caller that needs them
// itself too (the tessellated terrain's pixel normal); neutral mid-gray before a texture set is registered.
// geoN = the shading base the normal maps reorient onto. Call in uniform flow (screen derivatives).
TerrainSample terrainSplatLayers(vec3 worldPos, vec3 geoN, TerrainLayers L)
{
	const f16vec3 geoNh = f16vec3(geoN);
#if defined(TERRAIN_SPLAT_RELIEF) && TERRAIN_POM
	// The march's gradients (and the silhouette's curvature inputs), taken here in uniform flow.
	const vec3 dP3x = dFdx(worldPos), dP3y = dFdy(worldPos);
	const vec2 dPdx = dP3x.xz, dPdy = dP3y.xz;
#if TERRAIN_POM_SILHOUETTE
	const vec3 dNx = dFdx(geoN), dNy = dFdy(geoN);
#endif
#endif
	if (u_terrainTexParams0.x < 0.0 || u_terrainTexParams0.y < 1.0)
		return TerrainSample(f16vec3(0.5), geoNh, float16_t(0.92), float16_t(0.0), float16_t(1.0), float16_t(0.5));

	const int baseMat = L.baseMat;
	const int numGround = L.numGround;
	const int numRock = L.numRock;
	const float16_t snowW = L.snowW, beachW = L.beachW, rockW = L.rockW;

	// Every layer samples at the parallax-shifted position; the coverages above stay at the mesh point.
	vec3 texPos = worldPos;
#if defined(TERRAIN_SPLAT_RELIEF) && TERRAIN_POM
	float curv = 0.0;
	vec3 faceN = geoN;
#if TERRAIN_POM_SILHOUETTE
	{
		const vec3 V = normalize(u_viewPos - worldPos);
		// The facet's own normal, turned toward the camera (the derivative cross's sign follows the screen).
		faceN = normalize(cross(dP3x, dP3y));
		if (dot(faceN, V) < 0.0)
			faceN = -faceN;
		// Along the direction the march runs: the view vector's tangential part, reversed.
		const vec3 run = geoN * dot(geoN, V) - V;
		const float runLen = length(run);
		if (runLen > 1e-3)
			curv = terrainCurvatureAlong(run / runLen, dP3x, dP3y, dNx, dNy);
	}
#endif
	texPos += terrainParallaxOffset(L, worldPos, geoN, faceN, dPdx, dPdy, curv);
#endif

	const float16_t opaque = float16_t(TERRAIN_LAYER_OPAQUE);
	const float16_t blendEps = float16_t(TERRAIN_BLEND_EPS);
	if (snowW >= opaque)
	{
		TerrainSample surf = sampleTerrainXZ(L.snowMatIdx, texPos.xz * u_terrainTexParams2.w, geoNh);
		surf.normal = normalize(surf.normal);
		return surf;
	}
	const vec2 uvGround = texPos.xz * u_terrainTexParams1.x;

	// --- Composite bottom-up, sampling only what shows ---
	// 1. Ground - buried under a full beach band or a full-coverage cliff face: placeholder, mixed away.
	TerrainSample surf;
	if (beachW < opaque && rockW < opaque)
	{
		const ClimatePick g = L.g;
		surf = sampleTerrainXZ(uint(baseMat + g.i0), uvGround, geoNh);
		if (g.n1 > blendEps)
			terrainMixInto(surf, sampleTerrainXZ(uint(baseMat + g.i1), uvGround, geoNh), g.n1 / max(float16_t(1.0) - g.n2, float16_t(1e-4)));
		if (g.n2 > blendEps)
			terrainMixInto(surf, sampleTerrainXZ(uint(baseMat + g.i2), uvGround, geoNh), g.n2);
	}
	else
		surf = TerrainSample(f16vec3(0.0), geoNh, float16_t(0.9), float16_t(0.0), float16_t(1.0), float16_t(0.0));

	// 2. Beach (invisible under a full rock face).
	if (beachW > blendEps && rockW < opaque)
	{
		const TerrainSample beach = sampleTerrainXZ(uint(baseMat + numGround + numRock), uvGround, geoNh);
		if (beachW >= opaque)
			surf = beach;
		else
			terrainMixInto(surf, beach, beachW);
	}

	// 3. Rock
	if (rockW > blendEps)
	{
		const ClimatePick r = L.r;
		TerrainSample rock = sampleTerrainTriplanar(uint(baseMat + r.i0), texPos, geoNh, u_terrainTexParams1.y);
		if (r.n1 > blendEps)
			terrainMixInto(rock, sampleTerrainTriplanar(uint(baseMat + r.i1), texPos, geoNh, u_terrainTexParams1.y), r.n1 / max(float16_t(1.0) - r.n2, float16_t(1e-4)));
		if (r.n2 > blendEps)
			terrainMixInto(rock, sampleTerrainTriplanar(uint(baseMat + r.i2), texPos, geoNh, u_terrainTexParams1.y), r.n2);
		if (rockW >= opaque)
			surf = rock;
		else
			terrainMixInto(surf, rock, rockW);
	}

	// 4. Snow (partial cover; full cover returned above).
	if (snowW > blendEps)
		terrainMixInto(surf, sampleTerrainXZ(L.snowMatIdx, texPos.xz * u_terrainTexParams2.w, geoNh), snowW);

	surf.normal = normalize(surf.normal);
	return surf;
}

// The full splatted terrain surface. geoN = the shading base the normal maps reorient onto; coverN = the normal
// the layer coverages read (slope: rock, snow).
TerrainSample terrainSplat(vec3 worldPos, vec3 geoN, vec3 coverN, TerrainFields f)
{
	return terrainSplatLayers(worldPos, geoN, terrainLayers(worldPos, coverN, f));
}

#endif // !TERRAIN_SPLAT_HEIGHT_ONLY

#endif // TERRAIN_SPLAT_INC_GLSL
