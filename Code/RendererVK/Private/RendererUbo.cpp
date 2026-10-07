module RendererVK;

import RendererVK.fwd;

import Core;
import Core.fwd;
import Core.glm;
import Core.Window;
import Core.Frustum;
import Core.imgui;
import Core.Camera;
import Settings;
import Settings.Tweaks;
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
import :UboBlock;
import :UboRoot;

// The values both a lockable entry and the live build (or two entries) read: one definition, so they cannot drift.
namespace
{
    // Earth sea-level scattering coefficients, scaled by the atmosphere tweaks.
    glm::vec3 skyBetaRayleigh(const SkyParams& sky) { return glm::vec3(5.802e-6f, 13.558e-6f, 33.1e-6f) * sky.rayleighScatter; }
    float skyBetaMie(const SkyParams& sky) { return 3.996e-6f * sky.mieScatter; }
    glm::vec3 skyMoonDirection(const SkyParams& sky) { return glm::normalize(sky.moonDirection); }
    float skyMoonCos(const SkyParams& sky) { return cosf(glm::radians(sky.moonSizeDeg)); }

    // The cloud noise periods (m, double: the live wind / evolve offsets wrap by them).
    double cloudWeatherPeriod(const CloudParams& c) { return glm::max((double)c.weatherSizeKm, 1.0) * 1000.0; }
    double cloudBasePeriod(const CloudParams& c) { return cloudWeatherPeriod(c) / glm::max(c.baseRepeats, 1); }
    double cloudDetailPeriod(const CloudParams& c) { return cloudBasePeriod(c) / glm::max(c.detailRepeats, 1); }
    // The main layer's band, the upper layer's, and the SHELL = their union (what the march, the shadow map and the
    // height-in-shell lighting cover).
    float cloudMainBottom(const CloudParams& c) { return glm::max(c.bottom, 0.0f); }
    float cloudMainTop(const CloudParams& c) { return glm::max(c.top, cloudMainBottom(c) + 100.0f); }
    float cloudUpperBottom(const CloudParams& c) { return glm::max(c.upperBottom, 0.0f); }
    float cloudUpperTop(const CloudParams& c) { return glm::max(c.upperTop, cloudUpperBottom(c) + 50.0f); }
    bool cloudUpperOn(const CloudParams& c) { return c.upperEnabled && c.upperDensity > 0.0f && c.upperCoverage > 0.0f; }
    float cloudShellBottom(const CloudParams& c) { return cloudUpperOn(c) ? glm::min(cloudMainBottom(c), cloudUpperBottom(c)) : cloudMainBottom(c); }
    float cloudShellTop(const CloudParams& c) { return cloudUpperOn(c) ? glm::max(cloudMainTop(c), cloudUpperTop(c)) : cloudMainTop(c); }
    float cloudCoverage(const CloudParams& c) { return glm::clamp(c.coverage, 0.0f, 1.0f); }

    // The effective RT sun shadow: with RT off (or RT-sun off) the PCSS cascades supply the sun shadow.
    bool rtSunShadowActive(const RTParams& rt) { return rt.enabled && rt.rtSunShadow; }

    glm::vec3 srgbToLinear(const glm::vec3& srgb) { return glm::pow(glm::clamp(srgb, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f)); }
    float grassLod2Distance(const GrassParams& g) { return glm::max(g.lod2Distance, g.lod1Distance); }

    float rockCoverStart(const RockParams& r) { return glm::clamp(r.coverSlopeStart, 0.0f, 1.0f); }

    float forceIsoThreshold(const ForceFieldParams& f) { return glm::max(f.isoThreshold, 1e-3f); }

    float terrainFilmMaxSlopeDeg(const TerrainSettings& w) { return glm::clamp(w.wetFilmMaxSlope, 0.0f, 90.0f); }
    float terrainFilmFlowMinSlopeRad(const TerrainSettings& w) { return glm::radians(glm::clamp(w.wetFilmFlowMinSlope, 0.0f, 80.0f)); }
}

float Renderer::grassPatchSize() const
{
    return glm::clamp(m_grassParams.patchSize, 1.0f, 16.0f);
}

// Capped so the patch grid fits GRASS_MAX_PATCHES.
float Renderer::grassGridRange() const
{
    const uint32 maxHalf = (uint32)std::sqrt((float)RendererVKLayout::GRASS_MAX_PATCHES) / 2u - 1u; // (2 half + 1)^2 patches
    return glm::min(m_grassParams.range, grassPatchSize() * (float)maxHalf);
}

// Renderer: THE FRAME UBO. buildFrameUbo assembles the one uniform buffer every pass reads, from the
// param blocks the outside pushes in and the registries under Data/; the buildUbo* helpers below write its root
// values by subject through m_u's handles (UboRoot.ixx: everything that depends on the camera, the clock, the sun,
// the wind or a value the outside pushes per frame), and registerUboFields (at the end) lists the LOCKABLE values - computed from tweaks
// only, each a constant in the shaders while its tweaks are locked (applyUboLocks, RendererUboBake.cpp). An
// entry's lambda names every input it reads as its sources: a new input needs a new source too. Pure CPU math -
// nothing here records or touches the device.
//
// It runs wherever beginFrame runs (the desktop "Begin frame job" or main in VR), so everything it
// reads is written only outside that quiescent window, and m_ubo PERSISTS across frames: buildUboViews
// reads last frame's mvps out of it for reprojection before overwriting them. The outputs consumers read
// directly (m_centerViewProj, m_sunCascadeViewProj, the returned frustum) are contractually valid once
// beginFrame's job is joined.
void Renderer::buildFrameUbo(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation, PerFrameData& frameData)
{
    ProfileScope uboScope("UBO build", EProfileCategory::Renderer);
    UboBlock& ubo = m_ubo;

    ubo.zero(m_u.present.begin(), m_u.present.size()); // the tree range and the TLAS count: present() patches them in
    ubo.set(m_u.frameIndex, m_frameCounter);
    // SIM clock, not the wall clock: shader animation (ocean waves, force pulses, fog) freezes with
    // the global pause (see Time::setPaused).
    ubo.set(m_u.timeSeconds, (float)Globals::time.getSimElapsedSec());
    ubo.set(m_u.mipPixelScale, m_mipPixelScale);

    buildUboViews(cameraIn, camera, vrBaseOrientation);
    buildUboWeather(camera);
    buildUboRayTracing();
    buildUboSky();
    buildUboClouds(camera);
    buildUboSunShadow(camera);
    buildUboFog();
    buildUboOcean();
    buildUboForce();
    buildUboTerrain();
    buildUboGrass(camera);
    buildUboFoliage();

    ubo.evaluate(); // the lockable values
    Globals::stagingManager.upload(frameData.ubo.getBuffer(), ubo.size(), ubo.data());
}

// THE wind ("Sky/Wind": the particles, the vegetation - wind.inc.glsl - and the fog read it), the weather
// volumes' camera values and the rain occlusion map.
void Renderer::buildUboWeather(const Camera& camera)
{
    UboBlock& ubo = m_ubo;
    const UboRoot::Weather& weather = m_u.weather;
    const ParticleParams& particles = m_particles.getParams();

    const WindParams& wind = m_windParams;
    const glm::vec2 dir = wind.direction();
    const float speed = glm::max(wind.speed, 0.0f);
    ubo.set(weather.windVelocity, glm::vec3(dir.x * speed, 0.0f, dir.y * speed));
    ubo.set(weather.windDirection, dir);
    ubo.set(weather.windSpeed, speed);
    ubo.set(weather.gustStrength, glm::max(wind.gustStrength, 0.0f));
    ubo.set(weather.invGustSize, 1.0f / glm::max(wind.gustSize, 1.0f));
    ubo.set(weather.cameraWaterY, m_oceanSimPipeline.getCameraWaterSurface());
    ubo.set(weather.cameraWaterValid, m_oceanSimPipeline.hasCameraWaterSurface() ? 1.0f : 0.0f);

    // Camera velocity over the WALL-CLOCK frame delta (camera motion is not paused with the sim): the
    // weather volumes' streaks are motion blur relative to the eye. A teleport (first frame, scene
    // load) reads as zero rather than one huge streak.
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
    ubo.set(weather.cameraVelocity, velocity);

    // The weather volume's rain occlusion map: a top-down orthographic view over the latched volume box
    // (setRainOcclusionVolume), looking straight down -Y. The eye sits casterPad above the box top so a roof
    // well above the volume still shelters it; the XZ footprint is padded 25 % over the box (one-frame lag
    // behind the camera-following emitter). Standard Z, plain bottom row: the ray-query pass (RainOcclusionPipeline,
    // through its inverse) and the particle sim's shelter test both read it verbatim. Off without RT (no TLAS to trace).
    const ParticleState::RainVolume& v = m_particles.getRainVolume();
    if (!v.active || !particles.rainOcclusion || !m_rtParams.enabled)
    {
        ubo.set(weather.rainOcclusionViewProj, glm::mat4(1.0f));
        ubo.set(weather.rainOcclusionPresent, 0.0f);
        ubo.set(weather.rainOcclusionInvRange, 0.0f);
        return;
    }
    const float hx = glm::max(v.halfExtents.x, 1.0f) * 1.25f;
    const float hz = glm::max(v.halfExtents.z, 1.0f) * 1.25f;
    const float pad = glm::max(particles.rainOcclusionCasterPad, 1.0f);
    const float range = 2.0f * glm::max(v.halfExtents.y, 1.0f) + pad + 1.0f; // 1 m below the box bottom
    const glm::vec3 eye = v.center + glm::vec3(0.0f, v.halfExtents.y + pad, 0.0f);
    const glm::mat4 view = glm::lookAtRH(eye, v.center, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 proj = glm::orthoRH_ZO(-hx, hx, -hz, hz, 0.0f, range);
    ubo.set(weather.rainOcclusionViewProj, proj * view);
    ubo.set(weather.rainOcclusionPresent, 1.0f);
    ubo.set(weather.rainOcclusionInvRange, 1.0f / range);
}

// GI's live values (the RT / RTAO / GI switches and settings are registerUboFields' entries).
void Renderer::buildUboRayTracing()
{
    const bool giOn = m_rtParams.enabled && m_rtParams.giEnabled;
    // Once per frame: the fullBake flag is a one-frame request (takeVisibilityParams).
    m_ubo.set(m_u.giLive.fullBake, m_giProbePipeline.takeVisibilityParams(giOn).y);
    // WALL delta: GI converges through a sim pause.
    m_ubo.set(m_u.giLive.temporalAlpha, m_giProbePipeline.getTraceParams0((float)Globals::time.getDeltaSec()).y);
    // The previous focus advances only while GI traces, so probes that scrolled in during a GI-off spell
    // still read as fresh (full replace) on the first traced frame.
    m_ubo.set(m_u.giLive.prevFocus, m_giPrevFocusPos);
    if (giOn)
        m_giPrevFocusPos = sceneFocusOrCamera();
}

// The grass's live values and the near grass cascade (the "Grass" settings are registerUboFields' entries; the patch
// grid uploadGrassFrame writes reads the same grassPatchSize / grassRange).
void Renderer::buildUboGrass(const Camera& camera)
{
    UboBlock& ubo = m_ubo;
    const UboRoot::GrassLive& live = m_u.grassLive;
    const GrassParams& g = m_grassParams;
    const float patchSize = grassPatchSize();

    // The pixel floor as world width per metre of distance: one pixel spans 2 d / m_mipPixelScale.
    ubo.set(live.minWidthPerMetre, m_mipPixelScale > 0.0f ? g.minPixelWidth * 2.0f / m_mipPixelScale : 0.0f);
    // The canopy's base extinction (1/m; grass.inc.glsl): "Canopy shadow" x blades per m^2 x the mean blade width
    // (it tapers to the tip: half the root width). 0 without grass: the terrain FS then skips the canopy.
    const float bladesPerM2 = (float)m_grassPipeline.getBladesPerPatch() / (patchSize * patchSize);
    ubo.set(live.canopyExtinction, grassActive() ? glm::max(g.canopyShadow, 0.0f) * bladesPerM2 * 0.5f * g.bladeWidth : 0.0f);
    ubo.set(live.prevTime, m_grassPrevTime);
    m_grassPrevTime = ubo.get(m_u.timeSeconds);

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

        const glm::vec3 L = glm::normalize(ubo.get(m_u.sunDirection));
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
        ubo.set(live.shadowViewProj, lightProj * lightView);
    }
    ubo.set(live.nearRange, nearRange);
    ubo.set(live.nearTexel, 2.0f * nearRange / res); // the receivers' normal offset
    ubo.set(live.nearCentre, glm::vec2(nearCentre.x, nearCentre.z)); // the receivers' fade disc and the casters' selection measure from it
}

