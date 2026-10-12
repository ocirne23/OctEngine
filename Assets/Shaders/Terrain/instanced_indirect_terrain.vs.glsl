#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Terrain variant of instanced_indirect.vs.glsl: world position + geometric normal only (the FS builds its own
// tangent bases), plus the baked terrain fields per vertex as one interpolant (see main).

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl; the terrain writes no motion, but the stride must match).
struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;
    uint meshIdxMaterialIdx;
    uint prevVertexDelta;
    uvec2 prevQuat;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

// positionU.w (MeshVertex's texCoord u) carries the RIVER influence on the terrain, not a UV.
layout (location = 0) in vec4 in_posRiver;
layout (location = 1) in vec3 in_normal;
// NOT a tangent on the terrain: the EDGE STITCH (TerrainChunkMesh) - xyz = this vertex's height on the straight edge
// of a neighbour 1 / 2 / 3 LODs coarser, w = the chunk's LOD.
layout (location = 2) in vec4 in_stitch;
layout (location = 4) in uint inst_idx;

// INVARIANT: the ground and the terrain film (TERRAIN_OVERLAY_PASS, depth test GREATER_OR_EQUAL) compute the same
// position from the same inputs where the film has no lift - their depth must match bit for bit there.
invariant gl_Position;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_terrainFields; // x = macro altitude, y = temperature C, z = humidity, w = water level
layout (location = 4) out float out_river;        // the river influence (TerrainFields::river)

#ifdef TERRAIN_OVERLAY_PASS
// The film: out_pos is LIFTED to its water level, this is the lift along the normal (m). The film FS rebuilds the mesh
// point under it (out_pos - normalize(out_normal) * this) to light from. One component, not a vec3: 2 fewer in TRAM.
layout (location = 3) out float out_meshLift;

#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"  // the film's water level: terrainWetnessAt + terrainPoolLevel
#define UNDERWATER_OCEAN_BINDING 7
#include "underwater_light.inc.glsl" // underwaterLiveWaveY: the live displaced ocean surface

// The film's surface as a height in the relief band (0 = its low points, 0.5 = the mesh, 1 = its top): the pool level
// of the local wetness, or the LIVE OCEAN surface where that stands higher (the waterline continues onto the sand).
// Capped at the relief top: past that the ocean's own surface draws, and a film at the same height would z-fight it.
// normalY = the smooth mesh normal's y (the FS's coverN).
float terrainFilmLevel(vec3 meshPos, float normalY, float waterLevel, float reliefDepth)
{
    float level = terrainPoolLevel(terrainWetnessAt(meshPos.xz), normalY);
    if (terrainHeightMapPresent())
    {
        float depthBelow = waterLevel - meshPos.y; // calm column over this point (negative on dry land)
        // The swash band rides the live displaced surface (the lit core's gate); deeper ground skips the wave taps.
        const float reach = u_ocean_swashReach;
        if (abs(depthBelow) < reach)
            depthBelow += underwaterLiveWaveY(meshPos.xz, depthBelow, waterLevel);
        level = max(level, clamp(0.5 + depthBelow / reliefDepth, 0.0, 1.0));
    }
    return level;
}
#endif

// THE QUADTREE at the streamer's DRAW camera (u_terrain_stitch.yz; base-chunk units). MIRRORS Procedural nodeEdgeDist,
// ringLodAt and Ring::leafLodAt (TerrainStreamer.cpp) BIT FOR BIT - the same float operations in the same order: both
// nodes of an edge must get the same answer, and the streamer draws a node only while it lies inside a leaf of this.
float terrainNodeEdgeDist(ivec2 coord, uint lod)
{
    const int span = 1 << lod;
    const float size = float(span);
    const vec2 lo = vec2(coord * span);
    const vec2 cam = u_terrain_stitch.yz;
    const vec2 d = max(max(lo - cam, cam - (lo + size)), vec2(0.0));
    return max(d.x, d.y);
}

uint terrainRingLod(float edgeDist)
{
    const float k = max(edgeDist - u_terrain_stitchBands.x, 0.0);
    const float lodStep = u_terrain_stitchBands.y;
    const uint maxLod = uint(u_terrain_stitchBands.z);
    float threshold = lodStep;
    uint lod = 0u;
    while (lod < maxLod && k >= threshold)
    {
        ++lod;
        threshold = threshold * 2.0 + lodStep;
    }
    return lod;
}

// The LOD of the leaf over a base chunk: from maxLod down, the first node over it whose band reaches its lod.
uint terrainLeafLod(ivec2 base)
{
    for (uint lod = uint(u_terrain_stitchBands.z); lod > 0u; --lod)
        if (terrainRingLod(terrainNodeEdgeDist(base >> lod, lod)) >= lod)
            return lod;
    return 0u;
}

