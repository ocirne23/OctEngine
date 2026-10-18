// Shadow mapping and PCSS.

// Fraction of a cascade's far range over which it cross-fades into the next cascade. Not read here: the band is
// u_cascadeBlendStart / u_cascadeBlendInvWidth, from the CPU's CASCADE_BLEND in RendererUbo.cpp - keep both in sync.
#define SHADOW_CASCADE_BLEND 0.25
#define GOLDEN_RATIO_FRACT 0.6180339887
#define PCSS_MAX_PENUMBRA_TEXELS 15.0  // cap so the kernel never gets too sparse/noisy
#define PCSS_MIN_PENUMBRA_TEXELS 1.0   // floor so contact shadows stay crisp
// The blocker search GATHERS: each tap is one textureGather (2x2 neighbour texels), so 6 taps read 24 texels with
// few fetches (L1TEX latency is the Static meshes range's top stall). The taps stay spread over the Vogel disk.
#define PCSS_BLOCKER_GATHERS 6
#define PCSS_BLOCKER_TEXELS (PCSS_BLOCKER_GATHERS * 4)
#define PCSS_FILTER_SAMPLES 12
#define GOLDEN_ANGLE 2.39996323

// Per-cascade scalars are stashed in the matrix's bottom row; restore [0,0,0,1] before using it.
float cascadeSplit(int c) { return u_cascadeViewProj[c][0][3]; }
float cascadeTexelWorldSize(int c) { return u_cascadeViewProj[c][1][3]; }
float cascadeDepthRange(int c) { return u_cascadeViewProj[c][2][3]; }

// PCF disk radius (texels) per unit of normalized depth gap, from the sun's angular size so the PCSS softness matches
// the ray-traced sun (Renderer::buildUboSunShadow, per cascade).
float pcssSunSizeTexels(int c) { return u_cascadeSunSizeTexels[c]; }

#ifdef SHADOW_FOLIAGE_BIAS
// FOLIAGE cards (MATERIAL_FLAG_BILLBOARD; set by the lit FS around its lookup): the constant depth bias only. A crossed
// billboard's back half sits in the crossing card's shadow by a small depth step, which the slope-scaled bias and the
// normal offset would erase (a strip lit through the tree). Alpha-tested foliage hides the bit of acne this allows.
bool g_shadowFoliage = false;
#endif

#ifdef SHADOW_LEAF_TRANSMIT
// Tree MESH leaves (the LitTree FS sets it per leaf pixel): the fade length below, 0 = off. The normal bias stays.
float g_shadowLeafLength = 0.0;
#endif

// FOLIAGE receivers: light reaches INTO a crown, so the shadow fades in with the gap to the blockers, 1 - exp(-gap /
// length (m)). The crossed cards' own shadow then starts at nothing on their crossing axis and deepens into the crown;
// a blocker far up-sun still shadows fully. `gap` = the normalized depth gap to the average blocker. The length is
// u_foliage_shadowLength on the billboard cards, g_shadowLeafLength on the tree mesh leaves (a leaf just under its own
// billboard's top card is barely shaded, one deep under it fully).
float foliageTransmit(float visibility, float gap, int cascade)
{
	float shadowLength = 0.0;
#ifdef SHADOW_FOLIAGE_BIAS
	if (g_shadowFoliage)
		shadowLength = u_foliage_shadowLength;
#endif
#ifdef SHADOW_LEAF_TRANSMIT
	shadowLength = max(shadowLength, g_shadowLeafLength);
#endif
	if (shadowLength > 0.0)
	{
		const float metres = gap * cascadeDepthRange(cascade); // every blocker found is nearer than p.z: gap > 0
		return 1.0 - (1.0 - visibility) * (1.0 - exp(-metres / shadowLength));
	}
	return visibility;
}

mat4 cascadeMatrix(int c)
{
	mat4 m = u_cascadeViewProj[c];
	m[0][3] = 0.0; m[1][3] = 0.0; m[2][3] = 0.0; m[3][3] = 1.0;
	return m;
}

int getSunCascade(vec3 worldPos)
{
	float dist = length(worldPos - u_sceneFocus.xyz); // the cascades are concentric about the scene focus (the player in game mode)
	for (int i = 0; i < NUM_SHADOW_CASCADES; ++i)
		if (dist < cascadeSplit(i)) return i;
	return NUM_SHADOW_CASCADES - 1;
}

