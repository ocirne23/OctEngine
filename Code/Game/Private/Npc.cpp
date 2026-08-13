module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Entity;
import Physics;
import Force;
import :Npc;
import :Structures;

// Multipliers over the "Unit *" tweak baseline.
static constexpr NpcTypeDef c_npcTypes[(int)ENpcType::Count] = {
    //  name      prefab                            hp    en    spd   dps   drain out   retgt
    { "Enemy",   "Entities/Game/enemyUnit.pre",    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { "Brute",   "Entities/Game/enemyBrute.pre",   3.0f, 2.0f, 0.6f, 2.0f, 1.5f, 1.3f, 1.5f },
    { "Runner",  "Entities/Game/enemyRunner.pre",  0.5f, 0.4f, 1.9f, 0.7f, 0.5f, 0.7f, 0.5f },
    { "Spitter", "Entities/Game/enemySpitter.pre", 0.8f, 0.8f, 0.9f, 0.0f, 0.5f, 0.8f, 1.0f },
};

float NpcSystem::unitHealthMax(int index) const
{
    return m_unitHealth * c_npcTypes[(int)m_units[index].type].health;
}

float NpcSystem::unitEnergyMax(int index) const
{
    return m_unitEnergy * c_npcTypes[(int)m_units[index].type].energy;
}

void NpcSystem::registerTweaks()
{
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::floatVar("Game/Enemies", "Unit health", &m_unitHealth, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Move speed", &m_moveSpeed, 0.5f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Accel", &m_accel, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Attack range", &m_attackRange, 0.5f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Enemies", "Attack dps", &m_attackDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Player attack dps", &m_playerAttackDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Retarget interval", &m_retargetInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Projectile damage", &m_projectileDamage, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &m_fieldPushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Enemies", "Push tension", &m_fieldTension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain/s per unit", &m_emitterDrainPerUnit, 0.0f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Enemies", "Emitter drain radius", &m_emitterDrainRadius, 1.0f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Unit energy", &m_unitEnergy, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Unit energy drain/s @ pressure 1", &m_unitEnergyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Unit shield output", &m_unitShieldOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Unit damage radius", &m_unitDamageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field damage/s", &m_unitFieldDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spitter range", &m_spitterRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spitter fire interval", &m_spitterFireInterval, 0.5f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Enemies", "Spitter shot damage", &m_spitterShotDamage, 0.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Spitter shot speed", &m_spitterShotSpeed, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Shot emitter drain/s", &m_shotEmitterDrain, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Shot drain radius", &m_shotEmitterDrainRadius, 1.0f, 20.0f, 0.25f);
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
}

void NpcSystem::clear()
{
    for (Unit& u : m_units)
        if (u.entity)
            Globals::world.removeRootEntity(u.entity.get());
    m_units.clear();
    for (Shot& s : m_shots)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    m_shots.clear();
    for (Shot& s : m_turretShots)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    m_turretShots.clear();
    m_pendingShotHits.clear();
    m_barracksTimers.clear();
    m_turretTimers.clear();
}

void NpcSystem::tickTurrets(StructureSystem& structures, float deltaSec)
{
    for (int i = 0; i < structures.structureCount(); ++i)
    {
        if (structures.structureType(i) != EStructureType::Turret || structures.structureBlueprint(i))
            continue;
        const uint32 id = structures.structureId(i);
        float& timer = m_turretTimers[id];
        timer = glm::max(0.0f, timer - deltaSec);
        if (timer > 0.0f)
            continue;
        // Nearest ENEMY-TEAM unit in range; no target = no cooldown reset, fires the moment one
        // appears.
        const glm::vec3 turretPos = structures.structurePos(i);
        const uint8 turretTeam = structures.structureTeam(i);
        int target = -1;
        float bestDistSq = m_turretRange * m_turretRange;
        for (int e = 0; e < (int)m_units.size(); ++e)
        {
            if (m_units[e].team == turretTeam)
                continue;
            const glm::vec3 d = unitPos(e) - turretPos;
            const float distSq = glm::dot(d, d);
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                target = e;
            }
        }
        if (target < 0 || !structures.trySpendEnergy(id, m_turretShotEnergy))
            continue;
        glm::vec3 dir = unitPos(target) - (turretPos + glm::vec3(0.0f, 1.0f, 0.0f));
        const float len = glm::length(dir);
        if (len < 1e-3f)
            continue;
        dir /= len;
        Shot shot;
        shot.entity = Globals::world.spawnAssetFile("Entities/Game/projectile.pre",
            Transform(turretPos + glm::vec3(0.0f, 1.0f, 0.0f) + dir * 1.0f), true);
        if (!shot.entity)
            continue;
        shot.entity->setName("Projectile"); // the name IS the unit contact-damage hook
        if (ForceComponent* fc = getComponent<ForceComponent>(shot.entity.get()))
            fc->emitter.setTeam(turretTeam); // the hooks team-filter by the projectile's field team
        if (PhysicsComponent* pc = getComponent<PhysicsComponent>(shot.entity.get()))
            pc->body.setLinearVelocity(dir * m_turretShotSpeed);
        Globals::world.addRootEntity(shot.entity);
        m_turretShots.push_back(std::move(shot));
        timer = m_turretFireInterval;
    }

    // Age turret shots out; their small team-0 bubbles ride enemy fields like player shots.
    for (size_t i = 0; i < m_turretShots.size();)
    {
        Shot& s = m_turretShots[i];
        s.age += deltaSec;
        PhysicsComponent* pc = s.entity ? getComponent<PhysicsComponent>(s.entity.get()) : nullptr;
        if (!pc || !pc->body.isValid() || s.age > m_spitterShotLifetime
            || pc->body.getPosition().y < -20.0f)
        {
            if (s.entity)
                Globals::world.removeRootEntity(s.entity.get());
            m_turretShots.erase(m_turretShots.begin() + i);
            continue;
        }
        if (const ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        {
            const glm::vec3 force = fc->emitter.getAppliedForce();
            const float pressure = fc->emitter.getPressure();
            if (glm::dot(force, force) > 1e-8f)
                pc->body.applyImpulse(force * deltaSec * m_fieldPushGain * pressure);
        }
        ++i;
    }

    std::erase_if(m_turretTimers, [&](const auto& kv) {
        return structures.structureIndexById(kv.first) < 0; });
}

void NpcSystem::fireSpitterShot(const glm::vec3& from, const glm::vec3& at, uint8 team)
{
    glm::vec3 dir = (at + glm::vec3(0.0f, 1.0f, 0.0f)) - (from + glm::vec3(0.0f, 0.8f, 0.0f));
    const float len = glm::length(dir);
    if (len < 1e-3f)
        return;
    dir /= len;
    Shot shot;
    shot.entity = Globals::world.spawnAssetFile("Entities/Game/enemyShot.pre",
        Transform(from + glm::vec3(0.0f, 0.8f, 0.0f) + dir * 1.2f), true);
    if (!shot.entity)
        return;
    shot.entity->setName("EnemyShot");
    shot.team = team;
    if (ForceComponent* fc = getComponent<ForceComponent>(shot.entity.get()))
        fc->emitter.setTeam(team); // the prefab authors team 1 — PvP spitters shoot their own team's field
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(shot.entity.get()))
    {
        pc->body.setLinearVelocity(dir * m_spitterShotSpeed);
        // Contact hooks fire inside physics.update; only POINTER VALUES are recorded (processed
        // next tick, never dereferenced raw), so lifetime races are harmless.
        pc->onContact = [this, self = (const Entity*)shot.entity.get()](Entity& other, bool begin)
        {
            if (begin)
                m_pendingShotHits.push_back(ShotHit{ self, &other });
        };
    }
    Globals::world.addRootEntity(shot.entity);
    m_shots.push_back(std::move(shot));
}

void NpcSystem::tickShots(StructureSystem& structures, float deltaSec)
{
    // Resolve last frame's contacts: structure hits damage and pop the shot; anything else
    // (ground, player, units) just despawns it — no bouncing shells rolling around.
    for (const ShotHit& hit : m_pendingShotHits)
    {
        uint8 shotTeam = 1;
        for (Shot& s : m_shots)
            if (s.entity.get() == hit.shot)
            {
                shotTeam = s.team;
                s.age = m_spitterShotLifetime + 1.0f; // marked: the sweep below removes it
                break;
            }
        const int structure = structures.structureIndexByEntity(hit.victim);
        if (structure >= 0 && structures.structureTeam(structure) != shotTeam)
            structures.damageStructure(structures.structureId(structure), m_spitterShotDamage);
    }
    m_pendingShotHits.clear();

    for (size_t i = 0; i < m_shots.size();)
    {
        Shot& s = m_shots[i];
        s.age += deltaSec;
        PhysicsComponent* pc = s.entity ? getComponent<PhysicsComponent>(s.entity.get()) : nullptr;
        if (!pc || !pc->body.isValid() || s.age > m_spitterShotLifetime
            || pc->body.getPosition().y < -20.0f)
        {
            if (s.entity)
                Globals::world.removeRootEntity(s.entity.get());
            m_shots.erase(m_shots.begin() + i);
            continue;
        }
        // Player emitter fields brake/deflect incoming shots (the force-ball pattern, mirrored),
        // and a shot pressing near a bubble SAPS it: same nearest-active-emitter deposit the units
        // use, so barrages drain a shield wall even when nothing penetrates.
        structures.addEmitterLoad(pc->body.getPosition(), m_shotEmitterDrainRadius, m_shotEmitterDrain, s.team);
        if (const ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        {
            const glm::vec3 force = fc->emitter.getAppliedForce();
            const float pressure = fc->emitter.getPressure();
            if (glm::dot(force, force) > 1e-8f)
                pc->body.applyImpulse(force * deltaSec * m_fieldPushGain * pressure);
        }
        ++i;
    }
}

// A unit spawn point on a ring around a building, on the first of 8 probed angles whose cell is
// free of structures (and inside the arena bounds) — units must never spawn INSIDE the building
// (they have no pathfinding to escape it). Falls back to the first angle when everything is full.
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

// Spawn ring: just outside the barracks' own footprint.
static float barracksSpawnRadius()
{
    return StructureSystem::footprintCellsOf(EStructureType::Barracks)
        * StructureSystem::GridCellSize * 0.5f + 1.5f;
}

glm::vec3 NpcSystem::unitPos(int index) const
{
    const PhysicsComponent* pc = getComponent<PhysicsComponent>(m_units[index].entity.get());
    return pc && pc->body.isValid() ? pc->body.getPosition() : m_units[index].entity->pos;
}

void NpcSystem::collectShieldStates(std::vector<ShieldNetState>& out) const
{
    const auto append = [&out](const Unit& u, float healthMax, float energyMax, uint8 kind)
    {
        const NetworkComponent* net = getComponent<NetworkComponent>(u.entity.get());
        if (!net || net->netId == 0)
            return;
        const ForceComponent* fc = getComponent<ForceComponent>(u.entity.get());
        ShieldNetState s;
        s.netId = net->netId;
        s.healthFrac = glm::clamp(u.health / glm::max(healthMax, 1e-3f), 0.0f, 1.0f);
        s.energyFrac = glm::clamp(u.energy / glm::max(energyMax, 1e-3f), 0.0f, 1.0f);
        s.output = fc ? fc->emitter.getOutput() : 0.0f;
        s.collapsed = u.energy <= 0.0f;
        s.kind = kind;
        s.team = u.team;
        out.push_back(s);
    };
    for (int i = 0; i < (int)m_units.size(); ++i)
        append(m_units[i], unitHealthMax(i), unitEnergyMax(i), 0);
}

void NpcSystem::damageByEntity(const Entity* entity, float amount)
{
    for (Unit& u : m_units)
        if (u.entity.get() == entity)
        {
            u.health -= amount; // <= 0 despawns in the next tickAuthority sweep
            return;
        }
}

void NpcSystem::saveUnits(AssetNode& root) const
{
    for (const Unit& u : m_units)
    {
        if (!u.entity)
            continue;
        AssetNode& n = root.addChild("Unit");
        n.set("Type", std::to_string((int)u.type));
        n.set("Team", std::to_string((int)u.team));
        n.set("Position", u.entity->pos);
        n.set("Health", u.health);
        n.set("Energy", u.energy);
        n.set("Source", std::to_string(u.sourceId));
        n.set("RouteIndex", std::to_string(u.routeIndex));
    }
}

void NpcSystem::loadUnits(const AssetNode& root)
{
    for (Unit& u : m_units)
        if (u.entity)
            Globals::world.removeRootEntity(u.entity.get());
    m_units.clear();
    for (Shot& s : m_shots)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    m_shots.clear();
    for (Shot& s : m_turretShots)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    m_turretShots.clear();
    m_pendingShotHits.clear();

    for (const AssetNode* n : root.findAll("Unit"))
    {
        const int typeInt = glm::clamp(n->find("Type") ? n->find("Type")->asInt() : 0,
            0, (int)ENpcType::Count - 1);
        const uint8 team = (uint8)glm::clamp(n->find("Team") ? n->find("Team")->asInt() : 1,
            0, GameMaxTeams - 1);
        const glm::vec3 pos = n->find("Position") ? n->find("Position")->asVec3() : glm::vec3(0.0f, 1.0f, 0.0f);
        const uint32 source = n->find("Source") ? (uint32)n->find("Source")->asInt() : 0;
        const size_t before = m_units.size();
        spawnUnitAt(pos, source, team, (ENpcType)typeInt);
        if (m_units.size() == before)
            continue; // spawn failed
        Unit& u = m_units.back();
        const NpcTypeDef& def = c_npcTypes[typeInt];
        u.health = glm::clamp(n->find("Health") ? n->find("Health")->asFloat() : u.health,
            1.0f, m_unitHealth * def.health);
        u.energy = glm::clamp(n->find("Energy") ? n->find("Energy")->asFloat() : u.energy,
            0.0f, m_unitEnergy * def.energy);
        u.routeIndex = n->find("RouteIndex") ? (uint32)n->find("RouteIndex")->asInt() : 0;
    }
}

void NpcSystem::spawnUnitAt(const glm::vec3& pos, uint32 sourceId, uint8 team, ENpcType type)
{
    const NpcTypeDef& def = c_npcTypes[(int)type];
    Unit u;
    u.type = type;
    u.team = team;
    u.sourceId = sourceId;
    u.health = m_unitHealth * def.health;
    u.energy = m_unitEnergy * def.energy;
    for (float& h : u.outputHistory)
        h = m_unitShieldOutput * def.shieldOutput;
    u.entity = Globals::world.spawnAssetFile(def.prefab, Transform(pos), true);
    if (!u.entity)
        return;
    u.entity->setName(def.name);
    Globals::world.addRootEntity(u.entity);
    if (ForceComponent* fc = getComponent<ForceComponent>(u.entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 1 — units carry their builder's team
    // ENEMY-team projectile hits hurt (own shots pass free). Contact events fire main-thread
    // inside physics.update, and this system outlives its units (GameMatch member), so the
    // capture is safe.
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(u.entity.get()))
        pc->onContact = [this, unit = u.entity.get(), team](Entity& other, bool begin)
        {
            if (!begin || std::string_view(other.getName()) != "Projectile")
                return;
            const ForceComponent* fc = getComponent<ForceComponent>(&other);
            if (!fc || fc->emitter.getTeam() != team)
                damageByEntity(unit, m_projectileDamage);
        };
    m_units.push_back(std::move(u));
}

// The nearest ENEMY player body to `pos` — the unit/turret targeting fallback. Returns `pos`
// itself when no enemy players exist (the unit simply idles).
static glm::vec3 nearestPlayerTo(const glm::vec3& pos, std::span<const PlayerInfo> players, uint8 excludeTeam)
{
    glm::vec3 best = pos;
    float bestDistSq = FLT_MAX;
    for (const PlayerInfo& p : players)
    {
        if (p.team == excludeTeam)
            continue;
        const glm::vec2 d = glm::vec2(p.pos.x, p.pos.z) - glm::vec2(pos.x, pos.z);
        const float distSq = glm::dot(d, d);
        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            best = p.pos;
        }
    }
    return best;
}

void NpcSystem::tickBarracks(StructureSystem& structures, float deltaSec)
{
    // Barracks are the ONLY unit source — each VARIANT produces its own unit type, paying
    // MATERIALS from its conveyor-fed store; the units ride the shared sim and hunt the enemy.
    const auto unitOfBarracks = [](EStructureType t)
    {
        return t == EStructureType::BarracksBrute ? ENpcType::Brute
             : t == EStructureType::BarracksRunner ? ENpcType::Runner
             : t == EStructureType::BarracksSpitter ? ENpcType::Spitter : ENpcType::Grunt;
    };
    for (int i = 0; i < structures.structureCount(); ++i)
    {
        if (!isBarracksType(structures.structureType(i)) || structures.structureBlueprint(i))
            continue;
        const uint32 id = structures.structureId(i);
        float& timer = m_barracksTimers[id];
        // idle Constructors in range accelerate the spawn clock (boost 1 = double speed)
        timer = glm::max(0.0f, timer - deltaSec * (1.0f + structures.barracksSpawnBoost(id)));
        if (timer > 0.0f)
            continue;
        // The ONLY cap is the per-barracks "Barracks unit limit" — each team's army size is its
        // own barracks count x limit.
        int alive = 0;
        for (const Unit& u : m_units)
            if (u.sourceId == id)
                ++alive;
        if (alive >= m_barracksUnitLimit)
            continue;
        const ENpcType unitType = unitOfBarracks(structures.structureType(i));
        if (!structures.trySpendStoredMinerals(id, m_spawnMaterials[(int)unitType]))
            continue; // each unit type has its own material price
        spawnUnitAt(freeSpawnPointAround(structures, structures.structurePos(i), barracksSpawnRadius()),
            id, structures.structureTeam(i), unitType);
        timer = m_barracksSpawnInterval;
    }
    std::erase_if(m_barracksTimers, [&](const auto& kv) {
        return structures.structureIndexById(kv.first) < 0; });
}

void NpcSystem::tickAuthority(std::span<const PlayerInfo> players, std::span<float> outPlayerDamage,
    StructureSystem& structures, float deltaSec)
{
    for (size_t i = 0; i < m_units.size();)
    {
        Unit& u = m_units[i];
        PhysicsComponent* pc = u.entity ? getComponent<PhysicsComponent>(u.entity.get()) : nullptr;
        if (!pc || !pc->body.isValid() || u.health <= 0.0f)
        {
            if (u.entity)
                Globals::world.removeRootEntity(u.entity.get());
            m_units.erase(m_units.begin() + i);
            continue;
        }
        const glm::vec3 pos = pc->body.getPosition();
        if (pos.y < -20.0f)
        {
            // Fell out of the world (field impulses can fling bodies through the thin ground
            // plane): a voided unit would keep planar-steering onto targets invisibly forever.
            Globals::world.removeRootEntity(u.entity.get());
            m_units.erase(m_units.begin() + i);
            continue;
        }

        // Random target, re-rolled on a jittered timer or when the target dies — units roam
        // between structures instead of grinding forever into one parked behind a forcefield.
        // Whether they can actually hurt it stays purely physical: they must REACH attack range,
        // and a powered emitter's field pushes them out well before that.
        const NpcTypeDef& def = c_npcTypes[(int)u.type];
        u.retargetTimer -= deltaSec;
        int target = u.targetId != 0 ? structures.structureIndexById(u.targetId) : -1;
        if (target < 0 || u.retargetTimer <= 0.0f)
        {
            u.targetId = structures.randomTargetStructureId(pos, u.team);
            u.retargetTimer = m_retargetInterval * def.retarget * glm::linearRand(0.7f, 1.3f);
            target = u.targetId != 0 ? structures.structureIndexById(u.targetId) : -1;
        }
        glm::vec3 targetPos = target >= 0 ? structures.structurePos(target)
                                          : nearestPlayerTo(pos, players, u.team);
        // BARRACKS ROUTE: walk the spawning barracks' waypoints first — combat targeting only
        // takes over past the last one. Touching a waypoint's circle advances the route.
        bool routing = false;
        if (const std::vector<glm::vec3>* route = structures.structureRouteById(u.sourceId);
            route && u.routeIndex < route->size())
        {
            if (glm::distance(glm::vec2(pos.x, pos.z),
                glm::vec2((*route)[u.routeIndex].x, (*route)[u.routeIndex].z)) < structures.waypointRadius())
                ++u.routeIndex;
            if (u.routeIndex < route->size())
            {
                targetPos = (*route)[u.routeIndex];
                routing = true;
                target = -1; // no gnawing/shooting mid-march
            }
        }
        const glm::vec2 toTarget(targetPos.x - pos.x, targetPos.z - pos.z);
        const float dist = glm::length(toTarget);
        const float moveSpeed = m_moveSpeed * def.speed;
        const bool ranged = !routing && u.type == ENpcType::Spitter;
        // Melee reach measures to the structure's FOOTPRINT, not its center: structure positions
        // sit at half height (1-3 m up) and big buildings' centers are unreachable — a Brute
        // pressed against a 3x3 barracks was 3+ m from its center and never landed a bite. The
        // height gate keeps flung/voided units from ghost-gnawing at their XZ.
        const float gnawReach = m_attackRange + (target >= 0
            ? StructureSystem::footprintCellsOf(structures.structureType(target))
                * StructureSystem::GridCellSize * 0.5f : 0.0f);
        // Ranged units hold at standoff and shoot; melee types must physically reach the target.
        const float stopRange = ranged ? m_spitterRange : gnawReach;

        if (ranged && dist <= m_spitterRange)
        {
            u.fireTimer -= deltaSec;
            if (u.fireTimer <= 0.0f)
            {
                fireSpitterShot(pos, targetPos, u.team);
                u.fireTimer = m_spitterFireInterval * glm::linearRand(0.8f, 1.2f);
            }
        }
        else if (!ranged && target >= 0 && dist <= gnawReach && pos.y > -2.0f && pos.y < 8.0f)
            structures.damageStructure(structures.structureId(target), m_attackDps * def.structDps * deltaSec);

        // DIRECT player damage: any enemy-team player inside melee reach gets chewed — no
        // targeting needed, standing in the swarm hurts. The caller routes each player's total
        // to the instance that owns that player's health.
        if (!routing)
            for (size_t p = 0; p < players.size() && p < outPlayerDamage.size(); ++p)
            {
                if (players[p].team == u.team)
                    continue;
                const glm::vec2 toPlayer(players[p].pos.x - pos.x, players[p].pos.z - pos.z);
                const float reach = m_attackRange + 0.8f; // capsule body allowance
                if (glm::dot(toPlayer, toPlayer) < reach * reach
                    && glm::abs(players[p].pos.y - pos.y) < 3.0f)
                    outPlayerDamage[p] += m_playerAttackDps * def.structDps * deltaSec;
            }

        if (dist > stopRange)
        {
            // Camera-free planar velocity steering (the player-movement pattern).
            glm::vec2 dir = toTarget / glm::max(dist, 1e-3f);
            glm::vec3 vel = pc->body.getLinearVelocity();
            // NO pathfinding: a unit driving straight into a static box (its own barracks, a wall)
            // pins there forever. Detect "wants to move but barely moves" and sidestep for a
            // moment — repeated detours walk it around any convex obstacle.
            const glm::vec2 planarVel(vel.x, vel.z);
            if (glm::dot(planarVel, planarVel) < 0.09f * moveSpeed * moveSpeed)
                u.stuckTimer += deltaSec;
            else
                u.stuckTimer = 0.0f;
            if (u.stuckTimer > 0.7f && u.avoidTimer <= 0.0f)
            {
                u.avoidTimer = glm::linearRand(0.6f, 1.2f);
                u.avoidSign = glm::linearRand(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
                u.stuckTimer = 0.0f;
            }
            if (u.avoidTimer > 0.0f)
            {
                u.avoidTimer -= deltaSec;
                dir = glm::normalize(glm::vec2(-dir.y, dir.x) * u.avoidSign + dir * 0.25f);
            }
            glm::vec3 dv = glm::vec3(dir.x * moveSpeed - vel.x, 0.0f, dir.y * moveSpeed - vel.z);
            const float maxDv = m_accel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            vel.x += dv.x;
            vel.z += dv.z;
            pc->body.setLinearVelocity(vel);
        }

        // A unit leaning on emitter bubbles costs them energy (nearest-active-bubble deposit) —
        // SHIELD-INDEPENDENT: a collapsed unit strains the emitter exactly like a shielded one, so
        // breaking a unit's battery never makes the siege cheaper for the defender.
        structures.addEmitterLoad(pos, m_emitterDrainRadius, m_emitterDrainPerUnit * def.emitterDrain, u.team);

        // The unit's bubble follows the PLAYER's shield rules, minus regen: player-field pressure
        // drains its energy battery; empty = PERMANENT collapse (sentinel output keeps the
        // pressure sensor alive, no bubble, no push-back); once the equilibrium radius no longer
        // covers the body while pressure is present, health drains — pushing into a powered
        // emitter line kills them. The same readback also shoves them (force-ball pattern), which
        // is what makes that line a physical wall.
        if (ForceComponent* fc = getComponent<ForceComponent>(u.entity.get()))
        {
            const float pressure = fc->emitter.getPressure();
            const float tension = 1.0f + m_fieldTension * pressure; // surface tension (player rule)
            const float energyBefore = u.energy;
            u.energy = glm::max(0.0f, u.energy - pressure * tension * m_unitEnergyDrainRate * deltaSec);
            if (energyBefore > 0.0f && u.energy <= 0.0f)
                m_shieldCollapseEdge = true; // co-op: flush the shield mirror this tick
            const float liveOutput = m_unitShieldOutput * def.shieldOutput;
            fc->emitter.setOutput(u.energy > 0.0f ? liveOutput : 0.01f);

            const float iso = Globals::forceSystem.getParams().isoThreshold;
            if (fc->emitter.getEquilibriumRadius() < m_unitDamageRadius && pressure > iso)
                u.health -= m_unitFieldDps * deltaSec;

            // Push-back INDEPENDENT of the shield state: getAppliedForce scales with the bubble's
            // own output, so normalize by the output that produced the readback (oldest history
            // entry — matches the ~2-frame latency without a collapse-frame spike). A unit whose
            // shield broke keeps getting shoved by emitter lines exactly as before.
            const glm::vec3 force = fc->emitter.getAppliedForce() / glm::max(u.outputHistory[0], 1e-3f);
            if (glm::dot(force, force) > 1e-8f)
            {
                pc->body.applyImpulse(force * deltaSec * m_fieldPushGain * pressure * tension);
                // Field shoves must never turn into launches: clamp the resulting speed, or a
                // strong emitter wall flings units through the ground / into orbit.
                const glm::vec3 vel = pc->body.getLinearVelocity();
                const float speed = glm::length(vel);
                const float maxSpeed = glm::max(moveSpeed, m_moveSpeed) * 3.0f;
                if (speed > maxSpeed)
                    pc->body.setLinearVelocity(vel * (maxSpeed / speed));
            }
            u.outputHistory[0] = u.outputHistory[1];
            u.outputHistory[1] = u.outputHistory[2];
            u.outputHistory[2] = u.energy > 0.0f ? liveOutput : 0.01f;
        }
        ++i;
    }

    tickShots(structures, deltaSec); // spitter projectiles: contact damage + aging + field deflection
}
