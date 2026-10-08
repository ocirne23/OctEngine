module RendererVK;

import Core;
import Core.glm;
import Settings;
import Settings.Tweaks;

import :VK;
import :Layout;

// The settings are registered (Settings::register*) before the Renderer exists as a device, so a Saved or --tweak
// value is already in Globals::settings here: the baked state the pipelines are created with is handed over below,
// up front. EVERY reload listener returns while !m_initialized (a later loadSaved / override may fire it before the
// device and the pipelines exist). The Renderer lives until exit, so it never removes its listeners.
void Renderer::attachSettingsListeners()
{
    EngineSettings& s = Globals::settings;
    const auto reRecord = [this]() { setHaveToRecordCommandBuffers(); };
    // The lit fragment variants' baked defines: GPU idle + static mesh pipeline reload + re-record.
    const auto reloadLitShaders = [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getOpaqueRenderPass(), m_textures.getLayoutCap());
        setHaveToRecordCommandBuffers();
    };

    // "Shadows/Debug mode" is the SHADOW_DEBUG define on the lit fragment variants.
    m_staticMeshGraphicsPipeline.setShadowDebugMode(s.shadow.debugMode);
    Tweak::onChange(s.shadow.debugMode, this, [this, reloadLitShaders]() {
        m_staticMeshGraphicsPipeline.setShadowDebugMode(m_shadowParams.debugMode);
        reloadLitShaders();
    });
    // "Trees/Debug view" is the TREE_DEBUG define on the lit mesh fragments.
    m_staticMeshGraphicsPipeline.setTreeDebugMode(s.foliage.debugView);
    Tweak::onChange(s.foliage.debugView, this, [this, reloadLitShaders]() {
        m_staticMeshGraphicsPipeline.setTreeDebugMode(m_foliageParams.debugView);
        reloadLitShaders();
    });
    // "Blades per patch" is the blade index buffer's size: GPU idle, rebuild, re-record (the draw binds it).
    Tweak::onChange(s.grass.bladesPerPatch, this, [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_grassPipeline.setBladesPerPatch((uint32)m_grassParams.bladesPerPatch);
        setHaveToRecordCommandBuffers();
    });
    // The cloud toggles are baked defines (g_cloudShaders): a change reloads every shader.
    syncCloudDefines();
    const auto onCloudDefines = [this]() {
        if (!syncCloudDefines() || !m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        reloadShaders();
    };
    Tweak::onChange(s.clouds.enabled, this, onCloudDefines);
    Tweak::onChange(s.clouds.shadows, this, onCloudDefines);
    Tweak::onChange(s.clouds.selfShadowFromMap, this, onCloudDefines);
    Tweak::onChange(s.clouds.checkerboard, this, onCloudDefines);
    Tweak::onChange(s.clouds.debugMode, this, onCloudDefines);
    // The terrain relief's two BAKED switches: TERRAIN_POM (the terrain fragment shaders) and the tessellation (the
    // cull's TERRAIN_TESS_ROUTE + whether the tess pipeline exists and its draws are recorded).
    m_staticMeshGraphicsPipeline.setTerrainRelief(s.terrain.texParallaxEnabled, s.terrain.texTessEnabled);
    m_indirectCullComputePipeline.setTerrainTess(s.terrain.texTessEnabled);
    const auto onTerrainRelief = [this]() {
        const TerrainSettings& t = Globals::settings.terrain;
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setTerrainRelief(t.texParallaxEnabled, t.texTessEnabled);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getOpaqueRenderPass(), m_textures.getLayoutCap());
        m_indirectCullComputePipeline.setTerrainTess(t.texTessEnabled);
        m_indirectCullComputePipeline.reloadShaders();
        setHaveToRecordCommandBuffers();
    };
    Tweak::onChange(s.terrain.texParallaxEnabled, this, onTerrainRelief);
    Tweak::onChange(s.terrain.texTessEnabled, this, onTerrainRelief);
    // The sky draw is in the cached static mesh secondary.
    Tweak::onChange(s.sky.enabled, this, reRecord);
    // The master + GI toggles are baked into the cached GI secondary; the master, "RT Sun" and "RT Lights"
    // also into the lit fragments (LIT_RT_*).
    m_staticMeshGraphicsPipeline.setRtShadows(s.rt.effectiveSunShadow(), s.rt.effectiveLightShadows());
    const auto onRtShadows = [this, reloadLitShaders]() {
        m_staticMeshGraphicsPipeline.setRtShadows(m_rtParams.effectiveSunShadow(), m_rtParams.effectiveLightShadows());
        reloadLitShaders();
    };
    Tweak::onChange(s.rt.enabled, this, onRtShadows);
    Tweak::onChange(s.rt.giEnabled, this, reRecord);
    Tweak::onChange(s.rt.rtLightShadows, this, onRtShadows);
    Tweak::onChange(s.rt.rtSunShadow, this, onRtShadows);
    // RTAO: every knob is baked into the cached secondary; "Alpha Test" is a define of its pipeline.
    Tweak::onChange(s.rtao.enabled, this, reRecord);
    Tweak::onChange(s.rtao.rays, this, reRecord);
    Tweak::onChange(s.rtao.radius, this, reRecord);
    Tweak::onChange(s.rtao.power, this, reRecord);
    Tweak::onChange(s.rtao.intensity, this, reRecord);
    Tweak::onChange(s.rtao.fadeStart, this, reRecord);
    Tweak::onChange(s.rtao.maxDistance, this, reRecord);
    Tweak::onChange(s.rtao.normalBias, this, reRecord);
    Tweak::onChange(s.rtao.distanceBias, this, reRecord);
    Tweak::onChange(s.rtao.maxHistory, this, reRecord);
    Tweak::onChange(s.rtao.blurRadius, this, reRecord);
    Tweak::onChange(s.rtao.alphaTest, this, [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_rtaoPipeline.reloadShaders();
        setHaveToRecordCommandBuffers();
    });
    Tweak::onChange(s.taa.taaEnabled, this, reRecord);
    Tweak::onChange(s.taa.taaFeedback, this, reRecord);
    Tweak::onChange(s.taa.taaOceanFeedback, this, reRecord);
    // The DLSS mode sets the render resolution: GPU idle + the render-size targets re-created (initDeviceAndSwapchain
    // sizes them from the registered value).
    const auto onRenderResolution = [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        applyRenderResolution();
    };
    Tweak::onChange(s.dlss.mode, this, onRenderResolution);
    Tweak::onChange(s.dlss.preset, this, reRecord);
    Tweak::onChange(s.dlss.mipBias, this, onRenderResolution);
    // Motion blur and bloom ride the cached secondaries (push constants) or gate them: every change re-records.
    Tweak::onChange(s.motionBlur.enabled, this, reRecord);
    Tweak::onChange(s.motionBlur.shutter, this, reRecord);
    Tweak::onChange(s.motionBlur.maxRadius, this, reRecord);
    Tweak::onChange(s.motionBlur.cameraScale, this, reRecord);
    Tweak::onChange(s.motionBlur.samples, this, reRecord);
    Tweak::onChange(s.bloom.enabled, this, reRecord);
    Tweak::onChange(s.bloom.intensity, this, reRecord);
    Tweak::onChange(s.bloom.threshold, this, reRecord);
    Tweak::onChange(s.bloom.knee, this, reRecord);
    Tweak::onChange(s.bloom.radius, this, reRecord);
    Tweak::onChange(s.bloom.levels, this, reRecord);
    // The composite push constants.
    Tweak::onChange(s.post.exposureEV, this, reRecord);
    Tweak::onChange(s.post.tonemapper, this, reRecord);
    Tweak::onChange(s.post.autoExposure, this, reRecord);
    // "Debug Mode" = the LIGHT_GRID_DEBUG define on the lit fragments (the LOD params are read by the CPU build
    // every frame: no listener).
    m_staticMeshGraphicsPipeline.setLightGridDebugMode(s.lightGrid.debugMode);
    Tweak::onChange(s.lightGrid.debugMode, this, [this, reloadLitShaders]() {
        m_staticMeshGraphicsPipeline.setLightGridDebugMode(m_lightGridParams.debugMode);
        reloadLitShaders();
    });
    // Wireframe is baked pipeline state (polygonMode): the lit shader reload pattern.
    Tweak::onChange(s.renderer.wireframe, this, reloadLitShaders);
    // "Anisotropy": a new scene texture sampler; the re-record writes it into every texture slot.
    Tweak::onChange(s.renderer.anisotropyLevel, this, [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.recreateSampler();
        setHaveToRecordCommandBuffers();
    });
    Tweak::onChange(s.renderer.vsync, this, [this]() { if (m_initialized) recreateSwapchain(); });
    // "Terrain/Water" Diffusion is a baked define on the wetness compute shader.
    Tweak::onChange(s.renderer.terrainWetDiffusion, this, [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_terrainWetnessPipeline.reloadShaders();
        setHaveToRecordCommandBuffers();
    });
    // The GI grid shape is a #define in every probe-sampling shader: GPU idle, the SH clipmap re-allocated, EVERY
    // shader reloaded (reloadShaders waits + re-records). GIProbePipeline::initialize allocates from the registered value.
    const auto onGiGrid = [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_giProbePipeline.resizeGrid();
        reloadShaders();
    };
    Tweak::onChange(s.gi.grid.numCascades, this, onGiGrid);
    Tweak::onChange(s.gi.grid.dimLog2X, this, onGiGrid);
    Tweak::onChange(s.gi.grid.dimLog2Y, this, onGiGrid);
    Tweak::onChange(s.gi.grid.dimLog2Z, this, onGiGrid);
    Tweak::onChange(s.gi.grid.focusOffsetY, this, onGiGrid);
    // The irradiance volume: only the volume images and the defines change - the probe history stays.
    const auto onGiVolume = [this]() {
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_giProbePipeline.resizeVolume();
        reloadShaders();
    };
    Tweak::onChange(s.gi.grid.volume, this, onGiVolume);
    Tweak::onChange(s.gi.grid.volumeRes, this, onGiVolume);
    // The debug probes' colour mode and radius are push constants of the cached debug secondary.
    Tweak::onChange(s.gi.debugMode, this, reRecord);
    Tweak::onChange(s.gi.debugRadius, this, reRecord);
}
