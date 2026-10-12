#ifndef OCEAN_BUBBLES_INC_GLSL
#define OCEAN_BUBBLES_INC_GLSL

// Entrained-bubble cloud (the foam field's amount, ocean_foam_field.inc.glsl): the churn breaking leaves UNDER the surface.
// A high-albedo scattering layer "Bubble depth" down, so the water above absorbs the light both ways: red goes first and
// the cloud reads bright turquoise, not grey. Shared by the ocean's top side and the terrain film so both keep the same
// colour at the hand-over. HALF math (both callers shade the top side in half).
//   bubble = foamAlbedo * brightness * (sun + sky at depth) * T_up + inscatter * (1 - T_up)
// depth: m of water over the cloud. NoV: the facet's N.V (the view refracts through it). sunCos: max(L.y, 0) (the cloud
// lies under the MEAN surface). sunLight: the shadowed sun at the surface; skyLight: sky + ambient (whitewater's E / pi).
f16vec3 oceanBubbleRadiance(float16_t depth, float16_t NoV, float16_t sunCos, f16vec3 sunLight, f16vec3 skyLight, f16vec3 inscatter)
{
    const f16vec3 sigma = f16vec3(u_ocean_absorption);
    const float16_t one = float16_t(1.0);
    const float16_t invN2 = float16_t(1.0 / (1.33 * 1.33));
    const float16_t muV = sqrt(one - (one - NoV * NoV) * invN2);       // refracted view cosine
    const float16_t muL = sqrt(one - (one - sunCos * sunCos) * invN2); // refracted sun cosine
    const f16vec3 tUp = exp(-sigma * (depth / muV));
    const f16vec3 light = sunLight * (sunCos * float16_t(INV_PI)) * exp(-sigma * (depth / muL)) + skyLight * exp(-sigma * depth);
    return f16vec3(u_ocean_foamColor) * float16_t(u_ocean_bubbleBrightness) * light * tUp + inscatter * (f16vec3(1.0) - tUp);
}

// The same at the frame's own "Bubble depth" (the ocean): the CPU folds the path DOWN and the albedo (u_ocean_bubbleSun /
// bubbleSky, buildUboOcean), so only the path up is per pixel - one exp, not three (three set the ocean's register peak).
f16vec3 oceanBubbleRadianceFrame(float16_t NoV, f16vec3 sunLight, f16vec3 skyLight, f16vec3 inscatter)
{
    const float16_t one = float16_t(1.0);
    const float16_t muV = sqrt(one - (one - NoV * NoV) * float16_t(1.0 / (1.33 * 1.33))); // refracted view cosine
    const f16vec3 tUp = exp(-f16vec3(u_ocean_absorption) * (float16_t(u_ocean_bubbleDepth) / muV));
    return (sunLight * f16vec3(u_ocean_bubbleSun) + skyLight * f16vec3(u_ocean_bubbleSky)) * tUp + inscatter * (f16vec3(1.0) - tUp);
}

#endif
