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

// Per-worker scratch for the depth-parallel entity update: the next level's nodes this worker found.
export struct EntityUpdateStaging
{
    std::vector<EntityUpdateNode> children;
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
    EntityPtr spawn(const std::string& name, const Transform& base);
    EntityPtr spawnAssetFile(const std::string& path, const Transform& base, bool overrideDefaultTransform = true);
    EntityPtr createEmptyEntity(const std::string& name);

    // Entity Ownership
    void addRootEntity(EntityPtr entity) { if (entity) m_rootEntities.push_back(std::move(entity)); }
    // Drops the World's ownership of a root entity (it dies here unless something else still holds it).
    void removeRootEntity(const Entity* entity) { std::erase_if(m_rootEntities, [entity](const EntityPtr& e) { return e.get() == entity; }); }
    const std::vector<EntityPtr>& rootEntities() const { return m_rootEntities; }
    void clearRootEntities() { m_rootEntities.clear(); }

    // Applies one EntityChange event
    void handleEntityChange(EntityChange& change, const Camera& camera, const Rect& viewportRect);

    // Editor prefab editing
    void setOnPrefabOpened(std::function<void(const EntityPtr&, const std::string&)> callback) { m_onPrefabOpened = std::move(callback); }
    void setOnEntityRespawned(std::function<void(const EntityPtr&, const EntityPtr&)> callback) { m_onEntityRespawned = std::move(callback); }
    void reloadPrefabs();
    void invalidatePrefab(const std::string& name);

    // captureCollisionSource keeps a CPU snapshot of the scene geometry for physics
    ObjectContainer* getOrLoadContainer(const std::string& name, bool captureCollisionSource = false);
    size_t getNumContainers() const { return m_containers.size(); }

    // Keep old instances of edited templates alive (for pre-existing entities)
    void keepTemplateAlive(std::shared_ptr<const EntitySpawnTemplate> tmpl) { m_editorTemplates.push_back(std::move(tmpl)); }

    // Component SpawnInfo builders
    std::shared_ptr<RenderComponent::SpawnInfo> buildRenderSpawnInfo(const AssetNode& renderNode, const std::string& ownerName, bool captureCollisionSource = false);
    std::shared_ptr<AnimatorComponent::SpawnInfo> buildAnimatorSpawnInfo(const AssetNode& animatorNode, const std::string& siblingContainerName, const std::string& ownerName);
    std::shared_ptr<PhysicsComponent::SpawnInfo> buildPhysicsSpawnInfo(const AssetNode& physicsNode, const std::string& containerName, const std::string& nodePath, const std::string& ownerName);
    std::shared_ptr<AudioComponent::SpawnInfo> buildAudioSpawnInfo(const AssetNode& audioNode, const std::string& ownerName);

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

    std::shared_ptr<const EntitySpawnTemplate> getOrBuildPrefabTemplate(const std::string& name);

    std::shared_ptr<const EntitySpawnTemplate> cacheTemplate(const std::string& name, const std::string& sourceFile, const AssetNode& node);

    std::shared_ptr<const EntitySpawnTemplate> buildInlineTemplate(const AssetNode& node);

    std::shared_ptr<const EntitySpawnTemplate> buildFileTemplate(const std::string& path);

    void buildTemplate(const AssetNode& node, EntitySpawnTemplate& tmpl);

    std::shared_ptr<SceneComponent::SpawnInfo> buildSceneSpawnInfo(const AssetNode& sceneNode);

    // Audio buffer for a sound file, shared between every entity referencing the same path. A failed
    // load is cached too (as an invalid buffer) so a bad path doesn't retry + re-log every spawn.
    std::shared_ptr<AudioBuffer> getOrLoadAudioBuffer(const std::string& path);

    // Makes sure the cache holds a snapshot for this container: normally captured during loadContainer,
    // falling back to a one-time re-import when the container was loaded before physics asked for it.
    bool ensureCollisionSource(const std::string& containerName);

    std::unordered_map<std::string, std::unique_ptr<ObjectContainer>> m_containers;
    CollisionCache m_collision; // collision snapshots + BVHs + occluders derived from loaded containers
    std::unordered_map<std::string, std::unique_ptr<AnimationSet>> m_clipSets; // key: skeleton ptr + animator name
    std::unordered_map<std::string, std::shared_ptr<AudioBuffer>> m_audioBuffers; // key: sound file path
    std::unordered_map<std::string, std::shared_ptr<EntitySpawnTemplate>> m_templates; // prefab templates, keyed by name
    std::vector<std::shared_ptr<EntitySpawnTemplate>> m_retiredTemplates; // superseded by reloadPrefabs, kept alive for live entities
    std::unordered_set<std::string> m_buildingTemplates; // prefab names currently being built (cycle guard)
    std::shared_ptr<EntitySpawnTemplate> m_emptyTemplate; // blank Scene-only template for editable (non-prefab) entities
    std::vector<std::shared_ptr<const EntitySpawnTemplate>> m_editorTemplates; // ad-hoc templates kept alive via keepTemplateAlive()
    std::vector<EntityPtr> m_rootEntities;
    bool m_headless = false;
    std::vector<EntityUpdateNode> m_updateLevel;
    PerWorker<EntityUpdateStaging> m_updateStaging;
    JobCost m_updateCost{ 2000 };
    std::function<void(const EntityPtr&, const std::string&)> m_onPrefabOpened;
    std::function<void(const EntityPtr&, const EntityPtr&)> m_onEntityRespawned;
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
