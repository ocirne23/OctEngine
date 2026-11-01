// THE TERRAIN RAY MARCH. The terrain is not in the TLAS: its nodes' vertices are a height field, so a ray walks them here and
// the TLAS trace stops at the march's hit (tMax). The same triangles the terrain draws, without the edge stitch and the
// tessellation, like a BLAS over them.
//
// One flat loop, three levels, each step taken at the coarsest level that can rule the ray out from t to where it leaves:
//   a BASE CHUNK (the per-frame table, RendererState::uploadTerrainRayFrame: the finest drawn node over it, with the node's height range),
//   a BRICK (TERRAIN_BRICK_SHIFT: its min and max height, stored after the node's vertices),
//   a CELL (its two triangles: the diagonal runs from (col + 1, row) to (col, row + 1), TerrainGenerator.cpp).
// Each step holds nothing from the last but the ray's side of the surface, so a step across a boundary loses no crossing: a
// side change between two steps is a hit too. Past the table (around the scene focus, "RT/TLAS Range") and past
// TERRAIN_RAY_MAX_STEPS the ray misses.
//
// The includer has the frame UBO, or defines TERRAIN_RAY_TABLE as its own address (a pass without the UBO). The preamble enables
// GL_EXT_buffer_reference (Shader.cpp).

#ifndef TERRAIN_RAY_INC_GLSL
#define TERRAIN_RAY_INC_GLSL

