// GROUND CLUTTER (ClutterPipeline; Docs/GroundClutterPlan.md): pebbles, branches, mushrooms and flowers, placed on the
// GPU every frame by clutter_cull.cs.glsl and drawn by clutter.vs/fs.glsl (rigid meshes) and clutter_flower.vs/fs.glsl
// (flowers, built in the vertex shader). The structs MIRROR RendererVKLayout (Layout.ixx) - keep them in step.
//
// With CLUTTER_FRAME_BINDING defined: the per-frame CLUTTER FRAME (ClutterFrameGpu: the patch grid, the counts, the
// FOREST FLOOR MAP) at that binding, and clutterFloorAt.

#ifndef CLUTTER_INC_GLSL
#define CLUTTER_INC_GLSL

struct ClutterType // RendererVKLayout::ClutterTypeGpu
{
    vec4 climate;   // x..y temperature (t01), z..w precipitation (01): the ideal box
    vec4 placement; // x density (per m^2), y 1 / climate width, z 1 / cluster size (0 = none), w cluster coverage
    vec4 terms0;    // Grass (bare .. full), Crag (none .. full)
    vec4 terms1;    // Beach (none .. full), Canopy (open .. shaded)
    vec4 terms2;    // Trunk (far .. near), RockNear (far .. near)
    vec4 terms3;    // Wet (dry .. wet), z max slope, w min altitude above water (m)
    vec4 ring;      // x radius (0 = none), y width, z cell (m), w chance
    vec4 shape;     // x..y scale range, z range (m), w sink (fraction of the height)
    vec4 albedo0;   // rgb main (linear), w roughness
    vec4 albedo1;   // rgb second (linear), w ground align
    vec4 flower;    // x stem height, y head size, z petal width, w petal open angle (rad)
    vec4 bound;     // x max density (per m^2), y spots
    uvec4 info;     // x first mesh, y variants, z kind, w head | petals << 8
};

struct ClutterMesh // RendererVKLayout::ClutterMeshGpu
{
    uvec4 lods[CLUTTER_LODS]; // x first index, y index count (0 = none), z vertex offset
    vec4 bounds;              // x radius around (0, height / 2, 0), y height
};

struct ClutterInstance // RendererVKLayout::ClutterInstanceGpu
{
    vec4 posScale;
    uvec4 data; // xy quaternion (halves), z type | variant << 8 | lod << 16 | kind << 24, w hash
    uvec4 look; // x albedo0 (sqrt rgb, roughness), y albedo1 (sqrt rgb, spots), z half2(stem height, head size) - rigid: half2(its height m, 0) -, w head | petals << 8 | petal width << 16 | open << 24
};

// The flower bucket of a LOD, then (mesh, LOD) for the rigid meshes.
uint clutterBucket(uint kind, uint mesh, uint lod)
{
    return kind == CLUTTER_KIND_FLOWER ? lod : CLUTTER_FLOWER_LODS + mesh * CLUTTER_LODS + lod;
}

vec3 clutterQuatRotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec4 clutterUnpackQuat(uvec2 packed)
{
    return vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
}

// The albedos ride the record as rgba8 of their SQUARE ROOT (more steps in the darks).
vec3 clutterUnpackAlbedo(uint packed, out float w)
{
    const vec4 v = unpackUnorm4x8(packed);
    w = v.w;
    return v.rgb * v.rgb;
}

#ifdef CLUTTER_FRAME_BINDING
layout (binding = CLUTTER_FRAME_BINDING, std430) readonly buffer InClutterFrame // RendererVKLayout::ClutterFrameGpu
{
    vec2 cf_floorOrigin;
    float cf_floorInvTexel;
    uint cf_floorDim;
    vec2 cf_gridOrigin;
    uint cf_gridDim;
    float cf_patchSize;
    uint cf_numTypes;
    uint cf_numMeshes;
    float cf_range;
    uint cf_pad0;
    uvec4 cf_flowerLods[CLUTTER_FLOWER_LODS];
    uint cf_floor[];
};

// THE FOREST FLOOR at xz (bilinear): x canopy, y trunk proximity, z rock proximity, w occupied (a trunk or a rock stands
// there). 0 everywhere without a map (no trees or rocks, or the trees' world off) and outside it.
vec4 clutterFloorAt(vec2 xz)
{
    if (cf_floorDim < 2u)
        return vec4(0.0);
    const vec2 g = (xz - cf_floorOrigin) * cf_floorInvTexel - 0.5;
    const float last = float(cf_floorDim - 1u);
    if (any(lessThan(g, vec2(0.0))) || any(greaterThan(g, vec2(last))))
        return vec4(0.0);
    const uvec2 i0 = min(uvec2(g), uvec2(cf_floorDim - 2u));
    const vec2 f = g - vec2(i0);
    const uint base = i0.y * cf_floorDim + i0.x;
    const vec4 a = unpackUnorm4x8(cf_floor[base]);
    const vec4 b = unpackUnorm4x8(cf_floor[base + 1u]);
    const vec4 c = unpackUnorm4x8(cf_floor[base + cf_floorDim]);
    const vec4 d = unpackUnorm4x8(cf_floor[base + cf_floorDim + 1u]);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
#endif

#endif
