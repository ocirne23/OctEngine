#version 460

// FAR-TREE MARCH (TreeVolumePipeline): full res every pixel, or ("Far half res") one ray per 2x2 block - its centre,
// to the block's FARTHEST surface (the upsample drops trees a nearer pixel has in front of it, as cloud_march) -
// and / or ("Far checkerboard") this frame's parity only ((x + y + frame) even; cloud_temporal.cs fills the
// rest). Both only under TREE_TEMPORAL_OUT (the temporal pass reconstructs). The view ray through the far-tree
// volume's ring (tree_volume.inc.glsl), from "Far start" to the scene surface or "Far end": steps of about one
// volume cell (x "Step scale"), so they grow with the distance like the cells do; above the volume's
// top the step is the height gap. Per step with extinction: the leaves' colour lit by the sun (its transmittance
// through the volume toward the sun - two taps - x the terrain's sun visibility, one per ray at the first hit)
// plus an ambient that darkens toward the ground. Out: rgb = in-scatter, a = transmittance (the apply composites
// colour + scene x T, like the clouds).

#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : require

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 2
#include "terrain_height.inc.glsl"
#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 1) uniform sampler2D u_sceneDepth;
layout (binding = 3) uniform sampler3D u_density;
layout (binding = 4) uniform sampler2D u_colour;
layout (binding = 5, rgba16f) uniform writeonly image2D u_out;
#ifdef TREE_TEMPORAL_OUT
// The variant under the temporal pass ("Far temporal blend" > 0): the distances in cloud_temporal.cs's format, log2 of
// x = the first tree, y = the transmittance-weighted mean distance, z = the march's limit. No tree: all three the
// limit.
layout (binding = 6, rgba16f) uniform writeonly image2D u_outDepth;
#else
layout (binding = 6, r16f) uniform writeonly image2D u_outDepth; // the weighted mean distance (m): the fog apply's layer depth
#endif
layout (binding = 7, r32ui) uniform readonly uimage2D u_floor;   // the columns' tree floors (tree_volume.inc.glsl)

layout (push_constant, scalar) uniform Push
{
    TreeVolumeParams vol;
    uvec2 size;          // the MARCH image's size (half the render size at "Far half res")
    float stepScale;     // x the cell size
    float startDistance; // m from the CAMERA: the volume fades in from here...
    uint maxSteps;
    float ambient;       // x the sun radiance, the sky light on the canopy
    float shrink;        // 1/m off the baked extinction: a blob shrinks toward its dense core ("Far blob shrink")
    float overlap;       // ...over this band (m), where the billboards still draw ("Far overlap")
    // The lighting ("Trees/Far ..."):
    float sunScale;       // the direct sun's factor (a leaf's mean cosine)
    float selfShadow;     // x the sun taps' optical depth
    float normalStrength; // 0..1: the sun term toward max(N.L, 0), N = -grad(density) - the volume's "normal map"
    float groundDark;     // how much darker the sky light gets at the ground
    float forwardScatter; // Henyey-Greenstein g of the sun term (> 0: backlit leaves glow)
    float albedoScale;    // x the leaf colour
    float interiorShadow; // darkening of a blob's core ("Far interior shadow"; 0 = off)
    float interiorRadius; // its taps' distance, x the cell size ("Far interior radius")
    uint pad0;
    uint pad1;
    uvec2 fullSize;       // the render size (the scene depth)
} pc;

// BAKED (TreeVolumePipeline compiles a variant per setting, so the dead paths and their registers go):
// TREE_MARCH_SCALE = render pixels per march pixel per axis (1, or 2 at "Far half res"; TREE_TEMPORAL_OUT only);
// TREE_MARCH_SKIP = 0 none, 1 the CHECKERBOARD (1 of 2: the dispatch covers half the columns, this frame's parity),
// 2 = 1 of 4 (the dispatch covers one pixel per 2x2 block; plain variant only).
#ifndef TREE_MARCH_SCALE
#define TREE_MARCH_SCALE 1
#endif
#ifndef TREE_MARCH_SKIP
#define TREE_MARCH_SKIP 0
#endif
#define MARCH_SCALE TREE_MARCH_SCALE
#define MARCH_CHECKER (TREE_MARCH_SKIP == 1)
#define MARCH_QUAD (TREE_MARCH_SKIP == 2)

