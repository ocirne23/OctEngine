export module Entity:GameComponents;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Force;

// The GAME-LAYER components: shared per-entity simulation for the three actor kinds of the Force
// game (units, structures, projectiles), running INSIDE the engine's entity update pass instead of
// game-side lists. The heavy per-entity logic (steering, shield battery, territory damage, contact
// damage, lifetime) is C++ HERE; the DSL is the ORDERS/CONFIG tier on top (set a target, read
// health, re-tune stats) through the ctx->gameUnit*/gameStructure*/gameProjectile* thunk surface.
//
// AUTHORITY CONTRACT: update() simulates only when this instance is NOT a network client (single
// player and the server; clients early-out and the components are pure state containers the game's
// mirrors write into). Cross-entity WRITES from the parallel pass go through damage()/addLoad(),
// which are atomic CAS adds — two units biting one structure on different workers must not lose
// hits. Cross-entity LOOKUP is spatial queries only (read-only between commits, worker-safe);
// nothing holds entity lists.

// Class-level tuning shared by every instance — the Game layer's tweaks point at these statics.
export struct GameUnitParams
{
    float energyDrainRate = 25.0f; // energy/s per unit of pressure (player shield rule, no regen)
    float tension = 1.5f;          // surface tension: drain + push scale by (1 + tension*pressure)
    float fieldDps = 10.0f;        // health/s while squished below damageRadius under pressure
    float damageRadius = 0.6f;     // equilibrium radius below this + pressure = exposure damage
    float pushGain = 20000.0f;     // enemy fields shoving the body (force-ball scale)
    float retargetInterval = 8.0f; // auto-target re-roll cadence (jittered per unit)
    float targetSearchRadius = 50.0f; // spatial radius of the auto-target search (structures +
                                      // player fallback) — LOCAL harassment: the barracks route
                                      // does the long-distance delivery, this only picks fights
                                      // around wherever the unit ends up
    float maxSpeedMult = 3.0f;     // field shoves never launch: speed clamp = moveSpeed * this
    float waypointRadius = 3.0f;   // a route waypoint counts as reached inside this
    float voidY = -20.0f;          // fell out of the world -> despawn
    bool navEnabled = true;        // steer by the Nav fields when they exist
    // Context steering weights (see GameComponents.cpp): each candidate heading scores
    //   free * (Goal*dot(goal) + Flow*dot(lane)*|lane|/speed + Persist*dot(last) + Free)
    //   - Pressure*dot(gradP)     (pressure = diffused crowd presence + jams)
    float steerGoal = 0.7f;
    float steerFree = 0.5f;        // reward for open run along the candidate
    float steerFlow = 0.8f;        // join the crowd lane
    float steerPersist = 0.8f;     // keep the last heading (no dithering / reversals)
    float steerPressure = 0.5f;    // away from diffused pressure (crowd presence + jams)
    float orderFlowBlind = 1.5f;   // s a freshly ordered unit ignores the crowd lane (so it can turn around)
    float unstickAfter = 2.0f;     // s of stall after which goal/persistence are dropped: open, lowest-pressure direction + jitter
    float presencePressure = 0.025f; // pressure every unit injects per tick (x60/s) just by being there
    float steerLook = 8.0f;        // metres of whisker (min; scales with speed)
    float stuckPressure = 2.0f;    // pressure a stalled unit injects per second (x stall, <= 1.5)
};