#define TERRAIN_RAY_MAX_STEPS 256u
#define TERRAIN_RAY_ALBEDO 1.0 // a terrain hit's albedo where the splat is out of reach: the terrain material's white fallback texture

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainRayFloats { float f[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainRayWords { uint w[]; };

struct TerrainRayEntry // RendererVKLayout::TerrainRayEntry
{
    uint firstVertex; // TERRAIN_RAY_NO_NODE = no node
    uint lod;
    float minHeight;
    float maxHeight;
};

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainRayTable // RendererVKLayout::TerrainRayFrameGpu
{
    uvec2 vertices;
    ivec2 tableMin;
    uint tableDim;
    float chunkSize;
    float lod0Quad;
    uint lod0Res;
    float minHeight;
    float maxHeight;
    uvec2 _pad;
    TerrainRayEntry entries[];
};

#ifndef TERRAIN_RAY_TABLE
#define TERRAIN_RAY_TABLE TerrainRayTable(u_present_terrainRay.xy)
#endif

// The t at which the ray leaves the XZ box [lo, lo + size]. invD: 1 / d.xz.
float terrainRayExit(vec2 o, vec2 invD, vec2 lo, float size)
{
    const vec2 s = max((lo - o) * invD, (lo + size - o) * invD);
    return min(s.x, s.y);
}

// The ray's height over the surface at t, in a cell whose corners are a (min x, min z), b (+x), c (+z), d (+x +z).
float terrainRayGap(vec3 o, vec3 d, float t, vec2 cellLo, float quad, vec4 h)
{
    const vec3 p = o + d * t;
    const vec2 f = (p.xz - cellLo) / quad;
    const float surface = f.x + f.y <= 1.0 ? h.x + (h.y - h.x) * f.x + (h.z - h.x) * f.y
                                           : h.w + (h.z - h.w) * (1.0 - f.x) + (h.y - h.w) * (1.0 - f.y);
    return p.y - surface;
}

// The first hit in [tMin, tMax), or tMax for none. Hits either side of a triangle, like a ray query.
float terrainRayMarch(vec3 o, vec3 d, float tMin, float tMax)
{
    TerrainRayTable table = TERRAIN_RAY_TABLE;
    const uint dim = table.tableDim;
    if (dim == 0u)
        return tMax;

    // Clip to the table's box and its height band. A zero component gets a huge inverse (never 0 x inf).
    const float chunkSize = table.chunkSize;
    const vec3 invD = vec3(1.0) / mix(d, vec3(1e-8), lessThan(abs(d), vec3(1e-8)));
    const vec3 boxLo = vec3(float(table.tableMin.x) * chunkSize, table.minHeight, float(table.tableMin.y) * chunkSize);
    const vec3 boxHi = vec3(boxLo.x + float(dim) * chunkSize, table.maxHeight, boxLo.z + float(dim) * chunkSize);
    const vec3 t0s = (boxLo - o) * invD, t1s = (boxHi - o) * invD;
    const vec3 near = min(t0s, t1s), far = max(t0s, t1s);
    float t = max(tMin, max(near.x, max(near.y, near.z)));
    const float tEnd = min(tMax, min(far.x, min(far.y, far.z)));
    if (t >= tEnd)
        return tMax;

    TerrainRayFloats floats = TerrainRayFloats(table.vertices);
    const uint res = table.lod0Res;
    const uint vpr = res + 1u;
    const float lod0Quad = table.lod0Quad;
    const uint bricksPerSide = max(res >> TERRAIN_BRICK_SHIFT, 1u);
    const uint brickCells = min(1u << TERRAIN_BRICK_SHIFT, res);

    float side = 0.0; // the ray's side of the surface at the last step: > 0 above, < 0 below, 0 unknown
    for (uint i = 0u; i < TERRAIN_RAY_MAX_STEPS && t < tEnd; ++i)
    {
        const vec3 p = o + d * t;
        const ivec2 chunk = ivec2(floor(p.xz / chunkSize));
        const ivec2 cellInTable = clamp(chunk - table.tableMin, ivec2(0), ivec2(int(dim) - 1));
        const TerrainRayEntry entry = table.entries[uint(cellInTable.y) * dim + uint(cellInTable.x)];
        float exitT = terrainRayExit(o.xz, invD.xz, vec2(table.tableMin + cellInTable) * chunkSize, chunkSize);

        float stepSide = 0.0; // set when the step's level rules the ray out
        if (entry.firstVertex == TERRAIN_RAY_NO_NODE)
        {
            side = 0.0;
        }
        else
        {
            float yEnd = o.y + d.y * min(exitT, tEnd);
            if (min(p.y, yEnd) > entry.maxHeight)
                stepSide = 1.0;
            else if (max(p.y, yEnd) < entry.minHeight)
                stepSide = -1.0;
            else
            {
                const int span = 1 << entry.lod;
                const vec2 nodeLo = vec2((chunk >> entry.lod) * span) * chunkSize;
                const float quad = lod0Quad * float(span);
                const uvec2 cell = uvec2(clamp(ivec2(floor((p.xz - nodeLo) / quad)), ivec2(0), ivec2(int(res) - 1)));
                const uvec2 brick = min(cell / brickCells, uvec2(bricksPerSide - 1u));
                const uint brickFloat = (entry.firstVertex + vpr * vpr) * 3u + (brick.y * bricksPerSide + brick.x) * 2u;
                const float brickExit = terrainRayExit(o.xz, invD.xz, nodeLo + vec2(brick * brickCells) * quad, float(brickCells) * quad);
                exitT = min(exitT, brickExit);
                yEnd = o.y + d.y * min(exitT, tEnd);
                if (min(p.y, yEnd) > floats.f[brickFloat + 1u])
                    stepSide = 1.0;
                else if (max(p.y, yEnd) < floats.f[brickFloat])
                    stepSide = -1.0;
                else
                {
                    const vec2 cellLo = nodeLo + vec2(cell) * quad;
                    exitT = min(exitT, terrainRayExit(o.xz, invD.xz, cellLo, quad));
                    const uint a = (entry.firstVertex + cell.y * vpr + cell.x) * 3u;
                    const vec4 h = vec4(floats.f[a], floats.f[a + 3u], floats.f[a + vpr * 3u], floats.f[a + (vpr + 1u) * 3u]);

                    // Up to two pieces: the height is linear in t on each side of the diagonal.
                    const float tCellEnd = min(exitT, tEnd);
                    const float along = d.x + d.z;
                    const float tDiag = t + (1.0 - ((p.x - cellLo.x) + (p.z - cellLo.y)) / quad) * quad * (abs(along) > 1e-8 ? 1.0 / along : 1e8);
                    const float tMid = clamp(tDiag, t, tCellEnd);

                    const float g0 = terrainRayGap(o, d, t, cellLo, quad, h);
                    if (side != 0.0 && (side < 0.0) != (g0 < 0.0))
                        return t;
                    const float g1 = terrainRayGap(o, d, tMid, cellLo, quad, h);
                    if ((g0 < 0.0) != (g1 < 0.0))
                        return t + (tMid - t) * g0 / (g0 - g1);
                    const float g2 = terrainRayGap(o, d, tCellEnd, cellLo, quad, h);
                    if ((g1 < 0.0) != (g2 < 0.0))
                        return tMid + (tCellEnd - tMid) * g1 / (g1 - g2);
                    side = g2;
                }
            }
        }
        if (stepSide != 0.0)
        {
            if (side != 0.0 && (side < 0.0) != (stepSide < 0.0))
                return t;
            side = stepSide;
        }
        // Always forward, past the boundary (float rounding can leave p on its near side).
        t = max(exitT, t) + 1e-3 + t * 1e-6;
    }
    return tMax;
}

// The interpolated vertex normal of the surface at a march hit (unit, y > 0).
vec3 terrainRayNormal(vec3 hitPos)
{
    TerrainRayTable table = TERRAIN_RAY_TABLE;
    const float chunkSize = table.chunkSize;
    const ivec2 chunk = ivec2(floor(hitPos.xz / chunkSize));
    const ivec2 cellInTable = clamp(chunk - table.tableMin, ivec2(0), ivec2(int(table.tableDim) - 1));
    const TerrainRayEntry entry = table.entries[uint(cellInTable.y) * table.tableDim + uint(cellInTable.x)];
    if (entry.firstVertex == TERRAIN_RAY_NO_NODE)
        return vec3(0.0, 1.0, 0.0);

    const int span = 1 << entry.lod;
    const vec2 nodeLo = vec2((chunk >> entry.lod) * span) * chunkSize;
    const float quad = table.lod0Quad * float(span);
    const uint res = table.lod0Res;
    const uint vpr = res + 1u;
    const vec2 local = clamp((hitPos.xz - nodeLo) / quad, vec2(0.0), vec2(float(res) - 1e-3));
    const uvec2 cell = uvec2(local);
    const vec2 f = local - vec2(cell);
    TerrainRayWords words = TerrainRayWords(table.vertices);
    const uint a = (entry.firstVertex + cell.y * vpr + cell.x) * 3u + 1u; // the normal word of vertex a
    const vec3 nb = terrainVertexNormal(words.w[a + 3u]);
    const vec3 nc = terrainVertexNormal(words.w[a + vpr * 3u]);
    if (f.x + f.y <= 1.0)
    {
        const vec3 na = terrainVertexNormal(words.w[a]);
        return normalize(na + (nb - na) * f.x + (nc - na) * f.y);
    }
    const vec3 nd = terrainVertexNormal(words.w[a + (vpr + 1u) * 3u]);
    return normalize(nd + (nc - nd) * (1.0 - f.x) + (nb - nd) * (1.0 - f.y));
}

#endif
