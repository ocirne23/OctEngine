module File;

import Core;
import Core.glm;
import Core.AABB;

import :ProceduralMeshData;

ProceduralMeshData::ProceduralMeshData()
{
}

ProceduralMeshData::~ProceduralMeshData()
{
}

bool ProceduralMeshData::initialize(EProceduralShape shape, const char* name)
{
	switch (shape)
	{
	case EProceduralShape::Cube:
		m_name = name ? name : "Cube";
		buildCube();
		break;
	case EProceduralShape::Plane:
		m_name = name ? name : "Plane";
		buildPlane();
		break;
	case EProceduralShape::Sphere:
		m_name = name ? name : "Sphere";
		buildSphere();
		break;
	case EProceduralShape::SkySphere:
		m_name = name ? name : "SkySphere";
		buildSkySphere();
		break;
	default:
		return false;
	}

	m_aabb.min = glm::vec3(oc::numeric_limits<float>::max());
	m_aabb.max = glm::vec3(oc::numeric_limits<float>::lowest());
	for (const glm::vec3& v : m_vertices)
	{
		m_aabb.min = glm::min(m_aabb.min, v);
		m_aabb.max = glm::max(m_aabb.max, v);
	}
	return true;
}

const glm::vec3* ProceduralMeshData::getVertices()    const { return m_vertices.data(); }
const glm::vec3* ProceduralMeshData::getNormals()     const { return m_normals.data(); }
const glm::vec3* ProceduralMeshData::getTangents()    const { return m_tangents.data(); }
const glm::vec3* ProceduralMeshData::getBitangents()  const { return m_bitangents.data(); }
const glm::vec3* ProceduralMeshData::getTexCoords()   const { return m_texCoords.data(); }
const uint32*    ProceduralMeshData::getIndices()      const { return m_indices.data(); }
uint32           ProceduralMeshData::getNumVertices()  const { return (uint32)m_vertices.size(); }
uint32           ProceduralMeshData::getNumIndices()   const { return (uint32)m_indices.size(); }
uint32           ProceduralMeshData::getMaterialIndex() const { return 0; }
AABB             ProceduralMeshData::getAABB()          const { return m_aabb; }
const char*      ProceduralMeshData::getName()          const { return m_name.c_str(); }

