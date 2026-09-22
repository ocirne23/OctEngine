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


Renderer::~Renderer()
{
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::~RendererVK");
    }
    // The pending texture frees are NOT processed here: ~TextureManager (init_seg XCU4, destroyed before this XCU3 global) destroys every texture wholesale, pending ones included.
    Globals::textureStreamer.shutdown(); // stop the disk worker + retire swapped-out images while the device is idle
    Globals::meshStreamer.shutdown();
    m_vrEyes.destroy();
    m_gpuProfiler.destroy();
    ImGui_ImplVulkan_Shutdown();
}

bool Renderer::initialize(Window& window, EValidation validation, EVr vr)
{
    ProfileScope scope("Renderer::initialize", EProfileCategory::Renderer);

    // Disable layers we don't care about for now to eliminate potential issues
    _putenv("DISABLE_LAYER_NV_OPTIMUS_1=True");
    _putenv("DISABLE_VULKAN_OW_OVERLAY_LAYER=True");
    _putenv("DISABLE_VULKAN_OW_OBS_CAPTURE=True");
    _putenv("DISABLE_VULKAN_OBS_CAPTURE=True");

    // Per-worker staging for the lock-free submission surface (requires the JobSystem first).
    assert(Globals::jobSystem.getNumContexts() != 0 && "initialize the JobSystem before the Renderer");
    m_debugLineVerts.initialize();
    m_particles.initialize();
    m_force.initialize([this]() { waitForGpuAndFlushStaging(); }, [this]() { setHaveToRecordCommandBuffers(); });

    registerTweaks(); // before the device: a Saved/override value must be live when the swapchain is made
    if (!initDeviceAndSwapchain(window, validation, vr))
        return false;
    initPipelines();
    initPerFrameResources();
    initSharedBuffers();

    m_gpuCrashTracker.Initialize(false);
    m_initialized = true; // headless server mode never calls initialize; renderer-touching paths gate on this
    return true;
}

void Renderer::registerTweaks()
{
    auto rerecordCallback = [this]() { setHaveToRecordCommandBuffers(); };
    m_skyParams.registerTweaks();
    // "Shadows/Debug mode" is the SHADOW_DEBUG define on the lit fragment variants: GPU-idle + pipeline
    // rebuild, the wireframe pattern below.
    m_shadowParams.registerTweaks([this]() {
        if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setShadowDebugMode(m_shadowParams.debugMode);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
        setHaveToRecordCommandBuffers();
    });
    m_fogParams.registerTweaks();
    // The master + GI toggles are baked into the cached GI secondary; the master, "RT Sun" and "RT Lights"
    // also into the lit fragments (LIT_RT_*). The flags are handed over BEFORE the idle test: a Saved value
    // fires at registration, before the device exists, and initialize() must then build with it.
    m_rtParams.registerTweaks(rerecordCallback, [this]() {
        m_staticMeshGraphicsPipeline.setRtShadows(m_rtParams.effectiveSunShadow(), m_rtParams.effectiveLightShadows());
        if (!m_initialized || Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
        setHaveToRecordCommandBuffers();
    });
    m_staticMeshGraphicsPipeline.setRtShadows(m_rtParams.effectiveSunShadow(), m_rtParams.effectiveLightShadows());
    m_rtaoParams.registerTweaks(rerecordCallback, [this]() { if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess) return; m_rtaoPipeline.reloadShaders(); setHaveToRecordCommandBuffers(); });
    m_taaParams.registerTweaks(rerecordCallback);
    m_postParams.registerTweaks(rerecordCallback);
    m_lodParams.registerTweaks();
    m_lightGridParams.registerTweaks( // the LOD params are read by the CPU build every frame: no reload
        [this]() { // "Debug Mode" = the LIGHT_GRID_DEBUG define on the lit fragments (the wireframe pattern below)
            if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
                return;
            m_staticMeshGraphicsPipeline.setLightGridDebugMode(m_lightGridParams.debugMode);
            m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
            setHaveToRecordCommandBuffers();
        });
    // Wireframe is baked pipeline state (polygonMode), so flipping it rebuilds the static mesh pipeline -
    // same GPU-idle + reload pattern as the RTAO alpha-test and ocean hit-lighting tweaks.
    m_staticMeshGraphicsPipeline.registerTweaks([this]() {
        if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
        setHaveToRecordCommandBuffers();
    });
    // Live toggles: the primary CB re-records every frame, so no re-record callback is needed.
    m_particles.registerTweaks();
    m_oceanSimPipeline.registerSprayTweaks();
    m_decalPipeline.registerTweaks();
    Tweak::boolean("Renderer", "Log pipeline stats", &Device::s_logPipelineStats); // F5 re-creates the pipelines with it

    Tweak::boolean("Time", "VSync", &m_vsyncEnabled, [this]() { if (m_initialized) recreateSwapchain(); }, ETweakFlags::Saved);
}

bool Renderer::initDeviceAndSwapchain(Window& window, EValidation validation, EVr vr)
{
    Globals::meshStreamer.initialize();

    glslang::InitializeProcess();
    const bool enableValidationLayers = (validation == EValidation::ENABLED);
    if (enableValidationLayers)
    {
        _putenv("VK_LAYER_PRINTF_ENABLE=1");
        _putenv("VK_LAYER_PRINTF_TO_STDOUT=0");
        //_putenv("VK_LAYER_PRINTF_BUFFER_SIZE=16777216");
    }
    if (vr == EVr::ENABLED)
    {
        m_taaParams.taaFeedback *= 0.5f; // Reduce blur for VR
        Globals::openXR.initInstanceAndSystem();
    }

    Globals::instance.initialize(window, enableValidationLayers);
    Globals::instance.setBreakOnValidationLayerError(enableValidationLayers);
    Globals::device.initialize();

    if (Globals::openXR.isEnabled())
    {
        if (!Globals::openXR.createSession(Globals::instance.getInstance(), Globals::device.getPhysicalDevice(),
            Globals::device.getDevice(), Globals::device.getGraphicsQueueIndex(), 0))
        {
            printf("Renderer: failed to create OpenXR session, continuing without VR.\n");
            Globals::openXR.destroy(); // clears isEnabled()
        }
    }

    window.getWindowSize(m_windowSize);
    m_surface.initialize(window);
    assert(m_surface.deviceSupportsSurface());

    m_swapChain.initialize(m_surface, RendererVKLayout::NUM_FRAMES_IN_FLIGHT, m_vsyncEnabled);
    m_viewportRect.max = glm::ivec2(m_swapChain.getLayout().extent.width, m_swapChain.getLayout().extent.height);

    m_gpuProfiler.initialize(); // needs the device; Globals::profiler was initialized in main before us

    Globals::stagingManager.initialize();
    Globals::textureStreamer.initialize();
    Globals::meshDataManager.initialize(RendererVKLayout::INITIAL_VERTEX_DATA, RendererVKLayout::INITIAL_INDEX_DATA);

    m_renderPass.initialize(m_swapChain);
    m_framebuffers.initialize(m_renderPass, m_swapChain);

    initImgui(window);
    return true;
}