// The plain variant's PIXEL SKIP source: every pixel's latest march (persistent, not per slot - a skipped pixel was
// last marched up to 3 frames ago). Cleared to "no trees, distance 0" at a restart, which fails every copy test.
layout (binding = 8, rgba16f) uniform image2D u_latest;
layout (binding = 9, r16f) uniform image2D u_latestDepth;

// A point's polar image coordinates (tvUvXZ) from its offset to the bake centre and that offset's length: callers that
// also need the length (the cell size) compute it once. Every density read takes ONE of these - the atan + log were
// computed twice per read before (once for the density, once more for the floor), 25 times per lit step.
vec2 polarUv(vec2 rel, float r)
{
    return vec2(atan(rel.y, rel.x) / TV_TWO_PI + 0.5, tvRadialUv(r, pc.vol));
}

// The volume's ground at the point of `uv`: the nearest column's tree floor (the splat measured that column's slices
// from it). False where the column has no tree (the caller falls back to the height map - only where it needs to).
bool floorAtUv(vec2 uv, out float floorY)
{
    floorY = 0.0;
    if (uv.y < 0.0 || uv.y >= 1.0)
        return false;
    const int angularRes = int(pc.vol.angularRes);
    const ivec2 texel = ivec2(tvWrapAngle(int(floor(uv.x * float(angularRes))), angularRes),
        int(uv.y * float(pc.vol.radialRes)));
    const uint bits = imageLoad(u_floor, texel).r;
    floorY = tvFloorDecode(bits);
    return bits != 0u;
}

// The PRIMARY sample: the 4 columns around p bilinearly, EACH at its own height above its own floor - what the splat
// stored. The hardware filter across columns mixes a neighbour's density in at THIS column's height, so on a steep floor
// step (peaks, slopes) a neighbour's crown leaked up by the floor difference: thin spikes over the trees. Per column
// the texture is read at the column's centre, so only the vertical axis filters in hardware.
// `smoothFloor`: the same 4 columns' floors blended bilinearly (the columns with a floor; `fallback` without any) - the
// CONTINUOUS ground the lighting taps measure their heights from (densityTap / densityAbove).
float densityAtColumns(vec3 p, vec2 uv, float fallback, out float smoothFloor)
{
    smoothFloor = fallback;
    if (uv.y < 0.0 || uv.y > 1.0)
        return 0.0;
    const int angularRes = int(pc.vol.angularRes), radialRes = int(pc.vol.radialRes);
    const vec2 tc = uv * vec2(angularRes, radialRes) - 0.5;
    const ivec2 base = ivec2(floor(tc));
    const vec2 f = tc - vec2(base);
    float sum = 0.0, floorSum = 0.0, floorWeight = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        const ivec2 col = ivec2(tvWrapAngle(base.x + o.x, angularRes), clamp(base.y + o.y, 0, radialRes - 1));
        const uint bits = imageLoad(u_floor, col).r;
        if (bits == 0u)
            continue; // no tree reaches the column: no density there
        const vec2 w2 = mix(1.0 - f, f, vec2(o));
        const float w = w2.x * w2.y;
        const float colFloor = tvFloorDecode(bits);
        floorSum += w * colFloor;
        floorWeight += w;
        const float h = p.y - colFloor;
        if (h < 0.0 || h > pc.vol.height)
            continue;
        const vec3 at = vec3((vec2(col) + 0.5) / vec2(angularRes, radialRes), h / pc.vol.height);
        sum += w * textureLod(u_density, at, 0.0).r;
    }
    if (floorWeight > 1e-6)
        smoothFloor = floorSum / floorWeight;
    return max(sum - pc.shrink, 0.0) * pc.vol.densityScale;
}

