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
import Settings; // GameUnitParams (Settings.Game) + Globals::settings

// AUTHORITY: update() simulates only when this instance is not a network client; clients are state
// containers the mirrors write into. Cross-entity writes are atomic (damage/heal/addLoad), lookup is
// spatial only - nothing holds entity lists; the game drains the report queues below.
export struct GameUnitComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameUnit; }
    ~GameUnitComponent() {}

    static inline GameUnitParams& params = Globals::settings.game.unitParams; // the "Game/Enemies" + "Game/Nav" tweaks

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
    // SOUND CUES for the game's sound system (Game:Sound), from the unit and the projectile components. Reported from any
    // thread, on every role. BOUNDED: between two drains a kind keeps MaxSoundCues positions; a report over that is
    // only counted, so 30 000 deaths in one frame cost 30 000 atomic adds and 48 queue entries.
    // (There is no "hurt" cue: a unit that takes a hit makes no sound of its own; the hit does.)
    enum class ESoundCue : uint8 { UnitReveal, UnitDeath, ShotFired, LobFired, ShotImpact, SplashImpact, Count };
    struct SoundCue
    {
        glm::vec3 pos{ 0.0f };
        float size = 1.0f; // the body radius (a unit) or the splash radius (a shell)
        ESoundCue kind = ESoundCue::UnitDeath;
    };
    static constexpr uint32 MaxSoundCues = 48;
    static void reportSound(ESoundCue kind, const glm::vec3& pos, float size = 1.0f);
    // counts: every report of the kind since the last drain, the ones over the bound included.
    static void takeSoundCues(oc::vector<SoundCue>& out, uint32 counts[(int)ESoundCue::Count]);
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
        float structureDamage = -1.0f; // per bite on a STRUCTURE, before params.structureBiteMult; < 0 = attackDamage (the siege units author more)
        float emitterDrain = 1.0f;    // energy/s this unit costs the nearest active enemy emitter
        bool ranged = false;
        float standoffRange = 16.0f;
        float fireInterval = 3.0f;
        uint8 shotKind = 0;           // 0 direct, 1 splash lob
        bool alwaysDisplayHealth = false;
        bool fieldBlocked = false;    // an enemy field pushes this unit even where params.fieldPush is off (a unit with a bubble only)
        float heightLimit = 0.0f;     // 0 = params.heightLimit, > 0 = own ceiling, < 0 = NONE (flying)
        glm::vec3 color{ -1.0f };     // x >= 0: the unit's colour on EVERY render node of its subtree (a shared model prefab has none)
    };

    uint32 team = 1;
    float health = 60.0f, healthMax = 60.0f;
    float energy = 40.0f, energyMax = 40.0f;
    float shieldOutput = 0.8f;
    float moveSpeed = 4.0f, accel = 15.0f;
    float attackRange = 1.5f, attackInterval = 1.0f, attackDamage = 6.0f, playerDamage = 10.0f;
    float structureDamage = 6.0f; // per bite on a structure, before params.structureBiteMult (resolved at spawn: never < 0)
    float emitterDrain = 1.0f;
    bool ranged = false;
    float standoffRange = 16.0f, fireInterval = 3.0f;
    uint8 shotKind = 0;
    bool alwaysDisplayHealth = false;
    bool fieldBlocked = false;
    float heightLimit = 0.0f;
    float effectiveHeightLimit() const // also read by GamePlayer for the capsule's puppet
    {
        if (heightLimit < 0.0f || !params.flatGround) // the sandbox terrain world has no absolute ceiling
            return FLT_MAX;
        return heightLimit > 0.0f ? heightLimit : params.heightLimit;
    }
    float bodyRadius = 0.5f;    // planar collider radius; the nav line-of-sight tests use it

    // PUPPET (player capsules): a pure state carrier, update() runs no sim. GamePlayer writes it and
    // the entity sync's game blob moves it owner -> server -> other clients.
    bool puppet = false;
    float pendingDamage = 0.0f; // damage inbox: units drain it in their own tick (shield-first),
                                // puppets' is routed to the owning GamePlayer by the game
    float pendingShadowDamage = 0.0f; // ... the part that hurts a SHADOW too (damage's throughShadow: a cannon shell)
    float pendingShieldDrain = 0.0f;  // battery energy taken by deflected shots and barracks units in the bubble (drainShield; tickField applies it)
    float materialsFrac = 0.0f; // players: carried construction stock (server-authoritative)
    bool collapsed = false;     // latched at empty battery; only a heal lifts it
    bool deathReported = false;
    uint32 sourceId = 0;        // the spawner's stable id (rides the death event)
    uint8 popCost = 0;          // population this unit holds on its barracks (game-stamped at spawn)
    float buildCost = 0.0f;     // light its type costs a barracks (game-stamped at spawn); a medic's heal is paid from it

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
    // atomic. throughShadow: it hurts the unit even while it is a shadow (not solid).
    void damage(float amount, bool throughShadow = false);
    float takePendingDamage(); // main thread: drain the puppet inbox (both parts)
    bool alive() const { return health > 0.0f; }
    // The bubble is GONE: the battery is empty and the collapse has shrunk it to nothing (authority; a client never
    // runs the collapse, so it says false there). Its emitter is then switched off - World's bubble gate reads this.
    bool shieldGone() const { return energy <= 0.0f && m_shieldFade <= 0.0f; }
    void heal(float amount); // atomic inbox; applied in the unit's own tick (keeps `energy` single-writer)
    // The same: a deflected shot's cost to the battery (GameProjectileComponent's FieldDeflect), and a barracks unit standing
    // in the bubble (tickCombat).
    void drainShield(float amount);
    float pendingHeal = 0.0f;
    static void applyTeamTint(Entity& entity); // main thread; idempotent (spawn + replicated team)
    uint8 tintState = 0; // 0 authored, 1 friendly green, 2 authored (restored after a re-team)
    // SHADOW FORM (params.shadowTeam): a unit of that team is SOLID only in an opposing field or in daylight. While not
    // solid it takes no direct damage (its inbox is dropped) and turrets do not pick it. Written by its own tickField,
    // read by other entities' ticks (a stale read is one tick old).
    bool solid = true;
    // THE SLOW TEAM (params.slowTeam): its units walk at slowMult x their MoveSpeed, and at fieldSlowMult x that while
    // they stand in an opposing field (inField, set by tickField) - a field does not push them, it holds them back.
    bool inField = false;
    float speedScale() const
    {
        return (int)team != params.slowTeam ? 1.0f : params.slowMult * (inField ? params.fieldSlowMult : 1.0f);
    }
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
        bool walkIsTarget = false;  // the walk target IS an enemy (Nav source, local search, engage): a ranged unit fires only at one
        glm::vec3 walkTarget{ 0.0f };
        bool navResolved = false;   // a Nav team field settled the target (the local re-search stays off)
        bool navSteer = false;      // navResolved AND the descent is usable (it is zero AT a source)
        bool navTracking = false;   // target within targetTrackRadius
        glm::vec2 navDir{ 0.0f };
        float stopRange = 0.0f;     // hold this far from the walk target
        bool inEnemyBubble = false; // stamped-radius signal - the shield-less fallback when the bake is off
        int8 sheltered = -1;        // isSheltered's answer this tick (-1 = not asked yet)
    };
    // SHELTERED: the point it stands on is owned by a bubble of its OWN team (the baked field: its team's field is the
    // strongest there and over the iso value) - no enemy field or light cone reveals it there. With no bake: its own
    // bubble while its battery holds. Asked lazily, once a tick (one bake sample).
    bool isSheltered(Tick& t);
    void tickDeathCue(const Entity& entity);                  // every role, before the client gate
    void tickModel(Entity& entity, float deltaSec);           // every role too: works off the position delta
    void applyHeightLimit(Tick& t);
    bool applyDamageAndHeal(Tick& t); // false = the unit died this tick
    void resolveWalkTarget(Tick& t); // route, then the locked order, then the Nav fields, then the local search
    void resolveNavTarget(Tick& t);
    void searchLocalTarget(Tick& t);
    void tickCombat(Tick& t);        // ONE probe: structure bite, emitter strain, melee sweep, ranged shots
    void tickSteering(Tick& t);
    glm::vec2 steerHeading(Tick& t, glm::vec2 goalDir, const Nav::TeamField& raster);
    void tickField(Tick& t);         // tickShieldBubble for a live bubble, else tickBakedField
    void tickShieldBubble(Tick& t);  // the battery, the collapse and the field push (the emitter's readbacks)
    void tickBakedField(Tick& t);    // the reveal and the burn off the baked field: no push
    void applyPush(Tick& t);
    void setSolid(Tick& t, float opposing, float iso); // the shadow form's state + its dark tint, from tickField

    oc::string m_shortName = "UNIT";
    float m_retargetTimer = 0.0f;
    float m_fireTimer = 0.0f;
    float m_attackTimer = 0.0f; // clamps at 0 with no victim in reach, so the first hit on arrival lands at once
    float m_lastHealth = 0.0f;  // the death cue: compared on EVERY role (replicated drops count too)
    float m_bodyTop = 1.0f;     // the collider's top over the entity origin
    glm::vec2 m_modelLastPos{ 0.0f }; // tickModel: the MODEL is the child with a HumanoidAnimatorComponent
    float m_modelYaw = 0.0f;
    glm::vec2 m_modelForward{ 1.0f, 0.0f }; // of m_modelYaw: the aligned test needs no trig
    bool m_hasModelPos = false;
    bool m_modelResting = false; // standing, and the model reported its rest pose: tickModel returns at the top
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
    float m_shieldFade = 1.0f; // 1 = the bubble at full output; an empty battery takes it to 0 over a short time (tickField)
};

export const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity);
export void writeGameUnitSpawnInfo(const GameUnitComponent::SpawnInfo& info, AssetNode& out);
