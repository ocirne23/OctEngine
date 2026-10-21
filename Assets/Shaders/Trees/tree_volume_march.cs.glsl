#version 460

// FAR-TREE MARCH (TreeVolumePipeline): the view ray through the far-tree volume's ring (tree_volume.inc.glsl), from "Far
// start" to the scene surface or "Far end", in steps of about one volume cell (x "Far step scale"), so they grow with the
// distance like the cells. Per step with extinction: the colour lit by the sun (its transmittance through the crown x the
// terrain's sun visibility, once per ray x the cloud shadow) plus the sky light, darker toward the ground.
// Out: rgb = in-scatter, a = transmittance (the apply composites colour + scene x T, like the clouds).
// Which pixels march: every one; or ("Far half res") one ray per 2x2 block to its FARTHEST surface; and / or this frame's
// checkerboard parity under TREE_TEMPORAL_OUT (the temporal pass reconstructs the rest); or, without it, 1 of 2 / 1 of 4
// per block ("Far pixel skip", main: the others copied from their latest march).
// ROCKS: the colour map's alpha is 1 - the column's rock fraction (tree_volume_resolve.cs). By it the sun term loses its
// phase (a surface does not glow backlit), takes the full N.L, and the interior darkening drops to a quarter.

#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : require

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 2
#include "terrain_height.inc.glsl"
#include "tree_volume.inc.glsl"
#define CLOUD_SHADOW_BINDING 11
#include "cloud_shadow.inc.glsl"

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 1) uniform sampler2D u_sceneDepth;
layout (binding = 3) uniform sampler3D u_density;
layout (binding = 4) uniform sampler2D u_colour;
layout (binding = 5, rgba16f) uniform writeonly image2D u_out;
#ifdef TREE_TEMPORAL_OUT
// Under the temporal pass: the distances in cloud_temporal.cs's format, log2 of x = the first tree, y = the
// transmittance-weighted mean distance, z = the march's limit. No tree: all three the limit.
layout (binding = 6, rgba16f) uniform writeonly image2D u_outDepth;
#else
layout (binding = 6, r16f) uniform writeonly image2D u_outDepth; // the weighted mean distance (m): the fog apply's layer depth
#endif
layout (binding = 7, r32ui) uniform readonly uimage2D u_floor;   // the columns' tree floors (tree_volume.inc.glsl)
layout (binding = 10) uniform sampler2DArray u_skyMap;           // GI's sky bake (atmosphere.inc.glsl): the canopy's sky light
layout (binding = 12, rg32f) uniform readonly image2D u_floorMax; // the max-floor grid: x dilated, y ahead (tree_volume_floor_max.cs.glsl)

// THE HAND-OVER (TREE_HANDOVER, only while it lasts): a finished bake CROSS-FADES into the shown one over "Far swap time".
// Per RAY a dither picks one bake (the new one where the pixel's noise lies under u_foliage_handoverFade); TAA / the
// temporal pass averages the picks into an image-space blend, so the opacity mixes correctly (a per-sample density mix
// would not). The whole ray reads its pick (g_new), so the variant costs what the normal one does.
#ifdef TREE_HANDOVER
layout (binding = 13) uniform sampler3D u_densityNew; // RG16F: two slices per texel (densityTexelNew)
layout (binding = 14, r32ui) uniform readonly uimage2D u_floorNew;
layout (binding = 15) uniform sampler2D u_colourNew;
layout (binding = 16, rg32f) uniform readonly image2D u_floorMaxNew;
#endif

// The push block: TreeVolumePipeline's MarchPush (pc_vol_*: the SHOWN bake's lockable values). The shading tweaks
// ("Trees/Far ...") ride the frame UBO: u_foliage_far*.
#include "push.generated.glsl"

// The bake a read goes to (TREE_HANDOVER: g_new, per ray; else the front one).
#ifdef TREE_HANDOVER
bool g_new = false; // this ray shows the new bake

bool handoverPicksNew(ivec2 px)
{
    // IGN offset from the step jitter's (decorrelated), stepped by the golden ratio per frame: a short history already
    // averages to the fade.
    const float ign = fract(52.9829189 * fract(dot(vec2(px) + vec2(17.0, 59.0), vec2(0.06711056, 0.00583715))));
    return fract(ign + float(u_frameIndex & 1023u) * 0.6180339887) < u_foliage_handoverFade;
}

