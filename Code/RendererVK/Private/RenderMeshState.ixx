export module RendererVK:RenderMeshState;

import Core;
import Core.Sphere;

export class Renderer; // declared before the friend line below, which would otherwise introduce the name (RendererVK.fwd has an extern "C++" one)

// Everything of RenderMesh but its public API (RenderMesh.ixx).
export class RenderMeshState
{
protected:

    friend class Renderer;
    friend class RendererState;

    void moveFrom(RenderMeshState& other)
    {
        m_firstVertex = other.m_firstVertex;
        m_numVertices = other.m_numVertices;
        m_firstIndex = other.m_firstIndex;
        m_numIndices = other.m_numIndices;
        m_bounds = other.m_bounds;
        m_lodGroup = other.m_lodGroup;
        m_meshIdx = other.m_meshIdx;
        m_numLevels = other.m_numLevels;
        other.m_lodGroup = UINT32_MAX;
        other.m_meshIdx = UINT16_MAX;
    }

    // Element units in the mega-buffers (MeshVertex / MeshIndex), like MeshInfo's: ONE range each over every level (level 0 first).
    uint32 m_firstVertex = 0;
    uint32 m_numVertices = 0;
    uint32 m_firstIndex = 0;
    uint32 m_numIndices = 0;
    Sphere m_bounds;                  // level 0's
    uint32 m_lodGroup = UINT32_MAX;   // the GPU LOD chain over the levels (UINT32_MAX = one level)
    uint16 m_meshIdx = UINT16_MAX;    // level 0's MeshInfo; level k is m_meshIdx + k
    uint8 m_numLevels = 1;
};
