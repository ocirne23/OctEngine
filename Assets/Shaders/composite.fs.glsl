#version 450

// Composites the TAA-resolved scene colour into the swapchain: applies the scene exposure and the
// selected tonemap operator (the only place HDR is mapped to display range - everything upstream is
// linear radiance). The resolved image is full render-target sized; the area outside the editor
// viewport panel holds the scene clear colour and is later covered by ImGui.

layout (location = 0) in vec2 v_uv;
layout (binding = 0) uniform sampler2D u_resolved;
layout (binding = 1, std430) readonly buffer Adapt { float u_avgLum; float u_autoExposure; };
layout (location = 0) out vec4 out_color;

layout (push_constant) uniform PostPC
{
    float u_exposure;   // linear scale (exp2 of the EV tweak); in auto mode this is exposure compensation
    int   u_tonemapper; // 0 = off (clip), 1 = Reinhard, 2 = ACES, 3 = AgX
    int   u_autoExpEnable; // 1 = multiply by the eye-adaptation exposure, 0 = manual exposure only
};

// Extended Reinhard on luminance (hue-preserving, soft asymptote at white = 4).
vec3 tonemapReinhard(vec3 c)
{
    const float whiteSq = 16.0;
    float l = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-6);
    float lOut = l * (1.0 + l / whiteSq) / (1.0 + l);
    return c * (lOut / l);
}

// ACES filmic fit (Stephen Hill / Krzysztof Narkowicz "ACES fitted" RRT+ODT approximation).
vec3 tonemapACES(vec3 c)
{
    const mat3 inMat = mat3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777);
    const mat3 outMat = mat3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    c = inMat * c;
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    return clamp(outMat * (a / b), 0.0, 1.0);
}

// Minimal AgX (Benjamin Wrensch's approximation of Troy Sobotka's AgX): inset matrix, log2
// encoding over ~17.5 stops, 6th-order sigmoid fit, outset matrix. Desaturates highlights
// gracefully instead of skewing hue, which suits very bright skies/suns.
vec3 agxContrast(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}
vec3 tonemapAgX(vec3 c)
{
    const mat3 inset = mat3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv =   4.026069;
    c = inset * max(c, vec3(0.0));
    c = clamp((log2(max(c, vec3(1e-10))) - minEv) / (maxEv - minEv), 0.0, 1.0);
    c = agxContrast(c);
    return clamp(outset * c, 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main()
{
    float exposure = u_exposure * (u_autoExpEnable != 0 ? u_autoExposure : 1.0);
    vec3 color = texture(u_resolved, v_uv).rgb * exposure;
    // The swapchain is UNORM (no hardware sRGB encode), so display encoding happens here too.
    // "Off" keeps the legacy raw-linear passthrough; AgX's sigmoid already outputs display-encoded.
    if      (u_tonemapper == 1) color = linearToSrgb(tonemapReinhard(color));
    else if (u_tonemapper == 2) color = linearToSrgb(tonemapACES(color));
    else if (u_tonemapper == 3) color = tonemapAgX(color);
    out_color = vec4(color, 1.0);
}
