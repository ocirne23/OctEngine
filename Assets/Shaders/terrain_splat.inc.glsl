// --- Terrain texture splatting (setTerrainSplatMaterials; u_terrainTexParams*/u_terrainSplatClimate) ---
// Four physical layers composited bottom-up — no biome enum, climate selects textures directly:
//   1. GROUND — climate-picked soil/vegetation, world-XZ projection
//   2. BEACH  — shoreline band just above the local waterline (not climate-selected)
//   3. ROCK   — bedrock where too steep / too prominent to hold soil; climate-picked type, triplanar
//   4. SNOW   — cover over ALL of the above (snow falls ON bedrock — as a ground type the rock layer
//               would paint over it and peaks would come out gray)
//
// Shared by the terrain fragment shader (its own pixels) and the ocean shader (the seabed at a
// refraction-ray hit, so the sand seen through the water IS the terrain next to it). The includer
// declares, before including:
//   in_materialInfos[] (MaterialInfo: flags / diffuseNormalTexIdx / metalRoughnessTexIdxAlphaMode),
//   u_textures[] + GL_EXT_nonuniform_qualifier, the UBO (u_terrainTexParams*, u_terrainSplatClimate,
//   u_terrainParams).
// Optional, before including:
//   TERRAIN_SPLAT_TEX(tex, uv)  — the texture fetch. Defaults to texture() (screen derivatives); a ray
//                                 hit has none, so the ocean defines it as textureLod at a ray-cone LOD.
//   TERRAIN_SPLAT_ALBEDO_ONLY   — skip the normal + ARM taps (the seabed only needs colour: the water
//                                 column blurs any detail normal away). TerrainSample.normal is then the
//                                 geometric normal and rough/metal/ao are constants.

#ifndef TERRAIN_SPLAT_INC_GLSL
#define TERRAIN_SPLAT_INC_GLSL

#ifndef TERRAIN_SPLAT_TEX
#define TERRAIN_SPLAT_TEX(tex, uv) texture(tex, uv)
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
	vec3 albedo;
	vec3 normal;   // world space
	float rough;
	float metal;
	float ao;
};

// One splat material with world-XZ UVs; tangent basis = world X/Z reoriented onto the geometric
// normal. matIdx diverges between neighbouring pixels at climate borders -> nonuniformEXT.
TerrainSample sampleTerrainXZ(uint matIdx, vec2 uv, vec3 geoN)
{
	const MaterialInfo material = in_materialInfos[nonuniformEXT(matIdx)];
	const uint diffuseTexIdx = material.diffuseNormalTexIdx & 0xFFFFu;

	TerrainSample s;
	s.albedo = TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uv).rgb;
#ifdef TERRAIN_SPLAT_ALBEDO_ONLY
	s.ao = 1.0;
	s.rough = 0.9;
	s.metal = 0.0;
	s.normal = geoN;
#else
	const uint normalTexIdx = material.diffuseNormalTexIdx >> 16;
	const uint armTexIdx    = material.metalRoughnessTexIdxAlphaMode & 0xFFFFu;
	const vec3 arm = armTexIdx != 0xFFFFu ? TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uv).rgb : vec3(1.0, 0.9, 0.0);
	s.ao = arm.r;
	s.rough = max(arm.g, 0.01);
	s.metal = arm.b;

	const vec3 normalSample = TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uv).xyz;
	vec3 tn;
	if ((material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u)
	{
		const vec2 nxy = normalSample.xy * 2.0 - 1.0;
		tn = vec3(nxy, sqrt(max(1.0 - dot(nxy, nxy), 0.0)));
	}
	else
		tn = normalize(normalSample * 2.0 - 1.0);
	s.normal = normalize(tn.x * vec3(1.0, 0.0, 0.0) + tn.y * vec3(0.0, 0.0, 1.0) + tn.z * geoN);
#endif
	return s;
}

// Decode one triplanar normal-map tap into tangent space (BC5 reconstructs Z).
vec3 decodeTriplanarNormal(vec3 ns, bool bc5)
{
	if (bc5)
	{
		const vec2 xy = ns.xy * 2.0 - 1.0;
		return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
	}
	return normalize(ns * 2.0 - 1.0);
}

