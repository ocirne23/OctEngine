export module Nav:Density;

import Core;
import Core.glm;
import Threading;
import :Grid;
import :ChunkMap;

// Per-team crowd density: units splat their presence into their cell every tick (atomic adds
// from the parallel entity pass), and read last frame's buffer as a gradient to walk AWAY from
// their own crowd (anti-clumping) — no neighbour queries. Chunks appear where units are (a splat
// into a missing chunk queues its creation for the next frame) and evict after a keep window, so
// the crowd map is exactly as large as the crowd. Double-buffered by frame: workers write buffer
// W and read buffer 1-W; update() (main, between passes) flips and clears.
export namespace Nav
{
    class DensityField final
    {
    public:

        struct Chunk
        {
            uint16 v[2][ChunkArea];
            uint32 touchedFrame;
            Chunk() { memset(v, 0, sizeof(v)); touchedFrame = 0; }
        };

        void initialize(); // JobSystem must be initialized (PerWorker staging)
        bool isInitialized() const { return m_touch.isInitialized(); }

        // Worker-safe (parallel entity pass): atomic saturating add into the write buffer.
        void splat(const glm::vec2& xz, uint16 amount = 1);
        // Worker-safe: gradient of last frame's density (units per metre), zero where no chunk.
        glm::vec2 gradient(const glm::vec2& xz) const;
        float valueAt(const glm::vec2& xz) const;

        // Main thread, once per frame OUTSIDE the entity pass: create touched chunks, flip
        // buffers, clear the new write buffer, evict chunks idle for `keepFrames`.
        void update(uint32 keepFrames);
        void clear();

        const ChunkMap<Chunk>& chunks() const { return m_chunks; }
        uint32 readBuffer() const { return m_write ^ 1u; }

    private:

        uint16 cellValue(const glm::ivec2& cell, uint32 buffer, const Chunk*& cached, glm::ivec2& cachedCoord) const;

        ChunkMap<Chunk> m_chunks;
        PerWorker<oc::vector<uint64>> m_touch; // chunk keys splatted into while absent
        uint32 m_write = 0;
        uint32 m_frame = 0;
    };
}
