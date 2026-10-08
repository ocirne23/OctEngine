#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Terrain variant of instanced_indirect.vs.glsl. The terrain fragment shader needs only world position and
// the geometric normal (it builds its own tangent bases for XZ/triplanar splatting), so this VS skips the
// tangent/bitangent (no full TBN) and the UV entirely. The shared vertex input layout still describes
// tangent (loc 2) and uv (loc 3); leaving them unconsumed is valid.
//
// It ALSO evaluates the baked terrain fields (altitude/temperature/humidity/water level) here, per vertex,
// and hands them to the FS as one interpolant - see the comment at the evaluation below.

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl; the prev* fields are the motion vectors' - the
// terrain is static and writes none, but the stride must match).
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

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
// NOT a tangent on the terrain: the EDGE STITCH (Procedural TerrainChunkMesh) - xyz = this vertex's height on the
// straight edge of a neighbour 1 / 2 / 3 LODs coarser, w = the chunk's LOD.
layout (location = 2) in vec4 in_stitch;
layout (location = 4) in uint inst_idx;

// INVARIANT: the ground and the terrain film (TERRAIN_OVERLAY_PASS, depth test GREATER_OR_EQUAL) are two
// pipelines computing the same position from the same inputs where the film has no lift - their depth must
// match bit for bit there.
invariant gl_Position;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_terrainFields; // x = macro altitude, y = temperature C, z = humidity, w = water level

#ifdef TERRAIN_OVERLAY_PASS
// The film: out_pos is LIFTED to its water level, this is the lift along the normal (m). The film FS rebuilds
// the mesh point under it, out_pos - normalize(out_normal) * this, and lights and measures the relief from it
// (the shadow map and the TLAS hold the flat mesh). One component, not the vec3 mesh point: 2 fewer in TRAM.
layout (location = 3) out float out_meshLift;

#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"  // the film's water level: terrainWetnessAt + terrainPoolLevel
#define UNDERWATER_OCEAN_BINDING 7
#include "underwater_light.inc.glsl" // underwaterLiveWaveY: the live displaced ocean surface

