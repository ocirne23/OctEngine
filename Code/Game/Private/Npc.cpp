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
}

void NpcSystem::clear()
{
    for (Unit& u : m_units)
        if (u.entity)
            Globals::world.removeRootEntity(u.entity.get());
    m_units.clear();
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
    const glm::vec3 pos = glm::vec3(center.x, 0.0f, center.z)
        + glm::vec3(std::cos(angle) * r, 1.0f, std::sin(angle) * r);
    Unit u;
    u.health = m_unitHealth;
    u.energy = m_unitEnergy;
    for (float& h : u.outputHistory)
        h = m_unitShieldOutput;
    u.entity = Globals::world.spawnAssetFile("Entities/Game/enemyUnit.pre", Transform(pos), true);
    if (!u.entity)
        return;
    u.entity->setName("Enemy");
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

void NpcSystem::applyPlayerMelee(const glm::vec3& pos, const glm::vec3& dirPlanar, float range)
{
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
        u.retargetTimer -= deltaSec;
        int target = u.targetId != 0 ? structures.structureIndexById(u.targetId) : -1;
        if (target < 0 || u.retargetTimer <= 0.0f)
        {
            u.targetId = structures.randomTargetStructureId(pos);
            u.retargetTimer = m_retargetInterval * glm::linearRand(0.7f, 1.3f);
            target = u.targetId != 0 ? structures.structureIndexById(u.targetId) : -1;
        }
        const glm::vec3 targetPos = target >= 0 ? structures.structurePos(target) : playerPos;
        const glm::vec2 toTarget(targetPos.x - pos.x, targetPos.z - pos.z);
        const float dist = glm::length(toTarget);

        // Attack range is 3D, deliberately: the planar-only check let units flung into the air or
        // under the ground keep gnawing structures at their XZ — invisible "ghost" damage.
        if (target >= 0 && glm::distance(pos, targetPos) <= m_attackRange)
            structures.damageStructure(structures.structureId(target), m_attackDps * deltaSec);
        else if (dist > 1e-3f)
        {
            // Camera-free planar velocity steering (the player-movement pattern).
            const glm::vec2 dir = toTarget / dist;
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

        // A shielded unit leaning on emitter bubbles costs them energy (linear proximity falloff);
        // collapsed shields press nothing. This is the units' main siege weapon: crowds drain a
        // contested emitter's supply line faster than it can recharge.
        if (u.energy > 0.0f)
            structures.addEmitterLoad(pos, m_emitterDrainRadius, m_emitterDrainPerUnit);

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
            fc->emitter.setOutput(u.energy > 0.0f ? m_unitShieldOutput : 0.01f);

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
}
