#version 460

#extension GL_EXT_nonuniform_qualifier : enable

// IRRADIANCE VOLUME BAKE (GIProbePipeline::recordVolumeBake). One invocation per voxel of every cascade: the
// voxel lattice is the probe lattice refined GI_VOLUME_RES times per axis, stored toroidally exactly like the
// probes (slot = fine lattice coord & (dims - 1)), so the forward shaders sample it with REPEAT addressing and
// hardware trilinear filtering (evalProbeVolumeCoverage in gi_probe.inc.glsl).
//
// A voxel holds the visibility-weighted blend of its 8 probes, evaluated AT THE VOXEL CENTRE: the trilinear
// weight, the backface-dead fade and the Chebyshev depth test of giSampleCascade. The half-Lambert term toward
// each probe needs the surface normal, so it is NOT baked - the price of the volume (more leaking through
// geometry thinner than a voxel). L0 is stored PREMULTIPLIED by the summed weight W (and W itself), so the
// trilinear fetch at a pixel is a weight-correct blend (sum A / sum W) in which a voxel whose probes are all
// dead contributes nothing instead of black; L1 is stored as ratios to L0 (16 B per voxel, see the encode).

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };
#define GI_GRID_DATA_NAME gi_gridData
#include "gi_probe.inc.glsl"

// Cascade c's images, 16 B per voxel (the layout is documented at RendererVKLayout::GI_VOLUME_FORMATS):
//   L0 x W at u_volumeL0[c], the L1 ratios at u_volumeL1[2c], [2c + 1], the last ratio + W at u_volumeTail[c].
layout (binding = 2, r11f_g11f_b10f) uniform writeonly image3D u_volumeL0[GI_MAX_CASCADES];
layout (binding = 3, rgba8_snorm)    uniform writeonly image3D u_volumeL1[GI_MAX_CASCADES * 2];
layout (binding = 5, rg16f)          uniform writeonly image3D u_volumeTail[GI_MAX_CASCADES];
// The virtual sky probe's 3 SH vec4s (the out-of-field fallback, giEvalSkySH), copied every frame: the trace
// re-projects the sky each frame, and the consumers in volume mode read nothing from the probe buffer.
layout (binding = 4, rgba16f) uniform writeonly image3D u_volumeSky;
// The trace's per-wave visit stamps (gi_probe_trace.cs.glsl): u_frameIndex + 1 = the wave was traced this frame.
layout (binding = 6, std430) readonly buffer GiWaveStamps { uint gi_waveStamp[]; };

#define GI_VOLUME_DIMS (GI_PROBE_DIMS * GI_VOLUME_RES)