bool ProceduralMeshData::initializeFromGeometry(
	const glm::vec3* positions, const glm::vec3* normals,
	const glm::vec3* tangents, const glm::vec3* bitangents, const glm::vec3* texCoords,
	uint32 numVertices, const uint32* indices, uint32 numIndices, const char* name)
{
	if (!positions || !normals || !indices || numVertices == 0 || numIndices == 0)
		return false;

	m_name = name ? name : "Mesh";
	m_vertices.assign(positions, positions + numVertices);
	m_normals.assign(normals, normals + numVertices);

	if (tangents)   m_tangents.assign(tangents, tangents + numVertices);
	else            m_tangents.assign(numVertices, glm::vec3(1.0f, 0.0f, 0.0f));
	if (bitangents) m_bitangents.assign(bitangents, bitangents + numVertices);
	else            m_bitangents.assign(numVertices, glm::vec3(0.0f, 0.0f, 1.0f));
	if (texCoords)  m_texCoords.assign(texCoords, texCoords + numVertices);
	else            m_texCoords.assign(numVertices, glm::vec3(0.0f));

	m_indices.assign(indices, indices + numIndices);

	m_aabb.min = glm::vec3(oc::numeric_limits<float>::max());
	m_aabb.max = glm::vec3(oc::numeric_limits<float>::lowest());
	for (const glm::vec3& v : m_vertices)
	{
		m_aabb.min = glm::min(m_aabb.min, v);
		m_aabb.max = glm::max(m_aabb.max, v);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Cube: 6 faces * 4 verts = 24 vertices, 6 faces * 6 indices = 36 indices
// All vertices in CCW winding order when viewed from outside.
// ---------------------------------------------------------------------------
void ProceduralMeshData::buildCube()
{
	struct FaceDesc
	{
		glm::vec3 normal;
		glm::vec3 tangent;
		glm::vec3 bitangent;
		glm::vec3 verts[4]; // BL, BR, TR, TL (CCW from outside)
	};

	constexpr float h = 0.5f;
	const FaceDesc faces[6] =
	{
		// +Z front
		{ {0,0,1},  {1,0,0},  {0,1,0},  { {-h,-h,h}, { h,-h,h}, { h, h,h}, {-h, h,h} } },
		// -Z back
		{ {0,0,-1}, {-1,0,0}, {0,1,0},  { { h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h} } },
		// +X right
		{ {1,0,0},  {0,0,-1}, {0,1,0},  { { h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h} } },
		// -X left
		{ {-1,0,0}, {0,0,1},  {0,1,0},  { {-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h} } },
		// +Y top  (verts go front-left→front-right→back-right→back-left so cross gives +Y)
		{ {0,1,0},  {1,0,0},  {0,0,-1}, { {-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h} } },
		// -Y bottom (verts go back-left→back-right→front-right→front-left so cross gives -Y)
		{ {0,-1,0}, {1,0,0},  {0,0,1},  { {-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h} } },
	};

	const glm::vec3 uvs[4] = { {0,0,0},{1,0,0},{1,1,0},{0,1,0} };

	for (const FaceDesc& face : faces)
	{
		const uint32 base = (uint32)m_vertices.size();
		for (int i = 0; i < 4; i++)
		{
			m_vertices.push_back(face.verts[i]);
			m_normals.push_back(face.normal);
			m_tangents.push_back(face.tangent);
			m_bitangents.push_back(face.bitangent);
			m_texCoords.push_back(uvs[i]);
		}
		// Two CCW triangles from the quad
		m_indices.push_back(base + 0); m_indices.push_back(base + 1); m_indices.push_back(base + 2);
		m_indices.push_back(base + 0); m_indices.push_back(base + 2); m_indices.push_back(base + 3);
	}
}

// ---------------------------------------------------------------------------
// Plane: unit quad in the XZ plane, normal pointing +Y
// ---------------------------------------------------------------------------
void ProceduralMeshData::buildPlane()
{
	constexpr float h = 0.5f;
	// Vertices ordered front-left→front-right→back-right→back-left so cross gives +Y
	m_vertices   = { {-h,0, h}, { h,0, h}, { h,0,-h}, {-h,0,-h} };
	m_normals    = { {0,1,0}, {0,1,0}, {0,1,0}, {0,1,0} };
	m_tangents   = { {1,0,0}, {1,0,0}, {1,0,0}, {1,0,0} };
	// V direction goes from z=+h to z=-h, so bitangent = (0,0,-1)
	m_bitangents = { {0,0,-1}, {0,0,-1}, {0,0,-1}, {0,0,-1} };
	m_texCoords  = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} };
	m_indices    = { 0,1,2, 0,2,3 };
}

