export module RendererVK:RenderForceEmitter;

import Core;
import Core.glm;

import :Layout;
import :RenderForceEmitterState; // the state

// RAII owner of a GPU forcefield emitter slot (move-only, like RenderMesh). Destroying it retires the slot once the GPU has drained it. create and destroy
// run from ANY THREAD (parallel entity spawning; the slot table is lock-free), but the renderer must be alive: the owner destructs before it (InitSeg.h).
// update and readback: between beginFrame and present, one writer per emitter. The state is in RenderForceEmitterState.ixx; the multi-line bodies are below
// the class.
export class RenderForceEmitter final : public RenderForceEmitterState
{
public:

    static RenderForceEmitter create(const RendererVKLayout::ForceEmitterGpu& desc); // invalid: out of slots

    RenderForceEmitter() = default;
    RenderForceEmitter(RenderForceEmitter&& other) noexcept { moveFrom(other); }
    RenderForceEmitter& operator=(RenderForceEmitter&& other) noexcept;
    ~RenderForceEmitter() { destroy(); }

    bool isValid() const { return m_slot != UINT32_MAX; }
    void destroy();

    void update(const RendererVKLayout::ForceEmitterGpu& desc);
    glm::vec4 readback() const; // ~2 frames old: xyz = applied force (opposing pressure integral), w = mean opposing pressure

    RenderForceEmitter(const RenderForceEmitter&) = delete;
    RenderForceEmitter& operator=(const RenderForceEmitter&) = delete;
};

// ---- Inline bodies ----

inline RenderForceEmitter& RenderForceEmitter::operator=(RenderForceEmitter&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
