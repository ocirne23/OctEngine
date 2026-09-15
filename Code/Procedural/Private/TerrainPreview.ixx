export module Procedural:TerrainPreview;

import Core;
import Core.Log;
import Core.glm;
import Threading;
import File;
import :TerrainSampler;
import :GeneratorV3;

export namespace Procedural
{
	// The lobby's WORLD PREVIEW: one overview image of a seed, sampled from the diffusion generator's
	// COARSE stage alone (ESampleDetail::Coarse - one texel per coarse pixel, 256 native pixels), so a
	// map spanning the whole seed's continents costs a handful of coarse tiles and seconds, not the
	// thousands of full-detail tiles the mesh would need for the same area. The player picks a spot on
	// it; TerrainConfigV3::originX/Z then put the engine's origin there (see TerrainStreamer's "Origin"
	// tweaks), so the full-detail world the streamer builds is the spot the preview showed.
	//
	// Owned by the App's session (a stack local in main), not a global: it holds a job and a generator,
	// nothing an entity destructor reaches, and its dtor cancels + joins before the job system goes.
	// Main thread: request() / update() / the accessors. The bake itself is ONE Low job that fills the
	// grid in row bands (a cancel lands within one band's tile fetches) and colours it.
	//
	// SHARED RUNTIME: the diffusion runtime holds ONE seed for the process, and constructing a
	// TerrainGenV3 reseeds it. So a preview of a different seed than the streamer's live world would
	// pull the world's tiles out from under it - the session disables the terrain before previewing a
	// new seed, and only seeds the world with the seed the preview showed.
	class TerrainPreview
	{
	public:
		enum class EState : uint8 { Idle, LoadingModels, Generating, Ready, Failed };

		// The finished map: RGBA8 texels, row-major, row 0 = the most negative Z. Immutable once
		// published (consumers copy the shared_ptr, never the pixels).
		struct Image
		{
			uint32 width = 0;
			uint32 height = 0;
			uint32 generation = 0; // increments per published image, so a consumer knows to re-upload
			oc::vector<uint32> rgba;
			double texelWorldSize = 0.0; // world metres per texel (coarse pixel size at this world scale)
			double tileWorldSize = 0.0;  // world metres per FULL-detail tile (what seeding generates)
			uint32 seed = 0;
		};

		// Texels per side = coarse pixels per side: 256 is 4 coarse tiles across (5 with the halo), ~33 km
		// at the clamped 0.5 m/px - room for the largest playable area many times over, at a quarter
		// of the coarse inference a 512 map cost. The page scales it up to its 512 px slot.
		static constexpr uint32 c_resolution = 256;

		TerrainPreview() = default;
		~TerrainPreview() { cancel(); Globals::jobSystem.wait(m_counter); }
		TerrainPreview(const TerrainPreview&) = delete;
		TerrainPreview& operator=(const TerrainPreview&) = delete;

		// Start (or restart) a preview of `seed` at the given world scale. A bake in flight is cancelled
		// and the new one starts once it has drained (its generator must not be reseeded underneath it).
		void request(uint32 seed, float metersPerPixel)
		{
			m_seed = seed;
			m_metersPerPixel = metersPerPixel;
			m_requestPending = true;
			cancel();
			m_generator = nullptr; // rebuilt for the new seed once the old bake has drained (see update)
			m_state = EState::LoadingModels;
			m_image = nullptr;
			m_statusText.clear();
		}

		// Back to nothing (exit-to-menu): drops the image; a bake in flight is cancelled and drains.
		void reset()
		{
			cancel();
			m_requestPending = false;
			m_state = EState::Idle;
			m_image = nullptr;
			m_statusText.clear();
		}

		// Main thread, every frame the lobby is up: drives the model-load wait, the kick and the publish.
		void update()
		{
			if (m_bake && m_counter.isDone())
			{
				oc::shared_ptr<Bake> bake = oc::move(m_bake);
				m_bake = nullptr;
				if (!bake->cancel.load(oc::memory_order_relaxed) && bake->result)
				{
					m_image = oc::move(bake->result);
					m_state = EState::Ready;
				}
			}
			if (!m_requestPending || m_bake)
				return;

			if (!m_generator)
			{
				// Constructing the generator kicks the model load (and probes the assets on this thread).
				FileSystem::AllowMainThreadIO probeIo;
				TerrainConfigV3 cfg;
				cfg.seed = m_seed;
				cfg.metersPerPixel = m_metersPerPixel;
				m_generator = oc::make_shared<const TerrainGenV3>(cfg);
			}
			if (TerrainGenV3::hasFailed())
			{
				m_requestPending = false;
				m_state = EState::Failed;
				m_statusText = TerrainGenV3::statusText();
				return;
			}
			if (!TerrainGenV3::canGenerate()) // the weights, or the caches when loading is switched off
			{
				m_statusText = TerrainGenV3::statusText();
				return;
			}
			m_requestPending = false;
			kick();
		}

