module;

#include <cstdio> // fopen_s / fread for the single-blob cache read (macros don't cross header units)

module File;

// File IS the filesystem library: its readers stream binary ranges directly (Core no longer
// exports these). Entry points call FileSystem::assertIoThread so the main-thread policy still holds.
import <filesystem>;
import <fstream>;

import Core;
import Core.glm;
import Core.AABB;
import Core.Log;

import :CookedSceneData;

using namespace SceneCache;

namespace
{
    uint64 fileMTimeTicks(const std::filesystem::path& path, std::error_code& ec)
    {
        return (uint64)std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    }
}

AABB CookedMeshData::getAABB() const
{
    AABB aabb;
    aabb.min = glm::vec3(m_pMesh->aabbMin[0], m_pMesh->aabbMin[1], m_pMesh->aabbMin[2]);
    aabb.max = glm::vec3(m_pMesh->aabbMax[0], m_pMesh->aabbMax[1], m_pMesh->aabbMax[2]);
    return aabb;
}

const uint32* CookedMeshData::getLodIndices(uint32 level, uint32& outNumIndices) const
{
    if (level >= m_pMesh->numLodLevels)
    {
        outNumIndices = 0;
        return nullptr;
    }
    uint64 offset = 0;
    for (uint32 i = 0; i < level; ++i)
        offset += m_pMesh->lodIndexCounts[i];
    outNumIndices = m_pMesh->lodIndexCounts[level];
    return m_lodIndices + offset;
}

const SceneCache::CookedNode& CookedNodeData::node() const
{
    return m_pScene->m_nodes[m_nodeIdx];
}

const char* CookedNodeData::getName() const
{
    return m_pScene->string(node().nameOffset);
}

uint32 CookedNodeData::getNumChildren() const
{
    return node().numChildren;
}

std::unique_ptr<INodeData> CookedNodeData::getChild(uint32 idx) const
{
    assert(idx < node().numChildren);
    return std::make_unique<CookedNodeData>(m_pScene, node().firstChild + idx);
}

uint32 CookedNodeData::getNumChildrenRecursive() const
{
    return node().numChildrenRecursive;
}

std::vector<std::string> CookedNodeData::getChildrenNames() const
{
    const CookedNode& n = node();
    std::vector<std::string> names;
    names.reserve(n.numChildren);
    for (uint32 i = 0; i < n.numChildren; ++i)
        names.emplace_back(m_pScene->string(m_pScene->m_nodes[n.firstChild + i].nameOffset));
    return names;
}

uint32 CookedNodeData::getNumMeshes() const
{
    return node().numMeshes;
}

uint32 CookedNodeData::getMeshIndex(uint32 meshIdx) const
{
    assert(meshIdx < node().numMeshes);
    return m_pScene->m_meshRefs[node().firstMeshRef + meshIdx];
}

void CookedNodeData::getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const
{
    const CookedNode& n = node();
    pos = glm::vec3(n.pos[0], n.pos[1], n.pos[2]);
    scale = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);
    rot = glm::quat(n.rot[0], n.rot[1], n.rot[2], n.rot[3]);
}

glm::vec3 CookedNodeData::getPosition() const { const CookedNode& n = node(); return glm::vec3(n.pos[0], n.pos[1], n.pos[2]); }
glm::vec3 CookedNodeData::getScale() const { const CookedNode& n = node(); return glm::vec3(n.scale[0], n.scale[1], n.scale[2]); }
glm::quat CookedNodeData::getRotation() const { const CookedNode& n = node(); return glm::quat(n.rot[0], n.rot[1], n.rot[2], n.rot[3]); }

const char* CookedSceneData::string(uint32 offset) const
{
    const CookedHeader& header = *reinterpret_cast<const CookedHeader*>(m_blob.data());
    assert(offset < header.stringsSize);
    return reinterpret_cast<const char*>(m_blob.data() + header.stringsOffset + offset);
}

bool CookedSceneData::getMeshStreamSource(uint32 meshIdx, MeshStreamSource& out) const
{
    if (meshIdx >= m_meshes.size())
        return false;
    const CookedMesh& mesh = *m_meshes[meshIdx].m_pMesh;
    out.filePath = m_cachePath.c_str();
    out.positionsOffset = mesh.positionsOffset;
    out.normalsOffset = mesh.normalsOffset;
    out.tangentsOffset = mesh.tangentsOffset;
    out.bitangentsOffset = mesh.bitangentsOffset;
    out.texCoordsOffset = mesh.texCoordsOffset;
    out.indicesOffset = mesh.indicesOffset;
    out.lodIndicesOffset = mesh.lodIndicesOffset;
    out.numVertices = mesh.numVertices;
    out.numIndices = mesh.numIndices;
    return true;
}

const IMeshData* CookedSceneData::getMesh(const char* pMeshName) const
{
    for (const CookedMeshData& mesh : m_meshes)
        if (std::string_view(mesh.getName()) == pMeshName)
            return &mesh;
    return nullptr;
}

