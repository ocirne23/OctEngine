module RendererVK;

import RendererVK.fwd;

import Core;
import Core.fwd;
import Core.glm;
import Core.Window;
import Core.Frustum;
import Core.imgui;
import Core.Camera;
import Core.Tweaks;
import Core.Time;
import Core.Log;

import File;

import :RenderNode;
import :VK;
import :GpuProfiler;
import :Allocator;
import :StagingManager;
import :TextureManager;
import :TextureStreamer;
import :MeshStreamer;
import :MeshDataManager;
import :glslang;
import :Layout;
import :ObjectContainer;
import :LightingUtils;

// Renderer: THE FRAME UBO. buildFrameUbo assembles the one uniform buffer every pass reads, from the
// param blocks the outside pushes in and the registries under Data/; the buildUbo* helpers below split
// it by subject. Pure CPU math - nothing here records or touches the device.
//
// It runs wherever beginFrame runs (the desktop "Begin frame job" or main in VR), so everything it
// reads is written only outside that quiescent window, and m_ubo PERSISTS across frames: buildUboViews
// reads last frame's mvps out of it for reprojection before overwriting them.

// m_ubo persists across frames: buildUboViews reads last frame's mvps out of it for reprojection
// before overwriting them. Runs wherever beginFrame runs (the desktop "Begin frame job" or main in
// VR); every param it reads (tweak-bound floats, force/terrain params) is written only outside the
// quiescent window. The outputs consumers read directly (m_centerViewProj, m_sunCascadeViewProj, the
// returned frustum) are contractually valid once beginFrame's job is joined.
void Renderer::buildFrameUbo(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation, PerFrameData& frameData)
{
    ProfileScope uboScope("UBO build", EProfileCategory::Renderer);
    RendererVKLayout::Ubo& ubo = m_ubo;

    ubo.lodParams0 = glm::vec4(
        oc::max(0.01f, m_lodParams.maxErrorPixels) * std::exp2((float)m_lodParams.bias),
        m_lodParams.hysteresis, m_lodParams.fullResPixels, m_mipPixelScale);
    ubo.lodParams1 = glm::vec4((float)m_lodParams.forceLod, (float)m_lodParams.bias,
        m_lodParams.enabled ? 1.0f : 0.0f, 0.0f);
    ubo.foliageParams = glm::vec4(m_foliageParams.rtRange, m_foliageParams.crownNormal,
        m_foliageParams.shadowLength, m_foliageParams.interiorShadow);
    ubo.foliageParams2 = glm::vec4(m_foliageParams.edgeFadeStart, m_foliageParams.edgeFadeEnd,
        m_foliageParams.edgeFadeCentreScale, glm::clamp(m_foliageParams.interiorStart, 0.0f, 1.0f));
    ubo.foliageParams3 = glm::vec4(glm::clamp(m_foliageParams.interiorEnd, 0.0f, 1.0f), m_foliageParams.edgeFadeTopCardScale,
        m_foliageParams.interiorTopCardScale, m_foliageParams.transmission);
    ubo.foliageParams4 = glm::vec4(m_foliageParams.transmissionFocus, m_foliageParams.transmissionGlow,
        m_foliageParams.transmissionShadow, farTreesActive() ? 1.0f : 0.0f);
    ubo.foliageParams5 = glm::vec4(glm::clamp(m_foliageParams.minNoV, 0.0f, 0.9f), glm::max(m_foliageParams.selfShadow, 0.0f),
        glm::clamp(m_foliageParams.interiorViewFade, 0.0f, 1.0f), glm::clamp(m_foliageParams.transmissionSelfShadow, 0.0f, 1.0f));
    ubo.treeCull = glm::uvec4(0u); // no tree range until present() patches it (uploadTreeCullUbo)
    ubo.treeCullParams = glm::vec4(0.0f);
    buildUboViews(cameraIn, camera, vrBaseOrientation);

    // Camera velocity over the WALL-CLOCK frame delta (camera motion is not paused with the sim): the
    // weather volumes' streaks are motion blur relative to the eye. A teleport (first frame, scene
    // load) reads as zero rather than one huge streak.
    {
        const float dt = (float)Globals::time.getDeltaSec();
        glm::vec3 velocity(0.0f);
        if (m_havePrevCameraPos && dt > 1e-4f)
        {
            velocity = (camera.position - m_prevCameraPos) / dt;
            if (glm::dot(velocity, velocity) > 200.0f * 200.0f)
                velocity = glm::vec3(0.0f);
        }
        m_prevCameraPos = camera.position;
        m_havePrevCameraPos = true;
        ubo.cameraVelocity = glm::vec4(velocity, glm::clamp(m_particles.getParams().streakCameraBlur, 0.0f, 1.0f));
    }
    {
        const ParticleParams& particles = m_particles.getParams();
        const float a = glm::radians(particles.windAngleDeg);
        const glm::vec2 dir(std::cos(a), std::sin(a));
        ubo.weatherWind0 = glm::vec4(dir.x * particles.windSpeed, 0.0f, dir.y * particles.windSpeed, glm::max(particles.windGustStrength, 0.0f));
        ubo.weatherWind1 = glm::vec4(1.0f / glm::max(particles.windGustSize, 1.0f), glm::clamp(particles.windSheetContrast, 0.0f, 1.0f),
            1.0f / glm::max(particles.windSheetSize, 1.0f), glm::max(particles.windSheetDrift, 0.0f));
        ubo.weatherWind2 = glm::vec4(dir, m_oceanSimPipeline.getCameraWaterSurface(), m_oceanSimPipeline.hasCameraWaterSurface() ? 1.0f : 0.0f);
    }

    // RTAO and the GI probe contribution both need the acceleration structures, so both fold in the RT
    // master toggle; GI additionally gates on its own switch.
    // z = the RTAO max distance: past it the trace writes exactly (N, 1.0) (rtao.cs.glsl early-out), so
    // the forward pass skips its depth-aware AO upsample there and uses those values directly.
    ubo.aoParams = glm::vec4((m_rtParams.enabled && m_rtaoParams.enabled) ? 1.0f : 0.0f,
        (m_rtParams.enabled && m_rtParams.giEnabled) ? m_giProbePipeline.getStrength() : 0.0f,
        m_rtaoParams.maxDistance, 0.0f); // w unused: the light grid debug overlay is the LIGHT_GRID_DEBUG define
    ubo.giVisParams = m_giProbePipeline.takeVisibilityParams(m_rtParams.enabled && m_rtParams.giEnabled); // once per frame: y = the one-frame full volume bake
    // GI trace / TLAS-instance parameters (the GI secondary is cached, so everything per-frame rides here).
    // The previous focus advances only while GI traces, so probes that scrolled in during a GI-off spell
    // still read as fresh (full replace) on the first traced frame, as before.
    ubo.giTrace0 = m_giProbePipeline.getTraceParams0((float)Globals::time.getDeltaSec()); // WALL delta: GI converges through a sim pause
    ubo.giTrace1 = glm::vec4(m_giPrevFocusPos, m_giProbePipeline.getTlasRange());
    const glm::vec3 giPriority = m_giProbePipeline.getPriorityParams();
    ubo.giPriorityDist = giPriority.x;
    ubo.giPriorityFalloff = giPriority.y;
    ubo.giPriorityFrustumWeight = giPriority.z;
    if (m_rtParams.enabled && m_rtParams.giEnabled)
        m_giPrevFocusPos = sceneFocusOrCamera();
    ubo.giTlasNumInstances = 0; // the counter was just reset: present() patches the real count in
    ubo.frameIndex = m_frameCounter;
    // SIM clock, not the wall clock: shader animation (ocean waves, force pulses, fog) freezes with
    // the global pause (see Time::setPaused).
    ubo.timeSeconds = (float)Globals::time.getSimElapsedSec();

    buildUboSky();
    buildUboClouds(camera);
    buildUboSunShadow(camera);
    buildUboRainOcclusion();
    buildUboFog();
    buildUboOcean();
    buildUboForce();
    buildUboTerrain();
    buildUboGrass(camera);

    Globals::stagingManager.upload(frameData.ubo.getBuffer(), sizeof(RendererVKLayout::Ubo), &m_ubo);
}

