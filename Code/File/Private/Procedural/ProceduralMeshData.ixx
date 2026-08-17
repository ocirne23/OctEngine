export module File:ProceduralMeshData;

import Core;
import Core.glm;
import Core.AABB;
import :IMeshData;

export enum class EProceduralShape
{
	Cube,
	Plane,
	Sphere,
	SkySphere, // inward-facing sphere (viewed from inside); equirectangular UVs
	Terrain,
};

// Parameters for procedural terrain generation.
export struct TerrainParams
{
	uint32 subdivisions = 64;   // number of quads per side (total verts = (subdivisions+1)^2)
	float  amplitude    = 0.1f; // peak-to-peak height range
	uint32 octaves      = 6;    // fBm octave count
	float  frequency    = 3.0f; // base spatial frequency
	float  persistence  = 0.5f; // amplitude scale per octave
	float  lacunarity   = 2.0f; // frequency scale per octave
	uint32 seed         = 0;
};

export class ProceduralMeshData final : public IMeshData
{
public:
	ProceduralMeshData();
	~ProceduralMeshData();
	ProceduralMeshData(const ProceduralMeshData&) = delete;
	ProceduralMeshData(ProceduralMeshData&&) = default;

	bool initialize(EProceduralShape shape, const char* name = nullptr);
	bool initializeTerrain(const TerrainParams& params, const char* name = nullptr);

	// Build directly from caller-supplied geometry (e.g. runtime-generated terrain chunks). Arrays are
	// getNumVertices() long; tangents/bitangents/texCoords may be null (filled with sensible defaults).
	bool initializeFromGeometry(
		const glm::vec3* positions, const glm::vec3* normals,
		const glm::vec3* tangents, const glm::vec3* bitangents, const glm::vec3* texCoords,
		uint32 numVertices, const uint32* indices, uint32 numIndices, const char* name = nullptr);

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
	const char* getName() const override;

private:
	void buildCube();
	void buildPlane();
	void buildSphere(uint32 stacks = 16, uint32 slices = 32);
	void buildSkySphere(uint32 stacks = 24, uint32 slices = 48);
	void buildTerrain(const TerrainParams& params);

	oc::string              m_name;
	oc::vector<glm::vec3>   m_vertices;
	oc::vector<glm::vec3>   m_normals;
	oc::vector<glm::vec3>   m_tangents;
	oc::vector<glm::vec3>   m_bitangents;
	oc::vector<glm::vec3>   m_texCoords; // xy = UV, z = 0
	oc::vector<uint32>      m_indices;
	AABB                     m_aabb;
};
