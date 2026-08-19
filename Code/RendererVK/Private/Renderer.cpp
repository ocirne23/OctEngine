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
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::~RendererVK");
    }
    // m_pendingTextureFrees is NOT processed here: ~TextureManager (init_seg XCU4, destroyed before
    // this XCU3 global) destroys every texture wholesale, pending ones included.
    Globals::textureStreamer.shutdown(); // stop the disk worker + retire swapped-out images while the device is idle
    Globals::meshStreamer.shutdown();
    destroyEyeCompositeTargets();
    ImGui_ImplVulkan_Shutdown();
}

bool Renderer::initialize(Window& window, EValidation validation, EVSync vsync, EVr vr)
{
    ProfileScope scope("Renderer::initialize", EProfileCategory::Renderer);

    // Disable layers we don't care about for now to eliminate potential issues
    _putenv("DISABLE_LAYER_NV_OPTIMUS_1=True");
    _putenv("DISABLE_VULKAN_OW_OVERLAY_LAYER=True");
    _putenv("DISABLE_VULKAN_OW_OBS_CAPTURE=True");
    _putenv("DISABLE_VULKAN_OBS_CAPTURE=True");

    // Per-worker staging for the lock-free submission surface (requires the JobSystem first).
    assert(Globals::jobSystem.getNumContexts() != 0 && "initialize the JobSystem before the Renderer");
    Globals::renderNodeDirtyLists.initialize();
    m_debugLineVerts.initialize();

    auto rerecordCallback = [this]() { setHaveToRecordCommandBuffers(); };
    m_skyParams.registerTweaks();
    m_shadowParams.registerTweaks();
    m_fogParams.registerTweaks();
    m_rtParams.registerTweaks();
    m_rtaoParams.registerTweaks(rerecordCallback, [this]() { if (Globals::device.getGraphicsQueue().waitIdle() != vk::Result::eSuccess) return; m_rtaoPipeline.reloadShaders(); setHaveToRecordCommandBuffers(); });
    m_taaParams.registerTweaks(rerecordCallback);
    m_postParams.registerTweaks(rerecordCallback);
    m_lodParams.registerTweaks();
    // Wireframe is baked pipeline state (polygonMode), so flipping it rebuilds the static mesh pipeline —
    // same GPU-idle + reload pattern as the RTAO alpha-test and ocean hit-lighting tweaks.
    Tweak::boolean("Editor", "Wireframe", &m_wireframe, [this]() {
        if (Globals::device.getGraphicsQueue().waitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setWireframe(m_wireframe);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_maxTextures);
        setHaveToRecordCommandBuffers();
    });
    // Depth prepass reuse binds the G-buffer prepass depth READ-ONLY as the scene pass depth (no copy):
    // flipping it swaps the scene render pass AND every scene pipeline's depthWrite, so rebuild like the
    // wireframe toggle.
    Tweak::boolean("Spatial", "Depth prepass reuse", &m_depthPrepassReuse, [this]() {
        if (Globals::device.getGraphicsQueue().waitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setDepthReadOnly(m_depthPrepassReuse);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_maxTextures);
        m_giProbePipeline.setDebugDepthReadOnly(m_depthPrepassReuse);
        m_giProbePipeline.reloadDebugShaders(m_perFrameData[0].sceneColor.getRenderPass());
        setHaveToRecordCommandBuffers();
    });
    // Live toggles: the primary CB re-records every frame, so no re-record callback is needed.
    Tweak::boolean("Particles", "Enabled", &m_particlesEnabled);
    Tweak::boolean("Particles", "Depth collision", &m_particleCollision);
    Tweak::floatVar("Particles", "Time scale", &m_particleTimeScale, 0.0f, 4.0f);
    Tweak::boolean("Particles", "Log stats", &m_particleLogStats);
    Tweak::boolean("Decals", "Enabled", &m_decalsEnabled);
    Globals::meshStreamer.initialize();

    glslang::InitializeProcess();
    const bool enableValidationLayers = (validation == EValidation::ENABLED);
    if (enableValidationLayers)
    {
        _putenv("VK_LAYER_PRINTF_ENABLE=1");
        _putenv("VK_LAYER_PRINTF_TO_STDOUT=0");
        //_putenv("VK_LAYER_PRINTF_BUFFER_SIZE=16777216");
    }
    m_vsyncEnabled = (vsync == EVSync::ENABLED);

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

    const vk::Extent2D ext = m_swapChain.getLayout().extent;

    m_sceneViewCount = Globals::openXR.isEnabled() ? 2u : 1u;

    // G-buffer first: the SceneColor reuse render pass binds its depth views directly (depth-prepass reuse).
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.gbuffer.initialize(ext.width, ext.height, m_sceneViewCount);
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount,
            { perFrame.gbuffer.getDepthView(0), perFrame.gbuffer.getDepthView(m_sceneViewCount > 1 ? 1 : 0) });
    }
    const vk::RenderPass sceneRenderPass = m_perFrameData[0].sceneColor.getRenderPass();

    m_maxTextures = Globals::textureManager.getDescriptorCap(); // fixed layout cap; live count grows separately
    m_staticMeshGraphicsPipeline.setDepthReadOnly(m_depthPrepassReuse); // tweak may have restored a saved value
    m_staticMeshGraphicsPipeline.initialize(sceneRenderPass, m_maxUniqueMeshes, m_maxTextures, m_sceneViewCount > 1);
    m_rtaoPipeline.initialize(&m_rtaoParams, ext.width, ext.height, m_maxTextures, m_numTextureDescriptors, m_sceneViewCount);
    m_oceanSimPipeline.initialize();
    m_volumetricFogPipeline.initialize();
    m_volumetricFogPipeline.initializeApply(sceneRenderPass, m_sceneViewCount);
    m_fogTerrainMap.initialize(RendererVKLayout::FOG_TERRAIN_RES, RendererVKLayout::FOG_TERRAIN_CASCADES, 4, "FogTerrainHeight"); // RGBA: terrain height, water level, fog thickness, spare
    m_taaPipeline.initialize(ext.width, ext.height, m_sceneViewCount);
    m_eyeAdaptationPipeline.initialize();
    m_compositePipeline.initialize(m_renderPass);
    m_indirectCullComputePipeline.initialize(m_maxInstanceData, m_maxUniqueMeshes);
    m_skinningComputePipeline.initialize(m_maxSkinningPaletteEntries, m_maxSkinningJobs);
    m_lightGridComputePipeline.initialize();
    m_accelStructure.initialize(m_maxUniqueMeshes);
    m_giProbePipeline.initialize(m_maxGiTlasInstances, m_maxTextures, m_numTextureDescriptors);
    m_giProbePipeline.setDebugDepthReadOnly(m_depthPrepassReuse);
    m_giProbePipeline.initializeDebug(sceneRenderPass);
    m_debugLinePipeline.initialize(sceneRenderPass);
    m_particlePipeline.initialize(sceneRenderPass, m_maxTextures, m_numTextureDescriptors, m_sceneViewCount);
    m_decalPipeline.initialize(sceneRenderPass, m_maxTextures, m_numTextureDescriptors, m_sceneViewCount);
    m_forceFieldPipeline.initialize(sceneRenderPass, m_sceneViewCount);


    m_shadowCullComputePipeline.initialize(m_maxInstanceData, m_maxUniqueMeshes);
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.shadowMap.initialize();
    m_shadowMapGraphicsPipeline.initialize(m_perFrameData[0].shadowMap, m_maxUniqueMeshes, m_maxTextures);
    m_gbufferPipeline.initialize(m_perFrameData[0].gbuffer, m_maxTextures);

    vk::Device vkDevice = Globals::device.getDevice();

    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.indirectCullPipelineDescriptorSet.initialize(m_indirectCullComputePipeline.getDescriptorSetLayout());
        perFrame.skinningDescriptorSet.initialize(m_skinningComputePipeline.getDescriptorSetLayout());
        perFrame.lightGridPipelineDescriptorSet.initialize(m_lightGridComputePipeline.getDescriptorSetLayout());
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
        {
            perFrame.staticMeshPipelineDescriptorSet[eye].initialize(m_staticMeshGraphicsPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);
            perFrame.gbufferDescriptorSet[eye].initialize(m_gbufferPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);
        }
        perFrame.compositeDescriptorSet.initialize(m_compositePipeline.getDescriptorSetLayout());

        perFrame.shadowCullDescriptorSet.initialize(m_shadowCullComputePipeline.getDescriptorSetLayout());
        perFrame.shadowDrawDescriptorSet.initialize(m_shadowMapGraphicsPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);

        perFrame.primaryCommandBuffer.initialize(vk::CommandBufferLevel::ePrimary);
        perFrame.staticMeshCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.gbufferCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.aoCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.indirectCullCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.skinningCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.oceanSimCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.lightGridCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.imguiCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.shadowCullCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.shadowDrawCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.globalIllumCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.volumetricFogCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.fogApplyCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.giProbeDebugCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.debugLineCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.particleSimCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.particleCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.decalCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.forceFieldCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.forceComputeCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.taaCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.eyeAdaptCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);
        perFrame.compositeCommandBuffer.initialize(vk::CommandBufferLevel::eSecondary);

        perFrame.ubo.initialize(sizeof(RendererVKLayout::Ubo),
            vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "Ubo");

        perFrame.inRenderNodeTransformsBuffer.initialize(m_maxRenderNodes * sizeof(RendererVKLayout::RenderNodeTransform),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "RenderNodeTransforms", BufferHostAccess::eSequentialWrite);
        perFrame.mappedRenderNodeTransforms = perFrame.inRenderNodeTransformsBuffer.mapMemory<RendererVKLayout::RenderNodeTransform>();

        perFrame.inNodePassMasksBuffer.initialize(m_maxRenderNodes * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "NodePassMasks", BufferHostAccess::eSequentialWrite);
        perFrame.mappedNodePassMasks = perFrame.inNodePassMasksBuffer.mapMemory<uint32>();

        perFrame.inNodeLodStateBiasBuffer.initialize(m_maxRenderNodes * sizeof(int32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "NodeLodStateBias", BufferHostAccess::eSequentialWrite);
        perFrame.mappedNodeLodStateBias = perFrame.inNodeLodStateBiasBuffer.mapMemory<int32>();

        perFrame.lodStatsBuffer.initialize(RendererVKLayout::MAX_MESH_LODS * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LodStats");
        perFrame.mappedLodStats = perFrame.lodStatsBuffer.mapMemory<uint32>();
        memset(perFrame.mappedLodStats.data(), 0, perFrame.mappedLodStats.size_bytes());

        // Kept cached/random: growMeshInstanceCapacity reads the existing mapping to preserve in-flight
        // instances across a resize, so this buffer must stay CPU-readable.
        perFrame.inMeshInstancesBuffer.initialize(m_maxInstanceData * sizeof(RendererVKLayout::InMeshInstance),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "MeshInstances");
        perFrame.mappedMeshInstances = perFrame.inMeshInstancesBuffer.mapMemory<RendererVKLayout::InMeshInstance>();

        perFrame.inFirstInstancesBuffer.initialize(m_maxUniqueMeshes * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "FirstInstances", BufferHostAccess::eSequentialWrite);
        perFrame.mappedFirstInstances = perFrame.inFirstInstancesBuffer.mapMemory<uint32>();

        // Indirect usage: consumed by DGC (sequenceCountAddress) and drawIndexedIndirectCount.
        perFrame.meshCountBuffer.initialize(sizeof(uint32),
            vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "MeshCount", BufferHostAccess::eSequentialWrite);
        perFrame.mappedMeshCount = perFrame.meshCountBuffer.mapMemory<uint32>();
        perFrame.mappedMeshCount[0] = 0;
        perFrame.meshCountBuffer.flushMappedMemory(sizeof(uint32));

        perFrame.lightInfosBuffer.initialize(sizeof(RendererVKLayout::LightInfo) * RendererVKLayout::MAX_LIGHTS,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightInfos", BufferHostAccess::eSequentialWrite);
        perFrame.mappedLightInfos = perFrame.lightInfosBuffer.mapMemory<RendererVKLayout::LightInfo>();

        perFrame.fogVolumesBuffer.initialize(sizeof(RendererVKLayout::FogVolumes),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "FogVolumes", BufferHostAccess::eSequentialWrite);
        perFrame.mappedFogVolumes = perFrame.fogVolumesBuffer.mapMemory<RendererVKLayout::FogVolumes>();
        perFrame.mappedFogVolumes.data()->count = 0;

    }
    createLightGridBuffers();

    if (m_sceneViewCount > 1)
    {
        for (uint32 i = 0; i < 2; ++i)
            m_vrCompositeDescriptorSet[i].initialize(m_compositePipeline.getDescriptorSetLayout());
        createEyeCompositeTargets();
    }

    m_meshInfosBuffer.initialize(m_maxUniqueMeshes * sizeof(RendererVKLayout::MeshInfo),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, true, "MeshInfos");

    m_materialInfosBuffer.initialize(m_maxUniqueMaterials * sizeof(RendererVKLayout::MaterialInfo),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, true, "MaterialInfos");

    m_instanceOffsetsBuffer.initialize(m_maxInstanceOffsets * sizeof(RendererVKLayout::MeshInstanceOffset),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, true, "InstanceOffsets");

    m_meshLodGroupIdxBuffer.initialize(m_maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroupIdx");
    m_meshLodGroupsBuffer.initialize(m_maxMeshLodGroups * sizeof(RendererVKLayout::GpuMeshLodGroup),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroups");
    m_lodLevelStateBuffer.initialize(m_maxLodStateSlots * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LodLevelState");

	uint16 diffuseIdx = Globals::textureManager.upload(*ITextureData::createFallbackWhiteTexture(), false);
	assert(diffuseIdx == RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX);
	uint16 normalIdx = Globals::textureManager.upload(*ITextureData::createFallbackNormalTexture(), false);
	assert(normalIdx == RendererVKLayout::FALLBACK_NORMAL_TEX_IDX);

    m_gpuCrashTracker.Initialize(false);

    m_initialized = true; // headless server mode never calls initialize; renderer-touching paths gate on this
    return true;
}

void Renderer::recreateWindowSurface(Window& window)
{
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
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
    {
        perFrame.gbuffer.initialize(ext.width, ext.height, m_sceneViewCount);
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount,
            { perFrame.gbuffer.getDepthView(0), perFrame.gbuffer.getDepthView(m_sceneViewCount > 1 ? 1 : 0) });
    }
    createEyeCompositeTargets(); // VR: resize the per-eye LDR composite targets

    // The cached scene command buffers embed the (now-recreated) scene-colour render pass in their
    // inheritance info, so force them to re-record against the new handle.
    setHaveToRecordCommandBuffers();
    auto waitResult2 = Globals::device.getGraphicsQueue().waitIdle();
    if (waitResult2 != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::recreateWindowSurface");
    }
}

void Renderer::recreateSwapchain()
{
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
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
    {
        perFrame.gbuffer.initialize(ext.width, ext.height, m_sceneViewCount);
        perFrame.sceneColor.initialize(RendererVKLayout::SCENE_COLOR_FORMAT, ext.width, ext.height, m_sceneViewCount,
            { perFrame.gbuffer.getDepthView(0), perFrame.gbuffer.getDepthView(m_sceneViewCount > 1 ? 1 : 0) });
    }
    createEyeCompositeTargets(); // VR: resize the per-eye LDR composite targets

    // Cached scene command buffers reference the recreated scene-colour render pass; re-record them.
    setHaveToRecordCommandBuffers();
}

void Renderer::reloadShaders()
{
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in RendererVK::reloadShaders");
        return;
    }
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();

    m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_maxTextures);
    m_gbufferPipeline.reloadShaders(m_perFrameData[m_swapChain.getPrevFrameIdx()].gbuffer);
    m_rtaoPipeline.reloadShaders();
    m_oceanSimPipeline.reloadShaders();
    m_volumetricFogPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
    m_indirectCullComputePipeline.reloadShaders();
    m_skinningComputePipeline.reloadShaders();
    m_lightGridComputePipeline.reloadShaders();
    m_shadowCullComputePipeline.reloadShaders();
    m_shadowMapGraphicsPipeline.reloadShaders(m_maxTextures);
    m_giProbePipeline.reloadShaders(m_maxTextures);
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
    // OCEAN_HIT_LIGHTS is a compile-time variant define: flipping the tweak rebuilds the ocean fragment
    // pipeline (GPU idle first — cached CBs reference the old pipeline; the re-record this queues happens
    // in present(), so a mid-frame toggle is safe). Same pattern as the RTAO alpha-test tweak.
    const bool rebuildOceanVariant = ocean.hitLighting != m_oceanParams.hitLighting;
    m_oceanParams = ocean;
    if (rebuildOceanVariant)
    {
        if (Globals::device.getGraphicsQueue().waitIdle() != vk::Result::eSuccess)
            return;
        m_staticMeshGraphicsPipeline.setOceanHitLights(ocean.hitLighting);
        m_staticMeshGraphicsPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass(), m_maxTextures);
        setHaveToRecordCommandBuffers();
    }
}

void Renderer::setWindowMinimized(bool minimized)
{
    m_windowMinimized = minimized;
}

void Renderer::setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts)
{
    assert(mats.size() <= RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS);
    assert((size_t)counts.numGround + counts.numRock + (counts.hasBeach ? 1 : 0) + (counts.hasSnow ? 1 : 0) == mats.size()
        && "counts must describe the whole [ground][rock][beach?][snow?] span");
    // Replacing a live set: the old images may still be sampled in flight — same deferred free path as
    // ObjectContainer teardown (processed in present() after the GPU drain). The old material slots are
    // not recycled (nothing tracks their range), but a set replacement is a rare config-refresh event.
    m_pendingTextureFrees.insert(m_pendingTextureFrees.end(), m_terrainSplatTextures.begin(), m_terrainSplatTextures.end());
    m_terrainSplatTextures.clear();

    oc::vector<RendererVKLayout::MaterialInfo> materialInfos;
    materialInfos.reserve(mats.size());
    for (size_t i = 0; i < mats.size(); ++i)
    {
        const TerrainSplatMaterial& mat = mats[i];
        RendererVKLayout::MaterialInfo& info = materialInfos.emplace_back();
        info.flags = 0;
        info.opacity = 1.0f;
        info.alphaMode = (uint16)RendererVKLayout::EAlphaMode::Opaque;
        info.diffuseTexIdx = RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX;
        info.normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
        info.metalRoughnessTexIdx = UINT16_MAX;

        const auto upload = [&](const oc::string& path, bool sRGB) -> uint16 {
            if (path.empty())
                return UINT16_MAX;
            const uint16 idx = Globals::textureManager.upload(path.c_str(), true, sRGB);
            if (idx != UINT16_MAX)
                m_terrainSplatTextures.push_back(idx);
            return idx;
        };
        if (const uint16 idx = upload(mat.diffuseDds, true); idx != UINT16_MAX)
            info.diffuseTexIdx = idx;
        if (const uint16 idx = upload(mat.normalDds, false); idx != UINT16_MAX)
        {
            info.normalTexIdx = idx;
            const vk::Format normalFormat = Globals::textureManager.getTexture(idx).getFormat();
            if (normalFormat == vk::Format::eBc5UnormBlock || normalFormat == vk::Format::eBc5SnormBlock)
                info.flags |= RendererVKLayout::MATERIAL_FLAG_BC5_NORMAL;
        }
        if (const uint16 idx = upload(mat.armDds, false); idx != UINT16_MAX)
            info.metalRoughnessTexIdx = idx;

    }

    m_terrainSplatBaseMaterial = (int32)addMaterialInfos(materialInfos);
    m_terrainSplatCounts = counts;
}

void Renderer::setTerrainSplatClimate(oc::span<const glm::vec4> boxes)
{
    assert(boxes.size() <= RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS);
    memcpy(m_terrainSplatClimate, boxes.data(), boxes.size() * sizeof(glm::vec4));
}

void Renderer::waitFrameSlot()
{
    // This frame slot's fence must be waited BEFORE anything writes its host-visible per-frame buffers
    // (renderNode instance memcpys, LOD meshIdx redirects, firstInstance prefix sums, sparse transform
    // uploads) — the wait inside acquireNextImage happens at the END of the CPU frame, after all those
    // writes. Without this, the CPU scribbles over the slot while frame N-2 still reads it on the GPU;
    // invisible while per-frame data is byte-identical, but LOD switches change instance meshIdx en
    // masse and the torn instance/prefix data made the cull's buckets overflow into neighbouring
    // meshes' draw ranges (one-frame flashes with foreign materials).
    // It runs at the TOP of the main loop (not inside beginFrame) so the vsync/GPU stall lands
    // before input sampling rather than between input and the sim; the slot index only advances in
    // present(), so it is the same fence either way.
    if (m_frameSlotWaited)
        return; // idempotent within a frame
    // GPU-bound (or vsync-throttled) frames show up here: the CPU waiting for the frame slot's fence.
    ProfileScope profileScope("Frame wait", EProfileCategory::Wait);
    if (!m_swapChain.waitForFrame(m_swapChain.getCurrentFrameIndex()))
    {
        Log::error("Renderer: failed to wait for frame, recreating swapchain");
        recreateSwapchain();
    }
    m_frameSlotWaited = true;
}

