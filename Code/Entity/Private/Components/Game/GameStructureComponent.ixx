export module Entity:GameStructureComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;

// The game-layer STRUCTURE component. See GameUnitComponent.ixx for the design shared by the three
// game components and the authority/thread contract.

export struct GameStructureParams
{
    float fieldDamageRate = 6.0f; // health/s while an enemy team's bubble owns the query point
    // Shared production tuning (the game's tweaks point here; per-TYPE values are stamped
    // per-instance instead — e.g. BarracksData::spawnCost). The barracks' build TIME is not a
    // parameter here: it is cost / the cable intake the game caps its links to.
    float turretRange = 18.0f;
    float turretFireInterval = 1.2f;
    float turretShotEnergy = 2.0f; // spent from the turret's own energy store per shot — a WHOLE
                                   // number: the cable transport delivers whole cells into a
                                   // store whose capacity is this, so 1.5 stalled one cell short
    float turretDamage = 25.0f;    // per hitscan lightning strike (never misses)
    float medicRange = 12.0f;      // a powered Medic station heals own-team units inside this
    float medicHealRate = 4.0f;    // health/s AND battery energy/s per body (stations stack)
};

// A building: team + health (health IS the construction progress while `blueprint`), a territory
// bake tap that drains health inside enemy-owned field, the melee/emitter-load intake the units'
// C++ pushes into, AND the endpoint of the CABLE TRANSPORT: three FLOAT stores (energy/fuel/
// minerals) with capacities. Resources move over the cable networks in WHOLE CELLS, owned and
// ticked by the game's StructureSystem (its transport job: per-segment integer fills, a rate per
// segment, producers push / consumers pull / storage by fill hysteresis). The component never
// moves anything itself: the game reserves cells out of / adds cells into these stores at its
// tick boundary, main-thread, and `flowUtil` is the gauge the game stamps from the served
// fraction. PRODUCTION (income, fuel burn, consumer drain, emitter ramps) also stays in the game's
// StructureSystem — per-entity state, no roster here.
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
        bool alwaysDisplayHealth = false; // overhead health bar shows even at full health
        bool alwaysShowResources = false; // overhead STORE bars (energy/fuel/minerals) show even
                                          // unselected — the stores a player watches at a glance
                                          // (storage, emitters, barracks); everything else shows
                                          // them only while selected
    };

    uint32 structureId = 0;    // stable game-minted id (selection, mirror wire, save files)
    uint8 team = 0;            // owning Force team (< GameMaxTeams)
    // The flags share one byte, and every one of them is MAIN-THREAD-ONLY (the game's tick, which
    // does not overlap the parallel entity pass). A bitfield write is a read-modify-write of the
    // whole byte, so a flag written from the pass would clobber its neighbours — such a flag must
    // live outside this byte.
    uint8 blueprint : 1 = 0;      // inert ghost until materials heal it to full (game sets/clears)
    uint8 invulnerable : 1 = 0;   // the Base: skipped by every damage path
    uint8 strainable : 1 = 0;     // game marks ACTIVE emitters; units deposit load on the nearest
    uint8 powered : 1 = 0;        // consumers: last production tick's draw was paid
    uint8 alwaysDisplayHealth : 1 = 0; // overhead health bar even at full (spawn-copied, main-read)
    uint8 alwaysShowResources : 1 = 0; // overhead STORE bars even unselected (spawn-copied, main-read)
    float health = 100.0f, healthMax = 100.0f;
    float meleeRadius = 1.0f;
    float bubbleRadius = 0.0f; // ACTIVE emitters: the current visible bubble radius, stamped by
                               // the game next to `strainable` (main-write, worker-read — a gauge
                               // like flowUtil). SHIELD-LESS units (no ForceComponent — the swarm
                               // types) take field exposure damage inside it: without an emitter
                               // of their own, the GPU pressure/push readback path does not exist.
    // ---- the transport endpoint (game stamps capacity per tick so tweaks stay live) ----
    float store[3] = {};       // energy, fuel, minerals
    float capacity[3] = {};    // 0 = this structure does not carry the medium
    uint8 attachedMask = 0;    // bit m = a cable network of medium m touches this structure (the
                               // game stamps it at every network rebuild; the "no cable" badge)
    float flowUtil = 0.0f;     // EMA of the served fraction of this structure's transport demand /
                               // supply (gauges/mirror) — the game stamps it per transport tick
    // BARRACKS spawn waypoints (orders tier — copied onto units at spawn), capped at
    // MaxRoutePoints. OUTSIDE the union on purpose: a union member with a non-trivial type would
    // force manual construct/destruct of the active variant, and the component carries no type
    // tag to pick it by (the game derives the type from the entity).
    oc::vector<glm::vec3> route;

    // ---- TYPE-SPECIFIC state: a UNION discriminated by the game's structure type — the game
    // only ever touches the variant matching the entity's prefab (the barracks uses `barracks`,
    // turrets `turret`, the three emitter types `emitter`; everything else touches none).
    // Cross-variant reads are bugs — the NetEntityState role-union pattern. All variants are
    // trivially destructible; state dies with the structure.
    static constexpr int MaxRoutePoints = 6;
    struct BarracksData
    {
        // (No spawn timer: the energy store IS the build bar — capacity = spawnCost, filled at
        // the capped cable intake; full = a unit. See update().)
        // POPULATION: the cap is the barracks' own allowance + what its linked houses add (game-
        // stamped each tick); `population` is what its live units hold — += spawnPop at each
        // spawn decision, -= the unit's popCost by the game per death event. A spawn only happens
        // while population + spawnPop <= popCap.
        int population;
        int popCap;
        // The PRODUCED unit type (a player order — synced/saved by the game) and its per-unit
        // prices, stamped by the game each tick from that type: energy and population.
        uint8 unitType;
        uint8 spawnPop;
        uint8 houses;       // linked houses (game-derived each tick, for the info readout)
        float spawnCost;
    };
    struct TurretData
    {
        uint8 unused; // (the fire clock is the ENERGY store now — see the turret block in update)
    };
    // The union's ACTIVE variant, stamped by the game from the structure's type (None for plain
    // buildings). update() runs the matching machine logic: a BARRACKS counts its spawn clock
    // down, pays energy from its own store and QUEUES a spawn request; a TURRET picks the
    // nearest enemy unit via its own spatial query, pays energy, applies its HITSCAN damage on
    // the spot and QUEUES the beam visual — spawning/drawing is main-thread only, so the game
    // drains both queues.
    enum class EMachineKind : uint8 { None, Barracks, Turret, Medic };
    EMachineKind machineKind = EMachineKind::None;

    struct TurretFireRequest // a lightning strike that already landed: from the muzzle to the victim
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
        float unitLoad;         // energy/s deposited by enemy units/shots (atomic adds via
                                // addLoad; the game's production tick consumes and clears it)
    };
    union
    {
        BarracksData barracks{};
        TurretData turret;
        EmitterData emitter;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec); // authority: territory drain + OWNED link flows
    // Both atomic and no-ops while invulnerable. Two entry points only so the call sites read
    // clearly (attacks vs the territory drain); they do the same thing.
    void damage(float amount);
    void fieldDrain(float amount) { damage(amount); }
    void addLoad(float energyPerSec); // atomic unitLoad deposit (workers)
    bool alive() const { return health > 0.0f; }
};

export const GameStructureComponent::SpawnInfo* getGameStructureSpawnInfo(const Entity* entity);
export void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out);
