module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Time;
import Core.Transform;
import Core.Camera;
import Core.Frustum;
import RendererVK;
import Entity;
import Physics;
import Force;
import Spatial;
import Nav;
import Threading;
import :Npc;
import :Structures;

// Per-type PREFABS: the stats live in each prefab's Component GameUnit block.
static constexpr const char* c_npcPrefabs[(int)ENpcType::Count] = {
    "Entities/Game/enemyGrunt.pre", "Entities/Game/enemyBrute.pre",
    "Entities/Game/enemyRunner.pre", "Entities/Game/enemySpitter.pre",
    "Entities/Game/enemySwarm.pre", "Entities/Game/enemyElite.pre",
    "Entities/Game/enemyGiant.pre", "Entities/Game/enemyTitan.pre",
    "Entities/Game/enemyLobber.pre", "Entities/Game/enemySpawner.pre", "Entities/Game/enemyWarrior.pre" };
static constexpr const char* c_npcNames[(int)ENpcType::Count] = { "Enemy", "Brute", "Runner", "Spitter", "Swarm",
    "Elite", "Giant", "Titan", "Lobber", "Spawner", "Warrior" };

// Units inside the view frustum, for the overhead labels — anything off screen would be projected
// and thrown away, so it is never fetched. The hits are filtered straight out of the traversal
// into `out` (no intermediate buffer: the labels job may park on another thread).
void NpcSystem::queryVisibleUnits(const Camera& camera, oc::vector<Entity*>& out)
{
    ProfileScope scope("Npc visible units (query)", EProfileCategory::Game);
    out.clear();
    const glm::dvec3 cameraPos(camera.position);
    // The renderer's current view-projection is the frustum this frame is drawn with; the spatial
    // index wants it camera-relative (exact at any world scale).
    const Frustum world(Globals::rendererVK.getCenterViewProj());
    Globals::spatialIndex.forEachInFrustum(rebaseFrustum(world, cameraPos), cameraPos, FLT_MAX,
        SpatialLayer_Render, [&](uint64 user)
    {
        Entity* entity = reinterpret_cast<Entity*>(user);
        if (hasComponent<GameUnitComponent>(entity))
            out.push_back(entity);
    });
}

// Every unit in the world — for save/load (which must persist units the camera cannot see) and
// the profiling scenario's select-all. A roster walk, not a query.
void NpcSystem::queryAllUnits(oc::vector<Entity*>& out) const
{
    out.clear();
    for (const EntityPtr& u : m_units)
        out.push_back(u.get());
}

void NpcSystem::onWorldRootRemoved(const Entity* entity)
{
    const auto drop = [entity](oc::vector<EntityPtr>& roster)
    {
        for (size_t i = 0; i < roster.size(); ++i)
            if (roster[i].get() == entity)
            {
                roster[i] = oc::move(roster.back());
                roster.pop_back();
                return true;
            }
        return false;
    };
    if (!drop(m_units))
        drop(m_shots);
}