const Frustum& Renderer::beginFrame(const Camera& cameraIn, const Rect& viewportRect)
{
    ProfileScope beginFrameScope("Begin frame", EProfileCategory::Renderer);
    setViewportRect(viewportRect); // first: this frame's projection is built from the viewport's aspect below

    // The slot's fence is waited at the loop top (waitFrameSlot); anything that reached here without
    // it is a main-loop ordering bug — but never run unsynchronized, so wait now (asserting).
    assert(m_frameSlotWaited && "Renderer::waitFrameSlot() must run at the top of the frame, before any per-slot writes");
    waitFrameSlot();

    // This slot's fence is waited, so its previous submission's GPU timestamps have landed.
    m_gpuProfiler.collect(m_swapChain.getCurrentFrameIndex());

    Globals::openXR.pollEvents();

    Camera camera = cameraIn;
    const glm::quat vrBaseOrientation = glm::quat_cast(glm::inverse(cameraIn.viewMatrix)) * cameraIn.playSpaceOrientation;
    if (Globals::openXR.beginFrame())
    {
        glm::mat4 headView;
        glm::vec3 headPos;
        Globals::openXR.getHeadView(cameraIn.position, vrBaseOrientation, headView, headPos);
        camera.viewMatrix = headView;
        camera.position = headPos;
    }

    // Mesh instances overflowed mid-frame last frame
    if (m_pendingMaxInstanceData > m_maxInstanceData)
        growMeshInstanceCapacity(m_pendingMaxInstanceData);
    // Last frame's instance count (still live here) outgrew the GI TLAS instance buffers.
    if (m_meshInstanceCounter > m_maxGiTlasInstances)
    {
        while (m_maxGiTlasInstances < m_meshInstanceCounter)
            m_maxGiTlasInstances *= 2;
        waitForGpuAndFlushStaging();
        m_giProbePipeline.resizeTlasInstanceBuffers(m_maxGiTlasInstances);
        printf("Renderer: grew GI TLAS instance capacity to %u\n", m_maxGiTlasInstances);
    }
    // A mesh mega-buffer was reallocated (vertex/index data growth)
    if (Globals::meshDataManager.getGeneration() != m_meshDataGeneration)
    {
        m_meshDataGeneration = Globals::meshDataManager.getGeneration();
        setHaveToRecordCommandBuffers();
    }

    syncTextureDescriptorCapacity();

    checkLightGridCapacity();
    checkForceGridCapacity();

    m_meshInstanceCounter = 0;
    m_instanceOverflowStart = UINT32_MAX;
    memset(m_numInstancesPerMesh.data(), 0, m_numInstancesPerMesh.size() * sizeof(m_numInstancesPerMesh[0]));

    const glm::ivec2 viewportSize = m_viewportRect.getSize();
    // In VR the "centre view" (used for culling, GI region, shadow cascade fit, and the shared screen-space
    // froxel fog volume) uses a head-centred projection spanning the union of both eyes' FOV, so it covers
    // everything either eye renders. Desktop uses the plain camera perspective.
    const glm::mat4x4 projection = reverseZProjection(Globals::openXR.isEnabled()
        ? Globals::openXR.getCombinedProjection(camera.near, camera.far)
        : glm::perspective(glm::radians(camera.fovDeg), (float)viewportSize.x / (float)viewportSize.y, camera.near, camera.far),
        camera.near, camera.far);
    glm::mat4 viewMatrix = camera.viewMatrix;

    glm::vec2 taaJitterNdc(0.0f);
    if (m_taaParams.taaEnabled && viewportSize.x > 0 && viewportSize.y > 0)
    {
        const uint32 sampleIdx = (m_frameCounter % 16u) + 1u;
        taaJitterNdc.x = (radicalInverse(sampleIdx, 2u) - 0.5f) * 2.0f / (float)viewportSize.x;
        taaJitterNdc.y = (radicalInverse(sampleIdx, 3u) - 0.5f) * 2.0f / (float)viewportSize.y;
    }

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    PerFrameData& frameData = m_perFrameData[frameIdx];

    m_cameraPos = camera.position; // drives the GI probe region each frame
    m_mipPixelScale = (float)oc::max(1, viewportSize.y) / oc::max(1e-3f, std::tan(glm::radians(camera.fovDeg) * 0.5f));

    // This slot's fence was waited above, so its last submitted cull's LOD stats have landed: snapshot
    // them for getStats (the mapped buffer is zeroed here for the frame about to record).
    for (uint32 i = 0; i < RendererVKLayout::MAX_MESH_LODS; ++i)
        m_lodInstanceCounts[i] = frameData.mappedLodStats[i];
    memset(frameData.mappedLodStats.data(), 0, frameData.mappedLodStats.size_bytes());
    frameData.lodStatsBuffer.flushMappedMemory(vk::WholeSize);

    static RendererVKLayout::Ubo ubo;

    ubo.lodParams0 = glm::vec4(
        oc::max(0.01f, m_lodParams.maxErrorPixels) * std::exp2((float)m_lodParams.bias),
        m_lodParams.hysteresis, m_lodParams.fullResPixels, m_mipPixelScale);
    ubo.lodParams1 = glm::vec4((float)m_lodParams.forceLod, (float)m_lodParams.bias,
        m_lodParams.enabled ? 1.0f : 0.0f, 0.0f);

    const uint32 numViews = Globals::openXR.isEnabled() ? RendererVKLayout::NUM_UBO_VIEWS : 1;
    for (uint32 v = 0; v < numViews; ++v)
    {
        ubo.views[v].prevMvp = ubo.views[v].mvp;
        ubo.views[v].prevInvMvp = ubo.views[v].invMvp;
    }

    RendererVKLayout::ViewData& centerView = ubo.views[RendererVKLayout::VIEW_CENTER];
    centerView.mvp = projection * viewMatrix;
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
    // reprojection (all raster passes jitter, the prepass included — see taaJitterUv in shared.inc.glsl).
    ubo.taaJitter = glm::vec4(taaJitterNdc, m_prevTaaJitter);
    m_prevTaaJitter = taaJitterNdc;
    // RTAO and the GI probe contribution both need the acceleration structures, so both fold in the RT
    // master toggle; GI additionally gates on its own switch.
    // z = the RTAO max distance: past it the trace writes exactly (N, 1.0) (rtao.cs.glsl early-out), so
    // the forward pass skips its depth-aware AO upsample there and uses those values directly.
    ubo.aoParams = glm::vec4((m_rtParams.enabled && m_rtaoParams.enabled) ? 1.0f : 0.0f,
        (m_rtParams.enabled && m_rtParams.giEnabled) ? m_giProbePipeline.getStrength() : 0.0f,
        m_rtaoParams.maxDistance, 0.0f);
    ubo.giVisParams = m_giProbePipeline.getVisibilityParams();
    ubo.frameIndex = m_frameCounter;

    const SkyParams& sky = m_skyParams;
    const FogParams& fog = m_fogParams;

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
    ubo.rtSunShadow = (m_rtParams.enabled && m_rtParams.rtSunShadow) ? 1.0f : 0.0f;

    // Use the effective flag: with RT off (or RT-sun off) the PCSS cascades supply the sun shadow.
    if (ubo.rtSunShadow < 0.5f)
    {
        const float aspect = (float)viewportSize.x / (float)viewportSize.y;
        computeSunCascades(camera, aspect, sky.sunDirection,
            m_shadowParams.maxDistance, m_shadowParams.splitLambda, m_shadowParams.casterPad, m_sunCascadeViewProj);
        m_numSunCascades = RendererVKLayout::NUM_SHADOW_CASCADES;
        for (uint32 c = 0; c < RendererVKLayout::NUM_SHADOW_CASCADES; ++c)
            ubo.cascadeViewProj[c] = m_sunCascadeViewProj[c];
        ubo.shadowParams = glm::vec3(m_shadowParams.depthBias, m_shadowParams.normalBias, 1.0f / (float)RendererVKLayout::SHADOW_MAP_RESOLUTION);
    }
    else
        m_numSunCascades = 0;

    ubo.sunShadowRays = (float)m_rtParams.sunShadowRays;
    ubo.rtLightShadows = (m_rtParams.enabled && m_rtParams.rtLightShadows) ? 1.0f : 0.0f;
    static const Clock::time_point timeStart = Clock::now();
    ubo.timeSeconds = std::chrono::duration<float>(Clock::now() - timeStart).count();
    ubo.cloudCoverage = sky.cloudCoverage;
    ubo.cloudThickness = sky.cloudThickness * sky.cloudThickness;
    ubo.cloudParams0 = glm::vec4(sky.cloudHeight, 0.00012f * sky.cloudScale, 0.0043f * sky.cloudWindSpeed, sky.cloudWindAngle);
    ubo.cloudParams1 = glm::vec4(sky.cloudSoftness, sky.cloudShading, 0.0f, 0.0f);
    ubo.cloudParams2 = glm::vec4(sky.cloudDensity, sky.cloudSharpness, sky.cloudBaseVar, sky.moonBrightness);
    ubo.skySunParams = glm::vec4(sky.scatterBoost, sky.mieG, sky.sunRolloff, sky.starDensity);

    // A freshly uploaded fog terrain height map activates here, in the same frame slot as the UBO that
    // carries its world center/sizes — descriptors (refreshed per frame in recordCommandBuffers) and
    // params stay coherent. Presence (fogParams3.y) is independent of Terrain Follow: the ocean also
    // reads these cascades as its shore-map fallback.
    m_fogTerrainMap.flipIfPending();
    const glm::vec2 fogTerrainSizes = m_fogTerrainMap.getWorldSizes();
    ubo.fogParams0 = glm::vec4(fog.density, fog.heightBase, fog.heightFalloff * fog.heightFalloff, fog.range);
    ubo.fogParams1 = glm::vec4(fog.albedo * fog.albedoIntensity, fog.anisotropy);
    ubo.fogParams2 = glm::vec4(fog.noiseScale, fog.noiseStrength, fog.windSpeed, fog.temporalBlend);
    ubo.fogParams3 = glm::vec4(glm::clamp(fog.terrainFollow, 0.0f, 1.0f),
        fogTerrainSizes.x > 1.0f ? 1.0f / fogTerrainSizes.x : 0.0f,
        fog.enabled ? 1.0f : 0.0f, fog.lightShadows ? 1.0f : 0.0f);
    ubo.fogParams4 = glm::vec4((float)fog.sunRays, fog.spatialFilter ? 1.0f : 0.0f, fog.giAmbient ? 1.0f : 0.0f, fog.sunSoftness);
    ubo.fogParams5 = glm::vec4(m_fogTerrainMap.getCenter(),
        fogTerrainSizes.y > 1.0f ? 1.0f / fogTerrainSizes.y : 0.0f, m_fogTerrainMap.getUserParam());
    ubo.fogParams6 = glm::vec4(glm::clamp(fog.slicePower, 0.25f, 2.0f), fog.terrainShadowDist,
        glm::clamp(fog.regionStrength, 0.0f, 1.0f), // z: baked regional fog-thickness modulation
        glm::max(fog.underwaterDensity, 0.0f));     // w: density multiplier below the local water surface
    // x: underwater fog boundary margin (m) relative to the LIVE wave surface (the fog scatter samples
    // the FFT displacement maps directly). y: the waterline band half-height gating those samples —
    // froxel segments outside +-band of the calm level are trivially above/below any possible wave, so
    // only a thin shell pays for wave taps. The CPU trough estimate (ocean readback) bounds the wave
    // amplitude; 0 disables wave sampling entirely (ocean off).
    const float waveBand = m_oceanParams.enabled ? m_oceanWaveTrough * 2.0f + 0.5f : 0.0f;
    ubo.fogParams7 = glm::vec4(
        glm::max(fog.shaftBoost, 0.0f),        // x: underwater sun in-scatter gain (fog light shafts)
        waveBand,
        glm::max(fog.causticStrength, 0.0f),   // z: underwater caustic focus strength (surfaces + fog shafts)
        glm::max(fog.causticDepthFade, 0.0f)); // w: caustic contrast decay with depth (1/m)
    ubo.fogParams8 = glm::vec4(fog.underwaterOffset, glm::max(fog.causticShoreFade, 0.0f), 0.0f, 0.0f);
    // z: thickness scale inverted into a falloff multiplier on fogParams0.z. w: far-field ground samples.
    ubo.fogParams9 = glm::vec4(fog.farField ? 1.0f : 0.0f, glm::max(fog.farFieldDensity, 0.0f),
        1.0f / glm::clamp(fog.farFieldThickness, 0.01f, 100.0f), (float)glm::max(fog.farFieldSteps, 1));

    ubo.moonParams = glm::vec4(glm::normalize(sky.moonDirection), cosf(glm::radians(sky.moonSizeDeg)));
    ubo.starParams = glm::vec4(sky.starSize, sky.starSizeVar, sky.starBrightness, sky.starColorVar);
    ubo.nebulaParams = glm::vec4(sky.nebulaIntensity, sky.nebulaScale, sky.nebulaBandWidth, sky.nebulaDust);
    ubo.nebulaAxis = glm::vec4(glm::normalize(sky.nebulaAxis), 0.0f);
    ubo.atmosParams = glm::vec4(sky.rayleighHeight, sky.mieHeight, sky.mieExtinction, sky.ozone);
    ubo.groundParams = glm::vec4(sky.groundColor * sky.groundIntensity, glm::clamp(sky.groundHorizon, 0.0f, 1.0f));

    const OceanParams& ocean = m_oceanParams;
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
    // crest scale) — sizes the on-land sampling band and keeps the vertex cull off the wet beach.
    const float swashAmp = glm::clamp(ocean.swashAmp, 0.0f, 4.0f);
    const float swashReach = swashAmp * (m_oceanWaveTrough + 0.25f);
    ubo.oceanParams7 = glm::vec4(glm::max(ocean.cullMargin, 0.0f), glm::clamp(ocean.shoreFoamMax, 0.0f, 1.0f), swashAmp, swashReach);
    ubo.oceanParams8 = glm::vec4(glm::max(ocean.swashDrawdown, 0.0f), glm::clamp(ocean.shoreFoamBias, -1.0f, 1.0f),
        glm::max(ocean.swashFlow, 0.0f), glm::max(ocean.rtRayCutoffDist, 0.0f));
    ubo.oceanParams9 = glm::vec4(glm::max(ocean.troughMargin, 0.0f), glm::max(ocean.rtRefractionRange, 10.0f),
        glm::max(ocean.rtReflectionRange, 50.0f), glm::clamp(ocean.rtReflectionMaxRough, 0.0f, 1.0f));
    ubo.oceanParams10 = glm::vec4(glm::max(ocean.waveHeightLimit, 0.0f), 0.0f, 0.0f, 0.0f);

    ubo.terrainParams = m_terrainParams;
    ubo.terrainShadowParams = glm::vec4(glm::max(m_shadowParams.terrainMarchStart, 0.0f),
        glm::max(m_shadowParams.terrainMarchBias, 1.0f), glm::max(m_shadowParams.terrainMarchSpread, 0.002f), 0.0f);

    { // Forcefield bubbles (Force library pushes m_forceFieldParams every frame; all UBO-driven = live)
        const ForceFieldParams& force = m_forceFieldParams;
        for (uint32 i = 0; i < RendererVKLayout::MAX_FORCE_TEAMS; ++i)
            ubo.forceTeamColors[i] = glm::vec4(force.teamColors[i], 0.0f);
        ubo.forceParams0 = glm::vec4(glm::max(force.isoThreshold, 1e-3f), glm::max(force.rimPower, 0.1f),
            glm::max(force.rimIntensity, 0.0f), glm::clamp(force.shellAlpha, 0.0f, 1.0f));
        ubo.forceParams1 = glm::vec4(glm::max(force.contactGlowIntensity, 0.0f), glm::max(force.contactGlowWidth, 1e-3f),
            glm::max(force.geoGlowDistance, 0.0f), (float)glm::clamp(force.marchSteps, 8, 256));
        ubo.forceParams2 = glm::vec4(glm::max(force.patternScale, 0.0f), force.patternSpeed,
            glm::max(force.patternIntensity, 0.0f), 0.0f); // w unused (force gain is CPU-side)
        ubo.forceParams3 = glm::vec4(glm::clamp(force.interiorAlpha, 0.0f, 1.0f),
            glm::clamp(force.backfaceAlpha, 0.0f, 1.0f), glm::clamp(force.contactWallAlpha, 0.0f, 1.0f),
            glm::clamp(force.junctionSmoothing, 0.0f, 2.0f));
        ubo.forceParams4 = glm::vec4(force.densityView ? 1.0f : 0.0f, glm::max(force.densityRange, 1e-3f),
            force.ambientSlope, (float)glm::min(force.ambientTeam, RendererVKLayout::MAX_FORCE_TEAMS - 1));
        ubo.forceParams5 = glm::vec4(force.ambientCenter, glm::max(force.ambientSafeRadius, 0.0f),
            glm::max(force.ambientMaxStrength, 0.0f));
    }

    const TerrainTexTweaks& tex = m_terrainTexTweaks;
    // Every start/full pair is ordered here rather than in the shader: an inverted pair from the tweak
    // UI would otherwise make smoothstep divide by a negative span and flip the layer inside out.
    ubo.terrainTexParams0 = glm::vec4(m_terrainSplatBaseMaterial < 0 ? -1.0f : (float)m_terrainSplatBaseMaterial,
        (float)m_terrainSplatCounts.numGround, (float)m_terrainSplatCounts.numRock, glm::max(tex.climateBlend, 1e-3f));
    ubo.terrainTexParams1 = glm::vec4(tex.uvScaleGround, tex.uvScaleRock,
        tex.slopeRockStart, glm::max(tex.slopeRockFull, tex.slopeRockStart + 1e-3f));
    ubo.terrainTexParams2 = glm::vec4(tex.cragStart, glm::max(tex.cragFull, tex.cragStart + 1e-3f),
        tex.beachBand, tex.uvScaleSnow);
    ubo.terrainTexParams3 = glm::vec4(m_terrainSplatCounts.hasBeach ? 1.0f : 0.0f, m_terrainSplatCounts.hasSnow ? 1.0f : 0.0f,
        tex.snowTempFull, glm::max(tex.snowTempNone, tex.snowTempFull + 1e-3f));
    ubo.terrainTexParams4 = glm::vec4(tex.snowSlopeStart, glm::max(tex.snowSlopeFull, tex.snowSlopeStart + 1e-3f),
        tex.snowAridity, 0.0f);
    ubo.terrainTexParams5 = glm::vec4(glm::max(tex.cragWanderAmp, 0.0f),
        1.0f / glm::max(tex.cragWanderWavelength, 1.0f), 0.0f, 0.0f);
    static_assert(sizeof(ubo.terrainSplatClimate) == sizeof(m_terrainSplatClimate));
    memcpy(ubo.terrainSplatClimate, m_terrainSplatClimate, sizeof(m_terrainSplatClimate));
    // The splat textures belong to no rendered instance's material, so the projected-size priority pass
    // never sees them — report them here instead: terrain tiles them across the whole view, so they can
    // always display roughly a screen's worth of texels.
    if (m_terrainSplatBaseMaterial >= 0)
    {
        const float log2Screen = std::log2((float)oc::max(m_windowSize.x, m_windowSize.y) * 2.0f);
        for (const uint16 texIdx : m_terrainSplatTextures)
            Globals::textureStreamer.noteUse(texIdx, log2Screen);
    }

    Globals::stagingManager.upload(frameData.ubo.getBuffer(), sizeof(RendererVKLayout::Ubo), &ubo);

    m_lightCounter = 0;
    m_fogVolumeCounter = 0;
    m_decalCounter = 0;
    m_frameCounter++;
    return ubo.frustum;
}

void Renderer::renderNode(const RenderNode& node, uint32 passMask)
{
    const uint32 numInstances = (uint32)node.m_meshInstances.size();
    const uint32 startIdx = oc::atomic_ref<uint32>(m_meshInstanceCounter).fetch_add(numInstances);
    if (startIdx + numInstances > m_maxInstanceData)
    {
        // Doesn't fit this frame: drop the node and grow at the next beginFrame. NO rollback -
        // un-bumping a non-top claim corrupts the cursor (later claims would land above the final
        // counter or leave unwritten gaps below it). The cursor is monotonic, so no claim after
        // this one can fit either: successful claims stay one contiguous prefix, and present()
        // clamps the counter to the smallest failed claim recorded here.
        oc::atomic_ref<uint32> overflow(m_instanceOverflowStart);
        uint32 curOverflow = overflow.load(oc::memory_order_relaxed);
        while (startIdx < curOverflow && !overflow.compare_exchange_weak(curOverflow, startIdx)) {}
        oc::atomic_ref<uint32> pending(m_pendingMaxInstanceData);
        uint32 cur = pending.load(oc::memory_order_relaxed);
        while (cur < startIdx + numInstances && !pending.compare_exchange_weak(cur, startIdx + numInstances)) {}
        return;
    }

    for (auto& pair : node.m_numInstancesPerMesh)
        oc::atomic_ref<uint32>(m_numInstancesPerMesh[pair.first]) += pair.second;

    noteTextureUse(node, passMask);
    for (const RendererVKLayout::InMeshInstance& instance : node.m_meshInstances)
        Globals::meshStreamer.noteUse(instance.meshIdx);

    PerFrameData& frameData = m_perFrameData[m_swapChain.getCurrentFrameIndex()];
    frameData.mappedNodePassMasks[node.m_transformIdx] = passMask;
    memcpy(frameData.mappedMeshInstances.data() + startIdx, node.m_meshInstances.data(), numInstances * sizeof(node.m_meshInstances[0]));
    if (!node.m_lodInstances.empty())
        noteLodChainUse(node, startIdx, frameData); // benign races: same-value stamp + thread-safe noteUse
}

