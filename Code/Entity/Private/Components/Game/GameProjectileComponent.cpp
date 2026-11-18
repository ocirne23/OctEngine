module Entity;

import Core;
import Core.glm;
import Core.Time; // the impact lights' age (real seconds)
import Core.Transform;
import :Entity;
import Force;
import Particle; // the impact burst (ParticleSystem::emitPuff)
import Physics;
import RendererVK; // the muzzle flash's and the impact's lights
import Spatial;

GameProjectileParams GameProjectileComponent::params;

namespace
{
    // The impact lights (GameProjectileComponent::drawImpactLights): the last c_maxImpacts impacts, by their real time.
    constexpr int c_maxImpacts = 32;
    constexpr float c_impactSeconds = 0.3f, c_impactRange = 10.8f, c_impactIntensity = 500.0f;
    struct ImpactLight { glm::vec3 pos{ 0.0f }; double born = -1.0; };
    ImpactLight g_impacts[c_maxImpacts];
    int g_impactNext = 0;
    std::mutex g_impactMutex; // (constant-initialized: safe at namespace scope)
}

void GameProjectileComponent::drawImpactLights()
{
    const double now = Globals::time.getElapsedSec();
    const std::lock_guard lock(g_impactMutex);
    for (const ImpactLight& impact : g_impacts)
    {
        const float age = (float)(now - impact.born);
        if (impact.born < 0.0 || age < 0.0f || age >= c_impactSeconds)
            continue;
        const float fade = 1.0f - age / c_impactSeconds;
        Globals::rendererVK.addLightInfo(PointLight(impact.pos + glm::vec3(0.0f, 1.0f, 0.0f), c_impactRange, glm::vec3(0.6f, 0.9f, 1.0f),
            c_impactIntensity * fade * fade));
    }
}

void GameProjectileComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    team = info.team;
    unitDamage = info.unitDamage;
    structureDamage = info.structureDamage;
    lifetime = info.lifetime;
    emitterDrain = info.emitterDrain;
    emitterDrainRadius = info.emitterDrainRadius;
    splashRadius = info.splashRadius;
    splashEdge = glm::clamp(info.splashEdge, 0.0f, 1.0f);
    hitsShadows = info.hitsShadows;
    impactPuff = info.impactPuff;
    muzzleFlash = info.muzzleFlash;
    fieldDeflect = glm::max(info.fieldDeflect, 0.0f);
    deflectDrain = glm::max(info.deflectDrain, 0.0f);
    origin = entity.pos;
    if (muzzleFlash) // the launch, as big as an impact: a few puffs of shards, the impact's flash and smoke where it starts (the light: update)
    {
        for (int k = 0; k < 3; ++k)
            Globals::particleSystem.emitPuff(params.muzzlePuff, entity.pos + glm::vec3(0.0f, 0.3f * (float)k, 0.0f));
        Globals::particleSystem.emitPuff(params.impactFlash, entity.pos);
        Globals::particleSystem.emitPuff(params.muzzleSmoke, entity.pos);
    }
    if (ForceComponent* fc = getComponent<ForceComponent>(&entity))
        fc->emitter.setTeam(team);
    // The shot's sound, on every role (a replicated shell spawns here too).
    GameUnitComponent::reportSound(splashRadius > 0.0f ? GameUnitComponent::ESoundCue::LobFired : GameUnitComponent::ESoundCue::ShotFired, entity.pos);
}

void GameProjectileComponent::destroy(Entity& entity, const SpawnInfo&)
{
    if (age > lifetime)
        return; // it ran out in the air: no impact
    GameUnitComponent::reportSound(splashRadius > 0.0f ? GameUnitComponent::ESoundCue::SplashImpact : GameUnitComponent::ESoundCue::ShotImpact,
        entity.pos, glm::max(splashRadius, 1.0f));
    if (impactPuff) // the burst: a few puffs of shards over the impact and one flash (capped by the particle system)
    {
        for (int k = 0; k < 3; ++k)
            Globals::particleSystem.emitPuff(params.impactPuff, entity.pos + glm::vec3(0.0f, 0.4f * (float)k, 0.0f));
        Globals::particleSystem.emitPuff(params.impactFlash, entity.pos);
        const std::lock_guard lock(g_impactMutex); // ... and its light (drawImpactLights)
        g_impacts[g_impactNext] = { entity.pos, Globals::time.getElapsedSec() };
        g_impactNext = (g_impactNext + 1) % c_maxImpacts;
    }
}

