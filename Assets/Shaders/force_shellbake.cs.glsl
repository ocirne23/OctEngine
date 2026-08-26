#version 460

// The SAMPLED SHELL TIER's volume bake: one thread per voxel of the fixed-size 3D bake volume,
// accumulating EVERY team's analytic field (the same forceAccumulate every evaluation uses — the
// volume therefore carries small-bubble deformation of the big shells too) into two RGBA16F
// volumes (phi[0..3] / phi[4..7]). The volume's world mapping (u_forceBake0/1) is refit each frame
// over the union of the LARGE drawable emitters' support boxes, so the fixed texel grid's
// resolution self-adjusts. Shell proxies of large emitters march THESE textures instead of the
// per-sample analytic candidate loop (force_shell.fs.glsl).

layout (local_size_x = FORCE_SHELL_VOLUME_GROUP, local_size_y = FORCE_SHELL_VOLUME_GROUP,
    local_size_z = FORCE_SHELL_VOLUME_GROUP) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

// TEAM-SIZED (see ForceFieldPipeline::createShellVolume): ONE RGBA16F volume at <= 4 live teams,
// a second only at 5+. Always rgba16f: a format-less writeonly image and rg16f stores each need a
// device feature the engine does not enable, and rgba16f is in the always-supported storage set.
layout (binding = 5, rgba16f) uniform writeonly image3D u_outA; // phi[0..3]
#if NUM_FORCE_TEAMS > 4
layout (binding = 6, rgba16f) uniform writeonly image3D u_outB; // phi[4..7]
#endif

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID);
    const ivec3 dims = imageSize(u_outA);
    if (any(greaterThanEqual(p, dims)))
        return;
    // Voxel CENTERS: world = min + (i + 0.5) / dims * size — texture() at uvw = (x - min) / size
    // then samples exactly these centers (the shell FS's mapping).
    const vec3 size = 1.0 / max(u_forceBake1.xyz, vec3(1e-9));
    const vec3 world = u_forceBake0.xyz + (vec3(p) + 0.5) / vec3(dims) * size;

    float phi[NUM_FORCE_TEAMS];
    forceAccumulate(world, phi);
    vec4 outA = vec4(0.0);
    for (uint t = 0u; t < min(uint(NUM_FORCE_TEAMS), 4u); ++t)
        outA[t] = phi[t];
    imageStore(u_outA, p, outA);
#if NUM_FORCE_TEAMS > 4
    vec4 outB = vec4(0.0);
    for (uint t = 4u; t < NUM_FORCE_TEAMS; ++t)
        outB[t - 4u] = phi[t];
    imageStore(u_outB, p, outB);
#endif
}