void Renderer::noteTextureUse(const RenderNode& node, uint32 passMask)
{
    if (!Globals::textureStreamer.isStreamingEnabled())
        return;
    const Sphere bounds = node.getWorldBounds();
    const float dist = oc::max(0.01f, glm::length(bounds.pos - m_cameraPos) - bounds.radius);
    const float projPixels = bounds.radius * m_mipPixelScale / dist;
    float log2P = std::log2(oc::max(1.0f, projPixels));
    if (!(passMask & RendererVKLayout::PASS_MAIN))
        log2P -= 2.0f; // off-screen (shadow/GI-only) nodes tolerate two mips coarser

    const oc::span<const RendererVKLayout::MaterialInfo> materials =
        m_materialInfosBuffer.getBackingStoreAs<RendererVKLayout::MaterialInfo>();
    for (const RendererVKLayout::InMeshInstance& instance : node.m_meshInstances)
    {
        const RendererVKLayout::MaterialInfo& material = materials[instance.materialIdx];
        Globals::textureStreamer.noteUse(material.diffuseTexIdx, log2P);
        Globals::textureStreamer.noteUse(material.normalTexIdx, log2P);
        Globals::textureStreamer.noteUse(material.metalRoughnessTexIdx, log2P);
    }
}

void Renderer::noteLodChainUse(const RenderNode& node, uint32 startIdx, PerFrameData& frameData)
{
    // The GPU cull picks each instance's level, so every level of a referenced chain must stay warm in
    // the mesh streamer. Only AUTHORED chains (no error data) need this: their levels are independent
    // mesh sets, and a cold one would draw nothing while it re-streams. Generated chains share LOD0's
    // set, which the caller's per-instance noteUse already touched. Stamped once per group per frame;
    // races on the stamp are benign: both threads write the same frame value, worst case double-noting.
    for (const RenderNode::LodInstance& lod : node.m_lodInstances)
    {
        MeshLodGroup& group = m_meshLodGroups[lod.lodGroupIdx];
        if (group.errors[1] == 0.0f && oc::atomic_ref<uint32>(group.lastUseFrame).load(oc::memory_order_relaxed) != m_frameCounter)
        {
            oc::atomic_ref<uint32>(group.lastUseFrame).store(m_frameCounter, oc::memory_order_relaxed);
            for (uint8 k = 1; k < group.numLods; ++k)
                Globals::meshStreamer.noteUse(group.meshIdx[k]);
        }
    }
    // Publish the cull's hysteresis-state addressing for this node: stateSlot = instanceIdx + bias.
    frameData.mappedNodeLodStateBias[node.m_transformIdx] = (int32)node.m_lodStateBase - (int32)startIdx;
}

void Renderer::uploadMeshLodGroup(uint32 groupIdx)
{
    const MeshLodGroup& group = m_meshLodGroups[groupIdx];
    RendererVKLayout::GpuMeshLodGroup gpu{};
    gpu.numLods = group.numLods;
    gpu.mesh01 = (uint32)group.meshIdx[0] | ((uint32)group.meshIdx[1] << 16);
    gpu.mesh23 = (uint32)group.meshIdx[2] | ((uint32)group.meshIdx[3] << 16);
    gpu.mesh4 = (uint32)group.meshIdx[4];
    for (uint32 k = 1; k < RendererVKLayout::MAX_MESH_LODS; ++k)
        gpu.errors1_4[k - 1] = group.errors[k];
    uploadToSharedBuffer(m_meshLodGroupsBuffer, sizeof(gpu), &gpu, (size_t)groupIdx * sizeof(gpu));
}

void Renderer::setMeshLodGroupIdx(uint16 meshIdx, uint32 groupIdx)
{
    m_meshToLodGroup[meshIdx] = groupIdx;
    uploadToSharedBuffer(m_meshLodGroupIdxBuffer, sizeof(uint32), &m_meshToLodGroup[meshIdx], (size_t)meshIdx * sizeof(uint32));
}

uint32 Renderer::allocateLodStateRange(uint32 count)
{
    if (const uint32 reusedBase = m_freeLodStateSlots.allocate(count); reusedBase != UINT32_MAX)
        return reusedBase;
    const uint32 base = m_lodStateCounter;
    m_lodStateCounter += count;
    if (m_lodStateCounter > m_maxLodStateSlots)
        growLodStateCapacity(m_lodStateCounter);
    return base;
}


void Renderer::addLightInfo(const RendererVKLayout::LightInfo& light)
{
    // lock-free bump; claims beyond MAX_LIGHTS are dropped (present clamps the flush count)
    const uint32 idx = oc::atomic_ref<uint32>(m_lightCounter).fetch_add(1);
    if (idx < RendererVKLayout::MAX_LIGHTS)
    {
        PerFrameData& frameData = m_perFrameData[m_swapChain.getCurrentFrameIndex()];
        frameData.mappedLightInfos[idx] = light;
    }
}

void Renderer::addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume)
{
    const uint32 idx = oc::atomic_ref<uint32>(m_fogVolumeCounter).fetch_add(1);
    if (idx < RendererVKLayout::MAX_FOG_VOLUMES)
    {
        PerFrameData& frameData = m_perFrameData[m_swapChain.getCurrentFrameIndex()];
        frameData.mappedFogVolumes.data()->volumes[idx] = fogVolume;
    }
}

void Renderer::addPointLight(const PointLight& light)   { addLightInfo(light); }
void Renderer::addAreaLight(const AreaLight& areaLight) { addLightInfo(areaLight); }
void Renderer::addSpotLight(const SpotLight& spotLight) { addLightInfo(spotLight); }

void Renderer::addDecal(const RendererVKLayout::DecalInfo& decal)
{
    // lock-free bump like addLightInfo; claims beyond MAX_DECALS are dropped (present clamps the count)
    const uint32 idx = oc::atomic_ref<uint32>(m_decalCounter).fetch_add(1);
    if (idx < RendererVKLayout::MAX_DECALS)
        m_decalPipeline.getMapped(m_swapChain.getCurrentFrameIndex())[idx] = decal;
}

uint32 Renderer::createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc)
{
    // Recycle retired slots once their KILL flag has drained through the sim (their particles are gone
    // after the flag has been live for a couple of simulated frames).
    for (size_t i = 0; i < m_retiredParticleEmitters.size();)
    {
        if (m_frameCounter - m_retiredParticleEmitters[i].second > RendererVKLayout::NUM_FRAMES_IN_FLIGHT + 2)
        {
            m_freeParticleEmitterSlots.push_back(m_retiredParticleEmitters[i].first);
            m_retiredParticleEmitters.erase(m_retiredParticleEmitters.begin() + i);
        }
        else
            ++i;
    }
    uint32 slot;
    if (!m_freeParticleEmitterSlots.empty())
    {
        slot = m_freeParticleEmitterSlots.back();
        m_freeParticleEmitterSlots.pop_back();
    }
    else
    {
        if (m_particleEmitters.size() >= RendererVKLayout::MAX_PARTICLE_EMITTERS)
            return UINT32_MAX;
        m_particleEmitters.emplace_back();
        slot = (uint32)m_particleEmitters.size() - 1;
    }
    m_particleEmitters[slot] = desc;
    return slot;
}

void Renderer::updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc)
{
    assert(slot < m_particleEmitters.size());
    m_particleEmitters[slot] = desc;
}

void Renderer::emitParticles(uint32 slot, uint32 count)
{
    assert(slot < m_particleEmitters.size());
    if (count > 0)
        m_particleSpawnRequests.emplace_back((uint16)slot, (uint16)oc::min(count, 0xFFFFu));
}

void Renderer::destroyParticleEmitter(uint32 slot)
{
    assert(slot < m_particleEmitters.size());
    m_particleEmitters[slot].texFlags.y |= RendererVKLayout::PARTICLE_FLAG_KILL;
    m_retiredParticleEmitters.emplace_back(slot, m_frameCounter);
}

uint32 Renderer::createForceEmitter(const RendererVKLayout::ForceEmitterGpu& desc)
{
    // Recycle retired slots once every frame that could still deliver their slot-indexed force
    // readback has drained (particle emitter slot pattern).
    for (size_t i = 0; i < m_retiredForceEmitters.size();)
    {
        if (m_frameCounter - m_retiredForceEmitters[i].second > RendererVKLayout::NUM_FRAMES_IN_FLIGHT + 2)
        {
            m_freeForceEmitterSlots.push_back(m_retiredForceEmitters[i].first);
            m_retiredForceEmitters.erase(m_retiredForceEmitters.begin() + i);
        }
        else
            ++i;
    }
    uint32 slot;
    if (!m_freeForceEmitterSlots.empty())
    {
        slot = m_freeForceEmitterSlots.back();
        m_freeForceEmitterSlots.pop_back();
    }
    else
    {
        if (m_forceEmitters.size() >= RendererVKLayout::MAX_FORCE_EMITTERS)
            return UINT32_MAX;
        m_forceEmitters.emplace_back();
        slot = (uint32)m_forceEmitters.size() - 1;
    }
    m_forceEmitters[slot] = desc;
    m_forceEmitters[slot].teamFlags.y |= RendererVKLayout::FORCE_FLAG_ACTIVE;
    return slot;
}

void Renderer::updateForceEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc)
{
    assert(slot < m_forceEmitters.size());
    m_forceEmitters[slot] = desc;
}

void Renderer::destroyForceEmitter(uint32 slot)
{
    assert(slot < m_forceEmitters.size());
    m_forceEmitters[slot].teamFlags.y &= ~RendererVKLayout::FORCE_FLAG_ACTIVE;
    m_retiredForceEmitters.emplace_back(slot, m_frameCounter);
}

uint32 Renderer::createForceQuerySlot()
{
    for (size_t i = 0; i < m_retiredForceQuerySlots.size();)
    {
        if (m_frameCounter - m_retiredForceQuerySlots[i].second > RendererVKLayout::NUM_FRAMES_IN_FLIGHT + 2)
        {
            m_freeForceQuerySlots.push_back(m_retiredForceQuerySlots[i].first);
            m_retiredForceQuerySlots.erase(m_retiredForceQuerySlots.begin() + i);
        }
        else
            ++i;
    }
    uint32 slot;
    if (!m_freeForceQuerySlots.empty())
    {
        slot = m_freeForceQuerySlots.back();
        m_freeForceQuerySlots.pop_back();
    }
    else
    {
        if (m_forceQueries.size() >= RendererVKLayout::MAX_FORCE_QUERIES)
            return UINT32_MAX;
        m_forceQueries.emplace_back();
        slot = (uint32)m_forceQueries.size() - 1;
    }
    m_forceQueries[slot].posActive = glm::vec4(0.0f); // inactive until the first setForceQuery
    return slot;
}

void Renderer::setForceQuery(uint32 slot, const glm::vec3& pos)
{
    assert(slot < m_forceQueries.size());
    m_forceQueries[slot].posActive = glm::vec4(pos, 1.0f);
}

void Renderer::destroyForceQuerySlot(uint32 slot)
{
    assert(slot < m_forceQueries.size());
    m_forceQueries[slot].posActive = glm::vec4(0.0f);
    m_retiredForceQuerySlots.emplace_back(slot, m_frameCounter);
}

glm::vec4 Renderer::getForceEmitterReadback(uint32 slot) const
{
    const oc::span<const glm::vec4> forces = m_forceFieldPipeline.getForceReadback(m_swapChain.getCurrentFrameIndex());
    return slot < forces.size() ? forces[slot] : glm::vec4(0.0f);
}

RendererVKLayout::ForceQueryResult Renderer::getForceQueryReadback(uint32 slot) const
{
    const oc::span<const RendererVKLayout::ForceQueryResult> results = m_forceFieldPipeline.getQueryReadback(m_swapChain.getCurrentFrameIndex());
    return slot < results.size() ? results[slot] : RendererVKLayout::ForceQueryResult{ RendererVKLayout::MAX_FORCE_TEAMS, 0.0f, 0.0f, 0u };
}

void Renderer::setForceFieldParams(const ForceFieldParams& params)
{
    // The grid toggle is a compile-time shader define (FORCE_GRID): rebuild the force pipelines,
    // same GPU-idle + reload pattern as the ocean hit-lighting tweak.
    if (params.useGrid != m_forceFieldPipeline.getUseGrid())
    {
        if (Globals::device.getGraphicsQueue().waitIdle() == vk::Result::eSuccess)
        {
            m_forceFieldPipeline.setUseGrid(params.useGrid);
            m_forceFieldPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
            setHaveToRecordCommandBuffers();
        }
    }
    m_forceFieldParams = params;
}

void Renderer::checkForceGridCapacity()
{
    // Light-grid growth contract: read LAST frame's demand counters and grow before the buffers
    // fill (overflowing inserts drop for one frame but keep incrementing the counter, so the
    // readback measures true demand).
    if (!m_forceFieldParams.enabled || !m_forceFieldPipeline.getUseGrid())
        return;
    const uint32 prevIdx = (m_swapChain.getCurrentFrameIndex() + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const ForceFieldPipeline::GridDemand demand = m_forceFieldPipeline.getGridDemand(prevIdx);
    const bool tableHigh = demand.numCells > m_forceFieldPipeline.getTableEntries() / 4;
    const bool dataHigh = (size_t)demand.dataCounter * sizeof(uint32) > m_forceFieldPipeline.getGridDataSize() / 4 * 3;
    if (tableHigh || dataHigh)
    {
        waitForGpuAndFlushStaging();
        m_forceFieldPipeline.growGridBuffers(
            dataHigh ? (size_t)(demand.dataCounter * sizeof(uint32) * 1.5f) : m_forceFieldPipeline.getGridDataSize(),
            tableHigh ? demand.numCells * 8 : m_forceFieldPipeline.getTableEntries());
        setHaveToRecordCommandBuffers();
    }
}

uint16 Renderer::createSolidColorMaterial(const glm::vec3& color)
{
    const glm::vec3 c = glm::clamp(color, 0.0f, 1.0f);
    const uint32 key = uint32(c.x * 255.0f + 0.5f) | (uint32(c.y * 255.0f + 0.5f) << 8)
        | (uint32(c.z * 255.0f + 0.5f) << 16);
    if (const auto it = m_solidColorMaterials.find(key); it != m_solidColorMaterials.end())
        return it->second;

    struct SolidColorTexture final : public ITextureData
    {
        Pixel pixel{};
        const char* getFileName() const override { return "solid_color"; }
        const Pixel* getPixels() const override { return &pixel; }
        uint32 getWidth() const override { return 1; }
        uint32 getHeight() const override { return 1; }
        const char* getFormatInfo() const override { return "rgba8888"; }
    };
    SolidColorTexture texture;
    texture.pixel = { uint8(key & 0xFF), uint8((key >> 8) & 0xFF), uint8((key >> 16) & 0xFF), 0xFF };
    const uint16 texIdx = Globals::textureManager.upload(texture, false, true);

    RendererVKLayout::MaterialInfo material{};
    material.flags = RendererVKLayout::MATERIAL_FLAG_NO_RAYTRACING; // tints are debug-flat visuals
    material.opacity = 1.0f;
    material.diffuseTexIdx = texIdx;
    material.normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
    material.metalRoughnessTexIdx = 0xFFFF; // lit shading's "no texture": metal 0, roughness 0.65
    material.alphaMode = 0;
    const uint16 materialIdx = (uint16)addMaterialInfos({ material });
    m_solidColorMaterials.emplace(key, materialIdx);
    setHaveToRecordCommandBuffers(); // the new texture slot must land in the bindless arrays
    return materialIdx;
}

uint16 Renderer::loadEffectTexture(const char* filePath, bool sRGB)
{
    const uint16 idx = Globals::textureManager.upload(filePath, true, sRGB);
    // The bindless arrays are fully (re)written when the cached draw CBs record, so make sure the new
    // slot lands in them (growth beyond the descriptor capacity is caught by syncTextureDescriptorCapacity).
    setHaveToRecordCommandBuffers();
    return idx;
}

void Renderer::setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity)
{
    m_skyParams.sunDirection = glm::normalize(direction);
    m_skyParams.sunColor = color;
    m_skyParams.sunIntensity = intensity;
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
    m_meshInstanceCounter = oc::min(m_meshInstanceCounter, m_instanceOverflowStart);
    m_lightCounter = oc::min(m_lightCounter, uint32(RendererVKLayout::MAX_LIGHTS));
    m_fogVolumeCounter = oc::min(m_fogVolumeCounter, uint32(RendererVKLayout::MAX_FOG_VOLUMES));
    m_decalCounter = oc::min(m_decalCounter, uint32(RendererVKLayout::MAX_DECALS));

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    PerFrameData& frameData = m_perFrameData[frameIdx];

    assert(frameData.mappedRenderNodeTransforms.size() >= m_renderNodeTransforms.size());
    assert(frameData.mappedMeshInstances.size() >= m_meshInstanceCounter);
    assert(frameData.mappedFirstInstances.size() >= m_meshInfoCounter);
    assert(m_renderNodeTransforms.size() == 0 || Globals::textureManager.getNumTextures() > 0 && "Attempting to render object without any textures loaded!");

    ProfileScope uploadScope("Per-frame uploads", EProfileCategory::Renderer);
    // Sparse transform upload: only slots that changed since this frame-in-flight last consumed
    // them, gathered from every worker's dirty list.
    Globals::renderNodeDirtyLists.forEach([&](oc::array<oc::vector<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT>& lists)
        {
            oc::vector<uint32>& transformDirtyList = lists[frameIdx];
            for (const uint32 idx : transformDirtyList)
            {
                memcpy(&frameData.mappedRenderNodeTransforms[idx], &m_renderNodeTransforms[idx], sizeof(Transform));
                Globals::renderNodeDirtyBits[idx] &= uint8(~(1u << frameIdx));
            }
            transformDirtyList.clear();
        });
    // Bucket layout for the GPU culls: instances are pushed referencing LOD0, and the cull redirects
    // each one to its selected level — so every member of a LOD chain gets a bucket sized to the
    // CHAIN's instance count (any split of the instances across levels fits). Non-chain meshes keep
    // exact buckets. The expansion can exceed the pushed instance count; the instance-index buffers
    // are sized to m_maxInstanceData, so grow when the expanded total outruns it.
    uint32 instanceCounter = 0;
    const uint32 numMeshInfos = (uint32)m_numInstancesPerMesh.size();
    for (uint32 meshIdx = 0; meshIdx < numMeshInfos; ++meshIdx)
    {
        frameData.mappedFirstInstances[meshIdx] = instanceCounter;
        const uint32 groupIdx = m_meshToLodGroup[meshIdx];
        instanceCounter += groupIdx == UINT32_MAX ? m_numInstancesPerMesh[meshIdx]
            : m_numInstancesPerMesh[m_meshLodGroups[groupIdx].meshIdx[0]];
    }
    if (instanceCounter > m_maxInstanceData)
        growMeshInstanceCapacity(instanceCounter);

    frameData.inRenderNodeTransformsBuffer.flushMappedMemory(m_renderNodeTransforms.size() * sizeof(m_renderNodeTransforms[0]));
    frameData.inNodePassMasksBuffer.flushMappedMemory(m_renderNodeTransforms.size() * sizeof(uint32));
    frameData.inNodeLodStateBiasBuffer.flushMappedMemory(m_renderNodeTransforms.size() * sizeof(int32));
    frameData.inMeshInstancesBuffer.flushMappedMemory(m_meshInstanceCounter * sizeof(RendererVKLayout::InMeshInstance));
    frameData.inFirstInstancesBuffer.flushMappedMemory(numMeshInfos * sizeof(uint32));
    frameData.lightInfosBuffer.flushMappedMemory(m_lightCounter * sizeof(RendererVKLayout::LightInfo));

    frameData.mappedMeshCount[0] = m_meshInfoCounter; // DGC sequence count / G-buffer draw count for this frame
    frameData.meshCountBuffer.flushMappedMemory(sizeof(uint32));

    frameData.mappedFogVolumes.data()->count = m_fogVolumeCounter;
    frameData.fogVolumesBuffer.flushMappedMemory(RendererVKLayout::FOG_VOLUME_HEADER_SIZE + m_fogVolumeCounter * sizeof(RendererVKLayout::FogVolumeInfo));

    // Debug overlay lines accumulated since the last present (safe here: this slot's fence was waited
    // in beginFrame). First use lazily creates the GPU buffers -> re-record to pick up the new pass.
    m_debugLineMergedVerts.clear();
    m_debugLineVerts.forEach([this](oc::vector<DebugLinePipeline::LineVertex>& verts)
        {
            m_debugLineMergedVerts.insert(m_debugLineMergedVerts.end(), verts.begin(), verts.end());
            verts.clear();
        });
    if (m_debugLinePipeline.upload(frameIdx, m_debugLineMergedVerts))
        setHaveToRecordCommandBuffers();

    // Only spend the pool reset on a frame that will actually execute the sim (see
    // m_particleResetPending); the flag is cleared after this frame's successful submit below.
    const bool particleResetCarried = m_particleResetPending && m_particlesEnabled && m_meshInstanceCounter > 0;
    { // Particle emitter table + spawn map + decals into this slot's mapped buffers (fence waited).
        static Clock::time_point s_lastParticleTime = Clock::now();
        const Clock::time_point now = Clock::now();
        const float dt = oc::min(std::chrono::duration<float>(now - s_lastParticleTime).count(), 0.25f);
        s_lastParticleTime = now;
        uint32 spawnRequestTotal = 0;
        for (const auto& [slot, count] : m_particleSpawnRequests)
            spawnRequestTotal += count;
        m_particlePipeline.update(frameIdx, m_particleEmitters, m_particleSpawnRequests,
            dt * m_particleTimeScale, m_particleCollision, particleResetCarried);
        m_particleSpawnRequests.clear();
        m_decalPipeline.upload(frameIdx, m_decalCounter);
        // Compacts the ACTIVE emitter slots + uploads query positions (fence-safe here).
        m_forceFieldPipeline.upload(frameIdx, m_forceEmitters, m_forceQueries, m_forceFieldParams.bigReachThreshold);

        if (m_particleLogStats && m_frameCounter % 120 == 0)
        {
            const ParticlePipeline::DebugCounters counters = m_particlePipeline.getDebugCounters(frameIdx);
            printf("Particles: alive %u/%u (parity 0/1), dead %d, simGroups %u, emitters %u, spawn reqs %u, decals %u\n",
                counters.alive[0], counters.alive[1], counters.deadCount, counters.simGroups,
                (uint32)m_particleEmitters.size(), spawnRequestTotal, m_decalCounter);
        }
    }

    m_indirectCullComputePipeline.update(frameIdx, m_meshInstanceCounter);
    m_skinningComputePipeline.update(frameIdx, m_skinningPalettes, m_skinningJobs);
    m_lightGridComputePipeline.update(frameIdx, m_lightCounter);
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
    syncTextureDescriptorCapacity();

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

    if (!m_pendingTextureFrees.empty())
    {
        // A container was destroyed this frame; its textures may still be sampled by an in-flight frame,
        // so drain before freeing them. Their bindless slots rewrite to the fallback in recordCommandBuffers
        // below, before anything is submitted.
        ProfileScope profileScope("Texture free drain", EProfileCategory::Wait);
        auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
        assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle before freeing textures");
        processPendingTextureFrees();
    }

    vk::Semaphore waitSemaphore;
    {
        ProfileScope profileScope("Staging", EProfileCategory::Renderer);
        waitSemaphore = Globals::stagingManager.update();
    }
    if (waitSemaphore != VK_NULL_HANDLE)
		frameData.primaryCommandBuffer.addWaitSemaphore(waitSemaphore, vk::PipelineStageFlagBits::eAllCommands); // eAllCommands (not just transfer) for GI BLAS

    {
        // Can block on the presentation engine (vsync backpressure lands here or in the fence wait).
        ProfileScope profileScope("Acquire image", EProfileCategory::Wait);
        if (!m_swapChain.acquireNextImage())
        {
            Globals::openXR.endFrame(nullptr, nullptr, {}, vk::ImageLayout::eUndefined); // balance the begun XR frame
            recreateSwapchain();
            return;
        }
    }
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
        m_particleResetPending = false;
    // This frame's submission is now new GPU work that could read the shared mesh/material/instance-offset
    // buffers; any upload into them from here on needs a fresh drain. See StagingManager::ensureDrainedForSharedWrite.
    Globals::stagingManager.resetSharedWriteGate();

    Globals::openXR.endFrame(m_eyeColorImage[0], m_eyeColorImage[1], m_swapChain.getLayout().extent, vk::ImageLayout::ePresentSrcKHR);

    {
        ProfileScope profileScope("Queue present", EProfileCategory::Wait);
        if (!m_swapChain.present())
        {
            recreateSwapchain();
        }
    }
    m_frameSlotWaited = false; // the slot advanced: next frame must wait its own fence first
}

