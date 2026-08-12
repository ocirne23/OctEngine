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

// Multipliers over the "Unit *" tweak baseline; spawnWeight sets the wave mix.
static constexpr NpcTypeDef c_npcTypes[(int)ENpcType::Count] = {
    //  name      prefab                            hp    en    spd   dps   drain out   retgt weight
    { "Enemy",   "Entities/Game/enemyUnit.pre",    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { "Brute",   "Entities/Game/enemyBrute.pre",   3.0f, 2.0f, 0.6f, 2.0f, 1.5f, 1.3f, 1.5f, 0.35f },
    { "Runner",  "Entities/Game/enemyRunner.pre",  0.5f, 0.4f, 1.9f, 0.7f, 0.5f, 0.7f, 0.5f, 0.7f },
    { "Spitter", "Entities/Game/enemySpitter.pre", 0.8f, 0.8f, 0.9f, 0.0f, 0.5f, 0.8f, 1.0f, 0.5f },
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
    Tweak::floatVar("Game/Enemies", "Unit health", &m_unitHealth, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Move speed", &m_moveSpeed, 0.5f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Accel", &m_accel, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Attack range", &m_attackRange, 0.5f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Enemies", "Attack dps", &m_attackDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Retarget interval", &m_retargetInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spawn interval", &m_spawnInterval, 0.1f, 30.0f, 0.1f);
    Tweak::intVar("Game/Enemies", "Max units", &m_maxUnits, 0, 512, 1);
    Tweak::floatVar("Game/Enemies", "Spawn margin", &m_spawnMargin, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Projectile damage", &m_projectileDamage, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Melee damage", &m_meleeDamage, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &m_fieldPushGain, 0.0f, 100000.0f, 100.0f);
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
    Tweak::floatVar("Game/Friendlies", "Barracks spawn energy", &m_barracksSpawnEnergy, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Attack dps", &m_friendlyAttackDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Aggro radius", &m_friendlyAggroRadius, 2.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Leash radius", &m_friendlyLeash, 4.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret range", &m_turretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret fire interval", &m_turretFireInterval, 0.1f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Turret shot energy", &m_turretShotEnergy, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Turret shot speed", &m_turretShotSpeed, 5.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Enemies", "Camp count", &m_campCount, 0, 12, 1);
    Tweak::floatVar("Game/Enemies", "Camp distance", &m_campDistance, 40.0f, 190.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Camp core health", &m_campCoreHealth, 20.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Game/Enemies", "Camp structure health", &m_campStructHealth, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Enemies", "Camp spawn interval", &m_campSpawnInterval, 2.0f, 120.0f, 0.5f);
    Tweak::intVar("Game/Enemies", "Camp unit cap", &m_campUnitCap, 0, 16, 1);
    Tweak::floatVar("Game/Enemies", "Camp turret range", &m_campTurretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Camp turret interval", &m_campTurretInterval, 0.25f, 20.0f, 0.25f);
}

void NpcSystem::clear()
{
    for (Unit& u : m_units)
        if (u.entity)
            Globals::world.removeRootEntity(u.entity.get());
    m_units.clear();
    for (Unit& u : m_friendlies)
        if (u.entity)
            Globals::world.removeRootEntity(u.entity.get());
    m_friendlies.clear();
    for (EnemyStructure& s : m_enemyStructures)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    m_enemyStructures.clear();
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
        if (structures.structureType(i) != EStructureType::Turret)
            continue;
        const uint32 id = structures.structureId(i);
        float& timer = m_turretTimers[id];
        timer = glm::max(0.0f, timer - deltaSec);
        if (timer > 0.0f)
            continue;
        // Nearest enemy in range; no target = no cooldown reset, fires the moment one appears.
        const glm::vec3 turretPos = structures.structurePos(i);
        int target = -1;
        float bestDistSq = m_turretRange * m_turretRange;
        for (int e = 0; e < (int)m_units.size(); ++e)
        {
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
        shot.entity->setName("Projectile"); // the name IS the enemy contact-damage hook
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

void NpcSystem::fireSpitterShot(const glm::vec3& from, const glm::vec3& at)
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
        const int structure = structures.structureIndexByEntity(hit.victim);
        if (structure >= 0)
            structures.damageStructure(structures.structureId(structure), m_spitterShotDamage);
        for (Shot& s : m_shots)
            if (s.entity.get() == hit.shot)
            {
                s.age = m_spitterShotLifetime + 1.0f; // marked: the sweep below removes it
                break;
            }
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
        structures.addEmitterLoad(pc->body.getPosition(), m_shotEmitterDrainRadius, m_shotEmitterDrain);
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

glm::vec3 NpcSystem::friendlyPos(int index) const
{
    const PhysicsComponent* pc = getComponent<PhysicsComponent>(m_friendlies[index].entity.get());
    return pc && pc->body.isValid() ? pc->body.getPosition() : m_friendlies[index].entity->pos;
}

void NpcSystem::spawnFriendly(const glm::vec3& barracksPos, uint32 barracksId)
{
    const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
    const glm::vec3 pos = glm::vec3(barracksPos.x, 1.0f, barracksPos.z)
        + glm::vec3(std::cos(angle) * 2.5f, 0.0f, std::sin(angle) * 2.5f);
    Unit u;
    u.health = m_unitHealth;
    u.energy = m_unitEnergy;
    for (float& h : u.outputHistory)
        h = m_unitShieldOutput;
    u.sourceId = barracksId;
    u.homePos = barracksPos;
    u.entity = Globals::world.spawnAssetFile("Entities/Game/friendlyUnit.pre", Transform(pos), true);
    if (!u.entity)
        return;
    u.entity->setName("Guard");
    Globals::world.addRootEntity(u.entity);
    m_friendlies.push_back(std::move(u));
}

void NpcSystem::tickFriendlies(StructureSystem& structures, float deltaSec)
{
    // Barracks production: one unit per interval while under the per-barracks alive limit AND the
    // barracks can pay the spawn energy from its internal store (supply-gated unit output).
    for (int i = 0; i < structures.structureCount(); ++i)
    {
        if (structures.structureType(i) != EStructureType::Barracks)
            continue;
        const uint32 id = structures.structureId(i);
        float& timer = m_barracksTimers[id];
        timer = glm::max(0.0f, timer - deltaSec);
        if (timer > 0.0f)
            continue;
        int alive = 0;
        for (const Unit& u : m_friendlies)
            if (u.sourceId == id)
                ++alive;
        if (alive >= m_barracksUnitLimit)
            continue; // full roster: retry the moment a slot frees (timer stays 0)
        if (!structures.trySpendEnergy(id, m_barracksSpawnEnergy))
            continue; // store empty: retry when the cables have refilled it
        spawnFriendly(structures.structurePos(i), id);
        timer = m_barracksSpawnInterval;
    }

    for (size_t i = 0; i < m_friendlies.size();)
    {
        Unit& u = m_friendlies[i];
        PhysicsComponent* pc = u.entity ? getComponent<PhysicsComponent>(u.entity.get()) : nullptr;
        const bool orphaned = structures.structureIndexById(u.sourceId) < 0; // barracks died
        if (!pc || !pc->body.isValid() || u.health <= 0.0f || orphaned)
        {
            if (u.entity)
                Globals::world.removeRootEntity(u.entity.get());
            m_friendlies.erase(m_friendlies.begin() + i);
            continue;
        }
        const glm::vec3 pos = pc->body.getPosition();
        if (pos.y < -20.0f)
        {
            Globals::world.removeRootEntity(u.entity.get());
            m_friendlies.erase(m_friendlies.begin() + i);
            continue;
        }

        // Guard behavior: chase the nearest enemy unit seen within aggro of SELF while it stays
        // inside the leash around home; otherwise walk home. Attack = proximity gnaw, symmetric
        // with the enemies' rule.
        glm::vec3 targetPos = u.homePos;
        int enemyTarget = -1;
        float bestDistSq = m_friendlyAggroRadius * m_friendlyAggroRadius;
        for (int e = 0; e < (int)m_units.size(); ++e)
        {
            const glm::vec3 ePos = unitPos(e);
            const glm::vec2 toEnemy(ePos.x - pos.x, ePos.z - pos.z);
            const glm::vec2 fromHome(ePos.x - u.homePos.x, ePos.z - u.homePos.z);
            if (glm::dot(fromHome, fromHome) > m_friendlyLeash * m_friendlyLeash)
                continue;
            const float distSq = glm::dot(toEnemy, toEnemy);
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                enemyTarget = e;
            }
        }
        if (enemyTarget >= 0)
            targetPos = unitPos(enemyTarget);

        const glm::vec2 toTarget(targetPos.x - pos.x, targetPos.z - pos.z);
        const float dist = glm::length(toTarget);
        if (enemyTarget >= 0 && glm::distance(pos, targetPos) <= m_attackRange)
            m_units[enemyTarget].health -= m_friendlyAttackDps * deltaSec;
        else if (dist > (enemyTarget >= 0 ? 1e-3f : 2.5f)) // idle: park near home
        {
            const glm::vec2 dir = toTarget / glm::max(dist, 1e-3f);
            glm::vec3 vel = pc->body.getLinearVelocity();
            glm::vec3 dv = glm::vec3(dir.x * m_moveSpeed - vel.x, 0.0f, dir.y * m_moveSpeed - vel.z);
            const float maxDv = m_accel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            vel.x += dv.x;
            vel.z += dv.z;
            pc->body.setLinearVelocity(vel);
        }

        // Same shield/battery/exposure rules as the enemies, mirrored across teams: enemy-field
        // pressure drains the battery (no regen), collapse is permanent, exposure bleeds health,
        // and enemy fields shove them (speed-clamped like the enemy push).
        if (ForceComponent* fc = getComponent<ForceComponent>(u.entity.get()))
        {
            const float pressure = fc->emitter.getPressure();
            u.energy = glm::max(0.0f, u.energy - pressure * m_unitEnergyDrainRate * deltaSec);
            fc->emitter.setOutput(u.energy > 0.0f ? m_unitShieldOutput : 0.01f);

            const float iso = Globals::forceSystem.getParams().isoThreshold;
            if (fc->emitter.getEquilibriumRadius() < m_unitDamageRadius && pressure > iso)
                u.health -= m_unitFieldDps * deltaSec;

            const glm::vec3 force = fc->emitter.getAppliedForce() / glm::max(u.outputHistory[0], 1e-3f);
            if (glm::dot(force, force) > 1e-8f)
            {
                pc->body.applyImpulse(force * deltaSec * m_fieldPushGain * pressure);
                const glm::vec3 vel = pc->body.getLinearVelocity();
                const float speed = glm::length(vel);
                const float maxSpeed = m_moveSpeed * 3.0f;
                if (speed > maxSpeed)
                    pc->body.setLinearVelocity(vel * (maxSpeed / speed));
            }
            u.outputHistory[0] = u.outputHistory[1];
            u.outputHistory[1] = u.outputHistory[2];
            u.outputHistory[2] = u.energy > 0.0f ? m_unitShieldOutput : 0.01f;
        }
        ++i;
    }

    // Prune timers of dead barracks so the map doesn't grow forever.
    std::erase_if(m_barracksTimers, [&](const auto& kv) {
        return structures.structureIndexById(kv.first) < 0; });
}

glm::vec3 NpcSystem::unitPos(int index) const
{
    const PhysicsComponent* pc = getComponent<PhysicsComponent>(m_units[index].entity.get());
    return pc && pc->body.isValid() ? pc->body.getPosition() : m_units[index].entity->pos;
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

void NpcSystem::spawnUnit(const glm::vec3& center, float ringRadius)
{
    const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
    const float r = ringRadius + m_spawnMargin;
    spawnUnitAt(glm::vec3(center.x, 0.0f, center.z)
        + glm::vec3(std::cos(angle) * r, 1.0f, std::sin(angle) * r), 0);
}

void NpcSystem::spawnUnitAt(const glm::vec3& pos, uint32 sourceId)
{
    // Weighted type roll: the wave mix comes from the table's spawnWeight column.
    float totalWeight = 0.0f;
    for (const NpcTypeDef& def : c_npcTypes)
        totalWeight += def.spawnWeight;
    float roll = glm::linearRand(0.0f, totalWeight);
    ENpcType type = ENpcType::Grunt;
    for (int t = 0; t < (int)ENpcType::Count; ++t)
    {
        roll -= c_npcTypes[t].spawnWeight;
        if (roll <= 0.0f)
        {
            type = (ENpcType)t;
            break;
        }
    }
    const NpcTypeDef& def = c_npcTypes[(int)type];
    Unit u;
    u.type = type;
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
    // Projectile hits hurt: contact events fire main-thread inside physics.update, and this system
    // outlives its units (GameMatch member), so the capture is safe.
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(u.entity.get()))
        pc->onContact = [this, unit = u.entity.get()](Entity& other, bool begin)
        {
            if (begin && std::string_view(other.getName()) == "Projectile")
                damageByEntity(unit, m_projectileDamage);
        };
    m_units.push_back(std::move(u));
}

static constexpr const char* c_enemyStructPrefabs[3] = {
    "Entities/Game/enemyCore.pre", "Entities/Game/enemySpawner.pre", "Entities/Game/enemyTurret.pre" };
static constexpr const char* c_enemyStructNames[3] = { "Enemy Core", "Enemy Spawner", "Enemy Turret" };

void NpcSystem::spawnEnemyStructure(EEnemyStructType type, const glm::vec3& pos, int camp)
{
    EnemyStructure s;
    s.type = type;
    s.camp = camp;
    s.maxHealth = type == EEnemyStructType::Core ? m_campCoreHealth : m_campStructHealth;
    s.health = s.maxHealth;
    s.timer = glm::linearRand(0.0f, 3.0f); // desync camp rhythms
    s.entity = Globals::world.spawnAssetFile(c_enemyStructPrefabs[(int)type], Transform(pos), true);
    if (!s.entity)
        return;
    s.entity->setName(c_enemyStructNames[(int)type]);
    Globals::world.addRootEntity(s.entity);
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity.get()))
        pc->onContact = [this, self = s.entity.get()](Entity& other, bool begin)
        {
            if (begin && std::string_view(other.getName()) == "Projectile")
                damageEnemyStructByEntity(self, m_projectileDamage);
        };
    m_enemyStructures.push_back(std::move(s));
}

void NpcSystem::damageEnemyStructByEntity(const Entity* entity, float amount)
{
    for (EnemyStructure& s : m_enemyStructures)
        if (s.entity.get() == entity)
        {
            s.health -= amount; // <= 0 despawns in the next tickEnemyStructures sweep
            return;
        }
}

void NpcSystem::spawnEnemyCamps(const glm::vec3& basePos)
{
    // Evenly spaced ring with jitter; each camp = core (center) + spawner + two turrets flanking.
    for (int c = 0; c < m_campCount; ++c)
    {
        const float angle = (float)c / glm::max(m_campCount, 1) * glm::two_pi<float>()
            + glm::linearRand(-0.25f, 0.25f);
        const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 side(-dir.z, 0.0f, dir.x);
        const glm::vec3 center = glm::vec3(basePos.x, 0.0f, basePos.z)
            + dir * (m_campDistance + glm::linearRand(-10.0f, 10.0f));
        spawnEnemyStructure(EEnemyStructType::Core, center + glm::vec3(0.0f, 1.6f, 0.0f), c);
        spawnEnemyStructure(EEnemyStructType::Spawner, center + dir * 5.0f + glm::vec3(0.0f, 1.0f, 0.0f), c);
        spawnEnemyStructure(EEnemyStructType::Turret, center + side * 6.0f + glm::vec3(0.0f, 1.3f, 0.0f), c);
        spawnEnemyStructure(EEnemyStructType::Turret, center - side * 6.0f + glm::vec3(0.0f, 1.3f, 0.0f), c);
    }
}

void NpcSystem::tickEnemyStructures(StructureSystem& structures, const glm::vec3& playerPos, float deltaSec)
{
    // A camp is POWERED while its core lives — dead cores silence the spawner and turrets (and the
    // core's bubble died with its entity), but the remains stay clearable.
    bool campPowered[16] = {};
    for (const EnemyStructure& s : m_enemyStructures)
        if (s.type == EEnemyStructType::Core && s.camp < 16)
            campPowered[s.camp] = true;

    for (size_t i = 0; i < m_enemyStructures.size();)
    {
        EnemyStructure& s = m_enemyStructures[i];
        if (!s.entity || s.health <= 0.0f)
        {
            Log::info(std::string(c_enemyStructNames[(int)s.type]) + " destroyed");
            if (s.entity)
                Globals::world.removeRootEntity(s.entity.get());
            m_enemyStructures.erase(m_enemyStructures.begin() + i);
            continue;
        }
        const bool powered = s.camp < 16 && campPowered[s.camp];
        s.timer = glm::max(0.0f, s.timer - deltaSec);
        if (powered && s.timer <= 0.0f)
        {
            const glm::vec3 pos = s.entity->pos;
            if (s.type == EEnemyStructType::Spawner)
            {
                int alive = 0;
                const uint32 sourceId = 1000u + (uint32)s.camp; // camp-tagged units for the cap
                for (const Unit& u : m_units)
                    if (u.sourceId == sourceId)
                        ++alive;
                if (alive < m_campUnitCap && (int)m_units.size() < m_maxUnits)
                {
                    const float a = glm::linearRand(0.0f, glm::two_pi<float>());
                    spawnUnitAt(glm::vec3(pos.x + std::cos(a) * 3.0f, 1.0f, pos.z + std::sin(a) * 3.0f), sourceId);
                    s.timer = m_campSpawnInterval;
                }
            }
            else if (s.type == EEnemyStructType::Turret)
            {
                // Player first if in range, else the nearest player structure.
                glm::vec3 target = playerPos;
                bool haveTarget = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(playerPos.x, playerPos.z))
                    <= m_campTurretRange;
                if (!haveTarget)
                {
                    const int nearest = structures.findConnectableNear(pos, m_campTurretRange);
                    if (nearest >= 0)
                    {
                        target = structures.structurePos(nearest);
                        haveTarget = true;
                    }
                }
                if (haveTarget)
                {
                    fireSpitterShot(pos, target);
                    s.timer = m_campTurretInterval;
                }
            }
        }
        ++i;
    }
}

void NpcSystem::applyPlayerMelee(const glm::vec3& pos, const glm::vec3& dirPlanar, float range)
{
    for (EnemyStructure& s : m_enemyStructures)
    {
        if (!s.entity)
            continue;
        const glm::vec3 d = s.entity->pos - pos;
        const glm::vec2 planar(d.x, d.z);
        const float dist = glm::length(planar);
        if (dist <= range && dist > 1e-3f
            && glm::dot(planar / dist, glm::vec2(dirPlanar.x, dirPlanar.z)) >= 0.5f)
            s.health -= m_meleeDamage;
    }
    for (Unit& u : m_units)
    {
        const PhysicsComponent* pc = getComponent<PhysicsComponent>(u.entity.get());
        if (!pc || !pc->body.isValid())
            continue;
        const glm::vec3 d = pc->body.getPosition() - pos;
        const glm::vec2 planar(d.x, d.z);
        const float dist = glm::length(planar);
        if (dist > range || dist < 1e-3f)
            continue;
        if (glm::dot(planar / dist, glm::vec2(dirPlanar.x, dirPlanar.z)) < 0.5f)
            continue; // same 120° cone as the shove
        u.health -= m_meleeDamage;
    }
}

void NpcSystem::tickAuthority(const glm::vec3& spawnCenter, float spawnRingRadius, const glm::vec3& playerPos,
    StructureSystem& structures, float deltaSec)
{
    m_spawnTimer -= deltaSec;
    if (m_spawnTimer <= 0.0f && (int)m_units.size() < m_maxUnits)
    {
        spawnUnit(spawnCenter, spawnRingRadius);
        m_spawnTimer = m_spawnInterval;
    }

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
            u.targetId = structures.randomTargetStructureId(pos);
            u.retargetTimer = m_retargetInterval * def.retarget * glm::linearRand(0.7f, 1.3f);
            target = u.targetId != 0 ? structures.structureIndexById(u.targetId) : -1;
        }
        const glm::vec3 targetPos = target >= 0 ? structures.structurePos(target) : playerPos;
        const glm::vec2 toTarget(targetPos.x - pos.x, targetPos.z - pos.z);
        const float dist = glm::length(toTarget);
        const float moveSpeed = m_moveSpeed * def.speed;
        const bool ranged = u.type == ENpcType::Spitter;
        // Ranged units hold at standoff and shoot; melee types must physically reach the target.
        const float stopRange = ranged ? m_spitterRange : m_attackRange;

        if (ranged && dist <= m_spitterRange)
        {
            u.fireTimer -= deltaSec;
            if (u.fireTimer <= 0.0f)
            {
                fireSpitterShot(pos, targetPos);
                u.fireTimer = m_spitterFireInterval * glm::linearRand(0.8f, 1.2f);
            }
        }
        // Attack range is 3D, deliberately: the planar-only check let units flung into the air or
        // under the ground keep gnawing structures at their XZ — invisible "ghost" damage.
        else if (!ranged && target >= 0 && glm::distance(pos, targetPos) <= m_attackRange)
            structures.damageStructure(structures.structureId(target), m_attackDps * def.structDps * deltaSec);

        if (dist > stopRange || (!ranged && dist > 1e-3f && glm::distance(pos, targetPos) > m_attackRange))
        {
            // Camera-free planar velocity steering (the player-movement pattern).
            const glm::vec2 dir = toTarget / glm::max(dist, 1e-3f);
            glm::vec3 vel = pc->body.getLinearVelocity();
            glm::vec3 dv = glm::vec3(dir.x * moveSpeed - vel.x, 0.0f, dir.y * moveSpeed - vel.z);
            const float maxDv = m_accel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            vel.x += dv.x;
            vel.z += dv.z;
            pc->body.setLinearVelocity(vel);
        }

        // A shielded unit leaning on emitter bubbles costs them energy (linear proximity falloff);
        // collapsed shields press nothing. This is the units' main siege weapon: crowds drain a
        // contested emitter's supply line faster than it can recharge.
        if (u.energy > 0.0f)
            structures.addEmitterLoad(pos, m_emitterDrainRadius, m_emitterDrainPerUnit * def.emitterDrain);

        // The unit's bubble follows the PLAYER's shield rules, minus regen: player-field pressure
        // drains its energy battery; empty = PERMANENT collapse (sentinel output keeps the
        // pressure sensor alive, no bubble, no push-back); once the equilibrium radius no longer
        // covers the body while pressure is present, health drains — pushing into a powered
        // emitter line kills them. The same readback also shoves them (force-ball pattern), which
        // is what makes that line a physical wall.
        if (ForceComponent* fc = getComponent<ForceComponent>(u.entity.get()))
        {
            const float pressure = fc->emitter.getPressure();
            u.energy = glm::max(0.0f, u.energy - pressure * m_unitEnergyDrainRate * deltaSec);
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
                pc->body.applyImpulse(force * deltaSec * m_fieldPushGain * pressure);
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
