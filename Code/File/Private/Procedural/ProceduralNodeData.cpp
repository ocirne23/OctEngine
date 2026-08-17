module File;

import Core;
import Core.glm;

import :ProceduralNodeData;

bool ProceduralNodeData::initialize(const char* name, oc::vector<uint32> meshIndices)
{
	m_name        = name ? name : "ProceduralNode";
	m_meshIndices = oc::move(meshIndices);
	return true;
}

oc::unique_ptr<INodeData> ProceduralNodeData::clone() const
{
	auto node = oc::make_unique<ProceduralNodeData>();
	node->initialize(m_name.c_str(), m_meshIndices);
	return node;
}

uint32 ProceduralNodeData::getMeshIndex(uint32 meshIdx) const
{
	assert(meshIdx < m_meshIndices.size());
	return m_meshIndices[meshIdx];
}

void ProceduralNodeData::getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const
{
	pos   = glm::vec3(0.0f);
	scale = glm::vec3(1.0f);
	rot   = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}
