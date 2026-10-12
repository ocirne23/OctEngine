#version 450

// Fullscreen triangle. The [0,1] UV maps 1:1 to the full render target, origin top-left like the resolved image.

layout (location = 0) out vec2 v_uv;

void main()
{
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
