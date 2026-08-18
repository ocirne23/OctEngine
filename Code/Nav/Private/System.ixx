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
        uint32 m_publishedCount = 0;
        bool m_initialized = false;

        // tweaks
        bool m_enabled = true;
        float m_fieldRadius = 250.0f; // covers a whole arena; a unit outside it falls back to the local search
        float m_rebuildInterval = 0.25f;
        int m_clearanceCost = 1; // 2x on wall-adjacent cells: nudge off walls, no wide detours
        int m_keepFrames = 120;
        float m_flowDecay = 0.995f; // per frame: ~10 s trail at 60 Hz (lanes outlive the group)
        float m_pressureDiffusion = 0.2f; // Jacobi step weight (stable <= 0.25)
        float m_pressureDecay = 0.97f;    // per frame
        float m_wallBounce = 1.0f;        // flow into a wall: 1 = reflect, 0 = slip
        float m_pressureFlowGain = 2.0f;  // m/s of flow per unit of pressure gradient (pressure PUSHES the lanes)
        int m_debugMode = 2;
        int m_debugTeam = 0;
        float m_debugRadius = 60.0f;
    };
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_NAV)
    Nav::NavSystem navSystem;
}