// The LIGHTING TAPS (sun, normal, interior - 12 per lit step) lie within ~10 m of their sample, which lies >= ~400 m
// from the bake centre: their polar uv to FIRST ORDER around the sample's (polarJ: d uv / d xz), under ~0.1 texel off
// even for the farthest tap - instead of an atan + log + length per tap. A purely VERTICAL tap keeps the sample's
// column outright: its uv, only another height (densityAbove).
// THE TAPS' HEIGHT is measured from the sample's SMOOTH floor (densityAtColumns), not from each tap's nearest column's
// floor: that one jumps at every column boundary where two dominant trees stand at different bases, and the shading
// stepped with it - a bright / dark vertical line along every angular column edge (finer columns: thinner lines, as
// many). No floor load per tap either; a column without trees holds no density, so its tap reads ~0 anyway.
mat2 polarJ(vec2 rel, float r)
{
    const float ir2 = 1.0 / (r * r);
    const float a = ir2 / TV_TWO_PI, b = ir2 / tvLogSpan(pc.vol);
    return mat2(vec2(-rel.y * a, rel.x * b), vec2(rel.x * a, rel.y * b)); // columns: d/dx, d/dz
}
float densityAbove(vec2 uv, float h)
{
    if (uv.y < 0.0 || uv.y > 1.0 || h < 0.0 || h > pc.vol.height)
        return 0.0;
    // u (the angle) repeats. The shrink cuts the filtered fall-off at a blob's edge first.
    return max(textureLod(u_density, vec3(uv, h / pc.vol.height), 0.0).r - pc.shrink, 0.0) * pc.vol.densityScale;
}
float densityTap(vec2 uv, mat2 J, float h, vec3 d)
{
    return densityAbove(uv + J * d.xz, h + d.y);
}

// The plain variant's distance image: the trees' weighted mean where the result holds trees (transmittance below
// PLAIN_HAS_TREES), else the march limit - the scene distance, the sky as PLAIN_LIMIT_MAX (R16F's range).
const float PLAIN_HAS_TREES = 0.999;
const float PLAIN_LIMIT_MAX = 65000.0;

// The marched pixel's stored result (the plain checkerboard's neighbour fallback, main).
vec4 g_colour = vec4(0.0, 0.0, 0.0, 1.0);
float g_distance = PLAIN_LIMIT_MAX;

void storeOut(ivec2 px, vec4 colour)
{
    imageStore(u_out, px, colour);
    g_colour = colour;
}

#ifndef TREE_TEMPORAL_OUT
void storeDistance(ivec2 px, float distance)
{
    imageStore(u_outDepth, px, vec4(distance));
    g_distance = distance;
}
#endif

// The march of one (march-image) pixel.
void marchAt(ivec2 px)
{
    const int scale = MARCH_SCALE;
    const ivec2 full = px * scale;
    const vec2 uv = (vec2(full) + 0.5 * float(scale)) / vec2(pc.fullSize); // the block's centre (full-target UV)
    const vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    if (any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
    {
        storeOut(px, vec4(0.0, 0.0, 0.0, 1.0));
#ifdef TREE_TEMPORAL_OUT
        imageStore(u_outDepth, px, vec4(log2(max(pc.vol.rMax, 1.0))));
#else
        storeDistance(px, PLAIN_LIMIT_MAX);
#endif
        return;
    }

    // The block's FARTHEST surface (reversed-Z: the smallest depth, 0 = sky); one texel at full res.
    float depth = texelFetch(u_sceneDepth, full, 0).r;
    if (scale > 1)
    {
        const ivec2 last = ivec2(pc.fullSize) - 1;
        depth = min(min(depth, texelFetch(u_sceneDepth, min(full + ivec2(1, 0), last), 0).r),
                    min(texelFetch(u_sceneDepth, min(full + ivec2(0, 1), last), 0).r, texelFetch(u_sceneDepth, min(full + ivec2(1, 1), last), 0).r));
    }
    // THE RAY as vol_apply.fs.glsl's fogRay: from u_mvp's x / y / w rows, NOT a reconstructed position - u_invMvp is
    // a float32 CPU inverse whose error re-rolls every frame, and kilometres out that wobble moved the blobs - and
    // through this frame's TAA-JITTERED sub-pixel position (-u_taaJitter in NDC), as every raster pass samples: an
    // unjittered ray had TAA un-jitter content that never was jittered, a pixel's wobble per frame.
    const vec2 rayNdc = vec2(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0) - u_taaJitter.xy;
    const mat3 rayFromNdc = inverse(mat3(
        vec3(u_mvp[0][0], u_mvp[1][0], u_mvp[2][0]),
        vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]),
        vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3])));
    const vec3 dir = normalize(vec3(rayNdc, 1.0) * rayFromNdc);
    // Reversed Z: 0 = sky. Camera-relative (shared.inc.glsl), at the jittered position the depth was rasterized at.
    const float sceneT = depth > 0.0 ? length(viewRelFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth)) : 1e30;
    // The volume is the ring rMin..rMax around the BAKE centre (rMin lies a rebake distance + 30 m inside the
    // horizontal distance of "Far start" at the camera's height, so the ring covers the hand-over wherever the
    // camera moved since the bake).
    // The march starts at "Far start" from the CAMERA and the result fades in over "Far overlap" (at the end) - the
    // billboards draw up to its end (tree_cull.inc.glsl): an overlap, not a seam. It leaves at rMax.
    const vec2 rel = u_viewPos.xz - pc.vol.centre;
    const float a = dot(dir.xz, dir.xz);
    const float b = dot(rel, dir.xz);
    const float c = dot(rel, rel);
    // The far root of |rel + dir.xz t| = R (the camera inside the circle).
    #define TV_RING_T(R) ((-b + sqrt(max(b * b - a * (c - (R) * (R)), 0.0))) / a)
    if (a < 1e-8)
    {
        storeOut(px, vec4(0.0, 0.0, 0.0, 1.0)); // straight up / down: never reaches the ring
#ifdef TREE_TEMPORAL_OUT
        imageStore(u_outDepth, px, vec4(log2(max(min(sceneT, pc.vol.rMax), 1.0))));
#else
        storeDistance(px, min(sceneT, PLAIN_LIMIT_MAX));
#endif
        return;
    }
    const float tEnd = min(sceneT, TV_RING_T(pc.vol.rMax));
