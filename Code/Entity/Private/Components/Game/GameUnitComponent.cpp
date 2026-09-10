module Entity;

import Core;
import Core.glm;
import Core.Transform;
import Core.Time; // the seed-request clock
import :Entity;
import Force;
import Physics;
import Spatial;
import Nav;
import RendererVK; // applyTeamTint's material override

// See GameUnitComponent.ixx for the design + authority/thread contract. Everything here runs either
// on the parallel entity pass (update — authority instances only) or on the main thread (spawn,
// the orders); cross-entity writes are the atomic CAS helpers, cross-entity lookup is spatial.

GameUnitParams GameUnitComponent::params;

// Cross-entity writes from the parallel pass (two units biting one structure on different workers
// must not lose hits).
static void atomicAdd(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    while (!ref.compare_exchange_weak(cur, cur + amount)) {}
}

// Worker-side reports, drained by the game (see the queues' declarations). The mutex only ever
// guards four small append-only vectors touched on the rare tick where a unit fires, hits, asks
// for a lane or dies.
static std::mutex g_unitEventMutex;
static oc::vector<GameUnitComponent::FireRequest> g_fireRequests;
static oc::vector<GameUnitComponent::DeathRecord> g_deaths;
static oc::vector<GameUnitComponent::SeedRequest> g_seedRequests;
static oc::vector<GameUnitComponent::HitRecord> g_hits;
// Live non-puppet units (see liveCount): +1 in spawn, -1 in destroy — both worker-side in the
// batch spawn / release paths, so an atomic; relaxed, nothing orders on it.
static oc::atomic<int> g_liveUnits = 0;

int GameUnitComponent::liveCount()
{
    return g_liveUnits.load(oc::memory_order_relaxed);
}

void GameUnitComponent::destroy(Entity&, const SpawnInfo& info)
{
    if (!info.puppet)
        g_liveUnits.fetch_sub(1, oc::memory_order_relaxed);
}

void GameUnitComponent::takeHits(oc::vector<HitRecord>& out)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    out.swap(g_hits);
    g_hits.clear();
}

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

// Tiny LCG: units roll targets/jitters on WORKERS — the engine script RNG is fine but this keeps
// each unit's stream independent of scheduling order.
static float unitRand01(uint32& state)
{
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) * (1.0f / 16777216.0f);
}

void GameUnitComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    if (const ForceComponent::SpawnInfo* si = getForceSpawnInfo(&entity))
    {
        entity.setProfiled(); // units with shields carry a per-entity profile scope
    }
    puppet = info.puppet;
    if (!info.puppet)
        g_liveUnits.fetch_add(1, oc::memory_order_relaxed); // paired with destroy()
    if (!info.shortName.empty())
        m_shortName = info.shortName;
    team = info.team;
    health = healthMax = info.healthMax;
    shieldOutput = info.shieldOutput;
    // No shield = no battery: a shield-less body (swarm) carries zero energy, so nothing —
    // damage absorb, the label pass's shield-vs-health branch — ever mistakes it for shielded.
    energy = energyMax = shieldOutput > 0.0f ? info.energyMax : 0.0f;
    moveSpeed = info.moveSpeed;
    accel = info.accel;
    attackRange = info.attackRange;
    attackInterval = info.attackInterval;
    attackDamage = info.attackDamage;
    playerDamage = info.playerDamage;
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
    m_seedDue = (float)Globals::time.getSimElapsedSec() + params.seedRequestInterval * unitRand01(m_rng);
    m_stuckCheckTimer = 0.75f * unitRand01(m_rng);
    m_attackTimer = attackInterval * unitRand01(m_rng); // a batch must not swing in lockstep
    if (const PhysicsComponent::SpawnInfo* si = getPhysicsSpawnInfo(&entity))
    {
        float r = 0.5f, top = 1.0f;
        switch (si->shape.type)
        {
        case EPhysicsShapeType::Box:     r = glm::max(si->shape.halfExtents.x, si->shape.halfExtents.z); top = si->shape.halfExtents.y; break;
        case EPhysicsShapeType::Sphere:  r = top = si->shape.radius; break;
        case EPhysicsShapeType::Capsule: r = si->shape.radius; top = si->shape.radius + si->shape.halfHeight; break;
        default: break;
        }
        bodyRadius = r * entity.scale;
        m_bodyTop = top * entity.scale;
    }
    m_lastHealth = health;
}

// ---------------------------------------------------------------- the light area budgets
// World XZ is bucketed into params.lightArea squares, each hashed to a slot row of a fixed table
// (collisions merge two far-apart areas' budgets — harmless for a visual). Everything is relaxed
// atomics: workers race for slots, a lost race only means no light this frame.
namespace
{
    constexpr uint32 c_areaSlots = 4096;                 // power of two
    oc::atomic<float> g_flashSlots[c_areaSlots]; // sim seconds of the area's last flash

    uint32 areaSlotOf(const glm::vec3& pos)
    {
        const float area = glm::max(GameUnitComponent::params.lightArea, 1.0f);
        const uint32 x = uint32(int32(glm::floor(pos.x / area)));
        const uint32 z = uint32(int32(glm::floor(pos.z / area)));
        uint32 n = x * 1597334673u ^ z * 3812015801u;
        n = n * 747796405u + 2891336453u;
        n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
        return (n ^ (n >> 22u)) & (c_areaSlots - 1u);
    }

