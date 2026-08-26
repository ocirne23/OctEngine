module RendererVK;

import Core;
import Core.glm;
import File;
import :ForceFieldPipeline;
import :GraphicsPipeline;
import :ComputePipeline;
import :Device;
import :Allocator;
import :Layout;

using namespace RendererVKLayout;

ForceFieldPipeline::~ForceFieldPipeline()
{
    destroyShellVolume();
    destroyIntervalTarget();
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_intervalRenderPass)
        vkDevice.destroyRenderPass(m_intervalRenderPass);
    m_intervalRenderPass = nullptr;
    if (m_intervalSampler)
        vkDevice.destroySampler(m_intervalSampler);
    m_intervalSampler = nullptr;
}

// ---- the union march's interval target ----

void ForceFieldPipeline::createIntervalRenderPass()
{
    // RG16F (fp16 range holds any gameplay march distance; ~6 cm quantization at 100 m only seeds
    // the march): cleared to fp16-max, MIN-blend accumulated, handed to the union FS SHADER_READ.
    const vk::AttachmentDescription2 attachment{
        .format = vk::Format::eR16G16Sfloat,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::AttachmentReference2 colorRef{ .attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eColor };
    const vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
    };
    const vk::SubpassDependency2 dependency{ // interval writes -> the union FS's sampled read
        .srcSubpass = 0,
        .dstSubpass = vk::SubpassExternal,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
    };
    const vk::RenderPassCreateInfo2 info{
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    auto result = Globals::device.getDevice().createRenderPass2(info);
    if (result.result != vk::Result::eSuccess) { assert(false && "force interval renderpass"); return; }
    m_intervalRenderPass = result.value;
}

void ForceFieldPipeline::destroyIntervalTarget()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_intervalFramebuffer)
        vkDevice.destroyFramebuffer(m_intervalFramebuffer);
    m_intervalFramebuffer = nullptr;
    if (m_intervalView)
        vkDevice.destroyImageView(m_intervalView);
    m_intervalView = nullptr;
    Globals::gpuAllocator.destroyImage(m_intervalImage, m_intervalMemory);
    m_intervalImage = nullptr;
    m_intervalMemory = nullptr;
}

void ForceFieldPipeline::resizeIntervalTarget(uint32 width, uint32 height)
{
    vk::Device vkDevice = Globals::device.getDevice();
    destroyIntervalTarget();
    m_intervalWidth = width;
    m_intervalHeight = height;
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    if (!Globals::gpuAllocator.createImage(info, m_intervalImage, m_intervalMemory, "ForceShellInterval"))
    {
        assert(false && "force interval image");
        return;
    }
    const vk::ImageViewCreateInfo viewInfo{
        .image = m_intervalImage,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    auto viewResult = vkDevice.createImageView(viewInfo);
    if (viewResult.result != vk::Result::eSuccess) { assert(false && "force interval view"); return; }
    m_intervalView = viewResult.value;
    const vk::FramebufferCreateInfo fbInfo{
        .renderPass = m_intervalRenderPass,
        .attachmentCount = 1,
        .pAttachments = &m_intervalView,
        .width = width,
        .height = height,
        .layers = 1,
    };
    auto fbResult = vkDevice.createFramebuffer(fbInfo);
    if (fbResult.result != vk::Result::eSuccess) { assert(false && "force interval framebuffer"); return; }
    m_intervalFramebuffer = fbResult.value;
    if (!m_intervalSampler)
    {
        const vk::SamplerCreateInfo samplerInfo{
            .magFilter = vk::Filter::eNearest,
            .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        };
        auto samplerResult = vkDevice.createSampler(samplerInfo);
        if (samplerResult.result != vk::Result::eSuccess) { assert(false && "force interval sampler"); return; }
        m_intervalSampler = samplerResult.value;
    }
}

// The sampled shell tier's two field volumes (phi[0..3]/phi[4..7], RGBA16F) + the sampler the
// shell FS reads them with. One set for all frames in flight: the bake's acquire barrier
// serializes prev-frame fragment reads against this frame's compute writes on the queue.
void ForceFieldPipeline::createShellVolume()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (int i = 0; i < 2; ++i)
    {
        const vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e3D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = { FORCE_SHELL_VOLUME_X, FORCE_SHELL_VOLUME_Y, FORCE_SHELL_VOLUME_Z },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        if (!Globals::gpuAllocator.createImage(info, m_shellVolumeImage[i], m_shellVolumeMemory[i],
            i == 0 ? "ForceShellVolumeA" : "ForceShellVolumeB"))
        {
            assert(false && "force shell volume image");
            return;
        }
        const vk::ImageViewCreateInfo viewInfo{
            .image = m_shellVolumeImage[i],
            .viewType = vk::ImageViewType::e3D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        if (viewResult.result != vk::Result::eSuccess) { assert(false && "force shell volume view"); return; }
        m_shellVolumeView[i] = viewResult.value;
    }
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        // Outside the fitted volume = ZERO field (transparent black border) — correct by
        // construction: the volume covers every sampled-tier emitter's support.
        .addressModeU = vk::SamplerAddressMode::eClampToBorder,
        .addressModeV = vk::SamplerAddressMode::eClampToBorder,
        .addressModeW = vk::SamplerAddressMode::eClampToBorder,
        .borderColor = vk::BorderColor::eFloatTransparentBlack,
    };
    auto samplerResult = vkDevice.createSampler(samplerInfo);
    if (samplerResult.result != vk::Result::eSuccess) { assert(false && "force shell volume sampler"); return; }
    m_shellVolumeSampler = samplerResult.value;

    // One-time GENERAL transition + zero-clear (the images stay GENERAL for life: compute writes
    // and fragment samples both use it, so the per-frame reuse needs no layout traffic) — a
    // never-yet-baked read decodes as zero field.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    for (int i = 0; i < 2; ++i)
    {
        vk::ImageMemoryBarrier2 toGeneral{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = m_shellVolumeImage[i],
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral });
        const vk::ClearColorValue zero{};
        const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        cmd.clearColorImage(m_shellVolumeImage[i], vk::ImageLayout::eGeneral, &zero, 1, &range);
    }
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void ForceFieldPipeline::destroyShellVolume()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_shellVolumeSampler)
        vkDevice.destroySampler(m_shellVolumeSampler);
    m_shellVolumeSampler = nullptr;
    for (int i = 0; i < 2; ++i)
    {
        if (m_shellVolumeView[i])
            vkDevice.destroyImageView(m_shellVolumeView[i]);
        m_shellVolumeView[i] = nullptr;
        Globals::gpuAllocator.destroyImage(m_shellVolumeImage[i], m_shellVolumeMemory[i]);
        m_shellVolumeImage[i] = nullptr;
        m_shellVolumeMemory[i] = nullptr;
    }
}

