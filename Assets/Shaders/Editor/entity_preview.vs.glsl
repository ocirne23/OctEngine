#version 450

layout (push_constant) uniform PreviewPC
{
    mat4 u_viewProj;
    vec4 u_posScale;
    vec4 u_quat;
    vec4 u_tintMaterial;  // xyz = flat colour (x < 0: none), w = material index
    vec4 u_cameraFlags;   // xyz = camera position, w = 1: highlighted
};

layout (location = 0) in vec4 in_posU;
layout (location = 1) in vec4 in_normalV;
layout (location = 2) in vec4 in_tangent;

layout (location = 0) out vec3 out_worldPos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_tangent;
layout (location = 3) out vec2 out_uv;

vec3 quatTransform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    out_worldPos = quatTransform(in_posU.xyz * u_posScale.w, u_quat) + u_posScale.xyz;
    out_normal = quatTransform(in_normalV.xyz, u_quat);
    // The tree meshes pack a wind payload into the magnitude of the sign; only the sign matters here.
    out_tangent = vec4(quatTransform(in_tangent.xyz, u_quat), in_tangent.w < 0.0 ? -1.0 : 1.0);
    out_uv = vec2(in_posU.w, in_normalV.w);
    gl_Position = u_viewProj * vec4(out_worldPos, 1.0);
}
