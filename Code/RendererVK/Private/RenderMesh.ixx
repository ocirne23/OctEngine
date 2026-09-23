export module RendererVK:RenderMesh;

import Core;
import Core.glm;
import Core.Sphere;
import File;

import :Layout;

// ONE mesh without an ObjectContainer, for generated geometry that streams in and out as single
// meshes with a shared material (terrain chunks, ocean sectors). No nodes, names, materials or
// textures of its own: Renderer::createMesh uploads the data and registers one MeshInfo (plus its
// BLAS), Renderer::spawnMeshNode places it as a RenderNode with the caller's shared material.

// The CPU side, pure and callable from any job (the terrain builds it in its generation pump).
export struct RenderMeshData
{
    oc::vector<RendererVKLayout::MeshVertex> vertices;
    oc::vector<RendererVKLayout::MeshIndex> indices;
    Sphere bounds; // mesh-local, the AABB's centre + half diagonal

    // Null tangents / bitangents / texCoords default like ISceneData::createMeshScene's (the tangent
    // handedness is ObjectContainer's formula), so the shading matches the container path exactly.
    void build(const MeshGeometryDesc& geometry)
    {
        const uint32 numVertices = geometry.numVertices;
        vertices.resize(numVertices);
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (uint32 i = 0; i < numVertices; ++i)
        {
            const glm::vec3 normal = geometry.normals[i];
            const glm::vec3 tangent = geometry.tangents ? geometry.tangents[i] : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 bitangent = geometry.bitangents ? geometry.bitangents[i] : glm::vec3(0.0f, 0.0f, 1.0f);
            RendererVKLayout::MeshVertex& v = vertices[i];
            v.position = geometry.positions[i];
            v.normal = normal;
            v.tangent = glm::vec4(tangent, glm::dot(normal, glm::cross(tangent, bitangent)) >= 0.0f ? 1.0f : -1.0f);
            v.texCoord = geometry.texCoords ? glm::vec2(geometry.texCoords[i]) : glm::vec2(0.0f);
            mn = glm::min(mn, v.position);
            mx = glm::max(mx, v.position);
        }
        indices.assign(geometry.indices, geometry.indices + geometry.numIndices);
        bounds.pos = (mn + mx) * 0.5f;
        bounds.radius = glm::length(mx - mn) * 0.5f;
    }
};

// RAII owner of the uploaded mesh (move-only, like RenderNode). Destroy every RenderNode spawned
// from it first; the free neutralizes the MeshInfo slot, so a node pushed earlier this frame draws
// nothing instead of stale data.
export class RenderMesh final
{
public:

    RenderMesh() = default;
    RenderMesh(const RenderMesh&) = delete;
    RenderMesh& operator=(const RenderMesh&) = delete;
    RenderMesh(RenderMesh&& other) noexcept { moveFrom(other); }
    RenderMesh& operator=(RenderMesh&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            moveFrom(other);
        }
        return *this;
    }
    ~RenderMesh() { destroy(); }

    bool isValid() const { return m_meshIdx != UINT16_MAX; }
    void destroy(); // main thread (Renderer::destroyMesh)
    const Sphere& getBounds() const { return m_bounds; }

private:

    friend class Renderer;

    void moveFrom(RenderMesh& other)
    {
        m_firstVertex = other.m_firstVertex;
        m_numVertices = other.m_numVertices;
        m_firstIndex = other.m_firstIndex;
        m_numIndices = other.m_numIndices;
        m_bounds = other.m_bounds;
        m_meshIdx = other.m_meshIdx;
        other.m_meshIdx = UINT16_MAX;
    }

    // Element units in the mega-buffers (MeshVertex / MeshIndex), like MeshInfo's.
    uint32 m_firstVertex = 0;
    uint32 m_numVertices = 0;
    uint32 m_firstIndex = 0;
    uint32 m_numIndices = 0;
    Sphere m_bounds;
    uint16 m_meshIdx = UINT16_MAX;
};
