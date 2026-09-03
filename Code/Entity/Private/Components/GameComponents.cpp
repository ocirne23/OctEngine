module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;
import Physics;
import Spatial;
import Nav;
import RendererVK; // applyTeamTint's material override

// See GameComponents.ixx for the design + authority/thread contract. Everything here runs either
// on the parallel entity pass (update — authority instances only) or on the main thread (spawn,
// onContact); cross-entity writes are the atomic CAS helpers, cross-entity lookup is spatial.

GameUnitParams GameUnitComponent::params;
GameStructureParams GameStructureComponent::params;
GameProjectileParams GameProjectileComponent::params;

// Worker-side reports, drained by the game (see the queues' declarations). The mutex only ever
// guards two small append-only vectors touched on the rare tick where a unit fires or dies.
static std::mutex g_unitEventMutex;
static oc::vector<GameUnitComponent::FireRequest> g_fireRequests;
static oc::vector<GameUnitComponent::DeathRecord> g_deaths;
static oc::vector<GameUnitComponent::SeedRequest> g_seedRequests;

void GameUnitComponent::takeFireRequests(oc::vector<FireRequest>& out)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    out.swap(g_fireRequests);
    g_fireRequests.clear();
}

void GameUnitComponent::takeSeedRequests(oc::vector<SeedRequest>& out)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    out.swap(g_seedRequests);
    g_seedRequests.clear();
}

void GameUnitComponent::takeDeaths(oc::vector<DeathRecord>& out)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    out.swap(g_deaths);
    g_deaths.clear();
}

static bool isAuthority()
{
    return Globals::networkManager.role() != ENetRole::Client;
}

static void atomicSubClamped(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    float next;
    do { next = glm::max(cur - amount, 0.0f); } while (!ref.compare_exchange_weak(cur, next));
}

static void atomicAdd(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    while (!ref.compare_exchange_weak(cur, cur + amount)) {}
}

// ---------------------------------------------------------------- GameUnitComponent

static float unitRand01(uint32& state); // tiny per-unit LCG, defined below

void GameUnitComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    if (const ForceComponent::SpawnInfo* si = getForceSpawnInfo(&entity))
    {
        entity.setProfiled(); // units with shields carry a per-entity profile scope
    }
    puppet = info.puppet;
    if (!info.shortName.empty())
        m_shortName = Globals::profiler.internName(info.shortName);
    team = info.team;
    health = healthMax = info.healthMax;
    shieldOutput = info.shieldOutput;
    // No shield = no battery: a shield-less body (swarm) carries zero energy, so nothing —
    // damage absorb, the label pass's shield-vs-health branch — ever mistakes it for shielded.
    energy = energyMax = shieldOutput > 0.0f ? info.energyMax : 0.0f;
    moveSpeed = info.moveSpeed;
    accel = info.accel;
    attackRange = info.attackRange;
    attackDps = info.attackDps;
    playerDps = info.playerDps;
    emitterDrain = info.emitterDrain;
    ranged = info.ranged;
    standoffRange = info.standoffRange;
    fireInterval = info.fireInterval;
    shotKind = info.shotKind;
    alwaysDisplayHealth = info.alwaysDisplayHealth;
    heightLimit = info.heightLimit;
    for (float& h : m_outputHistory)
        h = shieldOutput;
    m_rng = uint32(uintptr_t(this) >> 4) * 2654435761u + 1u; // worker-safe per-unit stream
    m_retargetTimer = 0.0f; // pick a target on the first authority tick
    // Random PHASE on the periodic timers: a barracks batch spawns in one frame, and without this
    // every unit of it would ask for a plan (and checkpoint its progress) on the same tick forever.
    m_seedTimer = params.seedRequestInterval * unitRand01(m_rng);
    m_stuckCheckTimer = 0.75f * unitRand01(m_rng);
    if (const PhysicsComponent::SpawnInfo* si = getPhysicsSpawnInfo(&entity))
    {
        float r = 0.5f;
        switch (si->shape.type)
        {
        case EPhysicsShapeType::Box:     r = glm::max(si->shape.halfExtents.x, si->shape.halfExtents.z); break;
        case EPhysicsShapeType::Sphere:
        case EPhysicsShapeType::Capsule: r = si->shape.radius; break;
        default: break;
        }
        bodyRadius = r * entity.scale;
    }
}

// Tiny LCG: units roll targets/jitters on WORKERS — the engine script RNG is fine but this keeps
// each unit's stream independent of scheduling order.
static float unitRand01(uint32& state)
{
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) * (1.0f / 16777216.0f);
}

