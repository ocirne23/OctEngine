#version 450

#extension GL_EXT_nonuniform_qualifier : enable

// A studio light of its own: two directional lights and a hemisphere ambient. The preview shows the ASSET; the scene's sun, GI, shadows and light grid all
// belong to the main view and are not read here.

layout (push_constant) uniform PreviewPC
{
    mat4 u_viewProj;
    vec4 u_posScale;
    vec4 u_quat;
    vec4 u_tintMaterial;  // xyz = flat colour (x < 0: none), w = material index
    vec4 u_cameraFlags;   // xyz = camera position, w = 1: highlighted
};

struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};

layout (binding = 0, std430) readonly buffer InMaterialInfos
{
    MaterialInfo in_materialInfos[];
};
layout (binding = 1) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count

layout (location = 0) in vec3 in_worldPos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec4 in_tangent;
layout (location = 3) in vec2 in_uv;

layout (location = 0) out vec4 out_color;

float ggx(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}

vec3 shadeLight(vec3 N, vec3 V, vec3 L, vec3 lightColor, vec3 albedo, vec3 F0, float roughness)
{
    const float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);
    const vec3 H = normalize(L + V);
    const float VdotH = max(dot(V, H), 0.0);
    const vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    // The Kelemen visibility approximation: this is a viewer, not the scene's BRDF.
    const vec3 specular = F * ggx(max(dot(N, H), 0.0), roughness) * 0.25 / max(VdotH * VdotH, 0.05);
    return (albedo * (1.0 - F) / 3.14159265 + specular) * lightColor * NdotL;
}

void main()
{
    const bool flat_ = u_tintMaterial.x >= 0.0;
    const MaterialInfo material = in_materialInfos[uint(u_tintMaterial.w)];
    const uint diffuseTexIdx = material.diffuseNormalTexIdx & 0xFFFFu;
    const uint normalTexIdx = material.diffuseNormalTexIdx >> 16;
    const uint metalRoughnessTexIdx = material.metalRoughnessTexIdxAlphaMode & 0xFFFFu;
    const uint alphaMode = material.metalRoughnessTexIdxAlphaMode >> 16;

    vec4 diffuse = texture(u_textures[diffuseTexIdx], in_uv);
    if (alphaMode == ALPHA_MODE_MASK && diffuse.a < material.opacity)
        discard;
    if (alphaMode == ALPHA_MODE_BLEND && diffuse.a * material.opacity < 0.05)
        discard;
    if (flat_)
        diffuse.xyz = u_tintMaterial.xyz;

    const vec3 V = normalize(u_cameraFlags.xyz - in_worldPos);
    vec3 N = normalize(in_normal);
    if (dot(N, V) < 0.0)
        N = -N; // the pass draws both faces

    float roughness = 0.65;
    float metalness = 0.0;
    if (!flat_)
    {
        if (metalRoughnessTexIdx != 0xFFFFu)
        {
            const vec2 mr = texture(u_textures[metalRoughnessTexIdx], in_uv).bg;
            metalness = mr.x;
            roughness = mr.y;
        }
        const vec3 normalSample = texture(u_textures[normalTexIdx], in_uv).xyz;
        vec3 tangentNormal;
        if ((material.flags & MATERIAL_FLAG_BC5_NORMAL) != 0u)
        {
            const vec2 xy = normalSample.xy * 2.0 - 1.0;
            tangentNormal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        }
        else
            tangentNormal = normalSample * 2.0 - 1.0;
        const vec3 T = in_tangent.xyz - N * dot(N, in_tangent.xyz);
        if (dot(T, T) > 1e-6)
        {
            const vec3 Tn = normalize(T);
            const vec3 B = cross(N, Tn) * in_tangent.w;
            N = normalize(Tn * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
        }
    }
    roughness = clamp(roughness, 0.08, 1.0);

    const vec3 albedo = diffuse.xyz * (1.0 - metalness);
    const vec3 F0 = mix(vec3(0.04), diffuse.xyz, metalness);

    vec3 color = shadeLight(N, V, normalize(vec3(0.55, 0.75, 0.35)), vec3(3.0, 2.85, 2.65), albedo, F0, roughness);
    color += shadeLight(N, V, normalize(vec3(-0.7, 0.25, -0.45)), vec3(0.75, 0.85, 1.0), albedo, F0, roughness);
    // Hemisphere ambient, metals get it through F0 so they do not read black.
    const vec3 ambient = mix(vec3(0.16, 0.15, 0.14), vec3(0.42, 0.46, 0.52), N.y * 0.5 + 0.5);
    color += ambient * (albedo + F0 * (1.0 - roughness * 0.7));

    if (u_cameraFlags.w > 0.5)
    {
        const float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
        color += vec3(1.0, 0.55, 0.12) * rim * 1.5;
    }

    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14); // filmic curve
    out_color = vec4(pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
