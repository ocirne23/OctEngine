module Entity;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Threading;

import RendererVK;
import :Entity;
import :EntityNames;
import :Component;
import File;

import :AssetRegistry;
import Spatial;
import Force;
import :AnimationDescription;
import Animation;
import Physics;
import Audio;

bool World::initialize()
{
    Globals::assetRegistry.scanDirectory();
    m_updateStaging.initialize(); // main calls this after JobSystem::initialize

    // SIM LOD tweaks (see SimLodConfig in World.ixx). Neither Saved nor Synced: the code defaults
    // rule every run (a stale tweaks.cfg must not override a tuning change), and the LOD is a
    // per-process performance setting.
    {
        SimLodConfig& c = m_simLod;
        Tweak::boolean("Game/Sim LOD", "Enabled", &c.enabled);
        Tweak::boolean("Game/Sim LOD", "Horizontal distance", &c.horizontal);
        Tweak::floatVar("Game/Sim LOD", "Full rate within (m)", &c.radius[0], 0.0f, 2000.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Tier 1 within (m)", &c.radius[1], 0.0f, 2000.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Tier 1 interval (s)", &c.intervalSec[0], 0.0f, 5.0f, 0.01f);
        Tweak::intVar("Game/Sim LOD", "Tier 1 min frames", &c.minFrames[0], 1, 60, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Tier 2 within (m)", &c.radius[2], 0.0f, 2000.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Tier 2 interval (s)", &c.intervalSec[1], 0.0f, 5.0f, 0.01f);
        Tweak::intVar("Game/Sim LOD", "Tier 2 min frames", &c.minFrames[1], 1, 60, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Dormant interval (s, 0 = never)", &c.intervalSec[2], 0.0f, 60.0f, 0.1f);
        Tweak::intVar("Game/Sim LOD", "Dormant min frames", &c.minFrames[2], 1, 600, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Interval jitter", &c.intervalJitter, 0.0f, 0.5f, 0.01f);
        Tweak::intVar("Game/Sim LOD", "Visible max tier", &c.visibleMaxTier, 0, 2, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Max catch-up (s)", &c.maxCatchUpSec, 0.01f, 10.0f, 0.05f);
        Tweak::floatVar("Game/Sim LOD", "Query margin (m)", &c.queryMargin, 0.0f, 100.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Zone margin (m)", &c.zoneMargin, 0.0f, 100.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Zone tier 2 band (m)", &c.zoneTier2Band, 0.0f, 200.0f, 1.0f);
        Tweak::floatVar("Game/Sim LOD", "Selection interval (s)", &c.selectionIntervalSec, 0.0f, 1.0f, 0.01f);
        Globals::spatialIndex.setVisibleCollect(SpatialLayer_Entity); // the cull job hands over the visible entities (see update)
        Tweak::intVar("Game/Sim LOD", "Force bubbles max tier (3 = always)", &c.forceMaxTier, 0, 3, 1.0f);
        Tweak::boolean("Game/Sim LOD", "Dormant disables physics body", &c.dormantDisableBody);
        Tweak::boolean("Game/Sim LOD/Follows", "Units", &c.units);
        Tweak::boolean("Game/Sim LOD/Follows", "Structures", &c.structures);
        Tweak::boolean("Game/Sim LOD/Follows", "Projectiles", &c.projectiles);
        Tweak::boolean("Game/Sim LOD/Follows", "Scripts", &c.scripts);
        Tweak::boolean("Game/Sim LOD/Follows", "Animators", &c.animators);
    }
    // Live readouts (overwritten every pass; edits are meaningless) — deliberately not Saved.
    Tweak::intVar("Game/Sim LOD/Stats", "Full rate", &m_simLodStats[0], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Tier 1", &m_simLodStats[1], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Tier 2", &m_simLodStats[2], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Dormant", &m_simLodStats[3], 0, 1 << 20, 0.0f);
    return true;
}


void World::setSimLodFocus(const glm::vec3* points, uint32 count)
{
    m_simLodFocusCount = glm::min(count, MaxSimLodFocus);
    for (uint32 i = 0; i < m_simLodFocusCount; ++i)
        m_simLodFocus[i] = glm::dvec3(points[i]);
}

void World::setSimLodZones(const glm::vec4* spheres, uint32 count)
{
    m_simLodZoneCount = glm::min(count, MaxSimLodZones);
    for (uint32 i = 0; i < m_simLodZoneCount; ++i)
        m_simLodZone[i] = glm::dvec4(spheres[i]);
}

// A focus point stamps all three tiers at the config radii; a zone stamps tier 1 out to its
// radius + margin and tier 2 over the band beyond, never tier 0. Each ball is queried at its
// outer tier radius + the query margin, so an entity LEAVING the outer tier is still visited
// once with no tier stamp (= dormant) and takes its dormancy edge.
void World::buildSelectSpheres(oc::vector<SelectSphere>& out) const
{
    out.clear();
    for (uint32 i = 0; i < m_simLodFocusCount; ++i)
        out.push_back({ m_simLodFocus[i], m_simLod.radius[2] + m_simLod.queryMargin,
            { m_simLod.radius[0], m_simLod.radius[1], m_simLod.radius[2] } });
    for (uint32 i = 0; i < m_simLodZoneCount; ++i)
    {
        const float tier1 = float(m_simLodZone[i].w) + m_simLod.zoneMargin;
        const float tier2 = tier1 + m_simLod.zoneTier2Band;
        out.push_back({ glm::dvec3(m_simLodZone[i]), tier2 + m_simLod.queryMargin, { 0.0f, tier1, tier2 } });
    }
}

bool World::simLodSelected(const Entity& entity) const
{
    if (!m_simLodActive || entity.isGlobal() || !entity.spatialEntry.isValid())
        return true;
    const SpatialIndex& spatialIndex = Globals::spatialIndex;
    const SpatialHandle handle = entity.spatialEntry.handle();
    if (spatialIndex.getPassMask(handle) & (SpatialPassBits_UpdateTiers | SpatialPassBit_Main))
        return true;
    // Linked but never tier-stamped (fresh, or never inside a ball): follows a non-Global parent —
    // its tier then comes from the distance (simLodTiers). A Global root's children need a real
    // stamp, or every rock under the terrain root would be visited.
    return !spatialIndex.hasStamp(handle, ESpatialPass::UpdateTier2) && entity.parent && !entity.parent->isGlobal();
}

// The selection job. A new tier stamp generation first (nothing else stamps the tiers, so the
// stamps hold until the next job), then, sphere by sphere, ONE PARALLEL traversal (the index's
// frontier fan-out: it stamps the hits' tiers by distance band and hands the hits back as a
// list per traversal chunk), followed by a parallelFor over those chunks that walks every hit
// up to its root — owner-sliced scratch (a roots slot per chunk: no per-worker state across the
// fan-out's waits). The serial tail merges the roots, already deduped by the atomic UpdateRoot
// stamp (overlapping balls, several hits under one root), into submit-ready nodes, so update()
// only checks liveness and slices them to the workers. Runs between commits (post-update) — see
// the kick in update() for what makes the root walk and the stamps safe.
void World::computeSelection(SelectResult& out)
{
    ProfileScope scope("Update selection query", EProfileCategory::Entity);
    out.nodes.clear();
    out.rootHandles.clear();
    Globals::spatialIndex.advanceUpdateTiers();
    for (const SelectSphere& s : out.spheres)
    {
        const oc::vector<oc::vector<uint64>>& hitChunks =
            Globals::spatialIndex.queryUpdateTiers(s.center, s.queryRadius, s.tierRadius, m_simLod.horizontal, SpatialLayer_Entity);
        const uint32 numChunks = uint32(hitChunks.size());
        if (out.roots.size() < numChunks)
            out.roots.resize(numChunks);
        Globals::jobSystem.parallelFor(0u, numChunks, 1u, JobProfile{ "Update selection roots", EProfileCategory::Entity },
            [this, &out, &hitChunks](uint32 begin, uint32 end)
        {
            for (uint32 c = begin; c < end; ++c)
            {
                oc::vector<Entity*>& roots = out.roots[c];
                roots.clear();
                for (const uint64 userData : hitChunks[c])
                    selectUpdateRoot(reinterpret_cast<Entity*>(userData), ESpatialPass::UpdateRoot, roots);
            }
        });
        for (uint32 c = 0; c < numChunks; ++c)
            for (Entity* root : out.roots[c])
            {
                out.nodes.push_back({ root, Transform() });
                out.rootHandles.push_back(root->spatialEntry.handle());
            }
    }
    out.valid = true;
}

// A hit at any depth: its ancestors are stamped UpdateTier2 (the job's fresh generation; a pure
// store, so overlapping balls may stamp the same chain concurrently) so the descent reaches it —
// a visited parent emits only selected children (submitEntityBatches) — and its root is recorded
// ONCE: the atomic stampCurrentOnce on `rootPass` makes exactly one caller win a shared root. A
// Global ancestor is visited from the global list anyway; a Global root is skipped, which is
// what makes the merge with m_globalRoots dedupe-free.
void World::selectUpdateRoot(Entity* hit, ESpatialPass rootPass, oc::vector<Entity*>& roots)
{
    Entity* e = hit;
    while (Entity* p = e->parent)
    {
        if (p->isGlobal())
            return;
        if (p->spatialEntry.isValid())
            Globals::spatialIndex.stampCurrent(p->spatialEntry.handle(), ESpatialPass::UpdateTier2);
        e = p;
    }
    if (e->isGlobal() || !e->spatialEntry.isValid())
        return;
    const SpatialHandle handle = e->spatialEntry.handle();
    if (rootPass == ESpatialPass::VisibleRoot && Globals::spatialIndex.isStampedCurrent(handle, ESpatialPass::UpdateRoot))
        return; // the periodic result holds it already
    if (Globals::spatialIndex.stampCurrentOnce(handle, rootPass))
        roots.push_back(e);
}

// The tiers from the entity's OWN spatial stamps: the UpdateTier balls around every focus point
// (cull job) and Main (the camera's main pass). DISTANCE tier = what the bubble gate and the
// dormant transition go by — the camera sees the whole tier-1/2 area from above, so visibility
// must never override distance for those; TICK tier = the distance tier floored by "Visible max
// tier" for an in-view entity (a rate floor for what the player can see), dormant stays dormant.
// NOT placed: the entry carries no real tier stamp — the spawn-frame visit from the pending list
// (unlinked, the mask is only the spawn GUARD saying "in every pass"), so the tier is unknown.
World::SimLodTiers World::simLodTiers(const Entity& entity) const
{
    const SpatialIndex& spatialIndex = Globals::spatialIndex;
    const SpatialHandle handle = entity.spatialEntry.handle();
    SimLodTiers t{ 3, 3 };
    if (spatialIndex.hasStamp(handle, ESpatialPass::UpdateTier2))
    {
        // Placed by the periodic job at some point: the EXACT stamps (an old generation = the last
        // job saw it outside every ball = dormant).
        const uint32 mask = spatialIndex.getPassMaskExact(handle);
        if (mask & SpatialPassBit_UpdateTier0)      t.dist = 0;
        else if (mask & SpatialPassBit_UpdateTier1) t.dist = 1;
        else if (mask & SpatialPassBit_UpdateTier2) t.dist = 2;
    }
    else
        t.dist = uint8(simLodDistanceTier(entity.pos)); // fresh (unlinked, or linked since the last job): the same tier, by distance
    t.tick = t.dist;
    if (t.tick != 3 && t.tick > uint8(m_simLod.visibleMaxTier) && (spatialIndex.getPassMask(handle) & SpatialPassBit_Main))
        t.tick = uint8(glm::clamp(m_simLod.visibleMaxTier, 0, 2));
    return t;
}

// The dormant / wake transitions on the entity's scheduling state. DORMANT: the body would glide
// on at its last steering velocity for as long as nothing ticks it — park it (disabled or
// asleep, per the tweak). WAKE (also the FIRST stamped visit of a fresh entity — schedTier
// starts at 3): unpark, no catch-up over the stretch.
void World::simLodTransition(Entity& entity, uint8 tier)
{
    const uint8 prevTier = entity.schedTier;
    if (tier == prevTier)
        return;
    entity.schedTier = tier;
    if (tier == 3)
    {
        if (PhysicsComponent* physics = getComponent<PhysicsComponent>(&entity))
            physics->park(m_simLod.dormantDisableBody);
    }
    else if (prevTier == 3)
    {
        entity.schedTick = schedTickNow(); // no catch-up over the dormant stretch
        if (PhysicsComponent* physics = getComponent<PhysicsComponent>(&entity))
        {
            // A marching unit wakes at its walk velocity: its first throttled tick may be a second away.
            GameUnitComponent* unit = getComponent<GameUnitComponent>(&entity);
            physics->unpark(entity, unit ? unit->wakeVelocity(entity) : glm::vec3(0.0f));
        }
    }
}

// TIME-based cadence with a minimum frame gap. The sim time a tick covers is exact: the ring
// holds the cumulative time at the end of every recent pass, so now minus the value at the pass
// of the last tick (schedSkipped + 1 passes back) is the covered stretch. Returns the delta for
// this visit: the frame delta (tier 0), the covered stretch capped by "Max catch-up" (a tick),
// 0 (skipped frame) or < 0 (dormant: not visited).
float World::simLodCadence(Entity& entity, uint8 tier)
{
    const uint32 now = schedTickNow();
    if (tier == 0)
    {
        entity.schedTick = now;
        return m_updateDelta;
    }
    const float intervalSec = m_simLod.intervalSec[tier - 1];
    if (tier == 3 && intervalSec <= 0.0f)
        return -1.0f; // dormant, never ticks
    // Wrap-safe on the 14-bit clock; both ends sit on the same grid, so consecutive deltas sum
    // exactly to the grid time.
    const float elapsed = float((now - entity.schedTick) & Entity::SchedTickMask) / Entity::SchedTickHz;
    // Per-entity jitter on the interval (+-intervalJitter): a wave that entered the tier on the
    // same frame drifts apart within a few ticks instead of ticking in lockstep. The frame-gap
    // floor is minFrames x this frame's delta.
    const float hashFrac = float(uint32((uintptr_t(&entity) >> 6) * 2654435761u) >> 8) * (1.0f / 16777216.0f);
    const float threshold = glm::max(intervalSec * (1.0f + m_simLod.intervalJitter * (hashFrac * 2.0f - 1.0f)),
        float(glm::max(m_simLod.minFrames[tier - 1], 1)) * m_updateDelta);
    if (elapsed < threshold)
        return tier == 3 ? -1.0f : 0.0f;
    entity.schedTick = now;
    return glm::min(elapsed, glm::max(m_simLod.maxCatchUpSec, m_updateDelta));
}

// The tier by direct distance (XZ per the tweak) to the focus points and zones — what the stamps
// encode, computed here for an entity that has no stamp yet (a fresh one: the periodic job has not
// run since it linked). O(focus + zones) per call: the pending list is short.
int World::simLodDistanceTier(const glm::vec3& pos) const
{
    const glm::dvec3 p(pos);
    const auto dist2 = [&](const glm::dvec3& c)
    {
        const glm::dvec3 d = p - c;
        return m_simLod.horizontal ? d.x * d.x + d.z * d.z : glm::dot(d, d);
    };
    int tier = 3;
    for (uint32 i = 0; i < m_simLodFocusCount && tier > 0; ++i)
    {
        const double d2 = dist2(m_simLodFocus[i]);
        for (int t = 0; t < tier; ++t)
            if (d2 < double(m_simLod.radius[t]) * double(m_simLod.radius[t]))
            {
                tier = t;
                break;
            }
    }
    for (uint32 i = 0; i < m_simLodZoneCount && tier > 1; ++i)
    {
        const double d2 = dist2(glm::dvec3(m_simLodZone[i]));
        const double tier1 = m_simLodZone[i].w + double(m_simLod.zoneMargin);
        const double tier2 = tier1 + double(m_simLod.zoneTier2Band);
        if (d2 < tier1 * tier1)
            tier = 1;
        else if (d2 < tier2 * tier2 && tier > 2)
            tier = 2;
    }
    return tier;
}

// The delta for this visit AND the bubble gate: a bubble spawns DARK, and this is the one place
// that switches it — on for an entity the LOD does not apply to, by distance tier otherwise —
// so it never holds a GPU slot before its entity has a tier.
float World::simLodDelta(Entity& entity)
{
    ForceComponent* force = getComponent<ForceComponent>(&entity);
    if (!m_simLodActive || entity.isGlobal() || !entity.spatialEntry.isValid())
    {
        if (force)
            force->setActive(true);
        return m_updateDelta;
    }
    // Only entities carrying a following sim kind and no pinning one are THROTTLED; a bubble is
    // tier-gated on every selected entity regardless.
    const bool throttled = (entity.typeBits & m_simLodFollowMask) && !(entity.typeBits & m_simLodPinMask);
    if (!throttled && !force)
        return m_updateDelta;
    const SimLodTiers tiers = simLodTiers(entity);
    // The bubble gate, by DISTANCE tier, set every visit (a handle resolve + a store) so a tweak
    // change applies without a tier change; an entity leaving the selection keeps its last
    // state — the query-margin visit (distance tier 3) switches it off on the way out.
    if (force)
        force->setActive(int(tiers.dist) <= m_simLod.forceMaxTier);
    if (!throttled)
        return m_updateDelta;
    m_updateStaging.local().simLodCount[tiers.tick]++;
    simLodTransition(entity, tiers.tick);
    return simLodCadence(entity, tiers.tick);
}

// CONTINUATION BATCHES, no level barrier: the tree used to be walked breadth-first with a
// parallelFor + merge per depth, which made every level wait for the SLOWEST batch of the previous
// one ("Level merge" stalls). The only real dependency is parent-before-CHILD - a child reads its
// parent solely through the parentWorld baked into its node - so a batch job now slices the
// children it just emitted into new batch jobs and submits them immediately: subtrees descend
// independently, and a worker's own continuations run LIFO from its local deque (cache-warm,
// DFS-ish). One JobCounter spans the whole pass; submits happen before the submitting job
// finishes, so the counter can never transiently hit zero early.
void World::update(Renderer& renderer, float deltaSeconds)
{
    ProfileScope updateScope("World update", EProfileCategory::Entity);
    ++m_updateFrame;
    ProfileScope setupScope("Update setup", EProfileCategory::Entity); // arena sizing, budget, LOD masks

    // Arena sizing happens BETWEEN passes only (jobs hold pointers into it): last frame's use plus
    // any overflow, doubled for slack.
    const uint32 lastUse = m_updateArenaCursor.load(oc::memory_order_relaxed)
        + m_updateArenaOverflow.load(oc::memory_order_relaxed);
    if (uint32(m_updateArena.size()) < glm::max(4096u, lastUse * 2))
        m_updateArena.resize(glm::max(4096u, lastUse * 2));
    m_updateArenaCursor.store(0, oc::memory_order_relaxed);
    m_updateArenaOverflow.store(0, oc::memory_order_relaxed);
    m_updateArenaSpill.clear(); // last pass's spill blocks: the arena above now covers them

    m_updateRenderer = &renderer;
    m_updateDelta = deltaSeconds;
    m_simTimeAccum += deltaSeconds;
    // COST-BUDGETED batches instead of a uniform grain: every entity carries a measured updateCost
    // (unmeasured counts as 1 so it still partitions), and a batch fills until the summed cost
    // reaches ~25us of measured time (m_updateCost's EMA is fed COST UNITS as its item count, so
    // nsPerItem() self-calibrates to "ns per cost unit"). No even-split term anymore: parallelism
    // now comes from the fan-out itself - children spawn jobs the moment their parent's batch ends.
    m_updateBudget = uint32(glm::clamp<uint64>(25000 / glm::max<uint64>(m_updateCost.nsPerItem(), 1), 1, 4096));

    // SIM LOD: resolve the per-pass constants once (the tweaks are live, the pass reads copies).
    m_simLodFollowMask = m_simLodPinMask = 0;
    m_simLodActive = m_simLod.enabled && (m_simLodFocusCount + m_simLodZoneCount) > 0 && m_updateDelta > 0.0f;
    if (m_simLodActive)
    {
        const auto kind = [this](bool follows, EComponentID id) { (follows ? m_simLodFollowMask : m_simLodPinMask) |= uint16(1 << id); };
        kind(m_simLod.units,       EComponentID_GameUnit);
        kind(m_simLod.structures,  EComponentID_GameStructure);
        kind(m_simLod.projectiles, EComponentID_GameProjectile);
        kind(m_simLod.scripts,     EComponentID_Script);
        kind(m_simLod.animators,   EComponentID_Animator);
    }
    m_updateStaging.forEach([](EntityUpdateStaging& s) { for (uint32& n : s.simLodCount) n = 0; });
    setupScope.stop();

    // SELECTION. LOD inactive (no focus, paused, disabled): every root, every child — the pass
    // scales with the entity count. LOD active: the pass is DETACHED from the entity count — the
    // visit set is the global roots, the roots added since the last pass (unlinked until the next
    // commit, so a query cannot find them yet) and one sphere query per focus point at the outer
    // tier radius + margin on the Entity layer, at ANY depth: every hit walks up to its root
    // (stamping the ancestors so the descent passes through them), and a visited parent emits
    // only its selected children. An organisational root at the origin therefore neither hides
    // its far-flung children nor drags all of them in.
    m_updateLevel.clear();
    if (!m_simLodActive)
    {
        for (const EntityPtr& root : m_rootEntities)
            m_updateLevel.push_back({ root.get(), Transform() });
    }
    else
    {
        ProfileScope selectScope("Update selection", EProfileCategory::Entity);
        // The QUERY already ran as last frame's post-update job (computeSelection, joined at the
        // top of this frame) and left submit-ready nodes, so the batches kick without waiting on
        // it. Left here on main, O(roots) over the CONTIGUOUS handle array only: swap-remove roots
        // that died in between (the spatial handle proves liveness — a slot reuse fails its
        // generation), then gather the Global and pending roots. Everything that touches an
        // Entity (the visible walk, the selection filter, the cost partition) runs on the slice
        // jobs the submit below kicks. NO dedupe is needed: the job skips Global roots, and a
        // pending root's entry is unlinked until the commit AFTER the job ran, so the query could
        // not have found it. The first LOD frame has no result: inline.
        Globals::jobSystem.wait(m_selectCounter); // main already joined before the spatial kick: a no-op guard
        if (m_selectKickFrame == 0)
        {
            buildSelectSpheres(m_selectResult.spheres); // the first LOD pass: inline, nothing ran yet
            computeSelection(m_selectResult);
            m_selectKickFrame = m_updateFrame;
            m_selectKickTime = m_simTimeAccum;
        }
        SelectResult& sel = m_selectResult;
        SpatialIndex& spatialIndex = Globals::spatialIndex;
        // "In the periodic result" = the root's UpdateRoot stamp is current (the job's generation,
        // held until the next job). A FRESH result retires the pending roots it covers (see
        // PendingRoot): those added before the kick pass — linked by the time it ran — and any it
        // contains anyway.
        const auto inResult = [&](const Entity* e) {
            return e->spatialEntry.isValid() && spatialIndex.isStampedCurrent(e->spatialEntry.handle(), ESpatialPass::UpdateRoot); };
        if (sel.valid)
        {
            sel.valid = false;
            const uint64 kickFrame = m_selectKickFrame;
            oc::erase_if(m_pendingRoots, [&](const PendingRoot& p) { return p.frame < kickFrame || inResult(p.entity); });
        }
        // Dead roots out (order is free). The result itself stays: reused until the next job.
        for (size_t i = 0; i < sel.nodes.size();)
        {
            if (spatialIndex.isAlive(sel.rootHandles[i]))
            {
                ++i;
                continue;
            }
            sel.nodes[i] = sel.nodes.back();
            sel.nodes.pop_back();
            sel.rootHandles[i] = sel.rootHandles.back();
            sel.rootHandles.pop_back();
        }
        // THE VISIBLE SET, fresh EVERY frame from the cull job's Main pass (no traversal here — the
        // stamp collected the handles): each visible entity walked to its root (ancestors stamped);
        // the VisibleRoot stamp (a new generation per pass) dedupes among them and the UpdateRoot
        // stamp skips those the periodic result holds, so a root is visited once. This is what
        // keeps what the player sees at full rate no matter how stale the periodic selection is.
        // The walk itself runs on the visible-slice jobs below; main only opens the generation
        // and claims the PENDING roots in it FIRST (stampCurrentOnce), so a slice that walks a
        // visible descendant up to a pending root loses the exchange and skips it.
        spatialIndex.advanceStamp(ESpatialPass::VisibleRoot);
        for (Entity* e : m_globalRoots)
            m_updateLevel.push_back({ e, Transform() });
        for (const PendingRoot& p : m_pendingRoots)
            if (!inResult(p.entity) && (!p.entity->spatialEntry.isValid()
                || spatialIndex.stampCurrentOnce(p.entity->spatialEntry.handle(), ESpatialPass::VisibleRoot)))
                m_updateLevel.push_back({ p.entity, Transform() }); // not queued by either source yet: one visit
    }
    if (!m_simLodActive)
        m_pendingRoots.clear(); // every root was visited; nothing is owed a visit
    {
        // The batch job submits: main hands the root SOURCES to slice jobs (the workers were idle
        // while main filtered and copied every root serially) and submits only the small
        // global + pending list itself, so the first batches start within a few microseconds.
        ProfileScope submitScope("Update batch submit", EProfileCategory::Entity);
        {
            ProfileScope scope("Root slices: global + pending", EProfileCategory::Entity);
            submitRootSlices(m_updateLevel.data(), uint32(m_updateLevel.size())); // LOD: global + pending, inline below one slice
        }
        if (m_simLodActive)
        {
            {
                ProfileScope scope("Root slices: selection", EProfileCategory::Entity);
                submitRootSlices(m_selectResult.nodes.data(), uint32(m_selectResult.nodes.size()));
            }
            ProfileScope scope("Visible root slices", EProfileCategory::Entity);
            submitVisibleRootSlices();
        }
    }

    {
        // Helps: main runs batch jobs alongside the workers until the whole tree is done.
        ProfileScope waitScope("Update job wait", EProfileCategory::Wait);
        Globals::jobSystem.wait(m_updateCounter);
    }

    // NEXT frame's selection, FIRE-AND-FORGET the moment the pass is done: it has the whole rest
    // of the frame (send, procedural updates, UI, present, the fence wait, the next frame's input
    // and physics) instead of only the present window, and main joins it just before the next
    // spatial kick (joinSelection — the commit in there would mutate the index under it). Nothing
    // destroys entities in that stretch (the destroy windows sit after the joins), so the root
    // walk is safe; registrations take the index's exclusive lock against the query. It sees
    // positions one commit older than an inline query would; the query margin covers a frame of
    // motion, and roots spawned meanwhile arrive through m_pendingRoots.
    // Once "Selection interval (s)" of sim time has passed since the last kick (frame-rate
    // independent; 0 = every pass): in between the last result is reused (the tier stamps are
    // the job's own generation, so they stay current too). NOT on a physics-step frame when a
    // step-free one follows (JobSystem::deferFromPhysicsFrame): the workers carry the solver
    // tasks OR this job in a frame, never both.
    if (m_simLodActive && m_simTimeAccum - m_selectKickTime >= m_simLod.selectionIntervalSec
        && !Globals::jobSystem.deferFromPhysicsFrame())
    {
        ProfileScope queueScope("Update selection queue", EProfileCategory::Entity);
        m_selectKickFrame = m_updateFrame;
        m_selectKickTime = m_simTimeAccum;
        buildSelectSpheres(m_selectResult.spheres);
        Globals::jobSystem.submit([this] { computeSelection(m_selectResult); },
            { "Update selection query", EProfileCategory::Entity }, EJobPriority::Normal, &m_selectCounter);
    }

    ProfileScope statsScope("Update stats", EProfileCategory::Entity);
    for (int& n : m_simLodStats)
        n = 0;
    m_updateStaging.forEach([this](const EntityUpdateStaging& s)
    {
        for (int t = 0; t < 4; ++t)
            m_simLodStats[t] += int(s.simLodCount[t]);
    });
}

void World::joinSelection()
{
    ProfileScope scope("Update selection join", EProfileCategory::Wait);
    Globals::jobSystem.wait(m_selectCounter);
}

// Copies the nodes into the frame arena in ONE claim, then slices the arena range into
// cost-budgeted batch jobs. The single up-front copy is load-bearing, not an optimization: `nodes`
// is the caller's per-worker staging vector, and a submit can execute its job INLINE on queue
// exhaustion - an inline child batch reuses that same staging slot, so nothing may read `nodes`
// once the first submit goes out. Called from the main thread for the roots and from batch jobs
// for their children.
bool World::submitEntityBatches(const EntityUpdateNode* nodes, uint32 count, const EntityUpdateNode** inlineNodes, uint32* inlineCount)
{
    static_assert(std::is_trivially_copyable_v<EntityUpdateNode>);
    if (count == 0)
        return false;
    const uint32 slot = m_updateArenaCursor.fetch_add(count, oc::memory_order_relaxed);
    EntityUpdateNode* dst;
    if (slot + count <= uint32(m_updateArena.size()))
        dst = m_updateArena.data() + slot;
    else
    {
        // Arena exhausted (the claim is never rolled back): SPILL into a block of this claim's
        // own, pointer-stable like the arena and freed between passes once the arena has grown
        // to cover it. The mutex guards the block list only; this is the rare path (the arena is
        // sized from last pass, so it takes a burst - a wave spawn, a camera swing - to get
        // here), and it used to run these subtrees SERIALLY on the submitting thread instead,
        // which on main was a 20 ms "Update batch submit" on a burst frame.
        m_updateArenaOverflow.fetch_add(count, oc::memory_order_relaxed);
        ProfileScope scope("Update arena spill", EProfileCategory::Entity); // rare by design: visible when it is not
        std::lock_guard lock(m_updateArenaSpillMutex);
        dst = m_updateArenaSpill.emplace_back(oc::make_unique<EntityUpdateNode[]>(count)).get();
    }
    // SELECTION filter while copying: only stamped children ride into the arena (see update();
    // the root list from main is pre-selected, so only emitted children ever drop). The claim
    // covers all `count` nodes — the few slots a dropped child leaves unused are cheaper than a
    // second predicate pass, and the arena is sized from last frame's claims anyway.
    uint32 out = 0;
    for (uint32 i = 0; i < count; ++i)
        if (simLodSelected(*nodes[i].entity))
            dst[out++] = nodes[i];
    count = out;

    bool handedBack = false;
    uint32 begin = 0;
    while (begin < count)
    {
        uint32 cost = 0, end = begin;
        while (end < count && cost < m_updateBudget)
            cost += glm::max<uint32>(dst[end++].entity->updateCost, 1);
        const EntityUpdateNode* batch = dst + begin;
        const uint32 n = end - begin;
        begin = end;
        // THE CONTINUATION: the first batch goes back to the calling batch job to run on the same
        // fiber instead of becoming a job. A unit's few child parts are then processed by the job
        // that just updated the unit — one job per SUBTREE budget instead of one per level per
        // parent, which at 40k units was 40k+ continuation jobs and exhausted the job pool.
        if (inlineNodes && !handedBack)
        {
            *inlineNodes = batch;
            *inlineCount = n;
            handedBack = true;
            continue;
        }
        // High: the pass is the frame's critical path (main waits on it), every batch is bounded
        // (~25us budget), and High is what the window-thread helper serves between pumps.
        Globals::jobSystem.submit([this, batch, n] { updateBatchJob(batch, n); },
            { "Entity Update", EProfileCategory::Entity }, EJobPriority::High, &m_updateCounter);
    }
    return handedBack;
}

// A root source handed to the workers in slices: each slice job runs submitEntityBatches on its
// range (the selection filter, the cost partition, the arena copy and the batch submits all
// happen there), so the per-root cache misses spread over the workers and the first batches
// start while the rest of the list is still being sliced. `nodes` must stay valid and unchanged
// for the whole pass: main's m_updateLevel and the selection result both are (the next selection
// job is kicked only after the pass's wait). Below one slice it is an inline call.
void World::submitRootSlices(const EntityUpdateNode* nodes, uint32 count)
{
    if (count <= rootSliceSize)
    {
        ProfileScope scope("Root batches inline", EProfileCategory::Entity); // the copy, partition and submits on the caller
        submitEntityBatches(nodes, count);
        return;
    }
    for (uint32 begin = 0; begin < count; begin += rootSliceSize)
    {
        const uint32 n = glm::min(rootSliceSize, count - begin);
        Globals::jobSystem.submit([this, nodes, begin, n] { submitEntityBatches(nodes + begin, n); },
            { "Update root slice", EProfileCategory::Entity }, EJobPriority::High, &m_updateCounter);
    }
}

// The visible set's root walk, sliced over the cull job's collected handles: each slice job
// walks its handles to their roots into ITS OWN scratch slot (owner-sliced: no per-worker state,
// the job never waits) and submits those roots as batches straight away. Concurrent slices are
// safe by construction: the ancestor stamps are pure stores of one generation, the root dedupe is
// stampCurrentOnce's atomic exchange, and nothing links or unlinks entries during the pass. The
// slots are sized on main BEFORE any job goes out (a resize would move them).
void World::submitVisibleRootSlices()
{
    const oc::vector<SpatialHandle>& handles = Globals::spatialIndex.visibleHandles();
    const uint32 count = uint32(handles.size());
    const uint32 numSlices = (count + rootSliceSize - 1) / rootSliceSize;
    if (m_visibleRootSlices.size() < numSlices)
    {
        ProfileScope scope("Visible slices resize", EProfileCategory::Entity);
        m_visibleRootSlices.resize(numSlices);
    }
    ProfileScope submitScope("Visible slice submits", EProfileCategory::Entity);
    for (uint32 slice = 0; slice < numSlices; ++slice)
    {
        const uint32 begin = slice * rootSliceSize;
        const uint32 end = glm::min(begin + rootSliceSize, count);
        const auto walk = [this, slice, begin, end, handleData = handles.data()]
        {
            const SpatialIndex& spatialIndex = Globals::spatialIndex;
            VisibleRootSlice& s = m_visibleRootSlices[slice];
            s.roots.clear();
            for (uint32 i = begin; i < end; ++i)
                if (Entity* e = reinterpret_cast<Entity*>(spatialIndex.userData(handleData[i])))
                    selectUpdateRoot(e, ESpatialPass::VisibleRoot, s.roots);
            s.nodes.clear();
            for (Entity* e : s.roots)
                s.nodes.push_back({ e, Transform() });
            submitEntityBatches(s.nodes.data(), uint32(s.nodes.size()));
        };
        if (numSlices == 1)
        {
            ProfileScope scope("Visible root walk inline", EProfileCategory::Entity);
            walk(); // one slice: inline, no job round trip
        }
        else
            Globals::jobSystem.submit(walk, { "Update visible slice", EProfileCategory::Entity }, EJobPriority::High, &m_updateCounter);
    }
}

void World::updateBatchJob(const EntityUpdateNode* nodes, uint32 count)
{
    // Safe because updateSelf never fiber-waits (and neither does this job - submits don't wait):
    // everything it touches is the audited thread-safe inline set, and the per-worker staging slot
    // stays exclusively ours for the job's whole body.
    EntityUpdateStaging& staging = m_updateStaging.local();
    const ThreadLocalScope tlsPin; // asserts should any update path ever park the fiber
    // Loops over the CONTINUATION: after a batch, the first budget of the children it emitted is
    // run right here (submitEntityBatches hands it back) and only the rest become jobs — so a
    // subtree descends on this fiber until it fans out wider than one budget.
    for (;;)
    {
    staging.children.clear();
    const Clock::time_point batchStart = Clock::now();
    uint32 batchCost = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        const EntityUpdateNode& node = nodes[i];
        Entity* entity = node.entity;
        // SIM LOD: the delta this visit gets (0 = sync/placement only), or no visit at all —
        // a dormant entity's subtree is dropped from the pass here.
        const float delta = simLodDelta(*entity);
        if (delta < 0.0f)
            continue;
        batchCost += glm::max<uint32>(entity->updateCost, 1);
        // MEASURED cost: an entity's FIRST update is timed and becomes its updateCost
        // (250ns units — no guessed initial value), then a ~1/1024-per-frame random
        // re-measure keeps it honest as behaviour changes (a unit that starts fighting,
        // a script that goes idle). Two clock reads only on measured updates.
        const uint32 mix = uint32((uintptr_t(entity) >> 4) * 2654435761u)
            ^ uint32(m_updateFrame * 0x9E3779B9u);
        const bool measure = entity->updateCost == 0 || (mix & 1023) == 0;
        const Clock::time_point measureStart = measure ? Clock::now() : Clock::time_point{};
        // Per-entity scope for OPT-IN entities only (EEntityFlag_Profiled: simulated
        // things — animators, scripts, units, machine structures; static scenery stays
        // scope-free so it cannot flood the rings). NAMED by the registry-owned entity name from
        // Globals::entityNames; destruction transfers its buffer to the profiler's frame history.
        if (entity->isProfiled())
        {
            ProfileScope entityScope(entity->spawnTemplate->displayName.c_str(), EProfileCategory::Entity);
            entity->updateSelf(*m_updateRenderer, delta, node.parentWorld, staging.children);
        }
        else
            entity->updateSelf(*m_updateRenderer, delta, node.parentWorld, staging.children);
        if (measure)
        {
            const uint64 ns = uint64(std::chrono::nanoseconds(Clock::now() - measureStart).count());
            entity->updateCost = uint8(glm::clamp<uint64>(ns / 250, 1, 255));
        }
    }
    // Calibrate: ns per COST UNIT, so the batch budget tracks real update costs.
    m_updateCost.addSample(uint64(std::chrono::nanoseconds(Clock::now() - batchStart).count()), batchCost);

    // The continuation: this batch's children are partitioned RIGHT NOW - no level barrier. The
    // first budget of them comes back as (begin, count) for this very fiber; the rest become jobs,
    // which from a worker land on its local deque (LIFO), so wide fan-outs get stolen while the
    // warm subtree stays here. The staging vector is copied into the arena BEFORE any submit
    // (see submitEntityBatches), so an inline child batch reusing this slot cannot hurt us.
    if (staging.children.empty()
        || !submitEntityBatches(staging.children.data(), uint32(staging.children.size()), &nodes, &count))
        return;
    }
}

static RendererVKLayout::EPipelineIndex parsePipeline(const oc::string& name)
{
    using P = RendererVKLayout::EPipelineIndex;
    if (name == "LitTransparent")   return P::LitTransparent;
    if (name == "UnlitOpaque")      return P::UnlitOpaque;
    if (name == "UnlitTransparent") return P::UnlitTransparent;
    if (name == "Sky")              return P::Sky;
    if (name == "WireframeTransparent") return P::WireframeTransparent;
    if (name == "GizmoUI")          return P::GizmoUI;
    if (name == "GizmoWorld")       return P::GizmoWorld;
    return P::LitOpaque;
}

// Artist-authored collision proxies: "Col_Wall" collides in place of "Wall" and is never rendered
// (the renderer applies the same prefix rule in ObjectContainer::initializeNodes).
// Cook options for the scene cache: the renderer's LOD generation params get baked into the cooked
// file (and its options hash), so changing the LOD tweaks re-cooks affected scenes on the next load.
static SceneCookOptions makeSceneCookOptions()
{
    const MeshLodParams& lodParams = Globals::rendererVK.getLodParams();
    SceneCookOptions options;
    options.generateLods = lodParams.generate;
    options.lodLevels = lodParams.generateLevels;
    options.lodReduction = lodParams.generateReduction;
    options.lodMinIndices = lodParams.minIndices;
    return options;
}

static oc::unique_ptr<ISceneData> loadSceneData(const ObjectContainerDesc& desc)
{
    if (!desc.procedural)
    {
        SceneCookOptions options = makeSceneCookOptions();
        options.decimationFactor = desc.decimationFactor;
        return ISceneData::loadCached(desc.path.c_str(), desc.mergeNodes, desc.preTransformVertices, options);
    }
    oc::unique_ptr<ISceneData> sceneData = ISceneData::createProceduralLoader();
    if (!sceneData->initialize(desc.path.c_str(), desc.mergeNodes, desc.preTransformVertices))
        sceneData.reset();
    return sceneData;
}

ObjectContainer* World::loadContainer(const ObjectContainerDesc& desc, bool captureCollisionSource)
{
    if (auto it = m_containers.find(desc.name); it != m_containers.end())
        return it->second.get();

    oc::unique_ptr<ISceneData> sceneData = loadSceneData(desc);
    if (!sceneData)
    {
        Log::warning("Scene: failed to load '" + desc.path + "' for ObjectContainer '" + desc.name + "'");
        return nullptr;
    }

    if (captureCollisionSource)
        m_collision.captureSource(desc.name, *sceneData);

    auto container = oc::make_unique<ObjectContainer>();
    if (desc.materialOverrides.present)
    {
        const MaterialOverridesDesc& mo = desc.materialOverrides;
        ObjectContainer::MaterialOverrides overrides;
        if (!mo.pipeline.empty())
            overrides.pipelineIdx = parsePipeline(mo.pipeline);
        overrides.excludeFromRayTracing = mo.excludeFromRayTracing;
        overrides.useSceneTextures = mo.useSceneTextures;
        if (mo.diffuseTexIdx >= 0)         overrides.diffuseTexIdx = uint16(mo.diffuseTexIdx);
        if (mo.normalTexIdx >= 0)          overrides.normalTexIdx = uint16(mo.normalTexIdx);
        if (mo.metalRoughnessTexIdx >= 0)  overrides.metalRoughnessTexIdx = uint16(mo.metalRoughnessTexIdx);
        container->initialize(*sceneData, &overrides);
    }
    else
    {
        container->initialize(*sceneData);
    }
    ObjectContainer* ptr = container.get();
    m_containers.emplace(desc.name, oc::move(container));
    return ptr;
}

ObjectContainer* World::getOrLoadContainer(const oc::string& name, bool captureCollisionSource)
{
    if (auto it = m_containers.find(name); it != m_containers.end())
        return it->second.get(); // already loaded; a late collision request falls back to ensureCollisionSource
    if (const ObjectContainerDesc* desc = Globals::assetRegistry.findObjectContainer(name))
        return loadContainer(*desc, captureCollisionSource);
    Log::warning("Scene: unknown ObjectContainer reference '" + name + "'");
    return nullptr;
}

ObjectContainer* World::findLoadedContainer(const oc::string& name)
{
    const auto it = m_containers.find(name);
    return it != m_containers.end() ? it->second.get() : nullptr;
}

EntityPtr World::spawn(const oc::string& name, const Transform& base)
{
    if (oc::shared_ptr<const EntitySpawnTemplate> tmpl = getOrBuildPrefabTemplate(name))
        return Entity::create(*tmpl, base);
    return EntityPtr{};
}

oc::vector<EntityPtr> World::spawnBatch(oc::span<const SpawnRequest> requests, bool addRoots)
{
    if (requests.empty())
        return {};
    ProfileScope profileScope("Spawn batch", EProfileCategory::Entity);

    // Template resolution stays on main: the cache builds/loads on a miss (file IO, renderer
    // container imports) and is not job-safe. The jobs below only run Entity::create.
    oc::vector<oc::shared_ptr<const EntitySpawnTemplate>> templates(requests.size());
    oc::vector<Transform> transforms(requests.size());
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const SpawnRequest& request = requests[i];
        if (FileSystem::extension(request.name).empty())
        {
            // Plain prefab name: the spawn() route, transform used as-is.
            templates[i] = getOrBuildPrefabTemplate(request.name);
            transforms[i] = request.transform;
        }
        else
        {
            // Asset FILE: the spawnAssetFile route — lexical normalize, registry root lookup with
            // file-template fallback, and the same override composition (caller position, caller
            // rotation composed onto the authored default, authored scale).
            oc::string fileName = FileSystem::isAbsolute(request.name)
                ? FileSystem::relativePath(request.name, oc::string(), /*allowMainThread*/ true)
                : FileSystem::normalize(request.name);
            if (fileName.empty())
                fileName = request.name;
            const oc::string* rootName = Globals::assetRegistry.findRootForFile(fileName);
            templates[i] = rootName ? getOrBuildPrefabTemplate(*rootName) : buildFileTemplate(fileName);
            if (templates[i])
            {
                const Transform& dt = templates[i]->defaultTransform;
                transforms[i] = Transform(request.transform.pos, dt.scale, request.transform.quat * dt.quat);
            }
        }
    }

    oc::vector<EntityPtr> results(requests.size());
    Globals::jobSystem.parallelFor(0, (uint32)requests.size(), m_spawnBatchCost,
        { "Spawn batch", EProfileCategory::Entity },
        [&](uint32 begin, uint32 end)
        {
            for (uint32 i = begin; i < end; ++i)
                if (templates[i])
                    results[i] = Entity::create(*templates[i], transforms[i]);
        });

    if (addRoots)
        for (const EntityPtr& e : results)
            if (e)
                addRootEntity(e);
    return results;
}

void World::releaseBatch(oc::vector<EntityPtr>&& entities)
{
    if (entities.empty())
        return;
    ProfileScope profileScope("Destroy batch", EProfileCategory::Entity);
    // Each release is an atomic decrement; the last one runs Entity::destroy on that worker —
    // every component teardown seam locks (see the parallel-spawning notes), and a parent and
    // its child both in the batch compose through the refcounts like any external holder.
    Globals::jobSystem.parallelFor(0, (uint32)entities.size(), m_destroyBatchCost,
        { "Destroy batch", EProfileCategory::Entity },
        [&](uint32 begin, uint32 end)
        {
            for (uint32 i = begin; i < end; ++i)
                entities[i].release();
        });
    entities.clear();
}

void World::reloadPrefabs()
{
    for (auto& [name, tmpl] : m_templates)
        m_retiredTemplates.push_back(oc::move(tmpl));
    m_templates.clear();
}

void World::invalidatePrefab(const oc::string& name)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
    {
        m_retiredTemplates.push_back(oc::move(it->second)); // kept alive for live entities
        m_templates.erase(it);
    }
}

static const AssetNode* findComponentNode(const AssetNode& node, const char* name)
{
    for (const AssetNode* comp : node.findAll("Component"))
        if (comp->asString() == name)
            return comp;
    return nullptr;
}

static bool keyIs(const AssetNode& node, oc::string_view key)
{
    if (node.key.size() != key.size())
        return false;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char a = node.key[i] | 0x20, b = key[i] | 0x20; // ASCII lower
        if (a != b)
            return false;
    }
    return true;
}

