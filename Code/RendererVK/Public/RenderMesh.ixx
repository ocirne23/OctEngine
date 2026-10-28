export module RendererVK:RenderMesh;

import Core;
import Core.glm;
import Core.Sphere;
import File;

import :Layout;
import :RenderMeshState; // the state

// ONE mesh without an ObjectContainer, for generated geometry that streams in and out as single meshes with a shared material (terrain chunks, ocean
// sectors, rocks). No nodes, names, materials or textures of its own: Renderer::createMesh uploads the data and registers one MeshInfo (plus its BLAS) per
// LOD level - with 2+ levels also the GPU LOD chain over them, so the cull picks each instance's level - and Renderer::spawnMeshNode places it as a
// RenderNode with the caller's shared material. The multi-line bodies are below the types.

// The CPU side, pure and callable from any job (the terrain builds it in its generation pump).
export struct RenderMeshData
{
    oc::vector<RendererVKLayout::MeshVertex> vertices;
    oc::vector<RendererVKLayout::MeshIndex> indices;
    Sphere bounds; // mesh-local, the AABB's centre + half diagonal

    // Null tangents / bitangents / texCoords default like ISceneData::createMeshScene's (the tangent handedness is ObjectContainer's formula), so the shading
    // matches the container path exactly. texCoords.z >= 2 rides the tangent's w as its MAGNITUDE (the sign stays the handedness): the tree billboard cards'
    // strip code (2 + the strip height, < 3) or the merged branch cards' axis code (>= 3) (Procedural billboardMesh, read by the lit FS's foliageCrownFrame).
    // texCoords.z in (1, 2) does too: a tree mesh vertex's WIND payload (Procedural TreeGenerator, read by tree_wind.inc.glsl). Every other mesh: |w| = 1.
    void build(const MeshGeometryDesc& geometry);
};

// RAII owner of the uploaded mesh and its LOD levels (move-only, like RenderNode). Destroy every RenderNode spawned from it first; the free drops the LOD
// chain and neutralizes the MeshInfo slots, so a node pushed earlier this frame draws nothing instead of stale data. The state is in RenderMeshState.ixx.
export class RenderMesh final : public RenderMeshState
{
public:

    RenderMesh() = default;
    RenderMesh(RenderMesh&& other) noexcept { moveFrom(other); }
    RenderMesh& operator=(RenderMesh&& other) noexcept;
    ~RenderMesh() { destroy(); }

    bool isValid() const { return m_meshIdx != UINT16_MAX; }
    void destroy(); // main thread (Renderer::destroyMesh)

    const Sphere& getBounds() const { return m_bounds; } // level 0's
    uint32 getFirstVertex() const { return m_firstVertex; } // in the vertex mega-buffer, MeshVertex units (the grass reads the terrain's)

    RenderMesh(const RenderMesh&) = delete;
    RenderMesh& operator=(const RenderMesh&) = delete;
};

// ---- Inline bodies ----

inline void RenderMeshData::build(const MeshGeometryDesc& geometry)
{
    const uint32 numVertices = geometry.numVertices;
    vertices.resize(numVertices);
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (uint32 i = 0; i < numVertices; ++i)
    {
        const glm::vec3 normal = geometry.normals[i];
        const glm::vec3 tangent = geometry.tangents ? geometry.tangents[i] : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 bitangent = geometry.bitangents ? geometry.bitangents[i] : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 position = geometry.positions[i];
        const float magnitude = geometry.texCoords && geometry.texCoords[i].z > 1.0f ? geometry.texCoords[i].z : 1.0f;
        vertices[i].set(position, normal,
            glm::vec4(tangent, glm::dot(normal, glm::cross(tangent, bitangent)) >= 0.0f ? magnitude : -magnitude),
            geometry.texCoords ? glm::vec2(geometry.texCoords[i]) : glm::vec2(0.0f));
        mn = glm::min(mn, position);
        mx = glm::max(mx, position);
    }
    indices.assign(geometry.indices, geometry.indices + geometry.numIndices);
    bounds.pos = (mn + mx) * 0.5f;
    bounds.radius = glm::length(mx - mn) * 0.5f;
}

inline RenderMesh& RenderMesh::operator=(RenderMesh&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
