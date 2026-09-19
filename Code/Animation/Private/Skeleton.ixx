export module Animation:Skeleton;

import Core;
import Core.glm;

// A flattened bone/joint hierarchy for skeletal animation. Bones are stored parent-before-child so a
// single forward pass can compose global transforms (a child's parent is always already computed). Every
// node of the source hierarchy becomes a bone here (intermediate non-skinning nodes included) so the
// transform chain stays intact; nodes that are not actual skin bones simply keep an identity inverseBind.
export struct Skeleton
{
    oc::vector<oc::string> boneNames;                   // parent-before-child order
    oc::vector<int32> parentIndices;                     // index into this skeleton, -1 for a root
    oc::vector<glm::mat4> localBind;                     // node-local bind-pose transform
    oc::vector<glm::mat4> inverseBind;                   // mesh space -> bone space (offset matrix); identity if not a skin bone
    oc::unordered_map<oc::string, uint32> nameToIndex;

    uint32 numBones() const { return (uint32)boneNames.size(); }
    bool isValid() const { return !boneNames.empty(); }

    int32 findBone(const oc::string& name) const
    {
        const auto it = nameToIndex.find(name);
        return it == nameToIndex.end() ? -1 : (int32)it->second;
    }

    // Code-built skeletons (no skin: identity inverseBind). The parent must already be added. A duplicate
    // name stays addressable by index only - the first bone keeps the name.
    uint32 addBone(const oc::string& name, int32 parent, const glm::mat4& bind = glm::mat4(1.0f))
    {
        assert(parent < (int32)numBones());
        const uint32 idx = numBones();
        boneNames.push_back(name);
        parentIndices.push_back(parent);
        localBind.push_back(bind);
        inverseBind.push_back(glm::mat4(1.0f));
        nameToIndex.try_emplace(name, idx);
        return idx;
    }
};