uint32 Renderer::addRenderNodeTransform(const Transform& transform)
{
    if (!m_freeRenderNodeIndexes.empty())
    {
        const uint32 renderNodeIdx = m_freeRenderNodeIndexes.back();
        m_freeRenderNodeIndexes.pop_back();
        m_renderNodeTransforms[renderNodeIdx] = transform;
        markRenderNodeTransformDirty(renderNodeIdx);
        return renderNodeIdx;
    }
    const uint32 renderNodeIdx = (uint32)m_renderNodeTransforms.size();
    m_renderNodeTransforms.emplace_back(transform);
    Globals::renderNodeDirtyBits.push_back(0);
    markRenderNodeTransformDirty(renderNodeIdx);
    if ((uint32)m_renderNodeTransforms.size() > m_maxRenderNodes)
        growRenderNodeCapacity((uint32)m_renderNodeTransforms.size());
    return renderNodeIdx;
}

void RenderNode::destroy()
{
    if (m_transformIdx == UINT32_MAX && m_skinnedBundleHandle == UINT32_MAX)
        return;
    Globals::rendererVK.freeRenderNode(*this);
}

// No GPU sync needed anywhere in the free path: mesh instances, skinning jobs and palettes are
// re-uploaded into per-frame buffers every frame, transforms upload sparsely on change (dirty on
// slot reuse), so frames still in flight read their own copies and frames recorded from here on
// simply never reference the freed slots.
void Renderer::freeRenderNode(RenderNode& node)
{
    if (node.m_transformIdx != UINT32_MAX)
    {
        m_freeRenderNodeIndexes.push_back(node.m_transformIdx);
        node.m_transformIdx = UINT32_MAX;
    }
    if (node.m_skinnedBundleHandle != UINT32_MAX)
    {
        releaseSkinnedBundle(node.m_skinnedBundleHandle);
        node.m_skinnedBundleHandle = UINT32_MAX;
    }
    if (node.m_lodStateBase != UINT32_MAX)
    {
        m_freeLodStateSlots.release(node.m_lodStateBase, (uint32)node.m_meshInstances.size());
        node.m_lodStateBase = UINT32_MAX;
    }
    node.m_skinnedPaletteHandle = UINT32_MAX;
    node.m_meshInstances.clear();
    node.m_numInstancesPerMesh.clear();
}

uint32 Renderer::registerSkinnedBundle(const SkinnedInstanceBundle& bundle)
{
    if (!m_freeSkinnedBundleSlots.empty())
    {
        const uint32 handle = m_freeSkinnedBundleSlots.back();
        m_freeSkinnedBundleSlots.pop_back();
        m_skinnedBundles[handle] = bundle;
        return handle;
    }
    const uint32 handle = (uint32)m_skinnedBundles.size();
    m_skinnedBundles.push_back(bundle);
    return handle;
}

void Renderer::releaseSkinnedBundle(uint32 bundleHandle)
{
    const SkinnedInstanceBundle& bundle = m_skinnedBundles[bundleHandle];
    // Park in place: a zero vertexCount makes the skinning dispatch skip the job, a zero indexCount makes
    // recordBuildSkinnedBlas skip the rebuild. The entries must keep their positions (see SkinnedInstanceBundle).
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        m_skinningJobs[bundle.firstJob + k].vertexCount = 0;
        m_skinnedBlasBuilds[bundle.firstJob + k].indexCount = 0;
    }
    m_freeSkinnedBundles[bundle.sourceKey].push_back(bundleHandle);
}

uint32 Renderer::acquireSkinnedBundle(uint32 sourceKey)
{
    const auto it = m_freeSkinnedBundles.find(sourceKey);
    if (it == m_freeSkinnedBundles.end() || it->second.empty())
        return UINT32_MAX;
    const uint32 bundleHandle = it->second.back();
    it->second.pop_back();

    const SkinnedInstanceBundle& bundle = m_skinnedBundles[bundleHandle];
    const SkinningPaletteRegion& region = m_skinningPaletteRegions[bundle.paletteHandle];
    oc::fill_n(m_skinningPalettes.begin() + region.offset, region.boneCount, glm::mat4(1.0f)); // bind pose until the first setSkinningPalette
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        const RendererVKLayout::SkinnedMeshSource& src = m_skinnedMeshSources[bundle.sourceKey + k];
        m_skinningJobs[bundle.firstJob + k].vertexCount = src.vertexCount;
        // The bundle's recorded count, NOT src.indexCount: the skinned BLAS was created (and its buffer
        // sized) for the chain's RT level, which may be coarser than level 0.
        m_skinnedBlasBuilds[bundle.firstJob + k].indexCount = bundle.blasIndexCounts[k];
    }
    return bundleHandle;
}

namespace
{
    uint32 growCapacity(uint32 current, uint32 needed, uint32 limit = UINT32_MAX)
    {
        uint64 capacity = current;
        while (capacity < needed)
            capacity *= 2;
        assert(needed <= limit);
        return (uint32)oc::min<uint64>(capacity, limit);
    }
}

void Renderer::waitForGpuAndFlushStaging()
{
    // Drain first: some buffers being grown here (e.g. m_instanceOffsetsBuffer) aren't per-frame-in-flight,
    // so an already-submitted draw/dispatch may still be reading one while a queued staging copy is about
    // to write into it (WRITE_AFTER_READ - no fence/semaphore otherwise orders a fresh copy submission
    // against earlier submissions on the same queue). Flushing pending copies only after this first wait
    // means their vkCmdCopyBuffer/Image writes always land on an idle GPU.
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle during capacity growth");
    Globals::stagingManager.flushPending();
    // Drain again: flushPending() just submitted those copies (targeting the buffer the caller is about to
    // destroy/recreate) as new GPU work. Without this second wait, destroy() below would race that
    // submission - "buffer currently in use by command buffer" at vkDestroyBuffer.
    waitResult = Globals::device.getGraphicsQueue().waitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle after staging flush");
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();
    processPendingTextureFrees();
}

void Renderer::growRenderNodeCapacity(uint32 needed)
{
    m_maxRenderNodes = growCapacity(m_maxRenderNodes, needed);
    waitForGpuAndFlushStaging();
    for (PerFrameData& perFrame : m_perFrameData)
    {
        // No contents to preserve: present() re-copies the full CPU transform list every frame.
        perFrame.inRenderNodeTransformsBuffer.initialize(m_maxRenderNodes * sizeof(RendererVKLayout::RenderNodeTransform),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "RenderNodeTransforms", BufferHostAccess::eSequentialWrite);
        perFrame.mappedRenderNodeTransforms = perFrame.inRenderNodeTransformsBuffer.mapMemory<RendererVKLayout::RenderNodeTransform>();

        // No contents to preserve either: masks are rewritten by every renderNode() push, and the grow
        // happens between frames' pushes (only pushed nodes' instances are ever read).
        perFrame.inNodePassMasksBuffer.initialize(m_maxRenderNodes * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "NodePassMasks", BufferHostAccess::eSequentialWrite);
        perFrame.mappedNodePassMasks = perFrame.inNodePassMasksBuffer.mapMemory<uint32>();

        perFrame.inNodeLodStateBiasBuffer.initialize(m_maxRenderNodes * sizeof(int32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "NodeLodStateBias", BufferHostAccess::eSequentialWrite);
        perFrame.mappedNodeLodStateBias = perFrame.inNodeLodStateBiasBuffer.mapMemory<int32>();
    }
    // Fresh (empty) GPU buffers: every live slot has to upload again.
    Globals::renderNodeDirtyLists.forEach([](oc::array<oc::vector<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT>& lists)
        {
            for (oc::vector<uint32>& dirtyList : lists)
                dirtyList.clear();
        });
    oc::fill(Globals::renderNodeDirtyBits.begin(), Globals::renderNodeDirtyBits.end(), uint8(0));
    for (uint32 idx = 0; idx < (uint32)m_renderNodeTransforms.size(); ++idx)
        markRenderNodeTransformDirty(idx);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew render node capacity to %u\n", m_maxRenderNodes);
}

void Renderer::growMeshInstanceCapacity(uint32 needed)
{
    m_maxInstanceData = growCapacity(m_maxInstanceData, needed);
    waitForGpuAndFlushStaging();

    // renderNode() grows inline mid-frame, so the instances already written to the current frame's
    // mapped buffer this frame must survive the reallocation. (At the beginFrame call site the counter
    // holds last frame's count and the copy is redundant but harmless.)
    PerFrameData& currentFrameData = m_perFrameData[m_swapChain.getCurrentFrameIndex()];
    oc::vector<RendererVKLayout::InMeshInstance> writtenInstances(currentFrameData.mappedMeshInstances.begin(), currentFrameData.mappedMeshInstances.begin() + m_meshInstanceCounter);

    for (PerFrameData& perFrame : m_perFrameData)
    {
        // Kept cached/random: growMeshInstanceCapacity reads the existing mapping to preserve in-flight
        // instances across a resize, so this buffer must stay CPU-readable.
        perFrame.inMeshInstancesBuffer.initialize(m_maxInstanceData * sizeof(RendererVKLayout::InMeshInstance),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "MeshInstances");
        perFrame.mappedMeshInstances = perFrame.inMeshInstancesBuffer.mapMemory<RendererVKLayout::InMeshInstance>();
    }
    memcpy(currentFrameData.mappedMeshInstances.data(), writtenInstances.data(), writtenInstances.size() * sizeof(RendererVKLayout::InMeshInstance));

    m_indirectCullComputePipeline.resizeInstanceBuffers(m_maxInstanceData);
    m_shadowCullComputePipeline.resizeInstanceBuffers(m_maxInstanceData);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew mesh instance capacity to %u\n", m_maxInstanceData);
}

void Renderer::growUniqueMeshCapacity(uint32 needed)
{
    m_maxUniqueMeshes = growCapacity(m_maxUniqueMeshes, needed, RendererVKLayout::MESH_MATERIAL_INDEX_LIMIT);
    waitForGpuAndFlushStaging();
    for (PerFrameData& perFrame : m_perFrameData)
    {
        // No contents to preserve: first-instance offsets are rewritten every present().
        perFrame.inFirstInstancesBuffer.initialize(m_maxUniqueMeshes * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "FirstInstances", BufferHostAccess::eSequentialWrite);
        perFrame.mappedFirstInstances = perFrame.inFirstInstancesBuffer.mapMemory<uint32>();
    }
    m_meshInfosBuffer.resize(m_maxUniqueMeshes * sizeof(RendererVKLayout::MeshInfo));
    // Fresh (unmirrored) buffer: re-upload the whole per-mesh LOD group mapping, padded to capacity so
    // future slots read as "no chain".
    m_meshLodGroupIdxBuffer.initialize(m_maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroupIdx");
    {
        oc::vector<uint32> mapping(m_maxUniqueMeshes, UINT32_MAX);
        memcpy(mapping.data(), m_meshToLodGroup.data(), m_meshToLodGroup.size() * sizeof(uint32));
        uploadToSharedBuffer(m_meshLodGroupIdxBuffer, mapping.size() * sizeof(uint32), mapping.data(), 0);
    }
    m_accelStructure.resizeBlasAddressBuffer(m_maxUniqueMeshes);
    m_indirectCullComputePipeline.resizeCommandBuffers(m_maxUniqueMeshes);
    m_shadowCullComputePipeline.resizeCommandBuffers(m_maxUniqueMeshes);
    m_staticMeshGraphicsPipeline.resizeMeshCapacity(m_maxUniqueMeshes);
    m_shadowMapGraphicsPipeline.resizeMeshCapacity(m_maxUniqueMeshes);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew unique mesh capacity to %u\n", m_maxUniqueMeshes);
}

void Renderer::growMaterialCapacity(uint32 needed)
{
    m_maxUniqueMaterials = growCapacity(m_maxUniqueMaterials, needed, RendererVKLayout::MESH_MATERIAL_INDEX_LIMIT);
    waitForGpuAndFlushStaging();
    m_materialInfosBuffer.resize(m_maxUniqueMaterials * sizeof(RendererVKLayout::MaterialInfo));
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew material capacity to %u\n", m_maxUniqueMaterials);
}

void Renderer::createLightGridBuffers()
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.lightGridsBuffer.initialize(m_lightGridBufferSize,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LightGrids");

        // Device-local + host-visible (ReBAR) so GPU writes are fast; the CPU reads the header in getStats,
        // hence the default random (cached-where-possible) access.
        perFrame.lightTableBuffer.initialize(3 * sizeof(uint32) + sizeof(uint32) * m_lightTableEntries,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible, false, "LightTable");

        // Zero the readback header so the capacity check / stats never see uninitialized counters
        // before the first recorded frame fills them in.
        oc::span<uint32> header = perFrame.lightTableBuffer.mapMemory<uint32>(0, 3 * sizeof(uint32));
        memset(header.data(), 0, 3 * sizeof(uint32));
        perFrame.lightTableBuffer.flushMappedMemory(3 * sizeof(uint32));
        perFrame.lightTableBuffer.unmapMemory();
    }
}

void Renderer::growLightGridBuffers(size_t neededGridBytes, uint32 neededTableEntries)
{
    while (m_lightGridBufferSize < neededGridBytes)
        m_lightGridBufferSize *= 2;
    while (m_lightTableEntries < neededTableEntries)
        m_lightTableEntries *= 2; // stays a power of 2 for the hash
    waitForGpuAndFlushStaging();
    createLightGridBuffers(); // per-frame GPU scratch, rebuilt every frame: nothing to preserve
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew light grid buffers to %zu bytes / %u table entries\n", m_lightGridBufferSize, m_lightTableEntries);
}

void Renderer::checkLightGridCapacity()
{
    // Read last frame's usage counters (same readback as getStats) and grow before the buffers fill:
    // above a quarter of the table (hash collision quality) or 3/4 of the grid data. Growth targets FIT
    // the observed demand (with headroom) rather than doubling once: a burst can overshoot the current
    // capacity many times over within a single frame. When the shader runs out of space it drops lights
    // for that frame (getOrInsertGrid -> INVALID_GRID) but keeps incrementing gridDataCounter, so the
    // readback measures the true demand of an overflowed frame.
    PerFrameData& lastFrameData = m_perFrameData[(m_swapChain.getCurrentFrameIndex() + m_perFrameData.size() - 1) % m_perFrameData.size()];
    struct LightGridInfo { uint32 numGrids; uint32 gridDataCounter; };
    oc::span<LightGridInfo> infoSpan = lastFrameData.lightTableBuffer.mapMemory<LightGridInfo>(0, sizeof(LightGridInfo));
    const LightGridInfo info = *infoSpan.data();
    lastFrameData.lightTableBuffer.unmapMemory();

    const bool tableHigh = info.numGrids > m_lightTableEntries / 4;
    const bool gridHigh = (size_t)info.gridDataCounter * sizeof(uint32) > m_lightGridBufferSize / 4 * 3;
    if (tableHigh || gridHigh)
        growLightGridBuffers(gridHigh ? (size_t)(info.gridDataCounter * sizeof(uint32) * 1.5f) : m_lightGridBufferSize, tableHigh ? info.numGrids * 8 : m_lightTableEntries);
}

void Renderer::growInstanceOffsetCapacity(uint32 needed)
{
    m_maxInstanceOffsets = growCapacity(m_maxInstanceOffsets, needed);
    waitForGpuAndFlushStaging();
    m_instanceOffsetsBuffer.resize(m_maxInstanceOffsets * sizeof(RendererVKLayout::MeshInstanceOffset));
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew instance offset capacity to %u\n", m_maxInstanceOffsets);
}

void Renderer::growLodStateCapacity(uint32 needed)
{
    m_maxLodStateSlots = growCapacity(m_maxLodStateSlots, needed);
    waitForGpuAndFlushStaging();
    // Contents are advisory hysteresis history (clamped into a valid band on read), so the old
    // buffer's state doesn't need preserving.
    m_lodLevelStateBuffer.initialize(m_maxLodStateSlots * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "LodLevelState");
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew LOD state capacity to %u\n", m_maxLodStateSlots);
}

void Renderer::growMeshLodGroupCapacity(uint32 needed)
{
    m_maxMeshLodGroups = growCapacity(m_maxMeshLodGroups, needed);
    waitForGpuAndFlushStaging();
    m_meshLodGroupsBuffer.initialize(m_maxMeshLodGroups * sizeof(RendererVKLayout::GpuMeshLodGroup),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshLodGroups");
    for (uint32 i = 0; i < (uint32)m_meshLodGroups.size(); ++i)
        uploadMeshLodGroup(i); // fresh buffer: re-publish every registered group
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew mesh LOD group capacity to %u\n", m_maxMeshLodGroups);
}

void Renderer::recordIndirectCull(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.indirectCullCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    IndirectCullComputePipeline::RecordParams cullParams
    {
        .descriptorSet = frameData.indirectCullPipelineDescriptorSet,
        .ubo = frameData.ubo,
        .inRenderNodeTransformsBuffer = frameData.inRenderNodeTransformsBuffer,
        .inMeshInstancesBuffer = frameData.inMeshInstancesBuffer,
        .inMeshInstanceOffsetsBuffer = m_instanceOffsetsBuffer,
        .inMeshInfoBuffer = m_meshInfosBuffer,
        .inFirstInstancesBuffer = frameData.inFirstInstancesBuffer,
        .inNodePassMasksBuffer = frameData.inNodePassMasksBuffer,
        .inMeshLodGroupIdxBuffer = m_meshLodGroupIdxBuffer,
        .inMeshLodGroupsBuffer = m_meshLodGroupsBuffer,
        .lodLevelStateBuffer = m_lodLevelStateBuffer,
        .inNodeLodStateBiasBuffer = frameData.inNodeLodStateBiasBuffer,
        .outLodStatsBuffer = frameData.lodStatsBuffer,
    };
    m_indirectCullComputePipeline.record(cb, frameIdx, cullParams);
    cb.end();
}

uint32 Renderer::allocateSkinningPalette(uint32 boneCount)
{
    // The palette store is a bump allocator; freed regions (destroyed containers) are recycled on an
    // exact boneCount match — same-skeleton respawns, the common case — instead of tracking sub-ranges.
    for (size_t i = 0; i < m_freeSkinningPaletteHandles.size(); ++i)
    {
        const uint32 handle = m_freeSkinningPaletteHandles[i];
        const SkinningPaletteRegion& region = m_skinningPaletteRegions[handle];
        if (region.boneCount == boneCount)
        {
            m_freeSkinningPaletteHandles[i] = m_freeSkinningPaletteHandles.back();
            m_freeSkinningPaletteHandles.pop_back();
            oc::fill_n(m_skinningPalettes.begin() + region.offset, boneCount, glm::mat4(1.0f));
            return handle;
        }
    }
    const uint32 handle = (uint32)m_skinningPaletteRegions.size();
    const uint32 offset = (uint32)m_skinningPalettes.size();
    m_skinningPaletteRegions.push_back({ offset, boneCount });
    m_skinningPalettes.resize(offset + boneCount, glm::mat4(1.0f)); // identity until first setSkinningPalette
    if ((uint32)m_skinningPalettes.size() > m_maxSkinningPaletteEntries)
        growSkinningPaletteCapacity((uint32)m_skinningPalettes.size());
    return handle;
}

void Renderer::setSkinningPalette(uint32 paletteHandle, oc::span<const glm::mat4> palette)
{
    assert(paletteHandle < m_skinningPaletteRegions.size());
    const SkinningPaletteRegion& region = m_skinningPaletteRegions[paletteHandle];
    const uint32 count = oc::min((uint32)palette.size(), region.boneCount);
    if (count > 0)
        memcpy(m_skinningPalettes.data() + region.offset, palette.data(), count * sizeof(glm::mat4));
}

uint32 Renderer::allocateSkinningJobRange(uint32 count)
{
    uint32 firstJob = m_freeSkinningJobSlots.allocate(count);
    if (firstJob == UINT32_MAX)
    {
        firstJob = (uint32)m_skinningJobs.size();
        // Value-initialized: vertexCount/indexCount 0 keeps a slot inert until setSkinnedInstance fills it.
        m_skinningJobs.resize(firstJob + count);
        m_skinnedBlasBuilds.resize(firstJob + count);
        if ((uint32)m_skinningJobs.size() > m_maxSkinningJobs)
            growSkinningJobCapacity((uint32)m_skinningJobs.size());
    }
    return firstJob;
}

void Renderer::setSkinnedInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset, uint32 vertexCount, uint32 paletteHandle,
    uint32 meshIdx, uint32 firstIndex, uint32 indexCount)
{
    assert(paletteHandle < m_skinningPaletteRegions.size());
    m_skinningJobs[jobIdx] = RendererVKLayout::SkinningJob{
        .baseVertexOffset = baseVertexOffset,
        .skinVertexOffset = skinVertexOffset,
        .outVertexOffset = outVertexOffset,
        .vertexCount = vertexCount,
        .paletteOffset = m_skinningPaletteRegions[paletteHandle].offset,
    };
    m_skinnedBlasBuilds[jobIdx] = AccelerationStructure::SkinnedBlasBuild{
        .meshIdx = meshIdx, .vertexOffset = outVertexOffset, .vertexCount = vertexCount, .firstIndex = firstIndex, .indexCount = indexCount };
    // No re-record needed: the jobs are uploaded per frame and dispatched indirectly; the skinned BLAS
    // list is consumed by recordGlobalIllum, which re-records every frame.
}

void Renderer::growSkinningPaletteCapacity(uint32 needed)
{
    m_maxSkinningPaletteEntries = growCapacity(m_maxSkinningPaletteEntries, needed);
    waitForGpuAndFlushStaging();
    m_skinningComputePipeline.resizePaletteBuffer(m_maxSkinningPaletteEntries);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew skinning palette capacity to %u\n", m_maxSkinningPaletteEntries);
}

void Renderer::growSkinningJobCapacity(uint32 needed)
{
    m_maxSkinningJobs = growCapacity(m_maxSkinningJobs, needed);
    waitForGpuAndFlushStaging();
    m_skinningComputePipeline.resizeJobBuffer(m_maxSkinningJobs);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew skinning job capacity to %u\n", m_maxSkinningJobs);
}

void Renderer::recordSkinning(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.skinningCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    SkinningComputePipeline::RecordParams params{
        .descriptorSet = frameData.skinningDescriptorSet,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .skinningBuffer = Globals::meshDataManager.getSkinningBuffer(),
    };
    m_skinningComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordOceanSim(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.oceanSimCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    m_oceanSimPipeline.record(cb, frameIdx, frameData.ubo);
    cb.end();
}

void Renderer::recordLightGrid(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.lightGridCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    LightGridComputePipeline::RecordParams params
    {
        .descriptorSet = frameData.lightGridPipelineDescriptorSet,
        .ubo = frameData.ubo,
        .inLightInfoBuffer = frameData.lightInfosBuffer,
        .outLightGridBuffer = frameData.lightGridsBuffer,
        .outLightTableBuffer = frameData.lightTableBuffer,
        .numTableEntries = m_lightTableEntries,
    };
    m_lightGridComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordShadowCull(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.shadowCullCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);

    ShadowCullComputePipeline::RecordParams params{
        .descriptorSet = frameData.shadowCullDescriptorSet,
        .ubo = frameData.ubo,
        .dispatchIndirectBuffer = m_indirectCullComputePipeline.getDispatchIndirectBuffer(frameIdx),
        .inRenderNodeTransformsBuffer = frameData.inRenderNodeTransformsBuffer,
        .inMeshInstancesBuffer = frameData.inMeshInstancesBuffer,
        .inMeshInstanceOffsetsBuffer = m_instanceOffsetsBuffer,
        .inMeshInfoBuffer = m_meshInfosBuffer,
        .inFirstInstancesBuffer = frameData.inFirstInstancesBuffer,
        .inMaterialInfoBuffer = m_materialInfosBuffer,
        .inNodePassMasksBuffer = frameData.inNodePassMasksBuffer,
        .inMeshLodGroupIdxBuffer = m_meshLodGroupIdxBuffer,
        .inMeshLodGroupsBuffer = m_meshLodGroupsBuffer,
    };
    m_shadowCullComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordShadowDraw(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    ShadowMap& shadowMap = frameData.shadowMap;
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = shadowMap.getRenderPass() };
    CommandBuffer& cb = frameData.shadowDrawCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    // All cascades render in a single multiview render pass; gl_ViewIndex selects the layer.
    const vk::Extent2D shadowExtent{ shadowMap.getResolution(), shadowMap.getResolution() };
    const float shadowRes = (float)shadowMap.getResolution();
    const vk::Viewport viewport{ .x = 0.0f, .y = 0.0f, .width = shadowRes, .height = shadowRes, .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = shadowExtent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });

    ShadowMapGraphicsPipeline::RecordParams params{
        .descriptorSet = frameData.shadowDrawDescriptorSet,
        .ubo = frameData.ubo,
        .meshInstanceBuffer = m_shadowCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .instanceIdxBuffer = m_shadowCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .indirectCommandBuffer = m_shadowCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .meshCountBuffer = frameData.meshCountBuffer,
    };
    m_shadowMapGraphicsPipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordReuseDepthBarrier(vk::CommandBuffer cb, vk::Image gbufferDepth, uint32 eyeIndex, bool toAttachment)
{
    // Reads only on both sides (the reuse pass tests depth read-only; AO/fog/TAA sample) — nothing to
    // flush, so srcAccess stays empty and the barrier is an execution dependency + the layout transition.
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = toAttachment ? (vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader)
                                     : (vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests),
        .srcAccessMask = vk::AccessFlags2(),
        .dstStageMask = toAttachment ? (vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests)
                                     : (vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader),
        .dstAccessMask = toAttachment ? vk::AccessFlags2(vk::AccessFlagBits2::eDepthStencilAttachmentRead)
                                      : vk::AccessFlags2(vk::AccessFlagBits2::eShaderSampledRead),
        .oldLayout = toAttachment ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        .newLayout = toAttachment ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = gbufferDepth,
        .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, eyeIndex, 1 },
    };
    cb.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier });
}

