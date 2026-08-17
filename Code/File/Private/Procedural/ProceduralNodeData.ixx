export module File:ProceduralNodeData;

import Core;
import Core.glm;
import :INodeData;

export class ProceduralNodeData final : public INodeData
{
public:
	ProceduralNodeData() = default;
	~ProceduralNodeData() = default;

	bool initialize(const char* name, oc::vector<uint32> meshIndices = {});

	operator bool()                          const override { return !m_name.empty(); }
	bool operator==(const INodeData& other)  const override { return m_name == other.getName(); }
	bool isValid()                           const override { return !m_name.empty(); }

	oc::unique_ptr<INodeData> clone() const override;

	const char* getName() const override { return m_name.c_str(); }

	uint32                     getNumChildren()          const override { return 0; }
	oc::unique_ptr<INodeData> getChild(uint32 idx)      const override { (void)idx; return nullptr; }
	uint32                     getNumChildrenRecursive() const override { return 0; }
	oc::vector<oc::string>   getChildrenNames()        const override { return {}; }

	uint32 getNumMeshes()              const override { return (uint32)m_meshIndices.size(); }
	uint32 getMeshIndex(uint32 meshIdx) const override;

	void      getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const override;
	glm::vec3 getPosition() const override { return glm::vec3(0.0f); }
	glm::vec3 getScale()    const override { return glm::vec3(1.0f); }
	glm::quat getRotation() const override { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }

private:
	oc::string          m_name;
	oc::vector<uint32>  m_meshIndices;
};