#ifdef TREE_TEMPORAL_OUT
    // The stored march LIMIT is the scene distance capped at the volume's end - the rule cloud_temporal.cs applies to a
    // checkerboard's unmarched pixels (the ring exit, a function of the ray's slope, would never match it).
    const float logLimit = log2(max(min(sceneT, pc.vol.rMax), 1.0));
#endif
    // From the air the inner radius shrinks (TreeVolumePipeline innerRadius: the hand-over is a 3D distance) and the
    // camera may sit OUTSIDE the small inner circle: then it is already in the ring (t from 0).
    const float tRing = c <= pc.vol.rMin * pc.vol.rMin ? TV_RING_T(pc.vol.rMin) : 0.0;
    float t = max(tRing, pc.startDistance);
    if (t >= tEnd)
    {
        storeOut(px, vec4(0.0, 0.0, 0.0, 1.0));
#ifdef TREE_TEMPORAL_OUT
        imageStore(u_outDepth, px, vec4(logLimit));
#else
        storeDistance(px, min(sceneT, PLAIN_LIMIT_MAX));
#endif
        return;
    }

    const vec3 L = u_sunDirection.xyz;
    const vec3 sunRadiance = u_sunTransmittance * u_sunColor.rgb;
    // PER RAY, not per lit step: the sun term's direction-only factors (Henyey-Greenstein relative to isotropic, "Far
    // forward scatter" - 1 at g = 0 - times "Far sun scale") and the sky light (a COOL blue tint x the sun's luminance,
    // x "Far ambient"; its height factor stays per step).
    const float g = pc.forwardScatter;
    const float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * dot(dir, L), 1e-4), 1.5);
    const vec3 sunPart = sunRadiance * (pc.sunScale * phase);
    const vec3 skyBase = vec3(0.45, 0.6, 1.0) * dot(sunRadiance, vec3(0.2126, 0.7152, 0.0722)) * pc.ambient;
    const float sliceH = pc.vol.height / float(pc.vol.slices);
    const float jitter = fract(52.9829189 * fract(dot(vec2(px) + 5.588238 * float(u_frameIndex & 7u), vec2(0.06711056, 0.00583715))));
    // TWO SEGMENTS, split at the fade-in's end: the BAND (from "Far start" over "Far overlap", where the billboards still
    // draw) and BEHIND it. Each accumulates its own in-scatter, transmittance and weighted distance; only the band
    // fades in (as a whole, by its own mean distance - below). One fade for the whole ray let a ray that grazed a
    // band tree's blob beside its billboard pull its mean distance into the band and fade the far trees behind out
    // with it: a sky-coloured outline along the billboard silhouettes.
    const float fadeEnd = pc.startDistance + max(pc.overlap, 1.0);
    vec3 inBand = vec3(0.0), inBehind = vec3(0.0);
    float tBand = 1.0, tBehind = 1.0;
    float distSumBand = 0.0, distWeightBand = 0.0, distSumBehind = 0.0, distWeightBehind = 0.0;
#ifdef TREE_TEMPORAL_OUT
    float tFront = tEnd;                   // the first tree