static Transform readNodeTransform(const AssetNode& node)
{
    Transform t;
    t.scale = 1.0f;
    if (const AssetNode* n = node.find("Position")) t.pos = n->asVec3();
    if (const AssetNode* n = node.find("Rotation")) t.quat = glm::quat(glm::radians(n->asVec3()));
    if (const AssetNode* n = node.find("Scale"))    t.scale = n->asFloat(0, 1.0f);
    return t;
}

oc::shared_ptr<RenderComponent::SpawnInfo> World::buildRenderSpawnInfo(const AssetNode& renderNode, const oc::string& ownerName, bool captureCollisionSource)
{
    const AssetNode* containerNode = renderNode.find("ObjectContainer");
    if (!containerNode)
        return nullptr;
    const oc::string containerName = containerNode->asString();
    const oc::string nodePath = renderNode.find("Node") ? renderNode.find("Node")->asString() : oc::string();

    ObjectContainer* container = getOrLoadContainer(containerName, captureCollisionSource);
    if (!container)
        return nullptr;

    // Type defaults from the container itself (skinned iff its source scene had a skeleton) but can be
    // overridden explicitly — `Type StaticMesh` / `Type SkinnedMesh`, with an optional nested `Rig` token.
    bool skinned = container->isSkinned();
    oc::string rigType;
    if (const AssetNode* typeNode = renderNode.find("Type"))
    {
        skinned = typeNode->asString() == "SkinnedMesh";
        if (const AssetNode* rigNode = typeNode->find("Rig"))
            rigType = rigNode->asString();
    }

    auto info = oc::make_shared<RenderComponent::SpawnInfo>();
    info->container = container;
    info->containerName = containerName; // kept so an inline entity re-serializes its mesh
    info->skinned = skinned;
    info->rigType = rigType;

    if (nodePath.empty() || nodePath == "ROOT")
        info->nodeIdx = NodeSpawnIdx_ROOT;
    else if (NodeSpawnIdx idx = container->getSpawnIdxForPath(nodePath); idx != NodeSpawnIdx_INVALID)
        info->nodeIdx = idx;
    else
        Log::warning("Scene: entity '" + ownerName + "' references unknown node '" + nodePath + "', using ROOT");
    info->nodePath = nodePath.empty() ? "ROOT" : nodePath;
    info->localTransform = readNodeTransform(renderNode); // mesh offset within the entity
    if (const AssetNode* colorNode = renderNode.find("Color"))
        info->color = glm::clamp(colorNode->asVec3(), 0.0f, 1.0f); // solid tint (see SpawnInfo)
    return info;
}

