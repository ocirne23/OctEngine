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
import Particle; // the reveal smoke (ParticleSystem::emitPuff)
import RendererVK;

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
static std::mutex g_soundCueMutex;
static oc::vector<GameUnitComponent::SoundCue> g_soundCues;
static oc::atomic<uint32> g_soundCueCounts[(int)GameUnitComponent::ESoundCue::Count];

void GameUnitComponent::reportSound(ESoundCue kind, const glm::vec3& pos, float size)
{
    if (g_soundCueCounts[(int)kind].fetch_add(1, oc::memory_order_relaxed) >= MaxSoundCues)
        return; // counted only
    const std::lock_guard<std::mutex> lock(g_soundCueMutex);
    g_soundCues.push_back(SoundCue{ pos, size, kind });
}

void GameUnitComponent::takeSoundCues(oc::vector<SoundCue>& out, uint32 counts[(int)ESoundCue::Count])
{
    const std::lock_guard<std::mutex> lock(g_soundCueMutex);
    out.swap(g_soundCues);
    g_soundCues.clear();
    for (int k = 0; k < (int)ESoundCue::Count; ++k)
        counts[k] = g_soundCueCounts[k].exchange(0, oc::memory_order_relaxed);
}

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

// Every render node of the subtree. unitColor (the prefab's GameUnit Color) wins over a node's own
// authored colour; the friendly tint pulls toward green, not to a flat colour, so the unit types stay
// told apart.
static void tintSubtree(Entity* node, const glm::vec3* unitColor, bool friendly, bool shadow = false)
{
    const auto tint = [&](RenderNode& renderNode, const RenderComponent::SpawnInfo* info)
    {
        if (!renderNode.isValid())
            return;
        // AN EYE (a piece of the unlit `eyes` container, see Debug/CubeGuy.pre): never the body's colour. An enemy's
        // keeps its authored glow, the shadow form included; a friendly unit's is pale.
        if (info && info->containerName == "eyes")
        {
            // An enemy's eyes GLOW at their authored strength; a friendly unit's are pale and do not glow.
            renderNode.setMaterialOverride(friendly ? RenderMaterial::getOrCreateSolidColor(glm::vec3(0.85f, 0.95f, 1.0f))
                : RenderMaterial::getOrCreateSolidColor(info->color.x >= 0.0f ? info->color : glm::vec3(1.0f, 0.12f, 0.08f), info->emissive));
            return;
        }
        glm::vec3 color(1.0f);
        if (unitColor)
            color = *unitColor;
        else if (info && info->color.x >= 0.0f)
            color = info->color;
        constexpr glm::vec3 c_friendlyGreen(0.3f, 1.0f, 0.4f); // the HUD's own-team bar colour
        constexpr float c_tintStrength = 0.55f;
        if (friendly)
            color = glm::mix(color, c_friendlyGreen, c_tintStrength);
        if (shadow) // the shadow form: near-black smoke with a trace of the type's colour, so the types stay told apart
            color = color * 0.06f + glm::vec3(0.012f, 0.008f, 0.02f);
        renderNode.setMaterialOverride(RenderMaterial::getOrCreateSolidColor(color));
    };
    if (RenderComponent* rc = getComponent<RenderComponent>(node))
        tint(rc->node, getRenderSpawnInfo(node));
    if (HumanoidAnimatorComponent* boneModel = getComponent<HumanoidAnimatorComponent>(node); boneModel && boneModel->rig)
        for (size_t i = 0; i < boneModel->bones.size(); ++i)
            tint(boneModel->bones[i].node, boneModel->rig->bones[i].render.get());
    if (const SceneComponent* sc = getComponent<SceneComponent>(node))
        for (const EntityPtr& child : sc->children)
            tintSubtree(child.get(), unitColor, friendly, shadow);
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
    structureDamage = info.structureDamage >= 0.0f ? info.structureDamage : info.attackDamage;
    emitterDrain = info.emitterDrain;
    ranged = info.ranged;
    standoffRange = info.standoffRange;
    fireInterval = info.fireInterval;
    shotKind = info.shotKind;
    alwaysDisplayHealth = info.alwaysDisplayHealth;
    fieldBlocked = info.fieldBlocked;
    heightLimit = info.heightLimit;
    for (float& h : m_outputHistory)
        h = shieldOutput;
    m_rng = uint32(uintptr_t(this) >> 4) * 2654435761u + 1u;
    m_retargetTimer = 0.0f;
    // Random phase on the periodic timers: a batch spawned in one frame must not act in lockstep.
    m_seedDue = (float)Globals::time.getSimElapsedSec() + params.seedRequestInterval * unitRand01(m_rng);
    m_stuckCheckTimer = 0.75f * unitRand01(m_rng);
    m_attackTimer = attackInterval * unitRand01(m_rng);
    m_snowIdle = unitRand01(m_rng); // the standing re-stamp's phase (tickSnow)
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
    if (info.color.x >= 0.0f && Globals::rendererVK.isInitialized())
        tintSubtree(&entity, &info.color, false); // Scene (bit 0) spawned the model before this component
}

