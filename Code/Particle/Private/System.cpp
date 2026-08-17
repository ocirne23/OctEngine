module Particle;

import Core;
import Core.glm;
import RendererVK;
import :Effect;
import :System;

using namespace RendererVKLayout;

// ---- small math helpers (no gtx dependency) ----

static glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to)
{
    const float d = glm::dot(from, to);
    if (d > 0.99999f)
        return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f)
    {
        glm::vec3 axis = glm::cross(from, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < 1e-6f)
            axis = glm::cross(from, glm::vec3(0, 1, 0));
        axis = glm::normalize(axis);
        return glm::quat(0.0f, axis.x, axis.y, axis.z); // 180 degrees
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + d, axis.x, axis.y, axis.z));
}

static glm::quat axisAngle(const glm::vec3& axis, float angle)
{
    const float half = angle * 0.5f;
    const float s = std::sin(half);
    return glm::quat(std::cos(half), axis.x * s, axis.y * s, axis.z * s);
}

static glm::vec4 quatToVec4(const glm::quat& q) { return glm::vec4(q.x, q.y, q.z, q.w); }

// ---- ParticleEffect handle ----

ParticleEffect& ParticleEffect::operator=(ParticleEffect&& move) noexcept
{
    if (this != &move)
    {
        destroy();
        m_id = move.m_id;
        move.m_id = 0;
    }
    return *this;
}

void ParticleEffect::destroy()
{
    if (m_id != 0)
    {
        Globals::particleSystem.destroyEffect(m_id);
        m_id = 0;
    }
}

void ParticleEffect::setTransform(const glm::vec3& pos, const glm::quat& rot)
{
    if (ParticleSystem::EffectInstance* inst = Globals::particleSystem.findEffect(m_id))
    {
        inst->pos = pos;
        inst->rot = rot;
    }
}

void ParticleEffect::setVelocity(const glm::vec3& velocity)
{
    if (ParticleSystem::EffectInstance* inst = Globals::particleSystem.findEffect(m_id))
        inst->velocity = velocity;
}

void ParticleEffect::setEmitting(bool emitting)
{
    if (ParticleSystem::EffectInstance* inst = Globals::particleSystem.findEffect(m_id))
        inst->emitting = emitting;
}

bool ParticleEffect::isEmitting() const
{
    const ParticleSystem::EffectInstance* inst = Globals::particleSystem.findEffect(m_id);
    return inst != nullptr && inst->emitting;
}

void ParticleEffect::burst()
{
    if (ParticleSystem::EffectInstance* inst = Globals::particleSystem.findEffect(m_id))
        inst->pendingBurst = true;
}

// ---- ParticleSystem ----

void ParticleSystem::initialize()
{
}

uint16 ParticleSystem::getTexture(const oc::string& path, bool sRGB)
{
    if (path.empty())
        return (uint16)PARTICLE_TEX_NONE;
    if (auto it = m_textureCache.find(path); it != m_textureCache.end())
        return it->second;
    const uint16 idx = Globals::rendererVK.loadEffectTexture(path.c_str(), sRGB);
    m_textureCache.emplace(path, idx);
    return idx;
}