// Triplanar version for the rock layer (whiteout-style normal blend), so cliff faces don't smear.
// PLANE SKIPPING: the three projection weights sum to 1, so a plane below TRIPLANAR_WMIN contributes
// <~4% and is dropped (its texture taps skipped) — on shallow crag one axis dominates and the other two
// are near-zero, cutting up to 2/3 of the taps. Surviving weights are renormalised so no energy is lost.
// The branch is coherent across a quad except on the exact plane-crossover diagonal, where one pixel may
// get a slightly wrong mip; invisible in practice.
#define TRIPLANAR_WMIN 0.05
TerrainSample sampleTerrainTriplanar(uint matIdx, vec3 worldPos, vec3 geoN, float uvScale)
{
	const MaterialInfo material = in_materialInfos[nonuniformEXT(matIdx)];
	const uint diffuseTexIdx = material.diffuseNormalTexIdx & 0xFFFFu;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
	const uint normalTexIdx  = material.diffuseNormalTexIdx >> 16;
	const uint armTexIdx     = material.metalRoughnessTexIdxAlphaMode & 0xFFFFu;
	const bool bc5 = (material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u;
	const bool hasArm = armTexIdx != 0xFFFFu;
#endif

	vec3 w = abs(geoN);
	w = w / (w.x + w.y + w.z);
	const bvec3 use = greaterThan(w, vec3(TRIPLANAR_WMIN));
	w /= dot(w, vec3(use)); // renormalise over the kept planes
	const vec2 uvX = worldPos.zy * uvScale; // plane normal = X
	const vec2 uvY = worldPos.xz * uvScale; // plane normal = Y
	const vec2 uvZ = worldPos.xy * uvScale; // plane normal = Z

	vec3 albedo = vec3(0.0);
	vec3 arm = vec3(0.0);
	vec3 nrm = vec3(0.0);
	if (use.x)
	{
		albedo += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvX).rgb * w.x;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const vec3 tn = decodeTriplanarNormal(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvX).xyz, bc5);
		nrm += vec3(tn.z * sign(geoN.x), tn.y, tn.x) * w.x;
		if (hasArm) arm += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvX).rgb * w.x;
#endif
	}
	if (use.y)
	{
		albedo += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvY).rgb * w.y;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const vec3 tn = decodeTriplanarNormal(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvY).xyz, bc5);
		nrm += vec3(tn.x, tn.z * sign(geoN.y), tn.y) * w.y;
		if (hasArm) arm += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvY).rgb * w.y;
#endif
	}
	if (use.z)
	{
		albedo += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(diffuseTexIdx)], uvZ).rgb * w.z;
#ifndef TERRAIN_SPLAT_ALBEDO_ONLY
		const vec3 tn = decodeTriplanarNormal(TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(normalTexIdx)], uvZ).xyz, bc5);
		nrm += vec3(tn.x, tn.y, tn.z * sign(geoN.z)) * w.z;
		if (hasArm) arm += TERRAIN_SPLAT_TEX(u_textures[nonuniformEXT(armTexIdx)], uvZ).rgb * w.z;
#endif
	}

	TerrainSample s;
	s.albedo = albedo;
#ifdef TERRAIN_SPLAT_ALBEDO_ONLY
	s.ao = 1.0;
	s.rough = 0.9;
	s.metal = 0.0;
	s.normal = geoN;
#else
	if (!hasArm)
		arm = vec3(1.0, 0.9, 0.0);
	s.ao = arm.r;
	s.rough = max(arm.g, 0.01);
	s.metal = arm.b;
	s.normal = normalize(nrm + geoN * 2.0); // biased toward geoN: detail, not replacement
#endif
	return s;
}

// Crag wander noise (value fBm over world XZ). Lives HERE and not in the generator: the wander must be
// scaled by local relief, which the generator's coarse path cannot supply (its relief is 0 by
// construction — the cascades would disagree). The shader always has the true mesh height.
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

