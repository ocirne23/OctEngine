module Entity;

import Core;
import Core.Log;
import Core.Time;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File;
import :Component;
import :Allocator;
import :EntityNames;
import :NetworkManager;

import RendererVK;
import Spatial;

EntityArchetype makeEntityArchetype(uint16 typeBits)
{
    return EntityArchetype{ uint16(getEntityAllocSize(typeBits)), typeBits };
}

// The entity owns NO name storage: Globals::entityNames maps the entity pointer to its owned copy -
// but ONLY for a name set after the spawn. A freshly spawned entity's name is its template's
// displayName, read straight from the template (which outlives every entity spawned from it:
// World's caches, retired lists and keepTemplateAlive), so a spawn makes no name allocation.
void Entity::setName(oc::string_view newName)
{
    Globals::entityNames.set(this, newName);
}

const char* Entity::getName() const
{
    if (const char* name = Globals::entityNames.get(this))
        return name;
    return spawnTemplate ? spawnTemplate->displayName.c_str() : "";
}

bool Entity::hasName() const
{
    return Globals::entityNames.get(this) != nullptr || (spawnTemplate && !spawnTemplate->displayName.empty());
}

void Entity::setFrozen(bool on)
{
    flags = on ? uint8(flags | EEntityFlag_Frozen) : uint8(flags & ~EEntityFlag_Frozen);
    if (SceneComponent* sc = getComponent<SceneComponent>(this))
        for (const EntityPtr& child : sc->children)
            child->setFrozen(on);
}

EEntityCullMode Entity::getCullMode() const
{
    return spawnTemplate ? spawnTemplate->cullMode : EEntityCullMode::PerEntity;
}

const Entity* Entity::getCullOwner() const
{
    if (spatialEntry.isValid())
        return this;
    if (getCullMode() != EEntityCullMode::None)
        for (const Entity* p = parent; p; p = p->parent)
            if (p->spatialEntry.isValid() && p->getCullMode() == EEntityCullMode::RootOnly)
                return p;
    return nullptr;
}

// Whether a spawn under `parent` is covered by a RootOnly ancestor. By template mode, not by entry:
// an ancestor still in its spawn registers only after its children.
static bool hasRootOnlyAncestor(const Entity* parent)
{
    for (const Entity* p = parent; p; p = p->parent)
        if (p->getCullMode() == EEntityCullMode::RootOnly)
            return true;
    return false;
}

static void gatherTreeCullBounds(Entity* entity, const Transform& toRoot, Sphere& bounds, bool& any)
{
    if (const RenderComponent* render = getComponent<RenderComponent>(entity); render && render->node.isValid())
    {
        const Sphere& local = render->node.getLocalBounds();
        if (local.radius >= 0.0f && std::isfinite(local.radius) && !glm::any(glm::isnan(local.pos)) && !glm::any(glm::isinf(local.pos)))
        {
            const Transform t = composeTransform(toRoot, render->localTransform);
            const float inflate = render->node.isSkinned() ? Globals::spatialIndex.getCullingConfig().skinnedRadiusScale : 1.0f;
            Sphere placed(t.quat * (local.pos * t.scale) + t.pos, local.radius * t.scale * inflate);
            if (any)
                bounds.combineSphere(placed);
            else
                bounds = placed;
            any = true;
        }
    }
    if (const HumanoidAnimatorComponent* humanoid = getComponent<HumanoidAnimatorComponent>(entity))
        humanoid->addRestBounds(toRoot, bounds, any);
    if (const SceneComponent* sc = getComponent<SceneComponent>(entity))
        for (const EntityPtr& child : sc->children)
            gatherTreeCullBounds(child.get(), composeTransform(toRoot, Transform(child->pos, child->scale, child->rot)), bounds, any);
}

void Entity::refreshTreeCullBounds()
{
    if (!spawnTemplate || spawnTemplate->cullMode != EEntityCullMode::RootOnly)
        return;
    Sphere bounds{ glm::vec3(0.0f), 0.0f };
    bool any = false;
    gatherTreeCullBounds(this, Transform(), bounds, any);
    spawnTemplate->treeCullBounds = bounds;
    oc::atomic_ref<uint32>(spawnTemplate->treeCullBoundsState).store(any ? 2u : 1u);
}

