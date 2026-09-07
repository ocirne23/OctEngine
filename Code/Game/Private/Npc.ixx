export module Game:Npc;

import Core;
import Core.glm;
import Core.Camera;
import Entity;
import File; // AssetNode (save/load)
import :Structures;

// Unit types = PREFAB variants: per-type stats are authored in the .pre's Component GameUnit
// (Entities/Game/enemyUnit/Brute/Runner/Spitter/swarmUnit.pre); the shared sim baseline lives in
// GameUnitComponent::params (tweaked here). SWARM is the CHEAP body: health only — no Force
// emitter, no shield battery (the co-op waves are built from it; see GameMatch's coop block).
// SAVE FILES store the type as an int — APPEND only. Elite/Giant/Titan/Lobber are the late-wave
// ELITE tier (enemy-only: not barracks options — see isBarracksUnitType); the Lobber is the ranged
// one, firing SPLASH shots (enemyLob.pre, ShotKind 1).
// The Spawner is a "ranged" unit whose ShotKind 2 "shot" is a SWARM body spawned next to it.
// The Warrior is the SHIELDED grunt (the Grunt/Runner/Swarm are health-only bodies).
export enum class ENpcType : uint8 { Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant, Titan, Lobber, Spawner, Warrior, Count };
// The barracks' unit-type selection + its per-type price tables index by THIS order.
static_assert((int)ENpcType::Count == GameNumUnitTypes);

// The unit/projectile PRODUCTION layer. The per-entity simulation itself (steering, shields,
// melee, lifetimes, contact damage) is GameUnitComponent/GameProjectileComponent inside the
// engine's entity pass, and a unit REPORTS what the game needs (shots to spawn, its death, player
// damage) through the component's event queues. This system only spawns actors, drains those
// queues, and keeps the ROSTERS (owning EntityPtrs of every unit/projectile it spawned — added at
// spawn, deregistered by World::removeRootEntity's callback via onWorldRootRemoved, so every
// despawn path is covered and no world-wide query exists anywhere). Unit shield/health state still
// syncs through the entity snapshot's game blob, the overhead labels run a frustum query at the
// point of need, and barracks roster COUNTS ride the spawn/death events.
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks(); // barracks/turret production + the GameUnitComponent::params baseline
    void clear();          // despawns every unit/projectile entity (before world teardown)

    // Drains everything the per-entity sims queued during the pass: barracks spawn requests (the
    // BARRACKS decided, paid energy and claimed its roster slot in its own component update —
    // this only performs the main-thread entity spawn, refunding on failure), turret + spitter
    // shots, and unit deaths (freeing the spawner's roster slot). Player damage needs no queue:
    // it lands on the victim's puppet component (GameUnitComponent::pendingDamage) via the same
    // damage() call as every other victim, and the game drains it to the owning instance.
    void service(StructureSystem& structures);

    // Units inside the view frustum AND within maxDist of the camera — the overhead labels only
    // draw what is on screen and readable, so they never ask for more than that.
    static void queryVisibleUnits(const Camera& camera, float maxDist, oc::vector<Entity*>& out);
    void queryAllUnits(oc::vector<Entity*>& out) const; // roster walk: save/load + the profiling scenario
	int getNumUnits() const { return (int)m_units.size(); }

    // World::removeRootEntity notification (wired by GameMatch): drops the unit/projectile roster
    // entry for ANY despawn path (death destroy request, network despawn, editor delete). Must NOT
    // call removeRootEntity (see World.ixx).
    void onWorldRootRemoved(const Entity* entity);
    oc::span<const EntityPtr> units() const { return m_units; } // feedNav's per-team sources

    // A unit with no owning barracks (sourceId 0, no route, no death accounting) — the co-op
    // ambient scatter + wave director's entry point. Returns null on spawn failure.
    Entity* spawnLooseUnit(const StructureSystem& structures, const glm::vec3& pos, uint8 team,
        ENpcType type);

    // BATCHED loose-unit spawn — the co-op trickle's path: one frame's rolled wave + ambient
    // spawns materialize through World::spawnBatch (entity creation fanned out over the job
    // system), then team/order/roster fixup runs serially after the join. Main thread, in
    // game.update (the legal spawn window).
    struct LooseSpawn
    {
        glm::vec3 pos = glm::vec3(0.0f);
        glm::vec3 orderDest = glm::vec3(0.0f); // marched to when hasOrder (wave units)
        ENpcType type = ENpcType::Grunt;
        uint8 team = 0;
        bool hasOrder = false;
    };
    void spawnLooseUnits(oc::span<const LooseSpawn> spawns);

    // SAVE/LOAD (server): every live unit into/from an AssetNode tree (projectiles are transient —
    // a load clears them). loadUnits despawns the live units first.
    void saveUnits(AssetNode& root) const;
    void loadUnits(const AssetNode& root, StructureSystem& structures); // re-seeds roster counts