void Renderer::recordStaticMesh(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{
        .renderPass = m_depthPrepassReuse ? frameData.sceneColor.getReuseRenderPass() : frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.staticMeshCommandBuffer;
    cb.begin(false, &inheritance);
    recordStaticMeshInto(cb, frameIdx, 0);
    cb.end();
}

void Renderer::recordStaticMeshInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();

    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 viewportSize = m_viewportRect.getSize();
    const vk::Viewport viewport{
        .x = (float)m_viewportRect.min.x,
        .y = (float)m_viewportRect.max.y,
        .width = (float)viewportSize.x,
        .height = -((float)viewportSize.y),
        .minDepth = 0.0f,
        .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    StaticMeshGraphicsPipeline::RecordParams drawParams
    {
        .descriptorSet = frameData.staticMeshPipelineDescriptorSet[eyeIndex],
        .ubo = frameData.ubo,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .materialInfoBuffer = m_materialInfosBuffer,
        .instanceIdxBuffer = m_indirectCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .meshInstanceBuffer = m_indirectCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .indirectCommandBuffer = m_indirectCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .transparentIndirectCommandBuffer = m_indirectCullComputePipeline.getTransparentIndirectCommandBuffer(frameIdx),
        .meshCountBuffer = frameData.meshCountBuffer,
        .lightInfosBuffer = frameData.lightInfosBuffer,
        .lightGridsBuffer = frameData.lightGridsBuffer,
        .lightTableBuffer = frameData.lightTableBuffer,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .meshInfoBuffer = m_meshInfosBuffer,
        .rtMeshInstancesBuffer = frameData.inMeshInstancesBuffer,
        .shadowMapView = frameData.shadowMap.getSampleView(),
        .shadowMapSampler = frameData.shadowMap.getSampler(),
        .shadowMapDepthSampler = frameData.shadowMap.getDepthSampler(),
        .gbufferDepthView = frameData.gbuffer.getDepthView(eyeIndex),
        .gbufferSampler = frameData.gbuffer.getSampler(),
        .oceanMapsView = m_oceanSimPipeline.getMapsView(),
        .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
        .viewIndex = RendererVKLayout::eyeToViewIndex(eyeIndex, m_sceneViewCount),
    };
    // Each eye has its own descriptor set (per-eye AO + gbuffer depth), so both eyes write their own.
    m_staticMeshGraphicsPipeline.record(cb, frameIdx, drawParams, true);
}

void Renderer::recordGBufferInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();

    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 viewportSize = m_viewportRect.getSize();
    const vk::Viewport viewport{
        .x = (float)m_viewportRect.min.x,
        .y = (float)m_viewportRect.max.y,
        .width = (float)viewportSize.x,
        .height = -((float)viewportSize.y),
        .minDepth = 0.0f,
        .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    GBufferPipeline::RecordParams gbufferParams{
        .descriptorSet = frameData.gbufferDescriptorSet[eyeIndex],
        .ubo = frameData.ubo,
        .meshInstanceBuffer = m_indirectCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .instanceIdxBuffer = m_indirectCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .indirectCommandBuffer = m_indirectCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .materialInfoBuffer = m_materialInfosBuffer,
        .meshCountBuffer = frameData.meshCountBuffer,
        .oceanMapsView = m_oceanSimPipeline.getMapsView(),
        .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
    };
    m_gbufferPipeline.record(cb, frameIdx, gbufferParams, RendererVKLayout::eyeToViewIndex(eyeIndex, m_sceneViewCount));
}

void Renderer::recordGBuffer(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.gbuffer.getRenderPass() };
    CommandBuffer& cb = frameData.gbufferCommandBuffer;
    cb.begin(false, &inheritance);
    recordGBufferInto(cb, frameIdx, 0);
    cb.end();
}


// AO trace+denoise for one eye (compute, no render pass); reads that eye's G-buffer, writes that eye's AO.
void Renderer::recordAOInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    GBuffer& gbuffer = frameData.gbuffer;
    const vk::AccelerationStructureKHR tlas = m_accelStructure.getTlas(frameIdx);
    if (!m_rtParams.enabled || m_meshInfoCounter == 0 || !tlas) // RT off: tlas is stale, don't trace it
        return;
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    RTAOPipeline::RecordParams aoParams{
        .ubo = frameData.ubo,
        .gbufferNormalView = gbuffer.getNormalView(eyeIndex),
        .gbufferDepthView = gbuffer.getDepthView(eyeIndex),
        .prevGbufferDepthView = m_perFrameData[prevFrameIdx].gbuffer.getDepthView(eyeIndex),
        .gbufferSampler = gbuffer.getSampler(),
        .tlas = tlas,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .meshInfos = m_meshInfosBuffer,
        .meshInstances = frameData.inMeshInstancesBuffer,
        .materialInfos = m_materialInfosBuffer,
    };
    m_rtaoPipeline.record(cb, frameIdx, eyeIndex, aoParams);
}

// Fog apply draw for one eye, inside the eye's scene-colour render pass (viewport set by the caller).
void Renderer::recordFogApplyInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });
    VolumetricFogPipeline::ApplyParams params{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .gbufferDepthView = frameData.gbuffer.getDepthView(eyeIndex),
        // Runs inside the scene pass, where depth-prepass reuse holds the image in DEPTH_STENCIL_READ_ONLY.
        .gbufferDepthLayout = m_depthPrepassReuse ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_volumetricFogPipeline.recordApply(cb, frameIdx, eyeIndex, params);
}

// TAA resolve for one eye (compute, no render pass); reads that eye's scene colour + depth, writes its history.
void Renderer::recordTaaInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    GBuffer& gbuffer = frameData.gbuffer;
    SceneColor& sceneColor = frameData.sceneColor;
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    TaaPipeline::RecordParams taaParams{
        .ubo = frameData.ubo,
        .currentColorView = sceneColor.getColorLayerView(eyeIndex),
        .currentColorSampler = sceneColor.getSampler(),
        .gbufferDepthView = gbuffer.getDepthView(eyeIndex),
        .prevGbufferDepthView = m_perFrameData[prevFrameIdx].gbuffer.getDepthView(eyeIndex),
        .gbufferNormalView = gbuffer.getNormalView(eyeIndex),
        .gbufferSampler = gbuffer.getSampler(),
        .feedback = m_taaParams.taaEnabled ? m_taaParams.taaFeedback : 0.0f,
        .oceanFeedback = m_taaParams.taaEnabled ? m_taaParams.taaOceanFeedback : 0.0f,
    };
    m_taaPipeline.record(cb, frameIdx, eyeIndex, taaParams);
}

void Renderer::recordGiProbeDebug(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.giProbeDebugCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    m_giProbePipeline.recordDebugDraw(cb, frameIdx, frameData.ubo, m_giProbeDebugRadius, m_giProbeDebugMode);
    cb.end();
}

void Renderer::recordDebugLines(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.debugLineCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    m_debugLinePipeline.record(cb, frameIdx, frameData.ubo);
    cb.end();
}

// Particle GPU sim (begin/emit/simulate compute chain), its own secondary outside any render pass;
// executed right after the light grid in the primary. Reads LAST frame's G-buffer for depth collision.
void Renderer::recordParticleSim(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.particleSimCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    ParticlePipeline::SimParams simParams{
        .ubo = frameData.ubo,
        .prevDepthView = m_perFrameData[prevFrameIdx].gbuffer.getDepthView(),
        .prevNormalView = m_perFrameData[prevFrameIdx].gbuffer.getNormalView(),
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_particlePipeline.recordSim(cb, frameIdx, simParams);
    cb.end();
}

// Particle billboard draw for one eye, inside the eye's scene-colour render pass (after the opaque
// forward + decal draws, before fog apply).
void Renderer::recordParticlesInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    ParticlePipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .gbufferDepthView = frameData.gbuffer.getDepthView(eyeIndex),
        // Runs inside the scene pass, where depth-prepass reuse holds the image in DEPTH_STENCIL_READ_ONLY.
        .gbufferDepthLayout = m_depthPrepassReuse ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_particlePipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams);
}

void Renderer::recordParticles(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.particleCommandBuffer;
    cb.begin(false, &inheritance);
    recordParticlesInto(cb, frameIdx, 0);
    cb.end();
}

// Projected decal draw for one eye, inside the eye's scene-colour render pass right after the opaque
// forward draw (so particles/fog layer on top).
void Renderer::recordDecalsInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    DecalPipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .gbufferDepthView = frameData.gbuffer.getDepthView(eyeIndex),
        // Runs inside the scene pass, where depth-prepass reuse holds the image in DEPTH_STENCIL_READ_ONLY.
        .gbufferDepthLayout = m_depthPrepassReuse ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .gbufferNormalView = frameData.gbuffer.getNormalView(eyeIndex),
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_decalPipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams);
}

void Renderer::recordDecals(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.decalCommandBuffer;
    cb.begin(false, &inheritance);
    recordDecalsInto(cb, frameIdx, 0);
    cb.end();
}

// Forcefield shell draw for one eye, inside the eye's scene-colour render pass after the debug
// overlays (so particles/fog layer on top of the bubbles).
void Renderer::recordForceFieldInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    ForceFieldPipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .gbufferDepthView = frameData.gbuffer.getDepthView(eyeIndex),
        // Runs inside the scene pass, where depth-prepass reuse holds the image in DEPTH_STENCIL_READ_ONLY.
        .gbufferDepthLayout = m_depthPrepassReuse ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_forceFieldPipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams);
}

void Renderer::recordForceField(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.forceFieldCommandBuffer;
    cb.begin(false, &inheritance);
    recordForceFieldInto(cb, frameIdx, 0);
    cb.end();
}

// Force grid build + per-emitter force / point-query dispatches (outside any render pass, after the
// light grid). All dispatches ride mapped indirect buffers, so emitter/query changes never re-record.
void Renderer::recordForceCompute(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.forceComputeCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    m_forceFieldPipeline.recordCompute(cb, frameIdx, frameData.ubo);
    cb.end();
}

void Renderer::recordAO(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    GBuffer& gbuffer = frameData.gbuffer;
    CommandBuffer& cb = frameData.aoCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    const vk::AccelerationStructureKHR tlas = m_accelStructure.getTlas(frameIdx);
    if (m_rtParams.enabled && m_rtaoParams.enabled && m_meshInfoCounter > 0 && tlas)
    {
        const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
        RTAOPipeline::RecordParams aoParams{
            .ubo = frameData.ubo,
            .gbufferNormalView = gbuffer.getNormalView(),
            .gbufferDepthView = gbuffer.getDepthView(),
            .prevGbufferDepthView = m_perFrameData[prevFrameIdx].gbuffer.getDepthView(),
            .gbufferSampler = gbuffer.getSampler(),
            .tlas = tlas,
            .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
            .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
            .meshInfos = m_meshInfosBuffer,
            .meshInstances = frameData.inMeshInstancesBuffer,
            .materialInfos = m_materialInfosBuffer,
        };
        m_rtaoPipeline.record(cb, frameIdx, 0, aoParams);
    }
    cb.end();
}

void Renderer::recordVolumetricFog(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.volumetricFogCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    const vk::AccelerationStructureKHR tlas = m_accelStructure.getTlas(frameIdx);
    if (m_rtParams.enabled && m_meshInfoCounter > 0 && tlas)
    {
        VolumetricFogPipeline::RecordParams params{
            .ubo = frameData.ubo,
            .lightInfosBuffer = frameData.lightInfosBuffer,
            .lightGridsBuffer = frameData.lightGridsBuffer,
            .lightTableBuffer = frameData.lightTableBuffer,
            .fogVolumesBuffer = frameData.fogVolumesBuffer,
            .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
            .shadowMapView = frameData.shadowMap.getSampleView(),
            .shadowMapSampler = frameData.shadowMap.getSampler(),
            .oceanMapsView = m_oceanSimPipeline.getMapsView(),
            .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
            .tlas = tlas,
        };
        m_volumetricFogPipeline.record(cb, frameIdx, params);
    }
    cb.end();
}

