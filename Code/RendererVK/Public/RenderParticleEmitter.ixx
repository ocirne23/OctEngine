export module RendererVK:RenderParticleEmitter;

import Core;

import :Layout;
import :RenderParticleEmitterState; // the state

// RAII owner of a GPU particle emitter slot (move-only, like RenderMesh). Destroying it retires the slot: its live particles die over the next frames, and
// the slot is reused once the GPU has drained them. create and destroy run from ANY THREAD (parallel entity spawning; the slot table is lock-free), but the
// renderer must be alive: the owner destructs before it (InitSeg.h). update and emit: between beginFrame and present, one writer per emitter. The state is in
// RenderParticleEmitterState.ixx; the multi-line bodies are below the class.
export class RenderParticleEmitter final : public RenderParticleEmitterState
{
public:

    static RenderParticleEmitter create(const RendererVKLayout::ParticleEmitterGpu& desc); // invalid: out of slots

    RenderParticleEmitter() = default;
    RenderParticleEmitter(RenderParticleEmitter&& other) noexcept { moveFrom(other); }
    RenderParticleEmitter& operator=(RenderParticleEmitter&& other) noexcept;
    ~RenderParticleEmitter() { destroy(); }

    bool isValid() const { return m_slot != UINT32_MAX; }
    uint32 slot() const { return m_slot; } // what Renderer::setOceanSprayEmitter / setRiverMistEmitter take
    void destroy();

    void update(const RendererVKLayout::ParticleEmitterGpu& desc);
    void emit(uint32 count); // spawns `count` particles this frame

    RenderParticleEmitter(const RenderParticleEmitter&) = delete;
    RenderParticleEmitter& operator=(const RenderParticleEmitter&) = delete;
};

// ---- Inline bodies ----

inline RenderParticleEmitter& RenderParticleEmitter::operator=(RenderParticleEmitter&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