private:
    Entity* spawnUnit(const StructureSystem& structures, const glm::vec3& pos, uint32 sourceId,
        uint8 team, ENpcType type); // spawn + team/source/route setup on the component
    void fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
        const glm::vec3& velocity, uint8 team); // projectile spawn (main thread, pre-physics)

public:
    // TURRET LIGHTNING: hitscan strikes are pure visuals here (the component already landed the
    // damage). Each lives "Turret beam lifetime" seconds as a jagged debug line. The server
    // broadcasts this frame's new ones (GLt) so clients add the same beams.
    struct Beam
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
        float ttl = 0.0f;
    };
    void addBeam(const glm::vec3& from, const glm::vec3& to);
    oc::span<const Beam> newBeams() const { return m_newBeams; } // added since the last service()
    void drawBeams(float deltaSec); // ages + draws (main thread, every windowed frame)
private:

    // Spawn cooldowns and alive counts live ON the barracks (GameStructureComponent::barracks) —
    // no id-keyed maps, and the state dies with its structure.
    // The rosters: owning refs, maintained by spawn + onWorldRootRemoved (never queried).
    oc::vector<EntityPtr> m_units;
    oc::vector<EntityPtr> m_shots;
    oc::vector<GameUnitComponent::FireRequest> m_fireScratch; // drained queues (reused buffers)
    oc::vector<GameUnitComponent::DeathRecord> m_deathScratch;
    oc::vector<GameUnitComponent::SeedRequest> m_seedScratch;
    // FAR TICK (service): units the SIM LOD left unselected walk their orders by
    // GameUnitComponent::updateFar every m_farInterval seconds of sim time.
    float m_farInterval = 0.5f;
    float m_farAccum = 0.0f;
    int m_farTicked = 0; // live readout: units moved by the last far tick
    // Seeded-lane strength, split by WHO asked for it: a player ORDER (move command, barracks
    // route) writes a strong, wide lane the whole group should commit to, while a STUCK unit's
    // request is a hint — weak and narrow enough that it bends the crowd around the jam without
    // overriding what everyone else is doing.
    float m_orderLaneSpeed = 10.0f;
    float m_stuckLaneSpeed = 10.0f;
    float m_laneWidth = 3.0f; // metres PAINTED (0 = one cell)
public:
    float orderLaneSpeed() const { return m_orderLaneSpeed; }
    float stuckLaneSpeed() const { return m_stuckLaneSpeed; }
    float laneWidth() const { return m_laneWidth; }
private:
    oc::vector<uint32> m_spawnScratch;
    oc::vector<GameStructureComponent::TurretFireRequest> m_turretFireScratch;

    oc::vector<Beam> m_beams;
    oc::vector<Beam> m_newBeams;

    // Tweaks (unit stats are prefab-authored; the shared sim baselines live on the components'
    // params — barracks/turret production tuning now registers from StructureSystem. What remains
    // here is the spitter SHOT SPEED, applied when this system services the fire queue, and the
    // turret beam visual.)
    float m_spitterShotSpeed = 18.0f;
    float m_lobberShotSpeed = 14.0f; // ShotKind 1: the slow splash shell
    float m_beamLifetime = 0.5f;
};
