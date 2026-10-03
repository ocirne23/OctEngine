#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE GRASS BLADES (StaticMeshGraphicsPipeline's grass pipeline; the patch model is in grass.inc.glsl). No vertex
// buffer: the index buffer holds vertex IDs (blade << GRASS_BLADE_VERTEX_SHIFT | vertex), the patch arrives as
// instance-rate attributes (the cull's record, firstInstance = its slot), and everything else is hashed from the
// patch cell and the blade rank. A blade is a quadratic Bezier from its root on the terrain mesh: the control point
// straight above the root at the tip's height, the tip pushed sideways by its lean and the wind (the curve keeps
// about its length). Its width narrows to the tip; the tip is one vertex.
// MOTION VECTORS: the same blade at LAST frame's time (u_grassParams5.w) - the wind is the only motion.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"

layout (binding = 14, std430) readonly buffer InVertices { MeshVertex in_vertices[]; };

#include "grass.inc.glsl"

layout (location = 0) in vec4 in_patchOrigins; // xy = the patch's min corner (world XZ), zw = its terrain chunk's origin
layout (location = 1) in uvec4 in_patchData;   // x = the chunk's first vertex, y = its cells per side | LOD << 16,
                                               // z = the corner densities (unorm8 x 4),
                                               // w = packHalf2x16(the chunk's cell size (m), the patch's temperature (C))

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;       // the blade's normal (one face; the FS flips it to the viewer)
layout (location = 2) out vec3 out_groundNormal; // the terrain's facet normal under the root
layout (location = 3) out vec4 out_blade;        // x = along the blade (0 root .. 1 tip), y = colour variation [0, 1],
                                                 // (with the cold darkening: an albedo factor), z = dryness,
                                                 // w = blend toward the ground normal
layout (location = 4) out vec3 out_prevWorldDelta;
// THE CANOPY (grass.inc.glsl): x = the point's depth below the canopy top (m), y = the canopy's extinction (1/m). The
// FS turns them into the sun reaching the point; the depth is linear along the blade, so it interpolates exactly.
layout (location = 5) out vec2 out_canopy;

const float GRASS_TWO_PI = 6.28318530718;

vec3 grassBezier(vec3 p0, vec3 p1, vec3 p2, float t)
{
    const float s = 1.0 - t;
    return s * s * p0 + 2.0 * s * t * p1 + t * t * p2;
}

// The wind's push on the tip at a point and time, in blade heights along the wind direction: a steady bend with a
// sway, plus gusts - value noise blowing downwind at "Gust speed".
float grassWind(vec2 xz, float time, float phase)
{
    const float sway = sin(time * u_grassParams5.z * GRASS_TWO_PI + phase);
    const float gust = grassValueNoise((xz - u_grassParams4.xy * (u_grassParams5.y * time)) * u_grassParams5.x);
    return u_grassParams4.z * (0.85 + 0.15 * sway) + u_grassParams4.w * gust * (0.8 + 0.2 * sway);
}

// The tip and the control point of a blade of height h: lean + wind sideways (capped at 0.9 h), the rest upward.
vec3 grassTip(vec3 root, vec2 lean, float wind, float h, out vec3 ctrl)
{
    vec2 offset = lean + u_grassParams4.xy * (wind * h);
    const float len = length(offset);
    if (len > 0.9 * h)
        offset *= 0.9 * h / len;
    const float y = sqrt(max(h * h - dot(offset, offset), 0.0));
    ctrl = root + vec3(0.0, y, 0.0);
    return root + vec3(offset.x, y, offset.y);
}

// The blade being built (set in main): its curve, its width axis and half width at the root, and the blend from the
// curve normal to the linear one.
struct GrassBlade
{
    vec3 root;
    vec3 ctrl;
    vec3 tip;
    vec3 side;
    float halfWidth;
    float linearBlend;
};
GrassBlade g_blade;

