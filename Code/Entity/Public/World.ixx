export module Entity:World;

import Core;
import Core.glm;
import Core.Transform;
import Core.Camera;
import Core.Rect;

import RendererVK;
import Physics;
import File;
import :Entity;
import :Component;
import :WorldState; // the caches, the template builders, the update pass and the SIM LOD

// THE WORLD (Globals::world): the root entities, spawning and the per-frame parallel update pass with its SIM LOD. SimLodConfig ("Game/Sim LOD") is
// Globals::settings.world.simLod (Settings.World). The state and the private helpers are in WorldState.ixx; the multi-line bodies are below the class.
export class World final : public WorldState
{
public:

    // ---- Lifetime and the frame ----

    bool initialize();
    void update(Renderer& renderer, float deltaSeconds);
    ~World(); // releases Globals::scriptEvents' undrained EntityChange queue, see World.cpp
    // The selection job is FIRE-AND-FORGET from the end of update(): main calls this right BEFORE the next frame's spatial kick (the commit inside it would
    // mutate the index under a still-running query) - normally a no-op, a real wait only when the job outlasted the frame.
    void joinSelection();
    // Headless server mode: set BEFORE any spawn. Templates then carry only Scene / Physics / Script / Network components - everything renderer-touching
    // and Audio is dropped at build time, so the pass never dereferences the (uninitialized) renderer and no GPU resource is ever created.
    void setHeadless(bool headless) { m_headless = headless; }

    // ---- SIM LOD ----

    // Focus points, set every frame BEFORE update() by whoever knows where the players are (GameMatch::update: every player capsule; main.cpp: the camera in
    // the plain testbed). No focus = no LOD (everything ticks at full rate).
    static constexpr uint32 MaxSimLodFocus = MaxSimLodFocusSlots;
    void setSimLodFocus(const glm::vec3* points, uint32 count);
    // Zones: spheres (xyz center, w radius) that stamp tier 1 and 2 only (SimLodConfig::zoneMargin). GameMatch publishes the friendly structures' bubble
    // spheres. Same timing as the focus; stays until set again.
    static constexpr uint32 MaxSimLodZones = MaxSimLodZoneSlots;
    void setSimLodZones(const glm::vec4* spheres, uint32 count);
    bool simLodActive() const { return m_simLodActive; } // the last pass selected by spatial query (else everything was visited)
    // The tier by direct distance to the focus points / zones (3 = none): what a never-stamped entity is scheduled by.
    int simLodDistanceTier(const glm::vec3& pos) const;
    // Whether a child a visited parent emitted is part of this frame's pass (its spatial mask carries a tier or Main stamp; a never-stamped fresh entry
    // counts as stamped). Everything when the LOD is inactive. An unselected entity is not visited: its owner updates it directly (a client's snapshot,
    // the game's far tick).
    bool simLodSelected(const Entity& entity) const;

    // ---- Spawning ----

    EntityPtr spawn(const oc::string& name, const Transform& base);
    EntityPtr spawnAssetFile(const oc::string& path, const Transform& base, bool overrideDefaultTransform = true);
    // The two halves of spawnAssetFile, for a caller that spawns the SAME file every frame (the projectile shots): resolve once, spawn from the template.
    // templateGeneration() changes whenever the prefabs reload and retire templates, so a holder re-resolves when it sees a new value.
    oc::shared_ptr<const EntitySpawnTemplate> resolveAssetTemplate(const oc::string& path);
    EntityPtr spawnTemplate(const EntitySpawnTemplate& tmpl, const Transform& base, bool overrideDefaultTransform = true);
    uint32 templateGeneration() const { return m_templateGeneration; }
    // NO components (archetype 0): editable, serializes inline - but it cannot hold children. A grouping root must come from a prefab with `Component Scene`.
    EntityPtr createEmptyEntity(const oc::string& name);
    void keepTemplateAlive(oc::shared_ptr<const EntitySpawnTemplate> tmpl) { m_editorTemplates.push_back(oc::move(tmpl)); } // an edited template's live instances

    // PARALLEL SPAWNING: a batch of prefabs with the Entity::create calls fanned out over the job system (main thread, in the window where spawning is
    // legal: the entity-change drains / game.update, outside the parallel entity pass). Templates resolve on main; results stay index-aligned with the
    // requests (unknown prefab = null entry); addRoots attaches every spawned root serially after the join.
    struct SpawnRequest
    {
        // A plain name resolves like spawn(); a name WITH an extension resolves like spawnAssetFile with overrideDefaultTransform = true.
        oc::string name;
        Transform transform;
    };
    oc::vector<EntityPtr> spawnBatch(oc::span<const SpawnRequest> requests, bool addRoots = true);
    // PARALLEL DESTRUCTION: releases a batch of handles, the resulting Entity::destroy calls fanned out (same window as spawnBatch). A handle that is not
    // the entity's LAST reference just decrements - drop every other owner (root list, rosters) BEFORE this, on main.
    void releaseBatch(oc::vector<EntityPtr>&& entities);

    // ---- The roots ----

    // A new root is also queued for ONE unconditional visit (its spatial entry links at the next commit), and a Global root - or one WITHOUT a spatial
    // entry - joins the always-visited list (no selection query can find it).
    void addRootEntity(EntityPtr entity);
    // Drops the World's ownership of a root (it dies here unless something else holds it). Notifies m_onRootEntityRemoved FIRST, while the entity is alive:
    // the Game rosters deregister through it, so EVERY removal path reaches them. The callback must not call removeRootEntity itself.
    void removeRootEntity(const Entity* entity);
    const oc::vector<EntityPtr>& rootEntities() const { return m_rootEntities; }
    // Drops EVERY root at once (no removal notifications - the callers are the teardowns, which reset their own holders first), as one parallel release.
    void clearRootEntities();

