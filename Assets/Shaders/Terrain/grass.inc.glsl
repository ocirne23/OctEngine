// PROCEDURAL GRASS, shared by the cell cull (grass_cull.cs.glsl) and the blade vertex shader (grass.vs.glsl).
// The world is a grid of BASE CELLS (u_grass_patchSize metres square), each with N blades in RANK order: blade k sits at
// the k-th point of an R2 low-discrepancy sequence, so EVERY PREFIX of the ranks is evenly spread. They are drawn in
// RING CELLS around the camera (2^level base cells a side, larger with the distance): the cull writes one indexed draw
// per ring cell (the first K ranks of all its base cells); the VS keeps rank k where (k + 0.5) / N < density x thinning
// and shrinks it into the ground over the grow band below that. One rule thins by density AND by distance, so a blade
// fades out by shrinking, never by popping.
// Blades stand on the terrain MESH: their roots are interpolated from the chunk's own vertices (two triangles per
// cell, as TerrainGenerator.cpp splits them) - the baked height map (4 m texels) is far too coarse.
// Needs shared.inc.glsl and the includer's `in_terrainVertices` (the vertex mega-buffer) first. GRASS_* come from Layout.ixx.
// GRASS_NOISE_SAMPLER: the terrain noise texture (its A channel is the value noise's lattice); default its slot of the
// includer's u_textures[] - the grass cull and the near-shadow casters (no bindless array) bind it alone.

#ifndef GRASS_INC_GLSL
#define GRASS_INC_GLSL

#ifndef GRASS_NOISE_SAMPLER
#define GRASS_NOISE_SAMPLER u_textures[u_terrain_noiseTex]
#endif

#include "mesh_vertex.inc.glsl" // TerrainVertex (the ground)

uint grassLodSegments(uint lod)
{
    return lod == 0u ? GRASS_LOD0_SEGMENTS : lod == 1u ? GRASS_LOD1_SEGMENTS : lod == 2u ? GRASS_LOD2_SEGMENTS : GRASS_LOD3_SEGMENTS;
}

// A blade of S segments: 2 vertices per row below the tip, one tip vertex; 2S - 1 triangles.
uint grassIndicesPerBlade(uint lod)
{
    return (2u * grassLodSegments(lod) - 1u) * 3u;
}

// THE NEAR CASTERS draw ONE LOD COARSER than the blades they shade (half the segments; the last LOD its own): a blade's
// shadow does not show its segment count. The cull sizes the casters' draws with it, the casters' shader builds with it.
uint grassCasterLod(uint lod)
{
    return min(lod + 1u, 3u);
}

// The facet normal packed by grass_blade.inc.glsl's grassPackFlat (its x / z snorm16, its y > 0 rebuilt).
vec3 grassUnpackGroundNormal(uint packed)
{
    const vec2 xz = unpackSnorm2x16(packed);
    return vec3(xz.x, sqrt(max(1.0 - dot(xz, xz), 0.0)), xz.y);
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

// Two values in [0, 1) from one hash: its 16-bit halves.
vec2 grassUnit2(uint h)
{
    return vec2(float(h & 0xFFFFu), float(h >> 16)) * (1.0 / 65536.0);
}

// Smooth value noise in [0, 1] (the type patches, dry spots, gusts): ONE bilinear tap of the lattice (the terrain
// noise texture's A: grassUnit(grassHash2(cell)) per texel, TERRAIN_NOISE_SIZE cells, repeating) at the smoothstepped
// fraction - the hardware blend is the value noise's own. The cell wraps in the shader: the sampler never sees large UVs.
float grassValueNoise(vec2 p)
{
    const float size = float(TERRAIN_NOISE_SIZE);
    const vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    const vec2 cell = i - size * floor(i * (1.0 / size));
    return textureLod(GRASS_NOISE_SAMPLER, (cell + f + 0.5) * (1.0 / size), 0.0).a;
}

// THE GRASS TYPES' PATCH PATTERN at xz: one weight per type (GRASS_TYPES = 4), each a corner of two value noises of
// "Type patch size" (stretched toward an even spread) - so one type rules an area and every two types can meet. The
// blades pick a tuft's type from it (grass_blade.inc.glsl grassType), the far tier its colour (grassFarColor).
vec4 grassTypeCorners(vec2 xz)
{
    vec2 n = vec2(grassValueNoise(xz * u_grass_invTypePatchSize + vec2(53.1, 17.7)),
                  grassValueNoise(xz * (u_grass_invTypePatchSize * 1.31) + vec2(-21.4, 88.2)));
    n = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0);
    return vec4(n.x * n.y, (1.0 - n.x) * n.y, n.x * (1.0 - n.y), (1.0 - n.x) * (1.0 - n.y));
}

