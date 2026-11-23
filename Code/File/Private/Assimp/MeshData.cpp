module;

#include "AssimpGmf.h"

module File;

import Core;
import Core.glm;
import Core.AABB;
import Animation;

import :MeshData;
import :Assimp;

using namespace Assimp;

MeshData::MeshData()
{
}

MeshData::~MeshData()
{
}

bool MeshData::initialize(const aiMesh* pMesh, const Skeleton* pSkeleton)
{
    m_pMesh = pMesh;
    m_pName = pMesh->mName.C_Str();

    m_indices.resize(pMesh->mNumFaces * 3);
    for (uint32 i = 0; i < pMesh->mNumFaces; i++)
    {
        memcpy(&m_indices[i * 3], pMesh->mFaces[i].mIndices, 3 * sizeof(uint32));
    }

    if (pMesh->mNumBones > 0 && pSkeleton != nullptr)
        buildBoneInfluences(pSkeleton);

    return true;
}

void MeshData::buildBoneInfluences(const Skeleton* pSkeleton)
{
    const uint32 numVerts = m_pMesh->mNumVertices;
    m_boneIndices.assign(numVerts, glm::uvec4(0));
    m_boneWeights.assign(numVerts, glm::vec4(0.0f));

    for (uint32 b = 0; b < m_pMesh->mNumBones; ++b)
    {
        const aiBone* pBone = m_pMesh->mBones[b];
        const int32 boneIdx = pSkeleton->findBone(pBone->mName.C_Str());
        if (boneIdx < 0)
            continue; // bone has no matching skeleton joint (shouldn't happen for valid rigs)

        for (uint32 w = 0; w < pBone->mNumWeights; ++w)
        {
            const aiVertexWeight& vw = pBone->mWeights[w];
            if (vw.mWeight <= 0.0f || vw.mVertexId >= numVerts)
                continue;

            // Keep the four strongest influences per vertex: fill an empty slot, else replace the
            // smallest weight if this one is larger.
            glm::uvec4& idx = m_boneIndices[vw.mVertexId];
            glm::vec4& wgt = m_boneWeights[vw.mVertexId];
            uint32 slot = 4;
            float smallest = vw.mWeight;
            for (uint32 s = 0; s < 4; ++s)
            {
                if (wgt[s] == 0.0f) { slot = s; break; }
                if (wgt[s] < smallest) { smallest = wgt[s]; slot = s; }
            }
            if (slot < 4)
            {
                idx[slot] = (uint32)boneIdx;
                wgt[slot] = vw.mWeight;
            }
        }
    }

    // Normalize so the four weights sum to 1 (LimitBoneWeights drops influences, leaving a deficit).
    for (uint32 v = 0; v < numVerts; ++v)
    {
        glm::vec4& wgt = m_boneWeights[v];
        const float sum = wgt.x + wgt.y + wgt.z + wgt.w;
        if (sum > 1e-6f)
            wgt /= sum;
        else
            wgt = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // unweighted vertex: bind to bone 0
    }
}

const glm::vec3* MeshData::getVertices() const
{
    return reinterpret_cast<glm::vec3*>(m_pMesh->mVertices);
}

const glm::vec3* MeshData::getNormals() const
{
    return reinterpret_cast<glm::vec3*>(m_pMesh->mNormals);
}

const glm::vec3* MeshData::getTangents() const
{
    return reinterpret_cast<glm::vec3*>(m_pMesh->mTangents);
}

const glm::vec3* MeshData::getBitangents() const
{
    return reinterpret_cast<glm::vec3*>(m_pMesh->mBitangents);
}

const glm::vec3* MeshData::getTexCoords() const
{
    return reinterpret_cast<glm::vec3*>(m_pMesh->mTextureCoords[0]);
}

uint32 MeshData::getNumVertices() const
{
    return m_pMesh->mNumVertices;
}

const uint32* MeshData::getIndices() const
{
    return m_indices.data();
}

uint32 MeshData::getNumIndices() const
{
    return (uint32)m_indices.size();
}

uint32 MeshData::getMaterialIndex() const
{
    return m_pMesh->mMaterialIndex;
}

AABB MeshData::getAABB() const
{
    AABB aabb;
    aabb.min = glm::vec3(m_pMesh->mAABB.mMin.x, m_pMesh->mAABB.mMin.y, m_pMesh->mAABB.mMin.z);
    aabb.max = glm::vec3(m_pMesh->mAABB.mMax.x, m_pMesh->mAABB.mMax.y, m_pMesh->mAABB.mMax.z);
    return aabb;
}