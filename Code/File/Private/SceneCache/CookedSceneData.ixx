export module File:CookedSceneData;

import File.fwd;

import Core;
import Core.glm;
import Core.AABB;

import :ISceneData;
import :IMeshData;
import :IMaterialData;
import :ITextureData;
import :INodeData;

// Cooked scene cache (.vsc): a binary snapshot of everything ObjectContainer / CollisionSource consume
// from an imported model, written to Assets/Local/Cooked on first import and re-read with a single
// fread afterwards (see SceneCooker.cpp). Raw little-endian structs, no versioning/compat: any layout
// change bumps SCENE_CACHE_VERSION and stale files silently re-cook. All offsets are absolute file
// offsets; strings are null-terminated in one blob. Skinned/animated scenes are never cooked.
export namespace SceneCache
{
    constexpr uint32 SCENE_CACHE_MAGIC   = 'V' | ('R' << 8) | ('S' << 16) | ('C' << 24);
    constexpr uint32 SCENE_CACHE_VERSION = 6; // v6: deterministic cache names (the name hash used to fold
                                              // in a raw pointer, changing every launch); bumping flushes
                                              // the junk those runs accumulated via the GC's version check
    constexpr uint32 MAX_COOKED_LOD_LEVELS = 7; // generated levels beyond LOD0 the format can hold

    struct CookedHeader
    {
        uint32 magic;
        uint32 version;
        uint64 sourceMTime;   // std::filesystem::last_write_time ticks of the source model
        uint64 sourceSize;
        uint64 optionsHash;   // import + cook options (mergeNodes/preTransform/LOD params/...)
        uint64 meshesOffset, materialsOffset, texturesOffset, nodesOffset, meshRefsOffset, texStampsOffset, stringsOffset;
        uint32 numMeshes, numMaterials, numTextures, numNodes, numMeshRefs, numTexStamps;
        uint32 stringsSize;
        uint32 sourcePathOffset; // the source model path (into strings) — lets the cook-time GC pass
                                 // identify caches whose source vanished or changed without a lookup table
    };
    static_assert(sizeof(CookedHeader) == 120);

    struct CookedMesh
    {
        uint64 positionsOffset, normalsOffset, tangentsOffset, bitangentsOffset, texCoordsOffset; // glm::vec3 arrays
        uint64 indicesOffset;     // uint32 array
        uint64 lodIndicesOffset;  // baked LOD chains: concatenated uint32 arrays in level order
        uint32 nameOffset;
        uint32 materialIdx;
        uint32 numVertices;
        uint32 numIndices;
        float aabbMin[3];
        float aabbMax[3];
        uint32 numLodLevels;
        uint32 lodIndexCounts[MAX_COOKED_LOD_LEVELS];
        float lodErrors[MAX_COOKED_LOD_LEVELS]; // per level: geometric deviation from LOD0 (mesh-local units)
        uint32 pad;
    };
    static_assert(sizeof(CookedMesh) == 160);

    struct CookedMaterial
    {
        float baseColor[3], emissiveColor[3], specularColor[3];
        float roughness, metalness, opacity, alphaCutoff, emissiveIntensity, refractiveIndex;
        uint32 nameOffset;
        uint32 alphaMode; // IMaterialData::EAlphaMode
        uint32 diffuseTexIdx, normalTexIdx, opacityTexIdx, metalRoughnessTexIdx;
    };
    static_assert(sizeof(CookedMaterial) == 84);

    struct CookedTexture
    {
        uint32 pathOffset; // converted textures point at the cooked .dds (Assets-root-relative)
    };

    struct CookedNode
    {
        float pos[3];
        float scale[3];
        float rot[4]; // quat w,x,y,z
        uint32 nameOffset;
        uint32 firstChild, numChildren; // children of a node are contiguous
        uint32 firstMeshRef, numMeshes; // range in the meshRefs uint32 array
        uint32 numChildrenRecursive;
    };
    static_assert(sizeof(CookedNode) == 64);

