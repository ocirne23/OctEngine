module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;
import Physics;
import Spatial;

GameProjectileParams GameProjectileComponent::params;

void GameProjectileComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    team = info.team;
    unitDamage = info.unitDamage;
    structureDamage = info.structureDamage;
    lifetime = info.lifetime;
    emitterDrain = info.emitterDrain;
    emitterDrainRadius = info.emitterDrainRadius;
    splashRadius = info.splashRadius;
    if (ForceComponent* fc = getComponent<ForceComponent>(&entity))
        fc->emitter.setTeam(team);
}

void GameProjectileComponent::update(Entity& entity, float deltaSec)
{
    if (Globals::networkManager.role() == ENetRole::Client)
        return; // replicated shots fly on the owner's spawn velocities; the server despawns them
    age += deltaSec;
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

void GameProjectileComponent::onContact(Entity& self, Entity& other, bool begin)
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
            if (GameUnitComponent* unit = getComponent<GameUnitComponent>(victim); unit && unit->team != team)
                unit->damage(unitDamage, team);
            else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(victim); sc && sc->team != team)
                sc->damage(structureDamage);
        });
    }
    else if (GameUnitComponent* unit = getComponent<GameUnitComponent>(&other); unit && unit->team != team)
        unit->damage(unitDamage, team);
    else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(&other); sc && sc->team != team)
        sc->damage(structureDamage);
    spent = true; // any contact spends the shot, own team and ground included
    Globals::scriptEvents.addDestroyRequest(EntityPtr(&self));
}

const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<GameProjectileComponent>(entity))
        return nullptr;
    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_GameProjectile); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const GameProjectileComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
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
}