void Renderer::initPipelines()
{
    auto rerecordCallback = [this]() { setHaveToRecordCommandBuffers(); };
    const vk::Extent2D ext = m_swapChain.getLayout().extent;

    initBindlessTextures(); // the layout cap the pipelines below bake in comes from here

    // The per-frame instance stream and the three append-only scene tables, FIRST: every pipeline below is sized from their capacities.
    m_instances.initialize(RendererVKLayout::INITIAL_UNIQUE_MESHES,
        [this]() { waitForGpuAndFlushStaging(); }, [this]() { setHaveToRecordCommandBuffers(); },
        [this](uint32 maxInstances)
        {
            m_indirectCullComputePipeline.resizeInstanceBuffers(maxInstances);
            m_shadowCullComputePipeline.resizeInstanceBuffers(maxInstances);
            m_rainCullComputePipeline.resizeInstanceBuffers(maxInstances);
        });

    m_submission.initialize([this]() { waitForGpuAndFlushStaging(); }, [this]() { setHaveToRecordCommandBuffers(); });

    // The three append-only scene tables. A growth is: GPU idle -> the table re-creates and re-uploads its own buffer -> onGrown resizes everything else that capacity feeds, and re-records.
    const auto onGpuIdle = [this]() { waitForGpuAndFlushStaging(); };
    constexpr vk::BufferUsageFlags2 tableUsage = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst;
    m_meshInfos.initialize("MeshInfos", RendererVKLayout::INITIAL_UNIQUE_MESHES, RendererVKLayout::MESH_MATERIAL_INDEX_LIMIT,
        tableUsage, onGpuIdle, [this](uint32 capacity) { onUniqueMeshCapacityGrown(capacity); });
    m_materials.initialize("MaterialInfos", RendererVKLayout::INITIAL_UNIQUE_MATERIALS, RendererVKLayout::MESH_MATERIAL_INDEX_LIMIT,
        tableUsage, onGpuIdle, [this](uint32 capacity) { setHaveToRecordCommandBuffers(); printf("Renderer: grew material capacity to %u\n", capacity); });
    m_instanceOffsets.initialize("InstanceOffsets", RendererVKLayout::INITIAL_INSTANCE_OFFSETS, UINT32_MAX,
        tableUsage, onGpuIdle, [this](uint32 capacity) { setHaveToRecordCommandBuffers(); printf("Renderer: grew instance offset capacity to %u\n", capacity); });

    m_sceneViewCount = Globals::openXR.isEnabled() ? 2u : 1u;

    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount);
    const vk::RenderPass sceneRenderPass = m_perFrameData[0].sceneColor.getRenderPass();

    m_staticMeshGraphicsPipeline.initialize(sceneRenderPass, m_meshInfos.capacity(), m_textures.getLayoutCap(), m_sceneViewCount > 1);
    m_rtaoPipeline.initialize(&m_rtaoParams, ext.width, ext.height, m_textures.getLayoutCap(), m_textures.getDescriptorCount(), m_sceneViewCount);
    m_oceanSimPipeline.initialize();

    // "Terrain/Wetness" Diffusion is a baked define on the wetness compute shader: GPU idle + reload + re-record, the light grid's pattern.
    m_terrainWetnessPipeline.initialize([this]() {
        if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_terrainWetnessPipeline.reloadShaders();
        setHaveToRecordCommandBuffers();
    });
    m_volumetricFogPipeline.initialize();
    m_volumetricFogPipeline.initializeApply(sceneRenderPass, m_sceneViewCount);
    m_terrain.initialize();
    m_taaPipeline.initialize(ext.width, ext.height, m_sceneViewCount);
    m_eyeAdaptationPipeline.initialize();
    m_compositePipeline.initialize(m_renderPass);
    m_indirectCullComputePipeline.initialize(m_instances.getMaxInstances(), m_meshInfos.capacity());
    m_skinningComputePipeline.initialize(m_skinned.getMaxPaletteEntries(), m_skinned.getMaxJobs());
    m_skinned.initialize(
        [this](uint32 entries) { waitForGpuAndFlushStaging(); m_skinningComputePipeline.resizePaletteBuffer(entries);
            setHaveToRecordCommandBuffers(); printf("Renderer: grew skinning palette capacity to %u\n", entries); },
        [this](uint32 jobs) { waitForGpuAndFlushStaging(); m_skinningComputePipeline.resizeJobBuffer(jobs);
            setHaveToRecordCommandBuffers(); printf("Renderer: grew skinning job capacity to %u\n", jobs); });
    m_lightGridComputePipeline.initialize(m_submission.getLightTableEntries());
    m_rt.initialize(m_meshInfos.capacity(), [this]() { waitForGpuAndFlushStaging(); },
        [this](uint32 maxInstances)
        {
            m_giProbePipeline.resizeTlasInstanceBuffers(maxInstances);
            setHaveToRecordCommandBuffers(); // the cached GI secondary bakes the instance buffers + dispatch size
        });
    m_giProbePipeline.initialize(m_rt.getMaxTlasInstances(), m_textures.getLayoutCap(), m_textures.getDescriptorCount());

    // The GI grid shape is a #define in every probe-sampling shader (Layout.ixx g_giGrid): a change waits for the GPU, re-allocates the SH clipmap, reloads EVERY shader (reloadShaders waits + re-records).
    m_giProbePipeline.registerGridTweaks(
        [this]() {
            if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
                return;
            m_giProbePipeline.resizeGrid();
            reloadShaders();
        },
        // The irradiance-volume tweaks: only the volume images and the defines change - the probe history stays.
        [this]() {
            if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
                return;
            m_giProbePipeline.resizeVolume();
            reloadShaders();
        });
    m_giProbePipeline.initializeDebug(sceneRenderPass);
    m_giProbePipeline.registerDebugTweaks(rerecordCallback);
    m_debugLinePipeline.initialize(sceneRenderPass);
    m_particlePipeline.initialize(sceneRenderPass, m_textures.getLayoutCap(), m_textures.getDescriptorCount(), m_sceneViewCount);
    m_decalPipeline.initialize(sceneRenderPass, m_textures.getLayoutCap(), m_textures.getDescriptorCount(), m_sceneViewCount);
    m_forceFieldPipeline.initialize(sceneRenderPass, m_sceneViewCount);
    m_forceFieldPipeline.resizeIntervalTarget(ext.width, ext.height); // the union march's target

    m_shadowCullComputePipeline.initialize(m_instances.getMaxInstances(), m_meshInfos.capacity());
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.shadowMap.initialize("ShadowMap");
    m_shadowMapGraphicsPipeline.initialize(m_perFrameData[0].shadowMap, m_meshInfos.capacity(), m_textures.getLayoutCap());
    // The weather volume's top-down rain occlusion map: the same two pipelines in their RAIN_OCCLUSION variant over a single-layer map per frame slot.
    m_rainCullComputePipeline.initialize(m_instances.getMaxInstances(), m_meshInfos.capacity(), true);
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.rainOcclusionMap.initialize("RainOcclusionMap", RendererVKLayout::RAIN_OCCLUSION_RESOLUTION, 1);
    m_rainMapGraphicsPipeline.initialize(m_perFrameData[0].rainOcclusionMap, m_meshInfos.capacity(), m_textures.getLayoutCap(), true);
}

