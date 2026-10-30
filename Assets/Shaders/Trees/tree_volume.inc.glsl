// The FAR-TREE VOLUME (TreeVolumePipeline): one camera-centred volume of leaf / wood EXTINCTION (1/m) that the far trees
// are marched through, from "Far start" to "Far end". Mirrors TreeVolumePipeline.cpp - keep them in step.
//
// POLAR + LOG: image x = the angle around the bake centre (atan2(dz, dx), -pi..pi -> 0..1; it WRAPS, so sample with a
// repeating sampler), y = log(r / rMin) / log(rMax / rMin) (the cells grow linearly with the distance: a constant angular
// size), z = the height above the column's FLOOR, [0, height] in `slices` slices. Nothing is stored inside rMin, a FIXED
// horizontal radius (a high camera moves the hand-over out instead).
// THE FLOOR (R32UI, tvFloorEncode; 0 = no floor) is the base of the column's dominant tree (tree_volume_splat.cs's
// TREE_FLOOR_PASS), NOT the terrain height map: its far texels put the ground tens of metres off on mountains and the trees
// fell out of the thin layer. A column without a floor holds no density.

#ifndef TREE_VOLUME_INC_GLSL
#define TREE_VOLUME_INC_GLSL

struct TreeVolumeParams
{
    vec2 centre;        // world XZ of the bake centre
    float rMin;         // m: the volume's inner radius (horizontal)
    float rMax;         // m: its outer radius ("Far end")
    uint angularRes;    // texels around
    uint radialRes;     // texels from rMin to rMax
    uint slices;        // height slices
    float height;       // m above the floor the volume covers
    float densityScale; // x the baked extinction
    float pad0, pad1, pad2;
};

// The params in a pass's PUSH BLOCK (push.generated.glsl, TreeVolumePipeline's TvVolPush): flat pc_vol_* members, all but
// the centre lockable. TV_PUSH_VOL packs them for the helpers below, so the baked members fold.
#define TV_PUSH_VOL TreeVolumeParams(pc_vol_centre, pc_vol_rMin, pc_vol_rMax, pc_vol_angularRes, pc_vol_radialRes, pc_vol_slices, pc_vol_height, \
    pc_vol_densityScale, 0.0, 0.0, 0.0)

const float TV_TWO_PI = 6.28318531;

// THE ACCUMULATION (the bake's R32UI 3D image) packs FOUR SLICES per texel, INTERLEAVED: with L = (slices + 3) / 4 layers,
// slice s in layer s % L, byte s / L - neighbour slices lie in neighbour layers of ONE channel, so the hand-over's fetch lets
// the hardware blend the slice axis (one fetch; only every L-th pair crosses into the next channel). Per slice a
// fixed-point extinction x TV_ACCUM_SCALE (255 = TV_ACCUM_MAX, the density's own max) summed by imageAtomicAdd:
// - STOCHASTIC ROUNDING (`dither` in [0, 1), a hash): a step is 1/64 per metre, so a rounded small add (a tent's edge, a
//   thin overlap) was lost - dithered, the sum stays right on average.
// - SATURATING, by a COMPARE-SWAP loop: each add raises its byte by min(amount, 255 - byte) - nothing ever carries into
//   the next byte (the slice L layers up), and a full byte costs no atomic. (A plain add + undoing its carry + OR to 255
//   was not exact under concurrency: the OR is no add, and a racing undo borrowed from the slice above - dots stacked
//   above the dense crowns.)
// After the resolve each byte holds its slice's extinction sRGB-ENCODED as the density's (tvDensityEncode's curve): the
// hand-over samples the image through an RGBA8_SRGB view (x TV_ACCUM_MAX), and tree_volume_copy.cs copies the bytes.
const float TV_ACCUM_MAX = 4.0; // 16 / m (a step 1/16 per metre) re-sampled every texel visibly at each rebake: ~0.5-1 optical depth per far cell
const float TV_ACCUM_SCALE = 255.0 / TV_ACCUM_MAX;
uint tvAccumLayers(uint slices) { return (slices + 3u) / 4u; }

// The rock sum (R32UI 2D) keeps its own, finer scale.
const float TV_ROCK_SCALE = 1024.0;

// [0, 1) from three integers (a lowbias32 hash): the accumulation's dither.
float tvDither(uvec3 v)
{
    uint h = v.x * 0x8DA6B343u ^ v.y * 0xD8163841u ^ v.z * 0xCB1AB31Fu;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return float(h) * (1.0 / 4294967296.0);
}

// The saturating add into accumulation image IMG (instantiate it once after the image's declaration):
// tvAccumAdd(col, slice, layers = tvAccumLayers(slices), extinction, dither).
#define TV_ACCUM_ADD_FUNCTION(IMG) \
void tvAccumAdd(ivec2 col, uint s, uint layers, float extinction, float dither) \
{ \
    const uint amount = min(uint(extinction * TV_ACCUM_SCALE + dither), 255u); \
    if (amount == 0u) \
        return; \
    const uint channel = s / layers; \
    const ivec3 texel = ivec3(col, int(s - channel * layers)); \
    const uint shift = channel * 8u; \
    uint v = imageLoad(IMG, texel).r; /* a guess: the swap checks it */ \
    for (int attempt = 0; attempt < 64; ++attempt) \
    { \
        const uint b = (v >> shift) & 0xFFu; \
        if (b == 255u) \
            return; \
        const uint prev = imageAtomicCompSwap(IMG, texel, v, v + (min(amount, 255u - b) << shift)); \
        if (prev == v) \
            return; \
        v = prev; \
    } \
}