// A point of the blade's edge (sideSign -1 / +1) or its middle (0) at t: the width narrows linearly to the tip.
vec3 grassBladePoint(float t, float sideSign)
{
    return grassBezier(g_blade.root, g_blade.ctrl, g_blade.tip, t) + g_blade.side * (sideSign * g_blade.halfWidth * (1.0 - t));
}

// The shading normal at t (one face; the FS flips it to the viewer and normalizes).
// LINEAR: root -> across the upright stem, tip -> across the chord of the upper half (tip - curve(0.5)), which always
// rises; the edge tilt fades to the tip like the width. Every LOD interpolates it the same.
// CURVE: across the curve's tangent, with the full edge tilt (the tip vertex has none).
vec3 grassBladeNormal(float t, float sideSign)
{
    const vec3 side = g_blade.side;
    const float r = u_grassShade.z;
    const vec3 rootN = normalize(cross(vec3(0.0, 1.0, 0.0), side));
    const vec3 tipAcross = cross(g_blade.tip - grassBezier(g_blade.root, g_blade.ctrl, g_blade.tip, 0.5), side);
    const vec3 tipN = tipAcross * inversesqrt(max(dot(tipAcross, tipAcross), 1e-12));
    const vec3 linearN = mix(rootN, tipN, t) + side * (sideSign * r * (1.0 - t));
    const vec3 tangent = 2.0 * (1.0 - t) * (g_blade.ctrl - g_blade.root) + 2.0 * t * (g_blade.tip - g_blade.ctrl);
    const vec3 across = cross(tangent, side);
    const vec3 curveN = normalize(across * inversesqrt(max(dot(across, across), 1e-12)) + side * ((t < 1.0 ? sideSign : 0.0) * r));
    return mix(curveN, linearN, g_blade.linearBlend);
}

