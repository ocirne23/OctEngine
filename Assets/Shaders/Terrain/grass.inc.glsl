// PROCEDURAL GRASS, shared by the patch cull (grass_cull.cs.glsl) and the blade vertex shader (grass.vs.glsl).
//
// The world is a grid of PATCHES (u_grass_patchSize metres square). A patch is drawn from ONE static index buffer of N
// blades (u_grass_bladesPerPatch) in RANK order: blade k sits at the k-th point of an R2 low-discrepancy sequence over the
// patch, so EVERY PREFIX of the ranks is evenly spread. The cull writes one indexed draw per visible patch and draws
// the first K blades; the vertex shader keeps blade k where rank (k + 0.5) / N < density x thinning, and shrinks it
// into the ground over the grow band below that. One rule thins by density AND by distance, so the two always agree
// and a blade fades out by shrinking, never by popping.
//
// Blades stand on the terrain MESH: their roots are interpolated from the chunk's own vertices in the vertex
// mega-buffer (the chunk grid's two triangles per cell, as TerrainGenerator.cpp splits them), so they sit exactly on
// the drawn ground - the baked height map (8 m texels) is far too coarse for that.
//
// Needs shared.inc.glsl (the UBO) and the includer's `in_vertices` (MeshVertex, the vertex mega-buffer) first.
// The constants (GRASS_*) are injected from RendererVKLayout (Layout.ixx).

#ifndef GRASS_INC_GLSL
#define GRASS_INC_GLSL

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

// THE CANOPY: grass self-shadowing without a shadow map (a blade is far below a shadow-map texel). The grass layer is
// a thin volume of blades: its top at the mean blade height, its extinction (1/m) = u_grass_canopyExtinction ("Canopy
// shadow" x blades per m^2 x the mean blade width, the CPU's fold) x the COVER (not the clumps: those are sampled along
// the sun path, grassCanopySun) x the size by cover; its RESULT fades with the blades over the range's end
// (grassRangeFade). The blades
// (grass.vs/fs.glsl) and the ground under them (instanced_indirect_terrain.fs.glsl) use the same terms, so the soil
// between the blades matches their feet.
float grassCanopyHeight(float clump, float coverSize)
{
    return u_grass_bladeHeight * (1.0 - 0.5 * u_grass_heightVariation) * mix(0.6, 1.0, clump) * coverSize;
}
float grassCanopyExtinction(float cover, float coverSize, float dist)
{
    return u_grass_canopyExtinction * cover * coverSize;
}
// The blades' "Range fade" (1 inside, 0 at the range). The canopy's RESULT fades with it, linearly: fading the
// extinction instead kept exp(-extinction x path) nearly black until the band's last metres - an edge, not a fade.
float grassRangeFade(float dist)
{
    return 1.0 - smoothstep(u_grass_range - u_grass_rangeFade, u_grass_range, dist);
}

// The sun reaching pos, `depth` metres below the canopy top, along the sun's path up through it (capped at 10x the
// depth: a sun near the horizon):
// - the CLUMPS at 4 points along that path (not at pos): a clump shadows the bare ground beside it, stretched away
//   from the sun;
// - SUN FLECKS: what gets through arrives in bright and dark spots, not as an even gray - a noise at the path's ENTRY
//   into the canopy decides each spot, about T of the ground lit (the mean keeps the transmittance T), stretched into
//   streaks along the sun (below). Every point of one sun path shares its spot, so a fleck lights a blade and the soil
//   under it alike. "Fleck size" (u_grass_invFleckSize
//   = 1 / size), "Fleck contrast" (.fleckContrast), faded out from half the "Fleck fade distance" (.fleckFadeDistance) to it: smaller than a pixel
//   it only shimmers.
float grassCanopySun(vec3 pos, float depth, float extinction, float dist)
{
    if (extinction <= 0.0 || depth <= 0.0)
        return 1.0;
    const vec3 L = u_sunDirection.xyz;
    const float sunY = max(L.y, 0.1);
    const vec2 toSun = L.xz / sunY; // XZ per metre of height along the sun
    float clumps = 0.0;
    for (int i = 0; i < 4; ++i)
        clumps += grassClump(pos.xz + toSun * (depth * (float(i) + 0.5) * 0.25));
    const float T = exp(-extinction * (0.25 * clumps) * depth / sunY);
    const float rangeFade = grassRangeFade(dist);
    const float contrast = u_grass_fleckContrast * (1.0 - smoothstep(0.5 * u_grass_fleckFadeDistance, u_grass_fleckFadeDistance, dist));
    if (contrast <= 0.0)
        return mix(1.0, T, rangeFade);
    // STREAKS, not round spots: the shadows of upright blades fall as lines away from the sun, longer as it sinks
    // (a blade of height h casts h / tan(elevation)). The noise is stretched along the sun's horizontal direction by
    // that ratio x "Fleck stretch" (u_grass_fleckStretch; capped at 16); across it the spots stay "Fleck size".
    const vec2 entryPos = pos.xz + toSun * depth;
    const float horizontal = length(L.xz);
    const vec2 along = horizontal > 1e-4 ? L.xz / horizontal : vec2(1.0, 0.0);
    const float stretch = clamp(1.0 + u_grass_fleckStretch * horizontal / sunY, 1.0, 16.0);
    const vec2 entry = vec2(dot(entryPos, along) / stretch, dot(entryPos, vec2(-along.y, along.x))) * u_grass_invFleckSize;
    float n = 0.65 * grassValueNoise(entry) + 0.35 * grassValueNoise(entry * 2.7 + 5.1);
    n = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0); // value noise bunches at 0.5: stretched toward an even spread
    return mix(1.0, mix(T, smoothstep(n - 0.1, n + 0.1, T), contrast), rangeFade);
}

