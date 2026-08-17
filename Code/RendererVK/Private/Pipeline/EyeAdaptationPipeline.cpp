module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr uint32 NUM_BINS = 256;
}

void EyeAdaptationPipeline::buildHistogramLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/eyeadapt_histogram.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(HistogramPC) });
}

void EyeAdaptationPipeline::buildReduceLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/eyeadapt_reduce.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ReducePC) });
}

void EyeAdaptationPipeline::initialize()
{
    { ComputePipelineLayout layout; buildHistogramLayout(layout); m_histogramPipeline.initialize(layout); }
    { ComputePipelineLayout layout; buildReduceLayout(layout); m_reducePipeline.initialize(layout); }
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_histogramSets[i].initialize(m_histogramPipeline.getDescriptorSetLayout());
        m_reduceSets[i].initialize(m_reducePipeline.getDescriptorSetLayout());
    }

    m_histogramBuffer.initialize(NUM_BINS * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "EyeAdaptHistogram");
    m_adaptBuffer.initialize(2 * sizeof(float),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "EyeAdaptState");
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_paramsBuffer[i].initialize(sizeof(GpuParams), vk::BufferUsageFlagBits2::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "EyeAdaptParams", BufferHostAccess::eSequentialWrite);
        m_mappedParams[i] = m_paramsBuffer[i].mapMemory<GpuParams>();
    }

    // Zero the persistent adaptation state (avgLum = 0 -> the reduce shader snaps on the first frame).
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    cmd.fillBuffer(m_adaptBuffer.getBuffer(), 0, vk::WholeSize, 0u);
    init.end();
    init.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();
}

void EyeAdaptationPipeline::reloadShaders()
{
    { ComputePipelineLayout layout; buildHistogramLayout(layout); if (!m_histogramPipeline.reloadShaders(layout)) printf("EyeAdaptationPipeline: histogram shader reload failed\n"); }
    { ComputePipelineLayout layout; buildReduceLayout(layout); if (!m_reducePipeline.reloadShaders(layout)) printf("EyeAdaptationPipeline: reduce shader reload failed\n"); }
}

void EyeAdaptationPipeline::updateParams(uint32 frameIdx, const PostParams& post, float deltaSeconds)
{
    const float logLumRange = oc::max(post.adaptMaxLogLum - post.adaptMinLogLum, 1e-3f);
    m_mappedParams[frameIdx][0] = GpuParams{
        .minLogLum = post.adaptMinLogLum,
        .invLogLumRange = 1.0f / logLumRange,
        .logLumRange = logLumRange,
        .dt = deltaSeconds,
        .tau = post.adaptTau,
        .key = post.adaptKey,
        .minExposure = exp2f(post.adaptMinEV),
        .maxExposure = exp2f(post.adaptMaxEV),
    };
}

void EyeAdaptationPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 pixelCount = (uint32)oc::max(params.viewportSize.x * params.viewportSize.y, 1);
    auto paramsInfo = vk::DescriptorBufferInfo{ .buffer = m_paramsBuffer[frameIdx].getBuffer(), .range = vk::WholeSize };

    // Clear the histogram, then make the clear visible to the build pass.
    cmd.fillBuffer(m_histogramBuffer.getBuffer(), 0, vk::WholeSize, 0u);
    {
        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    }

    // Pass 1: build histogram.
    {
        DescriptorSet& set = m_histogramSets[frameIdx];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        auto histInfo = vk::DescriptorBufferInfo{ .buffer = m_histogramBuffer.getBuffer(), .range = vk::WholeSize };
        oc::array<DescriptorSetUpdateInfo, 3> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.sampler, .imageView = params.resolvedView, .imageLayout = params.resolvedLayout } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { histInfo } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { paramsInfo } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_histogramPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_histogramPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_histogramPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        HistogramPC pc{ .vpMin = params.viewportMin, .vpSize = params.viewportSize };
        cmd.pushConstants(m_histogramPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        const uint32 gx = ((uint32)params.viewportSize.x + 15) / 16;
        const uint32 gy = ((uint32)params.viewportSize.y + 15) / 16;
        cmd.dispatch(gx, gy, 1);
    }

    // histogram write -> reduce read
    {
        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    }

    // Pass 2: reduce to an adapted exposure.
    {
        DescriptorSet& set = m_reduceSets[frameIdx];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        auto histInfo = vk::DescriptorBufferInfo{ .buffer = m_histogramBuffer.getBuffer(), .range = vk::WholeSize };
        auto adaptInfo = vk::DescriptorBufferInfo{ .buffer = m_adaptBuffer.getBuffer(), .range = vk::WholeSize };
        oc::array<DescriptorSetUpdateInfo, 3> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { histInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { adaptInfo } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { paramsInfo } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_reducePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_reducePipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_reducePipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        ReducePC pc{ .pixelCount = pixelCount };
        cmd.pushConstants(m_reducePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(1, 1, 1);
    }

    // adapt write -> composite fragment read
    {
        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader, .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    }
}