void main()
{
    const uint vid = uint(gl_VertexIndex);
    const uint blade = vid >> GRASS_BLADE_VERTEX_SHIFT;
    const uint vert = vid & ((1u << GRASS_BLADE_VERTEX_SHIFT) - 1u);
    const uint segments = grassLodSegments(in_patchData.y >> 16);
    const float N = u_grassParams0.x;
    const float P = u_grassParams0.y;

    // The blade's spot: the rank's point of the patch's R2 sequence (offset per patch), with a small jitter.
    const vec2 origin = in_patchOrigins.xy;
    const uint patchHash = grassHash2(ivec2(floor(origin / P + 0.5)));
    const uint bladeHash = grassHash(patchHash ^ (blade * 0x9E3779B9u));
    const vec2 r2Offset = vec2(grassUnit(patchHash), grassUnit(grassHash(patchHash + 1u)));
    vec2 uv = fract(r2Offset + float(blade) * vec2(0.7548776662, 0.5698402910));
    uv = fract(uv + (vec2(grassUnit(bladeHash), grassUnit(grassHash(bladeHash + 1u))) - 0.5) * (0.5 / sqrt(N)));
    const vec2 xz = origin + uv * P;

    const vec2 cellTemperature = unpackHalf2x16(in_patchData.w);
    const GrassGround ground = GrassGround(in_patchOrigins.zw, in_patchData.x, in_patchData.y & 0xFFFFu, cellTemperature.x);
    vec3 groundN;
    const float groundY = grassGroundHeight(ground, xz, groundN);
    const float dist = distance(vec3(xz.x, groundY, xz.y), u_viewPos);

    // Density (the corners, bilinear; the clumps) x thinning, against the blade's rank.
    // The COVER (the corners: climate, slope, water, snow) and the clumps. Where the cover fades the blades get fewer
    // AND smaller ("Size by cover"): fewer blades of full size read as sparse long stalks.
    const vec4 corners = unpackUnorm4x8(in_patchData.z);
    const float cover = mix(mix(corners.x, corners.y, uv.x), mix(corners.z, corners.w, uv.x), uv.y);
    const float clump = grassClump(xz);
    const float density = cover * clump;
    const float coverSize = grassCoverSize(cover);
    const float thinning = grassKeep(dist);
    const float keep = density * thinning;
    const float rank = (float(blade) + 0.5) / N;
    const float grow = clamp((keep - rank) / max(u_grassParams6.w * keep, 1e-5), 0.0, 1.0);
    if (grow <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // every vertex of the blade lands here: no area, no fragments
        return;
    }

    // The root sinks below the mesh by "Root sink" plus half the tessellated relief (terrain_tess.tes.glsl: the same
    // fade and slope gate), so no blade floats over a hollow of the displaced ground; the blade grows by as much.
    float sink = u_grassParams1.w;
    if (u_terrainTessParams0.x > 0.5 && dist < u_terrainTessParams1.y)
    {
        const float t = clamp((dist - u_terrainTessParams1.x) / max(u_terrainTessParams1.y - u_terrainTessParams1.x, 1e-3), 0.0, 1.0);
        sink += 0.5 * u_terrainTessParams1.z * (1.0 - pow(t, u_terrainTessParams2.y)) * smoothstep(0.35, 0.6, groundN.y);
    }
    const float heightVar = grassUnit(grassHash(bladeHash + 2u));
    const float height = (u_grassParams1.x * mix(1.0 - u_grassParams1.y, 1.0, heightVar) * mix(0.6, 1.0, clump) * coverSize + sink) * grow;
    const vec3 root = vec3(xz.x, groundY - sink, xz.y);

    const float facing = grassUnit(grassHash(bladeHash + 3u)) * GRASS_TWO_PI;
    const vec3 side = vec3(cos(facing), 0.0, sin(facing));
    const float leanAngle = grassUnit(grassHash(bladeHash + 4u)) * GRASS_TWO_PI;
    const vec2 lean = vec2(cos(leanAngle), sin(leanAngle)) * (u_grassParams6.x * height * grassUnit(grassHash(bladeHash + 5u)));
    const float phase = grassUnit(grassHash(bladeHash + 6u)) * GRASS_TWO_PI;
    // The wind eases out with the distance ("Wind/Fade start / end"): far blades are a pixel or less wide, and their
    // motion only reads as grain. Both frames take the same factor, so the motion vectors stay exact.
    const float windFade = 1.0 - smoothstep(u_grassParams9.x, u_grassParams9.y, dist);
    vec3 ctrl, ctrlPrev;
    const vec3 tip = grassTip(root, lean, grassWind(xz, u_timeSeconds, phase) * windFade, height, ctrl);
    const vec3 tipPrev = grassTip(root, lean, grassWind(xz, u_grassParams5.w, phase) * windFade, height, ctrlPrev);

    // The vertex: row r of S (2 per row, sides -1 / +1), the tip last.
    const uint row = min(vert >> 1u, segments);
    const float t = float(row) / float(segments);
    const float sideSign = vert >= 2u * segments ? 0.0 : ((vert & 1u) != 0u ? 1.0 : -1.0);
    // Fewer blades far away get WIDER (toward the same coverage, capped), and never narrower than the pixel floor.
    const float widthScale = min(pow(1.0 / max(thinning, 1e-3), u_grassParams2.z), u_grassParams2.w);
    g_blade.root = root;
    g_blade.side = side;
    g_blade.halfWidth = 0.5 * max(u_grassParams1.z * widthScale * coverSize, dist * u_grassParams3.z);
    g_blade.ctrl = ctrl;
    g_blade.tip = tip;
    // The normal: NEAR the curve's own (the bent top catches the light), easing per blade into one LINEAR in t from
    // "LOD 2 distance" to "LOD 3 distance" - a LOD 3 blade is ONE triangle, and the curve's tip normal (horizontal tangent
    // there) spread over it flipped it bright or dark at the switch.
    g_blade.linearBlend = smoothstep(u_grassParams3.y, u_grassParams10.x, dist);

    // GEOMORPH (the LODs nest: 8 -> 4 -> 2 -> 1 segments): over the last "LOD morph band" before the next LOD's distance,
    // the rows the coarser LOD drops (odd rows) move onto the line between their neighbours, normals included. At the
    // switch the blade is the coarser one exactly; a blade of a finer patch past the distance draws it fully morphed.
    // The patch's LOD comes from its NEAREST point, so its blades are never nearer than the switch.
    const uint lod = in_patchData.y >> 16;
    const float nextLodDist = lod == 0u ? u_grassParams3.x : lod == 1u ? u_grassParams3.y : u_grassParams10.x;
    const float morph = lod < 3u && (row & 1u) != 0u
        ? smoothstep(nextLodDist * (1.0 - u_grassParams9.w), nextLodDist, dist) : 0.0;
    const float h = 1.0 / float(segments);
    vec3 pos = grassBladePoint(t, sideSign);
    if (morph > 0.0)
        pos = mix(pos, 0.5 * (grassBladePoint(t - h, sideSign) + grassBladePoint(t + h, sideSign)), morph);

#ifdef GRASS_NEAR_SHADOW
    // THE NEAR GRASS CASCADE's caster (GrassPipeline::recordNearShadow): the same blade into the extra shadow layer's
    // box (ahead of the camera); past its half size x 1.5 + 2 m from the box's centre (the receivers' disc, and casters
    // up-sun of it) nothing. (The blades never cast into the scene cascades: the "Cast shadows" path was removed
    // 2026-10-03 - the canopy and this cascade replace it.)
    if (distance(xz, u_grassParams14.yz) > u_grassParams13.y * 1.5 + 2.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    gl_Position = u_grassShadowViewProj * vec4(pos, 1.0);
#else
    vec3 normal = grassBladeNormal(t, sideSign);
    if (morph > 0.0)
        normal = mix(normal, 0.5 * (grassBladeNormal(t - h, sideSign) + grassBladeNormal(t + h, sideSign)), morph);
    g_blade.ctrl = ctrlPrev;
    g_blade.tip = tipPrev;
    vec3 prevPos = grassBladePoint(t, sideSign);
    if (morph > 0.0)
        prevPos = mix(prevPos, 0.5 * (grassBladePoint(t - h, sideSign) + grassBladePoint(t + h, sideSign)), morph);
    out_normal = normal;
    out_groundNormal = groundN;

    const float dryNoise = grassValueNoise(xz * (u_grassParams6.y * 0.37) + 17.3);
    const float dryness = smoothstep(0.0, 0.3, dryNoise - (1.0 - u_grassColor2.w));
    const float groundBlend = clamp(dist / max(u_grassParams3.w, 1e-3), 0.0, 1.0) * u_grassShade.w;
    // The albedo factor: the per-blade variation, x the COLD darkening (full at "Cold temperature", none from "Warm
    // temperature"; the patch's mean temperature at its height - climate is km-scale, so no patch steps show).
    const float variation = 1.0 + u_grassColor1.w * (grassUnit(grassHash(bladeHash + 7u)) * 2.0 - 1.0);
    const float warm = smoothstep(u_grassParams10.y, max(u_grassParams10.z, u_grassParams10.y + 0.01), cellTemperature.y);
    out_blade = vec4(t, variation * mix(1.0 - u_grassParams10.w, 1.0, warm), dryness, groundBlend);
    out_pos = pos;
    out_prevWorldDelta = prevPos - pos;
    // The canopy at this blade: its mean height and extinction (the ground under it takes the same); the depth is
    // capped at the canopy height (the sunk root is below the ground).
    const float canopyHeight = grassCanopyHeight(clump, coverSize);
    out_canopy = vec2(clamp(canopyHeight - (pos.y - groundY), 0.0, canopyHeight), grassCanopyExtinction(cover, coverSize, dist));

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