// The grass (GrassParams). The patch size and the range set the patch grid uploadGrassFrame writes for the cull, which
// reads them from here: the range is capped so the grid fits GRASS_MAX_PATCHES.
void Renderer::buildUboGrass(const Camera& camera)
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const GrassParams& g = m_grassParams;
    const auto linear = [](const glm::vec3& srgb) { return glm::pow(glm::clamp(srgb, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f)); };
    const float patchSize = glm::clamp(g.patchSize, 1.0f, 16.0f);
    const uint32 maxHalf = (uint32)std::sqrt((float)RendererVKLayout::GRASS_MAX_PATCHES) / 2u - 1u; // (2 half + 1)^2 patches
    const float range = glm::min(g.range, patchSize * (float)maxHalf);
    ubo.grassParams0 = glm::vec4((float)m_grassPipeline.getBladesPerPatch(), patchSize, range, glm::clamp(g.rangeFade, 0.0f, range));
    ubo.grassParams1 = glm::vec4(g.bladeHeight, glm::clamp(g.heightVariation, 0.0f, 1.0f), g.bladeWidth, g.rootSink);
    ubo.grassParams2 = glm::vec4(glm::max(g.thinStart, 0.1f), g.thinExponent, g.widthCompensation, glm::max(g.maxWidthScale, 1.0f));
    // The pixel floor as world width per metre of distance: one pixel spans 2 d / m_mipPixelScale.
    const float minWidthPerMetre = m_mipPixelScale > 0.0f ? g.minPixelWidth * 2.0f / m_mipPixelScale : 0.0f;
    const float lod2Distance = glm::max(g.lod2Distance, g.lod1Distance);
    ubo.grassParams3 = glm::vec4(g.lod1Distance, lod2Distance, minWidthPerMetre, g.groundBlendDistance);
    ubo.grassParams10 = glm::vec4(glm::max(g.lod3Distance, lod2Distance), g.coldTemperature, g.warmTemperature,
        glm::clamp(g.coldDarkening, 0.0f, 1.0f));
    // w = the canopy's base extinction (1/m; grass.inc.glsl): "Canopy shadow" x blades per m^2 x the mean blade width
    // (it tapers to the tip: half the root width). 0 without grass: the terrain FS then skips the canopy.
    const float bladesPerM2 = (float)m_grassPipeline.getBladesPerPatch() / (patchSize * patchSize);
    const float canopyExtinction = grassActive() ? glm::max(g.canopyShadow, 0.0f) * bladesPerM2 * 0.5f * g.bladeWidth : 0.0f;
    ubo.grassParams11 = glm::vec4(glm::max(g.shadowBias, 0.0f), 0.0f, 0.0f, canopyExtinction);

    // THE NEAR GRASS CASCADE (the shadow array's extra layer): an ortho box of +-range looking down the sun - the
    // cascades' construction (LightingUtils computeSunCascades: standard Z, the eye up-sun, texel-snapped so the blade
    // shadows hold still while the camera moves). Its slab reaches 50 m up-sun past the box: blades on a slope up-sun
    // are never clipped.
    // AHEAD of the camera, not around it (half of a centred box lay behind the view): the bottom-centre ray of the view
    // meets the ground (the terrain height under the camera) at d0 along the horizontal view direction, and the centre
    // goes range - 1 m past that - the receivers' full-weight disc (range around the centre; grass.inc.glsl) then
    // starts 1 m behind the bottom of the frustum. Looking steeply down, d0 goes negative (capped at -range).
    const float nearRange = grassNearShadowActive() ? glm::max(g.nearShadowRange, 1.0f) : 0.0f;
    const float res = (float)RendererVKLayout::SHADOW_MAP_RESOLUTION;
    glm::vec3 nearCentre = m_cameraPos;
    if (nearRange > 0.0f)
    {
        const glm::mat4 invView = glm::inverse(camera.viewMatrix);
        const glm::vec3 forward = -glm::normalize(glm::vec3(invView[2]));
        const glm::vec3 up = glm::normalize(glm::vec3(invView[1]));
        const glm::vec3 bottomRay = forward - up * std::tan(glm::radians(camera.fovDeg) * 0.5f);
        glm::vec2 horizontal(forward.x, forward.z);
        if (glm::dot(horizontal, horizontal) < 0.01f) // looking straight down: the top of the screen is "ahead"
            horizontal = glm::vec2(up.x, up.z);
        horizontal = glm::dot(horizontal, horizontal) > 1e-8f ? glm::normalize(horizontal) : glm::vec2(0.0f, -1.0f);
        // The terrain under the camera (setCameraGround); unknown: assume the camera stands on the ground (sea level, the
        // old fallback, put the box at its cap far ahead wherever the land is high).
        const float ground = std::isnan(m_cameraGround) ? m_cameraPos.y - 2.0f : m_cameraGround;
        float d0 = 0.0f;
        if (bottomRay.y < -1e-4f && m_cameraPos.y > ground)
        {
            const glm::vec3 hit = m_cameraPos + bottomRay * ((ground - m_cameraPos.y) / bottomRay.y);
            d0 = glm::dot(glm::vec2(hit.x - m_cameraPos.x, hit.z - m_cameraPos.z), horizontal);
        }
        // At most HALF the range ahead: a low camera looking near the horizon meets the ground far ahead, and with the old
        // cap (4 x range) the box slid up to ~5 x range forward - the near grass, the shadows that matter most, fell out.
        d0 = glm::clamp(d0, -nearRange, 0.5f * nearRange);
        // The full-weight disc (range around the centre) starts 1 m behind where the bottom of the view meets the ground.
        constexpr float NEAR_BACK_MARGIN = 1.0f;
        const glm::vec2 centreXZ = glm::vec2(m_cameraPos.x, m_cameraPos.z) + horizontal * (d0 - NEAR_BACK_MARGIN + nearRange);
        nearCentre = glm::vec3(centreXZ.x, glm::min(m_cameraPos.y, ground), centreXZ.y);

        const glm::vec3 L = glm::normalize(glm::vec3(ubo.sunDirection));
        const glm::vec3 upRef = (fabsf(L.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const float zPad = 50.0f;
        const glm::vec3 eye = nearCentre + L * (nearRange + zPad);
        const glm::mat4 lightView = glm::lookAtRH(eye, nearCentre, upRef);
        glm::mat4 lightProj = glm::orthoRH_ZO(-nearRange, nearRange, -nearRange, nearRange, 0.0f, 2.0f * nearRange + zPad);
        glm::vec4 origin = lightProj * lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        origin *= res * 0.5f;
        const glm::vec2 off = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) * (2.0f / res);
        lightProj[3][0] += off.x;
        lightProj[3][1] += off.y;
        ubo.grassShadowViewProj = lightProj * lightView;
    }
    // w = one texel (m): the receivers' normal offset.
    ubo.grassParams13 = glm::vec4(0.0f, nearRange, glm::max(g.nearShadowBias, 0.0f), 2.0f * nearRange / res);
    // yz = the box's centre (XZ): the receivers' fade disc and the casters' selection measure from it.
    ubo.grassParams14 = glm::vec4(glm::clamp(g.nearShadowStrength, 0.0f, 1.0f), nearCentre.x, nearCentre.z,
        glm::clamp(g.bareFraction, 0.0f, 1.0f));
    ubo.grassParams12 = glm::vec4(1.0f / glm::max(g.fleckSize, 0.01f), glm::clamp(g.fleckContrast, 0.0f, 1.0f),
        glm::max(g.fleckFadeDistance, 1.0f), glm::max(g.fleckStretch, 0.0f));
    const float windAngle = glm::radians(g.windAngleDeg);
    ubo.grassParams4 = glm::vec4(std::cos(windAngle), std::sin(windAngle), g.windBend, g.gustBend);
    ubo.grassParams5 = glm::vec4(1.0f / glm::max(g.gustSize, 0.01f), g.gustSpeed, g.swayFrequency, m_grassPrevTime);
    m_grassPrevTime = ubo.timeSeconds;
    ubo.grassParams6 = glm::vec4(g.curvature, 1.0f / glm::max(g.clumpSize, 0.01f), glm::clamp(g.patchiness, 0.0f, 1.0f), glm::max(g.growBand, 0.01f));
    ubo.grassColor0 = glm::vec4(linear(g.rootColor), g.roughness);
    ubo.grassColor1 = glm::vec4(linear(g.tipColor), g.colorVariation);
    ubo.grassColor2 = glm::vec4(linear(g.dryColor), glm::clamp(g.dryAmount, 0.0f, 1.0f));
    ubo.grassShade = glm::vec4(g.rootOcclusion, g.transmission, g.roundness, g.groundBlend);
    ubo.grassParams9 = glm::vec4(g.windFadeStart, glm::max(g.windFadeEnd, g.windFadeStart + 0.01f), glm::clamp(g.sizeByCover, 0.0f, 1.0f),
        glm::clamp(g.lodMorphBand, 0.0f, 1.0f));
}

