module File;

import Core;
import Core.glm;

import :NodeData;
import :Assimp;

oc::unique_ptr<INodeData> NodeData::clone() const
{
    return oc::make_unique<NodeData>(m_pNode);
}

bool NodeData::initialize(const aiNode* pNode)
{
    m_pNode = pNode;
    return true;
}

const char* NodeData::getName() const
{
    return m_pNode->mName.C_Str();
}

uint32 NodeData::getNumChildren() const
{
    return m_pNode->mNumChildren;
}

oc::unique_ptr<INodeData> NodeData::getChild(uint32 idx) const
{
    assert(idx < m_pNode->mNumChildren);
    return oc::make_unique<NodeData>(m_pNode->mChildren[idx]);
}

uint32 NodeData::getNumMeshes() const
{
    return m_pNode->mNumMeshes;
}

uint32 NodeData::getMeshIndex(uint32 meshIdx) const
{
    assert(meshIdx < m_pNode->mNumMeshes);
    return m_pNode->mMeshes[meshIdx];
}

void NodeData::getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const
{
    // aiQuaternion is laid out w,x,y,z; glm::quat defaults to x,y,z,w in memory, so it can't be aliased
    // directly. Decompose into a real aiQuaternion, then build the glm::quat by component (w,x,y,z).
    aiQuaternion q;
    m_pNode->mTransformation.Decompose(reinterpret_cast<aiVector3D&>(scale), q, reinterpret_cast<aiVector3D&>(pos));
    rot = glm::quat(q.w, q.x, q.y, q.z);
}

glm::vec3 NodeData::getPosition() const
{
    aiVector3f pos;
    aiVector3f scale;
    aiQuaternion rot;
    m_pNode->mTransformation.Decompose(scale, rot, pos);
    return glm::vec3(pos.x, pos.y, pos.z);
}

glm::quat NodeData::getRotation() const
{
	aiVector3f pos;
	aiVector3f scale;
	aiQuaternion rot;
	m_pNode->mTransformation.Decompose(scale, rot, pos);
	return glm::quat(rot.w, rot.x, rot.y, rot.z);
}

glm::vec3 NodeData::getScale() const
{
    aiVector3f pos;
    aiVector3f scale;
    aiQuaternion rot;
    m_pNode->mTransformation.Decompose(scale, rot, pos);
    return glm::vec3(scale.x, scale.y, scale.z);
}

uint32 NodeData::getNumChildrenRecursive() const
{
    uint32 numChildren = m_pNode->mNumChildren;
    for (uint32 i = 0; i < m_pNode->mNumChildren; i++)
    {
        numChildren += NodeData(m_pNode->mChildren[i]).getNumChildrenRecursive();
    }
    return numChildren;
}
/*
oc::unique_ptr<INodeData> NodeData::findChild(oc::initializer_list<const char*> hierarchy) const
{
    const aiNode* pNode = m_pNode;
    for (const char* name : hierarchy)
    {
        for (uint32 childIdx = 0; childIdx < pNode->mNumChildren; childIdx++)
        {
            if (strcmp(pNode->mChildren[childIdx]->mName.C_Str(), name) == 0)
            {
                pNode = pNode->mChildren[childIdx];
                break;
            }
        }
        if (pNode == m_pNode)
        {
            assert(false && "Could not find child node with name");
            pNode = nullptr;
            break;
        }
    }

    return oc::make_unique<NodeData>(pNode);
}*/

oc::vector<oc::string> NodeData::getChildrenNames() const
{
    oc::vector<oc::string> childrenNames;
    for (uint32 i = 0; i < m_pNode->mNumChildren; i++)
    {
        childrenNames.push_back(m_pNode->mChildren[i]->mName.C_Str());
    }
    return childrenNames;
}
