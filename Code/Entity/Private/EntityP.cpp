module Entity;

import Core;
import Core.Log;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File;
import :Component;
import :Allocator;

import RendererVK;
import Spatial;

EntityArchetype makeEntityArchetype(uint16 typeBits)
{
    return EntityArchetype{ uint16(getEntityAllocSize(typeBits)), typeBits };
}

void Entity::setName(std::string_view name)
{
    if (name.empty())
    {
        displayName.reset();
        return;
    }
    displayName = std::make_unique<char[]>(name.size() + 1);
    std::char_traits<char>::copy(displayName.get(), name.data(), name.size());
    displayName[name.size()] = '\0';
}

void Entity::setFrozen(bool on)
{
    flags = on ? uint8(flags | EEntityFlag_Frozen) : uint8(flags & ~EEntityFlag_Frozen);
    if (SceneComponent* sc = getComponent<SceneComponent>(this))
        for (const EntityPtr& child : sc->children)
            child->setFrozen(on);
}

void Entity::updateSelf(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, std::vector<EntityUpdateNode>& outChildren)
{
    SceneComponent* sc = getComponent<SceneComponent>(this);
    if (!isEnabled())
    {
        if (!isPhysicsSuspended())
        {
            suspendPhysicsTree(*this);
            setPhysicsSuspended(true);
        }
        return;
    }
    if (isPhysicsSuspended())
        setPhysicsSuspended(false);

    const bool frozen = isFrozen();

    if (!frozen)
    {
        // Game-layer sim BEFORE the script, so DSL orders/reads see this frame's state. Authority
        // gating and thread-safety live inside the components (see GameComponents.ixx).
        if (GameUnitComponent* gameUnit = getComponent<GameUnitComponent>(this))
            gameUnit->update(*this, deltaSeconds);
        if (GameStructureComponent* gameStructure = getComponent<GameStructureComponent>(this))
            gameStructure->update(*this, deltaSeconds);
        if (GameProjectileComponent* gameProjectile = getComponent<GameProjectileComponent>(this))
            gameProjectile->update(*this, deltaSeconds);

        if (ScriptComponent* script = getComponent<ScriptComponent>(this))
            script->update(*this, deltaSeconds);

        // before Physics/composeTransform: a client-side correction to pos/rot lands this same frame
        if (NetworkComponent* network = getComponent<NetworkComponent>(this))
            network->update(*this, deltaSeconds);

        if (AnimatorComponent* animator = getComponent<AnimatorComponent>(this))
            animator->update(*this, renderer, deltaSeconds); // advance animation + refresh skinning palette

        if (PhysicsComponent* physics = getComponent<PhysicsComponent>(this))
            physics->update(*this, parentWorld); // dynamic bodies write the simulated pose into pos/rot
    }

    const Transform world = composeTransform(parentWorld, Transform(pos, scale, rot));
    if (RenderComponent* render = getComponent<RenderComponent>(this))
    {
        if (render->node.isValid()) // empty when spawned without a container, or after destroy()
        {
            render->node.setTransform(composeTransform(world, render->localTransform));
            SpatialIndex& spatialIndex = Globals::spatialIndex;
            const SpatialCullingConfig& culling = spatialIndex.getCullingConfig();
            if (render->spatialEntry.isValid())
            {
                const Sphere bounds = render->node.getWorldBounds();
                const float radius = render->node.isSkinned() ? bounds.radius * culling.skinnedRadiusScale : bounds.radius;
                spatialIndex.updateEntry(render->spatialEntry.handle(), glm::dvec3(bounds.pos), radius);
            }
            if (culling.mode >= int(ESpatialCullMode::Cull) && render->spatialEntry.isValid())
            {
                const uint32 spatialMask = spatialIndex.getPassMask(render->spatialEntry.handle());
                uint32 passMask = 0;
                if (spatialMask & SpatialPassBit_Main)
                    passMask = RendererVKLayout::PASS_ALL; // in view: feeds every pass
                else if ((spatialMask & SpatialPassBit_Near) && culling.mode != int(ESpatialCullMode::MainOnly))
                    passMask = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI; // off-screen but shadow/RT relevant
                if (passMask != 0)
                    renderer.renderNode(render->node, passMask);
            }
            else
                renderer.renderNode(render->node);
        }
    }

    if (AudioComponent* audio = getComponent<AudioComponent>(this))
        audio->update(*this, world); // playing follow-sounds track the entity

    if (ParticleComponent* particle = getComponent<ParticleComponent>(this))
        if (!frozen)
            particle->update(*this, world, deltaSeconds); // effect follows the entity (position + velocity)

    // Not frozen-gated: placing the bubble is not simulation, and an editor document being dragged by the
    // gizmo still has to carry its field along.
    if (ForceComponent* force = getComponent<ForceComponent>(this))
        force->update(*this, world);

    // Also not frozen-gated: a light is placement, not simulation, so an editor document lights its
    // own scene while frozen. Pushes are lock-free (addLightInfo/addDebugLine), like renderNode.
    if (LightComponent* light = getComponent<LightComponent>(this))
        light->update(*this, renderer, world);

    if (sc)
        for (const EntityPtr& child : sc->children)
            outChildren.push_back({ child.get(), world });
}

