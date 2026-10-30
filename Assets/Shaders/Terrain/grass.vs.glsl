#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable // u_textures (unsized)

// THE GRASS BLADES (the patch model is in grass.inc.glsl). No vertex buffer: the index buffer holds vertex IDs (blade
// << GRASS_BLADE_VERTEX_SHIFT | vertex), the patch arrives as instance-rate attributes (the cull's record), and
// everything else is hashed from the patch cell and the blade rank. A blade is a quadratic Bezier from its root on the
// terrain mesh: the control point above the root at the tip's height, the tip pushed sideways by its lean and the
// wind. Its width narrows to the tip; the tip is one vertex.
// MOTION VECTORS: the same blade at LAST frame's time (u_grass_prevTime) - the wind is the only motion.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"

layout (binding = 14, std430) readonly buffer InVertices { MeshVertex in_vertices[]; };
// grass.inc.glsl's value noise (the clumps, the dry spots, the wind ripple): the terrain noise texture - in the main set's
// bindless array, alone in the near casters' set.
#ifdef GRASS_NEAR_SHADOW
layout (binding = 24) uniform sampler2D u_grassNoise;
#define GRASS_NOISE_SAMPLER u_grassNoise
#else
layout (binding = 24) uniform sampler2D u_textures[];
#endif

#include "grass.inc.glsl"
#include "wind.inc.glsl"
#include "grass_wind.inc.glsl" // grassBezier, grassWind, grassTip (shared with the flower stems)

layout (location = 0) in vec4 in_patchOrigins; // xy = the patch's min corner (world XZ), zw = its terrain chunk's origin
layout (location = 1) in uvec4 in_patchData;   // x = the chunk's first vertex, y = its cells per side | LOD << 16, z = the corner
                                               // densities (unorm8 x 4), w = packHalf2x16(cell size (m), the patch's temperature (C))

// Interpolated: 11 components; what is constant per blade is FLAT (no interpolation).
// THE CANOPY (grass.inc.glsl): the point's depth below the canopy top (m) and the canopy's extinction (1/m). The FS turns
// them into the sun reaching the point; the depth is linear along the blade, so it interpolates exactly.
layout (location = 0) out vec4 out_posAlong;     // xyz = the position, w = along the blade (0 root .. 1 tip)
layout (location = 1) out vec4 out_normalDepth;  // xyz = the blade's normal (one face; the FS flips it to the viewer), w = the canopy depth
layout (location = 2) flat out vec3 out_groundNormal; // the terrain's facet normal under the root (unit)
layout (location = 3) flat out vec4 out_blade;   // x = the albedo factor (colour variation x cold darkening), y = dryness,
                                                 // z = blend toward the ground normal, w = the canopy extinction
layout (location = 4) out vec3 out_prevWorldDelta;

// The blade being built (set in main): its curve, its width axis and half width at the root, the blend from the curve
// normal to the linear one, and the linear normal's tip end (the root end: grassBladeRootNormal).
struct GrassBlade
{
    vec3 root;
    vec3 ctrl;
    vec3 tip;
    vec3 side;
    float halfWidth;
    float linearBlend;
    vec3 tipN;
};

GrassBlade g_blade;

// The upright stem's normal: across the width axis, cross(up, side) - rebuilt from side, never held (side is horizontal
// and unit, so it is unit).
vec3 grassBladeRootNormal()
{
    return vec3(g_blade.side.z, 0.0, -g_blade.side.x);
}

// A point of the blade's edge (sideSign -1 / +1) or its middle (0) at t: the width narrows linearly to the tip.
vec3 grassBladePoint(float t, float sideSign)
{
    return grassBezier(g_blade.root, g_blade.ctrl, g_blade.tip, t) + g_blade.side * (sideSign * g_blade.halfWidth * (1.0 - t));
}

// The shading normal (one face; the FS flips it to the viewer and normalizes) is mix(CURVE, LINEAR, linearBlend):
// LINEAR: root -> across the upright stem, tip -> across the chord of the upper half (tip - curve(0.5)), which always
// rises; the edge tilt fades to the tip like the width. Every LOD interpolates it the same. It is affine in t, so the
// geomorph's neighbour average is itself: only the curve part morphs.
vec3 grassBladeLinearNormal(float t, float sideSign)
{
    return mix(grassBladeRootNormal(), g_blade.tipN, t) + g_blade.side * (sideSign * u_grass_roundness * (1.0 - t));
}

