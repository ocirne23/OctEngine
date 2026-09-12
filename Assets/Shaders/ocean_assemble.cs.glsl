#version 460

// FFT ocean, pass 3/4: assemble the output maps from the transformed spectra.
//
// The spectrum was laid out centered (k = 0 mid-texture), which by the DFT shift theorem multiplies the
// spatial output by (-1)^(x+z) - undone here. Each cascade's two complex layers unpack into the 8 real
// signals, written as:
//   layer c                  : displacement (Dx, h, Dz, dDx/dz)   [choppy lambda applied at sample time]
//   layer   OCEAN_CASCADES+c : gradients    (dh/dx, dh/dz, dDx/dx, dDz/dz)
//   layer 2*OCEAN_CASCADES+c : (dhx^2, dhz^2, d2h/dt2, foam) - xy are slope second moments: the mip chain
//                              averages them, so at mip m the shader recovers the slope variance the
//                              filtering removed (E[s^2]_m - E[s]_m^2) and turns it into microfacet
//                              roughness (LEAN mapping, Olano & Baker 2010; Bruneton et al. 2010).
//                              z is the surface's vertical acceleration (Longuet-Higgins breaking-crest
//                              foam source). w is the accumulated foam (ocean_foam.cs.glsl, cascade 0).
// The mip chain is blitted afterwards by OceanSimulationPipeline.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, rgba32f) uniform readonly image2DArray in_fft;
layout (binding = 1, rgba16f) uniform writeonly image2DArray out_maps;

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const int cascade = int(gl_GlobalInvocationID.z);

    const float sgn = ((xy.x + xy.y) & 1) == 0 ? 1.0 : -1.0;
    const vec4 a = imageLoad(in_fft, ivec3(xy, cascade * 3 + 0)) * sgn; // (h, Dx | Dz, dh/dx)
    const vec4 b = imageLoad(in_fft, ivec3(xy, cascade * 3 + 1)) * sgn; // (dh/dz, dDx/dx | dDz/dz, dDx/dz)
    const vec4 c = imageLoad(in_fft, ivec3(xy, cascade * 3 + 2)) * sgn; // (d2h/dt2, dh/dt | -, -)

    imageStore(out_maps, ivec3(xy, cascade),                      vec4(a.y, a.x, a.z, b.w)); // Dx, h, Dz, dDx/dz
    imageStore(out_maps, ivec3(xy, OCEAN_CASCADES + cascade),     vec4(a.w, b.x, b.y, b.z)); // dh/dx, dh/dz, dDxx, dDzz
    imageStore(out_maps, ivec3(xy, 2 * OCEAN_CASCADES + cascade), vec4(a.w * a.w, b.x * b.x, c.x, 0.0));
}
