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
// set comes from one sphere query per focus point (radius[2] + queryMargin) plus the Global and
// freshly added roots; a visited parent emits only stamped children. The decision is PER ENTITY
// in the World's batch job: an entity is THROTTLED when it carries a following kind and no
// pinning kind (a kind with follow = false pins the entity to full rate while selected); the
// bubble gate and the dormant physics edge apply to every selected entity by distance. The
// entity itself only sees the delta it is handed (0 = skipped frame: sync + placement, no sim
// step). Full account in Code/Entity/CONTEXT.md.
export struct SimLodConfig
{
    bool enabled = true;
    bool horizontal = true;      // XZ distance (top-down game); off = full 3D distance
    float radius[3] = { 25.0f, 50.0f, 100.0f }; // tier t applies while dist < radius[t]; beyond radius[2] = dormant
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
    int visibleMaxTier = 2;      // TICK-RATE floor for an in-view entity inside the outer radius (0 = full
                                 // rate on screen); never affects the bubble gate or dormancy, which go
                                 // by distance alone. 2 = distance rules everything (default)
    float queryMargin = 10.0f;   // the selection query reaches radius[2] + this, so an entity LEAVING the
                                 // outer tier is still visited once in the band with no tier stamp
                                 // (= dormant) and takes its dormancy edge (units park their body)
    int maxCatchUp = 8;          // cap on the frames of dt a resumed tick receives
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
    // Whether the last pass selected by spatial query (else everything was visited). The Game's
    // far tick for unselected units keys on it.
    bool simLodActive() const { return m_simLodActive; }

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
        m_pendingRoots.push_back(e);
        if (e->isGlobal())
            m_globalRoots.push_back(e);
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
        oc::erase_if(m_pendingRoots, [entity](const Entity* e) { return e == entity; });
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
                // Shared game-layer contact behavior (Components/GameComponents.ixx): a projectile
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
    void updateBatchJob(uint32 begin, uint32 count);
    void submitEntityBatches(const EntityUpdateNode* nodes, uint32 count);
    // SIM LOD decision for one node of the pass, from the entity's own spatial pass mask (the
    // UpdateTier stamps + Main; worker-safe: writes the entity's sched bytes and this worker's
    // staging counters only). Returns the delta to hand updateSelf: the frame delta (full rate),
    // the accumulated catch-up (a throttled entity's tick frame), 0 (skipped frame — the entity is
    // visited for its sync/placement but takes no sim step) or < 0 = DORMANT: do not visit the
    // entity or its subtree at all this frame.
    float simLodDelta(Entity& entity);
    // Its three pieces: the tiers from the entity's own spatial stamps (placed = a real tier
    // stamp exists — a fresh unlinked entry only carries the spawn guard), the dormant / wake
    // transitions (PhysicsComponent park/unpark), and the time-based tick cadence.
    struct SimLodTiers
    {
        uint8 dist;  // 0..2 by the tier balls, 3 = beyond the outer radius (dormant)
        uint8 tick;  // dist floored by "Visible max tier" for an in-view entity
        bool placed;
    };
    SimLodTiers simLodTiers(const Entity& entity) const;
    void simLodTransition(Entity& entity, uint8 tier);
    float simLodCadence(Entity& entity, uint8 tier);
    // Update SELECTION (see update()): whether a child a visited parent emitted is part of this
    // frame's pass — its spatial mask carries a tier or Main stamp (a never-stamped fresh entry
    // counts as stamped). Everything when the LOD is inactive.
    bool simLodSelected(const Entity& entity) const;
    void selectUpdateRoot(Entity* hit); // walks a query hit up to its root, stamping the ancestors

    uint64 m_updateFrame = 0; // salts the per-entity random re-measure below
    oc::vector<EntityUpdateNode> m_updateLevel; // root gather scratch
    // Frame arena for in-flight batch nodes: claimed with an atomic bump (NEVER rolled back),
    // pointer-stable during the pass (resized only between frames, from last frame's use +
    // overflow). A claim past the end runs that batch's subtrees serially instead (correct, just
    // not parallel) and grows the arena next frame.
    oc::vector<EntityUpdateNode> m_updateArena;
    oc::atomic<uint32> m_updateArenaCursor = 0;
    oc::atomic<uint32> m_updateArenaOverflow = 0;
    Renderer* m_updateRenderer = nullptr; // pass-scoped: shrinks every batch job's capture to 16 bytes
    float m_updateDelta = 0.0f;
    uint32 m_updateBudget = 1;            // cost units per batch (~25us), computed once per pass
    JobCounter m_updateCounter;           // every batch job in the pass, incl. ones batches spawn
    PerWorker<EntityUpdateStaging> m_updateStaging;
    JobCost m_updateCost{ 2000 };
    SimLodConfig m_simLod;
    glm::dvec3 m_simLodFocus[MaxSimLodFocus]; // dvec3: the index API's type (query centers, tier stamps)
    uint32 m_simLodFocusCount = 0;
    uint16 m_simLodFollowMask = 0; // per pass: sim kinds that follow the LOD
    uint16 m_simLodPinMask = 0;    // per pass: sim kinds that pin their entity to full rate
    bool m_simLodActive = false;   // per pass: selection by spatial query + stamps (else every root, every child)
    // Cumulative sim time at the end of each of the last 256 passes (indexed by m_updateFrame):
    // the time a throttled entity's tick covers = now - ring[frame of its last tick], from its
    // schedSkipped count alone (no per-entity clock — Entity stays one cache line).
    float m_frameTimeRing[256] = {};
    float m_simTimeAccum = 0.0f;
    oc::vector<Entity*> m_globalRoots;  // EEntityFlag_Global roots: always visited
    oc::vector<Entity*> m_pendingRoots; // roots added since the last pass: visited once unconditionally
    oc::vector<uint64> m_queryScratch;  // selection query hits (Entity* as userData)
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
