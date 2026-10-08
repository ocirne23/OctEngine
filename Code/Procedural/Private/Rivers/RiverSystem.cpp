module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import RendererVK;
import Threading;
import Settings;

import :GeneratorV3;
import :RiverRouting;
import :RiverNetwork;
import :RiverUnits;
import :RiverSystem;

namespace
{
	uint64 unitKey(int32 ui, int32 uj) { return ((uint64)(uint32)ui << 32) | (uint64)(uint32)uj; }

	// The renderer's packed 0xAABBGGRR.
	uint32 packColor(glm::vec3 c)
	{
		c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
		const uint32 r = (uint32)(c.x * 255.0f + 0.5f), g = (uint32)(c.y * 255.0f + 0.5f), b = (uint32)(c.z * 255.0f + 0.5f);
		return 0xFF000000u | (b << 16) | (g << 8) | r;
	}
}

namespace Procedural
{
	RiverSystem::~RiverSystem()
	{
		if (m_job)
			m_job->cancel.store(true, oc::memory_order_relaxed);
		Globals::jobSystem.wait(m_counter);
	}

	void RiverSystem::setGenerator(oc::shared_ptr<const TerrainGenV3> generator)
	{
		if (generator == m_generator)
			return;
		m_generator = oc::move(generator);
		m_network = nullptr;
		dropUnits();
	}

	void RiverSystem::dropUnits()
	{
		if (m_job)
			m_job->cancel.store(true, oc::memory_order_relaxed); // it drains; its result is dropped by the generation
		m_units.clear();
		++m_generation;
	}

	float RiverSystem::unitDistance(int32 ui, int32 uj, glm::vec2 camera) const
	{
		const TerrainConfigV3& gc = m_generator->config();
		const float size = (float)(oc::clamp(m_unitCfg.unitTiles, 1, 8) * TerrainGenV3::fullTilePixels()) * gc.metersPerPixel;
		const glm::vec2 lo((float)uj * size - gc.originX, (float)ui * size - gc.originZ);
		const glm::vec2 d = glm::max(glm::max(lo - camera, camera - (lo + glm::vec2(size))), glm::vec2(0.0f));
		return glm::length(d);
	}

	void RiverSystem::update(Renderer& renderer, const Camera& camera)
	{
		// The job's result first: adopted only if nothing changed while it ran.
		if (m_job && m_counter.isDone())
		{
			oc::shared_ptr<Job> job = oc::move(m_job);
			m_job = nullptr;
			if (job->generation == m_generation && job->result && !job->cancel.load(oc::memory_order_relaxed))
			{
				Resident& r = m_units[unitKey(job->ui, job->uj)];
				r.unit = job->result;
				buildLines(r);
			}
		}

		const TerrainSettings& s = Globals::settings.terrain;
		if (!m_generator || !s.riverDebugLines || !TerrainGenV3::isReady())
			return;

		const RiverConfig rc = riverConfigFromSettings(s);
		const RiverUnitConfig uc = riverUnitConfigFromSettings(s);
		if (!m_network || rc != m_coarseCfg)
		{
			m_coarseCfg = rc;
			m_network = oc::make_shared<const CoarseRiverNetwork>(m_generator, rc);
			dropUnits();
		}
		if (uc != m_unitCfg)
		{
			m_unitCfg = uc;
			dropUnits();
		}

		const TerrainConfigV3& gc = m_generator->config();
		const glm::vec2 cam(camera.position.x, camera.position.z);
		const float radius = glm::max(s.riverDebugRadius, 1.0f);
		const int32 unitPx = oc::clamp(m_unitCfg.unitTiles, 1, 8) * TerrainGenV3::fullTilePixels();
		const float unitM = (float)unitPx * gc.metersPerPixel;
		const int32 cu = (int32)std::floor((cam.x + gc.originX) / unitM);
		const int32 cv = (int32)std::floor((cam.y + gc.originZ) / unitM);
		const int32 reach = (int32)std::ceil(radius / unitM) + 1;

		// Evict the far ones (a margin past the radius, so turning on the spot does not rebuild).
		for (auto it = m_units.begin(); it != m_units.end();)
		{
			const int32 ui = (int32)(it->first >> 32), uj = (int32)(uint32)(it->first & 0xFFFFFFFFu);
			if (unitDistance(ui, uj, cam) > radius * 1.5f + unitM)
				it = m_units.erase(it);
			else
				++it;
		}

		// Kick the nearest missing unit.
		if (!m_job)
		{
			float bestDist = 0.0f;
			int32 bestI = 0, bestJ = 0;
			bool found = false;
			for (int32 i = cv - reach; i <= cv + reach; i++)
				for (int32 j = cu - reach; j <= cu + reach; j++)
				{
					const float dist = unitDistance(i, j, cam);
					if (dist > radius || m_units.count(unitKey(i, j)))
						continue;
					if (!found || dist < bestDist)
					{
						found = true;
						bestDist = dist;
						bestI = i;
						bestJ = j;
					}
				}
			if (found)
			{
				auto job = oc::make_shared<Job>();
				job->generator = m_generator;
				job->network = m_network;
				job->cfg = m_unitCfg;
				job->ui = bestI;
				job->uj = bestJ;
				job->generation = m_generation;
				m_job = job;
				Globals::jobSystem.submit([job]()
				{
					job->result = buildRiverUnit(*job->generator, *job->network, job->cfg, job->ui, job->uj, &job->cancel);
				}, { "RiverSystem::buildUnit", EProfileCategory::Procedural }, EJobPriority::Low, &m_counter);
			}
		}

		if (!renderer.isInitialized())
			return;
		for (const auto& [key, r] : m_units)
		{
			const int32 ui = (int32)(key >> 32), uj = (int32)(uint32)(key & 0xFFFFFFFFu);
			if (unitDistance(ui, uj, cam) > radius)
				continue;
			for (const Line& l : r.lines)
				renderer.addDebugLine(l.a, l.b, l.color);
		}
	}

