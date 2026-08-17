export module Nav:System;

import Core;
import Core.glm;
import Threading;
import :Grid;
import :Field;
import :Density;

// The navigation service: owns one published TeamField per team (rebuilt on Low-priority jobs
// whenever obstacles/sources change or the rebuild interval elapses, swapped in on the main thread
// between entity passes) and one DensityField per team. The GAME feeds obstacles + sources every
// frame (it knows footprints and teams; this library knows nothing about entities); UNITS read
// teamField(otherTeam)->sample() and density(ownTeam) from the parallel entity pass.
//
// Thread contract: setObstacles/setTeamSources/update/drawDebug are MAIN THREAD, called outside
// the entity pass. teamField()/density() reads are worker-safe during the pass because published
// pointers only change inside update().
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
        DensityField& density(uint32 team) { return m_density[glm::min(team, MaxTeams - 1)]; }
        const DensityField& density(uint32 team) const { return m_density[glm::min(team, MaxTeams - 1)]; }
        bool anyFieldPublished() const { return m_publishedCount > 0; }

        // GOAL fields: a single-destination field for one walker (the local player's move order),
        // the same TeamField with ONE source. Rebuilt when the destination moves > half a cell or
        // the obstacle raster changes; radius sized per order. Main thread; goalField() is
        // main-thread too (the player steers on main).
        static constexpr uint32 MaxGoals = 4;
        void setGoal(uint32 slot, const glm::vec3& dest, float radius);
        void clearGoal(uint32 slot);
        const TeamField* goalField(uint32 slot) const
        {
            return slot < MaxGoals ? m_goals[slot].published.get() : nullptr;
        }

        // Debug lines (renderer-agnostic sink): mode 1 chunk outlines, 2 + descent arrows of
        // "Debug team" near `focus`, 3 + density bars.
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
            bool periodic = true;  // team fields re-run on the interval; goals only on change
            bool sourcesDirty = false;
            bool building = false;
        };

        bool sourcesChanged(const oc::vector<NavSource>& a, oc::span<const NavSource> b) const;
        void kickBuild(TeamSlot& slot);
        void tickSlot(TeamSlot& slot, float deltaSec);
        void waitAll();

        TeamSlot m_teams[MaxTeams];
        TeamSlot m_goals[MaxGoals];
        DensityField m_density[MaxTeams];
        oc::vector<NavObstacle> m_obstacles;
        oc::vector<NavObstacle> m_buildObstacles; // snapshot shared by every in-flight job
        uint64 m_obstacleHash = 0;
        bool m_obstaclesDirty = false;
        uint32 m_publishedCount = 0;
        bool m_initialized = false;

        // tweaks
        bool m_enabled = true;
        float m_fieldRadius = 120.0f;
        float m_rebuildInterval = 0.25f;
        int m_clearanceCost = 1; // 2x on wall-adjacent cells: nudge off walls, no wide detours
        int m_keepFrames = 120;
        int m_debugMode = 0;
        int m_debugTeam = 1;
        float m_debugRadius = 60.0f;
    };
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_NAV)
    Nav::NavSystem navSystem;
}