// View matrices, frustum, and TAA jitter: the center (culling) view, the VR eye views, and the
// screen/viewport constants. Sets m_centerViewProj + m_prevTaaJitter.
void Renderer::buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation)
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    // The jitter is one RENDER pixel (the scene renders through m_renderRect; == m_viewportRect unless upscaling).
    const glm::ivec2 renderSize = m_renderRect.getSize();

    glm::vec2 taaJitterNdc(0.0f);
    if (resolveActive() && renderSize.x > 0 && renderSize.y > 0)
    {
        // DLSS wants about 8 phases per output pixel: 8 x (output / render)^2 (TAA: 16).
        const float upscale = 1.0f / oc::max(m_renderScale.y, 0.01f);
        const uint32 phases = dlssActive() ? oc::clamp((uint32)glm::ceil(8.0f * upscale * upscale), 8u, 128u) : 16u;
        const uint32 sampleIdx = (m_frameCounter % phases) + 1u;
        taaJitterNdc.x = (radicalInverse(sampleIdx, 2u) - 0.5f) * 2.0f / (float)renderSize.x;
        taaJitterNdc.y = (radicalInverse(sampleIdx, 3u) - 0.5f) * 2.0f / (float)renderSize.y;
    }

    const uint32 numViews = Globals::openXR.isEnabled() ? RendererVKLayout::NUM_UBO_VIEWS : 1;
    for (uint32 v = 0; v < numViews; ++v)
    {
        ubo.views[v].prevMvp = ubo.views[v].mvp;
        ubo.views[v].prevInvMvp = ubo.views[v].invMvp;
    }

    RendererVKLayout::ViewData& centerView = ubo.views[RendererVKLayout::VIEW_CENTER];
    centerView.mvp = computeCenterViewProj(camera);
    // Invert in double precision: a float32 inverse of a perspective mvp is ill-conditioned and its
    // error grows with the camera translation, which shows up as per-frame reconstruction jitter
    // (sky ray, TAA reprojection, RTAO, fog) away from the world origin.
    const glm::dmat4 centerInvMvpD = glm::inverse(glm::dmat4(centerView.mvp));
    centerView.invMvp = glm::mat4(centerInvMvpD);
    // Fused clip->prev-clip reprojection: the double product cancels the (position-scaled) translations
    // exactly, leaving a near-identity matrix that survives float32 storage at any camera position.
    centerView.reprojClip = glm::mat4(glm::dmat4(centerView.prevMvp) * centerInvMvpD);
    centerView.viewPos = glm::vec4(camera.position, 1.0f);
    // ZO plane extraction (the projection is reversed-Z [0,1] clip; the near/far plane slots swap roles
    // under the reversal but the extracted volume is identical, so all cull consumers stay correct).
    ubo.frustum.fromMatrixZO(centerView.mvp);
    m_centerViewProj = centerView.mvp;

    if (Globals::openXR.isEnabled())
    {
        for (uint32 eye = 0; eye < 2; ++eye)
        {
            glm::mat4 eyeView;
            glm::vec3 eyePos;
            Globals::openXR.getEyeView(eye, cameraIn.position, vrBaseOrientation, eyeView, eyePos);
            const glm::mat4 eyeProj = Globals::openXR.getEyeProjection(eye, camera.near, camera.far);
            RendererVKLayout::ViewData& v = ubo.views[eye + 1]; // [1] = left eye, [2] = right eye
            v.mvp = eyeProj * eyeView;
            const glm::dmat4 eyeInvMvpD = glm::inverse(glm::dmat4(v.mvp));
            v.invMvp = glm::mat4(eyeInvMvpD);
            v.reprojClip = glm::mat4(glm::dmat4(v.prevMvp) * eyeInvMvpD);
            v.viewPos = glm::vec4(eyePos, 1.0f);
        }
    }

    // The render-size targets and the scene's rect in them: every scene / screen-space pass works in these.
    // Without upscaling they are the swapchain extent and the viewport rect.
    const glm::vec2 targetSize(m_renderExtent);
    ubo.screenSize = glm::vec4(targetSize, 1.0f / targetSize);
    ubo.viewportRect = glm::vec4(glm::vec2(m_renderRect.min) / targetSize, glm::vec2(renderSize) / targetSize);
    // zw = LAST frame's jitter: TAA/AO-temporal compensate both frames' jittered depth images during
    // reprojection (all raster passes jitter, the prepass included - see taaJitterUv in shared.inc.glsl).
    ubo.taaJitter = glm::vec4(taaJitterNdc, m_prevTaaJitter);
    m_prevTaaJitter = taaJitterNdc;
}

// Sky atmosphere, sun + eclipse, clouds, stars/moon/nebula, and ground params.
void Renderer::buildUboSky()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const SkyParams& sky = m_skyParams;

    // Earth sea-level scattering coefficients, scaled by the atmosphere tweaks.
    ubo.betaRayleigh = glm::vec3(5.802e-6f, 13.558e-6f, 33.1e-6f) * sky.rayleighScatter;
    ubo.rolloffKnee = sky.sunRolloffKnee;
    ubo.betaMie = 3.996e-6f * sky.mieScatter;
    ubo.sunDirection = sky.sunDirection;
    ubo.sunAngularCos = sky.sunAngularCos;
    // Solar eclipse: fold the moon-covered sun fraction into the UBO sun color so every sun consumer
    // (sky atmosphere, forward lighting, GI trace, volumetric fog, clouds) dims consistently.
    const float eclipseVisible = sunVisibleFraction(sky.sunDirection, glm::normalize(sky.moonDirection), sky.sunAngularCos, cosf(glm::radians(sky.moonSizeDeg)), sky.sunGlow);
    ubo.sunColor = sky.sunColor * sky.sunIntensity;
    ubo.eclipseParams = glm::vec4(eclipseVisible, sky.sunRolloffHeadroom, 0.0f, 0.0f);
    ubo.sunGlow = sky.sunGlow;

    ubo.skyRadianceColor = sky.skyRadianceColor * sky.skyRadianceIntensity;
    ubo.rtSkyRadiance = (m_rtParams.enabled && m_rtParams.rtSkyRadiance) ? 1.0f : 0.0f;
    ubo.ambientColor = sky.ambientColor * sky.ambientIntensity;
    ubo.skyUp = sky.up;

    ubo.moonBrightness = sky.moonBrightness;
    ubo.skySunParams = glm::vec4(sky.scatterBoost, sky.mieG, sky.sunRolloff, sky.starDensity);

    ubo.moonParams = glm::vec4(glm::normalize(sky.moonDirection), cosf(glm::radians(sky.moonSizeDeg)));
    ubo.starParams = glm::vec4(sky.starSize, sky.starSizeVar, sky.starBrightness, sky.starColorVar);
    ubo.nebulaParams = glm::vec4(sky.nebulaIntensity, sky.nebulaScale, sky.nebulaBandWidth, sky.nebulaDust);
    ubo.nebulaAxis = glm::vec4(glm::normalize(sky.nebulaAxis), 0.0f);
    ubo.atmosParams = glm::vec4(sky.rayleighHeight, sky.mieHeight, sky.mieExtinction, sky.ozone);

    // Sun transmittance at ground level: the CPU mirror of atmosphere.inc.glsl's
    // atmosTransmittanceToLight(0, sunDir, up) - Chapman optical depth (r = planet radius, h = 0),
    // atmosTau, exp. Constant per frame, so the lit fragment shaders read u_sunTransmittance instead of
    // evaluating it per pixel. KEEP IN SYNC with the GLSL constants (ATMOS_R_PLANET, ATMOS_BETA_OZONE).
    {
        constexpr float c_planetRadius = 6371e3f;
        const glm::vec3 c_betaOzone(0.650e-6f, 1.881e-6f, 0.085e-6f);
        const auto chapman = [](float X, float cosChi) // Schüler's grazing-incidence closed form
        {
            const float c = std::sqrt(1.5707963f * X);
            if (cosChi >= 0.0f)
                return c / ((c - 1.0f) * cosChi + 1.0f);
            const float sinChi = std::sqrt(glm::max(1.0f - cosChi * cosChi, 1e-6f));
            const float X0 = X * sinChi;
            return 2.0f * std::sqrt(1.5707963f * X0) * std::exp(glm::min(X - X0, 60.0f)) - c / ((c - 1.0f) * (-cosChi) + 1.0f);
        };
        const float cosChi = glm::dot(glm::normalize(sky.up), sky.sunDirection); // pos = up * R, so cos(chi) = up . L
        const float odR = sky.rayleighHeight * chapman(c_planetRadius / sky.rayleighHeight, cosChi);
        const float odM = sky.mieHeight * chapman(c_planetRadius / sky.mieHeight, cosChi);
        const glm::vec3 tau = ubo.betaRayleigh * odR + glm::vec3(ubo.betaMie * sky.mieExtinction) * odM + c_betaOzone * (sky.ozone * odR);
        ubo.sunTransmittance = glm::exp(-tau);
    }
    ubo.groundParams = glm::vec4(sky.groundColor * sky.groundIntensity, glm::clamp(sky.groundHorizon, 0.0f, 1.0f));
}