void GameUnitComponent::update(Entity& entity, float deltaSec)
{
    if (!isAuthority())
        return; // clients mirror via the entity sync's game blob
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    ForceComponent* fc = getComponent<ForceComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return;
    const glm::vec3 pos = pc->body.getPosition();
    const glm::vec2 here(pos.x, pos.z);
    const bool fields = params.navEnabled && Globals::navSystem.isEnabled();

    // Crowd presence: a weak pressure source (players too) — the diffused field is a smoothed
    // crowd density that keeps units spaced. The stall injection below is far stronger.
    if (fields && params.presencePressure > 0.0f)
        Globals::navSystem.pressure(team).inject(here, params.presencePressure * deltaSec * 60.0f);
    // ---- HEIGHT LIMIT: launched above the ceiling -> put back AT the ceiling, climb cancelled
    // (the queued velocity keeps the planar part). Teleport contract: stomp the interpolation
    // poses and claim the step, or PhysicsComponent::update mixes toward the pre-teleport pose on
    // stepping frames. Runs BEFORE the puppet gate on purpose: the server's twins of client
    // capsules are held under the same ceiling their owners clamp themselves to in GamePlayer, so
    // both land at the same height and the owner's next claim re-anchors instead of fighting.
    // `vel` is read ONCE here and carried into the steering below, so a later SetLinearVelocity
    // command never re-applies the cancelled climb.
    glm::vec3 vel = pc->body.getLinearVelocity();
    if (const float ceiling = effectiveHeightLimit(); pos.y > ceiling)
    {
        const glm::vec3 clamped(pos.x, ceiling, pos.z);
        Globals::physics.teleportBody(pc->body, clamped, pc->body.getRotation());
        vel.y = glm::min(vel.y, 0.0f);
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, vel);
        pc->prevPos = pc->currPos = clamped;
        pc->lastStep = Globals::physics.getStepCount();
    }
    if (puppet)
        return; // state carrier: GamePlayer writes it
    // Direct damage lands via the inbox — the battery eats it FIRST, exactly the player's
    // applyDamage rule ("Damage absorb" energy per hp, collapse latch on empty), and only the
    // overflow reaches health. Shield-less bodies (swarm: shieldOutput 0) take it all on health.
    if (const float dmg = takePendingDamage(); dmg > 0.0f)
    {
        float remaining = dmg;
        if (!collapsed && shieldOutput > 0.0f && params.damageAbsorb > 0.0f && energy > 0.0f)
        {
            const float absorbedHp = glm::min(remaining, energy / params.damageAbsorb);
            energy = glm::max(energy - absorbedHp * params.damageAbsorb, 0.0f);
            remaining -= absorbedHp;
            if (energy <= 0.0f)
                collapsed = true;
        }
        if (remaining > 0.0f)
            health = glm::max(health - remaining, 0.0f);
    }
    // The DEATH check runs on the damage alone: a heal that landed the same tick must not revive a
    // unit the damage just killed (a medic station's radius was making units immortal).
    if (health <= 0.0f || pos.y < params.voidY)
    {
        kill(entity);
        return;
    }
    // The HEAL inbox (medic stations), for the survivors: health and the battery both, applied
    // here so `energy` stays single-writer; a battery holding charge again lifts the permanent
    // collapse latch.
    if (const float heal = oc::atomic_ref<float>(pendingHeal).exchange(0.0f, oc::memory_order_acq_rel); heal > 0.0f)
    {
        health = glm::min(health + heal, healthMax);
        energy = glm::min(energy + heal, energyMax);
        if (energy > 0.0f)
            collapsed = false;
    }

    // ---- where to walk: route, then the locked order, then nav fields, then local search ----
    if (targetLocked && moveOrder
        && glm::distance(here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
        targetLocked = moveOrder = false; // move order arrived: back to the AI
    if (targetLocked && moveOrder && wanderOrder)
    {
        wanderTimeLeft -= deltaSec; // a wander that cannot get there just gives up
        if (wanderTimeLeft <= 0.0f)
            targetLocked = moveOrder = false;
    }
    bool routing = false;
    glm::vec3 walkTarget = pos;
    bool haveWalkTarget = false;
    bool walkIsOrder = false; // the walk target is a route waypoint or a move order, not a hunted enemy
    if (routeIndex < routeCount)
    {
        if (glm::distance(here, glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
        if (routeIndex < routeCount)
        {
            walkTarget = route[routeIndex];
            routing = haveWalkTarget = walkIsOrder = true;
        }
    }
    // NAV: the geodesically nearest enemy across every other team's field; its descent direction
    // already routes around walls. Falls through to the local search where no field covers us.
    bool navResolved = false;
    bool navSteer = false;    // navResolved AND the descent is usable (it is zero AT a source)
    bool navTracking = false; // target within targetTrackRadius: field-tracking gets steering priority
    glm::vec2 navDir(0.0f);
    if (!routing && !targetLocked && fields && Globals::navSystem.anyFieldPublished())
    {
        Nav::TeamField::Sample best;
        const Nav::TeamField* bestField = nullptr;
        for (uint32 t = 0; t < Nav::MaxTeams; ++t)
        {
            if (t == team)
                continue;
            const Nav::TeamField* field = Globals::navSystem.teamField(t);
            if (!field)
                continue;
            const Nav::TeamField::Sample s = field->sample(here, m_rng);
            if (s.valid && (!best.valid || s.dist < best.dist))
            {
                best = s;
                bestField = field;
            }
        }
        // RANGE-GATED: only a source within targetSearchRadius (geodesic metres) counts. The field
        // rebuilds every ~0.25 s and its sources are LIVE positions, so this tracks a moving
        // player far tighter than the local search's retarget-interval snapshots — and the gate is
        // what keeps distant units holding their patch instead of marching across the map.
        if (best.valid && best.dist <= params.targetSearchRadius)
        {
            targetPos = bestField->sourceAt(best.srcIndex).pos;
            hasTarget = true;
            walkTarget = targetPos;
            haveWalkTarget = true;
            navDir = best.descentDir;
            navResolved = true;
            navTracking = best.dist <= params.targetTrackRadius;
            navSteer = glm::dot(navDir, navDir) > 0.5f;
        }
        else
        {
            // OUT OF THE SEARCH RADIUS: a target picked on an earlier tick is STALE — drop it
            // (targetLocked never gets here). Kept, the unit marched to the last known spot for up
            // to a whole retarget interval and looked as if it ignored the follow radius entirely.
            hasTarget = false;
            if (best.valid && best.dist <= params.navFollowRadius)
            {
                // NAV FOLLOW band: too far to TARGET (the player sprinted out of the search
                // radius), but still near the action — if the crowd FLOW field holds a lane here,
                // walk it (no target, no combat lock). The chasers' own trail + the seeded lane
                // keep pulling the pack along until the target is back in range or the lane decays.
                if (const Nav::TeamField* raster = Globals::navSystem.raster())
                {
                    const glm::vec2 lane = Globals::navSystem.flow(team).sample(here, raster);
                    const float laneLen = glm::length(lane);
                    if (laneLen > params.flowKnee * moveSpeed)
                    {
                        navDir = lane / laneLen;
                        navSteer = true;
                        navResolved = true; // following, not hunting: the local re-search stays off
                        walkTarget = pos + glm::vec3(navDir.x, 0.0f, navDir.y) * 8.0f;
                        haveWalkTarget = true;
                    }
                }
            }
        }
    }
    if (!routing && !navResolved)
    {
        m_retargetTimer -= deltaSec;
        if (!targetLocked && (m_retargetTimer <= 0.0f || !hasTarget))
        {
            // Random pick among the 4 nearest enemy structures; nearest enemy player (a puppet in
            // the same query) as the fallback.
            m_retargetTimer = params.retargetInterval * (0.7f + 0.6f * unitRand01(m_rng));
            thread_local oc::vector<uint64> results;
            Globals::spatialIndex.querySphere(glm::dvec3(pos), params.targetSearchRadius,
                SpatialLayer_Render, results);
            struct Candidate { float distSq; glm::vec3 pos; };
            Candidate best[4];
            int count = 0;
            float bestPlayerDistSq = FLT_MAX;
            glm::vec3 bestPlayerPos(0.0f);
            for (const uint64 user : results)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                if (const GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
                    pu && pu->puppet && pu->team != team && pu->alive())
                {
                    const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - here;
                    if (glm::dot(d, d) < bestPlayerDistSq)
                    {
                        bestPlayerDistSq = glm::dot(d, d);
                        bestPlayerPos = other->pos;
                    }
                    continue;
                }
                const GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
                if (!sc || sc->invulnerable || sc->team == team || !sc->alive())
                    continue;
                const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - here;
                const Candidate c{ glm::dot(d, d), other->pos };
                if (count < 4)
                    best[count++] = c;
                else
                {
                    int worst = 0;
                    for (int i = 1; i < 4; ++i)
                        if (best[i].distSq > best[worst].distSq)
                            worst = i;
                    if (c.distSq < best[worst].distSq)
                        best[worst] = c;
                }
            }
            if (count > 0)
            {
                targetPos = best[glm::min(int(unitRand01(m_rng) * count), count - 1)].pos;
                hasTarget = true;
            }
            else if (bestPlayerDistSq < FLT_MAX)
            {
                targetPos = bestPlayerPos;
                hasTarget = true;
            }
            else
                hasTarget = false;
        }
        walkTarget = targetPos;
        haveWalkTarget = hasTarget;
        walkIsOrder = hasTarget && targetLocked && moveOrder && !wanderOrder; // a wander never seeds
    }

    // ---- combat: ONE short probe serves the structure bite, the emitter strain and the melee
    // sweep over enemy players/units (decoupled from the walk target: a wall in the way gets
    // chewed too). The strain reach must fit inside the query radius. A unit MARCHING A ROUTE
    // runs it too: the nearest enemy unit inside routeEngageRadius becomes its walk target for
    // this tick (melee closes in, ranged fires at it), and the march resumes once it is gone.
    float stopRange = attackRange;
    bool inEnemyBubble = false; // stamped-radius signal — the shield-less FALLBACK when the baked
                                // field is off (see the field block at the end)
    {
        constexpr float c_strainRange = 12.0f; // emitter siege-drain reach
        const float engageRadius = routing ? params.routeEngageRadius : 0.0f;
        // A MOVE ORDER (the wave's march on the Base, a player's RMB) breaks off at the first
        // enemy structure inside orderBreakRadius: the order drops and the AI takes over, which
        // hunts the NEAREST structure — otherwise the whole wave walked past everything to the
        // Base and only bit what stood in its way.
        const bool ordered = targetLocked && moveOrder;
        const float breakRadius = ordered ? params.orderBreakRadius : 0.0f;
        thread_local oc::vector<uint64> nearby;
        Globals::spatialIndex.querySphere(glm::dvec3(pos),
            glm::max(glm::max(glm::max(attackRange + 6.0f, c_strainRange), engageRadius), breakRadius),
            SpatialLayer_Render, nearby);
        glm::vec3 bitePos(0.0f);
        float engageDistSq = engageRadius * engageRadius;
        glm::vec3 engagePos(0.0f);
        bool engage = false;
        // Unit-vs-unit melee hits ONE victim: the nearest enemy unit inside reach (players in the
        // swarm still take the area damage from every adjacent unit).
        GameUnitComponent* meleeVictim = nullptr;
        float meleeVictimDistSq = FLT_MAX;
        float meleeVictimReach = 0.0f;
        GameStructureComponent* bite = nullptr;
        GameStructureComponent* strain = nullptr;
        float biteDist = FLT_MAX, strainDist = c_strainRange;
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            if (GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
                sc && sc->team != team)
            {
                const float d = glm::distance(here, glm::vec2(other->pos.x, other->pos.z));
                if (sc->strainable && d < strainDist) // shield-state-independent siege drain
                {
                    strainDist = d;
                    strain = sc;
                }
                inEnemyBubble |= sc->strainable && d < sc->bubbleRadius;
                if (!sc->invulnerable && sc->alive() && d - sc->meleeRadius < biteDist)
                {
                    biteDist = d - sc->meleeRadius;
                    bite = sc;
                    bitePos = other->pos;
                }
                continue;
            }
            GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
            if (!pu || pu == this || pu->team == team || !pu->alive()
                || glm::abs(other->pos.y - pos.y) >= 3.0f)
                continue;
            const glm::vec2 to(other->pos.x - pos.x, other->pos.z - pos.z);
            if (routing && glm::dot(to, to) < engageDistSq) // nearest enemy on the march
            {
                engageDistSq = glm::dot(to, to);
                engagePos = other->pos;
                engage = true;
            }
            if (pu->puppet) // standing in the swarm hurts — no targeting needed
            {
                const float reach = attackRange + 0.8f; // capsule allowance
                if (playerDps > 0.0f && glm::dot(to, to) < reach * reach)
                    pu->damage(playerDps * deltaSec); // atomic: banks into the puppet inbox
            }
            else if (!ranged) // unit-vs-unit melee at the victim's body ring
            {
                const float reach = attackRange + pu->bodyRadius;
                const float distSq = glm::dot(to, to);
                if (distSq < reach * reach && distSq < meleeVictimDistSq)
                {
                    meleeVictim = pu;
                    meleeVictimDistSq = distSq;
                    meleeVictimReach = reach;
                }
            }
        }
        if (meleeVictim)
        {
            meleeVictim->damage(attackDps * deltaSec);
            stopRange = glm::max(stopRange, meleeVictimReach); // hold at the ring
        }
        if (!engage && routing && bite && biteDist < engageRadius)
        {
            engage = true; // a structure on the march is engaged like a unit (units first)
            engagePos = bitePos;
        }
        if (engage)
        {
            walkTarget = engagePos; // the route waypoint waits (routeIndex is untouched)
            haveWalkTarget = true;
            walkIsOrder = false;
        }
        if (ordered && bite && biteDist < breakRadius)
        {
            targetLocked = moveOrder = false; // the order is done: the AI hunts from here
            hasTarget = false;
            walkTarget = bitePos; // this tick already heads for it
            haveWalkTarget = true;
            walkIsOrder = false;
        }
        if (ranged)
        {
            stopRange = standoffRange;
            m_fireTimer -= deltaSec;
            if (haveWalkTarget && m_fireTimer <= 0.0f
                && glm::distance(here, glm::vec2(walkTarget.x, walkTarget.z)) <= standoffRange)
            {
                // Spawning is main-thread only: queue the shot for the game to service.
                const std::lock_guard<std::mutex> lock(g_unitEventMutex);
                g_fireRequests.push_back(FireRequest{ pos, walkTarget, (uint8)team, shotKind });
                m_fireTimer = fireInterval * (0.8f + 0.4f * unitRand01(m_rng));
            }
        }
        else if (bite && biteDist <= attackRange && pos.y > -2.0f && pos.y < 8.0f)
        {
            bite->damage(attackDps * deltaSec); // atomic — workers bite concurrently
            stopRange = attackRange + bite->meleeRadius;
        }
        if (strain)
            strain->addLoad(emitterDrain * params.emitterDrainMult);
    }

    // ---- steering: CONTEXT STEERING over the nav fields — score a fan of headings by goal
    // alignment, open run, crowd lane, persistence, minus the pressure gradient; stalls shift the
    // weights toward the fields. Physics writes are QUEUED (workers): one frame of latency.
    // (`vel` was read at the top, next to the height limit.)
    // BRAKE (no target, or arrived): the capsules run FRICTION 0 — the SIM LOD ticks them at up
    // to 1 s intervals and ground friction between ticks bled the commanded speed away — so
    // stopping is an explicit command too: planar velocity to zero at the steering accel.
    // Without it a coasting unit never stops, keeps splatting its velocity into the crowd lane,
    // and the pack follows the ghost trail. No lane splat while braking (only the walk branch).
    const auto brake = [&]
    {
        const glm::vec3 planar(vel.x, 0.0f, vel.z);
        const float speed = glm::length(planar);
        if (speed < 1e-3f)
            return;
        const float maxDv = accel * deltaSec;
        const glm::vec3 dv = speed > maxDv ? -planar * (maxDv / speed) : -planar;
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, vel + dv);
        vel += dv;
    };
    if (haveWalkTarget)
    {
        const glm::vec2 toTarget(walkTarget.x - pos.x, walkTarget.z - pos.z);
        const float dist = glm::length(toTarget);
        if (dist > stopRange)
        {
            glm::vec2 goalDir = navSteer ? navDir : toTarget / glm::max(dist, 1e-3f);
            glm::vec2 dir = goalDir;
            const Nav::TeamField* raster = fields ? Globals::navSystem.raster() : nullptr;
            // PLAN REQUEST on a jittered per-unit timer, while walking (not only while stuck): Nav
            // dedups by proximity, so a crowd going the same way costs one plan and the lane keeps
            // up with the group. Only ROUTES and MOVE ORDERS seed — a HUNTED target (nav field,
            // local search, engage) seeds only for params.huntSeedTeam (the co-op AI): friendly
            // units chasing an enemy must not carve lanes toward it.
            m_seedTimer -= deltaSec;
            // A WANDER never seeds, hunt-seed team or not (the co-op AI is that team, and its
            // strolls were carving lanes to random points).
            const bool wandering = targetLocked && moveOrder && wanderOrder;
            if (m_seedTimer <= 0.0f && !wandering && (walkIsOrder || (int)team == params.huntSeedTeam))
            {
                m_seedTimer = params.seedRequestInterval * (0.75f + 0.5f * unitRand01(m_rng));
                const std::lock_guard<std::mutex> lock(g_unitEventMutex);
                g_seedRequests.push_back(SeedRequest{ pos, walkTarget, (uint8)team,
                    m_pressureTimer > params.unstickAfter });
            }
            // STUCK DETECTION by displacement checkpoints (per-tick progress never accumulates on
            // a jittering heading): < 0.6 m per 0.75 s accrues stalled time into m_pressureTimer.
            m_stuckCheckTimer -= deltaSec;
            if (m_stuckCheckTimer <= 0.0f)
            {
                const float moved = m_hasStuckAnchor ? glm::distance(here, m_stuckAnchor) : 10.0f;
                m_pressureTimer = moved < 0.6f ? m_pressureTimer + 0.75f : 0.0f;
                m_stuckAnchor = here;
                m_hasStuckAnchor = true;
                m_stuckCheckTimer = 0.75f;
            }
            if (raster)
            {
                const bool stalled = m_pressureTimer > 0.4f;
                const bool unstick = m_pressureTimer > params.unstickAfter;
                float wGoal = unstick ? 0.0f : params.steerGoal * (stalled ? 0.3f : 1.0f);
                const float look = glm::max(params.steerLook, moveSpeed * 1.0f);
                // ONE raster snapshot for every probe this tick (the chunk-hash helpers pay a
                // find() per cell — 100+ per unit per tick without it).
                Nav::TeamField::CostWindow window;
                raster->snapshotCosts(here, int(std::ceil((look + bodyRadius + params.wallKeep)
                    / Nav::CellSize)) + 1, window);
                // Wall keep-away: in a gap both walls cancel (the unit centres itself), at a
                // corner the single push swings it wide. The run itself is a centre-line march so
                // a 1-tile gap reads as open.
                const glm::vec2 wallAway = window.wallPush(here, bodyRadius + params.wallKeep);
                const float wallLen = glm::length(wallAway);
                const glm::vec2 wallDir = wallLen > 1e-4f ? wallAway / wallLen : glm::vec2(0.0f);
                const float wallW = glm::min(wallLen, 1.0f) * params.steerWall;
                if (unstick)
                {
                    // Really stuck: replace the goal with an ESCAPE, backing away from whatever
                    // pins us — a nearby wall first (the wall push already points away from it),
                    // else the nearest unit/structure/player from a small spatial query, else
                    // simply the opposite of where we were trying to go.
                    if (wallLen > 1e-3f)
                        goalDir = wallDir;
                    else
                    {
                        thread_local oc::vector<uint64> crowd;
                        Globals::spatialIndex.querySphere(glm::dvec3(pos), 4.0,
                            SpatialLayer_Render, crowd);
                        float bestSq = FLT_MAX;
                        glm::vec2 away(0.0f);
                        for (const uint64 user : crowd)
                        {
                            Entity* other = reinterpret_cast<Entity*>(user);
                            if (other == &entity || (!getComponent<GameUnitComponent>(other)
                                && !getComponent<GameStructureComponent>(other)))
                                continue; // only the things that can pin us, never scenery
                            const glm::vec2 d = here - glm::vec2(other->pos.x, other->pos.z);
                            const float dSq = glm::dot(d, d);
                            if (dSq > 1e-6f && dSq < bestSq)
                            {
                                bestSq = dSq;
                                away = d;
                            }
                        }
                        goalDir = bestSq < FLT_MAX ? away / std::sqrt(bestSq) : -goalDir;
                    }
                    wGoal = 1.5f;
                }
                m_ignoreFlowTimer = glm::max(0.0f, m_ignoreFlowTimer - deltaSec);
                // LIVE TARGET WITHIN THE TRACK RADIUS: the goal is the team field's descent at the
                // target's LIVE position (~0.25 s fresh) — floor the goal weight up and near-mute
                // the seeded lane, whose periodic re-plans lag a moving player badly. Farther (but
                // inside the search radius) the unit marches lane-friendly toward the target.
                const bool tracking = navTracking && !unstick;
                if (tracking)
                    wGoal = glm::max(wGoal, params.steerTrackGoal * (stalled ? 0.3f : 1.0f));
                const float wFlow = m_ignoreFlowTimer > 0.0f ? 0.0f
                    : params.steerFlow * (unstick ? 0.5f : stalled ? 2.0f : 1.0f)
                    * (tracking ? params.trackFlowMult : 1.0f);
                const float wPersist = unstick ? 0.0f : params.steerPersist * (stalled ? 0.2f : 1.0f);
                const float wPressure = params.steerPressure * (unstick ? 3.0f : 1.0f);
                const float bodyProbe = bodyRadius + 0.1f;
                // Compressive response x/(x+knee): one stuck unit registers, a hundred never
                // saturate. The knee is the value scoring 0.5.
                const auto knee = [](float x, float k) { return x / (x + glm::max(k, 1e-4f)); };
                glm::vec2 lane = Globals::navSystem.flow(team).sample(here, raster);
                const float laneLen = glm::length(lane);
                const float laneW = knee(laneLen, params.flowKnee * glm::max(moveSpeed, 0.1f));
                if (laneLen > 1e-4f) lane /= laneLen;
                glm::vec2 gp = Globals::navSystem.pressure(team).gradient(here, raster);
                const float gpLen = glm::length(gp);
                const float gpW = knee(gpLen, params.pressureKnee);
                if (gpLen > 1e-4f) gp /= gpLen;
                float bestScore = -FLT_MAX;
                glm::vec2 best = goalDir;
                const auto consider = [&](const glm::vec2& d)
                {
                    const glm::vec2 probe = here + d * bodyProbe;
                    if (window.isBlocked(Nav::cellOf(probe)))
                        return; // blocked at the body: not a heading
                    const float free = window.freeDistance(here, d, look) / look;
                    float score = free * (wGoal * glm::dot(d, goalDir)
                        + wFlow * laneW * glm::dot(d, lane)
                        + (m_hasLastDir ? wPersist * glm::dot(d, m_lastDir) : 0.0f));
                    score -= wPressure * gpW * glm::dot(d, gp);
                    score += wallW * glm::dot(d, wallDir);
                    // Corner clip: two lateral body samples PENALIZE (not forbid) brushing a
                    // corner the centre-line run cannot see.
                    const glm::vec2 probeSide(-d.y * bodyRadius, d.x * bodyRadius);
                    const int clipped = int(window.isBlocked(Nav::cellOf(probe + probeSide)))
                        + int(window.isBlocked(Nav::cellOf(probe - probeSide)));
                    score -= float(clipped) * params.steerCornerClip;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = d;
                    }
                };
                // Fan of 16 anchored ON the goal (k = 0 IS goalDir — a free-floating fan often had
                // no candidate through a one-cell gap, which subtends about one 22.5 deg slot); the
                // other 15 carry a per-unit offset so a crowd does not walk in columns.
                constexpr int c_candidates = 16;
                constexpr float c_step = glm::two_pi<float>() / c_candidates;
                const float refAngle = std::atan2(goalDir.y, goalDir.x);
                const float spread = (float(m_rng >> 8 & 0xFFFF) / 65536.0f - 0.5f) * c_step;
                consider(goalDir);
                for (int k = 1; k < c_candidates; ++k)
                {
                    const float a = refAngle + float(k) * c_step + spread;
                    consider(glm::vec2(std::cos(a), std::sin(a)));
                }
                if (laneW > 0.0f) // the lane is generally not on the fan's grid
                    consider(lane);
                dir = best;
                if (unstick) // break symmetric two-unit locks
                {
                    const float jitter = (unitRand01(m_rng) - 0.5f) * glm::radians(60.0f);
                    const float c = std::cos(jitter), sn = std::sin(jitter);
                    dir = glm::vec2(dir.x * c - dir.y * sn, dir.x * sn + dir.y * c);
                }
                m_lastDir = dir;
                m_hasLastDir = true;
            }
            const glm::vec2 measured(vel.x, vel.z); // the body's REAL planar velocity (pre-command)
            // A wander is a stroll: a fraction of the run speed.
            const float walkSpeed = targetLocked && moveOrder && wanderOrder
                ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax) : moveSpeed;
            glm::vec3 dv(dir.x * walkSpeed - vel.x, 0.0f, dir.y * walkSpeed - vel.z);
            const float maxDv = accel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                vel + glm::vec3(dv.x, 0.0f, dv.z));
            vel += glm::vec3(dv.x, 0.0f, dv.z);
            if (fields)
            {
                // Contribute the MEASURED velocity (never the command — a pinned unit must not
                // write "into the wall"), one cell BEHIND (a trail belongs behind the walker, and
                // splatting the own cell fed the heading back to itself).
                const float measuredLen = glm::length(measured);
                if (measuredLen > 0.1f && params.flowSplatGain > 0.0f)
                    Globals::navSystem.flow(team).splat(here - measured / measuredLen * Nav::CellSize, measured * params.flowSplatGain);
                // Back-pressure: stalled time injects pressure that diffuses outward each frame.
                if (m_pressureTimer > 0.4f && params.stuckPressure > 0.0f)
                {
                    const float strength = glm::min(m_pressureTimer - 0.4f, 1.5f) * params.stuckPressure;
                    Globals::navSystem.pressure(team).inject(here, strength * deltaSec * 60.0f);
                }
            }
        }
        else
            brake(); // arrived: hold position
    }
    else
        brake(); // nothing to walk to

    // ---- HARD velocity cap, whatever launched the body (a field shove, a box3d push-out, a
    // wall clip): the queued command applies before the next step, so `vel` stays the frame's
    // truth for the push clamps below.
    if (const float speed = glm::length(vel); speed > params.maxSpeed)
    {
        vel *= params.maxSpeed / speed;
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, vel);
    }

    // ---- shield battery + push (the player rules, minus regen) ----
    if (fc && fc->emitter.isValid())
    {
        const float pressure = fc->emitter.getPressure();
        const float tension = 1.0f + params.tension * pressure;
        const float before = energy;
        energy = glm::max(0.0f, energy - pressure * tension * params.energyDrainRate * deltaSec);
        if (before > 0.0f && energy <= 0.0f)
            collapsed = true; // reaches clients promptly: the game blob's change detection forces
                              // this entity's next snapshot record out
        fc->emitter.setOutput(energy > 0.0f ? shieldOutput : 0.01f);

        const float iso = Globals::forceSystem.getParams().isoThreshold;
        // COLLAPSED gate (the player's rule): while the battery holds, pressure only DRAINS it —
        // health starts bleeding after the shield is gone, never before.
        if (collapsed && fc->emitter.getEquilibriumRadius() < params.damageRadius && pressure > iso)
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * deltaSec);

        // Push normalized by the output that PRODUCED the ~2-frame-latent readback, so a collapsed
        // shield is shoved exactly like a live one. Speed clamp rides the same queue (approximate
        // by one frame — the queue itself is one frame latent anyway).
        const glm::vec3 force = fc->emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
        // Push ramps in NEAR THE SURFACE only (pressure ~ iso): pushing everywhere in the support
        // stopped units out in the weak fringe, before the damage band could ever reach them.
        const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
        if (glm::dot(force, force) > 1e-8f && pushRamp > 0.0f)
        {
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                force * (deltaSec * params.pushGain * pressure * tension * pushRamp));
            const float speed = glm::length(vel);
            const float maxSpeed = moveSpeed * params.maxSpeedMult;
            if (speed > maxSpeed)
                Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                    vel * (maxSpeed / speed));
        }
        m_outputHistory[0] = m_outputHistory[1];
        m_outputHistory[1] = m_outputHistory[2];
        m_outputHistory[2] = energy > 0.0f ? shieldOutput : 0.01f;
    }
    else
    {
        // ---- SHIELD-LESS body: the BAKED pressure field stands in for the emitter readbacks
        // (ForceSystem::sampleBakedField — a CPU bilinear tap, no per-unit GPU slot) ----
        const ForceSystem::FieldSample fs = Globals::forceSystem.sampleBakedField(pos, team);
        if (fs.valid)
        {
            // GRADED exposure: the push equilibrium parks a pressing unit AT the shell surface
            // (opposing φ ~ iso), where a binary `inside` test read false most frames — units
            // ground against bubbles taking no damage. Damage now ramps with field DEPTH: zero
            // below iso x "Field damage starts (x iso)", full at the surface and beyond.
            // (fs.opposing is already the strongest NON-own field, so no owningTeam gate needed.)
            const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
            const float exposure = glm::smoothstep(iso * params.fieldDamageStart, iso, fs.opposing);
            if (exposure > 0.0f)
                health = glm::max(0.0f,
                    health - params.fieldDps * params.fieldDpsMult * exposure * deltaSec);
            // Push with the SAME chain the shielded units land on. Their force is
            // appliedForce / outputHistory = forceGain x (self-weighted mean of -grad over the
            // unit's bubble) — the 13-sample integral's mean self-weight is ~0.35 — times
            // pushGain * pressure * tension. Reproduce it from the field sample so ONE
            // "Field push gain" tweak rules both paths and a swarm body shoves like any unit.
            constexpr float c_bubbleSelfWeight = 0.35f;
            const glm::vec3 grad = fs.opposingGradient;
            const float pressure = fs.opposing;
            // Same near-surface push ramp as the shielded path: no shove in the weak fringe, so
            // bodies reach the damage band before the field starts holding them out.
            const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
            if (pressure > 0.0f && pushRamp > 0.0f && glm::dot(grad, grad) > 1e-8f)
            {
                const float tension = 1.0f + params.tension * pressure;
                const glm::vec3 force = -grad
                    * (c_bubbleSelfWeight * Globals::forceSystem.getParams().forceGain);
                Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                    force * (deltaSec * params.pushGain * pressure * tension * pushRamp));
                const float speed = glm::length(vel);
                const float maxSpeed = moveSpeed * params.maxSpeedMult;
                if (speed > maxSpeed)
                    Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                        vel * (maxSpeed / speed));
            }
        }
        else if (inEnemyBubble) // bake disabled: the stamped-radius fallback (damage only, no push)
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * deltaSec);
    }
}

