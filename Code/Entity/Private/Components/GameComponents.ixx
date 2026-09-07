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
    float fieldDpsMult = 1.0f;     // global scale on the field exposure damage (x fieldDps)
    float fieldPushStart = 0.7f;   // push ramp start as a fraction of iso: below it the field does
                                   // NOT shove, so units walk into the damage band instead of being
                                   // stopped out in the weak fringe before it (0 = push everywhere)
    float emitterDrainMult = 0.25f; // global scale on the per-type emitterDrain a pressing unit
                                   // deposits on the nearest active enemy emitter (addLoad)
    float strainRange = 20.0f;     // planar reach (m) of that siege drain, unit to the emitter
                                   // structure: the Bastion's visible bubble radius ("Bastion
                                   // reach" 45 / output 2.6 -> ~19.8 m), so a unit leaning on the
                                   // largest bubble drains it; shield state does not matter
    float damageAbsorb = 2.0f;     // shield ENERGY per hp of direct damage absorbed BEFORE health
                                   // (the player's "Damage absorb" rule, applied in the owner tick)
    float damageRadius = 0.6f;     // equilibrium radius below this + pressure = exposure damage
    float pushGain = 20000.0f;     // enemy fields shoving the body (force-ball scale) — shared by
                                   // the emitter readback path AND the shield-less query path,
                                   // which reproduces the same formula from the point readback
    float retargetInterval = 5.0f; // auto-target re-roll cadence (jittered per unit)
    float wanderSpeedMult = 0.25f; // a WANDER order (orderWander) walks at this fraction of moveSpeed...
    float wanderSpeedMax = 0.75f;   // ...capped at this many m/s (runners stroll like everyone else)
    int localTeam = -1;            // the VIEWER's team (the game stamps it): its units tint green
                                   // (applyTeamTint) on every instance, server and clients alike
    int huntSeedTeam = -1;         // the ONE team whose units also request seed paths toward a
                                   // HUNTED target (nav/local-search/engage); every other team seeds
                                   // only for routes and move orders. -1 = no team (the game stamps
                                   // the co-op AI team, so friendly units never carve lanes at enemies)
    float targetSearchRadius = 15.0f; // spatial radius of the auto-target search (structures +
                                      // player fallback) — LOCAL harassment: the barracks route
                                      // does the long-distance delivery, this only picks fights
                                      // around wherever the unit ends up
    float maxSpeedMult = 3.0f;     // field shoves never launch: speed clamp = moveSpeed * this
    float maxSpeed = 10.0f;        // absolute m/s cap on every unit body, every tick, any cause
    float waypointRadius = 3.0f;   // a route waypoint counts as reached inside this
    float routeEngageRadius = 5.0f;  // marching a route: an enemy unit/structure this near is
                                     // engaged (walk target diverts to it), the route resumes after
    float orderBreakRadius = 15.0f;  // a MOVE ORDER drops at the first enemy structure this near:
                                     // the AI then hunts the nearest structure (waves stop marching
                                     // past everything to the Base)
    float voidY = -3.0f;           // fell through the floor (ground is y 0) -> killed, checked
                                   // by the full sim AND the far tick so no unit escapes it
    // HEIGHT LIMIT (world Y, metres): the physics can launch a body (bubble shoves, stacked
    // bodies, contact impulses); above the ceiling an actor is put back AT the ceiling with its
    // climb cancelled. The shared default for every ground actor — units AND player capsules
    // (GamePlayer applies the same rule on the owner). Per-prefab override: SpawnInfo::heightLimit.
    float heightLimit = 5.0f;
    bool navEnabled = true;        // steer by the Nav fields when they exist
    // Context steering weights (see GameComponents.cpp): each candidate heading scores
    //   free * (Goal*dot(goal) + Flow*laneW*dot(lane) + Persist*dot(last))
    //   - Pressure*gpW*dot(gradP) + Wall*dot(wallAway) - clipped*CornerClip
    float steerGoal = 0.3f;
    float steerFlow = 1.0f;        // FOLLOW the crowd lane — a lane is a seeded/proven route, so
                                   // where one exists it should outweigh walking straight at the goal
    float flowSplatGain = 0.5f;    // scale on the MEASURED velocity a walking unit splats into the
                                   // crowd lane each tick (0 = units leave no trail; seeded lanes only)
    float steerPersist = 0.4f;     // keep the last heading (no dithering / reversals)
    // LIVE-TARGET TRACKING: within targetTrackRadius of a team-field target the goal direction
    // refreshes at the field rate (~0.25 s) against the target's LIVE position, so it must beat
    // the seeded lane — the lane's periodic re-plans lag a moving player badly. Between track and
    // search radius the unit still has the target but marches lane-friendly (seeded paths rule).
    float targetTrackRadius = 5.0f; // geodesic metres: closer than this = field-tracking priority
    float steerTrackGoal = 1.5f;   // goal weight floor while tracking (overrides steerGoal)
    float trackFlowMult = 0.15f;   // lane weight multiplier while tracking (near-mute)
    // NAV FOLLOW band (search radius .. this): too far to TARGET, but if the crowd FLOW field
    // holds a lane at the unit it walks the lane — pursuit survives a player sprinting out of the
    // search radius (the chase trail + seeded lane keep pulling the pack until the target is back
    // in range or the lane decays).
    float navFollowRadius = 40.0f; // geodesic metres to the nearest enemy source
    float steerPressure = 0.5f;    // away from diffused pressure (crowd presence + jams)
    float pressureKnee = 0.23f;    // pressure gradient scoring 0.5 (compressive: x/(x+knee), no saturation)
    float flowKnee = 0.15f;        // lane speed scoring 0.5, as a fraction of moveSpeed (low: even
                                   // a half-faded lane still counts as "there is a lane here")
    float orderFlowBlind = 0.0f;   // s a freshly ordered unit ignores the crowd lane (so it can turn around)
    float seedRequestInterval = 1.0f; // s between a unit's own "plan me a lane" requests
    float unstickAfter = 1.0f;     // s of stall after which goal/persistence are dropped: open, lowest-pressure direction + jitter
    float presencePressure = 0.01f; // pressure every unit injects per tick (x60/s) just by being
                                   // there — a weak spacing term next to the seeded lane troughs
    float steerLook = 6.0f;        // metres of whisker (min; scales with speed)
    float steerCornerClip = 0.7f;  // penalty per lateral body sample that hits a wall (corner clip)
    float steerWall = 1.0f;        // push away from walls within wallKeep of the body (corner rounding)
    float wallKeep = 0.9f;         // metres beyond the body radius the wall push reaches
    float stuckPressure = 0.5f;    // pressure a stalled unit injects per second (x stall, <= 1.5),
                                   // on top of the planned-path request a stuck unit also makes
};

