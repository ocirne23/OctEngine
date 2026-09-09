// Shadow mapping and PCSS functions

// Fraction of a cascade's far range over which it cross-fades into the next cascade.
#define SHADOW_CASCADE_BLEND 0.25
#define GOLDEN_RATIO_FRACT 0.6180339887
#define PCSS_MAX_PENUMBRA_TEXELS 15.0  // cap so the kernel never gets too sparse/noisy
#define PCSS_MIN_PENUMBRA_TEXELS 1.0   // floor so contact shadows stay crisp
#define PCSS_BLOCKER_SAMPLES 16
#define PCSS_FILTER_SAMPLES 16
#define GOLDEN_ANGLE 2.39996323

// Per-cascade scalars are stashed in the matrix's bottom row; restore [0,0,0,1] before using it.
float cascadeSplit(int c) { return u_cascadeViewProj[c][0][3]; }
float cascadeTexelWorldSize(int c) { return u_cascadeViewProj[c][1][3]; }
float cascadeDepthRange(int c) { return u_cascadeViewProj[c][2][3]; }

// PCF disk radius (texels) per unit of normalized depth gap, derived from the sun's angular size so
// PCSS softness matches the ray-traced sun (tan(sunRadius) * depthRange / texelWorldSize — the
// derivation sits at Renderer::buildUboSunShadow, which computes it per cascade once per frame).
float pcssSunSizeTexels(int c) { return u_cascadeSunSizeTexels[c]; }
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

// Evenly distributed disk sample (golden-angle spiral), rotated per pixel. Far better coverage than a
// fixed 12-point Poisson set when the kernel radius is large, which keeps PCSS smooth at low tap counts.
// rotSC = (cos, sin) of the per-pixel rotation, computed ONCE in sampleSunShadow: the tap's own angle
// i * GOLDEN_ANGLE is a compile-time constant once the fixed-count loops unroll, so the rotation is a
// 2x2 multiply per tap instead of a sincos per tap (32 sincos per pixel before).
vec2 vogelDisk(int i, int count, vec2 rotSC)
{
	float r = sqrt((float(i) + 0.5) / float(count));
	float a = float(i) * GOLDEN_ANGLE;
	vec2 ca = vec2(cos(a), sin(a));
	return r * vec2(ca.x * rotSC.x - ca.y * rotSC.y, ca.y * rotSC.x + ca.x * rotSC.y);
}

// PCSS blocker search: average the depths of texels closer to the light than the receiver. Returns the
// blocker count (0 => no occluders => fully lit).
float blockerSearch(vec2 uv, int cascade, float receiverDepth, float radiusUV, vec2 rotSC, out float avgBlocker)
{
	float sum = 0.0;
	float count = 0.0;
	for (int i = 0; i < PCSS_BLOCKER_SAMPLES; ++i)
	{
		vec2 off = vogelDisk(i, PCSS_BLOCKER_SAMPLES, rotSC) * radiusUV;
		float d = textureLod(u_shadowMapDepth, vec3(uv + off, float(cascade)), 0.0).r;
		if (d < receiverDepth)
		{
			sum += d;
			count += 1.0;
		}
	}
	avgBlocker = (count > 0.0) ? sum / count : 0.0;
	return count;
}

// Projects a world position into one cascade's shadow space. Returns (uv, biased depth, valid flag).
// The receiver is offset along its normal by normalOffset (world units) and the compare depth is
// nudged by depthBias; both are scaled per-cascade and by surface slope at the call site.
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

// Full PCSS for one cascade: blocker search -> penumbra estimate -> variable-radius PCF. Returns
// visibility in [0,1]; fragments outside the cascade or with no occluders read as fully lit.
float pcssCascade(vec4 p, int cascade, float texelUV, vec2 rotSC)
{
	if (p.w < 0.5)
		return 1.0; // outside this cascade's coverage
	float searchRadiusUV = PCSS_MAX_PENUMBRA_TEXELS * texelUV;
	float avgBlocker;
	float blockers = blockerSearch(p.xy, cascade, p.z, searchRadiusUV, rotSC, avgBlocker);
	if (blockers <= 0.0)
		return 1.0; // no occluders found
	// Directional penumbra: width grows with the world gap to the blocker (constant across cascades
	// once expressed in texels). Caster touching the surface => ~MIN texels (sharp); far => up to MAX.
	float penumbraTexels = clamp((p.z - avgBlocker) * pcssSunSizeTexels(cascade), PCSS_MIN_PENUMBRA_TEXELS, PCSS_MAX_PENUMBRA_TEXELS);
	return pcfVogelSingle(p, cascade, penumbraTexels * texelUV, rotSC);
}