    // One flash per 1/hurtFlashRate seconds per area: claim the slot's time by CAS.
    bool claimFlash(const glm::vec3& pos, float now)
    {
        const float rate = GameUnitComponent::params.hurtFlashRate;
        if (rate <= 0.0f)
            return true;
        oc::atomic<float>& slot = g_flashSlots[areaSlotOf(pos)];
        float last = slot.load(oc::memory_order_relaxed);
        while (now - last >= 1.0f / rate)
            if (slot.compare_exchange_weak(last, now, oc::memory_order_relaxed))
                return true;
        return false;
    }

}

// The HURT LIGHT (every role): a health drop since the last tick lights the body — when the area's
// flash budget lets it — and the glow decays after. Per-frame light record, lock-free — like
// LightComponent's pushes.
void GameUnitComponent::tickHurtLight(const Entity& entity, float deltaSec)
{
    m_hurtGlow = glm::max(m_hurtGlow - deltaSec / glm::max(params.hurtLightDecay, 1e-3f), 0.0f);
    if (health < m_lastHealth - 1e-4f && claimFlash(entity.pos, (float)Globals::time.getSimElapsedSec()))
        m_hurtGlow = 1.0f;
    m_lastHealth = health;
    if (m_hurtGlow <= 0.0f || params.hurtLightIntensity <= 0.0f || !Globals::rendererVK.isInitialized())
        return;
    // A wide range for the intensity: the inverse-square falloff must be near nothing where the
    // range window cuts it, or the light reads as a hard-edged disc on the ground.
    const glm::vec3 pos = entity.pos + glm::vec3(0.0f, m_bodyTop + 0.3f, 0.0f);
    const float range = glm::max(bodyRadius * 8.0f, 4.0f);
    // The ATTACKER's team colour (the force shell colours), a touch toward white; neutral red
    // when nothing tagged the damage.
    glm::vec3 color(1.0f, 0.35f, 0.2f);
    if (m_hurtTeam != 0xFF)
        color = glm::mix(Globals::forceSystem.getParams().teamColors[glm::min<uint32>(m_hurtTeam, 7u)], glm::vec3(1.0f), 0.3f);
    Globals::rendererVK.addPointLight(PointLight(pos, range, color,
        params.hurtLightIntensity * (0.5f + 0.5f * bodyRadius) * m_hurtGlow));
}

// ---------------------------------------------------------------- the authority tick

void GameUnitComponent::update(Entity& entity, float deltaSec)
{
    // (The shield GLOW is the Force system's bubble light — a collapsed shield has no bubble.)
    tickHurtLight(entity, deltaSec);
    if (Globals::networkManager.role() == ENetRole::Client)
        return; // clients mirror via the entity sync's game blob
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return;
    Tick t{ entity, *pc, getComponent<ForceComponent>(&entity), deltaSec };
    t.pos = pc->body.getPosition();
    t.here = glm::vec2(t.pos.x, t.pos.z);
    t.fields = params.navEnabled && Globals::navSystem.isEnabled();
    t.vel = pc->body.getLinearVelocity();

    // Crowd presence: a weak pressure source (players too) — the diffused field is a smoothed
    // crowd density that keeps units spaced. The stall injection in tickSteering is far stronger.
    if (t.fields && params.presencePressure > 0.0f)
        Globals::navSystem.pressure(team).inject(t.here, params.presencePressure * deltaSec * 60.0f);
    // The height limit runs BEFORE the puppet gate on purpose: the server's twins of client
    // capsules are held under the same ceiling their owners clamp themselves to in GamePlayer, so
    // both land at the same height and the owner's next claim re-anchors instead of fighting.
    applyHeightLimit(t);
    if (puppet)
        return; // state carrier: GamePlayer writes it
    if (!applyInboxes(t))
        return; // died this tick
    resolveWalkTarget(t);
    tickCombat(t);
    tickSteering(t);
    tickField(t);
}

// Launched above the ceiling -> put back AT the ceiling with the climb cancelled (the queued
// velocity keeps the planar part). Teleport contract: stomp the interpolation poses and claim the
// step, or PhysicsComponent::update mixes toward the pre-teleport pose on stepping frames.
void GameUnitComponent::applyHeightLimit(Tick& t)
{
    const float ceiling = effectiveHeightLimit();
    if (t.pos.y <= ceiling)
        return;
    const glm::vec3 clamped(t.pos.x, ceiling, t.pos.z);
    Globals::physics.teleportBody(t.pc.body, clamped, t.pc.body.getRotation());
    t.vel.y = glm::min(t.vel.y, 0.0f);
    Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity, t.vel);
    t.pc.prevPos = t.pc.currPos = clamped;
    t.pc.lastStep = Globals::physics.getStepCount();
}

