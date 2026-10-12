#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FLOWERS (with CLUTTER_NEAR_SHADOW: the near grass cascade's casters). No vertex buffer: one indexed draw per LOD
// over a fixed topology (ClutterPipeline::buildFlowerIndices - the stem's strip, CLUTTER_FLOWER_PETALS quads, one
// centre quad), the record as instance-rate attributes. Everything is built here from the record:
//   THE STEM is a GRASS BLADE: the same Bezier, lean and wind (grass_wind.inc.glsl), so a flower sways exactly as the
//   grass around it; the motion vectors are the same stem at last frame's time.
//   THE HEAD sits at the tip, in a frame along the stem's upper chord (it nods with the stem):
//     RADIAL - petals around a centre disc, tilted up by `open` (0 flat, ~60 deg a cup, below 0 swept back);
//     SPIKE  - florets in a spiral up the top of the stem;
//     UMBEL  - small flat florets over a disc;
//     BELL   - florets hanging from the arching top of the stem;
//     REED   - the petals are long, nearly pointed LEAVES (the stem's colours) in REED_TUFTS tufts side by side, the
//              centre a CATTAIL spike on the stalk's top;
//     TUFT   - no stalk: fine leaves (3 slots each near, 2 further out) rising as a bundle, then arching out (a sedge).
//   A slot past the petal count collapses to a point (no area). A LOD with fewer slots than petals widens them.

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
layout (location = 2) out vec4 out_flower;     // x part (0 stem, 1 petal, 2 centre), y along it (0 base .. 1 tip), zw unused
layout (location = 3) out vec3 out_prevWorldDelta;
layout (location = 4) flat out uvec2 out_look; // x albedo0 (petals), y albedo1 (centre)
#endif

#define REED_TUFTS          3u         // a reed's leaves form this many tufts side by side
#define FLOWER_GOLDEN_ANGLE 2.39996323 // rad: spreads the leaves / florets evenly around