		EState state() const { return m_state; }
		// Rows sampled over rows total while generating; 1 once ready.
		float progress() const
		{
			if (m_state == EState::Ready)
				return 1.0f;
			if (!m_bake)
				return 0.0f;
			return (float)m_bake->rowsDone.load(oc::memory_order_relaxed) / (float)c_resolution;
		}
		const oc::string& statusText() const { return m_statusText; }
		uint32 seed() const { return m_seed; }
		oc::shared_ptr<const Image> image() const { return m_image; }

		// A normalized pick on the image (u right, v down, both 0..1) -> the world-metre offset the
		// engine origin must take (TerrainConfigV3::originX/Z) for that spot to become (0, 0).
		glm::vec2 worldOffsetAt(float u, float v) const
		{
			if (!m_image)
				return glm::vec2(0.0f);
			const double extent = m_image->texelWorldSize * (double)m_image->width;
			return glm::vec2((float)(((double)u - 0.5) * extent), (float)(((double)v - 0.5) * extent));
		}

	private:
		struct Bake
		{
			oc::shared_ptr<const TerrainGenV3> generator;
			uint32 seed = 0;
			uint32 generation = 0;
			double step = 0.0;   // world metres per texel
			double origin = 0.0; // world coordinate of texel 0 on both axes
			oc::atomic<bool> cancel{ false };
			oc::atomic<uint32> rowsDone{ 0 };
			oc::vector<TerrainPoint> points;
			oc::shared_ptr<Image> result;
		};

		void cancel()
		{
			if (m_bake)
				m_bake->cancel.store(true, oc::memory_order_relaxed);
		}

		void kick()
		{
			const int32 npc = TerrainGenV3::nativePerCoarsePixel();
			const float mpp = m_generator->config().metersPerPixel; // the generator's clamped value
			auto bake = oc::make_shared<Bake>();
			bake->generator = m_generator;
			bake->seed = m_seed;
			bake->generation = ++m_generation;
			bake->step = (double)npc * (double)mpp;
			bake->origin = -0.5 * bake->step * (double)c_resolution + 0.5 * bake->step;
			bake->points.resize((size_t)c_resolution * c_resolution);
			m_bake = bake;
			m_state = EState::Generating;
			m_statusText.clear();

			Globals::jobSystem.submit([bake]()
			{
				const uint32 res = c_resolution;
				const uint32 band = 32; // rows per sampleGrid call: a band touches at most two coarse tile rows
				const TerrainGenV3& gen = *bake->generator;
				for (uint32 r0 = 0; r0 < res; r0 += band)
				{
					if (bake->cancel.load(oc::memory_order_relaxed))
						return;
					const uint32 rows = oc::min(band, res - r0);
					gen.sampleGrid(bake->origin, bake->origin + bake->step * (double)r0, bake->step, res, rows,
						oc::span<TerrainPoint>(bake->points.data() + (size_t)r0 * res, (size_t)rows * res),
						ESampleDetail::Coarse);
					bake->rowsDone.store(r0 + rows, oc::memory_order_relaxed);
				}
				if (bake->cancel.load(oc::memory_order_relaxed))
					return;
				auto image = oc::make_shared<Image>();
				image->width = res;
				image->height = res;
				image->generation = bake->generation;
				image->texelWorldSize = bake->step;
				image->tileWorldSize = (double)TerrainGenV3::fullTilePixels() * (double)gen.config().metersPerPixel;
				image->seed = bake->seed;
				colourise(gen, bake->points, res, image->rgba);
				bake->result = oc::move(image);
			}, { "TerrainPreview::bake", EProfileCategory::Procedural }, EJobPriority::Low, &m_counter);
		}

