export module File:ProceduralMaterialData;

import Core;
import Core.glm;
import :IMaterialData;

export class ProceduralMaterialData final : public IMaterialData
{
public:
	ProceduralMaterialData();
	~ProceduralMaterialData();
	ProceduralMaterialData(const ProceduralMaterialData&) = delete;
	ProceduralMaterialData(ProceduralMaterialData&&) = default;

	bool initialize(const char* name = nullptr);

	const char* getName() const override;

	glm::vec3  getBaseColor()         const override;
	glm::vec3  getEmissiveColor()     const override;
	glm::vec3  getSpecularColor()     const override;
	float      getRoughnessFactor()   const override;
	float      getMetalnessFactor()   const override;
	float      getOpacity()           const override;
	EAlphaMode getAlphaMode()         const override;
	float      getAlphaCutoff()       const override;
	float      getEmissiveIntensity() const override;
	float      getRefractiveIndex()   const override;

	oc::string getTexturePath(ETextureType type) const override;

	uint32 getDiffuseTexIdx() const override;
	uint32 getNormalTexIdx()  const override;
	uint32 getOpacityTexIdx() const override;
	uint32 getMetalRoughnessTexIdx() const override;

	// Setters used by ProceduralSceneData to wire up texture indices
	void setDiffuseTexIdx(uint32 idx)        { m_diffuseTexIdx        = idx; }
	void setNormalTexIdx(uint32 idx)         { m_normalTexIdx         = idx; }
	void setMetalRoughnessTexIdx(uint32 idx) { m_metalRoughnessTexIdx = idx; }

private:
	oc::string m_name;
	glm::vec3   m_baseColor         = { 1.0f, 1.0f, 1.0f };
	glm::vec3   m_emissiveColor     = { 0.0f, 0.0f, 0.0f };
	glm::vec3   m_specularColor     = { 0.0f, 0.0f, 0.0f };
	float       m_roughness         = 1.0f;
	float       m_metalness         = 0.0f;
	float       m_opacity           = 1.0;
	float       m_alphaCutoff       = 0.0f;
	float       m_emissiveIntensity = 1.0f;
	float       m_refractiveIndex   = 1.5f;
	uint32      m_diffuseTexIdx          = UINT32_MAX;
	uint32      m_normalTexIdx           = UINT32_MAX;
	uint32      m_metalRoughnessTexIdx   = UINT32_MAX;
};