// The death's sound cue, on every role (a replicated drop to 0 counts too). A hit that does not kill makes no cue.
void GameUnitComponent::tickDeathCue(const Entity& entity)
{
    if (m_lastHealth > 0.0f && health <= 0.0f)
        reportSound(ESoundCue::UnitDeath, entity.pos, bodyRadius);
    m_lastHealth = health;
}

// The MODEL is the child with a HumanoidAnimatorComponent (CubeGuy) - a shared body prefab. The body
// has LockRotation and the root's rot is the physics pose, so the facing goes on the model: a yaw about Y
// that turns the model's +X to the heading. The walk is fed with the same distance, because a child does
// not move in its parent's space. Off the POSITION delta, so a replicated unit on a client does it too.
// A unit that stands still with its model at rest touches nothing (m_modelResting). Parent -> child
// writes: safe in the pass.
void GameUnitComponent::tickModel(Entity& entity, float deltaSec)
{
    const SceneComponent* sc = getComponent<SceneComponent>(&entity);
    if (!sc)
        return;
    const glm::vec2 here(entity.pos.x, entity.pos.z);
    const glm::vec2 moved = m_hasModelPos ? here - m_modelLastPos : glm::vec2(0.0f);
    m_modelLastPos = here;
    m_hasModelPos = true;
    // A standing unit whose model is already at rest: nothing to feed and nothing to turn - out before the
    // child is even touched (no sqrt either: the test is on the squared length).
    const float distSq = glm::dot(moved, moved);
    if (distSq <= 0.0f && m_modelResting)
        return;
    const float dist = std::sqrt(distSq);

    // The model ticks ONLY on this feed, with this tick's (SIM LOD) delta - also at 0 m, while its swing
    // still has to fade out. Once the animator reports the rest pose (it sets atRest in its own visit,
    // after this one), a standing unit feeds nothing and returns at the top from the next tick on.
    Entity* model = nullptr;
    HumanoidAnimatorComponent* walk = nullptr;
    for (const EntityPtr& child : sc->children)
        if ((walk = getComponent<HumanoidAnimatorComponent>(child.get())) != nullptr)
        {
            model = child.get();
            break;
        }
    if (!model)
    {
        m_modelResting = true; // no model (the player capsule): never look again until it moves
        return;
    }
    m_modelResting = distSq <= 0.0f && walk->atRest;
    if (m_modelResting)
        return;
    walk->feed(dist, deltaSec);

    constexpr float c_minFacingSpeed = 0.25f; // m/s: below it the move is a shove, not a heading
    constexpr float c_turnRate = 12.0f;       // rad/s
    if (dist < c_minFacingSpeed * deltaSec)
        return;
    // A unit that marches straight already faces its heading: one dot, no atan2, no write.
    constexpr float c_alignedCos = 0.9998f; // ~1.1 degrees
    if (glm::dot(moved, m_modelForward) >= c_alignedCos * dist)
        return;
    constexpr float c_pi = 3.14159265f;
    float turn = std::atan2(-moved.y, moved.x) - m_modelYaw;
    turn -= 2.0f * c_pi * std::floor(turn / (2.0f * c_pi) + 0.5f); // shortest way round
    const float step = c_turnRate * deltaSec;
    turn = glm::clamp(turn, -step, step);
    if (turn == 0.0f)
        return;
    m_modelYaw += turn;
    const float sh = std::sin(0.5f * m_modelYaw), ch = std::cos(0.5f * m_modelYaw);
    model->rot = glm::quat(ch, 0.0f, sh, 0.0f);
    m_modelForward = glm::vec2(ch * ch - sh * sh, -2.0f * sh * ch); // the model's +X in world XZ: (cos yaw, -sin yaw)
}

