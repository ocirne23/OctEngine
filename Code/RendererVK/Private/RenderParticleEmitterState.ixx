export module RendererVK:RenderParticleEmitterState;

import Core;

// Everything of RenderParticleEmitter but its public API (RenderParticleEmitter.ixx).
export class RenderParticleEmitterState
{
protected:

    void moveFrom(RenderParticleEmitterState& other)
    {
        m_slot = other.m_slot;
        other.m_slot = UINT32_MAX;
    }

    uint32 m_slot = UINT32_MAX;
};