// Border PCSS: the fixed tap budget (blocker + filter) is split between the two cascades by t, so a
// border pixel costs the same as a normal one. Each tap is routed to a cascade by a deterministic
// low-discrepancy key, keeping both subsets spatially uniform; each cascade's visibility is averaged
// over its own taps, then blended by the continuous factor t.
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
	for (int i = 0; i < PCSS_BLOCKER_SAMPLES; ++i)
	{
		bool useB = fract(float(i) * GOLDEN_RATIO_FRACT) < t;
		if ((useB && !validB) || (!useB && !validA))
			continue;
		vec4 p = useB ? pb : pa;
		vec2 off = vogelDisk(i, PCSS_BLOCKER_SAMPLES, rotSC) * searchRadiusUV;
		float d = textureLod(u_shadowMapDepth, vec3(p.xy + off, float(useB ? cb : ca)), 0.0).r;
		if (d < p.z)
		{
			if (useB) { sumDB += d; nDB += 1.0; }
			else      { sumDA += d; nDA += 1.0; }
		}
	}
	// The blocker search only SIZES the penumbra; it never decides lit vs shadowed (the PCF does). With
	// the tap budget split, a cascade can easily find zero blockers even in shadow, so fall back to the
	// minimum (sharp) radius rather than declaring it lit -- otherwise the PCF is skipped and a bright
	// line appears between two fully-shadowed cascades.
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
	// If one cascade is invalid (or got no taps), use the other's visibility instead of injecting light.
	if (!validA || nA == 0.0) return sumB / nB;
	if (!validB || nB == 0.0) return sumA / nA;
	return mix(sumA / nA, sumB / nB, t);
}

// Selects the cascade, computes a cross-fade factor into the next one, and returns sun visibility.
float sampleSunShadow(vec3 worldPos, vec3 N)
{
	int cascade = getSunCascade(worldPos);
	int nextCascade = min(cascade + 1, NUM_SHADOW_CASCADES - 1);

	// Blend factor t ramps 0->1 across the last SHADOW_CASCADE_BLEND fraction of this cascade's range.
	float dist = length(worldPos - u_sceneFocus.xyz); // the cascades are concentric about the scene focus (the player in game mode)
	float splitFar = cascadeSplit(cascade);
	float prevSplit = (cascade > 0) ? cascadeSplit(cascade - 1) : 0.0;
	float bandStart = mix(prevSplit, splitFar, 1.0 - SHADOW_CASCADE_BLEND);
	float t = (cascade == nextCascade) ? 0.0 : clamp((dist - bandStart) / max(splitFar - bandStart, 1e-4), 0.0, 1.0);

	// Slope factor (tan of the angle between N and the sun): widen bias at grazing angles where acne
	// and leaking appear, leaving flat-lit surfaces lightly biased. The normal offset is additionally
	// scaled by each cascade's world texel size so near/far cascades get a matching offset.
	vec3 L = u_sunDirection.xyz; // normalized on the CPU
	float NdotL = clamp(dot(N, L), 0.0, 1.0);
	float slope = clamp(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 1e-3), 1.0, 4.0);
	// Distant fragments get progressively larger biases: shadow map depth precision and texel density
	// drop with distance, so the near-tuned bias starts to acne/leak far out. Ramps 1 -> MAX across the
	// full cascade range.
	const float SHADOW_BIAS_DIST_MAX = 6.0;
	float distScale = mix(0.0, SHADOW_BIAS_DIST_MAX, clamp(dist / cascadeSplit(NUM_SHADOW_CASCADES - 1), 0.0, 1.0));
	float depthBias = u_shadowParams.x * slope * distScale;
	float normalScale = u_shadowParams.y * slope * distScale;

	vec4 pa = projectCascade(worldPos, N, cascade, normalScale * cascadeTexelWorldSize(cascade), depthBias);
	float ditherBase = interleavedGradientNoise(gl_FragCoord.xy);

	float texelUV = u_shadowParams.z; // 1 / resolution
	float rotation = ditherBase * 6.2831853;
	vec2 rotSC = vec2(cos(rotation), sin(rotation)); // the ONE sincos per pixel every Vogel tap rotates by
	// Outside the cross-fade band: one full-quality cascade evaluation. Inside it: split the same tap
	// budget across both cascades (constant cost) and blend by t.
	if (t <= 0.0)
		return pcssCascade(pa, cascade, texelUV, rotSC);
	else
	{
		vec4 pb = projectCascade(worldPos, N, nextCascade, normalScale * cascadeTexelWorldSize(nextCascade), depthBias);
		return pcssBorder(pa, pb, cascade, nextCascade, texelUV, rotSC, t);
	}
}

// Debug overlay, compiled in by the SHADOW_DEBUG define ("Shadows/Debug mode" — a pipeline reload, not a
// uniform, so the release shader carries none of it). Applied to the final lit colour:
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
	float splitFar = cascadeSplit(cascade);
	float prevSplit = (cascade > 0) ? cascadeSplit(cascade - 1) : 0.0;
	float bandStart = mix(prevSplit, splitFar, 1.0 - SHADOW_CASCADE_BLEND);
	float t = (cascade == NUM_SHADOW_CASCADES - 1) ? 0.0 : clamp((dist - bandStart) / max(splitFar - bandStart, 1e-4), 0.0, 1.0);
	return mix(mix(color, cascadeDebugColor(cascade), 0.3), vec3(1.0), t);
#elif SHADOW_DEBUG == 3
	return vec3(sampleSunShadow(worldPos, N));
#else
	float heat = clamp(log2(cascadeTexelWorldSize(cascade) / 0.05) / log2(1.0 / 0.05), 0.0, 1.0);
	return mix(color, mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat), 0.6);
#endif
}
#endif
