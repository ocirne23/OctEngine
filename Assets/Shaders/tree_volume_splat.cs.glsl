#version 460

// FAR-TREE VOLUME BAKE, step 1 of 2 (TreeVolumePipeline): one workgroup per placed tree ADDS the tree's type
// extinction volume into the camera-centred polar volume (tree_volume.inc.glsl), fixed point through imageAtomicAdd -
// overlapping crowns sum, as extinction does. Every volume texel the tree's box touches samples the type volume
// at the mip that matches the texel's footprint (in tree space), at the texel centre clamped into the box, x the
// fraction of the texel the box covers: a texel larger than the tree still gets the tree's share instead of
// missing it. The texel's colour column takes the tree type's albedo (last writer wins - per-tree speckle).
// The type volumes: per type a res^3 float mip chain (x fastest, then y, then z), the mips following level 0.
//
// TREE_FLOOR_PASS 1 and 2 (dispatched first, in that order): the same footprint walk, per COLUMN instead of per
// voxel, setting the column's FLOOR (tree_volume.inc.glsl) that the splat measures its slices from. The floor is the
// base of the column's DOMINANT tree - the one whose tent covers the column most (pass 1: the largest coverage per
// column, atomic max; pass 2: the trees at that coverage write their base, atomic minimum among ties). The lowest
// base of any tree reaching the column put an upper tree at a cliff edge above the layer in its edge columns, where
// the splat moved it DOWN: blobs out of line with their billboards along every cliff. A ring column no tree covers
// (coverage 0) keeps the lowest base of the trees whose rectangle reaches it.
//
// TREE_SPLAT_RECORDS (all three passes): the trees come from the WORLD TREE RECORDS (Procedural TreeWorld, W4) instead
// of a tree set: one workgroup per record of the chunks within the record detail distance of the bake centre (the
// chunks beyond add mass per column - tree_volume_records.cs, the same test), each record EXPANDED exactly as Procedural
// TreeSystem::expandChunk does - variant, scale and yaw from its seed, its bushes - on the terrain map's ground, into
// the world set's volume types. The tree set's own copies of those trees are never splatted: one source, so a chunk
// entering or leaving the set does not change the volume.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#include "shared.inc.glsl"
#include "tree_volume.inc.glsl"
#ifdef TREE_SPLAT_RECORDS
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#define TREE_RECORD_GROUND
#include "tree_record.inc.glsl"
#endif

layout (local_size_x = 64) in;

layout (binding = 2, r32ui) uniform uimage3D u_accum;
layout (binding = 3, rgba8) uniform writeonly image2D u_colour;
layout (binding = 4, r32ui) uniform uimage2D u_floor;
layout (binding = 5, r32ui) uniform uimage2D u_floorCover; // the largest tree coverage per column (floor pass 1)

// A tree's coverage of a column as an orderable key: 0 = only the rectangle's ring reaches it.
uint coverKey(float coverXZ) { return coverXZ > 0.0 ? 1u + uint(clamp(coverXZ, 0.0, 1.0) * 65534.0) : 0u; }