	void RiverSystem::buildLines(Resident& res) const
	{
		const RiverUnit& u = *res.unit;
		res.lines.clear();
		if (u.empty)
			return;
		const TerrainConfigV3& gc = m_generator->config();
		const double mpp = (double)gc.metersPerPixel;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		const double px0 = (double)u.uj * (double)(u.tiles * TerrainGenV3::fullTilePixels());
		const double pz0 = (double)u.ui * (double)(u.tiles * TerrainGenV3::fullTilePixels());
		constexpr float c_lift = 0.5f; // engine m above the water, so the line is not inside the ground
		const auto world = [&](float x, float z, float water)
		{
			return glm::vec3((float)((px0 + (double)x) * mpp - (double)gc.originX),
			                 gc.seaLevel + water * vs + c_lift,
			                 (float)((pz0 + (double)z) * mpp - (double)gc.originZ));
		};
		const float perennialQ = glm::max(m_unitCfg.perennialQ, 1e-3f);

		for (const RiverSegment& seg : u.segments)
			for (uint32 k = 0; k + 1 < seg.count; k++)
			{
				const RiverPoint& a = u.points[seg.first + k];
				const RiverPoint& b = u.points[seg.first + k + 1];
				glm::vec3 c;
				if (a.flags & RiverPoint_Fall)
					c = glm::vec3(1.0f, 0.1f, 0.1f);
				else if (a.flags & RiverPoint_Rapids)
					c = glm::vec3(1.0f, 0.55f, 0.0f);
				else if (seg.ephemeral)
					c = glm::vec3(0.70f, 0.55f, 0.35f);
				else
				{
					const float t = glm::clamp(std::log10(glm::max(a.q, 1e-6f) / perennialQ) / 2.5f, 0.0f, 1.0f);
					c = glm::mix(glm::vec3(0.40f, 0.70f, 1.0f), glm::vec3(0.05f, 0.15f, 0.85f), t);
				}
				res.lines.push_back(Line{ world(a.x, a.z, a.water), world(b.x, b.z, b.water), packColor(c) });
			}

		for (const RiverCrossing& x : u.crossings)
		{
			const glm::vec3 p = world(x.x, x.z, x.water);
			const uint32 color = packColor(x.inlet ? glm::vec3(0.1f, 1.0f, 0.2f) : glm::vec3(1.0f, 0.1f, 1.0f));
			res.lines.push_back(Line{ p, p + glm::vec3(0.0f, 20.0f, 0.0f), color });
		}

		for (const RiverLakeRun& run : u.lakeRuns)
		{
			const RiverLake& lake = u.lakes[run.lake];
			glm::vec3 c;
			switch (lake.kind)
			{
			case ERiverWater::TerminalLake: c = glm::vec3(0.30f, 0.65f, 0.65f); break;
			case ERiverWater::Pan:          c = glm::vec3(0.92f, 0.90f, 0.84f); break;
			default:                        c = glm::vec3(0.0f, 0.75f, 1.0f); break;
			}
			const float x0 = (float)run.x0 - 0.5f, x1 = (float)(run.x0 + run.len) - 0.5f;
			res.lines.push_back(Line{ world(x0, (float)run.row, lake.level), world(x1, (float)run.row, lake.level), packColor(c) });
		}
	}
}
