export module Nav:Density;

import Core;
import Core.glm;
import Threading;
import :Grid;
import :ChunkMap;
import :Field;

// The MUTABLE per-team crowd fields (flow + pressure): units write into them from the parallel
// entity pass (atomic adds), main flips/steps them between passes. Chunks appear where units are
// (a splat into a missing chunk queues its creation for the next frame) and evict after a keep
// window, so a crowd map is exactly as large as the crowd.
export namespace Nav
{
    // Per-team CROWD FLOW: units splat their planar velocity into their cell every tick; the read
    // buffer is an exponentially decayed trail (persisting ~1 s), so a unit reads "which way is
    // the crowd already moving here" and blends into it — a group commits to one way round a wall
    // and stragglers follow the stream instead of picking their own side. Same chunk/buffer/touch
    // machinery as DensityField; values are fixed-point int16 (1/64 m/s summed).
    class FlowField final
    {
    public:

        struct Chunk
        {
            int16 vx[2][ChunkArea];
            int16 vz[2][ChunkArea];
            uint32 touchedFrame;
            Chunk() { memset(vx, 0, sizeof(vx)); memset(vz, 0, sizeof(vz)); touchedFrame = 0; }
        };
        static constexpr float Scale = 1024.0f; // int16 -> +-32 m/s summed per cell

        void initialize();
        bool isInitialized() const { return m_touch.isInitialized(); }
        void splat(const glm::vec2& xz, const glm::vec2& velocity); // worker-safe (atomic adds)
        // Worker-safe 3x3 mean. With a `raster`, BLOCKED cells are left out of the average
        // entirely instead of averaging in as zero: inside a one-cell gap two thirds of the
        // neighbourhood is wall, which used to cut the lane's sampled strength to a third of its
        // real value — exactly where a unit most needs to believe in it.
        glm::vec2 sample(const glm::vec2& xz, const TeamField* raster = nullptr) const;
        // Mean flow over the (2r+1)^2 cells around xz that are OPEN in `raster` (blocked cells and
        // absent chunks contribute nothing): "which way is the crowd around here going".
        glm::vec2 sampleArea(const glm::vec2& xz, int radiusCells, const TeamField* raster) const;
        // Main thread, once per frame outside the entity pass: create touched chunks, flip
        // buffers (write buffer = read faded toward zero — trails), evict idle chunks. Fading is
        // FRAME-RATE INDEPENDENT: `halfLifeSec` is how long a lane takes to lose half its speed,
        // and the splat gain that feeds the EMA is derived from the same step, so a lane's steady
        // state stays "the velocity the units are actually moving at" at any frame rate.
        // `raster` + `wallBounce`: the component of a cell's flow that points INTO an adjacent
        // blocked cell is reflected (1 = full bounce, 0 = slip along the wall) — pinned units and
        // old trails never keep a lane aimed at a wall.
        // `viscosity` (<= 0.25) is momentum DIFFUSION, one-way: a cell blends toward its STRONGER
        // open neighbours only, so a lane drags the flow beside it along like a real fluid while
        // the weak haze around it can never average the lane down or straighten it out of a corner.
        // `maxSpeed` caps a cell's magnitude: splats SUM, so a crowd milling in one spot can build
        // a vector several times stronger than any seeded lane and out-vote it.
        // `viscosityFloor` (m/s) is the magnitude a neighbour must reach before it may drag this
        // cell at all: only real lanes spread, a weak haze stays where it was made.
        // `viscosityBackflow` (0..1) scales the OTHER direction — how much a WEAKER neighbour may
        // pull this cell. 0 = strictly one-way (a lane keeps its exact shape), 1 = plain symmetric
        // diffusion (a lane also gets averaged against its surroundings, which rounds it off).
        void update(float deltaSec, uint32 keepFrames, float halfLifeSec, const TeamField* raster = nullptr,
            float wallBounce = 1.0f, float viscosity = 0.0f, float maxSpeed = 0.0f,
            float viscosityFloor = 0.0f, float viscosityBackflow = 0.0f);
        void clear();
        // Main thread: zero both buffers within `radius` of xz — a fresh order wipes the old lane
        // around the ordered units so they can turn around without the trail pulling them back.
        void clearArea(const glm::vec2& xz, float radius);
        // Main thread: WRITE a lane along a polyline into both buffers (max-magnitude, so it is
        // visible to units immediately and survives the next decay step) — a planned route handed
        // to the crowd as flow, see NavSystem::seedPath. `raster` is REQUIRED to keep the lane's
        // lateral spread out of walls (a wide lane beside a building would otherwise write flow
        // into cells nothing can stand in).
        void seedPath(oc::span<const glm::vec2> path, float speed, float radius, const TeamField* raster);
        const ChunkMap<Chunk>& chunks() const { return m_chunks; }
        uint32 readBuffer() const { return m_write ^ 1u; }