// The template's cached subtree bounds placed at `world`.
static Sphere placeTreeCullBounds(const EntitySpawnTemplate& tmpl, const Transform& world)
{
    const Sphere& local = tmpl.treeCullBounds;
    return Sphere(world.quat * (local.pos * world.scale) + world.pos, local.radius * world.scale);
}

void Entity::placeSpatialEntry()
{
    if (!spatialEntry.isValid())
        return;
    if (getCullMode() == EEntityCullMode::RootOnly)
    {
        const Sphere bounds = placeTreeCullBounds(*spawnTemplate, Transform(pos, scale, rot));
        Globals::spatialIndex.updateEntry(spatialEntry.handle(), glm::dvec3(bounds.pos), bounds.radius);
        return;
    }
    const RenderComponent* render = getComponent<RenderComponent>(this);
    const float radius = render && render->node.isValid() ? render->node.getWorldBounds().radius : 0.0f;
    Globals::spatialIndex.updateEntry(spatialEntry.handle(), glm::dvec3(pos), radius);
}

// The render pass mask a spatial entry's stamps give under the culling config.
static uint32 spatialRenderPassMask(SpatialHandle handle)
{
    const SpatialIndex& spatialIndex = Globals::spatialIndex;
    const SpatialCullingConfig& culling = spatialIndex.getCullingConfig();
    if (culling.mode < int(ESpatialCullMode::Cull))
        return RendererVKLayout::PASS_ALL;
    const uint32 spatialMask = spatialIndex.getPassMask(handle);
    if (spatialMask & SpatialPassBit_Main)
        return RendererVKLayout::PASS_ALL;
    if (culling.mode == int(ESpatialCullMode::MainOnly))
        return 0;
    if (spatialMask & SpatialPassBit_Near)
        return RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI; // off-screen but shadow/RT relevant
    if (spatialMask & SpatialPassBit_Shadow)
        return RendererVKLayout::PASS_SHADOW; // off-screen, up-sun of the view
    return 0;
}