void GameUnitComponent::damage(float amount)
{
    if (amount <= 0.0f)
        return;
    // EVERY victim banks into the inbox: puppets for owner routing, units so their OWN tick can
    // absorb shield-first (GamePlayer::applyDamage's rule) — the old direct health CAS bypassed
    // the shield entirely, and draining in the owner tick keeps `energy` single-writer.
    atomicAdd(pendingDamage, amount);
}

void GameUnitComponent::heal(float amount)
{
    if (amount > 0.0f)
        atomicAdd(pendingHeal, amount);
}

void GameUnitComponent::applyTeamTint(Entity& entity)
{
    GameUnitComponent* unit = getComponent<GameUnitComponent>(&entity);
    if (!unit || unit->puppet)
        return; // player capsules keep their own look
    const bool friendly = params.localTeam >= 0 && (int)unit->team == params.localTeam;
    const uint8 want = friendly ? 1 : 2;
    if (unit->tintState == want || (!friendly && unit->tintState == 0))
        return; // already right (an untouched non-friendly unit IS its authored colour)
    unit->tintState = want;
    // One node at a time: a friendly node is its OWN authored colour pulled toward green — a TINT,
    // so the unit types stay told apart — and a non-friendly one is restored to that colour.
    constexpr glm::vec3 c_friendlyGreen(0.3f, 1.0f, 0.4f); // the HUD's own-team bar colour
    constexpr float c_tintStrength = 0.55f;
    const auto tintNode = [&](Entity* node)
    {
        RenderComponent* rc = getComponent<RenderComponent>(node);
        if (!rc || !rc->node.isValid())
            return;
        glm::vec3 color(1.0f);
        if (const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(node); info && info->color.x >= 0.0f)
            color = info->color;
        if (friendly)
            color = glm::mix(color, c_friendlyGreen, c_tintStrength);
        rc->node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(color));
    };
    tintNode(&entity);
    if (const SceneComponent* sc = getComponent<SceneComponent>(&entity))
        for (const EntityPtr& child : sc->children)
            tintNode(child.get());
}

