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

// RendererVKLayout::TerrainVertex: 20 B, a terrain node's vertex in the same mega-buffer, read through a second block on
// the buffer's binding (in_terrainVertices). A mesh's vertexOffset counts these for a MATERIAL_FLAG_TERRAIN material.
struct TerrainVertex
{
    float x, y, z;  // node-local X/Z, world Y
    uint normalOct; // terrainVertexNormal
    uint data;      // unpackUnorm4x8: x = the river influence, y = the flow speed / TERRAIN_VERTEX_SPEED_MAX,
                    // z = 1 under river or lake water; the top byte = the node's LOD
};

vec3 terrainVertexNormal(uint normalOct)
{
    const vec2 e = unpackSnorm2x16(normalOct);
    return normalize(vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y));
}

#endif
