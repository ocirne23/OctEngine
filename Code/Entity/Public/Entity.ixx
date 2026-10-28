export module Entity:Entity;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import RendererVK.fwd;
import Spatial;

import :EntityState; // the state, the per-entity visit and the tree-allocation helpers

// `CullMode` in the .pre (entity level). Lives on the template - the entity reads it through spawnTemplate.
export enum class EEntityCullMode : uint8
{
    PerEntity, // every entity owns a spatial entry and culls itself
    RootOnly,  // only this entity registers, over the render bounds of its whole spawned subtree; the descendants register nothing and render under its pass mask
    None,      // no spatial entry: never culled, always visited, invisible to spatial queries
};

export enum EComponentID : uint16
{
    EComponentID_Scene    = 0,
    EComponentID_Render   = 1,
    EComponentID_Animator = 2,
    EComponentID_Physics  = 3,
    EComponentID_Audio    = 4,
    EComponentID_Particle = 5,
    EComponentID_Force    = 6,
    EComponentID_Light    = 7,
    EComponentID_Network  = 8,
    EComponentID_GameUnit = 9,
    EComponentID_GameStructure = 10,
    EComponentID_GameProjectile = 11,
    EComponentID_HumanoidAnimator = 12,
    EComponentID_Script   = 13, // should be last so all other components are available on spawn
};

// THE ENTITY: one cache line, its components appended in the same allocation (see Component.ixx). The local transform (pos / scale / rot) is plain public
// data in EntityState; everything else is read through the accessors below. The state is in EntityState.ixx; the multi-line bodies are below the types.
export class Entity final : public EntityState
{
public:

    // ---- Spawning ----

    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags = 0);
    // A tree NOT in the World, visited serially by its owner (the gizmo entity): main thread, outside the parallel pass.
    void updateDetached(Renderer& renderer, float deltaSeconds);

    // ---- The tree ----

    Entity* getParent() const { return parent; }
    void reparentEntity(Entity* newParent);
    void detachFromOwner(); // deletion path: leaves the parent's children list and breaks the tree allocation (conservative)

    // ---- Identity ----

    const EntitySpawnTemplate* getSpawnTemplate() const { return spawnTemplate; }
    uint16 getTypeBits() const { return typeBits; } // the component types it carries, 1 << EComponentID
    const char* getName() const; // "" when unnamed; a name set after the spawn is owned by EntityNameRegistry, else the template's displayName
    bool hasName() const;
    void setName(oc::string_view name);
    const oc::string& getPrefabName() const;
    const oc::string& getSourceFile() const;

    // ---- State ----

    bool isEnabled() const { return (flags & EEntityFlag_Enabled) != 0; }
    void setEnabled(bool on) { flags = on ? uint8(flags | EEntityFlag_Enabled) : uint8(flags & ~EEntityFlag_Enabled); }
    bool isFrozen() const { return (flags & EEntityFlag_Frozen) != 0; }
    void setFrozen(bool on); // applies to the whole subtree
    bool isGlobal() const { return (flags & EEntityFlag_Global) != 0; }
    bool isPrefabInstance() const { return (flags & EEntityFlag_PrefabInstance) != 0; }
    void setPrefabInstance(bool on) { flags = on ? uint8(flags | EEntityFlag_PrefabInstance) : uint8(flags & ~EEntityFlag_PrefabInstance); }
    bool isPrefabLocked() const;
    Entity* nearestPrefabInstance();

    // ---- Culling ----

    EEntityCullMode getCullMode() const;
    const SpatialEntry& getSpatialEntry() const { return spatialEntry; } // invalid: no entry (CullMode None, or covered by a RootOnly root)
    // The entity whose spatial entry culls this one: itself, the RootOnly ancestor covering it, or null (never culled).
    const Entity* getCullOwner() const;

private:

    friend class EntityState; // create / destroy

    Entity() = default;
    ~Entity() = default;
    Entity(const Entity&) = delete;
};
static_assert(sizeof(Entity) <= 64);

// Intrusive refcounted handle: the last release destroys the entity.
export struct EntityPtr
{
    EntityPtr() = default;
    explicit EntityPtr(Entity* entity) : m_entity(entity) { addRef(); }
    EntityPtr(const EntityPtr& other) : m_entity(other.m_entity) { addRef(); }
    EntityPtr(EntityPtr&& other) noexcept : m_entity(other.m_entity) { other.m_entity = nullptr; }
    EntityPtr& operator=(const EntityPtr& other);
    EntityPtr& operator=(EntityPtr&& other) noexcept;
    ~EntityPtr() { release(); }

