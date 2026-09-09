export module Entity:World;

import Core;
import Core.glm;
import Core.Transform;
import Core.Camera;
import Core.Rect;

import RendererVK;
import Animation;
import Physics;
import Audio;
import :Entity;
import :Component;
import :CollisionCache; // deliberately not re-exported: the asset -> physics bridge is World's business

import File;
import Spatial;
import Threading;
import :AnimationDescription;

// Per-worker scratch for the parallel entity update: the children the batch currently running on
// this worker emitted. Consumed (sliced into new batch jobs) before the batch job returns, so one
// slot per worker suffices - a batch job never fiber-waits.
export struct EntityUpdateStaging
{
    oc::vector<EntityUpdateNode> children;
    uint32 simLodCount[4] = {}; // entities classified per SIM LOD tier this pass (summed after the join)
};

// SIM LOD ("Game/Sim LOD" tweaks, not Saved): which entities the update pass visits and how
// often their SIMULATION components tick, by the distance to the nearest FOCUS point (the
// players — the Game layer publishes them; the testbed uses the camera). The SpatialIndex stamps
// three UpdateTier passes (balls at radius[0..2] around every focus point); an entity's own
// stamps give its DISTANCE tier: 0 = every frame, 1/2 = time-based intervals, 3 = DORMANT
// (beyond radius[2]: not visited at all — no sim, no render push, subtree skipped). The visit
// set comes from one sphere query per focus point (radius[2] + queryMargin), run as a POST-UPDATE
// job for the NEXT pass (see computeSelection), plus the Global and freshly added roots; a visited
// parent emits only stamped children. The decision is PER ENTITY
// in the World's batch job: an entity is THROTTLED when it carries a following kind and no
// pinning kind (a kind with follow = false pins the entity to full rate while selected); the
// bubble gate and the dormant physics edge apply to every selected entity by distance. The
// entity itself only sees the delta it is handed (0 = skipped frame: sync + placement, no sim
// step). Full account in Code/Entity/CONTEXT.md.
export struct SimLodConfig
{
    bool enabled = true;
    bool horizontal = true;      // XZ distance (top-down game); off = full 3D distance
    float radius[3] = { 25.0f, 50.0f, 225.0f }; // tier t applies while dist < radius[t]; beyond radius[2] = dormant
    // Tick cadence for tier 1, tier 2, dormant: TIME-based (seconds between ticks; dormant 0 =
    // never) with a MINIMUM frame gap so a low frame rate still skips frames. The tick receives
    // the exact sim time it covers (World keeps a per-frame time ring; nothing on the entity).
    float intervalSec[3] = { 0.25f, 1.0f, 0.0f };
    int minFrames[3] = { 4, 16, 8 };
    float intervalJitter = 0.25f;  // per-entity +-fraction on the interval so a wave that entered a tier
                                   // together spreads out instead of ticking in lockstep
    bool dormantDisableBody = true; // dormant edge: DISABLE a throttled entity's physics body (out of the
                                    // broadphase + solver, pose kept — nothing can wake it) instead of
                                    // only parking it asleep; re-enabled on the wake edge either way
    int forceMaxTier = 1;          // a ForceComponent's bubble is ACTIVE only while its entity's tier
                                   // is <= this (3 = always); applies to every selected entity with a
                                   // bubble, throttled or not (structures included)
    int buoyancyMaxTier = 1;       // a dynamic PhysicsComponent body runs its buoyancy probes only
                                   // while its entity's tier is <= this (3 = always)
    int visibleMaxTier = 2;      // TICK-RATE floor for an in-view entity inside the outer radius (0 = full
                                 // rate on screen); never affects the bubble gate or dormancy, which go
                                 // by distance alone. 2 = distance rules everything (default)
    float queryMargin = 10.0f;   // the selection query reaches radius[2] + this, so an entity LEAVING the
                                 // outer tier is still visited once in the band with no tier stamp
                                 // (= dormant) and takes its dormancy edge (units park their body)
    // ZONES (setSimLodZones — the Game publishes the friendly forcefield bubbles): a zone stamps
    // tier 1 within its radius + zoneMargin and tier 2 over a further zoneTier2Band, never tier
    // 0, so a unit walking into a far base's field ticks (and is pushed) without a player near.
    float zoneMargin = 5.0f;
    float zoneTier2Band = 25.0f;
    // The selection job (the sphere queries + tier stamps) runs once this much SIM TIME has
    // passed since its last kick (frame-rate independent; at least one pass apart); in between
    // the last result is reused (dead roots dropped, new roots visited from the pending list).
    // NOT frame-sensitive: what the camera sees is selected every frame from the cull job's
    // frustum pass regardless. The query margin has to cover this much motion.
    float selectionIntervalSec = 0.05f;
    float maxCatchUpSec = 1.0f;  // cap in SECONDS (never frames: frame-rate bound) on the dt a throttled
                                 // tick receives — a tick gets its whole stretch, only a return from
                                 // dormancy is clipped
    bool units = true;           // GameUnitComponent follows the LOD
    bool structures = false;     // GameStructureComponent (barracks/turret clocks, flows)
    bool projectiles = false;    // GameProjectileComponent (lifetime, deflection)
    bool scripts = false;        // ScriptComponent Update
    bool animators = true;       // AnimatorComponent
};

