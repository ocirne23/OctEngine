module Entity;

import Core;
import Core.glm;
import Core.Transform;
import Core.Time;
import :Entity;
import Force;
import Physics;
import Spatial;
import Nav;
import RendererVK;

GameUnitParams GameUnitComponent::params;

static void atomicAdd(float& value, float amount)
{
    oc::atomic_ref<float> ref(value);
    float cur = ref.load(oc::memory_order_relaxed);
    while (!ref.compare_exchange_weak(cur, cur + amount)) {}
}

static std::mutex g_unitEventMutex;
static oc::vector<GameUnitComponent::FireRequest> g_fireRequests;
static oc::vector<GameUnitComponent::DeathRecord> g_deaths;
static oc::vector<GameUnitComponent::SeedRequest> g_seedRequests;
static oc::vector<GameUnitComponent::HitRecord> g_hits;
static oc::atomic<int> g_liveUnits = 0; // spawn/destroy both run on workers in the batch paths

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

static float unitRand01(uint32& state) // per-unit stream, independent of scheduling order
{
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) * (1.0f / 16777216.0f);
}

void GameUnitComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    puppet = info.puppet;
    if (!info.puppet)
        g_liveUnits.fetch_add(1, oc::memory_order_relaxed);
    if (!info.shortName.empty())
        m_shortName = info.shortName;
    team = info.team;
    health = healthMax = info.healthMax;
    shieldOutput = info.shieldOutput;
    energy = energyMax = shieldOutput > 0.0f ? info.energyMax : 0.0f; // no shield = no battery
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
    m_rng = uint32(uintptr_t(this) >> 4) * 2654435761u + 1u;
    m_retargetTimer = 0.0f;
    // Random phase on the periodic timers: a batch spawned in one frame must not act in lockstep.
    m_seedDue = (float)Globals::time.getSimElapsedSec() + params.seedRequestInterval * unitRand01(m_rng);
    m_stuckCheckTimer = 0.75f * unitRand01(m_rng);
    m_attackTimer = attackInterval * unitRand01(m_rng);
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

// Hurt-flash area budget: world XZ bucketed into lightArea squares hashed to a slot table. Slot
// collisions merge far-apart areas' budgets, and a lost CAS race only means no light this frame.
namespace
{
    constexpr uint32 c_areaSlots = 4096;
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

void GameUnitComponent::tickHurtLight(const Entity& entity, float deltaSec)
{
    m_hurtGlow = glm::max(m_hurtGlow - deltaSec / glm::max(params.hurtLightDecay, 1e-3f), 0.0f);
    if (health < m_lastHealth - 1e-4f && claimFlash(entity.pos, (float)Globals::time.getSimElapsedSec()))
        m_hurtGlow = 1.0f;
    m_lastHealth = health;
    if (m_hurtGlow <= 0.0f || params.hurtLightIntensity <= 0.0f || !Globals::rendererVK.isInitialized())
        return;
    // Wide range: the inverse-square falloff must be near nothing where the range window cuts it,
    // or the light reads as a hard-edged disc on the ground.
    const glm::vec3 pos = entity.pos + glm::vec3(0.0f, m_bodyTop + 0.3f, 0.0f);
    const float range = glm::max(bodyRadius * 8.0f, 4.0f);
    glm::vec3 color(1.0f, 0.35f, 0.2f); // neutral red; else the attacker's team colour
    if (m_hurtTeam != 0xFF)
        color = glm::mix(Globals::forceSystem.getParams().teamColors[glm::min<uint32>(m_hurtTeam, 7u)], glm::vec3(1.0f), 0.3f);
    Globals::rendererVK.addPointLight(PointLight(pos, range, color,
        params.hurtLightIntensity * (0.5f + 0.5f * bodyRadius) * m_hurtGlow));
}

void GameUnitComponent::update(Entity& entity, float deltaSec)
{
    tickHurtLight(entity, deltaSec);
    if (Globals::networkManager.role() == ENetRole::Client)
        return;
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return;
    Tick t{ entity, *pc, getComponent<ForceComponent>(&entity), deltaSec };
    t.pos = pc->body.getPosition();
    t.here = glm::vec2(t.pos.x, t.pos.z);
    t.fields = params.navEnabled && Globals::navSystem.isEnabled();
    t.vel = pc->body.getLinearVelocity();

    if (t.fields && params.presencePressure > 0.0f)
        Globals::navSystem.pressure(team).inject(t.here, params.presencePressure * deltaSec * 60.0f);
    // Before the puppet gate on purpose: the server's twins of client capsules are held under the
    // same ceiling their owners clamp to in GamePlayer, so the owner's next claim does not fight it.
    applyHeightLimit(t);
    if (puppet)
        return;
    if (!applyDamageAndHeal(t))
        return;
    resolveWalkTarget(t);
    tickCombat(t);
    tickSteering(t);
    tickField(t);
    applyPush(t);
}

void GameUnitComponent::applyHeightLimit(Tick& t)
{
    const float ceiling = effectiveHeightLimit();
    if (t.pos.y <= ceiling)
        return;
    const glm::vec3 clamped(t.pos.x, ceiling, t.pos.z);
    Globals::physics.teleportBody(t.pc.body, clamped, t.pc.body.getRotation());
    t.vel.y = glm::min(t.vel.y, 0.0f);
    Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity, t.vel);
    // Teleport contract: stomp the interpolation poses and claim the step, or PhysicsComponent::update
    // mixes toward the pre-teleport pose on stepping frames.
    t.pc.prevPos = t.pc.currPos = clamped;
    t.pc.lastStep = Globals::physics.getStepCount();
}

