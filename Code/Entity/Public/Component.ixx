export module Entity:Component;

// The components and their accessors. Each component type lives in its own partition under Private/Components; they are re-exported here so importing
// :Component yields the whole set. The inline layout the accessors read is ComponentLayout.ixx.
export import :SceneComponent;
export import :RenderComponent;
export import :AnimatorComponent;
export import :PhysicsComponent;
export import :AudioComponent;
export import :ParticleComponent;
export import :ForceComponent;
export import :LightComponent;
export import :NetworkComponent;
export import :GameUnitComponent;
export import :GameStructureComponent;
export import :GameProjectileComponent;
export import :BoneModel; // not a component: what the two bone-model animators below share
export import :SceneAnimatorComponent; // PARKED: compiled and kept, but it has NO component id / type bit - see its .ixx
export import :HumanoidAnimatorComponent;
export import :ScriptComponent;

import Core;
import Core.OcBit;

import :EntityState;
import :Entity;
import :ComponentLayout; // the inline layout

// The entity's component of type T, or null when it has none. Components sit in the entity's own allocation, at offsets its typeBits give. They take
// the state base, so the entity's own state members can pass `this`.
export template <typename T>
bool hasComponent(const EntityState* entity)
{
    return (static_cast<const Entity*>(entity)->getTypeBits() & (1 << T::getId())) != 0;
}

export template <typename T>
T* getComponent(EntityState* entity)
{
    constexpr EComponentID id = T::getId();
    static_assert(uint16(id) < MaxInlineComponentTypes, "Only inline components are supported");
    const uint16 typeBits = static_cast<const Entity*>(entity)->getTypeBits();
    if (!(typeBits & (1 << id)))
        return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<uint8*>(entity) + EntityComponentDetail::componentByteOffset(typeBits, id));
}

// The SpawnInfo recipe the entity's template holds for component T (null: no template, or no T): the template's spawnInfos run in component-bit order.
export template <typename T>
const typename T::SpawnInfo* getSpawnInfo(const Entity* entity)
{
    const EntitySpawnTemplate* tmpl = entity->getSpawnTemplate();
    if (!tmpl || !hasComponent<T>(entity))
        return nullptr;
    const uint16 below = uint16(entity->getTypeBits() & ((1u << T::getId()) - 1));
    const size_t idx = size_t(oc::popcnt(below));
    return idx < tmpl->spawnInfos.size() ? static_cast<const typename T::SpawnInfo*>(tmpl->spawnInfos[idx].get()) : nullptr;
}
