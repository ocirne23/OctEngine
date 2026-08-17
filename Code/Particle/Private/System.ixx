export module Particle:System;

import Core;
import Core.glm;
import RendererVK;
import :Effect;

// Runtime particle/decal manager (Globals::particleSystem). Owns effect instances (each holding one
// renderer emitter slot per emitter in its desc), the .pfx/texture caches, and the aging decal pool.
// update() runs once per frame on the main thread (after the entity update, before present) and is
// the only place renderer emitter state is written; ParticleEffect::setTransform/setVelocity only
// store into the instance's own slot, so they are safe to call from the parallel entity update pass
// (one writer per instance). create/destroy/burst/spawnDecal are main-thread (serial script prepass,
// component spawn/destroy, or app code).

export class ParticleSystem;

// RAII handle to a spawned effect instance (all its emitters). Move-only, like PhysicsBody/RenderNode;
// destroying it retires the instance's live particles over the next frames.
export class ParticleEffect final
{
public:
    ParticleEffect() = default;
    ParticleEffect(ParticleEffect&& move) noexcept : m_id(move.m_id) { move.m_id = 0; }
    ParticleEffect& operator=(ParticleEffect&& move) noexcept;
    ParticleEffect(const ParticleEffect&) = delete;
    ~ParticleEffect() { destroy(); }

    bool isValid() const { return m_id != 0; }
    void destroy();

    // Safe from the parallel entity update (writes only this instance's slot).
    void setTransform(const glm::vec3& pos, const glm::quat& rot);
    void setVelocity(const glm::vec3& velocity);

    void setEmitting(bool emitting); // pauses/resumes continuous rates (bursts still fire)
    bool isEmitting() const;         // false for an invalid/destroyed effect
    void burst();                    // queues every emitter's Burst count this frame

private:
    friend class ParticleSystem;
    explicit ParticleEffect(uint64 id) : m_id(id) {}
    uint64 m_id = 0;
};

export class ParticleSystem final
{
public:
    void initialize();
    // Advances rate accumulators/bursts into renderer spawn requests, re-uploads emitter GPU state,
    // ages + submits decals. Call once per frame from the main loop, after world.update.
    void update(Renderer& renderer, float deltaSec);

    // Cached .pfx load (path relative to Assets/); nullptr + log on failure.
    const ParticleEffectDesc* getEffectDesc(const oc::string& path);
    void invalidateEffect(const oc::string& path); // next getEffectDesc reloads from disk

    // Instantiates an effect (by cached .pfx path or an in-code desc, which is copied).
    ParticleEffect createEffect(const oc::string& pfxPath, const glm::vec3& pos = glm::vec3(0.0f), const glm::quat& rot = glm::quat(1, 0, 0, 0));
    ParticleEffect createEffect(const ParticleEffectDesc& desc, const glm::vec3& pos = glm::vec3(0.0f), const glm::quat& rot = glm::quat(1, 0, 0, 0));

    // Projects a decal onto a surface (pos = hit point, normal = surface normal). Fire-and-forget:
    // it ages out per desc.lifetime (0 = persistent); the returned id can removeDecal early (fades
    // out over desc.fadeOutTime). The pool caps at MAX_DECALS; the oldest non-persistent decal is
    // evicted when full. Returns 0 when the spawn was dropped.
    uint32 spawnDecal(const DecalDesc& desc, const glm::vec3& pos, const glm::vec3& normal);
    void removeDecal(uint32 id);
    void clearDecals();

    uint32 getNumEffects() const { return (uint32)m_effects.size(); }
    uint32 getNumDecals() const { return (uint32)m_decals.size(); }

private:
    friend class ParticleEffect;

    struct EmitterInstance
    {
        uint32 rendererSlot = UINT32_MAX;
        uint16 textureIdx = 0xFFFF; // PARTICLE_TEX_NONE or resolved bindless index
        float rateAccum = 0.0f;
    };
    struct EffectInstance
    {
        uint64 id = 0;
        glm::vec3 pos{ 0.0f };
        glm::quat rot{ 1, 0, 0, 0 };
        glm::vec3 velocity{ 0.0f };
        bool emitting = true;
        bool pendingBurst = false;
        oc::shared_ptr<const ParticleEffectDesc> desc; // shared so cache invalidation can't dangle
        oc::vector<EmitterInstance> emitters;          // parallel to desc->emitters
    };
    struct DecalInstance
    {
        uint32 id = 0;
        RendererVKLayout::DecalInfo info;
        float age = 0.0f;
        float lifetime = 0.0f; // 0 = persistent (removeDecal turns it finite to fade out)
        float fadeOutTime = 1.0f;
    };

    ParticleEffect createEffectInstance(oc::shared_ptr<const ParticleEffectDesc> desc, const glm::vec3& pos, const glm::quat& rot);
    EffectInstance* findEffect(uint64 id);
    void destroyEffect(uint64 id);
    uint16 getTexture(const oc::string& path, bool sRGB);

    oc::vector<EffectInstance> m_effects;
    oc::vector<DecalInstance> m_decals;
    oc::unordered_map<oc::string, oc::shared_ptr<const ParticleEffectDesc>> m_effectCache;
    oc::unordered_map<oc::string, uint16> m_textureCache;
    uint64 m_nextEffectId = 1;
    uint32 m_nextDecalId = 1;
    uint32 m_rngState = 0x12345678;
};

export namespace Globals
{
    ParticleSystem particleSystem;
}