vec2 bakeCentre() { return g_new ? u_foliage_handoverCentre : pc_vol_centre; }
uint floorBitsAt(ivec2 c) { return g_new ? imageLoad(u_floorNew, c).r : imageLoad(u_floor, c).r; }

// The new bake's view holds TWO SLICES per texel (RG16F: x = the even slice): the hardware filters across the columns only
// (each fetch at a texel layer's centre), the slice axis is blended here.
float densityTexelNew(vec3 at)
{
    const int slices = int(pc_vol_slices);
    const float zs = at.z * float(slices) - 0.5;
    const float z0 = floor(zs);
    const int s0 = clamp(int(z0), 0, slices - 1), s1 = clamp(int(z0) + 1, 0, slices - 1);
    const float invLayers = 1.0 / float((slices + 1) / 2);
    const vec2 a = textureLod(u_densityNew, vec3(at.xy, (float(s0 >> 1) + 0.5) * invLayers), 0.0).rg;
    const vec2 b = textureLod(u_densityNew, vec3(at.xy, (float(s1 >> 1) + 0.5) * invLayers), 0.0).rg;
    return mix((s0 & 1) != 0 ? a.y : a.x, (s1 & 1) != 0 ? b.y : b.x, zs - z0);
}

float densityTexel(vec3 at) { return g_new ? densityTexelNew(at) : textureLod(u_density, at, 0.0).r; }
vec4 colourAt(vec2 uv) { return g_new ? textureLod(u_colourNew, uv, 0.0) : textureLod(u_colour, uv, 0.0); }
vec2 floorMaxAt(ivec2 block) { return g_new ? imageLoad(u_floorMaxNew, block).xy : imageLoad(u_floorMax, block).xy; }
#else
vec2 bakeCentre() { return pc_vol_centre; }
uint floorBitsAt(ivec2 c) { return imageLoad(u_floor, c).r; }
float densityTexel(vec3 at) { return textureLod(u_density, at, 0.0).r; }
vec4 colourAt(vec2 uv) { return textureLod(u_colour, uv, 0.0); }
vec2 floorMaxAt(ivec2 block) { return imageLoad(u_floorMax, block).xy; }
#endif

// BAKED (a variant per setting, so the dead paths and their registers go):
// TREE_MARCH_SCALE = render pixels per march pixel per axis (1, or 2 at "Far half res"; TREE_TEMPORAL_OUT only);
// TREE_MARCH_SKIP = 0 none, 1 = 1 of 2 (the CHECKERBOARD), 2 = 1 of 4 (one pixel per 2x2 block; plain variant only).
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

// A point's polar image coordinates from its offset to the bake centre and that offset's length (the caller needs the
// length for the cell size too). ONE per step: the atan + log are the expensive part.
vec2 polarUv(vec2 rel, float r)
{
    return vec2(atan(rel.y, rel.x) * (1.0 / TV_TWO_PI) + 0.5, tvRadialUv(r, TV_PUSH_VOL));
}

// The volume's ground at `uv`: the nearest column's tree floor. False where the column has no floor.
bool floorAtUv(vec2 uv, out float floorY)
{
    floorY = 0.0;
    if (uv.y < 0.0 || uv.y >= 1.0)
        return false;
    const int angularRes = int(pc_vol_angularRes);
    const ivec2 texel = ivec2(tvWrapAngle(int(floor(uv.x * float(angularRes))), angularRes),
        int(uv.y * float(pc_vol_radialRes)));
    const uint bits = floorBitsAt(texel);
    floorY = tvFloorDecode(bits);
    return bits != 0u;
}