void Renderer::recordFogApply(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = frameData.sceneColor.getRenderPass() };
    CommandBuffer& cb = frameData.fogApplyCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    // Fullscreen triangle in full-render-target UV space (like the composite), scissored to the viewport.
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });

    VolumetricFogPipeline::ApplyParams params{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .gbufferDepthView = frameData.gbuffer.getDepthView(),
        // Runs inside the scene pass, where depth-prepass reuse holds the image in DEPTH_STENCIL_READ_ONLY.
        .gbufferDepthLayout = m_depthPrepassReuse ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        .gbufferSampler = frameData.gbuffer.getSampler(),
    };
    m_volumetricFogPipeline.recordApply(cb, frameIdx, 0, params);
    cb.end();
}

void Renderer::recordTaa(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    GBuffer& gbuffer = frameData.gbuffer;
    SceneColor& sceneColor = frameData.sceneColor;
    CommandBuffer& cb = frameData.taaCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    TaaPipeline::RecordParams taaParams{
        .ubo = frameData.ubo,
        .currentColorView = sceneColor.getColorView(),
        .currentColorSampler = sceneColor.getSampler(),
        .gbufferDepthView = gbuffer.getDepthView(),
        .prevGbufferDepthView = m_perFrameData[prevFrameIdx].gbuffer.getDepthView(),
        .gbufferNormalView = gbuffer.getNormalView(),
        .gbufferSampler = gbuffer.getSampler(),
        .feedback = m_taaParams.taaEnabled ? m_taaParams.taaFeedback : 0.0f,
        .oceanFeedback = m_taaParams.taaEnabled ? m_taaParams.taaOceanFeedback : 0.0f,
    };
    m_taaPipeline.record(cb, frameIdx, 0, taaParams);
    cb.end();
}

void Renderer::recordEyeAdaptation(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.eyeAdaptCommandBuffer;
    vk::CommandBufferInheritanceInfo inheritance;
    cb.begin(false, &inheritance);
    // TAA OFF: the pass is not recorded or executed at all (see recordCommandBuffers), so the
    // post chain reads this frame's scene colour directly instead of TAA's resolved image.
    const bool taaOn = m_taaParams.taaEnabled;
    EyeAdaptationPipeline::RecordParams params{
        .resolvedView = taaOn ? m_taaPipeline.getResolvedView(frameIdx, 0) : frameData.sceneColor.getColorLayerView(0),
        .resolvedLayout = taaOn ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
        .sampler = taaOn ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
        .viewportMin = m_viewportRect.min,
        .viewportSize = m_viewportRect.getSize(),
    };
    m_eyeAdaptationPipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordComposite(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance { .renderPass = m_renderPass.getRenderPass(), };
    CommandBuffer& cb = frameData.compositeCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{.x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{.offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });

    const bool taaOn = m_taaParams.taaEnabled; // TAA bypassed: tonemap the scene colour directly
    CompositePipeline::RecordParams params{
        .descriptorSet = frameData.compositeDescriptorSet,
        .resolvedView = taaOn ? m_taaPipeline.getResolvedView(frameIdx, 0) : frameData.sceneColor.getColorLayerView(0),
        .resolvedLayout = taaOn ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
        .sampler = taaOn ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
        .exposureBuffer = m_eyeAdaptationPipeline.getExposureBuffer().getBuffer(),
        .exposureEV = m_postParams.exposureEV,
        .tonemapper = m_postParams.tonemapper,
        .autoExposure = m_postParams.autoExposure ? 1 : 0,
    };
    m_compositePipeline.record(cb, params);
    cb.end();
}

void Renderer::createEyeCompositeTargets()
{
    if (m_sceneViewCount <= 1)
        return;
    destroyEyeCompositeTargets();
    vk::Device vkDevice = Globals::device.getDevice();
    const vk::Extent2D ext = m_swapChain.getLayout().extent;
    const vk::Format colorFormat = m_swapChain.getLayout().surfaceFormat.format;

    // Shared depth (the swapchain render pass declares a depth attachment; the composite doesn't use it).
    vk::ImageCreateInfo depthInfo{
        .imageType = vk::ImageType::e2D, .format = vk::Format::eD32Sfloat, .extent = { ext.width, ext.height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment, .sharingMode = vk::SharingMode::eExclusive, .initialLayout = vk::ImageLayout::eUndefined };
    Globals::gpuAllocator.createImage(depthInfo, m_eyeDepthImage, m_eyeDepthMem, "VR.eyeDepth");
    vk::ImageViewCreateInfo depthViewInfo{ .image = m_eyeDepthImage, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eD32Sfloat,
        .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 } };
    m_eyeDepthView = vkDevice.createImageView(depthViewInfo).value;

    for (uint32 i = 0; i < 2; ++i)
    {
        vk::ImageCreateInfo colorInfo{
            .imageType = vk::ImageType::e2D, .format = colorFormat, .extent = { ext.width, ext.height, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive, .initialLayout = vk::ImageLayout::eUndefined };
        Globals::gpuAllocator.createImage(colorInfo, m_eyeColorImage[i], m_eyeColorMem[i], "VR.eyeColor");
        vk::ImageViewCreateInfo colorViewInfo{ .image = m_eyeColorImage[i], .viewType = vk::ImageViewType::e2D, .format = colorFormat,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 } };
        m_eyeColorView[i] = vkDevice.createImageView(colorViewInfo).value;
        oc::array<vk::ImageView, 2> atts{ m_eyeColorView[i], m_eyeDepthView };
        vk::FramebufferCreateInfo fbInfo{ .renderPass = m_renderPass.getRenderPass(), .attachmentCount = (uint32)atts.size(), .pAttachments = atts.data(),
            .width = ext.width, .height = ext.height, .layers = 1 };
        m_eyeFramebuffer[i] = vkDevice.createFramebuffer(fbInfo).value;
    }
}

void Renderer::destroyEyeCompositeTargets()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < 2; ++i)
    {
        if (m_eyeFramebuffer[i]) { vkDevice.destroyFramebuffer(m_eyeFramebuffer[i]); m_eyeFramebuffer[i] = nullptr; }
        if (m_eyeColorView[i]) { vkDevice.destroyImageView(m_eyeColorView[i]); m_eyeColorView[i] = nullptr; }
        if (m_eyeColorImage[i]) { Globals::gpuAllocator.destroyImage(m_eyeColorImage[i], m_eyeColorMem[i]); m_eyeColorImage[i] = nullptr; m_eyeColorMem[i] = nullptr; }
    }
    if (m_eyeDepthView) { vkDevice.destroyImageView(m_eyeDepthView); m_eyeDepthView = nullptr; }
    if (m_eyeDepthImage) { Globals::gpuAllocator.destroyImage(m_eyeDepthImage, m_eyeDepthMem); m_eyeDepthImage = nullptr; m_eyeDepthMem = nullptr; }
}

bool Renderer::recordGlobalIllum(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& globalIllumCommandBuffer = frameData.globalIllumCommandBuffer;
    vk::CommandBufferInheritanceInfo globalIllumInheritanceInfo;
    vk::CommandBuffer vkGlobalIllumCommandBuffer = globalIllumCommandBuffer.begin(false, &globalIllumInheritanceInfo);

    // RT master toggle off: record an EMPTY GI command buffer (the primary executes it unconditionally) so
    // no acceleration structures are built/compacted and no rays are traced, and return false so the caller
    // skips the RT-dependent AO / volumetric-fog passes. Diagnostic A/B for the acceleration-structure churn.
    if (!m_rtParams.enabled)
    {
        globalIllumCommandBuffer.end();
        return false;
    }

    auto fullBarrier = [&](vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
        {
            vk::MemoryBarrier2 bar{ .srcStageMask = srcStage, .srcAccessMask = srcAccess, .dstStageMask = dstStage, .dstAccessMask = dstAccess };
            vkGlobalIllumCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
        };

    // The skinning compute (executed earlier in the primary) wrote the deformed vertices that both the
    // one-time static build (for skinned output regions) and the per-frame skinned rebuild read.
    if (!m_skinnedBlasBuilds.empty())
        fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eShaderRead);

    // 1. Build BLASes for meshes added since last frame (one-time per mesh; LOD levels alias their
    // chain's RT-level BLAS and build nothing) plus any re-streamed meshes queued for a rebuild.
    if (m_blasBuiltCount < m_meshInfoCounter || !m_pendingBlasRebuilds.empty())
    {
        oc::vector<uint32> buildList = oc::move(m_pendingBlasRebuilds);
        m_pendingBlasRebuilds.clear();
        for (uint32 meshIdx = m_blasBuiltCount; meshIdx < m_meshInfoCounter; ++meshIdx)
        {
            // Skinned output regions never build static BLASes: their data is uninitialized until the
            // skinning compute runs, and their address entries are owned per frame slot by the skinned
            // rebuild — a static build entering compaction would clobber them with a garbage BLAS.
            if (!m_meshIsSkinnedOutput[meshIdx])
                buildList.push_back(meshIdx);
        }
        m_blasBuiltCount = m_meshInfoCounter;
        m_accelStructure.recordBuildBlas(frameIdx, vkGlobalIllumCommandBuffer, Globals::meshDataManager.getVertexBuffer(), Globals::meshDataManager.getIndexBuffer(),
            m_meshInfosBuffer.getBackingStoreAs<RendererVKLayout::MeshInfo>().data(), m_meshVertexCounts.data(), buildList,
            m_rtParams.blasCompaction);
        // No GI clear here: new meshes (terrain streaming!) leave the persistent probe volume intact.
        // Stale irradiance around new geometry self-corrects — embedded probes relocate out the next
        // frame (with a fast history re-sync), the rest re-converge at the temporal blend rate.
    }

    // 1a. Copy-compact BLASes whose size queries matured, and retire replaced originals.
    m_accelStructure.recordCompaction(frameIdx, vkGlobalIllumCommandBuffer);

    // Publish this frame slot's pending static BLAS-address changes (build/compaction/eviction) into its
    // own fenced address buffer. BEFORE the skinned rebuild, so a slot reused static->skinned keeps the
    // skinned per-slot address (written next), not a stale static value. Other slots pick up their copies
    // in their own frames -- static slots are never written here while they may be in flight.
    m_accelStructure.syncFrameAddresses(frameIdx);

    // 1b. Rebuild skinned meshes' BLASes every frame from this frame's deformed vertices, into this frame's
    // double-buffered slot (the other slot may still be referenced by the previous frame's in-flight TLAS).
    if (!m_skinnedBlasBuilds.empty())
    {
        m_accelStructure.recordBuildSkinnedBlas(vkGlobalIllumCommandBuffer, frameIdx,
            Globals::meshDataManager.getVertexBuffer(), Globals::meshDataManager.getIndexBuffer(), m_skinnedBlasBuilds);
        // Skinned BLAS builds -> TLAS build reads them.
        fullBarrier(vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    }

    // 2. One-time clear of the persistent probe table/SH (it accumulates across frames thereafter).
    if (m_rtParams.giEnabled && m_giProbePipeline.needsClear())
    {
        m_giProbePipeline.recordClearPersistent(globalIllumCommandBuffer);
        m_giProbePipeline.markCleared();
    }

    // Make prior writes visible to the GI compute passes:
    //  - the light grid (compute storage writes) that the trace reuses to shade hits, and
    //  - the sun cascade shadow map depth writes (late/early fragment tests) that the trace samples.
    // The shadow render pass only synchronizes its depth writes to the FRAGMENT stage (for the main
    // pass); without this the compute trace samples the depth image with no dependency, which faults
    // NVIDIA (depth-compression metadata read from the wrong stage).
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead);

    // 3. Write the per-instance TLAS records on the GPU.
    const uint32 numInstances = oc::min(m_meshInstanceCounter, m_maxGiTlasInstances);
    GIProbePipeline::TlasInstanceParams tlasParams{
        .renderNodeTransforms = frameData.inRenderNodeTransformsBuffer,
        .meshInstances = frameData.inMeshInstancesBuffer,
        .instanceOffsets = m_instanceOffsetsBuffer,
        .blasAddresses = m_accelStructure.getBlasAddressBuffer(frameIdx),
        .rtMeshAlias = m_accelStructure.getMeshAliasBuffer(),
        .materialInfos = m_materialInfosBuffer,
        .nodePassMasks = frameData.inNodePassMasksBuffer,
        .viewPos = m_cameraPos,
        .numInstances = numInstances,
    };
    m_giProbePipeline.recordTlasInstances(globalIllumCommandBuffer, frameIdx, tlasParams);

    // instance write -> TLAS build read. The build reads the instance buffer as SHADER_READ (AS_READ
    // covers the source acceleration structures, not the instance data), so the dst access must include it.
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead);

    // 4. Rebuild this frame's TLAS (double-buffered). Note if the handle changed so the recorded-once AO pass
    // (which bakes the handle) can be re-recorded.
    const bool tlasHandleChanged = m_accelStructure.recordBuildTlas(vkGlobalIllumCommandBuffer, frameIdx, m_giProbePipeline.getTlasInstanceBuffer(frameIdx), numInstances);

    // TLAS build -> ray-query read (GI/AO compute, and the forward fragment pass for RT light shadows)
    fullBarrier(vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderStorageRead);

    // 5. Trace rays per clipmap probe and temporally blend irradiance into the SH. The probe set and
    // its toroidal window are derived from the camera (this frame's u_viewPos in the UBO); probes that
    // scrolled in since last frame (relative to m_giPrevCameraPos) are full-replaced rather than blended.
    // Gated by the GI toggle — the TLAS built above still serves RTAO and RT shadows when GI is off.
    if (m_rtParams.giEnabled)
    {
    GIProbePipeline::TraceParams traceParams{
        .ubo = frameData.ubo,
        .lightInfos = frameData.lightInfosBuffer,
        .lightGrid = frameData.lightGridsBuffer,
        .lightTable = frameData.lightTableBuffer,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .meshInfos = m_meshInfosBuffer,
        .meshInstances = frameData.inMeshInstancesBuffer,
        .materialInfos = m_materialInfosBuffer,
        .tlas = m_accelStructure.getTlas(frameIdx),
        .shadowMapView = frameData.shadowMap.getSampleView(),
        .shadowMapSampler = frameData.shadowMap.getSampler(),
        .frameIndex = m_frameCounter,
        .prevViewPos = m_giPrevCameraPos,
    };
    m_giProbePipeline.recordTrace(globalIllumCommandBuffer, frameIdx, traceParams);
    m_giPrevCameraPos = m_cameraPos;

    // trace (SH write) -> fragment read in the main pass + vertex read (per-particle lighting)
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eVertexShader, vk::AccessFlagBits2::eShaderStorageRead);
    }

    globalIllumCommandBuffer.end();
    return tlasHandleChanged;
}

void Renderer::applyPendingTextureDescriptorWrites(uint32 frameIdx)
{
    if (Globals::textureStreamer.debugRewriteAllSlots())
        for (uint16 i = 0; i < (uint16)Globals::textureManager.getNumTextures(); ++i)
            Globals::textureStreamer.queueDescriptorWrite(i);

    oc::span<const uint16> writes = Globals::textureStreamer.getPendingDescriptorWrites(frameIdx);
    if (writes.empty())
        return;
    PerFrameData& frameData = m_perFrameData[frameIdx];
    for (uint16 texIdx : writes)
    {
        // Slots beyond the live descriptor count appear when a texture upload outgrew the arrays this
        // frame; the pending capacity growth re-allocates + fully refills every set, so drop them.
        if (texIdx >= m_numTextureDescriptors || texIdx >= Globals::textureManager.getNumTextures())
            continue;
        // Freed slots (destroyed container, not yet recycled) resolve to the fallback view.
        const vk::ImageView view = Globals::textureManager.getViewForDescriptor(texIdx);
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
        {
            m_staticMeshGraphicsPipeline.updateTextureDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(), texIdx, view);
            m_gbufferPipeline.updateTextureDescriptor(frameData.gbufferDescriptorSet[eye].getDescriptorSet(), texIdx, view);
        }
        m_shadowMapGraphicsPipeline.updateTextureDescriptor(frameData.shadowDrawDescriptorSet.getDescriptorSet(), texIdx, view);
        m_giProbePipeline.updateTextureDescriptor(frameIdx, texIdx, view);
        m_rtaoPipeline.updateTextureDescriptor(frameIdx, texIdx, view);
        m_particlePipeline.updateTextureDescriptor(frameIdx, texIdx, view);
        m_decalPipeline.updateTextureDescriptor(frameIdx, texIdx, view);
    }
    Globals::textureStreamer.clearPendingDescriptorWrites(frameIdx);
}

