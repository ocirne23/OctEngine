export module Entity:Entity;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File.fwd;
import RendererVK.fwd;
import Spatial;

export struct EntitySpawnTemplate;
export struct EntityPtr;
export class Entity;

// `CullMode` in the .pre (entity level). Lives on the template - the entity reads it through spawnTemplate.
export enum class EEntityCullMode : uint8
{
    PerEntity, // every entity owns a spatial entry and culls itself
    RootOnly,  // only this entity registers, over the render bounds of its whole spawned subtree; the
               // descendants register nothing and render under this entity's pass mask
    None,      // no spatial entry: never culled, always visited, invisible to spatial queries
};

// EntityUpdateNode::cullPassMask: no RootOnly ancestor covers this entity.
export constexpr uint32 EntityCullPass_Own = UINT32_MAX;

// One entity queued for the depth-parallel update pass: its parent's composed world transform.
export struct EntityUpdateNode
{
    Entity* entity = nullptr;
    Transform parentWorld;
    uint32 cullPassMask = EntityCullPass_Own; // the render pass mask of the RootOnly ancestor covering it
};

export enum EEntityFlags : uint8
{
    EEntityFlag_PrefabInstance = 1 << 0, // root of a locked prefab instance; cleared by "unpack"
    EEntityFlag_Enabled        = 1 << 1, // off = the entity and its whole subtree stop updating (see updateTree)
    EEntityFlag_Frozen         = 1 << 2, // scripts/physics/animator don't update this entity (Entity Editor documents);

    // A spawned prefab tree is ONE EntityAllocator block (see Entity::create). While the tree is intact
    // (no member reparented/deleted out, no external refs at teardown) the root frees the whole block in
    // one call and members skip their own free. A structural break SPLITS the allocation
    // (breakContiguousAllocation): path ancestors revert to per-entity freeing while every off-path
    // subtree becomes its own RootAllocation over its contiguous DFS range and keeps one-chunk freeing.
    // first entity of its allocation range; frees it while contiguous.
    EEntityFlag_RootAllocation       = 1 << 3,

    // slice still owned by an intact (sub)tree allocation
    EEntityFlag_ContiguousAllocation = 1 << 4, 

    // Latch for updateSelf: this entity's subtree has already been pulled from the simulation while
    // disabled, so the walk is not repeated every frame. Anything joining the subtree afterwards would
    // miss that walk, so reparentEntity clears it up the new ancestor chain.
    EEntityFlag_PhysicsSuspended     = 1 << 5,

    // OPT-IN per-entity ProfileScope in the parallel entity pass (named by the registry-owned name):
    // the interesting components set it at spawn (Animator/Force/Script/GameUnit), machine
    // structures (barracks/turret) latch it in their update - plain static scenery stays scope-
    // free so it cannot flood the profiler rings.
    EEntityFlag_Profiled             = 1 << 6,

    // GLOBAL root (`Global true` in the .pre, root only): the World visits it every frame from its
    // own list instead of finding it through the spatial index - for organisational roots whose
    // children spread across the world (the co-op terrain root). It still registers a spatial
    // entry (gameplay queries, render culling); its children are selected individually like
    // everything else. Never inherited by children.
    EEntityFlag_Global               = 1 << 7,
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

export class Entity
{
public:

    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags = 0);
    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags, uint8*& treeCursor, Entity* parent);
    static void destroy(Entity* entity);

