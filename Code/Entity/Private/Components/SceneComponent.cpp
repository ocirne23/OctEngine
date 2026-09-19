module Entity;

import Core;
import Core.Transform;
import :Entity;

// The owning entity sits directly in front of the component block (see EntityComponentDetail's layout
// table, which lives in :Component and so cannot be reached from this partition's interface).
Entity* SceneComponent::getEntity()
{
    return reinterpret_cast<Entity*>(reinterpret_cast<uint8*>(this) - EntityComponentDetail::entityBaseOffset);
}

void SceneComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base, uint8*& treeCursor)
{
    for (const SpawnInfo::ChildSpawnInfo& child : info.children)
    {
        if (!child.tmpl)
            continue;
        EntityPtr childEntity = Entity::create(*child.tmpl, child.localTransform, 0, treeCursor, &entity); // carve from the tree's single allocation
        if (!child.name.empty())
            childEntity->setName(child.name);
        if (!child.enabled)
            childEntity->setEnabled(false); // the reference site can disable, never re-enable a template's own default
        // attach the owning handle directly - reparentEntity would break the fresh allocation. No
        // childrenChanged(): every animator of a tree still in its spawn is unbound.
        children.emplace_back(oc::move(childEntity));
    }
}

void SceneComponent::destroy(Entity& entity, const SpawnInfo& info)
{

}

void SceneComponent::childrenChanged()
{
    for (Entity* e = getEntity(); e; e = e->parent)
        if (SceneAnimatorComponent* animator = getComponent<SceneAnimatorComponent>(e))
            animator->unbind();
}

void SceneComponent::addChild(EntityPtr child)
{
    children.emplace_back(oc::move(child));
    childrenChanged();
}

bool SceneComponent::removeChild(Entity* child)
{
    auto it = oc::find_if(children.begin(), children.end(),
        [child](const EntityPtr& p) { return p.get() == child; });
    if (it == children.end())
        return false;
    childrenChanged(); // before the erase: it can be the child's last reference
    children.erase(it);
    return true;
}

bool SceneComponent::replaceChild(Entity* oldChild, EntityPtr child)
{
    for (EntityPtr& slot : children)
        if (slot.get() == oldChild)
        {
            childrenChanged();
            slot = oc::move(child);
            return true;
        }
    return false;
}

void SceneComponent::adoptChildren(SceneComponent& from)
{
    from.childrenChanged();
    children = oc::move(from.children);
    Entity* self = getEntity();
    for (EntityPtr& child : children)
        child->parent = self;
    childrenChanged();
}
