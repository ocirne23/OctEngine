#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FLOWERS (with CLUTTER_NEAR_SHADOW: the near grass cascade's casters). No vertex buffer: one indexed draw per LOD over
// a fixed topology (ClutterPipeline::buildFlowerIndices: the stem's strip, CLUTTER_FLOWER_PETALS quads, a centre quad), the
// record instance-rate. THE STEM is a GRASS BLADE (grass_wind.inc.glsl: the same Bezier, lean and wind, so it sways as the
// grass around it; motion = the same stem at last frame's time). THE HEAD sits at the tip in a frame along the stem's
// upper chord: RADIAL (petals around a disc, tilted up by `open`), SPIKE (a floret spiral up the stem's top), UMBEL (flat
// florets over a disc), BELL (florets hanging from the arching top), REED (long leaves in REED_TUFTS tufts + a CATTAIL
// spike), TUFT (no stalk: arching leaves, a sedge), GRAIN (a tuft of stalks with ears and awns), FERN (fronds with
// saw-toothed pinnae from a crown; the LARGE topology), ROSETTE / LEAFY (weeds: broad leaves round the root / in pairs up
// the stalk, + a spike). A slot past the petal count collapses (no area); a LOD with fewer slots than petals widens them.

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
layout (location = 2) out vec4 out_flower;     // x part (0 stem, 1 petal, 2 centre), y along it (0 base .. 1 tip), z across a textured frond (0 .. 1)
layout (location = 3) out vec3 out_prevWorldDelta;
layout (location = 4) flat out uvec2 out_look; // x albedo0 (petals), y albedo1 (centre)
layout (location = 5) flat out uint out_texture; // u_textures index + 1 (0 = none)
#endif

float g_across = 0.0; // flowerVertex: the textured frond's u (0 .. 1 across)

#define REED_TUFTS          3u         // a reed's leaves form this many tufts side by side
#define FLOWER_GOLDEN_ANGLE 2.39996323 // rad: spreads the leaves / florets evenly around

uint flowerStemSegments(uint lod) { return lod == 0u ? CLUTTER_FLOWER_STEM0 : lod == 1u ? CLUTTER_FLOWER_STEM1 : CLUTTER_FLOWER_STEM2; }
// The petal slots of a head's topology (clutterFlowerTopology: a fern builds over the large one).
uint flowerPetalSlots(uint head, uint lod)
{
    if (clutterFlowerTopology(head) == 1u)
        return lod == 0u ? CLUTTER_FLOWER_BIG_PETALS0 : lod == 1u ? CLUTTER_FLOWER_BIG_PETALS1 : CLUTTER_FLOWER_BIG_PETALS2;
    return lod == 0u ? CLUTTER_FLOWER_PETALS0 : lod == 1u ? CLUTTER_FLOWER_PETALS1 : CLUTTER_FLOWER_PETALS2;
}

// One LEAF of two quads (`leafHalf` 0 = the base half, 1 = the tip half): narrow at the base, widest at 40 % of its
// length, pointed; its midrib from `base` along `dir` (unit), the tip lowered by `droop` x the length and pushed by
// `sway` (the wind, x the square of the way along). Returns the corner; its normal faces up.
vec3 flowerLeaf(vec3 base, vec3 dir, vec3 across, float len, float width, float droop, vec2 sway, uint leafHalf, uint corner,
    out vec3 normal, out float along)
{
    const vec3 up = vec3(0.0, 1.0, 0.0);
    const bool start = corner < 2u;
    const float s = leafHalf == 0u ? (start ? 0.0 : 0.4) : (start ? 0.4 : 1.0);
    const float w = (leafHalf == 0u ? (start ? 0.25 : 1.0) : (start ? 1.0 : 0.05)) * width;
    const vec3 p = base + dir * (s * len) - up * (droop * len * s * s) + vec3(sway.x, 0.0, sway.y) * (s * s);
    const vec3 tangent = dir * len - up * (2.0 * droop * len * s) + vec3(sway.x, 0.0, sway.y) * (2.0 * s);
    normal = normalize(cross(tangent, across));
    if (dot(normal, up) < 0.0)
        normal = -normal;
    along = s;
    return p + across * (((corner == 1u || corner == 2u) ? 0.5 : -0.5) * w);
}