export class World final
{
public:

    bool initialize();
    void update(Renderer& renderer, float deltaSeconds);

    // SIM LOD focus points (see SimLodConfig): set every frame BEFORE update() by whoever knows
    // where the players are — GameMatch::update publishes every player capsule, main.cpp the
    // camera in the plain testbed. No focus = no LOD (everything ticks at full rate).
    static constexpr uint32 MaxSimLodFocus = 16;
    void setSimLodFocus(const glm::vec3* points, uint32 count);
    // SIM LOD zones: spheres (xyz center, w radius) that stamp tier 1 and 2 only — see
    // SimLodConfig::zoneMargin. GameMatch publishes the friendly structures' bubble spheres (the
    // merge group's sphere where merged). Same timing as the focus; stays until set again.
    static constexpr uint32 MaxSimLodZones = 64;
    void setSimLodZones(const glm::vec4* spheres, uint32 count);
    // Whether the last pass selected by spatial query (else everything was visited). The Game's
    // far tick for unselected units keys on it.
    bool simLodActive() const { return m_simLodActive; }
    // The tier by direct distance to the focus points / zones (3 = none): what a never-stamped
    // entity is scheduled by (the game's far tick leaves such a unit to the pass).
    int simLodDistanceTier(const glm::vec3& pos) const;
    // Update SELECTION (see update()): whether a child a visited parent emitted is part of this
    // frame's pass — its spatial mask carries a tier or Main stamp (a never-stamped fresh entry
    // counts as stamped). Everything when the LOD is inactive. Public for the NetworkManager: an
    // unselected client entity gets its snapshot applied directly (the pass never visits it).
    bool simLodSelected(const Entity& entity) const;

    // Headless server mode: set BEFORE any spawn. Templates then carry only Scene/Physics/Script/
    // Network components — everything renderer-touching (Render/Animator/Light/Particle/Force) and
    // Audio is dropped at build time, so updateSelf never dereferences the (uninitialized) renderer
    // and no GPU resource is ever created. Hull/Mesh collision still works: the Render node's
    // container NAME is parsed textually and the geometry comes from the renderer-free
    // ensureCollisionSource import.
    void setHeadless(bool headless) { m_headless = headless; }
    bool isHeadless() const { return m_headless; }

    // Entity Creation
    EntityPtr spawn(const oc::string& name, const Transform& base);

