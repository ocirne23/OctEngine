export module RendererVK:ParticleState;

import Core;
import Core.glm;
import :Layout;
import :Settings;
import :SlotTable;

// The CPU side of the GPU particle system and the weather volume it shelters: the emitter slot table,
// this frame's spawn requests, the "Particles" tweaks (sim + wind + rain occlusion) and the two
// one-shot flags the frame loop carries.
//
// Two contracts worth knowing:
//  * a DESTROYED emitter is not freed - its KILL flag has to drain through the sim (its particles are
//    gone a couple of simulated frames later), which is what RecycledSlotTable's retirement gives it;
//  * the POOL RESET is spent only on a frame that will actually execute the sim. A reset consumed by a
//    frame that never runs (acquire failure, no mesh instances) would leave the dead stack empty
//    forever and silently drop every spawn afterwards, so the flag clears only after a submit.
//
// The rain occlusion volume is double-buffered by hand: the request the main thread writes after the
// begin-frame join is latched by present, so the NEXT frame's begin-frame job reads it without a race.
export class ParticleState final
{
public:
    struct RainVolume
    {
        glm::vec3 center{ 0.0f };
        glm::vec3 halfExtents{ 0.0f };
        bool active = false;
    };

    void initialize() { m_emitters.initialize(RendererVKLayout::MAX_PARTICLE_EMITTERS); }
    void registerTweaks() { m_params.registerTweaks(); }
    const ParticleParams& getParams() const { return m_params; }
    bool isEnabled() const { return m_params.enabled; }

    // ---- Emitters + spawns (spawn path, caller holds the spawn mutex) ----
    uint32 createEmitter(uint32 frameCounter, const RendererVKLayout::ParticleEmitterGpu& desc)
    {
        const uint32 slot = m_emitters.create(frameCounter);
        if (slot != UINT32_MAX)
            m_emitters[slot] = desc;
        return slot;
    }
    void updateEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc)
    {
        assert(m_emitters.isValid(slot));
        m_emitters[slot] = desc;
    }
    void destroyEmitter(uint32 slot, uint32 frameCounter)
    {
        assert(m_emitters.isValid(slot));
        m_emitters[slot].texFlags.y |= RendererVKLayout::PARTICLE_FLAG_KILL;
        m_emitters.retire(slot, frameCounter);
    }
    void emit(uint32 slot, uint32 count)
    {
        assert(m_emitters.isValid(slot));
        if (count > 0)
            m_spawnRequests.emplace_back((uint16)slot, (uint16)oc::min(count, 0xFFFFu));
    }
    oc::span<const RendererVKLayout::ParticleEmitterGpu> getEmitters() const { return m_emitters.slots(); }
    uint32 getNumEmitters() const { return m_emitters.size(); }
    oc::span<const oc::pair<uint16, uint16>> getSpawnRequests() const { return m_spawnRequests; } // emitter slot, count
    void clearSpawnRequests() { m_spawnRequests.clear(); }

    // ---- The weather volume's rain occlusion box ----
    void requestRainVolume(const glm::vec3& center, const glm::vec3& halfExtents) { m_rainRequest = { center, halfExtents, true }; }
    void latchRainVolume() { m_rainActive = m_rainRequest; m_rainRequest.active = false; } // present()
    const RainVolume& getRainVolume() const { return m_rainActive; }

    // ---- The one-shot pool reset ----
    bool isResetPending() const { return m_resetPending; }
    void requestReset() { m_resetPending = true; }
    void clearResetPending() { m_resetPending = false; } // only after a frame that carried it was SUBMITTED

    // ---- The always-on pool-exhaustion warning, rate limited to one line per 300 frames ----
    bool shouldLogDrop(uint32 frameCounter) const { return m_dropLogFrame == 0 || frameCounter - m_dropLogFrame >= 300; }
    void noteDropLogged(uint32 frameCounter) { m_dropLogFrame = frameCounter; }
    bool wasDropping() const { return m_dropLogFrame != 0; }
    void clearDropping() { m_dropLogFrame = 0; }

private:
    RecycledSlotTable<RendererVKLayout::ParticleEmitterGpu> m_emitters;
    oc::vector<oc::pair<uint16, uint16>> m_spawnRequests;
    ParticleParams m_params;

    RainVolume m_rainRequest;
    RainVolume m_rainActive;
    bool m_resetPending = true; // the first simulating frame initializes the pool
    uint32 m_dropLogFrame = 0;  // 0 = not warning
};
