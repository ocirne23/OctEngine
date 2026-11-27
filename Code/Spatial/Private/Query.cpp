module;

#include <immintrin.h>

module Spatial;

import Core;
import Core.glm;
import Core.Camera;
import Core.Frustum;

import :Morton;
import :Types;
import :CellMap;
import :RecordPool;
import :BlockStore;
import :SpatialIndexState;
import :OcclusionState;

// Shared hierarchical traversal: descend the per-level grids top-down through the occupancy
// masks, classifying each cell's loose bounds (inflated by half a cell - entities extend at most
// that far beyond their cell) as outside / intersecting / fully inside. Fully-inside subtrees
// append their entries without further per-entity tests. A cell's corner is integer on the fine
// lattice; the tests are float relative to the reference (TraversalRef), exact at planet scale.

namespace
{
    // A tester's classify returns the cell's new ACTIVE mask - the bounds it still straddles, 0 =
    // fully inside - or ClassifyOutside. Only the active bounds are tested further down the subtree.
    constexpr uint32 ClassifyOutside = UINT32_MAX;

    // The 8 lane layout of a cell's children in a classify8 batch.
    struct ChildBatch
    {
        alignas(32) float cx[8];
        alignas(32) float cy[8];
        alignas(32) float cz[8];
    };

    // Child bit (the low 6 Morton bits: x0 y0 z0 x1 y1 z1) -> its 4x4x4 grid coordinate, as floats (the classify8 batch) and as an
    // integer x, y, z, 0 (the child's fine min corner: the parent's plus this shifted by 2 * childLevel).
    struct ChildOffsets
    {
        float x[64], y[64], z[64];
        alignas(16) int32 cell[64][4];
    };
    constexpr ChildOffsets makeChildOffsets()
    {
        ChildOffsets o{};
        for (uint32 b = 0; b < 64; ++b)
        {
            const int32 x = int32((b & 1) | ((b >> 2) & 2));
            const int32 y = int32(((b >> 1) & 1) | ((b >> 3) & 2));
            const int32 z = int32(((b >> 2) & 1) | ((b >> 4) & 2));
            o.x[b] = float(x);
            o.y[b] = float(y);
            o.z[b] = float(z);
            o.cell[b][0] = x;
            o.cell[b][1] = y;
            o.cell[b][2] = z;
        }
        return o;
    }
    constexpr ChildOffsets g_childOffsets = makeChildOffsets();

    glm::vec3 toVec3(__m128 v)
    {
        return { _mm_cvtss_f32(v), _mm_cvtss_f32(_mm_movehdup_ps(v)), _mm_cvtss_f32(_mm_movehl_ps(v, v)) };
    }