void Renderer::initPerFrameResources()
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.indirectCullPipelineDescriptorSet.initialize(m_indirectCullComputePipeline.getDescriptorSetLayout(), "IndirectCull");
        perFrame.skinningDescriptorSet.initialize(m_skinningComputePipeline.getDescriptorSetLayout(), "Skinning");
        perFrame.lightGridPipelineDescriptorSet.initialize(m_lightGridComputePipeline.getDescriptorSetLayout(), "LightGrid");
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
            perFrame.staticMeshPipelineDescriptorSet[eye].initialize(m_staticMeshGraphicsPipeline.getDescriptorSetLayout(), "StaticMesh", m_textures.getDescriptorCount());
        perFrame.compositeDescriptorSet.initialize(m_compositePipeline.getDescriptorSetLayout(), "Composite");

        perFrame.shadowCullDescriptorSet.initialize(m_shadowCullComputePipeline.getDescriptorSetLayout(), "ShadowCull");
        perFrame.shadowDrawDescriptorSet.initialize(m_shadowMapGraphicsPipeline.getDescriptorSetLayout(), "ShadowDraw", m_textures.getDescriptorCount());
        perFrame.rainCullDescriptorSet.initialize(m_rainCullComputePipeline.getDescriptorSetLayout(), "RainOcclusionCull");
        perFrame.rainDrawDescriptorSet.initialize(m_rainMapGraphicsPipeline.getDescriptorSetLayout(), "RainOcclusionDraw", m_textures.getDescriptorCount());

        perFrame.primaryCommandBuffer.initialize(vk::CommandBufferLevel::ePrimary, "CB.primary");
        perFrame.staticMeshCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.staticMesh");
        perFrame.aoCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.rtao");
        perFrame.indirectCullCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.indirectCull");
        perFrame.skinningCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.skinning");
        perFrame.oceanSimCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.oceanSim");
        perFrame.terrainWetnessCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.terrainWetness");
        perFrame.lightGridCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.lightGrid");
        perFrame.imguiCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.imgui");
        perFrame.shadowCullCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.shadowCull");
        perFrame.shadowDrawCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.shadowDraw");
        perFrame.rainCullCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.rainOcclusionCull");
        perFrame.rainDrawCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.rainOcclusionDraw");
        perFrame.globalIllumCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.gi");
        perFrame.giPrepCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.giPrep");
        perFrame.volumetricFogCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.volumetricFog");
        perFrame.fogApplyCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.fogApply");
        perFrame.giProbeDebugCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.giProbeDebug");
        perFrame.debugLineCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.debugLines");
        perFrame.particleSimCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.particleSim");
        perFrame.particleCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.particles");
        perFrame.decalCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.decals");
        perFrame.forceFieldCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.forceShells");
        perFrame.forceUnionCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.forceUnionBlend");
        perFrame.forceIntervalCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.forceIntervals");
        perFrame.forceMarchCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.forceUnionMarch");
        perFrame.forceComputeCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.forceCompute");
        perFrame.taaCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.taa");
        perFrame.eyeAdaptCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.eyeAdapt");
        perFrame.compositeCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary, "CB.composite");

        perFrame.ubo.initialize(sizeof(RendererVKLayout::Ubo),
            vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "Ubo");

        perFrame.lodStatsBuffer.initialize(RendererVKLayout::MAX_MESH_LODS * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LodStats");
        perFrame.mappedLodStats = perFrame.lodStatsBuffer.mapMemory<uint32>();
        memset(perFrame.mappedLodStats.data(), 0, perFrame.mappedLodStats.size_bytes());

    }

    if (m_sceneViewCount > 1)
    {
        m_vrEyes.initialize(m_compositePipeline.getDescriptorSetLayout());
        recreateVrEyeTargets();
    }
}

void Renderer::initSharedBuffers()
{
    m_meshLods.initialize(m_meshInfos.capacity(),
        [this]() { waitForGpuAndFlushStaging(); }, [this]() { setHaveToRecordCommandBuffers(); });

	uint16 diffuseIdx = Globals::textureManager.upload(*ITextureData::createFallbackWhiteTexture(), false);
	assert(diffuseIdx == RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX);
	uint16 normalIdx = Globals::textureManager.upload(*ITextureData::createFallbackNormalTexture(), false);
	assert(normalIdx == RendererVKLayout::FALLBACK_NORMAL_TEX_IDX);
}

void Renderer::recreateVrEyeTargets()
{
    if (m_sceneViewCount <= 1)
        return;
    m_vrEyes.create(m_renderPass.getRenderPass(), m_swapChain.getLayout().extent, m_swapChain.getLayout().surfaceFormat.format);
}

void Renderer::recreateWindowSurface(Window& window)
{
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::recreateWindowSurface");
    }
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();

    window.getWindowSize(m_windowSize);
    m_swapChain.destroy();
    m_surface.initialize(window);
    m_swapChain.initialize(m_surface, RendererVKLayout::NUM_FRAMES_IN_FLIGHT, m_vsyncEnabled);
    m_framebuffers.initialize(m_renderPass, m_swapChain);

    const vk::Extent2D ext = m_swapChain.getLayout().extent;
    m_rtaoPipeline.recreateImages(ext.width, ext.height);
    m_taaPipeline.recreateImages(ext.width, ext.height);

    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount);
    recreateVrEyeTargets(); // VR: resize the per-eye LDR composite targets
    m_forceFieldPipeline.resizeIntervalTarget(ext.width, ext.height);

    // The cached scene command buffers embed the (now-recreated) scene-colour render pass in their inheritance info, so force them to re-record against the new handle.
    setHaveToRecordCommandBuffers();
    auto waitResult2 = Globals::device.graphicsQueueWaitIdle();
    if (waitResult2 != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::recreateWindowSurface");
    }
}

void Renderer::recreateSwapchain()
{
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::recreateSwapchain");
    }
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();
    printf("recreateSwapchain()\n");
    m_swapChain.initialize(m_surface, RendererVKLayout::NUM_FRAMES_IN_FLIGHT, m_vsyncEnabled);
    m_framebuffers.initialize(m_renderPass, m_swapChain);
    const vk::Extent2D ext = m_swapChain.getLayout().extent;
    m_rtaoPipeline.recreateImages(ext.width, ext.height);
    m_taaPipeline.recreateImages(ext.width, ext.height);
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount);
    recreateVrEyeTargets(); // VR: resize the per-eye LDR composite targets
    m_forceFieldPipeline.resizeIntervalTarget(ext.width, ext.height);

    // Cached scene command buffers reference the recreated scene-colour render pass; re-record them.
    setHaveToRecordCommandBuffers();
}

void Renderer::reloadShaders()
{
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::reloadShaders");
        return;
    }
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();

    m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
    m_rtaoPipeline.reloadShaders();
    m_oceanSimPipeline.reloadShaders();
    m_terrainWetnessPipeline.reloadShaders();
    m_volumetricFogPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_indirectCullComputePipeline.reloadShaders();
    m_skinningComputePipeline.reloadShaders();
    m_lightGridComputePipeline.reloadShaders();
    m_shadowCullComputePipeline.reloadShaders();
    m_shadowMapGraphicsPipeline.reloadShaders(m_textures.getLayoutCap());
    m_rainCullComputePipeline.reloadShaders();
    m_rainMapGraphicsPipeline.reloadShaders(m_textures.getLayoutCap());
    m_giProbePipeline.reloadShaders(m_textures.getLayoutCap());
    m_giProbePipeline.reloadDebugShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_debugLinePipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_particlePipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_decalPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_forceFieldPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_taaPipeline.reloadShaders();
    m_eyeAdaptationPipeline.reloadShaders();
    m_compositePipeline.reloadShaders(m_renderPass);

    setHaveToRecordCommandBuffers();
    printf("Reloaded shaders\n");
}