// THE TRACK IN THE SNOW (Renderer::addSnowTrack): the collider's bottom swept from the last stamp to here, once it has
// moved a few cm (a standing unit stamps nothing). A jump of metres (a teleport, a respawn, a correction) starts a new track
// instead of a trench across the gap. The renderer drops it where no snow lies, far away, or with the feet in the air.
// A unit that STANDS presses its feet again once a second (falling snow would bury its dent otherwise; an unchanged dent
// costs the stamp nothing), on a per-unit phase so a crowd does not stamp in one frame.
void GameUnitComponent::tickSnow(const Entity& entity, float deltaSec)
{
    const glm::vec3 feet(entity.pos.x, entity.pos.y - m_bodyTop, entity.pos.z);
    if (m_hasSnowFeet)
    {
        const glm::vec3 step = feet - m_snowFeet;
        const float stepSq = glm::dot(step, step);
        constexpr float c_minStep = 0.05f; // m
        if (stepSq < c_minStep * c_minStep)
        {
            constexpr float c_restampSeconds = 1.0f;
            m_snowIdle += deltaSec;
            if (m_snowIdle >= c_restampSeconds)
            {
                m_snowIdle -= c_restampSeconds * std::floor(m_snowIdle / c_restampSeconds);
                Globals::rendererVK.addSnowTrack(m_snowFeet, m_snowFeet, 0.8f * bodyRadius);
            }
            return;
        }
        constexpr float c_maxStep = 6.0f;  // m: farther is a jump, not a walk
        if (stepSq > c_maxStep * c_maxStep)
            Globals::rendererVK.addSnowTrack(feet, feet, 0.8f * bodyRadius);
        else
            Globals::rendererVK.addSnowTrack(m_snowFeet, feet, 0.8f * bodyRadius);
    }
    m_snowFeet = feet;
    m_hasSnowFeet = true;
}

void GameUnitComponent::update(Entity& entity, float deltaSec)
{
    tickDeathCue(entity);
    tickModel(entity, deltaSec);
    tickSnow(entity, deltaSec);
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
    // Teleport contract: snap the shown pose, or PhysicsComponent::update shows the pre-teleport
    // body pose until the queued teleport lands.
    t.pc.snapPose(t.entity, clamped);
}

