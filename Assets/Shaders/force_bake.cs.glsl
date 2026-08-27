#version 460

// The BAKED PRESSURE FIELD: one workgroup per CPU-selected chunk (16x16 samples at 1 m, corner-
// aligned to the world lattice — see RendererVKLayout's ForceBakeChunksGpu comment), each thread
// accumulating EVERY team's field value at its sample point on the fixed gameplay height. The CPU
// reads the whole thing back once (~2 frames latent) and serves any number of gameplay consumers
// (shield-less swarm bodies' push/exposure) with plain bilinear taps — no per-consumer query slot.

layout (local_size_x = FORCE_BAKE_CHUNK_SAMPLES, local_size_y = FORCE_BAKE_CHUNK_SAMPLES) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

// Matches RendererVKLayout::ForceBakeChunksGpu.
layout (binding = 5, std430) readonly buffer Chunks
{
    uint bk_count; float bk_sampleY; uint bk_pad0; uint bk_pad1;
    ivec4 bk_chunks[];
};
layout (binding = 6, std430) writeonly buffer OutField { vec4 out_field[]; };

void main()
{
    const uint chunk = gl_WorkGroupID.x;
    if (chunk >= bk_count)
        return;
    const ivec2 bc = bk_chunks[chunk].xy;
    const float spacing = 1.0; // = FORCE_BAKE_SAMPLE_SPACING (Layout.ixx)
    const vec3 pos = vec3(
        float(bc.x * int(FORCE_BAKE_CHUNK_SAMPLES) + int(gl_LocalInvocationID.x)) * spacing,
        bk_sampleY,
        float(bc.y * int(FORCE_BAKE_CHUNK_SAMPLES) + int(gl_LocalInvocationID.y)) * spacing);

    float phi[NUM_FORCE_TEAMS];
    forceAccumulate(pos, phi);

    // TEAM-SIZED stride: (NUM_FORCE_TEAMS + 3) / 4 vec4s per sample (one with <= 4 live teams —
    // half the readback). The CPU sampler mirrors the stride (ForceSystem::sampleBakedField).
    const uint vec4PerSample = (NUM_FORCE_TEAMS + 3u) / 4u;
    const uint idx = (chunk * FORCE_BAKE_CHUNK_SAMPLES * FORCE_BAKE_CHUNK_SAMPLES
        + gl_LocalInvocationID.y * FORCE_BAKE_CHUNK_SAMPLES + gl_LocalInvocationID.x) * vec4PerSample;
    vec4 outA = vec4(0.0);
    for (uint t = 0u; t < min(uint(NUM_FORCE_TEAMS), 4u); ++t)
        outA[t] = phi[t];
    out_field[idx] = outA;
#if NUM_FORCE_TEAMS > 4
    vec4 outB = vec4(0.0);
    for (uint t = 4u; t < NUM_FORCE_TEAMS; ++t)
        outB[t - 4u] = phi[t];
    out_field[idx + 1u] = outB;
#endif
}
