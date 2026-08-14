module Game;

import Core;
import Core.glm;
import Core.SDL;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Input;
import UI;
import Entity;
import Physics;
import Force;
import :Player;

void GamePlayer::registerTweaks()
{
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped( ETweakFlags::Synced);
    Tweak::floatVar("Game/Player", "Move speed", &m_moveSpeed, 0.5f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Accel", &m_accel, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Jump speed", &m_jumpSpeed, 0.5f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Sprint mult", &m_sprintMult, 1.0f, 5.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Health max", &m_healthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Player", "Health drain/s", &m_healthDrainRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Shield", "Max output", &m_shieldMaxOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Shield", "Energy max", &m_energyMax, 1.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Shield", "Energy regen/s", &m_energyRegenRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Shield", "Energy drain/s @ pressure 1", &m_energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Shield", "Reboot energy", &m_rebootEnergy, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Shield", "Damage absorb (energy per hp)", &m_damageAbsorb, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Shield", "Cover drain reduction", &m_coverDrainReduction, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Player", "Spawn grace (s)", &m_spawnGraceSec, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Materials max", &m_materialsMax, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Shield", "Damage radius", &m_damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Shield", "Push gain", &m_shieldPushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Shield", "Surface tension", &m_shieldTension, 0.0f, 10.0f, 0.05f);
}

void GamePlayer::spawn(const glm::vec3& pos)
{
    m_spawnPos = pos;
    m_health = m_healthMax;
    m_energy = m_energyMax;
    m_shieldCollapsed = false;
    m_graceTimer = m_spawnGraceSec;
    for (float& h : m_outputHistory)
        h = m_shieldMaxOutput;
    m_entity = Globals::world.spawnAssetFile("Entities/Game/player.pre", Transform(pos), true);
    if (m_entity)
    {
        m_entity->setName("Player");
        Globals::world.addRootEntity(m_entity);
    }
    m_query = Globals::forceSystem.createQuery(pos);
    m_jumpWasDown = true; // swallow a Space held through spawn
    setTeam(m_team);      // re-apply to the fresh capsule's emitter (prefab authors team 0)
}

void GamePlayer::setTeam(uint32 team)
{
    m_team = team;
    if (m_entity)
        if (ForceComponent* fc = getComponent<ForceComponent>(m_entity.get()))
            fc->emitter.setTeam(team);
}

void GamePlayer::clientAdopt(const glm::vec3& respawnPos)
{
    if (m_entity && getComponent<PhysicsComponent>(m_entity.get()))
        return; // already adopted and alive
    for (const EntityPtr& root : Globals::world.rootEntities())
    {
        NetworkComponent* net = getComponent<NetworkComponent>(root.get());
        if (!net || !net->state || net->authority() != ENetAuthority::LocalOwner
            || net->state->transferredOwnership)
            continue;
        if (!getComponent<PhysicsComponent>(root.get()) || !getComponent<ForceComponent>(root.get()))
            continue; // our player is the owned capsule WITH a shield
        m_entity = root;
        m_spawnPos = respawnPos;
        m_health = m_healthMax;
        m_energy = m_energyMax;
        m_shieldCollapsed = false;
        m_graceTimer = m_spawnGraceSec;
        for (float& h : m_outputHistory)
            h = m_shieldMaxOutput;
        if (!m_query.isValid())
            m_query = Globals::forceSystem.createQuery(respawnPos);
        setTeam(m_team); // the replicated prefab authors team 0 — re-team the local twin's field
        Log::info("Adopted our player capsule from the server");
        return;
    }
}

void GamePlayer::despawn()
{
    if (m_entity)
        Globals::world.removeRootEntity(m_entity.get());
    m_entity = EntityPtr();
    m_query = ForceQuery();
}

// Distance from the body center to the shape's lowest point, world units (grounds the jump
// raycast). Same derivation as the testbed's InputControls::shapeBottomDistance.
static float shapeBottomDistance(const Entity* entity, const PhysicsComponent& pc)
{
    const PhysicsComponent::SpawnInfo* si = getPhysicsSpawnInfo(entity);
    float d = 1.0f;
    if (si)
        switch (si->shape.type)
        {
        case EPhysicsShapeType::Box:     d = si->shape.halfExtents.y; break;
        case EPhysicsShapeType::Sphere:  d = si->shape.radius; break;
        case EPhysicsShapeType::Capsule: d = si->shape.halfHeight + si->shape.radius; break;
        default: break;
        }
    return d * pc.shapeScale;
}

void GamePlayer::tickMovement(const glm::vec3& cameraForwardPlanar, float deltaSec)
{
    Input& input = Globals::input;
    if (!input.isWindowHasFocus() || !Globals::ui.isViewportFocused())
        return;
    PhysicsComponent* pc = m_entity ? getComponent<PhysicsComponent>(m_entity.get()) : nullptr;
    if (!pc || pc->bodyType != EPhysicsBodyType::Dynamic || !pc->body.isValid())
        return;

    glm::vec3 forward = cameraForwardPlanar;
    const float forwardLen = glm::length(forward);
    forward = forwardLen > 1e-4f ? forward / forwardLen : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

    glm::vec3 move(0.0f);
    if (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_W)) move += forward;
    if (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_S)) move -= forward;
    if (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_D)) move += right;
    if (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_A)) move -= right;
    if (glm::dot(move, move) > 1e-8f)
        move = glm::normalize(move);
    const float speed = m_moveSpeed * (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT) ? m_sprintMult : 1.0f);

    glm::vec3 vel = pc->body.getLinearVelocity();
    glm::vec3 dv = glm::vec3(move.x * speed - vel.x, 0.0f, move.z * speed - vel.z);
    const float maxDv = m_accel * deltaSec;
    const float dvLen = glm::length(dv);
    if (dvLen > maxDv && dvLen > 1e-6f)
        dv *= maxDv / dvLen;
    vel.x += dv.x;
    vel.z += dv.z;

    const bool jumpDown = input.isKeyDown(SDL_Scancode::SDL_SCANCODE_SPACE);
    if (jumpDown && !m_jumpWasDown)
    {
        const float bottom = shapeBottomDistance(m_entity.get(), *pc);
        const glm::vec3 pos = pc->body.getPosition();
        if (Globals::physics.castRayClosest(pos, glm::vec3(0.0f, -(bottom + 0.3f), 0.0f), PhysicsLayers::All, &pc->body).hit)
            vel.y = m_jumpSpeed;
    }
    m_jumpWasDown = jumpDown;
    pc->body.setLinearVelocity(vel);
}

