export module Entity:Entity;

import Core;
import Core.glm;
import Core.Transform;
import File.fwd;
import RendererVK.fwd;

export struct EntitySpawnTemplate;
export struct EntityPtr;
export class Entity;

// One entity queued for the depth-parallel update pass: its parent's composed world transform.
export struct EntityUpdateNode
{
    Entity* entity = nullptr;
    Transform parentWorld;
};

export enum EEntityFlags : uint8
{
    EEntityFlag_PrefabInstance = 1 << 0, // root of a locked prefab instance; cleared by "unpack"
    EEntityFlag_Enabled        = 1 << 1, // off = the entity and its whole subtree stop updating (see updateTree)
    EEntityFlag_Frozen         = 1 << 2, // scripts/physics/animator don't update this entity (Entity Editor documents);
                                         // carried by every member of the subtree, see setFrozen
    // A spawned prefab tree is ONE EntityAllocator block (see Entity::create). While the tree is intact
    // (no member reparented/deleted out, no external refs at teardown) the root frees the whole block in
    // one call and members skip their own free. A structural break SPLITS the allocation
    // (breakContiguousAllocation): path ancestors revert to per-entity freeing while every off-path
    // subtree becomes its own RootAllocation over its contiguous DFS range and keeps one-chunk freeing.
    EEntityFlag_RootAllocation       = 1 << 3, // first entity of its allocation range; frees it while contiguous
    EEntityFlag_ContiguousAllocation = 1 << 4, // slice still owned by an intact (sub)tree allocation
    // Latch for updateSelf: this entity's subtree has already been pulled from the simulation while
    // disabled, so the walk is not repeated every frame. Anything joining the subtree afterwards would
    // miss that walk, so reparentEntity clears it up the new ancestor chain.
    EEntityFlag_PhysicsSuspended     = 1 << 5,
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
    EComponentID_GameUnit = 9,       // game-layer actors (Components/GameComponents.ixx)
    EComponentID_GameStructure = 10,
    EComponentID_GameProjectile = 11,
    EComponentID_Script   = 12, // should be last so all other components are available on spawn
};

export class Entity
{
public:

    // initialFlags (EEntityFlags) applies before components spawn, so e.g. EEntityFlag_Frozen is already
    // visible to a component's spawn(). EEntityFlag_Enabled comes from the template, not from here.
    // Allocates ONE block for the entity plus its entire SceneComponent child tree (size cached on the
    // template); the spawn recursion carves each entity from `treeCursor` via the overload below. Each
    // entity frees its own exact-size slice on destroy (the allocator's free lists recycle the pieces),
    // so tree members are independent: any of them can outlive the others or be reparented away.
    // `parent` links the entity into its tree at construction (before components spawn), so spawn-time
    // logic never sees a contiguous member with a null parent; the caller still owns attaching the
    // child handle to the parent's children list.
    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags = 0);
    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags, uint8*& treeCursor, Entity* parent);
    static void destroy(Entity* entity);

public:

    glm::vec3 pos;
    float scale = 1.0f;
    glm::quat rot;

    Entity* parent = nullptr;
    std::unique_ptr<char[]> displayName; // null when unnamed; access via getName()/setName()
    const EntitySpawnTemplate* spawnTemplate = nullptr;

    uint16 refCount = 0;
    uint16 typeBits = 0;
    uint8 flags = 0; // EEntityFlags bitmask
    uint8 _unused[3];

    void update(Renderer& renderer, float deltaSeconds, const Transform& parentWorld = Transform());
    void updateSelf(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, std::vector<EntityUpdateNode>& outChildren);
    const char* getName() const { return displayName ? displayName.get() : ""; }
    bool hasName() const { return displayName != nullptr; }
    void setName(std::string_view name);
    void reparentEntity(Entity* newParent);
    bool isPrefabInstance() const { return (flags & EEntityFlag_PrefabInstance) != 0; }
    void setPrefabInstance(bool on) { flags = on ? uint8(flags | EEntityFlag_PrefabInstance) : uint8(flags & ~EEntityFlag_PrefabInstance); }
    bool isEnabled() const { return (flags & EEntityFlag_Enabled) != 0; }
    void setEnabled(bool on) { flags = on ? uint8(flags | EEntityFlag_Enabled) : uint8(flags & ~EEntityFlag_Enabled); }
    bool isPhysicsSuspended() const { return (flags & EEntityFlag_PhysicsSuspended) != 0; }
    void setPhysicsSuspended(bool on) { flags = on ? uint8(flags | EEntityFlag_PhysicsSuspended) : uint8(flags & ~EEntityFlag_PhysicsSuspended); }
    bool isFrozen() const { return (flags & EEntityFlag_Frozen) != 0; }
    void setFrozen(bool on); // Applies to the whole subtree
    bool isPrefabLocked() const;
    Entity* nearestPrefabInstance();

    const std::string& getPrefabName() const;
    const std::string& getSourceFile() const;

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
        if (std::atomic_ref<uint16>(m_entity->refCount).fetch_sub(1) == 1)
            Entity::destroy(m_entity);
        m_entity = nullptr;
    }

private:

    void addRef()
    {
        if (m_entity)
            std::atomic_ref<uint16>(m_entity->refCount).fetch_add(1);
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
    std::vector<std::shared_ptr<void>> spawnInfos;
    std::string sourceFile;
    std::string prefabName;
    std::string displayName;
    bool enabled = true;             // spawns with EEntityFlag_Enabled set/cleared ("Enabled" in the .pre)
    mutable uint32 treeAllocSize = 0; // lazy cache: entity + recursive SceneComponent children, 0 = uncomputed
};

export struct EntityChange
{
    struct CreateHierarchy
    {
        std::string path;
        EntityPtr   parent;   // nullptr = World root
    };
    struct CreateViewport
    {
        glm::ivec2  screenPos;
        std::string path;
    };
    struct AddSceneEntity
    {
        std::string displayName;
        EntityPtr parent;
    };
    // Script spawn (ctx->spawnEntity). Deferred rather than spawned in the thunk: spawning touches the
    // World caches, box3d and the renderer, none of which tolerate the parallel entity pass running
    // around them. Lands on the root list, like the thunk's old reparent-to-null did.
    struct SpawnAtPosition
    {
        std::string path;
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
        std::string path;
        std::string text; // pre-serialized document (Entity Editor's transform draft); empty → serialize root
    };
    struct OpenPrefabForEdit
    {
        std::string path; // .pre to load and unpack for the Prefab Editor
    };
    struct NewPrefab
    {
        std::string displayName; // blank editable entity, name only (no source file yet)
    };
    struct RespawnEntity
    {
        EntityPtr oldEntity;
        std::shared_ptr<const EntitySpawnTemplate> tmpl; // freshly assembled from the entity's edited component set
    };
    std::variant<CreateHierarchy, CreateViewport, AddSceneEntity, SpawnAtPosition, Delete, Reparent, SavePrefab, OpenPrefabForEdit, NewPrefab, RespawnEntity> type;
};