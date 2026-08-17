module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :Layout;
import :ObjectContainer;
import :StagingManager;

IndirectCullComputePipeline::IndirectCullComputePipeline() {}
IndirectCullComputePipeline::~IndirectCullComputePipeline() {}

void IndirectCullComputePipeline::initialize(uint32 maxMeshInstances, uint32 maxUniqueMeshes)
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.inIndirectCommandBuffer.initialize(sizeof(vk::DispatchIndirectCommand), // x
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "IndirectCullCmd", BufferHostAccess::eSequentialWrite);
        perFrame.mappedIndirectCommands = perFrame.inIndirectCommandBuffer.mapMemory<vk::DispatchIndirectCommand>();
    }
    resizeInstanceBuffers(maxMeshInstances);
    resizeCommandBuffers(maxUniqueMeshes);

    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    m_computePipeline.initialize(computePipelineLayout);
}

void IndirectCullComputePipeline::resizeInstanceBuffers(uint32 maxMeshInstances)
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.outMeshInstancesBuffer.initialize(maxMeshInstances * sizeof(RendererVKLayout::OutMeshInstance), // 6
            vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "CullOutInstances");

        perFrame.outMeshInstanceIndexesBuffer.initialize(maxMeshInstances * sizeof(uint32), // 7
            vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "CullOutIndices");
    }
}

void IndirectCullComputePipeline::resizeCommandBuffers(uint32 maxUniqueMeshes)
{
    constexpr vk::BufferUsageFlags2 usage = vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst
        | vk::BufferUsageFlagBits2::eShaderDeviceAddress; // device address consumed by vkCmdExecuteGeneratedCommandsEXT
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.outIndirectCommandBuffer.initialize(maxUniqueMeshes * sizeof(RendererVKLayout::IndirectDrawSequence), // 8
            usage, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "CullOutDraws");
        perFrame.outTransparentIndirectCommandBuffer.initialize(maxUniqueMeshes * sizeof(RendererVKLayout::IndirectDrawSequence), // 9
            usage, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "CullOutDrawsTransparent");
    }
}

void IndirectCullComputePipeline::reloadShaders()
{
    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    if (!m_computePipeline.reloadShaders(computePipelineLayout))
        printf("IndirectCullComputePipeline: shader reload failed, keeping previous pipeline\n");
}

void IndirectCullComputePipeline::buildComputeLayout(ComputePipelineLayout& computePipelineLayout)
{
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/instanced_indirect.cs.glsl";
    computePipelineLayout.computeShaderText = FileSystem::readFileStr(computePipelineLayout.computeShaderDebugFilePath);
    auto& descriptorSetBindings = computePipelineLayout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // UBO
        .binding = 0,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InRenderNodeTransformsBuffer
        .binding = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InMeshInstancesBuffer
        .binding = 2,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InMeshInstanceOffsetsBuffer
        .binding = 3,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InMeshInfoBuffer
        .binding = 4,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InFirstInstanceBuffer
        .binding = 5,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // OutMeshInstancesBuffer
        .binding = 6,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // OutMeshInstanceIndexesBuffer
        .binding = 7,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // OutIndirectCommandBuffer
        .binding = 8,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // OutTransparentIndirectCommandBuffer
        .binding = 9,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InNodePassMasksBuffer
        .binding = 10,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    for (uint32 binding = 11; binding <= 15; ++binding) // LOD selection: group idx / groups / state / node bias / stats
    {
        descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = binding,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute
        });
    }
}

void IndirectCullComputePipeline::update(uint32 frameIdx, uint32 numMeshInstances)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
   
    frameData.mappedIndirectCommands[0] = vk::DispatchIndirectCommand{ .x = numMeshInstances, .y = 1, .z = 1 };

    frameData.inIndirectCommandBuffer.flushMappedMemory(vk::WholeSize);
}

void IndirectCullComputePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];

    oc::array<DescriptorSetUpdateInfo, 16> computeDescriptorSetUpdateInfos
    {
        DescriptorSetUpdateInfo { // UBO
            .binding = 0,
            .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.ubo.getBuffer(),
                    .range = recordParams.ubo.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InRenderNodeTransformsBuffer
            .binding = 1,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inRenderNodeTransformsBuffer.getBuffer(),
                    .range = recordParams.inRenderNodeTransformsBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InMeshInstancesBuffer
            .binding = 2,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inMeshInstancesBuffer.getBuffer(),
                    .range = recordParams.inMeshInstancesBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InMeshInstanceOffsetsBuffer
            .binding = 3,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inMeshInstanceOffsetsBuffer.getBuffer(),
                    .range = recordParams.inMeshInstanceOffsetsBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InMeshInfoBuffer
            .binding = 4,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inMeshInfoBuffer.getBuffer(),
                    .range = recordParams.inMeshInfoBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InFirstInstancesBuffer
            .binding = 5,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inFirstInstancesBuffer.getBuffer(),
                    .range = recordParams.inFirstInstancesBuffer.getSize(),
                }
            }
        },

        DescriptorSetUpdateInfo { // OutMeshInstancesBuffer
            .binding = 6,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = frameData.outMeshInstancesBuffer.getBuffer(),
                    .range = frameData.outMeshInstancesBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // OutMeshInstanceIndexesBuffer
            .binding = 7,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = frameData.outMeshInstanceIndexesBuffer.getBuffer(),
                    .range = frameData.outMeshInstanceIndexesBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // OutIndirectCommandBuffer
            .binding = 8,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = frameData.outIndirectCommandBuffer.getBuffer(),
                    .range = frameData.outIndirectCommandBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // OutTransparentIndirectCommandBuffer
            .binding = 9,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = frameData.outTransparentIndirectCommandBuffer.getBuffer(),
                    .range = frameData.outTransparentIndirectCommandBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InNodePassMasksBuffer
            .binding = 10,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inNodePassMasksBuffer.getBuffer(),
                    .range = recordParams.inNodePassMasksBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InMeshLodGroupIdxBuffer
            .binding = 11,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inMeshLodGroupIdxBuffer.getBuffer(),
                    .range = recordParams.inMeshLodGroupIdxBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InMeshLodGroupsBuffer
            .binding = 12,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inMeshLodGroupsBuffer.getBuffer(),
                    .range = recordParams.inMeshLodGroupsBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // LodLevelStateBuffer
            .binding = 13,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.lodLevelStateBuffer.getBuffer(),
                    .range = recordParams.lodLevelStateBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // InNodeLodStateBiasBuffer
            .binding = 14,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inNodeLodStateBiasBuffer.getBuffer(),
                    .range = recordParams.inNodeLodStateBiasBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo { // OutLodStatsBuffer
            .binding = 15,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.outLodStatsBuffer.getBuffer(),
                    .range = recordParams.outLodStatsBuffer.getSize(),
                }
            }
        }
    };

    // Compute shader frustum cull and indirect command buffer generation
    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    {
        vk::DescriptorSet descriptorSet = recordParams.descriptorSet.getDescriptorSet();
        vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_computePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, descriptorSet, computeDescriptorSetUpdateInfos);
        vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);
        // Clear the full capacity (not just the live mesh count) so the recorded size never depends on
        // how many meshes are registered — new meshes spawn without a re-record.
        vkCommandBuffer.fillBuffer(frameData.outIndirectCommandBuffer.getBuffer(), 0, vk::WholeSize, 0); // opaque
        vkCommandBuffer.fillBuffer(frameData.outTransparentIndirectCommandBuffer.getBuffer(), 0, vk::WholeSize, 0); // transparent
        {
            vk::MemoryBarrier2 memoryBarrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eClear,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            };
            vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
        }

        vkCommandBuffer.dispatchIndirect(frameData.inIndirectCommandBuffer.getBuffer(), 0);

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

        {
            vk::MemoryBarrier2 memoryBarrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eClear | vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite,
            };
            vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
        }
    }
}
