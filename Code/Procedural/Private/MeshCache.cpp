module;

#include <zstd/zstd.h> // the cache files' compressor

module Procedural;

import Core;
import Core.glm;
import Core.Log;
import File;
import RendererVK;

import :MeshCache;
import :TreeGenerator;
import :RockGenerator;

namespace
{
	using namespace Procedural;

	constexpr uint32 MESH_CACHE_MAGIC = 0x434D434Fu; // "OCMC"
	constexpr int32 MESH_CACHE_ZSTD_LEVEL = 3;       // fast: a species is tens of MB raw
	enum EMeshCacheKind : uint32 { MeshCacheTree = 1, MeshCacheRock = 2, MeshCacheClutter = 3 };

	struct Header
	{
		uint32 magic = MESH_CACHE_MAGIC;
		uint32 kind = 0;
		uint32 version = 0;
		uint32 reserved = 0;
		uint64 hash = 0;
		uint64 rawSize = 0; // the payload before compression
	};

	struct Writer
	{
		oc::vector<uint8> data;
		void bytes(const void* src, size_t n)
		{
			const uint8* p = (const uint8*)src;
			data.insert(data.end(), p, p + n);
		}
		template<typename T>
		void pod(const T& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			bytes(&v, sizeof(T));
		}
		template<typename T>
		void vec(const oc::vector<T>& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			pod((uint32)v.size());
			bytes(v.data(), sizeof(T) * v.size());
		}
	};

	struct Reader
	{
		const oc::vector<uint8>& data;
		size_t cursor = 0;
		bool ok = true;
		bool bytes(void* dst, size_t n)
		{
			if (!ok || n > data.size() - cursor)
				return ok = false;
			memcpy(dst, data.data() + cursor, n);
			cursor += n;
			return true;
		}
		template<typename T>
		bool pod(T& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return bytes(&v, sizeof(T));
		}
		template<typename T>
		bool vec(oc::vector<T>& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			uint32 n = 0;
			if (!pod(n) || (size_t)n * sizeof(T) > data.size() - cursor)
				return ok = false;
			v.resize(n);
			return bytes(v.data(), sizeof(T) * n);
		}
	};

	// The file: the header, then the zstd payload. A missing file, another kind / version / hash, or a damaged payload
	// is a miss.
	bool readCacheFile(const oc::string& path, uint32 kind, uint32 version, uint64 hash, oc::vector<uint8>& outRaw)
	{
		oc::vector<uint8> file;
		if (!FileSystem::readFileBytes(path, file) || file.size() < sizeof(Header))
			return false;
		Header h;
		memcpy(&h, file.data(), sizeof(Header));
		if (h.magic != MESH_CACHE_MAGIC || h.kind != kind || h.version != version || h.hash != hash || h.rawSize > (1ull << 32))
			return false;
		outRaw.resize((size_t)h.rawSize);
		const size_t n = ZSTD_decompress(outRaw.data(), outRaw.size(), file.data() + sizeof(Header), file.size() - sizeof(Header));
		return !ZSTD_isError(n) && n == outRaw.size();
	}

	void writeCacheFile(const oc::string& path, uint32 kind, uint32 version, uint64 hash, const oc::vector<uint8>& raw)
	{
		oc::vector<uint8> file(sizeof(Header) + ZSTD_compressBound(raw.size()));
		const size_t n = ZSTD_compress(file.data() + sizeof(Header), file.size() - sizeof(Header), raw.data(), raw.size(), MESH_CACHE_ZSTD_LEVEL);
		if (ZSTD_isError(n))
			return;
		Header h;
		h.kind = kind;
		h.version = version;
		h.hash = hash;
		h.rawSize = raw.size();
		memcpy(file.data(), &h, sizeof(Header));
		file.resize(sizeof(Header) + n);
		FileSystem::createDirectories(FileSystem::parentPath(path));
		if (!FileSystem::writeFileBytes(path, oc::span<const uint8>(file.data(), file.size())))
			Log::warning(oc::format("Mesh cache: could not write '{}'", path));
	}

