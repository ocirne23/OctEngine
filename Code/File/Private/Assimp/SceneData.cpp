module;

#include <unordered_map>

module File;

// File IS the filesystem library: its readers stream binary ranges directly (Core no longer
// exports these). Entry points call FileSystem::assertIoThread so the main-thread policy still holds.
import <filesystem>;
import <fstream>;

import Core;
import Core.glm;
import Animation;

import File.fwd;
import :SceneData;
import :Assimp;
import :MeshData;
import :TextureData;
import :MaterialData;
import :NodeData;

using namespace Assimp;

namespace
{
    // aiMatrix4x4 is row-major; glm::mat4 is column-major. Transpose the element layout on conversion.
    glm::mat4 toGlm(const aiMatrix4x4& m)
    {
        return glm::mat4(
            m.a1, m.b1, m.c1, m.d1,
            m.a2, m.b2, m.c2, m.d2,
            m.a3, m.b3, m.c3, m.d3,
            m.a4, m.b4, m.c4, m.d4);
    }

    // Builds one AnimationClip from an Assimp animation, resolving each channel against the given skeleton
    // BY BONE NAME. Name-based resolution is what lets animations come from a different file than the rig:
    // the clip's node ordering need not match the skeleton's, only the bone names.
    AnimationClip buildClip(const aiAnimation* pAnim, const Skeleton& skel)
    {
        const double ticksPerSecond = pAnim->mTicksPerSecond != 0.0 ? pAnim->mTicksPerSecond : 25.0;

        AnimationClip clip;
        clip.name = pAnim->mName.C_Str();
        clip.duration = (float)(pAnim->mDuration / ticksPerSecond);
        clip.channels.reserve(pAnim->mNumChannels);

        for (uint32 c = 0; c < pAnim->mNumChannels; ++c)
        {
            const aiNodeAnim* pCh = pAnim->mChannels[c];
            AnimationChannel& channel = clip.channels.emplace_back();
            channel.boneIndex = skel.findBone(pCh->mNodeName.C_Str());

            channel.positionKeys.reserve(pCh->mNumPositionKeys);
            for (uint32 k = 0; k < pCh->mNumPositionKeys; ++k)
            {
                const aiVectorKey& key = pCh->mPositionKeys[k];
                channel.positionKeys.push_back({ (float)(key.mTime / ticksPerSecond), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z) });
            }
            channel.rotationKeys.reserve(pCh->mNumRotationKeys);
            for (uint32 k = 0; k < pCh->mNumRotationKeys; ++k)
            {
                const aiQuatKey& key = pCh->mRotationKeys[k];
                channel.rotationKeys.push_back({ (float)(key.mTime / ticksPerSecond), glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z) });
            }
            channel.scaleKeys.reserve(pCh->mNumScalingKeys);
            for (uint32 k = 0; k < pCh->mNumScalingKeys; ++k)
            {
                const aiVectorKey& key = pCh->mScalingKeys[k];
                channel.scaleKeys.push_back({ (float)(key.mTime / ticksPerSecond), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z) });
            }
        }
        return clip;
    }

    // Overwrites each animated node's local transform with the value at the start of its animation track.
    // FBX commonly leaves an animated node's mTransformation at a default (often the origin) and stores its
    // real placement in the animation channels — so an unskinned mesh on such a node spawns in the wrong
    // place. Baking frame 0 into mTransformation fixes static (unskinned) placement. Each FBX pivot
    // sub-node ($AssimpFbx$_Translation/Rotation/Scaling) is its own channel, so baking per node and letting
    // the graph walk multiply them reconstructs the correct composed pose. Skinned bones are unaffected:
    // this runs after buildSkeleton() (which already captured the bind pose) and skinned meshes are driven
    // by the animation palette, not by mTransformation.
    void bakeFirstFrameRecursive(aiNode* pNode, const oc::unordered_map<oc::string, const aiNodeAnim*>& channels)
    {
        if (auto it = channels.find(pNode->mName.C_Str()); it != channels.end())
        {
            const aiNodeAnim* pCh = it->second;
            aiVector3D translation(0.0f, 0.0f, 0.0f);
            aiVector3D scaling(1.0f, 1.0f, 1.0f);
            aiQuaternion rotation; // identity
            if (pCh->mNumPositionKeys > 0) translation = pCh->mPositionKeys[0].mValue;
            if (pCh->mNumScalingKeys > 0)  scaling     = pCh->mScalingKeys[0].mValue;
            if (pCh->mNumRotationKeys > 0) rotation    = pCh->mRotationKeys[0].mValue;
            pNode->mTransformation = aiMatrix4x4(scaling, rotation, translation);
        }
        for (uint32 c = 0; c < pNode->mNumChildren; ++c)
            bakeFirstFrameRecursive(pNode->mChildren[c], channels);
    }

    // Adds a clip to a set under a unique name (suffixes _N on collision).
    void addClipToSet(AnimationSet& set, AnimationClip&& clip, const oc::string& desiredName)
    {
        oc::string name = !desiredName.empty() ? desiredName : (clip.name.empty() ? "anim" : clip.name);
        if (set.nameToIndex.count(name))
        {
            uint32 n = 1;
            oc::string unique;
            do { unique = name + "_" + oc::to_string(n++); } while (set.nameToIndex.count(unique));
            name = unique;
        }
        clip.name = name;
        set.nameToIndex[name] = (uint32)set.clips.size();
        set.clips.push_back(oc::move(clip));
    }
}

