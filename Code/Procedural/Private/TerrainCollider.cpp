module Procedural;

import Core;
import Core.glm;
import Core.Tweaks;
import Core.Transform;

import Physics;

import :TerrainCollider;
import :TerrainSampler;

namespace Procedural
{
	static uint64 tileKey(glm::ivec2 coord)
	{
		return (uint64)(uint32)coord.x << 32 | (uint64)(uint32)coord.y;
	}

	void TerrainCollider::initialize(void* terrainUserData)
	{
		m_terrainUserData = terrainUserData;
		const auto dirty = [this]() { m_configDirty = true; };
		Tweak::boolean("Terrain/Collision", "Enabled", &m_enabled);
		Tweak::floatVar("Terrain/Collision", "Radius", &m_radius, 16.0f, 512.0f, 1.0f);
		Tweak::floatVar("Terrain/Collision", "Tile size", &m_tileSize, 8.0f, 128.0f, 1.0f, dirty);
		// 1 m matches the render LOD0 lattice (chunkSize / lod0Res), so collision == drawn surface.
		// Coarser trades fidelity for memory; finer buys nothing the render mesh shows.
		Tweak::floatVar("Terrain/Collision", "Spacing", &m_spacing, 0.25f, 8.0f, 0.05f, dirty);
		Tweak::floatVar("Terrain/Collision", "Friction", &m_friction, 0.0f, 2.0f, 0.01f, dirty);
	}