void Renderer::recordCommandBuffers()
{
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    PerFrameData& frameData = m_perFrameData[frameIdx];

    // Streamed textures: refresh swapped bindless slots in this frame slot's sets before anything
    // records against them (safe here: acquireNextImage waited this slot's fence, and the texture
    // arrays are UPDATE_AFTER_BIND for the cached CBs).
    applyPendingTextureDescriptorWrites(frameIdx);

    // Baked terrain-data cascades: point this slot's sets at the active ping-pong image
    // (UPDATE_AFTER_BIND, like the AO/TLAS bindings — a re-bake swaps images without re-recording
    // anything). The ocean passes read them for water depth/level (shoaling, surf, swash, land cull).
    for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
    {
        m_staticMeshGraphicsPipeline.updateTerrainHeightDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(),
            m_fogTerrainMap.getView(), m_fogTerrainMap.getSampler());
        m_gbufferPipeline.updateTerrainHeightDescriptor(frameData.gbufferDescriptorSet[eye].getDescriptorSet(),
            m_fogTerrainMap.getView(), m_fogTerrainMap.getSampler());
    }
    m_volumetricFogPipeline.updateTerrainDescriptor(frameIdx, m_fogTerrainMap.getView(), m_fogTerrainMap.getSampler());

    const bool recordScene = !frameData.updated && m_meshInstanceCounter > 0;
    if (recordScene)
    {
        // Recorded even with no jobs: the dispatch is indirect (CPU-written dims per frame), so skinned
        // instances spawned later run without a re-record.
        recordSkinning(frameIdx);
        recordOceanSim(frameIdx); // executed only while an ocean is active (m_oceanParams.enabled)
        recordIndirectCull(frameIdx);
        recordLightGrid(frameIdx);
        recordForceCompute(frameIdx); // indirect dispatches: emitter/query changes never re-record
        recordParticleSim(frameIdx); // indirect dispatches: emitter/spawn changes never re-record
        recordShadowCull(frameIdx);
        recordShadowDraw(frameIdx);
        recordVolumetricFog(frameIdx); // shared scatter/integrate (center view in VR)
        recordEyeAdaptation(frameIdx); // shared (samples the left eye's resolved colour in VR)
        // Composite secondary draws into the desktop swapchain (the left eye / TAA-resolved colour); in VR
        // it's the desktop-window mirror, so it's recorded in both modes.
        recordComposite(frameIdx);
        // The remaining per-eye screen-space passes (gbuffer, AO, forward, fog apply, TAA) are recorded
        // inline in the primary below in VR; on desktop they stay cached secondaries (one eye).
        if (m_sceneViewCount == 1)
        {
            recordStaticMesh(frameIdx);
            recordGBuffer(frameIdx);
            recordGiProbeDebug(frameIdx);
            if (m_debugLinePipeline.hasBuffers())
                recordDebugLines(frameIdx);
            recordDecals(frameIdx);
            recordForceField(frameIdx);
            recordParticles(frameIdx);
            recordAO(frameIdx);
            recordFogApply(frameIdx);
            if (m_taaParams.taaEnabled) // bypassed entirely when off — nothing to record or execute
                recordTaa(frameIdx);
        }
        frameData.updated = true;
    }

    if (m_meshInstanceCounter > 0 && recordGlobalIllum(frameIdx))
    {
        if (m_sceneViewCount == 1)
            recordAO(frameIdx);
        recordVolumetricFog(frameIdx);
    }

    // Live tunables + delta time into the mapped params buffer (no command-buffer re-record needed).
    if (m_meshInstanceCounter > 0)
    {
        static Clock::time_point lastTime = Clock::now();
        const Clock::time_point now = Clock::now();
        const float deltaSeconds = oc::min(std::chrono::duration<float>(now - lastTime).count(), 0.25f);
        lastTime = now;
        m_eyeAdaptationPipeline.updateParams(frameIdx, m_postParams, deltaSeconds);
    }

    vk::CommandBuffer vkIndirectCullCommandBuffer = frameData.indirectCullCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkSkinningCommandBuffer = frameData.skinningCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkOceanSimCommandBuffer = frameData.oceanSimCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkLightGridCommandBuffer = frameData.lightGridCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkStaticMeshCommandBuffer = frameData.staticMeshCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkGbufferCommandBuffer = frameData.gbufferCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkShadowCullCommandBuffer = frameData.shadowCullCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkShadowDrawCommandBuffer = frameData.shadowDrawCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkGlobalIllumCommandBuffer = frameData.globalIllumCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkAoCommandBuffer = frameData.aoCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkVolumetricFogCommandBuffer = frameData.volumetricFogCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkFogApplyCommandBuffer = frameData.fogApplyCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkGiProbeDebugCommandBuffer = frameData.giProbeDebugCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkTaaCommandBuffer = frameData.taaCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkEyeAdaptCommandBuffer = frameData.eyeAdaptCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkCompositeCommandBuffer = frameData.compositeCommandBuffer.getCommandBuffer();

    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = m_renderPass.getRenderPass() };
    vk::CommandBuffer vkImguiCommandBuffer = frameData.imguiCommandBuffer.begin(true, &inheritance);
    // From the UI's deep-copied snapshot, never ImGui::GetDrawData(): the next widget pass (a job)
    // calls ImGui::NewFrame mid-frame, which invalidates the live draw lists. Null until the first
    // UI::render (frame 0) - the empty secondary CB still executes fine.
    if (m_imguiDrawData)
        ImGui_ImplVulkan_RenderDrawData(static_cast<ImDrawData*>(const_cast<void*>(m_imguiDrawData)), vkImguiCommandBuffer, nullptr);
    frameData.imguiCommandBuffer.end();

    CommandBuffer& commandBuffer = frameData.primaryCommandBuffer;
    vk::CommandBuffer vkCommandBuffer = commandBuffer.begin(true);
    // GPU pass timings: timestamps live in the primary (re-recorded every frame) and OUTSIDE render
    // passes only; results are collected in beginFrame when this slot's fence is next waited.
    m_gpuProfiler.beginRecord(vkCommandBuffer, frameIdx);
    m_gpuProfiler.beginScope(vkCommandBuffer, "GPU Frame");
    // Pending baked-map uploads (fog terrain cascades): copied here in the primary (re-recorded
    // every frame) because the destination ping-pong images were sampled by older submissions — the
    // transitions need an execution dependency on those reads, which the StagingManager's fresh-image
    // upload path doesn't emit.
    m_fogTerrainMap.recordUpload(commandBuffer);
    { // Sync for ubo copy
        vk::MemoryBarrier2 memoryBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eUniformRead,
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
    }

    if (m_meshInstanceCounter > 0)
    {
        // Skin first: deforms skinned meshes into their output vertex regions, which the cull / G-buffer /
        // forward / shadow passes then consume as ordinary static geometry.
        if (!m_skinningJobs.empty())
        {
            m_gpuProfiler.beginScope(vkCommandBuffer, "Skinning");
            vkCommandBuffer.executeCommands(1, &vkSkinningCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
        // FFT ocean simulation (spectrum -> IFFT -> maps + mips); the G-buffer/forward vertex shaders and
        // the ocean fragment shader sample the maps. Skipped entirely while no ocean is active (the maps
        // rest in SHADER_READ_ONLY, so the samplers stay valid).
        if (m_oceanParams.enabled)
        {
            m_gpuProfiler.beginScope(vkCommandBuffer, "Ocean sim");
            vkCommandBuffer.executeCommands(1, &vkOceanSimCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
        m_gpuProfiler.beginScope(vkCommandBuffer, "Indirect cull");
        vkCommandBuffer.executeCommands(1, &vkIndirectCullCommandBuffer);
        m_gpuProfiler.endScope(vkCommandBuffer);
        m_gpuProfiler.beginScope(vkCommandBuffer, "Light grid");
        vkCommandBuffer.executeCommands(1, &vkLightGridCommandBuffer);
        m_gpuProfiler.endScope(vkCommandBuffer);
        // Forcefield grid build + force/query compute (Force library readbacks land ~2 frames later).
        if (m_forceFieldParams.enabled)
        {
            vk::CommandBuffer vkForceComputeCommandBuffer = frameData.forceComputeCommandBuffer.getCommandBuffer();
            m_gpuProfiler.beginScope(vkCommandBuffer, "Force compute");
            vkCommandBuffer.executeCommands(1, &vkForceComputeCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
        // Particle emit/simulate (outside any render pass; reads LAST frame's G-buffer for collision,
        // writes the alive list + indirect draw args the in-pass billboard draw consumes).
        if (m_particlesEnabled)
        {
            vk::CommandBuffer vkParticleSimCommandBuffer = frameData.particleSimCommandBuffer.getCommandBuffer();
            m_gpuProfiler.beginScope(vkCommandBuffer, "Particle sim");
            vkCommandBuffer.executeCommands(1, &vkParticleSimCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
        // RT sun shadows replace the cascades entirely (forward pass traces, GI uses per-probe sun rays),
        // so skip the shadow cull + cascade render. The primary CB is re-recorded every frame, so the
        // toggle takes effect immediately; the cached secondary CBs just go unexecuted.
        if (!m_rtParams.rtSunShadow)
        {
            m_gpuProfiler.beginScope(vkCommandBuffer, "Shadow cull");
            vkCommandBuffer.executeCommands(1, &vkShadowCullCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
            m_gpuProfiler.beginScope(vkCommandBuffer, "Shadow draw");

            ShadowMap& shadowMap = frameData.shadowMap;
            vk::ClearValue shadowClear;
            shadowClear.depthStencil = vk::ClearDepthStencilValue{ .depth = 1.0f, .stencil = 0 };
            const vk::RenderPassBeginInfo shadowRpBegin{
                .renderPass = shadowMap.getRenderPass(),
                .framebuffer = shadowMap.getFramebuffer(),
                .renderArea = vk::Rect2D{.offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ shadowMap.getResolution(), shadowMap.getResolution() } },
                .clearValueCount = 1,
                .pClearValues = &shadowClear,
            };
            vkCommandBuffer.beginRenderPass(shadowRpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
            vkCommandBuffer.executeCommands(1, &vkShadowDrawCommandBuffer);
            vkCommandBuffer.endRenderPass();
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
        SceneColor& sceneColor = frameData.sceneColor;
        GBuffer& gbuffer = frameData.gbuffer;
        // Reversed-Z: the far plane / "no geometry" depth is 0.0 (shadow maps stay standard, cleared 1.0).
        oc::array<vk::ClearValue, 2> gbufferClears{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };
        const vk::Rect2D gbufferArea{ .offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ gbuffer.getWidth(), gbuffer.getHeight() } };
        oc::array<vk::ClearValue, 2> sceneClears{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };
        const vk::Rect2D sceneArea{ .offset = vk::Offset2D{ m_viewportRect.min.x, m_viewportRect.min.y }, .extent = vk::Extent2D{ sceneColor.getWidth() - m_viewportRect.min.x, sceneColor.getHeight() - m_viewportRect.min.y } };
        const vk::AccelerationStructureKHR tlas = m_accelStructure.getTlas(frameIdx);

        if (m_sceneViewCount > 1)
        {
            // ---- VR: per-eye screen-space chain (gbuffer -> AO -> forward+fog -> TAA), recorded inline ----
            // GI (TLAS build + probe trace) and fog scatter/integrate are shared (built once for the centre
            // view); each eye's gbuffer/AO/forward/TAA then runs against its own images.
            m_gpuProfiler.beginScope(vkCommandBuffer, "GI");
            vkCommandBuffer.executeCommands(1, &vkGlobalIllumCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
            if (m_fogParams.enabled)
            {
                m_gpuProfiler.beginScope(vkCommandBuffer, "Volumetric fog");
                vkCommandBuffer.executeCommands(1, &vkVolumetricFogCommandBuffer);
                m_gpuProfiler.endScope(vkCommandBuffer);
            }

            for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
            {
                m_gpuProfiler.beginScope(vkCommandBuffer, eye == 0 ? "Eye L" : "Eye R");
                { // G-buffer prepass for this eye (layer eye)
                    const vk::RenderPassBeginInfo gbufferRpBegin{
                        .renderPass = gbuffer.getRenderPass(),
                        .framebuffer = gbuffer.getFramebuffer(eye),
                        .renderArea = gbufferArea,
                        .clearValueCount = (uint32)gbufferClears.size(),
                        .pClearValues = gbufferClears.data(),
                    };
                    vkCommandBuffer.beginRenderPass(gbufferRpBegin, vk::SubpassContents::eInline);
                    if (m_meshInfoCounter > 0)
                        recordGBufferInto(commandBuffer, frameIdx, eye);
                    vkCommandBuffer.endRenderPass();
                }
                if (m_rtaoParams.enabled)
                    recordAOInto(commandBuffer, frameIdx, eye); // compute AO for this eye

                // This eye's forward descriptor set takes this eye's AO + the (per-frame) TLAS.
                m_staticMeshGraphicsPipeline.updateAODescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(), m_rtaoPipeline.getAOView(frameIdx, eye), m_rtaoPipeline.getAOSampler());
                if (tlas)
                    m_staticMeshGraphicsPipeline.updateTlasDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(), tlas);

                { // Forward (+ fog apply) into this eye's SceneColor layer; depth = prepass depth read-only when reusing
                    if (m_depthPrepassReuse)
                        recordReuseDepthBarrier(vkCommandBuffer, gbuffer.getDepthImage(), eye, true);
                    const vk::RenderPassBeginInfo eyeRpBegin{
                        .renderPass = m_depthPrepassReuse ? sceneColor.getReuseRenderPass() : sceneColor.getRenderPass(),
                        .framebuffer = m_depthPrepassReuse ? sceneColor.getReuseFramebuffer(eye) : sceneColor.getFramebuffer(eye),
                        .renderArea = sceneArea,
                        .clearValueCount = (uint32)sceneClears.size(),
                        .pClearValues = sceneClears.data(),
                    };
                    vkCommandBuffer.beginRenderPass(eyeRpBegin, vk::SubpassContents::eInline);
                    recordStaticMeshInto(commandBuffer, frameIdx, eye);
                    if (m_decalsEnabled)
                        recordDecalsInto(commandBuffer, frameIdx, eye);
                    if (m_forceFieldParams.enabled)
                        recordForceFieldInto(commandBuffer, frameIdx, eye);
                    if (m_particlesEnabled)
                        recordParticlesInto(commandBuffer, frameIdx, eye);
                    if (m_fogParams.enabled)
                        recordFogApplyInto(commandBuffer, frameIdx, eye);
                    vkCommandBuffer.endRenderPass();
                    if (m_depthPrepassReuse) // this eye's prepass depth back to sampled (TAA next)
                        recordReuseDepthBarrier(vkCommandBuffer, gbuffer.getDepthImage(), eye, false);
                }

                // Scene colour -> TAA compute sampled read. Explicit image barrier (not a global memory barrier -
                // see the desktop path below) naming this eye's colour layer so the finalLayout transition at
                // endRenderPass is actually resolved for the compute read.
                vk::ImageMemoryBarrier2 colorToTaa{
                    .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                    .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .image = sceneColor.getColorImage(),
                    .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, eye, 1 },
                };
                vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &colorToTaa });
                recordTaaInto(commandBuffer, frameIdx, eye); // resolve into this eye's history
                m_gpuProfiler.endScope(vkCommandBuffer);
            }

            // Eye adaptation samples the left eye's resolved colour (shared exposure, no per-eye flicker).
            m_gpuProfiler.beginScope(vkCommandBuffer, "Eye adaptation");
            vkCommandBuffer.executeCommands(1, &vkEyeAdaptCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
            m_gpuProfiler.beginScope(vkCommandBuffer, "VR composite");

            // Tonemap each eye's TAA-resolved colour into its LDR composite target (copied into the OpenXR
            // eye swapchains in present()). TAA left the resolved images in GENERAL with a write->read barrier.
            const vk::Extent2D ext = m_swapChain.getLayout().extent;
            for (uint32 eye = 0; eye < 2; ++eye)
            {
                oc::array<vk::ClearValue, 2> eyeClears{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };
                const vk::RenderPassBeginInfo eyeCompositeBegin{
                    .renderPass = m_renderPass.getRenderPass(),
                    .framebuffer = m_eyeFramebuffer[eye],
                    .renderArea = vk::Rect2D{ .offset = vk::Offset2D{ 0, 0 }, .extent = ext },
                    .clearValueCount = (uint32)eyeClears.size(),
                    .pClearValues = eyeClears.data(),
                };
                vkCommandBuffer.beginRenderPass(eyeCompositeBegin, vk::SubpassContents::eInline);
                vkCommandBuffer.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)ext.width, .height = (float)ext.height, .minDepth = 0.0f, .maxDepth = 1.0f });
                vkCommandBuffer.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ 0, 0 }, .extent = ext });
                CompositePipeline::RecordParams eyeComposite{
                    .descriptorSet = m_vrCompositeDescriptorSet[eye],
                    .resolvedView = m_taaParams.taaEnabled ? m_taaPipeline.getResolvedView(frameIdx, eye)
                        : frameData.sceneColor.getColorLayerView(eye),
                    .resolvedLayout = m_taaParams.taaEnabled ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
                    .sampler = m_taaParams.taaEnabled ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
                    .exposureBuffer = m_eyeAdaptationPipeline.getExposureBuffer().getBuffer(),
                    .exposureEV = m_postParams.exposureEV,
                    .tonemapper = m_postParams.tonemapper,
                    .autoExposure = m_postParams.autoExposure ? 1 : 0,
                };
                m_compositePipeline.record(commandBuffer, eyeComposite);
                vkCommandBuffer.endRenderPass();
            }
            m_gpuProfiler.endScope(vkCommandBuffer); // VR composite
        }
        else
        {
            { // Depth + world-normal G-buffer prepass (camera view)
                m_gpuProfiler.beginScope(vkCommandBuffer, "G-buffer");
                const vk::RenderPassBeginInfo gbufferRpBegin{
                    .renderPass = gbuffer.getRenderPass(),
                    .framebuffer = gbuffer.getFramebuffer(),
                    .renderArea = gbufferArea,
                    .clearValueCount = (uint32)gbufferClears.size(),
                    .pClearValues = gbufferClears.data(),
                };
                vkCommandBuffer.beginRenderPass(gbufferRpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
                if (m_meshInfoCounter > 0)
                    vkCommandBuffer.executeCommands(1, &vkGbufferCommandBuffer);
                vkCommandBuffer.endRenderPass();
                m_gpuProfiler.endScope(vkCommandBuffer);
            }
            m_gpuProfiler.beginScope(vkCommandBuffer, "GI");
            vkCommandBuffer.executeCommands(1, &vkGlobalIllumCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
            if (m_rtaoParams.enabled)
            {
                m_gpuProfiler.beginScope(vkCommandBuffer, "RTAO");
                vkCommandBuffer.executeCommands(1, &vkAoCommandBuffer);
                m_gpuProfiler.endScope(vkCommandBuffer);
            }
            // Fog scatter/integrate compute; the primary is re-recorded every frame, so the enable toggle
            // takes effect immediately (the integrated grid was cleared to "no fog" at init when disabled).
            if (m_fogParams.enabled)
            {
                m_gpuProfiler.beginScope(vkCommandBuffer, "Volumetric fog");
                vkCommandBuffer.executeCommands(1, &vkVolumetricFogCommandBuffer);
                m_gpuProfiler.endScope(vkCommandBuffer);
            }
            m_staticMeshGraphicsPipeline.updateAODescriptor(frameData.staticMeshPipelineDescriptorSet[0].getDescriptorSet(), m_rtaoPipeline.getAOView(frameIdx, 0), m_rtaoPipeline.getAOSampler());
            if (tlas)
                m_staticMeshGraphicsPipeline.updateTlasDescriptor(frameData.staticMeshPipelineDescriptorSet[0].getDescriptorSet(), tlas);

            // Depth-prepass reuse: the scene pass binds the G-buffer depth READ-ONLY; the explicit
            // barriers do the sampled<->attachment layout round-trip. Off = own cleared depth, rebuilt.
            m_gpuProfiler.beginScope(vkCommandBuffer, "Scene forward");
            if (m_depthPrepassReuse)
                recordReuseDepthBarrier(vkCommandBuffer, gbuffer.getDepthImage(), 0, true);
            const vk::RenderPassBeginInfo sceneRpBegin{
                .renderPass = m_depthPrepassReuse ? sceneColor.getReuseRenderPass() : sceneColor.getRenderPass(),
                .framebuffer = m_depthPrepassReuse ? sceneColor.getReuseFramebuffer(0) : sceneColor.getFramebuffer(),
                .renderArea = sceneArea,
                .clearValueCount = (uint32)sceneClears.size(),
                .pClearValues = sceneClears.data(),
            };
            vkCommandBuffer.beginRenderPass(sceneRpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
            vkCommandBuffer.executeCommands(1, &vkStaticMeshCommandBuffer);
            if (m_decalsEnabled) // right after opaque: particles/debug/fog layer on top
            {
                vk::CommandBuffer vkDecalCommandBuffer = frameData.decalCommandBuffer.getCommandBuffer();
                vkCommandBuffer.executeCommands(1, &vkDecalCommandBuffer);
            }
            if (m_giProbeDebugEnabled)
                vkCommandBuffer.executeCommands(1, &vkGiProbeDebugCommandBuffer);
            if (m_debugLinePipeline.hasBuffers())
            {
                vk::CommandBuffer vkDebugLineCommandBuffer = frameData.debugLineCommandBuffer.getCommandBuffer();
                vkCommandBuffer.executeCommands(1, &vkDebugLineCommandBuffer);
            }
            if (m_forceFieldParams.enabled) // after debug lines, before particles/fog (those layer on top)
            {
                vk::CommandBuffer vkForceFieldCommandBuffer = frameData.forceFieldCommandBuffer.getCommandBuffer();
                vkCommandBuffer.executeCommands(1, &vkForceFieldCommandBuffer);
            }
            if (m_particlesEnabled)
            {
                vk::CommandBuffer vkParticleCommandBuffer = frameData.particleCommandBuffer.getCommandBuffer();
                vkCommandBuffer.executeCommands(1, &vkParticleCommandBuffer);
            }
            if (m_fogParams.enabled)
                vkCommandBuffer.executeCommands(1, &vkFogApplyCommandBuffer);
            vkCommandBuffer.endRenderPass();
            if (m_depthPrepassReuse) // prepass depth back to sampled for TAA/fog/next-frame consumers
                recordReuseDepthBarrier(vkCommandBuffer, gbuffer.getDepthImage(), 0, false);
            m_gpuProfiler.endScope(vkCommandBuffer); // Scene forward

            // SceneColor's render pass has no 0->EXTERNAL dependency of its own (must stay dependency-identical
            // to the swapchain pass, see SceneColor.cpp), so its finalLayout->SHADER_READ_ONLY transition at
            // endRenderPass is only ordered by the implicit (no-access) end dependency. An explicit image
            // barrier naming the colour image is what actually resolves that transition for TAA's compute read
            // (a global vk::MemoryBarrier2 was insufficient - validation still saw it as an unsynchronized
            // layout-transition read). Same-layout SHADER_READ_ONLY->SHADER_READ_ONLY, sync-only.
            vk::ImageMemoryBarrier2 colorToTaaImg{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = sceneColor.getColorImage(),
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            };
            // TAA OFF: eye adaptation (compute) and the composite (fragment) sample this image
            // instead of TAA's resolved one, so the read must be visible to both stages.
            if (!m_taaParams.taaEnabled)
                colorToTaaImg.dstStageMask |= vk::PipelineStageFlagBits2::eFragmentShader;
            vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &colorToTaaImg });
            // Disabled TAA used to still run the full-screen resolve with feedback 0 (a visible "TAA"
            // scope in the profiler for a pass that only copied) — now the dispatch is skipped outright.
            if (m_taaParams.taaEnabled)
            {
                m_gpuProfiler.beginScope(vkCommandBuffer, "TAA");
                vkCommandBuffer.executeCommands(1, &vkTaaCommandBuffer);
                m_gpuProfiler.endScope(vkCommandBuffer);
            }
            // Eye adaptation: reads the resolved colour (TAA barrier above), writes the exposure the composite reads.
            m_gpuProfiler.beginScope(vkCommandBuffer, "Eye adaptation");
            vkCommandBuffer.executeCommands(1, &vkEyeAdaptCommandBuffer);
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
    }

    // Swapchain render pass: composite the resolved scene into the swapchain, then ImGui on top.
    constexpr oc::array<vk::ClearValue, 2> clearValues{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };
    const vk::RenderPassBeginInfo renderPassBeginInfo{
        .renderPass = m_renderPass.getRenderPass(),
        .framebuffer = m_framebuffers.getFramebuffer(m_swapChain.getCurrentImageIdx()),
        .renderArea = vk::Rect2D { .offset = vk::Offset2D { 0, 0 }, .extent = m_swapChain.getLayout().extent },
        .clearValueCount = (uint32)clearValues.size(),
        .pClearValues = clearValues.data(),
    };
    m_gpuProfiler.beginScope(vkCommandBuffer, "Composite + UI");
    vkCommandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eSecondaryCommandBuffers);
    if (m_meshInstanceCounter > 0)
        vkCommandBuffer.executeCommands(1, &vkCompositeCommandBuffer);
    vkCommandBuffer.executeCommands(1, &vkImguiCommandBuffer);
    vkCommandBuffer.endRenderPass();
    m_gpuProfiler.endScope(vkCommandBuffer);
    m_gpuProfiler.endScope(vkCommandBuffer); // GPU Frame
    commandBuffer.end();
}

void Renderer::setHaveToRecordCommandBuffers()
{
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.updated = false;
}