SceneData::SceneData()
{
}

SceneData::~SceneData()
{
}

bool SceneData::initialize(const char* filePath, bool mergeNodes, bool preTransformVertices)
{
    uint32 optimizationFlags = aiProcess_ImproveCacheLocality | aiProcess_RemoveRedundantMaterials | aiProcess_SortByPType;
    if (mergeNodes)
    {
        optimizationFlags |= aiProcess_FindInstances;
        optimizationFlags |= aiProcess_OptimizeMeshes;
        if (!preTransformVertices)
            optimizationFlags |= aiProcess_OptimizeGraph;
    }
    if (preTransformVertices)
        optimizationFlags |= aiProcess_PreTransformVertices;
    else
        // PreTransformVertices strips bones, so only request skinning data when we keep the node graph.
        // LimitBoneWeights caps influences to 4/vertex; PopulateArmatureData fills aiBone::mNode/mArmature.
        optimizationFlags |= aiProcess_LimitBoneWeights | aiProcess_PopulateArmatureData;
    //aiProcess_MakeLeftHanded
    // JoinIdenticalVertices is load-bearing: FBX exports commonly arrive as unindexed triangle soup
    // (every triangle carries 3 unique vertices — Bistro is ~3.5x duplicated). Welding restores real
    // connectivity, without which meshopt_simplify treats every vertex as complex and LOD generation
    // stalls at level 0 — and it cuts vertex memory by the same factor.
    m_pScene = m_importer.ReadFile(filePath, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_FlipUVs | aiProcess_GenBoundingBoxes | aiProcess_CalcTangentSpace | optimizationFlags);
    if (!m_pScene)
    {
        assert(false && "Failed to load scene");
        return false;
    }
    m_filePath = filePath;

    // Build the skeleton before meshes: per-vertex bone influences are remapped to skeleton bone indices.
    buildSkeleton();

    for (uint32 i = 0; i < m_pScene->mNumMeshes; i++)
    {
        MeshData& mesh = m_meshes.emplace_back();
        [[maybe_unused]] bool res = mesh.initialize(m_pScene->mMeshes[i], m_skeleton.isValid() ? &m_skeleton : nullptr);
        assert(res && "Failed to initialize MeshData");
    }

    // --- Unified texture registry ---
    // Embedded textures occupy indices [0, mNumTextures) and are registered first
    // so that material paths of the form "*N" map directly to index N.
    for (uint32 i = 0; i < m_pScene->mNumTextures; i++)
    {
        TextureData& texture = m_textures.emplace_back();
        [[maybe_unused]] bool res = texture.initialize(m_pScene->mTextures[i]);
        assert(res && "Failed to initialize TextureData");
    }

    // Build a path -> index map seeded with embedded textures.
    // Embedded texture paths use Assimp's "*N" convention.
    oc::unordered_map<oc::string, uint32> texRegistry;
    for (uint32 i = 0; i < (uint32)m_textures.size(); ++i)
        texRegistry[oc::string("*") + oc::to_string(i)] = i;

    // Resolves one texture slot for a material.
    // Embedded textures are looked up in the registry by "*N" key.
    // Loose file textures are added to m_textures on first encounter and
    // de-duplicated by their raw path string.
    auto resolveTexIdx = [&](const aiMaterial* pMat, aiTextureType type) -> uint32
    {
        aiString path;
        if (pMat->GetTexture(type, 0, &path) != aiReturn_SUCCESS || path.length == 0)
            return UINT32_MAX;

        const oc::string pathStr = path.C_Str();
        auto it = texRegistry.find(pathStr);
        if (it != texRegistry.end())
            return it->second;

        if (pathStr[0] == '*')
        {
            // Embedded reference that wasn't pre-registered – shouldn't happen.
            assert(false && "Embedded texture index out of range");
            return UINT32_MAX;
        }

        // Loose file texture not yet seen: add it to the unified list.
        const uint32 newIdx = (uint32)m_textures.size();
        TextureData& tex = m_textures.emplace_back();
        [[maybe_unused]] bool res = tex.initialize(pathStr.c_str());
        assert(res && "Failed to initialize loose TextureData");
        texRegistry[pathStr] = newIdx;
        return newIdx;
    };

    for (uint32 i = 0; i < m_pScene->mNumMaterials; i++)
    {
        MaterialData& material = m_materials.emplace_back();
        [[maybe_unused]] bool res = material.initialize(m_pScene->mMaterials[i]);
        assert(res && "Failed to initialize MaterialData");

        const aiMaterial* pMat = m_pScene->mMaterials[i];
        material.setDiffuseTexIdx(resolveTexIdx(pMat, aiTextureType_DIFFUSE));
        material.setNormalTexIdx(resolveTexIdx(pMat, aiTextureType_NORMALS));
        material.setOpacityTexIdx(resolveTexIdx(pMat, aiTextureType_OPACITY));
        // glTF combined metallic-roughness texture (roughness=G, metalness=B)
        material.setMetalRoughnessTexIdx(resolveTexIdx(pMat, aiTextureType_UNKNOWN));
    }

    // Place animated-but-unskinned nodes at their first animation frame (see bakeFirstFrameRecursive).
    // Must run after buildSkeleton() so skinned bind poses stay intact, and before the node graph is read.
    if (m_pScene->mNumAnimations > 0)
    {
        oc::unordered_map<oc::string, const aiNodeAnim*> firstFrameChannels;
        for (uint32 a = 0; a < m_pScene->mNumAnimations; ++a)
        {
            const aiAnimation* pAnim = m_pScene->mAnimations[a];
            for (uint32 c = 0; c < pAnim->mNumChannels; ++c)
                firstFrameChannels.try_emplace(pAnim->mChannels[c]->mNodeName.C_Str(), pAnim->mChannels[c]);
        }
        bakeFirstFrameRecursive(m_pScene->mRootNode, firstFrameChannels);
    }

    m_rootNode.initialize(m_pScene->mRootNode);

    buildAnimations();

    return m_pScene != nullptr;
}

