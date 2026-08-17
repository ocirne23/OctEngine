export module Entity:AssetRegistry;

import Core;
import File;
import :AnimationDescription;

export class AssetRegistry final
{
public:

    void scanDirectory(const oc::string& rootDir = ".");

    void clear();

    const ObjectContainerDesc* findObjectContainer(const oc::string& name) const;

    const AnimationClipDesc* findClip(const oc::string& name) const;

    const AnimatorDesc* findAnimator(const oc::string& name) const;

    const oc::string* findPrefab(const oc::string& name) const;

    void addPrefab(const oc::string& name, const oc::string& path);

    const oc::string* findRootForFile(const oc::string& fileName) const;

    const oc::unordered_map<oc::string, ObjectContainerDesc>& getObjectContainers() const { return m_objectContainers; }

    // .apl Animator entries by name — for editor tooling (Entity Editor) to offer a searchable list.
    const oc::unordered_map<oc::string, AnimatorDesc>& getAnimators() const { return m_animators; }

private:

    void registerFile(const oc::string& path);

    oc::unordered_map<oc::string, ObjectContainerDesc> m_objectContainers;
    oc::unordered_map<oc::string, AnimationClipDesc> m_clips;  // .anm Animation entries, global by name
    oc::unordered_map<oc::string, AnimatorDesc> m_animators;   // .apl Animator entries, global by name
    oc::unordered_map<oc::string, oc::string> m_prefabs; // name -> .pre file path
    oc::unordered_map<oc::string, oc::string> m_fileRoot; // lowercased file name -> root entity/prefab name
};

export namespace Globals
{
    AssetRegistry assetRegistry;
}