// Volumetric clouds (clouds.inc.glsl). The noise is world-anchored and tiles: base and detail repeat an
// integer number of times per weather tile, so ONE wrap of (camera + wind) by the weather period keeps
// every texture continuous and the shader's noise coordinates small at any camera position. The wind
// and the detail drift accumulate here in double on the SIM clock (they stop with the global pause).
void Renderer::buildUboClouds(const Camera& camera)
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const CloudParams& c = m_cloudParams;
    const bool enabled = cloudsEnabled();

    const double weatherPeriod = glm::max((double)c.weatherSizeKm, 1.0) * 1000.0;
    const int baseRepeats = glm::max(c.baseRepeats, 1);
    const int detailRepeats = glm::max(c.detailRepeats, 1);
    const double basePeriod = weatherPeriod / baseRepeats;
    const double detailPeriod = basePeriod / detailRepeats;

    const double dt = glm::min((double)Globals::time.getSimDeltaSec(), 0.25);
    const double windAngle = glm::radians((double)c.windAngleDeg);
    const glm::dvec2 windStep = glm::dvec2(std::cos(windAngle), std::sin(windAngle)) * ((double)c.windSpeed * dt);
    m_cloudWindOffset = glm::mod(m_cloudWindOffset + windStep, glm::dvec2(weatherPeriod));
    m_cloudEvolveOffset = std::fmod(m_cloudEvolveOffset + (double)c.evolveSpeed * dt, detailPeriod);
    // Noise space = world - wind, so the field travels WITH the wind.
    const glm::dvec2 origin = glm::mod(glm::dvec2(camera.position.x, camera.position.z) - m_cloudWindOffset, glm::dvec2(weatherPeriod));

    // The main layer's band, the upper layer's, and the SHELL = their union (what the march, the shadow map and
    // the height-in-shell lighting cover).
    const float mainBottom = glm::max(c.bottom, 0.0f);
    const float mainTop = glm::max(c.top, mainBottom + 100.0f);
    const float upperBottom = glm::max(c.upperBottom, 0.0f);
    const float upperTop = glm::max(c.upperTop, upperBottom + 50.0f);
    const bool upper = c.upperEnabled && c.upperDensity > 0.0f && c.upperCoverage > 0.0f;
    float bottom = mainBottom, top = mainTop;
    if (upper) { bottom = glm::min(bottom, upperBottom); top = glm::max(top, upperTop); }
    ubo.cloudShape0 = glm::vec4(bottom, top, glm::clamp(c.coverage, 0.0f, 1.0f), enabled ? 1.0f : 0.0f);
    ubo.cloudLayer0 = glm::vec4(mainBottom, 1.0f / (mainTop - mainBottom), upper ? 1.0f : 0.0f, c.upperDensity);
    // The upper layer's coverage is a multiplier on the main layer's (its density already is one: the shader adds
    // it x upperDensity to the main density before "Density (1/m)").
    ubo.cloudLayer1 = glm::vec4(upperBottom, 1.0f / (upperTop - upperBottom), glm::clamp(c.upperCoverage * c.coverage, 0.0f, 1.0f), glm::clamp(c.upperType, 0.0f, 1.0f));
    ubo.cloudLayer2 = glm::vec4((float)glm::clamp(c.shelfCount, 0, 3), glm::max(c.shelfStrength, 0.0f), glm::clamp(c.shelfThickness, 0.005f, 0.2f),
        glm::clamp(c.upperHeightVariation, 0.0f, 0.8f));
    // The shelf stack is CENTRED in the layer: the lowest at 0.5 - (count - 1) / 2 x spacing.
    const float shelfSpacing = glm::max(c.shelfSpacing, 0.0f);
    const float shelfLowest = 0.5f - 0.5f * (float)glm::max(glm::clamp(c.shelfCount, 0, 3) - 1, 0) * shelfSpacing;
    ubo.cloudLayer3 = glm::vec4(shelfLowest, shelfSpacing, glm::max(c.minStep, 0.25f), glm::max(c.aerialStrength, 0.0f));
    ubo.cloudShape1 = glm::vec4((float)(1.0 / weatherPeriod), (float)(1.0 / basePeriod), (float)(1.0 / detailPeriod), c.densityScale);
    ubo.cloudShape2 = glm::vec4(glm::clamp(c.cloudType, 0.0f, 1.0f), c.typeVariation, c.erosion, c.curl);
    ubo.cloudShape3 = glm::vec4(c.coverageVariation, c.nearDetailRadius, 1.0f / (top - bottom), 1.0f / glm::max(c.nearDetailRadius, 1e-3f));
    // The sky-map clouds' history weight per march of a texel: exp(-3 dt / T) reaches 95 % of a change in T seconds,
    // at any frame rate; a texel marches every SKY_UPDATE_FRAMES frames, so dt spans that many. Real time, not sim
    // time: the camera still moves while the sim is paused.
    const float realDt = glm::min((float)Globals::time.getDeltaSec(), 0.25f) * (float)CloudPipeline::SKY_UPDATE_FRAMES;
    const float skyHistory = c.skyMapHistorySec > 0.0f ? std::exp(-3.0f * realDt / c.skyMapHistorySec) : 0.0f;
    ubo.cloudShape4 = glm::vec4(glm::clamp(c.baseVariation, 0.0f, 0.6f), 1.0f / glm::max(c.groundLightDepth, 1.0f), skyHistory,
        glm::clamp(c.erosionCutoff, 0.0f, 0.9f));
    // Top roundness 0..1 -> the superellipse exponent 1..6 (1 = the plain taper, 2 = a circular cap, 6 = nearly flat).
    ubo.cloudShape5 = glm::vec4(glm::clamp(c.towerVariation, 0.0f, 0.9f), 1.0f + 5.0f * glm::clamp(c.topRoundness, 0.0f, 1.0f),
        glm::clamp(c.baseSharpness, 0.0f, 1.0f), glm::clamp(c.towerCoreLink, 0.0f, 1.0f));
    const float giSkyHistory = c.giSkyHistorySec > 0.0f ? std::exp(-3.0f * realDt / c.giSkyHistorySec) : 0.0f;
    ubo.cloudNoiseOrigin = glm::vec4((float)origin.x, giSkyHistory, (float)origin.y, (float)m_cloudEvolveOffset);
    ubo.cloudWind = glm::vec4((float)windStep.x, 0.0f, (float)windStep.y, glm::max(c.giSkyObserverRadius, 0.0f)); // the field's world displacement this frame
    // The HG + Draine fit to Mie scattering on water droplets (Jendersie & d'Eon 2023, "An Approximate Mie
    // Scattering Function for Fog and Cloud Rendering"), valid for diameters 5 .. 50 um.
    // The HG part's g is the droplets' DIFFRACTION peak (0.995 at 20 um: ~5400 / sr in a ~0.3 degree lobe, the size
    // of the sun disc). Behind thin cloud it scattered so much sunlight into that lobe that the lobe clipped to
    // white after the exposure - a bigger, brighter sun. "Forward peak limit" caps it: a wider, softer silver lining.
    {
        const float d = glm::clamp(c.dropletSize, 5.0f, 50.0f);
        const float gHG = glm::min(std::exp(-0.0990567f / (d - 1.67154f)), glm::clamp(c.forwardPeakLimit, 0.0f, 1.0f));
        const float gD = std::exp(-2.20679f / (d + 3.91029f) - 0.428934f);
        const float alpha = std::exp(3.62489f - 8.29288f / (d + 5.52825f));
        const float wD = std::exp(-0.599085f / (d - 0.641583f) - 0.665888f);
        ubo.cloudLight0 = glm::vec4(gHG, gD, alpha, wD);
    }
    ubo.cloudLight1 = glm::vec4(c.ambient, c.groundAlbedo, c.powder, c.multiScatter);
    // The ground bounce's albedo: the sky's "Ground Albedo" COLOUR (its hue, not its intensity - that one scales the
    // sky-sphere ground plane and defaults to 0) x the cloud "Ground albedo".
    ubo.cloudLight2 = glm::vec4(m_skyParams.groundColor * c.groundAlbedo, glm::max(c.multiScatterStrength, 0.0f));
    ubo.cloudMarch0 = glm::vec4((float)glm::max(c.maxSteps, 1), c.maxDistanceKm * 1000.0f, glm::max(c.nearStep, 0.5f), (float)glm::max(c.stepsPerRay, 1));
    ubo.cloudMarch1 = glm::vec4((float)glm::max(c.lightSteps, 0), c.lightDistance, glm::clamp(c.temporalBlend, 0.0f, 0.98f),
        1.0f / (glm::max(c.detailDistanceKm, 0.5f) * 1000.0f));

    // THE CLOUD SHADOW MAP: two sun-aligned ortho cascades around the camera, their centres snapped to whole
    // texels in light space (in double, world space) so the map does not swim when the camera moves.
    // PROGRESSIVE UPDATES in ONE buffer: each frame a cascade renders 1 / split of its texels, interleaved
    // (a rotating phase of a 2x2 or 4x4 pattern), so its cost is the same every frame; neighbouring texels
    // then differ in age by a few frames, well under a texel of cloud motion. That needs ONE texel-to-world
    // mapping for all of them, so the centre is FROZEN and re-centres only when the camera is 1/8 of the
    // extent away from it (or the sun / the extent changed, or the map was off) - that frame renders the
    // whole cascade, a rare spike. The shaders' lookup is unchanged: only the camera-relative offset of the
    // frozen centre is rebuilt each frame.
    const glm::dvec3 sun = glm::normalize(glm::dvec3(m_skyParams.sunDirection));
    const bool shadowOn = enabled && c.shadows && sun.y > 0.01;
    m_cloudShadowMask = 0;
    if (!shadowOn)
    {
        m_cloudShadowValid = {};
        ubo.cloudShadow4 = glm::vec4(0.0f);
        return;
    }
    const glm::dvec3 e0 = glm::abs(sun.y) < 0.999 ? glm::normalize(glm::cross(glm::dvec3(0.0, 1.0, 0.0), sun)) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 e1 = glm::cross(sun, e0);
    const glm::dvec3 camPos(camera.position);
    const double extents[2] = { glm::max((double)c.shadowNearKm, 0.1) * 1000.0, glm::max((double)c.shadowFarKm, 0.2) * 1000.0 };
    const int splits[2] = { glm::clamp(c.shadowNearSplit, 0, 3), glm::clamp(c.shadowFarSplit, 0, 3) };
    const bool sunChanged = sun != m_cloudShadowSun;
    m_cloudShadowSun = sun;
    for (uint32 cascade = 0; cascade < CloudPipeline::SHADOW_CASCADES; ++cascade)
    {
        const double extent = extents[cascade];
        const double texel = extent / CloudPipeline::SHADOW_RESOLUTION;
        const glm::dvec2 camLight(glm::dot(camPos, e0), glm::dot(camPos, e1));
        const glm::dvec2 centreLight(glm::dot(m_cloudShadowCenter[cascade], e0), glm::dot(m_cloudShadowCenter[cascade], e1));
        const glm::dvec2 offset = glm::abs(camLight - centreLight);
        const bool recentre = !m_cloudShadowValid[cascade] || sunChanged || extent != m_cloudShadowExtent[cascade]
            || glm::max(offset.x, offset.y) > extent * 0.125;
        if (recentre)
        {
            const glm::dvec2 snapped = glm::floor(camLight / texel + 0.5) * texel;
            m_cloudShadowCenter[cascade] = e0 * snapped.x + e1 * snapped.y + sun * glm::dot(camPos, sun);
            m_cloudShadowExtent[cascade] = extent;
            m_cloudShadowValid[cascade] = true;
            m_cloudShadowSplit[cascade] = 0; // this frame: every texel
            m_cloudShadowPhase[cascade] = 0;
        }
        else
        {
            m_cloudShadowSplit[cascade] = (uint32)splits[cascade];
            const uint32 phases = 1u << (2 * splits[cascade]); // 1, 4, 16 or 64
            m_cloudShadowPhase[cascade] = (m_cloudShadowPhase[cascade] + 1) % phases;
        }
        m_cloudShadowMask |= 1u << cascade;
    }
    const glm::vec3 rel0(m_cloudShadowCenter[0] - camPos);
    const glm::vec3 rel1(m_cloudShadowCenter[1] - camPos);
    ubo.cloudShadow0 = glm::vec4(rel0, (float)(1.0 / m_cloudShadowExtent[0]));
    ubo.cloudShadow1 = glm::vec4(rel1, (float)(1.0 / m_cloudShadowExtent[1]));
    ubo.cloudShadow2 = glm::vec4(glm::vec3(e0), glm::clamp(c.shadowStrength, 0.0f, 1.0f));
    // Past the cascades: a rough mean transmittance of the layer from its coverage (not measured).
    ubo.cloudShadow3 = glm::vec4(glm::vec3(e1), glm::mix(1.0f, 0.3f, glm::clamp(c.coverage, 0.0f, 1.0f)));
    ubo.cloudShadow4 = glm::vec4(1.0f, (float)glm::max(c.shadowNearSteps, 1), (float)glm::max(c.shadowFarSteps, 1), glm::max(c.shadowFarSoftness, 0.0f));
}