// A byte's sum (accumulation units) -> its sRGB code (the resolve, in place).
uint tvAccumEncode(uint sum)
{
    const float c = clamp(float(sum) * (1.0 / 255.0), 0.0, 1.0); // = extinction / TV_ACCUM_MAX
    return uint((c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055) * 255.0 + 0.5);
}
float tvAccumDecode(float code) // an sRGB code (0..1) -> extinction (the R16F fallback copy)
{
    const float c = code <= 0.04045 ? code * (1.0 / 12.92) : pow((code + 0.055) * (1.0 / 1.055), 2.4);
    return c * TV_ACCUM_MAX;
}

// THE DENSITY (the march's 3D extinction, copied from the resolved accumulation by tree_volume_copy.cs). TV_DENSITY_R8 (the
// GPU filters R8_SRGB): R8, the resolve's sRGB code of extinction / TV_ACCUM_MAX - written through an R8_UNORM view,
// sampled through an R8_SRGB one, so the hardware decodes to linear BEFORE the trilinear filter (exact filtering, a free
// decode) and the 8 bits go where the relative steps are small (~4 % at the "Far blob shrink" cut). Else R16F, decoded.
#ifdef TV_DENSITY_R8
#define TV_DENSITY_DECODE TV_ACCUM_MAX // x the sampled (already linear) value
#else
#define TV_DENSITY_DECODE 1.0
#endif

// THE COLOUR MAP's FAR TYPE (the bake, from the records' stage to the resolve): the RGBA8 map's R32UI view holds the record
// type in its ALPHA byte, written with its rgb by ONE imageAtomicMin (type << 24 | rgb: the lowest type wins, with its
// colour). 255 = none (the clear's alpha 1, and every detail splat write). The resolve's column pass sets the alpha back.
const uint TV_TYPE_NONE = 255u;
uint tvColourType(uint bits) { return bits >> 24; }
// The bake's clear colour (TreeVolumePipeline: 0.1, 0.16, 0.07) as packUnorm4x8's rgb bytes: a rock's colour write.
const uint TV_CLEAR_RGB = 26u | (41u << 8) | (18u << 16);

// The world tree records' mass per column (tree_volume_records.cs -> tree_volume_far.cs), fixed point.
const float TV_AMOUNT_SCALE = 4096.0;

// The max-floor grid (tree_volume_floor_max.cs): columns per block side (TreeVolumePipeline's FLOOR_MAX_BLOCK), the value
// of a region without any floor, and the blocks each way of the angle its AHEAD value covers.
const int TV_FLOOR_MAX_BLOCK = 16;
const float TV_FLOOR_MAX_NONE = -1e30;
const int TV_FLOOR_AHEAD_SECTORS = 4;

float tvLogSpan(TreeVolumeParams v) { return log(v.rMax / v.rMin); }

// Radius r -> the image's normalized y (outside [0, 1]: outside the volume).
float tvRadialUv(float r, TreeVolumeParams v) { return log(max(r, 1e-3) / v.rMin) / tvLogSpan(v); }

// Radial texel coordinate (texels from rMin, continuous) -> radius.
float tvRadius(float texel, TreeVolumeParams v) { return v.rMin * exp(texel / float(v.radialRes) * tvLogSpan(v)); }

// An angular texel index wrapped into [0, res), for any x >= -2 res. NOT `((x % res) + res) % res`: GLSL leaves `%`
// UNDEFINED for a negative operand, and the indices just below 0 lie exactly on the seam.
int tvWrapAngle(int x, int res) { return int(uint(x + 2 * res) % uint(res)); }

// A cell's radial / tangential size (m) at radius r, and the larger of the two.
float tvRadialCell(float r, TreeVolumeParams v) { return r * tvLogSpan(v) / float(v.radialRes); }
float tvTangentialCell(float r, TreeVolumeParams v) { return r * TV_TWO_PI / float(v.angularRes); }
float tvCellSize(float r, TreeVolumeParams v) { return max(tvRadialCell(r, v), tvTangentialCell(r, v)); }

// The floor's encoding: atomicMax of the INVERTED order-preserving bits keeps the MINIMUM height; 0 (a NaN's pattern,
// never a height) = no floor.
uint tvFloorEncode(float y)
{
    const uint u = floatBitsToUint(y);
    return ~((u & 0x80000000u) != 0u ? ~u : u | 0x80000000u);
}

float tvFloorDecode(uint e)
{
    const uint o = ~e;
    return uintBitsToFloat((o & 0x80000000u) != 0u ? o & 0x7FFFFFFFu : ~o);
}

// Texel (angle, radius) centre -> world XZ.
vec2 tvTexelWorldXZ(ivec2 texel, TreeVolumeParams v)
{
    const float theta = ((float(texel.x) + 0.5) / float(v.angularRes) - 0.5) * TV_TWO_PI;
    return v.centre + vec2(cos(theta), sin(theta)) * tvRadius(float(texel.y) + 0.5, v);
}

#endif
