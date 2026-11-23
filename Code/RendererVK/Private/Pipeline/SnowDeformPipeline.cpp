module RendererVK;

import Core;
import Core.glm;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :TextureManager;
import :SnowDeformPipeline;
import :PushBlock;

namespace
{
    constexpr vk::Format DEFORM_STORAGE_FORMAT = vk::Format::eR32Uint;
    constexpr vk::Format DEFORM_SAMPLED_FORMAT = vk::Format::eR16G16Unorm; // R = lo16 (the stamp's snowfall), G = hi16 (the offset)
    constexpr vk::ImageSubresourceRange c_colorRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    // Every stage that samples the maps: the terrain VS, the TCS / TES, the terrain FS (and a compute reader).
    constexpr vk::PipelineStageFlags2 c_readStages = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader
        | vk::PipelineStageFlagBits2::eTessellationControlShader | vk::PipelineStageFlagBits2::eTessellationEvaluationShader
        | vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr vk::AccessFlags2 c_storageAccess = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
}

SnowDeformPipeline::~SnowDeformPipeline()
{
    // The images and their sampled views are the TextureManager's (freed with it); the storage views are ours.
    vk::Device vkDevice = Globals::device.getDevice();
    for (Map* map : { &m_fine, &m_coarse })
        if (map->storageView)
            vkDevice.destroyImageView(map->storageView);
}

void SnowDeformPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/Terrain/snow_deform.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushDeclaration = m_block.declaration();
    auto& b = layout.descriptorSetLayoutBindings;
    const auto binding = [&b](uint32 index, vk::DescriptorType type) {
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = index, .descriptorType = type, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    };
    binding(0, vk::DescriptorType::eUniformBuffer);        // the frame UBO
    binding(1, vk::DescriptorType::eStorageImage);         // the fine map (R32UI)
    binding(2, vk::DescriptorType::eStorageImage);         // the coarse map (R32UI)
    binding(3, vk::DescriptorType::eCombinedImageSampler); // the terrain-data cascades
    binding(4, vk::DescriptorType::eCombinedImageSampler); // the snow site map: the top
    binding(5, vk::DescriptorType::eCombinedImageSampler); // ... and the pass
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = m_block.size() });
}

void SnowDeformPipeline::initialize()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
    for (DescriptorSet& set : m_sets)
        set.initialize(m_pipeline.getDescriptorSetLayout(), "SnowDeform");
}

void SnowDeformPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("SnowDeformPipeline: shader reload failed, keeping previous pipeline\n");
}

