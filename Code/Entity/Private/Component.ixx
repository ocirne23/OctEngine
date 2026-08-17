export module Entity:Component;

// The component system's shared machinery: the inline layout table over all component types, the
// component accessors built on it, and the entity-tree/allocation helpers. Each component type itself
// lives in its own partition under Private/Components; they are re-exported here so importing
// :Component still yields the whole set.
export import :SceneComponent;
export import :RenderComponent;
export import :AnimatorComponent;
export import :PhysicsComponent;
export import :AudioComponent;
export import :ParticleComponent;
export import :ForceComponent;
export import :LightComponent;
export import :NetworkComponent;
export import :GameComponents;
export import :ScriptComponent;

import :Entity;
import Core;
import Core.Transform;

export int componentIdFromName(oc::string_view name);
export void detachFromOwner(Entity* entity);          // deletion path: breaks the entity's tree allocation (conservative)
export void detachKeepAllocation(Entity* entity);     // reparentEntity only: break decision already made by the caller

// Tree-allocation contiguity (see EEntityFlag_RootAllocation/ContiguousAllocation):
// Nearest allocation root reachable through an unbroken contiguous parent chain (self included); null if broken.
export Entity* findAllocationRoot(Entity* entity);
// Splits the member's allocation before it departs: path ancestors revert to per-entity freeing, every
// off-path subtree (and the member itself) is promoted to its own RootAllocation and keeps one-chunk
// freeing for its contiguous DFS range. No-op for broken members and allocation roots.
export void breakContiguousAllocation(Entity* member);
// Same split entered at the allocation root itself (Entity::destroy's solely-owned fallback): the root
// reverts to its own slice, each child subtree becomes an independent allocation.
export void breakContiguousAllocationFromRoot(Entity* root);
// True if every contiguous member below `entity` is owned solely by its parent's children list
// (refcount 1) — i.e. destroying the tree is guaranteed to destroy all of them.
export bool contiguousTreeSolelyOwned(Entity* entity);

export constexpr uint16 MaxInlineComponentTypes = 13;
export constexpr uint16 ComponentAlignment = 16;

export Transform composeTransform(const Transform& parent, const Transform& local);

export constexpr const char* componentTypeName(EComponentID id)
{
    switch (id)
    {
    case EComponentID_Scene:  return "Scene";
    case EComponentID_Render: return "Render";
    case EComponentID_Animator: return "Animator";
    case EComponentID_Physics: return "Physics";
    case EComponentID_Audio:  return "Audio";
    case EComponentID_Particle: return "Particle";
    case EComponentID_Force:  return "Force";
    case EComponentID_Light:  return "Light";
    case EComponentID_Network: return "Network";
    case EComponentID_GameUnit: return "GameUnit";
    case EComponentID_GameStructure: return "GameStructure";
    case EComponentID_GameProjectile: return "GameProjectile";
    case EComponentID_Script: return "Script";
    default:                  return "Unknown";
    }
}

export namespace EntityComponentDetail
{
    template <typename T>
    constexpr T alignUp(T value, T alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    inline constexpr oc::array<uint16, MaxInlineComponentTypes> inlineSizes {
        alignUp(uint16(sizeof(SceneComponent)),   ComponentAlignment),
        alignUp(uint16(sizeof(RenderComponent)),  ComponentAlignment),
        alignUp(uint16(sizeof(AnimatorComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(PhysicsComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(AudioComponent)),   ComponentAlignment),
        alignUp(uint16(sizeof(ParticleComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(ForceComponent)),   ComponentAlignment),
        alignUp(uint16(sizeof(LightComponent)),   ComponentAlignment),
        alignUp(uint16(sizeof(NetworkComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(GameUnitComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(GameStructureComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(GameProjectileComponent)), ComponentAlignment),
        alignUp(uint16(sizeof(ScriptComponent)),  ComponentAlignment),
    };
    static_assert(EComponentID_Scene == 0);
    static_assert(EComponentID_Render == 1);
    static_assert(EComponentID_Animator == 2);
    static_assert(EComponentID_Physics == 3);
    static_assert(EComponentID_Audio == 4);
    static_assert(EComponentID_Particle == 5);
    static_assert(EComponentID_Force == 6);
    static_assert(EComponentID_Light == 7);
    static_assert(EComponentID_Network == 8);
    static_assert(EComponentID_GameUnit == 9);
    static_assert(EComponentID_GameStructure == 10);
    static_assert(EComponentID_GameProjectile == 11);
    static_assert(EComponentID_Script == 12);

    inline constexpr uint16 entityBaseOffset = alignUp(uint16(sizeof(Entity)), ComponentAlignment);
}

inline constexpr uint16 getComponentByteOffset(uint16 typeBits, EComponentID id)
{
    uint16 offset = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < uint16(id) && i < MaxInlineComponentTypes; ++i)
        if (typeBits & (1 << i))
            offset += EntityComponentDetail::inlineSizes[i];
    return offset;
}

export constexpr uint16 getEntityAllocSize(uint16 typeBits)
{
    uint16 size = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
        if (typeBits & (1 << i))
            size += EntityComponentDetail::inlineSizes[i];
    return size;
}

export template <typename T>
bool hasComponent(const Entity* entity)
{
    return (entity->typeBits & (1 << T::getId())) != 0;
}

export template <typename T>
T* getComponent(Entity* entity)
{
    constexpr EComponentID id = T::getId();
    static_assert(uint16(id) < MaxInlineComponentTypes, "Only inline components are supported");
    if (!(entity->typeBits & (1 << id)))
        return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<uint8*>(entity) + getComponentByteOffset(entity->typeBits, id));
}
