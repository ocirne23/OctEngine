#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FLOWERS (StaticMeshGraphicsPipeline's flower pipeline; ClutterPipeline's near-shadow casters with
// CLUTTER_NEAR_SHADOW). No vertex buffer: one indexed draw per flower LOD over a fixed topology (ClutterPipeline::
// buildFlowerIndices - the stem's strip, CLUTTER_FLOWER_PETALS quads, one centre quad), the record as instance-rate
// attributes. Everything else is built here from the record:
//   THE STEM is a GRASS BLADE: the same quadratic Bezier, lean and wind (grass_wind.inc.glsl), so a flower sways
//   exactly as the grass around it; its motion vectors are the same stem at last frame's time.
//   THE HEAD sits at the stem's tip, in a frame along the stem's upper chord (it nods with the stem):
//     RADIAL - `petals` petals around a centre, tilted up by `open` (0 a flat daisy, ~60 deg a tulip's cup, below 0
//              swept back like a coneflower), + the centre disc;
//     SPIKE  - florets in a spiral up the top of the stem (lupine, lavender, foxglove);
//     UMBEL  - small flat florets spread over a disc (yarrow, Queen Anne's lace);
//     BELL   - florets hanging from the arching top of the stem (bluebell).
//   A petal slot past the flower's petal count collapses to a point (no area). At a coarser LOD with fewer slots than
//   petals the petals get wider, so the head keeps its coverage.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
layout (binding = 14, std430) readonly buffer InVertices { MeshVertex in_vertices[]; }; // grass.inc.glsl's ground (unused here)
#include "grass.inc.glsl"
#include "wind.inc.glsl"
#include "grass_wind.inc.glsl"
#include "clutter.inc.glsl"

layout (location = 0) in vec4 in_instPosScale;
layout (location = 1) in uvec4 in_instData;
layout (location = 2) in uvec4 in_instLook;

#ifndef CLUTTER_NEAR_SHADOW
layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_flower;          // x part (0 stem, 1 petal, 2 centre), y along it (0 base .. 1 tip), z cold factor, w unused
layout (location = 3) out vec3 out_prevWorldDelta;
layout (location = 4) flat out uvec2 out_look;      // x albedo0 (petals), y albedo1 (centre)
#endif

uint flowerStemSegments(uint lod) { return lod == 0u ? CLUTTER_FLOWER_STEM0 : lod == 1u ? CLUTTER_FLOWER_STEM1 : CLUTTER_FLOWER_STEM2; }
uint flowerPetalSlots(uint lod) { return lod == 0u ? CLUTTER_FLOWER_PETALS0 : lod == 1u ? CLUTTER_FLOWER_PETALS1 : CLUTTER_FLOWER_PETALS2; }

// Everything about this flower that does not depend on the time.
struct Flower
{
    vec3 root;
    float height;
    float headSize;
    float stemHalfWidth;
    vec2 lean;
    float phase;
    float rotation;
    uint head;
    uint petals;      // drawn (<= the LOD's slots)
    float petalScale; // > 1 when the LOD draws fewer petals than the flower has: wider petals
    float petalWidth; // x the head size
    float open;       // rad
    vec3 side;        // the stem ribbon's width axis (across the view)
};

// A point of the stem (sideSign -1 / +1) at t, the stem's curve (root, ctrl, tip).
vec3 flowerStemPoint(Flower f, vec3 ctrl, vec3 tip, float t, float sideSign)
{
    return grassBezier(f.root, ctrl, tip, t) + f.side * (sideSign * f.stemHalfWidth * (1.0 - 0.4 * t));
}