// THE TYPES' SCORES at a point: the pattern's weights x the shares (u_grass_typeShares: the four in one value).
// "TYPE BLENDING" (0 .. 1): how the scores turn into each type's SHARE OF THE TUFTS at a point. 0: the highest score
// takes them all - a hard border where two scores cross. Above 0 the shares are the scores to the power 1 / blending:
// between two types the tufts of both stand mixed, a zone that widens with the tweak (at 1 the shares are the scores
// themselves - the whole way between two peaks is a blend). At a type's peak the other scores are 0: one type, always.
// The power comes from the CPU: u_grass_typeBlendPower for the blades' pick (0 = no blending), u_grass_typeFarPower for
// the far tier's weights.
vec4 grassTypeScores(vec2 xz)
{
    return grassTypeCorners(xz) * u_grass_typeShares;
}

// THE DRY PATCHES at xz [0, 1]: "Dry amount" of the ground with dry, straw-coloured grass, in patches about 2.7 x the
// grass types' ("Type patch size"). The blades and the far tier read the same noise, so the patches carry on past the blades.
float grassDryness(vec2 xz)
{
    return smoothstep(0.0, 0.3, grassValueNoise(xz * (u_grass_invTypePatchSize * 0.37) + 17.3) - (1.0 - u_grass_dryAmount));
}

// A SEED STALK PAST THE LODs THAT HOLD ITS HEAD: from "LOD 2 distance" a blade is 2 segments or one triangle - no row
// is left for the seed head's swell, so the stalk was a thin sliver whose seed-coloured top is its tip, under a pixel
// wide: the seeding patches lost their colour at mid distance. So over the last "LOD morph band" before that distance
// (0 -> 1, as the head's rows morph away) the stalk becomes a SPEAR that carries the head's area: GRASS_STALK_COARSE_WIDTH
// of a blade's width and twice the pixel floor (grass_blade.inc.glsl), seed-coloured over its top
// GRASS_STALK_COARSE_HEAD (grass.fs.glsl: a triangle's top 65 % of the length is 42 % of its area - the head's share).
const float GRASS_STALK_COARSE_WIDTH = 0.7;
const float GRASS_STALK_COARSE_HEAD = 0.65;

float grassStalkCoarse(float dist)
{
    return smoothstep(u_grass_lod2Distance * (1.0 - u_grass_lodMorphBand), u_grass_lod2Distance, dist);
}

// THE SEED STALKS AS A COLOUR over a type's blades, where the stalks themselves are too thin to show - the far tier
// (grassFarColor: u_grass_typeFarStalk, the CPU's colour) and the blades from LOD 2 to LOD 3 (a LOD 3 stalk is one
// triangle a pixel or two wide: grass_blade.inc.glsl puts the colour on ALL the type's blades there).
// Their cover of the view: the stalks stand ABOVE the blades, so the view meets them first - 1 - exp(-their optical
// depth), the depth = the type's u_grass_typeSeedFar (the CPU's fold: "Far/Seed heads" x stalks per m^2 x the area of a
// stalk that shows) x tan(the view's angle off the ground normal; NoV = its cosine): a grazing view sees a sheet of
// seed heads, a view from above the blades between them.
float grassStalkCover(float depth, float NoV)
{
    return 1.0 - exp(-depth * (sqrt(max(1.0 - NoV * NoV, 0.0)) / max(NoV, 0.05)));
}

