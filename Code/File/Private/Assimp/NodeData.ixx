export module File:NodeData;

import Core;
import Core.glm;
import :INodeData;

export class aiNode;

export class NodeData final : public INodeData
{
public:
    NodeData() {}
    NodeData(const aiNode* pNode) { initialize(pNode); }
    ~NodeData() {}

    bool initialize(const aiNode* pNode);
    operator bool() const override { return m_pNode != nullptr; }
    bool operator==(const INodeData& other) const override
    {
        return getName() == other.getName();
    }
    bool isValid() const override { return m_pNode != nullptr; }

    oc::unique_ptr<INodeData> clone() const override;

    const char* getName() const override;
    uint32 getNumChildren() const override;
    oc::unique_ptr<INodeData> getChild(uint32 idx) const override;
    uint32 getNumMeshes() const override;
    uint32 getMeshIndex(uint32 meshIdx) const override;
    void getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const override;
    glm::vec3 getPosition() const override;
    glm::vec3 getScale() const override;
    glm::quat getRotation() const override;

    uint32 getNumChildrenRecursive() const override;

    //oc::unique_ptr<INodeData> findChild(oc::initializer_list<const char*> hierarchy) const override;
    oc::vector<oc::string> getChildrenNames() const override;

private:

    const aiNode* m_pNode = nullptr;
};