	void TerrainCollider::update(const glm::vec3& focusPos, oc::shared_ptr<const ITerrainSampler> maps)
	{
		ProfileScope profileScope("Terrain collider", EProfileCategory::Procedural);
		// Drain the in-flight build FIRST: even on a reset frame the result must be consumed, and a
		// completed tile should land the same frame it finishes.
		BuildResult done;
		bool haveDone = false;
		if (m_buildInFlight && m_buildCounter.isDone())
		{
			done = oc::move(m_buildResult);
			m_buildInFlight = false;
			haveDone = true;
		}

		const bool active = m_enabled && maps != nullptr && Globals::physics.isInitialized();
		if (!active || maps.get() != m_mapsIdentity || m_configDirty)
		{
			m_tiles.clear();
			++m_generation; // in-flight/finished results built against the old world are stale now
			m_configDirty = false;
			m_mapsIdentity = active ? maps.get() : nullptr;
			if (!active)
				return;
		}

		const float tileSize = glm::clamp(m_tileSize, 8.0f, 128.0f);
		const float radius = glm::max(m_radius, tileSize);
		const glm::vec2 focus(focusPos.x, focusPos.z);

		// Distance from the focus to a tile's XZ footprint, per axis (0 inside).
		const auto tileDist = [&](glm::ivec2 coord) -> glm::vec2
		{
			const glm::vec2 lo = glm::vec2(coord) * tileSize;
			return glm::max(glm::max(lo - focus, focus - (lo + tileSize)), glm::vec2(0.0f));
		};

		// --- Adopt the finished build: create the static body at the tile origin (geometry is
		// tile-local in XZ with world Y, like render chunks — keeps float precision high).
		if (haveDone && done.generation == m_generation && done.mesh.isValid() && !m_tiles.contains(done.key))
		{
			PhysicsBodyDesc desc;
			desc.userData = m_terrainUserData;
			desc.type = EPhysicsBodyType::Static;
			desc.transform = Transform(
				glm::vec3((float)done.coord.x * tileSize, 0.0f, (float)done.coord.y * tileSize),
				1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

			PhysicsShape shape;
			shape.type = EPhysicsShapeType::Mesh;
			shape.mesh = &done.mesh; // box3d copies the b3MeshData* out; moving the wrapper after is fine
			shape.friction = m_friction;
			shape.categoryBits = PhysicsLayers::bit("Terrain");

			Tile tile;
			tile.body = Globals::physics.createBody(desc, { &shape, 1 });
			tile.mesh = oc::move(done.mesh);
			tile.coord = done.coord;
			m_tiles.emplace(done.key, oc::move(tile));
		}

		// --- Evict tiles that left the radius, with half a tile of hysteresis so the boundary
		// doesn't thrash builds as the focus wanders. A just-adopted stale tile evicts here too.
		for (auto it = m_tiles.begin(); it != m_tiles.end(); )
		{
			const glm::vec2 d = tileDist(it->second.coord);
			if (glm::max(d.x, d.y) > radius + tileSize * 0.5f)
				it = m_tiles.erase(it);
			else
				++it;
		}

		// --- Launch the nearest missing tile (one build in flight at a time; a tile is a few ms of
		// sampleGrid + BVH warm, but can wait on V3 tile inference — a Low job that parks its fiber).
		if (m_buildInFlight)
			return;

		const int minX = (int)std::floor((focus.x - radius) / tileSize);
		const int maxX = (int)std::floor((focus.x + radius) / tileSize);
		const int minZ = (int)std::floor((focus.y - radius) / tileSize);
		const int maxZ = (int)std::floor((focus.y + radius) / tileSize);
		glm::ivec2 bestCoord{ 0, 0 };
		float bestDist2 = FLT_MAX;
		for (int tz = minZ; tz <= maxZ; ++tz)
		{
			for (int tx = minX; tx <= maxX; ++tx)
			{
				const glm::ivec2 coord(tx, tz);
				if (m_tiles.contains(tileKey(coord)))
					continue;
				const glm::vec2 d = tileDist(coord);
				if (glm::max(d.x, d.y) > radius) // square scan, disk membership
					continue;
				const glm::vec2 toCenter = glm::vec2(coord) * tileSize + tileSize * 0.5f - focus;
				const float dist2 = glm::dot(toCenter, toCenter);
				if (dist2 < bestDist2)
				{
					bestDist2 = dist2;
					bestCoord = coord;
				}
			}
		}
		if (bestDist2 == FLT_MAX)
			return; // every wanted tile is resident

		const float spacing = glm::clamp(m_spacing, 0.25f, tileSize);
		const uint32 res = glm::clamp((uint32)std::lround(tileSize / spacing), 1u, 512u);
		const uint32 generation = m_generation;
		const uint64 key = tileKey(bestCoord);
		m_buildInFlight = true;
		Globals::jobSystem.submit([this, maps, coord = bestCoord, key, generation, tileSize, res]()
		{
			ProfileScope profileScope("TerrainCollider::update", EProfileCategory::Procedural);

			BuildResult out;
			out.key = key;
			out.coord = coord;
			out.generation = generation;

			// Sample on the same world lattice generateChunk uses (tile origins are multiples of the
			// spacing), one grid call — see ITerrainSampler::sampleGrid.
			const uint32 vpr = res + 1;
			const double step = (double)tileSize / (double)res;
			const double ox = (double)coord.x * (double)tileSize;
			const double oz = (double)coord.y * (double)tileSize;
			oc::vector<TerrainPoint> field((size_t)vpr * vpr);
			maps->sampleGrid(ox, oz, step, vpr, vpr, field);

			oc::vector<glm::vec3> positions;
			positions.reserve((size_t)vpr * vpr);
			for (uint32 row = 0; row < vpr; ++row)
				for (uint32 col = 0; col < vpr; ++col)
					positions.push_back({ (float)((double)col * step), field[(size_t)row * vpr + col].height, (float)((double)row * step) });

			// generateChunk's triangulation exactly, so the contact surface IS the drawn surface.
			oc::vector<uint32> indices;
			indices.reserve((size_t)res * res * 6);
			for (uint32 row = 0; row < res; ++row)
			{
				for (uint32 col = 0; col < res; ++col)
				{
					const uint32 a = row * vpr + col;
					const uint32 b = a + 1;
					const uint32 c = (row + 1) * vpr + col;
					const uint32 d = c + 1;
					indices.push_back(a); indices.push_back(c); indices.push_back(b);
					indices.push_back(b); indices.push_back(c); indices.push_back(d);
				}
			}

			// Standalone BVH build (no world touched), safe off the main thread.
			out.mesh = Globals::physics.createCollisionMesh(positions, indices);
			m_buildResult = oc::move(out); // single producer; read only after the counter completes
		}, EJobPriority::Low, &m_buildCounter, "terrainColliderTile");
	}
}
