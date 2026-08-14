module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Entity;
import Physics;
import Force;
import Spatial;
import :Npc;
import :Structures;

// Per-type PREFABS: the stats live in each prefab's Component GameUnit block.
static constexpr const char* c_npcPrefabs[(int)ENpcType::Count] = {
    "Entities/Game/enemyUnit.pre", "Entities/Game/enemyBrute.pre",
    "Entities/Game/enemyRunner.pre", "Entities/Game/enemySpitter.pre" };
static constexpr const char* c_npcNames[(int)ENpcType::Count] = { "Enemy", "Brute", "Runner", "Spitter" };

// The one arena-wide unit query (corridor fits well inside the radius). Spatial queries are the
// ONLY lookup — nothing holds unit lists.
static void queryUnits(std::vector<Entity*>& out)
{
    out.clear();
    thread_local std::vector<uint64> results;
    Globals::spatialIndex.querySphere(glm::dvec3(0.0), 300.0f, SpatialLayer_Render, results);
    for (const uint64 user : results)
    {
        Entity* entity = reinterpret_cast<Entity*>(user);
        if (hasComponent<GameUnitComponent>(entity))
            out.push_back(entity);
    }
}

void NpcSystem::registerTweaks()
{
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    // The shared unit-sim baseline (GameUnitComponent::params — every unit of every team).
    GameUnitParams& up = GameUnitComponent::params;
    Tweak::floatVar("Game/Enemies", "Unit energy drain/s @ pressure 1", &up.energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Push tension", &up.tension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field damage/s", &up.fieldDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Unit damage radius", &up.damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &up.pushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Enemies", "Retarget interval", &up.retargetInterval, 1.0f, 60.0f, 0.5f);
    // Production
    Tweak::intVar("Game/Friendlies", "Barracks unit limit", &m_barracksUnitLimit, 0, 16, 1);
    Tweak::floatVar("Game/Friendlies", "Barracks spawn interval", &m_barracksSpawnInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Grunt spawn materials", &m_spawnMaterials[(int)ENpcType::Grunt], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Brute spawn materials", &m_spawnMaterials[(int)ENpcType::Brute], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Runner spawn materials", &m_spawnMaterials[(int)ENpcType::Runner], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Spitter spawn materials", &m_spawnMaterials[(int)ENpcType::Spitter], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret range", &m_turretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret fire interval", &m_turretFireInterval, 0.1f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Turret shot energy", &m_turretShotEnergy, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Turret shot speed", &m_turretShotSpeed, 5.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spitter shot speed", &m_spitterShotSpeed, 2.0f, 80.0f, 0.5f);
}

void NpcSystem::clear()
{
    // Teardown: despawn every game actor entity (units + projectiles) — they are world roots.
    thread_local std::vector<uint64> results;
    Globals::spatialIndex.querySphere(glm::dvec3(0.0), 1000.0f, SpatialLayer_Render, results);
    for (const uint64 user : results)
    {
        Entity* entity = reinterpret_cast<Entity*>(user);
        if (hasComponent<GameUnitComponent>(entity) || hasComponent<GameProjectileComponent>(entity))
            Globals::world.removeRootEntity(entity);
    }
    m_unitScratch.clear();
}

// A unit spawn point on a ring around a building, on the first of 8 probed angles whose cell is
// free of structures (and inside the arena bounds) — units must never spawn INSIDE the building.
static glm::vec3 freeSpawnPointAround(const StructureSystem& structures, const glm::vec3& center,
    float ringRadius)
{
    const float start = glm::linearRand(0.0f, glm::two_pi<float>());
    for (int k = 0; k < 8; ++k)
    {
        const float a = start + (float)k * glm::two_pi<float>() / 8.0f;
        const glm::vec3 p(center.x + std::cos(a) * ringRadius, 1.0f, center.z + std::sin(a) * ringRadius);
        if (structures.cellsFree(EStructureType::Emitter, p)) // 1x1 probe
            return p;
    }
    return glm::vec3(center.x + std::cos(start) * ringRadius, 1.0f, center.z + std::sin(start) * ringRadius);
}

static float barracksSpawnRadius()
{
    return StructureSystem::footprintCellsOf(EStructureType::Barracks)
        * StructureSystem::GridCellSize * 0.5f + 1.5f;
}

Entity* NpcSystem::spawnUnit(const StructureSystem& structures, const glm::vec3& pos,
    uint32 sourceId, uint8 team, ENpcType type)
{
    EntityPtr entity = Globals::world.spawnAssetFile(c_npcPrefabs[(int)type], Transform(pos), true);
    if (!entity)
        return nullptr;
    GameUnitComponent* unit = getComponent<GameUnitComponent>(entity.get());
    if (!unit)
        return nullptr; // the prefab must carry Component GameUnit
    entity->setName(c_npcNames[(int)type]);
    Globals::world.addRootEntity(entity);
    unit->team = team;
    unit->sourceId = sourceId;
    if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 1 — units carry their builder's team
    // Copy the barracks route in AT SPAWN (orders tier): the unit marches it before its AI.
    if (const int source = structures.structureIndexById(sourceId); source >= 0)
    {
        const std::span<const glm::vec3> route = structures.structureRoute(source);
        unit->routeCount = (uint8)glm::min(route.size(), (size_t)GameUnitComponent::MaxRoutePoints);
        for (int i = 0; i < unit->routeCount; ++i)
            unit->route[i] = route[i];
        unit->routeIndex = 0;
    }
    return entity.get();
}

void NpcSystem::fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
    const glm::vec3& velocity, uint8 team)
{
    EntityPtr shot = Globals::world.spawnAssetFile(prefabPath, Transform(from), true);
    if (!shot)
        return;
    shot->setName(name);
    Globals::world.addRootEntity(shot);
    if (GameProjectileComponent* proj = getComponent<GameProjectileComponent>(shot.get()))
    {
        proj->team = team;
        if (ForceComponent* fc = getComponent<ForceComponent>(shot.get()))
            fc->emitter.setTeam(team);
    }
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(shot.get()))
        pc->body.setLinearVelocity(velocity); // main thread pre-physics: direct setter sanctioned
}

