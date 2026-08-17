module RendererVK;

import Core;
import :VK;
import :Device;
import :Shader;
import :RenderPass;

GraphicsPipeline::GraphicsPipeline() {}
GraphicsPipeline::~GraphicsPipeline()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (vk::Pipeline pipeline : m_pipelines)
        vkDevice.destroyPipeline(pipeline);
    if (m_pipelineCache)
        vkDevice.destroyPipelineCache(m_pipelineCache);
    if (m_pipelineLayout)
        vkDevice.destroyPipelineLayout(m_pipelineLayout);
    if (m_descriptorSetLayout)
        vkDevice.destroyDescriptorSetLayout(m_descriptorSetLayout);
}

bool GraphicsPipeline::initialize(const RenderPass& renderPass, GraphicsPipelineLayout& layout)
{
    return initialize(renderPass.getRenderPass(), layout);
}

bool GraphicsPipeline::reloadShaders(const RenderPass& renderPass, GraphicsPipelineLayout& layout)
{
    return reloadShaders(renderPass.getRenderPass(), layout);
}

bool GraphicsPipeline::initialize(vk::RenderPass renderPass, GraphicsPipelineLayout& layout)
{
    vk::Device vkDevice = Globals::device.getDevice();
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
        layoutInfo.flags |= vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
    }
    auto createLayoutResult = vkDevice.createDescriptorSetLayout(layoutInfo);
    if (createLayoutResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create descriptor set layout");
        return false;
    }
	m_descriptorSetLayout = createLayoutResult.value;

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo
    {
        .flags = {},
        .setLayoutCount = 1,
        .pSetLayouts = &m_descriptorSetLayout,
        .pushConstantRangeCount = (uint32)layout.pushConstantRanges.size(),
        .pPushConstantRanges = layout.pushConstantRanges.data(),
    };
    auto createPipelineLayoutResult = vkDevice.createPipelineLayout(pipelineLayoutCreateInfo);
    if (createPipelineLayoutResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create pipeline layout");
        return false;
    }
    m_pipelineLayout = createPipelineLayoutResult.value;

    auto createPipelineCacheResult = vkDevice.createPipelineCache(vk::PipelineCacheCreateInfo());
    if (createPipelineCacheResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create pipeline cache");
        return false;
    }
    m_pipelineCache = createPipelineCacheResult.value;

    return createPipelines(renderPass, layout, m_pipelines, true);
}

bool GraphicsPipeline::reloadShaders(vk::RenderPass renderPass, GraphicsPipelineLayout& layout)
{
    vk::Device vkDevice = Globals::device.getDevice();

    oc::vector<vk::Pipeline> newPipelines;
    if (!createPipelines(renderPass, layout, newPipelines, false))
    {
        for (vk::Pipeline pipeline : newPipelines)
            if (pipeline)
                vkDevice.destroyPipeline(pipeline);
        return false;
    }

    for (vk::Pipeline pipeline : m_pipelines)
        vkDevice.destroyPipeline(pipeline);
    m_pipelines = oc::move(newPipelines);
    return true;
}