const ParticleEffectDesc* ParticleSystem::getEffectDesc(const oc::string& path)
{
    if (auto it = m_effectCache.find(path); it != m_effectCache.end())
        return it->second.get();
    auto desc = oc::make_shared<ParticleEffectDesc>();
    oc::string error;
    if (!loadParticleEffect(path, *desc, error))
    {
        printf("ParticleSystem: failed to load %s: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    const ParticleEffectDesc* result = desc.get();
    m_effectCache.emplace(path, oc::move(desc));
    return result;
}

void ParticleSystem::invalidateEffect(const oc::string& path)
{
    m_effectCache.erase(path); // live instances keep their shared_ptr desc
}

ParticleEffect ParticleSystem::createEffect(const oc::string& pfxPath, const glm::vec3& pos, const glm::quat& rot)
{
    getEffectDesc(pfxPath); // populate the cache
    if (auto it = m_effectCache.find(pfxPath); it != m_effectCache.end())
        return createEffectInstance(it->second, pos, rot);
    return ParticleEffect();
}

ParticleEffect ParticleSystem::createEffect(const ParticleEffectDesc& desc, const glm::vec3& pos, const glm::quat& rot)
{
    return createEffectInstance(oc::make_shared<const ParticleEffectDesc>(desc), pos, rot);
}

ParticleEffect ParticleSystem::createEffectInstance(oc::shared_ptr<const ParticleEffectDesc> desc, const glm::vec3& pos, const glm::quat& rot)
{
    EffectInstance inst;
    inst.pos = pos;
    inst.rot = rot;
    inst.desc = oc::move(desc);
    inst.emitters.reserve(inst.desc->emitters.size());
    for (const ParticleEmitterDesc& e : inst.desc->emitters)
    {
        EmitterInstance emitter;
        emitter.textureIdx = getTexture(e.texturePath, e.textureSRGB);
        emitter.rendererSlot = Globals::rendererVK.createParticleEmitter(e.toGpu(emitter.textureIdx));
        if (emitter.rendererSlot == UINT32_MAX)
        {
            printf("ParticleSystem: out of emitter slots for effect '%s'\n", inst.desc->name.c_str());
            continue;
        }
        inst.emitters.push_back(emitter);
    }
    if (inst.emitters.empty())
        return ParticleEffect();
    inst.id = m_nextEffectId++;
    m_effects.push_back(oc::move(inst));
    return ParticleEffect(m_effects.back().id);
}

ParticleSystem::EffectInstance* ParticleSystem::findEffect(uint64 id)
{
    for (EffectInstance& inst : m_effects)
        if (inst.id == id)
            return &inst;
    return nullptr;
}

void ParticleSystem::destroyEffect(uint64 id)
{
    for (size_t i = 0; i < m_effects.size(); ++i)
    {
        if (m_effects[i].id == id)
        {
            for (const EmitterInstance& emitter : m_effects[i].emitters)
                Globals::rendererVK.destroyParticleEmitter(emitter.rendererSlot);
            m_effects.erase(m_effects.begin() + i);
            return;
        }
    }
}

void ParticleSystem::update(Renderer& renderer, float deltaSec)
{
    ProfileScope profileScope("Particles", EProfileCategory::Particle);
    // Effects: refresh every emitter slot's GPU config from its desc + instance transform (so live
    // .pfx edits and moving emitters both just work) and turn rates/bursts into spawn requests.
    for (EffectInstance& inst : m_effects)
    {
        for (size_t i = 0; i < inst.emitters.size() && i < inst.desc->emitters.size(); ++i)
        {
            const ParticleEmitterDesc& desc = inst.desc->emitters[i];
            EmitterInstance& emitter = inst.emitters[i];

            ParticleEmitterGpu gpu = desc.toGpu(emitter.textureIdx);
            const glm::vec3 worldPos = inst.pos + inst.rot * desc.localOffset;
            gpu.posSpawnRadius = glm::vec4(worldPos, gpu.posSpawnRadius.w);
            const glm::vec3 dir = glm::dot(desc.localDirection, desc.localDirection) > 1e-6f
                ? glm::normalize(desc.localDirection) : glm::vec3(0, 1, 0);
            gpu.rotation = quatToVec4(inst.rot * rotationBetween(glm::vec3(0, 1, 0), dir));
            gpu.velocityInherit = glm::vec4(inst.velocity, desc.inheritVelocity);
            renderer.updateParticleEmitter(emitter.rendererSlot, gpu);

            uint32 spawnCount = 0;
            if (inst.emitting && desc.rate > 0.0f)
            {
                emitter.rateAccum += desc.rate * deltaSec;
                spawnCount = (uint32)emitter.rateAccum;
                emitter.rateAccum -= (float)spawnCount;
            }
            if (inst.pendingBurst)
                spawnCount += desc.burst;
            if (spawnCount > 0)
                renderer.emitParticles(emitter.rendererSlot, spawnCount);
        }
        inst.pendingBurst = false;
    }

    // Decals: age, fade, submit the live set (per-frame push, like lights).
    for (size_t i = 0; i < m_decals.size();)
    {
        DecalInstance& decal = m_decals[i];
        decal.age += deltaSec;
        float opacity = 1.0f;
        if (decal.lifetime > 0.0f)
        {
            if (decal.age >= decal.lifetime)
            {
                m_decals.erase(m_decals.begin() + i);
                continue;
            }
            opacity = oc::min(1.0f, (decal.lifetime - decal.age) / oc::max(decal.fadeOutTime, 1e-3f));
        }
        RendererVKLayout::DecalInfo info = decal.info;
        info.opacity *= opacity;
        renderer.addDecal(info);
        ++i;
    }
}

uint32 ParticleSystem::spawnDecal(const DecalDesc& desc, const glm::vec3& pos, const glm::vec3& normal)
{
    if (m_decals.size() >= MAX_DECALS)
    {
        // Evict the oldest non-persistent decal; refuse only if everything is persistent.
        size_t evict = SIZE_MAX;
        for (size_t i = 0; i < m_decals.size(); ++i)
            if (m_decals[i].lifetime > 0.0f && (evict == SIZE_MAX || m_decals[i].age > m_decals[evict].age))
                evict = i;
        if (evict == SIZE_MAX)
            return 0;
        m_decals.erase(m_decals.begin() + evict);
    }

    const glm::vec3 n = glm::dot(normal, normal) > 1e-6f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
    glm::quat rot = rotationBetween(glm::vec3(0, 0, 1), n);
    if (desc.randomRotation)
    {
        m_rngState = m_rngState * 747796405u + 2891336453u;
        const float roll = (float)(m_rngState >> 8) * (1.0f / 16777216.0f) * 6.2831853f;
        rot = rot * axisAngle(glm::vec3(0, 0, 1), roll);
    }

    DecalInstance decal;
    decal.id = m_nextDecalId++;
    decal.lifetime = desc.lifetime;
    decal.fadeOutTime = desc.fadeOutTime;
    decal.info.pos = pos;
    decal.info.opacity = 1.0f;
    decal.info.rotation = quatToVec4(rot);
    decal.info.halfExtents = glm::vec3(desc.size * 0.5f, oc::max(desc.depth, 0.01f));
    decal.info.angleFadeCos = std::cos(glm::radians(desc.angleFadeDeg));
    decal.info.tint = desc.tint;
    decal.info.emissive = desc.emissive;
    decal.info.normalFadeWidth = desc.angleFadeWidth;
    decal.info.params = glm::uvec4(getTexture(desc.texturePath, desc.textureSRGB),
        desc.lit ? DECAL_FLAG_LIT : 0u, 0u, 0u);
    m_decals.push_back(decal);
    return decal.id;
}

void ParticleSystem::removeDecal(uint32 id)
{
    for (DecalInstance& decal : m_decals)
    {
        if (decal.id == id)
        {
            // Turn it finite so it fades out from here.
            decal.lifetime = decal.age + oc::max(decal.fadeOutTime, 1e-3f);
            return;
        }
    }
}

void ParticleSystem::clearDecals()
{
    m_decals.clear();
}