// Everything about this flower that does not depend on the time.
struct Flower
{
    vec3 root;
    float height;
    float headSize;
    float stemHalfWidth;
    float minHalfWidth; // the distance's floor on a ribbon's half width (no sub-pixel ribbons)
    vec2 lean;
    float phase;
    float rotation;
    uint head;
    uint petals;      // drawn (<= the LOD's slots)
    float petalScale; // > 1 when the LOD draws fewer petals than the flower has: wider petals
    float petalWidth; // x the head size
    float open;       // rad
    float awns;       // a grain's awns, 0..1 (the record's second-colour alpha: a flower has no spots)
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
        if (f.head == FLOWER_HEAD_GRAIN || f.head == FLOWER_HEAD_FERN)
        {
            // No main stem: a grain tuft draws each of its stalks in its slots, a fern only fronds.
            normal = up;
            part = 0.0;
            along = 0.0;
            return f.root;
        }
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
        if (j >= flowerPetalSlots(f.head, lod) || leaf >= max(f.petals / segs, 1u))
        {
            normal = up;
            along = 0.0;
            return f.root; // the centre / an unused slot: no area
        }
        const float fl = float(leaf);
        const float psi = fl * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const float lean = f.open * mix(0.55, 1.35, fract(fl * 0.618034 + f.phase));
        const float lenFrac = mix(0.65, 1.0, fract(fl * 0.754878 + f.rotation)); // len / height
        const float len = f.height * lenFrac;
        const vec2 wind = (tip.xz - f.root.xz - f.lean) * lenFrac;
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
        const uint slots = flowerPetalSlots(f.head, lod);
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
        along = corner < 2u ? 0.0 : 1.0;
        if (j >= slots)
        {
            // THE CATTAIL: a spike along the stalk's top (HeadSize long), facing the viewer as the stalk does.
            part = 2.0;
            const float tt = corner < 2u ? clamp(1.0 - 1.05 * f.headSize / f.height, 0.3, 0.95) : 0.97;
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
        const vec3 dir = o * sin(lean) + up * cos(lean); // unit: o is unit and horizontal
        const float lenFrac = tuftHeight * mix(0.6, 1.0, fract(fj * 0.754878 + f.rotation)); // len / height
        const float len = f.height * lenFrac;
        const vec3 across = vec3(-o.z, 0.0, o.x);
        const float width = f.petalWidth * f.headSize * f.petalScale;
        normal = cross(dir, across); // an orthonormal pair
        if (dot(normal, o) < 0.0)
            normal = -normal;
        normal = normalize(mix(normal, up, 0.3));
        const vec3 base = tuftCentre + o * (0.3 * width);
        if (corner < 2u)
            return base + across * (s * width);
        const vec2 sway = (tip.xz - f.root.xz - f.lean) * lenFrac;
        return base + dir * len + vec3(sway.x, 0.0, sway.y) + across * (s * 0.15 * width);
    }

    // The wind's push on the stem's tip (m, XZ): what the leaves, fronds and stalks of the heads below sway with.
    const vec2 stemSway = tip.xz - f.root.xz - f.lean;

