#ifndef SNOW_BRDF_INC_GLSL
#define SNOW_BRDF_INC_GLSL

// THE SNOW'S SUN BRDF (instanced_indirect_lit.inc.glsl, under LIT_SNOW). Snow is a dense pack of ice grains: the light
// scatters many times inside it and leaves it nearly white, so its reflection is not Lambert's. Two parts:
//   - THE DIFFUSE (snowSunDiffuseK, worked out in snow.inc.glsl snowLitSetup BEFORE computeLitColor as ONE scalar,
//     g_snowSunK, that doSunLight multiplies by the albedo and the sun): the ASYMPTOTIC RADIATIVE TRANSFER reflection of a weakly absorbing granular
//     layer (Kokhanovsky and Zege), R = R0(mu0, mu, theta) x r_s^(u(mu0) u(mu) / R0), r_s the spherical albedo
//     (snowAlbedo). R0 is the non-absorbing layer's reflection with the ice grains' phase function P(theta): bright with
//     the sun ahead at grazing angles (the forward sheen, "Forward scattering"), about 1 with the sun overhead. And THE
//     DIFFUSION across the relief ("Soft terminator"): the light that enters on a sunlit flank leaves on the shaded one,
//     so the incidence cosine is pulled toward the smooth base normal's; it moves light, it adds none (on flat snow it
//     changes nothing). Formed there, not in doSunLight: in the sun's BRDF it was live beside the sun's radiance, shadow
//     and lobe, and cost the lit FS 16 B of spill.
//   - THE ICE FACETS (snowSunLightH, in doSunLight): GGX at the ice's F0 x the glint factor g_snowGlint ("Sparkle
//     strength" x snow.inc.glsl snowGlint: single grains that mirror the sun; its mean is the strength at every distance),
//     capped at "Sparkle max" after the Fresnel term; the surface's own lobe and Lambert by 1 - the share.
// The local lights, the GI and the ambient keep Lambert + GGX with the snow's albedo. The includer has
// local_lights.inc.glsl (the fp16 BRDF) and the frame UBO.

#define SNOW_ICE_F0 0.018 // ice, n = 1.31

// The sun's BRDF x N.L on every pixel of a LIT_SNOW includer: at a snow share of 0 it IS doLightH / doLightSplitH (the same
// lobe, the same Lambert). Ns = the specular normal (a leaf keeps its curve under thin snow).
vec3 snowSunLightH(vec3 lightRadiance, f16vec3 L, f16vec3 V, f16vec3 N, f16vec3 Ns, f16vec3 specularCol, f16vec3 matColOverPi, float16_t alpha)
{
	const float16_t one = float16_t(1.0), w = g_snowW;
	const f16vec3 H = halfVectorH(L, V); // floored: no NaN at V = -L
	const float16_t NoL = max(dot(N, L), float16_t(0.0));
	// X multiplies AFTER the half-range clamp, so D x Vis = 0 stays 0 (never inf x 0).
	const float16_t NoLs = max(dot(Ns, L), float16_t(0.0)), NoVs = max(dot(Ns, V), float16_t(0.0)), NoHs = max(dot(Ns, H), float16_t(0.0));
	const f16vec3 F = FresnelSchlickH(dot(H, V), mix(specularCol, f16vec3(SNOW_ICE_F0), w));
	const float16_t dv = min(D_GGX_H(alpha, NoHs, cross(Ns, H)) * (V_SmithGGXCorrelatedFastH(NoVs, NoLs, alpha) * NoLs), float16_t(MEDIUMP_FLT_MAX));
	// The surface's own lobe keeps its full value (1 - w); the snow's facets (x the glint factor, which holds "Sparkle
	// strength") are capped AFTER the Fresnel term - "Sparkle max" in units of the white reflection at every angle. An fp16
	// inf from dv x the factor clamps there too.
	const f16vec3 Fs = (F * (dv * (one - w)) + min(F * (dv * g_snowGlint), f16vec3(u_snow_glintCap)) * w) * antiLightFade(L, V);
	// Lambert by the surface's share; the snow's diffuse = the albedo / pi x g_snowSunK (snowSunDiffuseK) by its share.
	return vec3(Fs + matColOverPi * ((f16vec3(1.0) - F) * (NoL * (one - w)) + w * g_snowSunK)) * lightRadiance;
}

// THE SNOW'S DIFFUSE REFLECTION of the sun as ONE scalar K (the albedo / pi x K x the radiance is the reflected light): the
// reflection R / the albedo, x the diffused incidence cosine. One scalar live into doSunLight - a colour (the diffusion's
// blue on the shaded flank, per channel) held three halves across the sun and cost the lit FS 16 B of spill. NoLr = the
// shading normal's raw sun cosine, baseNoL = the smooth base normal's, NoV = the view's, LoV = dot(L, V), albedo = the
// surface's luminance.
float16_t snowSunDiffuseK(float16_t NoLr, float16_t baseNoL, float16_t NoV, float16_t LoV, float16_t albedo)
{
	const float16_t one = float16_t(1.0);
	// The phase P(theta) (theta = the scattering angle, 0 = forward) with no acos: theta ~ 81 deg x sqrt(1 - cos theta) (exact
	// to ~1 deg out to 60 deg, where the forward peak lives; 115 deg for 180 - the backward tail, ~0.1, reads ~0.2). Half
	// math: the peak is 12.2.
	const float16_t sq = sqrt(max(one + LoV, float16_t(0.0))); // 1 - cos theta = 1 + L.V
	const float16_t P = float16_t(u_snow_forward) * (float16_t(11.1) * exp2(float16_t(-10.17) * sq) + float16_t(1.1) * exp2(float16_t(-1.637) * sq));
	const float16_t mu0 = max(NoLr, float16_t(0.02)), mu = max(NoV, float16_t(0.05));
	const float16_t s = mu0 + mu; // >= 0.07
	const float16_t R0 = (float16_t(1.247) + float16_t(1.186) * s + float16_t(5.157) * mu0 * mu + P) / (float16_t(4.0) * s);
	const float16_t e = float16_t(0.183673) * (one + float16_t(2.0) * mu0) * (one + float16_t(2.0) * mu) / R0; // (3/7)^2 u(mu0) u(mu) / R0
	// r_s^e / r_s to first order in the absorption (1 - r_s): snow's albedo is near 1 (0.98 at e 2.1: 0.978 for 0.979; grimy
	// 0.75 at e 2: 0.75 for 0.75) - no log2 / exp2.
	const float16_t absorb = max(one - (e - one) * (one - albedo), float16_t(0.0));
	// The diffusion: the incidence cosine toward the smooth base's.
	const float16_t mu0L = max(mix(NoLr, baseNoL, float16_t(u_snow_diffusion)), float16_t(0.0));
	return R0 * absorb * mu0L; // no (1 - F): the scattering already holds the grains' surfaces
}

#endif
