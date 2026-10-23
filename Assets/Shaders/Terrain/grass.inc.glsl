// PROCEDURAL GRASS, shared by the patch cull (grass_cull.cs.glsl) and the blade vertex shader (grass.vs.glsl).
// The world is a grid of PATCHES (u_grass_patchSize metres square). A patch is drawn from ONE static index buffer of N
// blades in RANK order: blade k sits at the k-th point of an R2 low-discrepancy sequence, so EVERY PREFIX of the ranks
// is evenly spread. The cull writes one indexed draw per visible patch (the first K blades); the VS keeps blade k where
// rank (k + 0.5) / N < density x thinning and shrinks it into the ground over the grow band below that. One rule thins
// by density AND by distance, so a blade fades out by shrinking, never by popping.
// Blades stand on the terrain MESH: their roots are interpolated from the chunk's own vertices (two triangles per
// cell, as TerrainGenerator.cpp splits them) - the baked height map (8 m texels) is far too coarse.
// Needs shared.inc.glsl and the includer's `in_vertices` (the vertex mega-buffer) first. GRASS_* come from Layout.ixx.

#ifndef GRASS_INC_GLSL
#define GRASS_INC_GLSL

#include "mesh_vertex.inc.glsl" // meshVertexUV (the ground's river data)

uint grassLodSegments(uint lod)
{
    return lod == 0u ? GRASS_LOD0_SEGMENTS : lod == 1u ? GRASS_LOD1_SEGMENTS : lod == 2u ? GRASS_LOD2_SEGMENTS : GRASS_LOD3_SEGMENTS;
}

// A blade of S segments: 2 vertices per row below the tip, one tip vertex; 2S - 1 triangles.
uint grassIndicesPerBlade(uint lod)
{
    return (2u * grassLodSegments(lod) - 1u) * 3u;
}

// The index buffer holds the LODs one after another, each N blades in rank order (GrassPipeline::buildIndices).
uint grassLodFirstIndex(uint lod, uint blades)
{
    uint first = 0u;
    for (uint l = 0u; l < lod; ++l)
        first += grassIndicesPerBlade(l) * blades;
    return first;
}

uint grassHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint grassHash2(ivec2 p)
{
    return grassHash(uint(p.x) * 1597334673u ^ grassHash(uint(p.y) * 3812015801u));
}

