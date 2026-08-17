module File;

import Core;
import Core.glm;

import :ProceduralMaterialData;

ProceduralMaterialData::ProceduralMaterialData()
{
}

ProceduralMaterialData::~ProceduralMaterialData()
{
}

bool ProceduralMaterialData::initialize(const char* name)
{
	m_name = name ? name : "ProceduralMaterial";
	return true;
}

const char* ProceduralMaterialData::getName()             const { return m_name.c_str(); }
glm::vec3   ProceduralMaterialData::getBaseColor()        const { return m_baseColor; }
glm::vec3   ProceduralMaterialData::getEmissiveColor()    const { return m_emissiveColor; }
glm::vec3   ProceduralMaterialData::getSpecularColor()    const { return m_specularColor; }
float       ProceduralMaterialData::getRoughnessFactor()  const { return m_roughness; }
float       ProceduralMaterialData::getMetalnessFactor()  const { return m_metalness; }
float       ProceduralMaterialData::getOpacity()          const { return m_opacity; }

IMaterialData::EAlphaMode ProceduralMaterialData::getAlphaMode() const
{
	return EAlphaMode::Opaque;
}

float ProceduralMaterialData::getAlphaCutoff()       const { return m_alphaCutoff; }
float ProceduralMaterialData::getEmissiveIntensity()  const { return m_emissiveIntensity; }
float ProceduralMaterialData::getRefractiveIndex()    const { return m_refractiveIndex; }

oc::string ProceduralMaterialData::getTexturePath(ETextureType /*type*/) const
{
	return {};
}

uint32 ProceduralMaterialData::getDiffuseTexIdx()          const { return m_diffuseTexIdx; }
uint32 ProceduralMaterialData::getNormalTexIdx()            const { return m_normalTexIdx; }
uint32 ProceduralMaterialData::getOpacityTexIdx()           const { return UINT32_MAX; }
uint32 ProceduralMaterialData::getMetalRoughnessTexIdx()    const { return m_metalRoughnessTexIdx; }