    // PARALLEL ENTITY SPAWNING: spawns a batch of prefabs with the Entity::create calls fanned out
    // over the job system (main thread only, called in the window where spawning is legal today —
    // the entity-change drains / game.update, after the spatial/begin-frame joins and outside the
    // parallel entity pass). Templates resolve HERE on main (the template cache is not job-safe);
    // the jobs run only Entity::create, whose resource seams are all safe against concurrent
    // creates/destroys (spawn locks in Spatial/Physics/RendererVK/Force/Particle/Network/Script).
    // Results stay index-aligned with requests (unknown prefab = null entry). addRoots attaches
    // every spawned root to the root list serially after the join.
    struct SpawnRequest
    {
        // A plain name resolves like spawn() (prefab registry); a name WITH an extension
        // ("Entities/Game/swarmUnit.pre") resolves like spawnAssetFile with
        // overrideDefaultTransform = true (position replaced, rotation composed onto the authored
        // default, authored scale kept).
        oc::string name;
        Transform transform;
    };
    oc::vector<EntityPtr> spawnBatch(oc::span<const SpawnRequest> requests, bool addRoots = true);

    // PARALLEL ENTITY DESTRUCTION: releases a batch of handles with the resulting Entity::destroy
    // calls fanned out over the job system (same window/contract as spawnBatch). A handle that is
    // not the entity's LAST reference just decrements — callers drop every other owner they mean
    // to (root list, rosters) BEFORE this, on main, so the notifications (removeRootEntity's
    // callback) stay serial and only the teardown itself runs on workers. The Delete drain in
    // handleEntityChanges and NpcSystem::clear go through it.
    void releaseBatch(oc::vector<EntityPtr>&& entities);
    EntityPtr spawnAssetFile(const oc::string& path, const Transform& base, bool overrideDefaultTransform = true);
    // NO components (archetype 0): editable, serializes inline — but it cannot hold children.
    // A grouping root must come from a prefab with `Component Scene`.
    EntityPtr createEmptyEntity(const oc::string& name);

    // Entity Ownership. A new root is also queued for ONE unconditional visit (its spatial entry
    // links at the next commit, so the selection query cannot find it on its spawn frame), and a
    // Global root joins the always-visited list.
    void addRootEntity(EntityPtr entity)
    {
        if (!entity)
            return;
        Entity* e = entity.get();
        m_rootEntities.push_back(oc::move(entity));
        if (e->isGlobal())
            m_globalRoots.push_back(e);
        else
            m_pendingRoots.push_back({ e, m_updateFrame });
    }
    // Drops the World's ownership of a root entity (it dies here unless something else still holds it).
    // Notifies m_onRootEntityRemoved FIRST (the entity is still alive during the callback) — the Game
    // layer's rosters deregister through it, so EVERY removal path (editor delete, script destroy
    // request, network despawn) reaches them without any world-wide query. The callback must not call
    // removeRootEntity itself (reentrant erase_if); a remover that already deregistered just sees a
    // no-op callback.
    void removeRootEntity(const Entity* entity)
    {
        if (m_onRootEntityRemoved)
            m_onRootEntityRemoved(entity);
        oc::erase_if(m_rootEntities, [entity](const EntityPtr& e) { return e.get() == entity; });
        oc::erase_if(m_pendingRoots, [entity](const PendingRoot& p) { return p.entity == entity; });
        oc::erase_if(m_globalRoots, [entity](const Entity* e) { return e == entity; });
    }
    const oc::vector<EntityPtr>& rootEntities() const { return m_rootEntities; }
    void clearRootEntities() { m_rootEntities.clear(); m_pendingRoots.clear(); m_globalRoots.clear(); }