// Sun shadow route: the RT-sun toggle, else the PCSS cascade matrices (also consumed CPU-side via
// getSunCascadeViewProj), plus the long-range terrain shadow march params.
void Renderer::buildUboSunShadow(const Camera& camera)
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    ubo.rtSunShadow = (m_rtParams.enabled && m_rtParams.rtSunShadow) ? 1.0f : 0.0f;
    // The shaders' cascade pick and the RTAO falloff measure from here; it matches computeSunCascades'
    // origin below.
    ubo.sceneFocus = glm::vec4(m_sceneFocusEnabled ? m_sceneFocus : camera.position, 0.0f);

    // Use the effective flag: with RT off (or RT-sun off) the PCSS cascades supply the sun shadow.
    if (ubo.rtSunShadow < 0.5f)
    {
        const glm::ivec2 viewportSize = m_viewportRect.getSize();
        const float aspect = (float)viewportSize.x / (float)viewportSize.y;
        computeSunCascades(camera, aspect, m_skyParams.sunDirection, m_sceneFocusEnabled ? &m_sceneFocus : nullptr,
            m_shadowParams.maxDistance, m_shadowParams.splitLambda, m_shadowParams.casterPad, m_sunCascadeViewProj);
        m_numSunCascades = RendererVKLayout::NUM_SHADOW_CASCADES;
        // PCSS penumbra scale per cascade (the shader's former pcssSunSizeTexels): the physical penumbra
        // over a world gap g spans 2 g tan(sunRadius); a PCF disk of radius r ramps over ~2r, so the
        // radius is g tan(sunRadius), and gapNorm * depthRange / texelWorldSize converts it to texels.
        // depthRange and texelWorldSize ride in the matrices' bottom rows (computeSunCascades).
        static_assert(RendererVKLayout::NUM_SHADOW_CASCADES == 4, "cascadeSunSizeTexels is one vec4");
        const float cosT = glm::clamp(m_skyParams.sunAngularCos, 0.5f, 0.9999999f);
        const float tanT = std::sqrt(1.0f - cosT * cosT) / cosT;
        for (uint32 c = 0; c < RendererVKLayout::NUM_SHADOW_CASCADES; ++c)
        {
            ubo.cascadeViewProj[c] = m_sunCascadeViewProj[c];
            const float texelWorldSize = m_sunCascadeViewProj[c][1][3];
            const float depthRange = m_sunCascadeViewProj[c][2][3];
            ubo.cascadeSunSizeTexels[c] = tanT * depthRange / glm::max(texelWorldSize, 1e-6f);
        }
        ubo.shadowParams = glm::vec3(m_shadowParams.depthBias, m_shadowParams.normalBias, 1.0f / (float)RendererVKLayout::SHADOW_MAP_RESOLUTION);
    }
    else
        m_numSunCascades = 0;

    ubo.sunShadowRays = (float)m_rtParams.sunShadowRays;
    ubo.rtLightShadows = (m_rtParams.enabled && m_rtParams.rtLightShadows) ? 1.0f : 0.0f;
    ubo.terrainShadowParams = glm::vec4(glm::max(m_shadowParams.terrainMarchStart, 0.0f),
        glm::max(m_shadowParams.terrainMarchBias, 1.0f), glm::max(m_shadowParams.terrainMarchSpread, 0.002f), 0.0f);
}

// The weather volume's rain occlusion map: a top-down orthographic view over the latched volume box
// (setRainOcclusionVolume), looking straight down -Y. The eye sits casterPad above the box top so a roof
// well above the volume still shelters it; the XZ footprint is padded 25 % over the box (one-frame lag
// behind the camera-following emitter). Standard Z, plain bottom row: the rain cull, the rain depth
// pass and the particle sim's shelter test all read it verbatim.
void Renderer::buildUboRainOcclusion()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const ParticleState::RainVolume& v = m_particles.getRainVolume();
    const float anisotropy = glm::clamp(m_particles.getParams().anisotropy, -0.95f, 0.95f); // rides the params' free w
    if (!v.active || !m_particles.getParams().rainOcclusion)
    {
        ubo.rainOcclusionViewProj = glm::mat4(1.0f);
        ubo.rainOcclusionParams = glm::vec4(0.0f, 0.0f, 0.0f, anisotropy);
        return;
    }
    const float hx = glm::max(v.halfExtents.x, 1.0f) * 1.25f;
    const float hz = glm::max(v.halfExtents.z, 1.0f) * 1.25f;
    const float pad = glm::max(m_particles.getParams().rainOcclusionCasterPad, 1.0f);
    const float range = 2.0f * glm::max(v.halfExtents.y, 1.0f) + pad + 1.0f; // 1 m below the box bottom
    const glm::vec3 eye = v.center + glm::vec3(0.0f, v.halfExtents.y + pad, 0.0f);
    const glm::mat4 view = glm::lookAtRH(eye, v.center, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 proj = glm::orthoRH_ZO(-hx, hx, -hz, hz, 0.0f, range);
    ubo.rainOcclusionViewProj = proj * view;
    ubo.rainOcclusionParams = glm::vec4(1.0f, 1.0f / range, glm::max(m_particles.getParams().rainOcclusionTolerance, 0.0f), anisotropy);
}

// Volumetric fog params + the fog terrain height cascades (also the ocean's shore-map fallback).
void Renderer::buildUboFog()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const FogParams& fog = m_fogParams;

    // A freshly uploaded fog terrain height map activates here, in the same frame slot as the UBO that
    // carries its world center/sizes - descriptors (refreshed per frame in recordCommandBuffers) and
    // params stay coherent. Presence (fogParams3.y) is independent of Terrain Follow: the ocean also
    // reads these cascades as its shore-map fallback.
    m_terrain.getHeightMap().flipIfPending();
    const glm::vec2 fogTerrainSizes = m_terrain.getHeightMap().getWorldSizes();
    ubo.fogParams0 = glm::vec4(fog.density, fog.heightBase, fog.heightFalloff * fog.heightFalloff, fog.range);
    ubo.fogParams1 = glm::vec4(fog.albedo * fog.albedoIntensity, fog.anisotropy);
    ubo.fogParams2 = glm::vec4(fog.noiseScale, fog.noiseStrength, fog.windSpeed, fog.temporalBlend);
    ubo.fogParams3 = glm::vec4(glm::clamp(fog.terrainFollow, 0.0f, 1.0f),
        fogTerrainSizes.x > 1.0f ? 1.0f / fogTerrainSizes.x : 0.0f,
        fog.enabled ? 1.0f : 0.0f, fog.lightShadows ? 1.0f : 0.0f);
    ubo.fogParams4 = glm::vec4((float)fog.sunRays, fog.spatialFilter ? 1.0f : 0.0f, fog.giAmbient ? 1.0f : 0.0f, fog.sunSoftness);
    ubo.fogParams5 = glm::vec4(m_terrain.getHeightMap().getCenter(),
        fogTerrainSizes.y > 1.0f ? 1.0f / fogTerrainSizes.y : 0.0f, m_terrain.getHeightMap().getUserParam());
    ubo.fogParams6 = glm::vec4(glm::clamp(fog.slicePower, 0.25f, 2.0f), fog.terrainShadowDist,
        glm::clamp(fog.regionStrength, 0.0f, 1.0f), // z: baked regional fog-thickness modulation
        glm::max(fog.underwaterDensity, 0.0f));     // w: density multiplier below the local water surface
    // x: underwater fog boundary margin (m) relative to the LIVE wave surface (the fog scatter samples
    // the FFT displacement maps directly). y: the waterline band half-height gating those samples -
    // froxel segments outside +-band of the calm level are trivially above/below any possible wave, so
    // only a thin shell pays for wave taps. The CPU trough estimate (ocean readback) bounds the wave
    // amplitude; 0 disables wave sampling entirely (ocean off).
    // The band must also cover the swash RUN-UP (amplitude x (trough + 0.25), the same reach
    // buildUboOcean packs into oceanParams7.w): with a large swash amplitude the tongue climbs past
    // 2 x trough, and froxels beyond the band got the calm level while their neighbours got the wave -
    // a line in the fog's caustics that moved with the amplitude.
    const float waveTrough = m_oceanSimPipeline.getWaveTrough();
    const float swashReachBand = glm::clamp(m_oceanSimPipeline.getOceanParams().swashAmp, 0.0f, 4.0f) * (waveTrough + 0.25f);
    const float waveBand = m_oceanSimPipeline.isOceanEnabled() ? glm::max(waveTrough * 2.0f + 0.5f, swashReachBand) : 0.0f;
    // The underwater metres are measured against the sea, so they ride "Ocean/World scale" like the
    // ocean's own tweaks: lengths x s, the per-metre depth fade / s (the same water column in fewer metres).
    const float oceanScale = getOceanWorldScale();
    ubo.fogParams7 = glm::vec4(
        glm::max(fog.shaftBoost, 0.0f),        // x: underwater sun in-scatter gain (fog light shafts)
        waveBand,
        glm::max(fog.causticStrength, 0.0f),   // z: underwater caustic focus strength (surfaces + fog shafts)
        glm::max(fog.causticDepthFade, 0.0f) / oceanScale); // w: caustic contrast decay with depth (1/m)
    // x: the boundary offset - "Underwater wave offset" x the deepest live wave trough, down (world metres
    // already; 0 with the ocean off): a higher sea needs a lower boundary, or the murk peeks through the
    // troughs the fog's coarse froxels miss.
    ubo.fogParams8 = glm::vec4(-glm::max(fog.underwaterWaveOffset, 0.0f) * m_oceanSimPipeline.getWaveTrough(),
        glm::max(fog.causticShoreFade, 0.0f) * oceanScale,
        fog.farFieldMaxDistanceKm > 0.0f ? fog.farFieldMaxDistanceKm * 1000.0f : 1e30f, glm::max(fog.sunScatter, 0.0f));
    // z: thickness scale inverted into a falloff multiplier on fogParams0.z. w: far-field ground samples.
    ubo.fogParams9 = glm::vec4(fog.farField ? 1.0f : 0.0f, glm::max(fog.farFieldDensity, 0.0f),
        1.0f / glm::clamp(fog.farFieldThickness, 0.01f, 100.0f), (float)glm::max(fog.farFieldSteps, 1));
    ubo.fogParams10 = glm::vec4(glm::max(fog.shaftHazeDensity, 0.0f), 1.0f / glm::max(fog.shaftHazeHeight, 1.0f), 0.0f, 0.0f);
}

