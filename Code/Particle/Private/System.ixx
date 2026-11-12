export module Particle:System;

import Core;
import Core.glm;
import RendererVK;
import Settings;
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
    void initialize() {}
    // Advances rate accumulators/bursts into renderer spawn requests, re-uploads emitter GPU state,
    // ages + submits decals. Call once per frame from the main loop, after world.update.
    void update(Renderer& renderer, float deltaSec);

    // Cached .pfx load (path relative to Assets/); nullptr + log on failure.
    const ParticleEffectDesc* getEffectDesc(const oc::string& path);
    void invalidateEffect(const oc::string& path); // next getEffectDesc reloads from disk

    // Instantiates an effect (by cached .pfx path or an in-code desc, which is copied).
    ParticleEffect createEffect(const oc::string& pfxPath, const glm::vec3& pos = glm::vec3(0.0f), const glm::quat& rot = glm::quat(1, 0, 0, 0));
    ParticleEffect createEffect(const ParticleEffectDesc& desc, const glm::vec3& pos = glm::vec3(0.0f), const glm::quat& rot = glm::quat(1, 0, 0, 0));

    // A PARTICLE STREAM: one .pfx look (its FIRST emitter) drawn along ANY NUMBER of lines, on a few emitter slots.
    // Call it every frame, for every line that flows (main thread, before update): particles spawn at `from`, fly to
    // `to` at the emitter's Speed and end there, `particlesPerSec` of them. A renderer emitter slot holds ONE place a
    // frame and there are MAX_PARTICLE_EMITTERS (256) for everything, so a slot per line does not scale. A stream
    // holds StreamSlots slots and MOVES them: each frame it serves the next StreamSlots lines of the frame's list
    // with what they are due since their last turn. A particle keeps its own position, velocity and life, so it
    // does not notice that its emitter slot is at another line a frame later. The slots are made at the first call
    // and released after ~10 s without one. The .pfx: `Rate 0`, no `Offset` / `Direction`; `Life` is set per line.
    void emitStream(const oc::string& pfxPath, const glm::vec3& from, const glm::vec3& to, float particlesPerSec);

    // PUFFS: one-shot bursts of one .pfx look (its FIRST emitter's `Burst` count) at points that ANY THREAD names -
    // with a HARD CAP, so a crowd of thousands cannot flood the pool. createPuffStream: main thread, once per look;
    // the id is stable (MaxPuffStreams looks; UINT32_MAX = none left). emitPuff: lock-free, from the parallel entity
    // pass too - the first MaxPuffRequests of a frame are kept, the rest are dropped. update then takes the ones
    // within PuffRange of the camera and draws at most PuffSlots of them, spread evenly over the kept list (so one
    // corner of a crowd does not take every slot). It moves PuffSlots emitter slots from point to point, as a stream.
    uint32 createPuffStream(const oc::string& pfxPath);
    void emitPuff(uint32 stream, const glm::vec3& pos);

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
        RenderParticleEmitter rendererSlot;
        uint16 textureIdx = 0xFFFF; // PARTICLE_TEX_NONE or resolved bindless index
        float rateAccum = 0.0f;
        uint32 volumeFillSpawned = 0; // weather volumes: particles of desc.count spawned so far (fills over frames)
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
    // emitStream's state per .pfx path (main thread only: no lock).
    static constexpr uint32 StreamSlots = 12;
    struct StreamLine { glm::vec3 from, to; float rate; };
    struct Stream
    {
        oc::string path;
        oc::shared_ptr<const ParticleEffectDesc> desc;
        oc::vector<EmitterInstance> slots;  // up to StreamSlots renderer emitters, all of the desc's first emitter
        oc::vector<StreamLine> lines;       // this frame's calls; cleared by update
        uint32 cursor = 0;                  // the first line the next update serves
        uint32 idleFrames = 0;              // frames without a call
    };
    oc::vector<Stream> m_streams;
    void updateStreams(float deltaSec);

    // createPuffStream / emitPuff. A fixed table: emitPuff reads it from worker threads, so it never moves.
    static constexpr uint32 MaxPuffStreams = 4, PuffSlots = 12, MaxPuffRequests = 128;
    static constexpr float PuffRange = 150.0f; // m from the camera
    struct PuffStream
    {
        oc::string path;
        oc::shared_ptr<const ParticleEffectDesc> desc;
        oc::vector<EmitterInstance> slots;
        glm::vec3 requests[MaxPuffRequests];
        oc::atomic<uint32> count{ 0 }; // this frame's emitPuff calls (may pass MaxPuffRequests: only the first are stored)
    };
    PuffStream m_puffStreams[MaxPuffStreams];
    uint32 m_numPuffStreams = 0;
    void updatePuffs(Renderer& renderer);

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

    // Parallel entity spawning: effect create/destroy (and the spawn-time setEmitting resolve) run
    // concurrently from spawn jobs - m_effects growth/erase and the caches serialize here. Held only
    // for short map/vector work: the .pfx load, texture loads and renderer slot creation all run
    // OUTSIDE it (no nesting, so a plain mutex). The pass-safe setters (setTransform/setVelocity)
    // stay lock-free - no create/destroy overlaps the parallel entity pass.
    std::mutex m_effectMutex;
    oc::vector<EffectInstance> m_effects;
    oc::vector<DecalInstance> m_decals;
    oc::unordered_map<oc::string, oc::shared_ptr<const ParticleEffectDesc>> m_effectCache;
    oc::unordered_map<oc::string, uint16> m_textureCache;
    uint64 m_nextEffectId = 1;
    uint32 m_nextDecalId = 1;
    uint32 m_rngState = 0x12345678;

    // The BUILT-IN ambient effects, each one camera-following volume .pfx instance created/destroyed in
    // update() by its "Particles/<Name>" toggle, with the "<Name> count / size / alpha" multipliers
    // applied on top of the .pfx (the emitter table is re-uploaded per frame, so they are live).
    struct BuiltinEffect
    {
        const ParticleBuiltinSettings& settings; // Globals::settings.particleSystem
        const char* path;
        ParticleEffect effect;
    };
    BuiltinEffect m_builtins[4] = {
        { Globals::settings.particleSystem.rain, "Effects/rain.pfx" },
        { Globals::settings.particleSystem.snow, "Effects/snow.pfx" },
        { Globals::settings.particleSystem.dust, "Effects/dust.pfx" },
        { Globals::settings.particleSystem.underwater, "Effects/underwater.pfx" },
    };
    // "Particles/Ocean spray": one Effects/ocean_spray.pfx instance (no CPU spawns) whose emitter slot the
    // renderer's ocean spray producer spawns into over the particle GPU spawn path.
    ParticleEffect m_oceanSprayEffect;
    // "Particles/River mist": the same for the renderer's river mist producer (rapids and falls; world scale).
    ParticleEffect m_riverMistEffect;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_GPU_EFFECTS)
    ParticleSystem particleSystem;
}