// Interleaved gradient noise: a cheap per-pixel value in [0,1) for rotating the sample kernel.
float interleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Golden-angle spiral disk sample, rotated per pixel: even coverage at a large radius and few taps. rotSC = (cos, sin)
// of the per-pixel rotation, computed ONCE in sampleSunShadow: the tap's own angle is a compile-time constant once the
// loops unroll, so the rotation is a 2x2 multiply per tap, not a sincos.
// Keep the loops FULLY unrolled: batching them (a [[dont_unroll]] loop over 4-tap groups, offsets from a table) costs
// 93 registers against 83.
vec2 vogelDisk(int i, int count, vec2 rotSC)
{
	float r = sqrt((float(i) + 0.5) / float(count));
	float a = float(i) * GOLDEN_ANGLE;
	vec2 ca = vec2(cos(a), sin(a));
	return r * vec2(ca.x * rotSC.x - ca.y * rotSC.y, ca.y * rotSC.x + ca.x * rotSC.y);
}

// PCSS blocker search: the average depth of the texels nearer the light than the receiver. Returns the blocker count in
// TEXELS, of PCSS_BLOCKER_TEXELS (0 = fully lit).
float blockerSearch(vec2 uv, int cascade, float receiverDepth, float radiusUV, vec2 rotSC, out float avgBlocker)
{
	float sum = 0.0;
	float count = 0.0;
	for (int i = 0; i < PCSS_BLOCKER_GATHERS; ++i)
	{
		vec2 off = vogelDisk(i, PCSS_BLOCKER_GATHERS, rotSC) * radiusUV;
		const vec4 d = textureGather(u_shadowMapDepth, vec3(uv + off, float(cascade)), 0);
		const vec4 isBlocker = vec4(lessThan(d, vec4(receiverDepth)));
		sum += dot(d, isBlocker);
		count += isBlocker.x + isBlocker.y + isBlocker.z + isBlocker.w;
	}

	avgBlocker = (count > 0.0) ? sum / count : 0.0;
	return count;
}

// A world position in one cascade's shadow space: (uv, biased depth, valid). normalOffset (world units) moves the
// receiver along its normal; the caller scales both biases per cascade and by slope.
vec4 projectCascade(vec3 worldPos, vec3 N, int cascade, float normalOffset, float depthBias)
{
	vec4 lightPos = cascadeMatrix(cascade) * vec4(worldPos + N * normalOffset, 1.0);
	vec3 proj = lightPos.xyz / lightPos.w;
	vec2 uv = proj.xy * 0.5 + 0.5;
	float valid = (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || proj.z > 1.0) ? 0.0 : 1.0;
	return vec4(uv, proj.z - depthBias, valid);
}

// Variable-radius PCF over a Vogel disk for a single cascade.
float pcfVogelSingle(vec4 p, int cascade, float radiusUV, vec2 rotSC)
{
	float sum = 0.0;
	for (int i = 0; i < PCSS_FILTER_SAMPLES; ++i)
	{
		vec2 off = vogelDisk(i, PCSS_FILTER_SAMPLES, rotSC) * radiusUV;
		sum += texture(u_shadowMap, vec4(p.xy + off, float(cascade), p.z));
	}
	return sum / float(PCSS_FILTER_SAMPLES);
}

// Full PCSS for one cascade: blocker search -> penumbra estimate -> variable-radius PCF. Visibility in [0, 1].
float pcssCascade(vec4 p, int cascade, float texelUV, vec2 rotSC)
{
	if (p.w < 0.5)
		return 1.0; // outside this cascade's coverage

	float searchRadiusUV = PCSS_MAX_PENUMBRA_TEXELS * texelUV;
	float avgBlocker;
	float blockers = blockerSearch(p.xy, cascade, p.z, searchRadiusUV, rotSC, avgBlocker);
	if (blockers <= 0.0)
		return 1.0;

	// Umbra: every search texel is occluded, and so is the hardware-PCF tap at the centre: skip the filter (its disk
	// lies inside the search disk, with the complementary compare). The centre tap catches what the sparse search can
	// miss: a small hole straight over the pixel (a gap in a grate or canopy with the caster close by).
	if (blockers >= float(PCSS_BLOCKER_TEXELS) && texture(u_shadowMap, vec4(p.xy, float(cascade), p.z)) <= 0.0)
		return foliageTransmit(0.0, p.z - avgBlocker, cascade);

	// The penumbra grows with the gap to the blocker: a caster touching the surface = ~MIN texels, far = up to MAX.
	float penumbraTexels = clamp((p.z - avgBlocker) * pcssSunSizeTexels(cascade), PCSS_MIN_PENUMBRA_TEXELS, PCSS_MAX_PENUMBRA_TEXELS);
	return foliageTransmit(pcfVogelSingle(p, cascade, penumbraTexels * texelUV, rotSC), p.z - avgBlocker, cascade);
}

