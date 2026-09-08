module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;
import Spatial;

// See GameStructureComponent.ixx (and the shared contract in GameUnitComponent.ixx): update runs on
// the parallel entity pass on authority instances only; cross-entity writes are the atomic CAS
// helpers, cross-entity lookup is spatial.

GameStructureParams GameStructureComponent::params;

// Machine event queues (rare, tiny — the same discipline as the unit events).
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
    // ---- territory: HOSTILE = any OTHER team's bubble owns the structure's point (push a field
    // over their base to siege it). Health is the construction progress too, so a blueprint under
    // an enemy bubble literally un-builds.
    if (!invulnerable) // one bake tap (worker-safe), no GPU query slot
    {
        const ForceSystem::FieldSample territory = Globals::forceSystem.sampleBakedField(entity.pos, team);
        if (territory.valid && territory.inside && territory.owningTeam != (uint32)team)
            fieldDrain(params.fieldDamageRate * deltaSec);
    }

    // (Resource transport is the game's cable network job — see StructureSystem: this component
    // only owns its float stores; cells enter and leave them at the game's tick boundary.)

    // ---- machine logic (the union's stamped variant): the DECISION runs here per-entity, worker-
    // side, spending from the structure's OWN stores; the actual entity spawn rides an event queue
    // because spawning is main-thread only.
    if (machineKind == EMachineKind::Barracks && !blueprint)
    {
        entity.setProfiled(); // machine structures earn a per-entity profile scope (latched here —
                              // machineKind is stamped by the game AFTER spawn, so spawn can't know)
        BarracksData& b = barracks;
        // THE ENERGY STORE IS THE BUILD BAR: the game stamps capacity = the selected unit's
        // cost and caps the barracks' cable intake ("Barracks energy intake/s"), so the store
        // fills at the build rate and a unit is born the moment it is FULL — build time = cost /
        // intake, no timer. The epsilon covers a fill that lands a rounding step short of the cap.
        if (b.population + (int)b.spawnPop <= b.popCap && store[0] >= b.spawnCost - 0.01f)
        {
            store[0] = glm::max(store[0] - b.spawnCost, 0.0f); // the bar restarts (refunded on spawn fail)
            b.population += b.spawnPop;   // the unit's death event frees it again
            const std::lock_guard<std::mutex> lock(g_structureEventMutex);
            g_spawnRequests.push_back(structureId);
        }
    }
    else if (machineKind == EMachineKind::Medic && !blueprint && powered)
    {
        // MEDIC: one spatial query of the heal radius per station, own-team units inside get a
        // heal banked into their inbox (their own tick applies it). Never a roster walk.
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
        // THE ENERGY STORE IS THE RELOAD BAR (the barracks rule): the game stamps capacity = one
        // shot's energy and caps the turret's cable intake to shotEnergy / fireInterval, so a fed
        // turret fires at exactly the authored cadence and a starved one simply fires slower —
        // no timer. A full store with no target holds its charge and fires the moment one appears.
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
                    return; // puppets are player capsules — turrets target only units (known gap)
                const glm::vec3 d = other->pos - pos;
                if (glm::dot(d, d) < bestDistSq)
                {
                    bestDistSq = glm::dot(d, d);
                    target = other;
                }
            });
            if (target)
            {
                store[0] = glm::max(store[0] - capacity[0], 0.0f); // the bar restarts
                // HITSCAN lightning: the damage lands right here (damage() is atomic — the melee
                // sweep uses the same call from workers); only the BEAM visual is queued.
                if (GameUnitComponent* victim = getComponent<GameUnitComponent>(target))
                    victim->damage(params.turretDamage);
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
    // Only strainable structures are ever deposited on, and strainable is only set on emitters —
    // the `emitter` union variant is the active one by contract.
    if (energyPerSec > 0.0f)
        atomicAdd(emitter.unitLoad, energyPerSec);
}

// ---------------------------------------------------------------- spawn-info plumbing

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