// Lay `s` over `acc` with coverage `w`.
void terrainMixInto(inout TerrainSample acc, TerrainSample s, float w)
{
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
	int i0, i1, i2;  // top three entries (0-based like u_terrainSplatClimate; caller adds baseMat)
	float n1, n2;    // normalized coverage of i1 and i2; the top pick i0 gets the rest (n0 = 1 - n1 - n2)
};

// Top THREE climate entries in [first, first + count) as a partition of unity, each weighted RELATIVE to
// the FOURTH (w - w3): when the #3/#4 ranking swaps both sit at zero contribution, so the third texture
// fades in and out rather than popping (the generalisation of the old top-two's relative-to-third trick).
// Three real entries — not two — is what lets a pixel where three climate boxes overlap show all three
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
	return ClimatePick(i0, i1, i2, a1 * inv, a2 * inv);
}

// A layer samples the top climate pick, then blends i1/i2 in ONLY when their coverage clears this — so
// the flat interior of a climate box stays one sample, a two-box border costs two, and only a genuine
// three-box junction pays for three. Sequential mix (i1 at n1/(1-n2), then i2 at n2) reproduces the
// weighted sum n0*s0 + n1*s1 + n2*s2 exactly; a skipped small weight just folds into s0.
#define TERRAIN_BLEND_EPS 0.004

// A layer at/above this coverage hides everything beneath it (< 0.3% contribution): the buried layers'
// texture taps are skipped entirely and the covering layer REPLACES the accumulator instead of mixing.
#define TERRAIN_LAYER_OPAQUE 0.997

