module RendererVK;

import Core;
import File;
import :Device;
import :CommandBuffer;
import :Layout;

namespace
{
    // VkDrawIndexedIndirectCommand
    constexpr uint32 GRASS_COMMAND_SIZE = 5 * sizeof(uint32);
}

void GrassPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/grass_cull.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    constexpr vk::ShaderStageFlags CS = vk::ShaderStageFlagBits::eCompute;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = CS });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = CS });
    for (uint32 binding = 2; binding <= 6; ++binding) // ground table, vertices, patches, commands, count
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = CS });
    // Binding 1 (the terrain-data cascades) is a ping-pong pair rewritten per frame by updateTerrainDescriptor.
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags[1] = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
}

void GrassPipeline::initialize(uint32 bladesPerPatch)
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
    using RendererVKLayout::GRASS_MAX_PATCHES;
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_sets[i].initialize(m_pipeline.getDescriptorSetLayout(), "GrassCull");
        m_frames[i].initialize(sizeof(RendererVKLayout::GrassFrameGpu), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "GrassFrame", BufferHostAccess::eSequentialWrite);
        m_mappedFrames[i] = (RendererVKLayout::GrassFrameGpu*)m_frames[i].mapMemory().data();
        *m_mappedFrames[i] = RendererVKLayout::GrassFrameGpu{}; // gridDim 0: the cull draws nothing
        m_frames[i].flushMappedMemory(sizeof(RendererVKLayout::GrassFrameGpu));
        m_patches[i].initialize(GRASS_MAX_PATCHES * sizeof(RendererVKLayout::GrassPatchGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassPatches");
        m_commands[i].initialize(GRASS_MAX_PATCHES * GRASS_COMMAND_SIZE,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassCommands");
        m_counts[i].initialize(16, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer
            | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassCount");
        const uint32 zero[4] = {};
        m_counts[i].upload(sizeof(zero), zero);
    }
    setBladesPerPatch(bladesPerPatch);
}

void GrassPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("GrassPipeline: shader reload failed, keeping previous pipeline\n");
}

void GrassPipeline::setBladesPerPatch(uint32 bladesPerPatch)
{
    bladesPerPatch = oc::clamp(bladesPerPatch, 1u, RendererVKLayout::GRASS_MAX_BLADES);
    if (bladesPerPatch == m_bladesPerPatch && m_indices.getSize() > 0)
        return;
    m_bladesPerPatch = bladesPerPatch;
    buildIndices();
}

// The blade mesh: per LOD (GRASS_LOD_SEGMENTS), N blades in RANK order, each a strip of S segments - rows of two
// vertices (left, right) and one tip vertex. A vertex id is blade << GRASS_BLADE_VERTEX_SHIFT | vertex; grass.vs.glsl
// builds the vertex from it, so there is no vertex buffer.
void GrassPipeline::buildIndices()
{
    using namespace RendererVKLayout;
    static_assert(2 * GRASS_LOD_SEGMENTS[0] + 1 <= (1u << GRASS_BLADE_VERTEX_SHIFT), "a blade's vertices must fit its id range");
    oc::vector<uint32> indices;
    for (uint32 lod = 0; lod < GRASS_LODS; ++lod)
    {
        const uint32 segments = GRASS_LOD_SEGMENTS[lod];
        for (uint32 blade = 0; blade < m_bladesPerPatch; ++blade)
        {
            const uint32 base = blade << GRASS_BLADE_VERTEX_SHIFT;
            for (uint32 row = 0; row + 1 < segments; ++row)
            {
                const uint32 l0 = base + 2 * row, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
                indices.insert(indices.end(), { l0, r0, l1, r0, r1, l1 });
            }
            const uint32 l = base + 2 * (segments - 1);
            indices.insert(indices.end(), { l, l + 1, base + 2 * segments });
        }
    }
    m_indices.destroy();
    m_indices.initialize(indices.size() * sizeof(uint32), vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassBladeIndices");
    m_indices.upload(indices.size() * sizeof(uint32), indices.data());
}

void GrassPipeline::updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = m_sets[frameIdx].getDescriptorSet(), .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void GrassPipeline::flushFrame(uint32 frameIdx)
{
    m_frames[frameIdx].flushMappedMemory(sizeof(RendererVKLayout::GrassFrameGpu));
}

GrassPipeline::Draw GrassPipeline::getDraw(uint32 frameIdx)
{
    return Draw{ .patches = &m_patches[frameIdx], .commands = &m_commands[frameIdx], .count = &m_counts[frameIdx],
        .indices = &m_indices, .maxDraws = RendererVKLayout::GRASS_MAX_PATCHES };
}

void GrassPipeline::recordClear(vk::CommandBuffer primary, uint32 frameIdx)
{
    // The slot's last draw read the count (its fence was waited); the scene pass reads the zero.
    primary.fillBuffer(m_counts[frameIdx].getBuffer(), 0, sizeof(uint32), 0u);
    const vk::MemoryBarrier2 toDraw{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
    };
    primary.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toDraw });
}

void GrassPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    Buffer& count = m_counts[frameIdx];

    // The count restarts at 0; the slot's last draw (which read these buffers) retired with its fence.
    cmd.fillBuffer(count.getBuffer(), 0, sizeof(uint32), 0u);
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    const auto storage = [](uint32 binding, const Buffer& buffer) {
        return DescriptorSetUpdateInfo{ .binding = binding, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() } } };
    };
    oc::array<DescriptorSetUpdateInfo, 7> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo->getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        storage(2, m_frames[frameIdx]),
        storage(3, *params.vertexBuffer),
        storage(4, m_patches[frameIdx]),
        storage(5, m_commands[frameIdx]),
        storage(6, count),
    };
    vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    cmd.dispatch(RendererVKLayout::GRASS_MAX_PATCHES / RendererVKLayout::GRASS_CULL_GROUP, 1, 1);

    // To the scene pass: the draw count + commands (indirect), the patch records (instance-rate vertex attributes).
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexAttributeInput,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eVertexAttributeRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &after });
}