float grassCoverSize(float cover)
{
    return mix(1.0, cover, u_grass_sizeByCover);
}

// The blade WIDTH factor by its height factor (the size by cover, as grassCanopyHeight): x that^-"Width by height" -
// short grass (sparse climate, tree cover) gets wider blades. Floored at 0.05, capped at 4.
float grassWidthSize(float coverSize)
{
    return min(pow(max(coverSize, 0.05), -u_grass_widthByHeight), 4.0);
}

// THE GRASS LAYER's height (the far tier's optical depths): the mean blade height x the grass types' share-weighted
// mean height (u_grass_meanHeightScale: one layer for every type - the ground under the blades knows no type).
float grassCanopyHeight(float coverSize)
{
    return u_grass_bladeHeight * u_grass_meanHeightScale * (1.0 - 0.5 * u_grass_heightVariation) * coverSize;
}

// The blades' "Range fade" (1 inside, 0 at the range).
float grassRangeFade(float dist)
{
    return 1.0 - smoothstep(u_grass_range - u_grass_rangeFade, u_grass_range, dist);
}

// THE ROOT OCCLUSION by the near grass cascade's weight at the point: "Look/Root occlusion" inside the cascade (the
// blades have real blade shadows there), "Shadows/Far root occlusion" outside it (no blade shadows: the occlusion
// alone stands in for the blades shading each other - so the callers put the far value on the direct sun too).
float grassRootOcclusion(float nearWeight)
{
    return mix(u_grass_farRootOcclusion, u_grass_rootOcclusion, nearWeight);
}