// The vertex `v` of the flower in the stem shape (ctrl, tip): its position, normal (one face; the FS flips it to the
// viewer), part and position along its part.
vec3 flowerVertex(Flower f, uint v, uint lod, vec3 ctrl, vec3 tip, out vec3 normal, out float part, out float along)
{
    const uint S = flowerStemSegments(lod);
    const uint stemVerts = 2u * (S + 1u);
    const vec3 up = vec3(0.0, 1.0, 0.0);
    if (v < stemVerts)
    {
        const float t = float(v >> 1u) / float(S);
        const float sideSign = (v & 1u) != 0u ? 1.0 : -1.0;
        const vec3 tangent = 2.0 * (1.0 - t) * (ctrl - f.root) + 2.0 * t * (tip - ctrl);
        normal = normalize(cross(f.side, tangent) + 1e-4 * up);
        part = 0.0;
        along = t;
        return flowerStemPoint(f, ctrl, tip, t, sideSign);
    }
    // The head's frame: along the stem's upper chord (it always rises - the tangent at the tip is the horizontal
    // lean), a random turn about it.
    vec3 axis = normalize(tip - grassBezier(f.root, ctrl, tip, 0.7));
    if (f.head == FLOWER_HEAD_BELL)
        axis = normalize(mix(axis, -up, 0.55));
    const vec3 ref = abs(axis.y) < 0.95 ? up : vec3(1.0, 0.0, 0.0);
    vec3 b1 = normalize(cross(axis, ref));
    vec3 b2 = cross(axis, b1);
    const float cr = cos(f.rotation), sr = sin(f.rotation);
    const vec3 r1 = b1 * cr + b2 * sr;
    b2 = b2 * cr - b1 * sr;
    b1 = r1;

    const uint slots = flowerPetalSlots(lod);
    const uint pv = v - stemVerts;
    const uint j = pv >> 2u;
    const uint corner = pv & 3u;
    const float H = f.headSize;
    part = 1.0;
    along = corner < 2u ? 0.0 : 1.0;
    if (j >= slots)
    {
        // THE CENTRE (radial heads only): a disc at the top of the stem, raised for a swept-back (coneflower) head.
        part = 2.0;
        along = 0.5;
        normal = axis;
        if (f.head != FLOWER_HEAD_RADIAL)
            return tip;
        const float c = 0.28 * H;
        const vec3 centre = tip + axis * (0.04 * H + 0.35 * H * max(-f.open, 0.0));
        const vec2 sq = vec2((corner == 1u || corner == 2u) ? 1.0 : -1.0, corner >= 2u ? 1.0 : -1.0);
        return centre + (b1 * sq.x + b2 * sq.y) * c;
    }
    if (j >= f.petals)
    {
        normal = axis;
        return tip; // an unused slot: no area
    }
    const float fj = float(j);
    const float n = float(f.petals);
    vec3 base, dir, across;
    float baseWidth, tipWidth, len;
    if (f.head == FLOWER_HEAD_RADIAL)
    {
        const float theta = 6.28318530718 * fj / n;
        const vec3 d = b1 * cos(theta) + b2 * sin(theta);
        dir = normalize(d * cos(f.open) + axis * sin(f.open));
        across = normalize(cross(axis, d));
        base = tip + d * (0.12 * H);
        len = H;
        tipWidth = f.petalWidth * H * f.petalScale;
        baseWidth = 0.35 * tipWidth;
    }
    else if (f.head == FLOWER_HEAD_UMBEL)
    {
        // Florets over a disc (a sunflower spiral), each a small flat quad facing along the axis.
        const float psi = fj * 2.39996323 + f.rotation;
        const float r = H * sqrt((fj + 0.5) / n);
        const vec3 d = b1 * cos(psi) + b2 * sin(psi);
        across = normalize(cross(axis, d));
        len = 0.45 * H * f.petalScale;
        base = tip + d * r - d * (0.5 * len) + axis * (0.1 * H * (1.0 - r / max(H, 1e-4)));
        dir = d;
        tipWidth = len;
        baseWidth = len;
    }
    else
    {
        // SPIKE / BELL: florets up the top of the stem, a spiral of outward directions (around the vertical: they read
        // as a column even when the stem leans).
        const float tj = (f.head == FLOWER_HEAD_SPIKE ? 0.5 : 0.62) + (f.head == FLOWER_HEAD_SPIKE ? 0.5 : 0.38) * (fj + 0.5) / n;
        const vec3 at = grassBezier(f.root, ctrl, tip, tj);
        const float psi = fj * 2.39996323 + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        across = vec3(-o.z, 0.0, o.x);
        const float size = H * f.petalScale * (f.head == FLOWER_HEAD_SPIKE ? mix(1.1, 0.55, (fj + 0.5) / n) : 1.0);
        dir = f.head == FLOWER_HEAD_SPIKE ? normalize(o + up * 0.7) : normalize(o * 0.35 - up);
        base = at;
        len = size;
        tipWidth = f.petalWidth * size * 1.6;
        baseWidth = f.head == FLOWER_HEAD_BELL ? 0.6 * tipWidth : 0.4 * tipWidth;
    }
    normal = normalize(cross(dir, across));
    if (dot(normal, axis) < 0.0)
        normal = -normal;
    normal = normalize(mix(normal, axis, 0.35)); // a petal is soft, not a flat card
    const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
    return corner < 2u ? base + across * (s * baseWidth) : base + dir * len + across * (s * tipWidth);
}