// FFT ocean simulation + shading params.
void Renderer::buildUboOcean()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const OceanParams& ocean = m_oceanSimPipeline.getOceanParams();
    const glm::vec2 windDir = glm::length(ocean.windDirection) > 1e-4f ? glm::normalize(ocean.windDirection) : glm::vec2(1.0f, 0.0f);
    ubo.oceanParams0 = glm::vec4(windDir, ocean.amplitude, ocean.choppiness);
    // Wind clamped just above 0: the JONSWAP 1/U terms must stay finite; the spectrum's wave-age limit
    // (ocean_spectrum.cs.glsl) makes this effectively glassy anyway.
    ubo.oceanParams1 = glm::vec4(glm::max(ocean.windSpeed, 0.01f), glm::max(ocean.fetchKm, 1.0f) * 1000.0f, glm::max(ocean.depth, 1.0f), ocean.normalStrength);
    ubo.oceanParams2 = glm::vec4(glm::max(ocean.cascadeSizes, glm::vec3(1.0f)), ocean.seaLevel);
    ubo.oceanAbsorption = glm::vec4(ocean.absorption, ocean.roughness);
    ubo.oceanScatter = glm::vec4(ocean.scatterColor, ocean.scatterStrength);
    ubo.oceanFoam = glm::vec4(ocean.foamColor, ocean.foamBias);
    ubo.oceanParams3 = glm::vec4(ocean.horizonLevelOffset, glm::max(ocean.horizonDepth, 0.0f),
        0.0f, glm::clamp(ocean.detailBias, -4.0f, 4.0f));
    ubo.oceanParams4 = glm::vec4(glm::max(ocean.horizonDepthRange, 0.0f),
        0.0f, glm::max(ocean.shoalScale, 0.0f), glm::max(ocean.foamSoftness, 0.02f));
    ubo.oceanParams5 = glm::vec4(0.0f, 0.0f, glm::max(ocean.shoreFoamDepth, 0.0f), glm::max(ocean.foamBreakAccel, 0.01f));
    ubo.oceanParams6 = glm::vec4(glm::max(ocean.farCullError, 0.0f), glm::max(ocean.glintFilter, 0.0f),
        glm::max(ocean.sssStrength, 0.0f), glm::max(ocean.sssPower, 1.0f));
    // Swash reach: conservative max run-up height from the wave-amplitude readback (trough estimate ~
    // crest scale) - sizes the on-land sampling band and keeps the vertex cull off the wet beach.
    const float swashAmp = glm::clamp(ocean.swashAmp, 0.0f, 4.0f);
    const float swashReach = swashAmp * (m_oceanSimPipeline.getWaveTrough() + 0.25f);
    ubo.oceanParams7 = glm::vec4(glm::max(ocean.cullMargin, 0.0f), glm::clamp(ocean.shoreFoamMax, 0.0f, 1.0f), swashAmp, swashReach);
    ubo.oceanParams8 = glm::vec4(glm::max(ocean.rtReflectionFog, 0.0f),glm::clamp(ocean.shoreFoamBias, -1.0f, 1.0f),
        glm::max(ocean.swashFlow, 0.0f), glm::max(ocean.rtRayCutoffDist, 0.0f));
    ubo.oceanParams9 = glm::vec4(glm::max(ocean.microRoughness, 0.0f), glm::max(ocean.rtRefractionRange, 1.0f), // the tweak's own minimum; 10 here silently floored 1..9 m
        glm::max(ocean.rtReflectionRange, 50.0f), glm::clamp(ocean.rtReflectionMaxRough, 0.0f, 1.0f));
    ubo.oceanParams10 = glm::vec4(glm::max(ocean.crestSlopeLimit, 0.0f), glm::max(ocean.timeScale, 0.0f),
        glm::clamp(ocean.undersideTransmission, 0.0f, 1.0f),
        ocean.enabled ? m_oceanSimPipeline.getDisplacementExtent() : 0.0f); // w: per-instance cull padding
    ubo.oceanParams11 = glm::vec4(glm::max(ocean.detailStrength, 0.0f), glm::max(ocean.detailScale, 0.001f),
        glm::max(ocean.detailFadeDist, 0.0f), ocean.detailRotation);
    // The bubble cloud's per-frame factors (ocean_bubbles.inc.glsl oceanBubbleRadianceFrame): the sun's and the
    // sky's path down to "Bubble depth" and the cloud's albedo depend on nothing per pixel, so the ocean pays one
    // exp (the path back up to the eye) per pixel, not three. The film's depth is per pixel: it keeps the full form.
    {
        const glm::vec3 L = glm::normalize(ubo.sunDirection); // buildUboSky ran first
        const float sunCos = glm::max(L.y, 0.0f);
        const float muL = glm::sqrt(1.0f - (1.0f - sunCos * sunCos) / (1.33f * 1.33f)); // refracted sun cosine
        const float depth = glm::max(ocean.bubbleDepth, 0.0f);
        const glm::vec3 albedo = ocean.foamColor * glm::max(ocean.bubbleBrightness, 0.0f);
        ubo.oceanBubble0 = glm::vec4(albedo * glm::exp(-ocean.absorption * (depth / muL)) * (sunCos / glm::pi<float>()), 0.0f);
        ubo.oceanBubble1 = glm::vec4(albedo * glm::exp(-ocean.absorption * depth), 0.0f);
    }
    ubo.oceanParams12 = glm::vec4(glm::max(ocean.bubbleDepth, 0.0f), glm::max(ocean.bubbleBrightness, 0.0f),
        glm::clamp(ocean.foamFlatten, 0.0f, 1.0f), ocean.cameraUnderwater ? 1.0f : 0.0f);
    // Ocean spray producer: the emitter slot the Particle system published (UINT32_MAX = off), the sim
    // delta the rate integrates over (frozen with the global pause, like the particle sim itself).
    const float sprayDt = oc::min((float)Globals::time.getSimDeltaSec(), 0.25f);
    // Off while the particle chain is disabled: nothing would consume (and reset) the request counter.
    const uint32 sprayEmitter = m_particles.isEnabled() ? m_oceanSimPipeline.getSprayEmitter() : UINT32_MAX;
    // The "Spray *" tweaks are MODEL units like every ocean tweak: metres and m/s ride the world scale
    // (the sea keeps its model periods, so speed scales as length), the per-m^2 rate rides 1/s^2 so the
    // spawns per model area stay the same.
    const OceanSprayParams& spray = m_oceanSimPipeline.getSprayParams();
    const float sprayScale = getOceanWorldScale();
    ubo.oceanSpray0 = glm::vec4(glm::uintBitsToFloat(sprayEmitter), glm::max(spray.rate, 0.0f) / (sprayScale * sprayScale),
        glm::max(spray.radius * sprayScale, 1.0f), sprayDt);
    ubo.oceanSpray1 = glm::vec4(glm::clamp(spray.threshold, 0.0f, 0.99f), glm::max(spray.kick, 0.0f) * sprayScale,
        glm::max(spray.speed, 0.0f) * sprayScale, spray.forward * sprayScale);
    ubo.oceanSpray2 = glm::vec4(spray.height * sprayScale, sprayScale, 0.0f, 0.0f);
    // The world-space foam field: drift over the SIM delta (frozen with the pause, like the waves), levels
    // around the camera (the water nearest the eye is where the detail shows).
    m_oceanSimPipeline.advanceFoamField(ubo, m_cameraPos, sprayDt);
}

