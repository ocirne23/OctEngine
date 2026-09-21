// Punctual / spot / area / tube light evaluation + ray-traced light shadows, shared by the forward
// static-mesh shader and the ocean water shader. Cook-Torrance GGX specular with representative-point
// approximations for area (rect) and tube lights; doLightShadowed gates the analytic result with
// alpha-masked-aware shadow rays (u_rtLightShadows), stratified over the emitter via a per-pixel
// spatiotemporal jitter that TAA integrates.
//
// Requires the includer to have declared/included, with these names:
//   - struct LightInfo + in_lightInfos[] (only for callers passing LightInfo; the core functions take it)
//   - rtShadowVisibility (rt_shadow.inc.glsl) + its geometry/texture requirements
//   - UBO (ubo.inc.glsl via shared.inc.glsl) + PI
//   - fragment stage (gl_FragCoord-based shadow jitter)
//   - GL_EXT_shader_explicit_arithmetic_types (the light types evaluate through the fp16 BRDF, see D_GGX_H)

#ifndef PUNCTUAL_LIGHTS_INC_GLSL
#define PUNCTUAL_LIGHTS_INC_GLSL
// Hard ray-traced visibility toward a point on the light; tMax stops just short of the target so the
// light's own position never registers as a hit. Alpha-masked geometry is alpha-tested.
float traceLightVisibility(vec3 pos, vec3 N, vec3 target)
{
	vec3 toLight = target - pos;
	float dist = length(toLight);
	return rtShadowVisibility(pos + N * 0.02, toLight / dist, 0.01, dist - 0.02);
}
uint hashU(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
// Spatiotemporal blue-noise-style jitter. The per-pixel seed is *static* across frames (white noise,
// spatially decorrelated between neighbours); each pixel's offset is then advanced along a low-discrepancy
// golden-ratio sequence per frame. So the TAA history window accumulates a well-stratified set per pixel
// (fast, low-variance convergence) instead of N independent white-noise draws. The temporal increment is a
// distinct irrational from R2_OFFSET (the within-frame per-ray stride) so the two sequences don't resonate.
#define SHADOW_TEMPORAL_OFFSET vec2(0.5545497, 0.3083158)
vec2 shadowJitter()
{
	const uint seed = hashU(uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u);
	const vec2 base = vec2(float(seed & 0x00FFFFFFu), float(hashU(seed) & 0x00FFFFFFu)) / float(0x01000000u);
	return fract(base + float(u_frameIndex) * SHADOW_TEMPORAL_OFFSET);
}
// R2 additive recurrence offsets successive rays so they stratify over the emitter within one frame.
#define R2_OFFSET vec2(0.7548777, 0.5698403)
// u_sunShadowRays rays jittered within the sun's angular cone (u_sunAngularCos), stratified across the
// disc with the R2 recurrence; TAA resolves the remaining noise over time. More rays smooth out the
// IGN's structured pattern when the disc is wide.
float traceSunVisibility(vec3 pos, vec3 N)
{
	const vec3 L = normalize(u_sunDirection.xyz);
	const vec3 ref = abs(L.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	const vec3 T = normalize(cross(ref, L));
	const vec3 B = cross(L, T);
	const vec3 origin = pos + N * 0.02;

	const uint numRays = clamp(uint(u_sunShadowRays), 1u, 8u);
	const vec2 jitter = shadowJitter();
	float vis = 0.0;
	for (uint i = 0u; i < numRays; ++i)
	{
		const vec2 u = fract(jitter + R2_OFFSET * float(i));
		const float cosTheta = mix(u_sunAngularCos, 1.0, u.x); // uniform over the spherical cap
		const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
		const float phi = 6.2831853 * u.y;
		const vec3 dir = T * (sinTheta * cos(phi)) + B * (sinTheta * sin(phi)) + L * cosTheta;
		vis += rtShadowVisibility(origin, dir, 0.01, 10000.0);
	}
	return vis / float(numRays);
}

float squareFalloff(float dist, float lightRadius)
{
    float attenuation = 1.0 / (dist * dist + 1.0);
    float dr = dist / lightRadius;
    float dr2 = dr * dr;
    float falloff = clamp(1.0 - dr2 * dr2, 0.0, 1.0);
    falloff *= falloff;
    return attenuation * falloff;
}
float DistributionGGX(const float NdotH, const float roughnessSq)
{
	float denom = (NdotH * NdotH) * (roughnessSq - 1.0) + 1.0;
	return roughnessSq / (PI * denom * denom);
}
float V_SmithGGXCorrelated(const float NdotV, const float NdotL, const float roughnessSq)
{
	float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - roughnessSq) + roughnessSq);
	float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - roughnessSq) + roughnessSq);
	return 0.5 / max(ggxV + ggxL, 1e-5);
}
float V_SmithGGXCorrelatedFast(const float NdotV, const float NdotL, const float roughness)
{
	float ggxV = NdotL * (NdotV * (1.0 - roughness) + roughness);
	float ggxL = NdotV * (NdotL * (1.0 - roughness) + roughness);
	return 0.5 / max(ggxV + ggxL, 1e-5);
}
vec3 FresnelSchlickRoughness(float HdotV, vec3 F0, float roughness)
{
	float x = 1.0 - HdotV;
	float x2 = x * x;
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * (x2 * x2 * x);
}
// The COLOUR side of the BRDF runs in HALF math - Fresnel, kD and the colour factors live in [0, 1] - and
// its inputs (specularCol, matColOverPi) are half end to end. GGX D / visibility, N.L and the radiance stay
// 32-bit (roughness^4 underflows in half; radiance is HDR); each colour factor widens ONCE, at the
// multiply with the radiance.
f16vec3 FresnelSchlick(float HdotV, f16vec3 F0)
{
	float16_t x = float16_t(clamp(1.0 - HdotV, 0.0, 1.0));
	float16_t x2 = x * x;
	return F0 + (f16vec3(1.0) - F0) * (x2 * x2 * x);
}
vec3 doLightDiffuseOnly(vec3 lightRadiance, f16vec3 F, f16vec3 matColOverPi, float metalness, float NdotL)
{
	const f16vec3 kDColor = (f16vec3(1.0) - F) * matColOverPi * float16_t(1.0 - metalness);
	return vec3(kDColor) * (lightRadiance * NdotL);
}
vec3 doPointLightSpecular(vec3 lightRadiance, f16vec3 F, float NdotL, float NdotV, float NdotH, float roughness, float roughnessSq)
{
	float NDF = DistributionGGX(NdotH, roughnessSq);
	float Vis = V_SmithGGXCorrelatedFast(NdotV, NdotL, roughness);
	return vec3(F) * ((NDF * Vis * NdotL) * lightRadiance);
}
vec3 doAreaLightSpecular(vec3 lightRadiance, vec3 Lspec, vec3 V, vec3 N, f16vec3 specularCol, float lightSize, float distSpec, float roughness, float roughnessSq)
{
	float alphaPrime = clamp(roughness + lightSize / (2.0 * distSpec), 0.0, 1.0);
	float ap2        = alphaPrime * alphaPrime;
	float sphereNorm = roughnessSq / max(ap2, 1e-5);

	vec3  H      = normalize(Lspec + V);
	float NdotL  = max(dot(N, Lspec), 0.0);
	float NdotV  = max(dot(N, V), 0.0);
	float HdotV  = max(dot(H, V), 0.0);
	float NdotH  = max(dot(N, H), 0.0);

	f16vec3 F = FresnelSchlick(HdotV, specularCol);
	float NDF = DistributionGGX(NdotH, ap2) * sphereNorm;
	float Vis = V_SmithGGXCorrelatedFast(NdotV, NdotL, roughness);
	return vec3(F) * ((NDF * Vis * NdotL) * lightRadiance);
}
vec3 doLight(vec3 lightRadiance, vec3 L, vec3 V, vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float roughness, float roughnessSq)
{
	vec3 H = normalize(L + V);
	float NdotL = max(dot(N, L), 0.0);
	float NdotV = max(dot(N, V), 0.0);
	float HdotV = max(dot(H, V), 0.0);
	float NdotH = max(dot(N, H), 0.0);

	f16vec3 F = FresnelSchlick(HdotV, specularCol);
	vec3 specular = doPointLightSpecular(lightRadiance, F, NdotL, NdotV, NdotH, roughness, roughnessSq);
	return specular + doLightDiffuseOnly(lightRadiance, F, matColOverPi, metalness, NdotL);
}

