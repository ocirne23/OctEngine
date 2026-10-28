export module Entity:ComponentLayout;

import Core;

import :EntityState;
import :Entity;
import :SceneComponent;
import :RenderComponent;
import :AnimatorComponent;
import :PhysicsComponent;
import :AudioComponent;
import :ParticleComponent;
import :ForceComponent;
import :LightComponent;
import :NetworkComponent;
import :GameUnitComponent;
import :GameStructureComponent;
import :GameProjectileComponent;
import :HumanoidAnimatorComponent;
import :ScriptComponent;

// The inline component layout: every component type's aligned size, and the byte offsets an entity's typeBits give them. getComponent (Component.ixx) reads it.

export constexpr uint16 MaxInlineComponentTypes = 14;
export constexpr uint16 ComponentAlignment = 16;

export using ComponentOffsets = oc::array<uint16, MaxInlineComponentTypes>;

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
    case EComponentID_HumanoidAnimator: return "HumanoidAnimator";
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
        alignUp(uint16(sizeof(HumanoidAnimatorComponent)), ComponentAlignment),
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
    static_assert(EComponentID_HumanoidAnimator == 12);
    static_assert(EComponentID_Script == 13);
    static_assert(MaxInlineComponentTypes == 14); // EntityState.ixx spells the offsets array out

    inline constexpr uint16 entityBaseOffset = alignUp(uint16(sizeof(Entity)), ComponentAlignment);

    constexpr uint16 componentByteOffset(uint16 typeBits, EComponentID id)
    {
        uint16 offset = entityBaseOffset;
        for (uint16 i = 0; i < uint16(id) && i < MaxInlineComponentTypes; ++i)
            if (typeBits & (1 << i))
                offset += inlineSizes[i];
        return offset;
    }
}

export constexpr uint16 getEntityAllocSize(uint16 typeBits)
{
    uint16 size = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
        if (typeBits & (1 << i))
            size += EntityComponentDetail::inlineSizes[i];
    return size;
}

export constexpr ComponentOffsets getComponentOffsets(uint16 typeBits)
{
    ComponentOffsets offsets;
    uint16 offset = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
    {
        if (typeBits & (1 << i))
        {
            offsets[i] = offset;
            offset += EntityComponentDetail::inlineSizes[i];
        }
        else
        {
            offsets[i] = 0;
        }
    }
    return offsets;
}

// With the offsets of the entity's typeBits computed once (the per-entity visit): no offset walk per component.
export template <typename T>
T* getComponent(EntityState* entity, const ComponentOffsets& offsets)
{
    const uint16 offset = offsets[T::getId()];
    return offset != 0 ? reinterpret_cast<T*>(reinterpret_cast<uint8*>(entity) + offset) : nullptr;
}
