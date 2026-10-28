export module RendererVK:RenderForceQueryState;

import Core;

// Everything of RenderForceQuery but its public API (RenderForceQuery.ixx).
export class RenderForceQueryState
{
protected:

    void moveFrom(RenderForceQueryState& other)
    {
        m_slot = other.m_slot;
        other.m_slot = UINT32_MAX;
    }

    uint32 m_slot = UINT32_MAX;
};
