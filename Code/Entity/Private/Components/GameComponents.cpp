module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Force;
import Physics;
import Spatial;
import Nav;

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
static oc::vector<uint32> g_deaths;

void GameUnitComponent::takeFireRequests(oc::vector<FireRequest>& out)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    out.swap(g_fireRequests);
    g_fireRequests.clear();
}

void GameUnitComponent::takeDeaths(oc::vector<uint32>& outSourceIds)
{
    const std::lock_guard<std::mutex> lock(g_unitEventMutex);
    outSourceIds.swap(g_deaths);
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

void GameUnitComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform&)
{
    puppet = info.puppet;
    team = info.team;
    health = healthMax = info.healthMax;
    energy = energyMax = info.energyMax;
    shieldOutput = info.shieldOutput;
    moveSpeed = info.moveSpeed;
    accel = info.accel;
    attackRange = info.attackRange;
    attackDps = info.attackDps;
    playerDps = info.playerDps;
    emitterDrain = info.emitterDrain;
    ranged = info.ranged;
    standoffRange = info.standoffRange;
    fireInterval = info.fireInterval;
    for (float& h : m_outputHistory)
        h = shieldOutput;
    m_rng = uint32(uintptr_t(this) >> 4) * 2654435761u + 1u; // worker-safe per-unit stream
    m_retargetTimer = 0.0f; // pick a target on the first authority tick
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
        return; // clients mirror via the entity sync's game blob — no sim here
    PhysicsComponent* pc = getComponent<PhysicsComponent>(&entity);
    ForceComponent* fc = getComponent<ForceComponent>(&entity);
    if (!pc || !pc->body.isValid())
        return;
    const glm::vec3 pos = pc->body.getPosition();
    // Crowd presence for the anti-clumping gradient (players count as crowd too).
    if (params.navEnabled && Globals::navSystem.isEnabled())
        Globals::navSystem.density(team).splat(glm::vec2(pos.x, pos.z), 1);
    if (puppet)
        return; // puppets are state carriers (GamePlayer writes them) — no sim
    if (health <= 0.0f || pos.y < params.voidY)
    {
        health = 0.0f;
        if (!deathReported) // report ONCE — the destroy is queued, so a tick may still run
        {
            deathReported = true;
            Globals::scriptEvents.addDestroyRequest(EntityPtr(&entity)); // drained by the main loop
            if (sourceId != 0)
            {
                const std::lock_guard<std::mutex> lock(g_unitEventMutex);
                g_deaths.push_back(sourceId); // its spawner frees a roster slot
            }
        }
        return;
    }

    // ---- where to walk: route first, then the locked order, then auto-targeting ----
    bool routing = false;
    glm::vec3 walkTarget = pos;
    bool haveWalkTarget = false;
    if (routeIndex < routeCount)
    {
        if (glm::distance(glm::vec2(pos.x, pos.z),
            glm::vec2(route[routeIndex].x, route[routeIndex].z)) < params.waypointRadius)
            ++routeIndex;
        if (routeIndex < routeCount)
        {
            walkTarget = route[routeIndex];
            routing = haveWalkTarget = true;
        }
    }
    // NAV: sample every other team's flow field at our cell; the geodesically nearest enemy wins
    // and its descent direction is the walk direction (routes around walls). Falls through to the
    // local search when no field covers us.
    bool navResolved = false; // a field named our target (steer by it, skip the local search)
    bool navSteer = false;    // ... and gave a usable direction (zero AT the source)
    glm::vec2 navDir(0.0f);
    if (!routing && !targetLocked && params.navEnabled && Globals::navSystem.anyFieldPublished())
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
            const Nav::TeamField::Sample s = field->sample(glm::vec2(pos.x, pos.z), m_rng);
            if (s.valid && (!best.valid || s.dist < best.dist))
            {
                best = s;
                bestField = field;
            }
        }
        if (best.valid)
        {
            const Nav::NavSource& src = bestField->sourceAt(best.srcIndex);
            targetPos = src.pos;
            hasTarget = true;
            walkTarget = targetPos;
            haveWalkTarget = true;
            navDir = best.descentDir;
            navResolved = true;
            navSteer = glm::dot(navDir, navDir) > 0.5f;
        }
    }
    if (!routing && !navResolved)
    {
        m_retargetTimer -= deltaSec;
        if (!targetLocked && (m_retargetTimer <= 0.0f || !hasTarget))
        {
            // Random pick among the 4 NEAREST enemy structures (spatial query — local harassment
            // with some unpredictability); fallback: the nearest enemy player body.
            m_retargetTimer = params.retargetInterval * (0.7f + 0.6f * unitRand01(m_rng));
            thread_local oc::vector<uint64> results;
            Globals::spatialIndex.querySphere(glm::dvec3(pos), params.targetSearchRadius,
                SpatialLayer_Render, results);
            struct Candidate { float distSq; glm::vec3 pos; };
            Candidate best[4];
            int count = 0;
            // The nearest enemy PLAYER rides the same query: capsules carry puppet
            // GameUnitComponents and sit in the spatial index like everything else.
            float bestPlayerDistSq = FLT_MAX;
            glm::vec3 bestPlayerPos(0.0f);
            for (const uint64 user : results)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                if (const GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
                    pu && pu->puppet && pu->team != team && pu->alive())
                {
                    const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - glm::vec2(pos.x, pos.z);
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
                const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - glm::vec2(pos.x, pos.z);
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
    }

    // ---- melee/ranged combat: hit whatever ENEMY structure is actually in reach (small spatial
    // ---- probe — decoupled from the walk target, so a wall in the way gets chewed too) ----
    float stopRange = attackRange;
    if (!routing)
    {
        thread_local oc::vector<uint64> nearby;
        Globals::spatialIndex.querySphere(glm::dvec3(pos), attackRange + 6.0f,
            SpatialLayer_Render, nearby);
        GameStructureComponent* bite = nullptr;
        float biteDist = FLT_MAX;
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            if (!sc || sc->invulnerable || sc->team == team || !sc->alive())
                continue;
            const float d = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(other->pos.x, other->pos.z))
                - sc->meleeRadius;
            if (d < biteDist)
            {
                biteDist = d;
                bite = sc;
            }
        }
        if (ranged)
        {
            stopRange = standoffRange;
            m_fireTimer -= deltaSec;
            if (haveWalkTarget && m_fireTimer <= 0.0f && glm::distance(glm::vec2(pos.x, pos.z),
                glm::vec2(walkTarget.x, walkTarget.z)) <= standoffRange)
            {
                // Spawning is main-thread only: ASK for the shot, the game services the queue.
                const std::lock_guard<std::mutex> lock(g_unitEventMutex);
                g_fireRequests.push_back(FireRequest{ pos, walkTarget, (uint8)team });
                m_fireTimer = fireInterval * (0.8f + 0.4f * unitRand01(m_rng));
            }
        }
        else if (bite && biteDist <= attackRange && pos.y > -2.0f && pos.y < 8.0f)
        {
            bite->damage(attackDps * deltaSec); // atomic — workers bite concurrently
            stopRange = attackRange + bite->meleeRadius;
        }
        // Lean on the nearest ACTIVE enemy bubble: emitter siege drain, shield-state-independent.
        GameStructureComponent* strain = nullptr;
        float strainDist = 12.0f;
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameStructureComponent* sc = getComponent<GameStructureComponent>(other);
            if (!sc || !sc->strainable || sc->team == team)
                continue;
            const float d = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(other->pos.x, other->pos.z));
            if (d < strainDist)
            {
                strainDist = d;
                strain = sc;
            }
        }
        if (strain)
            strain->addLoad(emitterDrain);

        // DIRECT player damage: any enemy-team player inside melee reach gets chewed — no
        // targeting needed, standing in the swarm hurts. Found in the SAME short probe as the
        // structure bite; same damage() call as every other victim (the puppet component banks it
        // in pendingDamage; the game routes that to the owner).
        if (playerDps > 0.0f)
            for (const uint64 user : nearby)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
                if (!pu || !pu->puppet || pu->team == team)
                    continue;
                const glm::vec2 toPlayer(other->pos.x - pos.x, other->pos.z - pos.z);
                const float reach = attackRange + 0.8f; // capsule body allowance
                if (glm::dot(toPlayer, toPlayer) < reach * reach
                    && glm::abs(other->pos.y - pos.y) < 3.0f)
                    pu->damage(playerDps * deltaSec);
            }
    }

    // ---- steering: planar velocity along the flow-field descent (or straight at the walk target
    // where no field covers us), minus the own-crowd density gradient, stuck-sidestep as the last
    // resort. Writes are QUEUED (workers) — one frame of latency, the queue's normal contract.
    glm::vec3 vel = pc->body.getLinearVelocity();
    if (haveWalkTarget)
    {
        const glm::vec2 toTarget(walkTarget.x - pos.x, walkTarget.z - pos.z);
        const float dist = glm::length(toTarget);
        if (dist > stopRange)
        {
            glm::vec2 dir = navSteer ? navDir : toTarget / glm::max(dist, 1e-3f);
            if (params.navEnabled && params.spreadGain > 0.0f && Globals::navSystem.isEnabled())
            {
                // Down the crowd gradient: a unit shifts away from where its own team is thickest.
                // Bounded so a wall of allies never overrides the destination entirely.
                const glm::vec2 g = Globals::navSystem.density(team).gradient(glm::vec2(pos.x, pos.z));
                const float gl = glm::length(g);
                if (gl > 1e-4f)
                    dir = glm::normalize(dir - g / gl * glm::min(gl * params.spreadGain, 0.9f));
            }
            const glm::vec2 planarVel(vel.x, vel.z);
            if (glm::dot(planarVel, planarVel) < 0.09f * moveSpeed * moveSpeed)
                m_stuckTimer += deltaSec;
            else
                m_stuckTimer = 0.0f;
            // With a field steering, the sidestep only fires after a longer stall (bodies blocking
            // each other) — the field already routes around geometry.
            if (m_stuckTimer > (navSteer ? 1.5f : 0.7f) && m_avoidTimer <= 0.0f)
            {
                m_avoidTimer = 0.6f + 0.6f * unitRand01(m_rng);
                m_avoidSign = unitRand01(m_rng) < 0.5f ? -1.0f : 1.0f;
                m_stuckTimer = 0.0f;
            }
            if (m_avoidTimer > 0.0f)
            {
                m_avoidTimer -= deltaSec;
                dir = glm::normalize(glm::vec2(-dir.y, dir.x) * m_avoidSign + dir * 0.25f);
            }
            glm::vec3 dv(dir.x * moveSpeed - vel.x, 0.0f, dir.y * moveSpeed - vel.z);
            const float maxDv = accel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetLinearVelocity,
                vel + glm::vec3(dv.x, 0.0f, dv.z));
            vel += glm::vec3(dv.x, 0.0f, dv.z);
        }
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
        if (fc->emitter.getEquilibriumRadius() < params.damageRadius && pressure > iso)
            health = glm::max(0.0f, health - params.fieldDps * deltaSec);

        // Push normalized by the output that PRODUCED the ~2-frame-latent readback, so a collapsed
        // shield is shoved exactly like a live one. Speed clamp rides the same queue (approximate
        // by one frame — the queue itself is one frame latent anyway).
        const glm::vec3 force = fc->emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
        if (glm::dot(force, force) > 1e-8f)
        {
            Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::ApplyImpulse,
                force * deltaSec * params.pushGain * pressure * tension);
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

}