// A combat unit: team bubble on player-shield rules minus regen (pressure drains the battery,
// empty = permanent collapse, exposure bleeds health), enemy-field push-back, C++ steering toward
// a target, melee gnaw on any enemy structure in reach, and an optional RANGED stance (stand off
// and ask for a shot — spawning is main-thread only).
// TARGETING + PATHING: the Nav flow fields first — every OTHER team's field is sampled at the
// unit's cell and the geodesically nearest enemy (structure or player) wins; its descent
// direction routes around walls, and the own-team density gradient spreads the crowd. Where no
// field covers the unit (far from every source, Nav disabled) the old local search runs: random
// pick among the 4 nearest enemy structures via spatial query, nearest enemy player fallback —
// puppets found in the SAME query, nothing publishes a player list — with the stuck-sidestep as
// the only obstacle handling. The DSL may LOCK an explicit target (setTarget), which steers
// straight — orders come from scripts, the walking/fighting is here.
export struct GameUnitComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameUnit; }
    ~GameUnitComponent() {}

    static GameUnitParams params;

    // ---- worker-side EVENTS, drained by the game on the main thread ----------------------------
    // Everything the game used to learn by walking a roster of units, a unit now REPORTS while it
    // updates. Nothing outside holds unit handles: the game drains these queues, and anything that
    // genuinely needs every unit at once (the shield mirror, overhead labels) runs its own spatial
    // query at the point of need.
    struct FireRequest // RANGED units ask for a shot; spawning is main-thread only
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 target{ 0.0f };
        uint8 team = 1;
    };
    static void takeFireRequests(oc::vector<FireRequest>& out);
    static void takeDeaths(oc::vector<uint32>& outSourceIds); // spawner ids of units that died
    // (There is NO separate shield mirror: this component's state RIDES THE ENTITY SYNC — the
    // engine's snapshot/claim records carry a quantized game blob whenever the entity has a
    // GameUnitComponent. See NetworkManager's packGameStateBlob/applyGameStateBlob.)

    struct SpawnInfo
    {
        uint32 team = 1;
        bool puppet = false; // see the `puppet` field
        float healthMax = 60.0f;
        float energyMax = 40.0f;      // shield battery (no regen)
        float shieldOutput = 0.8f;    // bubble output while the battery lives (collapse -> sentinel)
        float moveSpeed = 4.0f;
        float accel = 15.0f;          // soft on purpose: steering loses the shoving match vs bubbles
        float attackRange = 1.5f;     // melee reach measured to the victim's meleeRadius ring
        float attackDps = 6.0f;       // structure health/s while in reach
        float playerDps = 10.0f;      // player health/s while in reach (game routes it to the owner)
        float emitterDrain = 5.0f;    // energy/s this unit costs the nearest active enemy emitter
        bool ranged = false;          // spitter stance: hold at standoffRange and queue FireRequests
        float standoffRange = 16.0f;
        float fireInterval = 3.0f;
    };

    // ---- live state (spawn copies the SpawnInfo; DSL/game may re-tune any of it) ----
    uint32 team = 1;
    float health = 60.0f, healthMax = 60.0f;
    float energy = 40.0f, energyMax = 40.0f;
    float shieldOutput = 0.8f;
    float moveSpeed = 4.0f, accel = 15.0f;
    float attackRange = 1.5f, attackDps = 6.0f, playerDps = 10.0f;
    float emitterDrain = 5.0f;
    bool ranged = false;
    float standoffRange = 16.0f, fireInterval = 3.0f;
    float bodyRadius = 0.5f;    // planar collider radius (from the physics shape at spawn) — the
                                // nav line-of-sight tests are run for the BODY, not a point

    // PUPPET (player capsules author `Puppet true`): the component is a pure state CARRIER —
    // update() runs no sim at all. The owning GamePlayer writes health/energy/collapsed/materials
    // into it, and the entity sync's game blob moves it owner -> server -> other clients. Gives
    // players the same overhead bars/labels path as units without the unit AI ever touching them.
    bool puppet = false;
    float pendingDamage = 0.0f; // puppets: damage() lands HERE instead of on health (health is
                                // owner-computed) — the game drains it to the owning instance,
                                // whose GamePlayer::applyDamage runs the shield-absorb rules
    float materialsFrac = 0.0f; // players: carried construction stock (server-authoritative — the
                                // server writes it, the snapshot blob delivers it to the owner)
    bool collapsed = false;     // latched at empty battery (permanent — no regen)
    bool deathReported = false; // the death event is emitted once, even if a tick runs before the
                                // queued destroy is drained
    uint32 sourceId = 0;        // the spawner's stable id — rides the death event so the barracks
                                // can decrement its alive count without anyone tracking units

    // Orders: an explicit DSL/game target overrides auto-targeting until cleared or reached+dry.
    bool targetLocked = false;
    bool hasTarget = false;
    bool moveOrder = false;   // the locked target is a PLAYER MOVE ORDER (RTS right-click): walk
                              // there, then unlock and resume the AI (a DSL attack lock never clears)
    glm::vec3 targetPos{ 0.0f };
    void orderMove(const glm::vec3& worldPos, bool fresh = true) // main thread (game input): drops any route too
    {
        targetPos = worldPos;
        hasTarget = targetLocked = moveOrder = true;
        routeIndex = routeCount;
        if (fresh)
            m_ignoreFlowTimer = params.orderFlowBlind; // a fresh order: ignore the old lane for a moment
    }
    // Route: waypoints copied in AT SPAWN (the barracks route); marched before combat targeting.
    static constexpr int MaxRoutePoints = 6;
    glm::vec3 route[MaxRoutePoints]{};
    uint8 routeCount = 0, routeIndex = 0;

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec); // authority only — see the contract above
    // Atomic (projectile contacts are main-thread, melee is workers). Units: CAS on health.
    // Puppets: accumulates into pendingDamage — ONE damage entry point for every victim kind.
    void damage(float amount);
    float takePendingDamage(); // main thread: drain the puppet inbox (atomic exchange)
    bool alive() const { return health > 0.0f; }