void Renderer::setOceanParams(const OceanParams& ocean)
{
    const OceanParams& prev = m_oceanSimPipeline.getOceanParams();
    const bool rebuildOceanVariant = ocean.hitLighting != prev.hitLighting
        || ocean.rtReflections != prev.rtReflections
        || ocean.debugMode != prev.debugMode;
    m_oceanSimPipeline.setOceanParams(ocean);
    if (rebuildOceanVariant)
    {
        if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setOceanHitLights(ocean.hitLighting);
        m_staticMeshGraphicsPipeline.setOceanRtReflections(ocean.rtReflections);
        m_staticMeshGraphicsPipeline.setOceanDebugMode(ocean.debugMode);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
        setHaveToRecordCommandBuffers();
    }
}

void Renderer::setTerrainTextureParams(const TerrainTexTweaks& params)
{
    m_terrain.setTexTweaks(params);
    // The terrain relief's two BAKED switches: TERRAIN_POM (the terrain fragment shaders) and the tessellation
    // (the cull's TERRAIN_TESS_ROUTE + whether the tess pipeline exists and its draws are recorded). Compared
    // against what the pipelines were last built with, so the first push only rebuilds on a real difference.
    if (params.parallaxEnabled == m_staticMeshGraphicsPipeline.getTerrainPom()
        && params.tessEnabled == m_staticMeshGraphicsPipeline.getTerrainTess())
        return;
    if (Globals::device.graphicsQueueWaitIdle() != vk::Result::eSuccess)
        return;
    m_staticMeshGraphicsPipeline.setTerrainRelief(params.parallaxEnabled, params.tessEnabled);
    m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_textures.getLayoutCap());
    m_indirectCullComputePipeline.setTerrainTess(params.tessEnabled);
    m_indirectCullComputePipeline.reloadShaders();
    setHaveToRecordCommandBuffers();
}

void Renderer::setWindowMinimized(bool minimized)
{
    m_windowMinimized = minimized;
}

void Renderer::setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts)
{
    const TerrainResources::SplatUpload io{
        .addMaterials = [this](const oc::vector<RendererVKLayout::MaterialInfo>& infos) { return addMaterialInfos(infos); },
        .uploadTexture = [](const oc::string& path, bool sRGB) { return Globals::textureManager.upload(path.c_str(), true, sRGB); },
        .isBc5Normal = [](uint16 texIdx) {
            const vk::Format format = Globals::textureManager.getTexture(texIdx).getFormat();
            return format == vk::Format::eBc5UnormBlock || format == vk::Format::eBc5SnormBlock; },
    };
    // The retired set's images may still be sampled in flight: same deferred free path as
    // ObjectContainer teardown (processed in present() after the GPU drain).
    const oc::vector<uint16> retired = m_terrain.setSplatMaterials(mats, counts, io);
    m_textures.queueFree(retired);
}

bool Renderer::waitFrameSlot(uint64 timeoutNs)
{
    // This frame slot's fence must be waited BEFORE anything writes its host-visible per-frame buffers
    // (renderNode instance memcpys, LOD meshIdx redirects, firstInstance prefix sums, sparse transform
    // uploads) - the wait inside acquireNextImage happens at the END of the CPU frame, after all those
    // writes. Without this, the CPU scribbles over the slot while frame N-2 still reads it on the GPU;
    // invisible while per-frame data is byte-identical, but LOD switches change instance meshIdx en
    // masse and the torn instance/prefix data made the cull's buckets overflow into neighbouring
    // meshes' draw ranges (one-frame flashes with foreign materials).
    // It runs at the TOP of the main loop (not inside beginFrame) so the vsync/GPU stall lands
    // before input sampling rather than between input and the sim; the slot index only advances in
    // present(), so it is the same fence either way.
    if (m_frameSlotWaited)
        return true; // idempotent within a frame
    if (timeoutNs == 0)
    {
        // Status poll (Time::beginFrame polls every ~ms while timing the pump kick): no marker,
        // it would flood the ring; a signaled poll still claims the slot like a real wait.
        if (m_swapChain.waitForFrame(m_swapChain.getCurrentFrameIndex(), 0) != SwapChain::EFenceWait::Signaled)
            return false;
        m_frameSlotWaited = true;
        return true;
    }
    // GPU-bound (or vsync-throttled) frames show up here: the CPU waiting for the frame slot's fence.
    ProfileScope profileScope("Frame wait", EProfileCategory::Wait);
    const SwapChain::EFenceWait result = m_swapChain.waitForFrame(m_swapChain.getCurrentFrameIndex(), timeoutNs);
    if (result == SwapChain::EFenceWait::Timeout)
        return false;
    if (result == SwapChain::EFenceWait::Error)
    {
        Log::error("Renderer: failed to wait for frame, recreating swapchain");
        recreateSwapchain();
    }
    m_frameSlotWaited = true;
    return true;
}

// THE one place the centre view-projection is built. Its two callers - setFrameView before the frame
// and buildUboViews inside it - must agree bit-exactly, or the spatial cull and the GPU cull disagree.
glm::mat4 Renderer::computeCenterViewProj(const Camera& camera) const
{
    const glm::ivec2 viewportSize = m_viewportRect.getSize();
    // In VR the "centre view" (used for culling, GI region, shadow cascade fit, and the shared screen-space
    // froxel fog volume) uses a head-centred projection spanning the union of both eyes' FOV, so it covers
    // everything either eye renders. Desktop uses the plain camera perspective.
    const glm::mat4x4 projection = reverseZProjection(Globals::openXR.isEnabled()
        ? Globals::openXR.getCombinedProjection(camera.near, camera.far)
        : glm::perspective(glm::radians(camera.fovDeg), (float)viewportSize.x / (float)viewportSize.y, camera.near, camera.far),
        camera.near, camera.far);
    return projection * camera.viewMatrix;
}

