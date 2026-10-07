#version 450

// Debug line overlay: pulls world-space line vertices from a storage buffer (no vertex attributes),
// two consecutive entries per line (LINE_LIST). Matches DebugLinePipeline::LineVertex: xyz = position,
// w = packed RGBA8 color (R low byte).

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer LineVertices { vec4 lv_data[]; };

layout (location = 0) out vec3 v_color;

void main()
{
    const vec4 data = lv_data[gl_VertexIndex];
    v_color = unpackUnorm4x8(floatBitsToUint(data.w)).rgb;
    gl_Position = u_mvp * vec4(data.xyz, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
    gl_Position.z = mix(gl_Position.w, gl_Position.z, 0.01);
}
