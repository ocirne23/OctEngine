export module Entity:WorldState;

import Core;
import Core.glm;
import Core.Transform;
import Core.Camera;
import Core.Rect;

import RendererVK;
import Animation;
import Physics;
import Audio;
import :EntityState;
import :Entity;
import :Component;
import :CollisionCache; // the asset -> physics bridge is World's business

import File;
import Spatial;
import Threading;
import Settings;
import :AnimationDescription;

export class World; // declared before its use in self()
export class NetworkManager;

// Per-worker scratch for the parallel entity update: the children the batch currently running on this worker emitted. Consumed (sliced into new batch
// jobs) before the batch job returns, so one slot per worker suffices - a batch job never fiber-waits.
export struct EntityUpdateStaging
{
    oc::vector<EntityUpdateNode> children;
    uint32 simLodCount[4] = {}; // entities classified per SIM LOD tier this pass (summed after the join)
};

// Everything of World but its public API (World.ixx): the caches, the template builders, the parallel update pass and the SIM LOD selection. A helper
// reaches the API through self().
export class WorldState
{
protected:

    friend class NetworkManager; // reads isHeadless

    template<typename W = World> W& self() { return static_cast<W&>(*this); }
    template<typename W = World> const W& self() const { return static_cast<const W&>(*this); }

    // Outside the SIM LOD: a Global root (scheduling) or an entity with no spatial entry (CullMode None / covered by a RootOnly root - no query can find
    // it). Full rate, never throttled; as a ROOT it lives in m_globalRoots.
    static bool alwaysVisited(const Entity& entity) { return entity.isGlobal() || !entity.getSpatialEntry().isValid(); }
    bool isHeadless() const { return m_headless; }

    // ---- Templates and caches ----

    void reloadPrefabs();
    void invalidatePrefab(const oc::string& name);
    ObjectContainer* loadContainer(const ObjectContainerDesc& desc, bool captureCollisionSource);
    // Builds (or returns a cached) clip library for an animator, retargeted against `skel`. Cached by skeleton + animator name so a source FBX is imported
    // once, not per spawned entity.
    const AnimationSet* getOrBuildClipSet(const Skeleton* skel, const AnimatorDesc& desc);
    oc::shared_ptr<const EntitySpawnTemplate> getOrBuildPrefabTemplate(const oc::string& name);
    oc::shared_ptr<const EntitySpawnTemplate> cacheTemplate(const oc::string& name, const oc::string& sourceFile, const AssetNode& node);
    oc::shared_ptr<const EntitySpawnTemplate> buildInlineTemplate(const AssetNode& node);
    oc::shared_ptr<const EntitySpawnTemplate> buildFileTemplate(const oc::string& path);
    void buildTemplate(const AssetNode& node, EntitySpawnTemplate& tmpl);
    oc::shared_ptr<SceneComponent::SpawnInfo> buildSceneSpawnInfo(const AssetNode& sceneNode);
    oc::shared_ptr<SceneAnimatorComponent::SpawnInfo> buildSceneAnimatorSpawnInfo(const AssetNode& node, const oc::string& ownerName);
    oc::shared_ptr<HumanoidAnimatorComponent::SpawnInfo> buildHumanoidAnimatorSpawnInfo(const AssetNode& node, const oc::string& ownerName); // null = the setup does not fit
    void appendSceneAnimBones(const AssetNode& node, int32 parent, BoneModelRig& rig, const oc::string& ownerName);
    // Audio buffer for a sound file, shared between every entity referencing the same path. A failed load is cached too (as an invalid buffer) so a bad
    // path doesn't retry + re-log every spawn.
    oc::shared_ptr<AudioBuffer> getOrLoadAudioBuffer(const oc::string& path);
    // Makes sure the cache holds a snapshot for this container: normally captured during loadContainer, falling back to a one-time re-import when the
    // container was loaded before physics asked for it.
    bool ensureCollisionSource(const oc::string& containerName);
    void handleEntityChange(EntityChange& change, const Camera& camera, const Rect& viewportRect); // one EntityChange (handleEntityChanges runs them all)

