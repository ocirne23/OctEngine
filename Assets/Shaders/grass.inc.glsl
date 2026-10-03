// PROCEDURAL GRASS, shared by the patch cull (grass_cull.cs.glsl) and the blade vertex shader (grass.vs.glsl).
//
// The world is a grid of PATCHES (u_grassParams0.y metres square). A patch is drawn from ONE static index buffer of N
// blades (u_grassParams0.x) in RANK order: blade k sits at the k-th point of an R2 low-discrepancy sequence over the
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

// The fraction of the blades kept at a distance: all of them inside "Thinning start", then (start / d)^exponent
// (the blade count per screen area stays about constant), faded to none over the last "Range fade" metres.
float grassKeep(float dist)
{
    const float start = u_grassParams2.x;
    const float f = dist > start ? pow(start / dist, u_grassParams2.y) : 1.0;
    return f * (1.0 - smoothstep(u_grassParams0.z - u_grassParams0.w, u_grassParams0.z, dist));
}

// A terrain chunk's mesh in the vertex mega-buffer: a (res + 1)^2 vertex grid, row-major, local XZ (0 .. chunk size)
// with world Y; the skirt follows the grid and is never read.
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
