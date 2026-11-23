module RendererVK;

import Core;
import Core.glm;
import File;
import Settings;
import :Device;
import :Allocator;
import :CommandBuffer;
import :TextureManager;
import :SnowSitePipeline;
import :PushBlock;

namespace
{
    constexpr vk::Format SITE_TOP_FORMAT = vk::Format::eR32Sfloat;
    constexpr vk::Format SITE_PASS_FORMAT = vk::Format::eR8G8Unorm; // the pass + the highest foliage over the top
    constexpr vk::Format SITE_CLASS_FORMAT = vk::Format::eR8Uint;    // the class + the invalidation bits
    constexpr vk::MemoryBarrier2 c_classWritten{ // the class image between its compute passes (the invalidation, the trace, the caps)
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite };
    constexpr float SITE_TICK_SECONDS = 0.1f; // 10 Hz: the shelters change when the world does, not per frame
    constexpr float SITE_RAY_ABOVE = 400.0f;  // m above the focus the rays start ...
    constexpr float SITE_RAY_RANGE = 1200.0f; // ... and how far down they run

    constexpr vk::ImageSubresourceRange c_colorRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    // Every stage that samples the map: the culls and the wetness (compute), the terrain VS / TES, the fragment shaders.
    constexpr vk::PipelineStageFlags2 c_readStages = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader
        | vk::PipelineStageFlagBits2::eTessellationEvaluationShader | vk::PipelineStageFlagBits2::eFragmentShader;
}

SnowSitePipeline::~SnowSitePipeline()
{
    // The top and the pass are the TextureManager's; the class image is ours.
    if (m_classView)
        Globals::device.getDevice().destroyImageView(m_classView);
    if (m_classImage)
        Globals::gpuAllocator.destroyImage(m_classImage, m_classMemory);
}

void SnowSitePipeline::buildInvalidateLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/Sky/snow_site_invalidate.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushDeclaration = m_invalidateBlock.declaration();
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = m_invalidateBlock.size() });
}

void SnowSitePipeline::recordInvalidate(vk::CommandBuffer cmd, oc::span<const Invalidation> rects)
{
    if (rects.empty() || !m_traced)
        return; // (no trace held: the next one is full and writes every class)
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &c_classWritten }); // after the last frame's readers
    const vk::DescriptorSet set = m_invalidateSet.getDescriptorSet();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_invalidatePipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_invalidatePipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    const glm::ivec2 res((int)RendererVKLayout::SNOW_SITE_RES);
    bool first = true;
    for (const Invalidation& r : rects)
    {
        // Clipped to the window the map holds (the caller drops the rectangles outside it): outside it a slot is another
        // world position (toroidal), and what scrolls in later is traced fresh.
        const glm::ivec2 lo = glm::max(r.min, m_origin);
        const glm::ivec2 size = glm::min(r.min + r.size, m_origin + res) - lo;
        if (size.x <= 0 || size.y <= 0 || r.bits == 0)
            continue;
        if (!first) // the last rectangle's read-modify-write may share texels with this one
            cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &c_classWritten });
        first = false;
        PushData pc(m_invalidateBlock);
        pc.set(m_invalidatePush.min, lo);
        pc.set(m_invalidatePush.size, size);
        pc.set(m_invalidatePush.bits, r.bits);
        cmd.pushConstants(m_invalidatePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
        cmd.dispatch((uint32)(size.x + 7) / 8, (uint32)(size.y + 7) / 8, 1);
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &c_classWritten });
}

void SnowSitePipeline::registerPushFields()
{
    const float& block = Globals::settings.weather.foliageBlock;
    m_block.lockable(m_push.layerBlock, [&block] { return glm::clamp(block, 0.0f, 1.0f); }, block);
}

void SnowSitePipeline::buildLayout(ComputePipelineLayout& layout, uint32 maxTextures)
{
    layout.computeShaderDebugFilePath = "Shaders/Sky/snow_site.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushDeclaration = m_block.declaration();
    auto& b = layout.descriptorSetLayoutBindings;
    const auto binding = [&b](uint32 index, vk::DescriptorType type, uint32 count = 1) {
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = index, .descriptorType = type, .descriptorCount = count, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    };
    binding(0, vk::DescriptorType::eAccelerationStructureKHR);
    binding(1, vk::DescriptorType::eStorageImage); // the top
    binding(2, vk::DescriptorType::eStorageImage); // the pass
    for (uint32 i = 3; i <= 7; ++i)
        binding(i, vk::DescriptorType::eStorageBuffer); // vertices, indices, mesh infos, instances, materials
    binding(8, vk::DescriptorType::eStorageImage); // the class
    binding(9, vk::DescriptorType::eUniformBuffer); // the frame UBO: the frustum (the slice refresh's gate)
    binding(10, vk::DescriptorType::eCombinedImageSampler, maxTextures); // highest: the variable count
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount
        | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = m_block.size() });
}