uint flowerStemSegments(uint lod) { return lod == 0u ? CLUTTER_FLOWER_STEM0 : lod == 1u ? CLUTTER_FLOWER_STEM1 : CLUTTER_FLOWER_STEM2; }
uint flowerPetalSlots(uint lod)   { return lod == 0u ? CLUTTER_FLOWER_PETALS0 : lod == 1u ? CLUTTER_FLOWER_PETALS1 : CLUTTER_FLOWER_PETALS2; }

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
    if (f.head == FLOWER_HEAD_TUFT)
    {
        // A TUFT (sedge): no stalk (zero width, main) and no centre. A leaf takes `segs` slots (its bend): an arc out
        // of the root, leaning out by `open`, its tip drooping, swaying with the wind.
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        part = 1.0;
        const uint segs = lod == 0u ? 3u : 2u;
        const uint leaf = j / segs;
        if (j >= flowerPetalSlots(lod) || leaf >= max(f.petals / segs, 1u))
        {
            normal = up;
            along = 0.0;
            return f.root; // the centre / an unused slot: no area
        }
        const float fl = float(leaf);
        const float psi = fl * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const float lean = f.open * mix(0.55, 1.35, fract(fl * 0.618034 + f.phase));
        const float len = f.height * mix(0.65, 1.0, fract(fl * 0.754878 + f.rotation));
        const vec2 wind = (tip.xz - f.root.xz - f.lean) * (len / max(f.height, 1e-3));
        // A quadratic Bezier with its control STRAIGHT UP from the root: it rises as a bundle, then arches out to its
        // tip - out by sin(lean), the tip at least 15 % of its length up (else the low leaves lie in the relief).
        const vec3 c0 = f.root;
        const vec3 c1 = f.root + up * (0.55 * len);
        const vec3 c2 = f.root + o * (sin(lean) * len) + up * (len * max(cos(lean) - 0.35 * sin(lean), 0.15))
            + vec3(wind.x, 0.0, wind.y);
        // The arc at s (0 root .. 1 tip) and its width (full at the root, a point at the tip).
        const float s = (float(j % segs) + (corner < 2u ? 0.0 : 1.0)) / float(segs);
        const vec3 p = grassBezier(c0, c1, c2, s);
        const vec3 tangent = 2.0 * (1.0 - s) * (c1 - c0) + 2.0 * s * (c2 - c1);
        const vec3 across = vec3(-o.z, 0.0, o.x);
        normal = normalize(cross(tangent, across));
        if (dot(normal, up) < 0.0)
            normal = -normal;
        along = s;
        const float width = f.petalWidth * f.headSize * f.petalScale * mix(1.0, 0.08, s * s);
        return p + across * (((corner == 1u || corner == 2u) ? 0.5 : -0.5) * width);
    }
    if (f.head == FLOWER_HEAD_REED)
    {
        const uint slots = flowerPetalSlots(lod);
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
        along = corner < 2u ? 0.0 : 1.0;
        if (j >= slots)
        {
            // THE CATTAIL: a spike along the stalk's top (HeadSize long), facing the viewer as the stalk does.
            part = 2.0;
            const float tt = corner < 2u ? clamp(1.0 - 1.05 * f.headSize / max(f.height, 1e-3), 0.3, 0.95) : 0.97;
            const vec3 tangent = 2.0 * (1.0 - tt) * (ctrl - f.root) + 2.0 * tt * (tip - ctrl);
            normal = normalize(cross(f.side, tangent) + 1e-4 * up);
            return grassBezier(f.root, ctrl, tip, tt) + f.side * (s * 0.1 * f.headSize); // a tenth as wide as long
        }
        part = 0.0;
        if (j >= f.petals)
        {
            normal = up;
            return f.root; // an unused slot: no area
        }
        // A LEAF: long and nearly pointed, leaning out by `open`, its tip swaying with the stalk. Leaf j is in tuft
        // j % REED_TUFTS: the first around the stalk, the others 0.15 .. 0.25 x the height beside it, a little shorter.
        const float fj = float(j);
        const uint tuft = j % REED_TUFTS;
        vec3 tuftCentre = f.root;
        float tuftHeight = 1.0;
        if (tuft > 0u)
        {
            const float ta = f.rotation + float(tuft) * 2.2 + 0.6 * fract(f.phase * 1.7 + float(tuft) * 0.37);
            tuftCentre += vec3(cos(ta), 0.0, sin(ta)) * (f.height * mix(0.15, 0.25, fract(f.phase * 3.1 + float(tuft) * 0.61)));
            tuftHeight = mix(0.7, 0.9, fract(f.rotation * 2.3 + float(tuft) * 0.29));
        }
        const float psi = fj * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const float lean = f.open * mix(0.5, 1.4, fract(fj * 0.618034 + f.phase));
        const vec3 dir = normalize(o * sin(lean) + up * cos(lean));
        const float len = f.height * tuftHeight * mix(0.6, 1.0, fract(fj * 0.754878 + f.rotation));
        const vec3 across = vec3(-o.z, 0.0, o.x);
        const float width = f.petalWidth * f.headSize * f.petalScale;
        normal = normalize(cross(dir, across));
        if (dot(normal, o) < 0.0)
            normal = -normal;
        normal = normalize(mix(normal, up, 0.3));
        const vec3 base = tuftCentre + o * (0.3 * width);
        if (corner < 2u)
            return base + across * (s * width);
        const vec2 sway = (tip.xz - f.root.xz - f.lean) * (len / max(f.height, 1e-3));
        return base + dir * len + vec3(sway.x, 0.0, sway.y) + across * (s * 0.15 * width);
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
        const float theta = GRASS_TWO_PI * fj / n;
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
        const float psi = fj * FLOWER_GOLDEN_ANGLE + f.rotation;
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
        const float psi = fj * FLOWER_GOLDEN_ANGLE + f.rotation;
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
#ifdef CLUTTER_NEAR_SHADOW
    // Only the flowers around the near cascade's box (as the blades).
    if (distance(in_instPosScale.xz, u_grass_nearCentre) > u_grass_nearRange * 1.5 + 2.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
#endif
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
    f.open = (float(in_instLook.w >> 24) / 255.0 - 0.5) * PI;
    const vec3 toRoot = f.root - u_viewPos;
    const vec2 across = vec2(-toRoot.z, toRoot.x);
    const float acrossLen2 = dot(across, across);
    f.side = acrossLen2 > 1e-8 ? vec3(across.x, 0.0, across.y) * inversesqrt(acrossLen2) : vec3(1.0, 0.0, 0.0);
    if (f.head == FLOWER_HEAD_BELL)
        f.lean *= 2.5; // an arching stem
    else if (f.head == FLOWER_HEAD_REED)
        f.lean *= 0.3; // a reed's stalk stands up
    else if (f.head == FLOWER_HEAD_TUFT)
        f.stemHalfWidth = 0.0; // a tuft has no stalk: only its sway drives the leaves

    // The grass's wind, easing out with the distance as the grass's does.
    const float windFade = 1.0 - smoothstep(u_grass_windFadeStart, u_grass_windFadeEnd, dist);
    vec3 ctrl;
    const vec3 tip = grassTip(f.root, f.lean, grassWind(f.root.xz, u_timeSeconds, f.phase) * windFade, f.height, ctrl);

    vec3 normal;
    float part, along;
    const vec3 pos = flowerVertex(f, uint(gl_VertexIndex), lod, ctrl, tip, normal, part, along);
#ifdef CLUTTER_NEAR_SHADOW
    gl_Position = u_grass_shadowViewProj * vec4(pos, 1.0);
#else
    vec3 ctrlPrev;
    const vec3 tipPrev = grassTip(f.root, f.lean, grassWind(f.root.xz, u_grass_prevTime, f.phase) * windFade, f.height, ctrlPrev);
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