    if (f.head == FLOWER_HEAD_GRAIN)
    {
        // A GRAIN STALK (wild wheat / barley / oat): ONE stalk at every LOD (more, dropped by the further LODs' fewer
        // slots, popped in and out), four slots - the stalk's two halves (a bend), the EAR at its top (HeadSize long,
        // PetalWidth x HeadSize wide, nodding toward the ground by Open) and the AWNS beyond it (a thin bristle, `Awns` x
        // 2 x the ear long; none at 0). The stalk and the awns are straw (Color2), the ear Color. The ribbons face the
        // viewer, as the stem does.
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        const uint stalk = j >> 2u;
        const uint sub = j & 3u;
        normal = up;
        along = 0.0;
        part = 2.0;
        const uint slots = flowerPetalSlots(f.head, lod);
        if (j >= slots || stalk >= 1u)
            return f.root; // the centre / an unused slot: no area
        const float fs = float(stalk);
        const float psi = fs * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const vec3 base = f.root + (stalk == 0u ? vec3(0.0) : o * (f.height * mix(0.04, 0.1, fract(fs * 0.618034 + f.phase))));
        const float heightFrac = stalk == 0u ? 1.0 : mix(0.82, 1.05, fract(fs * 0.754878 + f.rotation));
        const float len = f.height * heightFrac;
        const float leanAngle = radians(4.0) + radians(10.0) * fract(fs * 0.381966 + f.phase);
        const vec2 sway = stemSway * heightFrac;
        const vec3 c0 = base;
        const vec3 c1 = base + up * (0.6 * len);
        const vec3 c2 = base + o * (sin(leanAngle) * len) + up * (cos(leanAngle) * len) + vec3(sway.x, 0.0, sway.y);
        const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
        if (sub < 2u)
        {
            // THE STALK: its halves s 0 .. 0.5 .. 1.
            const float t = 0.5 * (float(sub) + (corner < 2u ? 0.0 : 1.0));
            const vec3 tangent = 2.0 * (1.0 - t) * (c1 - c0) + 2.0 * t * (c2 - c1);
            normal = normalize(cross(f.side, tangent) + 1e-4 * up);
            along = t;
            // As wide at its top as the head's rachis there (the texture's), 1 / 0.7 of that at the root - not the
            // flower stem's width, which follows the head size.
            const float stalkTop = max(CLUTTER_GRAIN_STALK_HALF_WIDTH * f.petalWidth * f.headSize * f.petalScale, f.minHalfWidth);
            return grassBezier(c0, c1, c2, t) + f.side * (s * 2.0 * stalkTop * mix(1.0 / 0.7, 1.0, t));
        }
        // The ear's direction: the stalk's tip tangent, turned toward the ground by Open (0 upright .. pi/2 hanging).
        const vec3 tipDir = normalize(c2 - c1);
        const vec3 earDir = normalize(mix(tipDir, -up, clamp(f.open / (0.5 * PI), 0.0, 0.95)));
        const float earLen = f.headSize;
        const float earWidth = f.petalWidth * f.headSize * f.petalScale;
        normal = normalize(cross(f.side, earDir) + 1e-4 * up);
        if (in_instData.x != 0u)
        {
            // TEXTURED: one strip over the ear AND its awns (ClutterTexture: the ear the first 1 / (1 + 2 awns) of it).
            if (sub == 3u)
                return f.root; // the awns are in the texture
            part = 1.0;
            along = corner < 2u ? 0.0 : 1.0;
            g_across = s + 0.5;
            float width = earWidth * CLUTTER_GRAIN_STRIP_WIDEN; // room for the awns to fan out
#ifdef CLUTTER_NEAR_SHADOW
            width = earWidth * 0.6; // no alpha test in the cascade: about the ear
#endif
            return c2 + earDir * (corner < 2u ? 0.0 : earLen * (1.0 + 2.0 * f.awns)) + f.side * (s * width);
        }
        if (sub == 2u)
        {
            // THE EAR: widest near its base, narrower to its tip.
            part = 1.0;
            along = corner < 2u ? 0.0 : 1.0;
            return corner < 2u ? c2 + f.side * (s * 0.8 * earWidth) : c2 + earDir * earLen + f.side * (s * 0.45 * earWidth);
        }
        // THE AWNS: a thin bristle from the ear's middle out past its tip.
        const float awnLen = f.awns * 2.0 * earLen;
        if (awnLen <= 0.0)
            return f.root;
        along = corner < 2u ? 0.0 : 1.0;
        return corner < 2u ? c2 + earDir * (0.5 * earLen) + f.side * (s * 0.2 * earWidth)
                           : c2 + earDir * (earLen + awnLen) + f.side * (s * 0.12 * earWidth);
    }