void GamePlayer::applyDamage(float amount)
{
    if (m_graceTimer > 0.0f || amount <= 0.0f)
        return;
    // The battery eats the hit FIRST: a live shield converts damage into energy at "Damage absorb"
    // energy per hp, and only what the battery cannot pay reaches health. An emptied battery
    // collapses right here (same latch tickShieldAndHealth uses) and flushes the co-op mirror, so
    // the bubble drops the moment a swarm chews through it instead of at the next tick.
    if (!m_shieldCollapsed && m_damageAbsorb > 0.0f && m_energy > 0.0f)
    {
        const float absorbedHp = glm::min(amount, m_energy / m_damageAbsorb);
        m_energy = glm::max(m_energy - absorbedHp * m_damageAbsorb, 0.0f);
        amount -= absorbedHp;
        if (m_energy <= 0.0f)
            m_shieldCollapsed = true;
    }
    if (amount > 0.0f)
        m_health = glm::max(m_health - amount, 0.0f);
}

void GamePlayer::tickShieldAndHealth(float deltaSec)
{
    PhysicsComponent* pc = m_entity ? getComponent<PhysicsComponent>(m_entity.get()) : nullptr;
    ForceComponent* fc = m_entity ? getComponent<ForceComponent>(m_entity.get()) : nullptr;
    if (!pc || !pc->body.isValid() || !fc)
        return;

    const float pressure = fc->emitter.getPressure();
    m_lastPressure = pressure;

    const glm::vec3 bodyPos = pc->body.getPosition();
    m_query.setPosition(bodyPos);
    const ForceQuery::Result territory = m_query.getResult();
    const float density = territory.density() / glm::max(fc->emitter.getCenterDensityFactor(), 1e-3f);
    m_lastDensity = density;

    const float currentOutput = m_shieldCollapsed ? 0.01f : m_shieldMaxOutput;
    const float coverSurplus = territory.valid && territory.inside && territory.owningTeam == (int)m_team
        ? glm::max(0.0f, density - currentOutput) : 0.0f;
    float drainMult = glm::clamp(1.0f - coverSurplus * m_coverDrainReduction, 0.0f, 1.0f);

    const bool inGrace = m_graceTimer > 0.0f;
    if (inGrace)
    {
        m_graceTimer -= deltaSec;
        drainMult = 0.0f;
    }
    // Surface tension: contact stiffens superlinearly with pressure — the same factor scales the
    // push-out below, so shoving deep into a bubble both resists harder and drains faster.
    const float tension = 1.0f + m_shieldTension * pressure;
    m_energy = glm::clamp(m_energy - pressure * tension * m_energyDrainRate * drainMult * deltaSec
        + m_energyRegenRate * deltaSec, 0.0f, m_energyMax);
    if (m_energy <= 0.0f)
        m_shieldCollapsed = true;
    else if (m_shieldCollapsed && m_energy >= glm::min(m_rebootEnergy, m_energyMax))
        m_shieldCollapsed = false;
    fc->emitter.setOutput(m_shieldCollapsed ? 0.01f : m_shieldMaxOutput);

    // Physical push-out, INDEPENDENT of the shield state: getAppliedForce scales with the
    // emitter's own output, so it is normalized by the output that produced the readback (the
    // oldest history entry, matching the ~2-frame latency — dividing by the CURRENT output would
    // spike ~150x on the collapse frame). A collapsed shield pushes exactly like a full one.
    const glm::vec3 force = fc->emitter.getAppliedForce() / glm::max(m_outputHistory[0], 1e-3f);
    if (glm::dot(force, force) > 1e-8f)
        pc->body.applyImpulse(force * deltaSec * m_shieldPushGain * pressure * tension * drainMult);
    m_outputHistory[0] = m_outputHistory[1];
    m_outputHistory[1] = m_outputHistory[2];
    m_outputHistory[2] = m_shieldCollapsed ? 0.01f : m_shieldMaxOutput;

    const float iso = Globals::forceSystem.getParams().isoThreshold;
    // Exposure scales with how far the shield squished below "Damage radius": 1% under = 1% of
    // the drain rate, fully collapsed (radius 0) = the full rate — grazing contact only stings.
    const float exposure = glm::clamp(1.0f - shieldRadius() / glm::max(m_damageRadius, 1e-3f), 0.0f, 1.0f);
    if (!inGrace && coverSurplus <= 0.0f && exposure > 0.0f && pressure > iso)
        m_health -= m_healthDrainRate * exposure * deltaSec;
    if (bodyPos.y < -20.0f)
        m_health = 0.0f; // fell out of the world — respawn below

    if (m_health <= 0.0f)
    {
        // Death: hard respawn at the start position. Teleport contract: stomp the interpolation
        // poses too, or the render mix runs backward on stepping frames.
        Log::info("Player down — respawning");
        Globals::physics.teleportBody(pc->body, m_spawnPos, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        pc->body.setLinearVelocity(glm::vec3(0.0f));
        pc->prevPos = pc->currPos = m_spawnPos;
        pc->prevRot = pc->currRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        pc->lastStep = Globals::physics.getStepCount();
        m_health = m_healthMax;
        m_energy = m_energyMax;
        m_materials = 0.0f; // death drops the carried construction stock
        m_shieldCollapsed = false;
        m_graceTimer = m_spawnGraceSec;
    }
    // Publish into the capsule's PUPPET GameUnitComponent — the state's network surface. The
    // entity snapshot/claim game blob packs from and applies to it (change-detected server-side,
    // so a collapse edge flushes on the very next snapshot tick). Materials flow the OTHER way on
    // a client: server-authoritative, snapshot-applied to the component, read back here.
    if (GameUnitComponent* unit = getComponent<GameUnitComponent>(m_entity.get()))
    {
        // Damage other actors banked on our puppet inbox (unit melee, enemy projectiles — the
        // same damage() call every victim gets) applies through the shield-absorb rules.
        applyDamage(unit->takePendingDamage());
        unit->healthMax = m_healthMax;
        unit->health = m_health;
        unit->energyMax = m_energyMax;
        unit->energy = m_energy;
        unit->collapsed = m_shieldCollapsed;
        unit->team = uint8(m_team);
        const NetworkComponent* net = getComponent<NetworkComponent>(m_entity.get());
        if (net && net->authority() == ENetAuthority::LocalOwner)
            m_materials = glm::clamp(unit->materialsFrac, 0.0f, 1.0f) * m_materialsMax;
        else
            unit->materialsFrac = m_materialsMax > 0.0f ? m_materials / m_materialsMax : 0.0f;
    }
}

float GamePlayer::shieldRadius() const
{
    // Pressure-aware equilibrium estimate from the Force library (average radius, ~2 frames latent).
    const ForceComponent* fc = m_entity ? getComponent<ForceComponent>(m_entity.get()) : nullptr;
    return fc ? fc->emitter.getEquilibriumRadius() : 0.0f;
}

glm::vec3 GamePlayer::interpolatedPos() const
{
    const PhysicsComponent* pc = m_entity ? getComponent<PhysicsComponent>(m_entity.get()) : nullptr;
    if (!pc || !pc->body.isValid())
        return m_spawnPos;
    return glm::mix(pc->prevPos, pc->currPos, Globals::physics.getInterpolationAlpha());
}

glm::vec3 GamePlayer::bodyPos() const
{
    const PhysicsComponent* pc = m_entity ? getComponent<PhysicsComponent>(m_entity.get()) : nullptr;
    return pc && pc->body.isValid() ? pc->body.getPosition() : m_spawnPos;
}