#ifdef INSTANCED_INDIRECT_LIT_INC_GLSL
// THE NEAR GRASS CASCADE (the receivers: the blades, the ground under them): the real blade shadows from the extra
// layer of the sun shadow array (u_shadowMap layer NUM_SHADOW_CASCADES; GrassPipeline's near pass), around the camera.
// Returns the visibility; `weight` = how much of it to use (1 inside, fading to 0 over the box's outer part, where the
// canopy takes over). The receiver moves toward the sun by "Near shadow bias" and off its normal by one texel: the
// blades are thin and two-sided, and they receive their own casters.
float grassNearShadow(vec3 pos, vec3 N, out float weight)
{
    weight = 0.0;
    if (u_grass_nearRange <= 0.0)
        return 1.0;
    const vec4 lp = u_grass_shadowViewProj * vec4(pos + u_sunDirection.xyz * u_grass_nearBias + N * u_grass_nearTexel, 1.0);
    const vec2 uv = lp.xy * 0.5 + 0.5;
    const vec2 edge = abs(uv - 0.5) * 2.0;
    // Fades by the box's edge AND by the horizontal distance from the box's CENTRE (u_grass_nearCentre, ahead of the
    // camera: full within the range, gone at 1.3 x): the box is square in LIGHT space, so on the ground it reaches
    // range / sin(sun elevation) along the sun - far past the casters, which are drawn within range x 1.5 + 2 m of the
    // centre only. There the map is empty (lit) and replaced the canopy: a bright gap between the two.
    const float range = u_grass_nearRange;
    weight = (1.0 - smoothstep(0.85, 0.98, max(edge.x, edge.y)))
           * (1.0 - smoothstep(range, 1.3 * range, distance(pos.xz, u_grass_nearCentre)));
    if (weight <= 0.0 || lp.z >= 1.0)
    {
        weight = 0.0;
        return 1.0;
    }
    // 4 bilinear PCF taps one texel apart (a 3x3-texel footprint): a blade is a few texels wide.
    const float layer = float(NUM_SHADOW_CASCADES);
    const float texel = 1.0 / float(textureSize(u_shadowMap, 0).x);
    float sum = 0.0;
    sum += texture(u_shadowMap, vec4(uv + vec2(-0.5, -0.5) * texel, layer, lp.z));
    sum += texture(u_shadowMap, vec4(uv + vec2( 0.5, -0.5) * texel, layer, lp.z));
    sum += texture(u_shadowMap, vec4(uv + vec2(-0.5,  0.5) * texel, layer, lp.z));
    sum += texture(u_shadowMap, vec4(uv + vec2( 0.5,  0.5) * texel, layer, lp.z));
    return mix(1.0, 0.25 * sum, u_grass_nearStrength); // "Near shadow strength"
}
#endif

#ifdef TERRAIN_SPLAT_INC_GLSL
// THE TERRAIN TEXTURES' grass (terrain_splat.inc.glsl terrainLayers): what the beach, rock and snow layers leave of
// the GROUND x the grass amount of its climate-picked textures (u_terrain_splatGrass, TerrainSplatMaterial::grass).
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
// The ground UNDER the grass at pos (the terrain FS; N = its smooth normal): x = its sun - the near grass cascade's
// blade shadows inside its box (on ANY ground there: blades also shade the bare soil beside them), blending into the
// canopy's (the full canopy depth) outside it -, y = its ambient (the blades' root occlusion, by how opaque the canopy
// is). (1, 1) without grass there.
vec2 grassGroundCanopy(TerrainLayers L, vec3 pos, vec3 N)
{
    if (u_grass_canopyExtinction <= 0.0 && u_grass_nearRange <= 0.0) // no canopy, no near cascade (or no grass at all)
        return vec2(1.0);
    const float dist = distance(pos, u_viewPos);
    if (dist >= u_grass_range)
        return vec2(1.0);
    float nearWeight;
    const float nearSun = grassNearShadow(pos, N, nearWeight);
    const float cover = grassTerrainCover(L);
    if (cover <= 0.0)
        return vec2(mix(1.0, nearSun, nearWeight), 1.0);
    const float clump = grassClump(pos.xz);
    const float coverSize = grassCoverSize(cover);
    const float height = grassCanopyHeight(clump, coverSize);
    const float extinction = grassCanopyExtinction(cover, coverSize, dist);
    const float presence = (1.0 - exp(-extinction * clump * height)) * grassRangeFade(dist); // the blades standing HERE (the ambient)
    const float canopySun = nearWeight < 1.0 ? grassCanopySun(pos, height, extinction, dist) : 1.0;
    return vec2(mix(canopySun, nearSun, nearWeight), mix(1.0, 1.0 - u_grass_rootOcclusion, presence));
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

// A terrain chunk's mesh in the vertex mega-buffer: a (res + 1)^2 vertex grid, row-major, local XZ (0 .. chunk size)
// with world Y (unstitched: the terrain VS snaps the edge vertices, these are the generated heights).
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
    const uint vpr = g.res + 1u;
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
    const uint vpr = g.res + 1u;
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

#endif