#ifdef INSTANCED_INDIRECT_LIT_INC_GLSL
// THE NEAR GRASS CASCADE (receivers: the blades, the ground under them): the real blade shadows from its own map
// (u_grassNearShadow; GrassPipeline's + ClutterPipeline's near pass), around the camera. `weight` = how
// much of it to use (fading to 0 over the box's outer part: past it the root occlusion alone stands in). The receiver moves toward
// the sun by "Near shadow bias" and off its normal by one texel: the blades are thin, two-sided, and self-casting.
float grassNearShadow(vec3 pos, vec3 N, out float weight)
{
    weight = 0.0;
    // Fades by the box's edge AND by the horizontal distance from the box's CENTRE (ahead of the camera; gone at 1.3 x
    // the range): the box is square in LIGHT space, so on the ground it reaches range / sin(sun elevation) - far past
    // the casters (drawn within range x 1.5 + 2 m only), where the empty map would read as lit.
    // The centre's fade first: most points are past it, and skip the light-space transform.
    const float range = u_grass_nearRange;
    const float centreDist = distance(pos.xz, u_grass_nearCentre);
    if (range <= 0.0 || centreDist >= 1.3 * range)
        return 1.0;

    const vec4 lp = u_grass_shadowViewProj * vec4(pos + u_sunDirection.xyz * u_grass_nearBias + N * u_grass_nearTexel, 1.0);
    const vec2 uv = lp.xy * 0.5 + 0.5;
    const vec2 edge = abs(uv - 0.5) * 2.0;
    // "Near shadow blend": the share of both reaches over which the weight falls (u_grass_nearBlendStart = 1 - it) - the
    // band where the blade shadows fade into "Far root occlusion".
    weight = (1.0 - smoothstep(0.98 * u_grass_nearBlendStart, 0.98, max(edge.x, edge.y)))
        * (1.0 - smoothstep(1.3 * range * u_grass_nearBlendStart, 1.3 * range, centreDist));
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
// THE TERRAIN TEXTURES' grass: what the beach and rock layers leave of the GROUND, and the snow BURIES (by its depth:
// TerrainLayers::snowBury, not its cover - grass pokes through thin snow) x the grass amount of its climate-picked
// textures (u_terrain_splatGrass, TerrainSplatMaterial::grass).
float grassTextureAmount(uint slot)
{
    return u_terrain_splatGrass[slot >> 2u][slot & 3u];
}

float grassTerrainCover(TerrainLayers L)
{
    const float ground = (1.0 - float(L.beachW)) * (1.0 - float(L.rockW)) * (1.0 - float(L.snowBury));
    if (ground <= 0.0)
        return 0.0;
    const float n1 = float(L.g.n1), n2 = float(L.g.n2);
    return ground * ((1.0 - n1 - n2) * grassTextureAmount(climatePickIdx(L.g, 0))
        + n1 * grassTextureAmount(climatePickIdx(L.g, 1)) + n2 * grassTextureAmount(climatePickIdx(L.g, 2)));
}

// THE GRASS AT A RAY HIT on the terrain (the water's reflection rays; N = the hit's normal, `dir` = the ray, unit): no
// blade is in the TLAS and the hit's splat is the soil under them, so the hit takes THE FAR TIER's colour by the far
// tier's view cover, with the ray for the view (below: grassGroundCanopy, grassFarAlbedo). No blade cover to leave out,
// and no stripes or type patches (the types' mean tint): a ray has no anchor and no pixel footprint. rgb = the colour,
// a = its weight; 0 without grass.
vec4 grassRayAlbedo(TerrainLayers L, vec3 pos, vec3 N, vec3 dir, float temperature)
{
    if (u_grass_farExtinction <= 0.0)
        return vec4(0.0);
    const float cover = grassTerrainCover(L);
    if (cover <= 0.0)
        return vec4(0.0);
    const float coverSize = grassCoverSize(cover);
    const float height = max(grassCanopyHeight(coverSize) - float(L.snowDepth), 0.0); // the part over the snow
    const float tau = u_grass_farExtinction * cover * grassWidthSize(coverSize) * height / max(abs(dot(N, dir)), 0.05);
    const float warm = smoothstep(u_grass_coldTemperature, u_grass_warmTemperature, temperature);
    const vec3 green = mix(u_grass_tipAlbedo * u_grass_meanTint, u_grass_dryAlbedo, grassDryness(pos.xz)) * (mix(1.0 - u_grass_coldDarkening, 1.0, warm) * u_grass_farBrightness);
    // The snow on it as on the blades (grass.fs.glsl): the ground's white x "Grass snow".
    return vec4(mix(green, u_snow_freshAlbedo, float(L.snowW) * u_snow_grassSnow), 1.0 - exp(-tau));
}

#ifdef INSTANCED_INDIRECT_LIT_INC_GLSL // it reads the near grass cascade (the lit core's u_shadowMap)
// The ground UNDER the grass at pos (the terrain FS; N = its smooth normal): x = its sun - the near cascade's blade
// shadows inside its box (on ANY ground there: blades also shade the bare soil beside them), none outside it; y = its
// ambient (the blades' root occlusion - grassRootOcclusion - by the grass cover there); z = THE FAR TIER's
// weight (grassFarAlbedo). x and y blend by z into the far tier's own light (the blades' mean canopy sun and root
// occlusion), so the far grass is as dark as the blades it continues; w = the far tier's STRIPE scale, by how dense the
// grass is (the cover, 0.05 .. 0.6): a grazing view saturates the weight over a few tufts too, but sparse
// grass has no blade-on-blade pattern. (1, 1, 0, 0) without grass.
// THE FAR TIER: where the blades thin and fade out, the ground itself takes the grass layer's look. Its weight = the
// share of the blades' cover the LOD took away x how much of the view the layer covers.
// - The cover the blades keep: grass.vs.glsl's kept fraction k (grassKeep: (start / d)^exponent x the range fade), each
//   kept blade wider by (1 / k)^"Width compensation" (capped by "Max width scale"): k^(1 - compensation), at most
//   k x the cap. The far tier fills the rest, so the soil between the thinned blades does not show as a band.
// - The view's cover: 1 - exp(-tau), tau = u_grass_farExtinction (blades per m^2 x the mean width x "Far/Thickness") x
//   the cover x the size by cover x the height / N.V - a grazing view sees grass where a view from above still sees
//   soil between the blades.
vec4 grassGroundCanopy(TerrainLayers L, vec3 pos, vec3 N)
{
    if (u_grass_active <= 0.0)
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

    const float coverSize = grassCoverSize(cover);
    const float widthSize = grassWidthSize(coverSize);
    const float height = max(grassCanopyHeight(coverSize) - float(L.snowDepth), 0.0); // the canopy over the snow (the blades stand in it)
    const float rangeFade = grassRangeFade(dist);
    const float kept = (dist > u_grass_thinStart ? pow(u_grass_thinStart / dist, u_grass_thinExponent) : 1.0) * rangeFade;
    const float bladeCover = kept > 0.0 ? min(pow(kept, 1.0 - u_grass_widthCompensation), kept * u_grass_maxWidthScale) : 0.0;
    // 1 / N.V without the normalize: dist / dot(N, toEye), N.V floored at 0.05. tauV = the view's optical depth through
    // the layer, kv = the same per metre of the layer's depth.
    const float tauV = u_grass_farExtinction * cover * widthSize * height * dist / max(dot(N, toEye), 0.05 * dist);
    const float viewCover = 1.0 - exp(-tauV);
    const float far = (1.0 - min(bladeCover, 1.0)) * viewCover;
    // The far tier's LIGHT, as the blades get it, AT THE DEPTH THE VIEW REACHES: a view from above looks deep into the
    // layer (the shaded roots), a grazing one sees the lit tops. Along the view the layer's visible share at depth d is
    // kv e^(-kv d) / (1 - e^(-tauV)) over [0, h]:
    // - the sun = the canopy's exp(-ks d) under it (ks = the far canopy extinction - "Far/Canopy shadow" - x the cover x
    //   the size by cover / the sun's height): kv / (ks + kv) x (1 - e^(-(ks + kv) h)) / viewCover;
    // - the ambient: no term of its own - the dark stripes stand in for the view down between the blades (grassFarAlbedo).
    const float sh = u_grass_farSunExtinction * cover * widthSize * height * u_grass_invSunY; // ks x h
    const float farSun = viewCover > 1e-4 ? tauV / (sh + tauV) * (1.0 - exp(-(sh + tauV))) / viewCover : (sh > 1e-3 ? (1.0 - exp(-sh)) / sh : 1.0);
    const float stripeScale = smoothstep(0.05, 0.6, cover);
    if (!inRange)
        return vec4(mix(1.0, farSun, far), 1.0, far, stripeScale);
    const float presence = cover * rangeFade * (1.0 - float(L.snowW)); // the blades' roots HERE (the ambient): none on a snow surface
    // Outside the near cascade "Far root occlusion" is on the ground's sun too, as on the blades' roots (grass.fs.glsl).
    return vec4(mix(mix(max(1.0 - u_grass_farRootSun * presence, 0.0), nearSun, nearWeight), farSun, far),
        mix(max(1.0 - grassRootOcclusion(nearWeight) * presence, 0.0), 1.0, far), far, stripeScale);
}

// THE FAR TIER's GRASS COLOUR, per GRASS TYPE in the blades' own patches (grassTypeCorners: the same pattern, so a
// type's area keeps its look where its blades thin out and past them):
// - THE TYPES' WEIGHTS: the share of the tufts each type has there, as the blades draw them (grassType): the scores
//   to the power 1 / "Type blending". Without blending (the highest score takes every tuft) and for a blending below
//   1 / 6 the power is the 6th: smooth, and close to the hard pick (the 4th mixed the types too far into each other's
//   areas; the 8th drew the far patches with hard outlines). Where a pixel covers a patch or more the weights fade to the
//   types' shares (the mean): from half a cell of the finer noise per pixel to a whole one, the pixel's ground
//   footprint taken along the view at a view angle of 0.25 at the least (a grazing view's true length would lose the
//   patches a few hundred metres out; they are streaks along the view there).
// - A type's colour: the blades' TIP colour x its tint x "Far/Brightness" (the view into the blades meets darker
//   parts than their tips), and over it its SEED STALKS (the seed heads' colour + the stalks' straw, at FULL
//   brightness: they are the layer's top): they stand ABOVE the blades, so the view meets them first - their cover = 1 - exp(-their optical depth), the depth
//   = u_grass_typeSeedFar (the CPU's fold per type: "Far/Seed heads" x stalks per m^2 x a head's area) x tan(the view's
//   angle off the ground normal): a grazing view sees a sheet of heads, a view from above the blades between them.
vec3 grassFarColor(vec3 pos, vec3 N)
{
    const vec3 toEye = u_viewPos - pos;
    const float dist = length(toEye);
    const float NoV = clamp(dot(N, toEye) / dist, 0.0, 1.0);
    const float footprint = 2.0 * dist / (u_mipPixelScale * max(NoV, 0.25)); // m (inf with no viewport yet: the mean)
    const float local = 1.0 - smoothstep(0.5, 1.0, footprint * u_grass_invTypePatchSize * 1.31);
    vec4 weights = u_grass_typeShares; // sum 1
    if (local > 0.0)
    {
        vec4 w = grassTypeScores(pos.xz);
        const float top = max(max(w.x, w.y), max(w.z, w.w));
        if (top > 1e-6)
        {
            // The largest = 1: the power cannot underflow. The blades' power with "Type blending" (their exact shares),
            // at most the 6th - the stand-in for the hard pick (u_grass_typeFarPower).
            w = pow(w * (1.0 / top), vec4(u_grass_typeFarPower));
            weights = mix(weights, w * (1.0 / (w.x + w.y + w.z + w.w)), local);
        }
    }
    // Each type's blades and its stalks, weighted (the colours are the CPU's: u_grass_typeFarBlade / u_grass_typeFarStalk).
    const vec4 heads = weights * (1.0 - exp(-u_grass_typeSeedFar * (sqrt(1.0 - NoV * NoV) / max(NoV, 0.05))));
    const vec4 blades = weights - heads;
    return blades.x * u_grass_typeFarBlade[0].rgb + blades.y * u_grass_typeFarBlade[1].rgb
        + blades.z * u_grass_typeFarBlade[2].rgb + blades.w * u_grass_typeFarBlade[3].rgb
        + heads.x * u_grass_typeFarStalk[0].rgb + heads.y * u_grass_typeFarStalk[1].rgb
        + heads.z * u_grass_typeFarStalk[2].rgb + heads.w * u_grass_typeFarStalk[3].rgb;
}

// THE FAR TIER's colour: the blades' TIP colour (a far view sees their upper part; the darker depth is "Far/Canopy
// shadow"'s, on the sun) x the grass types' tint there, under their seed heads (grassFarColor), with the blades' dry
// patches (grassDryness) and the cold darkening (the temperature at the ground), x "Far/Brightness". The colours are
// lockable: they fold when locked.
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
//   hills too. The coordinate is log(r) x 2^j (a cell r / 2^j long), j = log2(r / that length), two WHOLE levels
//   crossfaded as the counts are: the length only sets the weights, so the pattern itself is a function of r alone. A
//   coordinate scaled by the length directly (log(r) x r / length) moved by hundreds of cells when the normal's angle
//   to a grazing view changed a little (a gentle slope): blotches of squeezed, bent stripes.
// - Each COLUMN (one stripe across) has its own along offset and stretch (0.75 .. 1.25), so the stripes start and end at
//   different distances - a plain lattice ends a whole row of them at one r: blocks.
float grassStripeColumn(int x, float y, int period)
{
    const uint column = grassHash(uint(x) * 0x9E3779B9u ^ uint(period) * 0x85EBCA6Bu);
    const vec2 o = grassUnit2(column);
    y = y * (0.75 + 0.5 * o.x) + o.y;
    const float i = floor(y);
    float f = y - i;
    f = f * f * (3.0 - 2.0 * f);
    const uint row = uint(int(i)) * 0xC2B2AE35u;
    return mix(grassUnit(grassHash(column ^ row)), grassUnit(grassHash(column ^ (row + 0xC2B2AE35u))), f);
}

float grassStripeNoise(vec2 p, int period)
{
    const float i = floor(p.x);
    float f = p.x - i;
    f = f * f * (3.0 - 2.0 * f);
    const int x0 = int(uint(int(i) + 2 * period) % uint(period)); // never % of a negative (undefined)
    const int x1 = x0 + 1 == period ? 0 : x0 + 1;
    return mix(grassStripeColumn(x0, p.y, period), grassStripeColumn(x1, p.y, period), f);
}

// The four levels (two counts across x two along) of grassStripeNoise, weights w, the variance re-normalized. Each level's
// offset follows its ABSOLUTE indices (k0, j0), never its slot.
float grassStripeLevels(float x0, int count0, float along0, float k0, float j0, vec4 w, float scale, float offset)
{
    const float a0 = along0 * scale + 29.0 * j0 + offset, a1 = 2.0 * along0 * scale + 29.0 * (j0 + 1.0) + offset;
    const float n = w.x * grassStripeNoise(vec2(x0, a0 + 17.0 * k0), count0)
        + w.y * grassStripeNoise(vec2(2.0 * x0, a0 + 17.0 * (k0 + 1.0)), 2 * count0)
        + w.z * grassStripeNoise(vec2(x0, a1 + 17.0 * k0), count0)
        + w.w * grassStripeNoise(vec2(2.0 * x0, a1 + 17.0 * (k0 + 1.0)), 2 * count0);
    return (n - 0.5) * inversesqrt(dot(w, w)) + 0.5;
}

// The stripe value [-1, 1] at pos (N: its smooth ground normal) seen from `anchor`; `width` (m). `fine` (out, 0..1):
// the same noise 4x finer across and 3x along (the single blades of "Far/Ripple glint"; 0.5 without it).
float grassStripeAt(vec3 pos, vec3 N, vec3 anchor, float width, out float fine)
{
    // Along: the hidden ground's length - from the ANCHOR's view angle - at least the width; two whole levels j0, j0 + 1.
    const vec3 fromAnchor3 = pos - anchor;
    const float NoV = clamp(-dot(N, fromAnchor3) / length(fromAnchor3), 0.02, 1.0);
    const float stripeLength = max(u_grass_farStripeLength * sqrt(1.0 - NoV * NoV) / NoV, width);
    const vec2 fromAnchor = fromAnchor3.xz;
    const float r = max(length(fromAnchor), 1.0);
    const float j = log2(r / stripeLength);
    const float j0 = floor(j);
    const float along0 = log(r) * exp2(j0);
    const float s = j - j0;
    // Across: the azimuth (0..1) x two power-of-two stripe counts around the circle.
    const float azimuth = atan(fromAnchor.y, fromAnchor.x) * (1.0 / 6.28318531) + 0.5;
    const float k = log2(max(6.28318531 * r / (8.0 * width), 1.0));
    const float k0 = floor(k);
    const int count0 = 8 << int(k0);
    const float t = k - k0;
    const vec4 w = vec4((1.0 - t) * (1.0 - s), t * (1.0 - s), (1.0 - t) * s, t * s);
    fine = 0.5;
    if (u_grass_farRippleGlint > 0.0)
        fine = grassStripeLevels(azimuth * float(4 * count0), 4 * count0, along0, k0, j0, w, 3.0, 53.0);
    const float n = grassStripeLevels(azimuth * float(count0), count0, along0, k0, j0, w, 1.0, 0.0);
    return smoothstep(0.2, 0.8, n) * 2.0 - 1.0;
}

// `stripe` (out): the blade's stripe value in [-1, 1] (0 without stripes) - its lean in grassFarNormal; `fine` (out):
// grassStripeAt's fine noise (0.5 without stripes).
vec3 grassFarAlbedo(vec3 pos, vec3 N, float temperature, float stripeScale, out float stripe, out float fine)
{
    const float dryness = grassDryness(pos.xz);
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
    // "Far/Brightness": on the blades (in grassFarColor: not on the seed heads) and on the dry grass.
    return mix(grassFarColor(pos, N), u_grass_dryAlbedo * u_grass_farBrightness, dryness)
        * (mix(1.0 - u_grass_coldDarkening, 1.0, warm) * (1.0 + u_grass_farStripes * stripe));
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
// azimuths spread, so a sun from the side lights about half of each). The FOCUS lobe ("Far/Transmission glow" x
// (-V.L)^"Far/Transmission focus", as the leaves' glow) adds the forward scatter: a glow under the sun that follows it -
// the broad back term alone left the grass below the sun barely brighter than beside it.
// THE STRIPES' LEANS (grassFarNormal's, `stripe` -1..1) turn the faces to or from the sun, so the whole blade light (front,
// through and the glow) x 1 + stripe x ("Normal variation" x the sun's sideways cosine across the view + "Diffuse
// variation"): the change of the faces' sun cosine relative to cos(elevation). A sideways lean moves it only with the sun
// to the side; the lean toward the sun always.
float grassFarBladeSun(vec3 pos, float stripe)
{
    const vec2 sunH = u_sunDirection.xz;
    const float cosElevation = length(sunH);
    const vec3 toEye = u_viewPos - pos;
    const float lenEyeH = length(toEye.xz);
    if (cosElevation < 1e-3 || lenEyeH < 1e-3)
        return 0.0;
    const vec2 sunDirH = sunH / cosElevation, eyeDirH = toEye.xz / lenEyeH;
    const float c = dot(eyeDirH, sunDirH);
    const float side = dot(vec2(eyeDirH.y, -eyeDirH.x), sunDirH); // grassFarNormal's cross(N, Vt) on flat ground
    const float lean = max(1.0 + stripe * (u_grass_farNormalVariation * side + u_grass_farDiffuseVariation), 0.0);
    const float glow = u_grass_farTransmissionGlow * pow(max(-dot(toEye, u_sunDirection.xyz) / length(toEye), 0.0), u_grass_farTransmissionFocus);
    return cosElevation * INV_PI * lean * (0.5 + 0.5 * c + u_grass_farTransmission * (0.5 - 0.5 * c) + glow);
}
#endif
#endif

// GRASS_NO_GROUND: an includer that only shades (the water's ray hits) has no ground table and no terrain vertices.
#ifndef GRASS_NO_GROUND
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
    const float ha = in_terrainVertices[a].y;
    const float hb = in_terrainVertices[a + 1u].y;
    const float hc = in_terrainVertices[a + vpr].y;
    float h, dx, dz;
    if (f.x + f.y <= 1.0)
    {
        dx = hb - ha;
        dz = hc - ha;
        h = ha + dx * f.x + dz * f.y;
    }
    else
    {
        const float hd = in_terrainVertices[a + vpr + 1u].y;
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
    const vec3 nb = terrainVertexNormal(in_terrainVertices[a + 1u].normalLod);
    const vec3 nc = terrainVertexNormal(in_terrainVertices[a + vpr].normalLod);
    vec3 n;
    if (f.x + f.y <= 1.0)
    {
        const vec3 na = terrainVertexNormal(in_terrainVertices[a].normalLod);
        n = na + (nb - na) * f.x + (nc - na) * f.y;
    }
    else
    {
        const vec3 nd = terrainVertexNormal(in_terrainVertices[a + vpr + 1u].normalLod);
        n = nd - (nd - nc) * (1.0 - f.x) - (nd - nb) * (1.0 - f.y);
    }
    return normalize(n);
}

// One ground vertex's river data (TerrainGenerator): the influence, the speed (m/s), 1 under river or lake water.
vec3 grassGroundRiverVertex(uint i)
{
    const vec4 data = unpackUnorm4x8(in_terrainVertices[i].data);
    return vec3(data.x, data.y * TERRAIN_VERTEX_SPEED_MAX, terrainVertexUnderWater(in_terrainVertices[i].normalLod));
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
#endif // !GRASS_NO_GROUND

#endif
