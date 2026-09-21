export module RendererVK:ForceFieldState;

import Core;
import Core.glm;
import :Layout;
import :Settings;
import :SlotTable;
import :ForceFieldPipeline;

// The CPU side of the force fields, owned by the Renderer: the params the Force library pushes every
// frame, the two persistent slot registries the outside creates and destroys, this frame's baked-field
// chunk set, and the three values the between-frames grid job passes between its kick and its join.
//
// BOTH registries hand out STABLE indices across the ~2-frame readback latency, which is why they are
// RecycledSlotTables and not a lock-free per-frame push: the emitter force readback and the query
// results are slot-indexed, so a slot must not be re-issued while stale results can still land. The
// caller holds the Renderer's spawn mutex around every mutator.
//
// Everything here is read by buildUboForce and the force record, which stay in the Renderer.
export class ForceFieldState final
{
public:
    void initialize()
    {
        m_emitters.initialize(RendererVKLayout::MAX_FORCE_EMITTERS);
        m_queries.initialize(RendererVKLayout::MAX_FORCE_QUERIES);
    }

    // ---- Params (pushed per frame; all UBO-driven, so all live) ----
    void setParams(const ForceFieldParams& params) { m_params = params; }
    const ForceFieldParams& getParams() const { return m_params; }
    bool isEnabled() const { return m_params.enabled; }

    // ---- Emitters. A destroyed slot keeps FORCE_FLAG_ACTIVE cleared until the in-flight frames drain. ----
    uint32 createEmitter(uint32 frameCounter, const RendererVKLayout::ForceEmitterGpu& desc)
    {
        const uint32 slot = m_emitters.create(frameCounter);
        if (slot == UINT32_MAX)
            return slot;
        m_emitters[slot] = desc;
        m_emitters[slot].teamFlags.y |= RendererVKLayout::FORCE_FLAG_ACTIVE;
        return slot;
    }
    void updateEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc)
    {
        assert(m_emitters.isValid(slot));
        m_emitters[slot] = desc;
    }
    void destroyEmitter(uint32 slot, uint32 frameCounter)
    {
        assert(m_emitters.isValid(slot));
        m_emitters[slot].teamFlags.y &= ~RendererVKLayout::FORCE_FLAG_ACTIVE;
        m_emitters.retire(slot, frameCounter);
    }
    oc::span<const RendererVKLayout::ForceEmitterGpu> getEmitters() const { return m_emitters.slots(); }

    // ---- Point-query slots (same contract; inactive until the first setQuery) ----
    uint32 createQuery(uint32 frameCounter)
    {
        const uint32 slot = m_queries.create(frameCounter);
        if (slot != UINT32_MAX)
            m_queries[slot].posActive = glm::vec4(0.0f);
        return slot;
    }
    void setQuery(uint32 slot, const glm::vec3& pos)
    {
        assert(m_queries.isValid(slot));
        m_queries[slot].posActive = glm::vec4(pos, 1.0f);
    }
    void destroyQuery(uint32 slot, uint32 frameCounter)
    {
        assert(m_queries.isValid(slot));
        m_queries[slot].posActive = glm::vec4(0.0f);
        m_queries.retire(slot, frameCounter);
    }
    oc::span<const RendererVKLayout::ForceQueryGpu> getQueries() const { return m_queries.slots(); }

    // ---- The baked pressure field ----
    // This frame's chunk set (main-thread, pushed with the emitters): uploaded at present, evaluated
    // by force_bake.cs, read back ~2 frames later.
    void setBakeChunks(oc::span<const glm::ivec4> chunks, float sampleY)
    {
        m_bakeChunks.assign(chunks.begin(), chunks.end());
        m_bakeSampleY = sampleY;
    }
    oc::span<const glm::ivec4> getBakeChunks() const { return m_bakeChunks; }
    float getBakeSampleY() const { return m_bakeSampleY; }
    // A large emitter qualified for the sampled shell tier this frame (buildUboForce fit the volume).
    void setShellBakeActive(bool active) { m_shellBakeActive = active; }
    bool isShellBakeActive() const { return m_shellBakeActive; }

    // ---- The between-frames grid job (kickGridBuilds -> joinGridBuilds) ----
    ForceFieldPipeline::ShellCull& getShellCull() { return m_shellCull; }   // compaction inputs, set at the kick
    ForceFieldPipeline::GridDemand& getGridDemand() { return m_gridDemand; } // what the job measured
    void setGridNeedsGrow(bool needsGrow) { m_gridNeedsGrow = needsGrow; }
    bool getGridNeedsGrow() const { return m_gridNeedsGrow; }

private:
    ForceFieldParams m_params;
    RecycledSlotTable<RendererVKLayout::ForceEmitterGpu> m_emitters;
    RecycledSlotTable<RendererVKLayout::ForceQueryGpu> m_queries;

    oc::vector<glm::ivec4> m_bakeChunks;
    float m_bakeSampleY = 1.0f;
    bool m_shellBakeActive = false;

    ForceFieldPipeline::ShellCull m_shellCull;
    ForceFieldPipeline::GridDemand m_gridDemand;
    bool m_gridNeedsGrow = false;
};
