module Entity;

import Core;
import Core.Log;
import Core.glm;
import Core.Transform;
import Physics;

static float quatAngleDeg(const glm::quat& a, const glm::quat& b)
{
    const float d = glm::min(1.0f, glm::abs(glm::dot(a, b)));
    return glm::degrees(2.0f * glm::acos(d));
}

void NetworkComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    // sync state exists only inside a session; allocated BEFORE registering — the registration path
    // (id adoption, setOwner in the spawn's frame) already writes through it
    if (Globals::networkManager.role() != ENetRole::None)
        state = std::make_unique<NetEntityState>(Globals::networkManager.role() == ENetRole::Server);
    // the manager is the only id authority: on the server this mints an id and replicates the spawn
    // to clients; on a client it adopts the incoming server id while executing a replicated Spawn;
    // everywhere else (client-local content, single player) it returns 0 = local-inert, never synced
    netId = Globals::networkManager.registerEntity(entity, this);
    if (netId == 0)
        state.reset(); // local-inert: the manager will never touch this component
}

void NetworkComponent::destroy(Entity& entity, const SpawnInfo&)
{
    Globals::networkManager.unregisterEntity(netId, this);
}

ENetAuthority NetworkComponent::authority() const
{
    if (netId == 0)
        return ENetAuthority::Local;
    if (ownerClientId == 0)
        return ENetAuthority::ServerOwned;
    return ownerClientId == Globals::networkManager.localClientId() ? ENetAuthority::LocalOwner
                                                                    : ENetAuthority::RemoteOwner;
}