    TraversalRef makeRef(const glm::dvec3& pos)
    {
        const __m256d p = _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(&pos.x)), _mm_load_sd(&pos.z), 1); // x, y, z, 0
        const __m256d lattice = _mm256_add_pd(p, _mm256_set1_pd(double(Morton::FineOffset) * Morton::FineCellSize));
        const __m256d cell = _mm256_floor_pd(_mm256_mul_pd(lattice, _mm256_set1_pd(Morton::InvFineCellSize)));
        return { _mm256_cvttpd_epi32(cell), _mm256_cvtpd_ps(_mm256_fnmadd_pd(cell, _mm256_set1_pd(Morton::FineCellSize), lattice)), pos };
    }

    // A fine-lattice corner relative to the reference, in metres. Two corners inside the world differ by < 2^22 fine cells, so the
    // integer difference is exact in float and the scale is too: the FMA is the only rounding.
    glm::vec3 relMin(const TraversalRef& ref, __m128i minFine)
    {
        return toVec3(_mm_fmsub_ps(_mm_cvtepi32_ps(_mm_sub_epi32(minFine, ref.fine)), _mm_set1_ps(float(Morton::FineCellSize)), ref.frac));
    }

    // A cell's min corner on the fine lattice from its key (the top level only: below it, children add their offset).
    __m128i minFineOf(uint64 key, uint32 level)
    {
        const Morton::CellCoord c = Morton::decode(key);
        return _mm_sll_epi32(_mm_set_epi32(0, int32(c.z), int32(c.y), int32(c.x)), _mm_cvtsi32_si128(int32(2 * level)));
    }

    // Turns a cell's lanes into metres relative to the reference (the lane encoding: BlockStore).
    struct LaneDecode
    {
        glm::vec3 base;       // the cell's min + half a position quantum: a lane decodes to its quantum's centre
        float posQuantum;
        float radiusQuantum;
        float oversizeRadius; // the top level only (else 0): a saturated radius tests as the largest clamped-oversize one
    };

    LaneDecode makeLaneDecode(const glm::vec3& cellMin, uint32 level, float oversizeRadius)
    {
        const LaneScale& s = g_laneScales.level[level];
        return { cellMin + glm::vec3(0.5f * s.posQuantum), s.posQuantum, s.radiusQuantum, oversizeRadius };
    }

    glm::vec3 lanePos(const LaneDecode& d, const CellBlock& block, uint32 lane)
    {
        return d.base + glm::vec3(float(block.posX[lane]), float(block.posY[lane]), float(block.posZ[lane])) * d.posQuantum;
    }

    float laneRadius(const LaneDecode& d, const CellBlock& block, uint32 lane)
    {
        const int16 r = block.radius[lane];
        return d.oversizeRadius > 0.0f && r == LaneRadiusMax ? d.oversizeRadius : float(r) * d.radiusQuantum;
    }

    // Lanes holding an entry (not tombstoned, not unused past the block's count): the sign bit of each int16 radius is an odd
    // movemask bit.
    uint32 liveLanes(const CellBlock& block)
    {
        return ~oc::pext(uint32(_mm_movemask_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(block.radius)))), 0xAAAAu) & 0xFFu;
    }

    // Lanes whose layer byte carries one of the layers.
    uint32 layerLanes(const uint8* layers, uint32 layerMask)
    {
        const __m128i lay = _mm_and_si128(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(layers)), _mm_set1_epi8(char(layerMask)));
        return ~uint32(_mm_movemask_epi8(_mm_cmpeq_epi8(lay, _mm_setzero_si128()))) & 0xFFu;
    }

    void spreadActiveBit(uint32 laneBits, uint32 bit, uint32 (&laneActive)[8])
    {
        for (; laneBits; laneBits &= laneBits - 1)
            laneActive[oc::tzcnt(laneBits)] |= bit;
    }

    // A block's 8 lane centers relative to the reference, and their radii (negative: a tombstone or an unused lane), in metres. All
    // four rows are the block's first cache line.
    struct BlockLanes
    {
        __m256 x, y, z, r;
    };
    BlockLanes loadLanes(const LaneDecode& d, const CellBlock& block)
    {
        const __m256 q = _mm256_set1_ps(d.posQuantum);
        const auto axis = [&](const uint16* row, float base)
        {
            const __m256i quanta = _mm256_cvtepu16_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(row)));
            return _mm256_fmadd_ps(_mm256_cvtepi32_ps(quanta), q, _mm256_set1_ps(base));
        };
        const __m256i radius = _mm256_cvtepi16_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(block.radius)));
        __m256 r = _mm256_mul_ps(_mm256_cvtepi32_ps(radius), _mm256_set1_ps(d.radiusQuantum));
        if (d.oversizeRadius > 0.0f)
            r = _mm256_blendv_ps(r, _mm256_set1_ps(d.oversizeRadius),
                                 _mm256_castsi256_ps(_mm256_cmpeq_epi32(radius, _mm256_set1_epi32(LaneRadiusMax))));
        return { axis(block.posX, d.base.x), axis(block.posY, d.base.y), axis(block.posZ, d.base.z), r };
    }

    struct SphereTester // query volume centered on the reference
    {
        static constexpr uint32 AllActive = 1;
        static constexpr uint32 Passes = 1;
        float radius;

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32) const
        {
            const glm::vec3 d = glm::abs(cellCenter);
            const glm::vec3 closest = glm::max(d - halfExtent, glm::vec3(0.0f));
            if (glm::dot(closest, closest) > radius * radius)
                return ClassifyOutside;
            const glm::vec3 farthest = d + halfExtent;
            return glm::dot(farthest, farthest) <= radius * radius ? 0u : 1u;
        }

        // 8 child cells (centers in the batch, one shared half extent) at once; returns the lanes
        // that are not outside.
        uint32 classify8(const ChildBatch& batch, const glm::vec3& halfExtent, uint32, uint32 (&laneActive)[8]) const
        {
            const __m256 zero = _mm256_setzero_ps();
            const __m256 signMask = _mm256_set1_ps(-0.0f);
            const __m256 ax = _mm256_andnot_ps(signMask, _mm256_load_ps(batch.cx));
            const __m256 ay = _mm256_andnot_ps(signMask, _mm256_load_ps(batch.cy));
            const __m256 az = _mm256_andnot_ps(signMask, _mm256_load_ps(batch.cz));
            const __m256 hx = _mm256_set1_ps(halfExtent.x), hy = _mm256_set1_ps(halfExtent.y), hz = _mm256_set1_ps(halfExtent.z);
            const __m256 r2 = _mm256_set1_ps(radius * radius);
            const __m256 nx = _mm256_max_ps(_mm256_sub_ps(ax, hx), zero);
            const __m256 ny = _mm256_max_ps(_mm256_sub_ps(ay, hy), zero);
            const __m256 nz = _mm256_max_ps(_mm256_sub_ps(az, hz), zero);
            const __m256 near2 = _mm256_fmadd_ps(nx, nx, _mm256_fmadd_ps(ny, ny, _mm256_mul_ps(nz, nz)));
            const __m256 fx = _mm256_add_ps(ax, hx), fy = _mm256_add_ps(ay, hy), fz = _mm256_add_ps(az, hz);
            const __m256 far2 = _mm256_fmadd_ps(fx, fx, _mm256_fmadd_ps(fy, fy, _mm256_mul_ps(fz, fz)));
            for (uint32& a : laneActive)
                a = 0;
            spreadActiveBit(uint32(_mm256_movemask_ps(_mm256_cmp_ps(far2, r2, _CMP_GT_OQ))), 1u, laneActive);
            return ~uint32(_mm256_movemask_ps(_mm256_cmp_ps(near2, r2, _CMP_GT_OQ))) & 0xFFu;
        }

        uint32 acceptCell(const glm::vec3&, const glm::vec3&, uint32 active) const { return active; }
        glm::vec3 boundHalf() const { return glm::vec3(radius); } // enables the direct cell path (traverse)

        // 8 spheres at once; mostly pays for mid-size radii (gameplay queries, queryNearest rings)
        // where partially-covered cells dominate - huge balls take the fully-inside wholesale path.
        uint32 test8(const LaneDecode& d, const CellBlock& block, uint32 layerMask, uint32) const
        {
            const BlockLanes l = loadLanes(d, block);
            const __m256 d2 = _mm256_fmadd_ps(l.x, l.x, _mm256_fmadd_ps(l.y, l.y, _mm256_mul_ps(l.z, l.z)));
            const __m256 rr = _mm256_add_ps(_mm256_set1_ps(radius), l.r);
            const uint32 inside = uint32(_mm256_movemask_ps(_mm256_cmp_ps(d2, _mm256_mul_ps(rr, rr), _CMP_LE_OQ)));
            return inside & liveLanes(block) & layerLanes(block.layer, layerMask);
        }
    };

    struct AABBTester // box centered on the reference
    {
        static constexpr uint32 AllActive = 1;
        static constexpr uint32 Passes = 1;
        glm::vec3 half;

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32) const
        {
            const glm::vec3 d = glm::abs(cellCenter);
            if (d.x - halfExtent.x > half.x || d.y - halfExtent.y > half.y || d.z - halfExtent.z > half.z)
                return ClassifyOutside;
            return d.x + halfExtent.x <= half.x && d.y + halfExtent.y <= half.y && d.z + halfExtent.z <= half.z ? 0u : 1u;
        }

        uint32 acceptCell(const glm::vec3&, const glm::vec3&, uint32 active) const { return active; }
        glm::vec3 boundHalf() const { return half; }

        // 8 sphere-vs-box tests at once (squared distance from the box to each sphere center).
        uint32 test8(const LaneDecode& d, const CellBlock& block, uint32 layerMask, uint32) const
        {
            const __m256 zero = _mm256_setzero_ps();
            const __m256 signMask = _mm256_set1_ps(-0.0f);
            const BlockLanes l = loadLanes(d, block);
            const __m256 cx = _mm256_max_ps(_mm256_sub_ps(_mm256_andnot_ps(signMask, l.x), _mm256_set1_ps(half.x)), zero);
            const __m256 cy = _mm256_max_ps(_mm256_sub_ps(_mm256_andnot_ps(signMask, l.y), _mm256_set1_ps(half.y)), zero);
            const __m256 cz = _mm256_max_ps(_mm256_sub_ps(_mm256_andnot_ps(signMask, l.z), _mm256_set1_ps(half.z)), zero);
            const __m256 d2 = _mm256_fmadd_ps(cx, cx, _mm256_fmadd_ps(cy, cy, _mm256_mul_ps(cz, cz)));
            const uint32 inside = uint32(_mm256_movemask_ps(_mm256_cmp_ps(d2, _mm256_mul_ps(l.r, l.r), _CMP_LE_OQ)));
            return inside & liveLanes(block) & layerLanes(block.layer, layerMask);
        }
    };

    // One frustum + distance band, camera-relative. Its active bits: 0-5 the planes, 6 the band.
    struct FrustumVolume
    {
        static constexpr uint32 PlaneBits = 0x3F;
        static constexpr uint32 DistanceBit = 1u << 6;
        static constexpr uint32 AllActive = PlaneBits | DistanceBit;

        const Frustum& frustum;
        float maxDist;
        glm::vec3 absNormal[6]; // |plane normal|: an AABB's projected radius on each plane

        FrustumVolume(const Frustum& frustumRelCamera, float maxDistance) : frustum(frustumRelCamera), maxDist(maxDistance)
        {
            for (uint32 p = 0; p < 6; ++p)
                absNormal[p] = glm::abs(glm::vec3(frustum.planes[p]));
        }

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32 active) const
        {
            uint32 result = 0;
            if (active & DistanceBit)
            {
                const glm::vec3 d = glm::abs(cellCenter);
                const glm::vec3 closest = glm::max(d - halfExtent, glm::vec3(0.0f));
                if (glm::dot(closest, closest) > maxDist * maxDist)
                    return ClassifyOutside;
                const glm::vec3 farthest = d + halfExtent;
                if (glm::dot(farthest, farthest) > maxDist * maxDist)
                    result |= DistanceBit;
            }
            for (uint32 planes = active & PlaneBits; planes; planes &= planes - 1)
            {
                const uint32 p = uint32(oc::tzcnt(planes));
                const glm::vec4& plane = frustum.planes[p];
                const float r = glm::dot(halfExtent, absNormal[p]);
                const float d = glm::dot(glm::vec3(plane), cellCenter) + plane.w;
                if (d < -r)
                    return ClassifyOutside;
                if (d < r)
                    result |= 1u << p;
            }
            return result;
        }

        // 8 child cells at once against the ACTIVE bounds: returns the lanes that are not outside, and ORs each lane's still-active
        // bits, shifted left by `shift`, into laneActive.
        uint32 classify8(const ChildBatch& batch, const glm::vec3& halfExtent, uint32 active, uint32 shift, uint32 (&laneActive)[8]) const
        {
            const __m256 x = _mm256_load_ps(batch.cx);
            const __m256 y = _mm256_load_ps(batch.cy);
            const __m256 z = _mm256_load_ps(batch.cz);
            __m256 outside = _mm256_setzero_ps();
            if (active & DistanceBit)
            {
                const __m256 zero = _mm256_setzero_ps();
                const __m256 signMask = _mm256_set1_ps(-0.0f);
                const __m256 ax = _mm256_andnot_ps(signMask, x), ay = _mm256_andnot_ps(signMask, y), az = _mm256_andnot_ps(signMask, z);
                const __m256 hx = _mm256_set1_ps(halfExtent.x), hy = _mm256_set1_ps(halfExtent.y), hz = _mm256_set1_ps(halfExtent.z);
                const __m256 md2 = _mm256_set1_ps(maxDist * maxDist);
                const __m256 nx = _mm256_max_ps(_mm256_sub_ps(ax, hx), zero);
                const __m256 ny = _mm256_max_ps(_mm256_sub_ps(ay, hy), zero);
                const __m256 nz = _mm256_max_ps(_mm256_sub_ps(az, hz), zero);
                const __m256 near2 = _mm256_fmadd_ps(nx, nx, _mm256_fmadd_ps(ny, ny, _mm256_mul_ps(nz, nz)));
                outside = _mm256_cmp_ps(near2, md2, _CMP_GT_OQ);
                const __m256 fx = _mm256_add_ps(ax, hx), fy = _mm256_add_ps(ay, hy), fz = _mm256_add_ps(az, hz);
                const __m256 far2 = _mm256_fmadd_ps(fx, fx, _mm256_fmadd_ps(fy, fy, _mm256_mul_ps(fz, fz)));
                spreadActiveBit(uint32(_mm256_movemask_ps(_mm256_cmp_ps(far2, md2, _CMP_GT_OQ))), DistanceBit << shift, laneActive);
            }
            for (uint32 planes = active & PlaneBits; planes; planes &= planes - 1)
            {
                const uint32 p = uint32(oc::tzcnt(planes));
                const glm::vec4& plane = frustum.planes[p];
                const float r = glm::dot(halfExtent, absNormal[p]);
                const __m256 d = _mm256_fmadd_ps(_mm256_set1_ps(plane.x), x,
                                 _mm256_fmadd_ps(_mm256_set1_ps(plane.y), y,
                                 _mm256_fmadd_ps(_mm256_set1_ps(plane.z), z, _mm256_set1_ps(plane.w))));
                outside = _mm256_or_ps(outside, _mm256_cmp_ps(d, _mm256_set1_ps(-r), _CMP_LT_OQ));
                spreadActiveBit(uint32(_mm256_movemask_ps(_mm256_cmp_ps(d, _mm256_set1_ps(r), _CMP_LT_OQ))), (1u << p) << shift, laneActive);
            }
            return ~uint32(_mm256_movemask_ps(outside)) & 0xFFu;
        }

        // The lanes whose spheres are inside the ACTIVE bounds (every lane when none is active). Tombstones are not excluded here.
        __m256 test8(const BlockLanes& l, uint32 active) const
        {
            __m256 ok = _mm256_castsi256_ps(_mm256_set1_epi32(-1));
            if (active & DistanceBit)
            {
                const __m256 d2 = _mm256_fmadd_ps(l.x, l.x, _mm256_fmadd_ps(l.y, l.y, _mm256_mul_ps(l.z, l.z)));
                const __m256 rr = _mm256_add_ps(_mm256_set1_ps(maxDist), l.r);
                ok = _mm256_cmp_ps(d2, _mm256_mul_ps(rr, rr), _CMP_LE_OQ);
            }
            if (active & PlaneBits)
            {
                const __m256 negR = _mm256_sub_ps(_mm256_setzero_ps(), l.r);
                for (uint32 planes = active & PlaneBits; planes; planes &= planes - 1)
                {
                    const glm::vec4& plane = frustum.planes[oc::tzcnt(planes)];
                    const __m256 d = _mm256_fmadd_ps(_mm256_set1_ps(plane.x), l.x,
                                     _mm256_fmadd_ps(_mm256_set1_ps(plane.y), l.y,
                                     _mm256_fmadd_ps(_mm256_set1_ps(plane.z), l.z, _mm256_set1_ps(plane.w))));
                    ok = _mm256_and_ps(ok, _mm256_cmp_ps(d, negR, _CMP_GE_OQ));
                }
            }
            return ok;
        }
    };

    struct FrustumTester // frustum + distance band, both camera-relative (the reference = the camera)
    {
        static constexpr uint32 AllActive = FrustumVolume::AllActive;
        static constexpr uint32 Passes = 1;

        FrustumVolume volume;
        IOcclusionTester* occlusion;

        FrustumTester(const Frustum& frustumRelCamera, IOcclusionTester* occlusionTester, float maxDistance)
            : volume(frustumRelCamera, maxDistance), occlusion(occlusionTester) {}

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32 active) const
        {
            const uint32 result = volume.classify(cellCenter, halfExtent, active);
            if (result != ClassifyOutside && occlusion && !occlusion->isVisible(cellCenter, halfExtent))
                return ClassifyOutside;
            return result;
        }

        // No occlusion here: the caller runs acceptCell on the survivors.
        uint32 classify8(const ChildBatch& batch, const glm::vec3& halfExtent, uint32 active, uint32 (&laneActive)[8]) const
        {
            for (uint32& a : laneActive)
                a = 0;
            return volume.classify8(batch, halfExtent, active, 0, laneActive);
        }

        uint32 acceptCell(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32 active) const
        {
            return !occlusion || occlusion->isVisible(cellCenter, halfExtent) ? active : ClassifyOutside;
        }

        // 8 spheres at once against the ACTIVE planes and band (the cell is inside the rest); lanes
        // with negative radius (tombstones) or non-matching layers drop out. Returns an 8-bit hit mask.
        uint32 test8(const LaneDecode& d, const CellBlock& block, uint32 layerMask, uint32 active) const
        {
            const BlockLanes l = loadLanes(d, block);
            return uint32(_mm256_movemask_ps(volume.test8(l, active))) & liveLanes(block) & layerLanes(block.layer, layerMask);
        }
    };

    // The Main and Shadow stamps in ONE traversal (update()). The Shadow frustum is the Main one with the planes that face against the
    // sun sweep moved out, and a longer band, so it CONTAINS the Main one: outside Shadow is outside both. A plane the sweep left alone
    // (`shared`) gives both passes the same answer, so it is tested once. Active bits: 0-6 Main, 7 Main ruled out (outside, or occluded:
    // the subtree still runs for Shadow), 8-14 Shadow. test8 hits: bits 0-7 Main, 8-15 Shadow. Occlusion prunes Main only.
    struct DualFrustumTester
    {
        static constexpr uint32 MainBits = FrustumVolume::AllActive;
        static constexpr uint32 MainOut = 1u << 7;
        static constexpr uint32 ShadowShift = 8;
        static constexpr uint32 ShadowBits = FrustumVolume::AllActive << ShadowShift;
        static constexpr uint32 AllActive = MainBits | ShadowBits;
        static constexpr uint32 Passes = 2;

        FrustumVolume main;
        FrustumVolume shadow;
        IOcclusionTester* occlusion;
        uint32 mainLayers;
        uint32 shadowLayers;
        uint32 shared = 0; // the planes (bits 0-5) the sweep did not move

        DualFrustumTester(const Frustum& mainFrustum, float mainMaxDist, uint32 mainLayerMask, const Frustum& shadowFrustum, float shadowMaxDist,
                          uint32 shadowLayerMask, IOcclusionTester* occlusionTester)
            : main(mainFrustum, mainMaxDist), shadow(shadowFrustum, shadowMaxDist), occlusion(occlusionTester),
              mainLayers(mainLayerMask), shadowLayers(shadowLayerMask)
        {
            for (uint32 p = 0; p < 6; ++p)
                if (mainFrustum.planes[p] == shadowFrustum.planes[p])
                    shared |= 1u << p;
        }

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32 active) const
        {
            const uint32 s = shadow.classify(cellCenter, halfExtent, active >> ShadowShift);
            if (s == ClassifyOutside)
                return ClassifyOutside;
            uint32 m = MainOut;
            if (!(active & MainOut))
            {
                m = main.classify(cellCenter, halfExtent, active & MainBits);
                if (m == ClassifyOutside || (occlusion && !occlusion->isVisible(cellCenter, halfExtent)))
                    m = MainOut;
            }
            return m | (s << ShadowShift);
        }

        // No occlusion here: the caller runs acceptCell on the survivors.
        uint32 classify8(const ChildBatch& batch, const glm::vec3& halfExtent, uint32 active, uint32 (&laneActive)[8]) const
        {
            for (uint32& a : laneActive)
                a = 0;
            const uint32 pass = shadow.classify8(batch, halfExtent, active >> ShadowShift, ShadowShift, laneActive);
            if (active & MainOut)
            {
                for (uint32& a : laneActive)
                    a |= MainOut;
                return pass;
            }
            const uint32 mainIn = main.classify8(batch, halfExtent, active & MainBits & ~shared, 0, laneActive);
            for (uint32 lane = 0; lane < 8; ++lane)
            {
                uint32& a = laneActive[lane];
                // a shared plane's Main bit is its Shadow bit (same plane, same history)
                a = (mainIn >> lane) & 1u ? a | ((a >> ShadowShift) & shared & active) : (a & ShadowBits) | MainOut;
            }
            return pass;
        }

        uint32 acceptCell(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32 active) const
        {
            if (!(active & MainOut) && occlusion && !occlusion->isVisible(cellCenter, halfExtent))
                return (active & ShadowBits) | MainOut;
            return active;
        }

        // active == 0: the cell is fully inside both.
        uint32 insideHits(const CellBlock& block) const
        {
            const uint32 live = liveLanes(block);
            return (live & layerLanes(block.layer, mainLayers)) | ((live & layerLanes(block.layer, shadowLayers)) << ShadowShift);
        }

        uint32 test8(const LaneDecode& d, const CellBlock& block, uint32, uint32 active) const
        {
            const uint32 shadowActive = active >> ShadowShift;
            const bool mainIn = !(active & MainOut);
            const uint32 mainActive = mainIn ? active & MainBits : 0u;
            const uint32 live = liveLanes(block);
            if ((shadowActive | mainActive) == 0) // Main ruled out, the cell fully inside Shadow
                return (live & layerLanes(block.layer, shadowLayers)) << ShadowShift;
            const BlockLanes l = loadLanes(d, block);
            const __m256 both = shadow.test8(l, (shadowActive | mainActive) & shared); // the shared planes, once for both
            const uint32 shadowHits = uint32(_mm256_movemask_ps(_mm256_and_ps(both, shadow.test8(l, shadowActive & ~shared))))
                                    & live & layerLanes(block.layer, shadowLayers);
            uint32 mainHits = 0;
            if (mainIn)
                mainHits = uint32(_mm256_movemask_ps(_mm256_and_ps(both, main.test8(l, mainActive & ~shared))))
                         & live & layerLanes(block.layer, mainLayers);
            return mainHits | (shadowHits << ShadowShift);
        }
    };

    struct RayTester // segment [0, maxDist] along dir from the reference; cells are never fully inside
    {
        static constexpr uint32 AllActive = 1;
        static constexpr uint32 Passes = 1;
        glm::vec3 dir; // normalized
        float maxDist;

        uint32 classify(const glm::vec3& cellCenter, const glm::vec3& halfExtent, uint32) const
        {
            float tMin = 0.0f;
            float tMax = maxDist;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (glm::abs(dir[axis]) < 1e-8f)
                {
                    if (glm::abs(cellCenter[axis]) > halfExtent[axis])
                        return ClassifyOutside;
                }
                else
                {
                    const float inv = 1.0f / dir[axis];
                    float t0 = (cellCenter[axis] - halfExtent[axis]) * inv;
                    float t1 = (cellCenter[axis] + halfExtent[axis]) * inv;
                    if (t0 > t1)
                    {
                        const float tmp = t0;
                        t0 = t1;
                        t1 = tmp;
                    }
                    tMin = glm::max(tMin, t0);
                    tMax = glm::min(tMax, t1);
                    if (tMin > tMax)
                        return ClassifyOutside;
                }
            }
            return 1u;
        }

        bool testEntity(const glm::vec3& pos, float entityRadius) const
        {
            const float t = glm::clamp(glm::dot(pos, dir), 0.0f, maxDist);
            const glm::vec3 closest = pos - dir * t;
            return glm::dot(closest, closest) <= entityRadius * entityRadius;
        }

        uint32 acceptCell(const glm::vec3&, const glm::vec3&, uint32 active) const { return active; }
    };

    template <typename Tester>
    concept HasTest8 = requires(const Tester& t, const LaneDecode& d, const CellBlock& b, uint32 m)
    {
        t.test8(d, b, m, m);
    };

    template <typename Tester>
    concept HasClassify8 = requires(const Tester& t, const ChildBatch& b, const glm::vec3& v, uint32 m, uint32 (&a)[8])
    {
        t.classify8(b, v, m, a);
    };

    template <typename Tester>
    concept HasInsideHits = requires(const Tester& t, const CellBlock& b) { t.insideHits(b); };

    template <typename Tester>
    concept HasBound = requires(const Tester& t) { t.boundHalf(); };

    // quantizeAxis without the world-bound assert: a query volume may legitimately reach past it
    uint64 quantizeAxisClamped(double v)
    {
        const int64 c = int64(glm::floor(v * Morton::InvFineCellSize)) + Morton::FineOffset;
        return uint64(c < 0 ? 0 : (c > Morton::FineCoordMax ? Morton::FineCoordMax : c));
    }

    // A bounded query touching at most this many cells over all occupied levels addresses them
    // directly instead of descending the hierarchy from the top (see traverse).
    constexpr uint32 DirectCellLimit = 96;
}

