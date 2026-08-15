module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Core.Camera;
import Core.Frustum;
import RendererVK;
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

static void collectUnits(std::span<const uint64> results, std::vector<Entity*>& out)
{
    out.clear();
    for (const uint64 user : results)
    {
        Entity* entity = reinterpret_cast<Entity*>(user);
        if (hasComponent<GameUnitComponent>(entity))
            out.push_back(entity);
    }
}

// Units inside the view frustum, for the overhead labels — anything off screen would be projected
// and thrown away, so it is never fetched. Transient by design: nothing holds the result.
void NpcSystem::queryVisibleUnits(const Camera& camera, std::vector<Entity*>& out)
{
    ProfileScope scope("Npc visible units (query)", EProfileCategory::Game);
    thread_local std::vector<uint64> results;
    const glm::dvec3 cameraPos(camera.position);
    // The renderer's current view-projection is the frustum this frame is drawn with; the spatial
    // index wants it camera-relative (exact at any world scale).
    const Frustum world(Globals::rendererVK.getCenterViewProj());
    Globals::spatialIndex.queryFrustum(rebaseFrustum(world, cameraPos), cameraPos, FLT_MAX,
        SpatialLayer_Render, results);
    collectUnits(results, out);
}

// Every unit in the world — ONLY for save/load, which must persist units the camera cannot see.
static void queryAllUnits(std::vector<Entity*>& out)
{
    thread_local std::vector<uint64> results;
    Globals::spatialIndex.querySphere(glm::dvec3(0.0), 1000.0f, SpatialLayer_Render, results);
    collectUnits(results, out);
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
    Tweak::floatVar("Game/Enemies", "Target search radius", &up.targetSearchRadius, 5.0f, 400.0f, 1.0f);
    // Shot speeds, applied when the fire queues are serviced here (the rest of the production
    // tuning registers from StructureSystem onto the component params).
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
        const GameUnitComponent* unit = getComponent<GameUnitComponent>(entity);
        if ((unit && !unit->puppet) || hasComponent<GameProjectileComponent>(entity))
            Globals::world.removeRootEntity(entity); // puppets are player capsules — never ours to despawn
    }
    // Drop any queued reports/requests the removed actors left behind, so stale deaths cannot
    // decrement (or stale requests spawn into) a world that has been reset (load/teardown paths).
    GameUnitComponent::takeFireRequests(m_fireScratch);
    GameUnitComponent::takeDeaths(m_deathScratch);
    GameStructureComponent::takeSpawnRequests(m_spawnScratch);
    GameStructureComponent::takeTurretFireRequests(m_turretFireScratch);
    m_fireScratch.clear();
    m_deathScratch.clear();
    m_spawnScratch.clear();
    m_turretFireScratch.clear();
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

void NpcSystem::service(StructureSystem& structures)
{
    ProfileScope scope("Npc service", EProfileCategory::Game);
    // Units the BARRACKS decided to produce during the pass (their component paid the minerals,
    // claimed the roster slot and set the cooldown — this only performs the entity spawn). A
    // failed spawn refunds the cost and the slot.
    GameStructureComponent::takeSpawnRequests(m_spawnScratch);
    for (const uint32 barracksId : m_spawnScratch)
    {
        const int index = structures.structureIndexById(barracksId);
        if (index < 0)
            continue; // the barracks died between deciding and servicing — its units died with it
        const StructureSystem::Ref& s = structures.structures()[index];
        const ENpcType unitType = s.type == EStructureType::BarracksBrute ? ENpcType::Brute
            : s.type == EStructureType::BarracksRunner ? ENpcType::Runner
            : s.type == EStructureType::BarracksSpitter ? ENpcType::Spitter : ENpcType::Grunt;
        if (!spawnUnit(structures, freeSpawnPointAround(structures, s.entity->pos, barracksSpawnRadius()),
            barracksId, (uint8)s.state->team, unitType))
        {
            s.state->store[2] = glm::min(s.state->store[2] + s.state->barracks.spawnCost,
                s.state->capacity[2]);
            s.state->barracks.aliveUnits = glm::max(s.state->barracks.aliveUnits - 1, 0);
        }
    }
    // Shots the TURRETS asked for (their component paid the energy and set the cooldown).
    GameStructureComponent::takeTurretFireRequests(m_turretFireScratch);
    for (const GameStructureComponent::TurretFireRequest& request : m_turretFireScratch)
    {
        glm::vec3 dir = request.target - request.from;
        const float len = glm::length(dir);
        if (len < 1e-3f)
            continue;
        dir /= len;
        fireShot("Entities/Game/projectile.pre", "Projectile", request.from + dir * 1.0f,
            dir * m_turretShotSpeed, request.team);
    }

    // Shots the RANGED units asked for during the pass (spawning is main-thread only).
    GameUnitComponent::takeFireRequests(m_fireScratch);
    for (const GameUnitComponent::FireRequest& request : m_fireScratch)
    {
        const glm::vec3 from = request.from + glm::vec3(0.0f, 0.8f, 0.0f);
        glm::vec3 dir = (request.target + glm::vec3(0.0f, 1.0f, 0.0f)) - from;
        const float len = glm::length(dir);
        if (len > 1e-3f)
            fireShot("Entities/Game/enemyShot.pre", "EnemyShot", from + dir / len * 1.2f,
                dir / len * m_spitterShotSpeed, request.team);
    }
    // Each reported death frees a slot on its spawner — the roster count is maintained by the
    // spawn/death edges instead of by recounting units every frame.
    GameUnitComponent::takeDeaths(m_deathScratch);
    for (const uint32 sourceId : m_deathScratch)
        if (GameStructureComponent* barracks = structures.structureStateById(sourceId))
            barracks->barracks.aliveUnits = glm::max(barracks->barracks.aliveUnits - 1, 0);
}

void NpcSystem::saveUnits(AssetNode& root) const
{
    std::vector<Entity*> units;
    queryAllUnits(units);
    for (Entity* entity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        if (!u || u->puppet)
            continue; // puppets are player capsules — players are never saved
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

void NpcSystem::loadUnits(const AssetNode& root, StructureSystem& structures)
{
    clear(); // despawn live units + projectiles (projectiles are transient, not saved)
    // Roster counts are maintained by the spawn/death edges, so a load has to re-seed them: the
    // structures were just rebuilt (all zero) and every unit below re-registers as it spawns.
    for (const StructureSystem::Ref& s : structures.structures())
        if (isBarracksType(s.type))
            s.state->barracks.aliveUnits = 0;
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
        if (GameStructureComponent* barracks = structures.structureStateById(source))
            ++barracks->barracks.aliveUnits;
    }
}