    if (f.head == FLOWER_HEAD_FERN)
    {
        // A FERN: fronds from one crown (the LARGE topology), `qpf` quads each. A frond leans out by Open, arches up and
        // over, its tip drooping and swaying. TEXTURED (the record's texture, ClutterTexture): a strip 2 x HeadSize wide (x
        // the frond's length share), the pinnae alpha-tested in it - only 4 / 3 / 2 quads for the bend. Untextured: a bare
        // stipe (its first fifth), then the blade, every quad narrow at its base and wide at its top, so the edge is
        // SAW-TOOTHED like pinnae. HeadSize = the widest pinna (a side), PetalWidth = how much of a pinna's width the
        // notches keep (0 = cut to the midrib, 1 = none).
        const bool textured = in_instData.x != 0u;
        const uint qpf = textured ? (lod == 0u ? 4u : lod == 1u ? 3u : 2u) : (lod == 0u ? 8u : lod == 1u ? 4u : 3u);
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        const uint frond = j / qpf;
        part = 1.0;
        normal = up;
        along = 0.0;
        const uint slots = flowerPetalSlots(f.head, lod);
        if (j >= slots || frond >= min(f.petals, slots / qpf))
            return f.root; // the centre / an unused slot: no area
        const float ff = float(frond);
        const float psi = ff * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const vec3 across = vec3(-o.z, 0.0, o.x);
        const float lean = f.open * mix(0.75, 1.25, fract(ff * 0.618034 + f.phase));
        const float lenFrac = mix(0.75, 1.0, fract(ff * 0.754878 + f.rotation));
        const float len = f.height * lenFrac;
        const vec2 sway = stemSway * lenFrac;
        const vec3 c0 = f.root;
        const vec3 c1 = f.root + o * (0.2 * len) + up * (0.65 * len);
        const vec3 c2 = f.root + o * (sin(lean) * len) + up * (len * max(cos(lean) - 0.45 * sin(lean), 0.05)) + vec3(sway.x, 0.0, sway.y);
        const uint k = j % qpf;
        const bool start = corner < 2u;
        const float sq = (float(k) + (start ? 0.0 : 1.0)) / float(qpf);
        const vec3 p = grassBezier(c0, c1, c2, sq);
        const vec3 tangent = 2.0 * (1.0 - sq) * (c1 - c0) + 2.0 * sq * (c2 - c1);
        normal = normalize(cross(tangent, across));
        if (dot(normal, up) < 0.0)
            normal = -normal;
        normal = normalize(mix(normal, up, 0.3));
        along = sq;
        const float side = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
        if (textured)
        {
            g_across = side + 0.5;
            float width = 2.0 * f.headSize * f.petalScale * lenFrac;
#ifdef CLUTTER_NEAR_SHADOW
            width *= 0.55; // no alpha test in the cascade: the strip narrowed to about the pinnae's cover
#endif
            return p + across * (side * width);
        }
        // The pinnae's envelope: a thin stipe below 20 %, then widest at ~40 % of the blade, a point at the tip.
        const float blade = clamp((sq - 0.2) / 0.8, 0.0, 1.0);
        const float envelope = sq < 0.2 ? 0.06 : sin(PI * pow(blade, 0.75));
        const float width = 2.0 * f.headSize * f.petalScale * envelope * (start ? f.petalWidth : 1.0);
        return p + across * (side * width);
    }