template <typename Tester>
uint32 SpatialIndexState::testCell(const Tester& tester, const glm::vec3& cellMin, float halfCell, uint32 level,
                              uint32 active, [[maybe_unused]] TraverseStats& stats) const
{
    float loose = halfCell;
    const float topLevelMaxRadius = m_topLevelMaxRadius.load(oc::memory_order_relaxed);
    if (level == m_numLevels - 1 && topLevelMaxRadius > halfCell)
        loose = topLevelMaxRadius; // clamped-oversize entries can stick out further
    const glm::vec3 center = cellMin + glm::vec3(halfCell);
    const glm::vec3 half = glm::vec3(halfCell + loose);
    if (active)
    {
        const uint32 result = tester.classify(center, half, active);
        if constexpr (SpatialStatsEnabled)
        {
            ++stats.cellsTested;
            stats.cellsFullyInside += result == 0 ? 1 : 0;
        }
        return result;
    }
    return tester.acceptCell(center, half, 0u); // occlusion still prunes inside fully-contained subtrees
}

template <typename Tester, typename ChildFunc>
void SpatialIndexState::forEachChild(const Tester& tester, const TraversalRef& ref, uint64 key, uint32 level,
                                const CellRecord& rec, const __m128i& minFine, const glm::vec3& cellMin, uint32 active,
                                TraverseStats& stats, const ChildFunc& onChild) const
{
    const uint32 childLevel = level - 1; // never the top level: its loose bound is the plain half cell
    const CellMap& cells = m_levels[childLevel];
    const float childSize = float(Morton::cellSize(childLevel));
    const float childHalf = childSize * 0.5f;
    const __m128i childShift = _mm_cvtsi32_si128(int32(2 * childLevel));
    // The child's corner: the parent's plus its grid offset on the fine lattice (integer, exact).
    const auto childCorner = [&](uint32 bit, __m128i& childMinFine)
    {
        childMinFine = _mm_add_epi32(minFine, _mm_sll_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(g_childOffsets.cell[bit])), childShift));
        return relMin(ref, childMinFine);
    };
    uint64 childMask = rec.childMask;
    if constexpr (HasClassify8<Tester>)
    {
        // Survivors only get the hash probe and the exact min corner. The batch centers are the
        // parent's float min plus the grid offset - a few float roundings off the exact ones, so
        // the batch tests a box inflated by a bound on that error: still conservative both ways
        // (outside and fully inside are only claimed when the exact box is too).
        const auto visit = [&](uint32 bit, uint32 childActive)
        {
            const uint64 childKey = (key << 6) | bit;
            const CellRecord* child = cells.find(childKey);
            if (!child)
                return;
            __m128i childMinFine;
            const glm::vec3 childMin = childCorner(bit, childMinFine);
            childActive = tester.acceptCell(childMin + glm::vec3(childHalf), glm::vec3(childSize), childActive);
            if (childActive != ClassifyOutside)
                onChild(childKey, *child, childMinFine, childMin, childActive);
        };
        if (active == 0)
        {
            for (; childMask; childMask &= childMask - 1)
                visit(uint32(oc::tzcnt(childMask)), 0u);
            return;
        }
        const glm::vec3 absMin = glm::abs(cellMin);
        const float eps = (glm::max(absMin.x, glm::max(absMin.y, absMin.z)) + 4.0f * childSize) * (1.0f / 1048576.0f);
        const glm::vec3 half = glm::vec3(childSize + eps);
        ChildBatch batch;
        uint32 bits[8];
        uint32 laneActive[8];
        while (childMask)
        {
            uint32 n = 0;
            for (; childMask && n < 8; childMask &= childMask - 1, ++n)
            {
                const uint32 bit = uint32(oc::tzcnt(childMask));
                bits[n] = bit;
                batch.cx[n] = cellMin.x + (g_childOffsets.x[bit] * childSize + childHalf);
                batch.cy[n] = cellMin.y + (g_childOffsets.y[bit] * childSize + childHalf);
                batch.cz[n] = cellMin.z + (g_childOffsets.z[bit] * childSize + childHalf);
            }
            for (uint32 lane = n; lane < 8; ++lane) // pad with a copy; masked off below
            {
                batch.cx[lane] = batch.cx[0];
                batch.cy[lane] = batch.cy[0];
                batch.cz[lane] = batch.cz[0];
            }
            if constexpr (SpatialStatsEnabled)
                stats.cellsTested += int(n);
            uint32 pass = tester.classify8(batch, half, active, laneActive) & ((1u << n) - 1);
            for (; pass; pass &= pass - 1)
            {
                const uint32 lane = uint32(oc::tzcnt(pass));
                if constexpr (SpatialStatsEnabled)
                    stats.cellsFullyInside += laneActive[lane] == 0 ? 1 : 0;
                visit(bits[lane], laneActive[lane]);
            }
        }
    }
    else
    {
        for (; childMask; childMask &= childMask - 1)
        {
            const uint32 bit = uint32(oc::tzcnt(childMask));
            const uint64 childKey = (key << 6) | bit;
            const CellRecord* child = cells.find(childKey);
            if (!child)
                continue;
            __m128i childMinFine;
            const glm::vec3 childMin = childCorner(bit, childMinFine);
            const uint32 childActive = testCell(tester, childMin, childHalf, childLevel, active, stats);
            if (childActive != ClassifyOutside)
                onChild(childKey, *child, childMinFine, childMin, childActive);
        }
    }
}