bool GameUnitComponent::applyInboxes(Tick& t)
{
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
    if (health <= 0.0f || t.pos.y < params.voidY)
    {
        kill(t.entity);
        return false;
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
    return true;
}

// Where to walk: the route, then the locked order, then the Nav fields, then the local search.
void GameUnitComponent::resolveWalkTarget(Tick& t)
{
    if (targetLocked && moveOrder
        && glm::distance(t.here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
        targetLocked = moveOrder = false; // move order arrived: back to the AI
    if (targetLocked && moveOrder && wanderOrder)
    {
        wanderTimeLeft -= t.deltaSec; // a wander that cannot get there just gives up
        if (wanderTimeLeft <= 0.0f)
            targetLocked = moveOrder = false;
    }
    t.walkTarget = t.pos;
    if (routeIndex < routeCount)
    {
        if (glm::distance(t.here, glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
        if (routeIndex < routeCount)
        {
            t.walkTarget = route[routeIndex];
            t.routing = t.haveWalkTarget = t.walkIsOrder = true;
        }
    }
    // NAV: the geodesically nearest enemy across every other team's field; its descent direction
    // already routes around walls. Falls through to the local search where no field covers us.
    if (!t.routing && !targetLocked && t.fields && Globals::navSystem.anyFieldPublished())
        resolveNavTarget(t);
    if (!t.routing && !t.navResolved)
        searchLocalTarget(t);
}

void GameUnitComponent::resolveNavTarget(Tick& t)
{
    Nav::TeamField::Sample best;
    const Nav::TeamField* bestField = nullptr;
    for (uint32 team_ = 0; team_ < Nav::MaxTeams; ++team_)
    {
        if (team_ == team)
            continue;
        const Nav::TeamField* field = Globals::navSystem.teamField(team_);
        if (!field)
            continue;
        const Nav::TeamField::Sample s = field->sample(t.here, m_rng);
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
        t.walkTarget = targetPos;
        t.haveWalkTarget = true;
        t.navDir = best.descentDir;
        t.navResolved = true;
        t.navTracking = best.dist <= params.targetTrackRadius;
        t.navSteer = glm::dot(t.navDir, t.navDir) > 0.5f;
        return;
    }
    // OUT OF THE SEARCH RADIUS: a target picked on an earlier tick is STALE — drop it
    // (targetLocked never gets here). Kept, the unit marched to the last known spot for up
    // to a whole retarget interval and looked as if it ignored the follow radius entirely.
    hasTarget = false;
    if (!best.valid || best.dist > params.navFollowRadius)
        return;
    // NAV FOLLOW band: too far to TARGET (the player sprinted out of the search radius), but
    // still near the action — if the crowd FLOW field holds a lane here, walk it (no target, no
    // combat lock). The chasers' own trail + the seeded lane keep pulling the pack along until
    // the target is back in range or the lane decays.
    const Nav::TeamField* raster = Globals::navSystem.raster();
    if (!raster)
        return;
    const glm::vec2 lane = Globals::navSystem.flow(team).sample(t.here, raster);
    const float laneLen = glm::length(lane);
    if (laneLen > params.flowKnee * moveSpeed)
    {
        t.navDir = lane / laneLen;
        t.navSteer = true;
        t.navResolved = true; // following, not hunting: the local re-search stays off
        t.walkTarget = t.pos + glm::vec3(t.navDir.x, 0.0f, t.navDir.y) * 8.0f;
        t.haveWalkTarget = true;
    }
}

void GameUnitComponent::searchLocalTarget(Tick& t)
{
    m_retargetTimer -= t.deltaSec;
    if (!targetLocked && (m_retargetTimer <= 0.0f || !hasTarget))
    {
        // Random pick among the 4 nearest enemy structures; nearest enemy player (a puppet in
        // the same query) as the fallback.
        m_retargetTimer = params.retargetInterval * (0.7f + 0.6f * unitRand01(m_rng));
        // (Every spatial probe in this pass consumes its hits INLINE — no result buffer, so
        // nothing thread_local rides a job that may park and resume on another thread.)
        struct Candidate { float distSq; glm::vec3 pos; };
        Candidate best[4];
        int count = 0;
        float bestPlayerDistSq = FLT_MAX;
        glm::vec3 bestPlayerPos(0.0f);
        Globals::spatialIndex.forEachInSphere(glm::dvec3(t.pos), params.targetSearchRadius,
            SpatialLayer_Render, [&](uint64 user)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            if (const GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
                pu && pu->puppet && pu->team != team && pu->alive())
            {
                const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - t.here;
                if (glm::dot(d, d) < bestPlayerDistSq)
                {
                    bestPlayerDistSq = glm::dot(d, d);
                    bestPlayerPos = other->pos;
                }
                return;
            }
            const GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            if (!sc || sc->invulnerable || sc->team == team || !sc->alive())
                return;
            const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - t.here;
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
        });
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
    t.walkTarget = targetPos;
    t.haveWalkTarget = hasTarget;
    t.walkIsOrder = hasTarget && targetLocked && moveOrder && !wanderOrder; // a wander never seeds
}

// Combat: ONE short probe serves the structure bite, the emitter strain and the melee sweep over
// enemy players/units (decoupled from the walk target: a wall in the way gets chewed too). The
// strain reach must fit inside the query radius. A unit MARCHING A ROUTE runs it too: the nearest
// enemy unit inside routeEngageRadius becomes its walk target for this tick (melee closes in,
// ranged fires at it), and the march resumes once it is gone.
void GameUnitComponent::tickCombat(Tick& t)
{
    t.stopRange = attackRange;
    const float c_strainRange = params.strainRange; // emitter siege-drain reach ("Emitter drain range")
    const float engageRadius = t.routing ? params.routeEngageRadius : 0.0f;
    // A MOVE ORDER (the wave's march on the Base, a player's RMB) breaks off at the first
    // enemy structure inside orderBreakRadius: the order drops and the AI takes over, which
    // hunts the NEAREST structure — otherwise the whole wave walked past everything to the
    // Base and only bit what stood in its way.
    const bool ordered = targetLocked && moveOrder;
    const float breakRadius = ordered ? params.orderBreakRadius : 0.0f;
    glm::vec3 bitePos(0.0f);
    float engageDistSq = engageRadius * engageRadius;
    glm::vec3 engagePos(0.0f);
    bool engage = false;
    // Melee hits ONE victim: the nearest enemy unit OR player capsule inside reach.
    GameUnitComponent* meleeVictim = nullptr;
    float meleeVictimDistSq = FLT_MAX;
    float meleeVictimReach = 0.0f;
    glm::vec3 meleeVictimPos(0.0f);
    GameStructureComponent* bite = nullptr;
    GameStructureComponent* strain = nullptr;
    float biteDist = FLT_MAX, strainDist = c_strainRange;
    Globals::spatialIndex.forEachInSphere(glm::dvec3(t.pos),
        glm::max(glm::max(glm::max(attackRange + 6.0f, c_strainRange), engageRadius), breakRadius),
        SpatialLayer_Render, [&](uint64 user)
    {
        Entity* other = reinterpret_cast<Entity*>(user);
        if (GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            sc && sc->team != team)
        {
            const float d = glm::distance(t.here, glm::vec2(other->pos.x, other->pos.z));
            if (sc->strainable && d < strainDist) // shield-state-independent siege drain
            {
                strainDist = d;
                strain = sc;
            }
            t.inEnemyBubble |= sc->strainable && d < sc->bubbleRadius;
            if (!sc->invulnerable && sc->alive() && d - sc->meleeRadius < biteDist)
            {
                biteDist = d - sc->meleeRadius;
                bite = sc;
                bitePos = other->pos;
            }
            return;
        }
        GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
        if (!pu || pu == this || pu->team == team || !pu->alive()
            || glm::abs(other->pos.y - t.pos.y) >= 3.0f)
            return;
        const glm::vec2 to(other->pos.x - t.pos.x, other->pos.z - t.pos.z);
        if (t.routing && glm::dot(to, to) < engageDistSq) // nearest enemy on the march
        {
            engageDistSq = glm::dot(to, to);
            engagePos = other->pos;
            engage = true;
        }
        if (!ranged) // melee at the victim's body ring (a capsule gets a flat allowance)
        {
            const float reach = attackRange + (pu->puppet ? 0.8f : pu->bodyRadius);
            const float distSq = glm::dot(to, to);
            if (distSq < reach * reach && distSq < meleeVictimDistSq)
            {
                meleeVictim = pu;
                meleeVictimDistSq = distSq;
                meleeVictimReach = reach;
                meleeVictimPos = other->pos;
            }
        }
    });
    // DISCRETE melee: the timer runs down whether or not anything is in reach (clamped at 0, so
    // arriving at a victim swings at once), and one swing lands on ONE victim — the enemy unit or
    // player first (attackDamage / playerDamage), else the structure — and reports the hit for
    // the game's visual.
    m_attackTimer = glm::max(m_attackTimer - t.deltaSec, 0.0f);
    const bool canBite = bite && biteDist <= attackRange && t.pos.y > -2.0f && t.pos.y < 8.0f;
    if (meleeVictim)
        t.stopRange = glm::max(t.stopRange, meleeVictimReach); // hold at the ring
    const float swing = meleeVictim ? (meleeVictim->puppet ? playerDamage : attackDamage) : attackDamage;
    if (!ranged && m_attackTimer <= 0.0f && swing > 0.0f && (meleeVictim || canBite))
    {
        if (meleeVictim)
            meleeVictim->damage(swing, team); // atomic: a unit's health, or the puppet's inbox
        else
            bite->damage(swing);        // atomic — workers bite concurrently
        m_attackTimer = attackInterval;
        const std::lock_guard<std::mutex> lock(g_unitEventMutex);
        g_hits.push_back(HitRecord{ t.pos, meleeVictim ? meleeVictimPos : bitePos });
    }
    if (!engage && t.routing && bite && biteDist < engageRadius)
    {
        engage = true; // a structure on the march is engaged like a unit (units first)
        engagePos = bitePos;
    }
    if (engage)
    {
        t.walkTarget = engagePos; // the route waypoint waits (routeIndex is untouched)
        t.haveWalkTarget = true;
        t.walkIsOrder = false;
    }
    if (ordered && bite && biteDist < breakRadius)
    {
        targetLocked = moveOrder = false; // the order is done: the AI hunts from here
        hasTarget = false;
        t.walkTarget = bitePos; // this tick already heads for it
        t.haveWalkTarget = true;
        t.walkIsOrder = false;
    }
    if (ranged)
    {
        t.stopRange = standoffRange;
        m_fireTimer -= t.deltaSec;
        if (t.haveWalkTarget && m_fireTimer <= 0.0f
            && glm::distance(t.here, glm::vec2(t.walkTarget.x, t.walkTarget.z)) <= standoffRange)
        {
            // Spawning is main-thread only: queue the shot for the game to service.
            const std::lock_guard<std::mutex> lock(g_unitEventMutex);
            g_fireRequests.push_back(FireRequest{ t.pos, t.walkTarget, (uint8)team, shotKind });
            m_fireTimer = fireInterval * (0.8f + 0.4f * unitRand01(m_rng));
        }
    }
    else if (canBite)
        t.stopRange = attackRange + bite->meleeRadius; // the swing itself landed above
    if (strain)
        strain->addLoad(emitterDrain * params.emitterDrainMult);
}

// Steering: walk toward the target through the context steering over the nav fields, or BRAKE.
// Physics writes are QUEUED (workers): one frame of latency.
// BRAKE (no target, or arrived): the capsules run FRICTION 0 — the SIM LOD ticks them at up to
// 1 s intervals and ground friction between ticks bled the commanded speed away — so stopping is
// an explicit command too: planar velocity to zero at the steering accel. Without it a coasting
// unit never stops, keeps splatting its velocity into the crowd lane, and the pack follows the
// ghost trail. No lane splat while braking (only the walk branch).
void GameUnitComponent::tickSteering(Tick& t)
{
    const auto brake = [&]
    {
        const glm::vec3 planar(t.vel.x, 0.0f, t.vel.z);
        const float speed = glm::length(planar);
        if (speed < 1e-3f)
            return;
        const float maxDv = accel * t.deltaSec;
        const glm::vec3 dv = speed > maxDv ? -planar * (maxDv / speed) : -planar;
        Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity, t.vel + dv);
        t.vel += dv;
    };
    const glm::vec2 toTarget(t.walkTarget.x - t.pos.x, t.walkTarget.z - t.pos.z);
    const float dist = glm::length(toTarget);
    if (!t.haveWalkTarget || dist <= t.stopRange)
        brake(); // nothing to walk to, or arrived: hold position
    else
    {
        const glm::vec2 goalDir = t.navSteer ? t.navDir : toTarget / glm::max(dist, 1e-3f);
        const Nav::TeamField* raster = t.fields ? Globals::navSystem.raster() : nullptr;
        // PLAN REQUEST on a jittered per-unit timer, while walking (not only while stuck): Nav
        // dedups by proximity, so a crowd going the same way costs one plan and the lane keeps
        // up with the group. Only ROUTES and MOVE ORDERS seed — a HUNTED target (nav field,
        // local search, engage) seeds only for params.huntSeedTeam (the co-op AI): friendly
        // units chasing an enemy must not carve lanes toward it.
        // The due time is on the SIM CLOCK, independent of the tier's tick rate (see m_seedDue).
        const float simNow = (float)Globals::time.getSimElapsedSec();
        // A WANDER never seeds, hunt-seed team or not (the co-op AI is that team, and its
        // strolls were carving lanes to random points).
        const bool wandering = targetLocked && moveOrder && wanderOrder;
        if (simNow >= m_seedDue && !wandering && (t.walkIsOrder || (int)team == params.huntSeedTeam))
        {
            m_seedDue = simNow + params.seedRequestInterval * (0.75f + 0.5f * unitRand01(m_rng));
            const std::lock_guard<std::mutex> lock(g_unitEventMutex);
            g_seedRequests.push_back(SeedRequest{ t.pos, t.walkTarget, (uint8)team,
                m_pressureTimer > params.unstickAfter });
        }
        // STUCK DETECTION by displacement checkpoints (per-tick progress never accumulates on
        // a jittering heading): < 0.6 m per 0.75 s accrues stalled time into m_pressureTimer.
        m_stuckCheckTimer -= t.deltaSec;
        if (m_stuckCheckTimer <= 0.0f)
        {
            const float moved = m_hasStuckAnchor ? glm::distance(t.here, m_stuckAnchor) : 10.0f;
            m_pressureTimer = moved < 0.6f ? m_pressureTimer + 0.75f : 0.0f;
            m_stuckAnchor = t.here;
            m_hasStuckAnchor = true;
            m_stuckCheckTimer = 0.75f;
        }
        const glm::vec2 dir = raster ? steerHeading(t, goalDir, *raster) : goalDir;
        const glm::vec2 measured(t.vel.x, t.vel.z); // the body's REAL planar velocity (pre-command)
        // A wander is a stroll: a fraction of the run speed.
        const float walkSpeed = wandering
            ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax) : moveSpeed;
        glm::vec3 dv(dir.x * walkSpeed - t.vel.x, 0.0f, dir.y * walkSpeed - t.vel.z);
        const float maxDv = accel * t.deltaSec;
        const float dvLen = glm::length(dv);
        if (dvLen > maxDv && dvLen > 1e-6f)
            dv *= maxDv / dvLen;
        Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
            t.vel + glm::vec3(dv.x, 0.0f, dv.z));
        t.vel += glm::vec3(dv.x, 0.0f, dv.z);
        if (t.fields)
        {
            // Contribute the MEASURED velocity (never the command — a pinned unit must not
            // write "into the wall"), one cell BEHIND (a trail belongs behind the walker, and
            // splatting the own cell fed the heading back to itself).
            const float measuredLen = glm::length(measured);
            if (measuredLen > 0.1f && params.flowSplatGain > 0.0f)
                Globals::navSystem.flow(team).splat(t.here - measured / measuredLen * Nav::CellSize, measured * params.flowSplatGain);
            // Back-pressure: stalled time injects pressure that diffuses outward each frame.
            if (m_pressureTimer > 0.4f && params.stuckPressure > 0.0f)
            {
                const float strength = glm::min(m_pressureTimer - 0.4f, 1.5f) * params.stuckPressure;
                Globals::navSystem.pressure(team).inject(t.here, strength * t.deltaSec * 60.0f);
            }
        }
    }

    // HARD velocity cap, whatever launched the body (a field shove, a box3d push-out, a wall
    // clip): the queued command applies before the next step, so `vel` stays the frame's truth
    // for the push clamps in tickField.
    if (const float speed = glm::length(t.vel); speed > params.maxSpeed)
    {
        t.vel *= params.maxSpeed / speed;
        Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity, t.vel);
    }
}

// CONTEXT STEERING over the nav fields: score a fan of headings by goal alignment, open run, crowd
// lane, persistence, minus the pressure gradient; stalls shift the weights toward the fields.
glm::vec2 GameUnitComponent::steerHeading(Tick& t, glm::vec2 goalDir, const Nav::TeamField& raster)
{
    const bool stalled = m_pressureTimer > 0.4f;
    const bool unstick = m_pressureTimer > params.unstickAfter;
    float wGoal = unstick ? 0.0f : params.steerGoal * (stalled ? 0.3f : 1.0f);
    const float look = glm::max(params.steerLook, moveSpeed * 1.0f);
    // ONE raster snapshot for every probe this tick (the chunk-hash helpers pay a find() per
    // cell — 100+ per unit per tick without it).
    Nav::TeamField::CostWindow window;
    raster.snapshotCosts(t.here, int(std::ceil((look + bodyRadius + params.wallKeep)
        / Nav::CellSize)) + 1, window);
    // Wall keep-away: in a gap both walls cancel (the unit centres itself), at a corner the
    // single push swings it wide. The run itself is a centre-line march so a 1-tile gap reads
    // as open.
    const glm::vec2 wallAway = window.wallPush(t.here, bodyRadius + params.wallKeep);
    const float wallLen = glm::length(wallAway);
    const glm::vec2 wallDir = wallLen > 1e-4f ? wallAway / wallLen : glm::vec2(0.0f);
    const float wallW = glm::min(wallLen, 1.0f) * params.steerWall;
    if (unstick)
    {
        // Really stuck: replace the goal with an ESCAPE, backing away from whatever pins us — a
        // nearby wall first (the wall push already points away from it), else the nearest
        // unit/structure/player from a small spatial query, else simply the opposite of where
        // we were trying to go.
        if (wallLen > 1e-3f)
            goalDir = wallDir;
        else
        {
            float bestSq = FLT_MAX;
            glm::vec2 away(0.0f);
            Globals::spatialIndex.forEachInSphere(glm::dvec3(t.pos), 4.0,
                SpatialLayer_Render, [&](uint64 user)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                if (other == &t.entity || (!getComponent<GameUnitComponent>(other)
                    && !getComponent<GameStructureComponent>(other)))
                    return; // only the things that can pin us, never scenery
                const glm::vec2 d = t.here - glm::vec2(other->pos.x, other->pos.z);
                const float dSq = glm::dot(d, d);
                if (dSq > 1e-6f && dSq < bestSq)
                {
                    bestSq = dSq;
                    away = d;
                }
            });
            goalDir = bestSq < FLT_MAX ? away / std::sqrt(bestSq) : -goalDir;
        }
        wGoal = 1.5f;
    }
    m_ignoreFlowTimer = glm::max(0.0f, m_ignoreFlowTimer - t.deltaSec);
    // LIVE TARGET WITHIN THE TRACK RADIUS: the goal is the team field's descent at the target's
    // LIVE position (~0.25 s fresh) — floor the goal weight up and near-mute the seeded lane,
    // whose periodic re-plans lag a moving player badly. Farther (but inside the search radius)
    // the unit marches lane-friendly toward the target.
    const bool tracking = t.navTracking && !unstick;
    if (tracking)
        wGoal = glm::max(wGoal, params.steerTrackGoal * (stalled ? 0.3f : 1.0f));
    const float wFlow = m_ignoreFlowTimer > 0.0f ? 0.0f
        : params.steerFlow * (unstick ? 0.5f : stalled ? 2.0f : 1.0f)
        * (tracking ? params.trackFlowMult : 1.0f);
    const float wPersist = unstick ? 0.0f : params.steerPersist * (stalled ? 0.2f : 1.0f);
    const float wPressure = params.steerPressure * (unstick ? 3.0f : 1.0f);
    const float bodyProbe = bodyRadius + 0.1f;
    // Compressive response x/(x+knee): one stuck unit registers, a hundred never saturate. The
    // knee is the value scoring 0.5.
    const auto knee = [](float x, float k) { return x / (x + glm::max(k, 1e-4f)); };
    glm::vec2 lane = Globals::navSystem.flow(team).sample(t.here, &raster);
    const float laneLen = glm::length(lane);
    const float laneW = knee(laneLen, params.flowKnee * glm::max(moveSpeed, 0.1f));
    if (laneLen > 1e-4f) lane /= laneLen;
    glm::vec2 gp = Globals::navSystem.pressure(team).gradient(t.here, &raster);
    const float gpLen = glm::length(gp);
    const float gpW = knee(gpLen, params.pressureKnee);
    if (gpLen > 1e-4f) gp /= gpLen;
    float bestScore = -FLT_MAX;
    glm::vec2 best = goalDir;
    const auto consider = [&](const glm::vec2& d)
    {
        const glm::vec2 probe = t.here + d * bodyProbe;
        if (window.isBlocked(Nav::cellOf(probe)))
            return; // blocked at the body: not a heading
        const float free = window.freeDistance(t.here, d, look) / look;
        float score = free * (wGoal * glm::dot(d, goalDir)
            + wFlow * laneW * glm::dot(d, lane)
            + (m_hasLastDir ? wPersist * glm::dot(d, m_lastDir) : 0.0f));
        score -= wPressure * gpW * glm::dot(d, gp);
        score += wallW * glm::dot(d, wallDir);
        // Corner clip: two lateral body samples PENALIZE (not forbid) brushing a corner the
        // centre-line run cannot see.
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
    // Fan of 16 anchored ON the goal (k = 0 IS goalDir — a free-floating fan often had no
    // candidate through a one-cell gap, which subtends about one 22.5 deg slot); the other 15
    // carry a per-unit offset so a crowd does not walk in columns.
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
    glm::vec2 dir = best;
    if (unstick) // break symmetric two-unit locks
    {
        const float jitter = (unitRand01(m_rng) - 0.5f) * glm::radians(60.0f);
        const float c = std::cos(jitter), sn = std::sin(jitter);
        dir = glm::vec2(dir.x * c - dir.y * sn, dir.x * sn + dir.y * c);
    }
    m_lastDir = dir;
    m_hasLastDir = true;
    return dir;
}

// The shield battery + push (the player rules, minus regen) off the emitter readbacks, or — for a
// SHIELD-LESS body — the BAKED pressure field standing in for them (ForceSystem::sampleBakedField,
// a CPU bilinear tap, no per-unit GPU slot).
void GameUnitComponent::tickField(Tick& t)
{
    if (t.fc && t.fc->emitter.isValid())
    {
        const float pressure = t.fc->emitter.getPressure();
        const float tension = 1.0f + params.tension * pressure;
        const float before = energy;
        energy = glm::max(0.0f, energy - pressure * tension * params.energyDrainRate * t.deltaSec);
        if (before > 0.0f && energy <= 0.0f)
            collapsed = true; // reaches clients promptly: the game blob's change detection forces
                              // this entity's next snapshot record out
        t.fc->emitter.setOutput(energy > 0.0f ? shieldOutput : 0.01f);

        const float iso = Globals::forceSystem.getParams().isoThreshold;
        // COLLAPSED gate (the player's rule): while the battery holds, pressure only DRAINS it —
        // health starts bleeding after the shield is gone, never before.
        if (collapsed && t.fc->emitter.getEquilibriumRadius() < params.damageRadius && pressure > iso)
        {
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * t.deltaSec);
            noteHurtTeam(opposingTeamGuess()); // the pressure readback carries no team
        }

        // Push normalized by the output that PRODUCED the ~2-frame-latent readback, so a collapsed
        // shield is shoved exactly like a live one. Speed clamp rides the same queue (approximate
        // by one frame — the queue itself is one frame latent anyway).
        const glm::vec3 force = t.fc->emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
        // Push ramps in NEAR THE SURFACE only (pressure ~ iso): pushing everywhere in the support
        // stopped units out in the weak fringe, before the damage band could ever reach them.
        const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
        if (glm::dot(force, force) > 1e-8f && pushRamp > 0.0f)
        {
            Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                force * (t.deltaSec * params.pushGain * pressure * tension * pushRamp));
            const float speed = glm::length(t.vel);
            const float maxSpeed = moveSpeed * params.maxSpeedMult;
            if (speed > maxSpeed)
                Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                    t.vel * (maxSpeed / speed));
        }
        m_outputHistory[0] = m_outputHistory[1];
        m_outputHistory[1] = m_outputHistory[2];
        m_outputHistory[2] = energy > 0.0f ? shieldOutput : 0.01f;
        return;
    }
    const ForceSystem::FieldSample fs = Globals::forceSystem.sampleBakedField(t.pos, team);
    if (!fs.valid)
    {
        if (t.inEnemyBubble) // bake disabled: the stamped-radius fallback (damage only, no push)
        {
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * t.deltaSec);
            noteHurtTeam(opposingTeamGuess());
        }
        return;
    }
    // GRADED exposure: the push equilibrium parks a pressing unit AT the shell surface (opposing
    // φ ~ iso), where a binary `inside` test read false most frames — units ground against
    // bubbles taking no damage. Damage ramps with field DEPTH instead: zero outside the field,
    // full at the surface and beyond.
    // (fs.opposing is already the strongest NON-own field, so no owningTeam gate needed.)
    const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
    const float exposure = glm::smoothstep(0.0f, iso, fs.opposing);
    if (exposure > 0.0f)
    {
        health = glm::max(0.0f,
            health - params.fieldDps * params.fieldDpsMult * exposure * t.deltaSec);
        // The sample's owning team is the strongest field's — the burner, unless our own field
        // is stronger here, in which case the opposing team is not named: guess.
        noteHurtTeam(fs.owningTeam != team ? fs.owningTeam : opposingTeamGuess());
    }
    // Push with the SAME chain the shielded units land on. Their force is
    // appliedForce / outputHistory = forceGain x (self-weighted mean of -grad over the unit's
    // bubble) — the 13-sample integral's mean self-weight is ~0.35 — times
    // pushGain * pressure * tension. Reproduce it from the field sample so ONE "Field push gain"
    // tweak rules both paths and a swarm body shoves like any unit.
    constexpr float c_bubbleSelfWeight = 0.35f;
    const glm::vec3 grad = fs.opposingGradient;
    const float pressure = fs.opposing;
    // Same near-surface push ramp as the shielded path: no shove in the weak fringe, so bodies
    // reach the damage band before the field starts holding them out.
    const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
    if (pressure > 0.0f && pushRamp > 0.0f && glm::dot(grad, grad) > 1e-8f)
    {
        const float tension = 1.0f + params.tension * pressure;
        const glm::vec3 force = -grad
            * (c_bubbleSelfWeight * Globals::forceSystem.getParams().forceGain);
        Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::ApplyImpulse,
            force * (t.deltaSec * params.pushGain * pressure * tension * pushRamp));
        const float speed = glm::length(t.vel);
        const float maxSpeed = moveSpeed * params.maxSpeedMult;
        if (speed > maxSpeed)
            Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                t.vel * (maxSpeed / speed));
    }
}

