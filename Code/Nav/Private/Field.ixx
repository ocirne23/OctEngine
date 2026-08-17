export module Nav:Field;

import Core;
import Core.glm;
import :Grid;
import :ChunkMap;

// A per-TEAM flow field: the geodesic distance from every reached cell to the NEAREST source of
// that team (its structures + player bodies), plus which source won — so one cell read gives a
// unit of any OTHER team both "which way to the closest enemy" and "which enemy" (nearest by
// walking distance around walls, not as the crow flies). Built by a radius-bounded multi-source
// Dijkstra over the obstacle raster; chunks come into being only where the front reaches, which is
// what keeps a field around one battle site from costing anything at another. Immutable once
// built: the system publishes whole fields, workers only ever read.
export namespace Nav
{
    struct NavSource
    {
        glm::vec3 pos{ 0.0f };
        float stopRadius = 0.0f; // footprint half — seeds every cell it covers (a wall is a target too)
        uint32 id = 0;           // the game's stable id (structureId), 0 for players
        uint8 kind = 0;          // 0 structure, 1 player
    };

    struct NavObstacle
    {
        glm::vec2 min{ 0.0f };
        glm::vec2 max{ 0.0f };
    };

    class TeamField final
    {
    public:

        static constexpr uint8 Blocked = 255;   // cost: impassable
        static constexpr uint16 Unreached = 0xFFFF;
        static constexpr uint16 NoSource = 0xFFFF;
        static constexpr float DistScale = 8.0f; // dist units per metre (u16 -> 8 km reach)

        struct Chunk
        {
            uint8 cost[ChunkArea];  // 0 free, Blocked, else an extra step multiplier (1 + cost)
            uint16 dist[ChunkArea]; // fixed-point geodesic distance to the nearest source
            uint16 src[ChunkArea];  // index into sources()
            Chunk()
            {
                memset(cost, 0, sizeof(cost));
                memset(dist, 0xFF, sizeof(dist));
                memset(src, 0xFF, sizeof(src));
            }
        };

        struct BuildParams
        {
            float radius = 120.0f;   // propagation stops past this (metres of path)
            uint8 clearanceCost = 3; // extra multiplier on the ring of cells touching a blocked one
        };

        struct Sample
        {
            bool valid = false;
            float dist = 0.0f;           // metres, along the descent
            uint32 srcIndex = 0;
            glm::vec2 descentDir{ 0.0f }; // unit vector toward lower distance (zero AT a source)
        };

        // Build job body (any thread — the field is private until published).
        void build(oc::span<const NavObstacle> obstacles, oc::span<const NavSource> sources,
            const BuildParams& params);

        // Worker-safe reads. `seed` breaks ties between equidistant neighbours so a plateau
        // (equidistant between two sources) does not stall a whole crowd on one line.
        Sample sample(const glm::vec2& xz, uint32 seed) const;
        bool hasData(const glm::vec2& xz) const;
        // Raster line of sight: no BLOCKED cell between a and b (clearance cells pass). Lets a
        // walker go straight when the goal is visible instead of following cell-centre steps.
        // `radius` > 0 also tests the two parallel lines offset by it (a body, not a point).
        bool lineOfSight(const glm::vec2& a, const glm::vec2& b, float radius = 0.0f) const;
        // String pulling for a single walker (main thread — O(steps * LOS)): greedily walk the
        // descent from xz up to maxSteps cells, return the farthest path point visible from xz
        // (the source position itself when the walk reaches it). false = no field data here.
        bool steerPoint(const glm::vec2& xz, int maxSteps, float radius, glm::vec2& outPoint) const;
        oc::span<const NavSource> sources() const { return m_sources; }
        const NavSource& sourceAt(uint32 index) const { return m_sources[index]; }
        const ChunkMap<Chunk>& chunks() const { return m_chunks; }
        uint32 numCellsReached() const { return m_cellsReached; }

    private:

        struct CellRef
        {
            Chunk* chunk;
            uint32 index;
        };
        Chunk& chunkAt(const glm::ivec2& cell) { return m_chunks.getOrCreate(chunkKey(chunkOf(cell))); }
        const Chunk* findChunk(const glm::ivec2& cell) const { return m_chunks.find(chunkKey(chunkOf(cell))); }
        uint8 costAt(const glm::ivec2& cell) const;
        void rasterizeObstacles(oc::span<const NavObstacle> obstacles, uint8 clearanceCost);

        ChunkMap<Chunk> m_chunks;
        oc::vector<NavSource> m_sources;
        uint32 m_cellsReached = 0;
    };
}