// One map: a MUTABLE R32UI image (+ the format list, so the driver may keep it compressed), its R32UI storage view and its
// R16G16 sampled view.
void SnowDeformPipeline::makeMap(Map& map, const char* name)
{
    using namespace RendererVKLayout;
    vk::Device vkDevice = Globals::device.getDevice();
    const vk::Format formats[2] = { DEFORM_STORAGE_FORMAT, DEFORM_SAMPLED_FORMAT };
    const vk::ImageFormatListCreateInfo formatList{ .viewFormatCount = 2, .pViewFormats = formats };
    const vk::ImageCreateInfo info{
        .pNext = &formatList,
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = DEFORM_STORAGE_FORMAT,
        .extent = { SNOW_DEFORM_RES, SNOW_DEFORM_RES, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, map.image, map.memory, name);

    const vk::ImageViewCreateInfo storageInfo{ .image = map.image, .viewType = vk::ImageViewType::e2D, .format = DEFORM_STORAGE_FORMAT, .subresourceRange = c_colorRange };
    auto storageResult = vkDevice.createImageView(storageInfo);
    assert(storageResult.result == vk::Result::eSuccess);
    map.storageView = storageResult.value;
    Globals::device.setDebugName(map.storageView, name);

    const vk::ImageViewUsageCreateInfo sampledOnly{ .usage = vk::ImageUsageFlagBits::eSampled }; // R16G16_UNORM needs no storage
    const vk::ImageViewCreateInfo sampledInfo{ .pNext = &sampledOnly, .image = map.image, .viewType = vk::ImageViewType::e2D,
        .format = DEFORM_SAMPLED_FORMAT, .subresourceRange = c_colorRange };
    auto sampledResult = vkDevice.createImageView(sampledInfo);
    assert(sampledResult.result == vk::Result::eSuccess);
    map.sampledView = sampledResult.value;
    Globals::device.setDebugName(map.sampledView, name);
}

// Both maps UNDISTURBED (offset 0), left in SHADER_READ_ONLY_OPTIMAL and handed to the TextureManager (the image, its
// memory and the sampled view; it frees them).
void SnowDeformPipeline::createImages()
{
    using namespace RendererVKLayout;
    makeMap(m_fine, "SnowDeformFine");
    makeMap(m_coarse, "SnowDeformCoarse");

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "SnowDeform.init");
    vk::CommandBuffer cmd = init.begin(true);
    vk::ImageMemoryBarrier2 toClear[2];
    vk::ImageMemoryBarrier2 toRead[2];
    const vk::Image images[2] = { m_fine.image, m_coarse.image };
    for (uint32 i = 0; i < 2; ++i)
    {
        toClear[i] = vk::ImageMemoryBarrier2{ .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe, .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite, .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = images[i], .subresourceRange = c_colorRange };
        toRead[i] = vk::ImageMemoryBarrier2{ .srcStageMask = vk::PipelineStageFlagBits2::eClear, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = c_readStages, .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal, .image = images[i], .subresourceRange = c_colorRange };
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = toClear });
    vk::ClearColorValue undisturbed;
    undisturbed.uint32[0] = SNOW_DEFORM_UNDISTURBED;
    for (const vk::Image image : images)
        cmd.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, &undisturbed, 1, &c_colorRange);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = toRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();

    for (Map* map : { &m_fine, &m_coarse })
        map->tex = Globals::textureManager.adoptImage(map->image, map->memory, map->sampledView, DEFORM_SAMPLED_FORMAT, SNOW_DEFORM_RES, SNOW_DEFORM_RES);
}

// The windows: SNOW_DEFORM_RES texels around the focus, their origin snapped to whole steps (the window's centre within a
// step of the focus: the fine one reaches >= 28 m out). A window that stepped, an activation, a clear or a rebase runs the
// maintenance; record commits (a frame that records nothing asks again on the next one).
SnowDeformPipeline::Tick SnowDeformPipeline::advance(bool wanted, bool clear, const glm::vec3& focus, double fallMetres)
{
    using namespace RendererVKLayout;
    Tick tick;
    m_clearPending |= clear;
    if (!wanted)
    {
        m_traced = false; // the next activation clears everything (the pending clear with it)
        m_clearPending = false;
        return tick;
    }
    tick.active = true;
    tick.centre = glm::vec2(focus.x, focus.z);
    tick.fall = fallMetres;
    const auto windowOrigin = [&focus](float texel, int step)
    {
        const glm::ivec2 cell(glm::floor(glm::vec2(focus.x, focus.z) / (texel * (float)step)));
        return cell * step - glm::ivec2((int)SNOW_DEFORM_RES / 2);
    };
    tick.fineOrigin = windowOrigin(SNOW_DEFORM_FINE_TEXEL, SNOW_DEFORM_FINE_STEP);
    tick.coarseOrigin = windowOrigin(SNOW_DEFORM_COARSE_TEXEL, SNOW_DEFORM_COARSE_STEP);
    tick.fresh = !m_traced || m_clearPending;
    const glm::ivec2 park(4 * (int)SNOW_DEFORM_RES); // nothing of the window is held: every texel clears
    tick.finePrev = tick.fresh ? tick.fineOrigin + park : m_fineOrigin;
    tick.coarsePrev = tick.fresh ? tick.coarseOrigin + park : m_coarseOrigin;
    // THE REBASE: the stamps' clock is the snowfall in mm mod 65536, so a track refilled long ago would open again after
    // 65.5 m of snowfall. Every 30 m (a track is at most 2 m deep: long refilled by then) the refilled ones are reset. A
    // clock that went back (a clear) starts the count again.
    constexpr double c_rebaseFall = 30.0;
    tick.rebase = !tick.fresh && (fallMetres - m_rebasedAt > c_rebaseFall || fallMetres < m_rebasedAt);
    tick.maintain = tick.fresh || tick.rebase || tick.fineOrigin != m_fineOrigin || tick.coarseOrigin != m_coarseOrigin;
    return tick;
}