    // Applies one EntityChange event
    void handleEntityChange(EntityChange& change, const Camera& camera, const Rect& viewportRect);
    // Takes the drained queue by rvalue (callers pass takeEntityChanges() temporaries).
    void handleEntityChanges(oc::vector<EntityChange>&& changes, const Camera& camera, const Rect& viewportRect)
    {
        if (changes.empty())
            return;
        ProfileScope profileScope("World EntityChanges", EProfileCategory::Entity);
		for (EntityChange& change : changes)
			handleEntityChange(change, camera, viewportRect);
        // The Delete changes were detached from the root list above (in order, callbacks fired);
        // their queue handles are usually the LAST references, so the actual teardown runs as one
        // parallel batch instead of one by one when `changes` dies.
        oc::vector<EntityPtr> deletes;
        for (EntityChange& change : changes)
            if (auto* del = oc::get_if<EntityChange::Delete>(&change.type); del && del->entity)
                deletes.push_back(oc::move(del->entity));
        releaseBatch(oc::move(deletes));
    }
    // Editor prefab editing
    void setOnPrefabOpened(oc::function<void(const EntityPtr&, const oc::string&)> callback) { m_onPrefabOpened = oc::move(callback); }
    void setOnEntityRespawned(oc::function<void(const EntityPtr&, const EntityPtr&)> callback) { m_onEntityRespawned = oc::move(callback); }
    // See removeRootEntity. Registered by the Game layer (GameMatch); cleared at its shutdown.
    void setOnRootEntityRemoved(oc::function<void(const Entity*)> callback) { m_onRootEntityRemoved = oc::move(callback); }
    void reloadPrefabs();
    void invalidatePrefab(const oc::string& name);

    // captureCollisionSource keeps a CPU snapshot of the scene geometry for physics
    ObjectContainer* getOrLoadContainer(const oc::string& name, bool captureCollisionSource = false);
    // Lookup only, never imports: safe from the off-main UI pass (an import creates renderer
    // resources, which is main-thread work - see EntityEditor's container load requests).
    ObjectContainer* findLoadedContainer(const oc::string& name);
    size_t getNumContainers() const { return m_containers.size(); }

    // Keep old instances of edited templates alive (for pre-existing entities)
    void keepTemplateAlive(oc::shared_ptr<const EntitySpawnTemplate> tmpl) { m_editorTemplates.push_back(oc::move(tmpl)); }

    // Component SpawnInfo builders
    oc::shared_ptr<RenderComponent::SpawnInfo> buildRenderSpawnInfo(const AssetNode& renderNode, const oc::string& ownerName, bool captureCollisionSource = false);
    oc::shared_ptr<AnimatorComponent::SpawnInfo> buildAnimatorSpawnInfo(const AssetNode& animatorNode, const oc::string& siblingContainerName, const oc::string& ownerName);
    oc::shared_ptr<PhysicsComponent::SpawnInfo> buildPhysicsSpawnInfo(const AssetNode& physicsNode, const oc::string& containerName, const oc::string& nodePath, const oc::string& ownerName);
    oc::shared_ptr<AudioComponent::SpawnInfo> buildAudioSpawnInfo(const AssetNode& audioNode, const oc::string& ownerName);

    void handleContactEvent(const PhysicsWorld::ContactEvent& evt)
    {
        Entity* a = static_cast<Entity*>(evt.userDataA);
        Entity* b = static_cast<Entity*>(evt.userDataB);
        auto fire = [&](Entity* target, Entity* other)
            {
                if (PhysicsComponent* pc = getComponent<PhysicsComponent>(target); pc && pc->onContact)
                    pc->onContact(*other, evt.begin);
                // Shared game-layer contact behavior (Components/Game/GameProjectileComponent.ixx): a projectile
                // spends itself on first touch and damages an enemy-team victim.
                if (GameProjectileComponent* proj = getComponent<GameProjectileComponent>(target))
                    proj->onContact(*target, *other, evt.begin);
                if (ScriptComponent* sc = getComponent<ScriptComponent>(target))
                    sc->firePhysicsEvent(*target, other, evt.begin, evt.sensor, evt.contactId);
            };
        fire(a, b);
        fire(b, a);
    }

private:

    ObjectContainer* loadContainer(const ObjectContainerDesc& desc, bool captureCollisionSource);

    // Builds (or returns a cached) clip library for an animator, retargeted against `skel`. Cached by
    // skeleton + animator name so a source FBX is imported once, not per spawned entity.
    const AnimationSet* getOrBuildClipSet(const Skeleton* skel, const AnimatorDesc& desc);