bool GameUnitComponent::applyDamageAndHeal(Tick& t)
{
    // The battery eats direct damage first (the player's applyDamage rule); shield-less bodies take it all.
    // The shadow form drops what hit it, but for the part that hurts a shadow too.
    const float plain = oc::atomic_ref<float>(pendingDamage).exchange(0.0f);
    const float throughShadow = oc::atomic_ref<float>(pendingShadowDamage).exchange(0.0f);
    if (const float dmg = (solid ? plain : 0.0f) + throughShadow; dmg > 0.0f)
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
    if (const float burn = params.sunBurn(healthMax); burn > 0.0f && (int)team == params.sunBurnTeam) // daylight burns this team (no shield absorbs it)
        health = glm::max(health - burn * t.deltaSec, 0.0f);
    // Death before the heal: a heal landing the same tick must not revive the unit (a medic
    // station's radius was making units immortal).
    if (health <= 0.0f || (params.flatGround && t.pos.y < params.voidY))
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
        t.walkIsTarget = true;
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
            if (!sc || sc->invulnerable || sc->rubble || sc->team == team || !sc->alive()) // (rubble being built up has health: no target)
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
    // A hunted enemy, or a wave's march order (its spot is the building it marches at) - not a wander's random spot.
    t.walkIsTarget = hasTarget && !(targetLocked && moveOrder && wanderOrder);
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
    // A BARRACKS unit (the players' own army) drains the battery of the nearest enemy unit whose live bubble covers it
    // (planar, its equilibrium radius) - the demons' emitter strain, turned around.
    constexpr float c_bubbleSearch = 16.0f; // m: past the largest demon bubble (the Titan's ~13 m)
    const float shieldDrain = sourceId != 0 ? glm::max(Globals::settings.game.structures.barracksUnitShieldDrain, 0.0f) : 0.0f;
    GameUnitComponent* shieldVictim = nullptr;
    float shieldVictimDistSq = FLT_MAX;
    Globals::spatialIndex.forEachInSphere(glm::dvec3(t.pos),
        glm::max(glm::max(glm::max(glm::max(attackRange + 6.0f, c_strainRange), engageRadius), breakRadius),
            shieldDrain > 0.0f ? c_bubbleSearch : 0.0f),
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
            if (!sc->invulnerable && !sc->rubble && sc->alive() && d - sc->meleeRadius < biteDist)
            {
                biteDist = d - sc->meleeRadius;
                bite = sc;
                bitePos = other->pos;
            }
            return;
        }
        GameUnitComponent* pu = getComponent<GameUnitComponent>(other);
        if (!pu || pu == this || pu->team == team || !pu->alive())
            return;
        const glm::vec2 to(other->pos.x - t.pos.x, other->pos.z - t.pos.z);
        if (shieldDrain > 0.0f && !pu->puppet && pu->energy > 0.0f) // (another worker's float: one tick stale at worst)
            if (const ForceComponent* force = getComponent<ForceComponent>(other); force && force->emitter.isValid())
            {
                const float distSq = glm::dot(to, to);
                const float radius = force->emitter.getEquilibriumRadius();
                if (distSq <= radius * radius && distSq < shieldVictimDistSq)
                {
                    shieldVictimDistSq = distSq;
                    shieldVictim = pu;
                }
            }
        if (glm::abs(other->pos.y - t.pos.y) >= 3.0f)
            return;
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
    const bool canBite = bite && biteDist <= attackRange
        && (!params.flatGround || (t.pos.y > -2.0f && t.pos.y < 8.0f));
    if (meleeVictim)
        t.stopRange = glm::max(t.stopRange, meleeVictimReach);
    const float swing = meleeVictim ? (meleeVictim->puppet ? playerDamage : attackDamage) : attackDamage;
    if (!ranged && m_attackTimer <= 0.0f && swing > 0.0f && (meleeVictim || canBite))
    {
        if (meleeVictim)
            meleeVictim->damage(swing);
        else if (const float back = bite->damage(structureDamage * params.structureBiteMult); back > 0.0f)
            damage(back, /*throughShadow*/ true); // a Retaliate ward's spikes: the biter is at the wall, solid or not
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
        t.walkIsTarget = true;
    }
    if (ordered && bite && biteDist < breakRadius)
    {
        targetLocked = moveOrder = false; // the order is done: the AI hunts from here
        hasTarget = false;
        t.walkTarget = bitePos;
        t.haveWalkTarget = true;
        t.walkIsOrder = false;
        t.walkIsTarget = true;
    }
    if (ranged)
    {
        t.stopRange = standoffRange;
        m_fireTimer -= t.deltaSec;
        // Only at a REAL target (walkIsTarget: an enemy the Nav field or the search found, or one it engages) - never
        // at a route waypoint, an order's spot, or the follow band's lane point 8 m ahead: it shot at empty ground.
        if (t.haveWalkTarget && t.walkIsTarget && m_fireTimer <= 0.0f
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
    if (shieldVictim)
        shieldVictim->drainShield(shieldDrain * t.deltaSec);
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
        const float walkSpeed = (wandering
            ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax) : moveSpeed) * speedScale();
        // Stuck detection by displacement checkpoints: per-tick progress never accumulates on a
        // jittering heading. STUCK = under 40 % of the progress its walk speed makes over a checkpoint, never more than
        // 0.6 m: a fixed 0.6 m read a slow demon in a player field (a Giant: 2 x "Demon speed" 0.7 x "Demon field speed"
        // 0.5 = 0.7 m/s, 0.53 m a checkpoint) as stuck at every check, and the unstick steering turned it away from the
        // field - it looked pushed out.
        constexpr float c_stuckCheckSec = 0.75f;
        m_stuckCheckTimer -= t.deltaSec;
        if (m_stuckCheckTimer <= 0.0f)
        {
            const float moved = m_hasStuckAnchor ? glm::distance(t.here, m_stuckAnchor) : 10.0f;
            const float stuckBelow = glm::min(0.6f, 0.4f * walkSpeed * c_stuckCheckSec);
            m_pressureTimer = moved < stuckBelow ? m_pressureTimer + c_stuckCheckSec : 0.0f;
            m_stuckAnchor = t.here;
            m_hasStuckAnchor = true;
            m_stuckCheckTimer = c_stuckCheckSec;
        }
        const glm::vec2 dir = raster ? steerHeading(t, goalDir, *raster) : goalDir;
        const glm::vec2 measured(t.vel.x, t.vel.z); // the body's REAL planar velocity (pre-command)
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

// A unit with a LIVE bubble reads the field off its own emitter (tickShieldBubble): the battery, the collapse and the
// field PUSH. Every other unit - a shield-less body, or one whose bubble is GONE - reads the baked field (tickBakedField):
// the reveal and the burn, never a push.
void GameUnitComponent::tickField(Tick& t)
{
    if (t.fc && t.fc->emitter.isValid())
    {
        if (!shieldGone())
        {
            tickShieldBubble(t);
            return;
        }
        // A GONE bubble (collapsed to nothing) has no field at all: its emitter is off (here at once, and World's bubble
        // gate keeps it off). A heal that refills the battery brings the bubble back.
        t.fc->emitter.setActive(false);
    }
    tickBakedField(t);
}

void GameUnitComponent::tickShieldBubble(Tick& t)
{
    auto& emitter = t.fc->emitter;
    const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
    const float pressure = emitter.getPressure();
    const float tension = 1.0f + params.tension * pressure;
    // THE DRAIN: "energy/s per unit of pressure" (the player rule) - but a pressure is a field value near the iso value
    // (0.01), so that alone barely touches a battery of hundreds. Plus a SHARE OF THE BATTERY per second at the iso
    // pressure ("Shield drain (battery share/s at iso)"; deeper pushes faster, up to 4x): a demon pushing into a player
    // field empties in about the same time whatever its battery's size.
    const float shareDrain = glm::min(pressure / iso, 4.0f) * glm::max(params.shieldDrainShare, 0.0f) * energyMax;
    const float deflected = oc::atomic_ref<float>(pendingShieldDrain).exchange(0.0f); // the shots it bent (drainShield)
    const float before = energy;
    energy = glm::max(0.0f, energy - (pressure * tension * params.energyDrainRate + shareDrain) * t.deltaSec - deflected);
    if (before > 0.0f && energy <= 0.0f)
        collapsed = true;
    // THE COLLAPSE: an emptied battery SHRINKS the bubble to NOTHING over c_shieldCollapseSec (the output follows
    // m_shieldFade down to 0), not at once. No floor: a 0.01 field per collapsed demon added up over a crowd to a real,
    // invisible field - it bent the cannon shells and drained buildings. A client follows the synced output (NetworkManager).
    constexpr float c_shieldCollapseSec = 0.8f;
    m_shieldFade = energy > 0.0f ? 1.0f : glm::max(m_shieldFade - t.deltaSec / c_shieldCollapseSec, 0.0f);
    const float output = shieldOutput * m_shieldFade * m_shieldFade; // eased: it falls away faster at the end
    emitter.setOutput(output);
    emitter.setShellAlpha(m_shieldFade > 0.0f ? 1.0f : 0.0f);

    setSolid(t, pressure, iso);
    if (collapsed && emitter.getEquilibriumRadius() < params.damageRadius && pressure > iso)
        health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * t.deltaSec);

    // THE PUSH, only where the fields push (params.fieldPush, or the prefab's FieldBlocked). The readback is normalized
    // by the output that PRODUCED it (~2 frames latent), so a collapsing shield is shoved like a live one. A unit whose
    // battery holds stands its ground: "Push on shielded units (x)" of the push.
    if (params.fieldPush || fieldBlocked)
    {
        const glm::vec3 force = emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
        const float pushRamp = glm::smoothstep(iso * params.fieldPushStart, iso, pressure);
        const float shieldedMult = energy > 0.0f ? glm::max(params.shieldedPushMult, 0.0f) : 1.0f;
        if (pushRamp > 0.0f && glm::dot(force, force) > 1e-8f)
            t.impulse += force * (t.deltaSec * params.pushGain * pressure * tension * pushRamp * shieldedMult);
    }
    m_outputHistory[0] = m_outputHistory[1];
    m_outputHistory[1] = m_outputHistory[2];
    m_outputHistory[2] = output;
}

// The burn is GRADED by the depth in the field (smoothstep up to the iso value), so it grows over the shell's fringe instead
// of switching on at it.
void GameUnitComponent::tickBakedField(Tick& t)
{
    const ForceSystem::FieldSample fs = Globals::forceSystem.sampleBakedField(t.pos, team);
    if (!fs.valid) // bake disabled: the stamped-radius fallback
    {
        setSolid(t, t.inEnemyBubble ? 1.0f : 0.0f, 1.0f);
        if (t.inEnemyBubble)
            health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * t.deltaSec);
        return;
    }
    const float iso = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f);
    t.sheltered = fs.inside && fs.owningTeam == team ? 1 : 0; // (isSheltered's answer: this is the same sample)
    // ON TERRAIN (not flatGround) the bake is ONE horizontal slice of the field, at one world height: a unit higher or
    // lower than that slice reads a weaker field than the one it stands in, or none. The combat probe's planar test
    // against the emitters' bubble radii (inEnemyBubble) does not depend on the height, and the published field zones
    // cover a large bubble's outer part, which the probe does not reach: inside a bubble by either, the unit is in the
    // field whatever the slice says. A SHELTERED unit (its own team's bubble owns the point) is in no enemy bubble.
    bool inBubble = !params.flatGround && !t.sheltered && t.inEnemyBubble;
    if (!params.flatGround && !t.sheltered && !inBubble && (int)team != params.fieldZoneTeam)
        for (int i = 0; i < glm::min(params.fieldZoneCount, 64) && !inBubble; ++i)
        {
            const float* zone = params.fieldZones[i];
            const glm::vec2 d(t.pos.x - zone[0], t.pos.z - zone[2]);
            inBubble = glm::dot(d, d) < zone[3] * zone[3];
        }
    const float exposure = t.sheltered ? 0.0f : inBubble ? 1.0f : glm::smoothstep(0.0f, iso, fs.opposing);
    setSolid(t, inBubble ? iso : fs.opposing, iso);
    if (exposure > 0.0f)
        health = glm::max(0.0f, health - params.fieldDps * params.fieldDpsMult * exposure * t.deltaSec);
}

