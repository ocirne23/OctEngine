#version 450

layout (push_constant) uniform PreviewPC
{
    mat4 u_viewProj;
    vec4 u_posScale;
    vec4 u_quat;
    vec4 u_tintMaterial;  // xyz = flat colour (x < 0: none), w = material index
    ivec4 u_flags;        // x = highlighted, y = skinning index delta, z = palette base in vec4s (-1: bind pose)
};

// RendererVKLayout::SkinningVertex: the influences of a skinned source, a parallel stream to the vertices.
struct SkinningVertex { uvec4 boneIndices; vec4 boneWeights; };
layout (binding = 1, std430) readonly buffer SkinningBuffer { SkinningVertex s_data[]; };
// [0] = the camera, then the palette: 4 vec4 columns per bone (global * inverseBind).
layout (binding = 2, std430) readonly buffer PreviewFrame { vec4 f_data[]; };

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

mat4 paletteMatrix(int base, uint bone)
{
    const int o = base + int(bone) * 4;
    return mat4(f_data[o], f_data[o + 1], f_data[o + 2], f_data[o + 3]);
}

void main()
{
    vec3 position = in_posU.xyz;
    vec3 normal = in_normalV.xyz;
    vec3 tangent = in_tangent.xyz;
    if (u_flags.z >= 0)
    {
        // gl_VertexIndex already holds the draw's vertexOffset: the delta moves it into the influence stream.
        const SkinningVertex sv = s_data[gl_VertexIndex + u_flags.y];
        mat4 skin = mat4(0.0);
        for (int b = 0; b < 4; ++b)
        {
            const float w = sv.boneWeights[b];
            if (w > 0.0)
                skin += paletteMatrix(u_flags.z, sv.boneIndices[b]) * w;
        }
        const mat3 skinRot = mat3(skin);
        position = (skin * vec4(position, 1.0)).xyz;
        normal = normalize(skinRot * normal);
        tangent = skinRot * tangent;
    }

    out_worldPos = quatTransform(position * u_posScale.w, u_quat) + u_posScale.xyz;
    out_normal = quatTransform(normal, u_quat);
    // The tree meshes pack a wind payload into the magnitude of the sign; only the sign matters here.
    out_tangent = vec4(quatTransform(tangent, u_quat), in_tangent.w < 0.0 ? -1.0 : 1.0);
    out_uv = vec2(in_posU.w, in_normalV.w);
    gl_Position = u_viewProj * vec4(out_worldPos, 1.0);
}