void NetworkComponent::update(Entity& entity, float deltaSeconds)
{
    if (!state || Globals::networkManager.role() != ENetRole::Client || !state->client.hasTarget)
        return;
    NetEntityState::ClientState& net = state->client; // role checked above: the client member is the active one

    // LOCAL OWNER: this process's claims are the authority — simulate freely and ignore the server's
    // corrections UNLESS the record is Forced (rejected claims: the hard resync below) or Arbitrated
    // (player-vs-player contact: the server solver owns the pose, soft-correct toward it while the
    // player keeps steering — input stays live, see updatePlayerControl which yields only on Forced)
    if (authority() == ENetAuthority::LocalOwner && !(net.targetFlags & (NetRecFlag_Forced | NetRecFlag_Arbitrated)))
        return;

    net.timeSinceSnapshot += deltaSeconds;
    const NetSyncParams& params = Globals::networkManager.params();
    const bool newSnapshot = net.serverTick != net.lastAppliedTick;

    PhysicsComponent* physics = getComponent<PhysicsComponent>(&entity);
    if (physics && physics->bodyType == EPhysicsBodyType::Dynamic && physics->body.isValid())
    {
        // The BODY owns the pose (PhysicsComponent::update rewrites entity.pos/rot from it right after
        // this), so corrections target the body through the thread-safe command queue — this runs on a
        // job worker inside the parallel entity pass, and even a direct velocity setter would wake the
        // body, mutating box3d's shared solver sets.
        if (!(net.targetFlags & NetRecFlag_Physics))
            return; // the server's twin has no live dynamic body (suspended there?) — don't fight it
        PhysicsWorld& physicsWorld = Globals::physics;

        // FORCED owner (claims rejected): hard-resync rather than ride the correction ladder — the
        // next claim then matches the twin, the re-anchor accepts it, and Forced clears within about
        // a tick. An ARBITRATED-only record falls through to the soft push so steering stays live.
        if (authority() == ENetAuthority::LocalOwner && (net.targetFlags & NetRecFlag_Forced))
        {
            physicsWorld.teleportBody(physics->body, net.targetPos, net.targetRot);
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, net.targetLinVel);
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, net.targetAngVel);
            physics->prevPos = physics->currPos = net.targetPos; // see the snap-path comment below
            physics->prevRot = physics->currRot = net.targetRot;
            physics->lastStep = Globals::physics.getStepCount(); // see the playback path
            if (newSnapshot)
                net.lastAppliedTick = net.serverTick;
            return;
        }

        const glm::vec3 bodyPos = physics->body.getPosition();
        const glm::quat bodyRot = physics->body.getRotation();

        // INTERACTION GRACE: corrections on a body we are actively shoving would drag it toward the
        // server's RTT-old pre-push state; the twin gets the same push an RTT later, so the residue
        // reconciles once the grace lingers out. STRICTLY ServerOwned — a RemoteOwner entity is
        // driven by ITS owner's claims, so suspending its corrections only fabricates false local
        // control over another player's cube.
        // The SERVER's player is server-owned by id but is an actively-steered body: grace-freeing
        // it for local shoves only fabricates divergence (its server sim resists the push) — it
        // follows the interp playback below instead, like every other remote player.
        const bool serverPlayer = (net.targetFlags & NetRecFlag_ServerPlayer) != 0;
        if (params.interactionRadius > 0.0f && authority() == ENetAuthority::ServerOwned && !serverPlayer)
        {
            const float radiusSq = params.interactionRadius * params.interactionRadius;
            for (const glm::vec3& ownedPos : Globals::networkManager.localOwnedBodyPositions())
                if (glm::dot(ownedPos - bodyPos, ownedPos - bodyPos) < radiusSq)
                {
                    net.localInteractionGrace = params.interactionLinger;
                    break;
                }
        }
        if (net.localInteractionGrace > 0.0f && authority() == ENetAuthority::ServerOwned && !serverPlayer)
        {
            net.localInteractionGrace -= deltaSeconds;
            // deliberately does NOT touch lastAppliedTick: when the grace ends, the newest
            // snapshot's velocities/sleep apply once and normal correction resumes
            return;
        }

        if (net.targetFlags & NetRecFlag_Asleep)
        {
            if (!newSnapshot)
                return;
            net.lastAppliedTick = net.serverTick;
            // hard-sync to the server's rest pose, then sleep our body too (asleep records only
            // refresh on keyframes, so this path is rare)
            if (glm::length(net.targetPos - bodyPos) > params.posDeadzone || quatAngleDeg(bodyRot, net.targetRot) > params.rotDeadzoneDeg)
            {
                physicsWorld.teleportBody(physics->body, net.targetPos, net.targetRot);
                physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, glm::vec3(0.0f));
                physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, glm::vec3(0.0f));
                physics->prevPos = physics->currPos = net.targetPos;
                physics->prevRot = physics->currRot = net.targetRot;
                physics->lastStep = Globals::physics.getStepCount(); // see the playback path
            }
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAwake, glm::vec3(0.0f));
            return;
        }

        // REMOTE PLAYERS: replay the owner's recorded trajectory a couple of ticks behind instead
        // of chasing the newest state — a capped-accel body following a delayed, input-spiky signal
        // lag-chases starts and overshoots stops, and no target tuning fixes that. A loss gap at the
        // cursor falls through to the push for that frame.
        if ((authority() == ENetAuthority::RemoteOwner || serverPlayer)
            && net.remoteBuffer != nullptr && net.remoteBuffer->count >= 2)
        {
            const NetSnapshotRing& ring = *net.remoteBuffer;
            const float newest = float(ring.newestTick);
            const float oldestValid = newest - float(glm::min(ring.count, NetSnapshotRing::Capacity) - 1);
            // (re)engage at the PRESENT when the cursor sits outside the buffered window (first frames
            // after an ownership transfer, or after a long loss burst) — the anchor below then slides
            // it back into the full delay over a few hundred ms instead of snapping the entity into
            // the past, which read as a jump the moment a player started pushing a prop
            if (net.playbackTick < oldestValid || net.playbackTick > newest)
                net.playbackTick = glm::max(newest - 1.0f, oldestValid);
            net.playbackTick += Globals::networkManager.serverSnapshotHz() * deltaSeconds; // advance in server tick units
            const float targetPlayback = newest - float(params.remoteInterpTicks);
            net.playbackTick += (targetPlayback - net.playbackTick) * glm::min(1.0f, deltaSeconds * 4.0f); // gentle drift re-anchor
            // cap at newest-1: a bracketing NEXT snapshot always exists there. Clamping to newest let
            // ordinary snapshot-arrival jitter break the bracket and fall through to the chase for a
            // frame — the source of the residual stop-overshoot flicker. Snapshots stalling now pause
            // the playback instead of chasing.
            net.playbackTick = glm::clamp(net.playbackTick, oldestValid, glm::max(newest - 1.0f, oldestValid));
            const uint32 tick0 = uint32(glm::max(0.0f, glm::floor(net.playbackTick)));
            const NetSnapshotRecord& r0 = ring.records[tick0 % NetSnapshotRing::Capacity];
            const NetSnapshotRecord& r1 = ring.records[(tick0 + 1) % NetSnapshotRing::Capacity];
            if (r0.tick == tick0 && r1.tick == tick0 + 1) // both bracketing ticks present (ring-wrap + loss validation)
            {
                const float alpha = glm::clamp(net.playbackTick - float(tick0), 0.0f, 1.0f);
                const glm::vec3 pos = glm::mix(r0.pos, r1.pos, alpha);
                const glm::quat rot = glm::slerp(r0.rot, r1.rot, alpha);
                physicsWorld.teleportBody(physics->body, pos, rot);
                physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, glm::mix(r0.linVel, r1.linVel, alpha));
                physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, glm::mix(r0.angVel, r1.angVel, alpha));
                physics->prevPos = physics->currPos = pos;
                physics->prevRot = physics->currRot = rot;
                // Claim the step: PhysicsComponent::update runs next and would otherwise overwrite
                // curr with body.getPosition(), which still holds LAST frame's teleport (they apply
                // at the next physics.update) — rendering backward on stepping frames only.
                physics->lastStep = Globals::physics.getStepCount();
                if (newSnapshot)
                    net.lastAppliedTick = net.serverTick;
                return;
            }
        }

        glm::vec3 target = net.targetPos;
        if (params.extrapolate)
            target += net.targetLinVel * glm::min(net.timeSinceSnapshot, 0.25f); // dead-reckon between snapshots, capped

        const glm::vec3 posErrVec = target - bodyPos;
        const float posErr = glm::length(posErrVec);
        const float rotErrDeg = quatAngleDeg(bodyRot, net.targetRot);

        // The non-physical teleport resync is the LAST resort: only an absurd position error reaches
        // it (the catch-up push below handles everything closer — teleporting into a space another
        // desynced body still occupies would depenetration-fling both apart into fresh desync).
        if (posErr > params.posTeleportThreshold)
        {
            physicsWorld.teleportBody(physics->body, target, net.targetRot);
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, net.targetLinVel);
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, net.targetAngVel);
            // The teleport applies at the NEXT physics.update and prev/curr only refresh on step
            // boundaries — stomping them makes PhysicsComponent::update (right after this, same
            // entity) show the corrected pose this frame instead of lerping from stale state.
            physics->prevPos = physics->currPos = target;
            physics->prevRot = physics->currRot = net.targetRot;
            physics->lastStep = Globals::physics.getStepCount(); // see the playback path
            if (newSnapshot)
                net.lastAppliedTick = net.serverTick;
            return;
        }

        // PHYSICAL PUSH: error becomes corrective velocity on top of the server's, as a bounded
        // impulse — so contacts, stacking and gameplay impulses compose with the correction instead
        // of being erased. Past the snap threshold it enters CATCH-UP (boosted gains/caps).
        // ARBITRATED owner: correct only REAL divergence (big deadzone, halved gains) or the
        // correction drags against the player's live input.
        const bool arbitratedOwner = authority() == ENetAuthority::LocalOwner;
        const float posDeadzone = arbitratedOwner ? glm::max(params.posDeadzone, params.arbitrateDeadzone) : params.posDeadzone;
        const float rotDeadzoneDeg = arbitratedOwner ? glm::max(params.rotDeadzoneDeg, 15.0f) : params.rotDeadzoneDeg;
        const float ownerGainScale = arbitratedOwner ? 0.5f : 1.0f;
        const bool pushPos = posErr > posDeadzone;
        const bool pushRot = rotErrDeg > rotDeadzoneDeg;
        if (pushPos || pushRot)
        {
            const bool catchUp = posErr > params.posSnapThreshold || rotErrDeg > params.rotSnapThresholdDeg;
            const float boost = (catchUp ? glm::max(1.0f, params.pushCatchUpBoost) : 1.0f) * ownerGainScale;
            glm::vec3 desiredLin = net.targetLinVel;
            if (pushPos)
            {
                glm::vec3 corrective = posErrVec * (params.pushPosGain * boost);
                const float maxVel = params.pushMaxVel * boost;
                const float len = glm::length(corrective);
                if (len > maxVel && len > 1e-6f)
                    corrective *= maxVel / len;
                desiredLin += corrective;
            }
            glm::vec3 desiredAng = net.targetAngVel;
            if (pushRot)
            {
                glm::quat errQuat = net.targetRot * glm::inverse(bodyRot);
                if (errQuat.w < 0.0f)
                    errQuat = -errQuat; // shortest arc
                glm::vec3 corrective = glm::axis(errQuat) * glm::angle(errQuat) * (params.pushRotGain * boost);
                const float maxAngVel = params.pushMaxAngVel * boost;
                const float len = glm::length(corrective);
                if (len > maxAngVel && len > 1e-6f)
                    corrective *= maxAngVel / len;
                desiredAng += corrective;
            }
            physicsWorld.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::NudgeVelocity,
                desiredLin, desiredAng,
                glm::vec3(params.pushMaxAccel * boost * deltaSeconds, params.pushMaxAngAccel * boost * deltaSeconds, 0.0f));
        }
        if (newSnapshot)
            net.lastAppliedTick = net.serverTick;
        return;
    }

    // non-physics (or kinematic/static body: the entity transform syncs, the collider stays put —
    // teleportBody is the engine-wide rule for moving colliders); targets are entity-LOCAL here
    if (authority() == ENetAuthority::LocalOwner && (net.targetFlags & NetRecFlag_Forced))
    {
        // rejected owner: same crisp hard-resync as the physics path above
        entity.pos = net.targetPos;
        entity.rot = net.targetRot;
        if (newSnapshot)
            net.lastAppliedTick = net.serverTick;
        return;
    }
    const float posErr = glm::length(net.targetPos - entity.pos);
    const float rotErrDeg = quatAngleDeg(entity.rot, net.targetRot);

    if (posErr > params.posSnapThreshold || rotErrDeg > params.rotSnapThresholdDeg)
    {
        entity.pos = net.targetPos;
        entity.rot = net.targetRot;
        return;
    }
    // exponential blend toward the target; inside the deadzone the local sim free-runs
    const float blend = 1.0f - glm::exp(-params.blendRate * deltaSeconds);
    if (posErr > params.posDeadzone)
        entity.pos = glm::mix(entity.pos, net.targetPos, blend);
    if (rotErrDeg > params.rotDeadzoneDeg)
        entity.rot = glm::slerp(entity.rot, net.targetRot, blend);
}

const NetworkComponent::SpawnInfo* getNetworkSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<NetworkComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Network); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const NetworkComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeNetworkSpawnInfo(const NetworkComponent::SpawnInfo& info, AssetNode& out)
{
    // nothing to serialize: the component is pure presence (ids are code-assigned) — Prefab.cpp
    // special-cases Network in its empty-component skip, like Scene
}