void writeRenderSpawnInfo(const RenderComponent::SpawnInfo& info, AssetNode& out)
{
    if (!info.container)
        return;
    out.set("ObjectContainer", info.containerName);
    out.set("Node", info.nodePath);
    AssetNode& typeNode = out.addChild("Type");
    typeNode.values.emplace_back(info.skinned ? "SkinnedMesh" : "StaticMesh");
    if (!info.rigType.empty())
        typeNode.addChild("Rig").values.emplace_back(info.rigType);

    const Transform& lt = info.localTransform;
    if (lt.pos != glm::vec3(0.0f))         out.set("Position", lt.pos);
    if (lt.quat != glm::quat(1, 0, 0, 0))  out.set("Rotation", glm::degrees(glm::eulerAngles(lt.quat)));
    if (lt.scale != 1.0f)                  out.set("Scale", lt.scale);
    if (info.color.x >= 0.0f)              out.set("Color", info.color);
}

const AnimationSet* World::getOrBuildClipSet(const Skeleton* skel, const AnimatorDesc& desc)
{
    const oc::string key = oc::to_string(reinterpret_cast<uintptr_t>(skel)) + "/" + desc.name;
    if (auto it = m_clipSets.find(key); it != m_clipSets.end())
        return it->second.get();

    auto set = oc::make_unique<AnimationSet>();
    AnimationSet& clips = *set;

    // Apply the .anm's loop flag + event notifies onto the clip just loaded under `localName`.
    auto applyClipMeta = [&](const AnimationClipDesc& anm, const oc::string& localName)
    {
        const auto idxIt = clips.nameToIndex.find(localName);
        if (idxIt == clips.nameToIndex.end())
            return;
        AnimationClip& clip = clips.clips[idxIt->second];
        clip.loop = anm.loop;
        clip.events.clear();
        for (const auto& [name, t] : anm.events)
            clip.events.push_back({ name, t });
    };

    // Loads one .anm into the set under `localName` (retargeted by bone name to `skel`).
    auto loadClipDesc = [&](const AnimationClipDesc& anm, const oc::string& localName)
    {
        oc::string sourcePath = anm.source;
        if (const ObjectContainerDesc* oc = Globals::assetRegistry.findObjectContainer(anm.source))
            sourcePath = oc->path; // source named a registered container; use its file
        const char* skip = anm.skip.empty() ? nullptr : anm.skip.c_str();
        const char* track = anm.track.empty() ? nullptr : anm.track.c_str();
        ISceneData::loadAnimations(sourcePath.c_str(), *skel, clips, skip, localName.c_str(), track);
        applyClipMeta(anm, localName);
    };

    // Explicit `Clip <local> Anim <anm>` declarations are optional: they alias a clip to a local name (or
    // force-load one nothing references). Anything not declared is lazy-loaded by name below.
    for (const AnimatorDesc::ClipRef& ref : desc.clips)
    {
        if (const AnimationClipDesc* anm = Globals::assetRegistry.findClip(ref.anmName))
            loadClipDesc(*anm, ref.localName);
        else
            Log::warning("Animator '" + desc.name + "': unknown Animation clip '" + ref.anmName + "'");
    }

    // Resolve a clip by name, lazily loading it from the .anm registry when it wasn't declared with `Clip`.
    auto ensureClip = [&](const oc::string& name)
    {
        if (name.empty() || clips.find(name))
            return;
        if (const AnimationClipDesc* anm = Globals::assetRegistry.findClip(name))
            loadClipDesc(*anm, name);
        else
            Log::warning("Animator '" + desc.name + "': unknown clip '" + name + "'");
    };

    oc::unordered_set<oc::string> blendNames;
    for (const AnimatorDesc::BlendSpace& bs : desc.blendSpaces)
        blendNames.insert(bs.name);
    for (const AnimatorDesc::BlendSpace& bs : desc.blendSpaces)
        for (const AnimatorDesc::BlendSample& s : bs.samples)
            ensureClip(s.clip);
    for (const AnimatorDesc::State& st : desc.stateMachine.states)
        if (!oc::contains(blendNames, st.play))
            ensureClip(st.play);

    Log::info("Animator '" + desc.name + "': built clip set (" + oc::to_string(clips.numClips()) + " clips)");

    const AnimationSet* ptr = set.get();
    m_clipSets.emplace(key, oc::move(set));
    return ptr;
}

