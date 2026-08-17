module File;

import Core;

import File.fwd;
import :ProceduralSceneData;
import :ProceduralMeshData;
import :ProceduralTextureData;
import :ProceduralMaterialData;
import :ProceduralNodeData;

oc::unique_ptr<ISceneData> createProceduralLoader()
{
	return oc::make_unique<ProceduralSceneData>();
}

ProceduralSceneData::ProceduralSceneData()
{
}

ProceduralSceneData::~ProceduralSceneData()
{
}

bool ProceduralSceneData::initialize(const char* shapeName, bool /*mergeNodes*/, bool /*preTransformVertices*/)
{
	m_shapeName = shapeName ? shapeName : "";

	EProceduralShape shape;
	bool isTerrain = false;
	bool isSky     = false;
	if (m_shapeName == "cube")
		shape = EProceduralShape::Cube;
	else if (m_shapeName == "plane")
		shape = EProceduralShape::Plane;
	else if (m_shapeName == "sphere")
		shape = EProceduralShape::Sphere;
	else if (m_shapeName == "skysphere")
	{
		shape = EProceduralShape::SkySphere;
		isSky = true;
	}
	else if (m_shapeName == "terrain")
	{
		shape     = EProceduralShape::Terrain;
		isTerrain = true;
	}
	else
	{
		assert(false && "ProceduralSceneData: unknown shape name. Use \"cube\", \"plane\", \"sphere\", \"skysphere\" or \"terrain\".");
		return false;
	}

	// Mesh
	ProceduralMeshData& mesh = m_meshes.emplace_back();
	bool meshOk;
	if (isTerrain)
		meshOk = mesh.initializeTerrain(TerrainParams{}, shapeName);
	else
		meshOk = mesh.initialize(shape, shapeName);
	assert(meshOk && "Failed to initialize ProceduralMeshData");

	// Textures: index 0 = diffuse (checkerboard, or a vertical gradient for the sky), index 1 = flat normal map
	ProceduralTextureData& diffuseTex = m_textures.emplace_back();
	[[maybe_unused]] bool diffuseOk = isSky
		? diffuseTex.initialize(EProceduralTextureType::SkyGradient, 4, 256)
		: diffuseTex.initialize(EProceduralTextureType::Checkerboard, isTerrain ? 1024 : 64, isTerrain ? 1024 : 64);
	assert(diffuseOk && "Failed to initialize diffuse ProceduralTextureData");

	ProceduralTextureData& normalTex = m_textures.emplace_back();
	[[maybe_unused]] bool normalOk = normalTex.initialize(EProceduralTextureType::FlatNormal, 8, 8);
	assert(normalOk && "Failed to initialize normal ProceduralTextureData");

	// Material
	ProceduralMaterialData& material = m_materials.emplace_back();
	[[maybe_unused]] bool matOk = material.initialize("ProceduralMaterial");
	assert(matOk && "Failed to initialize ProceduralMaterialData");
	material.setDiffuseTexIdx(0);
	material.setNormalTexIdx(1);

	// Root node references mesh 0
	m_rootNode.initialize(shapeName, { 0u });

	m_valid = true;
	return true;
}

bool ProceduralSceneData::initializeFromMesh(const MeshGeometryDesc& geometry, const uint8* colorRGBA, uint32 colorWidth, uint32 colorHeight)
{
	m_shapeName = geometry.name ? geometry.name : "Mesh";

	ProceduralMeshData& mesh = m_meshes.emplace_back();
	if (!mesh.initializeFromGeometry(geometry.positions, geometry.normals, geometry.tangents,
		geometry.bitangents, geometry.texCoords, geometry.numVertices, geometry.indices, geometry.numIndices, m_shapeName.c_str()))
	{
		m_meshes.pop_back();
		return false;
	}

	ProceduralMaterialData& material = m_materials.emplace_back();
	[[maybe_unused]] bool matOk = material.initialize("ProceduralMeshMaterial");
	assert(matOk && "Failed to initialize ProceduralMaterialData");

	// Only build textures when the caller supplies a color map. Otherwise the material keeps its default
	// UINT32_MAX texture indices, so the renderer resolves it to the shared fallback diffuse/normal textures
	// (no per-mesh texture upload). Texture 0 = diffuse (color map), texture 1 = flat normal.
	if (colorRGBA && colorWidth > 0 && colorHeight > 0)
	{
		ProceduralTextureData& diffuseTex = m_textures.emplace_back();
		[[maybe_unused]] bool diffuseOk = diffuseTex.initializeFromPixels(reinterpret_cast<const ITextureData::Pixel*>(colorRGBA), colorWidth, colorHeight, "mesh_diffuse");
		assert(diffuseOk && "Failed to initialize diffuse ProceduralTextureData");

		ProceduralTextureData& normalTex = m_textures.emplace_back();
		[[maybe_unused]] bool normalOk = normalTex.initialize(EProceduralTextureType::FlatNormal, 8, 8);
		assert(normalOk && "Failed to initialize normal ProceduralTextureData");

		material.setDiffuseTexIdx(0);
		material.setNormalTexIdx(1);
	}

	m_rootNode.initialize(m_shapeName.c_str(), { 0u });

	m_valid = true;
	return true;
}

const IMeshData* ProceduralSceneData::getMesh(const char* pMeshName) const
{
	for (const ProceduralMeshData& mesh : m_meshes)
	{
		if (oc::string(mesh.getName()) == pMeshName)
			return &mesh;
	}
	return nullptr;
}

const IMeshData* ProceduralSceneData::getMesh(uint32 idx) const
{
	assert(idx < m_meshes.size());
	return &m_meshes[idx];
}

const IMaterialData* ProceduralSceneData::getMaterial(uint32 idx) const
{
	assert(idx < m_materials.size());
	return &m_materials[idx];
}

const ITextureData* ProceduralSceneData::getTexture(uint32 idx) const
{
	assert(idx < m_textures.size());
	return &m_textures[idx];
}
