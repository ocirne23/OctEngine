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

export struct GameUnitParams
{
    float energyDrainRate = 25.0f; // energy/s per unit of pressure (player shield rule, no regen)
    float tension = 1.5f;          // drain + push scale by (1 + tension*pressure)
    float fieldDps = 10.0f;        // health/s while squished below damageRadius under pressure
    float fieldDpsMult = 1.0f;
    float fieldPushStart = 0.7f;   // push ramp start as a fraction of iso: below it no shove, so
                                   // units reach the damage band instead of parking in the fringe
    float emitterDrainMult = 0.25f;
    float strainRange = 20.0f;     // planar reach (m) of the siege drain: the Bastion's bubble radius
    float damageAbsorb = 2.0f;     // shield energy per hp of direct damage absorbed before health
    float damageRadius = 0.6f;     // equilibrium radius below this + pressure = exposure damage
    float pushGain = 20000.0f;     // shared by the emitter readback path AND the shield-less path
    float retargetInterval = 5.0f;
    float wanderSpeedMult = 0.25f;
    float wanderSpeedMax = 0.75f;  // m/s
    int localTeam = -1;            // the VIEWER's team: its units tint green on every instance
    int huntSeedTeam = -1;         // the ONE team that also seeds lanes toward HUNTED targets (the
                                   // co-op AI); every other team seeds only routes and move orders
    float targetSearchRadius = 15.0f; // LOCAL harassment only: the barracks route does the delivery
    float maxSpeed = 12.0f;        // absolute m/s cap on every unit body, every tick, any cause
                                   // (field shoves included): ~1.5x the runner's 7.6
    float waypointRadius = 3.0f;
    float farSpreadDeg = 40.0f;    // far walk: persistent per-unit heading bias so a wave fans out
    float routeEngageRadius = 5.0f;  // marching a route: an enemy this near is engaged, then resumed
    float orderBreakRadius = 15.0f;  // a MOVE ORDER drops at the first enemy structure this near
    float voidY = -3.0f;           // checked by the full sim AND the far tick
    float heightLimit = 5.0f;      // world Y ceiling for units AND player capsules (GamePlayer too)
    bool navEnabled = true;
    float hurtLightIntensity = 8.0f;
    float hurtLightDecay = 0.25f;  // seconds from full to dark
    float lightArea = 8.0f;        // m: hurt-flash budget bucket size (lock-free hashed slots)
    float hurtFlashRate = 4.0f;    // flashes per second per area
    // steerHeading score: free * (Goal*dot(goal) + Flow*laneW*dot(lane) + Persist*dot(last))
    //   - Pressure*gpW*dot(gradP) + Wall*dot(wallAway) - clipped*CornerClip
    float steerGoal = 0.3f;
    float steerFlow = 1.0f;        // a lane is a proven route: outweighs walking straight at the goal
    float flowSplatGain = 0.5f;    // scale on the MEASURED velocity splatted into the lane (0 = no trail)
    float steerPersist = 0.4f;
    float targetTrackRadius = 5.0f; // geodesic m: closer = field-tracking beats the (laggy) seeded lane
    float steerTrackGoal = 1.5f;   // goal weight floor while tracking
    float trackFlowMult = 0.15f;   // lane weight multiplier while tracking
    float navFollowRadius = 40.0f; // geodesic m: no target, but walk the crowd lane if one is here
    float steerPressure = 0.5f;
    float pressureKnee = 0.23f;    // pressure gradient scoring 0.5 (x/(x+knee))
    float flowKnee = 0.15f;        // lane speed scoring 0.5, as a fraction of moveSpeed
    float orderFlowBlind = 0.0f;   // s a freshly ordered unit ignores the lane (so it can turn around)
    float seedRequestInterval = 1.0f;
    float unstickAfter = 1.0f;     // s of stall after which goal/persistence are dropped
    float presencePressure = 0.01f; // pressure every unit injects per tick (x60/s)
    float steerLook = 6.0f;        // metres of whisker (min; scales with speed)
    float steerCornerClip = 0.7f;
    float steerWall = 1.0f;
    float wallKeep = 0.9f;         // metres beyond the body radius the wall push reaches
    float stuckPressure = 0.5f;    // pressure a stalled unit injects per second (x stall, <= 1.5)
};