// The trees' live values: the wind reach and the far-tree volume's per-frame state (the "Trees" settings are
// registerUboFields' entries).
void Renderer::buildUboFoliage()
{
    const FoliageParams& f = m_foliageParams;
    UboBlock& ubo = m_ubo;
    const UboRoot::FoliageLive& live = m_u.foliageLive;

    // The culls grow a tree's bound by the sway's reach: the strongest gust, a tree twice the reference height (the
    // bend x 4), the branch tips (x 1.5 for the tree scale) and a leaf.
    const float strongest = glm::max(m_windParams.speed, 0.0f) + glm::max(m_windParams.gustStrength, 0.0f) * 1.42f; // the gust is a 2D vector
    ubo.set(live.windReach, glm::max(f.windBend, 0.0f) * strongest * strongest * (1.0f + glm::max(f.windSway, 0.0f)) * 4.0f
        + glm::max(f.windBranch, 0.0f) * strongest * 1.5f + glm::max(f.windLeaf, 0.0f));
    ubo.set(live.windPrevTime, m_treeWindPrevTime);
    m_treeWindPrevTime = ubo.get(m_u.timeSeconds);
    ubo.set(live.farMarched, farTreesActive() ? 1.0f : 0.0f);
    // The far-tree volume's hand-over as of this frame (its state changes only in record(), after this build).
    const glm::vec4 handover = m_treeVolume.handoverUbo();
    ubo.set(live.handoverCentre, glm::vec2(handover.x, handover.y));
    ubo.set(live.handoverFade, handover.z);
}

// View matrices, frustum, and TAA jitter: the center (culling) view, the VR eye views, and the
// screen/viewport constants. Sets m_centerViewProj + m_centerFrustum + m_prevTaaJitter.
void Renderer::buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation)
{
    UboBlock& ubo = m_ubo;
    const UboRoot& h = m_u;
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
        ubo.set(h.viewPrevMvp, v, ubo.get(h.viewMvp, v));
        ubo.set(h.viewPrevInvMvp, v, ubo.get(h.viewInvMvp, v));
    }

    // One view's matrices from its mvp: the inverse in double precision - a float32 inverse of a perspective mvp is
    // ill-conditioned and its error grows with the camera translation, which shows up as per-frame reconstruction
    // jitter (sky ray, TAA reprojection, RTAO, fog) away from the world origin - and the fused clip->prev-clip
    // reprojection: the double product cancels the (position-scaled) translations exactly, leaving a near-identity
    // matrix that survives float32 storage at any camera position.
    const auto writeView = [&](uint32 v, const glm::mat4& mvp, const glm::vec3& position)
    {
        ubo.set(h.viewMvp, v, mvp);
        const glm::dmat4 invMvpD = glm::inverse(glm::dmat4(mvp));
        ubo.set(h.viewInvMvp, v, glm::mat4(invMvpD));
        ubo.set(h.viewReprojClip, v, glm::mat4(glm::dmat4(ubo.get(h.viewPrevMvp, v)) * invMvpD));
        ubo.set(h.viewPos, v, glm::vec4(position, 1.0f));
    };
    const glm::mat4 centerMvp = computeCenterViewProj(camera);
    writeView(RendererVKLayout::VIEW_CENTER, centerMvp, camera.position);
    // ZO plane extraction (the projection is reversed-Z [0,1] clip; the near/far plane slots swap roles
    // under the reversal but the extracted volume is identical, so all cull consumers stay correct).
    m_centerFrustum.fromMatrixZO(centerMvp);
    for (uint32 i = 0; i < 6; ++i)
        ubo.set(h.frustumPlanes, i, m_centerFrustum.planes[i]);
    m_centerViewProj = centerMvp;

    if (Globals::openXR.isEnabled())
    {
        for (uint32 eye = 0; eye < 2; ++eye)
        {
            glm::mat4 eyeView;
            glm::vec3 eyePos;
            Globals::openXR.getEyeView(eye, cameraIn.position, vrBaseOrientation, eyeView, eyePos);
            const glm::mat4 eyeProj = Globals::openXR.getEyeProjection(eye, camera.near, camera.far);
            writeView(eye + 1, eyeProj * eyeView, eyePos); // [1] = left eye, [2] = right eye
        }
    }

    // The render-size targets and the scene's rect in them: every scene / screen-space pass works in these.
    // Without upscaling they are the swapchain extent and the viewport rect.
    const glm::vec2 targetSize(m_renderExtent);
    ubo.set(h.screenSize, glm::vec4(targetSize, 1.0f / targetSize));
    ubo.set(h.viewportRect, glm::vec4(glm::vec2(m_renderRect.min) / targetSize, glm::vec2(renderSize) / targetSize));
    // zw = LAST frame's jitter: TAA/AO-temporal compensate both frames' jittered depth images during
    // reprojection (all raster passes jitter, the prepass included - see taaJitterUv in shared.inc.glsl).
    ubo.set(h.taaJitter, glm::vec4(taaJitterNdc, m_prevTaaJitter));
    m_prevTaaJitter = taaJitterNdc;
}

// The sun (direction, colour, the eclipse, the transmittance toward it), the ambient and the up axis: root (live).
// The "Sky" section's values are registerUboFields' entries.
void Renderer::buildUboSky()
{
    UboBlock& ubo = m_ubo;
    const SkyParams& sky = m_skyParams;

    ubo.set(m_u.sunDirection, sky.sunDirection);
    ubo.set(m_u.sunColor, sky.sunColor * sky.sunIntensity);
    // Solar eclipse: the moon-covered sun fraction, so every sun consumer (sky atmosphere, forward lighting, GI
    // trace, volumetric fog, clouds) dims consistently.
    ubo.set(m_u.sunVisible, sunVisibleFraction(sky.sunDirection, skyMoonDirection(sky), sky.sunAngularCos, skyMoonCos(sky), sky.sunGlow));
    ubo.set(m_u.ambientColor, sky.ambientColor * sky.ambientIntensity);
    ubo.set(m_u.skyUp, sky.up);

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
        const glm::vec3 tau = skyBetaRayleigh(sky) * odR + glm::vec3(skyBetaMie(sky) * sky.mieExtinction) * odM + c_betaOzone * (sky.ozone * odR);
        ubo.set(m_u.sunTransmittance, glm::exp(-tau));
    }
}

// Volumetric clouds (clouds.inc.glsl). The noise is world-anchored and tiles: base and detail repeat an
// integer number of times per weather tile, so ONE wrap of (camera + wind) by the weather period keeps
// every texture continuous and the shader's noise coordinates small at any camera position. The wind
// and the detail drift accumulate here in double on the SIM clock (they stop with the global pause).
void Renderer::buildUboClouds(const Camera& camera)
{
    UboBlock& ubo = m_ubo;
    const UboRoot::CloudsLive& live = m_u.cloudsLive;
    const CloudParams& c = m_cloudParams;

    const double weatherPeriod = cloudWeatherPeriod(c);
    const double detailPeriod = cloudDetailPeriod(c);

    const bool enabled = cloudsEnabled();
    ubo.set(live.enabled, enabled ? 1.0f : 0.0f);
    const double dt = glm::min((double)Globals::time.getSimDeltaSec(), 0.25);
    // The shared wind ("Sky/Wind"), x the layer's speed scale.
    const glm::dvec2 windStep = glm::dvec2(m_windParams.direction()) * ((double)glm::max(m_windParams.speed, 0.0f) * (double)c.windSpeedScale * dt);
    m_cloudWindOffset = glm::mod(m_cloudWindOffset + windStep, glm::dvec2(weatherPeriod));
    m_cloudEvolveOffset = std::fmod(m_cloudEvolveOffset + (double)c.evolveSpeed * dt, detailPeriod);
    // Noise space = world - wind, so the field travels WITH the wind.
    const glm::dvec2 origin = glm::mod(glm::dvec2(camera.position.x, camera.position.z) - m_cloudWindOffset, glm::dvec2(weatherPeriod));
    ubo.set(live.noiseOrigin, glm::vec2(origin));
    ubo.set(live.detailDrift, (float)m_cloudEvolveOffset);
    ubo.set(live.windStep, glm::vec3((float)windStep.x, 0.0f, (float)windStep.y)); // the field's world displacement this frame
    // The sky-map clouds' history weight per march of a texel: exp(-3 dt / T) reaches 95 % of a change in T seconds,
    // at any frame rate; a texel marches every SKY_UPDATE_FRAMES frames, so dt spans that many. Real time, not sim
    // time: the camera still moves while the sim is paused.
    const float realDt = glm::min((float)Globals::time.getDeltaSec(), 0.25f) * (float)CloudPipeline::SKY_UPDATE_FRAMES;
    // The floor ("Sky map min samples", N marches: w = (N - 1) / (N + 1)) keeps the average's noise the same at a low
    // frame rate, where the time-based weight alone averaged a few marches and the ocean mirrored a per-frame flicker.
    const float minSampleWeight = c.skyMapMinSamples > 1.0f ? (c.skyMapMinSamples - 1.0f) / (c.skyMapMinSamples + 1.0f) : 0.0f;
    ubo.set(live.skyHistory, c.skyMapHistorySec > 0.0f ? glm::max(std::exp(-3.0f * realDt / c.skyMapHistorySec), minSampleWeight) : 0.0f);
    ubo.set(live.giSkyHistory, c.giSkyHistorySec > 0.0f ? glm::max(std::exp(-3.0f * realDt / c.giSkyHistorySec), minSampleWeight) : 0.0f);
    // The ground bounce's albedo: the sky's "Ground Albedo" COLOUR (its hue, not its intensity - that one scales the
    // sky-sphere ground plane and defaults to 0) x the cloud "Ground albedo".
    ubo.set(live.groundBounceAlbedo, m_skyParams.groundColor * c.groundAlbedo);

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
    ubo.set(live.shadowRendered, shadowOn ? 1.0f : 0.0f);
    if (!shadowOn)
    {
        m_cloudShadowValid = {};
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
        ubo.set(live.shadowCascade, cascade, glm::vec4(glm::vec3(m_cloudShadowCenter[cascade] - camPos), (float)(1.0 / m_cloudShadowExtent[cascade])));
    }
    ubo.set(live.shadowAxis0, glm::vec3(e0));
    ubo.set(live.shadowAxis1, glm::vec3(e1));
}

// Sun shadow route: the PCSS cascade matrices (also consumed CPU-side via getSunCascadeViewProj) unless RT sun
// shadows replace them, plus the scene focus and the "Shadows" section's biases + long-range terrain march.
void Renderer::buildUboSunShadow(const Camera& camera)
{
    UboBlock& ubo = m_ubo;
    // The shaders' cascade pick and the RTAO falloff measure from here; it matches computeSunCascades'
    // origin below.
    ubo.set(m_u.sceneFocus, m_sceneFocusEnabled ? m_sceneFocus : camera.position);

    // Use the effective flag: with RT off (or RT-sun off) the PCSS cascades supply the sun shadow.
    if (rtSunShadowActive(m_rtParams))
    {
        m_numSunCascades = 0;
        return;
    }
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
    glm::vec4 sunSizeTexels;
    for (uint32 c = 0; c < RendererVKLayout::NUM_SHADOW_CASCADES; ++c)
    {
        ubo.set(m_u.cascadeViewProj, c, m_sunCascadeViewProj[c]);
        const float texelWorldSize = m_sunCascadeViewProj[c][1][3];
        const float depthRange = m_sunCascadeViewProj[c][2][3];
        sunSizeTexels[c] = tanT * depthRange / glm::max(texelWorldSize, 1e-6f);
    }
    ubo.set(m_u.cascadeSunSizeTexels, sunSizeTexels);
}

