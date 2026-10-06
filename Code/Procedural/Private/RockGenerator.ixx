export module Procedural:RockGenerator;

import Core;
import Core.glm;

import :RockType;

// The rock shape generator (Docs/RockRenderingPlan.md 4). A variant is a SIGNED DISTANCE FIELD at a nominal size
// of 1 (the longest axis), built from the type and a seed into a RockShape: every random decision is made ONCE
// there, so rockSdf is a pure function of (shape, point). The field is meshed on the CPU into a regular mesh LOD
// chain (no GPU tessellation).
//
// The field lives in the SHAPE frame (the body centred on the origin). The meshes are in ROCK-LOCAL space: Y up, the
// lowest point of the body at y = 0 (the caller sinks it into the ground) - shape frame = rock-local + (0, originY, 0).
export namespace Procedural
{
	constexpr uint32 ROCK_MAX_BLOCKS = (uint32)ROCK_MAX_PILE;
	constexpr uint32 ROCK_MAX_PLANES = 16;
	constexpr uint32 ROCK_MAX_PITS = 32;
	// Mesh LOD levels per variant (RendererVK's MAX_MESH_LODS is 5): level 0 at the type's `Lod` triangles, each
	// further level a quarter of the one before, down to ROCK_LOD_MIN_TRIANGLES.
	constexpr uint32 ROCK_MAX_LODS = 4;
	constexpr uint32 ROCK_LOD_MIN_TRIANGLES = 24;
	// The far volume's view of a variant (RendererVK "Far-tree volume"): its OCCUPANCY (0..1, soft over one voxel)
	// on a ROCK_DENSITY_RES^3 grid over its rock-local box. A rock is a solid: the volume gives it one extinction at any
	// size ("Trees/Far rock extinction"), so the grid holds no extinction of its own.
	constexpr uint32 ROCK_DENSITY_RES = 16;

	struct RockShape
	{
		ERockShape kind = ERockShape::Boulder;
		uint32 blockCount = 1;
		glm::vec3 blockCentre[ROCK_MAX_BLOCKS] = {};
		glm::vec3 blockHalf[ROCK_MAX_BLOCKS] = {};
		glm::mat3 blockRot[ROCK_MAX_BLOCKS] = {}; // shape frame -> block frame (a pile block is turned AND tilted)
		float blockProfile[ROCK_MAX_BLOCKS][ROCK_PROFILE_POINTS] = {}; // Pillar: each one's widths, base to top (max 1)
		float floorY = 0.0f;                   // Pillar: the flat floor every pillar of the group stands on (shape frame)
		float round = 0.05f;
		float squareness = 2.0f;               // the superellipsoid exponent (a Pillar's section)
		float erosion = 0.0f;                // the body is shrunk by this, then the field grown by it (rounded edges)
		float warpAmplitude = 0.0f;
		float warpFrequency = 1.0f;
		uint32 planeCount = 0;
		glm::vec4 planes[ROCK_MAX_PLANES] = {}; // xyz normal, w offset: the body is cut to dot(p, n) <= w
		bool split = false;
		glm::vec4 splitPlane{ 0.0f };          // the crack's centre plane
		float splitGap = 0.0f;
		uint32 pitCount = 0;
		glm::vec4 pits[ROCK_MAX_PITS] = {};     // xyz centre, w radius (a sphere subtracted)
		float strataSpacing = 0.0f;
		float strataDepth = 0.0f;
		float strataVar = 0.0f;
		float strataPhase = 0.0f;
		float noiseAmplitude = 0.0f;
		float noiseFrequency = 1.0f;
		uint32 noiseOctaves = 0;
		glm::vec3 noiseScale{ 1.0f };          // the noise's sample point x this (1 / NoiseStretch on y)
		float ridged = 0.0f;
		uint32 noiseSeed = 0;
		glm::vec3 boundsMin{ -0.5f };          // conservative: the body plus its noise
		glm::vec3 boundsMax{ 0.5f };
		float originY = 0.0f;                  // the body's lowest point in the shape frame (generateRockVariant)
	};

	// Pure; any thread. Deterministic from the type (its seed included) and `seed`.
	void buildRockShape(const RockTypeDesc& type, uint32 seed, RockShape& out);
	// The field at a rock-local point (negative inside). Not an exact distance once noise and strata displace it.
	float rockSdf(const RockShape& shape, const glm::vec3& p);
	glm::vec3 rockSdfNormal(const RockShape& shape, const glm::vec3& p, float eps);

	// A rock mesh in plain arrays (MeshGeometryDesc-compatible). texCoords.x = the vertex's CAVITY (1 = open,
	// 0 = deep in a crevice: an ambient occlusion from the field itself) - a rock has no uv (the rock material
	// projects in world space), so the u channel carries it to the rock vertex shader.
	struct RockMesh
	{
		oc::vector<glm::vec3> positions;
		oc::vector<glm::vec3> normals;
		oc::vector<glm::vec3> tangents;
		oc::vector<glm::vec3> bitangents;
		oc::vector<glm::vec3> texCoords;
		oc::vector<uint32> indices;

		uint32 numVertices() const { return (uint32)positions.size(); }
	};

	struct RockVariant
	{
		RockShape shape;
		// The mesh LOD chain, level 0 first; lodError = each level's deviation from the full surface in rock-local
		// units (0 for level 0) - RendererVK's screen-space-error selector (Renderer::createMeshLodChain).
		RockMesh lods[ROCK_MAX_LODS];
		float lodError[ROCK_MAX_LODS] = {};
		uint32 lodCount = 0;
		float height = 1.0f; // nominal (the body's top above y = 0)
		// ROCK_DENSITY_RES^3 (x fastest) over [densityMin, densityMax], rock-local.
		oc::vector<float> density;
		glm::vec3 densityMin{ 0.0f };
		glm::vec3 densityMax{ 0.0f };
	};

	// Surface nets + projection + the cavity + the LOD chain. Pure; any thread (single-threaded inside).
	void generateRockVariant(const RockTypeDesc& type, uint32 seed, uint32 gridResolution, RockVariant& out);
}
