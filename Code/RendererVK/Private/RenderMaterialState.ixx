export module RendererVK:RenderMaterialState;

import Core;

// Everything of RenderMaterial but its public API (RenderMaterial.ixx).
export class RenderMaterialState
{
protected:

    void moveFrom(RenderMaterialState& other)
    {
        m_index = other.m_index;
        m_ownsTextures = other.m_ownsTextures;
        other.m_index = UINT16_MAX;
    }

    uint16 m_index = UINT16_MAX;
    bool m_ownsTextures = false; // createTextured's: destroy frees its textures too (a mesh or derived material: the slot only)
};