void Entity::updateSelf(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, uint32 cullPassMask, oc::vector<EntityUpdateNode>& outChildren)
{
    // ProfileScope profileScope("Entity::updateSelf", EProfileCategory::Entity);
    const ComponentOffsets offsets = getComponentOffsets(typeBits);

    SceneComponent* sc = getComponent<SceneComponent>(this, offsets);
    if (!isEnabled())
    {
        if (!isPhysicsSuspended())
        {
            suspendPhysicsTree(*this, sc);
            setPhysicsSuspended(true);
        }
        return;
    }
    if (isPhysicsSuspended())
        setPhysicsSuspended(false);

    const bool frozen = isFrozen() || Globals::time.isPaused();
    const bool simStep = !frozen && deltaSeconds > 0.0f;
    if (!frozen)
    {
        if (simStep)
        {
            if (GameUnitComponent* gameUnit = getComponent<GameUnitComponent>(this, offsets))
                gameUnit->update(*this, deltaSeconds);
            if (GameStructureComponent* gameStructure = getComponent<GameStructureComponent>(this, offsets))
                gameStructure->update(*this, deltaSeconds);
            if (GameProjectileComponent* gameProjectile = getComponent<GameProjectileComponent>(this, offsets))
                gameProjectile->update(*this, deltaSeconds);

            if (ScriptComponent* script = getComponent<ScriptComponent>(this, offsets))
                script->update(*this, deltaSeconds);
        }

        if (NetworkComponent* network = getComponent<NetworkComponent>(this, offsets))
            network->update(*this, deltaSeconds);

        if (AnimatorComponent* animator = getComponent<AnimatorComponent>(this, offsets); animator && simStep)
            animator->update(*this, renderer, deltaSeconds);

        if (PhysicsComponent* physics = getComponent<PhysicsComponent>(this, offsets))
            physics->update(*this, parentWorld);

        if (HumanoidAnimatorComponent* humanoid = getComponent<HumanoidAnimatorComponent>(this, offsets); humanoid && simStep)
            humanoid->update(*this, deltaSeconds);
    }

    const Transform world = composeTransform(parentWorld, Transform(pos, scale, rot));
    RenderComponent* render = getComponent<RenderComponent>(this, offsets);
    if (render && !render->node.isValid())
        render = nullptr;
    if (render)
        render->place(world);

    const EEntityCullMode cullMode = getCullMode();
    uint32 passMask = RendererVKLayout::PASS_ALL;
    uint32 childCullPassMask = cullPassMask;
    if (spatialEntry.isValid())
    {
        const bool cullRoot = cullMode == EEntityCullMode::RootOnly;
        if (cullRoot)
        {
            const Sphere bounds = placeTreeCullBounds(*spawnTemplate, world);
            Globals::spatialIndex.updateEntry(spatialEntry.handle(), glm::dvec3(bounds.pos), bounds.radius);
        }
        else if (render)
        {
            const Sphere bounds = render->node.getWorldBounds();
            const float radius = render->node.isSkinned() ? bounds.radius * Globals::spatialIndex.getCullingConfig().skinnedRadiusScale : bounds.radius;
            Globals::spatialIndex.updateEntry(spatialEntry.handle(), glm::dvec3(bounds.pos), radius);
        }
        else
            Globals::spatialIndex.updateEntry(spatialEntry.handle(), glm::dvec3(world.pos), 0.0f);
        if (render || cullRoot)
            passMask = spatialRenderPassMask(spatialEntry.handle());
        childCullPassMask = cullRoot ? passMask : EntityCullPass_Own;
    }
    else if (cullPassMask != EntityCullPass_Own && cullMode != EEntityCullMode::None)
        passMask = cullPassMask;
    if (render && passMask != 0)
        renderer.renderNode(render->node, passMask);

    if (HumanoidAnimatorComponent* humanoid = getComponent<HumanoidAnimatorComponent>(this, offsets))
        humanoid->place(renderer, world, passMask);

    if (AudioComponent* audio = getComponent<AudioComponent>(this, offsets))
        audio->update(*this, world);

    if (ParticleComponent* particle = getComponent<ParticleComponent>(this, offsets))
        if (!frozen)
            particle->update(*this, world, deltaSeconds);

    if (ForceComponent* force = getComponent<ForceComponent>(this, offsets))
        force->update(*this, world);

    if (LightComponent* light = getComponent<LightComponent>(this, offsets))
        light->update(*this, renderer, world);

    if (sc)
        for (const EntityPtr& child : sc->children)
            outChildren.push_back({ child.get(), world, childCullPassMask });
}

void Entity::update(Renderer& renderer, float deltaSeconds, const Transform& parentWorld, uint32 cullPassMask)
{
    oc::vector<EntityUpdateNode> children;
    updateSelf(renderer, deltaSeconds, parentWorld, cullPassMask, children);
    for (const EntityUpdateNode& child : children)
        child.entity->update(renderer, deltaSeconds, child.parentWorld, child.cullPassMask);
}

// Recursive alloc size of the template's entity + its whole SceneComponent child tree, lazily cached on
// the template (idempotent, so the racy relaxed store is benign - every writer stores the same value).
static uint32 getTreeAllocSize(const EntitySpawnTemplate& tmpl)
{
    const uint32 cached = oc::atomic_ref<uint32>(tmpl.treeAllocSize).load(oc::memory_order_relaxed);
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
    oc::atomic_ref<uint32>(tmpl.treeAllocSize).store(size, oc::memory_order_relaxed);
    return size;
}

