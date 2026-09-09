#version 450

#include "shared.inc.glsl"

// Sky radiance lookup for the GI probe trace's MISS rays: skyRadiance(dir) (a 4-step atmosphere march
// plus the sunlit-ground term, ~10 Chapman evaluations) baked once per frame into a small lat-long map.
// The trace samples it bilinearly (skyMiss in gi_probe_trace.cs.glsl, the same mapping) instead of
// marching the atmosphere per miss — on the coarse cascades most gather rays miss.
// Mapping (world +Y pole): u = atan(d.x, d.z) / 2pi + 0.5 (repeat), v = acos(d.y) / pi (clamp).

layout (binding = 1, rgba16f) uniform writeonly image2D u_skyMap;

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    const ivec2 size = imageSize(u_skyMap);
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, size)))
        return;
    const vec2 uv = (vec2(xy) + 0.5) / vec2(size);
    const float phi = (uv.x - 0.5) * 2.0 * PI;
    const float theta = uv.y * PI;
    const float st = sin(theta);
    const vec3 dir = vec3(st * sin(phi), cos(theta), st * cos(phi));
    imageStore(u_skyMap, xy, vec4(skyRadiance(dir), 1.0));
}