bool ProceduralMeshData::initializeTerrain(const TerrainParams& params, const char* name)
{
	m_name = name ? name : "Terrain";
	buildTerrain(params);

	m_aabb.min = glm::vec3(oc::numeric_limits<float>::max());
	m_aabb.max = glm::vec3(oc::numeric_limits<float>::lowest());
	for (const glm::vec3& v : m_vertices)
	{
		m_aabb.min = glm::min(m_aabb.min, v);
		m_aabb.max = glm::max(m_aabb.max, v);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Terrain helpers: seeded value noise + fractional Brownian motion
// ---------------------------------------------------------------------------
namespace
{
	// Hash a 2D integer coordinate + seed into a float in [-1, 1]
	float valueNoise2D(int32_t ix, int32_t iy, uint32_t seed)
	{
		uint32_t h = (uint32_t)(ix * 1619 + iy * 31337) ^ (seed * 6971u);
		h ^= h >> 13;
		h  = h * (h * h * 15731u + 789221u) + 1376312589u;
		return (float)(h & 0xffffu) / 32767.5f - 1.0f; // [-1, 1]
	}

	// Bilinear noise with quintic smoothstep interpolation
	float smoothNoise2D(float x, float y, uint32_t seed)
	{
		const int32_t ix = (int32_t)std::floor(x);
		const int32_t iy = (int32_t)std::floor(y);
		const float   fx = x - (float)ix;
		const float   fy = y - (float)iy;
		// Quintic fade
		const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
		const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

		const float v00 = valueNoise2D(ix,     iy,     seed);
		const float v10 = valueNoise2D(ix + 1, iy,     seed);
		const float v01 = valueNoise2D(ix,     iy + 1, seed);
		const float v11 = valueNoise2D(ix + 1, iy + 1, seed);

		return glm::mix(glm::mix(v00, v10, ux), glm::mix(v01, v11, ux), uy);
	}

	// Fractional Brownian motion: accumulate octaves of noise
	float fbm(float x, float y, uint32_t octaves, float persistence, float lacunarity, uint32_t seed)
	{
		float value    = 0.0f;
		float amplitude = 1.0f;
		float freq     = 1.0f;
		float maxValue = 0.0f;
		for (uint32_t o = 0; o < octaves; o++)
		{
			value    += smoothNoise2D(x * freq, y * freq, seed + o * 1973u) * amplitude;
			maxValue += amplitude;
			amplitude *= persistence;
			freq      *= lacunarity;
		}
		return maxValue > 0.0f ? (value / maxValue) : 0.0f; // normalize to ~[-1, 1]
	}
}

// ---------------------------------------------------------------------------
// Terrain: subdivided plane in XZ with fBm height displacement on Y
// ---------------------------------------------------------------------------
void ProceduralMeshData::buildTerrain(const TerrainParams& params)
{
	const uint32 N    = glm::max(params.subdivisions, 1u);
	const float  invN = 1.0f / (float)N;

	// Helper lambda: sample terrain height for a normalised (u,v) in [0,1]
	auto sampleHeight = [&](float u, float v) -> float
	{
		const float sx = (u - 0.5f) * params.frequency;
		const float sz = (v - 0.5f) * params.frequency;
		return fbm(sx, sz, params.octaves, params.persistence, params.lacunarity, params.seed)
			   * params.amplitude;
	};

	// Build vertex grid
	m_vertices.reserve((N + 1) * (N + 1));
	m_normals.reserve((N + 1) * (N + 1));
	m_tangents.reserve((N + 1) * (N + 1));
	m_bitangents.reserve((N + 1) * (N + 1));
	m_texCoords.reserve((N + 1) * (N + 1));

	const float eps = invN * 0.5f; // finite-difference step (half a cell)

	for (uint32 row = 0; row <= N; row++)
	{
		for (uint32 col = 0; col <= N; col++)
		{
			const float u = (float)col * invN; // [0,1]
			const float v = (float)row * invN; // [0,1]

			const float x = u - 0.5f; // [-0.5, 0.5]
			const float z = v - 0.5f; // [-0.5, 0.5]
			const float y = sampleHeight(u, v);

			m_vertices.push_back({ x, y, z });
			m_texCoords.push_back({ u, v, 0.0f });

			// Central-difference normal: sample neighbors to get surface tangent vectors
			const float hL = sampleHeight(u - eps, v);
			const float hR = sampleHeight(u + eps, v);
			const float hD = sampleHeight(u, v - eps);
			const float hU = sampleHeight(u, v + eps);

			// dP/du and dP/dv in world space (spacing = eps in u/v = eps in x/z)
			const glm::vec3 dpdu(2.0f * eps, hR - hL, 0.0f);
			const glm::vec3 dpdv(0.0f,       hU - hD, 2.0f * eps);

			const glm::vec3 normal    = -glm::normalize(glm::cross(dpdu, dpdv));
			const glm::vec3 tangent   = glm::normalize(dpdu);
			const glm::vec3 bitangent = glm::normalize(dpdv);

			m_normals.push_back(normal);
			m_tangents.push_back(tangent);
			m_bitangents.push_back(bitangent);
		}
	}

	// Build index buffer (two CCW triangles per quad)
	m_indices.reserve(N * N * 6);
	for (uint32 row = 0; row < N; row++)
	{
		for (uint32 col = 0; col < N; col++)
		{
			const uint32 a = row       * (N + 1) + col;
				const uint32 b = row       * (N + 1) + col + 1;
				const uint32 c = (row + 1) * (N + 1) + col;
				const uint32 d = (row + 1) * (N + 1) + col + 1;

				m_indices.push_back(a); m_indices.push_back(c); m_indices.push_back(b);
				m_indices.push_back(b); m_indices.push_back(c); m_indices.push_back(d);
		}
	}
}

// ---------------------------------------------------------------------------
// Sky sphere: a UV sphere viewed from the inside. Built from the regular sphere, then normals/
// bitangents are flipped inward and the triangle winding reversed so the inside faces pass backface
// culling. UVs stay equirectangular (u = longitude, v = latitude, v=0 at the top).
// ---------------------------------------------------------------------------
void ProceduralMeshData::buildSkySphere(uint32 stacks, uint32 slices)
{
	buildSphere(stacks, slices);
	for (glm::vec3& n : m_normals)    n = -n;
	for (glm::vec3& b : m_bitangents) b = -b; // keep the TBN right-handed with the flipped normal
	for (size_t i = 0; i + 2 < m_indices.size(); i += 3)
		oc::swap(m_indices[i + 1], m_indices[i + 2]);

	// The sky gradient only varies vertically, so collapse u (the longitude seam and the u-derivative
	// blow-up at the poles otherwise cause a visible pinch) and inset v from the texture edges: at
	// exactly v=0/1 a bilinear repeat-wrap sampler blends the zenith row with the ground row, which
	// shows as a dark spot at the top of the sky.
	for (glm::vec3& uv : m_texCoords)
	{
		uv.x = 0.5f;
		uv.y = glm::mix(0.01f, 0.99f, uv.y);
	}
}

// ---------------------------------------------------------------------------
// UV Sphere: unit sphere (diameter 1), stacks = latitude bands, slices = longitude bands
// ---------------------------------------------------------------------------
void ProceduralMeshData::buildSphere(uint32 stacks, uint32 slices)
{
	const float pi = glm::pi<float>();

	for (uint32 stack = 0; stack <= stacks; stack++)
	{
		const float phi    = pi * (float)stack / (float)stacks; // 0 (top) to pi (bottom)
		const float sinPhi = std::sin(phi);
		const float cosPhi = std::cos(phi);

		for (uint32 slice = 0; slice <= slices; slice++)
		{
			const float theta    = 2.0f * pi * (float)slice / (float)slices;
			const float sinTheta = std::sin(theta);
			const float cosTheta = std::cos(theta);

			// Unit sphere position; scale to 0.5 for unit diameter
			const glm::vec3 normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
			const glm::vec3 tangent  = glm::normalize(glm::vec3(-sinTheta, 0.0f, cosTheta));
			const glm::vec3 bitangent = glm::cross(normal, tangent);
			const glm::vec3 uv((float)slice / (float)slices, (float)stack / (float)stacks, 0.0f);

			m_vertices.push_back(normal * 0.5f);
			m_normals.push_back(normal);
			m_tangents.push_back(tangent);
			m_bitangents.push_back(bitangent);
			m_texCoords.push_back(uv);
		}
	}

	for (uint32 stack = 0; stack < stacks; stack++)
	{
		for (uint32 slice = 0; slice < slices; slice++)
		{
			const uint32 a = stack * (slices + 1) + slice;
			const uint32 b = a + 1;
			const uint32 c = a + (slices + 1);
			const uint32 d = c + 1;

			m_indices.push_back(a); m_indices.push_back(b); m_indices.push_back(c);
			m_indices.push_back(b); m_indices.push_back(d); m_indices.push_back(c);
		}
	}
}