bool CookedSceneData::load(const std::string& cachePath, const std::string& sourcePath, uint64 sourceMTime, uint64 sourceSize, uint64 optionsHash)
{
    // Cooked scenes load at SPAWN (main thread, cached afterwards) and on the terrain/mesh-stream
    // jobs — both intended, so the main-thread policy is satisfied explicitly here.
    FileSystem::assertIoThread(/*allowMainThread*/ true);
    FILE* pFile = nullptr;
    fopen_s(&pFile, cachePath.c_str(), "rb");
    if (!pFile)
        return false;
    std::unique_ptr<FILE, void(*)(FILE*)> file(pFile, [](FILE* p) { fclose(p); });

    std::error_code ec;
    const uint64 fileSize = std::filesystem::file_size(cachePath, ec);
    if (ec || fileSize < sizeof(CookedHeader))
        return false;
    m_blob.resize(fileSize);
    if (fread(m_blob.data(), 1, fileSize, file.get()) != fileSize)
    {
        m_blob.clear();
        return false;
    }
    file.reset();

    const CookedHeader& header = *reinterpret_cast<const CookedHeader*>(m_blob.data());
    auto sectionValid = [&](uint64 offset, uint64 count, uint64 elemSize)
    {
        return offset <= fileSize && count * elemSize <= fileSize - offset;
    };
    if (header.magic != SCENE_CACHE_MAGIC || header.version != SCENE_CACHE_VERSION
        || header.sourceMTime != sourceMTime || header.sourceSize != sourceSize || header.optionsHash != optionsHash
        || !sectionValid(header.meshesOffset, header.numMeshes, sizeof(CookedMesh))
        || !sectionValid(header.materialsOffset, header.numMaterials, sizeof(CookedMaterial))
        || !sectionValid(header.texturesOffset, header.numTextures, sizeof(CookedTexture))
        || !sectionValid(header.nodesOffset, header.numNodes, sizeof(CookedNode))
        || !sectionValid(header.meshRefsOffset, header.numMeshRefs, sizeof(uint32))
        || !sectionValid(header.texStampsOffset, header.numTexStamps, sizeof(CookedTexStamp))
        || !sectionValid(header.stringsOffset, header.stringsSize, 1)
        || header.numNodes == 0
        || header.stringsSize == 0 || m_blob[header.stringsOffset + header.stringsSize - 1] != 0)
    {
        m_blob.clear();
        return false;
    }

    // Converted source textures must be unchanged and their cooked .dds files still present.
    const CookedTexStamp* stamps = reinterpret_cast<const CookedTexStamp*>(m_blob.data() + header.texStampsOffset);
    for (uint32 i = 0; i < header.numTexStamps; ++i)
    {
        const char* sourceTexPath = reinterpret_cast<const char*>(m_blob.data() + header.stringsOffset + stamps[i].sourcePathOffset);
        const char* cookedTexPath = reinterpret_cast<const char*>(m_blob.data() + header.stringsOffset + stamps[i].cookedPathOffset);
        const uint64 texSize = std::filesystem::file_size(sourceTexPath, ec);
        if (ec || texSize != stamps[i].size || fileMTimeTicks(sourceTexPath, ec) != stamps[i].mtime || ec
            || !std::filesystem::exists(cookedTexPath, ec))
        {
            m_blob.clear();
            return false;
        }
    }

    m_filePath = sourcePath;
    m_cachePath = cachePath;
    m_nodes = reinterpret_cast<const CookedNode*>(m_blob.data() + header.nodesOffset);
    m_numNodes = header.numNodes;
    m_meshRefs = reinterpret_cast<const uint32*>(m_blob.data() + header.meshRefsOffset);
    m_rootNode = CookedNodeData(this, 0);

    const CookedMesh* meshes = reinterpret_cast<const CookedMesh*>(m_blob.data() + header.meshesOffset);
    m_meshes.resize(header.numMeshes);
    for (uint32 i = 0; i < header.numMeshes; ++i)
    {
        CookedMeshData& mesh = m_meshes[i];
        mesh.m_pMesh = &meshes[i];
        mesh.m_pName = string(meshes[i].nameOffset);
        mesh.m_positions = reinterpret_cast<const glm::vec3*>(m_blob.data() + meshes[i].positionsOffset);
        mesh.m_normals = reinterpret_cast<const glm::vec3*>(m_blob.data() + meshes[i].normalsOffset);
        mesh.m_tangents = reinterpret_cast<const glm::vec3*>(m_blob.data() + meshes[i].tangentsOffset);
        mesh.m_bitangents = reinterpret_cast<const glm::vec3*>(m_blob.data() + meshes[i].bitangentsOffset);
        mesh.m_texCoords = reinterpret_cast<const glm::vec3*>(m_blob.data() + meshes[i].texCoordsOffset);
        mesh.m_indices = reinterpret_cast<const uint32*>(m_blob.data() + meshes[i].indicesOffset);
        mesh.m_lodIndices = reinterpret_cast<const uint32*>(m_blob.data() + meshes[i].lodIndicesOffset);
    }

    const CookedMaterial* materials = reinterpret_cast<const CookedMaterial*>(m_blob.data() + header.materialsOffset);
    m_materials.resize(header.numMaterials);
    for (uint32 i = 0; i < header.numMaterials; ++i)
    {
        m_materials[i].m_pMaterial = &materials[i];
        m_materials[i].m_pName = string(materials[i].nameOffset);
    }

    const CookedTexture* textures = reinterpret_cast<const CookedTexture*>(m_blob.data() + header.texturesOffset);
    m_textures.resize(header.numTextures);
    for (uint32 i = 0; i < header.numTextures; ++i)
        m_textures[i].m_pPath = string(textures[i].pathOffset);

    return true;
}
