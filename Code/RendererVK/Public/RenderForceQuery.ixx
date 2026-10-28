export module RendererVK:RenderForceQuery;

import Core;
import Core.glm;

import :Layout;
import :RenderForceQueryState; // the state

// RAII owner of a GPU forcefield query slot (move-only, like RenderMesh): the field sampled at one point. Destroying it retires the slot once the GPU has
// drained it. create and destroy run from ANY THREAD (parallel entity spawning; the slot table is lock-free), but the renderer must be alive: the owner
// destructs before it (InitSeg.h). setPosition and readback: between beginFrame and present. The state is in RenderForceQueryState.ixx; the multi-line
// bodies are below the class.
export class RenderForceQuery final : public RenderForceQueryState
{
public:

    static RenderForceQuery create(); // invalid: out of slots

    RenderForceQuery() = default;
    RenderForceQuery(RenderForceQuery&& other) noexcept { moveFrom(other); }
    RenderForceQuery& operator=(RenderForceQuery&& other) noexcept;
    ~RenderForceQuery() { destroy(); }

    bool isValid() const { return m_slot != UINT32_MAX; }
    void destroy();

    void setPosition(const glm::vec3& pos);
    RendererVKLayout::ForceQueryResult readback() const; // ~2 frames old

    RenderForceQuery(const RenderForceQuery&) = delete;
    RenderForceQuery& operator=(const RenderForceQuery&) = delete;
};

// ---- Inline bodies ----

inline RenderForceQuery& RenderForceQuery::operator=(RenderForceQuery&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
