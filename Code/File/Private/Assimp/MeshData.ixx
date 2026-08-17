export module File:MeshData;

import Core;
import Core.glm;
import Core.AABB;
import Animation;
import :IMeshData;

export struct aiMesh;

export class MeshData final : public IMeshData
{
public:
    MeshData();
    ~MeshData();
    MeshData(const MeshData&) = delete;
    MeshData(MeshData&&) = default;

    // pSkeleton (may be null) maps aiBone names to scene bone indices for the skinning influences.
    bool initialize(const aiMesh* pMesh, const Skeleton* pSkeleton);
    const glm::vec3* getVertices() const override;
    const glm::vec3* getNormals() const override;
    const glm::vec3* getTangents() const override;
    const glm::vec3* getBitangents() const override;
    const glm::vec3* getTexCoords() const override;
    const uint32* getIndices() const override;
    uint32 getNumVertices() const override;
    uint32 getNumIndices() const override;
    uint32 getMaterialIndex() const override;
    AABB getAABB() const override;
    const char* getName() const override { return m_pName; }

    bool isSkinned() const override { return !m_boneWeights.empty(); }
    const glm::uvec4* getBoneIndices() const override { return m_boneIndices.empty() ? nullptr : m_boneIndices.data(); }
    const glm::vec4* getBoneWeights() const override { return m_boneWeights.empty() ? nullptr : m_boneWeights.data(); }

private:

    void buildBoneInfluences(const Skeleton* pSkeleton);

    const char* m_pName = nullptr;
    const aiMesh* m_pMesh = nullptr;
    oc::vector<uint32> m_indices;
    oc::vector<glm::uvec4> m_boneIndices; // per-vertex, scene-skeleton bone indices (4 influences)
    oc::vector<glm::vec4> m_boneWeights;  // per-vertex, normalized influence weights (sum ~1)
};