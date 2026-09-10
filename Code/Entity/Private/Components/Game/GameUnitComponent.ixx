export module Entity:GameUnitComponent;

import :Entity;
import :PhysicsComponent;
import :ForceComponent;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Force;
import Nav;

// The GAME-LAYER components: shared per-entity simulation for the three actor kinds of the Force
// game (units, structures, projectiles), running INSIDE the engine's entity update pass instead of
// game-side lists. The heavy per-entity logic (steering, shield battery, territory damage, contact
// damage, lifetime) is C++ HERE; the DSL is the ORDERS/CONFIG tier on top (set a target, read
// health, re-tune stats) through the ctx->gameUnit*/gameStructure*/gameProjectile* thunk surface.
// One partition per component under Components/Game/; the helpers they share (the authority gate,
// the atomic CAS adds, the spawn-info readback) are Entity:GameComponentShared.
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
    float farSpreadDeg = 40.0f;    // far walk: a persistent per-unit heading bias in +-half this, so
                                   // a wave fans across its corridor instead of filing down one
                                   // line of field-descent cells (a biased step into rock falls
                                   // back to the plain heading)
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
    // The HURT LIGHT: a unit whose health DROPS glows (a temporary point light over its body) —
    // full brightness while it keeps dropping (field exposure), decaying over hurtLightDecay
    // after the last drop (a melee hit, a turret strike, a shell).
    float hurtLightIntensity = 8.0f;
    float hurtLightDecay = 0.25f;  // seconds from full to dark
    // AREA BUDGET for the hurt flashes (the Nav seed limiter's idea — a budget per world-area
    // bucket — as a lock-free hashed slot table, since this is decided on workers): at most
    // `hurtFlashRate` flashes per second per `lightArea` square no matter how many units stand
    // in it — a lone hit unit always flashes, a swarm in a field shows random flashes across it.
    // (The shield glow needs no budget: it is the Force system's bubble light, and merging
    // already collapses an overlapping crowd into one bubble.)
    float lightArea = 8.0f;        // m: the bucket size
    float hurtFlashRate = 4.0f;    // flashes per second per area
    // Context steering weights (see GameUnitComponent.cpp, steerHeading): each candidate heading scores
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
    // A MELEE HIT landed (the damage is already applied): the striker's position and the victim's,
    // for the game's hit visual (a line plus a flash). Pure report — nothing to service.
    struct HitRecord
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
    };
    static void takeHits(oc::vector<HitRecord>& out);
    // The number of LIVE UNITS — non-puppet instances of this component in existence, maintained
    // at the spawn / destroy edges (a relaxed atomic: both run on workers in the batch paths).
    // The game's "units alive" (the HUD, the wave cap, the wander budget) reads THIS instead of
    // walking anything. A unit at 0 hp counts until its queued destroy drains.
    static int liveCount();
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
        // MELEE is DISCRETE: one hit of attackDamage every attackInterval seconds while a victim
        // (an enemy unit, else a structure) is in reach. The victim takes the whole hit at once.
        float attackInterval = 1.0f;  // seconds between hits
        float attackDamage = 6.0f;    // health per hit
        float playerDamage = 10.0f;   // health per hit on a PLAYER capsule (the game routes it to the owner)
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
    float attackRange = 1.5f, attackInterval = 1.0f, attackDamage = 6.0f, playerDamage = 10.0f;
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
    // instead of carrying anything: ForceSystem::sampleBakedField in tickField(), worker-safe, no
    // per-unit GPU slot.)

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
    // but enemy units inside routeEngageRadius are still fought on the way (see tickCombat).
    static constexpr int MaxRoutePoints = 6;
    glm::vec3 route[MaxRoutePoints]{};
    uint8 routeCount = 0, routeIndex = 0;

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info); // the live count's decrement
    // Authority only — see the contract above. ONE tick is the step sequence declared under
    // `Tick` below: height limit, inboxes, walk target, combat probe, steering, field.
    void update(Entity& entity, float deltaSec);
    // FAR TICK — the stand-in simulation for a unit OUTSIDE the SIM LOD selection (never visited
    // by the entity pass, body disabled; see World::simLodDelta). Run by NpcSystem over its
    // roster (authority, worker-safe: writes only this entity + its own spatial entry) every
    // "Far tick interval": walks the route / move order — straight at the target where the
    // raster shows a clear line, else along the enemy team field's descent (geodesic, around
    // rocks) — by TELEPORT (entity pos + body pose + spatial entry), no physics, no combat, no
    // bubble; waypoints advance and the order clears with the full sim's radius rule. A unit with
    // nowhere to go stays parked. Returns true when it moved.
    bool updateFar(Entity& entity, float deltaSec);
    // The far-walk heading (route waypoint / locked order; straight with line of sight, else the
    // nearest enemy field's descent), walk speed and remaining distance; `spread` applies the
    // "Far spread" bias. false = nowhere to go. No state changes.
    bool farHeading(const Entity& entity, glm::vec2& dir, float& speed, float& dist, bool spread = true);
    // The velocity to wake with (World's SIM LOD wake edge -> PhysicsComponent::unpark): full walk
    // speed along the far heading, zero for a unit with nowhere to go.
    glm::vec3 wakeVelocity(const Entity& entity);
    // Health to 0 + the ONE-SHOT death report (destroy request, the spawner's population freed).
    // Called by update() at 0 hp or below voidY, and by updateFar() below voidY.
    void kill(Entity& entity);
    // Atomic (projectile contacts are main-thread, melee is workers). Units: CAS on health.
    // Puppets: accumulates into pendingDamage — ONE damage entry point for every victim kind.
    // `sourceTeam` = the attacker's team: the hurt light takes its colour (UnknownTeam = the
    // neutral red). A plain byte store — concurrent attackers race for it, harmlessly.
    static constexpr uint32 UnknownTeam = UINT32_MAX;
    void damage(float amount, uint32 sourceTeam = UnknownTeam);
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
    // ONE authority tick (update): the frame's shared state, handed through the steps below in
    // order. Every step reads what the earlier ones settled; nothing here outlives the tick.
    struct Tick
    {
        Entity& entity;
        PhysicsComponent& pc;
        ForceComponent* fc;         // null = a SHIELD-LESS body (the swarm types)
        float deltaSec;
        glm::vec3 pos{ 0.0f };      // the body's position at the top of the tick
        glm::vec2 here{ 0.0f };     // pos.xz
        bool fields = false;        // the Nav fields are on (params + NavSystem)
        // The body's velocity, read ONCE at the top: every queued command builds on it, so a
        // later SetLinearVelocity never re-applies a climb the height limit cancelled.
        glm::vec3 vel{ 0.0f };
        // ---- where to walk (resolveWalkTarget) ----
        bool routing = false;       // marching a route waypoint
        bool haveWalkTarget = false;
        bool walkIsOrder = false;   // the walk target is a route waypoint or a move order, not a hunted enemy
        glm::vec3 walkTarget{ 0.0f };
        bool navResolved = false;   // a Nav team field settled the target (the local re-search stays off)
        bool navSteer = false;      // navResolved AND the descent is usable (it is zero AT a source)
        bool navTracking = false;   // target within targetTrackRadius: field-tracking gets steering priority
        glm::vec2 navDir{ 0.0f };
        // ---- combat (tickCombat) ----
        float stopRange = 0.0f;     // hold this far from the walk target (melee reach / standoff)
        bool inEnemyBubble = false; // stamped-radius signal — the shield-less FALLBACK when the bake is off
    };
    void tickHurtLight(const Entity& entity, float deltaSec); // every role, before the client gate
    void applyHeightLimit(Tick& t);  // launched above the ceiling -> put back AT it, climb cancelled
    bool applyInboxes(Tick& t);      // the damage + heal inboxes; false = the unit died this tick
    void resolveWalkTarget(Tick& t); // route, then the locked order, then the Nav fields, then the local search
    void resolveNavTarget(Tick& t);  // the Nav team-field branch: range-gated target, or the follow-band lane
    void searchLocalTarget(Tick& t); // the spatial-query fallback on the retarget timer
    void tickCombat(Tick& t);        // ONE probe: structure bite, emitter strain, melee sweep, ranged shots
    void tickSteering(Tick& t);      // walk toward the target (or brake) + the hard velocity cap
    // Context steering over the nav fields: scores a fan of headings around `goalDir` and returns
    // the best; also owns the stuck/unstick weighting and the heading persistence.
    glm::vec2 steerHeading(Tick& t, glm::vec2 goalDir, const Nav::TeamField& raster);
    void tickField(Tick& t);         // shield battery + push (emitter readbacks), or the baked-field stand-in

    oc::string m_shortName = "UNIT";
    float m_retargetTimer = 0.0f;
    float m_fireTimer = 0.0f;
    float m_attackTimer = 0.0f; // melee: counts down to the next hit; clamps at 0 with no victim
                                // in reach, so the first hit on arrival lands at once
    // The hurt light (see GameUnitParams): health is compared against the last tick's on EVERY
    // role — a drop is a drop whether the authority applied it or the replicated blob landed it.
    float m_lastHealth = 0.0f;
    float m_hurtGlow = 0.0f;    // 0..1, the light's fade
    uint8 m_hurtTeam = 0xFF;    // the last attacker's team (0xFF = unknown): the light's colour
    void noteHurtTeam(uint32 sourceTeam) { m_hurtTeam = sourceTeam == UnknownTeam ? 0xFF : (uint8)glm::min(sourceTeam, 254u); }
    uint32 opposingTeamGuess() const;   // field exposure with no team on the sample: the next team over
    float m_bodyTop = 1.0f;     // the light's height over the entity origin (the collider's top)
    uint32 m_rng = 0;           // tiny per-unit LCG — worker-safe, seeded from the entity address
    float m_pressureTimer = 0.0f; // stalled time (displacement checkpoints) -> weights + pressure
    // Lane requests are due at an ABSOLUTE sim time, not on a countdown of the tick delta: a
    // throttled tick's delta is clipped by "Max catch-up", so a countdown ran slow whenever the
    // cap bit. The request cadence must not depend on the SIM LOD tier.
    float m_seedDue = 0.0f; // sim seconds (Time::getSimElapsedSec) of the next request; the area limiter is global
    float m_ignoreFlowTimer = 0.0f; // > 0: the lane term is skipped (fresh move order)
    glm::vec2 m_stuckAnchor{ 0.0f };
    float m_stuckCheckTimer = 0.0f;
    bool m_hasStuckAnchor = false;
    glm::vec2 m_lastDir{ 0.0f };  // context steering persistence
    bool m_hasLastDir = false;
    float m_outputHistory[3] = { 0.8f, 0.8f, 0.8f }; // push normalizes by the ~2-frame-old output
};

export const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity);
export void writeGameUnitSpawnInfo(const GameUnitComponent::SpawnInfo& info, AssetNode& out);