void GameUnitComponent::damage(float amount)
{
    if (amount <= 0.0f)
        return;
    if (puppet)
        atomicAdd(pendingDamage, amount); // player health is owner-computed — bank it for routing
    else
        atomicSubClamped(health, amount);
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
        BarracksData& b = barracks;
        // idle Constructors in range accelerate the spawn clock (boost 1 = double speed)
        b.spawnTimer = glm::max(0.0f, b.spawnTimer - deltaSec * (1.0f + b.boost));
        if (b.spawnTimer <= 0.0f && b.aliveUnits < params.barracksUnitLimit && store[2] >= b.spawnCost)
        {
            store[2] -= b.spawnCost; // conveyor-fed minerals pay the unit (refunded on spawn fail)
            ++b.aliveUnits;          // the unit's death event decrements it again
            b.spawnTimer = params.barracksSpawnInterval;
            const std::lock_guard<std::mutex> lock(g_structureEventMutex);
            g_spawnRequests.push_back(structureId);
        }
    }
    else if (machineKind == EMachineKind::Turret && !blueprint)
    {
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

void GameStructureComponent::link(Entity& a, Entity& b, uint8 medium, uint8 cableTier, float throughput)
{
    GameStructureComponent* ca = getComponent<GameStructureComponent>(&a);
    GameStructureComponent* cb = getComponent<GameStructureComponent>(&b);
    if (!ca || !cb || &a == &b)
        return;
    unlink(a, b, medium); // the pair's SAME-medium link re-links in place (retype); others stay
    ca->links.push_back(GameStructureLink{ EntityPtr(&b), medium, cableTier, throughput, /*owner*/ true });
    cb->links.push_back(GameStructureLink{ EntityPtr(&a), medium, cableTier, throughput, /*owner*/ false });
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
    // Main thread, inside physics.update's contact dispatch. First contact spends the shot:
    // enemy-team victims take the hit, everything else (ground, own team) just stops it.
    if (!begin || spent || !isAuthority())
        return;
    // damage() handles puppets itself (banks into pendingDamage for owner routing), so enemy
    // projectiles hurt players through the exact same call as units.
    if (GameUnitComponent* unit = getComponent<GameUnitComponent>(&other); unit && unit->team != team)
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
}

void writeGameStructureSpawnInfo(const GameStructureComponent::SpawnInfo& info, AssetNode& out)
{
    const GameStructureComponent::SpawnInfo d;
    if (info.team != d.team)                 out.set("Team", oc::to_string(info.team));
    if (info.healthMax != d.healthMax)       out.set("HealthMax", info.healthMax);
    if (info.invulnerable != d.invulnerable) out.set("Invulnerable", info.invulnerable);
    if (info.meleeRadius != d.meleeRadius)   out.set("MeleeRadius", info.meleeRadius);
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
}