		// Height tints by climate, sea by depth, a hillshade from the coarse elevation gradient, and a
		// one-texel coastline. Heights are converted back to MODEL metres (the real-world scale the model
		// was trained at) so the same elevation reads the same colour at every world compression.
		static void colourise(const TerrainGenV3& gen, const oc::vector<TerrainPoint>& points, uint32 res, oc::vector<uint32>& out)
		{
			const TerrainConfigV3& cfg = gen.config();
			const float vs = glm::max(TerrainGenV3::worldScale(cfg.metersPerPixel) * cfg.heightScale, 1e-6f);
			const float invVs = 1.0f / vs;
			// The coarse texel in real metres: native resolution x native pixels per coarse pixel.
			const float texelRealM = glm::max(cfg.metersPerPixel / TerrainGenV3::worldScale(cfg.metersPerPixel), 1.0f)
				* (float)TerrainGenV3::nativePerCoarsePixel();
			const glm::vec3 light = glm::normalize(glm::vec3(-0.5f, 0.75f, -0.5f));

			const auto elevAt = [&](int x, int y) -> float
			{
				x = glm::clamp(x, 0, (int)res - 1);
				y = glm::clamp(y, 0, (int)res - 1);
				const TerrainPoint& p = points[(size_t)y * res + x];
				return (p.height - p.waterLevel) * invVs;
			};

			out.resize((size_t)res * res);
			for (uint32 y = 0; y < res; ++y)
			{
				for (uint32 x = 0; x < res; ++x)
				{
					const TerrainPoint& p = points[(size_t)y * res + x];
					const float e = elevAt((int)x, (int)y); // model metres above the water
					glm::vec3 c;
					if (e <= 0.0f)
					{
						const float t = glm::clamp(-e / 3000.0f, 0.0f, 1.0f);
						c = glm::mix(glm::vec3(0.22f, 0.48f, 0.72f), glm::vec3(0.02f, 0.08f, 0.26f), std::sqrt(t));
						// coastline: a water texel with land beside it
						if (elevAt((int)x - 1, (int)y) > 0.0f || elevAt((int)x + 1, (int)y) > 0.0f
							|| elevAt((int)x, (int)y - 1) > 0.0f || elevAt((int)x, (int)y + 1) > 0.0f)
							c *= 0.55f;
					}
					else
					{
						const float h = glm::clamp(p.humidity, 0.0f, 1.0f);
						const glm::vec3 arid(0.78f, 0.68f, 0.46f);
						const glm::vec3 temperate(0.46f, 0.60f, 0.30f);
						const glm::vec3 lush(0.16f, 0.42f, 0.18f);
						c = h < 0.5f ? glm::mix(arid, temperate, h * 2.0f) : glm::mix(temperate, lush, (h - 0.5f) * 2.0f);
						const float rock = glm::clamp((e - 1500.0f) / 2500.0f, 0.0f, 1.0f);
						c = glm::mix(c, glm::vec3(0.52f, 0.47f, 0.44f), rock);
						const float snow = glm::clamp(-p.temperature / 10.0f, 0.0f, 1.0f);
						c = glm::mix(c, glm::vec3(0.92f, 0.94f, 0.97f), snow);
						// hillshade, relief exaggerated: a 3 km peak over a 7.68 km texel is a gentle slope
						const float dx = (elevAt((int)x + 1, (int)y) - elevAt((int)x - 1, (int)y)) * 4.0f / (2.0f * texelRealM);
						const float dz = (elevAt((int)x, (int)y + 1) - elevAt((int)x, (int)y - 1)) * 4.0f / (2.0f * texelRealM);
						const glm::vec3 n = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
						c *= 0.55f + 0.45f * glm::clamp(glm::dot(n, light), 0.0f, 1.0f) * 1.3f;
					}
					c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
					const uint32 r = (uint32)(c.x * 255.0f + 0.5f), g = (uint32)(c.y * 255.0f + 0.5f), b = (uint32)(c.z * 255.0f + 0.5f);
					out[(size_t)y * res + x] = r | (g << 8) | (b << 16) | 0xFF000000u; // ImGui's RGBA32 byte order
				}
			}
		}

		uint32 m_seed = 516121; // the streamer's default seed, so the page's field starts on it
		float m_metersPerPixel = 0.3f;
		bool m_requestPending = false;
		uint32 m_generation = 0;
		EState m_state = EState::Idle;
		oc::string m_statusText;
		oc::shared_ptr<const TerrainGenV3> m_generator;
		oc::shared_ptr<Bake> m_bake;
		JobCounter m_counter;
		oc::shared_ptr<const Image> m_image;
	};