float grassUnit(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Smooth value noise in [0, 1] (clumps, bare patches, dry spots, gusts).
float grassValueNoise(vec2 p)
{
    const vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    const ivec2 c = ivec2(i);
    const float a = grassUnit(grassHash2(c));
    const float b = grassUnit(grassHash2(c + ivec2(1, 0)));
    const float d = grassUnit(grassHash2(c + ivec2(0, 1)));
    const float e = grassUnit(grassHash2(c + ivec2(1, 1)));
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

// The clumps and bare spots [0, 1] (x the cover = the blade density), and the blade size by the cover ("Size by
// cover"): shared by the blades (grass.vs.glsl) and the canopy shading of the ground under them.
float grassClump(vec2 xz)
{
    // "Bare fraction" (u_grass_bareFraction) is the noise threshold: about that share of the ground is bare / thin.
    const float bare = u_grass_bareFraction;
    return mix(1.0, smoothstep(bare - 0.2, bare + 0.2, grassValueNoise(xz * u_grass_invClumpSize)), u_grass_patchiness);
}

float grassCoverSize(float cover)
{
    return mix(1.0, cover, u_grass_sizeByCover);
}

// The blade WIDTH factor by its height factor (the size by cover x the clump's, as grassCanopyHeight): x that^-"Width by
// height" - short grass (sparse climate, tree cover, thin clumps) gets wider blades. Floored at 0.05, capped at 4.
float grassWidthSize(float coverSize, float clump)
{
    return min(pow(max(coverSize * mix(0.6, 1.0, clump), 0.05), -u_grass_widthByHeight), 4.0);
}

// THE CANOPY: grass self-shadowing without a shadow map (a blade is far below a shadow-map texel). The grass layer is
// a thin volume of blades: its top at the mean blade height, its extinction (1/m) = u_grass_canopyExtinction ("Canopy
// shadow" x blades per m^2 x the mean blade width, the CPU's fold) x the COVER (the clumps are sampled along the sun
// path) x the blades' width factor (grassWidthSize). The blades and the ground under them use the same terms, so the
// soil between the blades matches their feet.
float grassCanopyHeight(float clump, float coverSize)
{
    return u_grass_bladeHeight * (1.0 - 0.5 * u_grass_heightVariation) * mix(0.6, 1.0, clump) * coverSize;
}

float grassCanopyExtinction(float cover, float widthSize, float dist)
{
    return u_grass_canopyExtinction * cover * widthSize;
}

// The blades' "Range fade" (1 inside, 0 at the range). The canopy's RESULT fades with it, linearly: fading the
// extinction instead kept exp(-extinction x path) nearly black until the band's last metres - an edge, not a fade.
float grassRangeFade(float dist)
{
    return 1.0 - smoothstep(u_grass_range - u_grass_rangeFade, u_grass_range, dist);
}

// The sun reaching pos, `depth` metres below the canopy top, along the sun's path up through it:
// - the CLUMPS at 4 points along that path (not at pos): a clump shadows the bare ground beside it, away from the sun;
// - SUN FLECKS: what gets through arrives in bright and dark spots, not an even gray - a noise at the path's ENTRY into
//   the canopy decides each spot, about T of the ground lit (the mean keeps the transmittance T). Every point of one
//   sun path shares its spot, so a fleck lights a blade and the soil under it alike. Faded out from half the "Fleck
//   fade distance" to it: smaller than a pixel it only shimmers.
float grassCanopySun(vec3 pos, float depth, float extinction, float dist)
{
    if (extinction <= 0.0 || depth <= 0.0)
        return 1.0;

    // The sun's terms from the CPU (RendererUbo): its height floored at 0.1.
    const vec2 toSun = u_grass_sunToXZ; // XZ per metre of height along the sun
    float clumps = 0.0;
    for (int i = 0; i < 4; ++i)
        clumps += grassClump(pos.xz + toSun * (depth * (float(i) + 0.5) * 0.25));
    const float T = exp(-extinction * (0.25 * clumps) * depth * u_grass_invSunY);
    const float rangeFade = grassRangeFade(dist);
    const float contrast = u_grass_fleckContrast * (1.0 - smoothstep(0.5 * u_grass_fleckFadeDistance, u_grass_fleckFadeDistance, dist));
    if (contrast <= 0.0)
        return mix(1.0, T, rangeFade);

    // STREAKS, not round spots: upright blades cast lines away from the sun, longer as it sinks (h / tan(elevation)).
    // The noise is stretched along the sun's horizontal direction by that ratio x "Fleck stretch" (capped at 16).
    const vec2 entryPos = pos.xz + toSun * depth;
    const vec2 along = u_grass_fleckAlong;
    const vec2 entry = vec2(dot(entryPos, along) * u_grass_fleckInvStretch, dot(entryPos, vec2(-along.y, along.x))) * u_grass_invFleckSize;
    float n = 0.65 * grassValueNoise(entry) + 0.35 * grassValueNoise(entry * 2.7 + 5.1);
    n = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0); // value noise bunches at 0.5: stretched toward an even spread
    return mix(1.0, mix(T, smoothstep(n - 0.1, n + 0.1, T), contrast), rangeFade);
}

#ifdef INSTANCED_INDIRECT_LIT_INC_GLSL
// THE NEAR GRASS CASCADE (receivers: the blades, the ground under them): the real blade shadows from its own map
// (u_grassNearShadow; GrassPipeline's + ClutterPipeline's near pass), around the camera. `weight` = how
// much of it to use (fading to 0 over the box's outer part, where the canopy takes over). The receiver moves toward
// the sun by "Near shadow bias" and off its normal by one texel: the blades are thin, two-sided, and self-casting.
float grassNearShadow(vec3 pos, vec3 N, out float weight)
{
    weight = 0.0;
    if (u_grass_nearRange <= 0.0)
        return 1.0;

    const vec4 lp = u_grass_shadowViewProj * vec4(pos + u_sunDirection.xyz * u_grass_nearBias + N * u_grass_nearTexel, 1.0);
    const vec2 uv = lp.xy * 0.5 + 0.5;
    const vec2 edge = abs(uv - 0.5) * 2.0;
    // Fades by the box's edge AND by the horizontal distance from the box's CENTRE (ahead of the camera; gone at 1.3 x
    // the range): the box is square in LIGHT space, so on the ground it reaches range / sin(sun elevation) - far past
    // the casters (drawn within range x 1.5 + 2 m only), where the empty map left a bright gap before the canopy.
    const float range = u_grass_nearRange;
    weight = (1.0 - smoothstep(0.85, 0.98, max(edge.x, edge.y)))
           * (1.0 - smoothstep(range, 1.3 * range, distance(pos.xz, u_grass_nearCentre)));
    if (weight <= 0.0 || lp.z >= 1.0)
    {
        weight = 0.0;
        return 1.0;
    }

    // 4 bilinear PCF taps one texel apart (a 3x3-texel footprint): a blade is a few texels wide.
    const float texel = 1.0 / float(GRASS_NEAR_SHADOW_RESOLUTION);
    float sum = 0.0;
    sum += texture(u_grassNearShadow, vec4(uv + vec2(-0.5, -0.5) * texel, 0.0, lp.z));
    sum += texture(u_grassNearShadow, vec4(uv + vec2( 0.5, -0.5) * texel, 0.0, lp.z));
    sum += texture(u_grassNearShadow, vec4(uv + vec2(-0.5,  0.5) * texel, 0.0, lp.z));
    sum += texture(u_grassNearShadow, vec4(uv + vec2( 0.5,  0.5) * texel, 0.0, lp.z));
    return mix(1.0, 0.25 * sum, u_grass_nearStrength); // "Near shadow strength"
}
#endif

#ifdef TERRAIN_SPLAT_INC_GLSL
// THE TERRAIN TEXTURES' grass: what the beach, rock and snow layers leave of the GROUND x the grass amount of its
// climate-picked textures (u_terrain_splatGrass, TerrainSplatMaterial::grass).
float grassTextureAmount(uint slot)
{
    return u_terrain_splatGrass[slot >> 2u][slot & 3u];
}

float grassTerrainCover(TerrainLayers L)
{
    const float ground = (1.0 - float(L.beachW)) * (1.0 - float(L.rockW)) * (1.0 - float(L.snowW));
    if (ground <= 0.0)
        return 0.0;
    const float n1 = float(L.g.n1), n2 = float(L.g.n2);
    return ground * ((1.0 - n1 - n2) * grassTextureAmount(climatePickIdx(L.g, 0))
        + n1 * grassTextureAmount(climatePickIdx(L.g, 1)) + n2 * grassTextureAmount(climatePickIdx(L.g, 2)));
}

#ifdef INSTANCED_INDIRECT_LIT_INC_GLSL // it reads the near grass cascade (the lit core's u_shadowMap)
// The ground UNDER the grass at pos (the terrain FS; N = its smooth normal): x = its sun - the near cascade's blade
// shadows inside its box (on ANY ground there: blades also shade the bare soil beside them), blending into the
// canopy's outside it; y = its ambient (the blades' root occlusion, by how opaque the canopy is); z = THE FAR TIER's
// weight (grassFarAlbedo). x and y blend by z into the far tier's own light (the blades' mean canopy sun and root
// occlusion), so the far grass is as dark as the blades it continues; w = the far tier's STRIPE scale, by how dense the
// grass is (the cover x the clump, 0.05 .. 0.6): a grazing view saturates the weight over a few tufts too, but sparse
// grass has no blade-on-blade pattern. (1, 1, 0, 0) without grass.
// THE FAR TIER: where the blades thin and fade out, the ground itself takes the grass layer's look. Its weight = the
// share of the blades' cover the LOD took away x how much of the view the layer covers.
// - The cover the blades keep: grass.vs.glsl's kept fraction k (grassKeep: (start / d)^exponent x the range fade), each
//   kept blade wider by (1 / k)^"Width compensation" (capped by "Max width scale"): k^(1 - compensation), at most
//   k x the cap. The far tier fills the rest, so the soil between the thinned blades does not show as a band.
// - The view's cover: 1 - exp(-tau), tau = u_grass_farExtinction (blades per m^2 x the mean width x "Far/Thickness") x
//   the cover x the size by cover x the clump x the height / N.V - a grazing view sees grass where a view from above
//   still sees soil between the clumps.
vec4 grassGroundCanopy(TerrainLayers L, vec3 pos, vec3 N)
{
    if (u_grass_canopyExtinction <= 0.0 && u_grass_nearRange <= 0.0 && u_grass_farExtinction <= 0.0) // no canopy, no near cascade, no far tier (or no grass at all)
        return vec4(1.0, 1.0, 0.0, 0.0);
    const vec3 toEye = u_viewPos - pos;
    const float dist = length(toEye);
    const bool inRange = dist < u_grass_range;
    if (!inRange && u_grass_farExtinction <= 0.0)
        return vec4(1.0, 1.0, 0.0, 0.0);

    float nearWeight = 0.0, nearSun = 1.0;
    if (inRange)
        nearSun = grassNearShadow(pos, N, nearWeight);
    const float cover = grassTerrainCover(L);
    if (cover <= 0.0)
        return vec4(mix(1.0, nearSun, nearWeight), 1.0, 0.0, 0.0);

    const float clump = grassClump(pos.xz);
    const float coverSize = grassCoverSize(cover);
    const float widthSize = grassWidthSize(coverSize, clump);
    const float height = grassCanopyHeight(clump, coverSize);
    const float rangeFade = grassRangeFade(dist);
    const float extinction = grassCanopyExtinction(cover, widthSize, dist);
    const float kept = (dist > u_grass_thinStart ? pow(u_grass_thinStart / dist, u_grass_thinExponent) : 1.0) * rangeFade;
    const float bladeCover = kept > 0.0 ? min(pow(kept, 1.0 - u_grass_widthCompensation), kept * u_grass_maxWidthScale) : 0.0;
    // 1 / N.V without the normalize: dist / dot(N, toEye), N.V floored at 0.05. tauV = the view's optical depth through
    // the layer, kv = the same per metre of the layer's depth.
    const float tauV = u_grass_farExtinction * cover * widthSize * clump * height * dist / max(dot(N, toEye), 0.05 * dist);
    const float viewCover = 1.0 - exp(-tauV);
    const float far = (1.0 - min(bladeCover, 1.0)) * viewCover;
    // The far tier's LIGHT, as the blades get it, AT THE DEPTH THE VIEW REACHES: a view from above looks deep into the
    // layer (the shaded roots), a grazing one sees the lit tops. Along the view the layer's visible share at depth d is
    // kv e^(-kv d) / (1 - e^(-tauV)) over [0, h]:
    // - the sun = the canopy's exp(-ks d) under it (ks = the far canopy extinction - "Far/Canopy shadow", as the blades'
    //   "Canopy shadow" - x the cover x the size by cover x the clump / the sun's height: grassCanopySun's mean, without
    //   the flecks - they average out): kv / (ks + kv) x (1 - e^(-(ks + kv) h)) / viewCover;
    // - the ambient: no term of its own - the dark stripes stand in for the view down between the blades (grassFarAlbedo).
    const float sh = u_grass_farSunExtinction * cover * widthSize * clump * height * u_grass_invSunY; // ks x h
    const float farSun = viewCover > 1e-4 ? tauV / (sh + tauV) * (1.0 - exp(-(sh + tauV))) / viewCover : (sh > 1e-3 ? (1.0 - exp(-sh)) / sh : 1.0);
    const float stripeScale = smoothstep(0.05, 0.6, cover * clump);
    if (!inRange)
        return vec4(mix(1.0, farSun, far), 1.0, far, stripeScale);
    const float presence = (1.0 - exp(-extinction * clump * height)) * rangeFade; // the blades standing HERE (the ambient)
    const float canopySun = nearWeight < 1.0 ? grassCanopySun(pos, height, extinction, dist) : 1.0;
    return vec4(mix(mix(canopySun, nearSun, nearWeight), farSun, far), mix(mix(1.0, 1.0 - u_grass_rootOcclusion, presence), 1.0, far), far, stripeScale);
}

// THE FAR TIER's colour: the blades' TIP colour (a far view sees their upper part; the darker depth is "Far/Canopy
// shadow"'s, on the sun), with their dry patches (grass.vs.glsl's noise, so the patches carry on past the blades) and the cold
// darkening (the temperature at the ground), x "Far/Brightness". The colours are lockable: they fold when locked.
// THE BLADES ("Far/Blade stripes"): thin streaks, VERTICAL ON SCREEN - long along the view on the ground, thin across it
// (a world noise would compress into HORIZONTAL streaks at a grazing view), sized as blades are:
// - THE ANCHOR they are built around is NOT the camera (a pattern following the camera slides in the world, and DLSS /
//   TAA's reprojection - the static terrain's, the camera's motion only - rejected its history in motion: flicker) but a
//   stand-in EYE stepping behind it (u_grass_stripeAnchorNew: xyz, w = the blend; u_grass_stripeAnchorOld: xyz;
//   Renderer::buildUboGrass), the old one crossfading to the new (both evaluated only then, the variance re-normalized).
//   EVERYTHING the stripe value reads comes from the anchor - the azimuth, the distance and the view angle that sets the
//   length - never from the camera, so between steps the pattern is WORLD-FIXED. The far tier starts ~100 m out, so an
//   anchor a step from the camera turns the stripes a few degrees.
// - ACROSS: the AZIMUTH around the anchor (world axes: turning the camera leaves them in place), a stripe "Stripe
//   width" blade widths wide IN THE WORLD - 2 pi r / width stripes around the circle at distance r. The wrapped noise needs a WHOLE count, so two power-of-two counts (8 x 2^k) crossfade by the fraction
//   of k, as mip levels do (the variance re-normalized: a plain mix of two noises dips in contrast mid-way). A count's
//   noise offset follows its ABSOLUTE index (k0, k0 + 1), never its slot: at a whole k the coarser count becomes the
//   finer one, and a slot offset changed its pattern there - a ring at that distance.
// - ALONG: the ground a blade hides behind it, "Stripe length" x its height x cot(the view's angle to the GROUND's
//   normal) - long at a grazing view, down to the stripe width (a dot) looking straight down, right on slopes and
//   hills too; in log(distance), so the scale follows r.
float grassStripeNoise(vec2 p, int period)
{
    const vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    const int x0 = int(uint(int(i.x) + 2 * period) % uint(period)); // never % of a negative (undefined)
    const int x1 = x0 + 1 == period ? 0 : x0 + 1;
    const int y = int(i.y);
    const float a = grassUnit(grassHash2(ivec2(x0, y))), b = grassUnit(grassHash2(ivec2(x1, y)));
    const float c = grassUnit(grassHash2(ivec2(x0, y + 1))), d = grassUnit(grassHash2(ivec2(x1, y + 1)));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The stripe value [-1, 1] at pos (N: its smooth ground normal) seen from `anchor`; `width` (m). `fine` (out, 0..1):
// the same noise 4x finer across and 3x along (the single blades of "Far/Ripple glint"; 0.5 without it).
float grassStripeAt(vec3 pos, vec3 N, vec3 anchor, float width, out float fine)
{
    // Along: the hidden ground's length - from the ANCHOR's view angle - at least the width.
    const vec3 fromAnchor3 = pos - anchor;
    const float NoV = clamp(-dot(N, fromAnchor3) / length(fromAnchor3), 0.02, 1.0);
    const float stripeLength = max(u_grass_farStripeLength * sqrt(1.0 - NoV * NoV) / NoV, width);
    const vec2 fromAnchor = fromAnchor3.xz;
    const float r = max(length(fromAnchor), 1.0);
    const float along = log(r) * (r / stripeLength);
    // Across: the azimuth (0..1) x two power-of-two stripe counts around the circle; each count's along offset follows its
    // absolute index (no ring at a whole k).
    const float azimuth = atan(fromAnchor.y, fromAnchor.x) * (1.0 / 6.28318531) + 0.5;
    const float k = log2(max(6.28318531 * r / (8.0 * width), 1.0));
    const float k0 = floor(k);
    const int count0 = 8 << int(k0);
    const float n0 = grassStripeNoise(vec2(azimuth * float(count0), along + 17.0 * k0), count0);
    const float n1 = grassStripeNoise(vec2(azimuth * float(2 * count0), along + 17.0 * (k0 + 1.0)), 2 * count0);
    const float t = k - k0;
    const float norm = inversesqrt(t * t + (1.0 - t) * (1.0 - t));
    fine = 0.5;
    if (u_grass_farRippleGlint > 0.0)
    {
        const float f0 = grassStripeNoise(vec2(azimuth * float(4 * count0), along * 3.0 + 17.0 * k0 + 53.0), 4 * count0);
        const float f1 = grassStripeNoise(vec2(azimuth * float(8 * count0), along * 3.0 + 17.0 * (k0 + 1.0) + 53.0), 8 * count0);
        fine = (mix(f0, f1, t) - 0.5) * norm + 0.5;
    }
    const float n = (mix(n0, n1, t) - 0.5) * norm + 0.5;
    return smoothstep(0.2, 0.8, n) * 2.0 - 1.0;
}

// `stripe` (out): the blade's stripe value in [-1, 1] (0 without stripes) - its lean in grassFarNormal; `fine` (out):
// grassStripeAt's fine noise (0.5 without stripes).
vec3 grassFarAlbedo(vec3 pos, vec3 N, float temperature, float stripeScale, out float stripe, out float fine)
{
    const vec2 xz = pos.xz;
    const float dryNoise = grassValueNoise(xz * (u_grass_invClumpSize * 0.37) + 17.3);
    const float dryness = smoothstep(0.0, 0.3, dryNoise - (1.0 - u_grass_dryAmount));
    const float warm = smoothstep(u_grass_coldTemperature, u_grass_warmTemperature, temperature);
    stripe = 0.0;
    fine = 0.5;
    if (u_grass_farStripes > 0.0)
    {
        const float width = u_grass_farStripeWidth;
        stripe = grassStripeAt(pos, N, u_grass_stripeAnchorNew.xyz, width, fine);
        const float b = u_grass_stripeAnchorNew.w;
        if (b < 1.0) // crossfading from the old anchor (uniform: one branch per frame)
        {
            float fineOld;
            const float stripeOld = grassStripeAt(pos, N, u_grass_stripeAnchorOld.xyz, width, fineOld);
            const float norm = inversesqrt(b * b + (1.0 - b) * (1.0 - b));
            stripe = clamp(mix(stripeOld, stripe, b) * norm, -1.0, 1.0);
            fine = (mix(fineOld, fine, b) - 0.5) * norm + 0.5;
        }
        stripe *= stripeScale; // weaker on sparse grass (the normal's leans and the mirror share follow it)
    }
    // The stripes brighten and darken by "Blade stripes" (their dark half also stands in for the view down between the
    // blades). On the albedo, so the sun and the ambient alike.
    return mix(u_grass_tipAlbedo, u_grass_dryAlbedo, dryness)
        * (mix(1.0 - u_grass_coldDarkening, 1.0, warm) * (1.0 + u_grass_farStripes * stripe) * u_grass_farBrightness);
}

// THE FAR TIER's shading NORMAL: a layer of UPRIGHT blades, not a surface - N = the smooth ground normal, then
// - "Upright normal": tilted toward the viewer along the ground, by sin(the view's angle off the normal): a grazing view
//   sees the blades' SIDES, turned to it (a view from above sees the ground between them);
// - "Normal variation": the BLADES' lean - each stripe (grassFarAlbedo's `stripe`, -1..1) tilted SIDEWAYS, across the
//   view along the ground: it follows the view, so mostly the SPECULAR (with the sun behind the view a sideways tilt
//   is about perpendicular to the sun: N.L hardly moves);
// - "Diffuse variation": the same stripes leaning TOWARD / AWAY FROM THE SUN, along the ground: N.L moves with it, so
//   the DIFFUSE gets light and dark stripes - most with a low sun;
// - "Sun glint": the share of the blades turned to MIRROR the sun (normal = the half vector, the specular's peak) - the
//   brightest stripes first, the share growing down the stripe values (without stripes all at 0: none up to 0.5, all
//   from 0.625);
// - "Ripple lean": the WIND RIPPLE (grassWind's noise, moving with the wind, stretched to -1..1, x the ripple's bend at
//   the mean wind speed, capped at 1) tilting the normal along the mean wind (before the glints), as the near blades
//   bend in it: their moving patches carry on (negative: the other way);
// - "Ripple glint": SINGLE BLADES (`fine`, grassStripeAt's finer noise, above a threshold) turned to mirror the sun
//   too, and `glint` (out, 0..1) for the FS to sharpen their roughness: a share of "Ripple glint" x (0.5 + 0.5 x the
//   ripple), so denser up the ripple and half of it with no wind.
vec3 grassFarNormal(vec3 pos, vec3 N, float stripe, float fine, out float glint)
{
    const vec3 V = normalize(u_viewPos - pos);
    const vec3 Vt = V - N * dot(N, V); // V's part along the ground, |Vt| = sin(the angle)
    vec3 n = N + Vt * u_grass_farUpright;
    if (u_grass_farNormalVariation > 0.0)
        n += cross(N, Vt) * (stripe * u_grass_farNormalVariation / max(length(Vt), 1e-3)); // sideways, unit length
    if (u_grass_farDiffuseVariation > 0.0)
    {
        const vec3 Lt = u_sunDirection.xyz - N * dot(N, u_sunDirection.xyz); // the sun's way along the ground
        n += Lt * (stripe * u_grass_farDiffuseVariation / max(length(Lt), 1e-3));
    }
    float wave = 0.0;
    if (u_grass_farRippleGlint > 0.0 || u_grass_farRippleLean != 0.0)
    {
        const float time = u_timeSeconds;
        const float ripple = grassValueNoise((pos.xz - u_weather_windDirection * (u_weather_windTravelSpeed * time)) * u_grass_invRippleSize);
        wave = (smoothstep(0.25, 0.75, ripple) * 2.0 - 1.0) * min(length(u_weather_windVelocity.xz) * u_grass_rippleBend, 1.0);
        n += vec3(u_weather_windDirection.x, 0.0, u_weather_windDirection.y) * (wave * u_grass_farRippleLean);
    }
    n = normalize(n);
    glint = 0.0;
    if (u_grass_farSunGlint > 0.0 || u_grass_farRippleGlint > 0.0)
    {
        const float edge = 1.0 - 2.0 * u_grass_farSunGlint;
        float mirror = smoothstep(edge, edge + 0.25, stripe);
        if (u_grass_farRippleGlint > 0.0)
        {
            const float fineEdge = 1.0 - 0.5 * u_grass_farRippleGlint * (0.5 + 0.5 * wave);
            glint = smoothstep(fineEdge, fineEdge + 0.04, fine);
            mirror = max(mirror, glint);
        }
        n = normalize(mix(n, normalize(u_sunDirection.xyz + V), mirror));
    }
    return n;
}

// THE BLADE LIGHT (the terrain FS: "Far/Blade light" of the far layer's sun, the ground model keeping the rest): the
// sun on UPRIGHT blades facing the viewer, per unit of sun radiance and albedo, before any visibility. A blade's face
// takes the sun at about cos(its elevation) - a low sun nearly face-on, where flat ground gets sin(elevation) - LIT in
// front with the sun behind the view and THROUGH ("Far/Transmission", the blades' term) with it ahead: c = the
// horizontal cosine between the way to the eye and the sun's, front = (1 + c) / 2, back = (1 - c) / 2 (the blades'
// azimuths spread, so a sun from the side lights about half of each).
float grassFarBladeSun(vec3 pos)
{
    const vec2 sunH = u_sunDirection.xz;
    const float cosElevation = length(sunH);
    const vec2 toEye = u_viewPos.xz - pos.xz;
    const float lenEye = length(toEye);
    if (cosElevation < 1e-3 || lenEye < 1e-3)
        return 0.0;
    const float c = dot(toEye, sunH) / (lenEye * cosElevation);
    return cosElevation * INV_PI * (0.5 + 0.5 * c + u_grass_farTransmission * (0.5 - 0.5 * c));
}
#endif
#endif

// The fraction of the blades kept at a distance: all of them inside "Thinning start", then (start / d)^exponent
// (the blade count per screen area stays about constant), faded to none over the last "Range fade" metres.
float grassKeep(float dist)
{
    const float start = u_grass_thinStart;
    const float f = dist > start ? pow(start / dist, u_grass_thinExponent) : 1.0;
    return f * (1.0 - smoothstep(u_grass_range - u_grass_rangeFade, u_grass_range, dist));
}

// A BASE terrain chunk's ground in the vertex mega-buffer: a res x res cell grid, row-major, local XZ (0 .. chunk size)
// with world Y, its rows u_present_groundStride vertices apart - a sub-grid of a bigger quadtree node's mesh
// (unstitched: the terrain VS snaps the edge vertices, these are the generated heights).
struct GrassGround
{
    vec2 chunkOrigin;
    uint firstVertex;
    uint res;
    float step; // m per cell
};

// The mesh height at xz and its facet normal. The cell's diagonal runs from (col + 1, row) to (col, row + 1)
// (TerrainGenerator.cpp: triangles a c b and b c d), so this is the exact surface the ground draws.
float grassGroundHeight(GrassGround g, vec2 xz, out vec3 normal)
{
    const float res = float(g.res);
    const vec2 local = clamp((xz - g.chunkOrigin) / g.step, vec2(0.0), vec2(res - 1e-3));
    const uvec2 cell = uvec2(local);
    const vec2 f = local - vec2(cell);
    const uint vpr = u_present_groundStride;
    const uint a = g.firstVertex + cell.y * vpr + cell.x;
    const float ha = in_vertices[a].positionU.y;
    const float hb = in_vertices[a + 1u].positionU.y;
    const float hc = in_vertices[a + vpr].positionU.y;
    float h, dx, dz;
    if (f.x + f.y <= 1.0)
    {
        dx = hb - ha;
        dz = hc - ha;
        h = ha + dx * f.x + dz * f.y;
    }
    else
    {
        const float hd = in_vertices[a + vpr + 1u].positionU.y;
        dx = hd - hc;
        dz = hd - hb;
        h = hd - dx * (1.0 - f.x) - dz * (1.0 - f.y);
    }
    normal = normalize(vec3(-dx, g.step, -dz));
    return h;
}

// The mesh's SMOOTH normal at xz: its vertex normals, interpolated over the same triangle as the height - what the
// terrain shading (its layer coverages) sees.
vec3 grassGroundSmoothNormal(GrassGround g, vec2 xz)
{
    const float res = float(g.res);
    const vec2 local = clamp((xz - g.chunkOrigin) / g.step, vec2(0.0), vec2(res - 1e-3));
    const uvec2 cell = uvec2(local);
    const vec2 f = local - vec2(cell);
    const uint vpr = u_present_groundStride;
    const uint a = g.firstVertex + cell.y * vpr + cell.x;
    const vec3 nb = in_vertices[a + 1u].normalV.xyz;
    const vec3 nc = in_vertices[a + vpr].normalV.xyz;
    vec3 n;
    if (f.x + f.y <= 1.0)
    {
        const vec3 na = in_vertices[a].normalV.xyz;
        n = na + (nb - na) * f.x + (nc - na) * f.y;
    }
    else
    {
        const vec3 nd = in_vertices[a + vpr + 1u].normalV.xyz;
        n = nd - (nd - nc) * (1.0 - f.x) - (nd - nb) * (1.0 - f.y);
    }
    return normalize(n);
}

// One ground vertex's river data (TerrainGenerator, the vertex's UV): the influence, the speed (m/s), 1 under river or
// lake water (v < 0: -(speed + 0.01)).
vec3 grassGroundRiverVertex(uint i)
{
    const vec2 uv = meshVertexUV(in_vertices[i]);
    return uv.y < 0.0 ? vec3(uv.x, -uv.y - 0.01, 1.0) : vec3(uv.x, uv.y, 0.0);
}

// The mesh's RIVER data at xz, over the same triangle as the height: x = the river influence (what the ground's beach
// layer sees, so the grass and the clutter leave the same river bed), y = the flow speed (m/s; 0 off rivers and on
// lakes), z = under the water 0..1 (a river channel or a lake, soft over a cell).
vec3 grassGroundRiverData(GrassGround g, vec2 xz)
{
    const float res = float(g.res);
    const vec2 local = clamp((xz - g.chunkOrigin) / g.step, vec2(0.0), vec2(res - 1e-3));
    const uvec2 cell = uvec2(local);
    const vec2 f = local - vec2(cell);
    const uint vpr = u_present_groundStride;
    const uint a = g.firstVertex + cell.y * vpr + cell.x;
    const vec3 rb = grassGroundRiverVertex(a + 1u);
    const vec3 rc = grassGroundRiverVertex(a + vpr);
    if (f.x + f.y <= 1.0)
    {
        const vec3 ra = grassGroundRiverVertex(a);
        return ra + (rb - ra) * f.x + (rc - ra) * f.y;
    }
    const vec3 rd = grassGroundRiverVertex(a + vpr + 1u);
    return rd - (rd - rc) * (1.0 - f.x) - (rd - rb) * (1.0 - f.y);
}

// The river influence alone (grassGroundRiverData's x).
float grassGroundRiver(GrassGround g, vec2 xz) { return grassGroundRiverData(g, xz).x; }

#endif