// Volumetric fog params, the baked terrain height map's placement (the fog terrain cascades: also the ocean's shore-map
// fallback and every terrain-aware pass's), and the fog's values that ride the ocean.
void Renderer::buildUboFog()
{
    UboBlock& ubo = m_ubo;
    const UboRoot::FogLive& live = m_u.fogLive;
    const UboRoot::TerrainLive& terrain = m_u.terrainLive;
    const FogParams& fog = m_fogParams;

    // A freshly uploaded fog terrain height map activates here, in the same frame slot as the UBO that
    // carries its world center/sizes - descriptors (refreshed per frame in recordCommandBuffers) and
    // params stay coherent. Its presence (mapInvNearSize) is independent of Terrain Follow: the ocean also
    // reads these cascades as its shore-map fallback.
    m_terrain.getHeightMap().flipIfPending();
    const glm::vec2 mapSizes = m_terrain.getHeightMap().getWorldSizes();
    ubo.set(terrain.mapCentre, m_terrain.getHeightMap().getCenter());
    ubo.set(terrain.mapInvNearSize, mapSizes.x > 1.0f ? 1.0f / mapSizes.x : 0.0f);
    ubo.set(terrain.mapInvFarSize, mapSizes.y > 1.0f ? 1.0f / mapSizes.y : 0.0f);
    ubo.set(terrain.mapSeaLevel, m_terrain.getHeightMap().getUserParam());

    // The waterline band gating the fog's FFT wave taps: froxel segments outside +-band of the calm level are
    // trivially above / below any possible wave, so only a thin shell pays for wave taps. The CPU trough estimate
    // (ocean readback) bounds the wave amplitude; 0 disables wave sampling entirely (ocean off).
    // The band must also cover the swash RUN-UP (amplitude x (trough + 0.25), the same reach buildUboOcean
    // writes as swashReach): with a large swash amplitude the tongue climbs past 2 x trough, and froxels beyond
    // the band got the calm level while their neighbours got the wave - a line in the fog's caustics that moved
    // with the amplitude.
    const float waveTrough = m_oceanSimPipeline.getWaveTrough();
    const float swashReachBand = getOceanSwashAmp() * (waveTrough + 0.25f);
    ubo.set(live.waveBand, m_oceanSimPipeline.isOceanEnabled() ? glm::max(waveTrough * 2.0f + 0.5f, swashReachBand) : 0.0f);
    // The boundary offset - "Underwater wave offset" x the deepest live wave trough, down (world metres already;
    // 0 with the ocean off): a higher sea needs a lower boundary, or the murk peeks through the troughs the fog's
    // coarse froxels miss.
    ubo.set(live.boundaryOffset, -glm::max(fog.underwaterWaveOffset, 0.0f) * waveTrough);
    // The underwater metres are measured against the sea, so they ride "Ocean/World scale" like the
    // ocean's own tweaks: lengths x s, the per-metre depth fade / s (the same water column in fewer metres).
    const float oceanScale = getOceanWorldScale();
    ubo.set(live.causticDepthFade, glm::max(fog.causticDepthFade, 0.0f) / oceanScale); // the caustic contrast's decay with depth (1/m)
    ubo.set(live.causticShoreFade, glm::max(fog.causticShoreFade, 0.0f) * oceanScale);
}

// The ocean's live values: what follows the wind (its direction and speed, the surf band), the sea level, the readback
// and the sun (the "Ocean" settings, in WORLD units from Procedural's setOceanParams, are registerUboFields' entries).
void Renderer::buildUboOcean()
{
    UboBlock& ubo = m_ubo;
    const UboRoot::OceanLive& live = m_u.oceanLive;
    const OceanParams& ocean = m_oceanSimPipeline.getOceanParams();

    // Wind clamped just above 0: the JONSWAP 1/U terms must stay finite; the spectrum's wave-age limit
    // (ocean_spectrum.cs.glsl) makes this effectively glassy anyway.
    ubo.set(live.windDirection, glm::length(ocean.windDirection) > 1e-4f ? glm::normalize(ocean.windDirection) : glm::vec2(1.0f, 0.0f));
    ubo.set(live.windSpeed, glm::max(ocean.windSpeed, 0.01f));
    ubo.set(live.seaLevel, ocean.seaLevel);
    ubo.set(live.shoreFoamDepth, glm::max(ocean.shoreFoamDepth, 0.0f));
    ubo.set(live.shoreFoamMax, glm::clamp(ocean.shoreFoamMax, 0.0f, 1.0f));
    // Swash reach: conservative max run-up height from the wave-amplitude readback (trough estimate ~
    // crest scale) - sizes the on-land sampling band and keeps the vertex cull off the wet beach.
    ubo.set(live.swashReach, getOceanSwashAmp() * (m_oceanSimPipeline.getWaveTrough() + 0.25f));
    ubo.set(live.displacementExtent, ocean.enabled ? m_oceanSimPipeline.getDisplacementExtent() : 0.0f); // the per-instance cull padding
    ubo.set(live.cameraUnderwater, ocean.cameraUnderwater ? 1.0f : 0.0f);
    // The bubble cloud's per-frame factors (ocean_bubbles.inc.glsl oceanBubbleRadianceFrame): the sun's and the
    // sky's path down to "Bubble depth" and the cloud's albedo depend on nothing per pixel, so the ocean pays one
    // exp (the path back up to the eye) per pixel, not three. The film's depth is per pixel: it keeps the full form.
    {
        const glm::vec3 L = glm::normalize(ubo.get(m_u.sunDirection)); // buildUboSky ran first
        const float sunCos = glm::max(L.y, 0.0f);
        const float muL = glm::sqrt(1.0f - (1.0f - sunCos * sunCos) / (1.33f * 1.33f)); // refracted sun cosine
        const glm::vec3 albedo = ocean.foamColor * getOceanBubbleBrightness();
        const float bubbleDepth = getOceanBubbleDepth();
        ubo.set(live.bubbleSun, albedo * glm::exp(-ocean.absorption * (bubbleDepth / muL)) * (sunCos / glm::pi<float>()));
        ubo.set(live.bubbleSky, albedo * glm::exp(-ocean.absorption * bubbleDepth));
    }
    // Ocean spray producer: the emitter slot the Particle system published (UINT32_MAX = off; also off while the
    // particle chain is disabled: nothing would consume and reset the request counter), the sim delta the rate
    // integrates over (frozen with the global pause, like the particle sim itself).
    const float sprayDt = oc::min((float)Globals::time.getSimDeltaSec(), 0.25f);
    ubo.set(live.sprayEmitter, m_particles.isEnabled() ? m_oceanSimPipeline.getSprayEmitter() : UINT32_MAX);
    ubo.set(live.sprayDt, sprayDt);
    // The world-space foam field: drift over the SIM delta (frozen with the pause, like the waves), levels
    // around the camera (the water nearest the eye is where the detail shows).
    oc::array<glm::vec4, RendererVKLayout::OCEAN_FOAM_LEVELS> foamLevels;
    glm::vec2 foamDrift;
    m_oceanSimPipeline.advanceFoamField(foamLevels, foamDrift, m_cameraPos, sprayDt);
    for (uint32 level = 0; level < RendererVKLayout::OCEAN_FOAM_LEVELS; ++level)
        ubo.set(live.foamLevels, level, foamLevels[level]);
    ubo.set(live.foamDrift, foamDrift);
}

// Forcefield bubbles' live values (the Force library pushes the params every frame; its settings are
// registerUboFields' entries).
void Renderer::buildUboForce()
{
    UboBlock& ubo = m_ubo;
    const UboRoot::ForceLive& live = m_u.forceLive;
    const ForceFieldParams& force = m_force.getParams();

    for (uint32 i = 0; i < RendererVKLayout::MAX_FORCE_TEAMS; ++i)
        ubo.set(live.teamColors, i, glm::vec4(force.teamColors[i], 0.0f));
    // The shell march's LOD scale - (px per radius/dist) / full-detail radius, so the FS's steps taper as
    // side/dist * this (clamped <= 1); 0 disables the taper.
    ubo.set(live.shellLodScale, m_mipPixelScale * 0.5f / glm::max(force.shellFullResPixels, 1.0f));
    ubo.set(live.unionPxScale, m_mipPixelScale * 0.5f); // px per (radius/dist) - the union march's distance LOD

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
        ubo.set(live.bakeMin, bakeLo);
        ubo.set(live.bakeThreshold, force.sampledShellRadius); // the VISIBLE-radius tier threshold
        ubo.set(live.bakeInvSize, 1.0f / glm::max(bakeHi - bakeLo, glm::vec3(1e-3f)));
        ubo.set(live.bakeEnabled, 1.0f);
    }
    else
    {
        ubo.set(live.bakeMin, glm::vec3(0.0f));
        ubo.set(live.bakeThreshold, FLT_MAX); // no emitter reaches the threshold
        ubo.set(live.bakeInvSize, glm::vec3(0.0f));
        ubo.set(live.bakeEnabled, 0.0f);
    }
    // CAMERA-INSIDE: the marches' "camera inside a bubble" test is a property of ONE point per frame, so evaluate
    // the full field at the camera here (CPU mirror) instead of a per-fragment field re-sample at the ray origin
    // in both fragment shaders.
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
    ubo.set(live.cameraInside, bestCam - glm::max(forceIsoThreshold(force), secondCam) > 0.0f ? 1.0f : 0.0f);
}