oc::shared_ptr<AnimatorComponent::SpawnInfo> World::buildAnimatorSpawnInfo(const AssetNode& animatorNode, const oc::string& siblingContainerName, const oc::string& ownerName)
{
    const AssetNode* nameNode = animatorNode.find("Animator");
    if (!nameNode)
        return nullptr;
    const oc::string animatorName = nameNode->asString();

    const AnimatorDesc* desc = Globals::assetRegistry.findAnimator(animatorName);
    if (!desc)
    {
        Log::warning("Scene: entity '" + ownerName + "' references unknown Animator '" + animatorName + "'");
        return nullptr;
    }
    ObjectContainer* siblingContainer = siblingContainerName.empty() ? nullptr : getOrLoadContainer(siblingContainerName);
    if (!siblingContainer || !siblingContainer->isSkinned() || !siblingContainer->getSkeleton())
    {
        Log::warning("Scene: entity '" + ownerName + "' has an Animator but no sibling skinned mesh to drive");
        return nullptr;
    }

    auto info = oc::make_shared<AnimatorComponent::SpawnInfo>();
    info->desc = desc;
    info->skeleton = siblingContainer->getSkeleton();
    info->clipSet = getOrBuildClipSet(info->skeleton, *desc); // shared, imported once per skeleton+animator
    info->animatorName = animatorName;
    if (const AssetNode* n = animatorNode.find("Enabled"))
        info->enabled = n->asBool();
    return info;
}

