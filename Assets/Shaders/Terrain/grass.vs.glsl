#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable // u_textures (unsized)

// THE GRASS BLADES. The blade model is grass_blade.inc.glsl's, and ALL of it runs - the tuft, the blade, the vertex - for
// every vertex. No vertex buffer: the index buffer holds vertex IDs (blade << GRASS_BLADE_VERTEX_SHIFT | vertex:
// GrassPipeline::buildIndices), the RING CELL arrives as instance-rate attributes (the cull's record). The blade index
// splits into the blade's base cell and its rank there (grassBaseCell); rank k is member k / T of tuft k % T (the ranks
// are dealt to the tufts in turn).
// MOTION VECTORS: the same blade at LAST frame's time (u_grass_prevTime) - the wind is the only motion.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"

layout (binding = 14, std430) readonly buffer InTerrainVertices { TerrainVertex in_terrainVertices[]; };
// grass.inc.glsl's value noise (the dry spots, the wind ripple, the types): the terrain noise texture - in the
// main set's bindless array, alone in the near casters' set.
#ifdef GRASS_NEAR_SHADOW
layout (binding = 24) uniform sampler2D u_grassNoise;
#define GRASS_NOISE_SAMPLER u_grassNoise
#else
layout (binding = 27) uniform sampler2D u_textures[];
#endif

layout (location = 0) in vec4 in_patchOrigins; // xy = the ring cell's min corner (world XZ), zw = its terrain chunk's origin
layout (location = 1) in uvec4 in_patchData;   // x = the chunk's first vertex, y = its cells per side | LOD << 16 | the ring level
                                               // << 18 | morph << 21, z = the corner densities (unorm8 x 4), w = packHalf2x16(the
                                               // chunk's cell size (m), the cell's temperature (C))

// THE INTERFACE: 7 interpolated components and one flat uvec4 - no position (the FS rebuilds it from its depth) and the
// normal as ONE value (GrassVertex::normalY + the flat width axis).
// THE CANOPY (grass.inc.glsl): the point's depth below the canopy top (m); the depth is linear along the blade, so it
// interpolates exactly.
#ifndef GRASS_NEAR_SHADOW
layout (location = 0) out vec4 out_blade;          // x = along the blade (0 root .. 1 tip), y = across (-1 .. 1: the midrib at 0),
                                                   // z = the canopy depth, w = the normal's y
layout (location = 1) out vec3 out_prevWorldDelta; // to last frame's point
layout (location = 2) flat out uvec4 out_flat;     // written by grassBuildBlade, part by part (grass_blade.inc.glsl)
#endif

#include "grass.inc.glsl"
#include "wind.inc.glsl"
#include "grass_wind.inc.glsl" // grassBezier, grassWind, grassTip (shared with the flower stems)
#include "grass_blade.inc.glsl"

void main()
{
    const uint vid = uint(gl_VertexIndex);
    const uint blade = vid >> GRASS_BLADE_VERTEX_SHIFT;
    const uint vert = vid & ((1u << GRASS_BLADE_VERTEX_SHIFT) - 1u);
    const uint T = uint(u_grass_tufts);
    GrassPatchIn p = grassPatchIn(in_patchOrigins, in_patchData);
    const uint rank = grassBaseCell(p, blade); // the blade's base cell in the ring cell, and its rank there
    const uint member = uint((float(rank) + 0.5) / float(T)); // rank / T (exact: the ranks are < 2^16), no integer division
    const GrassTuft tuft = grassTuft(p, rank - member * T);
    if (!grassBuildBlade(p, tuft, rank, member))
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // every vertex of the blade lands here: no area, no fragments
        return;
    }
    const GrassVertex v = grassVertex(vert, p.lod);

#ifdef GRASS_NEAR_SHADOW
    gl_Position = u_grass_shadowViewProj * vec4(v.pos, 1.0);
#else
    gl_Position = u_mvp * vec4(v.pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
    out_blade = vec4(v.t, v.sideSign, v.canopyDepth, v.normalY);
    out_prevWorldDelta = v.prevDelta;
#endif
}