void GameUnitComponent::kill(Entity& entity)
{
    health = 0.0f;
    if (deathReported) // once — the queued destroy may take a tick to drain
        return;
    deathReported = true;
    Globals::scriptEvents.addDestroyRequest(EntityPtr(&entity));
    if (sourceId != 0)
    {
        const std::lock_guard<std::mutex> lock(g_unitEventMutex);
        g_deaths.push_back({ sourceId, popCost }); // its spawner frees the population
    }
}

bool GameUnitComponent::updateFar(Entity& entity, float deltaSec)
{
    if (!isAuthority() || puppet || !alive() || deltaSec <= 0.0f)
        return false;
    if (entity.pos.y < params.voidY) // through the floor while unselected: the full sim never
    {                                // visits it, so the far tick has to do the killing
        kill(entity);
        return false;
    }
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return false;
    // The ENTITY position is the far truth: the pass never visits this unit, so nothing else
    // writes it, and the body (disabled) is teleported to match below.
    const glm::vec3 pos = entity.pos;
    const glm::vec2 here(pos.x, pos.z);

    // Where to: the route first, then the locked move order. Same arrival rule as update().
    glm::vec3 target;
    if (routeIndex < routeCount)
    {
        if (glm::distance(here, glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
        if (routeIndex >= routeCount)
            return false;
        target = route[routeIndex];
    }
    else if (targetLocked && moveOrder)
    {
        if (wanderOrder) // a stroll times out here too — the full sim's clock never runs while far
        {
            wanderTimeLeft -= deltaSec;
            if (wanderTimeLeft <= 0.0f)
            {
                targetLocked = moveOrder = false;
                return false;
            }
        }
        if (glm::distance(here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
        {
            targetLocked = moveOrder = false; // arrived: the AI resumes when the unit is selected again
            return false;
        }
        target = targetPos;
    }
    else
        return false;

    // Direction: straight where the raster shows a clear line to the target, else the enemy team
    // field's descent (geodesic, routes around rocks) — the order points at enemy ground anyway.
    const glm::vec2 toTarget(target.x - here.x, target.z - here.y);
    const float dist = glm::length(toTarget);
    if (dist < 1e-3f)
        return false;
    glm::vec2 dir = toTarget / dist;
    const Nav::TeamField* raster = Globals::navSystem.isEnabled() ? Globals::navSystem.raster() : nullptr;
    if (raster && !raster->lineOfSight(here, glm::vec2(target.x, target.z), bodyRadius) && Globals::navSystem.anyFieldPublished())
    {
        Nav::TeamField::Sample best;
        for (uint32 t = 0; t < Nav::MaxTeams; ++t)
        {
            if (t == team)
                continue;
            if (const Nav::TeamField* field = Globals::navSystem.teamField(t))
            {
                const Nav::TeamField::Sample s = field->sample(here, m_rng);
                if (s.valid && (!best.valid || s.dist < best.dist))
                    best = s;
            }
        }
        if (best.valid && glm::dot(best.descentDir, best.descentDir) > 0.5f)
            dir = best.descentDir;
    }
    const float walkSpeed = targetLocked && moveOrder && wanderOrder // a stroll (the flag stays set
        ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax) // after the order, so
        : moveSpeed;                                                          // gate on the order)
    const glm::vec2 next = here + dir * glm::min(walkSpeed * deltaSec, dist);
    if (raster && raster->isBlocked(Nav::cellOf(next)))
        return false; // into rock: hold until a field covers it or the full sim takes over

    // Teleport contract: body pose + prev/curr stomp + step claim, entity position, spatial entry.
    const glm::vec3 newPos(next.x, pos.y, next.y);
    Globals::physics.teleportBody(pc->body, newPos, pc->body.getRotation());
    pc->prevPos = pc->currPos = newPos;
    pc->lastStep = Globals::physics.getStepCount();
    entity.pos = newPos; // a unit is a root: local == world
    if (entity.spatialEntry.isValid())
    {
        const RenderComponent* render = getComponent<RenderComponent>(&entity);
        const float radius = render && render->node.isValid() ? render->node.getWorldBounds().radius : 0.0f;
        Globals::spatialIndex.updateEntry(entity.spatialEntry.handle(), glm::dvec3(newPos), radius);
    }
    return true;
}

float GameUnitComponent::takePendingDamage()
{
    oc::atomic_ref<float> ref(pendingDamage);
    return ref.exchange(0.0f);
}

// ---------------------------------------------------------------- GameStructureComponent

// Machine event queues (rare, tiny — same discipline as the unit events above).
static std::mutex g_structureEventMutex;
static oc::vector<uint32> g_spawnRequests;
static oc::vector<GameStructureComponent::TurretFireRequest> g_turretFire;

void GameStructureComponent::takeSpawnRequests(oc::vector<uint32>& outStructureIds)
{
    const std::lock_guard<std::mutex> lock(g_structureEventMutex);
    outStructureIds.swap(g_spawnRequests);
    g_spawnRequests.clear();
}

void GameStructureComponent::takeTurretFireRequests(oc::vector<TurretFireRequest>& out)
{
    const std::lock_guard<std::mutex> lock(g_structureEventMutex);
    out.swap(g_turretFire);
    g_turretFire.clear();
}

void GameStructureComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    team = (uint8)info.team;
    health = healthMax = info.healthMax;
    invulnerable = info.invulnerable ? 1 : 0;
    alwaysDisplayHealth = info.alwaysDisplayHealth ? 1 : 0;
    meleeRadius = info.meleeRadius;
    if (isAuthority()) // clients never damage-sim, so they never spend a query slot
        query = Globals::forceSystem.createQuery(base.pos);
}

// Atomically move `amount` from one store float to another, clamped by the source's content and
// the destination's headroom; returns what actually moved. Reserve-from-source first, clamped add
// second, remainder returned — concurrent link owners sharing an endpoint compose without loss.
static float atomicTransfer(float& source, float& dest, float destCap, float amount)
{
    if (amount <= 0.0f)
        return 0.0f;
    oc::atomic_ref<float> src(source);
    float cur = src.load(oc::memory_order_relaxed), take;
    do { take = glm::min(amount, glm::max(cur, 0.0f)); } while (!src.compare_exchange_weak(cur, cur - take));
    if (take <= 0.0f)
        return 0.0f;
    oc::atomic_ref<float> dst(dest);
    float dcur = dst.load(oc::memory_order_relaxed), put;
    do { put = glm::min(take, glm::max(destCap - dcur, 0.0f)); } while (!dst.compare_exchange_weak(dcur, dcur + put));
    if (const float leftover = take - put; leftover > 0.0f)
    {
        float scur = src.load(oc::memory_order_relaxed);
        while (!src.compare_exchange_weak(scur, scur + leftover)) {}
    }
    return put;
}

void GameStructureComponent::update(Entity& entity, float deltaSec)
{
    if (!isAuthority())
        return;
    // ---- territory: HOSTILE = any OTHER team's bubble owns the structure's point (push a field
    // over their base to siege it). Health is the construction progress too, so a blueprint under
    // an enemy bubble literally un-builds.
    if (!invulnerable && query.isValid())
    {
        const ForceQuery::Result territory = query.getResult();
        if (territory.valid && territory.inside && territory.owningTeam != (int)team)
            fieldDrain(params.fieldDamageRate * deltaSec);
    }

    // ---- distribution: gravity-fed flows over the links. Each link is processed EXACTLY ONCE per
    // tick, by its OWNER side, in whichever direction the bands/fills call for (push when this
    // side is the source, pull when the far side is). Both endpoints running it — which is what
    // "process on the source side" amounted to, since each side decides independently — moved the
    // same link twice per frame in opposite directions, so any balancing link visibly sloshed back
    // and forth no matter how small the steps were.
    // Bands: higher exports to lower at full throughput; equal bands converge on equal fill
    // fractions, moving a DAMPED fraction of the balancing transfer (the exact transfer, applied
    // by several sources that cannot see each other, overshoots and rebounds).
    // The owner's OUTGOING links are gathered first and share the store fairly (proportional scale
    // when it cannot cover them all; a full receiver desires ~0, so its share goes to the rest).
    // Pulls are not budgeted that way — the far side's other links belong to their own owners —
    // but every transfer is atomically clamped, so nothing is over-drawn or fabricated.
    struct Outflow
    {
        GameStructureComponent* far;
        GameStructureLink* link;
        float desired;
        uint8 medium;
    };
    constexpr float c_equalizeDamping = 0.25f;
    const float invDt = 1.0f / glm::max(deltaSec, 1e-6f);
    // A receiver's free space, divided by however many links can feed it that medium. Without
    // this each source independently desires the WHOLE headroom, so a full destination that is
    // being drained a little every tick hands its scraps to whichever source's worker happens to
    // run first — the inflow hops between the feeding links tick by tick. A share is steady, and
    // when a feeder cannot use its share the others simply take it on the following ticks.
    // Link vectors only change on the main thread, so walking a neighbour's is safe here.
    const auto headroomShare = [](const GameStructureComponent& dest, int m)
    {
        const float headroom = glm::max(dest.capacity[m] - dest.store[m], 0.0f);
        int feeders = 0;
        for (const GameStructureLink& dl : dest.links)
        {
            if ((int)dl.medium != m || !dl.other)
                continue;
            const GameStructureComponent* src = getComponent<GameStructureComponent>(dl.other.get());
            if (src && src->band[m] >= dest.band[m]) // same band counts: it may be the fuller side
                ++feeders;
        }
        return headroom / (float)glm::max(feeders, 1);
    };
    thread_local oc::vector<Outflow> outs;
    outs.clear();
    float totalDesired[3] = {};
    for (GameStructureLink& l : links)
    {
        if (!l.owner)
            continue; // the far side owns it and does the work
        l.lastFlow = 0.0f;
        GameStructureComponent* far = l.other ? getComponent<GameStructureComponent>(l.other.get()) : nullptr;
        if (!far)
            continue;
        const int m = l.medium;
        const float capA = capacity[m], capB = far->capacity[m];
        if (blueprint || far->blueprint || capA <= 0.0f || capB <= 0.0f)
            continue; // pre-wired/idle links carry nothing
        const float fillA = store[m] / capA, fillB = far->store[m] / capB;
        const float maxT = l.throughput * deltaSec;
        if (band[m] != far->band[m])
        {
            if (band[m] > far->band[m]) // downhill out of this side
            {
                const float desired = glm::min(maxT, headroomShare(*far, m));
                outs.push_back({ far, &l, desired, (uint8)m });
                totalDesired[m] += desired;
            }
            else // downhill INTO this side: pull, clamped by our own headroom
                l.lastFlow = -atomicTransfer(far->store[m], store[m], capA,
                    glm::min(maxT, glm::max(capA - store[m], 0.0f))) * invDt;
        }
        else if (fillA > fillB) // balancing outward
        {
            const float gap = (fillA - fillB) * (capA * capB / (capA + capB)) * c_equalizeDamping;
            const float desired = glm::min(glm::min(gap, maxT), headroomShare(*far, m));
            outs.push_back({ far, &l, desired, (uint8)m });
            totalDesired[m] += desired;
        }
        else if (fillB > fillA) // balancing inward
        {
            const float gap = (fillB - fillA) * (capA * capB / (capA + capB)) * c_equalizeDamping;
            l.lastFlow = -atomicTransfer(far->store[m], store[m], capA,
                glm::min(glm::min(gap, maxT), glm::max(capA - store[m], 0.0f))) * invDt;
        }
    }
    float shareFactor[3];
    for (int m = 0; m < 3; ++m)
        shareFactor[m] = totalDesired[m] > glm::max(store[m], 0.0f) && totalDesired[m] > 1e-9f
            ? glm::max(store[m], 0.0f) / totalDesired[m] : 1.0f;
    for (const Outflow& o : outs)
        o.link->lastFlow = atomicTransfer(store[o.medium], o.far->store[o.medium],
            o.far->capacity[o.medium], o.desired * shareFactor[o.medium]) * invDt;

    // ---- machine logic (the union's stamped variant): the DECISION runs here per-entity, worker-
    // side, spending from the structure's OWN stores; the actual entity spawn rides an event queue
    // because spawning is main-thread only.
    if (machineKind == EMachineKind::Barracks && !blueprint)
    {
        entity.setProfiled(); // machine structures earn a per-entity profile scope (latched here —
                              // machineKind is stamped by the game AFTER spawn, so spawn can't know)
        BarracksData& b = barracks;
        // THE ENERGY STORE IS THE BUILD BAR: the game stamps capacity = the selected unit's
        // cost and caps the barracks' cable intake ("Barracks energy intake/s"), so the store
        // fills at the build rate and a unit is born the moment it is FULL — build time = cost /
        // intake, no timer. The epsilon covers a fill that lands a rounding step short of the cap.
        if (b.population + (int)b.spawnPop <= b.popCap && store[0] >= b.spawnCost - 0.01f)
        {
            store[0] = glm::max(store[0] - b.spawnCost, 0.0f); // the bar restarts (refunded on spawn fail)
            b.population += b.spawnPop;   // the unit's death event frees it again
            const std::lock_guard<std::mutex> lock(g_structureEventMutex);
            g_spawnRequests.push_back(structureId);
        }
    }
    else if (machineKind == EMachineKind::Medic && !blueprint && powered)
    {
        // MEDIC: one spatial query of the heal radius per station, own-team units inside get a
        // heal banked into their inbox (their own tick applies it). Never a roster walk.
        entity.setProfiled();
        const glm::vec3 pos = entity.pos;
        thread_local oc::vector<uint64> nearby;
        Globals::spatialIndex.querySphere(glm::dvec3(pos), params.medicRange, SpatialLayer_Render, nearby);
        const float amount = params.medicHealRate * deltaSec;
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameUnitComponent* u = getComponent<GameUnitComponent>(other);
            if (!u || u->puppet || u->team != team || !u->alive())
                continue;
            const glm::vec3 d = other->pos - pos;
            if (d.x * d.x + d.z * d.z > params.medicRange * params.medicRange)
                continue; // the sphere query is a broadphase on bounds
            u->heal(amount);
        }
    }
    else if (machineKind == EMachineKind::Turret && !blueprint)
    {
        entity.setProfiled();
        turret.fireTimer = glm::max(0.0f, turret.fireTimer - deltaSec);
        // No target = no cooldown reset: it fires the moment one appears.
        if (turret.fireTimer <= 0.0f && store[0] >= params.turretShotEnergy)
        {
            const glm::vec3 pos = entity.pos;
            thread_local oc::vector<uint64> nearby;
            Globals::spatialIndex.querySphere(glm::dvec3(pos), params.turretRange,
                SpatialLayer_Render, nearby);
            Entity* target = nullptr;
            float bestDistSq = params.turretRange * params.turretRange;
            for (const uint64 user : nearby)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                const GameUnitComponent* u = getComponent<GameUnitComponent>(other);
                if (!u || u->puppet || u->team == team || !u->alive())
                    continue; // puppets are player capsules — turrets target only units (known gap)
                const glm::vec3 d = other->pos - pos;
                if (glm::dot(d, d) < bestDistSq)
                {
                    bestDistSq = glm::dot(d, d);
                    target = other;
                }
            }
            if (target)
            {
                store[0] -= params.turretShotEnergy;
                turret.fireTimer = params.turretFireInterval;
                // HITSCAN lightning: the damage lands right here (damage() is atomic — the melee
                // sweep uses the same call from workers); only the BEAM visual is queued.
                if (GameUnitComponent* victim = getComponent<GameUnitComponent>(target))
                    victim->damage(params.turretDamage);
                const std::lock_guard<std::mutex> lock(g_structureEventMutex);
                g_turretFire.push_back(TurretFireRequest{ pos + glm::vec3(0.0f, 1.0f, 0.0f),
                    target->pos, team });
            }
        }
    }
    // Smooth each OWNED link's rate for the visuals/gauges (~0.25 s): the raw per-tick transfer is
    // bursty — a consumer drains its buffer and then takes a full packet, and the fair split
    // reshuffles shares as receivers fill — which made the cable brightness and flow pulses
    // strobe. Only the owner entry carries a flow, so only it smooths.
    const float smoothing = glm::min(deltaSec * 4.0f, 1.0f);
    for (GameStructureLink& l : links)
        if (l.owner)
            l.flowAvg += (l.lastFlow - l.flowAvg) * smoothing;

    // Utilization gauge over ALL touching links: the owner side holds the flow; a mirror entry
    // reads the owner's value through the far component (benign cross-worker float read — it is
    // a gauge).
    float peakUtil = 0.0f;
    for (const GameStructureLink& l : links)
    {
        float flow = l.flowAvg;
        if (!l.owner && l.other)
            if (GameStructureComponent* far = getComponent<GameStructureComponent>(l.other.get()))
                if (const GameStructureLink* mirror = far->findLink(&entity, l.medium))
                    flow = mirror->flowAvg;
        peakUtil = glm::max(peakUtil, glm::abs(flow) / glm::max(l.throughput, 1e-3f));
    }
    flowUtil += (glm::min(peakUtil, 1.0f) - flowUtil) * smoothing;
}