oc::shared_ptr<SceneComponent::SpawnInfo> World::buildSceneSpawnInfo(const AssetNode& sceneNode)
{
    auto info = oc::make_shared<SceneComponent::SpawnInfo>();

    for (const AssetNode& child : sceneNode.children)
    {
        oc::shared_ptr<const EntitySpawnTemplate> childTmpl;
        if (keyIs(child, "Entity"))
            childTmpl = buildInlineTemplate(child);
        else if (keyIs(child, "Prefab"))
            childTmpl = getOrBuildPrefabTemplate(child.asString());
        else
            continue;
        if (!childTmpl)
            continue;

        SceneComponent::SpawnInfo::ChildSpawnInfo ci;
        ci.tmpl = oc::move(childTmpl);
        ci.localTransform = readNodeTransform(child);
        if (const AssetNode* n = child.find("Name")) ci.name = n->asString();
        if (const AssetNode* n = child.find("Enabled")) ci.enabled = n->asBool();
        info->children.push_back(oc::move(ci));
    }
    return info;
}

// The cache normally gets its snapshot for free while the render container loads. This is the fallback
// for the other order: the container was already loaded (or isn't loaded at all) when a Hull/Mesh shape
// asked for geometry, so the source file is imported once more and handed to the cache.
bool World::ensureCollisionSource(const oc::string& containerName)
{
    if (m_collision.hasSource(containerName))
        return true;

    const ObjectContainerDesc* desc = Globals::assetRegistry.findObjectContainer(containerName);
    if (!desc)
    {
        Log::warning("Physics: unknown ObjectContainer '" + containerName + "' for collision geometry");
        return false;
    }
    oc::unique_ptr<ISceneData> sceneData = loadSceneData(*desc);
    if (!sceneData)
    {
        Log::warning("Physics: failed to load '" + desc->path + "' for collision geometry");
        return false;
    }
    Log::info("Physics: container '" + containerName + "' re-imported for collision geometry (loaded before physics needed it)");

    m_collision.captureSource(containerName, *sceneData);
    return true;
}

