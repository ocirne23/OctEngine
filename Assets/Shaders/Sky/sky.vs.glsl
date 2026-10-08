#version 460

// Fullscreen triangle on the far plane (reversed-Z 0, no vertex buffers). With the Sky variant's GREATER_OR_EQUAL
// test only the pixels nothing drew pass. sky.fs.glsl builds the view ray from gl_FragCoord.

void main()
{
    const vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