GameStructureLink* GameStructureComponent::findLink(const Entity* otherEntity, int medium)
{
    for (GameStructureLink& l : links)
        if (l.other.get() == otherEntity && (medium < 0 || (int)l.medium == medium))
            return &l;
    return nullptr;
}

void GameStructureComponent::unlinkAll(Entity& self)
{
    for (GameStructureLink& l : links)
        if (l.other)
            if (GameStructureComponent* far = getComponent<GameStructureComponent>(l.other.get()))
                oc::erase_if(far->links, [&](const GameStructureLink& fl) { return fl.other.get() == &self; });
    links.clear();
}

void GameStructureComponent::link(Entity& a, Entity& b, uint8 medium, float throughput)
{
    GameStructureComponent* ca = getComponent<GameStructureComponent>(&a);
    GameStructureComponent* cb = getComponent<GameStructureComponent>(&b);
    if (!ca || !cb || &a == &b)
        return;
    unlink(a, b, medium); // the pair's SAME-medium link re-links in place; others stay
    ca->links.push_back(GameStructureLink{ EntityPtr(&b), medium, throughput, /*owner*/ true });
    cb->links.push_back(GameStructureLink{ EntityPtr(&a), medium, throughput, /*owner*/ false });
}

void GameStructureComponent::unlink(Entity& a, Entity& b, int medium)
{
    if (GameStructureComponent* ca = getComponent<GameStructureComponent>(&a))
        oc::erase_if(ca->links, [&](const GameStructureLink& l) {
            return l.other.get() == &b && (medium < 0 || (int)l.medium == medium); });
    if (GameStructureComponent* cb = getComponent<GameStructureComponent>(&b))
        oc::erase_if(cb->links, [&](const GameStructureLink& l) {
            return l.other.get() == &a && (medium < 0 || (int)l.medium == medium); });
}

