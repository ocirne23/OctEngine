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

// A player body + its Force team — the targeting input for units and camp turrets (PvE players
// are all team 0; PvP units only hunt OTHER teams' players).
export struct PlayerInfo
{
    glm::vec3 pos{ 0.0f };
    uint8 team = 0;
};

// One netId-keyed record of the co-op shield mirror ("GSh", server -> clients): enough for a
// receiving instance to drive the remote actor's bubble output and draw its overhead bars.
export struct ShieldNetState
{
    uint32 netId = 0;
    float healthFrac = 1.0f;
    float energyFrac = 1.0f;
    float output = 0.0f;    // live emitter output (world units; u8-quantized on the wire)
    bool collapsed = false;
    uint8 kind = 0;         // 0 = enemy unit, 1 = friendly unit, 2 = player, 3 = enemy camp
                            // structure, 4 = enemy camp CORE (3/4 are HEALTH ONLY — receivers must
                            // not touch their emitter: the core's bubble is authored constant,
                            // output 0 would stomp it; 4 additionally drives the camp ring marker)
    uint8 team = 0;         // Force team (bits 4-6 of the wire flags) — receivers color bars
                            // own-team green / enemy red
    float materialsFrac = 0.0f; // players only: carried construction stock (client HUD mirror)
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
    void setPvp(bool pvp) { m_pvp = pvp; } // PvP: no ambient waves — barracks are the only source
    void clear(); // drops every unit (before world teardown)

    // Spawns arrive on a ring: spawnRingRadius (the field frontier, from GameMatch) + the "Spawn
    // margin" tweak — always just inside the enemy field, all around the safe zone.
    // players = EVERY player body + team (server's own + each client capsule) — units fall back
    // to the nearest ENEMY player when no structure tempts them, and camp turrets shoot the
    // nearest player in range; a single server-player position left client players invisible.
    void tickAuthority(const glm::vec3& spawnCenter, float spawnRingRadius,
        std::span<const PlayerInfo> players, StructureSystem& structures, float deltaSec);
    // PvP: every barracks produces ATTACK units of its own team into the shared unit sim — they
    // seek out enemy structures/players like PvE waves do, instead of guarding home.
    void tickPvpBarracks(StructureSystem& structures, float deltaSec);
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
    void tickEnemyStructures(StructureSystem& structures, std::span<const PlayerInfo> players,
        float deltaSec);
    int enemyStructureCount() const { return (int)m_enemyStructures.size(); }
    bool enemyStructureIsCore(int index) const { return m_enemyStructures[index].type == EEnemyStructType::Core; }
    glm::vec3 enemyStructurePos(int index) const { return m_enemyStructures[index].entity->pos; }
    float enemyStructureHealth(int index) const { return m_enemyStructures[index].health; }
    float enemyStructureHealthMax(int index) const { return m_enemyStructures[index].maxHealth; }

    // The player's melee swing: damage every ENEMY-team unit in the planar cone (called by
    // GameMatch when GamePlayer reports a swing; the physics shove happens in tickCombat).
    void applyPlayerMelee(const glm::vec3& pos, const glm::vec3& dirPlanar, float range, uint8 team = 0);

    int unitCount() const { return (int)m_units.size(); }
    uint8 unitTeam(int index) const { return m_units[index].team; }
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

    // CO-OP: appends every unit's + friendly's shield record for the server's GSh mirror
    // (entities without a netId — single player / local-inert — are skipped).
    void collectShieldStates(std::vector<ShieldNetState>& out) const;
    // A battery hit zero this tick (permanent collapse). The mirror flushes immediately on it, so
    // clients see the bubble drop the same tick instead of at the next periodic send.
    bool takeShieldCollapseEdge() { const bool e = m_shieldCollapseEdge; m_shieldCollapseEdge = false; return e; }

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
        uint8 team = 1;             // Force team (PvE waves 1; PvP barracks units their builder's)
        float stuckTimer = 0.0f;    // seconds of wanting to move but barely moving (blocked by a
                                    // structure/wall — there is no pathfinding)
        float avoidTimer = 0.0f;    // sidestep steering remaining, armed by the stuck detector
        float avoidSign = 1.0f;     // which way the sidestep goes (rolled per detour)
    };
    struct Shot // spitter projectile: mini-field sphere, damages ENEMY-team structures on hit
    {
        EntityPtr entity;
        float age = 0.0f;
        uint8 team = 1;
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
    // ring waves + camp spawners (weighted type roll) + barracks (forceType = the variant's unit)
    void spawnUnitAt(const glm::vec3& pos, uint32 sourceId, uint8 team = 1,
        ENpcType forceType = ENpcType::Count);
    void spawnFriendly(const StructureSystem& structures, const glm::vec3& barracksPos, uint32 barracksId);
    void spawnEnemyStructure(EEnemyStructType type, const glm::vec3& pos, int camp);
    void damageByEntity(const Entity* entity, float amount); // projectile contact hook
    void damageEnemyStructByEntity(const Entity* entity, float amount);
    void fireSpitterShot(const glm::vec3& from, const glm::vec3& at, uint8 team = 1);
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
    bool m_pvp = false;        // see setPvp
    bool m_shieldCollapseEdge = false; // set where a battery empties; polled by GameMatch

    // Tweaks ("Game/Enemies")
    float m_unitHealth = 60.0f;
    float m_moveSpeed = 4.0f;
    float m_accel = 15.0f; // soft on purpose: units must lose the shoving match against bubbles
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
    float m_fieldTension = 1.5f;      // surface tension: unit push + battery drain scale by
                                      // (1 + tension * pressure) — mirrors the player shield
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
    float m_barracksSpawnMaterials = 10.0f; // MATERIALS spent from the barracks' conveyor-fed
                                            // mineral store per spawned unit
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