// The PRIMARY sample: the 4 columns around p bilinearly, EACH at its own height above its own floor, as the splat stored
// it (the hardware filter across columns put a neighbour's crown in at THIS column's height: spikes over every steep floor
// step). Each column is read at its centre, so only the vertical axis filters in hardware.
// `smoothFloor`: the same columns' floors blended bilinearly (`fallback` without any) - the CONTINUOUS ground the lighting
// taps measure their heights from.
float densityAtColumns(vec3 p, vec2 uv, float fallback, out float smoothFloor)
{
    smoothFloor = fallback;
    if (uv.y < 0.0 || uv.y > 1.0)
        return 0.0;

    const int angularRes = int(pc_vol_angularRes), radialRes = int(pc_vol_radialRes);
    const vec2 res = vec2(angularRes, radialRes);
    const vec2 invRes = 1.0 / res;
    const float invHeight = 1.0 / pc_vol_height;
    const vec2 tc = uv * res - 0.5;
    const ivec2 base = ivec2(floor(tc));
    const vec2 f = tc - vec2(base);
    float sum = 0.0, floorSum = 0.0, floorWeight = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        const ivec2 col = ivec2(tvWrapAngle(base.x + o.x, angularRes), clamp(base.y + o.y, 0, radialRes - 1));
        const uint bits = floorBitsAt(col);
        if (bits == 0u)
            continue; // no tree reaches the column: no density there
        const vec2 w2 = mix(1.0 - f, f, vec2(o));
        const float w = w2.x * w2.y;
        const float colFloor = tvFloorDecode(bits);
        floorSum += w * colFloor;
        floorWeight += w;
        const float h = p.y - colFloor;
        if (h < 0.0 || h > pc_vol_height)
            continue;
        const vec3 at = vec3((vec2(col) + 0.5) * invRes, h * invHeight);
        sum += w * densityTexel(at);
    }

    if (floorWeight > 1e-6)
        smoothFloor = floorSum / floorWeight;
    return max(sum - u_foliage_farShrink, 0.0) * pc_vol_densityScale;
}

// The LIGHTING TAPS (sun, normal, interior - up to 12 per lit step) lie within ~10 m of their sample, which lies
// >= ~400 m from the bake centre: their polar uv is taken to FIRST ORDER around the sample's (polarJ: d uv / d xz,
// < ~0.1 texel off) instead of an atan + log per tap. A purely VERTICAL tap keeps the sample's uv (densityAbove).
// THE TAPS' HEIGHT is measured from the sample's SMOOTH floor, not each tap's nearest column's floor: that one jumps at
// every column edge between two different floors, and the shading showed it as vertical lines. No floor load per tap.
mat2 polarJ(vec2 rel, float r)
{
    const float ir2 = 1.0 / (r * r);
    const float a = ir2 * (1.0 / TV_TWO_PI), b = ir2 / tvLogSpan(TV_PUSH_VOL);
    return mat2(vec2(-rel.y * a, rel.x * b), vec2(rel.x * a, rel.y * b)); // columns: d/dx, d/dz
}

float densityAbove(vec2 uv, float h)
{
    if (uv.y < 0.0 || uv.y > 1.0 || h < 0.0 || h > pc_vol_height)
        return 0.0;
    // u (the angle) repeats. "Far blob shrink" cuts the filtered fall-off at a blob's edge first.
    return max(densityTexel(vec3(uv, h * (1.0 / pc_vol_height))) - u_foliage_farShrink, 0.0) * pc_vol_densityScale;
}

float densityTap(vec2 uv, mat2 J, float h, vec3 d)
{
    return densityAbove(uv + J * d.xz, h + d.y);
}

// The plain variant's distance image: the trees' weighted mean where the result holds trees (transmittance below
// PLAIN_HAS_TREES), else the march limit - the scene distance, the sky as PLAIN_LIMIT_MAX (R16F's range).
const float PLAIN_HAS_TREES = 0.999;
const float PLAIN_LIMIT_MAX = 65000.0;

// The marched pixel's stored result (the pixel skip's donor, main).
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
#ifdef TREE_HANDOVER
    g_new = handoverPicksNew(px);
