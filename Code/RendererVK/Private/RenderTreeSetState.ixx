export module RendererVK:RenderTreeSetState;

import Core;

export class Renderer;      // declared before their friend lines
export class RendererState;

// Everything of RenderTreeSet but its public API (RenderTreeSet.ixx).
export class RenderTreeSetState
{
protected:

    friend class Renderer; // setTreeRecordTypes names the set
    friend class RendererState;

    void moveFrom(RenderTreeSetState& other)
    {
        m_setId = other.m_setId;
        other.m_setId = UINT32_MAX;
    }

    uint32 m_setId = UINT32_MAX;
};
