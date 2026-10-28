export module Entity:AssetRegistry;

import Core;
import File;
import :AssetRegistryState; // the name tables and the directory scan

// The registry of named assets under Assets/ (Globals::assetRegistry): ObjectContainers (.oc), clips (.anm), animator graphs (.apl) and prefabs (.pre),
// registered by World::initialize's scan. The state is in AssetRegistryState.ixx.
export class AssetRegistry final : public AssetRegistryState
{
public:

    const ObjectContainerDesc* findObjectContainer(const oc::string& name) const;
    const oc::string* findPrefab(const oc::string& name) const; // the .pre path
    void addPrefab(const oc::string& name, const oc::string& path);

    // For editor tooling (the Entity Editor) to offer searchable lists.
    const oc::unordered_map<oc::string, ObjectContainerDesc>& getObjectContainers() const { return m_objectContainers; }
    const oc::unordered_map<oc::string, AnimatorDesc>& getAnimators() const { return m_animators; }
};

export namespace Globals
{
    AssetRegistry assetRegistry;
}