public:

    glm::vec3 pos;
    float scale = 1.0f;
    glm::quat rot;

    Entity* parent = nullptr;
    const EntitySpawnTemplate* spawnTemplate = nullptr;
    SpatialEntry spatialEntry;

    uint16 refCount = 0;
    uint16 typeBits = 0;
    uint8 flags = 0;      // EEntityFlags bitmask
    uint8 updateCost = 0; // MEASURED updateSelf weight in 250ns units (0 = not yet measured): the

	uint16 schedTier : 2 = 3;  // SIM LOD tier (0 = near, 1 = mid, 2 = far, 3 = unplaced)
    uint16 schedTick : 14 = 0; // LOD timekeeping

    static constexpr float SchedTickHz = 64.0f;
    static constexpr uint32 SchedTickMask = 0x3FFF;

    void update(Renderer& renderer, float deltaSeconds, const Transform& parentWorld = Transform(), uint32 cullPassMask = EntityCullPass_Own);
    void updateSelf(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, uint32 cullPassMask, oc::vector<EntityUpdateNode>& outChildren);
    EEntityCullMode getCullMode() const;
    // The entity whose spatial entry culls this one: itself, the RootOnly ancestor covering it, or null (never culled).
    const Entity* getCullOwner() const;
    // Teleport contract: re-places the spatial entry for an entity moved outside its visit (a root: pos is world).
    void placeSpatialEntry();
    // RootOnly: rebuilds the template's cached subtree bounds from the live tree (main thread, outside the pass).
    void refreshTreeCullBounds();
    const char* getName() const; // "" when unnamed; owned by EntityNameRegistry (see EntityNames.ixx)
    bool hasName() const;
    void setName(oc::string_view name);
    void reparentEntity(Entity* newParent);
    bool isPrefabInstance() const { return (flags & EEntityFlag_PrefabInstance) != 0; }
    void setPrefabInstance(bool on) { flags = on ? uint8(flags | EEntityFlag_PrefabInstance) : uint8(flags & ~EEntityFlag_PrefabInstance); }
    bool isEnabled() const { return (flags & EEntityFlag_Enabled) != 0; }
    void setEnabled(bool on) { flags = on ? uint8(flags | EEntityFlag_Enabled) : uint8(flags & ~EEntityFlag_Enabled); }
    bool isPhysicsSuspended() const { return (flags & EEntityFlag_PhysicsSuspended) != 0; }
    void setPhysicsSuspended(bool on) { flags = on ? uint8(flags | EEntityFlag_PhysicsSuspended) : uint8(flags & ~EEntityFlag_PhysicsSuspended); }
    bool isProfiled() const { return (flags & EEntityFlag_Profiled) != 0; }
    void setProfiled() { flags |= EEntityFlag_Profiled; } // one-way: only ever latched on
    bool isFrozen() const { return (flags & EEntityFlag_Frozen) != 0; }
    void setFrozen(bool on); // Applies to the whole subtree
    bool isGlobal() const { return (flags & EEntityFlag_Global) != 0; }
    bool isPrefabLocked() const;
    Entity* nearestPrefabInstance();

    const oc::string& getPrefabName() const;
    const oc::string& getSourceFile() const;


private:
	Entity() = default;
	Entity(const Entity&) = delete;
    ~Entity() { assert(refCount == 0); }

    void createComponent(EComponentID id, uint16 componentOffset, const void* info, const Transform& base, uint8*& treeCursor);
    void destroyComponent(EComponentID id, uint16 componentOffset, const void* info);

private:

    friend class SceneComponent;
    friend class World;
};
static_assert(sizeof(Entity) <= 64);

export struct EntityPtr
{
    EntityPtr() = default;
    explicit EntityPtr(Entity* entity) : m_entity(entity) { addRef(); }

    EntityPtr(const EntityPtr& other) : m_entity(other.m_entity) { addRef(); }
    EntityPtr(EntityPtr&& other) noexcept : m_entity(other.m_entity) { other.m_entity = nullptr; }

    EntityPtr& operator=(const EntityPtr& other)
    {
        if (this != &other)
        {
            release();
            m_entity = other.m_entity;
            addRef();
        }
        return *this;
    }

    EntityPtr& operator=(EntityPtr&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_entity = other.m_entity;
            other.m_entity = nullptr;
        }
        return *this;
    }

    ~EntityPtr() { release(); }

    Entity* get() const { return m_entity; }
    Entity* operator->() const { return m_entity; }
    Entity& operator*() const { return *m_entity; }
    explicit operator bool() const { return m_entity != nullptr; }
    operator Entity*() const { return m_entity; }
    void release()
    {
        if (!m_entity)
            return;
        if (oc::atomic_ref<uint16>(m_entity->refCount).fetch_sub(1) == 1)
            Entity::destroy(m_entity);
        m_entity = nullptr;
    }

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
    // RootOnly lazy cache: the render bounds of the whole spawned subtree in this entity's LOCAL
    // space (the same for every instance). State 0 = uncomputed, 1 = no render node, 2 = valid;
    // stored LAST, same benign-race scheme as treeAllocSize (every writer stores the same values).
    mutable Sphere treeCullBounds{ glm::vec3(0.0f), 0.0f };
    mutable uint32 treeCullBoundsState = 0;
    mutable uint32 treeAllocSize = 0; // lazy cache: entity + recursive SceneComponent children, 0 = uncomputed
    // Lazy cache: NetworkComponents in the whole tree, UINT32_MAX = uncomputed (same benign-race
    // scheme as treeAllocSize). Entity::create needs it for server netId contiguity: only a tree
    // that mints MORE THAN ONE id must hold the manager's register lock across the whole spawn -
    // single-component trees (every unit/projectile prefab) mint atomically and stay parallel.
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