void ForceFieldPipeline::buildDrawLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/force_shell.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/force_shell.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    if (m_useGrid)
    {
        layout.vertexShader.defines.push_back({ "FORCE_GRID", "" });
        layout.fragmentShader.defines.push_back({ "FORCE_GRID", "" });
    }
    // Cull FRONT faces and skip the fixed-function depth test: the box's far/inside faces rasterize
    // exactly once per covered pixel even with the camera inside the volume; the fragment shader
    // marches within the box and depth-tests against the G-buffer depth itself.
    layout.cullMode = vk::CullModeFlagBits::eFront;
    layout.blendEnable = true; // premultiplied: out = src.rgb + dst * (1 - src.a)
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    // 5/6: the sampled shell tier's field volumes (see force_shellbake.cs.glsl).
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 6, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });

    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void ForceFieldPipeline::buildComputeLayout(ComputePipelineLayout& layout, const char* shaderPath)
{
    layout.computeShaderDebugFilePath = shaderPath;
    layout.computeShaderText = FileSystem::readFileStr(shaderPath);
    // force_grid.cs defines FORCE_GRID itself (it IS the grid pass); the others follow the toggle.
    if (m_useGrid)
        layout.defines.push_back({ "FORCE_GRID", "" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 binding : { 1u, 3u, 4u, 5u, 6u })
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

// The interval pass: the shell VS's proxy boxes with a rasterization-only FS MIN-blending each
// box's ray interval into the RG16F target (see recordIntervalPass).
void ForceFieldPipeline::buildIntervalLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/force_shell.vs.glsl"; // the same proxy boxes
    layout.fragmentShader.debugFilePath = "Shaders/force_interval.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    if (m_useGrid)
    {
        layout.vertexShader.defines.push_back({ "FORCE_GRID", "" });
        layout.fragmentShader.defines.push_back({ "FORCE_GRID", "" });
    }
    layout.cullMode = vk::CullModeFlagBits::eFront; // camera inside a box still rasterizes (shell rule)
    layout.blendEnable = true;
    layout.colorBlendOp = vk::BlendOp::eMin; // (tEntry, -tExit) union accumulate; factors ignored
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

// The union march: a fullscreen triangle whose FS marches each covered pixel's interval once
// (force_union.fs.glsl) — the analytic tier's one-march-per-pixel path.
void ForceFieldPipeline::buildUnionLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/composite.vs.glsl"; // the engine's fullscreen triangle
    layout.fragmentShader.debugFilePath = "Shaders/force_union.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    if (m_useGrid)
        layout.fragmentShader.defines.push_back({ "FORCE_GRID", "" });
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.blendEnable = true; // premultiplied over the lit scene, exactly like the shell draw
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

// The shell-volume bake's set shape: the common compute layout with STORAGE IMAGES at 5/6 (the
// two field volumes) instead of buffers.
void ForceFieldPipeline::buildShellBakeLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/force_shellbake.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    if (m_useGrid)
        layout.defines.push_back({ "FORCE_GRID", "" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 binding : { 1u, 3u, 4u })
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 binding : { 5u, 6u })
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void ForceFieldPipeline::createGridBuffers()
{
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        // Table header {numCells, dataCounter, tableSize, pad} is the CPU demand readback, so the
        // table stays DeviceLocal|HostVisible (light-grid contract).
        m_gridTableBuffers[i].initialize(16 + (size_t)m_tableEntries * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceGridTable");
        { // zero the demand header: checkForceGridCapacity reads it before the first GPU clear runs
            oc::span<uint32> header = m_gridTableBuffers[i].mapMemory<uint32>(0, 16);
            memset(header.data(), 0, 16);
            m_gridTableBuffers[i].unmapMemory();
        }
        m_gridDataBuffers[i].initialize(m_gridDataSize,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ForceGridData");
    }
}

void ForceFieldPipeline::initialize(vk::RenderPass sceneRenderPass, uint32 viewCount)
{
    m_viewCount = viewCount;
    GraphicsPipelineLayout drawLayout;
    buildDrawLayout(drawLayout);
    m_pipeline.initialize(sceneRenderPass, drawLayout);

    ComputePipelineLayout gridLayout, forceLayout, queryLayout, bakeLayout, shellBakeLayout;
    buildComputeLayout(gridLayout, "Shaders/force_grid.cs.glsl");
    buildComputeLayout(forceLayout, "Shaders/force_emitter.cs.glsl");
    buildComputeLayout(queryLayout, "Shaders/force_query.cs.glsl");
    buildComputeLayout(bakeLayout, "Shaders/force_bake.cs.glsl");
    buildShellBakeLayout(shellBakeLayout);
    m_gridPipeline.initialize(gridLayout);
    m_emitterForcePipeline.initialize(forceLayout);
    m_queryPipeline.initialize(queryLayout);
    m_bakePipeline.initialize(bakeLayout);
    m_shellBakePipeline.initialize(shellBakeLayout);

    createIntervalRenderPass();
    GraphicsPipelineLayout intervalLayout, unionLayout;
    buildIntervalLayout(intervalLayout);
    buildUnionLayout(unionLayout);
    m_intervalPipeline.initialize(m_intervalRenderPass, intervalLayout);
    m_unionPipeline.initialize(sceneRenderPass, unionLayout);

    createGridBuffers();
    createShellVolume();

    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_emitterBuffers[i].initialize(sizeof(ForceEmittersGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceEmitters", BufferHostAccess::eSequentialWrite);
        m_mappedEmitters[i] = m_emitterBuffers[i].mapMemory<ForceEmittersGpu>();
        m_mappedEmitters[i].data()->count = 0;
        m_mappedEmitters[i].data()->bigCount = 0;
        m_mappedEmitters[i].data()->evalCount = 0;
        m_emitterBuffers[i].flushMappedMemory(FORCE_EMITTER_HEADER_SIZE);

        m_queryBuffers[i].initialize(sizeof(ForceQueriesGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceQueries", BufferHostAccess::eSequentialWrite);
        m_mappedQueries[i] = m_queryBuffers[i].mapMemory<ForceQueriesGpu>();
        m_mappedQueries[i].data()->count = 0;
        m_queryBuffers[i].flushMappedMemory(FORCE_QUERY_HEADER_SIZE);

        m_bakeBrickBuffers[i].initialize(sizeof(ForceBakeBricksGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceBakeBricks", BufferHostAccess::eSequentialWrite);
        m_mappedBakeBricks[i] = m_bakeBrickBuffers[i].mapMemory<ForceBakeBricksGpu>();
        m_mappedBakeBricks[i].data()->count = 0;
        m_mappedBakeBricks[i].data()->sampleY = 0.0f;
        m_bakeBrickBuffers[i].flushMappedMemory(FORCE_BAKE_HEADER_SIZE);

        m_indirectBuffers[i].initialize(INDIRECT_UINTS * sizeof(uint32),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceIndirect", BufferHostAccess::eSequentialWrite);
        m_mappedIndirect[i] = m_indirectBuffers[i].mapMemory<uint32>();
        memset(m_mappedIndirect[i].data(), 0, INDIRECT_UINTS * sizeof(uint32));
        m_mappedIndirect[i][DRAW_CMD_OFFSET] = 36; // vertexCount: one cube per emitter
        m_mappedIndirect[i][GRID_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][GRID_DISPATCH_OFFSET + 2] = 1;
        m_mappedIndirect[i][EMITTER_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][EMITTER_DISPATCH_OFFSET + 2] = 1;
        m_mappedIndirect[i][QUERY_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][QUERY_DISPATCH_OFFSET + 2] = 1;
        m_mappedIndirect[i][BAKE_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][BAKE_DISPATCH_OFFSET + 2] = 1;
        // Shell-volume bake: y/z group counts are the fixed volume dims; x toggles per frame in
        // upload (0 = tier inactive — the cached CB's dispatch becomes a no-op).
        m_mappedIndirect[i][SHELLBAKE_DISPATCH_OFFSET + 1] = FORCE_SHELL_VOLUME_Y / FORCE_SHELL_VOLUME_GROUP;
        m_mappedIndirect[i][SHELLBAKE_DISPATCH_OFFSET + 2] = FORCE_SHELL_VOLUME_Z / FORCE_SHELL_VOLUME_GROUP;
        m_mappedIndirect[i][INTERVAL_DRAW_OFFSET] = 36;  // instanceCount + firstInstance per frame
        m_mappedIndirect[i][UNION_DRAW_OFFSET + 1] = 1;  // one fullscreen instance; vertexCount 3/0
        m_indirectBuffers[i].flushMappedMemory(vk::WholeSize);

        // GPU-written, CPU-read ~2 frames later; zeroed so pre-first-frame reads decode as "no force
        // / never evaluated" instead of garbage (ocean readback pattern).
        m_forceReadbackBuffers[i].initialize(MAX_FORCE_EMITTERS * sizeof(glm::vec4),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "ForceReadback");
        m_mappedForceReadback[i] = m_forceReadbackBuffers[i].mapMemory<glm::vec4>();
        memset(m_mappedForceReadback[i].data(), 0, m_mappedForceReadback[i].size_bytes());

        m_queryReadbackBuffers[i].initialize(MAX_FORCE_QUERIES * sizeof(ForceQueryResult),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "ForceQueryReadback");
        m_mappedQueryReadback[i] = m_queryReadbackBuffers[i].mapMemory<ForceQueryResult>();
        memset(m_mappedQueryReadback[i].data(), 0, m_mappedQueryReadback[i].size_bytes());

        m_bakeReadbackBuffers[i].initialize(
            (size_t)MAX_FORCE_BAKE_BRICKS * FORCE_BAKE_SAMPLES_PER_BRICK * 2 * sizeof(glm::vec4),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "ForceBakeReadback");
        m_mappedBakeReadback[i] = m_bakeReadbackBuffers[i].mapMemory<glm::vec4>();
        memset(m_mappedBakeReadback[i].data(), 0, m_mappedBakeReadback[i].size_bytes());

        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_drawSets[drawSlot(i, eye)].initialize(m_pipeline.getDescriptorSetLayout());
        m_gridSets[i].initialize(m_gridPipeline.getDescriptorSetLayout());
        m_emitterForceSets[i].initialize(m_emitterForcePipeline.getDescriptorSetLayout());
        m_querySets[i].initialize(m_queryPipeline.getDescriptorSetLayout());
        m_bakeSets[i].initialize(m_bakePipeline.getDescriptorSetLayout());
        m_shellBakeSets[i].initialize(m_shellBakePipeline.getDescriptorSetLayout());
        m_intervalSets[i].initialize(m_intervalPipeline.getDescriptorSetLayout());
        m_unionSets[i].initialize(m_unionPipeline.getDescriptorSetLayout());
    }
}

void ForceFieldPipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    GraphicsPipelineLayout drawLayout;
    buildDrawLayout(drawLayout);
    if (!m_pipeline.reloadShaders(sceneRenderPass, drawLayout))
        printf("ForceFieldPipeline: shell shader reload failed, keeping previous pipeline\n");
    ComputePipelineLayout gridLayout, forceLayout, queryLayout, bakeLayout, shellBakeLayout;
    buildComputeLayout(gridLayout, "Shaders/force_grid.cs.glsl");
    buildComputeLayout(forceLayout, "Shaders/force_emitter.cs.glsl");
    buildComputeLayout(queryLayout, "Shaders/force_query.cs.glsl");
    buildComputeLayout(bakeLayout, "Shaders/force_bake.cs.glsl");
    buildShellBakeLayout(shellBakeLayout);
    if (!m_gridPipeline.reloadShaders(gridLayout))
        printf("ForceFieldPipeline: grid shader reload failed, keeping previous pipeline\n");
    if (!m_emitterForcePipeline.reloadShaders(forceLayout))
        printf("ForceFieldPipeline: emitter force shader reload failed, keeping previous pipeline\n");
    if (!m_queryPipeline.reloadShaders(queryLayout))
        printf("ForceFieldPipeline: query shader reload failed, keeping previous pipeline\n");
    if (!m_bakePipeline.reloadShaders(bakeLayout))
        printf("ForceFieldPipeline: bake shader reload failed, keeping previous pipeline\n");
    if (!m_shellBakePipeline.reloadShaders(shellBakeLayout))
        printf("ForceFieldPipeline: shell-volume bake shader reload failed, keeping previous pipeline\n");
    GraphicsPipelineLayout intervalLayout, unionLayout;
    buildIntervalLayout(intervalLayout);
    buildUnionLayout(unionLayout);
    if (!m_intervalPipeline.reloadShaders(m_intervalRenderPass, intervalLayout))
        printf("ForceFieldPipeline: interval shader reload failed, keeping previous pipeline\n");
    if (!m_unionPipeline.reloadShaders(sceneRenderPass, unionLayout))
        printf("ForceFieldPipeline: union march shader reload failed, keeping previous pipeline\n");
}

void ForceFieldPipeline::upload(uint32 frameIdx, oc::span<const ForceEmitterGpu> slots,
    oc::span<const ForceQueryGpu> querySlots, oc::span<const glm::ivec4> bakeBricks,
    float bakeSampleY, float bigReachThreshold, const ShellCull& shellCull)
{
    // Would this shell's ray-march draw be visible? Mirrors forceEmitterBounds (the proxy's
    // bounding sphere): frustum test + projected-size floor. A culled shell still contributes
    // its FIELD — it only moves into the non-drawn partition below.
    const auto shellVisible = [&](const ForceEmitterGpu& e)
    {
        if (!shellCull.enabled)
            return true;
        const float R = e.posReach.w;
        const float m = glm::abs(1.0f - 2.0f * e.dirFocus.w);
        const float side = 0.5f * R * (1.0f + m) * e.outputParams.w * 1.03f;
        const float forward = R * 1.02f, back = R * 0.02f;
        const glm::vec3 center = glm::vec3(e.posReach) + glm::vec3(e.dirFocus) * ((forward - back) * 0.5f);
        const float radius = glm::length(glm::vec3(side, side, (forward + back) * 0.5f));
        if (!shellCull.frustum.sphereInFrustum(center, radius))
            return false;
        if (shellCull.minPixels <= 0.0f)
            return true;
        const float dist = glm::distance(center, shellCull.cameraPos);
        if (dist <= radius)
            return true; // the camera is inside the proxy: never size-cull
        return radius * shellCull.pixelScale >= shellCull.minPixels * dist;
    };
    ForceEmittersGpu* dst = m_mappedEmitters[frameIdx].data();
    uint32 count = 0;
    uint32 bigCount = 0;
    const auto compact = [&](const ForceEmitterGpu& e, uint32 slot, bool allowBig)
    {
        ForceEmitterGpu& out = dst->emitters[count];
        out = e;
        out.teamFlags.z = slot; // slot-indexed readback target
        // Big emitters bypass the grid into the globally-scanned list; when the list is full the
        // overflow inserts into the grid anyway (large footprint, but never a hole in the field).
        const float maxReach = e.posReach.w; // Reach IS the max extent (the bubble spans the output line)
        if (allowBig && maxReach > bigReachThreshold && bigCount < MAX_FORCE_BIG_EMITTERS)
        {
            out.teamFlags.y |= FORCE_FLAG_BIG;
            dst->bigIndices[bigCount++] = count;
        }
        else
            out.teamFlags.y &= ~FORCE_FLAG_BIG;
        ++count;
    };
    // Three-pass partition: drawable shells first so the shell draw's instanceCount can stop before
    // the shellAlpha-0 emitters — an invisible world-scale emitter still contributes field (grid/
    // field evaluations use `count`) but never costs its full-screen ray march — then the PASSIVE
    // tail past `count`: merge-group members that only want their own slot-indexed force/pressure
    // readback, seen by force_emitter.cs alone (`evalCount`).
    const auto isActive = [&](uint32 slot) { return (slots[slot].teamFlags.y & FORCE_FLAG_ACTIVE) != 0u; };
    const auto isPassive = [&](uint32 slot) { return (slots[slot].teamFlags.y & FORCE_FLAG_PASSIVE) != 0u; };
    const auto isSampledTier = [&](const ForceEmitterGpu& e) { return e.posReach.w >= shellCull.sampledReach; };
    // Drawable partition: SAMPLED-tier proxies first, ANALYTIC drawables second — the union pass's
    // interval draw covers exactly the second range via firstInstance, and with the union pass OFF
    // the proxy draw simply spans both.
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if (isActive(slot) && !isPassive(slot) && slots[slot].outputParams.y > 0.0f
            && shellVisible(slots[slot]) && isSampledTier(slots[slot]))
            compact(slots[slot], slot, true);
    const uint32 sampledDrawCount = count;
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if (isActive(slot) && !isPassive(slot) && slots[slot].outputParams.y > 0.0f
            && shellVisible(slots[slot]) && !isSampledTier(slots[slot]))
            compact(slots[slot], slot, true);
    const uint32 drawCount = count;
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if (isActive(slot) && !isPassive(slot)
            && (slots[slot].outputParams.y <= 0.0f || !shellVisible(slots[slot])))
            compact(slots[slot], slot, true);
    const uint32 fieldCount = count;
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if (isActive(slot) && isPassive(slot))
            compact(slots[slot], slot, false); // not a field: never in the big list either
    dst->count = fieldCount;
    dst->bigCount = bigCount;
    dst->evalCount = count;
    m_emitterBuffers[frameIdx].flushMappedMemory(FORCE_EMITTER_HEADER_SIZE + count * sizeof(ForceEmitterGpu));

    ForceQueriesGpu* q = m_mappedQueries[frameIdx].data();
    const uint32 numQueries = (uint32)glm::min(querySlots.size(), (size_t)MAX_FORCE_QUERIES);
    if (numQueries > 0)
        memcpy(q->queries, querySlots.data(), numQueries * sizeof(ForceQueryGpu));
    q->count = numQueries;
    m_queryBuffers[frameIdx].flushMappedMemory(FORCE_QUERY_HEADER_SIZE + numQueries * sizeof(ForceQueryGpu));

    // Bake bricks + the per-slot CPU pairing copy: this slot's readback (~2 frames from now) is
    // indexed by exactly this list (see getBakeReadback).
    ForceBakeBricksGpu* bk = m_mappedBakeBricks[frameIdx].data();
    const uint32 numBricks = (uint32)glm::min(bakeBricks.size(), (size_t)MAX_FORCE_BAKE_BRICKS);
    if (numBricks > 0)
        memcpy(bk->bricks, bakeBricks.data(), numBricks * sizeof(glm::ivec4));
    bk->count = numBricks;
    bk->sampleY = bakeSampleY;
    m_bakeBrickBuffers[frameIdx].flushMappedMemory(FORCE_BAKE_HEADER_SIZE + numBricks * sizeof(glm::ivec4));
    m_bakeBrickLists[frameIdx].assign(bakeBricks.begin(), bakeBricks.begin() + numBricks);

    uint32* ind = m_mappedIndirect[frameIdx].data();
    // UNION MARCH routing: with the pass on, the proxy draw keeps only the sampled tier — the
    // analytic drawables rasterize their intervals instead and the fullscreen march shades them.
    const uint32 analyticDrawCount = drawCount - sampledDrawCount;
    const bool unionActive = shellCull.unionPass && analyticDrawCount > 0;
    ind[DRAW_CMD_OFFSET + 1] = unionActive ? sampledDrawCount : drawCount;
    ind[INTERVAL_DRAW_OFFSET + 1] = unionActive ? analyticDrawCount : 0;
    ind[INTERVAL_DRAW_OFFSET + 3] = sampledDrawCount; // firstInstance: the analytic partition
    ind[UNION_DRAW_OFFSET] = unionActive ? 3u : 0u;   // the fullscreen triangle
    ind[GRID_DISPATCH_OFFSET] = fieldCount; // single-thread workgroups (see force_grid.cs.glsl)
    ind[EMITTER_DISPATCH_OFFSET] = (count + FORCE_SIM_GROUP_SIZE - 1) / FORCE_SIM_GROUP_SIZE; // + passive tail
    ind[QUERY_DISPATCH_OFFSET] = (numQueries + FORCE_SIM_GROUP_SIZE - 1) / FORCE_SIM_GROUP_SIZE;
    ind[BAKE_DISPATCH_OFFSET] = numBricks; // one 16x16 workgroup per brick
    ind[SHELLBAKE_DISPATCH_OFFSET] = shellCull.bakeVolume
        ? FORCE_SHELL_VOLUME_X / FORCE_SHELL_VOLUME_GROUP : 0; // 0 = tier inactive this frame
    m_indirectBuffers[frameIdx].flushMappedMemory(vk::WholeSize);
}

void ForceFieldPipeline::recordCompute(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo)
{
    vk::CommandBuffer vkCb = commandBuffer.getCommandBuffer();
    const auto bufInfo = [](const Buffer& buffer) {
        return vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() };
    };
    // One set shape for all three passes: 0 = UBO, 1 = emitters, 3/4 = grid table/data, 5/6 = per-pass IO.
    const auto makeUpdates = [&](const Buffer& io5, const Buffer& io6) {
        return oc::array<DescriptorSetUpdateInfo, 6>{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridTableBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridDataBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(io5) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(io6) } },
        };
    };

    if (m_useGrid)
    {
        // Clear the table (header counters 0, tableSize, entries EMPTY) + the cell data.
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 0, 8, 0);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 8, 4, m_tableEntries);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 12, 4, 0);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 16, vk::WholeSize, 0xFFFFFFFF);
        vkCb.fillBuffer(m_gridDataBuffers[frameIdx].getBuffer(), 0, vk::WholeSize, 0);
        {
            vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            };
            vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
        }
        vk::DescriptorSet gridSet = m_gridSets[frameIdx].getDescriptorSet();
        auto gridUpdates = makeUpdates(m_forceReadbackBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]); // 5/6 unused by the shader
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_gridPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_gridPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, gridSet, gridUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_gridPipeline.getPipelineLayout(), 0, 1, &gridSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), GRID_DISPATCH_OFFSET * sizeof(uint32));
        {
            vk::MemoryBarrier2 barrier{ // grid write -> force/query gather
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            };
            vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
        }
    }

    {
        vk::DescriptorSet forceSet = m_emitterForceSets[frameIdx].getDescriptorSet();
        auto forceUpdates = makeUpdates(m_forceReadbackBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]);
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_emitterForcePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_emitterForcePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, forceSet, forceUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_emitterForcePipeline.getPipelineLayout(), 0, 1, &forceSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), EMITTER_DISPATCH_OFFSET * sizeof(uint32));
    }
    {
        vk::DescriptorSet querySet = m_querySets[frameIdx].getDescriptorSet();
        auto queryUpdates = makeUpdates(m_queryBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]);
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_queryPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_queryPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, querySet, queryUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_queryPipeline.getPipelineLayout(), 0, 1, &querySet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), QUERY_DISPATCH_OFFSET * sizeof(uint32));
    }
    { // the baked pressure field: one workgroup per brick (reads grid + emitters, own output)
        vk::DescriptorSet bakeSet = m_bakeSets[frameIdx].getDescriptorSet();
        auto bakeUpdates = makeUpdates(m_bakeBrickBuffers[frameIdx], m_bakeReadbackBuffers[frameIdx]);
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_bakePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_bakePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, bakeSet, bakeUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_bakePipeline.getPipelineLayout(), 0, 1, &bakeSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), BAKE_DISPATCH_OFFSET * sizeof(uint32));
    }
    { // the SAMPLED SHELL TIER's volume bake: one thread per voxel into the two field volumes.
      // Acquire: the PREVIOUS frame's shell-fragment reads of the (single-set) volumes must finish
      // before this frame's writes — an execution+layout-preserving image barrier on the queue.
        oc::array<vk::ImageMemoryBarrier2, 2> acquire;
        for (int i = 0; i < 2; ++i)
            acquire[i] = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = m_shellVolumeImage[i],
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            };
        vkCb.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)acquire.size(), .pImageMemoryBarriers = acquire.data() });
        vk::DescriptorSet shellBakeSet = m_shellBakeSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 6> shellBakeUpdates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridTableBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridDataBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_shellVolumeView[0], .imageLayout = vk::ImageLayout::eGeneral } } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_shellVolumeView[1], .imageLayout = vk::ImageLayout::eGeneral } } },
        };
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_shellBakePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_shellBakePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, shellBakeSet, shellBakeUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_shellBakePipeline.getPipelineLayout(), 0, 1, &shellBakeSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), SHELLBAKE_DISPATCH_OFFSET * sizeof(uint32));
    }

    { // grid reads for the shell FS in the scene pass + readback visibility for the CPU
        oc::array<vk::MemoryBarrier2, 2> barriers{
            vk::MemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            },
            vk::MemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            },
        };
        vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = (uint32)barriers.size(), .pMemoryBarriers = barriers.data() });
    }
}