void GameStructureComponent::damage(float amount)
{
    if (invulnerable || amount <= 0.0f)
        return;
    atomicSubClamped(health, amount);
}

void GameStructureComponent::addLoad(float energyPerSec)
{
    // Only strainable structures are ever deposited on, and strainable is only set on emitters —
    // the `emitter` union variant is the active one by contract.
    if (energyPerSec > 0.0f)
        atomicAdd(emitter.unitLoad, energyPerSec);
}

// ---------------------------------------------------------------- GameProjectileComponent

void GameProjectileComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    team = info.team;
    unitDamage = info.unitDamage;
    structureDamage = info.structureDamage;
    lifetime = info.lifetime;
    emitterDrain = info.emitterDrain;
    emitterDrainRadius = info.emitterDrainRadius;
    splashRadius = info.splashRadius;
    if (ForceComponent* fc = getComponent<ForceComponent>(&entity))
        fc->emitter.setTeam(team); // the shot's field carries the shooter's team
}

void GameProjectileComponent::update(Entity& entity, float deltaSec)
{
    if (!isAuthority())
        return; // replicated shots fly on the owner's spawn velocities; the server despawns them
    age += deltaSec;
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    const glm::vec3 pos = pc && pc->body.isValid() ? pc->body.getPosition() : entity.pos;
    if (spent || age > lifetime || pos.y < params.voidY)
    {
        if (!spent)
            Globals::scriptEvents.addDestroyRequest(EntityPtr(&entity));
        spent = true;
        return;
    }
    // Enemy fields brake/deflect the shot (force-ball pattern), and pressing near a bubble SAPS
    // the nearest active enemy emitter — sustained barrages are a real siege drain.
    if (ForceComponent* fc = getComponent<ForceComponent>(&entity); fc && fc->emitter.isValid() && pc)
    {
        const glm::vec3 force = fc->emitter.getAppliedForce();
        const float pressure = fc->emitter.getPressure();
        if (glm::dot(force, force) > 1e-8f)
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                force * deltaSec * params.pushGain * pressure);
    }
    if (emitterDrain > 0.0f)
    {
        thread_local oc::vector<uint64> nearby;
        Globals::spatialIndex.querySphere(glm::dvec3(pos), emitterDrainRadius, SpatialLayer_Render, nearby);
        GameStructureComponent* strain = nullptr;
        float best = emitterDrainRadius;
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            if (!sc || !sc->strainable || sc->team == team)
                continue;
            const float d = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(other->pos.x, other->pos.z));
            if (d < best)
            {
                best = d;
                strain = sc;
            }
        }
        if (strain)
            strain->addLoad(emitterDrain);
    }
}