// Border PCSS: the fixed tap budget is split between the two cascades by t (a low-discrepancy key per tap), so a border
// pixel costs the same as a normal one. Each cascade averages its own taps; the results blend by t.
float pcssBorder(vec4 pa, vec4 pb, int ca, int cb, float texelUV, vec2 rotSC, float t)
{
	float searchRadiusUV = PCSS_MAX_PENUMBRA_TEXELS * texelUV;
	float minRadiusUV = PCSS_MIN_PENUMBRA_TEXELS * texelUV;
	bool validA = pa.w >= 0.5; // fragment lies inside this cascade's coverage
	bool validB = pb.w >= 0.5;
	if (!validA && !validB)
		return 1.0; // outside both cascades: no shadow data

	// Blocker search, split between cascades (taps routed to an invalid cascade are skipped).
	float sumDA = 0.0, nDA = 0.0, sumDB = 0.0, nDB = 0.0;
	for (int i = 0; i < PCSS_BLOCKER_GATHERS; ++i)
	{
		bool useB = fract(float(i) * GOLDEN_RATIO_FRACT) < t;
		if ((useB && !validB) || (!useB && !validA))
			continue;
		vec4 p = useB ? pb : pa;
		vec2 off = vogelDisk(i, PCSS_BLOCKER_GATHERS, rotSC) * searchRadiusUV;
		const vec4 d = textureGather(u_shadowMapDepth, vec3(p.xy + off, float(useB ? cb : ca)), 0);
		const vec4 isBlocker = vec4(lessThan(d, vec4(p.z)));
		const float n = isBlocker.x + isBlocker.y + isBlocker.z + isBlocker.w;
		const float s = dot(d, isBlocker);
		if (useB) { sumDB += s; nDB += n; }
		else      { sumDA += s; nDA += n; }
	}

	// Here the blocker search only SIZES the penumbra; the PCF decides lit vs shadowed. With the split budget a cascade
	// can find no blocker even in shadow: take the minimum radius, not "lit" (a bright line between shadowed cascades).
	float radA = (nDA > 0.0) ? clamp((pa.z - sumDA / nDA) * pcssSunSizeTexels(ca), PCSS_MIN_PENUMBRA_TEXELS, PCSS_MAX_PENUMBRA_TEXELS) * texelUV : minRadiusUV;
	float radB = (nDB > 0.0) ? clamp((pb.z - sumDB / nDB) * pcssSunSizeTexels(cb), PCSS_MIN_PENUMBRA_TEXELS, PCSS_MAX_PENUMBRA_TEXELS) * texelUV : minRadiusUV;

	// Filter, split between cascades; the PCF determines visibility for every tap.
	float sumA = 0.0, nA = 0.0, sumB = 0.0, nB = 0.0;
	for (int i = 0; i < PCSS_FILTER_SAMPLES; ++i)
	{
		bool useB = fract(float(i) * GOLDEN_RATIO_FRACT) < t;
		if ((useB && !validB) || (!useB && !validA))
			continue; // route only to valid cascades; the other supplies the result via fallback below
		vec4 p = useB ? pb : pa;
		float rad = useB ? radB : radA;
		float vis = texture(u_shadowMap, vec4(p.xy + vogelDisk(i, PCSS_FILTER_SAMPLES, rotSC) * rad, float(useB ? cb : ca), p.z));
		if (useB) { sumB += vis; nB += 1.0; }
		else      { sumA += vis; nA += 1.0; }
	}

	// FOLIAGE: each cascade's shadow fades in with its own blocker gap; no blocker found keeps the PCF result.
	float visA = (nA > 0.0) ? sumA / nA : 1.0;
	float visB = (nB > 0.0) ? sumB / nB : 1.0;
	if (nDA > 0.0) visA = foliageTransmit(visA, pa.z - sumDA / nDA, ca);
	if (nDB > 0.0) visB = foliageTransmit(visB, pb.z - sumDB / nDB, cb);

	// If one cascade is invalid (or got no taps), use the other's visibility instead of injecting light.
	if (!validA || nA == 0.0) return visB;
	if (!validB || nB == 0.0) return visA;
	return mix(visA, visB, t);
}

// The cascade, the next one, and the cross-fade factor t into it: t ramps 0->1 across the last
// SHADOW_CASCADE_BLEND fraction of this cascade's range. Returns the focus distance.
float sunCascadeBlend(vec3 worldPos, out int cascade, out int nextCascade, out float t)
{
	cascade = getSunCascade(worldPos);
	nextCascade = min(cascade + 1, NUM_SHADOW_CASCADES - 1);
	float dist = length(worldPos - u_sceneFocus.xyz); // the cascades are concentric about the scene focus (the player in game mode)
	t = (cascade == nextCascade) ? 0.0 : clamp((dist - u_cascadeBlendStart[cascade]) * u_cascadeBlendInvWidth[cascade], 0.0, 1.0);
	return dist;
}

