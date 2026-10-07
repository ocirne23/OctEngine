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

#endif