// The full splatted terrain surface; neutral mid-gray before a texture set is registered.
// All four layer coverages are computed FIRST (cheap ALU) so fully buried layers never sample: a
// snow-capped peak collapses to the 3 snow taps, a sheer cliff face skips ground + beach.
TerrainSample terrainSplat(vec3 worldPos, vec3 geoN, TerrainFields f)
{
	const int baseMat = int(u_terrainTexParams0.x);
	const int numGround = int(u_terrainTexParams0.y);
	const int numRock = int(u_terrainTexParams0.z);
	if (baseMat < 0 || numGround <= 0)
		return TerrainSample(vec3(0.5), geoN, 0.92, 0.0, 1.0);

	// The baked temperature already carries the altitude lapse, so elevation enters the selection as
	// the cold it causes — snow line and vegetation cannot disagree.
	const vec2 climate = vec2(clamp((f.temperature + 25.0) / 75.0, 0.0, 1.0), f.humidity);
	const float invS2 = 1.0 / (2.0 * u_terrainTexParams0.w * u_terrainTexParams0.w);
	const float slope = 1.0 - clamp(geoN.y, 0.0, 1.0);
	const vec2 uvGround = worldPos.xz * u_terrainTexParams1.x;

	// --- Coverages ---
	// Beach: the band just above the local waterline.
	float beachW = 0.0;
	if (u_terrainTexParams3.x > 0.5)
		beachW = 1.0 - smoothstep(0.3, max(u_terrainTexParams2.z, 0.31), worldPos.y - f.waterLevel);

	// Rock: too steep OR standing too far above the macro altitude (crag). max(), not a sum —
	// the two coincide on a cliff. On V3 terrain crag is what puts rock on mountains (the 30 m/px field
	// rarely reaches the slope threshold). The relief is wandered by fBm first or the rock boundary is
	// an elevation contour across a whole range; |wander| <= relief keeps flat lowlands untouched.
	float rockW = 0.0;
	if (numRock > 0)
	{
		float relief = (worldPos.y - u_terrainParams.z) - f.altitude;
		const float wanderAmp = u_terrainTexParams5.x;
		// The wander moves relief by at most +-amp, so only pixels where that can change the crag
		// smoothstep pay for the 12-hash fBm — saturated flatland (crag 0) and sheer crag (1) skip it.
		if (wanderAmp > 0.0 && relief + wanderAmp > u_terrainTexParams2.x && relief - wanderAmp < u_terrainTexParams2.y)
		{
			const float w = terrainFbm(worldPos.xz * u_terrainTexParams5.y) * wanderAmp;
			relief -= w * clamp(relief / wanderAmp, 0.0, 1.0);
		}
		const float crag = smoothstep(u_terrainTexParams2.x, u_terrainTexParams2.y, relief);
		rockW = max(smoothstep(u_terrainTexParams1.z, u_terrainTexParams1.w, slope), crag * 0.85);
	}

	// Snow over everything, but it slides off steep faces (the mountain's rock shows through)
	// and needs humidity to fall at all (no white polar deserts).
	float snowW = 0.0;
	if (u_terrainTexParams3.y > 0.5)
	{
		const float cold  = 1.0 - smoothstep(u_terrainTexParams3.z, u_terrainTexParams3.w, f.temperature);
		const float holds = 1.0 - smoothstep(u_terrainTexParams4.x, u_terrainTexParams4.y, slope);
		const float wet = smoothstep(0.0, max(u_terrainTexParams4.z, 1e-3), f.humidity);
		snowW = cold * holds * wet;
	}
	const uint snowMatIdx = uint(baseMat + numGround + numRock) + (u_terrainTexParams3.x > 0.5 ? 1u : 0u);

	// --- Composite bottom-up, sampling only what shows ---
	// Full snow cover: everything beneath is hidden — the whole splat is the snow sample alone.
	if (snowW >= TERRAIN_LAYER_OPAQUE)
	{
		TerrainSample surf = sampleTerrainXZ(snowMatIdx, worldPos.xz * u_terrainTexParams2.w, geoN);
		surf.normal = normalize(surf.normal);
		return surf;
	}

	// 1. Ground — buried under a full beach band or a full-coverage cliff face: placeholder, mixed away.
	TerrainSample surf;
	if (beachW < TERRAIN_LAYER_OPAQUE && rockW < TERRAIN_LAYER_OPAQUE)
	{
		const ClimatePick g = pickClimate(climate, 0, numGround, invS2);
		surf = sampleTerrainXZ(uint(baseMat + g.i0), uvGround, geoN);
		if (g.n1 > TERRAIN_BLEND_EPS)
			terrainMixInto(surf, sampleTerrainXZ(uint(baseMat + g.i1), uvGround, geoN), g.n1 / max(1.0 - g.n2, 1e-4));
		if (g.n2 > TERRAIN_BLEND_EPS)
			terrainMixInto(surf, sampleTerrainXZ(uint(baseMat + g.i2), uvGround, geoN), g.n2);
	}
	else
		surf = TerrainSample(vec3(0.0), geoN, 0.9, 0.0, 1.0);

	// 2. Beach (invisible under a full rock face).
	if (beachW > 0.004 && rockW < TERRAIN_LAYER_OPAQUE)
	{
		const TerrainSample beach = sampleTerrainXZ(uint(baseMat + numGround + numRock), uvGround, geoN);
		if (beachW >= TERRAIN_LAYER_OPAQUE)
			surf = beach;
		else
			terrainMixInto(surf, beach, beachW);
	}

	// 3. Rock
	if (rockW > 0.004)
	{
		const ClimatePick r = pickClimate(climate, numGround, numRock, invS2);
		TerrainSample rock = sampleTerrainTriplanar(uint(baseMat + r.i0), worldPos, geoN, u_terrainTexParams1.y);
		if (r.n1 > TERRAIN_BLEND_EPS)
			terrainMixInto(rock, sampleTerrainTriplanar(uint(baseMat + r.i1), worldPos, geoN, u_terrainTexParams1.y), r.n1 / max(1.0 - r.n2, 1e-4));
		if (r.n2 > TERRAIN_BLEND_EPS)
			terrainMixInto(rock, sampleTerrainTriplanar(uint(baseMat + r.i2), worldPos, geoN, u_terrainTexParams1.y), r.n2);
		if (rockW >= TERRAIN_LAYER_OPAQUE)
			surf = rock;
		else
			terrainMixInto(surf, rock, rockW);
	}

	// 4. Snow (partial cover; full cover returned above).
	if (snowW > 0.004)
		terrainMixInto(surf, sampleTerrainXZ(snowMatIdx, worldPos.xz * u_terrainTexParams2.w, geoN), snowW);

	surf.normal = normalize(surf.normal);
	return surf;
}

#endif // TERRAIN_SPLAT_INC_GLSL