    private:

        ChunkMap<Chunk> m_chunks;
        PerWorker<oc::vector<uint64>> m_touch;
        uint32 m_write = 0;
        uint32 m_frame = 0;
        float m_splatGain = 0.06f; // = 1 - decay: the read buffer is an EMA of the splatted
                                   // velocities (steady state = the velocity itself, not a sum)
    };

    // Per-team PRESSURE: a SIGNED scalar field jammed units INJECT into, that DIFFUSES outward every frame
    // (one Jacobi step over the touched chunks — walls are reflective, so pressure flows around
    // buildings, not through them) and decays. Units steer DOWN the pressure gradient, so a jam is
    // felt upstream before anyone reaches it, and the jam itself is squeezed out toward open space.
    // Chunks grow wherever pressure reaches a border cell and evict once they are quiet.
    class PressureField final
    {
    public:

        struct Chunk
        {
            float p[2][ChunkArea];
            uint32 touchedFrame;
            float peak; // max value after the last step (eviction / growth decisions)
            Chunk() { memset(p, 0, sizeof(p)); touchedFrame = 0; peak = 0.0f; }
        };

        void initialize();
        bool isInitialized() const { return m_touch.isInitialized(); }
        void inject(const glm::vec2& xz, float amount);   // worker-safe (atomic add into the write buffer)
        // Main thread: carve a NEGATIVE-pressure trough along a polyline (`amount` > 0 = depth).
        // Everything reads pressure as "go the other way", so a trough ATTRACTS: units steer into
        // the lane and the pressure->flow push sucks surrounding flow into it. Min semantics, so
        // re-seeding the same route is idempotent instead of digging deeper each time.
        // `squeezeGain` deepens the trough per BLOCKED cell of the 8-neighbourhood: a narrow
        // channel is where a crowd most needs to be pulled in (and where the flow has no
        // surrounding cells to support it), so a gap ends up several times more attractive than
        // the open stretches of the same route.
        void seedPath(oc::span<const glm::vec2> path, float amount, float radius, const TeamField* raster,
            float squeezeGain = 0.0f);
        float value(const glm::vec2& xz) const;           // worker-safe: read buffer
        // Worker-safe central difference (per metre). Pass the `raster`: a blocked neighbour is
        // substituted with the centre value (NO-FLUX, the same boundary the diffusion step uses).
        // Without it a wall reads as a plain 0, which against a negative seeded trough looks like a
        // high-pressure spot — every vector near a wall then pushes away from it.
        glm::vec2 gradient(const glm::vec2& xz, const TeamField* raster = nullptr) const;
        // Centre of the OPEN cell with the lowest pressure within radiusCells (own cell excluded);
        // false if none is open — the escape target for a unit boxed into a corner.
        bool lowestNearby(const glm::vec2& xz, int radiusCells, const TeamField* raster, glm::vec2& outCentre) const;
        // Main thread, once per frame outside the entity pass: grow chunks, one diffusion step
        // read -> write, then flip; evict quiet chunks. Both rates are FRAME-RATE INDEPENDENT:
        // `halfLifeSec` is how long pressure takes to halve, and `diffusionPerSec` is scaled by
        // deltaSec then clamped to the Jacobi stability limit (0.25 per step).
        // `propagationFloor` is the magnitude a neighbour must reach before it may diffuse into
        // this cell at all (the pressure twin of the flow's viscosity floor): faint remains stop
        // spreading, and a weak/empty neighbour can no longer drain a deep trough sideways. NOTE
        // the trade-off — a trough that spreads without paying for it grows until the half-life
        // catches up, and a field that floods flat has no gradient left to steer by, so raise this
        // together with a shorter half-life. 0 = plain diffusion.
        // `onActiveCell` (optional) is called for every cell that ends the step with a meaningful
        // value, with the gradient the step already had in hand — the pressure->flow push rides
        // this instead of a second pass that re-resolved 8 neighbours per cell through the hash.
        using CellVisit = oc::function<void(const glm::vec2& centre, const glm::vec2& gradient)>;
        void update(float deltaSec, const TeamField* raster, float diffusionPerSec, float halfLifeSec,
            uint32 keepFrames, float propagationFloor = 0.0f, const CellVisit* onActiveCell = nullptr);
        void clear();
        const ChunkMap<Chunk>& chunks() const { return m_chunks; }
        uint32 readBuffer() const { return m_write; } // one CURRENT buffer (read + inject); the other is the step's scratch

    private:

        float cellValue(const glm::ivec2& cell, uint32 buffer, const Chunk*& cached, glm::ivec2& cachedCoord) const;

        ChunkMap<Chunk> m_chunks;
        PerWorker<oc::vector<uint64>> m_touch;
        uint32 m_write = 0;
        uint32 m_frame = 0;
    };
}
