module RendererVK;

import Core;
import :VK;
import :Device;
import :Shader;
import :RenderPass;

// Width of every rasterized line: the debug-line pass (line topology) and the wireframe variants
// (eLine polygon mode). Needs the wideLines device feature, enabled in Device.cpp.
static constexpr float LINE_WIDTH = 3.0f;

static bool rasterizesLines(vk::PrimitiveTopology topology, vk::PolygonMode polygonMode)
{
    return polygonMode == vk::PolygonMode::eLine
        || topology == vk::PrimitiveTopology::eLineList
        || topology == vk::PrimitiveTopology::eLineStrip
        || topology == vk::PrimitiveTopology::eLineListWithAdjacency
        || topology == vk::PrimitiveTopology::eLineStripWithAdjacency;
}

// The fragment shader's file name (it names the pass - the vertex shader is often a shared one like
// composite.vs.glsl); a depth-only pass without a fragment shader takes its vertex shader.
static oc::string pipelineDebugName(const ShaderSource& vs, const ShaderSource& fs)
{
    return Shader::debugName(fs.text.empty() ? vs.debugFilePath : fs.debugFilePath);
}

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
    const oc::string debugName = pipelineDebugName(layout.vertexShader, layout.fragmentShader);
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
    Globals::device.setDebugName(m_descriptorSetLayout, debugName.c_str());

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
    Globals::device.setDebugName(m_pipelineLayout, debugName.c_str());

    auto createPipelineCacheResult = vkDevice.createPipelineCache(vk::PipelineCacheCreateInfo());
    if (createPipelineCacheResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create pipeline cache");
        return false;
    }
    m_pipelineCache = createPipelineCacheResult.value;
    Globals::device.setDebugName(m_pipelineCache, debugName.c_str());

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
    const bool hasTess = !layout.tessControlShader.text.empty() && !layout.tessEvalShader.text.empty();

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
        .topology = hasTess ? vk::PrimitiveTopology::ePatchList : layout.topology,
        .primitiveRestartEnable = vk::False,
    };
    vk::PipelineTessellationDomainOriginStateCreateInfo tessDomainOrigin{ .domainOrigin = vk::TessellationDomainOrigin::eLowerLeft };
    vk::PipelineTessellationStateCreateInfo pipelineTessellationStateCreateInfo{
        .pNext = &tessDomainOrigin,
        .patchControlPoints = layout.patchControlPoints,
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
        .lineWidth = rasterizesLines(layout.topology, layout.polygonMode) ? LINE_WIDTH : 1.0f,
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
    if (!layout.colorWriteAlpha)
        colorComponentFlags &= ~vk::ColorComponentFlags(vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState
    {
        .blendEnable = layout.blendEnable ? vk::True : vk::False,
        .srcColorBlendFactor = layout.srcColorBlendFactor,
        .dstColorBlendFactor = layout.dstColorBlendFactor,
        .colorBlendOp = layout.colorBlendOp,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = layout.colorBlendOp == vk::BlendOp::eAdd ? vk::BlendOp::eAdd : layout.colorBlendOp,
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
    // VS, then (tess) TCS + TES, then FS: fsSlot is where the fragment stage sits.
    oc::array<vk::PipelineShaderStageCreateInfo, 4> pipelineShaderStageCreateInfos =
    {
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eVertex,   .pName = "main", },
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eFragment, .pName = "main", },
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eFragment, .pName = "main", },
        vk::PipelineShaderStageCreateInfo {.stage = vk::ShaderStageFlagBits::eFragment, .pName = "main", },
    };
    const uint32 fsSlot = hasTess ? 3u : 1u;
    if (hasTess)
    {
        pipelineShaderStageCreateInfos[1].stage = vk::ShaderStageFlagBits::eTessellationControl;
        pipelineShaderStageCreateInfos[2].stage = vk::ShaderStageFlagBits::eTessellationEvaluation;
    }
    const bool hasFS = !layout.depthOnly || !layout.fragmentShader.text.empty();
    vk::PipelineCreateFlags2CreateInfo pipelineFlags2{ .flags = layout.indirectBindable ? vk::PipelineCreateFlagBits2::eIndirectBindableEXT : vk::PipelineCreateFlags2{} };
    if (Globals::device.capturePipelineStatistics())
        pipelineFlags2.flags |= vk::PipelineCreateFlagBits2::eCaptureStatisticsKHR;
    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
    {
        .pNext = &pipelineFlags2,
        .flags = {},
        // Depth-only passes may still carry a fragment shader (e.g. alpha-masked shadow discard); they
        // simply write no color attachments.
        .stageCount = fsSlot + (hasFS ? 1u : 0u),
        .pStages = pipelineShaderStageCreateInfos.data(),
        .pVertexInputState = &pipelineVertexInputStateCreateInfo,
        .pInputAssemblyState = &pipelineInputAssemblyStateCreateInfo,
        .pTessellationState = hasTess ? &pipelineTessellationStateCreateInfo : nullptr,
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

    Shader defaultVS, defaultFS, tessControl, tessEval;
    if (!defaultVS.initialize(vk::ShaderStageFlagBits::eVertex, layout.vertexShader.text, layout.vertexShader.debugFilePath, layout.vertexShader.defines, assertOnFailure))
        return false;
    if (hasFS &&
        !defaultFS.initialize(vk::ShaderStageFlagBits::eFragment, layout.fragmentShader.text, layout.fragmentShader.debugFilePath, layout.fragmentShader.defines, assertOnFailure))
        return false;
    if (hasTess)
    {
        if (!tessControl.initialize(vk::ShaderStageFlagBits::eTessellationControl, layout.tessControlShader.text, layout.tessControlShader.debugFilePath, layout.tessControlShader.defines, assertOnFailure))
            return false;
        if (!tessEval.initialize(vk::ShaderStageFlagBits::eTessellationEvaluation, layout.tessEvalShader.text, layout.tessEvalShader.debugFilePath, layout.tessEvalShader.defines, assertOnFailure))
            return false;
        pipelineShaderStageCreateInfos[1].module = tessControl.getModule();
        pipelineShaderStageCreateInfos[2].module = tessEval.getModule();
    }

    // Compile per-variant shader overrides; entries with no override keep a null module.
    const size_t additionalCount = layout.additionalVariants.size();
    oc::vector<Shader> overrideVS(additionalCount);
    oc::vector<Shader> overrideFS(additionalCount);
    oc::vector<Shader> overrideTES(additionalCount);
    for (size_t i = 0; i < additionalCount; i++)
    {
        const PipelineVariant& v = layout.additionalVariants[i];
        if (!v.vertexShader.text.empty())
            if (!overrideVS[i].initialize(vk::ShaderStageFlagBits::eVertex, v.vertexShader.text, v.vertexShader.debugFilePath, v.vertexShader.defines, assertOnFailure))
                return false;
        if (!v.fragmentShader.text.empty())
            if (!overrideFS[i].initialize(vk::ShaderStageFlagBits::eFragment, v.fragmentShader.text, v.fragmentShader.debugFilePath, v.fragmentShader.defines, assertOnFailure))
                return false;
        if (hasTess && !v.tessEvalShader.text.empty())
            if (!overrideTES[i].initialize(vk::ShaderStageFlagBits::eTessellationEvaluation, v.tessEvalShader.text, v.tessEvalShader.debugFilePath, v.tessEvalShader.defines, assertOnFailure))
                return false;
    }

    // Variant 0: both defaults, no blending, depth write on.
    outPipelines.reserve(1 + additionalCount);
    pipelineShaderStageCreateInfos[0].module = defaultVS.getModule();
    pipelineShaderStageCreateInfos[fsSlot].module = defaultFS.getModule();
    {
        vk::Result   result;
        vk::Pipeline pipeline;
        std::tie(result, pipeline) = vkDevice.createGraphicsPipeline(m_pipelineCache, graphicsPipelineCreateInfo);
        if (result != vk::Result::eSuccess)
        {
            assert((!assertOnFailure) && "Failed to create graphics pipeline");
            return false;
        }
        const oc::string debugName = pipelineDebugName(layout.vertexShader, layout.fragmentShader);
        Globals::device.setDebugName(pipeline, debugName.c_str());
        Globals::device.logPipelineStatistics(pipeline, debugName.c_str());
        outPipelines.push_back(pipeline);
    }

    for (size_t i = 0; i < additionalCount; i++)
    {
        const PipelineVariant& variant = layout.additionalVariants[i];
        pipelineShaderStageCreateInfos[0].module = overrideVS[i].getModule() ? overrideVS[i].getModule() : defaultVS.getModule();
        pipelineShaderStageCreateInfos[fsSlot].module = overrideFS[i].getModule() ? overrideFS[i].getModule() : defaultFS.getModule();
        if (hasTess)
            pipelineShaderStageCreateInfos[2].module = overrideTES[i].getModule() ? overrideTES[i].getModule() : tessEval.getModule();

        // Per-variant blend/depth/raster state (mutated in place; the create info points at these structs).
        pipelineDepthStencilStateCreateInfo.depthTestEnable = variant.depthTest ? vk::True : vk::False;
        pipelineDepthStencilStateCreateInfo.depthWriteEnable = variant.depthWrite ? vk::True : vk::False;
        pipelineDepthStencilStateCreateInfo.depthCompareOp = variant.depthEqual ? vk::CompareOp::eEqual : layout.depthCompareOp;
        pipelineColorBlendAttachmentState.blendEnable = variant.blendEnable ? vk::True : vk::False;
        // A blended variant KEEPS the dst alpha (the opaque surface behind it owns the scene colour's
        // alpha = TAA's ocean flag; a near-zero material alpha must not read as ocean), unless it composites
        // the alpha itself (dualSourceAlpha: the ocean's edge).
        const bool blendsAlpha = variant.dualSourceBlend && variant.dualSourceAlpha;
        pipelineColorBlendAttachmentState.colorWriteMask = variant.blendEnable && !blendsAlpha
            ? colorComponentFlags & ~vk::ColorComponentFlags(vk::ColorComponentFlagBits::eA) : colorComponentFlags;
        pipelineRasterizationStateCreateInfo.polygonMode = variant.polygonMode;
        pipelineRasterizationStateCreateInfo.lineWidth = rasterizesLines(layout.topology, variant.polygonMode) ? LINE_WIDTH : 1.0f;
        pipelineRasterizationStateCreateInfo.cullMode = variant.cullMode;
        if (variant.blendEnable)
        {
            // Standard "over" alpha blending: src.rgb*src.a + dst.rgb*(1-src.a), keep dst alpha. Dual-source:
            // src0.rgb + dst.rgb*src1.rgb (see PipelineVariant::dualSourceBlend).
            pipelineColorBlendAttachmentState.srcColorBlendFactor = variant.dualSourceBlend ? vk::BlendFactor::eOne : vk::BlendFactor::eSrcAlpha;
            pipelineColorBlendAttachmentState.dstColorBlendFactor = variant.dualSourceBlend ? vk::BlendFactor::eSrc1Color : vk::BlendFactor::eOneMinusSrcAlpha;
            pipelineColorBlendAttachmentState.colorBlendOp = vk::BlendOp::eAdd;
            pipelineColorBlendAttachmentState.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            pipelineColorBlendAttachmentState.dstAlphaBlendFactor = blendsAlpha ? vk::BlendFactor::eSrc1Alpha : vk::BlendFactor::eZero;
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
        const ShaderSource& vs = variant.vertexShader.text.empty() ? layout.vertexShader : variant.vertexShader;
        const ShaderSource& fs = variant.fragmentShader.text.empty() ? layout.fragmentShader : variant.fragmentShader;
        const oc::string debugName = oc::format("{} #{}", pipelineDebugName(vs, fs), i + 1);
        Globals::device.setDebugName(pipeline, debugName.c_str());
        Globals::device.logPipelineStatistics(pipeline, debugName.c_str());
        outPipelines.push_back(pipeline);
    }
    return true;
}