void NpcSystem::tickBarracks(StructureSystem& structures, float deltaSec)
{
    // Alive counts per barracks: zero every barracks' tally, then count this frame's units onto
    // their spawner (state lives ON the structures — no maps, and it dies with them).
    for (const StructureSystem::Ref& s : structures.structures())
        if (isBarracksType(s.type))
            s.state->barracks.aliveUnits = 0;
    for (Entity* entity : m_unitScratch)
        if (const GameUnitComponent* u = getComponent<GameUnitComponent>(entity); u && u->sourceId != 0)
        {
            const int source = structures.structureIndexById(u->sourceId);
            if (source >= 0 && isBarracksType(structures.structureType(source)))
                ++structures.structures()[source].state->barracks.aliveUnits;
        }

    const auto unitOfBarracks = [](EStructureType t)
    {
        return t == EStructureType::BarracksBrute ? ENpcType::Brute
             : t == EStructureType::BarracksRunner ? ENpcType::Runner
             : t == EStructureType::BarracksSpitter ? ENpcType::Spitter : ENpcType::Grunt;
    };
    for (const StructureSystem::Ref& s : structures.structures())
    {
        if (!isBarracksType(s.type) || s.state->blueprint)
            continue;
        const uint32 id = s.state->structureId;
        GameStructureComponent::BarracksData& data = s.state->barracks;
        // idle Constructors in range accelerate the spawn clock (boost 1 = double speed)
        data.spawnTimer = glm::max(0.0f, data.spawnTimer - deltaSec * (1.0f + data.boost));
        if (data.spawnTimer > 0.0f)
            continue;
        if (data.aliveUnits >= m_barracksUnitLimit)
            continue; // full roster: retry the moment a slot frees (timer stays 0)
        const ENpcType unitType = unitOfBarracks(s.type);
        if (!structures.trySpendStoredMinerals(id, m_spawnMaterials[(int)unitType]))
            continue; // each unit type has its own material price
        spawnUnit(structures, freeSpawnPointAround(structures, s.entity->pos, barracksSpawnRadius()),
            id, (uint8)s.state->team, unitType);
        data.spawnTimer = m_barracksSpawnInterval;
    }
}

void NpcSystem::tickTurrets(StructureSystem& structures, float deltaSec)
{
    for (const StructureSystem::Ref& s : structures.structures())
    {
        if (s.type != EStructureType::Turret || s.state->blueprint)
            continue;
        s.state->turret.fireTimer = glm::max(0.0f, s.state->turret.fireTimer - deltaSec);
        if (s.state->turret.fireTimer > 0.0f)
            continue;
        // Nearest ENEMY-TEAM unit in range (over this frame's query); no target = no cooldown
        // reset, fires the moment one appears.
        const glm::vec3 turretPos = s.entity->pos;
        const uint32 turretTeam = s.state->team;
        Entity* target = nullptr;
        float bestDistSq = m_turretRange * m_turretRange;
        for (Entity* entity : m_unitScratch)
        {
            const GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
            if (!u || u->team == turretTeam || !u->alive())
                continue;
            const glm::vec3 d = entity->pos - turretPos;
            if (glm::dot(d, d) < bestDistSq)
            {
                bestDistSq = glm::dot(d, d);
                target = entity;
            }
        }
        if (!target || !structures.trySpendEnergy(s.state->structureId, m_turretShotEnergy))
            continue;
        const glm::vec3 muzzle = turretPos + glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 dir = target->pos - muzzle;
        const float len = glm::length(dir);
        if (len < 1e-3f)
            continue;
        dir /= len;
        fireShot("Entities/Game/projectile.pre", "Projectile", muzzle + dir * 1.0f,
            dir * m_turretShotSpeed, (uint8)turretTeam);
        s.state->turret.fireTimer = m_turretFireInterval;
    }
}

