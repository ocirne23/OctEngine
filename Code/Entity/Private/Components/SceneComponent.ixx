export module Entity:SceneComponent;

import Core;
import Core.Transform;
import :Entity;

export struct SceneComponent
{
    static constexpr EComponentID getId() { return EComponentID_Scene; }

    Entity* getEntity();

    struct SpawnInfo
    {
        struct ChildSpawnInfo
        {
            oc::shared_ptr<const EntitySpawnTemplate> tmpl;
            Transform localTransform;                  // placement composed onto the parent's spawn transform
            oc::string name;                          // name override, empty = use the template's
            bool enabled = true;                       // reference-site "Enabled false" (a prefab reference has no template of its own to carry it)
        };
        oc::vector<ChildSpawnInfo> children;
    };

    // READ freely; mutate through the calls below (the one place to hook a future "children changed").
    // Main thread, outside the entity pass.
    oc::vector<EntityPtr> children;

    void addChild(EntityPtr child);                       // appends; does NOT set child->parent
    bool removeChild(Entity* child);                      // false = not a child
    bool replaceChild(Entity* oldChild, EntityPtr child); // same slot, so siblings and order stay
    void adoptChildren(SceneComponent& from);             // takes the whole list and re-parents it

    // treeCursor: children carve their slices from the tree's single spawn allocation (see Entity::create)
    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base, uint8*& treeCursor);
	void destroy(Entity& entity, const SpawnInfo& info);
};
