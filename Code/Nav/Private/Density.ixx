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
        glm::vec2 sample(const glm::vec2& xz) const;                 // worker-safe: 3x3 mean, m/s summed
        // Mean flow over the (2r+1)^2 cells around xz that are OPEN in `raster` (blocked cells and
        // absent chunks contribute nothing): "which way is the crowd around here going".
        glm::vec2 sampleArea(const glm::vec2& xz, int radiusCells, const TeamField* raster) const;
        // Main thread, once per frame outside the entity pass: create touched chunks, flip
        // buffers (write buffer = read * decay — trails), evict idle chunks.
        // `raster` + `wallBounce`: the component of a cell's flow that points INTO an adjacent
        // blocked cell is reflected (1 = full bounce, 0 = slip along the wall) — pinned units and
        // old trails never keep a lane aimed at a wall.
        void update(uint32 keepFrames, float decay, const TeamField* raster = nullptr, float wallBounce = 1.0f);
        void clear();
        // Main thread: zero both buffers within `radius` of xz — a fresh order wipes the old lane
        // around the ordered units so they can turn around without the trail pulling them back.
        void clearArea(const glm::vec2& xz, float radius);
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

    // Per-team PRESSURE: a scalar field jammed units INJECT into, that DIFFUSES outward every frame
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
        float value(const glm::vec2& xz) const;           // worker-safe: read buffer
        glm::vec2 gradient(const glm::vec2& xz) const;    // worker-safe: central difference (per metre)
        // Centre of the OPEN cell with the lowest pressure within radiusCells (own cell excluded);
        // false if none is open — the escape target for a unit boxed into a corner.
        bool lowestNearby(const glm::vec2& xz, int radiusCells, const TeamField* raster, glm::vec2& outCentre) const;
        // Main thread, once per frame outside the entity pass: grow chunks, one diffusion step
        // read -> write with `diffusion` (stable <= 0.25) and `decay`, then flip; evict quiet chunks.
        void update(const TeamField* raster, float diffusion, float decay, uint32 keepFrames);
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
