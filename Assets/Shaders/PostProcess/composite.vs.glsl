#version 450

// Fullscreen triangle (no vertex buffers). Emits a [0,1] UV that maps 1:1 to the full render target, so the
// fragment can sample the TAA-resolved image at its own screen position. UV origin is top-left, matching the
// compute pass that wrote the resolved image.

layout (location = 0) out vec2 v_uv;

void main()
{
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