void Entity::update(Renderer& renderer, float deltaSeconds, const Transform& parentWorld)
{
    std::vector<EntityUpdateNode> children;
    updateSelf(renderer, deltaSeconds, parentWorld, children);
    for (const EntityUpdateNode& child : children)
        child.entity->update(renderer, deltaSeconds, child.parentWorld);
}

// Recursive alloc size of the template's entity + its whole SceneComponent child tree, lazily cached on
// the template (idempotent, so the racy relaxed store is benign — every writer stores the same value).
static uint32 getTreeAllocSize(const EntitySpawnTemplate& tmpl)
{
    const uint32 cached = std::atomic_ref<uint32>(tmpl.treeAllocSize).load(std::memory_order_relaxed);
    if (cached)
        return cached;

    uint32 size = tmpl.archetype.allocSize;
    if (tmpl.archetype.typeBits & (1 << EComponentID_Scene)) // Scene is bit 0, so its SpawnInfo is spawnInfos[0]
    {
        const auto* info = static_cast<const SceneComponent::SpawnInfo*>(tmpl.spawnInfos[0].get());
        for (const SceneComponent::SpawnInfo::ChildSpawnInfo& child : info->children)
            if (child.tmpl)
                size += getTreeAllocSize(*child.tmpl);
    }
    std::atomic_ref<uint32>(tmpl.treeAllocSize).store(size, std::memory_order_relaxed);
    return size;
}

EntityPtr Entity::create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags)
{
    // One allocation for the whole prefab tree: root + every recursive child. The spawn recursion
    // (SceneComponent::spawn -> the create overload below) carves each entity's slice from the cursor;
    // slices free themselves individually on destroy.
    uint8* treeCursor = static_cast<uint8*>(Globals::entityAllocator.allocate(getTreeAllocSize(tmpl)));
    EntityPtr root = create(tmpl, transform, initialFlags | EEntityFlag_RootAllocation, treeCursor, nullptr);
    return root;
}

EntityPtr Entity::create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags, uint8*& treeCursor, Entity* parent)
{
    void* buffer = treeCursor;
    treeCursor += tmpl.archetype.allocSize; // per-entity sizes are 16-aligned, so a straight bump stays aligned

    if (parent && parent->isFrozen())
        initialFlags |= EEntityFlag_Frozen; // frozen is a subtree state; a child spawns into it already set

    Entity* entity = ::new (buffer) Entity();
    entity->parent = parent; // linked before components spawn (see declaration)
    entity->pos = transform.pos;
    entity->scale = transform.scale;
    entity->rot = transform.quat;

    entity->setName(tmpl.displayName);
    entity->spawnTemplate = &tmpl;
    entity->typeBits = tmpl.archetype.typeBits;
    entity->flags = initialFlags | EEntityFlag_ContiguousAllocation; // slice owned by the tree block until broken
    entity->setEnabled(tmpl.enabled);
    entity->setPrefabInstance(!tmpl.prefabName.empty()); // a registered prefab spawns as a locked instance

    int idx = 0;
    uint16 offset = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
        if (entity->typeBits & (1 << i))
        {
            entity->createComponent(EComponentID(i), offset, tmpl.spawnInfos[idx++].get(), transform, treeCursor);
            offset += EntityComponentDetail::inlineSizes[i];
        }

    return EntityPtr(entity);
}

