export module Nav:Grid;

import Core;
import Core.glm;

// The navigation lattice: 2D over XZ, 2 m cells (= the game's structure grid and the Spatial
// index's finest cell), grouped into 16x16-cell CHUNKS (32 m) that are the unit of sparse
// storage. Everything below is pure coordinate math shared by the fields.
export namespace Nav
{
    constexpr float CellSize = 2.0f;
    constexpr int ChunkBits = 4;
    constexpr int ChunkCells = 1 << ChunkBits; // 16
    constexpr int ChunkArea = ChunkCells * ChunkCells;
    constexpr float ChunkSize = CellSize * ChunkCells;
    constexpr uint64 InvalidChunkKey = UINT64_MAX; // never produced by chunkKey (28-bit axes)

    inline glm::ivec2 cellOf(const glm::vec2& xz)
    {
        return glm::ivec2(glm::floor(xz / CellSize));
    }
    inline glm::vec2 cellCenter(const glm::ivec2& cell)
    {
        return (glm::vec2(cell) + 0.5f) * CellSize;
    }
    inline glm::ivec2 chunkOf(const glm::ivec2& cell)
    {
        return glm::ivec2(cell.x >> ChunkBits, cell.y >> ChunkBits); // arithmetic shift floors
    }
    inline uint32 cellIndex(const glm::ivec2& cell) // index inside its chunk
    {
        return uint32((cell.y & (ChunkCells - 1)) << ChunkBits) | uint32(cell.x & (ChunkCells - 1));
    }
    inline uint64 chunkKey(const glm::ivec2& chunk)
    {
        const uint64 x = uint64(uint32(chunk.x)) & 0xFFF'FFFFull;
        const uint64 z = uint64(uint32(chunk.y)) & 0xFFF'FFFFull;
        return (x << 28) | z;
    }
    inline glm::ivec2 chunkFromKey(uint64 key)
    {
        const auto signExtend28 = [](uint64 v) { return int32(int64(v << 36) >> 36); };
        return glm::ivec2(signExtend28((key >> 28) & 0xFFF'FFFFull), signExtend28(key & 0xFFF'FFFFull));
    }
    inline glm::vec2 chunkMinWorld(const glm::ivec2& chunk)
    {
        return glm::vec2(chunk) * ChunkSize;
    }
}
