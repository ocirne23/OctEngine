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
};

export class World final
{
public:

    bool initialize();
    void update(Renderer& renderer, float deltaSeconds);

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
        oc::string name; // prefab name (AssetRegistry), same lookup as spawn()
        Transform transform;
    };
    oc::vector<EntityPtr> spawnBatch(oc::span<const SpawnRequest> requests, bool addRoots = true);
    EntityPtr spawnAssetFile(const oc::string& path, const Transform& base, bool overrideDefaultTransform = true);
    EntityPtr createEmptyEntity(const oc::string& name);

    // Entity Ownership
    void addRootEntity(EntityPtr entity) { if (entity) m_rootEntities.push_back(oc::move(entity)); }
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
    }
    const oc::vector<EntityPtr>& rootEntities() const { return m_rootEntities; }
    void clearRootEntities() { m_rootEntities.clear(); }

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
    JobCost m_spawnBatchCost{ 20000 }; // spawnBatch auto-grain seed (~20us/entity until measured)
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
