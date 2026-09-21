module RendererVK;

import :Device;
import :Shader;

ComputePipeline::~ComputePipeline()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_pipeline)
        vkDevice.destroyPipeline(m_pipeline);
    if (m_pipelineCache)
        vkDevice.destroyPipelineCache(m_pipelineCache);
    if (m_pipelineLayout)
        vkDevice.destroyPipelineLayout(m_pipelineLayout);
    if (m_descriptorSetLayout)
        vkDevice.destroyDescriptorSetLayout(m_descriptorSetLayout);
}

bool ComputePipeline::initialize(const ComputePipelineLayout& layout)
{
    vk::Device vkDevice = Globals::device.getDevice();
    const oc::string debugName = Shader::debugName(layout.computeShaderDebugFilePath);
    vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
        .bindingCount = (uint32)layout.descriptorBindingFlags.size(),
        .pBindingFlags = layout.descriptorBindingFlags.data(),
    };
    vk::DescriptorSetLayoutCreateInfo layoutInfo
    {
        .bindingCount = (uint32)layout.descriptorSetLayoutBindings.size(),
        .pBindings = layout.descriptorSetLayoutBindings.data(),
    };
    if (!layout.descriptorBindingFlags.empty())
    {
        layoutInfo.pNext = &bindingFlagsInfo;
        // Same contract as GraphicsPipeline: any binding flags imply the layout may carry
        // UPDATE_AFTER_BIND bindings (the pool is created with eUpdateAfterBind).
        layoutInfo.flags |= vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
    }
    auto layoutResult = vkDevice.createDescriptorSetLayout(layoutInfo);
    if (layoutResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create descriptor set layout");
        return false;
    }
    m_descriptorSetLayout = layoutResult.value;
    Globals::device.setDebugName(m_descriptorSetLayout, debugName.c_str());

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo
    {
        .flags = {},
        .setLayoutCount = 1,
        .pSetLayouts = &m_descriptorSetLayout,
        .pushConstantRangeCount = (uint32)layout.pushConstantRanges.size(),
        .pPushConstantRanges = layout.pushConstantRanges.data(),
    };
    auto pipelineLayoutResult = vkDevice.createPipelineLayout(pipelineLayoutCreateInfo);
    if (pipelineLayoutResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create pipeline layout");
        return false;
    }
    m_pipelineLayout = pipelineLayoutResult.value;
    Globals::device.setDebugName(m_pipelineLayout, debugName.c_str());

    auto pipelineCacheResult = vkDevice.createPipelineCache(vk::PipelineCacheCreateInfo());
    if (pipelineCacheResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create pipeline cache");
        return false;
    }
    m_pipelineCache = pipelineCacheResult.value;
    Globals::device.setDebugName(m_pipelineCache, debugName.c_str());

    return createPipeline(layout, m_pipeline, true);
}

bool ComputePipeline::reloadShaders(const ComputePipelineLayout& layout)
{
    vk::Pipeline newPipeline;
    if (!createPipeline(layout, newPipeline, false))
        return false;

    if (m_pipeline)
        Globals::device.getDevice().destroyPipeline(m_pipeline);
    m_pipeline = newPipeline;
    return true;
}

bool ComputePipeline::createPipeline(const ComputePipelineLayout& layout, vk::Pipeline& outPipeline, bool assertOnFailure)
{
    vk::Device vkDevice = Globals::device.getDevice();

    Shader computeShader;
    if (!computeShader.initialize(vk::ShaderStageFlagBits::eCompute, layout.computeShaderText, layout.computeShaderDebugFilePath, layout.defines, assertOnFailure))
        return false;

    vk::SpecializationInfo specializationInfo
    {
        .mapEntryCount = 0,
        .pMapEntries = nullptr,
        .dataSize = 0,
        .pData = nullptr,
    };
    vk::ComputePipelineCreateInfo pipelineCreateInfo
    {
        .flags = Globals::device.capturePipelineStatistics() ? vk::PipelineCreateFlags(vk::PipelineCreateFlagBits::eCaptureStatisticsKHR) : vk::PipelineCreateFlags{},
        .stage =
        {
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = computeShader.getModule(),
            .pName = "main",
            .pSpecializationInfo = &specializationInfo,
        },
        .layout = m_pipelineLayout,
        .basePipelineHandle = nullptr,
        .basePipelineIndex = -1,
    };
    auto pipelineResult = vkDevice.createComputePipeline(m_pipelineCache, pipelineCreateInfo);
    if (pipelineResult.result != vk::Result::eSuccess)
    {
        assert((!assertOnFailure) && "Failed to create compute pipeline");
        return false;
    }
    outPipeline = pipelineResult.value;
    const oc::string debugName = Shader::debugName(layout.computeShaderDebugFilePath);
    Globals::device.setDebugName(outPipeline, debugName.c_str());
    Globals::device.logPipelineStatistics(outPipeline, debugName.c_str());
    return true;
}