oc::shared_ptr<PhysicsComponent::SpawnInfo> World::buildPhysicsSpawnInfo(const AssetNode& physicsNode,
    const oc::string& containerName, const oc::string& nodePath, const oc::string& ownerName)
{
    auto info = oc::make_shared<PhysicsComponent::SpawnInfo>();
    if (const AssetNode* n = physicsNode.find("Body"))
    {
        const oc::string& type = n->asString();
        if (type == "Static")         info->bodyType = EPhysicsBodyType::Static;
        else if (type == "Kinematic") info->bodyType = EPhysicsBodyType::Kinematic;
        else                          info->bodyType = EPhysicsBodyType::Dynamic;
    }
    if (const AssetNode* n = physicsNode.find("Shape"))
    {
        const oc::string& type = n->asString();
        if (type == "Sphere")       info->shape.type = EPhysicsShapeType::Sphere;
        else if (type == "Capsule") info->shape.type = EPhysicsShapeType::Capsule;
        else if (type == "Hull")    info->shape.type = EPhysicsShapeType::Hull;
        else if (type == "Mesh")    info->shape.type = EPhysicsShapeType::Mesh;
        else                        info->shape.type = EPhysicsShapeType::Box;
    }
    PhysicsShape& shape = info->shape;

    // Hull/Mesh pull their geometry from the sibling render mesh's container.
    if (shape.type == EPhysicsShapeType::Hull)
    {
        if (!containerName.empty() && ensureCollisionSource(containerName))
            shape.hullPoints = m_collision.buildHullPoints(containerName, nodePath);
        if (shape.hullPoints.size() < 4)
        {
            Log::warning("Scene: entity '" + ownerName + "' has a Hull physics shape but no render geometry, using Box");
            shape.type = EPhysicsShapeType::Box;
        }
    }
    else if (shape.type == EPhysicsShapeType::Mesh)
    {
        if (!containerName.empty() && ensureCollisionSource(containerName))
            info->mesh = m_collision.getOrBuildMesh(containerName, nodePath);
        if (info->mesh)
        {
            shape.mesh = info->mesh.get();
            info->occluders = m_collision.getOccluders(containerName, nodePath); // static mesh colliders double as occlusion occluders
        }
        else
        {
            Log::warning("Scene: entity '" + ownerName + "' has a Mesh physics shape but no render geometry, using Box");
            shape.type = EPhysicsShapeType::Box;
        }
    }
    if (const AssetNode* n = physicsNode.find("HalfExtents")) shape.halfExtents = n->asVec3(shape.halfExtents);
    if (const AssetNode* n = physicsNode.find("Radius"))      shape.radius = n->asFloat(0, shape.radius);
    if (const AssetNode* n = physicsNode.find("HalfHeight"))  shape.halfHeight = n->asFloat(0, shape.halfHeight);
    if (const AssetNode* n = physicsNode.find("Offset"))      shape.offset = n->asVec3(shape.offset);
    if (const AssetNode* n = physicsNode.find("Density"))     shape.density = n->asFloat(0, shape.density);
    if (const AssetNode* n = physicsNode.find("Friction"))    shape.friction = n->asFloat(0, shape.friction);
    if (const AssetNode* n = physicsNode.find("Restitution")) shape.restitution = n->asFloat(0, shape.restitution);
    if (const AssetNode* n = physicsNode.find("Layer"))
    {
        info->layer = n->asString();
        shape.categoryBits = PhysicsLayers::bit(info->layer);
    }
    if (const AssetNode* n = physicsNode.find("CollidesWith"))
    {
        uint64 mask = 0;
        for (size_t i = 0; i < n->numValues(); ++i)
        {
            const oc::string& name = n->asString(i);
            if (name == "All")        mask = PhysicsLayers::All;
            else if (name != "None")  mask |= PhysicsLayers::bit(name);
            info->collidesWith.push_back(name);
        }
        shape.maskBits = mask;
    }
    if (const AssetNode* n = physicsNode.find("Group"))       shape.groupIndex = n->asInt();
    if (const AssetNode* n = physicsNode.find("MaxHullVertices")) shape.maxHullVertices = n->asInt(0, shape.maxHullVertices);
    if (const AssetNode* n = physicsNode.find("LockRotation"))  info->lockRotation = n->asBool();
    if (const AssetNode* n = physicsNode.find("Sensor"))        shape.isSensor = n->asBool();
    if (const AssetNode* n = physicsNode.find("ContactEvents")) shape.contactEvents = n->asBool();
    if (const AssetNode* n = physicsNode.find("Enabled"))     info->enabled = n->asBool();
    return info;
}

oc::shared_ptr<AudioBuffer> World::getOrLoadAudioBuffer(const oc::string& path)
{
    if (auto it = m_audioBuffers.find(path); it != m_audioBuffers.end())
        return it->second;
    auto buffer = oc::make_shared<AudioBuffer>(Globals::audio.loadSound(path));
    m_audioBuffers.emplace(path, buffer);
    return buffer;
}

oc::shared_ptr<AudioComponent::SpawnInfo> World::buildAudioSpawnInfo(const AssetNode& audioNode, const oc::string& ownerName)
{
    auto info = oc::make_shared<AudioComponent::SpawnInfo>();
    for (const AssetNode& soundNode : audioNode.children)
    {
        if (!keyIs(soundNode, "Sound"))
            continue;
        AudioComponent::SoundDesc sound;
        sound.alias = soundNode.asString();
        if (const AssetNode* n = soundNode.find("Select"))
            sound.select = audioSelectFromToken(n->asString());

        // Each `Path` child is one clip; its own child lines (Volume/Pitch/...) are the clip's settings.
        for (const AssetNode& pathNode : soundNode.children)
        {
            if (!keyIs(pathNode, "Path"))
                continue;
            AudioComponent::Clip clip;
            clip.path = pathNode.asString();
            if (clip.path.empty())
                continue;
            if (const AssetNode* n = pathNode.find("Volume"))            clip.volume = n->asFloat(0, clip.volume);
            if (const AssetNode* n = pathNode.find("Pitch"))             clip.pitch = n->asFloat(0, clip.pitch);
            if (const AssetNode* n = pathNode.find("Loop"))              clip.loop = n->asBool();
            if (const AssetNode* n = pathNode.find("Relative"))          clip.relative = n->asBool();
            if (const AssetNode* n = pathNode.find("ReferenceDistance")) clip.referenceDistance = n->asFloat(0, clip.referenceDistance);
            if (const AssetNode* n = pathNode.find("MaxDistance"))       clip.maxDistance = n->asFloat(0, clip.maxDistance);
            if (const AssetNode* n = pathNode.find("Rolloff"))           clip.rolloff = n->asFloat(0, clip.rolloff);
            clip.buffer = getOrLoadAudioBuffer(clip.path); // may be invalid (load failure logged); alias stays triggerable as a no-op
            sound.clips.push_back(oc::move(clip));
        }
        if (sound.alias.empty() || sound.clips.empty())
        {
            Log::warning("Scene: entity '" + ownerName + "' has an audio Sound entry without an alias or Path, skipping");
            continue;
        }
        info->sounds.push_back(oc::move(sound));
    }
    if (info->sounds.empty())
        return nullptr;
    return info;
}

