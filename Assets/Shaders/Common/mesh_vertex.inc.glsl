// RendererVKLayout::MeshVertex (Layout.ixx): 3 x vec4 = 48 B, every member 16 B-aligned, so this plain std430
// struct matches the C++ one and a member read is one wide load. The texCoord rides the two .w components:
// meshVertexUV. Read by the skinning pass and every ray-hit vertex fetch (the GI trace, RTAO, the shadow
// rays' alpha test, the ocean's and the terrain's mirror hits).

#ifndef MESH_VERTEX_INC_GLSL
#define MESH_VERTEX_INC_GLSL

struct MeshVertex
{
    vec4 positionU; // xyz = position, w = texCoord.x
    vec4 normalV;   // xyz = normal,   w = texCoord.y
    vec4 tangent;   // xyz = tangent,  w = bitangent sign
};

#define meshVertexUV(v) vec2((v).positionU.w, (v).normalV.w)

// RendererVKLayout::TerrainVertex: 12 B, a terrain node's vertex in the same mega-buffer, read through a second block on
// the buffer's binding (in_terrainVertices). A mesh's vertexOffset counts these for a MATERIAL_FLAG_TERRAIN material.
struct TerrainVertex
{
    float y;        // world Y
    uint normalLod; // terrainVertexNormal, terrainVertexUnderWater, terrainVertexLod
    uint data;      // bytes: the river influence, the flow speed / TERRAIN_VERTEX_SPEED_MAX, the grid column, the grid row
};

vec3 terrainVertexNormal(uint normalLod)
{
    const vec2 e = vec2(int(normalLod & 0x3FFFu) - 8192, int((normalLod >> 14) & 0x3FFFu) - 8192) * (1.0 / 8191.0);
    return normalize(vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y));
}
float terrainVertexUnderWater(uint normalLod) { return float((normalLod >> 28) & 1u); }
uint terrainVertexLod(uint normalLod) { return normalLod >> 29; }
// Node-local X/Z from the grid position. lod0Quad: u_terrain_stitchBands.w.
vec2 terrainVertexXZ(uint data, uint lod, float lod0Quad)
{
    return vec2((data >> 16) & 255u, data >> 24) * (lod0Quad * float(1u << lod));
}

#endif