void NpcSystem::tickUnits(std::span<const PlayerInfo> players, std::span<float> outPlayerDamage,
    StructureSystem& structures, float deltaSec)
{
    queryUnits(m_unitScratch); // cached for the frame (barracks counts, turrets, labels, mirror)
    for (Entity* entity : m_unitScratch)
    {
        GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        if (!u)
            continue;
        if (u->collapseEdge)
        {
            u->collapseEdge = false;
            m_shieldCollapseEdge = true; // the shield mirror flushes this tick
        }
        // Spitter fire requests raised on the worker pass — serviced here (spawns are main-thread).
        if (u->wantsFire)
        {
            u->wantsFire = false;
            const glm::vec3 from = entity->pos + glm::vec3(0.0f, 0.8f, 0.0f);
            glm::vec3 dir = (u->firePos + glm::vec3(0.0f, 1.0f, 0.0f)) - from;
            const float len = glm::length(dir);
            if (len > 1e-3f)
                fireShot("Entities/Game/enemyShot.pre", "EnemyShot", from + dir / len * 1.2f,
                    dir / len * m_spitterShotSpeed, (uint8)u->team);
        }
        // DIRECT melee damage to enemy-team players in reach — routed to each player's owning
        // instance by the caller (health is owner-computed).
        if (u->playerDps > 0.0f)
            for (size_t p = 0; p < players.size() && p < outPlayerDamage.size(); ++p)
            {
                if (players[p].team == u->team)
                    continue;
                const glm::vec2 toPlayer(players[p].pos.x - entity->pos.x, players[p].pos.z - entity->pos.z);
                const float reach = u->attackRange + 0.8f; // capsule body allowance
                if (glm::dot(toPlayer, toPlayer) < reach * reach
                    && glm::abs(players[p].pos.y - entity->pos.y) < 3.0f)
                    outPlayerDamage[p] += u->playerDps * deltaSec;
            }
    }
}

void NpcSystem::collectShieldStates(std::vector<ShieldNetState>& out) const
{
    for (Entity* entity : m_unitScratch)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        const NetworkComponent* net = getComponent<NetworkComponent>(entity);
        if (!u || !net || net->netId == 0)
            continue;
        const ForceComponent* fc = getComponent<ForceComponent>(entity);
        ShieldNetState s;
        s.netId = net->netId;
        s.healthFrac = glm::clamp(u->health / glm::max(u->healthMax, 1e-3f), 0.0f, 1.0f);
        s.energyFrac = glm::clamp(u->energy / glm::max(u->energyMax, 1e-3f), 0.0f, 1.0f);
        s.output = fc ? fc->emitter.getOutput() : 0.0f;
        s.collapsed = u->collapsed;
        s.kind = 0;
        s.team = (uint8)u->team;
        out.push_back(s);
    }
}

void NpcSystem::saveUnits(AssetNode& root) const
{
    std::vector<Entity*> units;
    queryUnits(units);
    for (Entity* entity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        if (!u)
            continue;
        int type = (int)ENpcType::Grunt; // the prefab variant, recovered from the entity name
        for (int t = 0; t < (int)ENpcType::Count; ++t)
            if (std::string_view(entity->getName()) == c_npcNames[t])
                type = t;
        AssetNode& n = root.addChild("Unit");
        n.set("Type", std::to_string(type));
        n.set("Team", std::to_string((int)u->team));
        n.set("Position", entity->pos);
        n.set("Health", u->health);
        n.set("Energy", u->energy);
        n.set("Source", std::to_string(u->sourceId));
        n.set("RouteIndex", std::to_string(u->routeIndex));
    }
}

void NpcSystem::loadUnits(const AssetNode& root, const StructureSystem& structures)
{
    clear(); // despawn live units + projectiles (projectiles are transient, not saved)
    for (const AssetNode* n : root.findAll("Unit"))
    {
        const int typeInt = glm::clamp(n->find("Type") ? n->find("Type")->asInt() : 0,
            0, (int)ENpcType::Count - 1);
        const uint8 team = (uint8)glm::clamp(n->find("Team") ? n->find("Team")->asInt() : 1,
            0, GameMaxTeams - 1);
        const glm::vec3 pos = n->find("Position") ? n->find("Position")->asVec3() : glm::vec3(0.0f, 1.0f, 0.0f);
        const uint32 source = n->find("Source") ? (uint32)n->find("Source")->asInt() : 0;
        Entity* entity = spawnUnit(structures, pos, source, team, (ENpcType)typeInt);
        if (!entity)
            continue;
        GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        u->health = glm::clamp(n->find("Health") ? n->find("Health")->asFloat() : u->health, 1.0f, u->healthMax);
        u->energy = glm::clamp(n->find("Energy") ? n->find("Energy")->asFloat() : u->energy, 0.0f, u->energyMax);
        u->routeIndex = (uint8)glm::clamp(n->find("RouteIndex") ? n->find("RouteIndex")->asInt() : 0, 0, 255);
    }
}