	// The lobby's "Seed world": PRE-GENERATES the full-detail diffusion tiles of the playable area
	// around the pick, nearest-first, on ONE Low job - the ~1.5 s per cold tile is the whole cost of a
	// new world; the streamer's mesh builds are cheap and run live against the tiles as they land (its
	// pumps park on the same per-tile events, so it streams in while this runs). A tile already in the
	// disk cache (Local/Diffusion/<seed>/) is loaded, not regenerated - a previously seeded area comes
	// back in seconds. Same ownership and threading contract as TerrainPreview; the same one-seed
	// runtime rule applies (the seed must be the preview's).
	class TerrainSeeder
	{
	public:
		enum class EState : uint8 { Idle, LoadingModels, Generating, Done, Failed };

		TerrainSeeder() = default;
		~TerrainSeeder() { cancel(); Globals::jobSystem.wait(m_counter); }
		TerrainSeeder(const TerrainSeeder&) = delete;
		TerrainSeeder& operator=(const TerrainSeeder&) = delete;

		// origin = the engine origin's model-space offset (TerrainConfigV3::originX/Z, what the
		// streamer runs with); halfSizeM = half the playable area's side, engine metres. The tiles
		// covering [-half, half]^2 around the origin are generated.
		void start(uint32 seed, float metersPerPixel, glm::vec2 origin, float halfSizeM)
		{
			m_seed = seed;
			m_metersPerPixel = metersPerPixel;
			m_origin = origin;
			m_halfSize = halfSizeM;
			m_requestPending = true;
			cancel(); // an old job keeps its own generator handle; ours is rebuilt for the new placement
			m_state = EState::LoadingModels;
			m_statusText.clear();

			// Built HERE, not when the job kicks: the covered rect is lattice arithmetic on the config
			// (no models needed) and the session wants it at once - it is the streamer's generated
			// bounds. Constructing also (re)kicks the model load, which the job below needs anyway.
			FileSystem::AllowMainThreadIO probeIo;
			TerrainConfigV3 cfg;
			cfg.seed = m_seed;
			cfg.metersPerPixel = m_metersPerPixel;
			cfg.originX = m_origin.x;
			cfg.originZ = m_origin.y;
			m_generator = oc::make_shared<const TerrainGenV3>(cfg);
			m_generator->fullTileRange(-m_halfSize, -m_halfSize, m_halfSize, m_halfSize, m_ti0, m_tj0, m_ti1, m_tj1);
			double x0, z0, x1, z1, x0b, z0b, x1b, z1b;
			m_generator->fullTileWorldRect(m_ti0, m_tj0, x0, z0, x1b, z1b);
			m_generator->fullTileWorldRect(m_ti1, m_tj1, x0b, z0b, x1, z1);
			m_coveredEngineMin = glm::vec2((float)x0, (float)z0);
			m_coveredEngineMax = glm::vec2((float)x1, (float)z1);
			m_coveredMin = m_coveredEngineMin + m_origin; // engine -> model space
			m_coveredMax = m_coveredEngineMax + m_origin;
			m_hasCoverage = true;
		}

		void reset()
		{
			cancel();
			m_requestPending = false;
			m_state = EState::Idle;
			m_hasCoverage = false;
			m_statusText.clear();
		}

		void update()
		{
			if (m_job && m_counter.isDone())
			{
				oc::shared_ptr<Job> job = oc::move(m_job);
				m_job = nullptr;
				if (!job->cancel.load(oc::memory_order_relaxed))
				{
					m_state = EState::Done;
					m_lastTotal = job->total;
					m_lastDone = job->done.load(oc::memory_order_relaxed);
					m_lastCached = job->cached.load(oc::memory_order_relaxed);
				}
			}
			if (!m_requestPending || m_job || !m_generator)
				return;
			if (TerrainGenV3::hasFailed())
			{
				m_requestPending = false;
				m_state = EState::Failed;
				m_statusText = TerrainGenV3::statusText();
				return;
			}
			if (!TerrainGenV3::canGenerate()) // the weights, or the caches when loading is switched off
			{
				m_statusText = TerrainGenV3::statusText();
				return;
			}
			m_requestPending = false;
			kick();
		}