// The two images, cleared to OPEN SKY (no top, everything passes) so a reader before the first trace sees no shelter,
// then left in SHADER_READ_ONLY_OPTIMAL and handed to the TextureManager, which frees them.
void SnowSitePipeline::createImages()
{
    using namespace RendererVKLayout;
    vk::Device vkDevice = Globals::device.getDevice();
    m_textureSampler.initialize(vk::SamplerAddressMode::eRepeat, 1.0f); // the alpha test's, and the direct readers' (repeat: the toroidal wrap)
    const auto makeImage =[&](vk::Format format, const char* name, vk::Image& image, vk::ImageView& view) -> VmaAllocation
    {
        const vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = { SNOW_SITE_RES, SNOW_SITE_RES, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        VmaAllocation memory = nullptr;
        (void)Globals::gpuAllocator.createImage(info, image, memory, name);
        const vk::ImageViewCreateInfo viewInfo{ .image = image, .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = c_colorRange };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        view = viewResult.value;
        Globals::device.setDebugName(view, name);
        return memory;
    };
    VmaAllocation topMemory = makeImage(SITE_TOP_FORMAT, "SnowSiteTop", m_topImage, m_topView);
    VmaAllocation passMemory = makeImage(SITE_PASS_FORMAT, "SnowSitePass", m_passImage, m_passView);

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "SnowSite.init");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageMemoryBarrier2 toClear[2] = {
        { .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe, .dstStageMask = vk::PipelineStageFlagBits2::eClear, .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
          .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal, .image = m_topImage, .subresourceRange = c_colorRange },
        { .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe, .dstStageMask = vk::PipelineStageFlagBits2::eClear, .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
          .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal, .image = m_passImage, .subresourceRange = c_colorRange },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = toClear });
    vk::ClearColorValue noTop;
    noTop.float32[0] = SNOW_SITE_NO_TOP;
    vk::ClearColorValue open; // everything passes, no foliage
    open.float32[0] = 1.0f;
    open.float32[1] = 0.0f;
    cmd.clearColorImage(m_topImage, vk::ImageLayout::eTransferDstOptimal, &noTop, 1, &c_colorRange);
    cmd.clearColorImage(m_passImage, vk::ImageLayout::eTransferDstOptimal, &open, 1, &c_colorRange);
    const vk::ImageMemoryBarrier2 toRead[2] = {
        { .srcStageMask = vk::PipelineStageFlagBits2::eClear, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = c_readStages,
          .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
          .image = m_topImage, .subresourceRange = c_colorRange },
        { .srcStageMask = vk::PipelineStageFlagBits2::eClear, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = c_readStages,
          .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
          .image = m_passImage, .subresourceRange = c_colorRange },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = toRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();

    m_topTex = Globals::textureManager.adoptImage(m_topImage, topMemory, m_topView, SITE_TOP_FORMAT, SNOW_SITE_RES, SNOW_SITE_RES);
    m_passTex = Globals::textureManager.adoptImage(m_passImage, passMemory, m_passView, SITE_PASS_FORMAT, SNOW_SITE_RES, SNOW_SITE_RES);

    // THE CLASS: the terrain everywhere (0), GENERAL for life.
    m_classMemory = makeImage(SITE_CLASS_FORMAT, "SnowSiteClass", m_classImage, m_classView);
    CommandBuffer classInit;
    classInit.initialize(vk::CommandBufferLevel::ePrimary, "SnowSite.classInit");
    vk::CommandBuffer classCmd = classInit.begin(true);
    const vk::ImageMemoryBarrier2 classToClear{ .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe, .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite, .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
        .image = m_classImage, .subresourceRange = c_colorRange };
    classCmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &classToClear });
    vk::ClearColorValue terrain;
    terrain.uint32[0] = 0u;
    classCmd.clearColorImage(m_classImage, vk::ImageLayout::eGeneral, &terrain, 1, &c_colorRange);
    const vk::MemoryBarrier2 cleared{ .srcStageMask = vk::PipelineStageFlagBits2::eClear, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite };
    classCmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &cleared });
    classInit.end();
    classInit.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();

    ComputePipelineLayout layout;
    buildInvalidateLayout(layout);
    (void)m_invalidatePipeline.initialize(layout);
    m_invalidateSet.initialize(m_invalidatePipeline.getDescriptorSetLayout(), "SnowSiteInvalidate");
    const vk::DescriptorImageInfo classInfo{ .imageView = m_classView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::WriteDescriptorSet write{ .dstSet = m_invalidateSet.getDescriptorSet(), .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &classInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

// The window: SNOW_SITE_RES texels centred on the focus, its origin an integer lattice coord. A tick re-centres it and
// traces what scrolled in plus one row slice; an activation (or a jump past the window) traces it all.
SnowSitePipeline::Tick SnowSitePipeline::advance(bool wanted, float realDeltaSec, const glm::vec3& focus)
{
    using namespace RendererVKLayout;
    Tick tick;
    if (!wanted)
    {
        m_traced = false; // the next activation traces everything
        m_accum = 0.0f;
        return tick;
    }
    tick.active = true;
    tick.readOrigin = m_origin;
    tick.origin = m_origin;
    tick.prevOrigin = m_origin;
    m_accum += glm::max(realDeltaSec, 0.0f);
    // Until a trace is RECORDED (record commits it), every frame asks again: a frame without a TLAS or a record (no
    // instances, a minimized window) loses no full trace and no scrolled-in strip.
    tick.tracing = !m_traced || m_accum >= SITE_TICK_SECONDS;
    if (tick.tracing)
    {
        m_accum = glm::clamp(m_accum - SITE_TICK_SECONDS, 0.0f, SITE_TICK_SECONDS); // never a burst of ticks after a hitch
        const glm::ivec2 origin = glm::ivec2(glm::floor(glm::vec2(focus.x, focus.z) / SNOW_SITE_TEXEL)) - glm::ivec2((int)SNOW_SITE_RES / 2);
        tick.prevOrigin = m_traced ? m_origin : origin + glm::ivec2(2 * (int)SNOW_SITE_RES); // nothing traced yet: everything is new
        const glm::ivec2 shift = glm::abs(origin - tick.prevOrigin);
        tick.full = !m_traced || glm::max(shift.x, shift.y) >= (int)SNOW_SITE_RES;
        tick.origin = origin;
        m_slice = (m_slice + 1u) % SNOW_SITE_SLICES;
    }
    tick.slice = m_slice;
    tick.topY = focus.y + SITE_RAY_ABOVE;
    tick.range = SITE_RAY_RANGE;
    return tick;
}

void SnowSitePipeline::ensureBuilt(uint32 layoutCap, uint32 descriptorCount)
{
    if (m_built)
        return;
    m_maxTextures = layoutCap;
    ComputePipelineLayout layout;
    buildLayout(layout, layoutCap);
    m_pipeline.initialize(layout);
    m_built = true;
    resizeTextureDescriptors(descriptorCount); // the sets, the bindless views, the storage images
}

void SnowSitePipeline::reloadShaders()
{
    if (m_classImage)
    {
        ComputePipelineLayout invalidateLayout;
        buildInvalidateLayout(invalidateLayout);
        if (!m_invalidatePipeline.reloadShaders(invalidateLayout))
            printf("SnowSitePipeline: invalidation shader reload failed, keeping previous pipeline\n");
    }
    if (!m_built)
        return; // built from the current file at the first activation
    ComputePipelineLayout layout;
    buildLayout(layout, m_maxTextures);
    if (!m_pipeline.reloadShaders(layout))
        printf("SnowSitePipeline: shader reload failed, keeping previous pipeline\n");
}

// The bindless array (the alpha test): new sets with the grown count, then every live view; afterwards slots arrive one at
// a time through updateTextureDescriptor.
void SnowSitePipeline::resizeTextureDescriptors(uint32 count)
{
    if (!m_built)
        return; // ensureBuilt sizes them
    for (DescriptorSet& set : m_sets)
        set.initialize(m_pipeline.getDescriptorSetLayout(), "SnowSite", count);
    fillTextureDescriptors();
    // A re-initialized set lost its storage images: write them again.
    const vk::DescriptorImageInfo top{ .imageView = m_topView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo pass{ .imageView = m_passView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo cls{ .imageView = m_classView, .imageLayout = vk::ImageLayout::eGeneral };
    for (DescriptorSet& set : m_sets)
    {
        const vk::WriteDescriptorSet writes[3] = {
            { .dstSet = set.getDescriptorSet(), .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &top },
            { .dstSet = set.getDescriptorSet(), .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &pass },
            { .dstSet = set.getDescriptorSet(), .dstBinding = 8, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cls },
        };
        Globals::device.getDevice().updateDescriptorSets(3, writes, 0, nullptr);
    }
}

void SnowSitePipeline::fillTextureDescriptors()
{
    const uint32 numTextures = (uint32)Globals::textureManager.getNumTextures();
    if (numTextures == 0)
        return;
    oc::vector<vk::DescriptorImageInfo> infos;
    infos.reserve(numTextures);
    for (uint16 texIdx = 0; texIdx < (uint16)numTextures; ++texIdx)
        infos.push_back(vk::DescriptorImageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = Globals::textureManager.getViewForDescriptor(texIdx),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
    for (DescriptorSet& set : m_sets)
    {
        const vk::WriteDescriptorSet write{ .dstSet = set.getDescriptorSet(), .dstBinding = 10, .dstArrayElement = 0, .descriptorCount = numTextures,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = infos.data() };
        Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
    }
}

void SnowSitePipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    if (!m_built)
        return;
    const vk::DescriptorImageInfo imageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::WriteDescriptorSet write{ .dstSet = m_sets[frameIdx].getDescriptorSet(), .dstBinding = 10, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

// Read-only <-> GENERAL, keeping the contents (the window is only partly rewritten). Into GENERAL: after every earlier
// sampled read on the queue (the last frame's readers); back: before every later one.
void SnowSitePipeline::transition(vk::CommandBuffer cmd, bool toWrite)
{
    const vk::ImageLayout read = vk::ImageLayout::eShaderReadOnlyOptimal;
    const vk::ImageLayout write = vk::ImageLayout::eGeneral;
    vk::ImageMemoryBarrier2 barriers[2];
    const vk::Image images[2] = { m_topImage, m_passImage };
    for (uint32 i = 0; i < 2; ++i)
    {
        barriers[i] = toWrite
            ? vk::ImageMemoryBarrier2{ .srcStageMask = c_readStages, .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, // the trace reads the top too (the slice gate's last top)
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = read, .newLayout = write, .image = images[i], .subresourceRange = c_colorRange }
            : vk::ImageMemoryBarrier2{ .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = c_readStages, .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = write, .newLayout = read, .image = images[i], .subresourceRange = c_colorRange };
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers });
}

void SnowSitePipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, const Tick& tick, const RecordParams& params)
{
    assert(m_built && tick.tracing && "SnowSitePipeline::record without a tracing tick");
    transition(cmd, true);

    // The TLAS and the instance buffer are this slot's (its fence was waited); the geometry buffers can grow.
    const vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    const vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &params.tlas };
    const vk::DescriptorBufferInfo buffers[5] = {
        { params.vertices, 0, vk::WholeSize }, { params.indices, 0, vk::WholeSize }, { params.meshInfos, 0, vk::WholeSize },
        { params.instances, 0, vk::WholeSize }, { params.materials, 0, vk::WholeSize },
    };
    const vk::DescriptorBufferInfo ubo{ params.ubo, 0, RendererVKLayout::UBO_RANGE };
    vk::WriteDescriptorSet writes[7];
    writes[0] = vk::WriteDescriptorSet{ .pNext = &asInfo, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
    for (uint32 i = 0; i < 5; ++i)
        writes[1 + i] = vk::WriteDescriptorSet{ .dstSet = set, .dstBinding = 3 + i, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffers[i] };
    writes[6] = vk::WriteDescriptorSet{ .dstSet = set, .dstBinding = 9, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &ubo };
    Globals::device.getDevice().updateDescriptorSets(7, writes, 0, nullptr);

    const uint32 res = RendererVKLayout::SNOW_SITE_RES;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    PushData pc(m_block);
    pc.set(m_push.origin, tick.origin);
    pc.set(m_push.prevOrigin, tick.prevOrigin);
    pc.set(m_push.slice, tick.slice);
    pc.set(m_push.full, tick.full ? 1u : 0u);
    pc.set(m_push.topY, tick.topY);
    pc.set(m_push.range, tick.range);
    pc.set(m_push.terrainRay, params.terrainRay);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
    cmd.dispatch(res / 8, res / 8, 1);

    transition(cmd, false);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &c_classWritten }); // the class: the caps' prepare
    m_origin = tick.origin; // the map now holds this window
    m_traced = true;
}