// Read-only <-> GENERAL, keeping the contents. Into GENERAL: after every earlier sampled read on the queue; back: before
// every later one.
void SnowDeformPipeline::transition(vk::CommandBuffer cmd, bool toWrite)
{
    const vk::ImageLayout read = vk::ImageLayout::eShaderReadOnlyOptimal;
    const vk::ImageLayout write = vk::ImageLayout::eGeneral;
    vk::ImageMemoryBarrier2 barriers[2];
    const vk::Image images[2] = { m_fine.image, m_coarse.image };
    for (uint32 i = 0; i < 2; ++i)
    {
        barriers[i] = toWrite
            ? vk::ImageMemoryBarrier2{ .srcStageMask = c_readStages, .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = c_storageAccess,
                .oldLayout = read, .newLayout = write, .image = images[i], .subresourceRange = c_colorRange }
            : vk::ImageMemoryBarrier2{ .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = c_storageAccess,
                .dstStageMask = c_readStages, .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = write, .newLayout = read, .image = images[i], .subresourceRange = c_colorRange };
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers });
}

void SnowDeformPipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, const Tick& tick, const RecordParams& params)
{
    using namespace RendererVKLayout;
    assert(tick.active && "SnowDeformPipeline::record without an active tick");
    if (!tick.maintain && params.deformerCount == 0)
        return; // nothing changes
    transition(cmd, true);

    // This slot's set (its fence was waited): the UBO and the ping-pong terrain map are the slot's.
    const vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    const vk::DescriptorBufferInfo ubo{ params.ubo, 0, UBO_RANGE };
    const vk::DescriptorImageInfo fine{ .imageView = m_fine.storageView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo coarse{ .imageView = m_coarse.storageView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo terrain{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorImageInfo siteTop{ .sampler = params.siteSampler, .imageView = params.siteTopView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorImageInfo sitePass{ .sampler = params.siteSampler, .imageView = params.sitePassView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::WriteDescriptorSet writes[6] = {
        { .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &ubo },
        { .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &fine },
        { .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &coarse },
        { .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &terrain },
        { .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &siteTop },
        { .dstSet = set, .dstBinding = 5, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &sitePass },
    };
    Globals::device.getDevice().updateDescriptorSets(6, writes, 0, nullptr);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    PushData pc(m_block);
    pc.set(m_push.deformers, params.deformers);
    pc.set(m_push.fineOrigin, tick.fineOrigin);
    pc.set(m_push.finePrev, tick.finePrev);
    pc.set(m_push.coarseOrigin, tick.coarseOrigin);
    pc.set(m_push.coarsePrev, tick.coarsePrev);
    pc.set(m_push.count, params.deformerCount);
    if (tick.maintain)
    {
        pc.set(m_push.mode, 0u);
        pc.set(m_push.flags, (tick.fresh ? 0u : 1u) | (tick.rebase ? 2u : 0u)); // (snow_deform.cs.glsl DEFORM_*)
        cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
        cmd.dispatch(SNOW_DEFORM_RES * SNOW_DEFORM_RES / 64, 2, 1); // one thread per texel, y = the map
        const vk::MemoryBarrier2 between{ .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = c_storageAccess,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = c_storageAccess };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &between });
    }
    if (params.deformerCount > 0)
    {
        pc.set(m_push.mode, 1u);
        cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
        cmd.dispatch(params.deformerCount, 1, 1); // one workgroup per stamp
    }
    transition(cmd, false);
    m_fineOrigin = tick.fineOrigin;
    m_coarseOrigin = tick.coarseOrigin;
    m_traced = true;
    if (tick.fresh)
        m_clearPending = false;
    if (tick.fresh || tick.rebase)
        m_rebasedAt = tick.fall;
}
