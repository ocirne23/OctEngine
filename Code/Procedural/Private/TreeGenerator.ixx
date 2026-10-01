export module Procedural:TreeGenerator;

import Core;
import Core.glm;
import Core.Transform;

import :TreeSpecies;

// The piece library generator and the composite function.
//
// A species generates once into a TreeLibrary: TRUNKS (with attach slots) and BRANCH MODULES (a main
// branch with its sub-branches and leaves). A tree is never generated as a whole: compositeTree() picks a
// trunk and fills its slots with modules from a seed. The composite is pure integer hashing (treeHash) so
// the GPU version (G4) reproduces it bit for bit; keep the two in step.
export namespace Procedural
{
	// One piece mesh in plain arrays (MeshGeometryDesc-compatible). `bones` is the per-vertex bone index
	// inside the piece (rigid, one bone per vertex for now).
	struct TreeMesh
	{
		oc::vector<glm::vec3> positions;
		oc::vector<glm::vec3> normals;
		oc::vector<glm::vec3> tangents;
		oc::vector<glm::vec3> bitangents;
		oc::vector<glm::vec3> texCoords;
		oc::vector<uint8> bones;
		oc::vector<uint32> indices;

		uint32 numVertices() const { return (uint32)positions.size(); }
	};

	// A bone inside a piece: bone 0 is the piece root. Pivot + axis give the wind its rotation centre and
	// branch direction (piece-local).
	struct TreeBone
	{
		int16 parent = -1;
		glm::vec3 pivot{ 0.0f };
		glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
		float length = 1.0f;
	};

	// Where a trunk accepts a module (trunk-local): the module root sits at `pos`, grows along `dir`, and is
	// scaled so its length becomes `length`.
	struct TreeSlot
	{
		glm::vec3 pos{ 0.0f };
		glm::vec3 dir{ 0.0f, 1.0f, 0.0f };
		float length = 1.0f;
		float radius = 0.1f;  // trunk radius at the slot
	};

	struct TreePiece
	{
		TreeMesh bark;
		TreeMesh leaves;
		oc::vector<TreeBone> bones;
		oc::vector<TreeSlot> slots; // trunks only
		float length = 1.0f;        // trunk height / nominal module length (m)
		float baseRadius = 0.1f;    // root radius at the base (m, unscaled)
	};

	struct TreeLibrary
	{
		oc::vector<TreePiece> trunks;
		oc::vector<TreePiece> modules;
	};

	// Pure; any thread. Deterministic from the species (its seed included).
	void generateTreeLibrary(const TreeSpeciesDesc& species, TreeLibrary& out);

	// One placed piece of a composited tree, tree-local.
	struct TreePiecePlacement
	{
		uint16 pieceIdx = 0;
		bool trunk = false;
		Transform local;
	};

	// The composite function: a unique tree from `seed`, tree-local (the caller adds the tree transform).
	// `outTreeScale` is the species-random uniform scale the caller multiplies into that transform.
	void compositeTree(const TreeSpeciesDesc& species, const TreeLibrary& library, uint32 seed,
		oc::vector<TreePiecePlacement>& out, float& outTreeScale);

	// PCG hash - the composite's ONLY randomness, mirrored in GLSL.
	constexpr uint32 treeHash(uint32 v)
	{
		const uint32 state = v * 747796405u + 2891336453u;
		const uint32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}
	constexpr uint32 treeHash(uint32 a, uint32 b) { return treeHash(a ^ treeHash(b)); }
	constexpr float treeHash01(uint32 h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }
}