private:
    float m_retargetTimer = 0.0f;
    float m_fireTimer = 0.0f;
    uint32 m_rng = 0;           // tiny per-unit LCG — worker-safe, seeded from the entity address
    float m_pressureTimer = 0.0f; // stalled time (displacement checkpoints) -> weights + pressure
    float m_ignoreFlowTimer = 0.0f; // > 0: the lane term is skipped (fresh move order)
    glm::vec2 m_stuckAnchor{ 0.0f };
    float m_stuckCheckTimer = 0.0f;
    bool m_hasStuckAnchor = false;
    glm::vec2 m_lastDir{ 0.0f };  // context steering persistence
    bool m_hasLastDir = false;
    float m_outputHistory[3] = { 0.8f, 0.8f, 0.8f }; // push normalizes by the ~2-frame-old output
};

export struct GameStructureParams
{
    float fieldDamageRate = 6.0f; // health/s while an enemy team's bubble owns the query point
    // Shared production tuning (the game's tweaks point here; per-TYPE values are stamped
    // per-instance instead — e.g. BarracksData::spawnCost):
    float barracksSpawnInterval = 8.0f;
    int barracksUnitLimit = 5;    // alive units per barracks
    float turretRange = 18.0f;
    float turretFireInterval = 1.2f;
    float turretShotEnergy = 1.5f; // spent from the turret's own energy store per shot
};

// One end of a resource link (cable/pipe/conveyor). The SAME link exists mirrored on BOTH
// endpoints' `links` vectors; exactly ONE side is the `owner`, and only the owner runs the flow
// (and carries the authoritative lastFlow for the visuals). `other` is an owning EntityPtr — the
// game UNLINKS a structure from all neighbors BEFORE destroying it, so a link never points at a
// dead entity.
export struct GameStructureLink
{
    EntityPtr other;
    uint8 medium = 0;     // 0 energy, 1 fuel, 2 minerals — which store pair it equalizes
    uint8 cableTier = 0;  // the game's ECableType (Basic/Heavy/Pipe/Conveyor) for draw/retype
    float throughput = 5.0f; // units/s cap
    bool owner = false;
    float lastFlow = 0.0f;   // signed, THIS side -> other (owner side only)
    float flowAvg = 0.0f;    // EMA of lastFlow — gravity-fed transfers are bursty tick to tick
                             // (a consumer drains its buffer, then takes a full packet), so the
                             // VISUALS and gauges read this instead and stop strobing. Its SIGN
                             // is the flow direction: same-band transfers are damped, so a link
                             // no longer overshoots and reverses around its balance point.
};