void Renderer::beginFrame()
{
    ProfileScope beginFrameScope("Begin frame", EProfileCategory::Renderer);
    assert(m_frameViewSet && "Renderer::setFrameView() must run before the frame's begin-frame work");

    // The slot's fence is waited at the loop top (waitFrameSlot); anything that reached here without
    // it is a main-loop ordering bug - but never run unsynchronized, so wait now (asserting).
    assert(m_frameSlotWaited && "Renderer::waitFrameSlot() must run at the top of the frame, before any per-slot writes");
    waitFrameSlot();

    // This slot's fence is waited, so its previous submission's GPU timestamps have landed. The
    // readback (two driver calls + the track pushes) needs nothing from this frame, so it runs as a
    // job across the sim; recordCommandBuffers joins it before beginRecord resets the slot's scope
    // list + query pool. One job in flight at a time keeps the GPU track single-writer - the wait
    // here only ever spins if the last present() bailed before recording (acquire failure).
    Globals::jobSystem.wait(m_gpuCollectCounter);
    Globals::jobSystem.submit([this, collectFrameIdx = m_swapChain.getCurrentFrameIndex()] { m_gpuProfiler.collect(collectFrameIdx); },
        { "GPU timestamps collect", EProfileCategory::Renderer }, EJobPriority::Normal, &m_gpuCollectCounter);

    Camera camera = m_frameCamera;
    glm::quat vrBaseOrientation;
    applyVrHeadPose(m_frameCamera, camera, vrBaseOrientation);

    checkFrameCapacities();

    PerFrameData& frameData = m_perFrameData[m_swapChain.getCurrentFrameIndex()];
    {
        ProfileScope resetScope("Counters + LOD stats", EProfileCategory::Renderer);
        m_instances.beginFrame();
        // Both read from any job during the entity pass (noteTextureUse), so set before returning.
        m_cameraPos = camera.position; // also drives the GI probe region each frame
        m_mipPixelScale = (float)oc::max(1, m_viewportRect.getSize().y) / oc::max(1e-3f, std::tan(glm::radians(camera.fovDeg) * 0.5f));
        snapshotLodStats(frameData);
    }

    buildFrameUbo(m_frameCamera, camera, vrBaseOrientation, frameData);

    m_submission.beginFrame();
    // The light grid's per-light phase runs inline in addLightInfo from here on: snapshot its inputs.
    // (Grid jobs left in flight by a present() that returned early must finish first - normally done.)
    Globals::jobSystem.wait(m_gridJobCounter);
    m_gridBuildsKicked = false;
    m_lightGridComputePipeline.beginFrame(m_swapChain.getCurrentFrameIndex(), m_lightGridParams, m_cameraPos);
    m_frameCounter++;
    if (Globals::openXR.isEnabled())
    {
        m_lastCullCamera = camera; // head-swapped: next frame's spatial cull runs on this view (see hasCullView)
        m_hasCullView = true;
    }
}

void Renderer::kickBeginFrameJob()
{
    ProfileScope scope("Begin frame kick", EProfileCategory::Renderer);
    if (Globals::openXR.isEnabled())
    {
        m_beginFrameDeferred = true; // xrWaitFrame owns VR pacing and would pin a worker - run at the join instead
        return;
    }
    Globals::jobSystem.submit([this] { beginFrame(); },
        { "Begin frame job", EProfileCategory::Renderer }, EJobPriority::High, &m_beginFrameJobCounter,
        EJobFlag_ForeignWait); // the body waits on m_gpuCollectCounter
}

// After the frame's last light add AND the force update (the App loop, right after it; present()
// kicks as a fallback). Two jobs on one counter: the light grid merge (O(grids + touches) - the
// per-light work already ran inline in addLightInfo) and the force emitter compaction + CPU grid
// build. Both overlap the UI kick, the nav update and present()'s upload phases; the main thread
// only joins (joinGridBuilds), plus the rare exact-fit growth.
void Renderer::kickGridBuilds()
{
    if (m_gridBuildsKicked)
        return;
    m_gridBuildsKicked = true;
    m_submission.settleCounts(); // settle the lock-free claims (present repeats it, harmless)
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    Globals::jobSystem.submit([this, frameIdx] { m_submission.buildLightGrid(m_lightGridComputePipeline, frameIdx); },
        { "Light grid job", EProfileCategory::Renderer }, EJobPriority::High, &m_gridJobCounter); // no waits inside: no ForeignWait

    // The force compaction's size-cull inputs (cheap, main): the CENTRE view, whose TAA jitter never
    // bakes in. VR is excluded - one centre frustum cannot serve both eyes.
    m_force.buildShellCull(!isVrEnabled(), Frustum(getCenterViewProj()), m_cameraPos,
        m_mipPixelScale * 0.5f); // pixelScale: viewportH/2 / tan(fov/2)
    Globals::jobSystem.submit([this, frameIdx] { m_force.buildGrid(m_forceFieldPipeline, frameIdx); },
        { "Force grid job", EProfileCategory::Force }, EJobPriority::High, &m_gridJobCounter); // waits only on its own parallelFor: no ForeignWait
}

void Renderer::joinGridBuilds(uint32 frameIdx, PerFrameData& frameData)
{
    kickGridBuilds(); // no-op when the App loop kicked
    {
        ProfileScope profileScope("Grid builds join", EProfileCategory::Wait);
        Globals::jobSystem.wait(m_gridJobCounter);
    }
    // The rare main-thread part: an exact-fit growth (GPU idle + re-record), then the upload the
    // job skipped. Both are no-ops on the frames that fit, which is nearly all of them.
    m_submission.applyLightGridGrowth(m_lightGridComputePipeline, frameIdx);
    m_force.applyGridGrowth(m_forceFieldPipeline, frameIdx);
}

void Renderer::joinBeginFrameJob()
{
    if (m_beginFrameDeferred)
    {
        m_beginFrameDeferred = false;
        beginFrame(); // VR: synchronous, on the caller's (main) thread
        return;
    }
    Globals::jobSystem.wait(m_beginFrameJobCounter); // helps; near-zero when the kick-to-join work covered it
}

CullView Renderer::setFrameView(const Camera& camera, const Rect& viewportRect)
{
    ProfileScope scope("Frame view", EProfileCategory::Renderer);
    m_frameCamera = camera;
    m_frameRect = viewportRect;
    m_frameViewSet = true;
    setViewportRect(viewportRect); // before anything builds a projection from its aspect

    CullView view;
    if (!Globals::openXR.isEnabled())
    {
        // Same builder buildUboViews uses, so this frustum and the GPU cull's agree bit-exactly.
        m_centerViewProj = computeCenterViewProj(m_frameCamera);
        view.camera = m_frameCamera;
        view.frustum.fromMatrixZO(m_centerViewProj);
        view.valid = true;
    }
    else
    {
        view.camera = m_lastCullCamera;
        view.frustum = m_ubo.frustum; // last frame's, matching m_lastCullCamera
        view.valid = m_hasCullView;
    }
    // The occlusion rasterizer takes camera-relative positions: the translate re-bases the reversed-z
    // center view-projection onto the cull camera.
    view.viewProjRelCamera = m_centerViewProj * glm::translate(glm::mat4(1.0f), view.camera.position);
    view.sunDirection = m_skyParams.sunDirection;
    return view;
}

// VR: poll OpenXR + begin the XR frame, then swap the camera's view for the tracked head pose.
// vrBaseOrientation (play-space anchor derived from the incoming camera) also feeds the per-eye
// views in buildUboViews. No-op on desktop.
void Renderer::applyVrHeadPose(const Camera& cameraIn, Camera& camera, glm::quat& vrBaseOrientation)
{
    if (!Globals::openXR.isEnabled())
        return;
    ProfileScope xrScope("XR poll + begin", EProfileCategory::Renderer);
    Globals::openXR.pollEvents();

    vrBaseOrientation = glm::quat_cast(glm::inverse(cameraIn.viewMatrix)) * cameraIn.playSpaceOrientation;
    if (Globals::openXR.beginFrame())
    {
        glm::mat4 headView;
        glm::vec3 headPos;
        Globals::openXR.getHeadView(cameraIn.position, vrBaseOrientation, headView, headPos);
        camera.viewMatrix = headView;
        camera.position = headPos;
    }
}

