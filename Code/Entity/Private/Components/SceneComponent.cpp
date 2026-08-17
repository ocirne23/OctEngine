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
        children.emplace_back(oc::move(childEntity)); // attach the owning handle directly — reparentEntity would break the fresh allocation
    }
}

void SceneComponent::destroy(Entity& entity, const SpawnInfo& info)
{

}
