export module RendererVK:LightingUtils;

import Core;
import Core.glm;
import Core.Camera;

import :Layout;

export float radicalInverse(uint32 i, uint32 base);
export float sunVisibleFraction(const glm::vec3& sunDir, const glm::vec3& moonDir, float cosSunRadius, float cosMoonRadius, float sunGlow);
export void computeSunCascades(const Camera& camera, float aspect, const glm::vec3& sunDir,
    float shadowFar, float splitLambda, float casterPad, glm::mat4(&outViewProj)[RendererVKLayout::NUM_SHADOW_CASCADES]);

// Radical inverse in an arbitrary base; (Halton(2), Halton(3)) gives the low-discrepancy sub-pixel jitter
// sequence used by the TAA accumulation.
float radicalInverse(uint32 i, uint32 base)
{
    float invBase = 1.0f / (float)base;
    float result = 0.0f;
    float f = invBase;
    while (i > 0)
    {
        result += (float)(i % base) * f;
        i /= base;
        f *= invBase;
    }
    return result;
}

// Solar eclipse: fraction of the sun's disc (plus its glow halo, which acts as an extended light
// source) the moon leaves visible. 1 = no overlap, 0 = totality, with the annular 1 - rm^2/rs^2
// floor when the moon is smaller than sun + corona. Radii and separation are chord lengths
// (sqrt(2-2cos) == the angle for discs this small); the partial phase uses a smoothstep area
// approximation (within a few percent of the exact circle-circle lens area).
float sunVisibleFraction(const glm::vec3& sunDir, const glm::vec3& moonDir,
    float cosSunRadius, float cosMoonRadius, float sunGlow)
{
    const float rs = sqrtf(oc::max(2.0f - 2.0f * cosSunRadius, 0.0f)) + sunGlow * 0.02f;
    const float rm = sqrtf(oc::max(2.0f - 2.0f * cosMoonRadius, 0.0f));
    const float d = glm::distance(sunDir, moonDir);
    const float contained = rm >= rs ? 0.0f : 1.0f - (rm * rm) / (rs * rs);
    return glm::mix(contained, 1.0f, glm::smoothstep(fabsf(rs - rm), rs + rm, d));
}

// Cascade distribution knobs (ShadowParams tweaks, "Shadows" panel):
//   shadowFar:    max distance from the camera that receives sun shadows. Lower = every cascade
//                 covers less ground = higher resolution everywhere (at the cost of range).
//   splitLambda:  0 = evenly spaced splits (uniform), 1 = logarithmic (splits bunch up close to
//                 the camera). Higher = more shadow-map resolution near the camera.
//   casterPad:    up-sun depth-slab extension per cascade — see the comment at its use.
void computeSunCascades(const Camera& camera, float aspect, const glm::vec3& sunDir,
    float shadowFar, float splitLambda, float casterPad, glm::mat4(&outViewProj)[RendererVKLayout::NUM_SHADOW_CASCADES])
{
    constexpr uint32 N = RendererVKLayout::NUM_SHADOW_CASCADES;
    const float shadowNear = camera.near;
    shadowFar = glm::max(shadowFar, shadowNear + 1.0f);
    splitLambda = glm::clamp(splitLambda, 0.0f, 1.0f);
    const float res = (float)RendererVKLayout::SHADOW_MAP_RESOLUTION;

    const glm::mat4 invView = glm::inverse(camera.viewMatrix);
    const glm::vec3 camPos = glm::vec3(invView[3]);
    const glm::vec3 right = glm::normalize(glm::vec3(invView[0]));
    const glm::vec3 up = glm::normalize(glm::vec3(invView[1]));
    const glm::vec3 forward = -glm::normalize(glm::vec3(invView[2])); // right-handed: -Z is forward
    const float tanHalfV = tanf(glm::radians(camera.fovDeg) * 0.5f);

    float splits[N + 1];
    splits[0] = shadowNear;
    for (uint32 i = 1; i <= N; ++i)
    {
        const float p = (float)i / (float)N;
        const float logd = shadowNear * powf(shadowFar / shadowNear, p);
        const float lind = shadowNear + (shadowFar - shadowNear) * p;
        splits[i] = glm::mix(lind, logd, splitLambda); // practical split scheme
    }

    const glm::vec3 L = glm::normalize(sunDir);
    const glm::vec3 upRef = (fabsf(L.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);

    for (uint32 c = 0; c < N; ++c)
    {
        const float dists[2] = { splits[c], splits[c + 1] };
        glm::vec3 corners[8];
        int idx = 0;
        for (int di = 0; di < 2; ++di)
        {
            const float d = dists[di];
            const float h = d * tanHalfV;
            const float w = h * aspect;
            const glm::vec3 cc = camPos + forward * d;
            corners[idx++] = cc + up * h + right * w;
            corners[idx++] = cc + up * h - right * w;
            corners[idx++] = cc - up * h + right * w;
            corners[idx++] = cc - up * h - right * w;
        }
        glm::vec3 center(0.0f);
        for (int k = 0; k < 8; ++k) center += corners[k];
        center /= 8.0f;
        float radius = 0.0f;
        for (int k = 0; k < 8; ++k) radius = glm::max(radius, glm::length(corners[k] - center));
        radius = ceilf(radius * 16.0f) / 16.0f;

        // How far up-sun a caster can be above this cascade and still be captured. Extends only the
        // near side of the depth slab (the far plane stays tight to the bounding sphere), so the only
        // cost is depth precision over a longer range (fine for D32). It must exceed the tallest
        // caster's height above the cascade; too small and casters get clipped out of the depth map,
        // making their shadows pop/disappear as the cascade slab slides with camera motion.
        const float zPad = casterPad;
        // L points towards the sun, so the light sits up-sun of the scene looking back along -L.
        const glm::vec3 eye = center + L * (radius + zPad);
        const glm::mat4 lightView = glm::lookAtRH(eye, center, upRef);
        glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + zPad);

        // Snap the projected origin to whole texels to keep the shadow stable under camera motion.
        const glm::mat4 vp = lightProj * lightView;
        glm::vec4 origin = vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        origin *= res * 0.5f;
        const glm::vec2 rounded = glm::round(glm::vec2(origin));
        const glm::vec2 off = (rounded - glm::vec2(origin)) * (2.0f / res);
        lightProj[3][0] += off.x;
        lightProj[3][1] += off.y;

        glm::mat4 viewProj = lightProj * lightView;
        // Stash per-cascade scalars in the matrix's structurally-zero bottom row: far distance in
        // m[0][3], world texel size (ortho width / resolution) in m[1][3], ortho depth range (world
        // units, converts normalized depth gaps back to world for the PCSS penumbra) in m[2][3].
        // Shaders mask these back to the canonical [0,0,0,1] bottom row before using the matrix.
        viewProj[0][3] = dists[1];
        viewProj[1][3] = (2.0f * radius) / res;
        viewProj[2][3] = 2.0f * radius + zPad;
        outViewProj[c] = viewProj;
    }
}