// Forcefield bubbles (the Force library pushes the params every frame; all UBO-driven = live).
void Renderer::buildUboForce()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const ForceFieldParams& force = m_force.getParams();
    for (uint32 i = 0; i < RendererVKLayout::MAX_FORCE_TEAMS; ++i)
        ubo.forceTeamColors[i] = glm::vec4(force.teamColors[i], 0.0f);
    ubo.forceParams0 = glm::vec4(glm::max(force.isoThreshold, 1e-3f), glm::max(force.rimPower, 0.1f),
        glm::max(force.rimIntensity, 0.0f), glm::clamp(force.shellAlpha, 0.0f, 1.0f));
    ubo.forceParams1 = glm::vec4(glm::max(force.contactGlowIntensity, 0.0f), glm::max(force.contactGlowWidth, 1e-3f),
        glm::max(force.geoGlowDistance, 0.0f), (float)glm::clamp(force.marchSteps, 8, 256));
    ubo.forceParams2 = glm::vec4(glm::max(force.patternScale, 0.0f), force.patternSpeed,
        glm::max(force.patternIntensity, 0.0f),
        // w: the shell march's LOD scale - (px per radius/dist) / full-detail radius, so the FS's
        // steps taper as side/dist * this (clamped <= 1); 0 disables the taper.
        m_mipPixelScale * 0.5f / glm::max(force.shellFullResPixels, 1.0f));
    ubo.forceParams3 = glm::vec4(glm::clamp(force.interiorAlpha, 0.0f, 1.0f),
        glm::clamp(force.backfaceAlpha, 0.0f, 1.0f), glm::clamp(force.contactWallAlpha, 0.0f, 1.0f),
        glm::clamp(force.junctionSmoothing, 0.0f, 2.0f));
    ubo.forceParams4 = glm::vec4(0.0f /* x unused: the density view is the FORCE_DENSITY_VIEW define */, glm::max(force.densityRange, 1e-3f), 0.0f, 0.0f);

    // SAMPLED SHELL TIER: fit the bake volume over the union of the LARGE drawable emitters'
    // support boxes (+ margin) - the FIXED texel grid's resolution then self-adjusts to the active
    // spread. No qualifying emitter (or tier off) = no bake dispatch and the FS branch stays cold.
    glm::vec3 bakeLo(FLT_MAX), bakeHi(-FLT_MAX);
    if (force.sampledShellRadius > 0.0f)
        for (const RendererVKLayout::ForceEmitterGpu& e : m_force.getEmitters())
        {
            if ((e.teamFlags.y & RendererVKLayout::FORCE_FLAG_ACTIVE) == 0u
                || (e.teamFlags.y & RendererVKLayout::FORCE_FLAG_PASSIVE) != 0u
                || e.outputParams.y <= 0.0f
                || RendererVKLayout::forceEmitterVisibleRadius(e) < force.sampledShellRadius)
                continue;
            // The proxy's support AABB (the forceEmitterBounds rule): the output line +- side.
            const float R = e.posReach.w;
            const float m = glm::abs(1.0f - 2.0f * e.dirFocus.w);
            const float side = 0.5f * R * (1.0f + m) * e.outputParams.w * 1.03f;
            const glm::vec3 a = glm::vec3(e.posReach);
            const glm::vec3 b = a + glm::vec3(e.dirFocus) * R;
            bakeLo = glm::min(bakeLo, glm::min(a, b) - side);
            bakeHi = glm::max(bakeHi, glm::max(a, b) + side);
        }
    // VIEW FOOTPRINT CLIP (XZ): the volume only has to cover what is on screen. The four corner
    // rays hit the union's height band at eight points; their XZ box (+ "Volume view margin") clips
    // the fit, so the fixed texel grid follows the zoom instead of stretching over every large
    // bubble in the world - a 7 m bubble 100 m off-screen no longer halves a 40 m shell's
    // resolution. Outside the clipped fit the volume reads border black, but that boundary lies
    // outside the view by construction. A ray that misses the band (camera looking up: the
    // free-fly editor camera) leaves the union unclipped.
    if (bakeLo.x <= bakeHi.x && force.shellVolumeViewMargin > 0.0f && !isVrEnabled())
    {
        const glm::mat4 invViewProj = glm::inverse(getCenterViewProj());
        glm::vec2 footLo(FLT_MAX), footHi(-FLT_MAX);
        bool valid = true;
        for (int c = 0; c < 4 && valid; ++c)
        {
            const glm::vec2 ndc((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f);
            glm::vec4 p0 = invViewProj * glm::vec4(ndc, 0.0f, 1.0f);
            glm::vec4 p1 = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
            p0 /= p0.w;
            p1 /= p1.w;
            // Reversed-Z or not, the point FARTHER from the camera is on the far side of the ray.
            const glm::vec3 d0 = glm::vec3(p0) - m_cameraPos, d1 = glm::vec3(p1) - m_cameraPos;
            const glm::vec3 dir = glm::dot(d1, d1) > glm::dot(d0, d0) ? d1 : d0;
            for (const float yPlane : { bakeLo.y, bakeHi.y })
            {
                const float t = glm::abs(dir.y) > 1e-6f ? (yPlane - m_cameraPos.y) / dir.y : -1.0f;
                if (t < 0.0f)
                {
                    valid = false; // the corner ray never reaches the band
                    break;
                }
                const glm::vec3 hit = m_cameraPos + dir * t;
                footLo = glm::min(footLo, glm::vec2(hit.x, hit.z));
                footHi = glm::max(footHi, glm::vec2(hit.x, hit.z));
            }
        }
        if (valid)
        {
            footLo -= force.shellVolumeViewMargin;
            footHi += force.shellVolumeViewMargin;
            bakeLo.x = glm::max(bakeLo.x, footLo.x);
            bakeLo.z = glm::max(bakeLo.z, footLo.y);
            bakeHi.x = glm::min(bakeHi.x, footHi.x);
            bakeHi.z = glm::min(bakeHi.z, footHi.y);
            if (bakeLo.x >= bakeHi.x || bakeLo.z >= bakeHi.z)
                bakeLo = glm::vec3(FLT_MAX); // every large bubble is off-screen: no bake this frame
        }
    }
    m_force.setShellBakeActive(bakeLo.x <= bakeHi.x);
    if (m_force.isShellBakeActive())
    {
        const glm::vec3 margin = (bakeHi - bakeLo) * 0.02f + 1.0f; // ~2 filter texels of slack
        bakeLo -= margin;
        bakeHi += margin;
        ubo.forceBake0 = glm::vec4(bakeLo, force.sampledShellRadius); // w = the VISIBLE-radius tier threshold
        ubo.forceBake1 = glm::vec4(1.0f / glm::max(bakeHi - bakeLo, glm::vec3(1e-3f)), 1.0f);
    }
    else
    {
        ubo.forceBake0 = glm::vec4(0.0f, 0.0f, 0.0f, FLT_MAX); // no emitter reaches the threshold
        ubo.forceBake1 = glm::vec4(0.0f);
    }
    // CAMERA-INSIDE bit (forceBake2.w): the marches' "camera inside a bubble" test is a property of
    // ONE point per frame, so evaluate the full field at the camera here (CPU mirror) instead of a
    // per-fragment field re-sample at the ray origin in both fragment shaders.
    float phiCam[RendererVKLayout::MAX_FORCE_TEAMS] = {};
    for (const RendererVKLayout::ForceEmitterGpu& e : m_force.getEmitters())
    {
        if ((e.teamFlags.y & RendererVKLayout::FORCE_FLAG_ACTIVE) == 0u
            || (e.teamFlags.y & RendererVKLayout::FORCE_FLAG_PASSIVE) != 0u)
            continue;
        phiCam[glm::min(e.teamFlags.x, RendererVKLayout::MAX_FORCE_TEAMS - 1u)] += RendererVKLayout::forceContributionCpu(m_cameraPos, e);
    }
    float bestCam = 0.0f, secondCam = 0.0f;
    for (float p : phiCam)
    {
        if (p > bestCam) { secondCam = bestCam; bestCam = p; }
        else secondCam = glm::max(secondCam, p);
    }
    const bool cameraInside = bestCam - glm::max(glm::max(force.isoThreshold, 1e-3f), secondCam) > 0.0f;

    ubo.forceBake2 = glm::vec4(glm::max(force.unionStepSize, 0.05f),
        (float)glm::clamp(force.unionMaxSteps, 8, 512),
        m_mipPixelScale * 0.5f,                // z: px per (radius/dist) - the union march's distance LOD
        cameraInside ? 1.0f : 0.0f);           // w: the camera-inside bit
}