ForceFieldPipeline::GridDemand ForceFieldPipeline::getGridDemand(uint32 frameIdx)
{
    oc::span<GridDemand> span = m_gridTableBuffers[frameIdx].mapMemory<GridDemand>(0, sizeof(GridDemand));
    const GridDemand demand = *span.data();
    m_gridTableBuffers[frameIdx].unmapMemory();
    return demand;
}

void ForceFieldPipeline::growGridBuffers(size_t neededDataBytes, uint32 neededTableEntries)
{
    while (m_gridDataSize < neededDataBytes)
        m_gridDataSize *= 2;
    while (m_tableEntries < neededTableEntries)
        m_tableEntries *= 2; // stays a power of 2 for the hash
    createGridBuffers(); // per-frame GPU scratch, rebuilt every frame: nothing to preserve
    printf("ForceFieldPipeline: grew grid buffers to %zu bytes / %u table entries\n", m_gridDataSize, m_tableEntries);
}

void ForceFieldPipeline::recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 viewIndex = eyeToViewIndex(eye, m_viewCount);
    DescriptorSet& set = m_drawSets[drawSlot(frameIdx, eye)];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    oc::array<DescriptorSetUpdateInfo, 7> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_emitterBuffers[frameIdx].getBuffer(), .range = m_emitterBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridTableBuffers[frameIdx].getBuffer(), .range = m_gridTableBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridDataBuffers[frameIdx].getBuffer(), .range = m_gridDataBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = m_shellVolumeSampler, .imageView = m_shellVolumeView[0], .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = m_shellVolumeSampler, .imageView = m_shellVolumeView[1], .imageLayout = vk::ImageLayout::eGeneral } } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), 0, 1, sizeof(vk::DrawIndirectCommand));

    // The UNION MARCH fullscreen draw (analytic tier, one march per pixel): vertexCount is 0
    // whenever the pass is off (VR, tweak, density view), so recording it is always safe.
    DescriptorSet& unionSet = m_unionSets[frameIdx];
    vk::DescriptorSet vkUnionSet = unionSet.getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 6> unionUpdates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_emitterBuffers[frameIdx].getBuffer(), .range = m_emitterBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridTableBuffers[frameIdx].getBuffer(), .range = m_gridTableBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridDataBuffers[frameIdx].getBuffer(), .range = m_gridDataBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = m_intervalSampler, .imageView = m_intervalView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_unionPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkUnionSet, unionUpdates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_unionPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_unionPipeline.getPipelineLayout(), 0, 1, &vkUnionSet, 0, nullptr);
    cmd.pushConstants(m_unionPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), UNION_DRAW_OFFSET * sizeof(uint32), 1, sizeof(vk::DrawIndirectCommand));
}