// ---------------------------------------------------------------- inboxes, tint, death

uint32 GameUnitComponent::opposingTeamGuess() const
{
    const uint32 n = glm::max(Globals::forceSystem.numTeams(), 2u);
    return (team + 1u) % n;
}

void GameUnitComponent::damage(float amount, uint32 sourceTeam)
{
    if (amount <= 0.0f)
        return;
    noteHurtTeam(sourceTeam);
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

float GameUnitComponent::takePendingDamage()
{
    oc::atomic_ref<float> ref(pendingDamage);
    return ref.exchange(0.0f);
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

// ---------------------------------------------------------------- the far tick

bool GameUnitComponent::farHeading(const Entity& entity, glm::vec2& dir, float& speed, float& dist, bool spread)
{
    // Where to: the route first, then the locked move order. Nothing else moves a far unit.
    glm::vec3 target;
    if (routeIndex < routeCount)
        target = route[routeIndex];
    else if (targetLocked && moveOrder)
        target = targetPos;
    else
        return false;
    const glm::vec2 here(entity.pos.x, entity.pos.z);
    // Direction: straight where the raster shows a clear line to the target, else the enemy team
    // field's descent (geodesic, routes around rocks) — the order points at enemy ground anyway.
    const glm::vec2 toTarget(target.x - here.x, target.z - here.y);
    dist = glm::length(toTarget);
    if (dist < 1e-3f)
        return false;
    dir = toTarget / dist;
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
    speed = targetLocked && moveOrder && wanderOrder // a stroll (the flag stays set after the
        ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax) // order, so gate on
        : moveSpeed;                                                          // the order)
    if (spread)
    {
        // Persistent per-unit bias (address hash): unbiased, every unit in an area walks the same
        // descent cells and the wave files into one line.
        const float hashFrac = float(uint32((uintptr_t(this) >> 4) * 2654435761u) >> 8) * (1.0f / 16777216.0f);
        const float angle = glm::radians(params.farSpreadDeg) * (hashFrac - 0.5f);
        const float c = std::cos(angle), s = std::sin(angle);
        dir = glm::vec2(dir.x * c - dir.y * s, dir.x * s + dir.y * c);
    }
    return true;
}

glm::vec3 GameUnitComponent::wakeVelocity(const Entity& entity)
{
    glm::vec2 dir;
    float speed, dist;
    if (Globals::networkManager.role() == ENetRole::Client || puppet || !alive() || !farHeading(entity, dir, speed, dist))
        return glm::vec3(0.0f); // nowhere to go (ambient, arrived, idle): wakes at rest
    return glm::vec3(dir.x * speed, 0.0f, dir.y * speed);
}

bool GameUnitComponent::updateFar(Entity& entity, float deltaSec)
{
    if (Globals::networkManager.role() == ENetRole::Client || puppet || !alive() || deltaSec <= 0.0f)
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

    // Arrival / timeout bookkeeping first (the same rules as update()), then the shared heading.
    if (routeIndex < routeCount)
    {
        if (glm::distance(here, glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
    }
    else if (targetLocked && moveOrder)
    {
        if (wanderOrder) // a stroll times out here too — the full sim's clock never runs while far
        {
            wanderTimeLeft -= deltaSec;
            if (wanderTimeLeft <= 0.0f)
                targetLocked = moveOrder = false;
        }
        if (targetLocked && glm::distance(here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
            targetLocked = moveOrder = false; // arrived: the AI resumes when the unit is selected again
    }
    glm::vec2 dir;
    float walkSpeed, dist;
    if (!farHeading(entity, dir, walkSpeed, dist))
        return false;
    const Nav::TeamField* raster = Globals::navSystem.isEnabled() ? Globals::navSystem.raster() : nullptr;
    glm::vec2 next = here + dir * glm::min(walkSpeed * deltaSec, dist);
    if (raster && raster->isBlocked(Nav::cellOf(next)))
    {
        // The spread bias aimed at rock: the plain heading, and hold only when that is blocked too.
        if (!farHeading(entity, dir, walkSpeed, dist, /*spread*/ false))
            return false;
        next = here + dir * glm::min(walkSpeed * deltaSec, dist);
        if (raster->isBlocked(Nav::cellOf(next)))
            return false;
    }

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

// ---------------------------------------------------------------- spawn-info plumbing

const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<GameUnitComponent>(entity))
        return nullptr;
    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_GameUnit); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const GameUnitComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

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
    if (info.attackInterval != d.attackInterval) out.set("AttackInterval", info.attackInterval);
    if (info.attackDamage != d.attackDamage) out.set("AttackDamage", info.attackDamage);
    if (info.playerDamage != d.playerDamage) out.set("PlayerDamage", info.playerDamage);
    if (info.emitterDrain != d.emitterDrain) out.set("EmitterDrain", info.emitterDrain);
    if (info.ranged != d.ranged)             out.set("Ranged", info.ranged);
    if (info.standoffRange != d.standoffRange) out.set("StandoffRange", info.standoffRange);
    if (info.fireInterval != d.fireInterval) out.set("FireInterval", info.fireInterval);
    if (info.shotKind != d.shotKind)         out.set("ShotKind", oc::to_string((int)info.shotKind));
    if (info.alwaysDisplayHealth != d.alwaysDisplayHealth) out.set("AlwaysDisplayHealth", info.alwaysDisplayHealth);
    if (info.heightLimit != d.heightLimit)   out.set("HeightLimit", info.heightLimit);
}