template <typename Tester, typename EmitFunc>
void SpatialIndexState::emitCellEntries(const Tester& tester, const glm::vec3& cellMin, uint32 layerMask,
                                   const CellRecord& rec, uint32 level, uint32 active,
                                   [[maybe_unused]] TraverseStats& stats, const EmitFunc& emit) const
{
    const BlockStore& store = m_blocks[level];
    const LaneDecode decode = makeLaneDecode(cellMin, level, level == m_numLevels - 1 ? m_topLevelMaxRadius.load(oc::memory_order_relaxed) : 0.0f);
    for (uint32 b = rec.head; b != UINT32_MAX;)
    {
        const CellBlock& block = store.blocks[b];
        b = block.next;
        uint32 hits;
        if (active == 0)
        {
            if constexpr (HasInsideHits<Tester>)
                hits = tester.insideHits(block);
            else
                hits = liveLanes(block) & layerLanes(block.layer, layerMask);
        }
        else if constexpr (HasTest8<Tester>)
        {
            // all 8 lanes: the unused ones carry a tombstone radius and drop out of the mask
            if constexpr (SpatialStatsEnabled)
                stats.entityTests += int(block.count);
            hits = tester.test8(decode, block, layerMask, active);
        }
        else
        {
            hits = 0;
            for (uint32 live = liveLanes(block) & layerLanes(block.layer, layerMask); live; live &= live - 1)
            {
                const uint32 lane = uint32(oc::tzcnt(live));
                if constexpr (SpatialStatsEnabled)
                    ++stats.entityTests;
                if (tester.testEntity(lanePos(decode, block, lane), laneRadius(decode, block, lane)))
                    hits |= 1u << lane;
            }
        }
        if constexpr (SpatialStatsEnabled)
        {
            stats.emitted += int(oc::popcnt(hits & 0xFFu));
            stats.emittedShadow += int(oc::popcnt(hits >> 8));
        }
        if constexpr (Tester::Passes == 1)
        {
            for (; hits; hits &= hits - 1)
                emit(block, uint32(oc::tzcnt(hits)), decode, 1u);
        }
        else
        {
            // two passes: bit `lane` = the first, bit `lane + 8` = the second; emit gets them as bits 0 and 1
            for (uint32 any = (hits | (hits >> 8)) & 0xFFu; any; any &= any - 1)
            {
                const uint32 lane = uint32(oc::tzcnt(any));
                emit(block, lane, decode, ((hits >> lane) & 1u) | ((hits >> (lane + 7)) & 2u));
            }
        }
    }
}

