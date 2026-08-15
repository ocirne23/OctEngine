module;

#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
#include <cstring>

module File;

// File IS the filesystem library: its readers stream binary ranges directly (Core no longer
// exports these). Entry points call FileSystem::assertIoThread so the main-thread policy still holds.
import <filesystem>;
import <fstream>;

import Core;
import Core.glm;

import :MaterialData;
import :Assimp;

using namespace Assimp;

MaterialData::MaterialData()
{
}

MaterialData::~MaterialData()
{
}

bool MaterialData::initialize(const aiMaterial* pMaterial)
{
    m_pMaterial = pMaterial;
    /*
	printf("Material: %s\n", getName());
	for (uint32 i = aiTextureType_DIFFUSE; i < AI_TEXTURE_TYPE_MAX; i++)
	{
		// print texture paths for debugging
		for (uint32 j = 0; j < m_pMaterial->GetTextureCount(static_cast<aiTextureType>(i)); j++)
		{
			aiString path;
			m_pMaterial->GetTexture(static_cast<aiTextureType>(i), j, &path);
            const bool isEmbedded = path.C_Str()[0] == '*';
			if (isEmbedded)
			{
				const int embeddedTexIdx = atoi(path.C_Str() + 1);
				printf("Embedded texture type: %s, index: %d\n", aiTextureTypeToString(static_cast<aiTextureType>(i)), embeddedTexIdx);
				assert(embeddedTexIdx >= 0 && "Invalid embedded texture index");
			}
            else
            {
				printf("Texture type: %s, path: %s\n", aiTextureTypeToString(static_cast<aiTextureType>(i)), path.C_Str());
            }
		}
	}
    */
    return true;
}

uint32 MaterialData::getDiffuseTexIdx() const
{
    return m_diffuseTexIdx;
}

uint32 MaterialData::getNormalTexIdx() const
{
    return m_normalTexIdx;
}

uint32 MaterialData::getOpacityTexIdx() const
{
    return m_opacityTexIdx;
}

uint32 MaterialData::getMetalRoughnessTexIdx() const
{
    return m_metalRoughnessTexIdx;
}

const char* MaterialData::getName() const
{
    static thread_local aiString name;
    m_pMaterial->Get(AI_MATKEY_NAME, name);
    return name.C_Str();
}

glm::vec3 MaterialData::getBaseColor() const
{
    aiColor4D color;
    aiReturn res = aiGetMaterialColor(m_pMaterial, AI_MATKEY_BASE_COLOR, &color);
    if (res == aiReturn_SUCCESS)
        return glm::vec3(color.r, color.g, color.b);
    else
    {
        aiReturn res2 = aiGetMaterialColor(m_pMaterial, AI_MATKEY_COLOR_DIFFUSE, &color);
        if (res2 == aiReturn_SUCCESS)
            return glm::vec3(color.r, color.g, color.b);
        else
            return glm::vec3(1);
    }
}

glm::vec3 MaterialData::getEmissiveColor() const
{
    aiColor4D color;
    aiReturn res = aiGetMaterialColor(m_pMaterial, AI_MATKEY_COLOR_EMISSIVE, &color);
    if (res == aiReturn_SUCCESS)
        return glm::vec3(color.r, color.g, color.b);
    else
        return glm::vec3(0);
}

glm::vec3 MaterialData::getSpecularColor() const
{
    aiColor4D color;
    aiReturn res = aiGetMaterialColor(m_pMaterial, AI_MATKEY_COLOR_SPECULAR, &color);
    if (res == aiReturn_SUCCESS)
        return glm::vec3(color.r, color.g, color.b);
    else
        return glm::vec3(1);
}

float MaterialData::getRoughnessFactor() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_ROUGHNESS_FACTOR, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 0.0f;
}

float MaterialData::getMetalnessFactor() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_METALLIC_FACTOR, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 0.0f;
}

float MaterialData::getOpacity() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_OPACITY, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 1.0f;
}

IMaterialData::EAlphaMode MaterialData::getAlphaMode() const
{
    // glTF stores the alpha mode explicitly as "OPAQUE" / "MASK" / "BLEND".
    aiString mode;
    aiReturn res = aiGetMaterialString(m_pMaterial, AI_MATKEY_GLTF_ALPHAMODE, &mode);
    if (res == aiReturn_SUCCESS)
    {
        if (strcmp(mode.C_Str(), "BLEND") == 0)
            return EAlphaMode::Blend;
        if (strcmp(mode.C_Str(), "MASK") == 0)
            return EAlphaMode::Mask;
        return EAlphaMode::Opaque;
    }
    // Non-glTF assets don't expose an alpha mode; fall back to inferring blend from opacity.
    return getOpacity() < 1.0f ? EAlphaMode::Blend : EAlphaMode::Opaque;
}

float MaterialData::getAlphaCutoff() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_GLTF_ALPHACUTOFF, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 0.5f; // glTF default
}

float MaterialData::getEmissiveIntensity() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_EMISSIVE_INTENSITY, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 1.0f;
}

float MaterialData::getRefractiveIndex() const
{
    float f;
    aiReturn res = aiGetMaterialFloat(m_pMaterial, AI_MATKEY_REFRACTI, &f);
    if (res == aiReturn_SUCCESS)
        return f;
    else
        return 0.04f;
}

std::string MaterialData::getTexturePath(ETextureType type) const
{
    const aiTextureType aiType = static_cast<aiTextureType>(type);
    assert(m_pMaterial->GetTextureCount(aiType) <= 1 && "Only one texture per type supported");

    aiString path;
    m_pMaterial->GetTexture(aiType, 0, &path);
    if (!path.length)
        return "";

    const bool isEmbedded = path.C_Str()[0] == '*';
	if (isEmbedded)
	{
		return path.C_Str();
	}
    else
        return std::filesystem::path(path.C_Str()).lexically_proximate(std::filesystem::current_path()).string();
}