// Reacts to LAST frame's overflows/generation bumps before this frame writes anything: capacity
// grows, descriptor sizing, and re-record triggers.
void Renderer::checkFrameCapacities()
{
    ProfileScope capacityScope("Capacity checks", EProfileCategory::Renderer);
    // Mesh instances overflowed mid-frame last frame
    m_instances.growToPendingDemand(m_swapChain.getCurrentFrameIndex());
    // Last frame's instance count (still live here) outgrew the GI TLAS instance buffers.
    m_rt.growTlasInstancesFor(m_instances.getInstanceCount());
    // A mesh mega-buffer was reallocated (vertex/index data growth)
    if (Globals::meshDataManager.getGeneration() != m_meshDataGeneration)
    {
        m_meshDataGeneration = Globals::meshDataManager.getGeneration();
        setHaveToRecordCommandBuffers();
    }

    m_textures.syncCapacity([this]() { waitForGpuAndFlushStaging(); });
    // The light and force grids grow synchronously in present() (joinGridBuilds): their builds are CPU-side.
}

// This slot's fence was waited at the loop top, so its last submitted cull's LOD stats have landed:
// snapshot them for getStats (the mapped buffer is zeroed here for the frame about to record).
void Renderer::snapshotLodStats(PerFrameData& frameData)
{
    for (uint32 i = 0; i < RendererVKLayout::MAX_MESH_LODS; ++i)
        m_lodInstanceCounts[i] = frameData.mappedLodStats[i];
    memset(frameData.mappedLodStats.data(), 0, frameData.mappedLodStats.size_bytes());
    frameData.lodStatsBuffer.flushMappedMemory(vk::WholeSize);
}

// Assembles the frame UBO (matrices, sky/fog/ocean/force/terrain params) into m_ubo and hands it to
// staging - pure CPU math, live-tweak driven, split by subject into the buildUbo* helpers below.
void Renderer::setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity)
{
    m_skyParams.sunDirection = glm::normalize(direction);
    m_skyParams.sunColor = color;
    m_skyParams.sunIntensity = intensity;
}

// ImGui 1.92 bakes glyphs ON DEMAND: NewFrame/Render queue create/update/destroy requests on the
// shared ImTextureData objects of ImGui::GetPlatformIO().Textures, and the backend normally services
// them from inside ImGui_ImplVulkan_RenderDrawData. That would run on MAIN inside present() while
// the widget pass - a post-update job kicked just before present - is inside NewFrame mutating those
// same objects and growing that same vector: a torn upload at best, a freed atlas read at worst. So
// the UI points its snapshot's ImDrawData::Textures at null (the documented "control the timing of
// texture updates yourself" path) and the uploads happen HERE instead, on the main thread in the
// window between the widget pass's join and the next UI::update, when the context is quiescent. A
// glyph baked by pass N uploads at the top of frame N+1, before the present that draws it - and
// THIS is also where pass N's snapshot becomes the one present records (the job only parks it as
// pending): a pass that finished before present N recorded must not be drawn a frame early, with
// its new textures still unuploaded.
void Renderer::updateImGuiTextures()
{
    if (!ImGui::GetCurrentContext())
        return;
    m_imguiDrawData = m_imguiPendingDrawData.load(oc::memory_order_acquire);
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    for (ImTextureData* texture : platformIo.Textures)
        if (texture->Status != ImTextureStatus_OK)
        {
            ProfileScope scope("ImGui texture update", EProfileCategory::Renderer);
            ImGui_ImplVulkan_UpdateTexture(texture);
        }
}

