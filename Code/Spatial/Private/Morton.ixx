export module Spatial:Morton;

import Core;
import Core.glm;

// Coordinate system of the SpatialIndex. World positions (double) quantize onto a
// 21-bit-per-axis lattice of "fine" cells; every level above quadruples the cell size, so a
// level-L cell key is the fine Morton key shifted right by 6*L, a cell's parent is key >> 6,
// and the low 6 bits of a key select one of the parent's 4x4x4 children.

// 8 m: at game density unit-sized entries then share cells ~10 to one. 2 m cells put them one per
// cell, so a traversal tested ~2 cells per hit and the 8-lane static blocks ran 2 lanes full.
#ifndef SPATIAL_FINEST_CELL_SIZE
#define SPATIAL_FINEST_CELL_SIZE 8.0
#endif

export namespace Morton
{
    constexpr double FineCellSize = SPATIAL_FINEST_CELL_SIZE;
    constexpr double InvFineCellSize = 1.0 / FineCellSize;
    constexpr uint32 BitsPerAxis = 21;
    constexpr uint32 MaxLevels = 11;                        // level 10 = 2x2x2 cells spanning the world
    constexpr int64  FineOffset = 1ll << (BitsPerAxis - 1); // world origin sits mid-lattice
    constexpr int64  FineCoordMax = (1ll << BitsPerAxis) - 1;
    constexpr uint64 InvalidKey = UINT64_MAX;               // bit 63 of a valid key is always 0

    constexpr uint64 xMask = 0x1249'2492'4924'9249ull;      // bits 0,3,6,...,60
    constexpr uint64 yMask = xMask << 1;
    constexpr uint64 zMask = xMask << 2;

    struct CellCoord
    {
        uint64 x, y, z;
    };

    constexpr double cellSize(uint32 level) { return FineCellSize * double(1ull << (2 * level)); }

    inline uint64 quantizeAxis(double v)
    {
        const int64 c = int64(glm::floor(v * InvFineCellSize)) + FineOffset;
        assert(c >= 0 && c <= FineCoordMax); // outside the +/-8389km world bound (see SPATIAL_FINEST_CELL_SIZE)
        return uint64(c < 0 ? 0 : (c > FineCoordMax ? FineCoordMax : c));
    }

    inline CellCoord quantize(const glm::dvec3& pos)
    {
        return { quantizeAxis(pos.x), quantizeAxis(pos.y), quantizeAxis(pos.z) };
    }

    inline uint64 encode(const CellCoord& c)
    {
        return oc::pdep(c.x, xMask) | oc::pdep(c.y, yMask) | oc::pdep(c.z, zMask);
    }

    inline CellCoord decode(uint64 key)
    {
        return { oc::pext(key, xMask), oc::pext(key, yMask), oc::pext(key, zMask) };
    }

    inline uint64 fineKey(const glm::dvec3& pos) { return encode(quantize(pos)); }

    constexpr uint64 keyAtLevel(uint64 fineKey, uint32 level) { return fineKey >> (6 * level); }
    constexpr uint64 parentKey(uint64 key) { return key >> 6; }
    constexpr uint32 childBit(uint64 key) { return uint32(key & 63); }

    inline glm::dvec3 cellMinWorld(uint64 key, uint32 level)
    {
        const CellCoord c = decode(key);
        const uint32 shift = 2 * level;
        return glm::dvec3(
            double(int64(c.x << shift) - FineOffset),
            double(int64(c.y << shift) - FineOffset),
            double(int64(c.z << shift) - FineOffset)) * FineCellSize;
    }

    inline glm::dvec3 cellCenterWorld(uint64 key, uint32 level)
    {
        return cellMinWorld(key, level) + glm::dvec3(cellSize(level) * 0.5);
    }

    // Smallest level whose cell size fits the bounding sphere (cellSize >= 2*radius), so an entity
    // extends at most half a cell beyond its own and queries only need half-a-cell loose bounds.
    inline uint32 levelForRadius(float radius)
    {
        const uint64 q = uint64(glm::ceil(double(radius) * (2.0 * InvFineCellSize)));
        const uint32 level = q <= 1 ? 0u : uint32(oc::bitWidth(q - 1) + 1) / 2u;
        return level < MaxLevels ? level : MaxLevels - 1;
    }
}