	// ---- Trees ----

	void writeMesh(Writer& w, const TreeMesh& m)
	{
		w.vec(m.positions); w.vec(m.normals); w.vec(m.tangents); w.vec(m.bitangents); w.vec(m.texCoords); w.vec(m.bones); w.vec(m.indices);
	}
	bool readMesh(Reader& r, TreeMesh& m)
	{
		return r.vec(m.positions) && r.vec(m.normals) && r.vec(m.tangents) && r.vec(m.bitangents) && r.vec(m.texCoords)
			&& r.vec(m.bones) && r.vec(m.indices);
	}

	void writePiece(Writer& w, const TreePiece& p)
	{
		for (uint32 k = 0; k < TREE_PIECE_LODS; ++k)
		{
			writeMesh(w, p.bark[k]);
			writeMesh(w, p.leaves[k]);
			writeMesh(w, p.trunkBark[k]);
			writeMesh(w, p.branchBark[k]);
			w.pod(p.lodError[k]);
		}
		w.vec(p.bones);
		w.vec(p.slots);
		w.vec(p.placements);
		w.pod(p.length);
		w.pod(p.baseRadius);
		w.pod(p.pitch);
	}
	bool readPiece(Reader& r, TreePiece& p)
	{
		for (uint32 k = 0; k < TREE_PIECE_LODS; ++k)
			if (!readMesh(r, p.bark[k]) || !readMesh(r, p.leaves[k]) || !readMesh(r, p.trunkBark[k]) || !readMesh(r, p.branchBark[k])
				|| !r.pod(p.lodError[k]))
				return false;
		return r.vec(p.bones) && r.vec(p.slots) && r.vec(p.placements) && r.pod(p.length) && r.pod(p.baseRadius) && r.pod(p.pitch);
	}

	void writePieces(Writer& w, const oc::vector<TreePiece>& pieces)
	{
		w.pod((uint32)pieces.size());
		for (const TreePiece& p : pieces)
			writePiece(w, p);
	}
	bool readPieces(Reader& r, oc::vector<TreePiece>& pieces)
	{
		uint32 n = 0;
		if (!r.pod(n) || n > 4096)
			return false;
		pieces.clear();
		pieces.resize(n);
		for (TreePiece& p : pieces)
			if (!readPiece(r, p))
				return false;
		return true;
	}

	// ---- Rocks ----

	void writeMesh(Writer& w, const RockMesh& m)
	{
		w.vec(m.positions); w.vec(m.normals); w.vec(m.tangents); w.vec(m.bitangents); w.vec(m.texCoords); w.vec(m.indices);
	}
	bool readMesh(Reader& r, RockMesh& m)
	{
		return r.vec(m.positions) && r.vec(m.normals) && r.vec(m.tangents) && r.vec(m.bitangents) && r.vec(m.texCoords) && r.vec(m.indices);
	}
}

namespace Procedural
{
	uint64 meshCacheHash(oc::string_view text)
	{
		uint64 h = 14695981039346656037ull;
		for (const char c : text)
			h = (h ^ (uint8)c) * 1099511628211ull;
		return h;
	}

	uint64 meshCacheMix(uint64 hash, uint64 value)
	{
		for (uint32 i = 0; i < 8; ++i)
			hash = (hash ^ ((value >> (i * 8)) & 0xFFu)) * 1099511628211ull;
		return hash;
	}

