export module Entity:EntityState;

import Core;
import Core.glm;
import Core.Transform;
import RendererVK.fwd;
import Spatial;

export class Entity; // declared before their friend lines
export struct EntityPtr;
export struct EntitySpawnTemplate;
export class World;
export class WorldState;
export class NetworkManager;
export class NetworkManagerState;
export struct AnimatorComponent;
export struct PhysicsComponent;
export struct GameUnitComponent;
export struct GameStructureComponent;
export struct SceneComponent;
export class EngineScriptContext;
export enum EComponentID : uint16; // Entity.ixx

// EntityUpdateNode::cullPassMask: no RootOnly ancestor covers this entity.
export constexpr uint32 EntityCullPass_Own = UINT32_MAX;

// One entity queued for the parallel update pass: its parent's composed world transform.
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

    // A spawned prefab tree is ONE EntityAllocator block (see Entity::create). While the tree is intact (no member reparented/deleted out, no external refs
    // at teardown) the root frees the whole block in one call and members skip their own free. A structural break SPLITS the allocation
    // (breakContiguousAllocation): path ancestors revert to per-entity freeing while every off-path subtree becomes its own RootAllocation over its
    // contiguous DFS range and keeps one-chunk freeing.
    EEntityFlag_RootAllocation       = 1 << 3, // first entity of its allocation range; frees it while contiguous
    EEntityFlag_ContiguousAllocation = 1 << 4, // slice still owned by an intact (sub)tree allocation

    // Latch for updateSelf: this entity's subtree has already been pulled from the simulation while disabled, so the walk is not repeated every frame.
    // Anything joining the subtree afterwards would miss that walk, so reparentEntity clears it up the new ancestor chain.
    EEntityFlag_PhysicsSuspended     = 1 << 5,

    // OPT-IN per-entity ProfileScope in the parallel entity pass (named by the registry-owned name): the interesting components set it at spawn
    // (Animator/Force/Script/GameUnit), machine structures (barracks/turret) latch it in their update - plain static scenery stays scope-free so it cannot
    // flood the profiler rings.
    EEntityFlag_Profiled             = 1 << 6,

    // GLOBAL root (`Global true` in the .pre, root only): the World visits it every frame from its own list instead of finding it through the spatial index -
    // for organisational roots whose children spread across the world (the co-op terrain root). It still registers a spatial entry (gameplay queries, render
    // culling); its children are selected individually like everything else. Never inherited by children.
    EEntityFlag_Global               = 1 << 7,
};

// Everything of Entity but its public API (Entity.ixx): the scheduling bytes, the allocation flags, the per-entity visit and the tree-allocation helpers.
// One cache line with Entity (static_asserted there). A helper reaches the Entity through self().
export class EntityState
{
public:

    // The LOCAL transform (world for a root): plain data, read and written by everyone.
    glm::vec3 pos;
    float scale = 1.0f;
    glm::quat rot;

protected:

    friend struct EntityPtr; // refCount, destroy
    friend class World;
    friend class WorldState;
    friend class NetworkManager;
    friend class NetworkManagerState;
    friend struct AnimatorComponent;      // setProfiled
    friend struct GameStructureComponent; // setProfiled
    friend struct GameUnitComponent;      // placeSpatialEntry (the far tick's teleport)
    friend struct PhysicsComponent;       // schedTier (buoyancy cadence)
    friend struct SceneComponent;         // spawns its children through the tree overload of create
    friend class EngineScriptContext;     // static_asserts the layout ScriptAPI.h mirrors (offsetof parent)

    EntityState() = default;
    EntityState(const EntityState&) = delete;
    ~EntityState() { assert(refCount == 0); }

    // A template so the cast compiles where Entity is complete (it is final and the only derived class); defined here, not in Entity.ixx - a member
    // defined in another partition broke MSVC's IFC import (an ICE in every importer).
    template<typename E = Entity> E& self() { return static_cast<E&>(*this); }
    template<typename E = Entity> const E& self() const { return static_cast<const E&>(*this); }

    // ---- Spawn and teardown ----

    // The tree overload: carves this entity out of `treeCursor` (the tree's one allocation) under `parent`.
    static EntityPtr create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags, uint8*& treeCursor, Entity* parent);
    static void destroy(Entity* entity);
    void createComponent(EComponentID id, uint16 componentOffset, const void* info, const Transform& base, uint8*& treeCursor);
    void destroyComponent(EComponentID id, uint16 componentOffset, const void* info);

    // ---- The per-entity visit (World's pass) ----

    void update(Renderer& renderer, float deltaSeconds, const Transform& parentWorld = Transform(), uint32 cullPassMask = EntityCullPass_Own);
    void updateSelf(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, uint32 cullPassMask, oc::vector<EntityUpdateNode>& outChildren);
    // Teleport contract: re-places the spatial entry for an entity moved outside its visit (a root: pos is world).
    void placeSpatialEntry();
    // RootOnly: rebuilds the template's cached subtree bounds from the live tree (main thread, outside the pass).
    void refreshTreeCullBounds();

    bool isPhysicsSuspended() const { return (flags & EEntityFlag_PhysicsSuspended) != 0; }
    void setPhysicsSuspended(bool on) { flags = on ? uint8(flags | EEntityFlag_PhysicsSuspended) : uint8(flags & ~EEntityFlag_PhysicsSuspended); }
    bool isProfiled() const { return (flags & EEntityFlag_Profiled) != 0; }
    void setProfiled() { flags |= EEntityFlag_Profiled; } // one-way: only ever latched on

    // ---- Tree allocations (see EEntityFlag_RootAllocation / ContiguousAllocation) ----

    static void detachKeepAllocation(Entity* entity); // reparentEntity only: break decision already made by the caller
    // Nearest allocation root reachable through an unbroken contiguous parent chain (self included); null if broken.
    static Entity* findAllocationRoot(Entity* entity);
    // Splits the member's allocation before it departs: path ancestors revert to per-entity freeing, every off-path subtree (and the member itself) is
    // promoted to its own RootAllocation and keeps one-chunk freeing for its contiguous DFS range. No-op for broken members and allocation roots.
    static void breakContiguousAllocation(Entity* member);
    // Same split entered at the allocation root itself (Entity::destroy's solely-owned fallback): the root reverts to its own slice, each child subtree
    // becomes an independent allocation.
    static void breakContiguousAllocationFromRoot(Entity* root);
    // True if every contiguous member below `entity` is owned solely by its parent's children list (refcount 1) - i.e. destroying the tree is guaranteed to
    // destroy all of them.
    static bool contiguousTreeSolelyOwned(Entity* entity);
    static void promoteChildSubtrees(Entity* parent, const Entity* skip); // the split's step: every intact non-root child below becomes its own root

    // ---- The state ----

    Entity* parent = nullptr;
    const EntitySpawnTemplate* spawnTemplate = nullptr;
    SpatialEntry spatialEntry;

    uint16 refCount = 0;
    uint16 typeBits = 0;
    uint8 flags = 0;      // EEntityFlags bitmask
    uint8 updateCost = 0; // MEASURED updateSelf weight in 250ns units (0 = not yet measured)

    uint16 schedTier : 2 = 3;  // SIM LOD tier (0 = near, 1 = mid, 2 = far, 3 = unplaced)
    uint16 schedTick : 14 = 0; // LOD timekeeping

    static constexpr float SchedTickHz = 64.0f;
    static constexpr uint32 SchedTickMask = 0x3FFF;
};