void GameProjectileComponent::onContact(Entity& self, Entity& other, bool begin)
{
    // Main thread, inside physics.dispatchContactEvents. First contact spends the shot:
    // enemy-team victims take the hit, everything else (ground, own team) just stops it.
    if (!begin || spent || !isAuthority())
        return;
    // damage() handles puppets itself (banks into pendingDamage for owner routing), so enemy
    // projectiles hurt players through the exact same call as units.
    if (splashRadius > 0.0f)
    {
        // SPLASH: every enemy-team unit/structure within the radius of the impact point takes the
        // full hit (the touched victim included — it is inside the radius by definition). The
        // contact dispatch runs after the frame's spatial commit, so the query is legal here.
        thread_local oc::vector<uint64> nearby;
        Globals::spatialIndex.querySphere(glm::dvec3(self.pos), splashRadius, SpatialLayer_Render, nearby);
        const float r2 = splashRadius * splashRadius;
        for (const uint64 user : nearby)
        {
            Entity* victim = reinterpret_cast<Entity*>(user);
            const glm::vec3 d = victim->pos - self.pos;
            if (glm::dot(d, d) > r2)
                continue;
            if (GameUnitComponent* unit = getComponent<GameUnitComponent>(victim); unit && unit->team != team)
                unit->damage(unitDamage);
            else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(victim); sc && sc->team != team)
                sc->damage(structureDamage);
        }
    }
    else if (GameUnitComponent* unit = getComponent<GameUnitComponent>(&other); unit && unit->team != team)
        unit->damage(unitDamage);
    else if (GameStructureComponent* sc = getComponent<GameStructureComponent>(&other); sc && sc->team != team)
        sc->damage(structureDamage);
    spent = true;
    Globals::scriptEvents.addDestroyRequest(EntityPtr(&self));
}