    Entity* get() const { return m_entity; }
    Entity* operator->() const { return m_entity; }
    Entity& operator*() const { return *m_entity; }
    explicit operator bool() const { return m_entity != nullptr; }
    operator Entity*() const { return m_entity; }
    void release();

private:

    void addRef()
    {
        if (m_entity)
            oc::atomic_ref<uint16>(m_entity->refCount).fetch_add(1);
    }

    Entity* m_entity = nullptr;
};

export struct EntityArchetype
{
    uint16 allocSize = 0;
    uint16 typeBits = 0;
};

export EntityArchetype makeEntityArchetype(uint16 typeBits);

// What Entity::create spawns from: the archetype plus one SpawnInfo recipe per component (World builds them from the .pre; the Entity Editor and the
// testbed assemble their own).
export struct EntitySpawnTemplate
{
    EntityArchetype archetype;
    Transform defaultTransform;
    oc::vector<oc::shared_ptr<void>> spawnInfos;
    oc::string sourceFile;
    oc::string prefabName;
    oc::string displayName;
    bool enabled = true;              // spawns with EEntityFlag_Enabled set/cleared ("Enabled" in the .pre)
    bool global = false;              // root spawns with EEntityFlag_Global ("Global true" in the .pre)
    EEntityCullMode cullMode = EEntityCullMode::PerEntity; // "CullMode" in the .pre
    // RootOnly lazy cache: the render bounds of the whole spawned subtree in this entity's LOCAL space (the same for every instance). State 0 = uncomputed,
    // 1 = no render node, 2 = valid; stored LAST, same benign-race scheme as treeAllocSize (every writer stores the same values).
    mutable Sphere treeCullBounds{ glm::vec3(0.0f), 0.0f };
    mutable uint32 treeCullBoundsState = 0;
    mutable uint32 treeAllocSize = 0; // lazy cache: entity + recursive SceneComponent children, 0 = uncomputed
    // Lazy cache: NetworkComponents in the whole tree, UINT32_MAX = uncomputed (same benign-race scheme as treeAllocSize). Entity::create needs it for server
    // netId contiguity: only a tree that mints MORE THAN ONE id must hold the manager's register lock across the whole spawn - single-component trees (every
    // unit/projectile prefab) mint atomically and stay parallel.
    mutable uint32 treeNetworkCount = UINT32_MAX;
};

// Deferred entity change events, thread safe API.
export struct EntityChange
{
    struct CreateHierarchy
    {
        oc::string path;
        EntityPtr parent; // nullptr = World root
    };
    struct CreateViewport
    {
        glm::ivec2 screenPos;
        oc::string path;
    };
    struct AddSceneEntity
    {
        oc::string displayName;
        EntityPtr parent;
    };
    struct SpawnAtPosition
    {
        oc::string path;
        glm::vec3   position = glm::vec3(0.0f);
    };
    struct Delete
    {
        EntityPtr entity;
    };
    struct Reparent
    {
        EntityPtr entity;
        EntityPtr newParent; // nullptr = root
    };
    struct SavePrefab
    {
        EntityPtr   root;
        oc::string path;
        oc::string text; // pre-serialized document (Entity Editor's transform draft); empty → serialize root
    };
    struct OpenPrefabForEdit
    {
        oc::string path; // .pre to load and unpack for the Prefab Editor
    };
    struct NewPrefab
    {
        oc::string displayName; // blank editable entity, name only (no source file yet)
    };
    struct RespawnEntity
    {
        EntityPtr oldEntity;
        oc::shared_ptr<const EntitySpawnTemplate> tmpl; // freshly assembled from the entity's edited component set
    };
    struct SetEnabled
    {
        EntityPtr entity;
        bool enabled;
    };
    oc::variant<CreateHierarchy, CreateViewport, AddSceneEntity, SpawnAtPosition, Delete, Reparent, SavePrefab, OpenPrefabForEdit, NewPrefab, RespawnEntity, SetEnabled> type;
};

// ---- Inline bodies ----

inline EntityPtr& EntityPtr::operator=(const EntityPtr& other)
{
    if (this != &other)
    {
        release();
        m_entity = other.m_entity;
        addRef();
    }
    return *this;
}

inline EntityPtr& EntityPtr::operator=(EntityPtr&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_entity = other.m_entity;
        other.m_entity = nullptr;
    }
    return *this;
}

inline void EntityPtr::release()
{
    if (!m_entity)
        return;
    if (oc::atomic_ref<uint16>(m_entity->refCount).fetch_sub(1) == 1)
        Entity::destroy(m_entity);
    m_entity = nullptr;
}