// The layouts mirror TreeVolumePipeline.ixx (TreeVolumeTypeGpu / TreeVolumePieceGpu).
struct VolumeType
{
    vec3 boxMin;
    uint offset; // float index of level 0 in the data buffer
    vec3 boxMax;
    uint res;    // 0 = no volume (the type adds nothing)
    vec4 albedo;
};
struct VolumePiece
{
    vec4 posScale;
    vec4 quat;
    uint type;
    uint pad0, pad1, pad2;
};
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer PieceList { VolumePiece p[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TypeList { VolumeType t[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer FloatList { float v[]; };

// Mirrors TreeRecordChunkGpu / TreeRecordTypeGpu (RendererVK TreeRecordPool.ixx).
struct RecordChunk
{
    ivec2 coord;
    uint first;
    uint count;
};
struct RecordType
{
    vec4 albedo;
    float mass;
    float height;
    float radius;
    float sizeVariation;
    vec2 scale;
    uint numVariants;
    uint numBushes;
    float bushesPerTree;
    float bushRadius;
    float pad0, pad1;
    uint variantType[8];
    float variantMass[8];
    uint bushTypes[8];
    float shape[32];
};
// A detail chunk (TreeVolumePipeline::bake): coord.xy, its first record's pool word, its first record's workgroup.
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer DetailList { uvec4 d[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RecordTypeList { RecordType t[]; };
#ifndef TREE_SPLAT_RECORDS
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TreeRecordWords { uint w[]; };
#endif

layout (push_constant, scalar) uniform Push
{
#ifdef TREE_SPLAT_RECORDS
    TreeRecordMap map;   // the chunk map (the records' ground)
#else
    PieceList pieces;
#endif
    TypeList types;      // TREE_SPLAT_RECORDS: the world set's
    FloatList data;
    uint numPieces;      // TREE_SPLAT_RECORDS: the detail chunks' records (one workgroup each)
    uint mapSize;        // TREE_SPLAT_RECORDS: the chunk map's size
    TreeVolumeParams vol;
    // TREE_SPLAT_RECORDS only:
    TreeRecordWords records; // each chunk's ground, then its records
    DetailList detailChunks;
    RecordTypeList recordTypes;
    float chunkSize;
    uint worldSeed;
    uint numDetailChunks;
    uint numRecordTypes;
    uint wgOffset;       // this dispatch's first record (the bake spreads the records over frames)
} pc;

// Fixed point of the accumulation: extinction (1/m) x this.
const float ACCUM_SCALE = 1024.0;

vec3 quatRotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

uint mipOffset(VolumeType t, uint mip)
{
    uint offset = t.offset;
    for (uint m = 0u; m < mip; ++m)
    {
        const uint r = max(t.res >> m, 1u);
        offset += r * r * r;
    }
    return offset;
}

// Trilinear sample of a type's mip at f = box-normalized position (0..1).
float sampleType(VolumeType t, vec3 f, uint mip)
{
    const uint r = max(t.res >> mip, 1u);
    const uint base = mipOffset(t, mip);
    const vec3 p = clamp(f * float(r) - 0.5, vec3(0.0), vec3(float(r - 1u)));
    const uvec3 i0 = uvec3(floor(p));
    const uvec3 i1 = min(i0 + 1u, uvec3(r - 1u));
    const vec3 w = p - vec3(i0);
    float s = 0.0;
    for (uint k = 0u; k < 8u; ++k)
    {
        const uvec3 c = uvec3((k & 1u) != 0u ? i1.x : i0.x, (k & 2u) != 0u ? i1.y : i0.y, (k & 4u) != 0u ? i1.z : i0.z);
        const float wk = ((k & 1u) != 0u ? w.x : 1.0 - w.x) * ((k & 2u) != 0u ? w.y : 1.0 - w.y) * ((k & 4u) != 0u ? w.z : 1.0 - w.z);
        s += wk * pc.data.v[base + c.x + r * (c.y + r * c.z)];
    }
    return s;
}

// The fraction of [c - h, c + h] inside [lo, hi].
float overlap(float c, float h, float lo, float hi) { return clamp(min(hi, c + h) - max(lo, c - h), 0.0, 2.0 * h) / (2.0 * h); }

// The TENT weight of a texel for a body over [lo, hi] (texel centre at 0, tent half-width h = one cell): the
// integral of max(0, 1 - |x| / h) over the body, / h. A tent splat with the trilinear reconstruction keeps a small
// body's centroid AT the body whatever the grid's offset; a box splat moved it to the cell centre (up to half a
// cell off), so every re-bake - a new grid centre - shifted every blob ("shaking").
float tentPrimitive(float x, float h)
{
    if (x <= -h) return 0.0;
    if (x >= h) return h;
    return x <= 0.0 ? (x + h) * (x + h) / (2.0 * h) : h - (h - x) * (h - x) / (2.0 * h);
}
float tentWeight(float lo, float hi, float h) { return (tentPrimitive(hi, h) - tentPrimitive(lo, h)) / h; }

// One tree, by the whole workgroup (its texels spread over the threads).
void splatPiece(VolumePiece piece)
{
    const VolumeType type = pc.types.t[piece.type];
    if (type.res == 0u)
        return;
    const float scale = piece.posScale.w;
    const vec4 invQuat = vec4(-piece.quat.xyz, piece.quat.w);

    // The tree's world AABB (its box's 8 corners).
    vec3 lo = vec3(1e30), hi = vec3(-1e30);
    for (uint k = 0u; k < 8u; ++k)
    {
        const vec3 c = vec3((k & 1u) != 0u ? type.boxMax.x : type.boxMin.x, (k & 2u) != 0u ? type.boxMax.y : type.boxMin.y,
            (k & 4u) != 0u ? type.boxMax.z : type.boxMin.z);
        const vec3 w = piece.posScale.xyz + quatRotate(piece.quat, c * scale);
        lo = min(lo, w);
        hi = max(hi, w);
    }
    // The box in polar terms: the radius range of its xz rectangle, the angle range of the circle around it.
    const vec2 midXZ = 0.5 * (lo.xz + hi.xz) - pc.vol.centre;
    const float halfDiag = 0.5 * length(hi.xz - lo.xz);
    const float rMid = length(midXZ);
    const float rLo = max(rMid - halfDiag, 0.0), rHi = rMid + halfDiag;
    if (rHi < pc.vol.rMin || rLo > pc.vol.rMax)
        return; // outside the volume
    const int radialRes = int(pc.vol.radialRes), angularRes = int(pc.vol.angularRes);
    const int r0 = clamp(int(floor(tvRadialUv(rLo, pc.vol) * float(radialRes))), 0, radialRes - 1);
    const int r1 = clamp(int(floor(tvRadialUv(rHi, pc.vol) * float(radialRes))), 0, radialRes - 1);
    // One texel more on every side: the tent reaches the neighbours. A box AROUND the centre (from the air the inner
    // radius is small, and a tree under the camera surrounds it) takes every angle.
    const bool around = rMid <= halfDiag;
    const float thetaMid = atan(midXZ.y, midXZ.x);
    const float halfAngle = around ? 0.0 : asin(min(halfDiag / rMid, 1.0));
    const int a0 = around ? 0 : int(floor(((thetaMid - halfAngle) / TV_TWO_PI + 0.5) * float(angularRes))) - 1;
    const int a1 = around ? angularRes - 1 : int(floor(((thetaMid + halfAngle) / TV_TWO_PI + 0.5) * float(angularRes))) + 1;
    const ivec2 t0 = ivec2(a0, max(r0 - 1, 0));
    const ivec2 n = ivec2(min(a1 - a0 + 1, angularRes), min(r1 + 1, radialRes - 1) - t0.y + 1);
    // The tree's footprint for the tent: its centre and its horizontal half size (the box's xz extent, rotated
    // about up: the larger half axis), world.
    const vec2 treeXZ = piece.posScale.xz + quatRotate(piece.quat, 0.5 * (type.boxMin + type.boxMax) * scale).xz;
    const float treeHalf = 0.5 * max(type.boxMax.x - type.boxMin.x, type.boxMax.z - type.boxMin.z) * scale;
    const uint slices = pc.vol.slices;
    const float sliceH = pc.vol.height / float(slices);
    const vec3 boxSize = type.boxMax - type.boxMin;
    const float voxel = max(max(boxSize.x, boxSize.y), boxSize.z) / float(type.res);
    const uint maxMip = uint(findMSB(type.res));

#ifdef TREE_FLOOR_PASS
    // The floor goes into the whole footprint rectangle PLUS one more ring, without the tent test: the march filters
    // the density bilinearly across columns but reads the floor of the NEAREST one, so every column the filter can
    // reach from a tree's density must carry a tree floor. One that fell back to the height map (tens of metres off)
    // placed the leaked density at the wrong height - thin hatched spikes above the crowns.
    const ivec2 base = ivec2(t0.x - 1, max(t0.y - 1, 0));
    const ivec2 ext = ivec2(min(n.x + 2, angularRes), min(t0.y + n.y + 1, radialRes) - base.y);
    const uint count = uint(ext.x * ext.y);
#else
    const ivec2 base = t0;
    const ivec2 ext = n;
    const uint count = uint(n.x * n.y) * slices;
#endif
    for (uint i = gl_LocalInvocationID.x; i < count; i += 64u)
    {
#ifdef TREE_FLOOR_PASS
        const uint s = 0u;
        const uint col = i;
#else
        const uint s = i % slices;
        const uint col = i / slices;
#endif
        ivec2 texel = base + ivec2(int(col % uint(ext.x)), int(col / uint(ext.x)));
        texel.x = tvWrapAngle(texel.x, angularRes); // the angle wraps
        const vec2 xz = tvTexelWorldXZ(texel, pc.vol);
        // Horizontally a TENT per polar axis (one cell wide each way, in the texel's radial / tangential frame),
        // vertically the box overlap of the slice (slices are thin against a crown).
        const vec2 rel = xz - pc.vol.centre;
        const float r = length(rel);
        const vec2 eR = rel / r;
        const vec2 eT = vec2(-eR.y, eR.x);
        const vec2 off = vec2(dot(treeXZ - xz, eR), dot(treeXZ - xz, eT)); // the tree centre, texel frame
        const float cellR = tvRadialCell(r, pc.vol);
        const float cellT = tvTangentialCell(r, pc.vol);
        const float coverXZ = tentWeight(off.x - treeHalf, off.x + treeHalf, cellR) * tentWeight(off.y - treeHalf, off.y + treeHalf, cellT);
#if defined(TREE_FLOOR_PASS) && TREE_FLOOR_PASS == 1
        imageAtomicMax(u_floorCover, texel, coverKey(coverXZ));
    }
}
#elif defined(TREE_FLOOR_PASS)
        // WITHIN A FEW STEPS of the column's largest coverage, not equal to it: the two passes are separately compiled
        // variants, and their float math may round a tree's coverage a few bits apart (contraction) - with an exact
        // match no tree matched, the column got no floor, and the splat skipped it: radial gaps in the volume (whole
        // angular columns at some angles, changing with every rebake - a moire).
        if (coverKey(coverXZ) + 4u >= imageLoad(u_floorCover, texel).r)
            imageAtomicMax(u_floor, texel, tvFloorEncode(lo.y)); // the minimum among the dominant trees
    }
}
#else
        if (coverXZ <= 0.0)
            continue;
        // No floor should not happen (the floor passes cover every column the tree reaches) - but if it does, the tree's
        // own base stands in: density a little off in height, never missing.
        const uint floorBits = imageLoad(u_floor, texel).r;
        const float ground = floorBits != 0u ? tvFloorDecode(floorBits) : lo.y;
        // A lower tree set the floor (a slope across the column): this one sits higher in the layer. A tree that would
        // reach past the layer's top moves DOWN into it (as far as its base allows) - off by that much vertically,
        // kilometres out, instead of cut.
        const float shift = max(min(hi.y - ground - pc.vol.height, lo.y - ground), 0.0);
        const vec3 world = vec3(xz.x, ground + (float(s) + 0.5) * sliceH + shift, xz.y);
        if (world.y < lo.y - sliceH || world.y > hi.y + sliceH)
            continue;
        const float halfXZ = 0.5 * max(cellR, cellT) / scale; // tree space, for the footprint mip
        const vec3 local = quatRotate(invQuat, world - piece.posScale.xyz) / scale;
        const float cover = coverXZ * overlap(local.y, 0.5 * sliceH / scale, type.boxMin.y, type.boxMax.y);
        if (cover <= 0.0)
            continue;
        const vec3 halfExt = vec3(halfXZ, 0.5 * sliceH / scale, halfXZ);
        const float footprint = 2.0 * max(halfXZ, halfExt.y);
        const uint mip = min(uint(max(log2(footprint / voxel), 0.0)), maxMip);
        const vec3 f = (clamp(local, type.boxMin, type.boxMax) - type.boxMin) / boxSize;
        // Extinction (1/m) in TREE space -> world: a scaled tree's leaves are spread over a scaled volume.
        const float extinction = sampleType(type, f, mip) * cover / scale;
        if (extinction <= 1e-4)
            continue;
        imageAtomicAdd(u_accum, ivec3(texel, int(s)), uint(extinction * ACCUM_SCALE + 0.5));
        imageStore(u_colour, texel, vec4(type.albedo.rgb, 1.0));
    }
}
#endif

#ifdef TREE_SPLAT_RECORDS
// One plant of a record (its tree, or one of its bushes): Procedural TreeSystem::expandChunk's placeVariant - keep in
// step. The ground is its chunk's height grid (16 m, from the generator's own field - the tree set's copy stands on
// the sampler's 2 m grid); the terrain map only where that chunk holds none.
void splatRecordPlant(uint recordType, uint seed, vec2 p)
{
    const uint numVariants = pc.recordTypes.t[recordType].numVariants;
    if (numVariants == 0u)
        return;
    const uint variant = treeHash(seed, 102u) % numVariants;
    const vec2 range = pc.recordTypes.t[recordType].scale;
    const float scale = mix(range.x, range.y, treeHash01(treeHash(seed, 103u)))
        * exp2(pc.recordTypes.t[recordType].sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0 - 1.0));
    const float yaw = treeHash01(treeHash(seed, 104u)) * 6.28318531;
    float ground;
    if (!treeRecordGround(pc.records, pc.map, pc.mapSize, pc.chunkSize, p, ground))
        ground = terrainHeightAt(p);
    VolumePiece piece;
    piece.posScale = vec4(p.x, ground - 0.05, p.y, scale);
    piece.quat = vec4(0.0, sin(0.5 * yaw), 0.0, cos(0.5 * yaw)); // about up
    piece.type = pc.recordTypes.t[recordType].variantType[variant];
    splatPiece(piece);
}
#endif

void main()
{
#ifdef TREE_SPLAT_RECORDS
    // ONE WORKGROUP PER RECORD (past 65535 the dispatch wraps into y): its chunk by a binary search over the detail
    // chunks' first workgroups (the CPU picked them: within the record detail distance - tree_volume_records.cs takes
    // the rest). A workgroup per CHUNK ran its ~3000 plants in sequence: 31 ms per bake.
    const uint local = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
    if (local >= pc.numPieces)
        return;
    const uint wg = local + pc.wgOffset; // this frame's slice of the bake
    uint lo = 0u, hi = pc.numDetailChunks - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1u) / 2u;
        if (pc.detailChunks.d[mid].w <= wg)
            lo = mid;
        else
            hi = mid - 1u;
    }
    const uvec4 chunk = pc.detailChunks.d[lo];
    const ivec2 coord = ivec2(chunk.xy);
    const uint record = pc.records.w[chunk.z + (wg - chunk.w)];
    const uint type = treeRecordType(record);
    if (type >= pc.numRecordTypes)
        return;
    const uint seed = treeRecordSeed(pc.worldSeed, coord, record);
    const vec2 p = vec2(coord) * pc.chunkSize + treeRecordLocal(record, pc.chunkSize);
    splatRecordPlant(type, seed, p);
    // "Bushes per tree": the whole part always, the fraction by chance, out of the trunk's way (1.5 m) to the bush
    // radius (area-uniform), a random bush type of its climate.
    const uint numBushes = pc.recordTypes.t[type].numBushes;
    const float perTree = pc.recordTypes.t[type].bushesPerTree;
    if (numBushes == 0u || perTree <= 0.0)
        return;
    const float whole = floor(perTree);
    const uint count = uint(whole) + (treeHash01(treeHash(seed, 110u)) < perTree - whole ? 1u : 0u);
    const float bushRadius = pc.recordTypes.t[type].bushRadius;
    for (uint b = 0u; b < count; ++b)
    {
        const uint bushSeed = treeHash(seed, 200u + b);
        const float angle = treeHash01(treeHash(bushSeed, 111u)) * 6.28318531;
        const float radius = mix(1.5, bushRadius, sqrt(treeHash01(treeHash(bushSeed, 112u))));
        const uint bushType = pc.recordTypes.t[type].bushTypes[treeHash(bushSeed, 113u) % numBushes];
        splatRecordPlant(bushType, bushSeed, p + vec2(cos(angle), sin(angle)) * radius);
    }
#else
    const uint pieceIdx = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u; // past 65535 trees the dispatch wraps into y
    if (pieceIdx >= pc.numPieces)
        return;
    splatPiece(pc.pieces.p[pieceIdx]);
#endif
}