    oc::shared_ptr<const EntitySpawnTemplate> getOrBuildPrefabTemplate(const oc::string& name);

    oc::shared_ptr<const EntitySpawnTemplate> cacheTemplate(const oc::string& name, const oc::string& sourceFile, const AssetNode& node);

    oc::shared_ptr<const EntitySpawnTemplate> buildInlineTemplate(const AssetNode& node);

    oc::shared_ptr<const EntitySpawnTemplate> buildFileTemplate(const oc::string& path);

    void buildTemplate(const AssetNode& node, EntitySpawnTemplate& tmpl);

    oc::shared_ptr<SceneComponent::SpawnInfo> buildSceneSpawnInfo(const AssetNode& sceneNode);

    // Audio buffer for a sound file, shared between every entity referencing the same path. A failed
    // load is cached too (as an invalid buffer) so a bad path doesn't retry + re-log every spawn.
    oc::shared_ptr<AudioBuffer> getOrLoadAudioBuffer(const oc::string& path);

    // Makes sure the cache holds a snapshot for this container: normally captured during loadContainer,
    // falling back to a one-time re-import when the container was loaded before physics asked for it.
    bool ensureCollisionSource(const oc::string& containerName);

    oc::unordered_map<oc::string, oc::unique_ptr<ObjectContainer>> m_containers;
    CollisionCache m_collision; // collision snapshots + BVHs + occluders derived from loaded containers
    oc::unordered_map<oc::string, oc::unique_ptr<AnimationSet>> m_clipSets; // key: skeleton ptr + animator name
    oc::unordered_map<oc::string, oc::shared_ptr<AudioBuffer>> m_audioBuffers; // key: sound file path
    oc::unordered_map<oc::string, oc::shared_ptr<EntitySpawnTemplate>> m_templates; // prefab templates, keyed by name
    oc::vector<oc::shared_ptr<EntitySpawnTemplate>> m_retiredTemplates; // superseded by reloadPrefabs, kept alive for live entities
    oc::unordered_set<oc::string> m_buildingTemplates; // prefab names currently being built (cycle guard)
    oc::shared_ptr<EntitySpawnTemplate> m_emptyTemplate; // blank Scene-only template for editable (non-prefab) entities
    oc::vector<oc::shared_ptr<const EntitySpawnTemplate>> m_editorTemplates; // ad-hoc templates kept alive via keepTemplateAlive()
    oc::vector<EntityPtr> m_rootEntities;
    bool m_headless = false;
    // CONTINUATION-BATCH entity update (see update()): one batch job processes a node range from
    // the arena and immediately slices the children it emitted into new batch jobs - no level
    // barrier, a child's only dependency is its own parent, which just finished.
    void updateBatchJob(const EntityUpdateNode* nodes, uint32 count); // nodes: arena or spill, pointer-stable for the pass
    // With inlineNodes/inlineCount the FIRST batch is handed back for the caller to run inline
    // (the batch job's continuation) and only the rest are submitted; returns false when nothing
    // was handed back. Without them every batch is submitted.
    bool submitEntityBatches(const EntityUpdateNode* nodes, uint32 count, const EntityUpdateNode** inlineNodes = nullptr, uint32* inlineCount = nullptr);
    // The root sources go to the workers in slices of rootSliceSize (see World.cpp): a slice job
    // runs submitEntityBatches on its range; a visible slice first walks its handles to roots.
    static constexpr uint32 rootSliceSize = 256;
    void submitRootSlices(const EntityUpdateNode* nodes, uint32 count);
    void submitVisibleRootSlices();
    // SIM LOD decision for one node of the pass, from the entity's own spatial pass mask (the
    // UpdateTier stamps + Main; worker-safe: writes the entity's sched bytes and this worker's
    // staging counters only). Returns the delta to hand updateSelf: the frame delta (full rate),
    // the accumulated catch-up (a throttled entity's tick frame), 0 (skipped frame — the entity is
    // visited for its sync/placement but takes no sim step) or < 0 = DORMANT: do not visit the
    // entity or its subtree at all this frame.
    float simLodDelta(Entity& entity);
    // Its three pieces: the tiers — from the entity's own spatial stamps once the periodic job has
    // placed it, else (fresh, or never inside a ball) by direct distance to the focus points and
    // zones — the dormant / wake transitions (PhysicsComponent park/unpark), and the time-based
    // tick cadence.
    struct SimLodTiers
    {
        uint8 dist;  // 0..2 by the tier balls, 3 = beyond the outer radius (dormant)
        uint8 tick;  // dist floored by "Visible max tier" for an in-view entity
    };
    SimLodTiers simLodTiers(const Entity& entity) const; // simLodDistanceTier (public) is the fallback while no stamp exists
    void simLodTransition(Entity& entity, uint8 tier);
    float simLodCadence(Entity& entity, uint8 tier);
    // THE SELECTION runs as a POST-UPDATE job (computeSelection) every "Selection interval"
    // passes: it opens a new UpdateTier stamp generation, then ONE traversal per sphere (a focus
    // point or a zone) on a parallelFor stamps every hit's tiers by distance band, walks it up to
    // its root (stamping the ancestors) and records the root — the tier stamps are the job's, not
    // the cull job's, so the stamps stay current until the next selection. ROOT DEDUPE is a stamp
    // too: the walk's atomic UpdateRoot stamp lets exactly one sphere record a shared root (no
    // sort, no merge), and the same stamp tells update() which visible / pending roots the result
    // already holds. The result carries each root's spatial handle (liveness proof at use — a root
    // may die in between) and is reused, dead roots dropped, until the next job replaces it.
    struct SelectSphere
    {
        glm::dvec3 center;
        float queryRadius;
        float tierRadius[3];
    };
    struct SelectResult
    {
        oc::vector<EntityUpdateNode> nodes;      // the roots, ready to submit (deduped by the UpdateRoot stamp, Global roots skipped)
        oc::vector<SpatialHandle> rootHandles;   // aligned with nodes
        oc::vector<SelectSphere> spheres;        // the job's inputs, built at kick from the focus + zones
        oc::vector<oc::vector<Entity*>> roots;   // owner-sliced per traversal chunk of the current sphere: the roots it won
        bool valid = false; // set by the job, consumed by the next update() (sequenced by the join)
    };
    SelectResult m_selectResult;
    JobCounter m_selectCounter;      // the in-flight selection job (submitted at the end of update, see joinSelection)
    uint64 m_selectKickFrame = 0;    // the pass that kicked the in-flight / last job (0 = never)
    float m_selectKickTime = 0.0f;   // sim time (m_simTimeAccum) at that kick: the interval clock
    // Roots added since the last selection that could see them: visited unconditionally every
    // pass. A root added in pass f links at the commit of pass f + 1 at the latest, so the job
    // kicked at the end of pass k >= f + 1 finds it; that job's result retires it (and any root the
    // result contains anyway — a root added before pass k's commit is found AND still pending).
    struct PendingRoot
    {
        Entity* entity;
        uint64 frame;
    };
public:
    // The selection job is FIRE-AND-FORGET from the end of update(): it gets the whole rest of the
    // frame instead of the present window. Main calls this right BEFORE the next frame's spatial
    // kick (the commit inside it would mutate the index under a still-running query) — normally a
    // no-op, a real wait only when the job outlasted the frame.
    void joinSelection();
private:
    void computeSelection(SelectResult& out); // the job body (also the first LOD frame's inline fallback)
    // A hit up to its root, ancestors stamped; the root is recorded once per generation of
    // `rootPass` (UpdateRoot from the job, VisibleRoot from the per-pass visible walk, which also
    // skips roots the current periodic result holds).
    void selectUpdateRoot(Entity* hit, ESpatialPass rootPass, oc::vector<Entity*>& roots);

