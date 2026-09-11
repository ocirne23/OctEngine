export module Entity:GameStructureComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;

export struct GameStructureParams
{
    float fieldDamageRate = 6.0f; // health/s while an enemy team's bubble owns the query point
    float turretRange = 18.0f;
    float turretFireInterval = 1.2f;
    float turretShotEnergy = 2.0f; // keep a WHOLE number: the cable transport delivers whole cells
                                   // into a store of this capacity, so 1.5 stalled one cell short
    float turretDamage = 25.0f;
    float medicRange = 12.0f;
    float medicHealRate = 4.0f;    // health/s AND battery energy/s per body (stations stack)
};

// Authority only (clients early-out of update). Cross-entity writes are the atomic damage()/addLoad().
// The cable transport moves resources in whole cells; the game's StructureSystem owns that, this
// component only holds the float stores it reserves from / adds into at its tick boundary.
export struct GameStructureComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameStructure; }
    ~GameStructureComponent() {}

    static GameStructureParams params;

    struct SpawnInfo
    {
        uint32 team = 0;
        float healthMax = 100.0f;
        bool invulnerable = false; // the Base: skipped by every damage path
        float meleeRadius = 1.0f;  // footprint half — units gnaw against this ring, not the center
        bool alwaysDisplayHealth = false;
        bool alwaysShowResources = false; // overhead store bars even unselected
    };

    uint32 structureId = 0;    // stable game-minted id (selection, mirror wire, save files)
    uint8 team = 0;
    // Flags share one byte and are MAIN-THREAD-ONLY: a bitfield write is a read-modify-write of the
    // byte, so a flag written from the parallel pass would clobber its neighbours.
    uint8 blueprint : 1 = 0;      // inert ghost until materials heal it to full; health IS the build progress
    uint8 invulnerable : 1 = 0;
    uint8 strainable : 1 = 0;     // game marks ACTIVE emitters; units deposit load on the nearest
    uint8 powered : 1 = 0;        // consumers: last production tick's draw was paid
    uint8 alwaysDisplayHealth : 1 = 0;
    uint8 alwaysShowResources : 1 = 0;
    float health = 100.0f, healthMax = 100.0f;
    float meleeRadius = 1.0f;
    float bubbleRadius = 0.0f; // active emitters, game-stamped (main-write, worker-read): shield-less
                               // units take field damage inside it when the bake is off
    float store[3] = {};       // energy, fuel, minerals
    float capacity[3] = {};    // 0 = this structure does not carry the medium; game-stamped per tick
    uint8 attachedMask = 0;    // bit m = a cable network of medium m touches this structure
    float flowUtil = 0.0f;     // EMA of the served fraction of the transport demand (gauge, game-stamped)
    // Outside the union on purpose: a non-trivial union member needs manual construct/destruct and
    // there is no type tag here to pick it by.
    oc::vector<glm::vec3> route;

    static constexpr int MaxRoutePoints = 6;
    struct BarracksData
    {
        // No spawn timer: the energy store IS the build bar (capacity = spawnCost).
        int population;     // += spawnPop per spawn decision, -= popCost by the game per death event
        int popCap;         // own allowance + linked houses, game-stamped each tick
        uint8 unitType;     // the produced type (a player order — synced/saved by the game)
        uint8 spawnPop;
        uint8 houses;
        float spawnCost;
    };
    struct TurretData
    {
        uint8 unused; // the fire clock is the energy store (see update)
    };
    // The union's active variant, game-stamped from the structure's type. Cross-variant reads are
    // bugs (the NetEntityState role-union pattern); all variants are trivially destructible.
    enum class EMachineKind : uint8 { None, Barracks, Turret, Medic };
    EMachineKind machineKind = EMachineKind::None;

    struct TurretFireRequest // a strike that already landed: from the muzzle to the victim
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 target{ 0.0f };
        uint8 team = 0;
    };
    static void takeSpawnRequests(oc::vector<uint32>& outStructureIds); // barracks wanting a unit
    static void takeTurretFireRequests(oc::vector<TurretFireRequest>& out);
    struct EmitterData
    {
        float outputFrac;       // smoothed output fraction (shrink/grow ramps)
        float outputFracTarget; // client mirror: the last synced fraction, eased toward
        bool down;              // latched off until the store refills to the restart charge
        float unitLoad;         // energy/s deposited by enemy units/shots (addLoad); the game's
                                // production tick consumes and clears it
    };
    union
    {
        BarracksData barracks{};
        TurretData turret;
        EmitterData emitter;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec);
    void damage(float amount); // atomic; no-op while invulnerable
    void fieldDrain(float amount) { damage(amount); }
    void addLoad(float energyPerSec); // atomic unitLoad deposit (workers)
    bool alive() const { return health > 0.0f; }
};

export const GameStructureComponent::SpawnInfo* getGameStructureSpawnInfo(const Entity* entity);
export void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out);
