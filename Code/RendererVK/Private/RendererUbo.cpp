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
    buildUboSunShadow(camera);
    buildUboRainOcclusion();
    buildUboFog();
    buildUboOcean();
    buildUboForce();
    buildUboTerrain();

    Globals::stagingManager.upload(frameData.ubo.getBuffer(), sizeof(RendererVKLayout::Ubo), &m_ubo);
}

// View matrices, frustum, and TAA jitter: the center (culling) view, the VR eye views, and the
// screen/viewport constants. Sets m_centerViewProj + m_prevTaaJitter.
void Renderer::buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation)
{
    RendererVKLayout::Ubo& ubo = m_ubo;
    const glm::ivec2 viewportSize = m_viewportRect.getSize();

    glm::vec2 taaJitterNdc(0.0f);
    if (m_taaParams.taaEnabled && viewportSize.x > 0 && viewportSize.y > 0)
    {
        const uint32 sampleIdx = (m_frameCounter % 16u) + 1u;
        taaJitterNdc.x = (radicalInverse(sampleIdx, 2u) - 0.5f) * 2.0f / (float)viewportSize.x;
        taaJitterNdc.y = (radicalInverse(sampleIdx, 3u) - 0.5f) * 2.0f / (float)viewportSize.y;
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

    const vk::Extent2D swapExtent = m_swapChain.getLayout().extent;
    ubo.screenSize = glm::vec4((float)swapExtent.width, (float)swapExtent.height,
        1.0f / (float)swapExtent.width, 1.0f / (float)swapExtent.height);
    ubo.viewportRect = glm::vec4(
        (float)m_viewportRect.min.x / (float)swapExtent.width,
        (float)m_viewportRect.min.y / (float)swapExtent.height,
        (float)viewportSize.x / (float)swapExtent.width,
        (float)viewportSize.y / (float)swapExtent.height);
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

    ubo.cloudCoverage = sky.cloudCoverage;
    ubo.cloudThickness = sky.cloudThickness * sky.cloudThickness;
    ubo.cloudParams0 = glm::vec4(sky.cloudHeight, 0.00012f * sky.cloudScale, 0.0043f * sky.cloudWindSpeed, sky.cloudWindAngle);
    ubo.cloudParams1 = glm::vec4(sky.cloudSoftness, sky.cloudShading, 0.0f, 0.0f);
    ubo.cloudParams2 = glm::vec4(sky.cloudDensity, sky.cloudSharpness, sky.cloudBaseVar, sky.moonBrightness);
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
    ubo.fogParams7 = glm::vec4(
        glm::max(fog.shaftBoost, 0.0f),        // x: underwater sun in-scatter gain (fog light shafts)
        waveBand,
        glm::max(fog.causticStrength, 0.0f),   // z: underwater caustic focus strength (surfaces + fog shafts)
        glm::max(fog.causticDepthFade, 0.0f)); // w: caustic contrast decay with depth (1/m)
    ubo.fogParams8 = glm::vec4(fog.underwaterOffset, glm::max(fog.causticShoreFade, 0.0f), 0.0f, 0.0f);
    // z: thickness scale inverted into a falloff multiplier on fogParams0.z. w: far-field ground samples.
    ubo.fogParams9 = glm::vec4(fog.farField ? 1.0f : 0.0f, glm::max(fog.farFieldDensity, 0.0f),
        1.0f / glm::clamp(fog.farFieldThickness, 0.01f, 100.0f), (float)glm::max(fog.farFieldSteps, 1));
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
        glm::clamp(ocean.foamDecay, 0.0f, 0.999f), glm::clamp(ocean.detailBias, -4.0f, 4.0f));
    ubo.oceanParams4 = glm::vec4(glm::max(ocean.horizonDepthRange, 0.0f),
        glm::clamp(ocean.foamSpread * 0.25f, 0.0f, 0.95f), glm::max(ocean.shoalScale, 0.0f), glm::max(ocean.foamSoftness, 0.02f));
    ubo.oceanParams5 = glm::vec4(glm::max(ocean.foamBoost, 0.0f), glm::clamp(ocean.turbidity, 0.0f, 1.0f), glm::max(ocean.shoreFoamDepth, 0.0f), glm::max(ocean.foamBreakAccel, 0.01f));
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
    // Ocean spray producer: the emitter slot the Particle system published (UINT32_MAX = off), the sim
    // delta the rate integrates over (frozen with the global pause, like the particle sim itself).
    const float sprayDt = oc::min((float)Globals::time.getSimDeltaSec(), 0.25f);
    // Off while the particle chain is disabled: nothing would consume (and reset) the request counter.
    const uint32 sprayEmitter = m_particles.isEnabled() ? m_oceanSimPipeline.getSprayEmitter() : UINT32_MAX;
    const OceanSprayParams& spray = m_oceanSimPipeline.getSprayParams();
    ubo.oceanSpray0 = glm::vec4(glm::uintBitsToFloat(sprayEmitter), glm::max(spray.rate, 0.0f),
        glm::max(spray.radius, 1.0f), sprayDt);
    ubo.oceanSpray1 = glm::vec4(glm::clamp(spray.threshold, 0.0f, 0.99f), glm::max(spray.kick, 0.0f),
        glm::max(spray.speed, 0.0f), spray.forward);
    ubo.oceanSpray2 = glm::vec4(spray.height, 0.0f, 0.0f, 0.0f);
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
    ubo.terrainTessParams2 = glm::vec4(glm::max(tex.tessFreezeDistance, 0.1f), 0.0f, 0.0f, 0.0f);
    const uint16* heightTex = m_terrain.getSplatHeightTex();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
        ubo.terrainSplatHeightTex[i >> 2][i & 3] = heightTex[i];
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
            1.0f / glm::clamp(wet.darkeningReach, 0.05f, 8.0f));
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
