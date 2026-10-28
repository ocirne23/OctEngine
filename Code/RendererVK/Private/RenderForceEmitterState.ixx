export module RendererVK:RenderForceEmitterState;

import Core;

// Everything of RenderForceEmitter but its public API (RenderForceEmitter.ixx).
export class RenderForceEmitterState
{
protected:

    void moveFrom(RenderForceEmitterState& other)
    {
        m_slot = other.m_slot;
        other.m_slot = UINT32_MAX;
    }

    uint32 m_slot = UINT32_MAX;
};