bool GraphicsPipeline::createPipelines(vk::RenderPass renderPass, GraphicsPipelineLayout& layout, oc::vector<vk::Pipeline>& outPipelines, bool assertOnFailure)
{
    vk::Device vkDevice = Globals::device.getDevice();

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo
    {
        .flags = {},
        .vertexBindingDescriptionCount = (uint32)layout.vertexLayoutInfo.bindingDescriptions.size(),
        .pVertexBindingDescriptions = layout.vertexLayoutInfo.bindingDescriptions.data(),
        .vertexAttributeDescriptionCount = (uint32)layout.vertexLayoutInfo.attributeDescriptions.size(),
        .pVertexAttributeDescriptions = layout.vertexLayoutInfo.attributeDescriptions.data(),
    };
    vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo
    {
        .flags = {},
        .topology = layout.topology,
        .primitiveRestartEnable = vk::False,
    };
    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo
    {
        .flags = {},
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr,
    };
    vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
    {
        .flags = {},
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = layout.polygonMode,
        .cullMode = layout.cullMode,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = layout.depthBiasEnable ? vk::True : vk::False,
        .depthBiasConstantFactor = layout.depthBiasConstantFactor,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = layout.depthBiasSlopeFactor,
        .lineWidth = 1.0f,
    };
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo
    {
        .flags = {},
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = vk::False,
        .minSampleShading = 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = vk::False,
        .alphaToOneEnable = vk::False,
    };
    vk::StencilOpState stencilOpState
    {
        .failOp = vk::StencilOp::eKeep,
        .passOp = vk::StencilOp::eKeep,
        .depthFailOp = vk::StencilOp::eKeep,
        .compareOp = vk::CompareOp::eAlways,
        .compareMask = 0,
        .writeMask = 0,
        .reference = 0,
    };
    vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo{
        .flags = {},
        .depthTestEnable = layout.depthTestEnable ? vk::True : vk::False,
        .depthWriteEnable = layout.depthWriteEnable ? vk::True : vk::False,
        .depthCompareOp = layout.depthCompareOp,
        .depthBoundsTestEnable = vk::False,
        .stencilTestEnable = vk::False,
        //	.front = stencilOpState,
        //	.back = stencilOpState,
        //	.minDepthBounds = 0.0f,
        //	.maxDepthBounds = 0.0f,
    };

    vk::ColorComponentFlags colorComponentFlags = vk::FlagTraits<vk::ColorComponentFlagBits>::allFlags;
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState
    {
        .blendEnable = layout.blendEnable ? vk::True : vk::False,
        .srcColorBlendFactor = layout.srcColorBlendFactor,
        .dstColorBlendFactor = layout.dstColorBlendFactor,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = colorComponentFlags,
    };
    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
    {
        .flags = {},
        .logicOpEnable = vk::False,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = layout.depthOnly ? 0u : 1u,
        .pAttachments = layout.depthOnly ? nullptr : &pipelineColorBlendAttachmentState,
        .blendConstants = { { 1.0f, 1.0f, 1.0f, 1.0f } },
    };
    oc::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo
    {
        .flags = {},
        .dynamicStateCount = static_cast<uint32>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    oc::array<vk::PipelineShaderStageCreateInfo, 2> pipelineShaderStageCreateInfos =
    {
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eVertex,   .pName = "main", },
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eFragment, .pName = "main", },
    };
    vk::PipelineCreateFlags2CreateInfo pipelineFlags2{ .flags = vk::PipelineCreateFlagBits2::eIndirectBindableEXT };
    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
    {
        .pNext = &pipelineFlags2,
        .flags = {},
        // Depth-only passes may still carry a fragment shader (e.g. alpha-masked shadow discard); they
        // simply write no color attachments.
        .stageCount = (!layout.depthOnly || !layout.fragmentShader.text.empty()) ? 2u : 1u,
        .pStages = pipelineShaderStageCreateInfos.data(),
        .pVertexInputState = &pipelineVertexInputStateCreateInfo,
        .pInputAssemblyState = &pipelineInputAssemblyStateCreateInfo,
        .pTessellationState = nullptr,
        .pViewportState = &pipelineViewportStateCreateInfo,
        .pRasterizationState = &pipelineRasterizationStateCreateInfo,
        .pMultisampleState = &pipelineMultisampleStateCreateInfo,
        .pDepthStencilState = &pipelineDepthStencilStateCreateInfo,
        .pColorBlendState = &pipelineColorBlendStateCreateInfo,
        .pDynamicState = &pipelineDynamicStateCreateInfo,
        .layout = m_pipelineLayout,
        .renderPass = renderPass,
        .subpass = 0,
        .basePipelineHandle = nullptr,
        .basePipelineIndex = 0,
    };

    Shader defaultVS, defaultFS;
    if (!defaultVS.initialize(vk::ShaderStageFlagBits::eVertex, layout.vertexShader.text, layout.vertexShader.debugFilePath, layout.vertexShader.defines, assertOnFailure))
        return false;
    if ((!layout.depthOnly || !layout.fragmentShader.text.empty()) &&
        !defaultFS.initialize(vk::ShaderStageFlagBits::eFragment, layout.fragmentShader.text, layout.fragmentShader.debugFilePath, layout.fragmentShader.defines, assertOnFailure))
        return false;

    // Compile per-variant shader overrides; entries with no override keep a null module.
    const size_t additionalCount = layout.additionalVariants.size();
    oc::vector<Shader> overrideVS(additionalCount);
    oc::vector<Shader> overrideFS(additionalCount);
    for (size_t i = 0; i < additionalCount; i++)
    {
        const PipelineVariant& v = layout.additionalVariants[i];
        if (!v.vertexShader.text.empty())
            if (!overrideVS[i].initialize(vk::ShaderStageFlagBits::eVertex, v.vertexShader.text, v.vertexShader.debugFilePath, v.vertexShader.defines, assertOnFailure))
                return false;
        if (!v.fragmentShader.text.empty())
            if (!overrideFS[i].initialize(vk::ShaderStageFlagBits::eFragment, v.fragmentShader.text, v.fragmentShader.debugFilePath, v.fragmentShader.defines, assertOnFailure))
                return false;
    }

    // Variant 0: both defaults, no blending, depth write on.
    outPipelines.reserve(1 + additionalCount);
    pipelineShaderStageCreateInfos[0].module = defaultVS.getModule();
    pipelineShaderStageCreateInfos[1].module = defaultFS.getModule();
    {
        vk::Result   result;
        vk::Pipeline pipeline;
        std::tie(result, pipeline) = vkDevice.createGraphicsPipeline(m_pipelineCache, graphicsPipelineCreateInfo);
        if (result != vk::Result::eSuccess)
        {
            assert((!assertOnFailure) && "Failed to create graphics pipeline");
            return false;
        }
        outPipelines.push_back(pipeline);
    }

    for (size_t i = 0; i < additionalCount; i++)
    {
        const PipelineVariant& variant = layout.additionalVariants[i];
        pipelineShaderStageCreateInfos[0].module = overrideVS[i].getModule() ? overrideVS[i].getModule() : defaultVS.getModule();
        pipelineShaderStageCreateInfos[1].module = overrideFS[i].getModule() ? overrideFS[i].getModule() : defaultFS.getModule();

        // Per-variant blend/depth/raster state (mutated in place; the create info points at these structs).
        pipelineDepthStencilStateCreateInfo.depthTestEnable = variant.depthTest ? vk::True : vk::False;
        pipelineDepthStencilStateCreateInfo.depthWriteEnable = variant.depthWrite ? vk::True : vk::False;
        pipelineColorBlendAttachmentState.blendEnable = variant.blendEnable ? vk::True : vk::False;
        pipelineRasterizationStateCreateInfo.polygonMode = variant.polygonMode;
        pipelineRasterizationStateCreateInfo.cullMode = variant.cullMode;
        if (variant.blendEnable)
        {
            // Standard "over" alpha blending: src.rgb*src.a + dst.rgb*(1-src.a), keep dst alpha.
            pipelineColorBlendAttachmentState.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
            pipelineColorBlendAttachmentState.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            pipelineColorBlendAttachmentState.colorBlendOp = vk::BlendOp::eAdd;
            pipelineColorBlendAttachmentState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            pipelineColorBlendAttachmentState.dstAlphaBlendFactor = vk::BlendFactor::eZero;
            pipelineColorBlendAttachmentState.alphaBlendOp = vk::BlendOp::eAdd;
        }

        vk::Result   result;
        vk::Pipeline pipeline;
        std::tie(result, pipeline) = vkDevice.createGraphicsPipeline(m_pipelineCache, graphicsPipelineCreateInfo);
        if (result != vk::Result::eSuccess)
        {
            assert((!assertOnFailure) && "Failed to create graphics pipeline");
            return false;
        }
        outPipelines.push_back(pipeline);
    }
    return true;
}