#endif
    const int scale = MARCH_SCALE;
    const ivec2 full = px * scale;
    const vec2 uv = (vec2(full) + 0.5 * float(scale)) / vec2(pc_fullSize); // the block's centre (full-target UV)
    const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
    if (any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
    {
        storeOut(px, vec4(0.0, 0.0, 0.0, 1.0));
#ifdef TREE_TEMPORAL_OUT
        imageStore(u_outDepth, px, vec4(log2(pc_vol_rMax)));
#else
        storeDistance(px, PLAIN_LIMIT_MAX);
#endif
        return;
    }

    // The block's FARTHEST surface (reversed-Z: the smallest depth, 0 = sky); one texel at full res.
    float depth = texelFetch(u_sceneDepth, full, 0).r;
    if (scale > 1)
    {
        const ivec2 last = ivec2(pc_fullSize) - 1;
        depth = min(min(depth, texelFetch(u_sceneDepth, min(full + ivec2(1, 0), last), 0).r),
                    min(texelFetch(u_sceneDepth, min(full + ivec2(0, 1), last), 0).r, texelFetch(u_sceneDepth, min(full + ivec2(1, 1), last), 0).r));
    }

    // THE RAY as vol_apply.fs.glsl's fogRay: from u_rayFromNdc (u_invMvp's float32 error re-rolls every frame and moved
    // the blobs kilometres out), through this frame's TAA-JITTERED sub-pixel position, as every raster pass samples.
    const vec2 rayNdc = vec2(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0) - u_taaJitter.xy;
    const vec3 dir = normalize(vec3(rayNdc, 1.0) * u_rayFromNdc);
    // Reversed Z: 0 = sky. At the jittered position the depth was rasterized at.
    const float sceneT = depth > 0.0 ? length(viewRelFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth)) : 1e30;

    // The volume is the ring rMin..rMax around the BAKE centre; rMin lies far enough inside "Far start" to cover the
    // hand-over wherever the camera moved since the bake. The march starts at "Far start" from the CAMERA and fades in
    // over "Far overlap", where the billboards still draw (tree_cull.inc.glsl).
    const vec2 rel = u_viewPos.xz - bakeCentre();
    const float a = dot(dir.xz, dir.xz);
    const float b = dot(rel, dir.xz);
    const float c = dot(rel, rel);
    // The far root of |rel + dir.xz t| = R (the camera inside the circle: c <= R^2, so the discriminant is >= b^2).
    #define TV_RING_T(R) ((-b + sqrt(b * b - a * (c - (R) * (R)))) / a)
    if (a < 1e-8)
    {
        storeOut(px, vec4(0.0, 0.0, 0.0, 1.0)); // straight up / down: never reaches the ring
#ifdef TREE_TEMPORAL_OUT
        imageStore(u_outDepth, px, vec4(log2(max(min(sceneT, pc_vol_rMax), 1.0))));
#else
        storeDistance(px, min(sceneT, PLAIN_LIMIT_MAX));
#endif
        return;
    }
    const float tEnd = min(sceneT, TV_RING_T(pc_vol_rMax));
#ifdef TREE_TEMPORAL_OUT
    // The stored march LIMIT is the scene distance capped at the volume's end: the rule cloud_temporal.cs applies to an
    // unmarched pixel (the ring exit depends on the ray's slope and would never match it).
    const float logLimit = log2(max(min(sceneT, pc_vol_rMax), 1.0));
#endif
    // A camera OUTSIDE the inner circle is already in the ring (t from 0).
    const float tRing = c <= pc_vol_rMin * pc_vol_rMin ? TV_RING_T(pc_vol_rMin) : 0.0;
    float t = max(tRing, pc_startDistance);
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
    const vec3 sunRadiance = u_sunTransmittance * u_sunColor.rgb * u_sunVisible; // u_sunVisible: the eclipse
    // PER RAY: the sun term's direction-only factors (Henyey-Greenstein relative to isotropic - 1 at g = 0 - x "Far sun
    // scale"; a rock takes no phase) and the SKY LIGHT: pi x the cosine-weighted mean radiance of GI's sky map over the
    // upper hemisphere (5 taps, baked once per frame: gi_sky_map.cs.glsl, SKY_MAP_TREE_SKY_TEXEL) x "Far ambient".
    const float g = u_foliage_farForwardScatter;
    const float hgBase = 1.0 + g * g - 2.0 * g * dot(dir, L); // |g| <= 0.95 on the CPU: >= 0.0025
    const float phase = (1.0 - g * g) / (hgBase * sqrt(hgBase)); // ^1.5
    const float sunLeaf = u_foliage_farSunScale * phase;
    const vec3 skyMean = texelFetch(u_skyMap, SKY_MAP_TREE_SKY_TEXEL, 0).rgb;
    const vec3 skyBase = skyMean * (PI * u_foliage_farAmbient);
    const float sliceH = pc_vol_height / float(pc_vol_slices);
    const float jitter = fract(52.9829189 * fract(dot(vec2(px) + 5.588238 * float(u_frameIndex & 7u), vec2(0.06711056, 0.00583715))));

    // TWO SEGMENTS, split at the fade-in's end: the BAND (over "Far overlap", where the billboards still draw) and BEHIND
    // it, each with its own in-scatter, transmittance and weighted distance. Only the band fades in, as a whole (one fade
    // for the whole ray let a band tree's blob fade out the far trees behind it: a sky-coloured outline along the
    // billboards).
    const float fadeEnd = pc_startDistance + u_foliage_farOverlap;
    // A segment's distance weights sum to 1 - its transmittance (sum of T x alpha telescopes): no weight sums are kept.
    vec3 inBand = vec3(0.0), inBehind = vec3(0.0);
    float tBand = 1.0, tBehind = 1.0;
    float distSumBand = 0.0, distSumBehind = 0.0;
