export module Game:Npc;

import Core;
import Core.glm;
import Entity;
import :Structures;

// Enemy unit types: stat MULTIPLIERS over the "Game/Enemies" "Unit *" tweak baseline (the Grunt).
export enum class ENpcType : uint8 { Grunt, Brute, Runner, Spitter, Count };
struct NpcTypeDef
{
    const char* name;
    const char* prefab;
    float health;       // x "Unit health"
    float energy;       // x "Unit energy"
    float speed;        // x "Move speed"
    float structDps;    // x "Attack dps"
    float emitterDrain; // x "Emitter drain/s per unit"
    float shieldOutput; // x "Unit shield output"
    float retarget;     // x "Retarget interval"
    float spawnWeight;  // relative pick chance per wave slot
};

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
    // FRIENDLY units: spawned by Barracks structures (per-barracks alive limit, each spawn spends
    // barracks energy), they guard their home — aggro on enemy units within range, leashed to the
    // barracks — and fight through the same field rules (team-0 bubble, no-regen battery).
    void tickFriendlies(StructureSystem& structures, float deltaSec);
    // TURRETS: fire player-type projectiles ("Projectile" name = the enemy contact-damage hook) at
    // the nearest enemy in range, spending turret energy per shot.
    void tickTurrets(StructureSystem& structures, float deltaSec);

    // ENEMY CAMPS: self-contained bases ringing the map — each a CORE (power + a strong team-1
    // bubble), a SPAWNER (local wave production with a per-camp alive cap) and two TURRETS (fire
    // spitter shots at the player / nearest structure). A dead core DISABLES the camp (no spawns,
    // no fire; its bubble died with it) — the camp's remains can still be cleared for good.
    void spawnEnemyCamps(const glm::vec3& basePos);
    void tickEnemyStructures(StructureSystem& structures, const glm::vec3& playerPos, float deltaSec);
    int enemyStructureCount() const { return (int)m_enemyStructures.size(); }
    glm::vec3 enemyStructurePos(int index) const { return m_enemyStructures[index].entity->pos; }
    float enemyStructureHealth(int index) const { return m_enemyStructures[index].health; }
    float enemyStructureHealthMax(int index) const { return m_enemyStructures[index].maxHealth; }

    // The player's melee swing: damage every unit in the planar cone (called by GameMatch when
    // GamePlayer reports a swing; the physics shove happens in GamePlayer::tickCombat).
    void applyPlayerMelee(const glm::vec3& pos, const glm::vec3& dirPlanar, float range);

    int unitCount() const { return (int)m_units.size(); }
    glm::vec3 unitPos(int index) const;
    float unitHealth(int index) const { return m_units[index].health; }
    float unitHealthMax(int index) const; // per-type (Brute 3x, Runner 0.5x the tweak baseline)
    float unitEnergy(int index) const { return m_units[index].energy; }
    float unitEnergyMax(int index) const;
    float friendlyHealthMax() const { return m_unitHealth; } // friendlies use the Grunt baseline
    float friendlyEnergyMax() const { return m_unitEnergy; }
    int friendlyCount() const { return (int)m_friendlies.size(); }
    glm::vec3 friendlyPos(int index) const;
    float friendlyHealth(int index) const { return m_friendlies[index].health; }
    float friendlyEnergy(int index) const { return m_friendlies[index].energy; }

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
        uint32 sourceId = 0;        // FRIENDLY: the barracks that spawned it (alive-limit counting)
        glm::vec3 homePos{ 0.0f };  // FRIENDLY: leash anchor (the barracks position)
        ENpcType type = ENpcType::Grunt; // ENEMY: which stat row of the type table
        float fireTimer = 0.0f;     // SPITTER: cooldown until the next shot
    };
    struct Shot // enemy projectile (Spitter): team-1 mini-field sphere, damages structures on hit
    {
        EntityPtr entity;
        float age = 0.0f;
    };
    struct ShotHit // recorded by contact hooks (fire inside physics.update), processed next tick.
    {              // POINTER VALUES only — never dereferenced, so a same-frame despawn is safe.
        const Entity* shot;
        const Entity* victim;
    };

    enum class EEnemyStructType : uint8 { Core, Spawner, Turret };
    struct EnemyStructure
    {
        EntityPtr entity;
        EEnemyStructType type = EEnemyStructType::Core;
        int camp = 0;
        float health = 100.0f;
        float maxHealth = 100.0f;
        float timer = 0.0f; // spawner/turret cooldown
    };

    void spawnUnit(const glm::vec3& center, float ringRadius);
    void spawnUnitAt(const glm::vec3& pos, uint32 sourceId); // shared init (ring waves + camp spawners)
    void spawnFriendly(const glm::vec3& barracksPos, uint32 barracksId);
    void spawnEnemyStructure(EEnemyStructType type, const glm::vec3& pos, int camp);
    void damageByEntity(const Entity* entity, float amount); // projectile contact hook
    void damageEnemyStructByEntity(const Entity* entity, float amount);
    void fireSpitterShot(const glm::vec3& from, const glm::vec3& at);
    void tickShots(StructureSystem& structures, float deltaSec);

    std::vector<Unit> m_units;
    std::vector<Unit> m_friendlies;
    std::vector<EnemyStructure> m_enemyStructures;
    std::vector<Shot> m_shots;
    std::vector<Shot> m_turretShots;
    std::vector<ShotHit> m_pendingShotHits;
    std::unordered_map<uint32, float> m_barracksTimers; // per-barracks spawn cooldown (by stable id)
    std::unordered_map<uint32, float> m_turretTimers;   // per-turret fire cooldown (by stable id)
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
    // Friendlies ("Game/Friendlies") — shield/battery stats reuse the enemy "Unit *" tweaks.
    int m_barracksUnitLimit = 3;        // alive units per barracks
    float m_barracksSpawnInterval = 8.0f;
    float m_barracksSpawnEnergy = 8.0f; // spent from the barracks' internal store per spawn
    float m_friendlyAttackDps = 8.0f;   // enemy-unit health/s in attack range
    float m_friendlyAggroRadius = 14.0f; // sees enemy units within this of ITSELF
    float m_friendlyLeash = 22.0f;      // never chases beyond this of its barracks
    // Spitter (the ranged type)
    float m_spitterRange = 16.0f;       // stands off at this distance and shoots
    float m_spitterFireInterval = 3.0f;
    float m_spitterShotDamage = 10.0f;  // structure damage per hit
    float m_spitterShotSpeed = 18.0f;
    float m_spitterShotLifetime = 6.0f;
    float m_shotEmitterDrain = 15.0f;  // energy/s a shot saps from the nearest active emitter
                                       // while pressing near its bubble (transient but stacking —
                                       // sustained spitter fire is a real siege drain)
    float m_shotEmitterDrainRadius = 5.0f;
    // Turrets
    float m_turretRange = 18.0f;
    float m_turretFireInterval = 1.2f;
    float m_turretShotEnergy = 1.5f;    // spent from the turret's internal store per shot
    float m_turretShotSpeed = 30.0f;
    // Enemy camps
    int m_campCount = 3;
    float m_campDistance = 120.0f;      // ring radius around the Base
    float m_campCoreHealth = 300.0f;
    float m_campStructHealth = 120.0f;  // spawner/turret health
    float m_campSpawnInterval = 15.0f;
    int m_campUnitCap = 4;              // alive units per camp spawner
    float m_campTurretRange = 22.0f;
    float m_campTurretInterval = 2.5f;
};
