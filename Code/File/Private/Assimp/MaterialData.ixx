export module File:MaterialData;

import Core;
import Core.glm;
import :IMaterialData;

export struct aiMaterial;
export enum aiTextureType : int;

export class MaterialData final : public IMaterialData
{
public:
    MaterialData();
    ~MaterialData();
    MaterialData(const MaterialData&) = delete;
    MaterialData(MaterialData&&) = default;

    bool initialize(const aiMaterial* pMaterial);

    const char* getName() const override;

    glm::vec3 getBaseColor() const override;
    glm::vec3 getEmissiveColor() const override;
    glm::vec3 getSpecularColor() const override;
    float getRoughnessFactor() const override;
    float getMetalnessFactor() const override;
    float getOpacity() const override;
    EAlphaMode getAlphaMode() const override;
    float getAlphaCutoff() const override;
    float getEmissiveIntensity() const override;
    float getRefractiveIndex() const override;

    oc::string getTexturePath(ETextureType type) const override;

    uint32 getDiffuseTexIdx() const override;
    uint32 getNormalTexIdx()  const override;
    uint32 getOpacityTexIdx() const override;
    uint32 getMetalRoughnessTexIdx() const override;

    // Called by SceneData after it resolves the unified texture registry.
    void setDiffuseTexIdx(uint32 idx)          { m_diffuseTexIdx          = idx; }
    void setNormalTexIdx(uint32 idx)           { m_normalTexIdx           = idx; }
    void setOpacityTexIdx(uint32 idx)          { m_opacityTexIdx          = idx; }
    void setMetalRoughnessTexIdx(uint32 idx)   { m_metalRoughnessTexIdx   = idx; }

private:

    const aiMaterial* m_pMaterial       = nullptr;
    uint32 m_diffuseTexIdx              = UINT32_MAX;
    uint32 m_normalTexIdx               = UINT32_MAX;
    uint32 m_opacityTexIdx              = UINT32_MAX;
    uint32 m_metalRoughnessTexIdx       = UINT32_MAX;
};