#endif
    float terrainVis = -1.0; // the terrain's sun visibility, once per ray at the first hit
    // The WHOLE ray shifts by a per-pixel, per-frame fraction of its first step (TAA averages the sampling). The
    // steps are measured from the camera, so they slide through the volume as it moves; jittering only the first
    // sample left every later one on that sliding grid - blobs sampled differently each frame, "shaking".
    t += jitter * max(tvCellSize(length(u_viewPos.xz + dir.xz * t - pc.vol.centre), pc.vol) * pc.stepScale, 0.25);
    for (uint i = 0u; i < pc.maxSteps && t < tEnd; ++i)
    {
        const vec3 p = u_viewPos + dir * t;
        // ONE polar lookup per step: the cell size, the floor and the primary sample all take it.
        const vec2 rel = p.xz - pc.vol.centre;
        const float r = length(rel);
        const vec2 uv = polarUv(rel, r);
        const float cell = max(r * tvLogSpan(pc.vol) / float(pc.vol.radialRes), r * TV_TWO_PI / float(pc.vol.angularRes)); // tvCellSize
        float floorY;
        const bool hasFloor = floorAtUv(uv, floorY);
        // A column WITHOUT a tree floor: the splat floors every tree's footprint PLUS one ring, so such a column lies
        // 2+ columns from any density and all 4 of densityAtColumns' taps read 0 - no sample (its 4 floor loads, below),
        // and at least a whole cell of step (the ring column still lies between it and the density). In the step
        // formula, not a branch of its own: that branch cost the march 8 registers (56 -> 64).
        float dt = max(cell * (hasFloor ? pc.stepScale : max(pc.stepScale, 1.0)), 0.25);
        if (!hasFloor)
            floorY = terrainHeightAt(p.xz); // no tree reaches the column: the height map
        const float h = p.y - floorY;
        // The tree floor and the height map disagree by up to tens of metres (the map's far texels): the break below
        // the ground takes the LOWER of the two, the skip above the layer the HIGHER - and a few cells at most, since
        // a column's floor can sit above the map's slope. The height map is read only for those two: inside the layer
        // neither applies (the lower of the two lies at or under the floor; the skip needs h above the layer), and the
        // break needs h below -0.5 x the layer.
        if (h > pc.vol.height)
        {
            // Above the volume: no tree before the ground comes within its height again.
            const float hHigh = p.y - (hasFloor ? max(floorY, terrainHeightAt(p.xz)) : floorY);
            dt = max(dt, min((hHigh - pc.vol.height) * 0.7, 4.0 * cell));
        }
        else if (h < 0.0)
        {
            if (h < -0.5 * pc.vol.height && p.y - (hasFloor ? min(floorY, terrainHeightAt(p.xz)) : floorY) < -0.5 * pc.vol.height)
                break; // under the ground (the scene depth ends the ray there)
        }
        else
        {
            const float tt = t;
            float smoothFloor = floorY;
            const float sigma = hasFloor ? densityAtColumns(p, uv, floorY, smoothFloor) : 0.0;
            if (sigma > 1e-4)
            {
                if (terrainVis < 0.0)
                    terrainVis = terrainSunVisibility(p, L, 20.0, 8, 0.02, 1.0);
                const mat2 J = polarJ(rel, r);
                const float hs = p.y - smoothFloor; // the taps' height (densityTap)
                // The sun through the crown toward it: three taps out to 14 m (segments 2 / 4 / 8 m), x "Far self shadow".
                const float sunT = exp(-(densityTap(uv, J, hs, L * 1.0) * 2.0 + densityTap(uv, J, hs, L * 4.0) * 4.0
                    + densityTap(uv, J, hs, L * 10.0) * 8.0) * pc.selfShadow);
                const vec3 albedo = textureLod(u_colour, uv, 0.0).rgb * pc.albedoScale;
                const float hNorm = clamp(h / pc.vol.height, 0.0, 1.0);
                // The volume's NORMAL: the density falls off outward, so -grad(density) points out of the blob. Forward
                // differences over half a cell (horizontal) / one slice (up); only when "Far normal strength" asks.
                float sunCos = 1.0;
                if (pc.normalStrength > 0.0)
                {
                    const float hx = 0.5 * cell;
                    // The base of the differences on the same floor as the taps (sigma's own is the per-column blend).
                    const float here = densityAbove(uv, hs);
                    const float up = densityAbove(uv, hs + sliceH);
                    const vec3 grad = vec3(densityTap(uv, J, hs, vec3(hx, 0.0, 0.0)) - here, (up - here) * hx / sliceH,
                        densityTap(uv, J, hs, vec3(0.0, 0.0, hx)) - here);
                    const float len2 = dot(grad, grad);
                    if (len2 > 1e-12)
                        sunCos = mix(1.0, max(dot(-grad * inversesqrt(len2), L), 0.0), pc.normalStrength);
                }
                // Leaves as Lambert surfaces of a mean cosine toward the sun (sunPart: the phase and "Far sun scale"); the
                // sky light darker toward the ground ("Far ground darkening").
                const vec3 sky = skyBase * mix(1.0 - pc.groundDark, 1.0, hNorm);
                // INTERIOR (the volume's "Foliage interior shadow"): the mean extinction of 6 taps around the sample
                // (+-x / +-z / +-y at "Far interior radius" cells) - deep in a blob dense on every side, at its surface
                // empty on one - as an AO term on the sun and the sky alike.
                float interior = 1.0;
                if (pc.interiorShadow > 0.0)
                {
                    const float rr = pc.interiorRadius * cell;
                    const float ry = min(rr, 0.5 * pc.vol.height);
                    const float m = (densityTap(uv, J, hs, vec3(rr, 0.0, 0.0)) + densityTap(uv, J, hs, vec3(-rr, 0.0, 0.0))
                        + densityTap(uv, J, hs, vec3(0.0, 0.0, rr)) + densityTap(uv, J, hs, vec3(0.0, 0.0, -rr))
                        + densityAbove(uv, hs + ry) + densityAbove(uv, hs - ry)) / 6.0;
                    interior = exp(-pc.interiorShadow * m * rr);
                }
                const vec3 lit = (albedo * INV_PI * (sunPart * (sunT * terrainVis * sunCos) + sky) + albedo * u_ambientColor) * interior;
                const float alpha = 1.0 - exp(-sigma * dt);
#ifdef TREE_TEMPORAL_OUT
                tFront = min(tFront, tt);
#endif
                if (tt < fadeEnd)
                {
                    inBand += tBand * alpha * lit;
                    distSumBand += tBand * alpha * tt;
                    distWeightBand += tBand * alpha;
                    tBand *= 1.0 - alpha;
                    if (tBand < 0.01)
                    {
                        // The band is opaque: behind it matters only while the band is not yet faded in fully.
                        tBand = 0.0;
                        if (smoothstep(pc.startDistance, fadeEnd, distSumBand / distWeightBand) > 0.99)
                            break;
                        t = fadeEnd + jitter * dt; // keep the ray's per-frame shift
                        continue;
                    }
                }
                else
                {
                    inBehind += tBehind * alpha * lit;
                    distSumBehind += tBehind * alpha * tt;
                    distWeightBehind += tBehind * alpha;
                    tBehind *= 1.0 - alpha;
                    if (tBehind < 0.01)
                    {
                        tBehind = 0.0;
                        break;
                    }
                }
            }
        }
        t += dt;
    }
    // The FADE-IN over "Far overlap": the BAND's result as a whole, by its weighted mean distance - not each sample's
    // density, which thinned a blob's front and let the ray reach its dark core (a half-faded tree read too dark).
    // Then the band over what lies behind it, unfaded.
    const float meanBand = distWeightBand > 0.0 ? distSumBand / distWeightBand : fadeEnd;
    const float meanBehind = distWeightBehind > 0.0 ? distSumBehind / distWeightBehind : tEnd;
    const float fade = smoothstep(pc.startDistance, fadeEnd, meanBand);
    const float coverBand = fade * (1.0 - tBand);
    const float T = (1.0 - coverBand) * tBehind;
    const vec3 inScatter = fade * inBand + (1.0 - coverBand) * inBehind;
    // The layer distance (fog, temporal): the two segments' means, weighted by what each shows.
    const float weightBehind = (1.0 - coverBand) * (1.0 - tBehind);
    const float meanT = coverBand + weightBehind > 0.0 ? (coverBand * meanBand + weightBehind * meanBehind) / (coverBand + weightBehind) : tEnd;
    storeOut(px, vec4(inScatter, T));