    // Loose source textures that were converted to .dds: the cache is stale if any of them changed.
    struct CookedTexStamp
    {
        uint64 mtime;
        uint64 size;
        uint32 sourcePathOffset;
        uint32 cookedPathOffset;
    };
    static_assert(sizeof(CookedTexStamp) == 24);
}

export class CookedSceneData;

export class CookedMeshData final : public IMeshData
{
public:
    const glm::vec3* getVertices() const override { return m_positions; }
    const glm::vec3* getNormals() const override { return m_normals; }
    const glm::vec3* getTangents() const override { return m_tangents; }
    const glm::vec3* getBitangents() const override { return m_bitangents; }
    const glm::vec3* getTexCoords() const override { return m_texCoords; }
    const uint32* getIndices() const override { return m_indices; }
    uint32 getNumVertices() const override { return m_pMesh->numVertices; }
    uint32 getNumIndices() const override { return m_pMesh->numIndices; }
    uint32 getMaterialIndex() const override { return m_pMesh->materialIdx; }
    AABB getAABB() const override;
    const char* getName() const override { return m_pName; }

    uint32 getNumLodLevels() const override { return m_pMesh->numLodLevels; }
    const uint32* getLodIndices(uint32 level, uint32& outNumIndices) const override;
    float getLodError(uint32 level) const override { return level < m_pMesh->numLodLevels ? m_pMesh->lodErrors[level] : 0.0f; }

private:
    friend class CookedSceneData;
    const SceneCache::CookedMesh* m_pMesh = nullptr;
    const char* m_pName = nullptr;
    const glm::vec3* m_positions = nullptr;
    const glm::vec3* m_normals = nullptr;
    const glm::vec3* m_tangents = nullptr;
    const glm::vec3* m_bitangents = nullptr;
    const glm::vec3* m_texCoords = nullptr;
    const uint32* m_indices = nullptr;
    const uint32* m_lodIndices = nullptr; // concatenated levels
};

export class CookedMaterialData final : public IMaterialData
{
public:
    const char* getName() const override { return m_pName; }
    glm::vec3 getBaseColor() const override { return glm::vec3(m_pMaterial->baseColor[0], m_pMaterial->baseColor[1], m_pMaterial->baseColor[2]); }
    glm::vec3 getEmissiveColor() const override { return glm::vec3(m_pMaterial->emissiveColor[0], m_pMaterial->emissiveColor[1], m_pMaterial->emissiveColor[2]); }
    glm::vec3 getSpecularColor() const override { return glm::vec3(m_pMaterial->specularColor[0], m_pMaterial->specularColor[1], m_pMaterial->specularColor[2]); }
    float getRoughnessFactor() const override { return m_pMaterial->roughness; }
    float getMetalnessFactor() const override { return m_pMaterial->metalness; }
    float getOpacity() const override { return m_pMaterial->opacity; }
    EAlphaMode getAlphaMode() const override { return (EAlphaMode)m_pMaterial->alphaMode; }
    float getAlphaCutoff() const override { return m_pMaterial->alphaCutoff; }
    float getEmissiveIntensity() const override { return m_pMaterial->emissiveIntensity; }
    float getRefractiveIndex() const override { return m_pMaterial->refractiveIndex; }
    oc::string getTexturePath(ETextureType) const override { return {}; } // unused outside importers

    uint32 getDiffuseTexIdx() const override { return m_pMaterial->diffuseTexIdx; }
    uint32 getNormalTexIdx() const override { return m_pMaterial->normalTexIdx; }
    uint32 getOpacityTexIdx() const override { return m_pMaterial->opacityTexIdx; }
    uint32 getMetalRoughnessTexIdx() const override { return m_pMaterial->metalRoughnessTexIdx; }

private:
    friend class CookedSceneData;
    const SceneCache::CookedMaterial* m_pMaterial = nullptr;
    const char* m_pName = nullptr;
};

