export module RendererVK:RenderTextureState;

import Core;

// Everything of RenderTexture but its public API (RenderTexture.ixx).
export class RenderTextureState
{
protected:

    void moveFrom(RenderTextureState& other)
    {
        m_index = other.m_index;
        other.m_index = UINT16_MAX;
    }

    uint16 m_index = UINT16_MAX;
};
