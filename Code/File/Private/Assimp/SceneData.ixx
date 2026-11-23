module;

#include "AssimpGmf.h"

export module File:SceneData;

import Core;
import Animation;

import File.fwd;
import :ISceneData;
import :Assimp;
import :MeshData;
import :TextureData;
import :MaterialData;
import :NodeData;

export oc::unique_ptr<ISceneData> createAssimpLoader();

// Module-internal (the scene cache uses them too):
// Adds a clip to a set under a unique name (suffixes _N on collision) - the one naming rule for every clip set.
void addClipToSet(AnimationSet& set, AnimationClip&& clip, const oc::string& desiredName);
// EVERY animation track of a file through Assimp, unfiltered (ISceneData::importClipAssets caches and filters it). False = unreadable.
bool importAllClipAssets(const char* filePath, oc::vector<ImportedClip>& out, oc::string* error);

export class SceneData final : public ISceneData
{
public:
	SceneData();
	~SceneData();
	SceneData(const SceneData&) = delete;
	SceneData(SceneData&&) = default;

	bool initialize(const char* filePath, bool mergeNodes, bool preTransformVertices) override;
	bool isValid() const override { return m_pScene != nullptr; }

	const oc::string& getFilePath() const override { return m_filePath; }
	const INodeData& getRootNode() const override { return m_rootNode; }

	const IMeshData* getMesh(const char* pMeshName) const override;

	virtual uint32 getNumMeshes() const override { return static_cast<uint32>(m_meshes.size()); }
	virtual uint32 getNumMaterials() const override { return static_cast<uint32>(m_materials.size()); }
	virtual uint32 getNumTextures() const override { return static_cast<uint32>(m_textures.size()); }

	virtual const IMeshData* getMesh(uint32 idx) const override { assert(idx < m_meshes.size()); return &m_meshes[idx]; }
	virtual const IMaterialData* getMaterial(uint32 idx) const override { assert(idx < m_materials.size()); return &m_materials[idx]; }
	virtual const ITextureData* getTexture(uint32 idx) const override { assert(idx < m_textures.size()); return &m_textures[idx]; }

	const Skeleton* getSkeleton() const override { return m_skeleton.isValid() ? &m_skeleton : nullptr; }
	const AnimationSet* getAnimations() const override { return m_animationSet.numClips() > 0 ? &m_animationSet : nullptr; }
	uint32 getNumAnimations() const override { return m_animationSet.numClips(); }
	const AnimationClip* getAnimation(uint32 idx) const override { return m_animationSet.get(idx); }
	oc::span<const ImportedClip> getClipAssets() const override { return m_clipAssets; }

private:

	void buildSkeleton();
	void addBoneRecursive(const aiNode* pNode, int32 parentIdx);
	void buildAnimations();

	oc::string m_filePath;
	Assimp::Importer m_importer;
	const aiScene* m_pScene = nullptr;
	oc::vector<MeshData> m_meshes;
	oc::vector<TextureData> m_textures;
	oc::vector<MaterialData> m_materials;
	NodeData m_rootNode;
	Skeleton m_skeleton;
	oc::vector<ImportedClip> m_clipAssets; // the file's animations by bone name (what the scene cache stores)
	AnimationSet m_animationSet;           // ... resolved against m_skeleton
};