// The film's surface, as a height in the relief band (0 = its low points, 0.5 = the mesh, 1 = its top): the
// WATER LEVEL the local wetness fills the relief to (rain puddles), or the LIVE OCEAN surface where that
// stands higher - the waterline then continues onto the sand instead of ending at the ocean mesh's
// intersection with it. Capped at the relief top: past that the ocean's own surface is what is drawn, and a
// film lifted to the same height would z-fight it.
// normalY = the smooth mesh normal's y (the FS's coverN: the pool level sinks on slopes).
float terrainFilmLevel(vec3 meshPos, float normalY, float waterLevel, float reliefDepth)
{
    float level = terrainPoolLevel(terrainWetnessAt(meshPos.xz), normalY);
    if (terrainHeightMapPresent())
    {
        float depthBelow = waterLevel - meshPos.y; // calm column over this point (negative on dry land)
        // The swash band rides the live displaced surface (the lit core's gate; underwater_light.inc.glsl).
        // Ground deeper than the reach is under water at any wave phase and skips the wave taps.
        const float reach = u_ocean_swashReach;
        if (reach > 0.0 && abs(depthBelow) < reach)
            depthBelow += underwaterLiveWaveY(meshPos.xz, depthBelow, waterLevel);
        level = max(level, clamp(0.5 + depthBelow / max(reliefDepth, 1e-3), 0.0, 1.0));
    }
    return level;
}
#endif

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// The streamer's ring LOD of a chunk at its DRAW camera (u_terrain_stitch.yz). MIRRORS Procedural ringLodAt +
// chunkEdgeDist (TerrainStreamer.cpp) BIT FOR BIT - the same float operations in the same order: both chunks of an
// edge must get the same answer, and the streamer draws a chunk only while its LOD is at most this.
uint terrainRingLod(vec2 coord)
{
    const vec2 cam = u_terrain_stitch.yz;
    const vec2 d = max(max(coord - cam, cam - (coord + 1.0)), vec2(0.0));
    const float k = max(max(d.x, d.y) - u_terrain_stitchBands.x, 0.0);
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

// EDGE STITCHING (the terrain has no skirts): an edge vertex takes its height on the edge's COARSER side, so the two
// chunks of every edge draw the same line. The edge's LOD = the coarser of the two chunks' ring LODs at the draw
// camera; this chunk is never coarser than its own (the streamer's invariant), so it has every vertex it needs.
// A corner sits on every lattice; an interior vertex never moves.
float terrainStitchedHeight(vec3 localPos, vec2 chunkOrigin)
{
    const float chunkSize = u_terrain_stitch.x;
    if (chunkSize <= 0.0)
        return localPos.y; // stitching off
    const bvec2 lo = equal(localPos.xz, vec2(0.0));
    const bvec2 hi = equal(localPos.xz, vec2(chunkSize));
    const bool onX = lo.x || hi.x, onZ = lo.y || hi.y; // on the west / east edge, the north / south edge
    if (onX == onZ)
        return localPos.y;
    const vec2 coord = round(chunkOrigin / chunkSize);
    const vec2 neighbour = coord + (onX ? vec2(hi.x ? 1.0 : -1.0, 0.0) : vec2(0.0, hi.y ? 1.0 : -1.0));
    const int delta = min(int(max(terrainRingLod(coord), terrainRingLod(neighbour))) - int(in_stitch.w + 0.5), 3);
    return delta > 0 ? in_stitch[delta - 1] : localPos.y;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];

    out_normal = quat_transform(in_normal, inst.quat);
    const vec3 localPos = vec3(in_pos.x, terrainStitchedHeight(in_pos, inst.posScale.xz), in_pos.z);
    out_pos    = quat_transform(localPos * inst.posScale.w, inst.quat) + inst.posScale.xyz;

    // Baked terrain fields, evaluated PER VERTEX and interpolated (the FS used to fetch these per pixel:
    // 2 cascade taps + a 4-8 texel-decode climate bilinear). Interpolation loses nothing: every field is
    // band-limited far below any LOD's vertex lattice (climate is 30 m/px generator output, altitude is
    // the macro band, water level is sea/lake surfaces), and temperature = baseline + lapse * height is
    // LINEAR in height, so interpolating the evaluated value equals evaluating at the interpolated height.
    // Climate must stay BILINEAR here (terrainClimateAt, not the nearest variant): the splat blend weights
    // derive from it, and nearest sampling quantizes the blending to the data map's texel grid.
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
        // The map stores a SEA-LEVEL baseline + one lapse rate, evaluated at the shaded height - never
        // bake a temperature sample (only valid at the height it was taken; the cascades' heights differ).
        temperature = terrainTemperatureAt(climate, out_pos.y);
    }
    out_terrainFields = vec4(altitude, temperature, humidity, waterLevel);

#ifdef TERRAIN_OVERLAY_PASS
    // THE FILM (never tessellated): inside the tessellation range, where the ground is displaced by the relief
    // (terrain_tess.tes.glsl: the same distance fade and slope gate), lifted along the normal to its water
    // level in the relief band - a pool's surface over the crevices, the rock standing out of it (the ground's
    // depth hides the film there). Elsewhere, and with tessellation off, no lift: it lies on the flat ground,
    // bit-identical (GREATER_OR_EQUAL). Per vertex: it does not follow the ground's relief, only its water
    // level. The ground relief depth only (the rock's is a per-pixel coverage the VS does not have).
    out_meshLift = 0.0;
    if (u_terrainTess_enabled > 0.5 && u_terrain_splatBase >= 0.0 && u_terrain_numGround >= 1.0)
    {
        const vec3 N = normalize(out_normal);
        const float fadeStart = u_terrainTess_fadeStart, fadeEnd = u_terrainTess_fadeEnd;
        const float dist = distance(out_pos, u_views_viewPos[VIEW_CENTER].xyz); // the centre view, as the TES: both VR eyes lift the same
        if (dist < fadeEnd)
        {
            const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
            const float depth = u_terrainTess_depthGround * (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, N.y); // the HEIGHT falloff
            if (depth > 1e-4)
            {
                out_meshLift = (terrainFilmLevel(out_pos, N.y, waterLevel, depth) - 0.5) * depth;
                out_pos += N * out_meshLift;
            }
        }
    }
#endif
#ifndef TERRAIN_TESS
    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
    // TERRAIN_TESS (the tessellated terrain, terrain_tess.tcs/.tes.glsl): these outputs are control points; the
    // evaluation shader interpolates them (the fields are band-limited far below the lattice, as above),
    // displaces and projects.
}
