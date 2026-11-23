#ifndef SNOW_BRDF_INC_GLSL
#define SNOW_BRDF_INC_GLSL

// THE SNOW'S SUN BRDF (instanced_indirect_lit.inc.glsl doSunLight, under LIT_SNOW, on the pixels with snow: g_snowW > 0).
// Snow is a dense pack of ice grains: the light scatters many times inside it and leaves it nearly white, so its
// reflection is not Lambert's:
//   - the ASYMPTOTIC RADIATIVE TRANSFER reflection of a weakly absorbing granular layer (Kokhanovsky and Zege):
//     R = R0(mu0, mu, theta) x r_s^(u(mu0) u(mu) / R0), r_s the spherical albedo (snowAlbedo). R0 is the non-absorbing
//     layer's reflection with the ice grains' phase function P(theta): bright with the sun ahead at grazing angles (the
//     forward sheen, "Forward scattering"), about 1 with the sun overhead;
//   - the DIFFUSION across the relief ("Soft terminator"): the light that enters on a sunlit flank leaves on the shaded one,
//     so per channel the incidence cosine is pulled toward the smooth base normal's (g_snowBaseNoL) - blue farthest, as red
//     is absorbed over the longer path. It moves light, it adds none: on flat snow it changes nothing;
//   - the ICE FACETS: GGX at the ice's F0 x the glint factor g_snowGlint ("Sparkle strength" x snow.inc.glsl snowGlint:
//     single grains that mirror the sun; its mean is the strength at every distance), capped at "Sparkle max" after the
//     Fresnel term.
// Mixed with the surface's own BRDF by g_snowW. The local lights, the GI and the ambient keep Lambert + GGX with the
// snow's albedo. The includer has punctual_lights.inc.glsl (the fp16 BRDF) and the frame UBO.

#define SNOW_ICE_F0 0.018 // ice, n = 1.31

vec3 snowSunLightH(vec3 lightRadiance, f16vec3 L, f16vec3 V, f16vec3 N, f16vec3 Ns, f16vec3 specularCol, f16vec3 matColOverPi, float16_t alpha)
{
	const float16_t one = float16_t(1.0), w = g_snowW;
	const f16vec3 H = halfVectorH(L, V); // floored: no NaN at V = -L
	const float16_t NoLr = dot(N, L);
	const float16_t NoL = max(NoLr, float16_t(0.0));
	const float16_t NoV = max(dot(N, V), float16_t(0.0));

	// The facets (from Ns: a leaf keeps its curve under thin snow). X multiplies AFTER the half-range clamp, so D x Vis = 0
	// stays 0 (never inf x 0).
	const float16_t NoLs = max(dot(Ns, L), float16_t(0.0)), NoVs = max(dot(Ns, V), float16_t(0.0)), NoHs = max(dot(Ns, H), float16_t(0.0));
	const f16vec3 F = FresnelSchlickH(dot(H, V), mix(specularCol, f16vec3(SNOW_ICE_F0), w));
	const float16_t dv = min(D_GGX_H(alpha, NoHs, cross(Ns, H)) * (V_SmithGGXCorrelatedFastH(NoVs, NoLs, alpha) * NoLs), float16_t(MEDIUMP_FLT_MAX));
	// The surface's own lobe keeps its full value (1 - w); the snow's facets (x the glint factor, which holds "Sparkle
	// strength") are capped AFTER the Fresnel term - "Sparkle max" in units of the white reflection at every angle. An fp16
	// inf from dv x the factor clamps there too.
	const f16vec3 Fs = (F * (dv * (one - w)) + min(F * (dv * g_snowGlint), f16vec3(u_snow_glintCap)) * w) * antiLightFade(L, V);
	const f16vec3 lambert = (f16vec3(1.0) - F) * matColOverPi * NoL;

	// The radiative transfer. The phase in fp32 (acos, exp2), then half: theta in degrees, 0 = forward scattering.
	const float thetaDeg = acos(clamp(-dot(vec3(L), vec3(V)), -1.0, 1.0)) * 57.2957795;
	const float16_t P = float16_t(u_snow_forward * (11.1 * exp2(-0.125514 * thetaDeg) + 1.1 * exp2(-0.020198 * thetaDeg)));
	const float16_t mu0 = max(NoL, float16_t(0.02)), mu = max(NoV, float16_t(0.05));
	const float16_t s = mu0 + mu; // >= 0.07
	const float16_t R0 = (float16_t(1.247) + float16_t(1.186) * s + float16_t(5.157) * mu0 * mu + P) / (float16_t(4.0) * s);
	const float16_t e = float16_t(0.183673) * (one + float16_t(2.0) * mu0) * (one + float16_t(2.0) * mu) / R0; // (3/7)^2 u(mu0) u(mu) / R0
	const f16vec3 R = R0 * exp2(e * log2(max(matColOverPi * float16_t(PI), f16vec3(1e-3))));
	// The diffusion: per channel the incidence cosine toward the smooth base's.
	const f16vec3 mu0L = max(mix(f16vec3(NoLr), f16vec3(g_snowBaseNoL), f16vec3(u_snow_diffusion)), f16vec3(0.0));
	const f16vec3 art = R * mu0L * float16_t(INV_PI); // no (1 - F): the scattering already holds the grains' surfaces
	return vec3(Fs + mix(lambert, art, w)) * lightRadiance;
}

#endif