layout (local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

void main()
{
    // z runs over every cascade's voxel slabs; a volume dim is a multiple of 4, so a workgroup never spans two
    // (so `cascade` is uniform across the workgroup - the image-array stores below rely on it).
    const ivec3 gid     = ivec3(gl_GlobalInvocationID);
    // Before any early-out: the sky SH changes every frame, whatever the partial bake skips.
    if (all(equal(gid, ivec3(0))))
        for (int k = 0; k < GI_VOLUME_SKY_TEXELS; ++k)
            imageStore(u_volumeSky, ivec3(k, 0, 0), GI_GRID_DATA_NAME[GI_SKY_SH_BASE + uint(k)]);
    const int   cascade = gid.z / GI_VOLUME_DIMS.z;
    if (cascade >= GI_NUM_CASCADES)
        return;
    const ivec3 vslot   = ivec3(gid.xy, gid.z - cascade * GI_VOLUME_DIMS.z);
    const int   s       = giCascadeSpacing(cascade);
    const ivec3 origin  = giCascadeOrigin(cascade, u_sceneFocus.xyz);
    const ivec3 originV = origin * GI_VOLUME_RES;
    const ivec3 fl      = originV + ((vslot - originV) & (GI_VOLUME_DIMS - 1)); // the fine lattice coord living in this slot

    // Voxel centre in probe-lattice units and world units, and its 8-probe stencil.
    const vec3  q      = (vec3(fl) + 0.5) / float(GI_VOLUME_RES);
    const ivec3 base   = ivec3(floor(q));
    const vec3  frac   = q - vec3(base);
    const vec3  center = q * float(s);

    // PARTIAL BAKE: the voxel only changes when one of its 8 probes does, i.e. when the trace visited that
    // probe's wave this frame (the trace's OWN stamp - no second evaluation of the schedule) or the probe is
    // fresh (giProbeFresh, the trace's own test: the stencil's two corners cover all 8 probes). Otherwise the
    // voxel keeps last frame's value. A voxel that scrolled in always has a fresh probe (its base probe is
    // outside the previous window), so the scroll needs no test of its own. u_giVisParams.y > 0.5 = bake
    // everything (the CPU sets it for one frame when the images are new or the Chebyshev knobs moved: the
    // baked value depends on them, and a far probe may not be visited for hundreds of frames).
    if (u_giVisParams.y < 0.5)
    {
        bool changed = giProbeFresh(cascade, base) || giProbeFresh(cascade, base + 1);
        // The stencil spans base..base+1: per axis one wave, or two where it crosses a 4-probe block edge.
        const ivec3 waveLo = giWaveMin(base, origin), waveHi = giWaveMin(base + 1, origin);
        const ivec3 waveCount = ivec3(notEqual(waveLo, waveHi)) + 1;
        const uint  stamp = u_frameIndex + 1u;
        for (int wz = 0; wz < waveCount.z && !changed; ++wz)
            for (int wy = 0; wy < waveCount.y && !changed; ++wy)
                for (int wx = 0; wx < waveCount.x && !changed; ++wx)
                    changed = gi_waveStamp[giWaveWorkgroup(cascade, mix(waveLo, waveHi, bvec3(wx == 1, wy == 1, wz == 1)))] == stamp;
        if (!changed)
            return;
    }

    vec3 a0 = vec3(0.0), a1 = vec3(0.0), a2 = vec3(0.0), a3 = vec3(0.0);
    float W = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        const ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const vec3  w3  = mix(1.0 - frac, frac, vec3(off));
        float w = w3.x * w3.y * w3.z;
        const ivec3 lc = base + off;
        // Voxels along the window's top faces reach one probe past it (its slot holds another probe). No
        // lookup reads them - the fade box keeps every sample two cells inside the window - so skip, not guess.
        if (w <= 0.0 || any(lessThan(lc, origin)) || any(greaterThanEqual(lc, origin + GI_PROBE_DIMS)))
            continue;
        const uint cellBase = giProbeBase(cascade, lc);

        const vec4 misc = GI_GRID_DATA_NAME[cellBase + GI_MISC_V4];
        w *= 1.0 - smoothstep(GI_BACKFACE_DEAD_MIN, GI_BACKFACE_DEAD_MAX, misc.x);
        if (w <= 0.0)
            continue;

        // The Chebyshev test of giSampleCascade, from the voxel centre instead of the shaded point.
        const vec3  toProbe = vec3(lc) * float(s) + misc.yzw - center;
        const float len     = length(toProbe);
        if (len > 1e-4)
        {
            vec4 dsh, d2sh;
            giReadDepthSH(cellBase, dsh, d2sh);
            float mean, variance;
            giVisMoments(dsh, d2sh, -toProbe / len, s, mean, variance);
            const float d = min(len, GI_DEPTH_CAP_SPACING * float(s) * 0.95);
            if (d2sh.x > 1e-4 && d > mean)
            {
                const float delta = d - mean;
                const float cheb  = variance / (variance + delta * delta);
                w *= max(giChebPow(cheb), u_giVisParams.z);
            }
        }

        vec3 c0, c1, c2, c3;
        giReadSH(cellBase, c0, c1, c2, c3);
        a0 += w * c0; a1 += w * c1; a2 += w * c2; a3 += w * c3;
        W += w;
    }

    // Encode (RendererVKLayout::GI_VOLUME_FORMATS). L0 stays premultiplied (a0 = sum w c0). The L1 terms become
    // ratios to L0 - the weights cancel (a_k / a0 = the normalized c_k / c0) - scaled by 1/sqrt(3) into SNORM
    // range; a black channel stores 0 (no direction). The unsigned 11/10-bit floats hold no negative and top out
    // at 65024: a0 >= 0 for a non-negative radiance, the clamps only guard numeric noise.
    const vec3 l0 = clamp(a0, vec3(0.0), vec3(65000.0));
    const vec3 inv = mix(vec3(0.0), 1.0 / (max(a0, vec3(1e-8)) * GI_SQRT3), greaterThan(a0, vec3(1e-8)));
    const vec3 q1 = clamp(a1 * inv, -1.0, 1.0), q2 = clamp(a2 * inv, -1.0, 1.0), q3 = clamp(a3 * inv, -1.0, 1.0);
    // `cascade` is uniform across the workgroup (see main's first comment), so these index the storage-image
    // arrays DYNAMICALLY UNIFORM - no nonuniformEXT, which would need shaderStorageImageArrayNonUniformIndexing.
    imageStore(u_volumeL0[cascade], vslot, vec4(l0, 0.0));
    imageStore(u_volumeL1[2 * cascade + 0], vslot, vec4(q1, q2.r));
    imageStore(u_volumeL1[2 * cascade + 1], vslot, vec4(q2.gb, q3.rg));
    imageStore(u_volumeTail[cascade], vslot, vec4(q3.b, W, 0.0, 0.0));
}