template <typename Tester, typename EmitFunc>
void SpatialIndexState::traverseCell(const Tester& tester, const TraversalRef& ref, uint32 layerMask,
                                uint64 key, uint32 level, const CellRecord& rec, const __m128i& minFine, const glm::vec3& cellMin, uint32 active,
                                TraverseStats& stats, const EmitFunc& emit) const
{
    emitCellEntries(tester, cellMin, layerMask, rec, level, active, stats, emit);
    if (level == 0)
        return;
    forEachChild(tester, ref, key, level, rec, minFine, cellMin, active, stats,
        [&](uint64 childKey, const CellRecord& child, const __m128i& childMinFine, const glm::vec3& childMin, uint32 childActive)
    {
        traverseCell(tester, ref, layerMask, childKey, level - 1, child, childMinFine, childMin, childActive, stats, emit);
    });
}

template <typename Tester, typename EmitFunc>
void SpatialIndexState::traverse(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask, const EmitFunc& emit) const
{
    // Local counters merged once at the end: traversals run concurrently (script queries on worker
    // threads), so per-test increments on the shared stats would race on the hottest path.
    TraverseStats stats;
    const TraversalRef ref = makeRef(refPos);
    const uint32 top = m_numLevels - 1;
    bool direct = false;
    if constexpr (HasBound<Tester>)
    {
        // DIRECT CELL PATH for small bounded queries (the gameplay case: many balls of a few metres):
        // address the cells the volume overlaps per level straight from the key math, and skip
        // levels holding no entries at all - instead of descending from the 8 top cells through
        // every level with a hash probe and a classify per child. The loose bound per level is the
        // same half cell (top: the clamped-oversize radius) the hierarchy uses, so the result set is
        // identical. Falls back to the descent when the volume spans too many cells.
        const glm::vec3 half = tester.boundHalf();
        const float topLevelMaxRadius = m_topLevelMaxRadius.load(oc::memory_order_relaxed);
        struct LevelRange { uint64 x0, x1, y0, y1, z0, z1; };
        LevelRange ranges[Morton::MaxLevels];
        uint32 total = 0;
        for (uint32 level = 0; level < m_numLevels; ++level)
        {
            if (m_levelEntityCount[level] == 0)
                continue;
            const float halfCell = float(Morton::cellSize(level) * 0.5);
            const float loose = level == top && topLevelMaxRadius > halfCell ? topLevelMaxRadius : halfCell;
            const glm::dvec3 lo = refPos - glm::dvec3(half + glm::vec3(loose));
            const glm::dvec3 hi = refPos + glm::dvec3(half + glm::vec3(loose));
            const uint32 shift = 2 * level;
            LevelRange& r = ranges[level];
            r.x0 = quantizeAxisClamped(lo.x) >> shift; r.x1 = quantizeAxisClamped(hi.x) >> shift;
            r.y0 = quantizeAxisClamped(lo.y) >> shift; r.y1 = quantizeAxisClamped(hi.y) >> shift;
            r.z0 = quantizeAxisClamped(lo.z) >> shift; r.z1 = quantizeAxisClamped(hi.z) >> shift;
            total += uint32((r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) * (r.z1 - r.z0 + 1));
            if (total > DirectCellLimit)
                break;
        }
        if (total <= DirectCellLimit)
        {
            direct = true;
            for (uint32 level = 0; level < m_numLevels; ++level)
            {
                if (m_levelEntityCount[level] == 0)
                    continue;
                const LevelRange& r = ranges[level];
                const CellMap& cells = m_levels[level];
                const float halfCell = float(Morton::cellSize(level) * 0.5);
                const uint32 shift = 2 * level;
                for (uint64 z = r.z0; z <= r.z1; ++z)
                    for (uint64 y = r.y0; y <= r.y1; ++y)
                        for (uint64 x = r.x0; x <= r.x1; ++x)
                        {
                            const uint64 key = Morton::encode({ x, y, z }); // level coords encode to the level key directly
                            const CellRecord* rec = cells.find(key);
                            if (!rec || rec->count == 0)
                                continue;
                            const glm::vec3 cellMin = relMin(ref, _mm_set_epi32(0, int32(z << shift), int32(y << shift), int32(x << shift)));
                            const uint32 active = testCell(tester, cellMin, halfCell, level, Tester::AllActive, stats);
                            if (active != ClassifyOutside)
                                emitCellEntries(tester, cellMin, layerMask, *rec, level, active, stats, emit);
                        }
            }
        }
    }
    if (!direct)
    {
        const float topHalf = float(Morton::cellSize(top) * 0.5);
        m_levels[top].forEachCell([&](uint64 key, const CellRecord& rec)
        {
            const __m128i minFine = minFineOf(key, top);
            const glm::vec3 cellMin = relMin(ref, minFine);
            const uint32 active = testCell(tester, cellMin, topHalf, top, Tester::AllActive, stats);
            if (active != ClassifyOutside)
                traverseCell(tester, ref, layerMask, key, top, rec, minFine, cellMin, active, stats, emit);
        });
    }
    if constexpr (SpatialStatsEnabled)
    {
        m_stats.cellsTested += stats.cellsTested;
        m_stats.cellsFullyInside += stats.cellsFullyInside;
        m_stats.entityTests += stats.entityTests;
    }
}

