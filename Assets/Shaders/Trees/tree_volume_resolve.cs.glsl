#version 460

// FAR-TREE VOLUME BAKE, the resolve (TreeVolumePipeline): the splat's fixed-point sums -> the extinction, IN PLACE as four
// sRGB codes per texel (tree_volume.inc.glsl tvAccumEncode). The march's hand-over reads them through the RGBA8_SRGB view,
// then tree_volume_copy.cs moves them into the density. A range of texel layers per dispatch.
// THE COLUMN PASS (pc_columnPass, before any layer converts): the ROCK fraction = the column's rock sum / its sum over the
// slices (each in its own scale). A column with rocks mixes the CLIMATE'S BEDROCK into its colour by it - the terrain's rock materials' mean colours,
// weighted by their climate boxes (terrain_splat.inc.glsl's box weight, over every rock entry) - and stores 1 - the fraction
// in alpha (the march's rock lighting). Columns without rocks keep the splat's colour, their alpha back at 1.

#extension GL_EXT_scalar_block_layout : require

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 2, r32ui) uniform uimage3D u_accum; // the sums in, the extinction's sRGB codes out (in place)
layout (binding = 4, r32ui) uniform readonly uimage2D u_rockSum;
layout (binding = 5, rgba8) uniform image2D u_colour;
layout (binding = 6, r32ui) uniform readonly uimage2D u_floor;
const int ROCK_TEXTURES = 8; // TreeVolumePipeline::ROCK_TEXTURES
layout (binding = 7) uniform sampler2D u_rockTextures[ROCK_TEXTURES];

// The push block: TreeVolumePipeline's ResolvePush (pc_vol_*: the bake's lockable values; pc_numRock: the bound rock textures).
#include "push.generated.glsl"

// The climate's bedrock colour at a point (y: the ground, for the temperature's lapse).
vec3 bedrockAlbedo(vec2 xz, float y)
{
    const int numGround = int(u_terrain_numGround);
    const int numRock = int(pc_numRock); // <= ROCK_TEXTURES, 0 without a splat set (the CPU)
    if (numRock <= 0)
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
        const uint sums = imageLoad(u_accum, p).r;
        imageStore(u_accum, p, uvec4(tvAccumEncode(sums & 0xFFu) | tvAccumEncode((sums >> 8) & 0xFFu) << 8
            | tvAccumEncode((sums >> 16) & 0xFFu) << 16 | tvAccumEncode(sums >> 24) << 24));
        return;
    }

    // THE COLUMN PASS (one layer dispatched). A column without rocks takes its alpha back to 1: the records' stage left
    // its far type there (tree_volume.inc.glsl tvColourType).
    const uint rock = imageLoad(u_rockSum, p.xy).r;
    if (rock == 0u)
    {
        const vec4 colour = imageLoad(u_colour, p.xy);
        if (colour.a < 1.0)
            imageStore(u_colour, p.xy, vec4(colour.rgb, 1.0));
        return;
    }

    uint total = 0u;
    for (int z = 0; z < imageSize(u_accum).z; ++z)
    {
        const uint sums = imageLoad(u_accum, ivec3(p.xy, z)).r;
        total += (sums & 0xFFu) + ((sums >> 8) & 0xFFu) + ((sums >> 16) & 0xFFu) + (sums >> 24);
    }
    const float fraction = clamp((float(rock) * (1.0 / TV_ROCK_SCALE)) / max(float(total) * (1.0 / TV_ACCUM_SCALE), 1e-3), 0.0, 1.0);

    const vec2 xz = tvTexelWorldXZ(p.xy, TV_PUSH_VOL);
    const uint floorBits = imageLoad(u_floor, p.xy).r;
    const float ground = floorBits != 0u ? tvFloorDecode(floorBits) : terrainHeightAt(xz);
    const vec3 trees = imageLoad(u_colour, p.xy).rgb;
    imageStore(u_colour, p.xy, vec4(mix(trees, bedrockAlbedo(xz, ground), fraction), 1.0 - fraction));
}