		EState state() const { return m_state; }
		float progress() const
		{
			if (m_state == EState::Done)
				return 1.0f;
			if (!m_job || m_job->total == 0)
				return 0.0f;
			return (float)m_job->done.load(oc::memory_order_relaxed) / (float)m_job->total;
		}
		uint32 tilesTotal() const { return m_job ? m_job->total : m_lastTotal; }
		uint32 tilesDone() const { return m_job ? m_job->done.load(oc::memory_order_relaxed) : m_lastDone; }
		// How many of the area's tiles were already on disk (known once the job scanned them).
		uint32 tilesCached() const { return m_job ? m_job->cached.load(oc::memory_order_relaxed) : m_lastCached; }
		const oc::string& statusText() const { return m_statusText; }
		// The MODEL-space rect the generated tiles cover (tile-aligned, so a little larger than the
		// requested area) - the square the preview map draws. False until start() was processed.
		bool coverage(glm::vec2& outMin, glm::vec2& outMax) const
		{
			outMin = m_coveredMin;
			outMax = m_coveredMax;
			return m_hasCoverage;
		}
		// The same rect in ENGINE space (after the origin): the streamer's generated bounds.
		bool engineCoverage(glm::vec2& outMin, glm::vec2& outMax) const
		{
			outMin = m_coveredEngineMin;
			outMax = m_coveredEngineMax;
			return m_hasCoverage;
		}

	private:
		struct Job
		{
			oc::shared_ptr<const TerrainGenV3> generator;
			oc::vector<glm::ivec2> tiles; // (ti, tj), nearest the centre first
			uint32 total = 0;
			oc::atomic<uint32> done{ 0 };
			oc::atomic<uint32> cached{ 0 };
			oc::atomic<bool> cancel{ false };
		};

		void cancel()
		{
			if (m_job)
				m_job->cancel.store(true, oc::memory_order_relaxed);
		}

		void kick()
		{
			auto job = oc::make_shared<Job>();
			job->generator = m_generator;
			for (int32 ti = m_ti0; ti <= m_ti1; ++ti)
				for (int32 tj = m_tj0; tj <= m_tj1; ++tj)
					job->tiles.push_back(glm::ivec2(ti, tj));
			// Nearest the origin first: the streamer's own nearest-first mesh order lands on tiles
			// that exist, and a cancelled seeding leaves the useful ones.
			const glm::vec2 centre(((float)m_ti0 + (float)m_ti1) * 0.5f, ((float)m_tj0 + (float)m_tj1) * 0.5f);
			oc::sort(job->tiles.begin(), job->tiles.end(), [centre](glm::ivec2 a, glm::ivec2 b)
			{
				return glm::distance(glm::vec2(a), centre) < glm::distance(glm::vec2(b), centre);
			});
			job->total = (uint32)job->tiles.size();
			m_job = job;
			m_state = EState::Generating;
			m_statusText.clear();

			Globals::jobSystem.submit([job]()
			{
				const TerrainGenV3& gen = *job->generator;
				uint32 cached = 0;
				for (const glm::ivec2 t : job->tiles)
					cached += gen.isFullTileCached(t.x, t.y) ? 1u : 0u;
				job->cached.store(cached, oc::memory_order_relaxed);
				for (const glm::ivec2 t : job->tiles)
				{
					if (job->cancel.load(oc::memory_order_relaxed))
						return;
					gen.prefetchFullTile(t.x, t.y); // disk hit = a read; miss = ~1.5 s of inference, fiber parked
					job->done.fetch_add(1, oc::memory_order_relaxed);
					Globals::jobSystem.preemptionPoint();
				}
			}, { "TerrainSeeder::generate", EProfileCategory::Procedural }, EJobPriority::Low, &m_counter);
		}

		uint32 m_seed = 0;
		float m_metersPerPixel = 0.3f;
		glm::vec2 m_origin = glm::vec2(0.0f);
		float m_halfSize = 512.0f;
		bool m_requestPending = false;
		EState m_state = EState::Idle;
		oc::string m_statusText;
		oc::shared_ptr<const TerrainGenV3> m_generator;
		int32 m_ti0 = 0, m_tj0 = 0, m_ti1 = 0, m_tj1 = 0;
		bool m_hasCoverage = false;
		glm::vec2 m_coveredMin = glm::vec2(0.0f);       // model space
		glm::vec2 m_coveredMax = glm::vec2(0.0f);
		glm::vec2 m_coveredEngineMin = glm::vec2(0.0f); // engine space
		glm::vec2 m_coveredEngineMax = glm::vec2(0.0f);
		oc::shared_ptr<Job> m_job;
		JobCounter m_counter;
		uint32 m_lastTotal = 0, m_lastDone = 0, m_lastCached = 0;
	};
}