// Terrain's live world (the splat texture, tessellation and surface water settings are registerUboFields' entries): the
// streamer's values, the splat material set + its climate boxes, the wetness clipmap's tick. Also reports the splat
// textures to the mip streamer.
void Renderer::buildUboTerrain()
{
    UboBlock& ubo = m_ubo;
    const UboRoot::TerrainLive& live = m_u.terrainLive;
    {
        const glm::vec4& params = m_terrain.getParams();
        ubo.set(live.meshRadius, params.x);
        ubo.set(live.lapseRate, params.y);
        ubo.set(live.seaLevel, params.z);
    }

    const TerrainSettings& terrainSettings = Globals::settings.terrain;

    // The splat set (setTerrainSplatMaterials): materials CONTIGUOUS in the material buffer - [base .. +numGround)
    // the climate-blended ground, [.. +numRock) the bedrock exposed by slope / crag, then the optional beach entry,
    // then the optional snow entry. Slot order only: the shader composites ground -> beach -> rock -> snow.
    ubo.set(live.splatBase, m_terrain.getSplatBaseMaterial() < 0 ? -1.0f : (float)m_terrain.getSplatBaseMaterial());
    ubo.set(live.numGround, (float)m_terrain.getSplatCounts().numGround);
    ubo.set(live.numRock, (float)m_terrain.getSplatCounts().numRock);
    ubo.set(live.hasBeach, m_terrain.getSplatCounts().hasBeach ? 1.0f : 0.0f);
    ubo.set(live.hasSnow, m_terrain.getSplatCounts().hasSnow ? 1.0f : 0.0f);
    // Four slots per uvec4 / vec4 (two per uvec4 for the texture pairs): packed here, one set per element.
    const uint16* heightTex = m_terrain.getSplatHeightTex();
    const glm::uvec2* splatTex = m_terrain.getSplatTex();
    const float* splatGrass = m_terrain.getSplatGrass();
    for (uint32 e = 0; e < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS / 4; ++e)
    {
        glm::uvec4 heights;
        glm::vec4 grass;
        for (uint32 k = 0; k < 4; ++k)
        {
            heights[k] = heightTex[e * 4 + k];
            grass[k] = glm::clamp(splatGrass[e * 4 + k], 0.0f, 1.0f);
        }
        ubo.set(live.splatHeightTex, e, heights);
        ubo.set(live.splatGrass, e, grass);
    }
    for (uint32 e = 0; e < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS / 2; ++e)
        ubo.set(live.splatTex, e, glm::uvec4(splatTex[e * 2].x, splatTex[e * 2].y, splatTex[e * 2 + 1].x, splatTex[e * 2 + 1].y));
    // Climate boxes: temperature arrives as t01, precipitation as mm/yr - its divisor is a live tweak.
    const float invPrecipFull = 1.0f / glm::max(terrainSettings.v3PrecipFullHumidity, 1.0f);
    const glm::vec4* climate = m_terrain.getSplatClimate();
    for (uint32 i = 0; i < RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS; ++i)
        ubo.set(live.splatClimate, i, glm::vec4(climate[i].x, climate[i].y,
            glm::clamp(climate[i].z * invPrecipFull, 0.0f, 1.0f), glm::clamp(climate[i].w * invPrecipFull, 0.0f, 1.0f)));

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
        // SIM delta (frozen by the global pause, like the particles), capped so a hitch cannot dry the map.
        const TerrainResources::WetnessTick tick = m_terrain.advanceWetness(
            oc::min((float)Globals::time.getSimDeltaSec(), 0.25f), sceneFocusOrCamera());
        const float dt = tick.dt;
        ubo.set(live.wetOrigin, glm::vec2((float)tick.origin.x, (float)tick.origin.y));
        ubo.set(live.wetPrevOrigin, glm::vec2((float)tick.prevOrigin.x, (float)tick.prevOrigin.y));
        ubo.set(live.wetDecay, terrainSettings.wetDryTime > 0.0f ? std::exp(-dt / terrainSettings.wetDryTime) : 0.0f);
        ubo.set(live.wetRain, glm::max(terrainSettings.wetRain, 0.0f) * dt);
        ubo.set(live.wetLayer, (float)tick.writeLayer);
        ubo.set(live.wetIn, terrainSettings.wetInTime > 0.0f ? dt / terrainSettings.wetInTime : 1.0f);
        // Diffusion spread as a per-tick mix fraction from a per-second rate: the tent's variance then
        // grows by ~rate * texel^2 per second at any framerate (a fixed per-frame fraction would spread
        // twice as fast at twice the fps).
        ubo.set(live.wetSpread, 1.0f - std::exp(-glm::max(terrainSettings.wetDiffusionRate, 0.0f) * dt));
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

// THE LOCKABLE VALUES: one entry per GLSL member u_<name> (UboBlock::add), each a settings variable itself or a lambda
// plus EVERY settings variable it reads. A value that is also computed for live work comes from a helper above (or a
// Settings helper like oceanWorldScaled), so the two cannot drift. Registered once (registerUboLocks), after the root
// values; the lambdas run every build and capture references to Globals::settings members or `this`.
void Renderer::registerUboFields(UboBlock& list)
{
    {
        // ---- Sky ("Sky")
        const SkyParams& s = Globals::settings.sky;
        list.add("sky_betaRayleigh", [&] { return skyBetaRayleigh(s); }, s.rayleighScatter);
        list.add("sky_betaMie", [&] { return skyBetaMie(s); }, s.mieScatter);
        list.add("sky_radiance", [&] { return s.skyRadianceColor * s.skyRadianceIntensity; }, s.skyRadianceColor, s.skyRadianceIntensity);
        list.add("sky_sunAngularCos", s.sunAngularCos);
        list.add("sky_groundAlbedo", [&] { return s.groundColor * s.groundIntensity; }, s.groundColor, s.groundIntensity);
        list.add("sky_groundHorizon", [&] { return glm::clamp(s.groundHorizon, 0.0f, 1.0f); }, s.groundHorizon);
        list.add("sky_moonDirection", [&] { return skyMoonDirection(s); }, s.moonDirection);
        list.add("sky_moonCos", [&] { return skyMoonCos(s); }, s.moonSizeDeg);
        list.add("sky_nebulaAxis", [&] { return glm::normalize(s.nebulaAxis); }, s.nebulaAxis);
        list.add("sky_sunGlow", s.sunGlow);
        list.add("sky_rolloffKnee", s.sunRolloffKnee);
        list.add("sky_rolloffHeadroom", s.sunRolloffHeadroom);
        list.add("sky_sunRolloff", s.sunRolloff);
        list.add("sky_scatterBoost", s.scatterBoost);
        list.add("sky_mieG", s.mieG);
        list.add("sky_moonBrightness", s.moonBrightness);
        list.add("sky_starDensity", s.starDensity);
        list.add("sky_starSize", s.starSize);
        list.add("sky_starSizeVariation", s.starSizeVar);
        list.add("sky_starBrightness", s.starBrightness);
        list.add("sky_starColorVariation", s.starColorVar);
        list.add("sky_nebulaIntensity", s.nebulaIntensity);
        list.add("sky_nebulaScale", s.nebulaScale);
        list.add("sky_nebulaBandWidth", s.nebulaBandWidth);
        list.add("sky_nebulaDust", s.nebulaDust);
        list.add("sky_rayleighHeight", s.rayleighHeight);
        list.add("sky_mieHeight", s.mieHeight);
        list.add("sky_mieExtinction", s.mieExtinction);
        list.add("sky_ozone", s.ozone);
    }
    {
        // ---- Clouds ("Sky/Clouds")
        const CloudParams& c = Globals::settings.clouds;
        list.add("clouds_shellBottom", [&] { return cloudShellBottom(c); },
            c.bottom, c.upperBottom, c.upperEnabled, c.upperDensity, c.upperCoverage);
        list.add("clouds_shellTop", [&] { return cloudShellTop(c); },
            c.bottom, c.top, c.upperBottom, c.upperTop, c.upperEnabled, c.upperDensity, c.upperCoverage);
        list.add("clouds_invShellHeight", [&] { return 1.0f / (cloudShellTop(c) - cloudShellBottom(c)); },
            c.bottom, c.top, c.upperBottom, c.upperTop, c.upperEnabled, c.upperDensity, c.upperCoverage);
        list.add("clouds_coverage", [&] { return cloudCoverage(c); }, c.coverage);
        list.add("clouds_mainBottom", [&] { return cloudMainBottom(c); }, c.bottom);
        list.add("clouds_mainInvHeight", [&] { return 1.0f / (cloudMainTop(c) - cloudMainBottom(c)); }, c.bottom, c.top);
        list.add("clouds_upperEnabled", [&] { return cloudUpperOn(c) ? 1.0f : 0.0f; }, c.upperEnabled, c.upperDensity, c.upperCoverage);
        list.add("clouds_upperDensity", c.upperDensity);
        list.add("clouds_upperBottom", [&] { return cloudUpperBottom(c); }, c.upperBottom);
        list.add("clouds_upperInvHeight", [&] { return 1.0f / (cloudUpperTop(c) - cloudUpperBottom(c)); }, c.upperBottom, c.upperTop);
        // The upper layer's coverage is a multiplier on the main layer's (its density already is one: the shader adds
        // it x upperDensity to the main density before "Density (1/m)").
        list.add("clouds_upperCoverage", [&] { return glm::clamp(c.upperCoverage * c.coverage, 0.0f, 1.0f); }, c.upperCoverage, c.coverage);
        list.add("clouds_upperType", [&] { return glm::clamp(c.upperType, 0.0f, 1.0f); }, c.upperType);
        list.add("clouds_upperHeightVariation", [&] { return glm::clamp(c.upperHeightVariation, 0.0f, 0.8f); }, c.upperHeightVariation);
        list.add("clouds_shelfCount", [&] { return (float)glm::clamp(c.shelfCount, 0, 3); }, c.shelfCount);
        list.add("clouds_shelfStrength", [&] { return glm::max(c.shelfStrength, 0.0f); }, c.shelfStrength);
        list.add("clouds_shelfHalfThickness", [&] { return glm::clamp(c.shelfThickness, 0.005f, 0.2f); }, c.shelfThickness);
        // The shelf stack is CENTRED in the layer: the lowest at 0.5 - (count - 1) / 2 x spacing.
        list.add("clouds_shelfLowest",
            [&] { return 0.5f - 0.5f * (float)glm::max(glm::clamp(c.shelfCount, 0, 3) - 1, 0) * glm::max(c.shelfSpacing, 0.0f); },
            c.shelfCount, c.shelfSpacing);
        list.add("clouds_shelfSpacing", [&] { return glm::max(c.shelfSpacing, 0.0f); }, c.shelfSpacing);
        list.add("clouds_invWeatherPeriod", [&] { return (float)(1.0 / cloudWeatherPeriod(c)); }, c.weatherSizeKm);
        list.add("clouds_baseFrequency", [&] { return (float)(1.0 / cloudBasePeriod(c)); }, c.weatherSizeKm, c.baseRepeats);
        list.add("clouds_detailFrequency", [&] { return (float)(1.0 / cloudDetailPeriod(c)); }, c.weatherSizeKm, c.baseRepeats, c.detailRepeats);
        list.add("clouds_extinction", c.densityScale);
        list.add("clouds_type", [&] { return glm::clamp(c.cloudType, 0.0f, 1.0f); }, c.cloudType);
        list.add("clouds_typeVariation", c.typeVariation);
        list.add("clouds_erosion", c.erosion);
        list.add("clouds_curl", c.curl);
        list.add("clouds_coverageVariation", c.coverageVariation);
        list.add("clouds_nearDetailRadius", c.nearDetailRadius);
        list.add("clouds_invNearDetailRadius", [&] { return 1.0f / glm::max(c.nearDetailRadius, 1e-3f); }, c.nearDetailRadius);
        list.add("clouds_baseVariation", [&] { return glm::clamp(c.baseVariation, 0.0f, 0.6f); }, c.baseVariation);
        list.add("clouds_invGroundLightDepth", [&] { return 1.0f / glm::max(c.groundLightDepth, 1.0f); }, c.groundLightDepth);
        list.add("clouds_erosionCutoff", [&] { return glm::clamp(c.erosionCutoff, 0.0f, 0.9f); }, c.erosionCutoff);
        list.add("clouds_towerVariation", [&] { return glm::clamp(c.towerVariation, 0.0f, 0.9f); }, c.towerVariation);
        // Top roundness 0..1 -> the superellipse exponent 1..6 (1 = the plain taper, 2 = a circular cap, 6 = nearly flat).
        list.add("clouds_topRoundness", [&] { return 1.0f + 5.0f * glm::clamp(c.topRoundness, 0.0f, 1.0f); }, c.topRoundness);
        list.add("clouds_baseSharpness", [&] { return glm::clamp(c.baseSharpness, 0.0f, 1.0f); }, c.baseSharpness);
        list.add("clouds_towerCoreLink", [&] { return glm::clamp(c.towerCoreLink, 0.0f, 1.0f); }, c.towerCoreLink);
        list.add("clouds_minStep", [&] { return glm::max(c.minStep, 0.25f); }, c.minStep);
        list.add("clouds_aerialStrength", [&] { return glm::max(c.aerialStrength, 0.0f); }, c.aerialStrength);
        list.add("clouds_giSkyObserverRadius", [&] { return glm::max(c.giSkyObserverRadius, 0.0f); }, c.giSkyObserverRadius);
        // The HG + Draine fit to Mie scattering on water droplets (Jendersie & d'Eon 2023, "An Approximate Mie
        // Scattering Function for Fog and Cloud Rendering"), valid for diameters 5 .. 50 um.
        // The HG part's g is the droplets' DIFFRACTION peak (0.995 at 20 um: ~5400 / sr in a ~0.3 degree lobe, the size
        // of the sun disc). Behind thin cloud it scattered so much sunlight into that lobe that the lobe clipped to
        // white after the exposure - a bigger, brighter sun. "Forward peak limit" caps it: a wider, softer silver lining.
        list.add("clouds_hgG",
            [&] { return glm::min(std::exp(-0.0990567f / (glm::clamp(c.dropletSize, 5.0f, 50.0f) - 1.67154f)), glm::clamp(c.forwardPeakLimit, 0.0f, 1.0f)); },
            c.dropletSize, c.forwardPeakLimit);
        list.add("clouds_draineG", [&] { return std::exp(-2.20679f / (glm::clamp(c.dropletSize, 5.0f, 50.0f) + 3.91029f) - 0.428934f); }, c.dropletSize);
        list.add("clouds_draineAlpha", [&] { return std::exp(3.62489f - 8.29288f / (glm::clamp(c.dropletSize, 5.0f, 50.0f) + 5.52825f)); }, c.dropletSize);
        list.add("clouds_draineWeight", [&] { return std::exp(-0.599085f / (glm::clamp(c.dropletSize, 5.0f, 50.0f) - 0.641583f) - 0.665888f); }, c.dropletSize);
        list.add("clouds_ambient", c.ambient);
        list.add("clouds_groundAlbedo", c.groundAlbedo);
        list.add("clouds_powder", [&] { return c.powder; }); // no tweak (its registration is commented out): a constant
        list.add("clouds_multiScatterAttenuation", c.multiScatter);
        list.add("clouds_multiScatterStrength", [&] { return glm::max(c.multiScatterStrength, 0.0f); }, c.multiScatterStrength);
        list.add("clouds_maxSteps", [&] { return (float)glm::max(c.maxSteps, 1); }, c.maxSteps);
        list.add("clouds_maxDistance", [&] { return c.maxDistanceKm * 1000.0f; }, c.maxDistanceKm);
        list.add("clouds_nearStep", [&] { return glm::max(c.nearStep, 0.5f); }, c.nearStep);
        list.add("clouds_stepsPerRay", [&] { return (float)glm::max(c.stepsPerRay, 1); }, c.stepsPerRay);
        list.add("clouds_lightSteps", [&] { return (float)glm::max(c.lightSteps, 0); }, c.lightSteps);
        list.add("clouds_lightDistance", c.lightDistance);
        list.add("clouds_temporalBlend", [&] { return glm::clamp(c.temporalBlend, 0.0f, 0.98f); }, c.temporalBlend);
        list.add("clouds_invDetailDistance", [&] { return 1.0f / (glm::max(c.detailDistanceKm, 0.5f) * 1000.0f); }, c.detailDistanceKm);
        list.add("clouds_shadowStrength", [&] { return glm::clamp(c.shadowStrength, 0.0f, 1.0f); }, c.shadowStrength);
        // Past the cascades: a rough mean transmittance of the layer from its coverage (not measured).
        list.add("clouds_shadowMeanTransmittance", [&] { return glm::mix(1.0f, 0.3f, cloudCoverage(c)); }, c.coverage);
        list.add("clouds_shadowNearSteps", [&] { return (float)glm::max(c.shadowNearSteps, 1); }, c.shadowNearSteps);
        list.add("clouds_shadowFarSteps", [&] { return (float)glm::max(c.shadowFarSteps, 1); }, c.shadowFarSteps);
        list.add("clouds_shadowFarSoftness", [&] { return glm::max(c.shadowFarSoftness, 0.0f); }, c.shadowFarSoftness);
    }
    {
        // ---- Shadows ("Shadows"; the game's preset writes the same tweak variables)
        const ShadowParams& sh = Globals::settings.shadow;
        list.add("shadow_depthBias", sh.depthBias);
        list.add("shadow_normalBias", sh.normalBias);
        list.add("shadow_invResolution", [] { return 1.0f / (float)RendererVKLayout::SHADOW_MAP_RESOLUTION; });
        list.add("shadow_terrainMarchStart", [&] { return glm::max(sh.terrainMarchStart, 0.0f); }, sh.terrainMarchStart);
        list.add("shadow_terrainMarchBias", [&] { return glm::max(sh.terrainMarchBias, 1.0f); }, sh.terrainMarchBias);
        list.add("shadow_terrainMarchSpread", [&] { return glm::max(sh.terrainMarchSpread, 0.002f); }, sh.terrainMarchSpread);
    }
    {
        // ---- Ray tracing ("RT" + "RTAO" + "GI"). RTAO and the GI probe contribution both need the acceleration
        // structures, so both fold in the RT master toggle; GI additionally gates on its own switch.
        const RTParams& r = Globals::settings.rt;
        const RTAOParams& ao = Globals::settings.rtao;
        list.add("rt_sunShadow", [&] { return rtSunShadowActive(r) ? 1.0f : 0.0f; }, r.enabled, r.rtSunShadow);
        list.add("rt_lightShadows", [&] { return r.enabled && r.rtLightShadows ? 1.0f : 0.0f; }, r.enabled, r.rtLightShadows);
        list.add("rt_skyRadiance", [&] { return r.enabled && r.rtSkyRadiance ? 1.0f : 0.0f; }, r.enabled, r.rtSkyRadiance);
        list.add("rt_sunShadowRays", r.sunShadowRays);
        list.add("rt_aoEnabled", [&] { return r.enabled && ao.enabled ? 1.0f : 0.0f; }, r.enabled, ao.enabled);
        // Past the RTAO max distance the trace writes exactly (N, 1.0) (rtao.cs.glsl early-out), so the forward pass
        // skips its depth-aware AO upsample there and uses those values directly.
        list.add("rt_aoMaxDistance", ao.maxDistance);
        list.add("rt_aoFadeStart", ao.fadeStart);
        list.add("rt_aoRays", [&] { return (float)oc::max(ao.rays, 1); }, ao.rays);
        list.add("rt_aoRadius", ao.radius);
        list.add("rt_aoPower", ao.power);
        list.add("rt_aoIntensity", ao.intensity);
        list.add("rt_aoNormalBias", ao.normalBias);
        list.add("rt_aoDistanceBias", ao.distanceBias);
        list.add("rt_aoMaxHistory", ao.maxHistory);
        list.add("rt_aoBlurRadius", [&] { return (float)oc::max(ao.blurRadius, 0); }, ao.blurRadius);
        m_giProbePipeline.registerUboFields(list, r.enabled, r.giEnabled); // rt_gi*: its tweaks are private
    }
    {
        // ---- Fog ("Fog")
        const FogParams& f = Globals::settings.fog;
        list.add("fog_albedo", [&] { return f.albedo * f.albedoIntensity; }, f.albedo, f.albedoIntensity);
        list.add("fog_anisotropy", f.anisotropy);
        list.add("fog_density", f.density);
        list.add("fog_heightBase", f.heightBase);
        list.add("fog_heightFalloff", [&] { return f.heightFalloff * f.heightFalloff; }, f.heightFalloff);
        list.add("fog_range", f.range);
        list.add("fog_noiseScale", f.noiseScale);
        list.add("fog_noiseStrength", f.noiseStrength);
        list.add("fog_temporalBlend", f.temporalBlend);
        list.add("fog_terrainFollow", [&] { return glm::clamp(f.terrainFollow, 0.0f, 1.0f); }, f.terrainFollow);
        list.add("fog_enabled", f.enabled);
        list.add("fog_lightShadows", f.lightShadows);
        list.add("fog_sunRays", f.sunRays);
        list.add("fog_spatialFilter", f.spatialFilter);
        list.add("fog_giAmbient", f.giAmbient);
        list.add("fog_sunSoftness", f.sunSoftness);
        list.add("fog_slicePower", [&] { return glm::clamp(f.slicePower, 0.25f, 2.0f); }, f.slicePower);
        list.add("fog_terrainShadowDistance", f.terrainShadowDist);
        list.add("fog_regionStrength", [&] { return glm::clamp(f.regionStrength, 0.0f, 1.0f); }, f.regionStrength); // the baked regional thickness modulation
        list.add("fog_underwaterDensity", [&] { return glm::max(f.underwaterDensity, 0.0f); }, f.underwaterDensity);
        list.add("fog_shaftBoost", [&] { return glm::max(f.shaftBoost, 0.0f); }, f.shaftBoost);
        list.add("fog_causticStrength", [&] { return glm::max(f.causticStrength, 0.0f); }, f.causticStrength);
        list.add("fog_farFieldMaxDistance", [&] { return f.farFieldMaxDistanceKm > 0.0f ? f.farFieldMaxDistanceKm * 1000.0f : 1e30f; }, f.farFieldMaxDistanceKm);
        list.add("fog_sunScatter", [&] { return glm::max(f.sunScatter, 0.0f); }, f.sunScatter);
        list.add("fog_farFieldEnabled", f.farField);
        list.add("fog_farFieldDensity", [&] { return glm::max(f.farFieldDensity, 0.0f); }, f.farFieldDensity);
        list.add("fog_farFieldFalloffScale", [&] { return 1.0f / glm::clamp(f.farFieldThickness, 0.01f, 100.0f); }, f.farFieldThickness);
        list.add("fog_farFieldSteps", [&] { return (float)glm::max(f.farFieldSteps, 1); }, f.farFieldSteps);
        list.add("fog_hazeDensity", [&] { return glm::max(f.shaftHazeDensity, 0.0f); }, f.shaftHazeDensity);
        list.add("fog_hazeInvHeight", [&] { return 1.0f / glm::max(f.shaftHazeHeight, 1.0f); }, f.shaftHazeHeight);
        list.add("fog_aerialStrength", [&] { return glm::max(f.aerialStrength, 0.0f); }, f.aerialStrength);
        list.add("fog_aerialMaxDistance", [&] { return glm::max(f.aerialMaxDistanceKm, 1.0f) * 1000.0f; }, f.aerialMaxDistanceKm);
    }
    {
        // ---- Ocean (o = the "Ocean" settings in MODEL units; oceanWorldScaled is THE world-scale conversion, shared
        // with OceanGenerator::pushOceanParams; spray = the "Ocean" spray tweaks). The "Spray *" tweaks are MODEL units
        // like every ocean tweak: metres and m/s ride the world scale (the sea keeps its model periods, so speed scales
        // as length), the per-m^2 rate rides 1/s^2 so the spawns per model area stay the same.
        const OceanSettings& o = Globals::settings.ocean;
        const OceanSprayParams& spray = Globals::settings.oceanSpray;
        const auto ws = [&o] { return oceanWorldScaled(o); };
        list.add("ocean_cascadeSizes", [ws] { return glm::max(ws().cascadeSizes, glm::vec3(1.0f)); }, o.cascadeSizes, o.worldScale);
        list.add("ocean_amplitude", o.amplitude);
        list.add("ocean_absorption", [ws] { return ws().absorption; }, o.absorption, o.worldScale);
        list.add("ocean_roughness", o.roughness);
        list.add("ocean_scatterColor", o.scatterColor);
        list.add("ocean_scatterStrength", o.scatterStrength);
        list.add("ocean_foamColor", o.foamColor);
        list.add("ocean_foamBias", o.foamBias);
        list.add("ocean_choppiness", o.choppiness);
        list.add("ocean_fetch", [ws] { return glm::max(ws().fetchKm, 1.0f) * 1000.0f; }, o.fetchKm, o.worldScale);
        list.add("ocean_depth", [ws] { return glm::max(ws().depth, 1.0f); }, o.depth, o.worldScale);
        list.add("ocean_normalStrength", o.normalStrength);
        list.add("ocean_horizonLevelOffset", [ws] { return ws().horizonLevelOffset; }, o.horizonLevelOffset, o.worldScale);
        list.add("ocean_horizonDepth", [ws] { return glm::max(ws().horizonDepth, 0.0f); }, o.horizonDepth, o.worldScale);
        list.add("ocean_horizonDepthRange", [ws] { return glm::max(ws().horizonDepthRange, 0.0f); }, o.horizonDepthRange, o.worldScale);
        list.add("ocean_detailBias", [&] { return glm::clamp(o.detailBias, -4.0f, 4.0f); }, o.detailBias);
        list.add("ocean_shoalScale", [&] { return glm::max(o.shoalScale, 0.0f); }, o.shoalScale);
        list.add("ocean_foamSoftness", [&] { return glm::max(o.foamSoftness, 0.02f); }, o.foamSoftness);
        list.add("ocean_foamBreakAccel", [&] { return glm::max(o.foamBreakAccel, 0.01f); }, o.foamBreakAccel);
        list.add("ocean_farCullError", [ws] { return glm::max(ws().farCullError, 0.0f); }, o.farCullError, o.worldScale);
        list.add("ocean_glintFilter", [&] { return glm::max(o.glintFilter, 0.0f); }, o.glintFilter);
        list.add("ocean_sssStrength", [ws] { return glm::max(ws().sssStrength, 0.0f); }, o.sssStrength, o.worldScale);
        list.add("ocean_sssPower", [&] { return glm::max(o.sssPower, 1.0f); }, o.sssPower);
        list.add("ocean_cullMargin", [ws] { return glm::max(ws().cullMargin, 0.0f); }, o.cullMargin, o.worldScale);
        list.add("ocean_swashAmp", [this] { return getOceanSwashAmp(); }, o.swashAmp);
        list.add("ocean_swashFlow", [&] { return glm::max(o.swashFlow, 0.0f); }, o.swashFlow);
        list.add("ocean_shoreFoamBias", [&] { return glm::clamp(o.shoreFoamBias, -1.0f, 1.0f); }, o.shoreFoamBias);
        list.add("ocean_rtReflectionFog", [&] { return glm::max(o.rtReflectionFog, 0.0f); }, o.rtReflectionFog);
        list.add("ocean_rtRayCutoff", [ws] { return glm::max(ws().rtRayCutoffDist, 0.0f); }, o.rtRayCutoffDist, o.worldScale);
        list.add("ocean_rtRefractionRange", [ws] { return glm::max(ws().rtRefractionRange, 1.0f); }, o.rtRefractionRange, o.worldScale); // the tweak's own minimum
        list.add("ocean_rtReflectionRange", [ws] { return glm::max(ws().rtReflectionRange, 50.0f); }, o.rtReflectionRange, o.worldScale);
        list.add("ocean_rtReflectionMaxRough", [&] { return glm::clamp(o.rtReflectionMaxRough, 0.0f, 1.0f); }, o.rtReflectionMaxRough);
        list.add("ocean_microRoughness", [&] { return glm::max(o.microRoughness, 0.0f); }, o.microRoughness);
        list.add("ocean_crestSlopeLimit", [&] { return glm::max(o.crestSlopeLimit, 0.0f); }, o.crestSlopeLimit);
        list.add("ocean_timeScale", [ws] { return ws().timeScale; }, o.worldScale);
        list.add("ocean_undersideTransmission", [&] { return glm::clamp(o.undersideTransmission, 0.0f, 1.0f); }, o.undersideTransmission);
        list.add("ocean_detailStrength", [&] { return glm::max(o.detailStrength, 0.0f); }, o.detailStrength);
        list.add("ocean_detailScale", [&] { return glm::max(o.detailScale, 0.001f); }, o.detailScale);
        list.add("ocean_detailFadeDistance", [ws] { return glm::max(ws().detailFadeDist, 0.0f); }, o.detailFadeDist, o.worldScale);
        list.add("ocean_detailRotation", o.detailRotation);
        list.add("ocean_bubbleDepth", [this] { return getOceanBubbleDepth(); }, o.bubbleDepth, o.worldScale);
        list.add("ocean_bubbleBrightness", [this] { return getOceanBubbleBrightness(); }, o.bubbleBrightness);
        list.add("ocean_foamFlatten", [&] { return glm::clamp(o.foamFlatten, 0.0f, 1.0f); }, o.foamFlatten);
        list.add("ocean_foamTexel", [ws] { return glm::max(ws().foamTexel, 0.01f); }, o.foamTexel, o.worldScale);
        list.add("ocean_foamDecay", [&] { return glm::clamp(o.foamSurfaceDecay, 0.0f, 0.9999f); }, o.foamSurfaceDecay);
        list.add("ocean_foamStrength", [&] { return glm::max(o.foamSurfaceStrength, 0.0f); }, o.foamSurfaceStrength);
        list.add("ocean_foamThreshold", [&] { return glm::max(o.foamThreshold, 0.0f); }, o.foamThreshold);
        list.add("ocean_foamEdge", [&] { return glm::max(o.foamEdge, 0.0f); }, o.foamEdge);
        list.add("ocean_foamDetail", [&] { return glm::max(o.foamDetail, 0.0f); }, o.foamDetail);
        list.add("ocean_foamFineWaves", [&] { return glm::clamp(o.foamFineWaves, 0.0f, 1.0f); }, o.foamFineWaves);
        list.add("ocean_bubbleBlur", [ws] { return glm::max(ws().bubbleBlur, 0.0f); }, o.bubbleBlur, o.worldScale);
        list.add("ocean_sprayRate", [this, &spray] { const float s = getOceanWorldScale(); return glm::max(spray.rate, 0.0f) / (s * s); }, spray.rate, o.worldScale);
        list.add("ocean_sprayRadius", [this, &spray] { return glm::max(spray.radius * getOceanWorldScale(), 1.0f); }, spray.radius, o.worldScale);
        list.add("ocean_sprayThreshold", [&] { return glm::clamp(spray.threshold, 0.0f, 0.99f); }, spray.threshold);
        list.add("ocean_sprayKick", [this, &spray] { return glm::max(spray.kick, 0.0f) * getOceanWorldScale(); }, spray.kick, o.worldScale);
        list.add("ocean_spraySpeed", [this, &spray] { return glm::max(spray.speed, 0.0f) * getOceanWorldScale(); }, spray.speed, o.worldScale);
        list.add("ocean_sprayForward", [this, &spray] { return spray.forward * getOceanWorldScale(); }, spray.forward, o.worldScale);
        list.add("ocean_sprayHeight", [this, &spray] { return spray.height * getOceanWorldScale(); }, spray.height, o.worldScale);
        list.add("ocean_worldScale", [this] { return getOceanWorldScale(); }, o.worldScale);
    }
    {
        // ---- Terrain textures ("Terrain/Textures"). Every start/full pair is ordered here rather than in the shader: an
        // inverted pair from the tweak UI would otherwise make smoothstep divide by a negative span and flip the layer
        // inside out. The four crag values are LIVE: they also ride V3's world scale (setTerrainCragScale), which the
        // loaded model sets after the startup bake.
        const TerrainSettings& t = Globals::settings.terrain;
        list.add("terrainTex_climateSigma", [&] { return glm::max(t.texClimateBlend, 1e-3f); }, t.texClimateBlend);
        list.add("terrainTex_uvScaleGround", t.texUvScaleGround);
        list.add("terrainTex_uvScaleRock", t.texUvScaleRock);
        list.add("terrainTex_uvScaleSnow", t.texUvScaleSnow);
        list.add("terrainTex_slopeRockStart", t.texSlopeRockStart);
        list.add("terrainTex_slopeRockFull", [&] { return glm::max(t.texSlopeRockFull, t.texSlopeRockStart + 1e-3f); }, t.texSlopeRockFull, t.texSlopeRockStart);
        list.addLive("terrainTex_cragStart", [this, &t] { return t.texCragStart * m_terrainCragScale; });
        list.addLive("terrainTex_cragFull", [this, &t] { return glm::max(t.texCragFull, t.texCragStart + 1e-3f) * m_terrainCragScale; });
        // The wander breaks the rock boundary off the elevation contour the crag test would otherwise trace.
        list.addLive("terrainTex_cragWanderAmp", [this, &t] { return glm::max(t.texCragWanderAmp * m_terrainCragScale, 0.0f); });
        list.addLive("terrainTex_invCragWanderWavelength", [this, &t] { return 1.0f / glm::max(t.texCragWanderWavelength * m_terrainCragScale, 1.0f); });
        list.add("terrainTex_beachBand", t.texBeachBand);
        list.add("terrainTex_snowTempFull", t.texSnowTempFull);
        list.add("terrainTex_snowTempNone", [&] { return glm::max(t.texSnowTempNone, t.texSnowTempFull + 1e-3f); }, t.texSnowTempNone, t.texSnowTempFull);
        list.add("terrainTex_snowSlopeStart", t.texSnowSlopeStart);
        list.add("terrainTex_snowSlopeFull", [&] { return glm::max(t.texSnowSlopeFull, t.texSnowSlopeStart + 1e-3f); }, t.texSnowSlopeFull, t.texSnowSlopeStart);
        list.add("terrainTex_snowAridity", t.texSnowAridity);
        list.add("terrainTex_parallaxDepthGround", [&] { return glm::max(t.texParallaxDepthGround, 0.0f); }, t.texParallaxDepthGround);
        list.add("terrainTex_parallaxDepthRock", [&] { return glm::max(t.texParallaxDepthRock, 0.0f); }, t.texParallaxDepthRock);
        list.add("terrainTex_parallaxFadeStart", [&] { return glm::max(t.texParallaxFadeStart, 0.0f); }, t.texParallaxFadeStart);
        list.add("terrainTex_parallaxFadeEnd",
            [&] { return t.texParallaxEnabled && t.texParallaxFadeEnd > 0.0f ? glm::max(t.texParallaxFadeEnd, t.texParallaxFadeStart + 0.1f) : 0.0f; },
            t.texParallaxEnabled, t.texParallaxFadeEnd, t.texParallaxFadeStart);
        list.add("terrainTex_parallaxSteps", [&] { return glm::clamp((float)t.texParallaxSteps, 2.0f, 64.0f); }, t.texParallaxSteps);
        list.add("terrainTex_parallaxShadow", [&] { return glm::clamp(t.texParallaxShadow, 0.0f, 1.0f); }, t.texParallaxShadow);
        list.add("terrainTex_heightBlendContrast", [&] { return glm::max(t.texHeightBlendContrast, 0.0f); }, t.texHeightBlendContrast);
    }
    {
        // ---- Terrain tessellation ("Terrain/Tessellation")
        const TerrainSettings& t = Globals::settings.terrain;
        list.add("terrainTess_enabled", t.texTessEnabled);
        list.add("terrainTess_maxFactor", [&] { return glm::clamp((float)t.texTessMaxFactor, 1.0f, 64.0f); }, t.texTessMaxFactor);
        list.add("terrainTess_targetEdgePx", [&] { return glm::max(t.texTessTargetPx, 1.0f); }, t.texTessTargetPx);
        list.add("terrainTess_factorFalloff", [&] { return glm::clamp(t.texTessFalloffExponent, 0.05f, 16.0f); }, t.texTessFalloffExponent);
        list.add("terrainTess_fadeStart", [&] { return glm::max(t.texTessFadeStart, 0.0f); }, t.texTessFadeStart);
        list.add("terrainTess_fadeEnd", [&] { return glm::max(t.texTessFadeEnd, t.texTessFadeStart + 0.1f); }, t.texTessFadeEnd, t.texTessFadeStart);
        list.add("terrainTess_depthGround", [&] { return glm::max(t.texTessDepthGround, 0.0f); }, t.texTessDepthGround);
        list.add("terrainTess_depthRock", [&] { return glm::max(t.texTessDepthRock, 0.0f); }, t.texTessDepthRock);
        list.add("terrainTess_freezeDistance", [&] { return glm::max(t.texTessFreezeDistance, 0.1f); }, t.texTessFreezeDistance);
        list.add("terrainTess_heightFalloff", [&] { return glm::clamp(t.texTessHeightFalloffExponent, 0.05f, 16.0f); }, t.texTessHeightFalloffExponent);
    }
    {
        // ---- Terrain water ("Terrain/Water")
        const TerrainSettings& w = Globals::settings.terrain;
        list.add("terrainWater_enabled", w.wetEnabled);
        list.add("terrainWater_texelSize", [&] { return terrainWetTexelSize(w); }, w.wetTexelSize);
        list.add("terrainWater_invTexelSize", [&] { return 1.0f / terrainWetTexelSize(w); }, w.wetTexelSize);
        list.add("terrainWater_darkening", [&] { return glm::clamp(w.wetDarkening, 0.0f, 1.0f); }, w.wetDarkening);
        list.add("terrainWater_waterRoughness", [&] { return glm::clamp(w.wetRoughness, 0.0f, 1.0f); }, w.wetRoughness);
        list.add("terrainWater_dryTempSensitivity", [&] { return glm::max(w.wetDryTempSens, 0.0f); }, w.wetDryTempSens);
        list.add("terrainWater_slopeDrain", [&] { return glm::max(w.wetSlopeDrain, 0.0f); }, w.wetSlopeDrain);
        list.add("terrainWater_fillStart", [&] { return glm::clamp(w.wetFillStart, 0.0f, 0.99f); }, w.wetFillStart);
        list.add("terrainWater_fillFull", [&] { return glm::clamp(w.wetFillFull, w.wetFillStart + 0.01f, 1.0f); }, w.wetFillFull, w.wetFillStart);
        list.add("terrainWater_fillCurve", [&] { return glm::clamp(w.wetFillCurve, 0.05f, 16.0f); }, w.wetFillCurve);
        list.add("terrainWater_edgeFade", [&] { return glm::max(w.wetEdgeFade, 1e-4f); }, w.wetEdgeFade);
        list.add("terrainWater_oceanBlend", [&] { return glm::max(w.wetOceanBlend, 0.01f); }, w.wetOceanBlend);
        list.add("terrainWater_waviness", [&] { return glm::clamp(w.wetWaviness, 0.0f, 1.0f); }, w.wetWaviness);
        list.add("terrainWater_normalScale", [&] { return glm::max(w.wetNormalScale, 0.0f); }, w.wetNormalScale);
        list.add("terrainWater_rippleStrength", [&] { return glm::max(w.wetRippleStrength, 0.0f); }, w.wetRippleStrength);
        list.add("terrainWater_oceanEdgeFade", [&] { return glm::max(w.wetOceanEdgeFade, 0.0f); }, w.wetOceanEdgeFade);
        // Film max slope as mesh normal.y thresholds for terrainPoolLevel: no pool below cos(max), the full level above
        // cos(max - fade). 90 = off (both below any normal).
        list.add("terrainWater_slopeCut", [&] {
            const float maxSlope = terrainFilmMaxSlopeDeg(w);
            return maxSlope >= 90.0f ? -2.0f : std::cos(glm::radians(maxSlope));
        }, w.wetFilmMaxSlope);
        list.add("terrainWater_slopeFull", [&] {
            const float maxSlope = terrainFilmMaxSlopeDeg(w);
            return maxSlope >= 90.0f ? -1.5f : std::cos(glm::radians(glm::max(maxSlope - glm::max(w.wetFilmSlopeFade, 0.0f), 0.0f))) + 1e-4f;
        }, w.wetFilmMaxSlope, w.wetFilmSlopeFade);
        list.add("terrainWater_flowSpeed", [&] { return glm::max(w.wetFilmFlowSpeed, 0.0f); }, w.wetFilmFlowSpeed);
        list.add("terrainWater_flowCycle", [&] { return glm::max(w.wetFilmFlowCycle, 0.05f); }, w.wetFilmFlowCycle);
        // Film flow slope gate as tan values: none below half the min slope, full at it.
        list.add("terrainWater_flowSlopeHalfTan", [&] { return std::tan(0.5f * terrainFilmFlowMinSlopeRad(w)); }, w.wetFilmFlowMinSlope);
        list.add("terrainWater_flowSlopeTan", [&] { return std::tan(terrainFilmFlowMinSlopeRad(w)) + 1e-4f; }, w.wetFilmFlowMinSlope);
        list.add("terrainWater_wetRoughness", [&] { return glm::clamp(w.wetGroundRoughness, 0.0f, 1.0f); }, w.wetGroundRoughness);
        list.add("terrainWater_underwaterRoughness", [&] { return glm::clamp(w.wetUnderwaterRoughness, 0.0f, 1.0f); }, w.wetUnderwaterRoughness);
        list.add("terrainWater_roughnessEdge", [&] { return glm::clamp(w.wetRoughnessEdge, 0.001f, 1.0f); }, w.wetRoughnessEdge);
        list.add("terrainWater_darkeningEdge", [&] { return glm::clamp(w.wetDarkeningEdge, 0.001f, 1.0f); }, w.wetDarkeningEdge);
        list.add("terrainWater_darkeningThreshold", [&] { return glm::clamp(w.wetDarkeningThreshold, 1e-3f, 1.0f); }, w.wetDarkeningThreshold);
        list.add("terrainWater_roughnessThreshold", [&] { return glm::clamp(w.wetRoughnessThreshold, 1e-3f, 1.0f); }, w.wetRoughnessThreshold);
        list.add("terrainWater_wetNormalScale", [&] { return glm::clamp(w.wetGroundNormalScale, 0.0f, 4.0f); }, w.wetGroundNormalScale);
        list.add("terrainWater_dryingPattern", [&] { return glm::clamp(w.wetDryingPattern, 0.0f, 1.0f); }, w.wetDryingPattern);
        list.add("terrainWater_invDryingPatternSize", [&] { return 1.0f / glm::max(w.wetDryingPatternSize, 0.05f); }, w.wetDryingPatternSize);
        list.add("terrainWater_dryingPatternRelief", [&] { return glm::clamp(w.wetDryingPatternRelief, 0.0f, 1.0f); }, w.wetDryingPatternRelief);
        list.add("terrainWater_dryingContrastHalf", [&] { return 0.5f * glm::max(w.wetDryingPatternContrast, 0.0f); }, w.wetDryingPatternContrast);
        list.add("terrainWater_invGlintSize", [&] { return 1.0f / glm::max(w.wetGlintSize, 0.005f); }, w.wetGlintSize);
        list.add("terrainWater_glintCoverage", [&] { return glm::clamp(w.wetGlintCoverage, 0.0f, 1.0f); }, w.wetGlintCoverage);
        list.add("terrainWater_glintRoughness", [&] { return glm::clamp(w.wetGlintRoughness, 0.01f, 1.0f); }, w.wetGlintRoughness);
    }
    {
        // ---- Grass ("Grass")
        const GrassParams& g = Globals::settings.grass;
        list.add("grass_rootAlbedo", [&] { return srgbToLinear(g.rootColor); }, g.rootColor);
        list.add("grass_roughness", g.roughness);
        list.add("grass_tipAlbedo", [&] { return srgbToLinear(g.tipColor); }, g.tipColor);
        list.add("grass_colorVariation", g.colorVariation);
        list.add("grass_dryAlbedo", [&] { return srgbToLinear(g.dryColor); }, g.dryColor);
        list.add("grass_dryAmount", [&] { return glm::clamp(g.dryAmount, 0.0f, 1.0f); }, g.dryAmount);
        // The clamp GrassPipeline::setBladesPerPatch applies to its index buffer.
        list.add("grass_bladesPerPatch", [&] { return (float)glm::clamp(g.bladesPerPatch, 1, (int)RendererVKLayout::GRASS_MAX_BLADES); }, g.bladesPerPatch);
        list.add("grass_patchSize", [this] { return grassPatchSize(); }, g.patchSize);
        list.add("grass_range", [this] { return grassGridRange(); }, g.range, g.patchSize);
        list.add("grass_rangeFade", [this, &g] { return glm::clamp(g.rangeFade, 0.0f, grassGridRange()); }, g.rangeFade, g.range, g.patchSize);
        list.add("grass_bladeHeight", g.bladeHeight);
        list.add("grass_heightVariation", [&] { return glm::clamp(g.heightVariation, 0.0f, 1.0f); }, g.heightVariation);
        list.add("grass_bladeWidth", g.bladeWidth);
        list.add("grass_rootSink", g.rootSink);
        list.add("grass_thinStart", [&] { return glm::max(g.thinStart, 0.1f); }, g.thinStart);
        list.add("grass_thinExponent", g.thinExponent);
        list.add("grass_widthCompensation", g.widthCompensation);
        list.add("grass_maxWidthScale", [&] { return glm::max(g.maxWidthScale, 1.0f); }, g.maxWidthScale);
        list.add("grass_lod1Distance", g.lod1Distance);
        list.add("grass_lod2Distance", [&] { return grassLod2Distance(g); }, g.lod2Distance, g.lod1Distance);
        list.add("grass_lod3Distance", [&] { return glm::max(g.lod3Distance, grassLod2Distance(g)); }, g.lod3Distance, g.lod2Distance, g.lod1Distance);
        list.add("grass_lodMorphBand", [&] { return glm::clamp(g.lodMorphBand, 0.0f, 1.0f); }, g.lodMorphBand);
        list.add("grass_groundBlendDistance", g.groundBlendDistance);
        list.add("grass_groundBlend", g.groundBlend);
        // The direction, speed and gusts are the shared wind's (wind.inc.glsl); these the blades' response to it.
        list.add("grass_windBend", [&] { return glm::max(g.windBend, 0.0f); }, g.windBend);
        list.add("grass_rippleBend", [&] { return glm::max(g.rippleBend, 0.0f); }, g.rippleBend);
        list.add("grass_invRippleSize", [&] { return 1.0f / glm::max(g.rippleSize, 0.01f); }, g.rippleSize);
        list.add("grass_swayFrequency", g.swayFrequency);
        list.add("grass_windFadeStart", g.windFadeStart);
        list.add("grass_windFadeEnd", [&] { return glm::max(g.windFadeEnd, g.windFadeStart + 0.01f); }, g.windFadeEnd, g.windFadeStart);
        list.add("grass_curvature", g.curvature);
        list.add("grass_invClumpSize", [&] { return 1.0f / glm::max(g.clumpSize, 0.01f); }, g.clumpSize);
        list.add("grass_patchiness", [&] { return glm::clamp(g.patchiness, 0.0f, 1.0f); }, g.patchiness);
        list.add("grass_growBand", [&] { return glm::max(g.growBand, 0.01f); }, g.growBand);
        list.add("grass_bareFraction", [&] { return glm::clamp(g.bareFraction, 0.0f, 1.0f); }, g.bareFraction);
        list.add("grass_sizeByCover", [&] { return glm::clamp(g.sizeByCover, 0.0f, 1.0f); }, g.sizeByCover);
        list.add("grass_rootOcclusion", g.rootOcclusion);
        list.add("grass_transmission", g.transmission);
        list.add("grass_roundness", g.roundness);
        list.add("grass_coldTemperature", g.coldTemperature);
        list.add("grass_warmTemperature", g.warmTemperature);
        list.add("grass_coldDarkening", [&] { return glm::clamp(g.coldDarkening, 0.0f, 1.0f); }, g.coldDarkening);
        list.add("grass_shadowBias", [&] { return glm::max(g.shadowBias, 0.0f); }, g.shadowBias);
        list.add("grass_invFleckSize", [&] { return 1.0f / glm::max(g.fleckSize, 0.01f); }, g.fleckSize);
        list.add("grass_fleckContrast", [&] { return glm::clamp(g.fleckContrast, 0.0f, 1.0f); }, g.fleckContrast);
        list.add("grass_fleckFadeDistance", [&] { return glm::max(g.fleckFadeDistance, 1.0f); }, g.fleckFadeDistance);
        list.add("grass_fleckStretch", [&] { return glm::max(g.fleckStretch, 0.0f); }, g.fleckStretch);
        list.add("grass_nearStrength", [&] { return glm::clamp(g.nearShadowStrength, 0.0f, 1.0f); }, g.nearShadowStrength);
        list.add("grass_nearBias", [&] { return glm::max(g.nearShadowBias, 0.0f); }, g.nearShadowBias);
        list.add("grass_canopyThinning", [&] { return glm::clamp(g.canopyThinning, 0.0f, 1.0f); }, g.canopyThinning);
    }
    {
        // ---- Ground clutter ("Clutter": clutter_cull.cs.glsl and the clutter / flower shaders)
        const ClutterSettings& c = Globals::settings.clutter;
        list.add("clutter_densityScale", [&] { return glm::max(c.densityScale, 0.0f); }, c.densityScale);
        list.add("clutter_rangeScale", [&] { return glm::max(c.rangeScale, 0.0f); }, c.rangeScale);
        list.add("clutter_growBand", [&] { return glm::max(c.growBand, 0.01f); }, c.growBand);
        list.add("clutter_rangeFade", [&] { return glm::clamp(c.rangeFade, 0.01f, 1.0f); }, c.rangeFade);
        list.add("clutter_lod1Size", c.lod1Size);
        list.add("clutter_lod2Size", [&] { return glm::min(c.lod2Size, c.lod1Size); }, c.lod2Size, c.lod1Size);
        list.add("clutter_flowerLod1Distance", c.flowerLod1Distance);
        list.add("clutter_flowerLod2Distance", [&] { return glm::max(c.flowerLod2Distance, c.flowerLod1Distance); }, c.flowerLod2Distance, c.flowerLod1Distance);
        list.add("clutter_flowerTransmission", c.flowerTransmission);
        list.add("clutter_flowerRoughness", [&] { return glm::clamp(c.flowerRoughness, 0.05f, 1.0f); }, c.flowerRoughness);
        list.add("clutter_contactDarkening", [&] { return glm::clamp(c.contactDarkening, 0.0f, 1.0f); }, c.contactDarkening);
        list.add("clutter_contactHeight", [&] { return glm::max(c.contactHeight, 0.001f); }, c.contactHeight);
    }
    {
        // ---- Trees ("Trees": the foliage cards, the tree wind, the far-tree volume's march shading - the bake's
        // geometry rides its push block: the march reads the BAKED volume, not this frame's settings)
        const FoliageParams& f = Globals::settings.foliage;
        const FarTreeParams& s = Globals::settings.farTree;
        list.add("foliage_rtRange", f.rtRange);
        list.add("foliage_crownNormal", f.crownNormal);
        list.add("foliage_shadowLength", f.shadowLength);
        list.add("foliage_interiorShadow", f.interiorShadow);
        list.add("foliage_interiorStart", [&] { return glm::clamp(f.interiorStart, 0.0f, 1.0f); }, f.interiorStart);
        list.add("foliage_interiorEnd", [&] { return glm::clamp(f.interiorEnd, 0.0f, 1.0f); }, f.interiorEnd);
        list.add("foliage_interiorTopCardScale", f.interiorTopCardScale);
        list.add("foliage_interiorViewFade", [&] { return glm::clamp(f.interiorViewFade, 0.0f, 1.0f); }, f.interiorViewFade);
        list.add("foliage_edgeFadeStart", f.edgeFadeStart);
        list.add("foliage_edgeFadeEnd", f.edgeFadeEnd);
        list.add("foliage_edgeFadeCentreScale", f.edgeFadeCentreScale);
        list.add("foliage_edgeFadeTopCardScale", f.edgeFadeTopCardScale);
        list.add("foliage_transmission", f.transmission);
        list.add("foliage_transmissionFocus", f.transmissionFocus);
        list.add("foliage_transmissionGlow", f.transmissionGlow);
        list.add("foliage_transmissionShadow", f.transmissionShadow);
        list.add("foliage_transmissionSelfShadow", [&] { return glm::clamp(f.transmissionSelfShadow, 0.0f, 1.0f); }, f.transmissionSelfShadow);
        list.add("foliage_selfShadow", [&] { return glm::max(f.selfShadow, 0.0f); }, f.selfShadow);
        list.add("foliage_minNoV", [&] { return glm::clamp(f.minNoV, 0.0f, 0.9f); }, f.minNoV);
        list.add("foliage_windBend", [&] { return glm::max(f.windBend, 0.0f); }, f.windBend);
        list.add("foliage_windInvRefHeight", [&] { return 1.0f / glm::max(f.windRefHeight, 0.1f); }, f.windRefHeight);
        list.add("foliage_windSway", [&] { return glm::max(f.windSway, 0.0f); }, f.windSway);
        list.add("foliage_windSwayHz", [&] { return glm::max(f.windSwayFrequency, 0.0f); }, f.windSwayFrequency);
        list.add("foliage_windBranch", [&] { return glm::max(f.windBranch, 0.0f); }, f.windBranch);
        list.add("foliage_windBranchHz", [&] { return glm::max(f.windBranchFrequency, 0.0f); }, f.windBranchFrequency);
        list.add("foliage_windLeaf", [&] { return glm::max(f.windLeaf, 0.0f); }, f.windLeaf);
        list.add("foliage_windLeafHz", [&] { return glm::max(f.windLeafFrequency, 0.0f); }, f.windLeafFrequency);
        list.add("foliage_windBranchFadeStart", f.windBranchFadeStart);
        list.add("foliage_windBranchFadeEnd", [&] { return glm::max(f.windBranchFadeEnd, f.windBranchFadeStart + 0.01f); }, f.windBranchFadeEnd, f.windBranchFadeStart);
        list.add("foliage_windLeafFadeStart", f.windLeafFadeStart);
        list.add("foliage_windLeafFadeEnd", [&] { return glm::max(f.windLeafFadeEnd, f.windLeafFadeStart + 0.01f); }, f.windLeafFadeEnd, f.windLeafFadeStart);
        list.add("foliage_windTrunkFadeEnd", [&] { return glm::max(f.windTrunkFadeEnd, 0.0f); }, f.windTrunkFadeEnd);
        list.add("foliage_windBillboardWaves", [&] { return glm::max(f.windBillboardWaves, 0.0f); }, f.windBillboardWaves);
        list.add("foliage_farStepScale", [&] { return glm::max(s.stepScale, 0.05f); }, s.stepScale);
        list.add("foliage_farMaxSteps", [&] { return (float)s.maxSteps; }, s.maxSteps);
        list.add("foliage_farOverlap", [&] { return glm::max(s.overlap, 1.0f); }, s.overlap);
        list.add("foliage_farShrink", [&] { return glm::max(s.blobShrink, 0.0f); }, s.blobShrink);
        list.add("foliage_farAmbient", s.ambient);
        list.add("foliage_farSunScale", s.sunScale);
        list.add("foliage_farSelfShadow", s.selfShadow);
        list.add("foliage_farNormalStrength", s.normalStrength);
        list.add("foliage_farGroundDark", s.groundDarkening);
        list.add("foliage_farForwardScatter", [&] { return glm::clamp(s.forwardScatter, -0.95f, 0.95f); }, s.forwardScatter);
        list.add("foliage_farAlbedoScale", s.albedoScale);
        list.add("foliage_farInteriorShadow", [&] { return glm::max(s.interiorShadow, 0.0f); }, s.interiorShadow);
        list.add("foliage_farInteriorRadius", [&] { return glm::max(s.interiorRadius, 0.0f); }, s.interiorRadius); // 0: the taps sit on the sample
        list.add("foliage_farSaturation", [&] { return glm::max(s.saturationScale, 0.0f); }, s.saturationScale);
    }
    {
        // ---- Rocks ("Rocks/Material")
        const RockParams& r = Globals::settings.rock;
        list.add("rock_coverAmount", [&] { return glm::clamp(r.coverAmount, 0.0f, 1.0f); }, r.coverAmount);
        list.add("rock_coverStart", [&] { return rockCoverStart(r); }, r.coverSlopeStart);
        list.add("rock_coverFull", [&] { return glm::max(glm::clamp(r.coverSlopeFull, 0.0f, 1.0f), rockCoverStart(r) + 1e-3f); }, r.coverSlopeFull, r.coverSlopeStart);
        list.add("rock_invCoverPatchSize", [&] { return 1.0f / glm::max(r.coverPatchSize, 0.05f); }, r.coverPatchSize);
        list.add("rock_contactHeight", [&] { return glm::max(r.contactHeight, 0.0f); }, r.contactHeight);
        list.add("rock_contactBlend", [&] { return glm::clamp(r.contactBlend, 0.0f, 1.0f); }, r.contactBlend);
        list.add("rock_contactDarkening", [&] { return glm::clamp(r.contactDarkening, 0.0f, 1.0f); }, r.contactDarkening);
        list.add("rock_contactFadeDistance", [&] { return glm::max(r.contactFadeDistance, 1.0f); }, r.contactFadeDistance);
        list.add("rock_uvScale", [&] { return glm::max(r.uvScale, 0.01f); }, r.uvScale);
        list.add("rock_cavityAo", [&] { return glm::clamp(r.cavityAo, 0.0f, 1.0f); }, r.cavityAo);
        list.add("rock_cavityCover", [&] { return glm::clamp(r.cavityCover, 0.0f, 1.0f); }, r.cavityCover);
    }
    {
        // ---- LOD ("LOD")
        const MeshLodParams& l = Globals::settings.lod;
        list.add("lod_maxErrorPx", [&] { return oc::max(0.01f, l.maxErrorPixels) * std::exp2((float)l.bias); }, l.maxErrorPixels, l.bias);
        list.add("lod_hysteresis", l.hysteresis);
        list.add("lod_fullResPixels", l.fullResPixels);
        list.add("lod_bias", l.bias);
        list.add("lod_forceLod", l.forceLod);
        list.add("lod_enabled", l.enabled);
    }
    {
        // ---- Force ("Force")
        const ForceFieldParams& f = Globals::settings.force;
        list.add("force_isoThreshold", [&] { return forceIsoThreshold(f); }, f.isoThreshold);
        list.add("force_rimPower", [&] { return glm::max(f.rimPower, 0.1f); }, f.rimPower);
        list.add("force_rimIntensity", [&] { return glm::max(f.rimIntensity, 0.0f); }, f.rimIntensity);
        list.add("force_shellAlpha", [&] { return glm::clamp(f.shellAlpha, 0.0f, 1.0f); }, f.shellAlpha);
        list.add("force_glowIntensity", [&] { return glm::max(f.contactGlowIntensity, 0.0f); }, f.contactGlowIntensity);
        list.add("force_glowWidth", [&] { return glm::max(f.contactGlowWidth, 1e-3f); }, f.contactGlowWidth);
        list.add("force_geoGlowDistance", [&] { return glm::max(f.geoGlowDistance, 0.0f); }, f.geoGlowDistance);
        list.add("force_marchSteps", [&] { return (float)glm::clamp(f.marchSteps, 8, 256); }, f.marchSteps);
        list.add("force_patternScale", [&] { return glm::max(f.patternScale, 0.0f); }, f.patternScale);
        list.add("force_patternSpeed", f.patternSpeed);
        list.add("force_patternIntensity", [&] { return glm::max(f.patternIntensity, 0.0f); }, f.patternIntensity);
        list.add("force_interiorAlpha", [&] { return glm::clamp(f.interiorAlpha, 0.0f, 1.0f); }, f.interiorAlpha);
        list.add("force_backfaceAlpha", [&] { return glm::clamp(f.backfaceAlpha, 0.0f, 1.0f); }, f.backfaceAlpha);
        list.add("force_contactWallAlpha", [&] { return glm::clamp(f.contactWallAlpha, 0.0f, 1.0f); }, f.contactWallAlpha);
        list.add("force_junctionSmoothing", [&] { return glm::clamp(f.junctionSmoothing, 0.0f, 2.0f); }, f.junctionSmoothing);
        list.add("force_densityRange", [&] { return glm::max(f.densityRange, 1e-3f); }, f.densityRange);
        list.add("force_unionStepSize", [&] { return glm::max(f.unionStepSize, 0.05f); }, f.unionStepSize);
        list.add("force_unionMaxSteps", [&] { return (float)glm::clamp(f.unionMaxSteps, 8, 512); }, f.unionMaxSteps);
    }
    {
        // ---- Particles ("Particles"; getParams() is ParticleState's registered storage)
        const ParticleParams& p = m_particles.getParams();
        list.add("particles_anisotropy", [&] { return glm::clamp(p.anisotropy, -0.95f, 0.95f); }, p.anisotropy);
        list.add("particles_rainShelterTolerance", [&] { return glm::max(p.rainOcclusionTolerance, 0.0f); }, p.rainOcclusionTolerance);
        list.add("particles_streakCameraBlur", [&] { return glm::clamp(p.streakCameraBlur, 0.0f, 1.0f); }, p.streakCameraBlur);
        list.add("particles_windSheetContrast", [&] { return glm::clamp(p.windSheetContrast, 0.0f, 1.0f); }, p.windSheetContrast);
        list.add("particles_invWindSheetSize", [&] { return 1.0f / glm::max(p.windSheetSize, 1.0f); }, p.windSheetSize);
        list.add("particles_windSheetDrift", [&] { return glm::max(p.windSheetDrift, 0.0f); }, p.windSheetDrift);
    }
    {
        // ---- Post ("TAA" + "Post": TAA, the motion blur, DLSS's ocean mask, the exposure, the tonemapper, the bloom mix.
        // The enables that gate or size a pass are read where it records)
        const TAAParams& taa = Globals::settings.taa;
        const MotionBlurParams& mb = Globals::settings.motionBlur;
        const PostParams& p = Globals::settings.post;
        const BloomParams& b = Globals::settings.bloom;
        list.add("post_taaFeedback", [&] { return taa.taaEnabled ? taa.taaFeedback : 0.0f; }, taa.taaEnabled, taa.taaFeedback);
        list.add("post_taaOceanFeedback", [&] { return taa.taaEnabled ? taa.taaOceanFeedback : 0.0f; }, taa.taaEnabled, taa.taaOceanFeedback);
        list.add("post_mbShutter", mb.shutter);
        list.add("post_mbMaxRadius", [&] { return MotionBlurPipeline::clampMaxRadius(mb.maxRadius); }, mb.maxRadius);
        list.add("post_mbCameraScale", mb.cameraScale);
        // LIVE: motionBlurEnabled() also reads the render scale (upscaling(): the DLSS mode AND the swapchain size).
        list.addLive("post_mbSamples", [this] { return motionBlurEnabled() ? (float)oc::max(m_motionBlurParams.samples, 1) : 0.0f; });
        list.add("post_dlssOceanBias", m_dlssParams.oceanBias);
        list.add("post_exposure", [&] { return exp2f(p.exposureEV); }, p.exposureEV);
        list.add("post_tonemapper", p.tonemapper);
        list.add("post_autoExposure", p.autoExposure);
        list.add("post_bloomKeep", [this] { return m_bloomParams.threshold > 0.0f ? 1.0f : 1.0f - bloomMixIntensity(); },
            b.threshold, b.enabled, b.intensity);
        // LIVE: the bloom pipeline's level count (the viewport size) also clamps the levels.
        list.addLive("post_bloomScale", [this] { return bloomMixIntensity() * m_bloomPipeline.getNormalize((uint32)m_bloomParams.levels, m_bloomParams.radius); });
    }
}