// ---------------------------------------------------------------- spawn-info plumbing

template <typename T>
static const typename T::SpawnInfo* spawnInfoOf(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<T>(entity))
        return nullptr;
    size_t idx = 0;
    for (uint16 i = 0; i < uint16(T::getId()); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const typename T::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity) { return spawnInfoOf<GameUnitComponent>(entity); }
const GameStructureComponent::SpawnInfo* getGameStructureSpawnInfo(const Entity* entity) { return spawnInfoOf<GameStructureComponent>(entity); }
const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity) { return spawnInfoOf<GameProjectileComponent>(entity); }

void writeGameUnitSpawnInfo(const GameUnitComponent::SpawnInfo& info, AssetNode& out)
{
    const GameUnitComponent::SpawnInfo d;
    if (info.team != d.team)                 out.set("Team", oc::to_string(info.team));
    if (info.puppet != d.puppet)             out.set("Puppet", info.puppet);
    if (!info.shortName.empty())             out.set("ShortName", info.shortName);
    if (info.healthMax != d.healthMax)       out.set("HealthMax", info.healthMax);
    if (info.energyMax != d.energyMax)       out.set("EnergyMax", info.energyMax);
    if (info.shieldOutput != d.shieldOutput) out.set("ShieldOutput", info.shieldOutput);
    if (info.moveSpeed != d.moveSpeed)       out.set("MoveSpeed", info.moveSpeed);
    if (info.accel != d.accel)               out.set("Accel", info.accel);
    if (info.attackRange != d.attackRange)   out.set("AttackRange", info.attackRange);
    if (info.attackDps != d.attackDps)       out.set("AttackDps", info.attackDps);
    if (info.playerDps != d.playerDps)       out.set("PlayerDps", info.playerDps);
    if (info.emitterDrain != d.emitterDrain) out.set("EmitterDrain", info.emitterDrain);
    if (info.ranged != d.ranged)             out.set("Ranged", info.ranged);
    if (info.standoffRange != d.standoffRange) out.set("StandoffRange", info.standoffRange);
    if (info.fireInterval != d.fireInterval) out.set("FireInterval", info.fireInterval);
    if (info.shotKind != d.shotKind)         out.set("ShotKind", oc::to_string((int)info.shotKind));
    if (info.alwaysDisplayHealth != d.alwaysDisplayHealth) out.set("AlwaysDisplayHealth", info.alwaysDisplayHealth);
    if (info.heightLimit != d.heightLimit)   out.set("HeightLimit", info.heightLimit);
}

void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out)
{
    const GameStructureComponent::SpawnInfo d;
    if (info.team != d.team)                 out.set("Team", oc::to_string(info.team));
    if (info.healthMax != d.healthMax)       out.set("HealthMax", info.healthMax);
    if (info.invulnerable != d.invulnerable) out.set("Invulnerable", info.invulnerable);
    if (info.meleeRadius != d.meleeRadius)   out.set("MeleeRadius", info.meleeRadius);
    if (info.alwaysDisplayHealth != d.alwaysDisplayHealth) out.set("AlwaysDisplayHealth", info.alwaysDisplayHealth);
}

void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out)
{
    const GameProjectileComponent::SpawnInfo d;
    if (info.team != d.team)                         out.set("Team", oc::to_string(info.team));
    if (info.unitDamage != d.unitDamage)             out.set("UnitDamage", info.unitDamage);
    if (info.structureDamage != d.structureDamage)   out.set("StructureDamage", info.structureDamage);
    if (info.lifetime != d.lifetime)                 out.set("Lifetime", info.lifetime);
    if (info.emitterDrain != d.emitterDrain)         out.set("EmitterDrain", info.emitterDrain);
    if (info.emitterDrainRadius != d.emitterDrainRadius) out.set("EmitterDrainRadius", info.emitterDrainRadius);
    if (info.splashRadius != d.splashRadius)         out.set("SplashRadius", info.splashRadius);
}