void Entity::destroy(Entity* entity)
{
    constexpr uint8 rootContiguous = EEntityFlag_RootAllocation | EEntityFlag_ContiguousAllocation;

    // Intact allocation root: verify every member is owned solely by its parent's children list before
    // committing to the one-chunk free — an externally referenced member would outlive this teardown.
    // On failure the allocation splits: each child subtree re-checks at its own death, so only the path
    // to the offending member degrades to per-entity freeing.
    // (Residual hole: a script OnDestroy grabbing a ref to a sibling member mid-teardown.)
    if ((entity->flags & rootContiguous) == rootContiguous && !contiguousTreeSolelyOwned(entity))
        breakContiguousAllocationFromRoot(entity);

    const uint8 flags = entity->flags;
    const bool freesWholeTree = (flags & rootContiguous) == rootContiguous;
    const bool ownedByTreeBlock = (flags & EEntityFlag_ContiguousAllocation) && !(flags & EEntityFlag_RootAllocation);
    const uint32 size = freesWholeTree ? getTreeAllocSize(*entity->spawnTemplate) : getEntityAllocSize(entity->typeBits);

    int idx = 0;
    uint16 offset = EntityComponentDetail::entityBaseOffset;
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
        if (entity->typeBits & (1 << i))
        {
            entity->destroyComponent(EComponentID(i), offset, entity->spawnTemplate->spawnInfos[idx++].get());
            offset += EntityComponentDetail::inlineSizes[i];
        }

    entity->~Entity();
    // Intact tree: members skip their own free (their slices belong to the root's block, reclaimed in
    // the root's one deallocate). Broken/standalone entities free their own exact-size slice.
    if (!ownedByTreeBlock)
        Globals::entityAllocator.deallocate(entity, size);
}