void SceneData::buildSkeleton()
{
    // Only build a skeleton if any mesh is actually skinned.
    bool hasBones = false;
    for (uint32 i = 0; i < m_pScene->mNumMeshes; ++i)
    {
        if (m_pScene->mMeshes[i]->mNumBones > 0)
        {
            hasBones = true;
            break;
        }
    }
    if (!hasBones)
        return;

    // Flatten the whole node hierarchy parent-before-child (every node becomes a bone, so intermediate
    // non-skinning nodes keep the transform chain intact).
    addBoneRecursive(m_pScene->mRootNode, -1);

    // Fill inverse-bind (offset) matrices from each mesh's bones, matched by name to the flattened bones.
    for (uint32 m = 0; m < m_pScene->mNumMeshes; ++m)
    {
        const aiMesh* pMesh = m_pScene->mMeshes[m];
        for (uint32 b = 0; b < pMesh->mNumBones; ++b)
        {
            const aiBone* pBone = pMesh->mBones[b];
            const int32 idx = m_skeleton.findBone(pBone->mName.C_Str());
            if (idx >= 0)
                m_skeleton.inverseBind[idx] = toGlm(pBone->mOffsetMatrix);
        }
    }
}

void SceneData::addBoneRecursive(const aiNode* pNode, int32 parentIdx)
{
    const int32 idx = (int32)m_skeleton.boneNames.size();
    m_skeleton.boneNames.push_back(pNode->mName.C_Str());
    m_skeleton.parentIndices.push_back(parentIdx);
    m_skeleton.localBind.push_back(toGlm(pNode->mTransformation));
    m_skeleton.inverseBind.push_back(glm::mat4(1.0f));
    m_skeleton.nameToIndex[pNode->mName.C_Str()] = (uint32)idx;

    for (uint32 c = 0; c < pNode->mNumChildren; ++c)
        addBoneRecursive(pNode->mChildren[c], idx);
}