void main()
{
    const uint lod = (in_instData.z >> 16) & 0xFFu;
    const uint h = in_instData.w;
    const vec2 stemHead = unpackHalf2x16(in_instLook.z);
    const float scale = in_instPosScale.w;

    Flower f;
    f.root = in_instPosScale.xyz;
    f.height = stemHead.x * scale * mix(0.75, 1.15, grassUnit(grassHash(h + 11u)));
    f.headSize = stemHead.y * scale;
    const float dist = distance(f.root, u_viewPos);
    f.stemHalfWidth = 0.5 * max(0.05 * f.headSize, dist * u_grass_minWidthPerMetre * 0.5);
    const float leanAngle = grassUnit(grassHash(h + 12u)) * GRASS_TWO_PI;
    f.lean = vec2(cos(leanAngle), sin(leanAngle)) * (f.height * 0.2 * grassUnit(grassHash(h + 13u)));
    f.phase = grassUnit(grassHash(h + 14u)) * GRASS_TWO_PI;
    f.rotation = grassUnit(grassHash(h + 15u)) * GRASS_TWO_PI;
    f.head = in_instLook.w & 0xFFu;
    const uint typePetals = max((in_instLook.w >> 8) & 0xFFu, 1u);
    f.petals = min(typePetals, flowerPetalSlots(lod));
    f.petalScale = f.head == FLOWER_HEAD_RADIAL || f.head == FLOWER_HEAD_UMBEL
        ? min(float(typePetals) / float(f.petals), 3.0) : sqrt(float(typePetals) / float(f.petals));
    f.petalWidth = float((in_instLook.w >> 16) & 0xFFu) / 255.0;
    f.open = (float(in_instLook.w >> 24) / 255.0 - 0.5) * 3.14159265;
    const vec3 toRoot = f.root - u_viewPos;
    const vec2 across = vec2(-toRoot.z, toRoot.x);
    f.side = dot(across, across) > 1e-8 ? vec3(normalize(across).x, 0.0, normalize(across).y) : vec3(1.0, 0.0, 0.0);
    if (f.head == FLOWER_HEAD_BELL)
        f.lean *= 2.5; // an arching stem

    // The wind: the grass's (the same blade), easing out with the distance as the grass's does.
    const float windFade = 1.0 - smoothstep(u_grass_windFadeStart, u_grass_windFadeEnd, dist);
    vec3 ctrl, ctrlPrev;
    const vec3 tip = grassTip(f.root, f.lean, grassWind(f.root.xz, u_timeSeconds, f.phase) * windFade, f.height, ctrl);
    const vec3 tipPrev = grassTip(f.root, f.lean, grassWind(f.root.xz, u_grass_prevTime, f.phase) * windFade, f.height, ctrlPrev);

    vec3 normal;
    float part, along;
    const vec3 pos = flowerVertex(f, uint(gl_VertexIndex), lod, ctrl, tip, normal, part, along);
#ifdef CLUTTER_NEAR_SHADOW
    if (distance(f.root.xz, u_grass_nearCentre) > u_grass_nearRange * 1.5 + 2.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    gl_Position = u_grass_shadowViewProj * vec4(pos, 1.0);
#else
    vec3 prevNormal;
    float prevPart, prevAlong;
    const vec3 prevPos = flowerVertex(f, uint(gl_VertexIndex), lod, ctrlPrev, tipPrev, prevNormal, prevPart, prevAlong);
    out_pos = pos;
    out_normal = normal;
    out_flower = vec4(part, along, 0.0, 0.0);
    out_prevWorldDelta = prevPos - pos;
    out_look = in_instLook.xy;
    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