    if (f.head == FLOWER_HEAD_ROSETTE || f.head == FLOWER_HEAD_LEAFY)
    {
        // WEEDS. ROSETTE (dock, plantain): broad leaves lying round the root, raised by Open; LEAFY (nettle, goosefoot,
        // thistle): leaves in opposite pairs up the stalk, each pair turned a quarter, smaller toward the top. Two slots per
        // leaf (flowerLeaf); the CENTRE slot is a seed / flower spike on the stalk's top (Color2).
        const uint slots = flowerPetalSlots(f.head, lod);
        const uint pv = v - stemVerts;
        const uint j = pv >> 2u;
        const uint corner = pv & 3u;
        if (j >= slots)
        {
            // THE SPIKE: the stalk's top, facing the viewer as the stalk does - a quarter of it (a rosette's seed
            // stalk) or a tenth (a leafy weed's flowers).
            part = 2.0;
            const float spike = f.head == FLOWER_HEAD_ROSETTE ? 0.25 : 0.1;
            const float tt = corner < 2u ? 1.0 - spike : 1.0;
            const vec3 tangent = 2.0 * (1.0 - tt) * (ctrl - f.root) + 2.0 * tt * (tip - ctrl);
            normal = normalize(cross(f.side, tangent) + 1e-4 * up);
            along = corner < 2u ? 0.0 : 1.0;
            const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
            return grassBezier(f.root, ctrl, tip, tt) + f.side * (s * 0.35 * spike * f.height * mix(1.0, 0.6, along));
        }
        part = 1.0;
        const uint leaves = min(f.petals, slots / 2u);
        const uint leaf = j >> 1u;
        if (leaf >= leaves)
        {
            normal = up;
            along = 0.0;
            return f.root; // an unused slot: no area
        }
        const float fl = float(leaf);
        vec3 base;
        float psi, size, droop;
        vec2 sway;
        if (f.head == FLOWER_HEAD_ROSETTE)
        {
            psi = fl * FLOWER_GOLDEN_ANGLE + f.rotation;
            base = f.root;
            size = f.headSize * mix(0.7, 1.0, fract(fl * 0.754878 + f.rotation));
            droop = 0.2;
            sway = stemSway * 0.15; // low to the ground: they barely move
        }
        else
        {
            const uint pairs = (leaves + 1u) / 2u;
            const uint pair = leaf >> 1u;
            const float t = mix(0.15, 0.85, (float(pair) + 0.5) / float(pairs));
            psi = f.rotation + float(pair) * (0.5 * PI) + float(leaf & 1u) * PI; // opposite, each pair turned a quarter
            base = grassBezier(f.root, ctrl, tip, t);
            size = f.headSize * mix(1.0, 0.45, t) * mix(0.85, 1.1, fract(fl * 0.618034 + f.phase));
            droop = 0.35;
            sway = stemSway * (0.3 * t);
        }
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        const float raise = f.open * mix(0.7, 1.3, fract(fl * 0.381966 + f.phase));
        const vec3 dir = o * cos(raise) + up * sin(raise);
        return flowerLeaf(base, dir, vec3(-o.z, 0.0, o.x), size, f.petalWidth * size * f.petalScale, droop, sway, j & 1u, corner,
            normal, along);
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

    const uint slots = flowerPetalSlots(f.head, lod);
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
        dir = d * cos(f.open) + axis * sin(f.open); // d and axis: an orthonormal pair
        across = cross(axis, d);
        base = tip + d * (0.12 * H);
        len = H;
        tipWidth = f.petalWidth * H * f.petalScale;
        baseWidth = 0.35 * tipWidth;
    }
    else if (f.head == FLOWER_HEAD_UMBEL)
    {
        // Florets over a disc (a sunflower spiral), each a small flat quad facing along the axis.
        const float psi = fj * FLOWER_GOLDEN_ANGLE + f.rotation;
        const float r01 = sqrt((fj + 0.5) / n); // the radius / H
        const vec3 d = b1 * cos(psi) + b2 * sin(psi);
        across = cross(axis, d);
        len = 0.45 * H * f.petalScale;
        base = tip + d * (H * r01 - 0.5 * len) + axis * (0.1 * H * (1.0 - r01));
        dir = d;
        tipWidth = len;
        baseWidth = len;
    }
    else
    {
        // SPIKE / BELL: florets up the top of the stem, a spiral of outward directions (around the vertical: they read
        // as a column even when the stem leans).
        const bool spike = f.head == FLOWER_HEAD_SPIKE;
        const float up01 = (fj + 0.5) / n; // this floret's place up the head
        const float tj = spike ? 0.5 + 0.5 * up01 : 0.62 + 0.38 * up01;
        const vec3 at = grassBezier(f.root, ctrl, tip, tj);
        const float psi = fj * FLOWER_GOLDEN_ANGLE + f.rotation;
        const vec3 o = vec3(cos(psi), 0.0, sin(psi));
        across = vec3(-o.z, 0.0, o.x);
        const float size = H * f.petalScale * (spike ? mix(1.1, 0.55, up01) : 1.0);
        dir = spike ? (o + up * 0.7) * (1.0 / sqrt(1.49)) : (o * 0.35 - up) * (1.0 / sqrt(1.1225));
        base = at;
        len = size;
        tipWidth = f.petalWidth * size * 1.6;
        baseWidth = f.head == FLOWER_HEAD_BELL ? 0.6 * tipWidth : 0.4 * tipWidth;
    }
    normal = cross(dir, across); // unit: dir and across are an orthonormal pair for every head kind
    if (dot(normal, axis) < 0.0)
        normal = -normal;
    normal = normalize(mix(normal, axis, 0.35)); // a petal is soft, not a flat card
    const float s = (corner == 1u || corner == 2u) ? 0.5 : -0.5;
    if (in_instData.x != 0u)
    {
        // TEXTURED (a radial head's petal outline, ClutterTexture): a rectangle of the petal's full width.
        g_across = s + 0.5;
#ifdef CLUTTER_NEAR_SHADOW
        tipWidth *= 0.65; // no alpha test in the cascade
#endif
        baseWidth = tipWidth;
    }
    return corner < 2u ? base + across * (s * baseWidth) : base + dir * len + across * (s * tipWidth);
}

