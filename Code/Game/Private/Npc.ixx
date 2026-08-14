export module Game:Npc;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import :Structures;

// Unit types = PREFAB variants: per-type stats are authored in the .pre's Component GameUnit
// (Entities/Game/enemyUnit/Brute/Runner/Spitter.pre); the shared sim baseline lives in
// GameUnitComponent::params (tweaked here).
export enum class ENpcType : uint8 { Grunt, Brute, Runner, Spitter, Count };

// A player body + its Force team — published to GameUnitComponent as the fallback target set, and
// the reach test for direct unit melee damage.
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
    uint8 kind = 0;         // 0 = unit, 2 = player (1/3/4 were the PvE kinds — values reserved)
    uint8 team = 0;         // Force team (bits 4-6 of the wire flags)
    float materialsFrac = 0.0f; // players only: carried construction stock (client HUD mirror)
};

// The unit/projectile PRODUCTION layer. The per-entity simulation itself (steering, shields,
// melee, lifetimes, contact damage) is GameUnitComponent/GameProjectileComponent inside the
// engine's entity pass — this system only SPAWNS actors (barracks, turrets, spitter fire
// requests), routes their outputs to the game (player damage, shield mirror, collapse edges) and
// save/loads them. NO entity lists: every lookup is a spatial query over the live world.
// All ticks main thread pre-physics (direct body setters sanctioned) — the authority seam.
export class NpcSystem final
{
public:
    void registerTweaks(); // barracks/turret production + the GameUnitComponent::params baseline
    void clear();          // despawns every unit/projectile entity (before world teardown)

    // Barracks produce their variant's ATTACK unit (per-barracks alive limit; each spawn pays the
    // per-type "<Type> spawn materials" from the conveyor-fed store; Constructor boost applies).
    void tickBarracks(StructureSystem& structures, float deltaSec);
    // TURRETS: fire team-tagged projectiles at the nearest enemy-team unit in range.
    void tickTurrets(StructureSystem& structures, float deltaSec);
    // The per-frame unit sweep (ONE arena spatial query, cached for the frame): services spitter
    // fire requests, folds collapse edges, and accumulates direct melee damage per player into
    // outPlayerDamage (the caller routes it — health is owner-computed).
    void tickUnits(std::span<const PlayerInfo> players, std::span<float> outPlayerDamage,
        StructureSystem& structures, float deltaSec);
    // This frame's units (filled by tickUnits — authority instances only): overhead labels etc.
    std::span<Entity* const> unitsThisFrame() const { return m_unitScratch; }

    void collectShieldStates(std::vector<ShieldNetState>& out) const; // over unitsThisFrame
    // A battery hit zero this tick (permanent collapse) — the mirror flushes immediately on it.
    bool takeShieldCollapseEdge() { const bool e = m_shieldCollapseEdge; m_shieldCollapseEdge = false; return e; }

    // SAVE/LOAD (server): every live unit into/from an AssetNode tree (projectiles are transient —
    // a load clears them). loadUnits despawns the live units first.
    void saveUnits(AssetNode& root) const;
    void loadUnits(const AssetNode& root, const StructureSystem& structures);

private:
    Entity* spawnUnit(const StructureSystem& structures, const glm::vec3& pos, uint32 sourceId,
        uint8 team, ENpcType type); // spawn + team/source/route setup on the component
    void fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
        const glm::vec3& velocity, uint8 team); // projectile spawn (main thread, pre-physics)

    std::vector<Entity*> m_unitScratch;  // THIS FRAME's arena query result (not owned state)
    // Spawn cooldowns and alive counts live ON the structures (GameStructureComponent::timer /
    // ::counter) — no id-keyed maps, and the state dies with its structure.
    bool m_shieldCollapseEdge = false;

    // Tweaks ("Game/Friendlies" — production; unit stats are prefab-authored, the shared sim
    // baseline is GameUnitComponent::params registered under "Game/Enemies")
    int m_barracksUnitLimit = 5;
    float m_barracksSpawnInterval = 8.0f;
    float m_spawnMaterials[(int)ENpcType::Count] = { 5.0f, 12.0f, 6.0f, 9.0f };
    float m_turretRange = 18.0f;
    float m_turretFireInterval = 1.2f;
    float m_turretShotEnergy = 1.5f;
    float m_turretShotSpeed = 30.0f;
    float m_spitterShotSpeed = 18.0f; // spitter fire requests are serviced here
};