// NetworkComponents over the template's entity + its whole SceneComponent child tree, lazily
// cached like getTreeAllocSize (idempotent racy relaxed store).
static uint32 getTreeNetworkCount(const EntitySpawnTemplate& tmpl)
{
    const uint32 cached = oc::atomic_ref<uint32>(tmpl.treeNetworkCount).load(oc::memory_order_relaxed);
    if (cached != UINT32_MAX)
        return cached;

    uint32 count = (tmpl.archetype.typeBits >> EComponentID_Network) & 1;
    if (tmpl.archetype.typeBits & (1 << EComponentID_Scene))
    {
        const auto* info = static_cast<const SceneComponent::SpawnInfo*>(tmpl.spawnInfos[0].get());
        for (const SceneComponent::SpawnInfo::ChildSpawnInfo& child : info->children)
            if (child.tmpl)
                count += getTreeNetworkCount(*child.tmpl);
    }
    oc::atomic_ref<uint32>(tmpl.treeNetworkCount).store(count, oc::memory_order_relaxed);
    return count;
}

EntityPtr Entity::create(const EntitySpawnTemplate& tmpl, const Transform& transform, uint8 initialFlags)
{
    // One allocation for the whole prefab tree: root + every recursive child. The spawn recursion
    // (SceneComponent::spawn -> the create overload below) carves each entity's slice from the cursor;
    // slices free themselves individually on destroy.
    uint8* treeCursor = static_cast<uint8*>(Globals::entityAllocator.allocate(getTreeAllocSize(tmpl)));
    // PARALLEL SPAWNING + server id contiguity: a replicated tree's netIds must mint back-to-back
    // (the client adopts base + cursor in DFS order), so a server tree that mints MORE THAN ONE id
    // holds the manager's register lock across the whole tree spawn. A single-component tree -
    // every unit/projectile prefab - mints atomically inside registerEntity and stays parallel.
    const bool lockNetIds = getTreeNetworkCount(tmpl) > 1
        && Globals::networkManager.role() == ENetRole::Server;
    if (lockNetIds)
        Globals::networkManager.beginTreeRegistration();
    if (tmpl.global)
        initialFlags |= EEntityFlag_Global; // root only: the recursion below never passes it to children
    EntityPtr root = create(tmpl, transform, initialFlags | EEntityFlag_RootAllocation, treeCursor, nullptr);
    if (lockNetIds)
        Globals::networkManager.endTreeRegistration();
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

    entity->spawnTemplate = &tmpl; // also the name: getName falls back to tmpl.displayName (no registry entry, no allocation)
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

    // Spatial registration for EVERY entity (parallel-spawn safe: registerEntry locks). Bounds from
    // the render node when there is one (skinned inflated by the culling config), else a point at
    // the spawn position (for a tree CHILD that is its LOCAL position - the entry links at the
    // next commit and the child's first visit re-places it in world space before any query can
    // see it). Headless has no render nodes, so every entry there is a point.
    // CullMode: None registers nothing, and neither does anything spawned under a RootOnly
    // ancestor; the RootOnly root itself registers over its whole subtree (already spawned above).
    if (tmpl.cullMode == EEntityCullMode::RootOnly && !hasRootOnlyAncestor(parent))
    {
        assert(!glm::any(glm::isnan(transform.pos)) && !glm::any(glm::isinf(transform.pos)) && "spawn at a non-finite position");
        if (oc::atomic_ref<uint32>(tmpl.treeCullBoundsState).load(oc::memory_order_relaxed) == 0)
            entity->refreshTreeCullBounds();
        const Sphere bounds = placeTreeCullBounds(tmpl, transform);
        const uint32 layers = tmpl.treeCullBoundsState == 2 ? SpatialLayer_Entity | SpatialLayer_Render : SpatialLayer_Entity;
        entity->spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(glm::dvec3(bounds.pos), bounds.radius, reinterpret_cast<uint64>(entity), layers));
    }
    else if (tmpl.cullMode == EEntityCullMode::PerEntity && !hasRootOnlyAncestor(parent))
    {
        glm::dvec3 center(transform.pos);
        float radius = 0.0f;
        uint32 layers = SpatialLayer_Entity;
        assert(!glm::any(glm::isnan(transform.pos)) && !glm::any(glm::isinf(transform.pos)) && "spawn at a non-finite position");
        if (const RenderComponent* render = getComponent<RenderComponent>(entity); render && render->node.isValid())
        {
            const Sphere bounds = render->node.getWorldBounds();
            // A node whose bounds are not usable yet (no mesh bounds, a degenerate sphere) reports
            // a negative or non-finite radius: keep the spawn point instead of tripping the
            // index's `radius >= 0` assert - the first visit re-places the entry from real bounds.
            const bool usable = bounds.radius >= 0.0f && std::isfinite(bounds.radius)
                && !glm::any(glm::isnan(bounds.pos)) && !glm::any(glm::isinf(bounds.pos));
            if (usable)
            {
                center = glm::dvec3(bounds.pos);
                radius = render->node.isSkinned() ? bounds.radius * Globals::spatialIndex.getCullingConfig().skinnedRadiusScale : bounds.radius;
            }
            layers |= SpatialLayer_Render;
        }
        entity->spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(center, radius, reinterpret_cast<uint64>(entity), layers));
    }

    return EntityPtr(entity);
}