void GameProjectileComponent::update(Entity& entity, float deltaSec)
{
    age += deltaSec; // (every role: the muzzle light's time, and destroy's impact test)
    // THE MUZZLE FLASH's light: a point light where it started, bright at once and gone in c_flashSeconds. addLightInfo
    // is lock-free (the entity pass runs this on workers).
    constexpr float c_flashSeconds = 0.15f, c_flashRange = 12.0f, c_flashIntensity = 700.0f;
    if (muzzleFlash && age < c_flashSeconds)
    {
        const float fade = 1.0f - age / c_flashSeconds;
        Globals::rendererVK.addLightInfo(PointLight(origin, c_flashRange, glm::vec3(0.6f, 0.9f, 1.0f), c_flashIntensity * fade * fade));
    }
    if (Globals::networkManager.role() == ENetRole::Client)
        return; // replicated shots fly on the owner's spawn velocities; the server despawns them
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    const glm::vec3 pos = pc && pc->body.isValid() ? pc->body.getPosition() : entity.pos;
    if (spent || age > lifetime || pos.y < params.voidY)
    {
        if (!spent)
            Globals::scriptEvents.addDestroyRequest(EntityPtr(&entity));
        spent = true;
        return;
    }
    if (ForceComponent* fc = getComponent<ForceComponent>(&entity); fc && fc->emitter.isValid() && pc)
    {
        const glm::vec3 force = fc->emitter.getAppliedForce();
        const float pressure = fc->emitter.getPressure();
        if (glm::dot(force, force) > 1e-8f)
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                force * deltaSec * params.pushGain * pressure);
    }
    // FIELD DEFLECTION (a shot with no field of its own): an enemy bubble bends its flight out of it, sideways only - down
    // its planar field gradient, by the depth in the field (0 at the shell's outer fringe, full at the iso value). The
    // bake is one slice at one height, so a shell high over a small bubble reads it too: "slightly" is the authored rate.
    if (fieldDeflect > 0.0f && pc && pc->body.isValid())
    {
        const ForceSystem::FieldSample fs = Globals::forceSystem.sampleBakedField(pos, team);
        const glm::vec2 grad(fs.opposingGradient.x, fs.opposingGradient.z);
        const float gradLen = glm::length(grad);
        if (fs.valid && fs.opposing > 0.0f && gradLen > 1e-6f)
        {
            const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
            const glm::vec2 away = -grad / gradLen;
            const float depth = glm::smoothstep(0.0f, iso, fs.opposing);
            const float dv = fieldDeflect * depth * deltaSec;
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                glm::vec3(away.x, 0.0f, away.y) * (dv * pc->body.getMass()));
            // THE COST of bending it: "DeflectDrain" energy/s x the depth, from the battery of the NEAREST enemy unit whose
            // live bubble covers the shell (planar, its equilibrium radius; up to c_bubbleSearch away), through its
            // atomic inbox (drainShield: the entity pass runs this on workers).
            if (deflectDrain > 0.0f)
            {
                constexpr float c_bubbleSearch = 16.0f; // m: past the largest demon bubble (the Titan's ~13 m)
                GameUnitComponent* owner = nullptr;
                float bestSq = FLT_MAX;
                Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), c_bubbleSearch, SpatialLayer_Render, [&](uint64 user)
                {
                    Entity* other = reinterpret_cast<Entity*>(user);
                    GameUnitComponent* unit = getComponent<GameUnitComponent>(other);
                    const ForceComponent* force = getComponent<ForceComponent>(other);
                    if (!unit || !force || !force->emitter.isValid() || unit->team == team || unit->energy <= 0.0f)
                        return;
                    const glm::vec2 d(other->pos.x - pos.x, other->pos.z - pos.z);
                    const float distSq = glm::dot(d, d);
                    const float radius = force->emitter.getEquilibriumRadius();
                    if (distSq <= radius * radius && distSq < bestSq)
                    {
                        bestSq = distSq;
                        owner = unit;
                    }
                });
                if (owner)
                    owner->drainShield(deflectDrain * depth * deltaSec);
            }
        }
    }
    if (emitterDrain > 0.0f)
    {
        GameStructureComponent* strain = nullptr;
        float best = emitterDrainRadius;
        Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), emitterDrainRadius, SpatialLayer_Render, [&](uint64 user)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            if (!sc || !sc->strainable || sc->team == team)
                return;
            const float d = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(other->pos.x, other->pos.z));
            if (d < best)
            {
                best = d;
                strain = sc;
            }
        });
        if (strain)
            strain->addLoad(emitterDrain);
    }
}