// CURVE: across the curve's tangent, with the full edge tilt (the tip vertex has none).
vec3 grassBladeCurveNormal(float t, float sideSign)
{
    const vec3 side = g_blade.side;
    const vec3 tangent = 2.0 * (1.0 - t) * (g_blade.ctrl - g_blade.root) + 2.0 * t * (g_blade.tip - g_blade.ctrl);
    const vec3 across = cross(tangent, side);
    // DEGENERATE at the tip: the tangent there is the horizontal lean + wind offset, so an upright blade or one bending
    // along its own width has across ~0 - noise, or NaN (which can blow up through the bloom). Where across is small
    // against the tangent, blend to the root's normal, always defined.
    const float acrossLen = length(across);
    const float acrossWeight = smoothstep(0.05, 0.25, acrossLen / max(length(tangent), 1e-6));
    const vec3 acrossN = mix(grassBladeRootNormal(), across / max(acrossLen, 1e-12), acrossWeight);
    return normalize(acrossN + side * ((t < 1.0 ? sideSign : 0.0) * u_grass_roundness));
}

void main()
{
    const uint vid = uint(gl_VertexIndex);
    const uint blade = vid >> GRASS_BLADE_VERTEX_SHIFT;
    const uint vert = vid & ((1u << GRASS_BLADE_VERTEX_SHIFT) - 1u);
    const uint segments = grassLodSegments(in_patchData.y >> 16);
    const float N = u_grass_bladesPerPatch;
    const float P = u_grass_patchSize;

    // The blade's spot: the rank's point of the patch's R2 sequence (offset per patch), with a small jitter.
    const vec2 origin = in_patchOrigins.xy;
    // The blade's random values: two per hash (16-bit halves) - bladeHash: the jitter; +1: height, facing; +2: lean
    // angle, lean amount; +3: wind phase, colour variation.
    const uint patchHash = grassHash2(ivec2(floor(origin / P + 0.5)));
    const uint bladeHash = grassHash(patchHash ^ (blade * 0x9E3779B9u));
    vec2 uv = fract(grassUnit2(patchHash) + float(blade) * vec2(0.7548776662, 0.5698402910));
    uv = fract(uv + (grassUnit2(bladeHash) - 0.5) * (0.5 / sqrt(N)));
    const vec2 xz = origin + uv * P;

#ifdef GRASS_NEAR_SHADOW
    // THE NEAR GRASS CASCADE's caster (GrassPipeline::recordNearShadow): the same blade into the extra shadow layer's
    // box; nothing past its half size x 1.5 + 2 m from the box's centre (the receivers' disc, and casters up-sun of
    // it). The blades never cast into the scene cascades: the canopy and this cascade replace that.
    if (distance(xz, u_grass_nearCentre) > u_grass_nearRange * 1.5 + 2.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
#endif

    const vec2 cellTemperature = unpackHalf2x16(in_patchData.w);
    const GrassGround ground = GrassGround(in_patchOrigins.zw, in_patchData.x, in_patchData.y & 0xFFFFu, cellTemperature.x);
    vec3 groundN;
    const float groundY = grassGroundHeight(ground, xz, groundN);
    const float dist = distance(vec3(xz.x, groundY, xz.y), u_viewPos);

    // Density (the COVER: the corners, bilinear - climate, slope, water, snow; x the clumps) x thinning, against the
    // blade's rank. Where the cover fades the blades get fewer AND smaller ("Size by cover"): fewer blades of full size
    // read as sparse long stalks.
    const vec4 corners = unpackUnorm4x8(in_patchData.z);
    const float cover = mix(mix(corners.x, corners.y, uv.x), mix(corners.z, corners.w, uv.x), uv.y);
    const float clump = grassClump(xz);
    const float density = cover * clump;
    const float coverSize = grassCoverSize(cover);
    const float thinning = grassKeep(dist);
    const float keep = density * thinning;
    const float rank = (float(blade) + 0.5) / N;
    const float grow = clamp((keep - rank) / (u_grass_growBand * keep), 0.0, 1.0); // keep = 0: -inf -> 0
    if (grow <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // every vertex of the blade lands here: no area, no fragments
        return;
    }

    // The root sinks below the mesh by "Root sink" plus half the tessellated relief (the TES's fade and slope gate), so
    // no blade floats over a hollow of the displaced ground; the blade grows by as much.
    float sink = u_grass_rootSink;
    if (u_terrainTess_enabled > 0.5 && dist < u_terrainTess_fadeEnd)
    {
        const float t = clamp((dist - u_terrainTess_fadeStart) * u_terrainTess_invFadeWidth, 0.0, 1.0);
        sink += 0.5 * u_terrainTess_depthGround * (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, groundN.y);
    }

    const vec2 heightFacing = grassUnit2(grassHash(bladeHash + 1u));
    const vec2 leanRandom = grassUnit2(grassHash(bladeHash + 2u));
    const vec2 phaseVariation = grassUnit2(grassHash(bladeHash + 3u));
    const float height = (u_grass_bladeHeight * mix(1.0 - u_grass_heightVariation, 1.0, heightFacing.x) * mix(0.6, 1.0, clump) * coverSize + sink) * grow;
    const vec3 root = vec3(xz.x, groundY - sink, xz.y);

    const float facing = heightFacing.y * GRASS_TWO_PI;
    // THE TURN TO THE VIEW: a blade seen edge-on is a line. Its width axis turns (per blade, at the root) toward the
    // view's horizontal perpendicular: its screen share |a| becomes mix(|a|, 1, "Turn to view" x smoothstep(0, "Turn
    // band", |a|)), and the depth part keeps its sign, so the axis stays unit. The band keeps it continuous where a changes sign.
    const vec2 toBlade = xz - u_viewPos.xz;
    const vec2 viewXZ = toBlade * inversesqrt(max(dot(toBlade, toBlade), 1e-8));
    const vec2 acrossXZ = vec2(-viewXZ.y, viewXZ.x);
    const vec2 facingXZ = vec2(cos(facing), sin(facing));
    const float a = dot(facingXZ, acrossXZ);
    const float turned = mix(abs(a), 1.0, u_grass_viewTurn * smoothstep(0.0, u_grass_viewTurnBand, abs(a)));
    const vec2 sideXZ = acrossXZ * (a < 0.0 ? -turned : turned)
        + viewXZ * ((dot(facingXZ, viewXZ) < 0.0 ? -1.0 : 1.0) * sqrt(max(1.0 - turned * turned, 0.0)));
    const vec3 side = vec3(sideXZ.x, 0.0, sideXZ.y);
    const float leanAngle = leanRandom.x * GRASS_TWO_PI;
    const vec2 lean = vec2(cos(leanAngle), sin(leanAngle)) * (u_grass_curvature * height * leanRandom.y);
    const float phase = phaseVariation.x * GRASS_TWO_PI;
    // The wind eases out with the distance ("Wind/Fade start / end"): far blades are a pixel wide, and their motion
    // only reads as grain. Both frames take the same factor, so the motion vectors stay exact.
    const float windFade = 1.0 - smoothstep(u_grass_windFadeStart, u_grass_windFadeEnd, dist);
    vec3 ctrl;
    const vec3 tip = grassTip(root, lean, grassWind(xz, u_timeSeconds, phase) * windFade, height, ctrl);

    // The vertex: row r of S (2 per row, sides -1 / +1), the tip last.
    const uint row = min(vert >> 1u, segments);
    const float t = float(row) / float(segments);
    const float sideSign = vert >= 2u * segments ? 0.0 : ((vert & 1u) != 0u ? 1.0 : -1.0);
    // Fewer blades far away get WIDER (toward the same coverage, capped), and never narrower than the pixel floor.
    const float widthScale = min(pow(1.0 / thinning, u_grass_widthCompensation), u_grass_maxWidthScale);
    g_blade.root = root;
    g_blade.side = side;
    const float widthSize = grassWidthSize(coverSize, clump);
    g_blade.halfWidth = 0.5 * max(u_grass_bladeWidth * widthScale * widthSize, dist * u_grass_minWidthPerMetre);
    g_blade.ctrl = ctrl;
    g_blade.tip = tip;
    // The normal: NEAR the curve's own (the bent top catches the light), easing into one LINEAR in t from "LOD 2
    // distance" to "LOD 3 distance" - a LOD 3 blade is ONE triangle, and the curve's tip normal spread over it flipped
    // it bright or dark at the switch.
    g_blade.linearBlend = smoothstep(u_grass_lod2Distance, u_grass_lod3Distance, dist);

    // GEOMORPH (the LODs nest: 8 -> 4 -> 2 -> 1 segments): over the last "LOD morph band" before the next LOD's
    // distance, the odd rows (the coarser LOD drops them) move onto the line between their neighbours, normals
    // included. At the switch the blade is the coarser one exactly. The patch's LOD comes from its NEAREST point.
    // The neighbours' midpoint on the quadratic Bezier is B(t) + h^2 (root - 2 ctrl + tip) exactly (B'' is constant, the
    // width is linear in t): one multiply-add, not two more curve points.
    const uint lod = in_patchData.y >> 16;
    const float nextLodDist = lod == 0u ? u_grass_lod1Distance : lod == 1u ? u_grass_lod2Distance : u_grass_lod3Distance;
    const float morph = lod < 3u && (row & 1u) != 0u
        ? smoothstep(nextLodDist * (1.0 - u_grass_lodMorphBand), nextLodDist, dist) : 0.0;
    const float h = 1.0 / float(segments);
    const float morphH2 = morph * h * h;
    const vec3 pos = grassBladePoint(t, sideSign) + morphH2 * (root - 2.0 * ctrl + tip);

#ifdef GRASS_NEAR_SHADOW
    gl_Position = u_grass_shadowViewProj * vec4(pos, 1.0);
#else
    // ORDER: every output but the normal is written FIRST, the normal last - written after the three curve evaluations,
    // the other outputs' inputs (the previous wind, the dryness noise, the canopy) stayed live across them: 52 -> 72
    // registers.
    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
    out_posAlong = vec4(pos, t);
    out_groundNormal = groundN;

    // The previous frame's point (g_blade keeps THIS frame's curve: the normal below reads it).
    vec3 ctrlPrev;
    const vec3 tipPrev = grassTip(root, lean, grassWind(xz, u_grass_prevTime, phase) * windFade, height, ctrlPrev);
    const vec3 prevPos = grassBezier(root, ctrlPrev, tipPrev, t) + side * (sideSign * g_blade.halfWidth * (1.0 - t))
        + morphH2 * (root - 2.0 * ctrlPrev + tipPrev);
    out_prevWorldDelta = prevPos - pos;

    const float dryNoise = grassValueNoise(xz * (u_grass_invClumpSize * 0.37) + 17.3);
    const float dryness = smoothstep(0.0, 0.3, dryNoise - (1.0 - u_grass_dryAmount));
    const float groundBlend = clamp(dist / u_grass_range, 0.0, 1.0) * u_grass_groundBlend;
    // The albedo factor: the per-blade variation x the COLD darkening (full at "Cold temperature", none from "Warm
    // temperature"; the patch's mean temperature - climate is km-scale, so no patch steps show).
    const float variation = 1.0 + u_grass_colorVariation * (phaseVariation.y * 2.0 - 1.0);
    const float warm = smoothstep(u_grass_coldTemperature, u_grass_warmTemperature, cellTemperature.y);
    out_blade = vec4(variation * mix(1.0 - u_grass_coldDarkening, 1.0, warm), dryness, groundBlend,
        grassCanopyExtinction(cover, widthSize, dist));

    // The canopy at this blade: its mean height (the ground under it takes the same); the depth is capped at the canopy
    // height (the sunk root is below the ground).
    const float canopyHeight = grassCanopyHeight(clump, coverSize);
    const float canopyDepth = clamp(canopyHeight - (pos.y - groundY), 0.0, canopyHeight);

    vec3 normal = vec3(0.0);
    if (g_blade.linearBlend < 1.0)
    {
        normal = grassBladeCurveNormal(t, sideSign);
        if (morph > 0.0)
            normal = mix(normal, 0.5 * (grassBladeCurveNormal(t - h, sideSign) + grassBladeCurveNormal(t + h, sideSign)), morph);
    }
    const vec3 tipAcross = cross(tip - grassBezier(root, ctrl, tip, 0.5), side);
    g_blade.tipN = tipAcross * inversesqrt(dot(tipAcross, tipAcross)); // |tipAcross| >= the chord's rise > 0
    normal = mix(normal, grassBladeLinearNormal(t, sideSign), g_blade.linearBlend);
    out_normalDepth = vec4(normal, canopyDepth);
#endif
}