void main()
{
#ifdef CLUTTER_NEAR_SHADOW
    // Only the flowers around the near cascade's box (as the blades).
    const vec2 toNear = in_instPosScale.xz - u_grass_nearCentre;
    const float nearReach = u_grass_nearRange * 1.5 + 2.0;
    if (dot(toNear, toNear) > nearReach * nearReach)
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
    const vec3 toRoot = f.root - u_viewPos;
    const float dist = length(toRoot);
    f.minHalfWidth = 0.25 * dist * u_grass_minWidthPerMetre;
    f.stemHalfWidth = max(0.025 * f.headSize, f.minHalfWidth);
    const float leanAngle = grassUnit(grassHash(h + 12u)) * GRASS_TWO_PI;
    f.lean = vec2(cos(leanAngle), sin(leanAngle)) * (f.height * 0.2 * grassUnit(grassHash(h + 13u)));
    f.phase = grassUnit(grassHash(h + 14u)) * GRASS_TWO_PI;
    f.rotation = grassUnit(grassHash(h + 15u)) * GRASS_TWO_PI;
    f.head = in_instLook.w & 0xFFu;
    const uint typePetals = (in_instLook.w >> 8) & 0xFFu; // ClutterType: [1, 48]
    f.petals = min(typePetals, flowerPetalSlots(f.head, lod));
    const float petalRatio = float(typePetals) / float(f.petals);
    f.petalScale = f.head == FLOWER_HEAD_RADIAL || f.head == FLOWER_HEAD_UMBEL ? min(petalRatio, 3.0) : sqrt(petalRatio);
    f.petalWidth = float((in_instLook.w >> 16) & 0xFFu) * (1.0 / 255.0);
    f.open = float(in_instLook.w >> 24) * (PI / 255.0) - 0.5 * PI; // -pi/2 .. pi/2
    f.awns = unpackUnorm4x8(in_instLook.y).w;
    const vec2 across = vec2(-toRoot.z, toRoot.x);
    const float acrossLen2 = dot(across, across);
    f.side = acrossLen2 > 1e-8 ? vec3(across.x, 0.0, across.y) * inversesqrt(acrossLen2) : vec3(1.0, 0.0, 0.0);

    if (f.head == FLOWER_HEAD_BELL)
        f.lean *= 2.5; // an arching stem
    else if (f.head == FLOWER_HEAD_REED)
        f.lean *= 0.3; // a reed's stalk stands up
    else if (f.head == FLOWER_HEAD_LEAFY || f.head == FLOWER_HEAD_ROSETTE)
        f.lean *= 0.4; // a weed's stalk stands up, its leaves spread
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
    out_flower = vec4(part, along, g_across, 0.0);
    out_prevWorldDelta = prevPos - pos;
    out_look = in_instLook.xy;
    out_texture = in_instData.x;

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