#ifdef TREE_TEMPORAL_OUT
    float tFront = tEnd; // the first tree
#endif
    float terrainVis = -1.0; // the terrain's sun visibility, once per ray at the first hit

    // tvCellSize(r) = r x cellPerR (both cell sides grow linearly with r).
    const float cellPerR = max(tvLogSpan(TV_PUSH_VOL) / float(pc_vol_radialRes), TV_TWO_PI / float(pc_vol_angularRes));
    // The WHOLE ray shifts by a per-pixel, per-frame fraction of its first step (TAA averages the sampling): the steps are
    // measured from the camera, and jittering only the first sample left the rest on a grid sliding with it.
    t += jitter * max(length(u_viewPos.xz + dir.xz * t - bakeCentre()) * cellPerR * u_foliage_farStepScale, 0.25);

    // THE SKIP over the MAX-FLOOR GRID (tree_volume_floor_max.cs): above its value + the volume's height, a point is above
    // every tree within one block in every direction - at least r x skipReach away (the block's tangential width as a chord,
    // or the inner neighbour block's radial depth, whichever is less) - so the ray steps that far (a descending one at most
    // down to that height).
    const float skipReach = min(sin(float(TV_FLOOR_MAX_BLOCK) * TV_TWO_PI / float(pc_vol_angularRes)),
        1.0 - exp(-float(TV_FLOOR_MAX_BLOCK) * tvLogSpan(TV_PUSH_VOL) / float(pc_vol_radialRes)));
    // THE END for a RISING ray: above the grid's AHEAD value (+ the height) - the highest floor of this block's row and
    // every row farther out, TV_FLOOR_AHEAD_SECTORS blocks each way - no tree lies ahead. Valid while the ray moves
    // OUTWARD (past its closest approach to the bake centre, so r only grows) and its angle cannot leave those sectors:
    // from radius r on, a straight ray's polar angle turns by at most asin(d / r), d = its distance from the centre
    // (at most the camera's) - so while d < r x aheadSin.
    const float aheadSin = sin(float(TV_FLOOR_AHEAD_SECTORS * TV_FLOOR_MAX_BLOCK) * TV_TWO_PI / float(pc_vol_angularRes));
    const float camCentre = length(rel);

    for (uint i = 0u; i < uint(u_foliage_farMaxSteps) && t < tEnd; ++i)
    {
        const vec3 p = u_viewPos + dir * t;
        // ONE polar lookup per step: the cell size, the floor and the primary sample all take it.
        const vec2 rel = p.xz - bakeCentre();
        const float r = length(rel);
        const vec2 uv = polarUv(rel, r);
        const float cell = r * cellPerR; // tvCellSize

        const ivec2 block = ivec2(tvWrapAngle(int(floor(uv.x * float(pc_vol_angularRes))), int(pc_vol_angularRes)),
            clamp(int(uv.y * float(pc_vol_radialRes)), 0, int(pc_vol_radialRes) - 1)) / TV_FLOOR_MAX_BLOCK;
        const vec2 floorMax = floorMaxAt(block);
        if (dir.y >= 0.0 && p.y > floorMax.y + pc_vol_height && camCentre < r * aheadSin && dot(rel, dir.xz) >= 0.0)
            break;

        const float reach = r * skipReach;
        const float clearance = p.y - floorMax.x - pc_vol_height;
        if (clearance > 0.0)
        {
            const float skip = dir.y >= 0.0 ? reach : min(reach, clearance / -dir.y);
            if (skip > cell)
            {
                t += skip;
                continue;
            }
        }

        float floorY;
        const bool hasFloor = floorAtUv(uv, floorY);
        // A column WITHOUT a tree floor lies 2+ columns from any density (the splat floors every footprint PLUS one
        // ring): no sample, and at least a whole cell of step. In the step formula, not a branch of its own: that branch
        // cost the march 8 registers.
        float dt = max(cell * (hasFloor ? u_foliage_farStepScale : max(u_foliage_farStepScale, 1.0)), 0.25);
        if (!hasFloor)
            floorY = terrainHeightAt(p.xz); // no tree reaches the column: the height map
        const float h = p.y - floorY;

        // The tree floor and the height map disagree by up to tens of metres (the map's far texels): the break below the
        // ground takes the LOWER of the two, the skip above the layer the HIGHER (capped at 4 cells). Inside the layer
        // neither applies, so the height map is read only above it or deeper than half a layer under it.
        if (h > pc_vol_height)
        {
            // Above the volume: no tree before the ground comes within its height again.
            const float hHigh = p.y - (hasFloor ? max(floorY, terrainHeightAt(p.xz)) : floorY);
            dt = max(dt, min((hHigh - pc_vol_height) * 0.7, 4.0 * cell));
        }
        else if (h < 0.0)
        {
            if (h < -0.5 * pc_vol_height && p.y - (hasFloor ? min(floorY, terrainHeightAt(p.xz)) : floorY) < -0.5 * pc_vol_height)
                break; // under the ground (the scene depth ends the ray there)
        }
        else
        {
            const float tt = t;
            float smoothFloor = floorY;
            const float sigma = hasFloor ? densityAtColumns(p, uv, floorY, smoothFloor) : 0.0;
            if (sigma > 1e-4)
            {
                // The terrain shadow marches the height MAP, not the tree floor: start at the sample's height above its floor,
                // put on the MAP's ground there. From `p` itself a crown in a valley started under the map's surface (its
                // coarse texels interpolate above a concave floor) and lost all sun: black patches along valleys.
                if (terrainVis < 0.0)
                    terrainVis = terrainSunVisibility(vec3(p.x, terrainHeightAt(p.xz) + max(h, 0.0), p.z), L, 20.0, 8, 0.02, 1.0);
                // The cloud shadow per lit step (a ray crosses kilometres of canopy).
                const float cloudT = cloudSunTransmittanceBilinear(p);
                const mat2 J = polarJ(rel, r);
                const float hs = p.y - smoothFloor; // the taps' height
                // The sun through the crown: three taps out to 14 m (segments 2 / 4 / 8 m), x "Far self shadow".
                const float sunT = exp(-(densityTap(uv, J, hs, L * 1.0) * 2.0 + densityTap(uv, J, hs, L * 4.0) * 4.0
                    + densityTap(uv, J, hs, L * 10.0) * 8.0) * u_foliage_farSelfShadow);

                const vec4 baked = colourAt(uv);
                const float rock = 1.0 - baked.a; // the column's ROCK fraction (the resolve)
                // "Far saturation scale": toward the colour's linear luminance (Rec. 709).
                const vec3 albedo = max(mix(vec3(dot(baked.rgb, vec3(0.2126, 0.7152, 0.0722))), baked.rgb, u_foliage_farSaturation), 0.0) * u_foliage_farAlbedoScale;
                const float hNorm = h * (1.0 / pc_vol_height);

                // The volume's NORMAL = -grad(density), by forward differences over half a cell (horizontal) / one slice
                // (up), from the taps' floor, x "Far normal strength"; a rock takes the full N.L.
                float sunCos = 1.0;
                {
                    const float hx = 0.5 * cell;
                    const float here = densityAbove(uv, hs);
                    const float up = densityAbove(uv, hs + sliceH);
                    const vec3 grad = vec3(densityTap(uv, J, hs, vec3(hx, 0.0, 0.0)) - here, (up - here) * hx / sliceH,
                        densityTap(uv, J, hs, vec3(0.0, 0.0, hx)) - here);
                    const float len2 = dot(grad, grad);
                    if (len2 > 1e-12)
                        sunCos = mix(1.0, max(dot(-grad * inversesqrt(len2), L), 0.0), mix(u_foliage_farNormalStrength, 1.0, rock));
                }

                // The sky light, darker toward the ground ("Far ground darkening").
                const vec3 sky = skyBase * mix(1.0 - u_foliage_farGroundDark, 1.0, hNorm);

                // INTERIOR: the mean extinction of 6 taps around the sample (+-x / +-z / +-y at "Far interior radius" cells)
                // as an AO term on the sun and the sky alike. Radius 0 puts the taps on the sample: skipped.
                float interior = 1.0;
                if (u_foliage_farInteriorShadow > 0.0 && u_foliage_farInteriorRadius > 0.0)
                {
                    const float rr = u_foliage_farInteriorRadius * cell;
                    const float ry = min(rr, 0.5 * pc_vol_height);
                    const float m = (densityTap(uv, J, hs, vec3(rr, 0.0, 0.0)) + densityTap(uv, J, hs, vec3(-rr, 0.0, 0.0))
                        + densityTap(uv, J, hs, vec3(0.0, 0.0, rr)) + densityTap(uv, J, hs, vec3(0.0, 0.0, -rr))
                        + densityAbove(uv, hs + ry) + densityAbove(uv, hs - ry)) * (1.0 / 6.0);
                    // A rock's core is solid, with no lit interior to darken: a quarter of the term.
                    interior = exp(-u_foliage_farInteriorShadow * m * rr * (1.0 - 0.75 * rock));
                }

                const vec3 sunPart = sunRadiance * mix(sunLeaf, u_foliage_farSunScale, rock);
                const vec3 lit = albedo * (INV_PI * (sunPart * (sunT * terrainVis * cloudT * sunCos) + sky) + u_ambientColor) * interior;
                const float alpha = 1.0 - exp(-sigma * dt);
#ifdef TREE_TEMPORAL_OUT
                tFront = min(tFront, tt);
#endif

                if (tt < fadeEnd)
                {
                    inBand += tBand * alpha * lit;
                    distSumBand += tBand * alpha * tt;
                    tBand *= 1.0 - alpha;
                    if (tBand < 0.01)
                    {
                        // The band is opaque: what lies behind it matters only while the band is not fully faded in.
                        // Its distance sum is normalised before its transmittance becomes 0.
                        distSumBand /= 1.0 - tBand;
                        tBand = 0.0;
                        if (smoothstep(pc_startDistance, fadeEnd, distSumBand) > 0.99)
                            break;
                        t = fadeEnd + jitter * dt; // keep the ray's per-frame shift
                        continue;
                    }
                }
                else
                {
                    inBehind += tBehind * alpha * lit;
                    distSumBehind += tBehind * alpha * tt;
                    tBehind *= 1.0 - alpha;
                    if (tBehind < 0.01)
                    {
                        distSumBehind /= 1.0 - tBehind;
                        tBehind = 0.0;
                        break;
                    }
                }
            }
        }
        t += dt;
    }

    // The FADE-IN over "Far overlap": the BAND's result as a whole, by its weighted mean distance (fading each sample's
    // density let the ray reach a blob's dark core), over what lies behind it, unfaded.
    const float meanBand = tBand < 1.0 ? distSumBand / (1.0 - tBand) : fadeEnd;
    const float meanBehind = tBehind < 1.0 ? distSumBehind / (1.0 - tBehind) : tEnd;
    const float fade = smoothstep(pc_startDistance, fadeEnd, meanBand);
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
    // Without trees the distance is the march LIMIT (the composites read it only where trees are): the pixel skip's
    // copy test (main) needs to know how far a tree-less pixel's march reached.
    storeDistance(px, T < PLAIN_HAS_TREES ? meanT : min(sceneT, PLAIN_LIMIT_MAX));
