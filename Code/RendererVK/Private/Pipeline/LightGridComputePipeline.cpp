module RendererVK;

import Core;
import Core.glm;
import File;
import :Allocator;
import :Layout;

void LightGridComputePipeline::initialize()
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.inIndirectCommandBuffer.initialize(sizeof(vk::DispatchIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightGridCmd", BufferHostAccess::eSequentialWrite);
        perFrame.mappedIndirectCommands = perFrame.inIndirectCommandBuffer.mapMemory<vk::DispatchIndirectCommand>();
    }

    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    m_computePipeline.initialize(computePipelineLayout);
}

void LightGridComputePipeline::reloadShaders()
{
    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    if (!m_computePipeline.reloadShaders(computePipelineLayout))
        printf("LightGridComputePipeline: shader reload failed, keeping previous pipeline\n");
}

void LightGridComputePipeline::buildComputeLayout(ComputePipelineLayout& computePipelineLayout)
{
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/light_grid.cs.glsl";
    computePipelineLayout.computeShaderText = FileSystem::readFileStr(computePipelineLayout.computeShaderDebugFilePath);
    auto& descriptorSetBindings = computePipelineLayout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // UBO
        .binding = 0,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 2,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 3,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
}

void LightGridComputePipeline::update(uint32 frameIdx, uint32 numLights)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];

    frameData.mappedIndirectCommands[0] = vk::DispatchIndirectCommand{ .x = numLights, .y = 1, .z = 1 };
    frameData.inIndirectCommandBuffer.flushMappedMemory(vk::WholeSize);
}

void LightGridComputePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams)
{
    oc::array<DescriptorSetUpdateInfo, 4> computeDescriptorSetUpdateInfos
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
        DescriptorSetUpdateInfo {
            .binding = 1,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.inLightInfoBuffer.getBuffer(),
                    .range = recordParams.inLightInfoBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo {
            .binding = 2,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.outLightGridBuffer.getBuffer(),
                    .range = recordParams.outLightGridBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo {
            .binding = 3,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = recordParams.outLightTableBuffer.getBuffer(),
                    .range = recordParams.outLightTableBuffer.getSize(),
                }
            }
        }
    };

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    {
        vk::DescriptorSet descriptorSet = recordParams.descriptorSet.getDescriptorSet();
        vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_computePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, descriptorSet, computeDescriptorSetUpdateInfos);
        vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);
        vkCommandBuffer.fillBuffer(recordParams.outLightTableBuffer.getBuffer(), 0, 8, 0);
        vkCommandBuffer.fillBuffer(recordParams.outLightTableBuffer.getBuffer(), 8, 4, recordParams.numTableEntries);
        vkCommandBuffer.fillBuffer(recordParams.outLightTableBuffer.getBuffer(), 12, vk::WholeSize, 0xFFFFFFFF);
        vkCommandBuffer.fillBuffer(recordParams.outLightGridBuffer.getBuffer(), 0, vk::WholeSize, 0);

        {
            vk::MemoryBarrier2 memoryBarrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead,
            };
            vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
        }

        vkCommandBuffer.dispatchIndirect(m_perFrameData[frameIdx].inIndirectCommandBuffer.getBuffer(), 0);

        {
            vk::MemoryBarrier2 memoryBarrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
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