// Always a file reference (embedded sources were converted to .dds at cook time); the consumer loads
// from getFileName() exactly like a loose-file TextureData.
export class CookedTextureData final : public ITextureData
{
public:
    const char* getFileName() const override { return m_pPath; }
    const Pixel* getPixels() const override { return nullptr; }
    uint32 getWidth() const override { return 0; }
    uint32 getHeight() const override { return 0; }
    const char* getFormatInfo() const override { return ""; }

private:
    friend class CookedSceneData;
    const char* m_pPath = nullptr;
};

export class CookedNodeData final : public INodeData
{
public:
    CookedNodeData() = default;
    CookedNodeData(const CookedSceneData* pScene, uint32 nodeIdx) : m_pScene(pScene), m_nodeIdx(nodeIdx) {}

    operator bool() const override { return m_pScene != nullptr; }
    bool operator==(const INodeData& other) const override { return getName() == other.getName(); }
    bool isValid() const override { return m_pScene != nullptr; }

    oc::unique_ptr<INodeData> clone() const override { return oc::make_unique<CookedNodeData>(m_pScene, m_nodeIdx); }

    const char* getName() const override;
    uint32 getNumChildren() const override;
    oc::unique_ptr<INodeData> getChild(uint32 idx) const override;
    uint32 getNumChildrenRecursive() const override;
    oc::vector<oc::string> getChildrenNames() const override;
    uint32 getNumMeshes() const override;
    uint32 getMeshIndex(uint32 meshIdx) const override;
    void getTransform(glm::vec3& pos, glm::vec3& scale, glm::quat& rot) const override;
    glm::vec3 getPosition() const override;
    glm::vec3 getScale() const override;
    glm::quat getRotation() const override;

private:
    const SceneCache::CookedNode& node() const;

    const CookedSceneData* m_pScene = nullptr;
    uint32 m_nodeIdx = 0;
};

export class CookedSceneData final : public ISceneData
{
public:
    CookedSceneData() = default;
    CookedSceneData(const CookedSceneData&) = delete;
    CookedSceneData(CookedSceneData&&) = delete;

    // Reads and validates a cooked cache file: magic/version, source stamp, options hash, and every
    // converted texture's source stamp + cooked .dds presence. Any mismatch returns false (recook).
    bool load(const oc::string& cachePath, const oc::string& sourcePath, uint64 sourceMTime, uint64 sourceSize, uint64 optionsHash);

    bool initialize(const char*, bool, bool) override { assert(false && "CookedSceneData loads via load()"); return false; }
    bool isValid() const override { return !m_blob.empty(); }

    const oc::string& getFilePath() const override { return m_filePath; }
    const INodeData& getRootNode() const override { return m_rootNode; }

    uint32 getNumMeshes() const override { return (uint32)m_meshes.size(); }
    uint32 getNumMaterials() const override { return (uint32)m_materials.size(); }
    uint32 getNumTextures() const override { return (uint32)m_textures.size(); }

    const IMeshData* getMesh(const char* pMeshName) const override;
    const IMeshData* getMesh(uint32 idx) const override { assert(idx < m_meshes.size()); return &m_meshes[idx]; }
    bool getMeshStreamSource(uint32 meshIdx, MeshStreamSource& out) const override;
    const IMaterialData* getMaterial(uint32 idx) const override { assert(idx < m_materials.size()); return &m_materials[idx]; }
    const ITextureData* getTexture(uint32 idx) const override { assert(idx < m_textures.size()); return &m_textures[idx]; }

private:
    friend class CookedNodeData;

    const char* string(uint32 offset) const;

    oc::string m_filePath;  // the ORIGINAL source model path (consumers resolve loose files against it)
    oc::string m_cachePath; // the .vsc this data came from (handed out as the mesh re-stream source)
    oc::vector<uint8> m_blob;
    const SceneCache::CookedNode* m_nodes = nullptr;
    const uint32* m_meshRefs = nullptr;
    uint32 m_numNodes = 0;
    oc::vector<CookedMeshData> m_meshes;
    oc::vector<CookedMaterialData> m_materials;
    oc::vector<CookedTextureData> m_textures;
    CookedNodeData m_rootNode;
};