    oc::unordered_map<oc::string, oc::unique_ptr<ObjectContainer>> m_containers;
    CollisionCache m_collision; // collision snapshots + BVHs + occluders derived from loaded containers
    oc::unordered_map<oc::string, oc::unique_ptr<AnimationSet>> m_clipSets; // key: skeleton ptr + animator name
    oc::unordered_map<oc::string, oc::shared_ptr<AudioBuffer>> m_audioBuffers; // key: sound file path
    oc::unordered_map<oc::string, oc::shared_ptr<EntitySpawnTemplate>> m_templates; // prefab templates, keyed by name
    oc::vector<oc::shared_ptr<EntitySpawnTemplate>> m_retiredTemplates; // superseded by reloadPrefabs, kept alive for live entities
    oc::unordered_set<oc::string> m_buildingTemplates; // prefab names currently being built (cycle guard)
    oc::shared_ptr<EntitySpawnTemplate> m_emptyTemplate; // blank Scene-only template for editable (non-prefab) entities
    uint32 m_templateGeneration = 1; // bumped by reloadPrefabs / invalidatePrefab: cached resolveAssetTemplate results re-resolve
    oc::vector<oc::shared_ptr<const EntitySpawnTemplate>> m_editorTemplates; // ad-hoc templates kept alive via keepTemplateAlive()
    oc::vector<EntityPtr> m_rootEntities;
    bool m_headless = false;

    // ---- The parallel update pass ----
    // CONTINUATION-BATCH entity update (see update()): one batch job processes a node range from the arena and immediately slices the children it emitted
    // into new batch jobs - no level barrier, a child's only dependency is its own parent, which just finished.

    void updateBatchJob(const EntityUpdateNode* nodes, uint32 count); // nodes: arena or spill, pointer-stable for the pass
    // With inlineNodes/inlineCount the FIRST batch is handed back for the caller to run inline (the batch job's continuation) and only the rest are
    // submitted; returns false when nothing was handed back. Without them every batch is submitted.
    bool submitEntityBatches(const EntityUpdateNode* nodes, uint32 count, const EntityUpdateNode** inlineNodes = nullptr, uint32* inlineCount = nullptr);
    // The root sources go to the workers in slices of rootSliceSize (see World.cpp): a slice job runs submitEntityBatches on its range; a visible slice
    // first walks its handles to roots.
    static constexpr uint32 rootSliceSize = 256;
    void submitRootSlices(const EntityUpdateNode* nodes, uint32 count);
    void submitVisibleRootSlices();

    // ---- SIM LOD ----

    // SIM LOD decision for one node of the pass, from the entity's own spatial pass mask (the UpdateTier stamps + Main; worker-safe: writes the entity's
    // sched bytes and this worker's staging counters only). Returns the delta to hand updateSelf: the frame delta (full rate), the accumulated catch-up (a
    // throttled entity's tick frame), 0 (skipped frame - the entity is visited for its sync/placement but takes no sim step) or < 0 = DORMANT: do not visit
    // the entity or its subtree at all this frame.
    float simLodDelta(Entity& entity);
    // Its three pieces: the tiers - from the entity's own spatial stamps once the periodic job has placed it, else (fresh, or never inside a ball) by direct
    // distance to the focus points and zones - the dormant / wake transitions (PhysicsComponent park/unpark), and the time-based tick cadence.
    struct SimLodTiers
    {
        uint8 dist;  // 0..2 by the tier balls, 3 = beyond the outer radius (dormant)
        uint8 tick;  // dist floored by "Visible max tier" for an in-view entity
    };
    SimLodTiers simLodTiers(const Entity& entity) const; // simLodDistanceTier (public) is the fallback while no stamp exists
    void simLodTransition(Entity& entity, uint8 tier);
    float simLodCadence(Entity& entity, uint8 tier);
    // THE SELECTION runs as a POST-UPDATE job (computeSelection) every "Selection interval" passes: it opens a new UpdateTier stamp generation, then ONE
    // traversal per sphere (a focus point or a zone) on a parallelFor stamps every hit's tiers by distance band, walks it up to its root (stamping the
    // ancestors) and records the root - the tier stamps are the job's, not the cull job's, so the stamps stay current until the next selection. ROOT DEDUPE
    // is a stamp too: the walk's atomic UpdateRoot stamp lets exactly one sphere record a shared root (no sort, no merge), and the same stamp tells update()
    // which visible / pending roots the result already holds. The result carries each root's spatial handle (liveness proof at use - a root may die in
    // between) and is reused, dead roots dropped, until the next job replaces it.
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
    void computeSelection(SelectResult& out); // the job body (also the first LOD frame's inline fallback)
    // A hit up to its root, ancestors stamped; the root is recorded once per generation of `rootPass` (UpdateRoot from the job, VisibleRoot from the
    // per-pass visible walk, which also skips roots the current periodic result holds).
    void selectUpdateRoot(Entity* hit, ESpatialPass rootPass, oc::vector<Entity*>& roots);
    void buildSelectSpheres(oc::vector<SelectSphere>& out) const; // focus + zones -> the job's inputs (at kick)
    // The SIM LOD's per-entity clock value for THIS pass (Entity::schedTick units: 1/64 s, 14 bits).
    uint32 schedTickNow() const { return uint32(m_simTimeAccum * EntityState::SchedTickHz) & EntityState::SchedTickMask; }