void World::buildTemplate(const AssetNode& node, EntitySpawnTemplate& tmpl)
{
    tmpl.defaultTransform = readNodeTransform(node); // the declaration's authored placement
    const AssetNode* nameNode = node.find("Name");
    tmpl.displayName = nameNode ? nameNode->asString() : node.asString();
    if (const AssetNode* n = node.find("Enabled")) tmpl.enabled = n->asBool();
    if (const AssetNode* n = node.find("Global")) tmpl.global = n->asBool();

    uint16 typeBits = 0;

    static_assert(EComponentID_Scene == 0);
    if (const AssetNode* sceneNode = findComponentNode(node, "Scene"))
        if (oc::shared_ptr<SceneComponent::SpawnInfo> info = buildSceneSpawnInfo(*sceneNode))
        {
            typeBits |= uint16(1 << EComponentID_Scene);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    // A Hull/Mesh physics shape (parsed below) sources its geometry from the render container, so the
    // collision snapshot is captured while the container's scene data is loaded — one import, not two.
    const AssetNode* physicsNode = findComponentNode(node, "Physics");
    bool wantsCollisionGeometry = false;
    if (physicsNode)
        if (const AssetNode* n = physicsNode->find("Shape"))
            wantsCollisionGeometry = n->asString() == "Hull" || n->asString() == "Mesh";

    oc::string renderContainerName; // physics hull/mesh shapes (below), and the animator, source from here
    oc::string renderNodePath;
    if (const AssetNode* renderNode = findComponentNode(node, "Render"))
    {
        if (m_headless)
        {
            // no RenderComponent (no ObjectContainer load — that path is all renderer), but a Hull/Mesh
            // physics shape still needs to know WHERE its geometry lives: the names are enough, the
            // geometry itself comes from ensureCollisionSource's renderer-free import below
            if (const AssetNode* containerNode = renderNode->find("ObjectContainer"))
            {
                renderContainerName = containerNode->asString();
                renderNodePath = renderNode->find("Node") ? renderNode->find("Node")->asString() : oc::string();
            }
        }
        else if (oc::shared_ptr<RenderComponent::SpawnInfo> info = buildRenderSpawnInfo(*renderNode, tmpl.displayName, wantsCollisionGeometry))
        {
            renderContainerName = info->containerName;
            renderNodePath = info->nodePath;
            typeBits |= uint16(1 << EComponentID_Render);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
    }

    if (const AssetNode* animatorNode = findComponentNode(node, "Animator"); animatorNode && !m_headless)
        if (oc::shared_ptr<AnimatorComponent::SpawnInfo> info = buildAnimatorSpawnInfo(*animatorNode, renderContainerName, tmpl.displayName))
        {
            typeBits |= uint16(1 << EComponentID_Animator);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    if (physicsNode) // found above, before the render container load
    {
        typeBits |= uint16(1 << EComponentID_Physics);
        tmpl.spawnInfos.emplace_back(buildPhysicsSpawnInfo(*physicsNode, renderContainerName, renderNodePath, tmpl.displayName));
    }

    // Headless drops everything below except Network/Script: Audio would load buffers on an
    // uninitialized device, Particle/Force/Light create renderer emitter slots / push to mapped
    // buffers. Scripts reaching an absent component no-op through the null-handle ABI contract.
    if (const AssetNode* audioNode = findComponentNode(node, "Audio"); audioNode && !m_headless)
        if (oc::shared_ptr<AudioComponent::SpawnInfo> info = buildAudioSpawnInfo(*audioNode, tmpl.displayName))
        {
            typeBits |= uint16(1 << EComponentID_Audio);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    if (const AssetNode* particleNode = findComponentNode(node, "Particle"); particleNode && !m_headless)
    {
        auto info = oc::make_shared<ParticleComponent::SpawnInfo>();
        if (const AssetNode* n = particleNode->find("Effect"))   info->effectPath = n->asString();
        if (const AssetNode* n = particleNode->find("Emitting")) info->emitting = n->asBool(0, true);
        if (!info->effectPath.empty())
        {
            typeBits |= uint16(1 << EComponentID_Particle);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
        else
            Log::warning("Scene: entity '" + tmpl.displayName + "' has a Particle component without an Effect path, skipping");
    }

    if (const AssetNode* forceNode = findComponentNode(node, "Force"); forceNode && !m_headless)
    {
        auto info = oc::make_shared<ForceComponent::SpawnInfo>();
        if (const AssetNode* n = forceNode->find("Team"))         info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = forceNode->find("Direction"))    info->direction = n->asVec3(info->direction);
        if (const AssetNode* n = forceNode->find("Offset"))       info->offset = n->asVec3(info->offset);
        if (const AssetNode* n = forceNode->find("Output"))       info->output = n->asFloat(0, info->output);
        if (const AssetNode* n = forceNode->find("Reach"))        info->reach = n->asFloat(0, info->reach);
        if (const AssetNode* n = forceNode->find("Focus"))        info->focus = n->asFloat(0, info->focus);
        if (const AssetNode* n = forceNode->find("Distribution")) info->distribution = n->asFloat(0, info->distribution);
        if (const AssetNode* n = forceNode->find("Width"))        info->width = n->asFloat(0, info->width);
        if (const AssetNode* n = forceNode->find("Centered"))     info->centered = n->asBool(0, true);
        if (const AssetNode* n = forceNode->find("Mergeable"))    info->mergeable = n->asBool(0, true);
        if (const AssetNode* n = forceNode->find("AnalyticReadback")) info->analyticReadback = n->asBool(0, true);
        typeBits |= uint16(1 << EComponentID_Force);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    if (const AssetNode* lightNode = findComponentNode(node, "Light"); lightNode && !m_headless)
    {
        auto info = oc::make_shared<LightComponent::SpawnInfo>();
        if (const AssetNode* n = lightNode->find("Debug")) info->debugDraw = n->asBool();
        // One "Light <Type>" child per light; a component with none is pointless, so it's skipped below.
        for (const AssetNode& entry : lightNode->children)
        {
            if (!keyIs(entry, "Light"))
                continue;
            LightComponent::LightDesc desc;
            desc.type = lightTypeFromToken(entry.asString());
            if (const AssetNode* n = entry.find("Offset"))       desc.offset = n->asVec3(desc.offset);
            if (const AssetNode* n = entry.find("Direction"))    desc.direction = n->asVec3(desc.direction);
            if (const AssetNode* n = entry.find("Color"))        desc.color = n->asVec3(desc.color);
            if (const AssetNode* n = entry.find("Intensity"))    desc.intensity = n->asFloat(0, desc.intensity);
            if (const AssetNode* n = entry.find("Range"))        desc.range = n->asFloat(0, desc.range);
            if (const AssetNode* n = entry.find("ConeAngle"))    desc.coneAngle = n->asFloat(0, desc.coneAngle);
            if (const AssetNode* n = entry.find("EdgeSoftness")) desc.edgeSoftness = n->asFloat(0, desc.edgeSoftness);
            if (const AssetNode* n = entry.find("Width"))        desc.width = n->asFloat(0, desc.width);
            if (const AssetNode* n = entry.find("Radius"))       desc.width = n->asFloat(0, desc.width); // Tube spells it Radius
            if (const AssetNode* n = entry.find("Height"))       desc.height = n->asFloat(0, desc.height);
            if (const AssetNode* n = entry.find("Length"))       desc.length = n->asFloat(0, desc.length);
            if (const AssetNode* n = entry.find("Rotation"))     desc.rotation = n->asFloat(0, desc.rotation);
            if (const AssetNode* n = entry.find("Enabled"))      desc.enabled = n->asBool(0, true);
            info->lights.push_back(desc);
        }
        if (!info->lights.empty())
        {
            typeBits |= uint16(1 << EComponentID_Light);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
        else
            Log::warning("Scene: entity '" + tmpl.displayName + "' has a Light component without any Light entries, skipping");
    }

    if (findComponentNode(node, "Network")) // pure presence: netIds are assigned in code, never authored
    {
        typeBits |= uint16(1 << EComponentID_Network);
        tmpl.spawnInfos.emplace_back(oc::make_shared<NetworkComponent::SpawnInfo>());
    }

    // GAME components (Components/Game/*.ixx) — !m_headless like Force: they create Force
    // queries/read readbacks, and --game refuses headless anyway.
    if (const AssetNode* unitNode = findComponentNode(node, "GameUnit"); unitNode && !m_headless)
    {
        auto info = oc::make_shared<GameUnitComponent::SpawnInfo>();
        if (const AssetNode* n = unitNode->find("Team"))          info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = unitNode->find("Puppet"))        info->puppet = n->asBool();
        if (const AssetNode* n = unitNode->find("ShortName"))     info->shortName = n->asString();
        if (const AssetNode* n = unitNode->find("HealthMax"))     info->healthMax = n->asFloat(0, info->healthMax);
        if (const AssetNode* n = unitNode->find("EnergyMax"))     info->energyMax = n->asFloat(0, info->energyMax);
        if (const AssetNode* n = unitNode->find("ShieldOutput"))  info->shieldOutput = n->asFloat(0, info->shieldOutput);
        if (const AssetNode* n = unitNode->find("MoveSpeed"))     info->moveSpeed = n->asFloat(0, info->moveSpeed);
        if (const AssetNode* n = unitNode->find("Accel"))         info->accel = n->asFloat(0, info->accel);
        if (const AssetNode* n = unitNode->find("AttackRange"))   info->attackRange = n->asFloat(0, info->attackRange);
        if (const AssetNode* n = unitNode->find("AttackInterval")) info->attackInterval = glm::max(n->asFloat(0, info->attackInterval), 0.05f);
        if (const AssetNode* n = unitNode->find("AttackDamage"))  info->attackDamage = n->asFloat(0, info->attackDamage);
        if (const AssetNode* n = unitNode->find("PlayerDamage"))  info->playerDamage = n->asFloat(0, info->playerDamage);
        if (const AssetNode* n = unitNode->find("EmitterDrain"))  info->emitterDrain = n->asFloat(0, info->emitterDrain);
        if (const AssetNode* n = unitNode->find("Ranged"))        info->ranged = n->asBool();
        if (const AssetNode* n = unitNode->find("StandoffRange")) info->standoffRange = n->asFloat(0, info->standoffRange);
        if (const AssetNode* n = unitNode->find("FireInterval"))  info->fireInterval = n->asFloat(0, info->fireInterval);
        if (const AssetNode* n = unitNode->find("ShotKind"))      info->shotKind = (uint8)glm::clamp(n->asInt(), 0, 255);
        if (const AssetNode* n = unitNode->find("AlwaysDisplayHealth")) info->alwaysDisplayHealth = n->asBool();
        if (const AssetNode* n = unitNode->find("HeightLimit"))   info->heightLimit = n->asFloat(0, info->heightLimit);
        typeBits |= uint16(1 << EComponentID_GameUnit);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }
    if (const AssetNode* structNode = findComponentNode(node, "GameStructure"); structNode && !m_headless)
    {
        auto info = oc::make_shared<GameStructureComponent::SpawnInfo>();
        if (const AssetNode* n = structNode->find("Team"))         info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = structNode->find("HealthMax"))    info->healthMax = n->asFloat(0, info->healthMax);
        if (const AssetNode* n = structNode->find("Invulnerable")) info->invulnerable = n->asBool();
        if (const AssetNode* n = structNode->find("MeleeRadius"))  info->meleeRadius = n->asFloat(0, info->meleeRadius);
        if (const AssetNode* n = structNode->find("AlwaysDisplayHealth")) info->alwaysDisplayHealth = n->asBool();
        if (const AssetNode* n = structNode->find("AlwaysShowResources")) info->alwaysShowResources = n->asBool();
        typeBits |= uint16(1 << EComponentID_GameStructure);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }
    if (const AssetNode* projNode = findComponentNode(node, "GameProjectile"); projNode && !m_headless)
    {
        auto info = oc::make_shared<GameProjectileComponent::SpawnInfo>();
        if (const AssetNode* n = projNode->find("Team"))            info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = projNode->find("UnitDamage"))      info->unitDamage = n->asFloat(0, info->unitDamage);
        if (const AssetNode* n = projNode->find("StructureDamage")) info->structureDamage = n->asFloat(0, info->structureDamage);
        if (const AssetNode* n = projNode->find("Lifetime"))        info->lifetime = n->asFloat(0, info->lifetime);
        if (const AssetNode* n = projNode->find("EmitterDrain"))    info->emitterDrain = n->asFloat(0, info->emitterDrain);
        if (const AssetNode* n = projNode->find("EmitterDrainRadius")) info->emitterDrainRadius = n->asFloat(0, info->emitterDrainRadius);
        if (const AssetNode* n = projNode->find("SplashRadius"))    info->splashRadius = n->asFloat(0, info->splashRadius);
        typeBits |= uint16(1 << EComponentID_GameProjectile);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    if (const AssetNode* scriptNode = findComponentNode(node, "Script"))
    {
        auto info = oc::make_shared<ScriptComponent::SpawnInfo>();
        if (const AssetNode* n = scriptNode->find("Path"))    info->scriptPath = n->asString();
        if (const AssetNode* n = scriptNode->find("Enabled")) info->enabled = n->asBool();
        // Authored INITIAL values for the script's exposed fields, one "<name> <value>" child each. Kept as
        // TEXT: the script hasn't compiled at this point, so nothing here knows what type any of them are --
        // ScriptComponent::applyInitialValues resolves them against the module's field table once it has.
        if (const AssetNode* dataNode = scriptNode->find("Data"))
            for (const AssetNode& fieldNode : dataNode->children)
            {
                // Re-joined with ", ": the parser splits a vector value into separate entries, and the text form
                // is what applyInitialValues parses back -- the same spelling the writer produces.
                oc::string text;
                for (const oc::string& value : fieldNode.values)
                    text += (text.empty() ? "" : ", ") + value;
                info->initialValues.push_back({ fieldNode.key, oc::move(text) });
            }
        typeBits |= uint16(1 << EComponentID_Script);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    tmpl.archetype = makeEntityArchetype(typeBits);
}

oc::shared_ptr<const EntitySpawnTemplate> World::cacheTemplate(const oc::string& name, const oc::string& sourceFile, const AssetNode& node)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
        return it->second;

    m_buildingTemplates.insert(name);
    auto tmpl = oc::make_shared<EntitySpawnTemplate>();
    tmpl->sourceFile = sourceFile;
    tmpl->prefabName = name; // a registered prefab name, so a re-serialized instance writes "Prefab <name>"
    buildTemplate(node, *tmpl);
    m_buildingTemplates.erase(name);

    m_templates.emplace(name, tmpl); // heap-owned: address stays stable for child back-pointers
    return tmpl;
}

oc::shared_ptr<const EntitySpawnTemplate> World::buildInlineTemplate(const AssetNode& node)
{
    auto tmpl = oc::make_shared<EntitySpawnTemplate>();
    buildTemplate(node, *tmpl);
    return tmpl;
}

oc::shared_ptr<const EntitySpawnTemplate> World::buildFileTemplate(const oc::string& path)
{
    AssetNode doc;
    oc::string error;
    if (!loadAssetFile(path, doc, error))
    {
        Log::warning("Scene: asset load failed: " + error);
        return nullptr;
    }

    for (const AssetNode& decl : doc.children)
        if (keyIs(decl, "Prefab"))
            return cacheTemplate(decl.asString(), path, decl);

    Log::warning("Scene: asset '" + path + "' declared no prefab");
    return nullptr;
}

oc::shared_ptr<const EntitySpawnTemplate> World::getOrBuildPrefabTemplate(const oc::string& name)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
        return it->second; // cache hit: no asset file touched

    if (oc::contains(m_buildingTemplates, name))
    {
        Log::warning("Scene: prefab cycle detected at '" + name + "', skipping");
        return nullptr;
    }

    if (const oc::string* prefabPath = Globals::assetRegistry.findPrefab(name))
    {
        if (oc::shared_ptr<const EntitySpawnTemplate> tmpl = buildFileTemplate(*prefabPath))
            return tmpl; // its declared root name == `name`
        Log::warning("Scene: prefab '" + name + "' not declared in '" + *prefabPath + "', skipping");
        return nullptr;
    }

    Log::warning("Scene: references unknown prefab '" + name + "', skipping");
    return nullptr;
}

EntityPtr World::spawnAssetFile(const oc::string& path, const Transform& base, bool overrideDefaultTransform)
{
    // Runtime callers pass Assets/-relative names ("Entities/Game/x.pre") — pure LEXICAL
    // normalization, no filesystem hit: this runs PER SPAWN (the co-op wave trickle spawns dozens
    // of units per frame through NpcSystem::service, and relativePath() resolves both sides
    // through weakly_canonical — a per-spawn syscall on the main thread). Only an ABSOLUTE path
    // (editor drag/drop) still resolves against the working directory; the registry lookup below
    // normalizes its keys, so the lexical form matches it.
    oc::string fileName = FileSystem::isAbsolute(path)
        ? FileSystem::relativePath(path, oc::string(), /*allowMainThread*/ true)
        : FileSystem::normalize(path);
    if (fileName.empty())
        fileName = path;

    const oc::string* rootName = Globals::assetRegistry.findRootForFile(fileName);
    oc::shared_ptr<const EntitySpawnTemplate> tmpl = rootName ? getOrBuildPrefabTemplate(*rootName) : buildFileTemplate(fileName);
    if (!tmpl)
        return EntityPtr{};

    const Transform& dt = tmpl->defaultTransform;
    // Override replaces the POSITION and COMPOSES the caller's rotation onto the authored default
    // (identity callers keep the authored rotation exactly). The rotation used to be dropped
    // outright — aimed Lances and replicated spawns silently spawned with the prefab default.
    const glm::vec3 pos = overrideDefaultTransform ? base.pos : dt.pos;
    const glm::quat quat = overrideDefaultTransform ? base.quat * dt.quat : dt.quat;
    return Entity::create(*tmpl, Transform(pos, dt.scale, quat));
}

EntityPtr World::createEmptyEntity(const oc::string& name)
{
    // A blank template with NO components (archetype 0) and no prefabName: Entity::create leaves
    // prefabInstance false, so the entity is editable and serializes inline. It has no
    // SceneComponent, so it cannot hold CHILDREN — a group root must come from a prefab with
    // `Component Scene` (Entities/Game/terrainroot.pre is the pattern). Cached (and kept across
    // reloadPrefabs) so its address stays stable for the entities that point at it.
    if (!m_emptyTemplate)
    {
        m_emptyTemplate = oc::make_shared<EntitySpawnTemplate>();
        m_emptyTemplate->archetype = makeEntityArchetype(0);
        m_emptyTemplate->displayName = "Entity";
    }
    EntityPtr entity = Entity::create(*m_emptyTemplate, Transform());
    entity->setName(name);
    return entity;
}

void World::handleEntityChange(EntityChange& change, const Camera& camera, const Rect& viewportRect)
{
    ProfileScope profileScope("Entity change", EProfileCategory::Entity);
    if (auto* cv = oc::get_if<EntityChange::CreateViewport>(&change.type))
    {
        const glm::vec3 worldPos = camera.screenToWorld(viewportRect, cv->screenPos);
        addRootEntity(spawnAssetFile(cv->path, Transform(worldPos, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))));
    }
    else if (auto* ch = oc::get_if<EntityChange::CreateHierarchy>(&change.type))
    {
        EntityPtr e = spawnAssetFile(ch->path, Transform(), false);
        if (ch->parent && hasComponent<SceneComponent>(ch->parent))
            e->reparentEntity(ch->parent);   // parent's SceneComponent takes ownership
        else
            addRootEntity(oc::move(e));
    }
    else if (auto* se = oc::get_if<EntityChange::SetEnabled>(&change.type))
    {
        if (se->entity)
            se->entity->setEnabled(se->enabled);
    }
    else if (auto* sap = oc::get_if<EntityChange::SpawnAtPosition>(&change.type))
        addRootEntity(spawnAssetFile(sap->path, Transform(sap->position, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))));
    else if (auto* as = oc::get_if<EntityChange::AddSceneEntity>(&change.type))
    {
        EntityPtr e = createEmptyEntity(as->displayName);
        if (as->parent && hasComponent<SceneComponent>(as->parent))
            e->reparentEntity(as->parent);
        else
            addRootEntity(oc::move(e));
    }
    else if (auto* del = oc::get_if<EntityChange::Delete>(&change.type))
        removeRootEntity(del->entity.get());
    else if (auto* rep = oc::get_if<EntityChange::Reparent>(&change.type))
        rep->newParent ? removeRootEntity(rep->entity.get()) : addRootEntity(oc::move(rep->entity));
    else if (auto* sp = oc::get_if<EntityChange::SavePrefab>(&change.type))
    {
        if (savePrefab(sp->root.get(), sp->path, sp->text))
            invalidatePrefab(FileSystem::stem(sp->path));
    }
    else if (auto* op = oc::get_if<EntityChange::OpenPrefabForEdit>(&change.type))
    {
        EntityPtr e = spawnAssetFile(op->path, Transform(), false);
        if (e)
        {
            e->setPrefabInstance(false); // unpack: the Entity Editor edits it freely
            addRootEntity(e);
            if (m_onPrefabOpened)
                m_onPrefabOpened(e, op->path);
        }
    }
    else if (auto* np = oc::get_if<EntityChange::NewPrefab>(&change.type))
    {
        EntityPtr e = createEmptyEntity(np->displayName);
        addRootEntity(e);
        if (m_onPrefabOpened)
            m_onPrefabOpened(e, "");
    }
    else if (auto* rs = oc::get_if<EntityChange::RespawnEntity>(&change.type))
    {
        // Entity::create() only stores a raw, non-owning pointer to the template — keep it alive for
        // as long as the entity might reference it (World's own template caches do the same for
        // prefabs; this one is ad-hoc, so nothing else would hold onto it).
        keepTemplateAlive(rs->tmpl);

        Transform t(rs->oldEntity->pos, rs->oldEntity->scale, rs->oldEntity->rot);
        // Pre-set the frozen flag so component spawn already sees it — the respawn attaches to its parent
        // only further down, so create() can't inherit it from there.
        const uint8 initialFlags = rs->oldEntity->isFrozen() ? uint8(EEntityFlag_Frozen) : uint8(0);
        EntityPtr newEntity = Entity::create(*rs->tmpl, t, initialFlags);
        newEntity->setPrefabInstance(rs->oldEntity->isPrefabInstance()); // keep the editor's unpacked state despite the template's prefabName

        // Preserve any existing children (the Entity Editor commits one entity's own component set at
        // a time; whatever was already parented under it stays put).
        if (SceneComponent* oldSc = getComponent<SceneComponent>(rs->oldEntity.get()))
            if (SceneComponent* newSc = getComponent<SceneComponent>(newEntity.get()))
            {
                newSc->children = oc::move(oldSc->children);
                for (EntityPtr& child : newSc->children)
                    child->parent = newEntity.get();
            }

        // Re-attach where the old entity was: a root (in m_rootEntities) or a child (in its parent's
        // SceneComponent::children) — replace it in place so siblings/order aren't disturbed.
        if (Entity* parent = rs->oldEntity->parent)
        {
            newEntity->parent = parent;
            if (SceneComponent* parentSc = getComponent<SceneComponent>(parent))
                for (EntityPtr& child : parentSc->children)
                    if (child.get() == rs->oldEntity.get())
                    {
                        child = newEntity;
                        break;
                    }
        }
        else
        {
            oc::erase_if(m_rootEntities, [&](const EntityPtr& e) { return e.get() == rs->oldEntity.get(); });
            addRootEntity(newEntity);
        }

        if (m_onEntityRespawned)
            m_onEntityRespawned(rs->oldEntity, newEntity);
    }
}
