export module Entity:AssetRegistryState;

import Core;
import File;
import :AnimationDescription;

export class World; // declared before their friend lines
export class WorldState;

// Everything of AssetRegistry but its public API (AssetRegistry.ixx): the name tables, the directory scan and the World-only lookups.
export class AssetRegistryState
{
protected:

    friend class World;      // scans at initialize, resolves clips / animators / file roots while building templates
    friend class WorldState;

    void scanDirectory(const oc::string& rootDir = ".");
    void clear();
    void registerFile(const oc::string& path);
    const AnimationClipDesc* findClip(const oc::string& name) const;
    const AnimatorDesc* findAnimator(const oc::string& name) const;
    const oc::string* findRootForFile(const oc::string& fileName) const;

    oc::unordered_map<oc::string, ObjectContainerDesc> m_objectContainers;
    oc::unordered_map<oc::string, AnimationClipDesc> m_clips;  // .anm Animation entries, global by name
    oc::unordered_map<oc::string, AnimatorDesc> m_animators;   // .apl Animator entries, global by name
    oc::unordered_map<oc::string, oc::string> m_prefabs;  // name -> .pre file path
    oc::unordered_map<oc::string, oc::string> m_fileRoot; // lowercased file name -> root entity/prefab name
};
