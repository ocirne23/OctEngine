#version 460

// FAR-TREE VOLUME BAKE, the last step (TreeVolumePipeline): the splat's fixed-point sums -> the extinction, IN PLACE as
// two half floats per texel in the accumulation (two slices per texel, tree_volume.inc.glsl; its RG16F view is the NEW
// volume of the march's hand-over cross-fade; then tree_volume_copy.cs moves it into the R16F density the march
// samples otherwise). A range of texel layers per dispatch.
// Per COLUMN (the z = 0 invocations), its ROCKS' share (R5; Procedural's world rocks): rock fraction = the column's rock
// sum / its sum over the slices (both in the accumulation's units). A column with rocks mixes the CLIMATE'S BEDROCK into
// its colour by that fraction - the terrain's rock materials' mean colours (each diffuse texture's smallest mip),
// weighted by their climate boxes at the column's climate (terrain_splat.inc.glsl's box weight, over every rock entry:
// a far colour needs no top-three pick) - and stores 1 - the fraction in alpha (the march's rock lighting). Columns
// without rocks keep the splat's colour.

#extension GL_EXT_scalar_block_layout : require

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 2, r32ui) uniform uimage3D u_accum; // the sums in, the extinction's half floats out (in place)
layout (binding = 4, r32ui) uniform readonly uimage2D u_rockSum;
layout (binding = 5, rgba8) uniform image2D u_colour;
layout (binding = 6, r32ui) uniform readonly uimage2D u_floor;
const int ROCK_TEXTURES = 8; // TreeVolumePipeline::ROCK_TEXTURES
layout (binding = 7) uniform sampler2D u_rockTextures[ROCK_TEXTURES];

// The push block: TreeVolumePipeline's ResolvePush (pc_vol_*: the bake's lockable values).
#include "push.generated.glsl"

// The climate's bedrock colour at a point (y: the ground, for the temperature's lapse).
vec3 bedrockAlbedo(vec2 xz, float y)
{
    const int numGround = int(u_terrain_numGround);
    const int numRock = min(int(u_terrain_numRock), ROCK_TEXTURES);
    if (u_terrain_splatBase < 0.0 || numRock <= 0)
        return vec3(0.3, 0.29, 0.27); // no texture set
    vec2 climate = vec2(0.5);
    if (terrainHeightMapPresent())
    {
        const vec4 c = terrainClimateAt(xz);
        climate = vec2(clamp((terrainTemperatureAt(c, y) + 25.0) / 75.0, 0.0, 1.0), c.w);
    }
    const float invS2 = 1.0 / (2.0 * u_terrainTex_climateSigma * u_terrainTex_climateSigma);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int i = 0; i < numRock; ++i)
    {
        const vec4 box = u_terrain_splatClimate[numGround + i];
        const vec2 d = max(max(box.xz - climate, climate - box.yw), vec2(0.0));
        const float w = exp(-dot(d, d) * invS2);
        sum += w * textureLod(u_rockTextures[i], vec2(0.5), 20.0).rgb; // the smallest mip: the mean
        weight += w;
    }
    return weight > 1e-6 ? sum / weight : textureLod(u_rockTextures[0], vec2(0.5), 20.0).rgb;
}

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z + pc_sliceOffset);
    if (any(greaterThanEqual(p, imageSize(u_accum))))
        return;
    if (pc_columnPass == 0u)
    {
        // IN PLACE: the two slices' fixed-point sums -> their extinctions as half floats in the same texel (the march's
        // hand-over reads them through the image's RG16F view, the copy as packed bits).
        const uint sums = imageLoad(u_accum, p).r;
        imageStore(u_accum, p, uvec4(packHalf2x16(vec2(float(sums & 0xFFFFu), float(sums >> 16)) / TV_ACCUM_SCALE)));
        return;
    }
    // THE COLUMN PASS (one layer dispatched, before any layer converts: it sums the column's fixed-point slices).
    const uint rock = imageLoad(u_rockSum, p.xy).r;
    if (rock == 0u)
        return;
    uint total = 0u;
    for (int z = 0; z < imageSize(u_accum).z; ++z)
    {
        const uint sums = imageLoad(u_accum, ivec3(p.xy, z)).r;
        total += (sums & 0xFFFFu) + (sums >> 16);
    }
    const float fraction = clamp(float(rock) / max(float(total), 1.0), 0.0, 1.0);
    const vec2 xz = tvTexelWorldXZ(p.xy, TV_PUSH_VOL);
    const uint floorBits = imageLoad(u_floor, p.xy).r;
    const float ground = floorBits != 0u ? tvFloorDecode(floorBits) : terrainHeightAt(xz);
    const vec3 trees = imageLoad(u_colour, p.xy).rgb;
    imageStore(u_colour, p.xy, vec4(mix(trees, bedrockAlbedo(xz, ground), fraction), 1.0 - fraction));
}