// AUTHORITY: update() simulates only when this instance is not a network client; clients are state
// containers the mirrors write into. Cross-entity writes are atomic (damage/heal/addLoad), lookup is
// spatial only - nothing holds entity lists; the game drains the report queues below.
export struct GameUnitComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameUnit; }
    ~GameUnitComponent() {}

    static GameUnitParams params;

    struct FireRequest // ranged units ask for a shot; spawning is main-thread only
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 target{ 0.0f };
        uint8 team = 1;
        uint8 shotKind = 0;
    };
    static void takeFireRequests(oc::vector<FireRequest>& out);
    // Per-unit lane requests are affordable because NavSystem::requestSeedPath dedups by proximity.
    struct SeedRequest
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
        uint8 team = 1;
        bool stuck = false; // seeded as the weaker "stuck" lane
    };
    static void takeSeedRequests(oc::vector<SeedRequest>& out);
    struct DeathRecord // the barracks frees exactly this much of its population cap
    {
        uint32 sourceId = 0;
        uint8 popCost = 0;
    };
    static void takeDeaths(oc::vector<DeathRecord>& out);
    struct HitRecord // a melee hit that already landed (for the game's visual)
    {
        glm::vec3 from{ 0.0f };
        glm::vec3 to{ 0.0f };
    };
    static void takeHits(oc::vector<HitRecord>& out);
    static int liveCount(); // non-puppet instances; a unit at 0 hp counts until its destroy drains

    struct SpawnInfo
    {
        uint32 team = 1;
        bool puppet = false;
        oc::string shortName; // the 3-5 char HUD tag; empty = "UNIT"
        float healthMax = 60.0f;
        float energyMax = 40.0f;      // shield battery (no regen)
        float shieldOutput = 0.8f;
        float moveSpeed = 4.0f;
        float accel = 15.0f;          // soft on purpose: steering loses the shoving match vs bubbles
        float attackRange = 1.5f;     // melee reach measured to the victim's meleeRadius ring
        float attackInterval = 1.0f;  // melee is discrete: one hit of attackDamage per interval
        float attackDamage = 6.0f;
        float playerDamage = 10.0f;   // per hit on a PLAYER capsule
        float emitterDrain = 1.0f;    // energy/s this unit costs the nearest active enemy emitter
        bool ranged = false;
        float standoffRange = 16.0f;
        float fireInterval = 3.0f;
        uint8 shotKind = 0;           // 0 direct, 1 splash lob
        bool alwaysDisplayHealth = false;
        float heightLimit = 0.0f;     // 0 = params.heightLimit, > 0 = own ceiling, < 0 = NONE (flying)
    };

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
    bool alwaysDisplayHealth = false;
    float heightLimit = 0.0f;
    float effectiveHeightLimit() const // also read by GamePlayer for the capsule's puppet
    {
        if (heightLimit < 0.0f)
            return FLT_MAX;
        return heightLimit > 0.0f ? heightLimit : params.heightLimit;
    }
    float bodyRadius = 0.5f;    // planar collider radius; the nav line-of-sight tests use it

    // PUPPET (player capsules): a pure state carrier, update() runs no sim. GamePlayer writes it and
    // the entity sync's game blob moves it owner -> server -> other clients.
    bool puppet = false;
    float pendingDamage = 0.0f; // damage inbox: units drain it in their own tick (shield-first),
                                // puppets' is routed to the owning GamePlayer by the game
    float materialsFrac = 0.0f; // players: carried construction stock (server-authoritative)
    bool collapsed = false;     // latched at empty battery; only a heal lifts it
    bool deathReported = false;
    uint32 sourceId = 0;        // the spawner's stable id (rides the death event)
    uint8 popCost = 0;          // population this unit holds on its barracks (game-stamped at spawn)

    bool targetLocked = false;  // explicit DSL/game target overrides auto-targeting
    bool hasTarget = false;
    bool moveOrder = false;     // the lock is a MOVE ORDER: arrive, then unlock (a DSL attack lock never clears)
    glm::vec3 targetPos{ 0.0f };
    void orderMove(const glm::vec3& worldPos, bool fresh = true) // main thread; drops any route too
    {
        targetPos = worldPos;
        hasTarget = targetLocked = moveOrder = true;
        wanderOrder = false;
        routeIndex = routeCount;
        if (fresh)
            m_ignoreFlowTimer = params.orderFlowBlind;
    }
    bool wanderOrder = false;   // a move order that never seeds a lane and gives up after wanderTimeLeft
    float wanderTimeLeft = 0.0f;
    void orderWander(const glm::vec3& worldPos, float seconds) // main thread
    {
        orderMove(worldPos, /*fresh*/ false);
        wanderOrder = true;
        wanderTimeLeft = seconds;
    }
    static constexpr int MaxRoutePoints = 6;
    glm::vec3 route[MaxRoutePoints]{}; // copied in at spawn (the barracks route)
    uint8 routeCount = 0, routeIndex = 0;

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, float deltaSec);
    // FAR TICK: the stand-in for a unit outside the SIM LOD selection (body disabled). Walks the
    // route / move order by TELEPORT - no physics, combat or bubble. Returns true when it moved.
    bool updateFar(Entity& entity, float deltaSec);
    bool farHeading(const Entity& entity, glm::vec2& dir, float& speed, float& dist, bool spread = true);
    glm::vec3 wakeVelocity(const Entity& entity); // World's SIM LOD wake edge -> PhysicsComponent::unpark
    void kill(Entity& entity); // health to 0 + the one-shot death report
    static constexpr uint32 UnknownTeam = UINT32_MAX;
    void damage(float amount, uint32 sourceTeam = UnknownTeam); // atomic; sourceTeam colours the hurt light
    float takePendingDamage(); // main thread: drain the puppet inbox
    bool alive() const { return health > 0.0f; }
    void heal(float amount); // atomic inbox; applied in the unit's own tick (keeps `energy` single-writer)
    float pendingHeal = 0.0f;
    static void applyTeamTint(Entity& entity); // main thread; idempotent (spawn + replicated team)
    uint8 tintState = 0; // 0 authored, 1 friendly green, 2 authored (restored after a re-team)
    const char* getShortName() const { return m_shortName.c_str(); }

