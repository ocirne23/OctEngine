#version 460

// The BAKED PRESSURE FIELD: one workgroup per CPU-selected brick (16x16 samples at 1 m, corner-
// aligned to the world lattice — see RendererVKLayout's ForceBakeBricksGpu comment), each thread
// accumulating EVERY team's field value at its sample point on the fixed gameplay height. The CPU
// reads the whole thing back once (~2 frames latent) and serves any number of gameplay consumers
// (shield-less swarm bodies' push/exposure) with plain bilinear taps — no per-consumer query slot.

layout (local_size_x = FORCE_BAKE_BRICK_SAMPLES, local_size_y = FORCE_BAKE_BRICK_SAMPLES) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

// Matches RendererVKLayout::ForceBakeBricksGpu.
layout (binding = 5, std430) readonly buffer Bricks
{
    uint bk_count; float bk_sampleY; uint bk_pad0; uint bk_pad1;
    ivec4 bk_bricks[];
};
layout (binding = 6, std430) writeonly buffer OutField { vec4 out_field[]; };

void main()
{
    const uint brick = gl_WorkGroupID.x;
    if (brick >= bk_count)
        return;
    const ivec2 bc = bk_bricks[brick].xy;
    const float spacing = 1.0; // = FORCE_BAKE_SAMPLE_SPACING (Layout.ixx)
    const vec3 pos = vec3(
        float(bc.x * int(FORCE_BAKE_BRICK_SAMPLES) + int(gl_LocalInvocationID.x)) * spacing,
        bk_sampleY,
        float(bc.y * int(FORCE_BAKE_BRICK_SAMPLES) + int(gl_LocalInvocationID.y)) * spacing);

    float phi[MAX_FORCE_TEAMS];
    forceAccumulate(pos, phi);

    const uint idx = (brick * FORCE_BAKE_BRICK_SAMPLES * FORCE_BAKE_BRICK_SAMPLES
        + gl_LocalInvocationID.y * FORCE_BAKE_BRICK_SAMPLES + gl_LocalInvocationID.x) * 2u;
    out_field[idx] = vec4(phi[0], phi[1], phi[2], phi[3]);
    out_field[idx + 1u] = vec4(phi[4], phi[5], phi[6], phi[7]);
}