// ---- fp16 BRDF ------------------------------------------------------------------------------------------
// Copies of the BRDF above in HALF math end to end: N, V, L, H, the dots, GGX, visibility, Fresnel and the
// colour factors. Positions, distances and the HDR radiance stay 32-bit; each light's BRDF factor widens
// ONCE, at the multiply with its radiance. `alpha` is the GGX alpha, >= 0.01 at every caller (smaller
// underflows (NoH * alpha)^2).
#define MEDIUMP_FLT_MAX 65504.0
// Filament's fp16 GGX: 1 - NoH^2 taken as |N x H|^2 (no cancellation near the highlight, where NoH has no
// half precision left), and the square taken AFTER the divide, so alpha^4 never forms on its own.
float16_t D_GGX_H(float16_t alpha, float16_t NoH, f16vec3 NxH)
{
	const float16_t a = NoH * alpha;
	const float16_t k = alpha / (dot(NxH, NxH) + a * a);
	return min(k * k * float16_t(INV_PI), float16_t(MEDIUMP_FLT_MAX));
}
float16_t V_SmithGGXCorrelatedFastH(float16_t NoV, float16_t NoL, float16_t alpha)
{
	const float16_t ggxV = NoL * (NoV * (float16_t(1.0) - alpha) + alpha);
	const float16_t ggxL = NoV * (NoL * (float16_t(1.0) - alpha) + alpha);
	return float16_t(0.5) / max(ggxV + ggxL, float16_t(1e-4));
}
f16vec3 FresnelSchlickH(float16_t HoV, f16vec3 F0)
{
	const float16_t x = clamp(float16_t(1.0) - HoV, float16_t(0.0), float16_t(1.0));
	const float16_t x2 = x * x;
	return F0 + (f16vec3(1.0) - F0) * (x2 * x2 * x);
}
vec3 doLightDiffuseOnlyH(vec3 lightRadiance, f16vec3 F, f16vec3 matColOverPi, float16_t metalness, float16_t NoL)
{
	return vec3((f16vec3(1.0) - F) * matColOverPi * ((float16_t(1.0) - metalness) * NoL)) * lightRadiance;
}
// (D * Vis * NoL) is clamped to the half range: Vis * NoL <= 0.5 / alpha, so D * that can overflow at the
// peak of a very smooth lobe. Vis * NoL is formed first, so NoL = 0 gives 0, never inf * 0.
vec3 doAreaLightSpecularH(vec3 lightRadiance, f16vec3 Lspec, f16vec3 V, f16vec3 N, f16vec3 specularCol, float lightSize, float distSpec, float16_t alpha)
{
	const float16_t alphaPrime = float16_t(clamp(float(alpha) + lightSize / (2.0 * distSpec), 0.0, 1.0));
	const float16_t normRatio  = alpha / alphaPrime;

	const f16vec3 H       = normalize(Lspec + V);
	const float16_t NoL   = max(dot(N, Lspec), float16_t(0.0));
	const float16_t NoV   = max(dot(N, V), float16_t(0.0));
	const float16_t NoH   = max(dot(N, H), float16_t(0.0));
	const f16vec3 F       = FresnelSchlickH(max(dot(H, V), float16_t(0.0)), specularCol);
	const float16_t spec  = min(D_GGX_H(alphaPrime, NoH, cross(N, H)) * (normRatio * normRatio)
		* (V_SmithGGXCorrelatedFastH(NoV, NoL, alpha) * NoL), float16_t(MEDIUMP_FLT_MAX));
	return vec3(F * spec) * lightRadiance;
}
vec3 doLightH(vec3 lightRadiance, f16vec3 L, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float16_t metalness, float16_t alpha)
{
	const f16vec3 H     = normalize(L + V);
	const float16_t NoL = max(dot(N, L), float16_t(0.0));
	const float16_t NoV = max(dot(N, V), float16_t(0.0));
	const float16_t NoH = max(dot(N, H), float16_t(0.0));
	const f16vec3 F     = FresnelSchlickH(max(dot(H, V), float16_t(0.0)), specularCol);
	const float16_t spec = min(D_GGX_H(alpha, NoH, cross(N, H)) * (V_SmithGGXCorrelatedFastH(NoV, NoL, alpha) * NoL),
		float16_t(MEDIUMP_FLT_MAX));
	return vec3(F * spec + (f16vec3(1.0) - F) * matColOverPi * ((float16_t(1.0) - metalness) * NoL)) * lightRadiance;
}
// The light types evaluate through the fp16 BRDF: V and N arrive half, and roughness is the half GGX alpha.
vec3 doPointLight(LightInfo light, vec3 pos, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	vec3 lightVec = light.pos - pos;
	float dist = length(lightVec);
	vec3 L = lightVec / dist;
	float falloff = squareFalloff(dist, light.range);
	vec3 lightRadiance = light.color * falloff;
	return doLightH(lightRadiance, f16vec3(L), V, N, specularCol, matColOverPi, float16_t(metalness), roughness);
}
vec3 doSpotLight(LightInfo light, vec3 pos, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	vec3 lightVec = light.pos - pos;
	float dist = length(lightVec);
	vec3 L = lightVec / dist;
	float falloff = squareFalloff(dist, light.range);

	// rotation holds the cone half-angle; direction's length is the edge softness (small = sharp).
	float softness = length(light.direction);
	float cosAngle = dot(-L, light.direction / softness);
	float cosOuter = cos(light.rotation);
	float cosInner = mix(cosOuter, 1.0, softness);
	float spot = smoothstep(cosOuter, cosInner, cosAngle);
	vec3 lightRadiance = light.color * falloff * spot;
	return doLightH(lightRadiance, f16vec3(L), V, N, specularCol, matColOverPi, float16_t(metalness), roughness);
}
vec3 closestPointOnSegment(vec3 p, vec3 a, vec3 b)
{
	vec3 ab = b - a;
	float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
	return a + ab * t;
}
vec3 closestPointOnRect(vec3 p, vec3 center, vec3 right, vec3 up, float halfWidth, float halfHeight)
{
	vec3 d = p - center;
	float x = clamp(dot(d, right), -halfWidth, halfWidth);
	float y = clamp(dot(d, up), -halfHeight, halfHeight);
	return center + right * x + up * y;
}
void areaLightBasis(LightInfo light, out vec3 right, out vec3 up, out float halfWidth, out float halfHeight)
{
	float height = length(light.direction);
	up          = light.direction / height;
	vec3 ref    = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 right0 = normalize(cross(up, ref));
	float cs    = cos(light.rotation);
	float sn    = sin(light.rotation);
	right       = right0 * cs + cross(up, right0) * sn;
	halfWidth   = light.width * 0.5;
	halfHeight  = height * 0.5;
}
vec3 doAreaLight(LightInfo light, vec3 pos, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	vec3 right, up;
	float halfWidth, halfHeight;
	areaLightBasis(light, right, up, halfWidth, halfHeight);
	vec3 quadNormal  = cross(up, right);
	float lightSize  = (halfWidth + halfHeight) * 0.5;
	vec3 center      = light.pos;

	// One-sided emitter: ignore fragments behind the quad.
	if (dot(pos - center, quadNormal) <= 0.0)
		return vec3(0.0);

	// ---- Diffuse -------------------------------------------------------------
	vec3 closest     = closestPointOnRect(pos, center, right, up, halfWidth, halfHeight);
	vec3 LdiffVec    = closest - pos;
	float distDiff   = length(LdiffVec);
	vec3 Ldiff       = LdiffVec / max(distDiff, 1e-5);
	float facingDiff = max(dot(quadNormal, -Ldiff), 0.0);

	// Horizon clamp: smoothstep the center N.L over the quad's angular half-extent.
	// As the quad clips below the horizon it fades out proportionally to its apparent size.
	float NdotLcenter = dot(vec3(N), normalize(center - pos));
	float halfExtent  = lightSize / max(distDiff, 1e-5);
	float horizonVis  = smoothstep(-halfExtent, halfExtent, NdotLcenter);
	vec3 diffRadiance = light.color * squareFalloff(distDiff, light.range) * facingDiff;
	f16vec3 Hdiff     = normalize(f16vec3(Ldiff) + V);
	f16vec3 Fdiff     = FresnelSchlickH(max(dot(Hdiff, V), float16_t(0.0)), specularCol);
	vec3 diffuse      = doLightDiffuseOnlyH(diffRadiance, Fdiff, matColOverPi, float16_t(metalness), float16_t(horizonVis));

	// ---- Specular (representative point) -------------------------------------
	// Intersect the mirror ray with the quad plane and clamp to the rectangle, then shade from
	// that point with its own distance and facing (skipped for rough surfaces: lobe is broad).
	vec3 Lspec;
	float distSpec;
	vec3 specRadiance;
	if (roughness < float16_t(0.6))
	{
		vec3 specPoint = closest;
		vec3 R  = reflect(-vec3(V), vec3(N));
		float d = dot(R, quadNormal);
		if (d < 0.0)
		{
			float t = dot(center - pos, quadNormal) / d;
			if (t > 0.0)
				specPoint = closestPointOnRect(pos + R * t, center, right, up, halfWidth, halfHeight);
		}

		vec3 LspecVec = specPoint - pos;
		distSpec      = max(length(LspecVec), 1e-4);
		Lspec         = LspecVec / distSpec;

		float facingSpec = max(dot(quadNormal, -Lspec), 0.0);
		specRadiance = light.color * squareFalloff(distSpec, light.range) * facingSpec;
	}
	else
	{
		Lspec        = Ldiff;
		distSpec     = distDiff;
		specRadiance = diffRadiance;
	}
	// Widen and renormalize the lobe by the quad's apparent size (lightSize) so the highlight spreads
	// out and dims as the light gets close to the surface, instead of staying a tiny punctual spike.
	vec3 specular = doAreaLightSpecularH(specRadiance, f16vec3(Lspec), V, N, specularCol, lightSize, distSpec, roughness);

	return diffuse + specular;
}
vec3 doTubeLight(LightInfo light, vec3 pos, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	float height   = length(light.direction);
	float halfLen  = height * 0.5;
	float radius   = abs(light.width);
	float absRange = abs(light.range);
	if (halfLen < 1e-5 || absRange < 1e-5)
		return vec3(0.0);

	vec3 axis = light.direction / height;
	vec3 pa   = light.pos - axis * halfLen;
	vec3 pb   = light.pos + axis * halfLen;

	// ---- Diffuse -------------------------------------------------------------
	// Closest point on the core segment drives distance falloff and the horizon fade.
	vec3 closest   = closestPointOnSegment(pos, pa, pb);
	vec3 LdiffVec  = closest - pos;
	float coreDist = length(LdiffVec);
	vec3 Ldiff     = LdiffVec / max(coreDist, 1e-5);
	float distDiff = max(coreDist - radius, 1e-4); // shade from the tube surface, not its core

	// Horizon clamp: fade as the tube sinks below the tangent plane, scaled by its apparent size.
	float NdotLcenter = dot(vec3(N), normalize(light.pos - pos));
	float halfExtent  = (halfLen + radius) / max(coreDist, 1e-5);
	float horizonVis  = smoothstep(-halfExtent, halfExtent, NdotLcenter);

	vec3 diffRadiance = light.color * squareFalloff(distDiff, absRange);
	f16vec3 Hdiff = normalize(f16vec3(Ldiff) + V);
	f16vec3 Fdiff = FresnelSchlickH(max(dot(Hdiff, V), float16_t(0.0)), specularCol);
	vec3 diffuse = doLightDiffuseOnlyH(diffRadiance, Fdiff, matColOverPi, float16_t(metalness), float16_t(horizonVis));

	// ---- Specular (representative point) -------------------------------------
	// Find the point on the tube surface most aligned with the mirror ray R, then shade it as a
	// punctual light *from that point* (its own distance/direction) with a size-widened lobe.
	vec3  Lspec;
	float distSpec;
	if (roughness < float16_t(0.6))
	{
		vec3 l0      = pa - pos;
		vec3 l1      = pb - pos;
		vec3 R       = reflect(-vec3(V), vec3(N));
		vec3 ld      = l1 - l0;
		float RdotLd = dot(R, ld);
		float ldLen2 = dot(ld, ld);
		float denom  = ldLen2 - RdotLd * RdotLd;
		float t      = (abs(denom) > 1e-6) ? clamp((dot(R, l0) * RdotLd - dot(l0, ld)) / denom, 0.0, 1.0) : 0.5;
		vec3 closestLine = l0 + ld * t;                     // closest core point to the reflection ray
		vec3 centerToRay = dot(closestLine, R) * R - closestLine;
		float ctrLen     = length(centerToRay);
		vec3 specVec     = closestLine + centerToRay * clamp(radius / max(ctrLen, 1e-5), 0.0, 1.0);\
		float specLen    = length(specVec);
		distSpec = max(specLen - radius, 1e-4);           // falloff at the actual specular distance
		Lspec    = specVec / max(specLen, 1e-5);
	}
	else
	{   // broad lobe: the representative point barely matters, reuse closest
		distSpec = coreDist;
		Lspec    = Ldiff;
	}
	vec3 specRadiance = light.color * squareFalloff(distSpec, absRange);
	vec3 specular = doAreaLightSpecularH(specRadiance, f16vec3(Lspec), V, N, specularCol, radius, distSpec, roughness);

	return diffuse + specular;
}
vec3 doLight(LightInfo light, vec3 pos, f16vec3 V, f16vec3 N, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	if (light.width > 0.0)
	{
		if (light.range < 0.0)
			return doTubeLight(light, pos, V, N, specularCol, matColOverPi, metalness, roughness);
		return doAreaLight(light, pos, V, N, specularCol, matColOverPi, metalness, roughness);
	}
	if (light.width < 0.0)
		return doSpotLight(light, pos, V, N, specularCol, matColOverPi, metalness, roughness);
	return doPointLight(light, pos, V, N, specularCol, matColOverPi, metalness, roughness);
}
// Area and tube shadows: ONE ray per pixel per frame, aimed at a jittered point on the emitter. The jitter
// is spatiotemporal (shadowJitter), so TAA integrates the penumbra over frames. A per-pixel loop of up to 4
// stratified rays cost the lit fragments 16 registers (96 -> 80) and the terrain 80 bytes of spills
// (128 -> 48): the loop state stayed live across every ray query + alpha-test loop it inlined.
float areaLightVisibility(LightInfo light, vec3 pos, vec3 N)
{
	vec3 right, up;
	float halfWidth, halfHeight;
	areaLightBasis(light, right, up, halfWidth, halfHeight);
	const vec2 u = shadowJitter() * 2.0 - 1.0;
	return traceLightVisibility(pos, N, light.pos + right * (u.x * halfWidth) + up * (u.y * halfHeight));
}
float tubeLightVisibility(LightInfo light, vec3 pos, vec3 N)
{
	float height  = length(light.direction);
	vec3 axis     = light.direction / height;
	float halfLen = height * 0.5;
	float radius  = abs(light.width);
	// Sample the visible silhouette: jitter along the axis, plus a perpendicular offset within the plane
	// facing the shaded point (the radial extent the surface actually sees).
	vec3 side  = cross(axis, pos - light.pos);
	float sideLen = length(side);
	side = sideLen > 1e-5 ? side / sideLen : vec3(0.0);
	const vec2 u = shadowJitter() * 2.0 - 1.0;
	return traceLightVisibility(pos, N, light.pos + axis * (u.x * halfLen) + side * (u.y * radius));
}
// The lit fragments bake the toggle (LIT_RT_LIGHT_SHADOWS 0/1): off compiles the ray queries out. The
// ocean has no define and reads the uniform.
#ifdef LIT_RT_LIGHT_SHADOWS
#define PL_RT_LIGHTS_COMPILED LIT_RT_LIGHT_SHADOWS
#define PL_RT_LIGHTS_ON true
#else
#define PL_RT_LIGHTS_COMPILED 1
#define PL_RT_LIGHTS_ON (u_rtLightShadows > 0.5)
#endif
// One light's ray-traced shadow visibility at pos (N = the ray origin's offset): one ray, jittered over an
// area / tube emitter. The caller gates it on PL_RT_LIGHTS_COMPILED / PL_RT_LIGHTS_ON and on a non-black
// analytic term.
float lightShadowVisibility(LightInfo light, vec3 pos, vec3 N)
{
	if (light.width > 0.0)
		return light.range < 0.0 ? tubeLightVisibility(light, pos, N) : areaLightVisibility(light, pos, N);
	return traceLightVisibility(pos, N, light.pos); // point/spot: genuinely punctual, one center ray
}
vec3 doLightShadowed(LightInfo light, vec3 pos, f16vec3 V, f16vec3 Nh, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	vec3 lit = doLight(light, pos, V, Nh, specularCol, matColOverPi, metalness, roughness);
#if PL_RT_LIGHTS_COMPILED
	if (PL_RT_LIGHTS_ON && dot(lit, lit) > 1e-7) // toggle off, or black analytic term: skip the trace
		lit *= lightShadowVisibility(light, pos, vec3(Nh));
#endif
	return lit;
}

#endif // PUNCTUAL_LIGHTS_INC_GLSL
