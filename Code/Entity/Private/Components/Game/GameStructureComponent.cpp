module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;
import Spatial;

GameStructureParams GameStructureComponent::params;

static std::mutex g_structureEventMutex;
static oc::vector<uint32> g_spawnRequests;
static oc::vector<GameStructureComponent::TurretFireRequest> g_turretFire;

static void atomicSubClamped(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    float next;
    do { next = glm::max(cur - amount, 0.0f); } while (!ref.compare_exchange_weak(cur, next));
}

static void atomicAdd(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    while (!ref.compare_exchange_weak(cur, cur + amount)) {}
}

void GameStructureComponent::takeSpawnRequests(oc::vector<uint32>& outStructureIds)
{
    const std::lock_guard<std::mutex> lock(g_structureEventMutex);
    outStructureIds.swap(g_spawnRequests);
    g_spawnRequests.clear();
}

void GameStructureComponent::takeTurretFireRequests(oc::vector<TurretFireRequest>& out)
{
    const std::lock_guard<std::mutex> lock(g_structureEventMutex);
    out.swap(g_turretFire);
    g_turretFire.clear();
}

void GameStructureComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    team = (uint8)info.team;
    health = healthMax = info.healthMax;
    invulnerable = info.invulnerable ? 1 : 0;
    alwaysDisplayHealth = info.alwaysDisplayHealth ? 1 : 0;
    alwaysShowResources = info.alwaysShowResources ? 1 : 0;
    meleeRadius = info.meleeRadius;
}

void GameStructureComponent::update(Entity& entity, float deltaSec)
{
    if (Globals::networkManager.role() == ENetRole::Client)
        return;
    // Any OTHER team's bubble over the point drains health — a blueprint under it un-builds.
    if (!invulnerable)
    {
        const ForceSystem::FieldSample territory = Globals::forceSystem.sampleBakedField(entity.pos, team);
        if (territory.valid && territory.inside && territory.owningTeam != (uint32)team)
            fieldDrain(params.fieldDamageRate * deltaSec);
    }

    if (machineKind == EMachineKind::Barracks && !blueprint)
    {
        entity.setProfiled(); // latched here: machineKind is stamped by the game AFTER spawn
        BarracksData& b = barracks;
        // The store fills at the capped cable intake; full = a unit. The epsilon covers a fill that
        // lands a rounding step short of the cap.
        if (b.population + (int)b.spawnPop <= b.popCap && store[0] >= b.spawnCost - 0.01f)
        {
            store[0] = glm::max(store[0] - b.spawnCost, 0.0f); // refunded by the game on spawn fail
            b.population += b.spawnPop;
            const std::lock_guard<std::mutex> lock(g_structureEventMutex);
            g_spawnRequests.push_back(structureId);
        }
    }
    else if (machineKind == EMachineKind::Medic && !blueprint && powered)
    {
        entity.setProfiled();
        const glm::vec3 pos = entity.pos;
        const float amount = params.medicHealRate * deltaSec;
        Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), params.medicRange, SpatialLayer_Render, [&](uint64 user)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameUnitComponent* u = getComponent<GameUnitComponent>(other);
            if (!u || u->puppet || u->team != team || !u->alive())
                return;
            const glm::vec3 d = other->pos - pos;
            if (d.x * d.x + d.z * d.z > params.medicRange * params.medicRange)
                return; // the sphere query is a broadphase on bounds
            u->heal(amount);
        });
    }
    else if (machineKind == EMachineKind::Turret && !blueprint)
    {
        entity.setProfiled();
        // The store is the reload bar: capacity = one shot, intake capped to shotEnergy / fireInterval.
        if (store[0] >= capacity[0] - 0.01f)
        {
            const glm::vec3 pos = entity.pos;
            Entity* target = nullptr;
            float bestDistSq = params.turretRange * params.turretRange;
            Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), params.turretRange,
                SpatialLayer_Render, [&](uint64 user)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                const GameUnitComponent* u = getComponent<GameUnitComponent>(other);
                if (!u || u->puppet || u->team == team || !u->alive())
                    return; // turrets never target player capsules (known gap)
                const glm::vec3 d = other->pos - pos;
                if (glm::dot(d, d) < bestDistSq)
                {
                    bestDistSq = glm::dot(d, d);
                    target = other;
                }
            });
            if (target)
            {
                store[0] = glm::max(store[0] - capacity[0], 0.0f);
                if (GameUnitComponent* victim = getComponent<GameUnitComponent>(target))
                    victim->damage(params.turretDamage, team);
                const std::lock_guard<std::mutex> lock(g_structureEventMutex);
                g_turretFire.push_back(TurretFireRequest{ pos + glm::vec3(0.0f, 1.0f, 0.0f),
                    target->pos, team });
            }
        }
    }
}

void GameStructureComponent::damage(float amount)
{
    if (invulnerable || amount <= 0.0f)
        return;
    atomicSubClamped(health, amount);
}

void GameStructureComponent::addLoad(float energyPerSec)
{
    // Only emitters are strainable, so `emitter` is the active variant by contract.
    if (energyPerSec > 0.0f)
        atomicAdd(emitter.unitLoad, energyPerSec);
}

const GameStructureComponent::SpawnInfo* getGameStructureSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<GameStructureComponent>(entity))
        return nullptr;
    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_GameStructure); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const GameStructureComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out)
{
    const GameStructureComponent::SpawnInfo d;
    if (info.team != d.team)                 out.set("Team", oc::to_string(info.team));
    if (info.healthMax != d.healthMax)       out.set("HealthMax", info.healthMax);
    if (info.invulnerable != d.invulnerable) out.set("Invulnerable", info.invulnerable);
    if (info.meleeRadius != d.meleeRadius)   out.set("MeleeRadius", info.meleeRadius);
    if (info.alwaysDisplayHealth != d.alwaysDisplayHealth) out.set("AlwaysDisplayHealth", info.alwaysDisplayHealth);
    if (info.alwaysShowResources != d.alwaysShowResources) out.set("AlwaysShowResources", info.alwaysShowResources);
}
