export module File:ISceneData;

import File.fwd;

import Core;
import Core.glm;
import Animation;

// Caller-supplied geometry for ISceneData::createMeshScene (e.g. runtime-generated terrain chunks). Arrays
// are numVertices long; tangents/bitangents/texCoords may be null (defaulted). Pointers are borrowed only
// for the duration of the call (the scene copies them).
export struct MeshGeometryDesc
{
	const glm::vec3* positions = nullptr;
	const glm::vec3* normals = nullptr;
	const glm::vec3* tangents = nullptr;
	const glm::vec3* bitangents = nullptr;
	const glm::vec3* texCoords = nullptr;
	uint32 numVertices = 0;
	const uint32* indices = nullptr;
	uint32 numIndices = 0;
	const char* name = "Mesh";
};

// Cook options for the scene cache (see loadCached / SceneCooker.cpp). All fields participate in the
// cache's options hash, so changing any of them re-cooks affected scenes on the next load.
export struct SceneCookOptions
{
	bool enableCache = true;      // false = always import directly (no read, no write)
	bool convertTextures = true;  // convert non-DDS model textures (loose + embedded) to BC .dds for mip streaming
	bool generateLods = true;     // bake meshopt LOD chains for static meshes without authored LodN_ chains
	int  lodLevels = 4;           // max generated levels beyond LOD0
	float lodReduction = 0.25f;   // index-count factor per generated level
	int  lodMinIndices = 32;      // don't generate for meshes below this index count
	float decimationFactor = 1.0f; // < 1 simplifies every mesh's base geometry to this index-count factor
	                               // at cook time (unused vertices dropped; LOD chains build on the result).
	                               // Skinned scenes bypass the cache, so they never decimate.
};

// Byte ranges a mesh's data occupies in its cooked cache file (see CookedSceneData), so a consumer can
// re-read it later without keeping the scene loaded — the renderer's mesh streaming uses this to evict
// cold mesh data from VRAM and re-stream it on demand. Attribute offsets are glm::vec3 arrays of
// numVertices; indices are uint32. Only cooked scenes provide one (getMeshStreamSource default: false).
export struct MeshStreamSource
{
	const char* filePath = nullptr; // cooked cache file; the string lives as long as the ISceneData
	uint64 positionsOffset = 0, normalsOffset = 0, tangentsOffset = 0, bitangentsOffset = 0, texCoordsOffset = 0;
	uint64 indicesOffset = 0;       // LOD0 index array
	uint64 lodIndicesOffset = 0;    // generated LOD levels, concatenated in level order
	uint32 numVertices = 0;
	uint32 numIndices = 0;
};

export class ISceneData
{
public:

	static oc::unique_ptr<ISceneData> createAssimpLoader();
	static oc::unique_ptr<ISceneData> createProceduralLoader();

	// Wraps caller-supplied geometry as a single-mesh, single-material scene ready for ObjectContainer.
	// colorRGBA (optional, RGBA8, colorWidth*colorHeight*4 bytes) becomes the material's diffuse texture;
	// pass null for a fallback checkerboard. Used to turn generated terrain chunks into renderable meshes.
	static oc::unique_ptr<ISceneData> createMeshScene(const MeshGeometryDesc& geometry,
		const uint8* colorRGBA = nullptr, uint32 colorWidth = 0, uint32 colorHeight = 0);

	// Cache-aware load: serves a cooked binary snapshot from Assets/Local/Cooked when it matches the
	// source file (mtime + size) and options, otherwise imports via Assimp, cooks, and writes it for
	// next time. Skinned/animated scenes always import directly (never cooked). Returns an initialized
	// scene, or null on import failure.
	static oc::unique_ptr<ISceneData> loadCached(const char* filePath, bool mergeNodes, bool preTransformVertices, const SceneCookOptions& options);

	// Loads animation clips from a separate file (rig in one file, animations in others) and appends them
	// to outSet, resolving each channel against targetSkeleton BY BONE NAME (retargeting across files that
	// share a skeleton, e.g. Mixamo exports). Any animation whose name contains skipName is ignored (null/
	// "" keeps all) — handy for stripping a "TPose" track exported into every file. trackName selects a
	// single track from a multi-track file (kept only if its name contains trackName; null/"" keeps all).
	// clipNameOverride names the kept clip(s) (suffixed _N if several are kept); when null the file-name
	// stem is used. Returns false if the file has no kept animations.
	static bool loadAnimations(const char* filePath, const Skeleton& targetSkeleton, AnimationSet& outSet, const char* skipName = nullptr, const char* clipNameOverride = nullptr, const char* trackName = nullptr);

	virtual ~ISceneData() = default;

	virtual bool initialize(const char* filePath, bool mergeNodes, bool preTransformVertices) = 0;
	virtual bool isValid() const = 0;

	virtual const oc::string& getFilePath() const = 0;
	virtual const INodeData& getRootNode() const = 0;

	virtual uint32 getNumMeshes() const = 0;
	virtual uint32 getNumMaterials() const = 0;
	virtual uint32 getNumTextures() const = 0;

	virtual const IMeshData* getMesh(const char* pMeshName) const = 0;
	virtual const IMeshData* getMesh(uint32 idx) const = 0;
	virtual bool getMeshStreamSource(uint32 /*meshIdx*/, MeshStreamSource& /*out*/) const { return false; }
	virtual const IMaterialData* getMaterial(uint32 idx) const = 0;
	virtual const ITextureData* getTexture(uint32 idx) const = 0;

	// Skeletal animation. getSkeleton() returns nullptr when the scene has no bones. Animation clips are
	// resolved against that skeleton (channel bone indices already mapped). getAnimations() returns the
	// full named set (for AnimationPlayer::setClipLibrary / play-by-name). Default: none.
	virtual const Skeleton* getSkeleton() const { return nullptr; }
	virtual const AnimationSet* getAnimations() const { return nullptr; }
	virtual uint32 getNumAnimations() const { return 0; }
	virtual const AnimationClip* getAnimation(uint32 idx) const { return nullptr; }
};