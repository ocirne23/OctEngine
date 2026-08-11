export module Game:Npc;

import Core;
import Core.glm;
import Entity;
import :Structures;

// Enemy NPC units (Entities/Game/enemyUnit.pre): spawned in waves from the objective, they steer
// toward the nearest player structure and damage it while in attack range (the Base is excluded —
// invulnerable). Each carries a small team-1 field bubble, so friendly emitter fields brake and
// shove them (applied-force readback -> impulse) and their presence pressures the player's shield.
// Killed by projectile contacts (ContactEvents -> onContact) and the player's melee cone.
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks();
    void clear(); // drops every unit (before world teardown)

    // Spawns arrive on a ring: spawnRingRadius (the field frontier, from GameMatch) + the "Spawn
    // margin" tweak — always just inside the enemy field, all around the safe zone.
    void tickAuthority(const glm::vec3& spawnCenter, float spawnRingRadius, const glm::vec3& playerPos,
        StructureSystem& structures, float deltaSec);
    // The player's melee swing: damage every unit in the planar cone (called by GameMatch when
    // GamePlayer reports a swing; the physics shove happens in GamePlayer::tickCombat).
    void applyPlayerMelee(const glm::vec3& pos, const glm::vec3& dirPlanar, float range);

    int unitCount() const { return (int)m_units.size(); }
    glm::vec3 unitPos(int index) const;
    float unitHealth(int index) const { return m_units[index].health; }
    float unitHealthMax() const { return m_unitHealth; }
    float unitEnergy(int index) const { return m_units[index].energy; }
    float unitEnergyMax() const { return m_unitEnergy; }

private:
    struct Unit
    {
        EntityPtr entity;
        float health = 60.0f;
        float energy = 40.0f;       // shield battery, player rules but NO regen: pressure drains
                                    // it, empty = permanent collapse — pushing into emitters kills
        float outputHistory[3] = { 0.8f, 0.8f, 0.8f }; // recent outputs: the applied-force readback
                                    // (~2 frames latent) scales with output, so the push normalizes
                                    // by the output that produced it — a collapsed shield is pushed
                                    // exactly like a full one
        uint32 targetId = 0;        // current target (stable structure id; 0 = none)
        float retargetTimer = 0.0f; // expiry re-rolls the target — units roam between structures
                                    // instead of grinding forever into one behind a forcefield
    };

    void spawnUnit(const glm::vec3& center, float ringRadius);
    void damageByEntity(const Entity* entity, float amount); // projectile contact hook

    std::vector<Unit> m_units;
    float m_spawnTimer = 4.0f; // first wave a few seconds in

    // Tweaks ("Game/Enemies")
    float m_unitHealth = 60.0f;
    float m_moveSpeed = 4.0f;
    float m_accel = 30.0f;
    float m_attackRange = 1.5f;    // must actually reach the structure — a powered emitter's field
                                   // pushes them out well past this, so damage is purely proximity
    float m_attackDps = 6.0f;      // structure health/s while in range
    float m_retargetInterval = 8.0f;
    float m_spawnInterval = 12.0f;
    int m_maxUnits = 8;
    float m_spawnMargin = 10.0f;   // metres beyond the frontier ring where waves appear
    float m_projectileDamage = 25.0f;
    float m_meleeDamage = 40.0f;
    float m_fieldPushGain = 20000.0f; // friendly fields shoving units (force-ball scale)
    float m_unitEnergy = 40.0f;      // shield battery (no regen)
    float m_unitEnergyDrainRate = 25.0f; // energy/s per unit of pressure (player rule)
    float m_unitShieldOutput = 0.8f; // bubble output while the battery lives (collapse -> sentinel)
    float m_unitDamageRadius = 0.6f; // equilibrium radius below this + pressure = health drain
    float m_unitFieldDps = 10.0f;    // health/s while exposed inside player fields
    float m_emitterDrainPerUnit = 5.0f; // energy/s a shielded unit costs an emitter at zero range
    float m_emitterDrainRadius = 12.0f;  // falloff radius of that drain (~the emitter bubble)
};