void SceneData::buildAnimations()
{
    if (!m_skeleton.isValid())
        return;

    for (uint32 a = 0; a < m_pScene->mNumAnimations; ++a)
        addClipToSet(m_animationSet, buildClip(m_pScene->mAnimations[a], m_skeleton), "");
}

bool ISceneData::loadAnimations(const char* filePath, const Skeleton& targetSkeleton, AnimationSet& outSet, const char* skipName, const char* clipNameOverride, const char* trackName)
{
    // No graph optimization: node names must survive so channels resolve against targetSkeleton by name.
    Assimp::Importer importer;
    const aiScene* pScene = importer.ReadFile(filePath, 0);
    if (!pScene || pScene->mNumAnimations == 0)
    {
        assert(false && "loadAnimations: file has no animations (or failed to load)");
        return false;
    }

    // A track is kept if it isn't skipped (name contains skipName, e.g. a "TPose") and, when trackName is
    // given, its name contains trackName (selecting one track from a multi-track file).
    const bool hasSkip = skipName && *skipName;
    const bool hasTrack = trackName && *trackName;
    auto included = [&](const aiAnimation* pAnim) {
        const oc::string name = pAnim->mName.C_Str();
        if (hasSkip && name.find(skipName) != oc::string::npos)
            return false;
        if (hasTrack && name.find(trackName) == oc::string::npos)
            return false;
        return true;
    };

    uint32 keptCount = 0;
    for (uint32 a = 0; a < pScene->mNumAnimations; ++a)
        if (included(pScene->mAnimations[a]))
            ++keptCount;

    const oc::string stem = oc::fromStd(std::filesystem::path(oc::toStd(filePath)).stem().string());
    const oc::string baseName = (clipNameOverride && *clipNameOverride) ? clipNameOverride : stem;
    const bool multiple = keptCount > 1;
    uint32 added = 0;
    for (uint32 a = 0; a < pScene->mNumAnimations; ++a)
    {
        const aiAnimation* pAnim = pScene->mAnimations[a];
        if (!included(pAnim))
            continue;
        // One clip per file is the common case (Mixamo etc.); suffix only when a file keeps several.
        const oc::string name = multiple ? baseName + "_" + oc::to_string(added) : baseName;
        addClipToSet(outSet, buildClip(pAnim, targetSkeleton), name);
        ++added;
    }
    return added > 0;
}

const IMeshData* SceneData::getMesh(const char* pMeshName) const
{
    for (const MeshData& mesh : m_meshes)
    {
        if (oc::string(mesh.getName()) == pMeshName)
        {
            return &mesh;
        }
    }
    return nullptr;
}