void Entity::createComponent(EComponentID id, uint16 componentOffset, const void* info, const Transform& base, uint8*& treeCursor)
{
    switch (id)
    {
    case EComponentID_Scene:
    {
        SceneComponent* sc = reinterpret_cast<SceneComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (sc) SceneComponent();
        sc->spawn(*this, *static_cast<const SceneComponent::SpawnInfo*>(info), base, treeCursor);
        break;
    }
    case EComponentID_Render:
    {
        RenderComponent* rc = reinterpret_cast<RenderComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (rc) RenderComponent();
        rc->spawn(*this, *static_cast<const RenderComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Animator:
    {
        AnimatorComponent* ac = reinterpret_cast<AnimatorComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (ac) AnimatorComponent();
        ac->spawn(*this, *static_cast<const AnimatorComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Physics:
    {
        PhysicsComponent* pc = reinterpret_cast<PhysicsComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (pc) PhysicsComponent();
        pc->spawn(*this, *static_cast<const PhysicsComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Audio:
    {
        AudioComponent* ac = reinterpret_cast<AudioComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (ac) AudioComponent();
        ac->spawn(*this, *static_cast<const AudioComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Particle:
    {
        ParticleComponent* pc = reinterpret_cast<ParticleComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (pc) ParticleComponent();
        pc->spawn(*this, *static_cast<const ParticleComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Force:
    {
        ForceComponent* fc = reinterpret_cast<ForceComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (fc) ForceComponent();
        fc->spawn(*this, *static_cast<const ForceComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Light:
    {
        LightComponent* lc = reinterpret_cast<LightComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (lc) LightComponent();
        lc->spawn(*this, *static_cast<const LightComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Network:
    {
        NetworkComponent* nc = reinterpret_cast<NetworkComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (nc) NetworkComponent();
        nc->spawn(*this, *static_cast<const NetworkComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_GameUnit:
    {
        GameUnitComponent* gu = reinterpret_cast<GameUnitComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (gu) GameUnitComponent();
        gu->spawn(*this, *static_cast<const GameUnitComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_GameStructure:
    {
        GameStructureComponent* gs = reinterpret_cast<GameStructureComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (gs) GameStructureComponent();
        gs->spawn(*this, *static_cast<const GameStructureComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_GameProjectile:
    {
        GameProjectileComponent* gp = reinterpret_cast<GameProjectileComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (gp) GameProjectileComponent();
        gp->spawn(*this, *static_cast<const GameProjectileComponent::SpawnInfo*>(info), base);
        break;
    }
    case EComponentID_Script:
    {
        ScriptComponent* scr = reinterpret_cast<ScriptComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (scr) ScriptComponent();
        scr->spawn(*this, *static_cast<const ScriptComponent::SpawnInfo*>(info), base);
        break;
    }
    default:
        __debugbreak();
    }
}

void Entity::destroyComponent(EComponentID id, uint16 componentOffset, const void* info)
{
    switch (id)
    {
    case EComponentID_Scene:
    {
        SceneComponent* sc = reinterpret_cast<SceneComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        sc->destroy(*this, *static_cast<const SceneComponent::SpawnInfo*>(info));
        sc->~SceneComponent();
        break;
    }
    case EComponentID_Render:
    {
        RenderComponent* rc = reinterpret_cast<RenderComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        rc->destroy(*this, *static_cast<const RenderComponent::SpawnInfo*>(info));
        rc->~RenderComponent();
        break;
    }
    case EComponentID_Animator:
    {
        AnimatorComponent* ac = reinterpret_cast<AnimatorComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        ac->destroy(*this, *static_cast<const AnimatorComponent::SpawnInfo*>(info));
        ac->~AnimatorComponent();
        break;
    }
    case EComponentID_Physics:
    {
        PhysicsComponent* pc = reinterpret_cast<PhysicsComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        pc->destroy(*this, *static_cast<const PhysicsComponent::SpawnInfo*>(info));
        pc->~PhysicsComponent();
        break;
    }
    case EComponentID_Audio:
    {
        AudioComponent* ac = reinterpret_cast<AudioComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        ac->destroy(*this, *static_cast<const AudioComponent::SpawnInfo*>(info));
        ac->~AudioComponent();
        break;
    }
    case EComponentID_Particle:
    {
        ParticleComponent* pc = reinterpret_cast<ParticleComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        pc->destroy(*this, *static_cast<const ParticleComponent::SpawnInfo*>(info));
        pc->~ParticleComponent();
        break;
    }
    case EComponentID_Force:
    {
        ForceComponent* fc = reinterpret_cast<ForceComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        fc->destroy(*this, *static_cast<const ForceComponent::SpawnInfo*>(info));
        fc->~ForceComponent();
        break;
    }
    case EComponentID_Light:
    {
        LightComponent* lc = reinterpret_cast<LightComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        lc->destroy(*this, *static_cast<const LightComponent::SpawnInfo*>(info));
        lc->~LightComponent();
        break;
    }
    case EComponentID_Network:
    {
        NetworkComponent* nc = reinterpret_cast<NetworkComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        nc->destroy(*this, *static_cast<const NetworkComponent::SpawnInfo*>(info));
        nc->~NetworkComponent();
        break;
    }
    case EComponentID_GameUnit:
    {
        GameUnitComponent* gu = reinterpret_cast<GameUnitComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        gu->destroy(*this, *static_cast<const GameUnitComponent::SpawnInfo*>(info));
        gu->~GameUnitComponent();
        break;
    }
    case EComponentID_GameStructure:
    {
        GameStructureComponent* gs = reinterpret_cast<GameStructureComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        gs->destroy(*this, *static_cast<const GameStructureComponent::SpawnInfo*>(info));
        gs->~GameStructureComponent();
        break;
    }
    case EComponentID_GameProjectile:
    {
        GameProjectileComponent* gp = reinterpret_cast<GameProjectileComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        gp->destroy(*this, *static_cast<const GameProjectileComponent::SpawnInfo*>(info));
        gp->~GameProjectileComponent();
        break;
    }
    case EComponentID_Script:
    {
        ScriptComponent* scr = reinterpret_cast<ScriptComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        scr->destroy(*this, *static_cast<const ScriptComponent::SpawnInfo*>(info));
        scr->~ScriptComponent();
        break;
    }
    default:
        __debugbreak();
    }
}

static bool isSelfOrDescendant(Entity* node, Entity* ancestor)
{
    for (Entity* p = node; p; p = p->parent)
        if (p == ancestor)
            return true;
    return false;
}

void Entity::reparentEntity(Entity* newParent)
{
    if (this == newParent || parent == newParent)
        return;

    EntityPtr keepAlive(this);

    if ((flags & EEntityFlag_ContiguousAllocation) && !(flags & EEntityFlag_RootAllocation))
    {
        // Moving within the same allocation keeps the tree intact (the root teardown still reaches us);
        // any other destination — including the failed moves that still detach below — leaves it.
        Entity* oldRoot = findAllocationRoot(parent);
        Entity* newRoot = (newParent && hasComponent<SceneComponent>(newParent) && !isSelfOrDescendant(newParent, this))
                        ? findAllocationRoot(newParent) : nullptr;
        if (!oldRoot || oldRoot != newRoot)
            breakContiguousAllocation(this);
    }
    detachKeepAllocation(this); // break decision made above; reparenting a whole allocation root never breaks

    if (newParent && !hasComponent<SceneComponent>(newParent))
    {
        // LOUD refusal: the detach above already ran, so the entity is now parentless — if the
        // caller drops its EntityPtr believing the parent holds one, the entity silently dies
        // (this exact failure shipped once: walls parented under a Scene-less ground vanished).
        Log::warning("Entity: cannot parent '" + std::string(getName()) + "' under '"
            + std::string(newParent->getName()) + "' — the parent has no SceneComponent (add "
            "'Component Scene' to its prefab); the entity is now UNPARENTED");
        return;
    }
    if (newParent && isSelfOrDescendant(newParent, this))
    {
        Log::warning("Entity: cannot parent '" + std::string(getName()) + "' under its own "
            "descendant '" + std::string(newParent->getName()) + "' (cycle); the entity is now UNPARENTED");
        return;
    }

    parent = newParent;

    if (newParent)
    {
        getComponent<SceneComponent>(newParent)->children.emplace_back(std::move(keepAlive));
        // The arrival was never part of any ancestor's suspend walk, so those latches are now stale.
        for (Entity* p = newParent; p; p = p->parent)
            p->setPhysicsSuspended(false);
    }

    // Frozen is a subtree state, so the moved subtree adopts its new parent's (root = never frozen).
    if (isFrozen() != (newParent && newParent->isFrozen()))
        setFrozen(!isFrozen());
}

Entity* Entity::nearestPrefabInstance()
{
    for (Entity* p = this; p; p = p->parent)
        if (p->isPrefabInstance())
            return p;
    return nullptr;
}

bool Entity::isPrefabLocked() const
{
    for (const Entity* p = this; p; p = p->parent)
        if (p->isPrefabInstance())
            return true;
    return false;
}

const std::string& Entity::getPrefabName() const
{
    static const std::string empty;
    return spawnTemplate ? spawnTemplate->prefabName : empty;
}

const std::string& Entity::getSourceFile() const
{
    static const std::string empty;
    return spawnTemplate ? spawnTemplate->sourceFile : empty;
}