// A combat unit: team bubble on player-shield rules minus regen (pressure drains the battery,
// empty = permanent collapse, exposure bleeds health), enemy-field push-back, C++ steering toward
// a target, melee gnaw on any enemy structure in reach, and an optional RANGED stance (stand off
// and ask for a shot — spawning is main-thread only).
// TARGETING + PATHING: the Nav flow fields first — every OTHER team's field is sampled at the
// unit's cell and the geodesically nearest enemy (structure or player) wins, RANGE-GATED by
// targetSearchRadius (a source farther than that geodesically is ignored: units hold their patch
// instead of marching across the map, and in-range units track LIVE positions at the field's
// ~0.25 s rebuild rate); its descent direction routes around walls, and the pressure gradient
// spreads the crowd. Where no field covers the unit (out of range, Nav disabled) the local search runs: random
// pick among the 4 nearest enemy structures via spatial query, nearest enemy player fallback —
// puppets found in the SAME query, nothing publishes a player list. The DSL may LOCK an
// explicit target (setTarget), which steers
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
        uint8 shotKind = 0; // the unit's ShotKind: which shell prefab the game spawns
    };
    static void takeFireRequests(oc::vector<FireRequest>& out);
    // EVERY unit with somewhere to be asks for a planned lane on its own timer (requestSeedPath is
    // a main-thread call). The game drains these and forwards them to NavSystem::requestSeedPath,
    // whose PROXIMITY dedup collapses a whole crowd walking the same way into ONE plan — that is
    // what makes a per-unit request affordable, and it keeps a group's lane refreshed as the group
    // advances without anyone tracking the group.
    struct SeedRequest
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
        uint8 team = 1;
        bool stuck = false; // stalled unit: the game seeds it as the weaker "stuck" lane
    };
    static void takeSeedRequests(oc::vector<SeedRequest>& out);
    // A unit died: its spawner's id + the POPULATION it held, so the barracks frees exactly
    // that much of its cap without anyone tracking units.
    struct DeathRecord
    {
        uint32 sourceId = 0;
        uint8 popCost = 0;
    };
    static void takeDeaths(oc::vector<DeathRecord>& out);
    // (There is NO separate shield mirror: this component's state RIDES THE ENTITY SYNC — the
    // engine's snapshot/claim records carry a quantized game blob whenever the entity has a
    // GameUnitComponent. See NetworkManager's packGameStateBlob/applyGameStateBlob.)

    struct SpawnInfo
    {
        uint32 team = 1;
        bool puppet = false; // see the `puppet` field
        oc::string shortName; // the 3-5 char HUD tag over the unit ("GRNT", "PLR"); empty = "UNIT"
        float healthMax = 60.0f;
        float energyMax = 40.0f;      // shield battery (no regen)
        float shieldOutput = 0.8f;    // bubble output while the battery lives (collapse -> sentinel)
        float moveSpeed = 4.0f;
        float accel = 15.0f;          // soft on purpose: steering loses the shoving match vs bubbles
        float attackRange = 1.5f;     // melee reach measured to the victim's meleeRadius ring
        float attackDps = 6.0f;       // structure health/s while in reach
        float playerDps = 10.0f;      // player health/s while in reach (game routes it to the owner)
        float emitterDrain = 1.0f;    // energy/s this unit costs the nearest active enemy emitter
        bool ranged = false;          // spitter stance: hold at standoffRange and queue FireRequests
        float standoffRange = 16.0f;
        float fireInterval = 3.0f;
        uint8 shotKind = 0;           // ranged: which shell the game fires (0 direct, 1 splash lob)
        bool alwaysDisplayHealth = false; // overhead bars show even at full health/shield
        float heightLimit = 0.0f;     // world-Y ceiling: 0 = the shared params.heightLimit,
                                      // > 0 = this prefab's own ceiling, < 0 = NONE (flying units)
    };

    // ---- live state (spawn copies the SpawnInfo; DSL/game may re-tune any of it) ----
    uint32 team = 1;
    float health = 60.0f, healthMax = 60.0f;
    float energy = 40.0f, energyMax = 40.0f;
    float shieldOutput = 0.8f;
    float moveSpeed = 4.0f, accel = 15.0f;
    float attackRange = 1.5f, attackDps = 6.0f, playerDps = 10.0f;
    float emitterDrain = 1.0f;
    bool ranged = false;
    float standoffRange = 16.0f, fireInterval = 3.0f;
    uint8 shotKind = 0;
    bool alwaysDisplayHealth = false; // overhead bars even at full health/shield (label pass reads it)
    float heightLimit = 0.0f;   // see SpawnInfo::heightLimit (0 = shared default, < 0 = none)
    // The ceiling this actor is held under (FLT_MAX = unlimited). Read by the unit tick AND by
    // GamePlayer for the capsule's puppet, so both actor kinds obey the same rule.
    float effectiveHeightLimit() const
    {
        if (heightLimit < 0.0f)
            return FLT_MAX;
        return heightLimit > 0.0f ? heightLimit : params.heightLimit;
    }
    float bodyRadius = 0.5f;    // planar collider radius (from the physics shape at spawn) — the
                                // nav line-of-sight tests are run for the BODY, not a point
    // (SHIELD-LESS bodies — the swarm types, no ForceComponent — read the BAKED pressure field
    // instead of carrying anything: ForceSystem::sampleBakedField in update(), worker-safe, no
    // per-unit GPU slot. See the field-push block at the end of update().)

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
                                // can free its population without anyone tracking units
    uint8 popCost = 0;          // population this unit holds on its barracks (game-stamped at spawn)

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
        wanderOrder = false;
        routeIndex = routeCount;
        if (fresh)
            m_ignoreFlowTimer = params.orderFlowBlind; // a fresh order: ignore the old lane for a moment
    }
    // WANDER: a move order that never seeds a lane and GIVES UP after `seconds` (a target behind
    // rock must not pin the unit forever) — the ambient enemies' idle stroll (GameMatch).
    bool wanderOrder = false;
    float wanderTimeLeft = 0.0f;
    void orderWander(const glm::vec3& worldPos, float seconds) // main thread
    {
        orderMove(worldPos, /*fresh*/ false);
        wanderOrder = true;
        wanderTimeLeft = seconds;
    }
    // Route: waypoints copied in AT SPAWN (the barracks route); marched before combat TARGETING,
    // but enemy units inside routeEngageRadius are still fought on the way (see update).
    static constexpr int MaxRoutePoints = 6;
    glm::vec3 route[MaxRoutePoints]{};
    uint8 routeCount = 0, routeIndex = 0;

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec); // authority only — see the contract above
    // FAR TICK — the stand-in simulation for a unit OUTSIDE the SIM LOD selection (never visited
    // by the entity pass, body disabled; see World::simLodDelta). Run by NpcSystem over its
    // roster (authority, worker-safe: writes only this entity + its own spatial entry) every
    // "Far tick interval": walks the route / move order — straight at the target where the
    // raster shows a clear line, else along the enemy team field's descent (geodesic, around
    // rocks) — by TELEPORT (entity pos + body pose + spatial entry), no physics, no combat, no
    // bubble; waypoints advance and the order clears with the full sim's radius rule. A unit with
    // nowhere to go stays parked. Returns true when it moved.
    bool updateFar(Entity& entity, float deltaSec);
    // Health to 0 + the ONE-SHOT death report (destroy request, the spawner's population freed).
    // Called by update() at 0 hp or below voidY, and by updateFar() below voidY.
    void kill(Entity& entity);
    // Atomic (projectile contacts are main-thread, melee is workers). Units: CAS on health.
    // Puppets: accumulates into pendingDamage — ONE damage entry point for every victim kind.
    void damage(float amount);
    float takePendingDamage(); // main thread: drain the puppet inbox (atomic exchange)
    bool alive() const { return health > 0.0f; }
    // HEAL inbox (workers — a Medic station's update): banked atomically like damage and applied
    // in the unit's OWN tick (health and the shield battery both, which keeps `energy`
    // single-writer); a refilled battery lifts the collapse latch there too.
    void heal(float amount);
    float pendingHeal = 0.0f;
    // FRIENDLY TINT (main thread): a unit on params.localTeam gets a green material override (its
    // render children too); any other team keeps its authored colours. Idempotent — called at
    // spawn (the game) and whenever the replicated team lands (the network blob), so clients
    // tint their own side as well.
    static void applyTeamTint(Entity& entity);
    uint8 tintState = 0; // 0 authored (never touched), 1 friendly green, 2 authored (restored after a re-team)
    // The HUD tag authored as `ShortName` in the .pre. Replicated units carry it too: the prefab
    // spawns identically on every instance.
    const char* getShortName() const { return m_shortName.c_str(); }

private:
    oc::string m_shortName = "UNIT";
    float m_retargetTimer = 0.0f;
    float m_fireTimer = 0.0f;
    uint32 m_rng = 0;           // tiny per-unit LCG — worker-safe, seeded from the entity address
    float m_pressureTimer = 0.0f; // stalled time (displacement checkpoints) -> weights + pressure
    // Lane requests are due at an ABSOLUTE sim time, not on a countdown of the tick delta: a
    // throttled tick hands the unit a delta capped at "Max catch-up" frames, so a countdown ran
    // ~8x slow at tier 2. The request cadence must not depend on the SIM LOD tier.
    float m_seedDue = 0.0f; // sim seconds (Time::getSimElapsedSec) of the next request; the area limiter is global
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
        float splashRadius = 0.0f;     // > 0: the contact damages EVERY enemy unit/structure within
                                       // it (the lobber shell), not just the one it touched
    };

    uint32 team = 0;
    float unitDamage = 25.0f, structureDamage = 20.0f;
    float lifetime = 6.0f;
    float emitterDrain = 0.0f, emitterDrainRadius = 5.0f;
    float splashRadius = 0.0f;
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