// The markVisible* fan-out. The hierarchy root is a handful of huge top-level cells, so
// parallelizing over THEM is no parallelism at all - instead the frontier expansion walks the top
// of the tree serially (classifying cells, emitting the upper cells' own few entries, collecting
// child cells) until it holds enough independent subtree roots to feed every worker, then
// parallelFors traverseCell over the roots. Each root's subtree is disjoint, an entry lives in
// exactly one cell, and the cell maps only mutate in commitFrame - so the only shared writes are
// the emit itself (thread-safe stamps) and the stats, which accumulate per chunk and merge once.
template <typename Tester, typename EmitFunc, typename PrepareFunc>
void SpatialIndexState::traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                                    TraverseStats& stats, const EmitFunc& emit, const PrepareFunc& prepareChunks)
{
    const TraversalRef ref = makeRef(refPos);
    const uint32 top = m_numLevels - 1;
    const float topHalf = float(Morton::cellSize(top) * 0.5);

    constexpr bool ChunkAware = std::is_invocable_v<EmitFunc, const CellBlock&, uint32, const LaneDecode&, uint32, uint32>;
    const auto serialEmit = [&](const CellBlock& block, uint32 lane, const LaneDecode& decode, uint32 passes)
    {
        if constexpr (ChunkAware)
            emit(block, lane, decode, passes, 0u);
        else
            emit(block, lane, decode, passes);
    };
    prepareChunks(1u); // slot 0 exists before the expansion emits
    // 4x over-partitioned so the shared parallelFor cursor can rebalance around expensive roots
    // (the cells around the camera hold most of the visible world).
    const uint32 target = Globals::jobSystem.getNumWorkers() * 4;
    // Frontier cells are classified BEFORE they enter the frontier (the top cells here, children by
    // forEachChild): the expansion and the chunks only emit and descend.
    m_frontier.clear();
    m_levels[top].forEachCell([&](uint64 key, const CellRecord& rec)
    {
        const __m128i minFine = minFineOf(key, top);
        const glm::vec3 cellMin = relMin(ref, minFine);
        const uint32 active = testCell(tester, cellMin, topHalf, top, Tester::AllActive, stats);
        if (active != ClassifyOutside)
            m_frontier.push_back(FrontierCell{ minFine, key, &rec, cellMin, top, active });
    });
    while (!m_frontier.empty() && uint32(m_frontier.size()) < target)
    {
        m_frontierNext.clear();
        bool anySplit = false;
        for (const FrontierCell& fc : m_frontier)
        {
            emitCellEntries(tester, fc.cellMin, layerMask, *fc.rec, fc.level, fc.active, stats, serialEmit);
            if (fc.level == 0)
                continue; // no children: fully consumed by the expansion
            forEachChild(tester, ref, fc.key, fc.level, *fc.rec, fc.minFine, fc.cellMin, fc.active, stats,
                [&](uint64 childKey, const CellRecord& child, const __m128i& childMinFine, const glm::vec3& childMin, uint32 childActive)
            {
                m_frontierNext.push_back(FrontierCell{ childMinFine, childKey, &child, childMin, fc.level - 1, childActive });
                anySplit = true;
            });
        }
        m_frontier.swap(m_frontierNext);
        if (!anySplit)
            break; // every surviving cell was a leaf: nothing left for the parallel phase
    }
    if (m_frontier.empty())
        return;

    // A chunk-aware emit gets slot 0 for the serial expansion above and 1 + the chunk's first
    // frontier index for the fan-out: an owner-sliced list per chunk.
    prepareChunks(uint32(m_frontier.size()) + 1);
    [[maybe_unused]] oc::atomic<int> cellsTested = 0, cellsFullyInside = 0, entityTests = 0, emitted = 0, emittedShadow = 0;
    Globals::jobSystem.parallelFor(0, uint32(m_frontier.size()), 1,
        JobProfile{ "Spatial traverse chunk", EProfileCategory::Spatial },
        [&](uint32 begin, uint32 end)
    {
        TraverseStats local;
        const auto chunkEmit = [&](const CellBlock& block, uint32 lane, const LaneDecode& decode, uint32 passes)
        {
            if constexpr (ChunkAware)
                emit(block, lane, decode, passes, begin + 1);
            else
                emit(block, lane, decode, passes);
        };
        for (uint32 i = begin; i < end; ++i)
        {
            const FrontierCell& fc = m_frontier[i];
            traverseCell(tester, ref, layerMask, fc.key, fc.level, *fc.rec, fc.minFine, fc.cellMin, fc.active, local, chunkEmit);
        }
        if constexpr (SpatialStatsEnabled)
        {
            cellsTested.fetch_add(local.cellsTested, oc::memory_order_relaxed);
            cellsFullyInside.fetch_add(local.cellsFullyInside, oc::memory_order_relaxed);
            entityTests.fetch_add(local.entityTests, oc::memory_order_relaxed);
            emitted.fetch_add(local.emitted, oc::memory_order_relaxed);
            emittedShadow.fetch_add(local.emittedShadow, oc::memory_order_relaxed);
        }
    }, EJobPriority::High); // pre-frame critical path: the render gate waits on these stamps

    if constexpr (SpatialStatsEnabled)
    {
        stats.cellsTested += cellsTested.load(oc::memory_order_relaxed);
        stats.cellsFullyInside += cellsFullyInside.load(oc::memory_order_relaxed);
        stats.entityTests += entityTests.load(oc::memory_order_relaxed);
        stats.emitted += emitted.load(oc::memory_order_relaxed);
        stats.emittedShadow += emittedShadow.load(oc::memory_order_relaxed);
    }
}