private:
    struct Tick // the frame's shared state, handed through the steps in order
    {
        Entity& entity;
        PhysicsComponent& pc;
        ForceComponent* fc;         // null = a SHIELD-LESS body (the swarm types)
        float deltaSec;
        glm::vec3 pos{ 0.0f };
        glm::vec2 here{ 0.0f };     // pos.xz
        bool fields = false;        // the Nav fields are on
        // Read ONCE at the top: every queued command builds on it, so a later SetLinearVelocity
        // never re-applies a climb the height limit cancelled.
        glm::vec3 vel{ 0.0f };
        glm::vec3 impulse{ 0.0f };  // the tick's field push, issued by applyPush as ONE ApplyImpulse
        bool routing = false;
        bool haveWalkTarget = false;
        bool walkIsOrder = false;   // a route waypoint or a move order, not a hunted enemy
        glm::vec3 walkTarget{ 0.0f };
        bool navResolved = false;   // a Nav team field settled the target (the local re-search stays off)
        bool navSteer = false;      // navResolved AND the descent is usable (it is zero AT a source)
        bool navTracking = false;   // target within targetTrackRadius
        glm::vec2 navDir{ 0.0f };
        float stopRange = 0.0f;     // hold this far from the walk target
        bool inEnemyBubble = false; // stamped-radius signal - the shield-less fallback when the bake is off
    };
    void tickHurtLight(const Entity& entity, float deltaSec); // every role, before the client gate
    void applyHeightLimit(Tick& t);
    bool applyDamageAndHeal(Tick& t); // false = the unit died this tick
    void resolveWalkTarget(Tick& t); // route, then the locked order, then the Nav fields, then the local search
    void resolveNavTarget(Tick& t);
    void searchLocalTarget(Tick& t);
    void tickCombat(Tick& t);        // ONE probe: structure bite, emitter strain, melee sweep, ranged shots
    void tickSteering(Tick& t);
    glm::vec2 steerHeading(Tick& t, glm::vec2 goalDir, const Nav::TeamField& raster);
    void tickField(Tick& t);         // shield battery + push (emitter readbacks), or the baked-field stand-in
    void applyPush(Tick& t);

    oc::string m_shortName = "UNIT";
    float m_retargetTimer = 0.0f;
    float m_fireTimer = 0.0f;
    float m_attackTimer = 0.0f; // clamps at 0 with no victim in reach, so the first hit on arrival lands at once
    float m_lastHealth = 0.0f;  // hurt light: compared on EVERY role (replicated drops count too)
    float m_hurtGlow = 0.0f;
    uint8 m_hurtTeam = 0xFF;    // the last attacker's team (0xFF = unknown)
    void noteHurtTeam(uint32 sourceTeam) { m_hurtTeam = sourceTeam == UnknownTeam ? 0xFF : (uint8)glm::min(sourceTeam, 254u); }
    uint32 opposingTeamGuess() const;
    float m_bodyTop = 1.0f;     // the collider's top over the entity origin
    uint32 m_rng = 0;           // per-unit LCG, worker-safe
    float m_pressureTimer = 0.0f; // stalled time (displacement checkpoints)
    // ABSOLUTE sim time, not a countdown: a throttled tick's delta is clipped by "Max catch-up",
    // and the request cadence must not depend on the SIM LOD tier.
    float m_seedDue = 0.0f;
    float m_ignoreFlowTimer = 0.0f;
    glm::vec2 m_stuckAnchor{ 0.0f };
    float m_stuckCheckTimer = 0.0f;
    bool m_hasStuckAnchor = false;
    glm::vec2 m_lastDir{ 0.0f };
    bool m_hasLastDir = false;
    float m_outputHistory[3] = { 0.8f, 0.8f, 0.8f }; // push normalizes by the ~2-frame-old output
};

export const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity);
export void writeGameUnitSpawnInfo(const GameUnitComponent::SpawnInfo& info, AssetNode& out);
