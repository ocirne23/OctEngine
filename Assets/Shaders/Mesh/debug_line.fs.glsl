#version 450

// Debug line overlay fragment: flat color into the linear HDR scene target (sRGB-ish input squared
// to approximate linearization; exposure/tonemap happen later in the composite).

layout (location = 0) in vec3 v_color;

layout (location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(v_color * v_color, 1.0);
}