// The bias grows with the slope (tan of the angle between N and the sun: acne and leaks at grazing angles) and with
// the distance (depth precision and texel density drop), across the full cascade range. The caller also scales the
// normal offset by each cascade's world texel size.
void sunShadowBias(vec3 N, float dist, out float depthBias, out float normalScale)
{
	vec3 L = u_sunDirection.xyz; // normalized on the CPU
	float NdotL = clamp(dot(N, L), 0.0, 1.0);
	float slope = clamp(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 1e-3), 1.0, 4.0);
	const float SHADOW_BIAS_DIST_MAX = 6.0;
	float distScale = mix(0.0, SHADOW_BIAS_DIST_MAX, clamp(dist / cascadeSplit(NUM_SHADOW_CASCADES - 1), 0.0, 1.0));
#ifdef SHADOW_FOLIAGE_BIAS
	if (g_shadowFoliage)
	{
		depthBias = u_shadow_depthBias * distScale;
		normalScale = 0.0;
		return;
	}
#endif
	depthBias = u_shadow_depthBias * slope * distScale;
	normalScale = u_shadow_normalBias * slope * distScale;
}

// ONE hardware-PCF tap: no blocker search, no Vogel disk. For surfaces whose motion hides a penumbra (the ocean). The
// cascade cross-fade is a per-pixel dither (TAA resolves it) instead of two evaluations.
float sampleSunShadowHard(vec3 worldPos, vec3 N)
{
	int cascade, nextCascade;
	float t;
	float dist = sunCascadeBlend(worldPos, cascade, nextCascade, t);
	if (t > interleavedGradientNoise(gl_FragCoord.xy))
		cascade = nextCascade;

	float depthBias, normalScale;
	sunShadowBias(N, dist, depthBias, normalScale);
	vec4 p = projectCascade(worldPos, N, cascade, normalScale * cascadeTexelWorldSize(cascade), depthBias);
	if (p.w < 0.5)
		return 1.0; // outside the cascade's coverage
	return texture(u_shadowMap, vec4(p.xy, float(cascade), p.z));
}

// The sun visibility, cross-faded into the next cascade near the cascade's end.
float sampleSunShadow(vec3 worldPos, vec3 N)
{
	int cascade, nextCascade;
	float t;
	float dist = sunCascadeBlend(worldPos, cascade, nextCascade, t);
	float depthBias, normalScale;
	sunShadowBias(N, dist, depthBias, normalScale);

	vec4 pa = projectCascade(worldPos, N, cascade, normalScale * cascadeTexelWorldSize(cascade), depthBias);
	if (t <= 0.0 && pa.w < 0.5)
		return 1.0; // outside the single cascade's coverage: skip the dither + sincos

	float ditherBase = interleavedGradientNoise(gl_FragCoord.xy);
	float texelUV = u_shadow_invResolution;
	float rotation = ditherBase * 6.2831853;
	vec2 rotSC = vec2(cos(rotation), sin(rotation)); // the ONE sincos per pixel every Vogel tap rotates by

	// Outside the cross-fade band: one cascade. Inside it: the same tap budget split across both.
	if (t <= 0.0)
		return pcssCascade(pa, cascade, texelUV, rotSC);
	else
	{
		vec4 pb = projectCascade(worldPos, N, nextCascade, normalScale * cascadeTexelWorldSize(nextCascade), depthBias);
		return pcssBorder(pa, pb, cascade, nextCascade, texelUV, rotSC, t);
	}
}

// Debug overlay on the final lit colour, compiled in by SHADOW_DEBUG ("Shadows/Debug mode", a pipeline reload):
//   1  cascade index tint (red / green / blue / magenta = cascade 0..3)
//   2  the cross-fade band: the cascade tint, going WHITE across the SHADOW_CASCADE_BLEND band into the next
//   3  the raw sun visibility term alone (what sampleSunShadow returns), greyscale
//   4  shadow-map texel size heat: green = 5 cm per texel, red = 1 m per texel (log scale)
#if defined(SHADOW_DEBUG) && SHADOW_DEBUG != 0
vec3 shadowDebugOverlay(vec3 color, vec3 worldPos, vec3 N)
{
	int cascade = getSunCascade(worldPos);
#if SHADOW_DEBUG == 1
	return mix(color, cascadeDebugColor(cascade), 0.5);
#elif SHADOW_DEBUG == 2
	float dist = length(worldPos - u_sceneFocus.xyz); // the cascades are concentric about the scene focus (the player in game mode)
	float t = (cascade == NUM_SHADOW_CASCADES - 1) ? 0.0 : clamp((dist - u_cascadeBlendStart[cascade]) * u_cascadeBlendInvWidth[cascade], 0.0, 1.0);
	return mix(mix(color, cascadeDebugColor(cascade), 0.3), vec3(1.0), t);
#elif SHADOW_DEBUG == 3
	return vec3(sampleSunShadow(worldPos, N));
#else
	float heat = clamp(log2(cascadeTexelWorldSize(cascade) / 0.05) / log2(1.0 / 0.05), 0.0, 1.0);
	return mix(color, mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat), 0.6);
#endif
}
#endif