#endif
}

#ifndef TREE_TEMPORAL_OUT
// Whether a stored march result fits a pixel whose surface lies at `surface`: with trees, the surface must lie behind
// them (their mean distance); without, not farther than that march reached (its stored limit). The tolerances are WIDE
// (x0.7 / x1.5): a silhouette that matters puts a near surface against trees kilometres out, while the TAA jitter alone
// moves a grazing terrain's distance by several %. A cleared pixel (no trees, distance 0) never fits.
bool copyFits(vec4 colour, float distance, float surface)
{
    return colour.a < PLAIN_HAS_TREES ? surface >= distance * 0.7 : min(surface, PLAIN_LIMIT_MAX) <= distance * 1.5;
}

// The scene surface's distance at a full-res pixel (the plain path: scale 1), as marchAt measures it.
float sceneDistanceAt(ivec2 px)
{
    const float depth = texelFetch(u_sceneDepth, px, 0).r;
    const vec2 uv = (vec2(px) + 0.5) / vec2(pc_fullSize);
    return depth > 0.0 ? length(viewRelFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth)) : 1e30;
}
#endif

void main()
{
    const uvec2 gid = gl_GlobalInvocationID.xy;
#ifdef TREE_TEMPORAL_OUT
    // The checkerboard under the temporal pass: each thread takes this frame's parity pixel of its pair, so the warps
    // stay full (as cloud_march.cs.glsl's marchPixel).
    const ivec2 px = MARCH_CHECKER ? ivec2(gid.x * 2u + ((gid.y + u_frameIndex) & 1u), gid.y) : ivec2(gid);
    if (any(greaterThanEqual(uvec2(px), pc_size)))
        return;
    marchAt(px);
#else
    // PIXEL SKIP (no temporal pass): each thread owns a BLOCK - a horizontal pair (1 of 2) or a 2x2 block (1 of 4) - marches
    // ONE pixel of it and fills the others from u_latest, each pixel's latest march (1 to 3 frames old, no reprojection). A
    // copy must still fit THIS frame's surface (copyFits): the TAA jitter moves every silhouette by up to a pixel per frame.
    // A failed test never marches again (a second march stalled the whole warp).
    // The block's SCHEDULE: the pixel due this frame first, then the rest in the order they come due. Without pixel skipping
    // the block is the thread's one pixel.
    const bool skip = MARCH_QUAD || MARCH_CHECKER;
    const ivec2 base = MARCH_QUAD ? ivec2(gid) * 2 : MARCH_CHECKER ? ivec2(gid.x * 2u, gid.y) : ivec2(gid);
    const int count = MARCH_QUAD ? 4 : MARCH_CHECKER ? 2 : 1;
    if (any(greaterThanEqual(uvec2(base), pc_size)))
        return;

    // A pixel whose surface lies nearer than "Far start" needs NO march (its result is exactly "no trees"), so the march goes
    // to the first FAR pixel of the schedule: at a near crown's alpha-tested fringe the background side is then marched this
    // frame and donates to the block's other far pixels. A block without a far pixel marches nothing.
    // REGISTERS, not local memory: the block loops unroll (their bound is baked) so the arrays are indexed by constants, the
    // marched pixel is its own variable, and the schedule / neighbour offsets are arithmetic (arrays indexed at run time
    // spilled to local memory).
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
            const bool inside = all(lessThan(uvec2(pixels[i]), pc_size));
            surfaces[i] = inside ? sceneDistanceAt(pixels[i]) : -1.0; // -1: outside the image
            if (pick < 0 && surfaces[i] > pc_startDistance)
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
        if (surface <= pc_startDistance)
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
            // Else (the background moved in) MORE DONORS through the same test: the block's marched pixel, then the 4
            // direct neighbours' latest marches (next to an edge one of them usually lies on the background side).
            // A neighbour another thread writes reads its old or this frame's march: both real results.
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
                const ivec2 d = clamp(q + (n < 2 ? ivec2(n * 2 - 1, 0) : ivec2(0, n * 2 - 5)), ivec2(0), ivec2(pc_size) - 1);
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
