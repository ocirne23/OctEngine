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
// damage) through the component's event queues. This system only spawns actors and drains those
// queues. NOTHING IS ROSTERED HERE: the World's root list is the one owner of every unit and
// projectile entity, and everything that needs "every unit" walks it (queryAllUnits / the far
// tick; the COUNT alone comes from GameUnitComponent::liveCount, maintained at the component's
// spawn / destroy edges) — a root with a GameUnitComponent that is not a puppet IS a unit, a root with a
// GameProjectileComponent IS a shot. Unit shield/health state still syncs through the entity
// snapshot's game blob, the overhead labels run a frustum query at the point of need, and
// barracks roster COUNTS ride the spawn/death events.
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks(); // barracks/turret production + the GameUnitComponent::params baseline
    // TEARDOWN: wipes the ENTIRE World — every root (structures, projectiles, units, player
    // capsules, terrain, ground) through World::clearRootEntities — and discards whatever the
    // removed actors left in the component queues. ~GameMatch calls it after every other holder
    // dropped its EntityPtrs, so the World's batch release is the last reference and the teardown
    // fans out over the job system. Not the load path: loadUnits despawns only units + shots.
    void clear();

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
    // Every live unit = every World root with a GameUnitComponent that is not a puppet (player
    // capsules are puppets — never units). A walk of World::rootEntities(), O(roots): save/load
    // and the profiling scenario only. Main thread, or a post-update job (the root list only
    // mutates on main, after those jobs join).
    static void queryAllUnits(oc::vector<Entity*>& out);
    // The live unit COUNT is no walk at all: GameUnitComponent keeps it at its spawn / destroy
    // edges (liveCount). O(1) from any thread — the HUD, the wave cap, the wander budget.
    static int countUnits();

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
    // a load clears them). loadUnits despawns the live units + shots first (a root walk).
    void saveUnits(AssetNode& root) const;
    void loadUnits(const AssetNode& root, StructureSystem& structures); // re-seeds roster counts

private:
    Entity* spawnUnit(const StructureSystem& structures, const glm::vec3& pos, uint32 sourceId,
        uint8 team, ENpcType type); // spawn + team/source/route setup on the component
    void fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
        const glm::vec3& velocity, uint8 team); // projectile spawn (main thread, pre-physics)
    static void despawnUnitsAndShots(); // every unit + projectile root out of the World (load path)
    void discardQueued(); // drops every queued report/request the removed actors left behind

public:
    // STRIKE VISUALS — pure visuals here (the components already landed the damage):
    //   Turret   — the hitscan lightning: a jagged bundle over "Turret beam lifetime", plus a
    //              muzzle flash.
    //   MeleeHit — a unit's swing: a plain line striker -> victim over "Melee hit lifetime".
    // (No impact flash: the VICTIM lights itself on its health drop — GameUnitComponent's hurt
    // light — on every role, which also covers shells and force-field exposure.)
    // A FLASH is a temporary point light: addBeam spawns them, drawBeams pushes each to the
    // renderer every frame with its intensity fading over its life (per-frame light records, so
    // nothing is owned). The server broadcasts this frame's new beams (GLt) so clients add the
    // same ones — melee hits capped per frame (c_maxHitBroadcast), turret strikes always.
    enum class EBeamKind : uint8 { Turret, MeleeHit };
    struct Beam
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
        float ttl = 0.0f;
        float life = 0.0f; // the lifetime it started with (the fade's denominator)
        EBeamKind kind = EBeamKind::Turret;
    };
    void addBeam(const glm::vec3& from, const glm::vec3& to, EBeamKind kind);
    oc::span<const Beam> newBeams() const { return m_newBeams; } // added since the last service()
    void drawBeams(float deltaSec); // ages + draws beams AND flashes (main thread, every windowed frame)
private:
    struct Flash
    {
        glm::vec3 pos{ 0.0f };
        glm::vec3 color{ 1.0f };
        float range = 3.0f;
        float intensity = 10.0f; // peak; fades linearly to 0 over `life`
        float ttl = 0.0f;
        float life = 0.0f;
    };
    void addFlash(const glm::vec3& pos, const glm::vec3& color, float range, float intensity, float life);
    static constexpr size_t c_maxFlashes = 256;      // the light grid is finite: past this the OLDEST flash goes
    static constexpr size_t c_maxHitBroadcast = 64;  // melee hit beams relayed to clients per service()

    // Spawn cooldowns and alive counts live ON the barracks (GameStructureComponent::barracks) —
    // no id-keyed maps, and the state dies with its structure. No entity lists here at all — see
    // the class comment.
    oc::vector<GameUnitComponent::FireRequest> m_fireScratch; // drained queues (reused buffers)
    oc::vector<GameUnitComponent::DeathRecord> m_deathScratch;
    oc::vector<GameUnitComponent::SeedRequest> m_seedScratch;
    // FAR TICK (service): units the SIM LOD left unselected walk their orders by
    // GameUnitComponent::updateFar every m_farInterval seconds of sim time — a parallelFor over
    // the World's root list, skipping non-units.
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

    oc::vector<GameUnitComponent::HitRecord> m_hitScratch;

    oc::vector<Beam> m_beams;
    oc::vector<Beam> m_newBeams;
    oc::vector<Flash> m_flashes;

    // Tweaks (unit stats are prefab-authored; the shared sim baselines live on the components'
    // params — barracks/turret production tuning now registers from StructureSystem. What remains
    // here is the spitter SHOT SPEED, applied when this system services the fire queue, and the
    // strike visuals.)
    float m_spitterShotSpeed = 18.0f;
    float m_lobberShotSpeed = 14.0f; // ShotKind 1: the slow splash shell
    float m_beamLifetime = 0.5f;     // turret lightning
    float m_hitLifetime = 0.25f;     // melee hit line + its flash
    float m_flashIntensity = 1.0f;   // multiplier on every flash's peak
};
