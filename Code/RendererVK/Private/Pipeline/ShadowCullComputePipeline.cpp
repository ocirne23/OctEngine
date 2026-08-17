module RendererVK;

import Core;
import File;
import :Device;
import :Layout;

ShadowCullComputePipeline::ShadowCullComputePipeline() {}
ShadowCullComputePipeline::~ShadowCullComputePipeline() {}

void ShadowCullComputePipeline::initialize(uint32 maxMeshInstances, uint32 maxUniqueMeshes)
{
    resizeInstanceBuffers(maxMeshInstances);
    resizeCommandBuffers(maxUniqueMeshes);

    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    m_computePipeline.initialize(computePipelineLayout);
}

void ShadowCullComputePipeline::resizeInstanceBuffers(uint32 maxMeshInstances)
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.outMeshInstancesBuffer.initialize(maxMeshInstances * sizeof(RendererVKLayout::OutShadowMeshInstance), // 6
            vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ShadowCullOutInstances");

        perFrame.outMeshInstanceIndexesBuffer.initialize(maxMeshInstances * sizeof(uint32), // 7
            vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ShadowCullOutIndices");
    }
}

void ShadowCullComputePipeline::resizeCommandBuffers(uint32 maxUniqueMeshes)
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.outIndirectCommandBuffer.initialize(maxUniqueMeshes * sizeof(RendererVKLayout::IndirectDrawSequence), // 8 (single opaque region)
            vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal);
    }
}

void ShadowCullComputePipeline::reloadShaders()
{
    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    if (!m_computePipeline.reloadShaders(computePipelineLayout))
        printf("ShadowCullComputePipeline: shader reload failed, keeping previous pipeline\n");
}

void ShadowCullComputePipeline::buildComputeLayout(ComputePipelineLayout& computePipelineLayout)
{
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/instanced_indirect_shadow.cs.glsl";
    computePipelineLayout.computeShaderText = FileSystem::readFileStr(computePipelineLayout.computeShaderDebugFilePath);

    auto& b = computePipelineLayout.descriptorSetLayoutBindings;
    for (uint32 i = 0; i <= 12; i++) // 11/12 = LOD group idx per mesh / LOD group data
    {
        b.push_back(vk::DescriptorSetLayoutBinding{
            .binding = i,
            .descriptorType = (i == 0) ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
}

void ShadowCullComputePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];

    oc::array<DescriptorSetUpdateInfo, 13> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = params.ubo.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inRenderNodeTransformsBuffer.getBuffer(), .range = params.inRenderNodeTransformsBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMeshInstancesBuffer.getBuffer(), .range = params.inMeshInstancesBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMeshInstanceOffsetsBuffer.getBuffer(), .range = params.inMeshInstanceOffsetsBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMeshInfoBuffer.getBuffer(), .range = params.inMeshInfoBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inFirstInstancesBuffer.getBuffer(), .range = params.inFirstInstancesBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = frameData.outMeshInstancesBuffer.getBuffer(), .range = frameData.outMeshInstancesBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = frameData.outMeshInstanceIndexesBuffer.getBuffer(), .range = frameData.outMeshInstanceIndexesBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = frameData.outIndirectCommandBuffer.getBuffer(), .range = frameData.outIndirectCommandBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMaterialInfoBuffer.getBuffer(), .range = params.inMaterialInfoBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 10, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inNodePassMasksBuffer.getBuffer(), .range = params.inNodePassMasksBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 11, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMeshLodGroupIdxBuffer.getBuffer(), .range = params.inMeshLodGroupIdxBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 12, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.inMeshLodGroupsBuffer.getBuffer(), .range = params.inMeshLodGroupsBuffer.getSize() } } },
    };

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipeline());
    commandBuffer.cmdUpdateDescriptorSets(m_computePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, descriptorSet, updates);
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    // Full-capacity clear so the recorded size never depends on the live mesh count (see IndirectCull).
    vkCommandBuffer.fillBuffer(frameData.outIndirectCommandBuffer.getBuffer(), 0, vk::WholeSize, 0); // clear per-mesh instance counts
    {
        vk::MemoryBarrier2 memoryBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
    }

    vkCommandBuffer.dispatchIndirect(params.dispatchIndirectBuffer.getBuffer(), 0);

    {
        vk::PipelineStageFlags2 dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eCommandPreprocessEXT;
        vk::AccessFlags2 dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eCommandPreprocessReadEXT;
        vk::MemoryBarrier2 memoryBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = dstStageMask,
            .dstAccessMask = dstAccessMask,
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
    }
}