void Renderer::syncTextureDescriptorCapacity()
{
    // The live texture count outgrew the variable-count texture-array descriptors.
    if (Globals::textureManager.getGeneration() == m_textureGeneration)
        return;
    m_textureGeneration = Globals::textureManager.getGeneration();
    m_numTextureDescriptors = Globals::textureManager.getMaxTextures();
    waitForGpuAndFlushStaging();
    m_giProbePipeline.resizeTextureDescriptors(m_numTextureDescriptors);
    m_rtaoPipeline.resizeTextureDescriptors(m_numTextureDescriptors);
    m_particlePipeline.resizeTextureDescriptors(m_numTextureDescriptors);
    m_decalPipeline.resizeTextureDescriptors(m_numTextureDescriptors);
    for (PerFrameData& perFrame : m_perFrameData)
    {
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
        {
            perFrame.staticMeshPipelineDescriptorSet[eye].initialize(m_staticMeshGraphicsPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);
            perFrame.gbufferDescriptorSet[eye].initialize(m_gbufferPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);
        }
        perFrame.shadowDrawDescriptorSet.initialize(m_shadowMapGraphicsPipeline.getDescriptorSetLayout(), m_numTextureDescriptors);
    }
    setHaveToRecordCommandBuffers();
}

void Renderer::addObjectContainer(ObjectContainer* pObjectContainer)
{
    m_objectContainers.push_back(pObjectContainer);
}

void Renderer::removeObjectContainer(ObjectContainer* pObjectContainer)
{
    oc::erase(m_objectContainers, pObjectContainer);
    ObjectContainer& container = *pObjectContainer;

    // Parked skinned bundles first (they free MeshInfos/output regions that reference the sources).
    // Every live RenderNode must have been destroyed before the container, so all of its bundles are
    // parked in m_freeSkinnedBundles by now.
    if (container.m_baseSkinnedMeshIdx != UINT32_MAX)
    {
        if (auto it = m_freeSkinnedBundles.find(container.m_baseSkinnedMeshIdx); it != m_freeSkinnedBundles.end())
        {
            for (const uint32 bundleHandle : it->second)
                destroySkinnedBundle(bundleHandle);
            m_freeSkinnedBundles.erase(it);
        }
        m_freeSkinnedSourceSlots.release(container.m_baseSkinnedMeshIdx, container.m_numSkinnedMeshes);
    }

    // Streamed mesh sets own their CURRENT mega-buffer ranges (re-streams relocate them); everything
    // the container uploaded outside a stream set is freed from its own records.
    if (container.m_numMeshInfos > 0)
        Globals::meshStreamer.unregisterSets(container.m_baseMeshInfoIdx, container.m_numMeshInfos);
    for (const ObjectContainer::OwnedDataRange& range : container.m_ownedDataRanges)
    {
        switch (range.kind)
        {
        case ObjectContainer::EOwnedRange::Vertex:   Globals::meshDataManager.freeVertexData(range.offset, range.size); break;
        case ObjectContainer::EOwnedRange::Index:    Globals::meshDataManager.freeIndexData(range.offset, range.size); break;
        case ObjectContainer::EOwnedRange::Skinning: Globals::meshDataManager.freeSkinningData(range.offset, range.size); break;
        }
    }

    freeMeshInfoRange(container.m_baseMeshInfoIdx, container.m_numMeshInfos);
    m_freeMaterialSlots.release(container.m_baseMaterialInfoIdx, (uint32)container.m_materialNames.size());

    m_freeInstanceOffsetSlots.release(container.m_baseMeshInstanceOffsetsIdx, (uint32)container.m_meshInstanceOffsets.size());
    for (uint32 i = 0; i < (uint32)container.m_rebasedOffsetBaseForIdx.size(); ++i)
        if (container.m_rebasedOffsetBaseForIdx[i] != UINT32_MAX)
            m_freeInstanceOffsetSlots.release(container.m_rebasedOffsetBaseForIdx[i], container.m_nodeMeshRanges[i].numNodes);
    if (container.m_skinnedIdentityOffsetIdx != UINT32_MAX)
        m_freeInstanceOffsetSlots.release(container.m_skinnedIdentityOffsetIdx, 1);

    for (const uint32 groupIdx : container.m_ownedLodGroups)
        freeMeshLodGroup(groupIdx); // also detaches the member meshes from GPU LOD selection

    // The images may still be sampled by in-flight frames: destroyed in present() once the GPU has
    // drained, their bindless slots rewritten to the fallback at the next record.
    m_pendingTextureFrees.insert(m_pendingTextureFrees.end(), container.m_ownedTextures.begin(), container.m_ownedTextures.end());
    // Freed mega-buffer/slot ranges may be recycled by an upload later this frame; uploadToSharedBuffer
    // drains the GPU itself before queuing, so no extra synchronization is needed here for that.
}

void Renderer::freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count)
{
    if (count == 0)
        return;
    // Neutralize the slots: zero indexCount makes the cull's DGC draws, the shadow pass and the TLAS
    // writer no-ops for any instance still referencing them this frame (a node pushed before the
    // container died), exactly like streamed-out meshes.
    const oc::span<RendererVKLayout::MeshInfo> infos = m_meshInfosBuffer.getBackingStoreAs<RendererVKLayout::MeshInfo>();
    for (uint32 i = baseMeshInfoIdx; i < baseMeshInfoIdx + count; ++i)
    {
        infos[i] = RendererVKLayout::MeshInfo{};
        m_meshVertexCounts[i] = 0;
        m_meshIsSkinnedOutput[i] = 0;
    }
    uploadToSharedBuffer(m_meshInfosBuffer, count * sizeof(RendererVKLayout::MeshInfo), infos.data() + baseMeshInfoIdx,
        (size_t)baseMeshInfoIdx * sizeof(RendererVKLayout::MeshInfo));
    m_accelStructure.onMeshRangeFreed(baseMeshInfoIdx, count);
    oc::erase_if(m_pendingBlasRebuilds, [&](uint32 meshIdx) { return meshIdx >= baseMeshInfoIdx && meshIdx < baseMeshInfoIdx + count; });
    m_freeMeshInfoSlots.release(baseMeshInfoIdx, count);
}

void Renderer::destroySkinnedBundle(uint32 bundleHandle)
{
    SkinnedInstanceBundle& bundle = m_skinnedBundles[bundleHandle];
    uint32 numMeshInfos = bundle.numMeshes; // level 0s + the LOD levels appended after them
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        const RendererVKLayout::SkinnedMeshSource& src = m_skinnedMeshSources[bundle.sourceKey + k];
        numMeshInfos += src.numLodLevels;
        // The job entry is parked (vertexCount 0) but keeps its output offset for exactly this purpose.
        Globals::meshDataManager.freeVertexData(
            (size_t)m_skinningJobs[bundle.firstJob + k].outVertexOffset * sizeof(RendererVKLayout::MeshVertex),
            (size_t)src.vertexCount * sizeof(RendererVKLayout::MeshVertex));
    }
    freeMeshInfoRange(bundle.baseMeshIdx, numMeshInfos);
    m_accelStructure.freeSkinnedJobSlots(bundle.firstJob, bundle.numMeshes);
    m_freeSkinningJobSlots.release(bundle.firstJob, bundle.numMeshes);
    m_freeSkinningPaletteHandles.push_back(bundle.paletteHandle);
    for (const uint32 groupIdx : bundle.lodGroupForMesh)
        if (groupIdx != UINT32_MAX)
            freeMeshLodGroup(groupIdx); // also detaches the member meshes from GPU LOD selection
    bundle = SkinnedInstanceBundle{ .sourceKey = UINT32_MAX, .baseMeshIdx = 0, .paletteHandle = 0, .firstJob = 0, .numMeshes = 0 };
    m_freeSkinnedBundleSlots.push_back(bundleHandle);
}

void Renderer::processPendingTextureFrees()
{
    for (const uint16 texIdx : m_pendingTextureFrees)
        Globals::textureManager.free(texIdx);
    m_pendingTextureFrees.clear();
}

void Renderer::uploadToSharedBuffer(Buffer& buffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset)
{
    Globals::stagingManager.ensureDrainedForSharedWrite();
    buffer.upload(dataSize, data, dstOffset);
}

void Renderer::setMeshStreamedOut(uint16 meshInfoIdx)
{
    RendererVKLayout::MeshInfo& info = m_meshInfosBuffer.getBackingStoreAs<RendererVKLayout::MeshInfo>()[meshInfoIdx];
    info.indexCount = 0;
    uploadToSharedBuffer(m_meshInfosBuffer, sizeof(info), &info, (size_t)meshInfoIdx * sizeof(info));
    // The BLAS goes with the mesh data (rebuilt on re-stream); safe because eviction requires the set
    // to have been unreferenced for far longer than any in-flight TLAS.
    m_accelStructure.onMeshEvicted(meshInfoIdx);
}

void Renderer::setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount)
{
    RendererVKLayout::MeshInfo& info = m_meshInfosBuffer.getBackingStoreAs<RendererVKLayout::MeshInfo>()[meshInfoIdx];
    info.vertexOffset = vertexOffset;
    info.firstIndex = firstIndex;
    info.indexCount = indexCount;
    uploadToSharedBuffer(m_meshInfosBuffer, sizeof(info), &info, (size_t)meshInfoIdx * sizeof(info));
    if (m_accelStructure.getMeshAlias(meshInfoIdx) == meshInfoIdx)
        m_pendingBlasRebuilds.push_back(meshInfoIdx); // built by the next recordGlobalIllum
}

uint32 Renderer::addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs)
{
    assert(vertexCounts.size() == meshInfos.size() && "one exact vertex count per MeshInfo");
    if (meshInfos.empty())
        return m_meshInfoCounter;

    // Reuse a range freed by a destroyed container before growing the counter (holes are never compacted).
    if (const uint32 reusedBase = m_freeMeshInfoSlots.allocate((uint32)meshInfos.size()); reusedBase != UINT32_MAX)
    {
        const oc::span<RendererVKLayout::MeshInfo> infos = m_meshInfosBuffer.getBackingStoreAs<RendererVKLayout::MeshInfo>();
        memcpy(infos.data() + reusedBase, meshInfos.data(), meshInfos.size() * sizeof(RendererVKLayout::MeshInfo));
        for (uint32 i = 0; i < (uint32)meshInfos.size(); ++i)
        {
            m_meshVertexCounts[reusedBase + i] = vertexCounts[i];
            m_meshIsSkinnedOutput[reusedBase + i] = skinnedOutputs ? 1 : 0;
            // Slots below the one-time build watermark won't be caught up by recordGlobalIllum's
            // counter scan; queue their BLAS builds explicitly (aliases are identity, reset at free).
            if (!skinnedOutputs && reusedBase + i < m_blasBuiltCount)
                m_pendingBlasRebuilds.push_back(reusedBase + i);
        }
        uploadToSharedBuffer(m_meshInfosBuffer, meshInfos.size() * sizeof(RendererVKLayout::MeshInfo),
            meshInfos.data(), (size_t)reusedBase * sizeof(RendererVKLayout::MeshInfo));
        return reusedBase;
    }

    const uint32 baseMeshInfoIdx = m_meshInfoCounter;
    m_meshInfoCounter += (uint32)meshInfos.size();
    m_numInstancesPerMesh.resize(m_meshInfoCounter);
    m_meshVertexCounts.insert(m_meshVertexCounts.end(), vertexCounts.begin(), vertexCounts.end());
    m_meshIsSkinnedOutput.resize(m_meshInfoCounter, skinnedOutputs ? 1 : 0);
    m_meshToLodGroup.resize(m_meshInfoCounter, UINT32_MAX);
    assert(m_meshInfoCounter < USHRT_MAX);

    m_meshInfosBuffer.appendToBackingStore<RendererVKLayout::MeshInfo>(meshInfos);

    if (m_meshInfoCounter > m_maxUniqueMeshes)
        growUniqueMeshCapacity(m_meshInfoCounter); // re-uploads the full CPU copy; re-records
    // Within capacity no re-record is needed: nothing recorded bakes the mesh count (the cull clears fill
    // whole capacity-sized buffers, DGC reads the count via sequenceCountAddress, the G-buffer draws via
    // drawIndexedIndirectCount, and new BLASes build per frame in recordGlobalIllum).
    else
    {
        uploadToSharedBuffer(m_meshInfosBuffer, meshInfos.size() * sizeof(RendererVKLayout::MeshInfo),
            meshInfos.data(), baseMeshInfoIdx * sizeof(RendererVKLayout::MeshInfo));
        // Fresh mesh slots must read as "no LOD chain" on the GPU (device memory starts undefined;
        // addMeshLodGroup overwrites the chained ones right after).
        uploadToSharedBuffer(m_meshLodGroupIdxBuffer, meshInfos.size() * sizeof(uint32),
            m_meshToLodGroup.data() + baseMeshInfoIdx, (size_t)baseMeshInfoIdx * sizeof(uint32));
    }

    // After the capacity check: the alias buffer is grown by resizeBlasAddressBuffer inside
    // growUniqueMeshCapacity, and setNumMeshes writes identity entries for the new range.
    m_accelStructure.setNumMeshes(m_meshInfoCounter); // identity RT aliases until a LOD group overrides

    return baseMeshInfoIdx;
}

uint32 Renderer::addSkinnedMeshSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources)
{
    if (const uint32 reusedBase = m_freeSkinnedSourceSlots.allocate((uint32)sources.size()); reusedBase != UINT32_MAX)
    {
        oc::copy(sources.begin(), sources.end(), m_skinnedMeshSources.begin() + reusedBase);
        return reusedBase;
    }
    const uint32 baseIdx = (uint32)m_skinnedMeshSources.size();
    m_skinnedMeshSources.insert(m_skinnedMeshSources.end(), sources.begin(), sources.end());
    return baseIdx;
}

uint32 Renderer::addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos)
{
    if (materialInfos.empty())
        return m_materialInfoCounter;

    if (const uint32 reusedBase = m_freeMaterialSlots.allocate((uint32)materialInfos.size()); reusedBase != UINT32_MAX)
    {
        const oc::span<RendererVKLayout::MaterialInfo> materials = m_materialInfosBuffer.getBackingStoreAs<RendererVKLayout::MaterialInfo>();
        memcpy(materials.data() + reusedBase, materialInfos.data(), materialInfos.size() * sizeof(RendererVKLayout::MaterialInfo));
        uploadToSharedBuffer(m_materialInfosBuffer, materialInfos.size() * sizeof(RendererVKLayout::MaterialInfo),
            materialInfos.data(), (size_t)reusedBase * sizeof(RendererVKLayout::MaterialInfo));
        return reusedBase;
    }

    const uint32 baseMaterialInfoIdx = m_materialInfoCounter;
    m_materialInfoCounter += (uint32)materialInfos.size();
    assert(m_materialInfoCounter < USHRT_MAX);

    m_materialInfosBuffer.appendToBackingStore<RendererVKLayout::MaterialInfo>(materialInfos);

    if (m_materialInfoCounter > m_maxUniqueMaterials)
        growMaterialCapacity(m_materialInfoCounter); // re-uploads the full CPU copy; re-records
    // Within capacity this is a contents-only upload: every pass binds the whole buffer and nothing
    // recorded depends on the material count, so no re-record is needed.
    else
        uploadToSharedBuffer(m_materialInfosBuffer, materialInfos.size() * sizeof(RendererVKLayout::MaterialInfo),
            materialInfos.data(), baseMaterialInfoIdx * sizeof(RendererVKLayout::MaterialInfo));

    return baseMaterialInfoIdx;
}

uint32 Renderer::addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets)
{
    if (meshInstanceOffsets.empty())
        return m_instanceOffsetCounter;

    if (const uint32 reusedBase = m_freeInstanceOffsetSlots.allocate((uint32)meshInstanceOffsets.size()); reusedBase != UINT32_MAX)
    {
        const oc::span<RendererVKLayout::MeshInstanceOffset> offsets = m_instanceOffsetsBuffer.getBackingStoreAs<RendererVKLayout::MeshInstanceOffset>();
        memcpy(offsets.data() + reusedBase, meshInstanceOffsets.data(), meshInstanceOffsets.size() * sizeof(RendererVKLayout::MeshInstanceOffset));
        uploadToSharedBuffer(m_instanceOffsetsBuffer, meshInstanceOffsets.size() * sizeof(RendererVKLayout::MeshInstanceOffset),
            meshInstanceOffsets.data(), (size_t)reusedBase * sizeof(RendererVKLayout::MeshInstanceOffset));
        return reusedBase;
    }

    const uint32 baseInstanceOffsetIdx = m_instanceOffsetCounter;
    m_instanceOffsetCounter += (uint32)meshInstanceOffsets.size();

    m_instanceOffsetsBuffer.appendToBackingStore<RendererVKLayout::MeshInstanceOffset>(meshInstanceOffsets);

    if (m_instanceOffsetCounter > m_maxInstanceOffsets)
        growInstanceOffsetCapacity(m_instanceOffsetCounter); // re-uploads the full CPU copy; re-records
    // Within capacity this is a contents-only upload: every pass binds the whole buffer and nothing
    // recorded depends on the offset count, so no re-record is needed (rebased static spawns hit this).
    else
        uploadToSharedBuffer(m_instanceOffsetsBuffer, meshInstanceOffsets.size() * sizeof(RendererVKLayout::MeshInstanceOffset),
            meshInstanceOffsets.data(), baseInstanceOffsetIdx * sizeof(RendererVKLayout::MeshInstanceOffset));

    return baseInstanceOffsetIdx;
}

void Renderer::initImgui(Window& window)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGui_ImplSDL3_InitForVulkan((SDL_Window*)window.getWindowHandle());
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

    stats.numLights = m_lightCounter;
    stats.maxLights = RendererVKLayout::MAX_LIGHTS;

    stats.numMeshInstances = m_meshInstanceCounter;
    stats.maxMeshInstances = m_maxInstanceData;

    stats.numInstanceOffsets = m_instanceOffsetCounter;
    stats.maxInstanceOffsets = m_maxInstanceOffsets;

    stats.numMeshTypes = m_meshInfoCounter;
    stats.maxMeshTypes = m_maxUniqueMeshes;

    stats.numMaterials = m_materialInfoCounter;
    stats.maxMaterials = m_maxUniqueMaterials;

    stats.numRenderNodes = (uint32)(m_renderNodeTransforms.size() - m_freeRenderNodeIndexes.size());
    stats.maxRenderNodes = m_maxRenderNodes;

    stats.numTextures = (uint32)Globals::textureManager.getNumTextures();
	stats.maxTextures = Globals::textureManager.getMaxTextures();

	stats.vertexDataUsedBytes = Globals::meshDataManager.getVertexBufUsed();
	stats.maxVertexDataBytes = Globals::meshDataManager.getVertexBufSize();

	stats.indexDataUsedBytes = Globals::meshDataManager.getIndexBufUsed();
	stats.maxIndexDataBytes = Globals::meshDataManager.getIndexBufSize();

    stats.numObjectContainers = (uint32)m_objectContainers.size();

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();

    PerFrameData& lastFrameData = m_perFrameData[(frameIdx - 1) % m_perFrameData.size()];
    struct LightGridInfo
    {
        uint32 inout_numGrids;
        uint32 inout_gridDataCounter;
    } info;
    oc::span<LightGridInfo> infoSpan = lastFrameData.lightTableBuffer.mapMemory<LightGridInfo>(0, sizeof(LightGridInfo));
    //lastFrameData.lightTableBuffer.flushMappedMemory(sizeof(LightGridInfo));
    info = *infoSpan.data();
    lastFrameData.lightTableBuffer.unmapMemory();
    stats.numLightGrids = info.inout_numGrids;
    stats.maxLightGrids = m_lightTableEntries / 2; // We don't want to go over 50% because of hash table collisions
    stats.lightGridMemUsageBytes = info.inout_gridDataCounter * sizeof(uint32);
    stats.maxLightGridMemUsageBytes = m_lightGridBufferSize;

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

    stats.blasBytes = m_accelStructure.getBlasTotalBytes();
    stats.blasCompactionSavedBytes = m_accelStructure.getCompactionSavedBytes();

    const MeshStreamer::Stats meshStreamStats = Globals::meshStreamer.getStats();
    stats.meshBudgetBytes = meshStreamStats.budgetBytes;
    stats.meshStreamableBytes = meshStreamStats.streamableBytes;
    stats.meshResidentBytes = meshStreamStats.residentBytes;
    stats.meshColdBytes = meshStreamStats.coldBytes;
    stats.numMeshSets = meshStreamStats.numSets;
    stats.numEvictedMeshSets = meshStreamStats.numEvictedSets;

    stats.numMeshLodGroups = (uint32)m_meshLodGroups.size();
    static_assert(sizeof(stats.lodInstanceCounts) == sizeof(uint32) * RendererVKLayout::MAX_MESH_LODS);
    // GPU-written, snapshotted in beginFrame — a few frames behind, and counting VISIBLE picks only.
    for (uint32 i = 0; i < RendererVKLayout::MAX_MESH_LODS; ++i)
        stats.lodInstanceCounts[i] = m_lodInstanceCounts[i];

    return stats;
}
