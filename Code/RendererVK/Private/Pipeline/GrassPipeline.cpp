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
    for (uint32 binding = 2; binding <= 7; ++binding) // ground table, vertices, patches, commands, counts, the near casters
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
        m_nearShadowCommands[i].initialize(GRASS_MAX_PATCHES * GRASS_COMMAND_SIZE,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassNearShadowCommands");
        m_counts[i].initialize(16, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer
            | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "GrassCount");
        const uint32 zero[4] = {};
        m_counts[i].upload(sizeof(zero), zero);
    }
    setBladesPerPatch(bladesPerPatch);
}

// The near grass cascade's casters: the blade VS's GRASS_SHADOW + GRASS_NEAR_SHADOW variant (its own matrix, a
// single-view pass), depth only (no fragment shader), the shadow pass's depth state (standard Z, its slope-scaled bias),
// both faces. Its set: the UBO (the matrix, the grass params) and the vertex mega-buffer (the roots) at the main set's
// binding numbers, so the VS source is the same.
void GrassPipeline::buildNearShadowLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/grass.vs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.vertexShader.defines.push_back(ShaderDefine{ "GRASS_SHADOW", "1" });
    layout.vertexShader.defines.push_back(ShaderDefine{ "GRASS_NEAR_SHADOW", "1" });
    layout.depthOnly = true;
    layout.depthCompareOp = vk::CompareOp::eLess;
    layout.depthBiasEnable = true;
    layout.depthBiasConstantFactor = 1.25f; // as ShadowMapGraphicsPipeline
    layout.depthBiasSlopeFactor = 2.5f;
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.indirectBindable = false;
    layout.vertexLayoutInfo.bindingDescriptions.push_back(vk::VertexInputBindingDescription{
        .binding = 0, .stride = sizeof(RendererVKLayout::GrassPatchGpu), .inputRate = vk::VertexInputRate::eInstance });
    layout.vertexLayoutInfo.attributeDescriptions.push_back(vk::VertexInputAttributeDescription{
        .location = 0, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(RendererVKLayout::GrassPatchGpu, origin) });
    layout.vertexLayoutInfo.attributeDescriptions.push_back(vk::VertexInputAttributeDescription{
        .location = 1, .binding = 0, .format = vk::Format::eR32G32B32A32Uint, .offset = offsetof(RendererVKLayout::GrassPatchGpu, firstVertex) });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 14, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
}

void GrassPipeline::initializeNearShadow(vk::RenderPass nearShadowRenderPass)
{
    m_nearShadowRenderPass = nearShadowRenderPass;
    GraphicsPipelineLayout nearLayout;
    buildNearShadowLayout(nearLayout);
    m_nearShadowBuilt = m_nearShadowPipeline.initialize(m_nearShadowRenderPass, nearLayout);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        m_nearShadowSets[i].initialize(m_nearShadowPipeline.getDescriptorSetLayout(), "GrassNearShadow");
}

void GrassPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("GrassPipeline: shader reload failed, keeping previous pipeline\n");
    if (!m_nearShadowRenderPass)
        return;
    GraphicsPipelineLayout nearLayout;
    buildNearShadowLayout(nearLayout);
    if (!m_nearShadowBuilt)
        m_nearShadowBuilt = m_nearShadowPipeline.initialize(m_nearShadowRenderPass, nearLayout);
    else if (!m_nearShadowPipeline.reloadShaders(m_nearShadowRenderPass, nearLayout))
        printf("GrassPipeline: near shadow shader reload failed, keeping previous pipeline\n");
}

void GrassPipeline::recordNearShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, Buffer& vertexBuffer)
{
    GraphicsPipeline& pipeline = m_nearShadowPipeline;
    if (!m_nearShadowBuilt)
        return;
    oc::array<DescriptorSetUpdateInfo, 2> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 14, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = vertexBuffer.getBuffer(), .range = vertexBuffer.getSize() } } },
    };
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    vk::DescriptorSet set = m_nearShadowSets[frameIdx].getDescriptorSet();
    commandBuffer.cmdUpdateDescriptorSets(pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, set, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    cmd.bindVertexBuffers(0, { m_patches[frameIdx].getBuffer() }, { 0 });
    cmd.bindIndexBuffer(m_indices.getBuffer(), 0, vk::IndexType::eUint32);
    // The cull's count [1]: the near cascade's casters.
    cmd.drawIndexedIndirectCount(m_nearShadowCommands[frameIdx].getBuffer(), 0,
        m_counts[frameIdx].getBuffer(), sizeof(uint32), RendererVKLayout::GRASS_MAX_PATCHES, GRASS_COMMAND_SIZE);
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
    // The slot's last draws read the counts (its fence was waited); the scene + shadow passes read the zeros.
    primary.fillBuffer(m_counts[frameIdx].getBuffer(), 0, 4 * sizeof(uint32), 0u);
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

    // The counts restart at 0; the slot's last draws (which read these buffers) retired with its fence.
    cmd.fillBuffer(count.getBuffer(), 0, 4 * sizeof(uint32), 0u);
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
    oc::array<DescriptorSetUpdateInfo, 8> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo->getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        storage(2, m_frames[frameIdx]),
        storage(3, *params.vertexBuffer),
        storage(4, m_patches[frameIdx]),
        storage(5, m_commands[frameIdx]),
        storage(6, count),
        storage(7, m_nearShadowCommands[frameIdx]),
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