    uint64 m_updateFrame = 0; // salts the per-entity random re-measure below
    oc::vector<EntityUpdateNode> m_updateLevel; // root gather scratch (LOD: the global + pending roots only)
    struct VisibleRootSlice // owner-sliced scratch of one visible-slice job: its handles walked to roots
    {
        oc::vector<Entity*> roots;
        oc::vector<EntityUpdateNode> nodes;
    };
    oc::vector<VisibleRootSlice> m_visibleRootSlices; // sized on main before the slice jobs go out
    // Frame arena for in-flight batch nodes: claimed with an atomic bump (NEVER rolled back),
    // pointer-stable during the pass (resized only between frames, from last frame's use +
    // overflow). A claim past the end SPILLS into a block of its own (m_updateArenaSpill, allocated
    // under the mutex - rare, the arena is sized from last pass), so the pass stays parallel; the
    // spill is freed between passes once the arena has grown to cover it.
    oc::vector<EntityUpdateNode> m_updateArena;
    oc::atomic<uint32> m_updateArenaCursor = 0;
    oc::atomic<uint32> m_updateArenaOverflow = 0;
    oc::vector<oc::unique_ptr<EntityUpdateNode[]>> m_updateArenaSpill;
    std::mutex m_updateArenaSpillMutex;
    Renderer* m_updateRenderer = nullptr; // pass-scoped: shrinks every batch job's capture to 16 bytes
    float m_updateDelta = 0.0f;
    uint32 m_updateBudget = 1;            // cost units per batch (~25us), computed once per pass
    JobCounter m_updateCounter;           // every batch job in the pass, incl. ones batches spawn
    PerWorker<EntityUpdateStaging> m_updateStaging;
    JobCost m_updateCost{ 2000 };
    SimLodConfig m_simLod;
    glm::dvec3 m_simLodFocus[MaxSimLodFocus]; // dvec3: the index API's type (query centers, tier stamps)
    uint32 m_simLodFocusCount = 0;
    glm::dvec4 m_simLodZone[MaxSimLodZones];  // xyz center, w radius
    uint32 m_simLodZoneCount = 0;
    void buildSelectSpheres(oc::vector<SelectSphere>& out) const; // focus + zones -> the job's inputs (at kick)
    uint16 m_simLodFollowMask = 0; // per pass: sim kinds that follow the LOD
    uint16 m_simLodPinMask = 0;    // per pass: sim kinds that pin their entity to full rate
    bool m_simLodActive = false;   // per pass: selection by spatial query + stamps (else every root, every child)
    float m_simTimeAccum = 0.0f;
    // The SIM LOD's per-entity clock value for THIS pass (Entity::schedTick units: 1/64 s, 14 bits).
    uint32 schedTickNow() const { return uint32(m_simTimeAccum * Entity::SchedTickHz) & Entity::SchedTickMask; }
    oc::vector<Entity*> m_globalRoots;  // EEntityFlag_Global roots: always visited
    oc::vector<PendingRoot> m_pendingRoots; // see PendingRoot
    int m_simLodStats[4] = {};     // last pass's per-tier entity counts (live readout tweaks)
    JobCost m_spawnBatchCost{ 20000 }; // spawnBatch auto-grain seed (~20us/entity until measured)
    JobCost m_destroyBatchCost{ 10000 }; // releaseBatch auto-grain seed
    oc::function<void(const EntityPtr&, const oc::string&)> m_onPrefabOpened;
    oc::function<void(const EntityPtr&, const EntityPtr&)> m_onEntityRespawned;
    oc::function<void(const Entity*)> m_onRootEntityRemoved;
};

export namespace Globals
{
// ~World destroys the root entities (their components need spatialIndex/physics/audio/renderer/
// networkManager and the job system's main-thread context — all destruct later, see InitSeg.h) and
// then the caches (ObjectContainers → Renderer::removeObjectContainer, audio buffers →
// Globals::audio). Members destruct in reverse declaration order, so m_rootEntities empties before
// the caches they reference.
OC_INIT_SEG(OC_SEG_WORLD)
    World world;
}