    SelectResult m_selectResult;
    JobCounter m_selectCounter;      // the in-flight selection job (submitted at the end of update, see joinSelection)
    uint64 m_selectKickFrame = 0;    // the pass that kicked the in-flight / last job (0 = never)
    float m_selectKickTime = 0.0f;   // sim time (m_simTimeAccum) at that kick: the interval clock
    // Roots added since the last selection that could see them: visited unconditionally every pass. A root added in pass f links at the commit of pass
    // f + 1 at the latest, so the job kicked at the end of pass k >= f + 1 finds it; that job's result retires it (and any root the result contains anyway -
    // a root added before pass k's commit is found AND still pending).
    struct PendingRoot
    {
        Entity* entity;
        uint64 frame;
    };

    uint64 m_updateFrame = 0; // salts the per-entity random re-measure
    oc::vector<EntityUpdateNode> m_updateLevel; // root gather scratch (LOD: the global + pending roots only)
    struct VisibleRootSlice // owner-sliced scratch of one visible-slice job: its handles walked to roots
    {
        oc::vector<Entity*> roots;
        oc::vector<EntityUpdateNode> nodes;
    };
    oc::vector<VisibleRootSlice> m_visibleRootSlices; // sized on main before the slice jobs go out
    // Frame arena for in-flight batch nodes: claimed with an atomic bump (NEVER rolled back), pointer-stable during the pass (resized only between frames,
    // from last frame's use + overflow). A claim past the end SPILLS into a block of its own (m_updateArenaSpill, allocated under the mutex - rare, the arena
    // is sized from last pass), so the pass stays parallel; the spill is freed between passes once the arena has grown to cover it.
    oc::vector<EntityUpdateNode> m_updateArena;
    oc::atomic<uint32> m_updateArenaCursor = 0;
    oc::atomic<uint32> m_updateArenaOverflow = 0;
    oc::vector<oc::unique_ptr<EntityUpdateNode[]>> m_updateArenaSpill;
    std::mutex m_updateArenaSpillMutex;
    Renderer* m_updateRenderer = nullptr; // pass-scoped: shrinks every batch job's capture to 16 bytes
    float m_updateDelta = 0.0f;
    uint32 m_updateBudget = 1;            // cost units per batch (~25us), computed once per pass
    uint64 m_batchTimeUs = 200;           // Target time in us per entity batch update
    JobCounter m_updateCounter;           // every batch job in the pass, incl. ones batches spawn
    PerWorker<EntityUpdateStaging> m_updateStaging;
    JobCost m_updateCost{ 2000 };
    const SimLodConfig& m_simLod = Globals::settings.world.simLod;
    static constexpr uint32 MaxSimLodFocusSlots = 16; // World::MaxSimLodFocus
    static constexpr uint32 MaxSimLodZoneSlots = 64;  // World::MaxSimLodZones
    glm::dvec3 m_simLodFocus[MaxSimLodFocusSlots]; // dvec3: the index API's type (query centers, tier stamps)
    uint32 m_simLodFocusCount = 0;
    glm::dvec4 m_simLodZone[MaxSimLodZoneSlots];  // xyz center, w radius
    uint32 m_simLodZoneCount = 0;
    uint16 m_simLodFollowMask = 0; // per pass: sim kinds that follow the LOD
    uint16 m_simLodPinMask = 0;    // per pass: sim kinds that pin their entity to full rate
    bool m_simLodActive = false;   // per pass: selection by spatial query + stamps (else every root, every child)
    float m_simTimeAccum = 0.0f;
    oc::vector<Entity*> m_globalRoots;  // EEntityFlag_Global roots: always visited
    oc::vector<PendingRoot> m_pendingRoots; // see PendingRoot
    int (&m_simLodStats)[4] = Globals::settings.world.simLodStats; // last pass's per-tier entity counts (live readout tweaks)
    JobCost m_spawnBatchCost{ 20000 }; // spawnBatch auto-grain seed (~20us/entity until measured)
    JobCost m_destroyBatchCost{ 10000 }; // releaseBatch auto-grain seed
    oc::function<void(const EntityPtr&, const oc::string&)> m_onPrefabOpened;
    oc::function<void(const EntityPtr&, const EntityPtr&)> m_onEntityRespawned;
    oc::function<void(const Entity*)> m_onRootEntityRemoved;
};
