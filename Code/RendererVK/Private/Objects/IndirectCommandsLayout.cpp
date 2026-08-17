module RendererVK;

import Core;
import :Device;
import :Layout;

IndirectCommandsLayout::IndirectCommandsLayout() {}
IndirectCommandsLayout::~IndirectCommandsLayout()
{
    destroy();
}

bool IndirectCommandsLayout::initialize(vk::PipelineLayout pipelineLayout, vk::ShaderStageFlags shaderStages, bool useExecutionSet)
{
    // Token 0 (optional): select the pipeline variant for this sequence from the Indirect Execution Set.
    vk::IndirectCommandsExecutionSetTokenEXT executionSetToken{
        .type = vk::IndirectExecutionSetInfoTypeEXT::ePipelines,
        .shaderStages = shaderStages,
    };
    vk::IndirectCommandsTokenDataEXT executionSetTokenData;
    executionSetTokenData.pExecutionSet = &executionSetToken;

    // Always present: DRAW_INDEXED carries no token data struct; its buffer payload is a VkDrawIndexedIndirectCommand.
    oc::vector<vk::IndirectCommandsLayoutTokenEXT> tokens;
    if (useExecutionSet)
    {
        tokens.push_back(vk::IndirectCommandsLayoutTokenEXT{
            .type = vk::IndirectCommandsTokenTypeEXT::eExecutionSet,
            .data = executionSetTokenData,
            .offset = offsetof(RendererVKLayout::IndirectDrawSequence, pipelineIndex),
        });
    }
    tokens.push_back(vk::IndirectCommandsLayoutTokenEXT{
        .type = vk::IndirectCommandsTokenTypeEXT::eDrawIndexed,
        .offset = offsetof(RendererVKLayout::IndirectDrawSequence, indexCount),
    });

    m_indirectStride = (uint32)sizeof(RendererVKLayout::IndirectDrawSequence);

    vk::IndirectCommandsLayoutCreateInfoEXT createInfo{
        .shaderStages = shaderStages,
        .indirectStride = m_indirectStride,
        .pipelineLayout = pipelineLayout,
        .tokenCount = (uint32)tokens.size(),
        .pTokens = tokens.data(),
    };
    auto createResult = Globals::device.getDevice().createIndirectCommandsLayoutEXT(createInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create indirect commands layout");
        return false;
    }
    m_layout = createResult.value;
    return true;
}

void IndirectCommandsLayout::destroy()
{
    if (m_layout)
    {
        Globals::device.getDevice().destroyIndirectCommandsLayoutEXT(m_layout);
        m_layout = nullptr;
    }
}