bool GameUnitComponent::applyDamageAndHeal(Tick& t)
{
    // The battery eats direct damage first (the player's applyDamage rule); shield-less bodies take it all.
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
    // Death before the heal: a heal landing the same tick must not revive the unit (a medic
    // station's radius was making units immortal).
    if (health <= 0.0f || t.pos.y < params.voidY)
    {
        kill(t.entity);
        return false;
    }
    if (const float heal = oc::atomic_ref<float>(pendingHeal).exchange(0.0f, oc::memory_order_acq_rel); heal > 0.0f)
    {
        health = glm::min(health + heal, healthMax);
        energy = glm::min(energy + heal, energyMax);
        if (energy > 0.0f)
            collapsed = false;
    }
    return true;
}

void GameUnitComponent::resolveWalkTarget(Tick& t)
{
    if (targetLocked && moveOrder
        && glm::distance(t.here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
        targetLocked = moveOrder = false;
    if (targetLocked && moveOrder && wanderOrder)
    {
        wanderTimeLeft -= t.deltaSec;
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
    // The range gate keeps distant units holding their patch instead of marching across the map.
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
    hasTarget = false; // an earlier tick's target is stale out here: kept, the unit marched to it
    if (!best.valid || best.dist > params.navFollowRadius)
        return;
    // Follow band: no target, but walk the crowd lane if one holds here (the pack keeps chasing).
    const Nav::TeamField* raster = Globals::navSystem.raster();
    if (!raster)
        return;
    const glm::vec2 lane = Globals::navSystem.flow(team).sample(t.here, raster);
    const float laneLen = glm::length(lane);
    if (laneLen > params.flowKnee * moveSpeed)
    {
        t.navDir = lane / laneLen;
        t.navSteer = true;
        t.navResolved = true;
        t.walkTarget = t.pos + glm::vec3(t.navDir.x, 0.0f, t.navDir.y) * 8.0f;
        t.haveWalkTarget = true;
    }
}

void GameUnitComponent::searchLocalTarget(Tick& t)
{
    m_retargetTimer -= t.deltaSec;
    if (!targetLocked && (m_retargetTimer <= 0.0f || !hasTarget))
    {
        m_retargetTimer = params.retargetInterval * (0.7f + 0.6f * unitRand01(m_rng));
        // Every probe consumes its hits inline: no result buffer, nothing thread_local across a job
        // that may park and resume on another thread.
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
    t.walkIsOrder = hasTarget && targetLocked && moveOrder && !wanderOrder;
}

void GameUnitComponent::tickCombat(Tick& t)
{
    t.stopRange = attackRange;
    const float c_strainRange = params.strainRange;
    const float engageRadius = t.routing ? params.routeEngageRadius : 0.0f;
    const bool ordered = targetLocked && moveOrder;
    const float breakRadius = ordered ? params.orderBreakRadius : 0.0f;
    glm::vec3 bitePos(0.0f);
    float engageDistSq = engageRadius * engageRadius;
    glm::vec3 engagePos(0.0f);
    bool engage = false;
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
            if (sc->strainable && d < strainDist)
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
        if (t.routing && glm::dot(to, to) < engageDistSq)
        {
            engageDistSq = glm::dot(to, to);
            engagePos = other->pos;
            engage = true;
        }
        if (!ranged)
        {
            const float reach = attackRange + (pu->puppet ? 0.8f : pu->bodyRadius); // capsules: flat allowance
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
    m_attackTimer = glm::max(m_attackTimer - t.deltaSec, 0.0f);
    const bool canBite = bite && biteDist <= attackRange && t.pos.y > -2.0f && t.pos.y < 8.0f;
    if (meleeVictim)
        t.stopRange = glm::max(t.stopRange, meleeVictimReach);
    const float swing = meleeVictim ? (meleeVictim->puppet ? playerDamage : attackDamage) : attackDamage;
    if (!ranged && m_attackTimer <= 0.0f && swing > 0.0f && (meleeVictim || canBite))
    {
        if (meleeVictim)
            meleeVictim->damage(swing, team);
        else
            bite->damage(swing);
        m_attackTimer = attackInterval;
        const std::lock_guard<std::mutex> lock(g_unitEventMutex);
        g_hits.push_back(HitRecord{ t.pos, meleeVictim ? meleeVictimPos : bitePos });
    }
    if (!engage && t.routing && bite && biteDist < engageRadius)
    {
        engage = true;
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
        t.walkTarget = bitePos;
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
            const std::lock_guard<std::mutex> lock(g_unitEventMutex);
            g_fireRequests.push_back(FireRequest{ t.pos, t.walkTarget, (uint8)team, shotKind });
            m_fireTimer = fireInterval * (0.8f + 0.4f * unitRand01(m_rng));
        }
    }
    else if (canBite)
        t.stopRange = attackRange + bite->meleeRadius;
    if (strain)
        strain->addLoad(emitterDrain * params.emitterDrainMult);
}

void GameUnitComponent::tickSteering(Tick& t)
{
    // The capsules run friction 0 (the SIM LOD ticks them at up to 1 s intervals), so stopping is
    // an explicit command; a coasting unit would keep splatting its velocity into the crowd lane.
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
        brake();
    else
    {
        const glm::vec2 goalDir = t.navSteer ? t.navDir : toTarget / glm::max(dist, 1e-3f);
        const Nav::TeamField* raster = t.fields ? Globals::navSystem.raster() : nullptr;
        const float simNow = (float)Globals::time.getSimElapsedSec();
        // Only routes and move orders seed; a hunted target seeds only for huntSeedTeam, and a
        // wander never (its strolls were carving lanes to random points).
        const bool wandering = targetLocked && moveOrder && wanderOrder;
        if (simNow >= m_seedDue && !wandering && (t.walkIsOrder || (int)team == params.huntSeedTeam))
        {
            m_seedDue = simNow + params.seedRequestInterval * (0.75f + 0.5f * unitRand01(m_rng));
            const std::lock_guard<std::mutex> lock(g_unitEventMutex);
            g_seedRequests.push_back(SeedRequest{ t.pos, t.walkTarget, (uint8)team,
                m_pressureTimer > params.unstickAfter });
        }
        // Stuck detection by displacement checkpoints: per-tick progress never accumulates on a
        // jittering heading.
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
            // Splat the MEASURED velocity (a pinned unit must not write "into the wall"), one cell
            // BEHIND (splatting the own cell fed the heading back to itself).
            const float measuredLen = glm::length(measured);
            if (measuredLen > 0.1f && params.flowSplatGain > 0.0f)
                Globals::navSystem.flow(team).splat(t.here - measured / measuredLen * Nav::CellSize, measured * params.flowSplatGain);
            if (m_pressureTimer > 0.4f && params.stuckPressure > 0.0f)
            {
                const float strength = glm::min(m_pressureTimer - 0.4f, 1.5f) * params.stuckPressure;
                Globals::navSystem.pressure(team).inject(t.here, strength * t.deltaSec * 60.0f);
            }
        }
    }

    // Hard cap on whatever launched the body (a box3d push-out, a wall clip); `vel` stays the
    // frame's truth that applyPush scales the field impulse against.
    if (const float speed = glm::length(t.vel); speed > params.maxSpeed)
    {
        t.vel *= params.maxSpeed / speed;
        Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::SetLinearVelocity, t.vel);
    }
}

glm::vec2 GameUnitComponent::steerHeading(Tick& t, glm::vec2 goalDir, const Nav::TeamField& raster)
{
    const bool stalled = m_pressureTimer > 0.4f;
    const bool unstick = m_pressureTimer > params.unstickAfter;
    float wGoal = unstick ? 0.0f : params.steerGoal * (stalled ? 0.3f : 1.0f);
    const float look = glm::max(params.steerLook, moveSpeed * 1.0f);
    // One raster snapshot per tick: the chunk-hash helpers pay a find() per cell, 100+ per unit.
    Nav::TeamField::CostWindow window;
    raster.snapshotCosts(t.here, int(std::ceil((look + bodyRadius + params.wallKeep)
        / Nav::CellSize)) + 1, window);
    // In a gap both walls cancel (the unit centres itself); at a corner the single push swings it wide.
    const glm::vec2 wallAway = window.wallPush(t.here, bodyRadius + params.wallKeep);
    const float wallLen = glm::length(wallAway);
    const glm::vec2 wallDir = wallLen > 1e-4f ? wallAway / wallLen : glm::vec2(0.0f);
    const float wallW = glm::min(wallLen, 1.0f) * params.steerWall;
    if (unstick)
    {
        // Escape: away from a nearby wall, else the nearest unit/structure/player, else reverse.
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
                    return;
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
    // Tracking a live target: the field descent is ~0.25 s fresh, the seeded lane's re-plans lag
    // a moving player badly - floor the goal weight and near-mute the lane.
    const bool tracking = t.navTracking && !unstick;
    if (tracking)
        wGoal = glm::max(wGoal, params.steerTrackGoal * (stalled ? 0.3f : 1.0f));
    const float wFlow = m_ignoreFlowTimer > 0.0f ? 0.0f
        : params.steerFlow * (unstick ? 0.5f : stalled ? 2.0f : 1.0f)
        * (tracking ? params.trackFlowMult : 1.0f);
    const float wPersist = unstick ? 0.0f : params.steerPersist * (stalled ? 0.2f : 1.0f);
    const float wPressure = params.steerPressure * (unstick ? 3.0f : 1.0f);
    const float bodyProbe = bodyRadius + 0.1f;
    const auto knee = [](float x, float k) { return x / (x + glm::max(k, 1e-4f)); }; // never saturates
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
            return;
        const float free = window.freeDistance(t.here, d, look) / look;
        float score = free * (wGoal * glm::dot(d, goalDir)
            + wFlow * laneW * glm::dot(d, lane)
            + (m_hasLastDir ? wPersist * glm::dot(d, m_lastDir) : 0.0f));
        score -= wPressure * gpW * glm::dot(d, gp);
        score += wallW * glm::dot(d, wallDir);
        // Two lateral body samples PENALIZE (not forbid) brushing a corner the centre-line run cannot see.
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
    // Fan anchored ON the goal (a free-floating fan often had no candidate through a one-cell
    // gap); the per-unit offset keeps a crowd from walking in columns.
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
    if (laneW > 0.0f)
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

void GameUnitComponent::tickField(Tick& t)
{
    if (t.fc && t.fc->emitter.isValid())
    {
        const float pressure = t.fc->emitter.getPressure();
        const float tension = 1.0f + params.tension * pressure;
        const float before = energy;
        energy = glm::max(0.0f, energy - pressure * tension * params.energyDrainRate * t.deltaSec);
        if (before > 0.0f && energy <= 0.0f)
            collapsed = true;
        t.fc->emitter.setOutput(energy > 0.0f ? shieldOutput : 0.01f);

        const float iso = Globals::forceSystem.getParams().isoThreshold;
        if (collapsed && t.fc->emitter.getEquilibriumRadius() < params.damageRadius && pressure > iso)
        {
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * t.deltaSec);
            noteHurtTeam(opposingTeamGuess()); // the pressure readback carries no team
        }

        // Normalized by the output that PRODUCED the ~2-frame-latent readback, so a collapsed
        // shield is shoved exactly like a live one.
        const glm::vec3 force = t.fc->emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
        const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
        if (glm::dot(force, force) > 1e-8f && pushRamp > 0.0f)
            t.impulse += force * (t.deltaSec * params.pushGain * pressure * tension * pushRamp);
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
    // Graded by field depth: the push equilibrium parks a pressing unit AT the shell, where a
    // binary `inside` test read false most frames (units ground against bubbles taking no damage).
    const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
    const float exposure = glm::smoothstep(0.0f, iso, fs.opposing);
    if (exposure > 0.0f)
    {
        health = glm::max(0.0f,
            health - params.fieldDps * params.fieldDpsMult * exposure * t.deltaSec);
        noteHurtTeam(fs.owningTeam != team ? fs.owningTeam : opposingTeamGuess());
    }
    // Reproduces the shielded chain (appliedForce / output = forceGain x self-weighted mean of
    // -grad over the bubble, mean self-weight ~0.35) so ONE "Field push gain" rules both paths.
    constexpr float c_bubbleSelfWeight = 0.35f;
    const glm::vec3 grad = fs.opposingGradient;
    const float pressure = fs.opposing;
    const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
    if (pressure > 0.0f && pushRamp > 0.0f && glm::dot(grad, grad) > 1e-8f)
    {
        const float tension = 1.0f + params.tension * pressure;
        const glm::vec3 force = -grad
            * (c_bubbleSelfWeight * Globals::forceSystem.getParams().forceGain);
        t.impulse += force * (t.deltaSec * params.pushGain * pressure * tension * pushRamp);
    }
}

void GameUnitComponent::applyPush(Tick& t)
{
    if (glm::dot(t.impulse, t.impulse) <= 1e-12f)
        return;
    const float mass = t.pc.body.getMass();
    if (mass <= 0.0f)
        return;
    // The impulse lands on top of the steering's queued velocity (queue order), so the post-step
    // speed is |vel + s a|; s = the positive root of |vel + s a|^2 = cap^2, clamped to [0, 1].
    const float cap = params.maxSpeed;
    const glm::vec3 a = t.impulse / mass;
    const float aa = glm::dot(a, a);
    const float va = glm::dot(t.vel, a);
    const float slack = cap * cap - glm::dot(t.vel, t.vel);
    float s = 1.0f;
    if (slack <= 0.0f)
        s = va < 0.0f ? 1.0f : 0.0f; // already over the cap: only a push that slows the body lands
    else if (aa + 2.0f * va > slack)
        s = (-va + std::sqrt(glm::max(va * va + aa * slack, 0.0f))) / aa;
    if (s <= 0.0f)
        return;
    Globals::physics.queueBodyCommand(t.pc.body, PhysicsWorld::EBodyCommand::ApplyImpulse, t.impulse * s);
}

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
    atomicAdd(pendingDamage, amount); // every victim banks into the inbox: the owner tick absorbs shield-first
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
        return;
    const bool friendly = params.localTeam >= 0 && (int)unit->team == params.localTeam;
    const uint8 want = friendly ? 1 : 2;
    if (unit->tintState == want || (!friendly && unit->tintState == 0))
        return; // an untouched non-friendly unit IS its authored colour
    unit->tintState = want;
    // A tint toward green (not a flat colour) so the unit types stay told apart.
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
    if (deathReported) // the queued destroy may take a tick to drain
        return;
    deathReported = true;
    Globals::scriptEvents.addDestroyRequest(EntityPtr(&entity));
    if (sourceId != 0)
    {
        const std::lock_guard<std::mutex> lock(g_unitEventMutex);
        g_deaths.push_back({ sourceId, popCost });
    }
}

bool GameUnitComponent::farHeading(const Entity& entity, glm::vec2& dir, float& speed, float& dist, bool spread)
{
    glm::vec3 target;
    if (routeIndex < routeCount)
        target = route[routeIndex];
    else if (targetLocked && moveOrder)
        target = targetPos;
    else
        return false;
    const glm::vec2 here(entity.pos.x, entity.pos.z);
    const glm::vec2 toTarget(target.x - here.x, target.z - here.y);
    dist = glm::length(toTarget);
    if (dist < 1e-3f)
        return false;
    dir = toTarget / dist;
    // No line of sight: the enemy team field's descent (geodesic, around rocks).
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
    speed = targetLocked && moveOrder && wanderOrder // wanderOrder stays set after the order, so gate on it
        ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax)
        : moveSpeed;
    if (spread)
    {
        // Persistent per-unit bias: unbiased, every unit walks the same descent cells in one file.
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
        return glm::vec3(0.0f);
    return glm::vec3(dir.x * speed, 0.0f, dir.y * speed);
}

bool GameUnitComponent::updateFar(Entity& entity, float deltaSec)
{
    if (Globals::networkManager.role() == ENetRole::Client || puppet || !alive() || deltaSec <= 0.0f)
        return false;
    if (entity.pos.y < params.voidY) // the full sim never visits an unselected unit: kill it here
    {
        kill(entity);
        return false;
    }
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return false;
    const glm::vec3 pos = entity.pos; // the far truth; the disabled body is teleported to match below
    const glm::vec2 here(pos.x, pos.z);

    if (routeIndex < routeCount)
    {
        if (glm::distance(here, glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
    }
    else if (targetLocked && moveOrder)
    {
        if (wanderOrder) // the full sim's clock never runs while far
        {
            wanderTimeLeft -= deltaSec;
            if (wanderTimeLeft <= 0.0f)
                targetLocked = moveOrder = false;
        }
        if (targetLocked && glm::distance(here, glm::vec2(targetPos.x, targetPos.z)) < params.waypointRadius)
            targetLocked = moveOrder = false;
    }
    glm::vec2 dir;
    float walkSpeed, dist;
    if (!farHeading(entity, dir, walkSpeed, dist))
        return false;
    const Nav::TeamField* raster = Globals::navSystem.isEnabled() ? Globals::navSystem.raster() : nullptr;
    glm::vec2 next = here + dir * glm::min(walkSpeed * deltaSec, dist);
    if (raster && raster->isBlocked(Nav::cellOf(next)))
    {
        // The spread bias aimed at rock: retry the plain heading, hold only when that is blocked too.
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
