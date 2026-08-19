export module Nav:System;

import Core;
import Core.glm;
import Threading;
import :Grid;
import :Field;
import :Density;

// The navigation service: owns one published TeamField per team (rebuilt on Low-priority jobs
// whenever obstacles/sources change or the rebuild interval elapses, swapped in on the main thread
// between entity passes), plus per-team Flow and Pressure fields. The GAME feeds obstacles + sources every
// frame (it knows footprints and teams; this library knows nothing about entities); UNITS read
// teamField(otherTeam)->sample(), flow(ownTeam) and pressure(ownTeam) from the parallel entity pass.
//
// Thread contract: setObstacles/setTeamSources/update/drawDebug are MAIN THREAD, called outside
// the entity pass. teamField()/density() reads are worker-safe during the pass because published
// pointers only change inside update(). teamField()/flow()/pressure() reads are worker-safe likewise.
export namespace Nav
{
    constexpr uint32 MaxTeams = 8;

    class NavSystem final
    {
    public:

        ~NavSystem();

        void initialize(); // registers the "Nav" tweaks; JobSystem must be initialized
        bool isEnabled() const { return m_enabled; }

        void setObstacles(oc::span<const NavObstacle> obstacles);         // main; change-detected
        void setTeamSources(uint32 team, oc::span<const NavSource> sources); // main; every frame
        void update(float deltaSec);                                        // main; publish + kick + density flip

        const TeamField* teamField(uint32 team) const // may be null (no sources / not built yet)
        {
            return team < MaxTeams ? m_teams[team].published.get() : nullptr;
        }
        FlowField& flow(uint32 team) { return m_flow[glm::min(team, MaxTeams - 1)]; }
        PressureField& pressure(uint32 team) { return m_pressure[glm::min(team, MaxTeams - 1)]; }
        const PressureField& pressure(uint32 team) const { return m_pressure[glm::min(team, MaxTeams - 1)]; }
        const FlowField& flow(uint32 team) const { return m_flow[glm::min(team, MaxTeams - 1)]; }
        bool anyFieldPublished() const { return m_publishedCount > 0; }
        // Any published field — every field shares the obstacle raster, so this is THE raster for
        // lineOfSight/avoid when a walker has no distance field of its own (routes, locked orders).
        const TeamField* raster() const { return m_raster.published.get(); }
        // SEED PATH (main thread): plan a route from -> to with A* over the obstacle raster
        // (string-pulled) and WRITE it into `team`'s flow field as a lane of `speed` m/s, `width`
        // metres wide. Units following the crowd flow then take that route without any of them
        // planning; the lane decays like any other ("Nav/Flow decay"). false = no raster yet or no
        // path within the budget. `outPath` (optional) receives the planned polyline for debug draw.
        // `laneWidth` is how wide the lane is PAINTED (0 = one cell; the pressure push spreads it
        // further either way); `clearance` is the planning width — how much room the planned route
        // keeps from walls.
        bool seedPath(uint32 team, const glm::vec3& from, const glm::vec3& to, float speed,
            float laneWidth = 0.0f, float clearance = 2.0f, oc::vector<glm::vec2>* outPath = nullptr);
        // The RATE-LIMITED entry point (main thread) — EVERY unit asks on its own timer, this is
        // what makes that affordable: a request is refused when a plan of the same team was already
        // made within "Seed area" metres of BOTH its start and its destination inside the last
        // "Seed cooldown" seconds, and at most "Seed max/frame" plans run in a frame. A crowd
        // heading the same way therefore produces ONE path and the rest ride the lane it writes.
        // Proximity, not buckets: two units either side of a bucket border are one request.
        bool requestSeedPath(uint32 team, const glm::vec3& from, const glm::vec3& to, float speed,
            float laneWidth = 0.0f, float clearance = 2.0f);

        // GOAL fields: a single-destination field per KEY (the local player's move order, each
        // barracks route waypoint), the same TeamField with ONE source. Rebuilt when the
        // destination moves > half a cell or the obstacle raster changes; radius sized per goal.
        // setGoal every frame the goal is wanted — a key not set for GoalExpireFrames is dropped.
        // setGoal/clearGoal are main-thread; goalField() is worker-safe during the entity pass
        // (the key map only changes inside update()).
        static constexpr uint32 GoalExpireFrames = 60;
        static constexpr uint64 GoalKeyPlayer = 1;
        static uint64 goalKeyRoute(uint32 structureId, uint32 waypoint) { return (uint64(structureId) << 8) | (waypoint & 0xFF) | (1ull << 62); }
        void setGoal(uint64 key, const glm::vec3& dest, float radius);
        void clearGoal(uint64 key);
        const TeamField* goalField(uint64 key) const;

        // Debug lines (renderer-agnostic sink): mode 1 = chunk outlines + descent arrows of "Debug
        // team" near `focus`, 2 = crowd FLOW arrows + PRESSURE columns of "Debug team".
        void drawDebug(const glm::vec3& focus,
            const oc::function<void(const glm::vec3&, const glm::vec3&, uint32)>& line) const;
        int debugMode() const { return m_debugMode; }

        void clear(); // drop every field/density chunk (game teardown / reload)

    private:

        struct TeamSlot
        {
            oc::vector<NavSource> sources;      // latest from the game
            oc::vector<NavSource> buildSources; // snapshot the in-flight job reads
            oc::shared_ptr<TeamField> pending;  // being built
            oc::shared_ptr<const TeamField> published;
            JobCounter counter;
            float timer = 0.0f;
            float radius = 0.0f;   // 0 = the "Field radius" tweak (team fields); goals set their own
            uint32 lastSetFrame = 0; // goals: expiry
            bool periodic = true;  // team fields re-run on the interval; goals only on change
            bool rasterOnly = false; // the obstacle-only raster: builds with NO sources
            bool sourcesDirty = false;
            bool building = false;
        };

        bool sourcesChanged(const oc::vector<NavSource>& a, oc::span<const NavSource> b) const;
        void kickBuild(TeamSlot& slot);
        void tickSlot(TeamSlot& slot, float deltaSec);
        void waitAll();

        TeamSlot m_teams[MaxTeams];
        TeamSlot m_raster; // obstacle raster only (rebuilt on obstacle change) — for avoid()/lineOfSight
        oc::unordered_map<uint64, oc::unique_ptr<TeamSlot>> m_goals; // JobCounter is immovable
        uint32 m_frame = 0;
        FlowField m_flow[MaxTeams];
        PressureField m_pressure[MaxTeams];
        oc::vector<NavObstacle> m_obstacles;
        oc::vector<NavObstacle> m_buildObstacles; // snapshot shared by every in-flight job
        uint64 m_obstacleHash = 0;
        bool m_obstaclesDirty = false;
        // Recent plans, BUCKETED BY THEIR START at exactly the dedup radius: a request only has to
        // look at the 3x3 bucket neighbourhood around its own start, so the check costs the same
        // whether ten units or a thousand are asking every second (a flat list was O(plans) per
        // request, and every unit now requests on its own timer).
        struct SeedStamp { glm::vec2 from{ 0.0f }, to{ 0.0f }; float time = 0.0f; };
        static uint64 seedBucketKey(uint32 team, const glm::ivec2& bucket)
        {
            return (uint64(team) << 56) | ((uint64(uint32(bucket.x)) & 0xFFF'FFFFull) << 28)
                | (uint64(uint32(bucket.y)) & 0xFFF'FFFFull);
        }
        oc::unordered_map<uint64, oc::vector<SeedStamp>> m_seedBuckets;
        // EXPIRY QUEUE: stamps are made in time order and each bucket is append-only at the back,
        // so one global FIFO of (time, bucket) retires them without ever walking the map — update()
        // pops only what has actually expired, which is nothing at all on most frames.
        oc::deque<oc::pair<float, uint64>> m_seedExpiry;
        float m_time = 0.0f;
        int m_seedsThisFrame = 0;
        uint32 m_publishedCount = 0;
        bool m_initialized = false;

        // tweaks
        bool m_enabled = true;
        float m_fieldRadius = 250.0f; // covers a whole arena; a unit outside it falls back to the local search
        float m_rebuildInterval = 0.25f;
        int m_clearanceCost = 1; // 2x on wall-adjacent cells: nudge off walls, no wide detours
        int m_keepFrames = 120;
        // Fade rates are HALF-LIVES in seconds (frame-rate independent), not per-frame factors.
        float m_flowHalfLife = 5.0f;     // seconds for a lane to lose half its speed (a seeded
                                          // order lane is still ~50% after 10 s, ~25% after 20 s)
        float m_pressureDiffusion = 0.1f; // Jacobi step weight at 60 Hz (dt-scaled, clamped to 0.25)
        float m_pressureFloor = 1.6f;      // magnitude a neighbour needs before it diffuses (0 = off)
        float m_pressureHalfLife = 1.0f;  // seconds for pressure to halve — the seeded TROUGH has
                                          // to outlive the walk it was planned for, and jams stay
                                          // felt after the crowd that made them moved on
        float m_flowMaxSpeed = 20.0f;      // per-cell magnitude cap (splats SUM — see FlowField::update)
        float m_seedArea = 10.0f;         // metres: requests from/to the same area are ONE lane
        float m_seedCooldown = 3.0f;      // seconds that area pair stays suppressed
        int m_seedMaxPerFrame = 2;        // hard cap on plans per frame (A* is main-thread work)
        float m_seedTrough = 20.0f;        // NEGATIVE pressure a seeded lane carves (0 = flow only)
        float m_seedSqueeze = 10.0f;       // extra trough depth per blocked neighbour of a lane cell
        float m_seedRange = 20.0f;        // metres of the plan actually written (0 = all of it)
        // LOG-SCALED tweak: the slider is the EXPONENT, the gain is 10^x — one slider covers
        // 0.01 .. 1000 m/s of flow per unit of pressure gradient, with fine control at the low end
        // (a linear 0..20 range could neither reach "pressure dominates" nor resolve small values).
        float m_pressureFlowGainExp = 0.3f;
        float pressureFlowGain() const { return std::pow(10.0f, m_pressureFlowGainExp); }
        int m_debugMode = 2;
        int m_debugTeam = 0;
        float m_debugRadius = 60.0f;
        float m_debugFlowMin = 0.1f; // hide flow arrows below this (the haze buries the lanes)
    };
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_NAV)
    Nav::NavSystem navSystem;
}