    // ---- Entity changes and editor hooks ----

    // Applies a drained queue (takeEntityChanges() temporaries); the Delete handles are released as one parallel batch afterwards.
    void handleEntityChanges(oc::vector<EntityChange>&& changes, const Camera& camera, const Rect& viewportRect);
    void setOnPrefabOpened(oc::function<void(const EntityPtr&, const oc::string&)> callback) { m_onPrefabOpened = oc::move(callback); }
    void setOnEntityRespawned(oc::function<void(const EntityPtr&, const EntityPtr&)> callback) { m_onEntityRespawned = oc::move(callback); }
    void setOnRootEntityRemoved(oc::function<void(const Entity*)> callback) { m_onRootEntityRemoved = oc::move(callback); } // the Game layer; cleared at its shutdown

    // ---- Assets (the Entity Editor builds its templates from these) ----

    // captureCollisionSource keeps a CPU snapshot of the scene geometry for physics.
    ObjectContainer* getOrLoadContainer(const oc::string& name, bool captureCollisionSource = false);
    // Lookup only, never imports: safe from the off-main UI pass (an import creates renderer resources).
    ObjectContainer* findLoadedContainer(const oc::string& name);
    oc::shared_ptr<RenderComponent::SpawnInfo> buildRenderSpawnInfo(const AssetNode& renderNode, const oc::string& ownerName, bool captureCollisionSource = false);
    oc::shared_ptr<AnimatorComponent::SpawnInfo> buildAnimatorSpawnInfo(const AssetNode& animatorNode, const oc::string& siblingContainerName, const oc::string& ownerName);
    oc::shared_ptr<PhysicsComponent::SpawnInfo> buildPhysicsSpawnInfo(const AssetNode& physicsNode, const oc::string& containerName, const oc::string& nodePath, const oc::string& ownerName);
    oc::shared_ptr<AudioComponent::SpawnInfo> buildAudioSpawnInfo(const AssetNode& audioNode, const oc::string& ownerName);

    // ---- Physics contacts ----

    // The contact callback (dispatchContactEvents, main, after the spatial and begin-frame joins): PhysicsComponent::onContact, the projectile, the script.
    void handleContactEvent(const PhysicsWorld::ContactEvent& evt);
};

export namespace Globals
{
// ~World destroys the root entities (their components need spatialIndex/physics/audio/renderer/networkManager and the job system's main-thread context -
// all destruct later, see InitSeg.h) and then the caches (ObjectContainers → Renderer::removeObjectContainer, audio buffers → Globals::audio). Members
// destruct in reverse declaration order, so m_rootEntities empties before the caches they reference.
OC_INIT_SEG(OC_SEG_WORLD)
    World world;
}

// ---- Inline bodies ----

inline void World::addRootEntity(EntityPtr entity)
{
    if (!entity)
        return;
    Entity* e = entity.get();
    m_rootEntities.push_back(oc::move(entity));
    if (alwaysVisited(*e))
        m_globalRoots.push_back(e);
    else
        m_pendingRoots.push_back({ e, m_updateFrame });
}

inline void World::removeRootEntity(const Entity* entity)
{
    if (m_onRootEntityRemoved)
        m_onRootEntityRemoved(entity);
    oc::erase_if(m_rootEntities, [entity](const EntityPtr& e) { return e.get() == entity; });
    oc::erase_if(m_pendingRoots, [entity](const PendingRoot& p) { return p.entity == entity; });
    oc::erase_if(m_globalRoots, [entity](const Entity* e) { return e == entity; });
}

inline void World::clearRootEntities()
{
    oc::vector<EntityPtr> roots = oc::move(m_rootEntities);
    m_rootEntities.clear();
    m_pendingRoots.clear();
    m_globalRoots.clear();
    releaseBatch(oc::move(roots));
}

inline void World::handleEntityChanges(oc::vector<EntityChange>&& changes, const Camera& camera, const Rect& viewportRect)
{
    if (changes.empty())
        return;
    ProfileScope profileScope("World EntityChanges", EProfileCategory::Entity);
    for (EntityChange& change : changes)
        handleEntityChange(change, camera, viewportRect);
    // The Delete changes were detached from the root list above (in order, callbacks fired); their queue handles are usually the LAST references, so the
    // actual teardown runs as one parallel batch instead of one by one when `changes` dies.
    oc::vector<EntityPtr> deletes;
    for (EntityChange& change : changes)
        if (auto* del = oc::get_if<EntityChange::Delete>(&change.type); del && del->entity)
            deletes.push_back(oc::move(del->entity));
    releaseBatch(oc::move(deletes));
}

inline void World::handleContactEvent(const PhysicsWorld::ContactEvent& evt)
{
    Entity* a = static_cast<Entity*>(evt.userDataA);
    Entity* b = static_cast<Entity*>(evt.userDataB);
    auto fire = [&](Entity* target, Entity* other)
    {
        if (PhysicsComponent* pc = getComponent<PhysicsComponent>(target); pc && pc->onContact)
            pc->onContact(*target, *other, evt.begin);
        // Shared game-layer contact behavior (Components/Game/GameProjectileComponent.ixx): a projectile spends itself on first touch and damages an
        // enemy-team victim.
        if (GameProjectileComponent* proj = getComponent<GameProjectileComponent>(target))
            proj->onContact(*target, *other, evt.begin);
        if (ScriptComponent* sc = getComponent<ScriptComponent>(target))
            sc->firePhysicsEvent(*target, other, evt.begin, evt.sensor, evt.contactId);
    };
    fire(a, b);
    fire(b, a);
}