#ifdef TREE_TEMPORAL_OUT
    // x = the first tree (the limit without one: the upsample's "behind this pixel's surface" test).
    imageStore(u_outDepth, px, vec4(tFront < tEnd ? log2(max(tFront, 1.0)) : logLimit,
        weightBehind + coverBand > 0.0 ? log2(max(meanT, 1.0)) : logLimit, logLimit, 0.0));
#else
    // Without trees the distance is the march LIMIT (the composites read it only where trees are): the checkerboard's
    // copy test (main) needs to know how far a tree-less pixel's march reached.
    storeDistance(px, T < PLAIN_HAS_TREES ? meanT : min(sceneT, PLAIN_LIMIT_MAX));
#endif
}

#ifndef TREE_TEMPORAL_OUT
// Whether a stored march result fits a pixel whose surface lies at `surface`: with trees, the surface must lie
// behind them (their mean distance); without, not farther than that march reached (its stored limit). Wide
// tolerances: a silhouette that matters puts a near surface against trees kilometres out; the TAA jitter alone moves a
// grazing terrain's distance by several %. A cleared pixel (no trees, distance 0) never fits.
bool copyFits(vec4 colour, float distance, float surface)
{
    return colour.a < PLAIN_HAS_TREES ? surface >= distance * 0.7 : min(surface, PLAIN_LIMIT_MAX) <= distance * 1.5;
}