// EDGE STITCHING (no skirts): an edge vertex takes its height on the edge's COARSER side, so both nodes of every edge
// draw the same line. The edge's LOD = the coarser of the leaves over the two base chunks either side of this vertex
// (the same pair from both nodes); this node is never coarser than its own leaf (the streamer's invariant). A corner
// sits on every lattice; an interior vertex never moves.
float terrainStitchedHeight(vec3 localPos, vec2 nodeOrigin)
{
    const float chunkSize = u_terrain_stitch.x;
    if (chunkSize <= 0.0)
        return localPos.y; // stitching off
    const uint ownLod = uint(in_stitch.w + 0.5);
    const int span = 1 << ownLod;
    const bvec2 lo = equal(localPos.xz, vec2(0.0));
    const bvec2 hi = equal(localPos.xz, vec2(chunkSize * float(span)));
    const bool onX = lo.x || hi.x, onZ = lo.y || hi.y; // on the west / east edge, the north / south edge
    if (onX == onZ)
        return localPos.y;

    const ivec2 nodeBase = ivec2(round(nodeOrigin / chunkSize));
    const int along = int(floor((onX ? localPos.z : localPos.x) / chunkSize)); // exact: a power-of-two step
    const ivec2 inside = nodeBase + (onX ? ivec2(hi.x ? span - 1 : 0, along) : ivec2(along, hi.y ? span - 1 : 0));
    const ivec2 outside = inside + (onX ? ivec2(hi.x ? 1 : -1, 0) : ivec2(0, hi.y ? 1 : -1));
    const int delta = min(int(max(terrainLeafLod(inside), terrainLeafLod(outside))) - int(ownLod), 3);
    return delta > 0 ? in_stitch[delta - 1] : localPos.y;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];

    const vec3 in_pos = in_posRiver.xyz;
    out_river = in_posRiver.w;
    out_normal = quat_transform(in_normal, inst.quat);
    const vec3 localPos = vec3(in_pos.x, terrainStitchedHeight(in_pos, inst.posScale.xz), in_pos.z);
    out_pos    = quat_transform(localPos * inst.posScale.w, inst.quat) + inst.posScale.xyz;

    // Baked terrain fields, evaluated PER VERTEX and interpolated. Interpolation loses nothing: every field is
    // band-limited far below any LOD's vertex lattice (30 m/px climate, the macro altitude band, sea/lake surfaces),
    // and temperature = baseline + lapse * height is LINEAR in height. Climate must stay BILINEAR (not the nearest
    // variant): the splat blend weights derive from it, and nearest quantizes the blending to the map's texel grid.
    float altitude = out_pos.y - u_terrain_seaLevel; // mild-climate fallbacks without a map
    float temperature = 12.5;
    float humidity = 0.5;
    float waterLevel = u_terrain_seaLevel;
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(out_pos.xz);
        altitude = td.w;
        waterLevel = td.y;
        const vec4 climate = terrainClimateAt(out_pos.xz);
        humidity = climate.w;
        temperature = terrainTemperatureAt(climate, out_pos.y);
    }
    out_terrainFields = vec4(altitude, temperature, humidity, waterLevel);

#ifdef TERRAIN_OVERLAY_PASS
    // THE FILM (never tessellated): inside the tessellation range (the TES's distance fade and slope gate), lifted
    // along the normal to its water level in the relief band - a pool's surface over the crevices, the displaced rock
    // standing out of it. Elsewhere, and with tessellation off, no lift: bit-identical to the flat ground
    // (GREATER_OR_EQUAL). The ground relief depth only (the rock's is a per-pixel coverage the VS does not have).
    out_meshLift = 0.0;
    if (u_terrainTess_enabled > 0.5 && u_terrain_splatBase >= 0.0 && u_terrain_numGround >= 1.0)
    {
        const vec3 N = out_normal; // unit: the mesh normal x the identity quat
        const float dist = distance(out_pos, u_views_viewPos[VIEW_CENTER].xyz); // the centre view, as the TES: both VR eyes lift the same
        if (dist < u_terrainTess_fadeEnd)
        {
            const float t = clamp((dist - u_terrainTess_fadeStart) * u_terrainTess_invFadeWidth, 0.0, 1.0);
            const float depth = u_terrainTess_depthGround * (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, N.y); // the HEIGHT falloff
            if (depth > 1e-4)
            {
                out_meshLift = (terrainFilmLevel(out_pos, N.y, waterLevel, depth) - 0.5) * depth;
                out_pos += N * out_meshLift;
            }
        }
    }
#endif

    // TERRAIN_TESS: these outputs are control points; the TES interpolates, displaces and projects.
#ifndef TERRAIN_TESS
    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
