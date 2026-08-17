module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;

Transform composeTransform(const Transform& parent, const Transform& local)
{
    return Transform(
        parent.pos + parent.quat * (local.pos * parent.scale),
        parent.scale * local.scale,
        glm::normalize(parent.quat * local.quat));
}

static void detachFromParent(Entity* parent, Entity* child)
{
    SceneComponent* psc = getComponent<SceneComponent>(parent);
    if (!psc)
        return;
    auto& kids = psc->children;
    auto it = oc::find_if(kids.begin(), kids.end(),
        [child](const EntityPtr& p) { return p.get() == child; });
    if (it != kids.end())
        kids.erase(it);
}

void detachFromOwner(Entity* child)
{
    if (!child->parent)
        return;
    // Deletion path: the child leaves the tree while the allocation lives on (external refs may even
    // keep it alive past the root), so the whole allocation reverts to per-entity freeing.
    breakContiguousAllocation(child);
    detachFromParent(child->parent, child);
}

void detachKeepAllocation(Entity* child)
{
    if (child->parent)
        detachFromParent(child->parent, child);
}

Entity* findAllocationRoot(Entity* entity)
{
    for (Entity* p = entity; p; p = p->parent)
    {
        if (!(p->flags & EEntityFlag_ContiguousAllocation))
            return nullptr; // broken chain — no intact allocation above
        if (p->flags & EEntityFlag_RootAllocation)
            return p;
    }
    return nullptr;
}

// Every intact contiguous non-root child of `parent` (except `skip`) becomes the root of its own
// allocation: subtrees are contiguous DFS ranges within the block, and each member's template caches
// its exact subtree size, so a promoted root frees its range in one deallocate when it dies intact.
static void promoteChildSubtrees(Entity* parent, const Entity* skip)
{
    SceneComponent* sc = getComponent<SceneComponent>(parent);
    if (!sc)
        return;
    for (const EntityPtr& c : sc->children)
    {
        Entity* child = c.get();
        if (child == skip || !(child->flags & EEntityFlag_ContiguousAllocation) || (child->flags & EEntityFlag_RootAllocation))
            continue; // the path continues below / broken member / grafted allocation — self-managing
        child->flags |= EEntityFlag_RootAllocation;
    }
}

void breakContiguousAllocation(Entity* member)
{
    if (!((member->flags & EEntityFlag_ContiguousAllocation) && !(member->flags & EEntityFlag_RootAllocation)))
        return; // standalone/broken, or an allocation root — moving a whole allocation never breaks it
    if (!findAllocationRoot(member))
    {
        assert(false); // a flagged member always has an intact chain to its root
        return;
    }
    // Split instead of a full clear: every ancestor on the path to the allocation root becomes a lone
    // per-entity slice, each off-path subtree becomes its own intact allocation, and the departing
    // member leaves as one too. Only the path loses chunk freeing.
    const Entity* pathChild = member;
    for (Entity* p = member->parent; ; p = p->parent)
    {
        const bool isAllocationRoot = (p->flags & EEntityFlag_RootAllocation) != 0;
        promoteChildSubtrees(p, pathChild);
        p->flags &= uint8(~(EEntityFlag_ContiguousAllocation | EEntityFlag_RootAllocation));
        if (isAllocationRoot)
            break;
        pathChild = p;
    }
    member->flags |= EEntityFlag_RootAllocation;
}

void breakContiguousAllocationFromRoot(Entity* root)
{
    assert(root->flags & EEntityFlag_RootAllocation);
    // Destroy-time fallback: the root reverts to freeing its own slice, each child subtree becomes its
    // own allocation and re-runs the solely-owned check at its own death — so an externally referenced
    // member only degrades the path leading to it, recursively, instead of the whole tree.
    promoteChildSubtrees(root, nullptr);
    root->flags &= uint8(~(EEntityFlag_ContiguousAllocation | EEntityFlag_RootAllocation));
}

bool contiguousTreeSolelyOwned(Entity* entity)
{
    SceneComponent* sc = getComponent<SceneComponent>(entity);
    if (!sc)
        return true;
    for (const EntityPtr& c : sc->children)
    {
        Entity* child = c.get();
        if (!(child->flags & EEntityFlag_ContiguousAllocation) || (child->flags & EEntityFlag_RootAllocation))
            continue; // self-managing (broken member or grafted allocation root) — not part of this block
        if (oc::atomic_ref<uint16>(child->refCount).load(oc::memory_order_relaxed) != 1)
            return false; // an external EntityPtr would outlive the tree teardown
        if (!contiguousTreeSolelyOwned(child))
            return false;
    }
    return true;
}

int componentIdFromName(oc::string_view name)
{
    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
        if (name == componentTypeName(EComponentID(i)))
            return int(i);
    return -1;
}