// The scene surface's distance at a full-res pixel (the plain path: scale 1), as marchAt measures it.
float sceneDistanceAt(ivec2 px)
{
    const float depth = texelFetch(u_sceneDepth, px, 0).r;
    const vec2 uv = (vec2(px) + 0.5) / vec2(pc.fullSize);
    return depth > 0.0 ? length(viewRelFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth)) : 1e30;
}
#endif

void main()
{
    const uvec2 gid = gl_GlobalInvocationID.xy;
#ifdef TREE_TEMPORAL_OUT
    // The checkerboard under the temporal pass (which reconstructs the rest): each thread takes this frame's parity
    // pixel of its row pair, so the warps stay full (an early-out per pixel would idle half of every warp) -
    // cloud_march.cs.glsl's marchPixel.
    const ivec2 px = MARCH_CHECKER ? ivec2(gid.x * 2u + ((gid.y + u_frameIndex) & 1u), gid.y) : ivec2(gid);
    if (any(greaterThanEqual(uvec2(px), pc.size)))
        return;
    marchAt(px);
#else
    // PIXEL SKIP (no temporal pass): each thread owns a BLOCK - a horizontal pair (1 of 2, the checkerboard: the
    // marched pixel alternates per frame and row) or a 2x2 block (1 of 4: the marched pixel cycles through the block
    // over 4 frames) - marches ONE pixel of it, and fills the others from u_latest: each pixel's latest march, one to
    // three frames old (no reprojection: late under camera motion). A copy must still fit THIS frame's surface - the
    // TAA jitter moves every silhouette by up to a pixel per frame, and a copy marched against the other side of an
    // edge gave a bright outline: trees over a near leaf, or none on the background beside it. With trees, this
    // frame's surface must lie behind them (their mean distance); without, not farther than that march reached (the
    // stored limit). A failed test needs NO march (one march per thread; a second one stalled the whole warp): trees
    // in front of a nearer surface -> "no trees" (exact: the surface hides them); a surface farther than the march
    // reached (the background moved in) -> this frame's marched pixel of the block, which usually shows the same
    // background. The tolerances are WIDE (x0.7 / x1.5): a silhouette that matters puts a near surface (tens of metres)
    // against trees kilometres out, while the jitter alone moves a grazing terrain's distance by more than a few %.
    // The block and its SCHEDULE: the pixel due this frame first, then the rest in the order they come due. Without
    // pixel skipping the block is the thread's one pixel.
    const bool skip = MARCH_QUAD || MARCH_CHECKER;
    const ivec2 base = MARCH_QUAD ? ivec2(gid) * 2 : MARCH_CHECKER ? ivec2(gid.x * 2u, gid.y) : ivec2(gid);
    const int count = MARCH_QUAD ? 4 : MARCH_CHECKER ? 2 : 1;
    if (any(greaterThanEqual(uvec2(base), pc.size)))
        return;
    // A pixel whose surface lies nearer than "Far start" needs NO march: its result is exactly "no trees" (the march
    // would stop before the volume begins). So the march goes to the first FAR pixel of the schedule: at a near
    // silhouette - the alpha-tested fringe of a crown, leaf and gap alternating per pixel and per frame - the block's
    // background side is marched THIS frame and donates to its other far pixels; marching the scheduled pixel blindly
    // left the gaps with only leaf-side copies to choose from: a bare-sky speckle along every near crown. A block
    // without a far pixel marches nothing.
    // REGISTERS, not local memory: the block loops unroll (their bound is baked) so the arrays are indexed by
    // constants, the marched pixel is its own variable, and the schedule / neighbour offsets are arithmetic - arrays
    // indexed at run time (pixels[pick], a const table by the frame index) lived in local memory: 48 bytes of spill.
    ivec2 pixels[4];
    float surfaces[4];
    int pick = skip ? -1 : 0; // without skipping: always march the one pixel (no surface test)
    ivec2 marchPx = base;
    if (skip)
    {
        [[unroll]] for (int i = 0; i < count; ++i)
        {
            // The schedule: 1 of 4 cycles (0,0) (1,1) (1,0) (0,1) over the frames; 1 of 2 alternates per frame and row.
            const uint j = (u_frameIndex + uint(i)) & 3u;
            pixels[i] = base + (MARCH_QUAD ? ivec2(int(((j + 1u) >> 1) & 1u), int(j & 1u)) : ivec2(int((gid.y + u_frameIndex + uint(i)) & 1u), 0));
            const bool inside = all(lessThan(uvec2(pixels[i]), pc.size));
            surfaces[i] = inside ? sceneDistanceAt(pixels[i]) : -1.0; // -1: outside the image
            if (pick < 0 && surfaces[i] > pc.startDistance)
            {
                pick = i;
                marchPx = pixels[i];
            }
        }
    }
    // ONE call site of the march (two inlined copies raise every warp's register count).
    if (pick >= 0)
        marchAt(marchPx);
    if (!skip)
        return;
    if (pick >= 0)
    {
        imageStore(u_latest, marchPx, g_colour);
        imageStore(u_latestDepth, marchPx, vec4(g_distance));
    }
    [[unroll]] for (int k = 0; k < count; ++k)
    {
        const ivec2 q = pixels[k];
        const float surface = surfaces[k];
        if (k == pick || surface < 0.0)
            continue;
        if (surface <= pc.startDistance)
        {
            // Nearer than the volume: exactly "no trees" - also this pixel's latest march.
            imageStore(u_out, q, vec4(0.0, 0.0, 0.0, 1.0));
            imageStore(u_outDepth, q, vec4(surface));
            imageStore(u_latest, q, vec4(0.0, 0.0, 0.0, 1.0));
            imageStore(u_latestDepth, q, vec4(surface));
            continue;
        }
        vec4 colour = imageLoad(u_latest, q);
        float distance = imageLoad(u_latestDepth, q).r;
        if (!copyFits(colour, distance, surface))
        {
            // Trees in front of a nearer surface: the surface hides them - exact, no donor needed.
            const bool hidden = colour.a < PLAIN_HAS_TREES;
            // Else (the background moved in) MORE DONORS before giving up: the block's marched pixel (this frame),
            // then the 4 direct neighbours' latest marches - next to an edge one of them usually lies on the
            // background side. Taking the marched pixel blindly gave a bare-sky halo along every near silhouette
            // whenever it sat on the near side. (Neighbours being written by other threads read either their old or
            // this frame's march - both real results, both tested.)
            bool found = false;
            if (!hidden && copyFits(g_colour, g_distance, surface))
            {
                colour = g_colour;
                distance = g_distance;
                found = true;
            }
            [[unroll]] for (int n = 0; n < 4; ++n)
            {
                if (hidden || found)
                    break;
                // Left, right, up, down.
                const ivec2 d = clamp(q + (n < 2 ? ivec2(n * 2 - 1, 0) : ivec2(0, n * 2 - 5)), ivec2(0), ivec2(pc.size) - 1);
                const vec4 c = imageLoad(u_latest, d);
                const float dist = imageLoad(u_latestDepth, d).r;
                if (copyFits(c, dist, surface))
                {
                    colour = c;
                    distance = dist;
                    found = true;
                }
            }
            if (!found)
            {
                colour = hidden ? vec4(0.0, 0.0, 0.0, 1.0) : g_colour;
                distance = hidden ? min(surface, PLAIN_LIMIT_MAX) : g_distance;
            }
        }
        imageStore(u_out, q, colour);
        imageStore(u_outDepth, q, vec4(distance));
    }
#endif
}