void NpcSystem::registerTweaks()
{
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    // The shared unit-sim baseline (GameUnitComponent::params — every unit of every team).
    GameUnitParams& up = GameUnitComponent::params;
    Tweak::floatVar("Game/Enemies", "Unit energy drain/s @ pressure 1", &up.energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Push tension", &up.tension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field damage/s", &up.fieldDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Field damage mult", &up.fieldDpsMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push starts (x iso)", &up.fieldPushStart, 0.0f, 0.95f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain mult", &up.emitterDrainMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain range (m)", &up.strainRange, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Damage absorb (energy per hp)", &up.damageAbsorb, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Unit damage radius", &up.damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &up.pushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Enemies", "Retarget interval", &up.retargetInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Target search radius", &up.targetSearchRadius, 5.0f, 400.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Route engage radius", &up.routeEngageRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Order break radius", &up.orderBreakRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Unit max speed (m/s)", &up.maxSpeed, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Wander speed mult", &up.wanderSpeedMult, 0.05f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Enemies", "Wander speed max (m/s)", &up.wanderSpeedMax, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Target track radius", &up.targetTrackRadius, 0.0f, 400.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Nav follow radius", &up.navFollowRadius, 0.0f, 400.0f, 1.0f);
    Tweak::boolean("Game/Enemies", "Nav fields", &up.navEnabled);
    // Shared by units AND player capsules (GamePlayer reads the same param); a prefab's
    // `HeightLimit` overrides it (< 0 = no ceiling, for flying units).
    Tweak::floatVar("Game/Actors", "Height limit (m)", &up.heightLimit, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Goal", &up.steerGoal, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Flow", &up.steerFlow, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Flow splat gain", &up.flowSplatGain, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Persist", &up.steerPersist, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Track goal", &up.steerTrackGoal, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Track flow mult", &up.trackFlowMult, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Pressure", &up.steerPressure, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Pressure knee", &up.pressureKnee, 0.01f, 3.0f, 0.01f);
    Tweak::floatVar("Game/Enemies/Steer", "Flow knee", &up.flowKnee, 0.01f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Enemies/Steer", "Presence pressure", &up.presencePressure, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Game/Enemies/Steer", "Look-ahead", &up.steerLook, 2.0f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Wall push", &up.steerWall, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Wall keep (m)", &up.wallKeep, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Corner clip penalty", &up.steerCornerClip, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies/Steer", "Stuck pressure", &up.stuckPressure, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Enemies/Steer", "Unstick after (s)", &up.unstickAfter, 0.5f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Enemies/Steer", "Seed request interval (s)", &up.seedRequestInterval, 0.5f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Order lane speed", &m_orderLaneSpeed, 0.0f, 20.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Stuck lane speed", &m_stuckLaneSpeed, 0.0f, 20.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Lane width (m)", &m_laneWidth, 0.0f, 12.0f, 0.5f);
    Tweak::floatVar("Game/Enemies/Steer", "Order flow blind (s)", &up.orderFlowBlind, 0.0f, 10.0f, 0.1f);
    // Shot speeds, applied when the fire queues are serviced here (the rest of the production
    // tuning registers from StructureSystem onto the component params).
    Tweak::floatVar("Game/Friendlies", "Turret beam lifetime", &m_beamLifetime, 0.02f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Enemies", "Lobber shot speed", &m_lobberShotSpeed, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spitter shot speed", &m_spitterShotSpeed, 2.0f, 80.0f, 0.5f);
    // Far tick: neither Saved nor Synced, like the rest of Game/Sim LOD (explicit flags beat the
    // scoped ones above).
    Tweak::floatVar("Game/Sim LOD", "Far tick interval (s)", &m_farInterval, 0.05f, 5.0f, 0.05f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Sim LOD/Stats", "Far ticked", &m_farTicked, 0, 1 << 20, 0.0f, {}, ETweakFlags::None);
}

void NpcSystem::clear()
{
    // Teardown: despawn every actor we spawned — the rosters ARE the world-wide answer, no query.
    // Deregister first (move-out), so removeRootEntity's onWorldRootRemoved callback no-ops
    // instead of mutating the roster under the loop. Puppets are never in the rosters.
    const auto despawnAll = [](oc::vector<EntityPtr>& roster)
    {
        oc::vector<EntityPtr> actors = oc::move(roster);
        roster.clear();
        for (const EntityPtr& e : actors)
            Globals::world.removeRootEntity(e.get());
        Globals::world.releaseBatch(oc::move(actors)); // the teardown itself fans out over the job system
    };
    despawnAll(m_units);
    despawnAll(m_shots);
    // Drop any queued reports/requests the removed actors left behind, so stale deaths cannot
    // decrement (or stale requests spawn into) a world that has been reset (load/teardown paths).
    GameUnitComponent::takeFireRequests(m_fireScratch);
    GameUnitComponent::takeDeaths(m_deathScratch);
    GameUnitComponent::takeSeedRequests(m_seedScratch);
    GameStructureComponent::takeSpawnRequests(m_spawnScratch);
    GameStructureComponent::takeTurretFireRequests(m_turretFireScratch);
    m_seedScratch.clear();
    m_fireScratch.clear();
    m_deathScratch.clear();
    m_spawnScratch.clear();
    m_turretFireScratch.clear();
}

static uint32 packColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

// A unit spawn point on a ring around a building, on the first of 8 probed angles whose cell is
// free of structures (and inside the arena bounds) — units must never spawn INSIDE the building.
// With a preferred direction (the barracks' first waypoint) the probe starts THERE and fans out
// to both sides (+45, -45, +90, ...), so the unit spawns facing its route; without one the start
// angle is random.
static glm::vec3 freeSpawnPointAround(const StructureSystem& structures, const glm::vec3& center,
    float ringRadius, const glm::vec2* preferDirXZ = nullptr)
{
    const bool directed = preferDirXZ && glm::dot(*preferDirXZ, *preferDirXZ) > 1e-6f;
    const float start = directed ? std::atan2(preferDirXZ->y, preferDirXZ->x)
                                 : glm::linearRand(0.0f, glm::two_pi<float>());
    for (int k = 0; k < 8; ++k)
    {
        // k -> 0, +1, -1, +2, -2, +3, -3, +4 steps of 45 degrees from the start angle
        const int step = (k + 1) / 2 * ((k & 1) ? 1 : -1);
        const float a = start + (float)step * glm::two_pi<float>() / 8.0f;
        const glm::vec3 p(center.x + std::cos(a) * ringRadius, 1.0f, center.z + std::sin(a) * ringRadius);
        if (structures.cellsFree(EStructureType::Emitter, p,
            glm::quat(1.0f, 0.0f, 0.0f, 0.0f), /*ignoreCables*/ true)) // 1x1 probe; cables are walk-through
            return p;
    }
    return glm::vec3(center.x + std::cos(start) * ringRadius, 1.0f, center.z + std::sin(start) * ringRadius);
}

void NpcSystem::addBeam(const glm::vec3& from, const glm::vec3& to)
{
    m_beams.push_back({ from, to, m_beamLifetime });
    m_newBeams.push_back({ from, to, m_beamLifetime });
}

void NpcSystem::drawBeams(float deltaSec)
{
    // A BUNDLE of jagged lines per strike (re-jittered every frame = flicker), fading over the
    // lifetime: a bright dense CORE of tightly packed strands plus wider, dimmer forks around it
    // — debug lines are 1 px, so thickness comes from count.
    constexpr int c_segments = 8;
    constexpr int c_coreStrands = 6;  // spread 0.12 m: reads as one thick bolt
    constexpr int c_forkStrands = 4;  // spread 0.9 m: the crackle around it
    for (Beam& b : m_beams)
    {
        b.ttl -= deltaSec;
        const float fade = glm::clamp(b.ttl / glm::max(m_beamLifetime, 1e-3f), 0.0f, 1.0f);
        const glm::vec3 axis = b.to - b.from;
        glm::vec3 side = glm::cross(axis, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::dot(side, side) < 1e-4f)
            side = glm::vec3(1.0f, 0.0f, 0.0f);
        side = glm::normalize(side);
        const glm::vec3 up = glm::normalize(glm::cross(side, glm::normalize(axis)));
        const auto strand = [&](float spread, const glm::vec3& tint)
        {
            const uint32 color = packColor(tint * (0.35f + 0.65f * fade));
            glm::vec3 prev = b.from;
            for (int i = 1; i <= c_segments; ++i)
            {
                const float t = (float)i / c_segments;
                glm::vec3 p = b.from + axis * t;
                if (i < c_segments) // the ends stay anchored on the muzzle and the victim
                    p += side * glm::linearRand(-spread, spread) + up * glm::linearRand(-spread, spread);
                Globals::rendererVK.addDebugLine(prev, p, color);
                prev = p;
            }
        };
        for (int k = 0; k < c_coreStrands; ++k)
            strand(0.12f, glm::vec3(0.8f, 0.95f, 1.0f));
        for (int k = 0; k < c_forkStrands; ++k)
            strand(0.9f, glm::vec3(0.45f, 0.75f, 1.0f));
    }
    oc::erase_if(m_beams, [](const Beam& b) { return b.ttl <= 0.0f; });
}

static float barracksSpawnRadius()
{
    return StructureSystem::footprintCellsOf(EStructureType::Barracks)
        * StructureSystem::GridCellSize * 0.5f + 1.5f;
}

Entity* NpcSystem::spawnUnit(const StructureSystem& structures, const glm::vec3& pos,
    uint32 sourceId, uint8 team, ENpcType type)
{
    EntityPtr entity = Globals::world.spawnAssetFile(c_npcPrefabs[(int)type], Transform(pos), true);
    if (!entity)
        return nullptr;
    GameUnitComponent* unit = getComponent<GameUnitComponent>(entity.get());
    if (!unit)
        return nullptr; // the prefab must carry Component GameUnit
    entity->setName(c_npcNames[(int)type]);
    Globals::world.addRootEntity(entity);
    unit->team = team;
    GameUnitComponent::applyTeamTint(*entity); // own-team units read green
    unit->sourceId = sourceId;
    unit->popCost = (uint8)glm::clamp(structures.unitPopulation((int)type), 0, 255);
    if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 1 — units carry their builder's team
    // Copy the barracks route in AT SPAWN (orders tier): the unit marches it before its AI
    // (still engaging enemy units that come within "Route engage radius" on the way).
    if (const int source = structures.structureIndexById(sourceId); source >= 0)
    {
        const oc::span<const glm::vec3> route = structures.structureRoute(source);
        unit->routeCount = (uint8)glm::min(route.size(), (size_t)GameUnitComponent::MaxRoutePoints);
        for (int i = 0; i < unit->routeCount; ++i)
            unit->route[i] = route[i];
        unit->routeIndex = 0;
    }
    m_units.push_back(entity); // roster: deregistered by onWorldRootRemoved on any despawn path
    return entity.get();
}

Entity* NpcSystem::spawnLooseUnit(const StructureSystem& structures, const glm::vec3& pos,
    uint8 team, ENpcType type)
{
    return spawnUnit(structures, pos, /*sourceId*/ 0, team, type);
}

void NpcSystem::spawnLooseUnits(oc::span<const LooseSpawn> spawns)
{
    if (spawns.empty())
        return;
    // The entity creations fan out over the job system; everything below the batch is the same
    // per-unit fixup spawnUnit does, minus the route copy (loose units have no owning barracks) —
    // cheap component writes, kept serial on main.
    oc::vector<World::SpawnRequest> requests;
    requests.reserve(spawns.size());
    for (const LooseSpawn& s : spawns)
        requests.push_back({ c_npcPrefabs[(int)s.type], Transform(s.pos) });
    oc::vector<EntityPtr> spawned = Globals::world.spawnBatch(requests, /*addRoots*/ false);
    for (size_t i = 0; i < spawned.size(); ++i)
    {
        EntityPtr& entity = spawned[i];
        if (!entity)
            continue;
        GameUnitComponent* unit = getComponent<GameUnitComponent>(entity.get());
        if (!unit)
            continue; // the prefab must carry Component GameUnit; the stray dies with the batch
        const LooseSpawn& s = spawns[i];
        entity->setName(c_npcNames[(int)s.type]);
        unit->team = s.team;
        GameUnitComponent::applyTeamTint(*entity);
        unit->sourceId = 0;
        if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
            fc->emitter.setTeam(s.team); // prefabs author team 1 — units carry their spawner's team
        if (s.hasOrder)
            unit->orderMove(s.orderDest);
        // PARKED AT SPAWN while the SIM LOD selects: loose units (waves, ambient) spawn far from
        // every player, in blobs that overlap — a live body there took box3d's push-out and, never
        // steered (unselected) and frictionless, coasted away. Disabled from the first step, the
        // far tick walks them by teleport and the World's wake edge enables them (velocities
        // zeroed) once a player is near. The queue applies before the next step, so the body
        // never simulates a single step here. (The bubble needs nothing: it spawns dark and the
        // World's gate switches it on only once the unit has a tier close enough.)
        if (Globals::world.simLodActive())
            if (PhysicsComponent* pc = getComponent<PhysicsComponent>(entity.get()))
                pc->park(/*disable*/ true);
        m_units.push_back(entity); // roster: deregistered by onWorldRootRemoved on any despawn path
        Globals::world.addRootEntity(oc::move(entity));
    }
}

void NpcSystem::fireShot(const char* prefabPath, const char* name, const glm::vec3& from,
    const glm::vec3& velocity, uint8 team)
{
    EntityPtr shot = Globals::world.spawnAssetFile(prefabPath, Transform(from), true);
    if (!shot)
        return;
    shot->setName(name);
    Globals::world.addRootEntity(shot);
    if (GameProjectileComponent* proj = getComponent<GameProjectileComponent>(shot.get()))
    {
        proj->team = team;
        if (ForceComponent* fc = getComponent<ForceComponent>(shot.get()))
            fc->emitter.setTeam(team);
    }
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(shot.get()))
        pc->body.setLinearVelocity(velocity); // main thread pre-physics: direct setter sanctioned
    m_shots.push_back(shot); // roster: deregistered by onWorldRootRemoved on any despawn path
}

void NpcSystem::service(StructureSystem& structures)
{
    ProfileScope scope("Npc service", EProfileCategory::Game);
    // FAR TICK: units the SIM LOD did not select (no tier stamp on their spatial entry — beyond
    // the outer radius of every player, body disabled, never visited by the pass) walk their
    // orders by teleport instead, every m_farInterval of sim time. Runs BEFORE world.update on
    // main and joins here, so no far-ticked unit is ever touched by the pass in the same window.
    if (Globals::world.simLodActive())
    {
        m_farAccum += float(Globals::time.getSimDeltaSec());
        // Not on a physics-step frame that a step-free frame follows (JobSystem::deferFromPhysicsFrame):
        // the accumulated time carries over, so the deferred tick just covers a little more.
        if (m_farAccum >= m_farInterval && !m_units.empty() && !Globals::jobSystem.deferFromPhysicsFrame())
        {
            ProfileScope farScope("Npc far tick", EProfileCategory::Game);
            const float dt = m_farAccum;
            m_farAccum = 0.0f;
            oc::atomic<int> moved = 0;
            Globals::jobSystem.parallelFor(0u, (uint32)m_units.size(), 64u, JobProfile{ "Npc far tick", EProfileCategory::Game },
                [&](uint32 begin, uint32 end)
            {
                int local = 0;
                for (uint32 i = begin; i < end; ++i)
                {
                    Entity* e = m_units[i].get();
                    GameUnitComponent* unit = getComponent<GameUnitComponent>(e);
                    // THE VOID KILL RUNS FIRST, on the whole roster, before either skip below.
                    // A unit that fell off the world keeps falling as long as its body is live,
                    // and it ends up somewhere nothing visits: OUT of the spatial index (the
                    // isValid skip), or holding a stale tier stamp (the selected skip) while the
                    // entity pass no longer reaches it. Either way updateFar's own voidY test was
                    // unreachable and the body sank forever. This walk is the one thing that sees
                    // every rostered unit, so the check belongs here.
                    if (unit && unit->alive() && e->pos.y < GameUnitComponent::params.voidY)
                    {
                        unit->kill(*e);
                        continue;
                    }
                    if (!e->spatialEntry.isValid()
                        || (Globals::spatialIndex.getPassMaskExact(e->spatialEntry.handle()) & (SpatialPassBits_UpdateTiers | SpatialPassBit_Main)))
                        continue; // selected (a tier, or on screen): the entity pass owns it
                    if (unit && unit->updateFar(*e, dt))
                        ++local;
                }
                moved.fetch_add(local, oc::memory_order_relaxed);
            });
            m_farTicked = moved.load(oc::memory_order_relaxed);
        }
    }
    else
        m_farAccum = 0.0f;
    // STUCK units asking for a planned lane to their destination. The area limiter inside
    // requestSeedPath collapses a jammed group into ONE plan (and caps plans per frame), so this
    // loop is safe to feed with every request that arrived during the pass.
    GameUnitComponent::takeSeedRequests(m_seedScratch);
    for (const GameUnitComponent::SeedRequest& r : m_seedScratch)
        Globals::navSystem.requestSeedPath(r.team, r.from, r.to,
            r.stuck ? m_stuckLaneSpeed : m_orderLaneSpeed, m_laneWidth);
    // Units the BARRACKS decided to produce during the pass (their component paid the energy,
    // claimed the roster slot and set the cooldown — this only performs the entity spawn). A
    // failed spawn refunds the cost and the slot.
    GameStructureComponent::takeSpawnRequests(m_spawnScratch);
    for (const uint32 barracksId : m_spawnScratch)
    {
        const int index = structures.structureIndexById(barracksId);
        if (index < 0)
            continue; // the barracks died between deciding and servicing — its units died with it
        const StructureSystem::Ref& s = structures.structures()[index];
        const ENpcType unitType = (ENpcType)glm::min((int)s.state->barracks.unitType, (int)ENpcType::Count - 1);
        // Spawn on the side facing the route's first waypoint (random side without a route).
        const oc::span<const glm::vec3> route = structures.structureRoute(index);
        const glm::vec2 toWaypoint = route.empty() ? glm::vec2(0.0f)
            : glm::vec2(route[0].x - s.entity->pos.x, route[0].z - s.entity->pos.z);
        if (!spawnUnit(structures, freeSpawnPointAround(structures, s.entity->pos, barracksSpawnRadius(),
            route.empty() ? nullptr : &toWaypoint), barracksId, (uint8)s.state->team, unitType))
        {
            s.state->store[0] = glm::min(s.state->store[0] + s.state->barracks.spawnCost,
                s.state->capacity[0]);
            s.state->barracks.population = glm::max(s.state->barracks.population - (int)s.state->barracks.spawnPop, 0);
        }
    }
    // Lightning the TURRETS fired (their component paid the energy, set the cooldown and landed
    // the hitscan damage): only the beam visual is left to add.
    m_newBeams.clear();
    GameStructureComponent::takeTurretFireRequests(m_turretFireScratch);
    for (const GameStructureComponent::TurretFireRequest& request : m_turretFireScratch)
        addBeam(request.from, request.target + glm::vec3(0.0f, 0.8f, 0.0f));

    // Shots the RANGED units asked for during the pass (spawning is main-thread only).
    GameUnitComponent::takeFireRequests(m_fireScratch);
    for (const GameUnitComponent::FireRequest& request : m_fireScratch)
    {
        const glm::vec3 from = request.from + glm::vec3(0.0f, 0.8f, 0.0f);
        glm::vec3 dir = (request.target + glm::vec3(0.0f, 1.0f, 0.0f)) - from;
        const float len = glm::length(dir);
        if (len <= 1e-3f)
            continue;
        // ShotKind picks the shell: 0 = the spitter's direct shot, 1 = the lobber's slow SPLASH
        // shell (enemyLob.pre: SplashRadius — the projectile's contact damages everything in it),
        // 2 = the SPAWNER: no shell at all — a loose Swarm body is born beside it, on the side
        // facing its target (it holds at StandoffRange like any ranged unit, so "in range" = the
        // same gate the fire timer uses).
        if (request.shotKind == 2)
        {
            const glm::vec2 toTarget(dir.x, dir.z);
            spawnLooseUnit(structures, freeSpawnPointAround(structures, request.from, 2.2f, &toTarget),
                request.team, ENpcType::Swarm);
            continue;
        }
        const bool lob = request.shotKind == 1;
        fireShot(lob ? "Entities/Game/enemyLob.pre" : "Entities/Game/enemyShot.pre",
            lob ? "EnemyLob" : "EnemyShot", from + dir / len * 1.2f,
            dir / len * (lob ? m_lobberShotSpeed : m_spitterShotSpeed), request.team);
    }
    // Each reported death frees its population on its spawner — the tally is maintained by the
    // spawn/death edges instead of by recounting units every frame.
    GameUnitComponent::takeDeaths(m_deathScratch);
    for (const GameUnitComponent::DeathRecord& death : m_deathScratch)
        if (GameStructureComponent* barracks = structures.structureStateById(death.sourceId))
            barracks->barracks.population = glm::max(barracks->barracks.population - (int)death.popCost, 0);
}

void NpcSystem::saveUnits(AssetNode& root) const
{
    oc::vector<Entity*> units;
    queryAllUnits(units);
    for (Entity* entity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        if (!u || u->puppet)
            continue; // puppets are player capsules — players are never saved
        int type = (int)ENpcType::Grunt; // the prefab variant, recovered from the entity name
        for (int t = 0; t < (int)ENpcType::Count; ++t)
            if (oc::string_view(entity->getName()) == c_npcNames[t])
                type = t;
        AssetNode& n = root.addChild("Unit");
        n.set("Type", oc::to_string(type));
        n.set("Team", oc::to_string((int)u->team));
        n.set("Position", entity->pos);
        n.set("Health", u->health);
        n.set("Energy", u->energy);
        n.set("Source", oc::to_string(u->sourceId));
        n.set("RouteIndex", oc::to_string(u->routeIndex));
        // The standing MOVE ORDER is what makes a co-op WAVE unit a wave unit — there is no
        // per-unit `ambient` flag any more, so without it a loaded wave stopped where it stood and
        // held its patch like ambient scatter. Transient WANDER strolls are deliberately skipped:
        // they time out in seconds, and restoring one would pin an idler to a stale spot.
        if (u->moveOrder && !u->wanderOrder)
            n.set("Order", u->targetPos);
    }
}

void NpcSystem::loadUnits(const AssetNode& root, StructureSystem& structures)
{
    clear(); // despawn live units + projectiles (projectiles are transient, not saved)
    // Population tallies are maintained by the spawn/death edges, so a load has to re-seed them:
    // the structures were just rebuilt (all zero) and every unit below re-registers as it spawns.
    for (const StructureSystem::Ref& s : structures.structures())
        if (isBarracksType(s.type))
            s.state->barracks.population = 0;
    for (const AssetNode* n : root.findAll("Unit"))
    {
        const int typeInt = glm::clamp(n->find("Type") ? n->find("Type")->asInt() : 0,
            0, (int)ENpcType::Count - 1);
        const uint8 team = (uint8)glm::clamp(n->find("Team") ? n->find("Team")->asInt() : 1,
            0, GameMaxTeams - 1);
        const glm::vec3 pos = n->find("Position") ? n->find("Position")->asVec3() : glm::vec3(0.0f, 1.0f, 0.0f);
        const uint32 source = n->find("Source") ? (uint32)n->find("Source")->asInt() : 0;
        Entity* entity = spawnUnit(structures, pos, source, team, (ENpcType)typeInt);
        if (!entity)
            continue;
        GameUnitComponent* u = getComponent<GameUnitComponent>(entity);
        u->health = glm::clamp(n->find("Health") ? n->find("Health")->asFloat() : u->health, 1.0f, u->healthMax);
        u->energy = glm::clamp(n->find("Energy") ? n->find("Energy")->asFloat() : u->energy, 0.0f, u->energyMax);
        u->routeIndex = (uint8)glm::clamp(n->find("RouteIndex") ? n->find("RouteIndex")->asInt() : 0, 0, 255);
        // Resume the march (co-op wave units, and any player-ordered unit). NOT `fresh`: this is
        // the same order continuing, not a new one, so it keeps following whatever lane exists —
        // the units re-request their own on their normal timers. `orderMove` re-clears routeIndex,
        // which is what an ordered unit saved anyway. Older saves have no key and stay AI-driven.
        if (const AssetNode* order = n->find("Order"))
            u->orderMove(order->asVec3(), /*fresh*/ false);
        if (GameStructureComponent* barracks = structures.structureStateById(source))
            barracks->barracks.population += u->popCost;
    }
}