bool GameUnitComponent::isSheltered(Tick& t)
{
    if (t.sheltered < 0)
    {
        const ForceSystem::FieldSample fs = Globals::forceSystem.sampleBakedField(t.pos, team);
        const bool sheltered = fs.valid ? fs.inside && fs.owningTeam == team
                                        : t.fc && t.fc->emitter.isValid() && energy > 0.0f;
        t.sheltered = sheltered ? 1 : 0;
    }
    return t.sheltered != 0;
}

// Solid from half the iso field on: a little outside the drawn shell, so a turret reaches a demon at the edge. Not
// where a bubble of its own team is the stronger one (isSheltered): an enemy field that reaches into it does not own it.
void GameUnitComponent::setSolid(Tick& t, float opposing, float iso)
{
    inField = opposing >= iso * 0.5f && !isSheltered(t); // for speedScale: a field slows the slow team (next tick's steering reads it)
    bool now = (int)team != params.shadowTeam || params.sunBurn(healthMax) > 0.0f || inField;
    for (int i = 0; !now && i < glm::min(params.lightPointCount, 8); ++i) // a player's staff light
    {
        const float* light = params.lightPoints[i];
        const glm::vec3 d = t.pos - glm::vec3(light[0], light[1], light[2]);
        now = glm::dot(d, d) < light[3] * light[3];
    }
    // THE LIGHT CONES of another team (flat wedges, see GameUnitParams::lightCones): in one, the unit is solid as in a
    // field, and it burns at each cone's own rate - so two crossed cones burn twice as fast. Every tickField path ends in
    // one setSolid call, so this is once a tick.
    {
        int cones = 0;
        float coneDps = 0.0f;
        for (int i = 0; i < glm::min(params.lightConeCount, (int)GameUnitParams::MaxLightCones); ++i)
        {
            const float* cone = params.lightCones[i];
            if ((uint32)cone[6] == team)
                continue;
            bool inside;
            if (cone[10] > 0.5f) // a 3D cone (the staff beam): from the source along (x, y, z)
            {
                const glm::vec3 d(t.pos.x - cone[0], t.pos.y - cone[8], t.pos.z - cone[1]);
                const float distSq = glm::dot(d, d);
                if (distSq > cone[2] * cone[2])
                    continue;
                // In the cone, or with its BODY on the axis (a beam aimed at a unit's middle: its origin may be under the
                // axis by more than the narrow cone is wide there).
                const float along = d.x * cone[3] + d.y * cone[9] + d.z * cone[4];
                inside = along > 0.0f && (along >= cone[5] * std::sqrt(distSq) || distSq - along * along <= bodyRadius * bodyRadius * 4.0f);
            }
            else
            {
                const glm::vec2 d(t.pos.x - cone[0], t.pos.z - cone[1]);
                const float distSq = glm::dot(d, d);
                if (distSq > cone[2] * cone[2])
                    continue;
                // Inside the wedge: the angle to the axis is under the half angle (dot >= cos x distance); the lamp's
                // own foot (under 1.5 m) always counts.
                inside = distSq < 2.25f || d.x * cone[3] + d.y * cone[4] >= cone[5] * std::sqrt(distSq);
            }
            if (inside)
            {
                ++cones;
                coneDps += cone[7];
            }
        }
        // SHELTERED (isSheltered): inside a bubble of its OWN team the cones do nothing to it - no reveal, no burn.
        if (cones > 0 && isSheltered(t))
            cones = 0;
        if (cones > 0)
        {
            now = true;
            health = glm::max(0.0f, health - coneDps * t.deltaSec);
        }
    }
    if (now == solid)
        return;
    // THE REVEAL: a puff of black smoke at the moment the light makes a shadow solid. One request a unit; the particle
    // system draws at most a dozen puffs a frame, near the camera (ParticleSystem::emitPuff), so a crowd of
    // thousands that walks into a field costs no more than a few units do.
    if (now && params.revealPuff != UINT32_MAX)
    {
        Globals::particleSystem.emitPuff(params.revealPuff, t.pos);
        reportSound(ESoundCue::UnitReveal, t.pos, bodyRadius); // the sizzle that goes with the smoke
    }
    solid = now;
    const SpawnInfo* info = getGameUnitSpawnInfo(&t.entity);
    tintSubtree(&t.entity, info && info->color.x >= 0.0f ? &info->color : nullptr, tintState == 1, !now);
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

void GameUnitComponent::damage(float amount, bool throughShadow)
{
    if (amount <= 0.0f)
        return;
    atomicAdd(throughShadow ? pendingShadowDamage : pendingDamage, amount); // every victim banks into the inbox: the owner tick absorbs shield-first
}

void GameUnitComponent::heal(float amount)
{
    if (amount > 0.0f)
        atomicAdd(pendingHeal, amount);
}

void GameUnitComponent::drainShield(float amount)
{
    if (amount > 0.0f)
        atomicAdd(pendingShieldDrain, amount);
}

float GameUnitComponent::takePendingDamage()
{
    oc::atomic_ref<float> ref(pendingDamage), shadow(pendingShadowDamage);
    return ref.exchange(0.0f) + shadow.exchange(0.0f);
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
    const SpawnInfo* info = getGameUnitSpawnInfo(&entity);
    tintSubtree(&entity, info && info->color.x >= 0.0f ? &info->color : nullptr, friendly);
}

void GameUnitComponent::kill(Entity& entity)
{
    health = 0.0f;
    if (deathReported) // the queued destroy may take a tick to drain
        return;
    deathReported = true;
    if (m_lastHealth > 0.0f) // the death cue, once: tickDeathCue compares against m_lastHealth too
    {
        reportSound(ESoundCue::UnitDeath, entity.pos, bodyRadius);
        m_lastHealth = 0.0f;
    }
    // THE DEATH BURST of an enemy of the viewer (never a player capsule): one request; the particle system draws at
    // most a dozen a frame, near the camera (ParticleSystem::emitPuff), so hundreds of deaths a second stay cheap.
    if (!puppet && (int)team != params.localTeam && params.deathPuff != UINT32_MAX)
        Globals::particleSystem.emitPuff(params.deathPuff, entity.pos);
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
    speed = (targetLocked && moveOrder && wanderOrder // wanderOrder stays set after the order, so gate on it
        ? glm::min(moveSpeed * params.wanderSpeedMult, params.wanderSpeedMax)
        : moveSpeed) * speedScale();
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
    if (const float burn = params.sunBurn(healthMax); burn > 0.0f && (int)team == params.sunBurnTeam) // the full sim's daylight burn, at the far cadence
        health = glm::max(health - burn * deltaSec, 0.0f);
    // The full sim never visits an unselected unit: kill it here.
    if (health <= 0.0f || (params.flatGround && entity.pos.y < params.voidY))
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

    // The height: a little over the terrain where the game knows it (the sandbox world's coarse ground) - the collider
    // exists only around the players, and a unit that kept its spawn height came in over or under the ground.
    float y = pos.y;
    if (params.farGround)
    {
        constexpr float c_none = -1.0e30f;
        const float ground = params.farGround(next.x, next.y, c_none);
        if (ground > c_none * 0.5f)
            y = ground + params.farLift;
    }
    // Teleport contract: body pose + pose snap, entity position, spatial entry.
    const glm::vec3 newPos(next.x, y, next.y);
    Globals::physics.teleportBody(pc->body, newPos, pc->body.getRotation());
    pc->snapPose(entity, newPos);
    entity.pos = newPos; // a unit is a root: local == world
    entity.placeSpatialEntry();
    return true;
}

const GameUnitComponent::SpawnInfo* getGameUnitSpawnInfo(const Entity* entity)
{
    return getSpawnInfo<GameUnitComponent>(entity);
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
    if (info.structureDamage != d.structureDamage) out.set("StructureDamage", info.structureDamage);
    if (info.emitterDrain != d.emitterDrain) out.set("EmitterDrain", info.emitterDrain);
    if (info.ranged != d.ranged)             out.set("Ranged", info.ranged);
    if (info.standoffRange != d.standoffRange) out.set("StandoffRange", info.standoffRange);
    if (info.fireInterval != d.fireInterval) out.set("FireInterval", info.fireInterval);
    if (info.shotKind != d.shotKind)         out.set("ShotKind", oc::to_string((int)info.shotKind));
    if (info.alwaysDisplayHealth != d.alwaysDisplayHealth) out.set("AlwaysDisplayHealth", info.alwaysDisplayHealth);
    if (info.fieldBlocked != d.fieldBlocked) out.set("FieldBlocked", info.fieldBlocked);
    if (info.heightLimit != d.heightLimit)   out.set("HeightLimit", info.heightLimit);
    if (info.color.x >= 0.0f)                out.set("Color", info.color);
}