	bool loadTreeMeshes(const oc::string& path, uint64 hash, TreeLibrary& library, oc::vector<TreePiece>& variants,
		oc::vector<TreeDensityGrid>& density)
	{
		oc::vector<uint8> raw;
		if (!readCacheFile(path, MeshCacheTree, TREE_MESH_CACHE_VERSION, hash, raw))
			return false;
		Reader r{ raw };
		if (!readPieces(r, library.trunks) || !readPieces(r, library.modules) || !readPieces(r, variants))
			return false;
		uint32 n = 0;
		if (!r.pod(n) || n != variants.size())
			return false;
		density.resize(n);
		for (TreeDensityGrid& grid : density)
			if (!r.vec(grid.values) || !r.pod(grid.min) || !r.pod(grid.max))
				return false;
		return r.cursor == raw.size();
	}

	void saveTreeMeshes(const oc::string& path, uint64 hash, const TreeLibrary& library, const oc::vector<TreePiece>& variants,
		const oc::vector<TreeDensityGrid>& density)
	{
		Writer w;
		writePieces(w, library.trunks);
		writePieces(w, library.modules);
		writePieces(w, variants);
		w.pod((uint32)density.size());
		for (const TreeDensityGrid& grid : density)
		{
			w.vec(grid.values);
			w.pod(grid.min);
			w.pod(grid.max);
		}
		writeCacheFile(path, MeshCacheTree, TREE_MESH_CACHE_VERSION, hash, w.data);
	}

	bool loadRockVariant(const oc::string& path, uint64 hash, RockVariant& out)
	{
		oc::vector<uint8> raw;
		if (!readCacheFile(path, MeshCacheRock, ROCK_MESH_CACHE_VERSION, hash, raw))
			return false;
		Reader r{ raw };
		if (!r.pod(out.shape) || !r.pod(out.lodCount) || out.lodCount > ROCK_MAX_LODS)
			return false;
		for (uint32 k = 0; k < ROCK_MAX_LODS; ++k)
			if (!readMesh(r, out.lods[k]) || !r.pod(out.lodError[k]))
				return false;
		return r.pod(out.height) && r.vec(out.density) && r.pod(out.densityMin) && r.pod(out.densityMax) && r.cursor == raw.size();
	}

	void saveRockVariant(const oc::string& path, uint64 hash, const RockVariant& variant)
	{
		Writer w;
		w.pod(variant.shape);
		w.pod(variant.lodCount);
		for (uint32 k = 0; k < ROCK_MAX_LODS; ++k)
		{
			writeMesh(w, variant.lods[k]);
			w.pod(variant.lodError[k]);
		}
		w.pod(variant.height);
		w.vec(variant.density);
		w.pod(variant.densityMin);
		w.pod(variant.densityMax);
		writeCacheFile(path, MeshCacheRock, ROCK_MESH_CACHE_VERSION, hash, w.data);
	}

	bool loadClutterMeshes(const oc::string& path, uint64 hash, oc::vector<Renderer::ClutterMesh>& out)
	{
		oc::vector<uint8> raw;
		if (!readCacheFile(path, MeshCacheClutter, CLUTTER_MESH_CACHE_VERSION, hash, raw))
			return false;
		Reader r{ raw };
		uint32 n = 0;
		if (!r.pod(n) || n > 4096)
			return false;
		out.clear();
		out.resize(n);
		for (Renderer::ClutterMesh& mesh : out)
			for (uint32 k = 0; k < RendererVKLayout::CLUTTER_LODS; ++k)
				if (!r.vec(mesh.vertices[k]) || !r.vec(mesh.indices[k]))
					return false;
		return r.cursor == raw.size();
	}

	void saveClutterMeshes(const oc::string& path, uint64 hash, const oc::vector<Renderer::ClutterMesh>& meshes)
	{
		Writer w;
		w.pod((uint32)meshes.size());
		for (const Renderer::ClutterMesh& mesh : meshes)
			for (uint32 k = 0; k < RendererVKLayout::CLUTTER_LODS; ++k)
			{
				w.vec(mesh.vertices[k]);
				w.vec(mesh.indices[k]);
			}
		writeCacheFile(path, MeshCacheClutter, CLUTTER_MESH_CACHE_VERSION, hash, w.data);
	}
}