uint32 SpatialIndex::querySphere(const glm::dvec3& center, float radius, uint32 layerMask, oc::vector<uint64>& outUserData) const
{
    outUserData.clear();
    // No lock: a concurrent registration (parallel entity spawning; a spawner's script OnSpawn queries while
    // another registers) never moves the pool rows (RecordPool) and the cell maps change only at commit.
    traverse(SphereTester{ radius }, center, layerMask,
        [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { outUserData.push_back(block.userData[lane]); });
    return uint32(outUserData.size());
}

void SpatialIndexState::forEachInSphereImpl(const glm::dvec3& center, float radius, uint32 layerMask, uint32 skipStampedIn, void* ctx, void (*emit)(void*, uint64)) const
{
    if (skipStampedIn == 0)
    {
        traverse(SphereTester{ radius }, center, layerMask,
            [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { emit(ctx, block.userData[lane]); });
        return;
    }
    traverse(SphereTester{ radius }, center, layerMask, [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32)
    {
        if (!(passBits(currentLanes(stampLanes(block.poolIdx[lane]))) & skipStampedIn))
            emit(ctx, block.userData[lane]);
    });
}

void SpatialIndexState::forEachInFrustumImpl(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                                        IOcclusionTester* occlusion, void* ctx, void (*emit)(void*, uint64)) const
{
    traverse(FrustumTester{ frustumRelCamera, occlusion, maxDist }, cameraPos, layerMask,
        [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { emit(ctx, block.userData[lane]); });
}

uint32 SpatialIndex::queryAABB(const glm::dvec3& boxMin, const glm::dvec3& boxMax, uint32 layerMask, oc::vector<uint64>& outUserData) const
{
    outUserData.clear();
    const glm::dvec3 center = (boxMin + boxMax) * 0.5;
    traverse(AABBTester{ glm::vec3((boxMax - boxMin) * 0.5) }, center, layerMask,
        [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { outUserData.push_back(block.userData[lane]); });
    return uint32(outUserData.size());
}

uint32 SpatialIndex::queryFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                                  oc::vector<uint64>& outUserData, IOcclusionTester* occlusion) const
{
    outUserData.clear();
    traverse(FrustumTester{ frustumRelCamera, occlusion, maxDist }, cameraPos, layerMask,
        [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { outUserData.push_back(block.userData[lane]); });
    return uint32(outUserData.size());
}

uint32 SpatialIndex::queryRay(const glm::dvec3& origin, const glm::dvec3& dir, double maxDist, uint32 layerMask,
                              oc::vector<uint64>& outUserData) const
{
    outUserData.clear();
    const glm::dvec3 normalized = glm::normalize(dir);
    traverse(RayTester{ glm::vec3(normalized), float(maxDist) }, origin, layerMask,
        [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32) { outUserData.push_back(block.userData[lane]); });
    return uint32(outUserData.size());
}

uint64 SpatialIndex::queryNearest(const glm::dvec3& pos, float maxRadius, uint32 layerMask, uint64 excludeUserData) const
{
    uint64 best = 0;
    float bestDist2 = FLT_MAX;
    bool found = false;
    const auto collectNearest = [&](const CellBlock& block, uint32 lane, const LaneDecode& decode, uint32)
    {
        const uint64 userData = block.userData[lane];
        if (userData == excludeUserData)
            return;
        const glm::vec3 rel = lanePos(decode, block, lane);
        const float dist2 = glm::dot(rel, rel);
        if (dist2 < bestDist2)
        {
            bestDist2 = dist2;
            best = userData;
            found = true;
        }
    };
    float searchRadius = glm::min(maxRadius, float(Morton::cellSize(1)));
    while (true)
    {
        traverse(SphereTester{ searchRadius }, pos, layerMask, collectNearest);
        if (found || searchRadius >= maxRadius)
            break;
        searchRadius = glm::min(searchRadius * 4.0f, maxRadius);
    }
    if (found && bestDist2 > searchRadius * searchRadius)
    {
        // best came from an oversized bound sticking into the search ball; anything with a nearer
        // center is guaranteed found by searching out to that center distance
        traverse(SphereTester{ glm::min(glm::sqrt(bestDist2), maxRadius) }, pos, layerMask, collectNearest);
    }
    return found ? best : 0;
}

// Both markVisible* run the parallel fan-out (traverseParallel). The stamp is a PURE STORE and
// thread-safe by structure: an entry lives in exactly one cell, so no two subtree roots ever write
// the same slot. The visible counts ride TraverseStats (SPATIAL_STATS builds only) - accumulated per
// chunk and merged once, never a shared per-entry atomic on the emit path.
template <typename Tester>
void SpatialIndexState::stampFrustum(const Tester& tester, const glm::dvec3& cameraPos, uint32 layerMask, bool collect, TraverseStats& stats)
{
    PassStamps* stamps = m_pool.stamps;
    const SpatialStamp mainId = m_visibleQueryId[uint32(ESpatialPass::Main)];
    const SpatialStamp shadowId = m_visibleQueryId[uint32(ESpatialPass::Shadow)];
    // passes: bit 0 Main, bit 1 Shadow (a single-pass tester always emits 1, so the Shadow store folds away)
    const auto stampPasses = [stamps, mainId, shadowId](uint32 idx, uint32 passes)
    {
        if (passes & 1u)
            stamps[idx].pass[uint32(ESpatialPass::Main)] = mainId;
        if (passes & 2u)
            stamps[idx].pass[uint32(ESpatialPass::Shadow)] = shadowId;
    };
    if (!collect)
    {
        traverseParallel(tester, cameraPos, layerMask, stats,
            [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32 passes) { stampPasses(block.poolIdx[lane], passes); },
            [](uint32) {});
        return;
    }
    // Stamp + collect (see setVisibleCollect): the chunk-aware emit appends the Main hits to its own list of every slot whose layers
    // the hit carries.
    const auto stampCollect = [&](const CellBlock& block, uint32 lane, const LaneDecode&, uint32 passes, uint32 chunk)
    {
        const uint32 idx = block.poolIdx[lane];
        stampPasses(idx, passes);
        if (!(passes & 1u))
            return;
        const uint32 layers = block.layer[lane];
        for (uint32 slot = 0; slot < NumVisibleCollectSlots; ++slot)
        {
            if (!(layers & m_visibleCollectLayers[slot]))
                continue;
            if (m_visibleCollectWhat[slot] == ECollect::Handles)
                m_visibleCollectChunks[slot][chunk].push_back(SpatialHandle{ idx, m_pool.gen[idx] });
            else if (const uint64 userData = block.userData[lane])
                m_visibleCollectUserDataChunks[slot][chunk].push_back(userData);
        }
    };
    const auto prepare = [this](uint32 slots)
    {
        for (uint32 slot = 0; slot < NumVisibleCollectSlots; ++slot)
        {
            if (m_visibleCollectWhat[slot] == ECollect::Handles)
            {
                if (m_visibleCollectChunks[slot].size() < slots)
                    m_visibleCollectChunks[slot].resize(slots);
            }
            else if (m_visibleCollectUserDataChunks[slot].size() < slots)
                m_visibleCollectUserDataChunks[slot].resize(slots);
        }
    };
    traverseParallel(tester, cameraPos, layerMask, stats, stampCollect, prepare);
}

void SpatialIndexState::markVisibleSet(const Frustum& mainFrustum, const glm::dvec3& cameraPos, float mainMaxDist, uint32 mainLayers,
                                       IOcclusionTester* occlusion, const Frustum* shadowFrustum, float shadowMaxDist, uint32 shadowLayers)
{
    [[maybe_unused]] const auto start = SpatialStatsEnabled ? Clock::now() : Clock::time_point();
    self().advanceStamp(ESpatialPass::Main);
    if (shadowFrustum)
        self().advanceStamp(ESpatialPass::Shadow);
    uint32 collectAny = 0;
    for (uint32 layers : m_visibleCollectLayers)
        collectAny |= layers;
    if (collectAny)
        for (uint32 slot = 0; slot < NumVisibleCollectSlots; ++slot)
        {
            for (oc::vector<SpatialHandle>& chunk : m_visibleCollectChunks[slot])
                chunk.clear();
            for (oc::vector<uint64>& chunk : m_visibleCollectUserDataChunks[slot])
                chunk.clear();
            m_visibleCollected[slot].clear();
            m_visibleCollectedUserData[slot].clear();
        }
    TraverseStats stats;
    if (shadowFrustum)
        stampFrustum(DualFrustumTester{ mainFrustum, mainMaxDist, mainLayers, *shadowFrustum, shadowMaxDist, shadowLayers, occlusion },
            cameraPos, mainLayers | shadowLayers, collectAny != 0, stats);
    else
        stampFrustum(FrustumTester{ mainFrustum, occlusion, mainMaxDist }, cameraPos, mainLayers, collectAny != 0, stats);
    if (collectAny)
    {
        for (uint32 slot = 0; slot < NumVisibleCollectSlots; ++slot)
        {
            for (const oc::vector<SpatialHandle>& chunk : m_visibleCollectChunks[slot])
                m_visibleCollected[slot].insert(m_visibleCollected[slot].end(), chunk.begin(), chunk.end());
            for (const oc::vector<uint64>& chunk : m_visibleCollectUserDataChunks[slot])
                m_visibleCollectedUserData[slot].insert(m_visibleCollectedUserData[slot].end(), chunk.begin(), chunk.end());
        }
        ++m_visibleCollectGeneration;
    }
    if constexpr (SpatialStatsEnabled)
    {
        m_stats.cellsTested += stats.cellsTested;
        m_stats.cellsFullyInside += stats.cellsFullyInside;
        m_stats.entityTests += stats.entityTests;
        m_stats.visiblePerPass[uint32(ESpatialPass::Main)] = stats.emitted;
        if (shadowFrustum)
            m_stats.visiblePerPass[uint32(ESpatialPass::Shadow)] = stats.emittedShadow;
        m_stats.markVisibleMs += std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    }
}

void SpatialIndexState::markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask)
{
    [[maybe_unused]] const auto start = SpatialStatsEnabled ? Clock::now() : Clock::time_point();
    const uint32 passIdx = uint32(pass);
    self().advanceStamp(pass);
    const SpatialStamp stampId = m_visibleQueryId[passIdx];
    PassStamps* stamps = m_pool.stamps;
    const auto stamp = [stamps, passIdx, stampId](const CellBlock& block, uint32 lane, const LaneDecode&, uint32)
    {
        stamps[block.poolIdx[lane]].pass[passIdx] = stampId;
    };
    TraverseStats stats;
    traverseParallel(SphereTester{ radius }, center, layerMask, stats, stamp, [](uint32) {});
    if constexpr (SpatialStatsEnabled)
    {
        m_stats.cellsTested += stats.cellsTested;
        m_stats.cellsFullyInside += stats.cellsFullyInside;
        m_stats.entityTests += stats.entityTests;
        m_stats.visiblePerPass[passIdx] = stats.emitted;
        m_stats.markVisibleMs += std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    }
}

static constexpr ESpatialPass g_updateTierPass[3] = { ESpatialPass::UpdateTier0, ESpatialPass::UpdateTier1, ESpatialPass::UpdateTier2 };

void SpatialIndex::advanceStamp(ESpatialPass pass)
{
    SpatialStamp& id = m_visibleQueryId[uint32(pass)];
    if (++id == SpatialStamp_Indexed) // the sentinel is never a generation
    {
        // WRAP (every 65k stamps of this pass): every stale value in the row would become "current"
        // again ~65k generations later - sweep them to the sentinel (0, the spawn guard, stays 0).
        // One pass over the pool, on the thread advancing the pass (no traversal of this pass runs
        // then). A concurrent registerEntry may write a fresh slot's stamps meanwhile: both writes mean
        // "never stamped", so either order is fine, and capacity() never passes the committed rows.
        id = 1;
        const uint32 count = m_pool.capacity();
        for (uint32 idx = 0; idx < count; ++idx)
            if (m_pool.stamps[idx].pass[uint32(pass)] != 0)
                m_pool.stamps[idx].pass[uint32(pass)] = SpatialStamp_Indexed;
    }
}

void SpatialIndex::advanceUpdateTiers()
{
    for (const ESpatialPass pass : g_updateTierPass)
        advanceStamp(pass);
    advanceStamp(ESpatialPass::UpdateRoot);
}

const oc::vector<oc::vector<uint64>>& SpatialIndex::queryUpdateTiers(const glm::dvec3& center, float queryRadius, const float tierRadius[3],
                                                                      bool horizontal, uint32 layerMask)
{
    for (oc::vector<uint64>& chunk : m_tierHitChunks)
        chunk.clear();
    SpatialStamp tierId[3];
    float tierR2[3];
    for (int t = 0; t < 3; ++t)
    {
        tierId[t] = m_visibleQueryId[uint32(g_updateTierPass[t])];
        tierR2[t] = tierRadius[t] > 0.0f ? tierRadius[t] * tierRadius[t] : -1.0f; // -1: no distance is below it
    }
    // Outside the kick/join window, concurrent with spawning: safe without a lock - a registration never
    // moves the pool rows (RecordPool) and the cell maps change only at commit.
    const auto stampCollect = [&](const CellBlock& block, uint32 lane, const LaneDecode& decode, uint32, uint32 chunk)
    {
        const glm::vec3 pos = lanePos(decode, block, lane);
        const float d2 = horizontal ? pos.x * pos.x + pos.z * pos.z : glm::dot(pos, pos);
        PassStamps& stamps = m_pool.stamps[block.poolIdx[lane]];
        for (int t = 0; t < 3; ++t)
            if (d2 < tierR2[t])
                stamps.pass[uint32(g_updateTierPass[t])] = tierId[t];
        m_tierHitChunks[chunk].push_back(block.userData[lane]);
    };
    const auto prepare = [this](uint32 slots) { if (m_tierHitChunks.size() < slots) m_tierHitChunks.resize(slots); };
    TraverseStats stats;
    traverseParallel(SphereTester{ queryRadius }, center, layerMask, stats, stampCollect, prepare);
    if constexpr (SpatialStatsEnabled)
    {
        m_stats.cellsTested += stats.cellsTested;
        m_stats.cellsFullyInside += stats.cellsFullyInside;
        m_stats.entityTests += stats.entityTests;
    }
    return m_tierHitChunks;
}

void SpatialIndexState::update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera, const glm::vec3& sunDirection)
{
    ProfileScope updateScope("Spatial", EProfileCategory::Spatial);

    self().commitFrame();   // applies cell moves queued during last frame's entity updates
    // The SIM LOD tier stamps are NOT made here: the World's selection job stamps them
    // (queryUpdateTiers), off the frame-critical path.
    m_culling.maxDist = camera.far; // cull to exactly the view distance, not a fixed cap
    if (m_culling.mode == int(ESpatialCullMode::Off) || m_culling.freeze)
        return;

    const glm::dvec3 cameraPos = glm::dvec3(camera.position);
    Frustum cullFrustum = rebaseFrustum(frustum, cameraPos);
    inflateFrustum(cullFrustum, m_culling.margin);

    IOcclusionTester* occlusion = nullptr;
    if (Globals::occlusionBuffer.isEnabled())
    {
        ProfileScope profileScope("Occlusion raster", EProfileCategory::Spatial);
        // The renderer's projection is REVERSED-Z; flip the z row back to standard orientation
        // (z' = w - z) so the rasterizer's internal comparisons keep their meaning.
        glm::mat4 standardZ = viewProjRelCamera;
        for (int c = 0; c < 4; ++c)
            standardZ[c][2] = standardZ[c][3] - standardZ[c][2];
        Globals::occlusionBuffer.render(standardZ, cameraPos);
        occlusion = &Globals::occlusionBuffer;
    }

    // SHADOW pass: a caster shadows the visible ground when it sits UP-SUN of it, so the set that
    // matters is the view frustum swept toward the sun on the horizontal plane by shadowReach - the
    // Minkowski sum of the (inflated, camera-relative) frustum with that segment. A convex volume
    // swept along a segment keeps its face normals; only the planes facing AGAINST the sweep move,
    // by reach * -(n . s). No occlusion test: a caster hidden behind a wall still casts. The swept
    // frustum contains the Main one, so both stamp in ONE traversal (DualFrustumTester).
    const float reach = m_culling.shadowReach;
    const glm::vec3 sunPlanar(sunDirection.x, 0.0f, sunDirection.z);
    const bool shadowOn = reach > 0.0f && glm::dot(sunPlanar, sunPlanar) > 1e-6f;
    Frustum shadowFrustum = cullFrustum;
    if (shadowOn)
    {
        const glm::vec3 sweep = glm::normalize(sunPlanar);
        for (glm::vec4& plane : shadowFrustum.planes)
        {
            const float facing = glm::dot(glm::vec3(plane), sweep);
            if (facing < 0.0f)
                plane.w -= facing * reach;
        }
    }

    // Terrain chunks and ocean sectors ride the Main stamp too (both on SpatialLayer_Terrain); they
    // skip the Near ball and the Shadow pass - main-culled terrain keeps its shadow/GI passes
    // unconditionally, and the ocean is PASS_MAIN only.
    {
        ProfileScope profileScope("Mark visible", EProfileCategory::Spatial);
        const float mainMaxDist = m_culling.maxDist + m_culling.margin;
        markVisibleSet(cullFrustum, cameraPos, mainMaxDist, SpatialLayer_Render | SpatialLayer_Terrain, occlusion,
            shadowOn ? &shadowFrustum : nullptr, mainMaxDist + reach, SpatialLayer_Render);
    }

    // The Near ball barely changes frame to frame: inflate it by the slack and requery only once the
    // camera has moved that far. The frame cap keeps off-screen MOVERS from staying unstamped too long
    // (they can enter the ball without the camera moving).
    const float nearSlack = m_culling.nearSlack;
    ++m_framesSinceNearQuery;
    if (nearSlack <= 0.0f || glm::distance(m_lastNearQueryPos, cameraPos) > double(nearSlack) || m_framesSinceNearQuery >= 30)
    {
        markVisibleSphere(ESpatialPass::Near, cameraPos, m_culling.nearRadius + nearSlack, SpatialLayer_Render);
        m_lastNearQueryPos = cameraPos;
        m_framesSinceNearQuery = 0;
    }
}

void SpatialIndex::kickUpdateJob(const CullView& view)
{
    ProfileScope scope("Spatial kick", EProfileCategory::Spatial);

    m_updateJobKicked = view.valid;
    if (!view.valid) // first VR frame: no head view yet - skip this frame's cull (see the declaration)
        return;
    m_updateJobCamera = view.camera;
    m_updateJobFrustum = view.frustum;
    m_updateJobViewProj = view.viewProjRelCamera;
    m_updateJobSunDirection = view.sunDirection;
    Globals::jobSystem.submit([this] { update(m_updateJobCamera, m_updateJobFrustum, m_updateJobViewProj, m_updateJobSunDirection); },
        { "Spatial cull", EProfileCategory::Spatial }, EJobPriority::High, &m_updateJobCounter);
}

void SpatialIndex::joinUpdateJob()
{
    if (m_updateJobKicked)
        Globals::jobSystem.wait(m_updateJobCounter); // helps; near-zero when the kick-to-join work covered it
}