// A building: team + health (health IS the construction progress while `blueprint`), a territory
// ForceQuery that drains health inside enemy-owned field, the melee/emitter-load intake the units'
// C++ pushes into, AND the resource node of the flow networks: three stores (energy/fuel/minerals)
// with capacities and gravity-fed BANDS, plus the `links` vector distribution runs over. There is
// NO global structure list anywhere — each structure moves resources across its OWN owned links in
// update() (gravity-fed: producers band 2 always export, storage band 1 balances by fill fraction,
// consumers band 0 outrank everything until FULL; cross-band = full throughput downhill, same-band
// = exact equalizing transfer). Cross-entity store mutation is atomic (reserve-from-source /
// clamped-add-to-dest / return-remainder), so concurrent owners on shared endpoints compose.
// PRODUCTION (income, fuel burn, consumer drain, emitter ramps) stays in the game's
// StructureSystem, iterating a per-frame spatial query — per-entity state, no roster.
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
    float health = 100.0f, healthMax = 100.0f;
    float meleeRadius = 1.0f;
    ForceQuery query;          // territory at the structure (authority instances only)
    // ---- the flow-network node (game stamps capacity/band per tick so tweaks stay live) ----
    float store[3] = {};       // energy, fuel, minerals
    float capacity[3] = {};    // 0 = this structure does not carry the medium
    int8 band[3] = { 0, 0, 0 };// gravity bands per medium: a HIGHER band exports to a lower one at
                               // full throughput, equal bands balance by fill fraction. The game
                               // assigns the values (producer > storage/relay > consumer).
    oc::vector<GameStructureLink> links;
    float flowUtil = 0.0f;     // EMA of the busiest touching link's utilization (gauges/mirror)
    // BARRACKS spawn waypoints (orders tier — copied onto units at spawn), capped at
    // MaxRoutePoints. OUTSIDE the union on purpose: a union member with a non-trivial type would
    // force manual construct/destruct of the active variant, and the component carries no type
    // tag to pick it by (the game derives the type from the entity).
    oc::vector<glm::vec3> route;

    // ---- TYPE-SPECIFIC state: a UNION discriminated by the game's structure type — the game
    // only ever touches the variant matching the entity's prefab (barracks variants use
    // `barracks`, turrets `turret`, the three emitter types `emitter`; everything else touches
    // none). Cross-variant reads are bugs — the NetEntityState role-union pattern. All variants
    // are trivially destructible; state dies with the structure.
    static constexpr int MaxRoutePoints = 6;
    struct BarracksData
    {
        float spawnTimer;   // counts down; Constructor boost accelerates it
        float boost;        // extra spawn speed from idle Constructors (rebuilt every tick)
        int aliveUnits;     // ++ here at each spawn decision, -- by the game per death event
        float spawnCost;    // minerals per unit, stamped per BARRACKS TYPE by the game each tick
    };
    struct TurretData
    {
        float fireTimer;
    };
    // The union's ACTIVE variant, stamped by the game from the structure's type (None for plain
    // buildings). update() runs the matching machine logic: a BARRACKS counts its spawn clock
    // down, pays minerals from its own store and QUEUES a spawn request; a TURRET picks the
    // nearest enemy unit via its own spatial query, pays energy and QUEUES a shot — spawning is
    // main-thread only, so the game drains both queues.
    enum class EMachineKind : uint8 { None, Barracks, Turret };
    EMachineKind machineKind = EMachineKind::None;

    struct TurretFireRequest
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

    // Link bookkeeping (MAIN THREAD — the game's placement/demolish seam). A pair may hold UP TO
    // ONE LINK PER MEDIUM (a power cable AND a fuel pipe between the same two structures is fine);
    // link() replaces the pair's existing link of the SAME medium (retype in place).
    GameStructureLink* findLink(const Entity* otherEntity, int medium = -1); // -1 = any medium
    void unlinkAll(Entity& self);                           // removes the mirror entries too
    static void link(Entity& a, Entity& b, uint8 medium, uint8 cableTier, float throughput);
    static void unlink(Entity& a, Entity& b, int medium = -1); // -1 = every link of the pair
};

export struct GameProjectileParams
{
    float pushGain = 20000.0f; // field deflection (the force-ball pattern)
    float voidY = -20.0f;
};

// A shot: ages out, dies below the world, deflects in enemy fields, saps the nearest active enemy
// emitter while pressing near it (via addLoad), and on ANY contact (main-thread, routed from
// World::handleContactEvent) damages an enemy-team unit/structure it hit and despawns — no
// bouncing shells. Team-tagged: own-team actors pass free but still stop it.
export struct GameProjectileComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameProjectile; }
    ~GameProjectileComponent() {}

    static GameProjectileParams params;

    struct SpawnInfo
    {
        uint32 team = 0;
        float unitDamage = 25.0f;      // on hitting an enemy unit
        float structureDamage = 20.0f; // on hitting an enemy structure
        float lifetime = 6.0f;
        float emitterDrain = 0.0f;     // energy/s sapped from the nearest active enemy emitter
        float emitterDrainRadius = 5.0f;
    };

    uint32 team = 0;
    float unitDamage = 25.0f, structureDamage = 20.0f;
    float lifetime = 6.0f;
    float emitterDrain = 0.0f, emitterDrainRadius = 5.0f;
    float age = 0.0f;
    bool spent = false; // hit something — despawn queued, never damage twice

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec);
    void onContact(Entity& self, Entity& other, bool begin); // main thread (physics contact dispatch)
};

export const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity);
export const GameStructureComponent::SpawnInfo* getGameStructureSpawnInfo(const Entity* entity);
export const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity);
export void writeGameUnitSpawnInfo(const GameUnitComponent::SpawnInfo& info, AssetNode& out);
export void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out);
export void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out);