// Terrain rendering + splat texture params; also reports the splat textures to the mip streamer.
void Renderer::buildUboTerrain()
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    ubo.terrainParams = m_terrain.getParams();

    const TerrainTexTweaks& tex = m_terrain.getTexTweaks();
    // Every start/full pair is ordered here rather than in the shader: an inverted pair from the tweak
    // UI would otherwise make smoothstep divide by a negative span and flip the layer inside out.
    ubo.terrainTexParams0 = glm::vec4(m_terrain.getSplatBaseMaterial() < 0 ? -1.0f : (float)m_terrain.getSplatBaseMaterial(),
        (float)m_terrain.getSplatCounts().numGround, (float)m_terrain.getSplatCounts().numRock, glm::max(tex.climateBlend, 1e-3f));
    ubo.terrainTexParams1 = glm::vec4(tex.uvScaleGround, tex.uvScaleRock,
        tex.slopeRockStart, glm::max(tex.slopeRockFull, tex.slopeRockStart + 1e-3f));
    ubo.terrainTexParams2 = glm::vec4(tex.cragStart, glm::max(tex.cragFull, tex.cragStart + 1e-3f),
        tex.beachBand, tex.uvScaleSnow);
    ubo.terrainTexParams3 = glm::vec4(m_terrain.getSplatCounts().hasBeach ? 1.0f : 0.0f, m_terrain.getSplatCounts().hasSnow ? 1.0f : 0.0f,
        tex.snowTempFull, glm::max(tex.snowTempNone, tex.snowTempFull + 1e-3f));
    ubo.terrainTexParams4 = glm::vec4(tex.snowSlopeStart, glm::max(tex.snowSlopeFull, tex.snowSlopeStart + 1e-3f),
        tex.snowAridity, 0.0f);
    ubo.terrainTexParams5 = glm::vec4(glm::max(tex.cragWanderAmp, 0.0f),
        1.0f / glm::max(tex.cragWanderWavelength, 1.0f), 0.0f, 0.0f);
    const float parallaxFadeEnd = tex.parallaxFadeEnd > 0.0f ? glm::max(tex.parallaxFadeEnd, tex.parallaxFadeStart + 0.1f) : 0.0f;
    ubo.terrainTexParams6 = glm::vec4(glm::max(tex.parallaxDepthGround, 0.0f), glm::max(tex.parallaxDepthRock, 0.0f),
        glm::max(tex.parallaxFadeStart, 0.0f), glm::max(tex.heightBlendContrast, 0.0f));
    ubo.terrainTexParams7 = glm::vec4(parallaxFadeEnd, glm::clamp(tex.parallaxSteps, 2.0f, 64.0f),
        glm::clamp(tex.parallaxShadow, 0.0f, 1.0f), 0.0f);
    ubo.terrainTessParams0 = glm::vec4(tex.tessEnabled ? 1.0f : 0.0f, glm::clamp(tex.tessMaxFactor, 1.0f, 64.0f),
        glm::max(tex.tessTargetPx, 1.0f), glm::clamp(tex.tessFalloffExponent, 0.05f, 16.0f));
    ubo.terrainTessParams1 = glm::vec4(glm::max(tex.tessFadeStart, 0.0f), glm::max(tex.tessFadeEnd, tex.tessFadeStart + 0.1f),
        glm::max(tex.tessDepthGround, 0.0f), glm::max(tex.tessDepthRock, 0.0f));
    ubo.terrainTessParams2 = glm::vec4(glm::max(tex.tessFreezeDistance, 0.1f), glm::clamp(tex.tessHeightFalloffExponent, 0.05f, 16.0f), 0.0f, 0.0f);
    const uint16* heightTex = m_terrain.getSplatHeightTex();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
        ubo.terrainSplatHeightTex[i >> 2][i & 3] = heightTex[i];
    const glm::uvec2* splatTex = m_terrain.getSplatTex();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
    {
        ubo.terrainSplatTex[i >> 1][(i & 1) * 2 + 0] = splatTex[i].x;
        ubo.terrainSplatTex[i >> 1][(i & 1) * 2 + 1] = splatTex[i].y;
    }
    const float* splatGrass = m_terrain.getSplatGrass();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
        ubo.terrainSplatGrass[i >> 2][i & 3] = glm::clamp(splatGrass[i], 0.0f, 1.0f);
    // Climate boxes: temperature arrives as t01, precipitation as mm/yr - its divisor is a live tweak.
    const float invPrecipFull = 1.0f / glm::max(tex.precipFullMm, 1.0f);
    const glm::vec4* climate = m_terrain.getSplatClimate();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
        ubo.terrainSplatClimate[i] = glm::vec4(climate[i].x, climate[i].y,
            glm::clamp(climate[i].z * invPrecipFull, 0.0f, 1.0f), glm::clamp(climate[i].w * invPrecipFull, 0.0f, 1.0f));

    // Terrain wetness clipmap window: TERRAIN_WET_RES texels of texelSize centred on the scene focus, its
    // origin an integer lattice coord (the shaders address the toroidal image by lattice & (RES-1)).
    // The compute pass carries a texel's wetness only if its coord was inside the LAST TICK's window, so
    // a window that was not live last frame (first enable, re-enable) parks the previous origin out of
    // range and every texel starts dry instead of inheriting a stale slot.
    //
    // FIXED TICK ("Terrain/Water/Update rate (Hz)"): the pass runs only when the accumulated sim delta
    // reaches the tick interval, and integrates that whole delta at once. Per frame the change was below
    // the R16F image's representable step at high fps and rounded away, so wetness depended on the
    // framerate. Between ticks the pass is skipped and the reader keeps the last tick's layer + origin.
    {
        const TerrainWetTweaks& wet = m_terrain.getWetTweaks();
        const float texel = glm::max(wet.texelSize, 0.05f);
        // SIM delta (frozen by the global pause, like the particles), capped so a hitch cannot dry the map.
        const TerrainResources::WetnessTick tick = m_terrain.advanceWetness(
            oc::min((float)Globals::time.getSimDeltaSec(), 0.25f), sceneFocusOrCamera());
        const float dt = tick.dt;
        const glm::ivec2 origin = tick.origin;
        const glm::ivec2 prevOrigin = tick.prevOrigin;
        const float decay = wet.dryTime > 0.0f ? std::exp(-dt / wet.dryTime) : 0.0f;
        ubo.terrainWetParams0 = glm::vec4((float)origin.x, (float)origin.y, (float)prevOrigin.x, (float)prevOrigin.y);
        ubo.terrainWetParams1 = glm::vec4(texel, 1.0f / texel, decay, glm::max(wet.rain, 0.0f) * dt);
        ubo.terrainWetParams2 = glm::vec4(wet.enabled ? 1.0f : 0.0f, glm::clamp(wet.darkening, 0.0f, 1.0f),
            glm::clamp(wet.roughness, 0.0f, 1.0f), glm::max(wet.dryTempSens, 0.0f));
        const float writeLayer = (float)tick.writeLayer;
        const float wetIn = wet.wetInTime > 0.0f ? dt / wet.wetInTime : 1.0f;
        // Diffusion spread as a per-frame mix fraction from a per-second rate: the tent's variance then
        // grows by ~rate * texel^2 per second at any framerate (a fixed per-frame fraction would spread
        // twice as fast at twice the fps).
        const float spread = 1.0f - std::exp(-glm::max(wet.diffusionRate, 0.0f) * dt);
        ubo.terrainWetParams3 = glm::vec4(writeLayer, wetIn, glm::max(wet.slopeDrain, 0.0f), spread);
        ubo.terrainWetParams4 = glm::vec4(glm::clamp(wet.fillStart, 0.0f, 0.99f),
            glm::clamp(wet.fillFull, wet.fillStart + 0.01f, 1.0f), glm::clamp(wet.fillCurve, 0.05f, 16.0f),
            glm::max(wet.edgeFade, 1e-4f));
        ubo.terrainWetParams5 = glm::vec4(glm::max(wet.oceanBlend, 0.01f), glm::clamp(wet.waviness, 0.0f, 1.0f),
            glm::max(wet.normalScale, 0.0f), glm::max(wet.rippleStrength, 0.0f));
        // Film max slope as mesh normal.y thresholds for terrainPoolLevel: no pool below cos(max), the full
        // level above cos(max - fade). 90 = off (both below any normal).
        const float maxSlope = glm::clamp(wet.filmMaxSlope, 0.0f, 90.0f);
        const float slopeCut = maxSlope >= 90.0f ? -2.0f : std::cos(glm::radians(maxSlope));
        const float slopeFull = maxSlope >= 90.0f ? -1.5f
            : std::cos(glm::radians(glm::max(maxSlope - glm::max(wet.filmSlopeFade, 0.0f), 0.0f))) + 1e-4f;
        ubo.terrainWetParams6 = glm::vec4(glm::max(wet.oceanEdgeFade, 0.0f), slopeCut, slopeFull,
            glm::max(wet.filmFlowSpeed, 0.0f));
        // Film flow slope gate as tan values: none below half the min slope, full at it.
        const float flowMinSlope = glm::radians(glm::clamp(wet.filmFlowMinSlope, 0.0f, 80.0f));
        ubo.terrainWetParams10 = glm::vec4(std::tan(0.5f * flowMinSlope), std::tan(flowMinSlope) + 1e-4f,
            1.0f / glm::max(wet.glintSize, 0.005f), glm::clamp(wet.glintCoverage, 0.0f, 1.0f));
        ubo.terrainWetParams11 = glm::vec4(glm::clamp(wet.glintRoughness, 0.01f, 1.0f), 0.0f, 0.0f, 0.0f);
        ubo.terrainWetParams7 = glm::vec4(glm::clamp(wet.wetRoughness, 0.0f, 1.0f),
            glm::clamp(wet.underwaterRoughness, 0.0f, 1.0f), glm::clamp(wet.roughnessEdge, 0.001f, 1.0f),
            glm::clamp(wet.dryingPattern, 0.0f, 1.0f));
        ubo.terrainWetParams8 = glm::vec4(glm::clamp(wet.darkeningThreshold, 1e-3f, 1.0f),
            glm::clamp(wet.roughnessThreshold, 1e-3f, 1.0f), glm::clamp(wet.wetNormalScale, 0.0f, 4.0f),
            glm::max(wet.filmFlowCycle, 0.05f));
        ubo.terrainWetParams9 = glm::vec4(glm::clamp(wet.darkeningEdge, 0.001f, 1.0f),
            1.0f / glm::max(wet.dryingPatternSize, 0.05f), glm::clamp(wet.dryingPatternRelief, 0.0f, 1.0f),
            0.5f * glm::max(wet.dryingPatternContrast, 0.0f));
    }
    // The splat textures belong to no rendered instance's material, so the projected-size priority pass
    // never sees them - report them here instead: terrain tiles them across the whole view, so they can
    // always display roughly a screen's worth of texels.
    if (m_terrain.getSplatBaseMaterial() >= 0)
    {
        const float log2Screen = std::log2((float)oc::max(m_windowSize.x, m_windowSize.y) * 2.0f);
        for (const uint16 texIdx : m_terrain.getSplatTextures())
            Globals::textureStreamer.noteUse(texIdx, log2Screen);
    }
}
