module RendererVK;

import Core;
import Core.glm;
import File;
import :Device;
import :SnowCapPipeline;
import :PushBlock;

namespace
{
    // VkDrawIndexedIndirectCommand
    struct DrawCommand
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
    };
    static_assert(sizeof(DrawCommand) == 20);
    constexpr uint32 MAX_TILES = RendererVKLayout::SNOW_CAP_TILES * RendererVKLayout::SNOW_CAP_TILES;
}

void SnowCapPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/Terrain/snow_cap_prepare.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushDeclaration = m_block.declaration();
    auto& b = layout.descriptorSetLayoutBindings;
    const auto binding = [&b](uint32 index, vk::DescriptorType type) {
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = index, .descriptorType = type, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    };
    binding(0, vk::DescriptorType::eUniformBuffer);        // the frame UBO
    binding(1, vk::DescriptorType::eCombinedImageSampler); // the terrain-data cascades
    binding(2, vk::DescriptorType::eCombinedImageSampler); // the site top
    binding(3, vk::DescriptorType::eCombinedImageSampler); // the site pass
    binding(4, vk::DescriptorType::eStorageImage);         // the site class (R8UI)
    binding(5, vk::DescriptorType::eCombinedImageSampler); // the deformation's coarse map
    binding(6, vk::DescriptorType::eStorageBuffer);        // the words
    binding(7, vk::DescriptorType::eStorageBuffer);        // the draw records
    binding(8, vk::DescriptorType::eStorageBuffer);        // the draw commands
    binding(9, vk::DescriptorType::eStorageBuffer);        // the LOD table
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = m_block.size() });
}

void SnowCapPipeline::initialize()
{
    using namespace RendererVKLayout;
    ComputePipelineLayout layout;
    buildLayout(layout);
    (void)m_pipeline.initialize(layout);
    m_words.initialize((vk::DeviceSize)SNOW_SITE_RES * SNOW_SITE_RES * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "SnowCapWords");
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_sets[i].initialize(m_pipeline.getDescriptorSetLayout(), "SnowCapPrepare");
        m_draws[i].initialize((vk::DeviceSize)SNOW_CAP_LODS * MAX_TILES * sizeof(SnowCapDraw),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "SnowCapDraws");
        m_lods[i].initialize(MAX_TILES * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "SnowCapLods");
        m_commands[i].initialize(SNOW_CAP_LODS * sizeof(DrawCommand),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "SnowCapCommands");
        const DrawCommand none[SNOW_CAP_LODS] = {};
        m_commands[i].upload(sizeof(none), none);
    }
    // The quads of a LOD 0 tile, each with 4 vertices of its own (q = 4q .. 4q + 3: (0, 0), (1, 0), (0, 1), (1, 1) in the
    // quad); a LOD draws the first (32 >> lod)^2 of them.
    oc::vector<uint16> indices;
    indices.reserve(SNOW_CAP_TILE * SNOW_CAP_TILE * 6);
    for (uint32 q = 0; q < SNOW_CAP_TILE * SNOW_CAP_TILE; ++q)
    {
        const uint16 v = (uint16)(4 * q);
        indices.insert(indices.end(), { v, (uint16)(v + 1), (uint16)(v + 2), (uint16)(v + 2), (uint16)(v + 1), (uint16)(v + 3) });
    }
    m_indices.initialize(indices.size() * sizeof(uint16), vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "SnowCapIndices");
    m_indices.upload(indices.size() * sizeof(uint16), indices.data());
}

void SnowCapPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("SnowCapPipeline: shader reload failed, keeping previous pipeline\n");
}

// No instances, each LOD's index count and its segment of the draw list (firstInstance). The slot's last draws read these
// (its fence was waited).
void SnowCapPipeline::resetCommands(vk::CommandBuffer cmd, uint32 frameIdx)
{
    using namespace RendererVKLayout;
    DrawCommand commands[SNOW_CAP_LODS];
    for (uint32 lod = 0; lod < SNOW_CAP_LODS; ++lod)
    {
        const uint32 n = SNOW_CAP_TILE >> lod;
        commands[lod] = DrawCommand{ .indexCount = n * n * 6, .instanceCount = 0, .firstIndex = 0, .vertexOffset = 0, .firstInstance = lod * MAX_TILES };
    }
    cmd.updateBuffer(m_commands[frameIdx].getBuffer(), 0, sizeof(commands), commands);
}

void SnowCapPipeline::recordClear(vk::CommandBuffer cmd, uint32 frameIdx)
{
    resetCommands(cmd, frameIdx);
    const vk::MemoryBarrier2 toDraw{ .srcStageMask = vk::PipelineStageFlagBits2::eTransfer, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect, .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toDraw });
}

void SnowCapPipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params)
{
    using namespace RendererVKLayout;
    resetCommands(cmd, frameIdx);
    if (!m_wordsCleared) // no texel holds a cap before its tile is first prepared (a drawn tile's VS reads its neighbours')
    {
        cmd.fillBuffer(m_words.getBuffer(), 0, vk::WholeSize, 0u);
        m_wordsCleared = true;
    }
    cmd.fillBuffer(m_lods[frameIdx].getBuffer(), 0, vk::WholeSize, 0xFFFFFFFFu); // no tile drawn yet
    // The words after every earlier frame's cap VS (one buffer for all slots), the commands after the reset.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    // This slot's set (its fence was waited): the UBO and the ping-pong terrain map are the slot's.
    const vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    const vk::DescriptorBufferInfo ubo{ params.ubo, 0, UBO_RANGE };
    const vk::DescriptorImageInfo terrain{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorImageInfo top{ .sampler = params.siteSampler, .imageView = params.siteTopView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorImageInfo pass{ .sampler = params.siteSampler, .imageView = params.sitePassView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorImageInfo cls{ .imageView = params.siteClassView, .imageLayout = vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo deform{ .sampler = params.siteSampler, .imageView = params.deformCoarseView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    const vk::DescriptorBufferInfo words{ m_words.getBuffer(), 0, vk::WholeSize };
    const vk::DescriptorBufferInfo draws{ m_draws[frameIdx].getBuffer(), 0, vk::WholeSize };
    const vk::DescriptorBufferInfo commands{ m_commands[frameIdx].getBuffer(), 0, vk::WholeSize };
    const vk::DescriptorBufferInfo lods{ m_lods[frameIdx].getBuffer(), 0, vk::WholeSize };
    const vk::WriteDescriptorSet writes[10] = {
        { .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &ubo },
        { .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &terrain },
        { .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &top },
        { .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &pass },
        { .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cls },
        { .dstSet = set, .dstBinding = 5, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &deform },
        { .dstSet = set, .dstBinding = 6, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &words },
        { .dstSet = set, .dstBinding = 7, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &draws },
        { .dstSet = set, .dstBinding = 8, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &commands },
        { .dstSet = set, .dstBinding = 9, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lods },
    };
    Globals::device.getDevice().updateDescriptorSets(10, writes, 0, nullptr);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    PushData pc(m_block);
    pc.set(m_push.origin, params.siteOrigin);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
    cmd.dispatch(MAX_TILES, 1, 1); // one workgroup per tile slot

    // To the scene pass: the commands (indirect), the records and the words (the VS's storage reads).
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &after });
}
