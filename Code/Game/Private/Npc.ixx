export module Game:Npc;

import Core;
import Core.glm;
import Core.Camera;
import Entity;
import File; // AssetNode (save/load)
import :Structures;

// Unit types = PREFAB variants: per-type stats are authored in the .pre's Component GameUnit
// (Entities/Game/enemyUnit/Brute/Runner/Spitter.pre); the shared sim baseline lives in
// GameUnitComponent::params (tweaked here).
export enum class ENpcType : uint8 { Grunt, Brute, Runner, Spitter, Count };

// The unit/projectile PRODUCTION layer. The per-entity simulation itself (steering, shields,
// melee, lifetimes, contact damage) is GameUnitComponent/GameProjectileComponent inside the
// engine's entity pass, and a unit REPORTS what the game needs (shots to spawn, its death, player
// damage) through the component's event queues. This system only spawns actors and drains those
// queues. It holds NO unit state of any kind: unit shield/health state syncs through the entity
// snapshot's game blob, the overhead labels run a frustum query at the point of need, and barracks
// roster counts ride the spawn/death events.
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks(); // barracks/turret production + the GameUnitComponent::params baseline
    void clear();          // despawns every unit/projectile entity (before world teardown)

    // Drains everything the per-entity sims queued during the pass: barracks spawn requests (the
    // BARRACKS decided, paid minerals and claimed its roster slot in its own component update —
    // this only performs the main-thread entity spawn, refunding on failure), turret + spitter
    // shots, and unit deaths (freeing the spawner's roster slot). Player damage needs no queue:
    // it lands on the victim's puppet component (GameUnitComponent::pendingDamage) via the same
    // damage() call as every other victim, and the game drains it to the owning instance.
    void service(StructureSystem& structures);

    // Units inside the view frustum — the overhead labels only draw what is on screen, so they
    // never ask for more than that.
    static void queryVisibleUnits(const Camera& camera, oc::vector<Entity*>& out);

    // SAVE/LOAD (server): every live unit into/from an AssetNode tree (projectiles are transient —
    // a load clears them). loadUnits despawns the live units first.
    void saveUnits(AssetNode& root) const;
    void loadUnits(const AssetNode& root, StructureSystem& structures); // re-seeds roster counts

private:
    Entity* spawnUnit(const StructureSystem& structures, const glm::vec3& pos, uint32 sourceId,
        uint8 team, ENpcType type); // spawn + team/source/route setup on the component
    void fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
        const glm::vec3& velocity, uint8 team); // projectile spawn (main thread, pre-physics)

    // Spawn cooldowns and alive counts live ON the barracks (GameStructureComponent::barracks) —
    // no rosters, no id-keyed maps, and the state dies with its structure.
    oc::vector<GameUnitComponent::FireRequest> m_fireScratch; // drained queues (reused buffers)
    oc::vector<uint32> m_deathScratch;
    oc::vector<uint32> m_spawnScratch;
    oc::vector<GameStructureComponent::TurretFireRequest> m_turretFireScratch;

    // Tweaks (unit stats are prefab-authored; the shared sim baselines live on the components'
    // params — barracks/turret production tuning now registers from StructureSystem. What remains
    // here are the SHOT SPEEDS, applied when this system services the fire queues.)
    float m_turretShotSpeed = 30.0f;
    float m_spitterShotSpeed = 18.0f;
};