void GameProjectileComponent::onContact(Entity& self, Entity* other, bool begin)
{
    if (!begin || spent || Globals::networkManager.role() == ENetRole::Client)
        return;
    if (splashRadius > 0.0f)
    {
        // Legal here: the contact dispatch runs after the frame's spatial commit.
        const float r2 = splashRadius * splashRadius;
        Globals::spatialIndex.forEachInSphere(glm::dvec3(self.pos), splashRadius, SpatialLayer_Render, [&](uint64 user)
        {
            Entity* victim = reinterpret_cast<Entity*>(user);
            const glm::vec3 d = victim->pos - self.pos;
            if (glm::dot(d, d) > r2)
                return;
            // THE FALLOFF: the full damage at the centre, splashEdge of it at the edge, linear between.
            const float share = glm::mix(1.0f, splashEdge, std::sqrt(glm::dot(d, d)) / splashRadius);
            if (GameUnitComponent* unit = getComponent<GameUnitComponent>(victim); unit && unit->team != team)
                unit->damage(unitDamage * share, team, hitsShadows);
            else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(victim); sc && sc->team != team)
                sc->damage(structureDamage * share);
        });
    }
    else if (!other)
    {
    }
    else if (GameUnitComponent* unit = getComponent<GameUnitComponent>(other); unit && unit->team != team)
        unit->damage(unitDamage, team, hitsShadows);
    else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(other); sc && sc->team != team)
        sc->damage(structureDamage);
    spent = true; // any contact spends the shot, own team and ground included
    Globals::scriptEvents.addDestroyRequest(EntityPtr(&self));
}

const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity)
{
    return getSpawnInfo<GameProjectileComponent>(entity);
}

void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out)
{
    const GameProjectileComponent::SpawnInfo d;
    if (info.team != d.team)                         out.set("Team", oc::to_string(info.team));
    if (info.unitDamage != d.unitDamage)             out.set("UnitDamage", info.unitDamage);
    if (info.structureDamage != d.structureDamage)   out.set("StructureDamage", info.structureDamage);
    if (info.lifetime != d.lifetime)                 out.set("Lifetime", info.lifetime);
    if (info.emitterDrain != d.emitterDrain)         out.set("EmitterDrain", info.emitterDrain);
    if (info.emitterDrainRadius != d.emitterDrainRadius) out.set("EmitterDrainRadius", info.emitterDrainRadius);
    if (info.splashRadius != d.splashRadius)         out.set("SplashRadius", info.splashRadius);
    if (info.splashEdge != d.splashEdge)             out.set("SplashEdge", info.splashEdge);
    if (info.hitsShadows)                            out.set("HitsShadows", true);
    if (info.impactPuff)                             out.set("ImpactPuff", true);
    if (info.muzzleFlash)                            out.set("MuzzleFlash", true);
    if (info.fieldDeflect != d.fieldDeflect)         out.set("FieldDeflect", info.fieldDeflect);
    if (info.deflectDrain != d.deflectDrain)         out.set("DeflectDrain", info.deflectDrain);
}