void Renderer::present()
{
    ProfileScope presentScope("Present", EProfileCategory::Renderer);
    if(m_windowMinimized)
    {
        m_debugLineVerts.forEach([](oc::vector<DebugLinePipeline::LineVertex>& verts) { verts.clear(); });
        Globals::openXR.endFrame(nullptr, nullptr, {}, vk::ImageLayout::eUndefined); // balance the begun XR frame
        return;
    }

    // Single-threaded again: settle the lock-free frame counters. Instance claims past capacity
    // never wrote (renderNode's monotonic-cursor overflow path), so the valid prefix ends at the
    // smallest failed claim; light/fog claims past their fixed maxima were dropped the same way.
    m_instances.clampInstanceCountToOverflow();
    m_submission.settleCounts();

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);

    assert(instances.mappedTransforms.size() >= m_instances.getNumTransforms());
    assert(instances.mappedMeshInstances.size() >= m_instances.getInstanceCount());
    assert(instances.mappedFirstInstances.size() >= m_meshInfos.count());
    assert(m_instances.getNumTransforms() == 0 || Globals::textureManager.getNumTextures() > 0 && "Attempting to render object without any textures loaded!");

    ProfileScope uploadScope("Per-frame uploads", EProfileCategory::Renderer);
    // The UBO was uploaded in beginFrame, where the instance counter had just been reset: the TLAS-instance
    // writer's live count is only known here, so patch that one word. (Left at the beginFrame value it
    // read 0 every frame - every TLAS slot inactive, an EMPTY TLAS, no ray hit anywhere.)
    m_ubo.giTlasNumInstances = oc::min(m_instances.getInstanceCount(), m_rt.getMaxTlasInstances());
    Globals::stagingManager.upload(frameData.ubo.getBuffer(), sizeof(uint32), &m_ubo.giTlasNumInstances,
        offsetof(RendererVKLayout::Ubo, giTlasNumInstances));
    ProfileScope bucketScope("Instance buckets + flushes", EProfileCategory::Renderer);
    // Bucket layout for the GPU culls: instances are pushed referencing LOD0, and the cull redirects
    // each one to its selected level - so every member of a LOD chain gets a bucket sized to the
    // CHAIN's instance count (any split of the instances across levels fits). Non-chain meshes keep
    // exact buckets. The expansion can exceed the pushed instance count; the instance-index buffers
    // are sized to m_instances.getMaxInstances(), so grow when the expanded total outruns it.
    uint32 instanceCounter = 0;
    const uint32 numMeshInfos = m_instances.getNumMeshCounters();
    for (uint32 meshIdx = 0; meshIdx < numMeshInfos; ++meshIdx)
    {
        instances.mappedFirstInstances[meshIdx] = instanceCounter;
        const uint32 groupIdx = m_meshLods.getGroupIdxForMesh((uint16)meshIdx);
        instanceCounter += m_instances.getNumInstancesForMesh(
            groupIdx == UINT32_MAX ? meshIdx : m_meshLods.getGroup(groupIdx).meshIdx[0]);
    }
    if (instanceCounter > m_instances.getMaxInstances())
        m_instances.growInstances(instanceCounter, frameIdx);

    const uint32 numNodes = m_instances.getNumTransforms();
    instances.transforms.flushMappedMemory(numNodes * sizeof(RendererVKLayout::RenderNodeTransform));
    instances.passMasks.flushMappedMemory(numNodes * sizeof(uint32));
    instances.lodStateBias.flushMappedMemory(numNodes * sizeof(int32));
    instances.meshInstances.flushMappedMemory(m_instances.getInstanceCount() * sizeof(RendererVKLayout::InMeshInstance));
    instances.firstInstances.flushMappedMemory(numMeshInfos * sizeof(uint32));
    m_submission.flushFrame(frameIdx);

    instances.mappedMeshCount[0] = m_meshInfos.count(); // DGC sequence count for this frame
    // The tessellated terrain's draws walk every mesh slot; with tessellation off they walk none (the
    // recorded draws stay in the cached command buffers either way).
    instances.mappedMeshCount[1] = m_terrain.getTexTweaks().tessEnabled ? m_meshInfos.count() : 0u;
    instances.meshCount.flushMappedMemory(2 * sizeof(uint32));


    bucketScope.stop();
    // Debug overlay lines accumulated since the last present (safe here: this slot's fence was waited
    // in beginFrame). First use lazily creates the GPU buffers -> re-record to pick up the new pass.
    ProfileScope debugLineScope("Debug lines upload", EProfileCategory::Renderer);
    if (m_debugLinePipeline.upload(frameIdx, m_debugLineVerts)) // drains + clears the per-worker lists
        setHaveToRecordCommandBuffers();
    debugLineScope.stop();

    // Only spend the pool reset on a frame that will actually execute the sim (see
    // ParticleState); the flag is cleared after this frame's successful submit below.
    const bool particleResetCarried = m_particles.isResetPending() && m_particles.isEnabled() && m_instances.getInstanceCount() > 0;
    { // Particle emitter table + spawn map + decals into this slot's mapped buffers (fence waited).
        ProfileScope effectsScope("Effects upload", EProfileCategory::Renderer);
        // SIM delta, not a wall-clock difference: the GPU particle sim must freeze with the global
        // pause (present runs once per Time frame, so this is the same interval when unpaused).
        const float dt = oc::min((float)Globals::time.getSimDeltaSec(), 0.25f);
        uint32 spawnRequestTotal = 0;
        for (const auto& [slot, count] : m_particles.getSpawnRequests())
            spawnRequestTotal += count;
        m_particlePipeline.update(frameIdx, m_particles.getEmitters(), m_particles.getSpawnRequests(),
            dt * m_particles.getParams().timeScale, m_particles.getParams().collision, particleResetCarried);
        m_particles.clearSpawnRequests();
        m_decalPipeline.upload(frameIdx, m_submission.getDecalCount());
        // The force emitter compaction + grid build ride the grid jobs (kickGridBuilds / joinGridBuilds).

        if (m_particles.isEnabled())
        {
            const ParticlePipeline::DebugCounters counters = m_particlePipeline.getDebugCounters(frameIdx);
            if (m_particles.getParams().logStats && m_frameCounter % 120 == 0)
                printf("Particles: alive %u/%u (parity 0/1), dead %d, simGroups %u, emitters %u, spawn reqs %u, GPU spawns %u, dropped %u, broken %u, decals %u\n",
                    counters.alive[0], counters.alive[1], counters.deadCount, counters.simGroups,
                    m_particles.getNumEmitters(), spawnRequestTotal, counters.gpuSpawns,
                    counters.dropSpawns, counters.dropBroken, m_submission.getDecalCount());
            // Pool exhaustion warning, ALWAYS on: a full pool makes every emit drop its spawn without a
            // trace, so the whole system looks switched off. Logged on the first frame it happens and
            // then at most once a second, with a recovery line, so a burst cannot flood the console.
            if (counters.dropSpawns > 0)
            {
                if (m_particles.shouldLogDrop(m_frameCounter))
                {
                    m_particles.noteDropLogged(m_frameCounter);
                    printf("Particles: POOL EXHAUSTED - dropped %u spawns this frame (alive %u, dead %d of %u, GPU spawns %u, emitters %u)\n",
                        counters.dropSpawns, counters.alive[1 - (frameIdx & 1u)], counters.deadCount,
                        RendererVKLayout::MAX_PARTICLES, counters.gpuSpawns, m_particles.getNumEmitters());
                }
            }
            else if (m_particles.wasDropping())
            {
                printf("Particles: pool recovered (dead %d of %u)\n", counters.deadCount, RendererVKLayout::MAX_PARTICLES);
                m_particles.clearDropping();
            }
            // A non-finite particle used to be an immortal invisible pool slot; the sim now retires it.
            // A steady count means a live NaN source upstream, so say so once a second.
            if (counters.dropBroken > 0 && m_frameCounter % 300 == 0)
                printf("Particles: retired %u non-finite particles this frame (NaN/Inf source upstream)\n", counters.dropBroken);
        }
    }

    {
        ProfileScope computeScope("Cull/skin update", EProfileCategory::Renderer);
        m_indirectCullComputePipeline.update(frameIdx, m_instances.getInstanceCount());
        m_skinningComputePipeline.update(frameIdx, m_skinned.getPalettes(), m_skinned.getJobs());
    }
    uploadScope.stop();

    {
        // Texture streaming step: folds this frame's priority pass, issues/completes mip streaming ops.
        // Must run before the staging update so stream-in uploads join this frame's staging batch.
        ProfileScope profileScope("Streamers", EProfileCategory::Renderer);
        Globals::textureStreamer.update();
        Globals::meshStreamer.update();
    }

    // Catch texture-array growth from containers loaded AFTER beginFrame (terrain streaming, mid-frame
    // spawns): without this, this frame's record writes past the descriptor capacity beginFrame sized.
    m_textures.syncCapacity([this]() { waitForGpuAndFlushStaging(); });

    // Same catch-up for the mesh vertex/index mega-buffers (beginFrame only samples the generation once,
    // early): a mid-frame container load (terrain streaming spawning a new chunk) can grow them here, via
    // MeshDataManager::growBuffer destroying the old buffer. Without this, recordCommandBuffers below would
    // reuse this frame's already-recorded secondary command buffers, which still reference (e.g. via a
    // baked vkCmdBindIndexBuffer) the now-destroyed old buffer - "invalidated because ... was destroyed".
    if (Globals::meshDataManager.getGeneration() != m_meshDataGeneration)
    {
        m_meshDataGeneration = Globals::meshDataManager.getGeneration();
        setHaveToRecordCommandBuffers();
    }

    if (m_textures.hasPendingFrees())
    {
        // A container was destroyed this frame; its textures may still be sampled by an in-flight frame,
        // so drain before freeing them. Their bindless slots rewrite to the fallback in recordCommandBuffers
        // below, before anything is submitted.
        ProfileScope profileScope("Texture free drain", EProfileCategory::Wait);
        auto waitResult = Globals::device.graphicsQueueWaitIdle();
        assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle before freeing textures");
        m_textures.processPendingFrees();
    }

    joinGridBuilds(frameIdx, frameData); // before the staging update: a growth waits the GPU and flushes staging

    vk::Semaphore waitSemaphore;
    {
        ProfileScope profileScope("Staging", EProfileCategory::Renderer);
        waitSemaphore = Globals::stagingManager.update();
    }
    {
        // Can block on the presentation engine (vsync backpressure lands here or in the fence wait).
        ProfileScope profileScope("Acquire image", EProfileCategory::Wait);
        if (!m_swapChain.acquireNextImage())
        {
            // The primary CB will not be submitted this frame, so registering the staging semaphore on
            // it would leave the signal without a waiter - and the next flush that cycles back to that
            // semaphore would re-signal it while still signaled (invalid for a binary semaphore). Hand
            // it back to the chain instead: the next flush waits it.
            Globals::stagingManager.restoreChainSemaphore(waitSemaphore);
            Globals::openXR.endFrame(nullptr, nullptr, {}, vk::ImageLayout::eUndefined); // balance the begun XR frame
            recreateSwapchain();
            return;
        }
    }
    if (waitSemaphore != VK_NULL_HANDLE)
        frameData.primaryCommandBuffer.addWaitSemaphore(waitSemaphore, vk::PipelineStageFlagBits::eAllCommands); // eAllCommands (not just transfer) for GI BLAS
    {
        ProfileScope profileScope("Record command buffers", EProfileCategory::Renderer);
        recordCommandBuffers();
    }

    m_gpuProfiler.onSubmit(frameIdx); // CPU-time anchor for the GPU timeline fallback
    {
        ProfileScope profileScope("Submit", EProfileCategory::Renderer);
        m_swapChain.submitCommandBuffer(getCurrentCommandBuffer());
    }
    // The reset actually reached the GPU with this submission; until here any early-out above
    // (acquire failure -> recreateSwapchain return) keeps it pending for the next frame.
    if (particleResetCarried)
        m_particles.clearResetPending();
    // Latch this frame's rain occlusion request for the NEXT frame's begin-frame job (the request is
    // main-thread state written between the join and here); a frame with no request switches it off.
    m_particles.latchRainVolume();
    m_frameViewSet = false; // the next frame must publish its own view
    // This frame's submission is now new GPU work that could read the shared mesh/material/instance-offset
    // buffers; any upload into them from here on needs a fresh drain. See StagingManager::ensureDrainedForSharedWrite.
    Globals::stagingManager.resetSharedWriteGate();

    Globals::openXR.endFrame(m_vrEyes.getColorImage(0), m_vrEyes.getColorImage(1), m_swapChain.getLayout().extent, vk::ImageLayout::ePresentSrcKHR);

    {
        ProfileScope profileScope("Queue present", EProfileCategory::Wait);
        if (!m_swapChain.present())
        {
            recreateSwapchain();
        }
    }
    m_frameSlotWaited = false; // the slot advanced: next frame must wait its own fence first
}

