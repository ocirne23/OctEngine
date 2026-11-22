// THE GRASS BLADE MODEL, in three steps; grass.vs.glsl runs all three for every vertex.
// grassTuft: the tuft's root (the R2 point), its type, size, ground (height + facet normal), distance, dry patch,
//   relief sink and the wind field at its root - everything its blades share.
// grassBuildBlade: one blade of a tuft into g_blade - its place in the tuft's spiral, its density test (false: not
//   drawn), its curve this frame and last (the wind's push with its own sway phase), its width, its normals' ends, its
//   look. Its ground is the tuft's, followed along the facet's slope (a member stands a few centimetres off the root).
// grassVertex: one vertex of g_blade (row, side), the geomorph, the normal.
// Needs shared.inc.glsl, the includer's in_terrainVertices and GRASS_NOISE_SAMPLER, then grass.inc.glsl, wind.inc.glsl
// and grass_wind.inc.glsl. GRASS_NEAR_SHADOW: the near grass cascade's casters (the position alone).

#ifndef GRASS_BLADE_INC_GLSL
#define GRASS_BLADE_INC_GLSL

// One visible patch (the cull's record, RendererVKLayout::GrassPatchGpu), decoded.
struct GrassPatchIn
{
    vec2 origin;          // the patch's min corner (world XZ)
    GrassGround ground;   // its terrain chunk
    vec4 corners;         // the corner densities
    float temperature;    // C: the patch's mean (the cold darkening)
    uint lod;
};

GrassPatchIn grassPatchIn(vec4 origins, uvec4 data)
{
    const vec2 cellTemperature = unpackHalf2x16(data.w);
    GrassPatchIn p;
    p.origin = origins.xy;
    p.ground = GrassGround(origins.zw, data.x, data.y & 0xFFFFu, cellTemperature.x);
    p.corners = unpackUnorm4x8(data.z);
    p.temperature = cellTemperature.y;
    p.lod = (data.y >> 16) & 3u;
#ifdef GRASS_NEAR_SHADOW
    p.lod = grassCasterLod(p.lod);
#endif
    return p;
}

// THE GRASS TYPE of a tuft rooted at xz: each type's weight in the types' patch pattern there (grass.inc.glsl
// grassTypeCorners), with "Type mixing" of a random score per tuft (`random`: four bytes) mixed in, x its share. The
// highest score wins (no type with a share: type 0).
uint grassType(vec2 xz, uint random)
{
    const vec4 score = mix(grassTypeCorners(xz), unpackUnorm4x8(random), u_grass_typeMixing) * grassTypeShares();
    uint best = 0u;
    for (uint i = 1u; i < GRASS_TYPES; ++i)
        if (score[i] > score[best])
            best = i;
    return best;
}

// What a tuft's blades share.
struct GrassTuft
{
    vec2 xz;          // the root
    uint hash;        // + 1: the spiral's turn, the size; + 2: the type's random scores
    float turn;       // rad: the spiral's start
    float size;       // x the blades' height: 1 - "Tuft size variation" x a random
    uint type;
    float groundY;    // the ground at the root, and its facet normal (its slope carries the members' ground)
    vec3 groundN;
    float dist;       // m: from the camera (the thinning, the LODs, the wind fade, the pixel floor)
    float sink;      // m: the roots under the mesh ("Root sink" + half the relief)
    vec4 wind;        // grassWindField at the root, this frame
#ifndef GRASS_NEAR_SHADOW
    vec4 windPrev;    // ... and last frame (the motion vectors)
    float dryness;    // the dry patches at the root
#endif
};