void Entity::destroy(Entity* entity)
{
    constexpr uint8 rootContiguous = EEntityFlag_RootAllocation | EEntityFlag_ContiguousAllocation;

    // Intact allocation root: verify every member is owned solely by its parent's children list before
    // committing to the one-chunk free - an externally referenced member would outlive this teardown.
    // On failure the allocation splits: each child subtree re-checks at its own death, so only the path
    // to the offending member degrades to per-entity freeing.
    // (Residual hole: a script OnDestroy grabbing a ref to a sibling member mid-teardown.)
    if ((entity->flags & rootContiguous) == rootContiguous && !contiguousTreeSolelyOwned(entity))
        breakContiguousAllocationFromRoot(entity);

    // PARALLEL DESTRUCTION: stop receiving global script events BEFORE anything is torn down (and
    // wait for dispatches already in flight on other workers to finish with this entity).
    if (ScriptComponent* script = getComponent<ScriptComponent>(entity))
        script->detachListener(*entity);

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

    Globals::entityNames.erase(entity);
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
    case EComponentID_HumanoidAnimator:
    {
        HumanoidAnimatorComponent* ha = reinterpret_cast<HumanoidAnimatorComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        new (ha) HumanoidAnimatorComponent();
        ha->spawn(*this, *static_cast<const HumanoidAnimatorComponent::SpawnInfo*>(info), base);
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
    case EComponentID_HumanoidAnimator:
    {
        HumanoidAnimatorComponent* ha = reinterpret_cast<HumanoidAnimatorComponent*>(reinterpret_cast<uint8*>(this) + componentOffset);
        ha->destroy(*this, *static_cast<const HumanoidAnimatorComponent::SpawnInfo*>(info));
        ha->~HumanoidAnimatorComponent();
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
    { // Moving within the same allocation keeps the tree intact, any other destination leaves it.
        Entity* oldRoot = findAllocationRoot(parent);
        Entity* newRoot = (newParent && hasComponent<SceneComponent>(newParent) && !isSelfOrDescendant(newParent, this))
                        ? findAllocationRoot(newParent) : nullptr;
        if (!oldRoot || oldRoot != newRoot)
            breakContiguousAllocation(this);
    }
    detachKeepAllocation(this); // break decision made above; reparenting a whole allocation root never breaks

    if (newParent && !hasComponent<SceneComponent>(newParent))
    {
        Log::warning("Entity: cannot parent '" + oc::string(getName()) + "' under '" + oc::string(newParent->getName()) + 
            "' - the parent has no SceneComponent (add 'Component Scene' to its prefab); the entity is now UNPARENTED");
        return;
    }
    if (newParent && isSelfOrDescendant(newParent, this))
    {
        Log::warning("Entity: cannot parent '" + oc::string(getName()) + "' under its own descendant '" + 
            oc::string(newParent->getName()) + "' (cycle); the entity is now UNPARENTED");
        return;
    }
    parent = newParent;

    if (newParent)
    {
        getComponent<SceneComponent>(newParent)->addChild(oc::move(keepAlive));
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

const oc::string& Entity::getPrefabName() const
{
    static const oc::string empty;
    return spawnTemplate ? spawnTemplate->prefabName : empty;
}

const oc::string& Entity::getSourceFile() const
{
    static const oc::string empty;
    return spawnTemplate ? spawnTemplate->sourceFile : empty;
}
