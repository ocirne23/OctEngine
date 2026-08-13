export module Game:Npc;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import :Structures;

// Unit types: stat MULTIPLIERS over the "Game/Enemies" "Unit *" tweak baseline (the Grunt).
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
};

// A player body + its Force team — the targeting input for units (they only hunt OTHER teams'
// players).
export struct PlayerInfo
{
    glm::vec3 pos{ 0.0f };
    uint8 team = 0;
};

// One netId-keyed record of the shield mirror ("GSh", server -> clients): enough for a receiving
// instance to drive the remote actor's bubble output and draw its overhead bars.
export struct ShieldNetState
{
    uint32 netId = 0;
    float healthFrac = 1.0f;
    float energyFrac = 1.0f;
    float output = 0.0f;    // live emitter output (world units; u8-quantized on the wire)
    bool collapsed = false;
    uint8 kind = 0;         // 0 = unit, 2 = player (1/3/4 were the PvE friendly/camp kinds — the
                            // values stay reserved so old captures read sanely)
    uint8 team = 0;         // Force team (bits 4-6 of the wire flags) — receivers color bars
                            // own-team green / enemy red
    float materialsFrac = 0.0f; // players only: carried construction stock (client HUD mirror)
};

// Barracks-produced attack units: each barracks VARIANT spawns its own unit type on its team,
// paying MATERIALS from its conveyor-fed store; units steer toward enemy structures/players and
// damage them in reach. Each carries a small team bubble, so enemy emitter fields brake and shove
// them (applied-force readback -> impulse) and their presence pressures enemy shields. Killed by
// enemy-team projectile contacts (ContactEvents -> onContact).
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks();
    void clear(); // drops every unit (before world teardown)

    // players = EVERY player body + team (server's own + each client capsule) — units fall back
    // to the nearest ENEMY player when no structure tempts them.
    // outPlayerDamage (same length as players): DIRECT melee damage units dealt to each player
    // this tick — the caller routes it to the owning instance (health is owner-computed).
    void tickAuthority(std::span<const PlayerInfo> players, std::span<float> outPlayerDamage,
        StructureSystem& structures, float deltaSec);
    // Every barracks produces ATTACK units of its own team into the shared unit sim.
    void tickBarracks(StructureSystem& structures, float deltaSec);
    // TURRETS: fire player-type projectiles ("Projectile" name = the unit contact-damage hook) at
    // the nearest enemy-team unit in range, spending turret energy per shot.
    void tickTurrets(StructureSystem& structures, float deltaSec);

    int unitCount() const { return (int)m_units.size(); }
    uint8 unitTeam(int index) const { return m_units[index].team; }
    ENpcType unitType(int index) const { return m_units[index].type; }
    glm::vec3 unitPos(int index) const;
    float unitHealth(int index) const { return m_units[index].health; }
    float unitHealthMax(int index) const; // per-type (Brute 3x, Runner 0.5x the tweak baseline)
    float unitEnergy(int index) const { return m_units[index].energy; }
    float unitEnergyMax(int index) const;

    // CO-OP: appends every unit's shield record for the server's GSh mirror (entities without a
    // netId — single player / local-inert — are skipped).
    void collectShieldStates(std::vector<ShieldNetState>& out) const;
    // A battery hit zero this tick (permanent collapse). The mirror flushes immediately on it, so
    // clients see the bubble drop the same tick instead of at the next periodic send.
    bool takeShieldCollapseEdge() { const bool e = m_shieldCollapseEdge; m_shieldCollapseEdge = false; return e; }

    // SAVE/LOAD (server): every unit into/from an AssetNode tree (shots are transient — a load
    // clears them). loadUnits clears the live units first; units respawn at their saved position.
    void saveUnits(AssetNode& root) const;
    void loadUnits(const AssetNode& root);

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
        uint32 sourceId = 0;        // the barracks that spawned it (alive-limit counting + routes)
        ENpcType type = ENpcType::Grunt; // which stat row of the type table
        float fireTimer = 0.0f;     // SPITTER: cooldown until the next shot
        uint8 team = 1;             // Force team (the spawning barracks builder's)
        uint32 routeIndex = 0;      // next waypoint of the spawning barracks' route (>= size = done)
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

    // barracks variants force their unit type; save/load restores it
    void spawnUnitAt(const glm::vec3& pos, uint32 sourceId, uint8 team, ENpcType type);
    void damageByEntity(const Entity* entity, float amount); // projectile contact hook
    void fireSpitterShot(const glm::vec3& from, const glm::vec3& at, uint8 team = 1);
    void tickShots(StructureSystem& structures, float deltaSec);

    std::vector<Unit> m_units;
    std::vector<Shot> m_shots;
    std::vector<Shot> m_turretShots;
    std::vector<ShotHit> m_pendingShotHits;
    std::unordered_map<uint32, float> m_barracksTimers; // per-barracks spawn cooldown (by stable id)
    std::unordered_map<uint32, float> m_turretTimers;   // per-turret fire cooldown (by stable id)
    bool m_shieldCollapseEdge = false; // set where a battery empties; polled by GameMatch

    // Tweaks ("Game/Enemies") — the shared unit-stat baseline (both teams' units use it)
    float m_unitHealth = 60.0f;
    float m_moveSpeed = 4.0f;
    float m_accel = 15.0f; // soft on purpose: units must lose the shoving match against bubbles
    float m_attackRange = 1.5f;    // must actually reach the structure — a powered emitter's field
                                   // pushes them out well past this, so damage is purely proximity
    float m_attackDps = 6.0f;      // structure health/s while in range
    float m_playerAttackDps = 10.0f; // PLAYER health/s while an enemy unit is in melee reach
    float m_retargetInterval = 8.0f;
    float m_projectileDamage = 25.0f;
    float m_fieldPushGain = 20000.0f; // enemy fields shoving units (force-ball scale)
    float m_fieldTension = 1.5f;      // surface tension: unit push + battery drain scale by
                                      // (1 + tension * pressure) — mirrors the player shield
    float m_unitEnergy = 40.0f;      // shield battery (no regen)
    float m_unitEnergyDrainRate = 25.0f; // energy/s per unit of pressure (player rule)
    float m_unitShieldOutput = 0.8f; // bubble output while the battery lives (collapse -> sentinel)
    float m_unitDamageRadius = 0.6f; // equilibrium radius below this + pressure = health drain
    float m_unitFieldDps = 10.0f;    // health/s while exposed inside enemy fields
    float m_emitterDrainPerUnit = 5.0f; // energy/s a shielded unit costs an emitter at zero range
    float m_emitterDrainRadius = 12.0f;  // falloff radius of that drain (~the emitter bubble)
    // Barracks ("Game/Friendlies") — production settings; unit stats reuse the baseline above.
    int m_barracksUnitLimit = 5;        // alive units per barracks
    float m_barracksSpawnInterval = 8.0f;
    float m_spawnMaterials[(int)ENpcType::Count] = { 5.0f, 12.0f, 6.0f, 9.0f }; // MATERIALS a
                                            // barracks spends from its conveyor-fed store per
                                            // spawned unit, PER TYPE (Grunt/Brute/Runner/Spitter)
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
};