GrassTuft grassTuft(GrassPatchIn p, uint tuft)
{
    const float P = u_grass_patchSize;
    const uint T = uint(u_grass_tufts);
    const uint patchHash = grassHash2(ivec2(floor(p.origin / P + 0.5)));
    GrassTuft t;
    t.hash = grassHash(patchHash ^ (tuft * 0x9E3779B9u));
    // The root: the tuft's point of the patch's R2 sequence (offset per patch), with a small jitter.
    vec2 uv = fract(grassUnit2(patchHash) + float(tuft) * vec2(0.7548776662, 0.5698402910));
    uv = fract(uv + (grassUnit2(t.hash) - 0.5) * (0.5 / sqrt(float(T))));
    t.xz = p.origin + uv * P;
    const vec2 turnSize = grassUnit2(grassHash(t.hash + 1u));
    t.turn = turnSize.x * GRASS_TWO_PI;
    t.size = 1.0 - u_grass_tuftSizeVariation * turnSize.y;
    t.type = grassType(t.xz, grassHash(t.hash + 2u));
    t.groundY = grassGroundHeight(p.ground, t.xz, t.groundN);
    t.dist = distance(vec3(t.xz.x, t.groundY, t.xz.y), u_viewPos);
    // The roots sink below the mesh by "Root sink" plus half the tessellated relief (the TES's fade and slope gate), so
    // no blade floats over a hollow of the displaced ground; the blades grow by as much.
    t.sink = u_grass_rootSink;
    if (u_terrainTess_enabled > 0.5 && t.dist < u_terrainTess_fadeEnd)
    {
        const float f = clamp((t.dist - u_terrainTess_fadeStart) * u_terrainTess_invFadeWidth, 0.0, 1.0);
        t.sink += 0.5 * u_terrainTess_depthGround * (1.0 - pow(f, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, t.groundN.y);
    }
    t.wind = grassWindField(t.xz, u_timeSeconds);
#ifndef GRASS_NEAR_SHADOW
    t.windPrev = grassWindField(t.xz, u_grass_prevTime);
    t.dryness = grassDryness(t.xz);
#endif
    return t;
}

// The blade being built (grassBuildBlade): its curve, its width axis and half width at the root, its normals' terms,
// whether it is a seed stalk, and what its vertices pass on.
// THE CURVE: a quadratic Bezier from the root, its control point straight above the root at the tip's height - so the
// point at t is root + (t^2 tip.x, (2t - t^2) tip.y, t^2 tip.z) (tip: the tip's offset from the root), and last frame's
// point is that + (t^2, 2t - t^2, t^2) x tipPrevDelta.
// THE NORMALS (without the edge tilt) all lie in ONE PLANE: the upright stem's normal n0 = cross(up, side) and up - for
// any direction a along the blade, cross(a, side) = a.y n0 - dot(a.xz, n0.xz) up. So a normal here is a vec2 (x along
// n0, y up), built from scalars: the tip offset's rise (tip.y) and its part across the blade's face (`turn`).
struct GrassBlade
{
    vec3 root;
    vec3 tip;            // the tip's offset from the root (grassTipOffset)
    vec2 side;           // the width axis (XZ, unit)
    float halfWidth;
    float linearBlend;   // the blend from the curve normal to the linear one
    float turn;          // dot(tip.xz, n0.xz): the lean + wind across the blade's face
    vec2 tipN;           // the linear normal's tip end (its root end: n0 = (1, 0))
    bool stalk;
    float dist;          // the tuft's (the geomorph)
    float groundY;       // the ground under the root (the canopy depth)
#ifndef GRASS_NEAR_SHADOW
    vec3 tipPrevDelta;   // last frame's tip offset - this frame's
    vec3 groundN;        // the facet normal (flat)
    vec4 look;           // flat: x the albedo factor, y dryness, z the ground normal blend, w the canopy extinction
    uint lookBits;       // flat: the type | seed stalk << 2 | the seed stalks' cover of this blade (unorm8) << 8
    float canopyHeight;
#endif
};

GrassBlade g_blade;

// The blade's width at t, x its half width at the root: a blade narrows linearly to its tip; a SEED STALK keeps most of
// its width up to its seed head (the top "Seed head length"), which swells to "Seed head width" x it and comes to a point.
float grassBladeWidth(float t)
{
    if (!g_blade.stalk)
        return 1.0 - t;
    const float s = clamp((t - (1.0 - u_grass_seedLength)) / u_grass_seedLength, 0.0, 1.0);
    return max((1.0 - 0.4 * t) * (1.0 - s), u_grass_seedHeadWidth * sin(GRASS_TWO_PI * 0.5 * s) * (1.0 - s * s));
}

// The shading normal (one face; the FS flips it to the viewer, adds the edge tilt and normalizes) is
// mix(CURVE, LINEAR, linearBlend), each a vec2 in the normals' plane (GrassBlade):
// LINEAR: mix(n0, tipN, t) - root -> across the upright stem, tip -> across the chord of the upper half (tip -
// curve(0.5)), which always rises. Every LOD interpolates it the same. It is affine in t, so the geomorph's neighbour
// average is itself: only the curve part morphs.
// CURVE: across the curve's tangent (half of it: (t tip.x, (1 - t) tip.y, t tip.z)).
vec2 grassBladeCurveNormal(float t)
{
    const float rise = (1.0 - t) * g_blade.tip.y;
    const vec2 across = vec2(rise, -t * g_blade.turn);
    // DEGENERATE at the tip: the tangent there is the horizontal lean + wind offset, so an upright blade or one bending
    // along its own width has across ~0 - noise, or NaN (which can blow up through the bloom). Where across is small
    // against the tangent, blend to the root's normal, always defined.
    const float acrossLen = length(across);
    const float tangentLen = sqrt(rise * rise + t * t * dot(g_blade.tip.xz, g_blade.tip.xz));
    const float acrossWeight = smoothstep(0.05, 0.25, acrossLen / max(tangentLen, 1e-6));
    return normalize(mix(vec2(1.0, 0.0), across / max(acrossLen, 1e-12), acrossWeight));
}

// One BLADE of tuft `t`: rank `blade` (its place in the patch's ranked list) = member `member` of the tuft. False = not
// drawn (thinned out, or - the near casters - out of the cascade's reach). Fills g_blade.
// The random values: two per hash (16-bit halves) - bladeHash: its place in the spiral; +1: height, facing; +2: lean
// angle, lean amount; +3: wind phase, colour variation; +4: seed stalk.
bool grassBuildBlade(GrassPatchIn p, GrassTuft t, uint blade, uint member)
{
    const uint bladeHash = grassHash(t.hash ^ ((member + 1u) * 0x85EBCA6Bu));
    // Its place: a spiral out from the root (the golden angle), inner members first - a sparser tuft keeps its core.
    const vec2 spiral = grassUnit2(bladeHash);
    const float memberAngle = float(member) * 2.39996323 + t.turn + (spiral.x - 0.5) * 0.8;
    const vec2 outward = vec2(cos(memberAngle), sin(memberAngle));
    const float memberRing = sqrt((float(member) + 0.5) / u_grass_tuftMembers) * mix(0.7, 1.0, spiral.y);
    const vec2 offset = outward * (u_grass_tuftRadius * memberRing);
    const vec2 xz = t.xz + offset;
#ifdef GRASS_NEAR_SHADOW
    // THE NEAR GRASS CASCADE's caster: nothing past its half size x 1.5 + 2 m from the box's centre (the receivers' disc,
    // and casters up-sun of it). The blades never cast into the scene cascades: the canopy and this cascade replace that.
    if (distance(xz, u_grass_nearCentre) > u_grass_nearRange * 1.5 + 2.0)
        return false;
#endif

    // Density (the COVER: the corners, bilinear at the blade - climate, slope, water, snow) x
    // thinning, against the blade's rank. Where the cover fades the blades get fewer AND smaller ("Size by cover").
    const vec2 uv = clamp((xz - p.origin) / u_grass_patchSize, vec2(0.0), vec2(1.0)); // the member may stand just outside
    const float cover = mix(mix(p.corners.x, p.corners.y, uv.x), mix(p.corners.z, p.corners.w, uv.x), uv.y);
    const float coverSize = grassCoverSize(cover);
    const float thinning = grassKeep(t.dist);
    const float keep = cover * thinning;
    const float rank = (float(blade) + 0.5) / u_grass_bladesPerPatch;
    const float grow = clamp((keep - rank) / (u_grass_growBand * keep), 0.0, 1.0); // keep = 0: -inf -> 0
    if (grow <= 0.0)
        return false;

    // The ground under it: the tuft's, along the facet's slope.
    const vec3 n = t.groundN;
    g_blade.groundY = t.groundY - (n.x * offset.x + n.z * offset.y) / n.y;
    g_blade.dist = t.dist;

    const vec2 heightFacing = grassUnit2(grassHash(bladeHash + 1u));
    const vec2 leanRandom = grassUnit2(grassHash(bladeHash + 2u));
    const vec2 phaseVariation = grassUnit2(grassHash(bladeHash + 3u));
    const float stalkRandom = grassUnit2(grassHash(bladeHash + 4u)).x;

    // THE TUFT's type and size; a seed stalk is taller and thin.
    const vec4 typeShape = u_grass_typeShape[t.type]; // x height, y width, z curvature (factors), w share
    g_blade.stalk = stalkRandom < u_grass_typeTint[t.type].w;
    const float typeHeight = typeShape.x * t.size * (g_blade.stalk ? u_grass_seedHeight : 1.0);
    const float height = (u_grass_bladeHeight * typeHeight * mix(1.0 - u_grass_heightVariation, 1.0, heightFacing.x) * coverSize + t.sink) * grow;
    g_blade.root = vec3(xz.x, g_blade.groundY - t.sink, xz.y);

    // FAN OUT: an outer member faces across its way out from the root and leans out along it ("Fan out", by how far out
    // it stands); the inner ones keep a random facing and lean.
    const float fan = u_grass_tuftFan * clamp(memberRing * 1.5, 0.0, 1.0);
    const float facing = mix(heightFacing.y * GRASS_TWO_PI, memberAngle + 0.25 * GRASS_TWO_PI + (heightFacing.y - 0.5) * 1.2, fan);
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
    g_blade.side = sideXZ;

    // The lean (a seed stalk stands stiffer: a third of it) and the wind - the tuft's field, the blade's own sway phase.
    // The wind eases out with the distance ("Wind/Fade start / end"): far blades are a pixel wide, and their motion only
    // reads as grain. Both frames take the same factor, so the motion vectors stay exact.
    const float leanAngle = leanRandom.x * GRASS_TWO_PI;
    const vec2 lean = (vec2(cos(leanAngle), sin(leanAngle)) * (1.0 - fan) + outward * (fan * 1.4))
        * (u_grass_curvature * typeShape.z * (g_blade.stalk ? 0.35 : 1.0) * height * leanRandom.y);
    const float phase = phaseVariation.x * GRASS_TWO_PI;
    const float windFade = 1.0 - smoothstep(u_grass_windFadeStart, u_grass_windFadeEnd, t.dist);
    g_blade.tip = grassTipOffset(lean, grassWindPush(t.wind, u_timeSeconds, phase) * windFade, height);

    // Fewer blades far away get WIDER (toward the same coverage, capped), and never narrower than the pixel floor.
    const float widthScale = min(pow(1.0 / thinning, u_grass_widthCompensation), u_grass_maxWidthScale);
    const float widthSize = grassWidthSize(coverSize);
    // A SEED STALK past the LODs that hold its head is a wider spear (grass.inc.glsl grassStalkCoarse).
    const float stalkCoarse = g_blade.stalk ? grassStalkCoarse(t.dist) : 0.0;
    const float typeWidth = typeShape.y * (g_blade.stalk ? mix(u_grass_seedWidth, max(u_grass_seedWidth, GRASS_STALK_COARSE_WIDTH), stalkCoarse) : 1.0);
    g_blade.halfWidth = 0.5 * max(u_grass_bladeWidth * typeWidth * widthScale * widthSize, t.dist * u_grass_minWidthPerMetre * (1.0 + stalkCoarse));
    // The normal: NEAR the curve's own (the bent top catches the light), easing into one LINEAR in t from "LOD 2
    // distance" to "LOD 3 distance" - a LOD 3 blade is ONE triangle, and the curve's tip normal spread over it flipped
    // it bright or dark at the switch.
    g_blade.linearBlend = smoothstep(u_grass_lod2Distance, u_grass_lod3Distance, t.dist);
    // The normals' terms (GrassBlade). The upper half's chord is (0.75 tip.x, 0.25 tip.y, 0.75 tip.z): its normal
    // (tip.y, -3 turn), normalized (its length >= the rise > 0).
    g_blade.turn = dot(g_blade.tip.xz, vec2(sideXZ.y, -sideXZ.x));
    g_blade.tipN = normalize(vec2(g_blade.tip.y, -3.0 * g_blade.turn));

#ifndef GRASS_NEAR_SHADOW
    g_blade.tipPrevDelta = grassTipOffset(lean, grassWindPush(t.windPrev, u_grass_prevTime, phase) * windFade, height) - g_blade.tip;
    g_blade.groundN = t.groundN;
    // The albedo factor: the per-blade variation x the COLD darkening (full at "Cold temperature", none from "Warm
    // temperature"; the patch's mean temperature - climate is km-scale, so no patch steps show).
    const float variation = 1.0 + u_grass_colorVariation * (phaseVariation.y * 2.0 - 1.0);
    const float warm = smoothstep(u_grass_coldTemperature, u_grass_warmTemperature, p.temperature);
    const float groundBlend = clamp(t.dist / u_grass_range, 0.0, 1.0) * u_grass_groundBlend;
    g_blade.look = vec4(variation * mix(1.0 - u_grass_coldDarkening, 1.0, warm), t.dryness, groundBlend,
        grassCanopyExtinction(cover, widthSize, t.dist));
    // THE SEED STALKS' COLOUR ON THE BLADES, from LOD 2 to LOD 3 (the normal's blend: 0 at "LOD 2 distance", 1 at "LOD 3
    // distance"): a LOD 3 stalk is one triangle a pixel or two wide, tapering to nothing where its seed head is - it
    // does not show. So every blade of the type takes the stalks' colour (grass.inc.glsl grassStalkColor) by the share of
    // the view the stalks would cover (grassStalkCover, as the far tier has it), their depth x "Seed stalks/Far blade
    // colour".
    float seedCover = 0.0;
    if (g_blade.linearBlend > 0.0 && !g_blade.stalk)
    {
        const vec3 toEye = u_viewPos - g_blade.root;
        seedCover = g_blade.linearBlend * grassStalkCover(u_grass_typeSeedFar[t.type] * u_grass_seedFarBlade,
            clamp(dot(t.groundN, toEye) / length(toEye), 0.0, 1.0));
    }
    // The type, the seed stalk and that cover (unorm8) for the FS's colours.
    g_blade.lookBits = t.type | (g_blade.stalk ? 4u : 0u) | (uint(seedCover * 255.0 + 0.5) << 8);
    // The canopy at this blade: its mean height (the ground under it takes the same).
    g_blade.canopyHeight = grassCanopyHeight(coverSize);
#endif
    return true;
}

#ifndef GRASS_NEAR_SHADOW
// THE BLADE's FLAT OUTPUT, one uvec4 (grass.fs.glsl unpacks it): x = half2(the albedo factor, dryness), y = half2(the
// ground normal blend, the canopy extinction), z = the facet normal's x / z (snorm16; its y > 0 rebuilt:
// grassUnpackGroundNormal), w = the look bits (16) | the WIDTH AXIS << 16 - the normal's frame (grassVertex): its x
// (15 bits, -1 .. 1) and its z's sign (the top bit); the axis is unit, so the FS rebuilds |z| with one square root -
// no angle, so no trigonometry on either side.
uvec4 grassPackFlat()
{
    const uint sideBits = uint(clamp(g_blade.side.x * 0.5 + 0.5, 0.0, 1.0) * 32767.0 + 0.5) | (g_blade.side.y < 0.0 ? 0x8000u : 0u);
    return uvec4(packHalf2x16(g_blade.look.xy), packHalf2x16(g_blade.look.zw), packSnorm2x16(g_blade.groundN.xz),
        g_blade.lookBits | (sideBits << 16));
}
#endif

// One vertex of g_blade.
struct GrassVertex
{
    vec3 pos;
    float t;           // along the blade (0 root .. 1 tip)
#ifndef GRASS_NEAR_SHADOW
    vec3 prevDelta;    // to last frame's point
    float sideSign;    // across (-1 / +1 the edges, 0 the tip): the midrib, and the normal's edge tilt
    // THE NORMAL as ONE value: without its edge tilt it lies in the plane across the width axis (side), on the upright
    // stem's side of it (its dot with cross(up, side) is the tangent's rise, >= 0) - so its y and the flat width axis
    // rebuild it (grass.fs.glsl), and the tilt from the across value.
    float normalY;
    float canopyDepth; // m below the canopy top
#endif
};

// Vertex `vert` of a blade of S segments (`lod`): row r (2 per row: sides -1 / +1), the tip last.
GrassVertex grassVertex(uint vert, uint lod)
{
    const uint segments = grassLodSegments(lod);
    const uint row = min(vert >> 1u, segments);
    const float sideSign = vert >= 2u * segments ? 0.0 : ((vert & 1u) != 0u ? 1.0 : -1.0);
    GrassVertex v;
    v.t = float(row) / float(segments);

    // GEOMORPH (the LODs nest: 8 -> 4 -> 2 -> 1 segments): over the last "LOD morph band" before the next LOD's
    // distance, the odd rows (the coarser LOD drops them) move onto the line between their neighbours, normals
    // included. At the switch the blade is the coarser one exactly. The patch's LOD comes from its NEAREST point.
    // The neighbours' midpoint on the curve is the point + h^2 (tip.x, -tip.y, tip.z) exactly (the curve's second
    // derivative is constant): the weights below take it, no more curve points. The width morphs to its neighbours' mean on
    // its own (a blade's is linear in t - its own value; a seed stalk's is not).
    const float nextLodDist = lod == 0u ? u_grass_lod1Distance : lod == 1u ? u_grass_lod2Distance : u_grass_lod3Distance;
    const float morph = lod < 3u && (row & 1u) != 0u
        ? smoothstep(nextLodDist * (1.0 - u_grass_lodMorphBand), nextLodDist, g_blade.dist) : 0.0;
    const float h = 1.0 / float(segments);
    float width = grassBladeWidth(v.t);
    if (morph > 0.0)
        width = mix(width, 0.5 * (grassBladeWidth(v.t - h) + grassBladeWidth(v.t + h)), morph);
    // The curve's weights (GrassBlade): sideways t^2, upward 2t - t^2.
    const float sideways = v.t * v.t + morph * h * h;
    const vec3 weights = vec3(sideways, 2.0 * v.t - sideways, sideways);
    const vec2 across = g_blade.side * (sideSign * g_blade.halfWidth * width);
    v.pos = g_blade.root + weights * g_blade.tip + vec3(across.x, 0.0, across.y);

#ifndef GRASS_NEAR_SHADOW
    v.prevDelta = weights * g_blade.tipPrevDelta; // the root and the width are the same in both frames
    v.sideSign = sideSign;
    // The depth below the canopy top, capped at its height (the sunk root is below the ground).
    v.canopyDepth = clamp(g_blade.canopyHeight - (v.pos.y - g_blade.groundY), 0.0, g_blade.canopyHeight);
    // The normal without its edge tilt (the FS adds that back from the across value), in the normals' plane.
    vec2 normal = vec2(0.0);
    if (g_blade.linearBlend < 1.0)
    {
        normal = grassBladeCurveNormal(v.t);
        if (morph > 0.0)
            normal = mix(normal, 0.5 * (grassBladeCurveNormal(v.t - h) + grassBladeCurveNormal(v.t + h)), morph);
    }
    v.normalY = normalize(mix(normal, mix(vec2(1.0, 0.0), g_blade.tipN, v.t), g_blade.linearBlend)).y;
#endif
    return v;
}

#endif