void Renderer::initImgui(Window& window)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // Layout persists in Assets/Local (the cwd is Assets/, see FileSystem::initialize), not next to
    // the checked-in assets. ImGui fopen()s this path itself on its own save timer.
    FileSystem::createDirectories("Local", /*allowMainThread*/ true);
    io.IniFilename = "Local/imgui.ini";

    // Creates the SDL system cursors among other window-affine work: on the window thread.
    window.runOnWindowThread([&] { ImGui_ImplSDL3_InitForVulkan((SDL_Window*)window.getWindowHandle()); }, /*wait*/ true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = Globals::instance.getApiVersion();
    init_info.Instance = Globals::instance.getInstance();
    init_info.PhysicalDevice = Globals::device.getPhysicalDevice();
    init_info.Device = Globals::device.getDevice();
    init_info.QueueFamily = Globals::device.getGraphicsQueueIndex();
    init_info.Queue = Globals::device.getGraphicsQueue();
    init_info.PipelineCache = nullptr;
    init_info.DescriptorPool = nullptr;
    init_info.DescriptorPoolSize = 10;
    init_info.MinImageCount = 2;
    init_info.ImageCount = 2;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = imgui_check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    ImGui_ImplVulkan_PipelineInfo pipeline_create;
    pipeline_create.RenderPass = m_renderPass.getRenderPass();
    pipeline_create.Subpass = 0;
    pipeline_create.MSAASamples = {};
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline_create);
}

Stats Renderer::getStats()
{
    Stats stats;

    stats.numLights = m_submission.getLightCount();
    stats.maxLights = RendererVKLayout::MAX_LIGHTS;

    stats.numMeshInstances = m_instances.getInstanceCount();
    stats.maxMeshInstances = m_instances.getMaxInstances();

    stats.numInstanceOffsets = m_instanceOffsets.count();
    stats.maxInstanceOffsets = m_instanceOffsets.capacity();

    stats.numMeshTypes = m_meshInfos.count();
    stats.maxMeshTypes = m_meshInfos.capacity();

    stats.numMaterials = m_materials.count();
    stats.maxMaterials = m_materials.capacity();

    stats.numRenderNodes = m_instances.getNumLiveTransforms();
    stats.maxRenderNodes = m_instances.getMaxRenderNodes();

    stats.numTextures = (uint32)Globals::textureManager.getNumTextures();
	stats.maxTextures = Globals::textureManager.getMaxTextures();

	stats.vertexDataUsedBytes = Globals::meshDataManager.getVertexBufUsed();
	stats.maxVertexDataBytes = Globals::meshDataManager.getVertexBufSize();

	stats.indexDataUsedBytes = Globals::meshDataManager.getIndexBufUsed();
	stats.maxIndexDataBytes = Globals::meshDataManager.getIndexBufSize();

    stats.numObjectContainers = (uint32)m_objectContainers.size();

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();

    const uint32 lastFrameIdx = (frameIdx - 1) % (uint32)m_perFrameData.size();
    struct LightGridInfo
    {
        uint32 inout_numGrids;
        uint32 inout_gridDataCounter;
    } info;
    oc::span<LightGridInfo> infoSpan = m_submission.slot(lastFrameIdx).lightTable.mapMemory<LightGridInfo>(0, sizeof(LightGridInfo));
    //m_submission.slot(lastFrameIdx).lightTable.flushMappedMemory(sizeof(LightGridInfo));
    info = *infoSpan.data();
    m_submission.slot(lastFrameIdx).lightTable.unmapMemory();
    stats.numLightGrids = info.inout_numGrids;
    stats.maxLightGrids = m_submission.getLightTableEntries() / 2; // We don't want to go over 50% because of hash table collisions
    stats.lightGridMemUsageBytes = info.inout_gridDataCounter * sizeof(uint32);
    stats.maxLightGridMemUsageBytes = m_submission.getLightGridBufferSize();

    const Allocator::MemoryUsage gpuMem = Globals::gpuAllocator.getMemoryUsage();
    stats.gpuMemoryUsedBytes = gpuMem.usedBytes;
    stats.gpuMemoryReservedBytes = gpuMem.reservedBytes;
    stats.gpuMemoryBudgetBytes = gpuMem.budgetBytes;

    const TextureStreamer::StreamerStats streamStats = Globals::textureStreamer.getStats();
    stats.textureBudgetBytes = streamStats.budgetBytes;
    stats.textureResidentBytes = streamStats.residentBytes;
    stats.texturePinnedBytes = streamStats.pinnedBytes;
    stats.textureDesiredBytes = streamStats.desiredBytes;
    stats.textureTailBytes = streamStats.tailBytes;
    stats.numStreamableTextures = streamStats.numStreamable;
    stats.numStreamOpsInFlight = streamStats.numOpsInFlight;

    stats.blasBytes = m_rt.accel().getBlasTotalBytes();
    stats.blasCompactionSavedBytes = m_rt.accel().getCompactionSavedBytes();

    const MeshStreamer::Stats meshStreamStats = Globals::meshStreamer.getStats();
    stats.meshBudgetBytes = meshStreamStats.budgetBytes;
    stats.meshStreamableBytes = meshStreamStats.streamableBytes;
    stats.meshResidentBytes = meshStreamStats.residentBytes;
    stats.meshColdBytes = meshStreamStats.coldBytes;
    stats.numMeshSets = meshStreamStats.numSets;
    stats.numEvictedMeshSets = meshStreamStats.numEvictedSets;

    stats.numMeshLodGroups = m_meshLods.getNumGroups();
    static_assert(sizeof(stats.lodInstanceCounts) == sizeof(uint32) * RendererVKLayout::MAX_MESH_LODS);
    // GPU-written, snapshotted in beginFrame - a few frames behind, and counting VISIBLE picks only.
    for (uint32 i = 0; i < RendererVKLayout::MAX_MESH_LODS; ++i)
        stats.lodInstanceCounts[i] = m_lodInstanceCounts[i];

    return stats;
}
