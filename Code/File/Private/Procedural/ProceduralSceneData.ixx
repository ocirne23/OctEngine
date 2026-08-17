export module File:ProceduralSceneData;

import Core;

import File.fwd;
import :ISceneData;
import :ProceduralMeshData;
import :ProceduralTextureData;
import :ProceduralMaterialData;
import :ProceduralNodeData;

export oc::unique_ptr<ISceneData> createProceduralLoader();

// Generates a single procedural shape scene from a shape name.
// Supported values for the 'filePath' parameter of initialize():
//   "cube"   - axis-aligned unit cube  (extent +-0.5)
//   "plane"  - unit quad in the XZ plane, normal +Y
//   "sphere" - UV sphere with unit diameter
// The 'mergeNodes' and 'preTransformVertices' parameters are unused.
export class ProceduralSceneData final : public ISceneData
{
public:
	ProceduralSceneData();
	~ProceduralSceneData();
	ProceduralSceneData(const ProceduralSceneData&) = delete;
	ProceduralSceneData(ProceduralSceneData&&) = default;

	bool initialize(const char* shapeName, bool mergeNodes, bool preTransformVertices) override;

	// Assemble a single-mesh scene from caller-supplied geometry + optional RGBA8 diffuse pixels.
	bool initializeFromMesh(const MeshGeometryDesc& geometry, const uint8* colorRGBA, uint32 colorWidth, uint32 colorHeight);

	bool isValid() const override { return m_valid; }

	const oc::string& getFilePath() const override { return m_shapeName; }
	const INodeData&   getRootNode() const override { return m_rootNode; }

	uint32 getNumMeshes()    const override { return (uint32)m_meshes.size(); }
	uint32 getNumMaterials() const override { return (uint32)m_materials.size(); }
	uint32 getNumTextures()  const override { return (uint32)m_textures.size(); }

	const IMeshData*     getMesh(const char* pMeshName) const override;
	const IMeshData*     getMesh(uint32 idx)            const override;
	const IMaterialData* getMaterial(uint32 idx)        const override;
	const ITextureData*  getTexture(uint32 idx)         const override;

private:
	oc::string                       m_shapeName;
	bool                              m_valid = false;
	oc::vector<ProceduralMeshData>     m_meshes;
	oc::vector<ProceduralTextureData>  m_textures;
	oc::vector<ProceduralMaterialData> m_materials;
	ProceduralNodeData                  m_rootNode;
};