// The interval pass: see the header comment. Recorded in the PRIMARY right before the scene
// stages; unconditional (clear + indirect draws that are 0 instances when the union pass is off).
void ForceFieldPipeline::recordIntervalPass(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo,
    const vk::Viewport& viewport, const vk::Rect2D& scissor)
{
    vk::CommandBuffer vkCb = commandBuffer.getCommandBuffer();
    const vk::ClearValue clear{ vk::ClearColorValue{ std::array<float, 4>{ 65504.0f, 65504.0f, 0.0f, 0.0f } } };
    const vk::RenderPassBeginInfo begin{
        .renderPass = m_intervalRenderPass,
        .framebuffer = m_intervalFramebuffer,
        .renderArea = { vk::Offset2D{ 0, 0 }, vk::Extent2D{ m_intervalWidth, m_intervalHeight } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCb.beginRenderPass(begin, vk::SubpassContents::eInline);
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
    vk::DescriptorSet set = m_intervalSets[frameIdx].getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 2> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_emitterBuffers[frameIdx].getBuffer(), .range = m_emitterBuffers[frameIdx].getSize() } } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_intervalPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, set, updates);
    vkCb.bindPipeline(vk::PipelineBindPoint::eGraphics, m_intervalPipeline.getPipeline());
    vkCb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_intervalPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    const uint32 viewIndex = 0; // desktop only (VR keeps per-proxy shells)
    vkCb.pushConstants(m_intervalPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    vkCb.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), INTERVAL_DRAW_OFFSET * sizeof(uint32), 1, sizeof(vk::DrawIndirectCommand));
    vkCb.endRenderPass();
}
