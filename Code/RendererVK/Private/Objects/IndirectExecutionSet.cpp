module RendererVK;

import Core;
import :Device;
import :GraphicsPipeline;

IndirectExecutionSet::IndirectExecutionSet() {}
IndirectExecutionSet::~IndirectExecutionSet()
{
    destroy();
}

bool IndirectExecutionSet::initialize(const GraphicsPipeline& pipeline)
{
    const uint32 variantCount = pipeline.getPipelineVariantCount();
    assert(variantCount > 0 && "Cannot build an indirect execution set without pipelines");

    vk::Device vkDevice = Globals::device.getDevice();

    // The initial pipeline establishes the state all other variants must be compatible with;
    // every slot is created pointing at it, then overwritten per-variant below.
    vk::IndirectExecutionSetPipelineInfoEXT pipelineInfo{
        .initialPipeline = pipeline.getPipelineVariant(0),
        .maxPipelineCount = variantCount,
    };
    vk::IndirectExecutionSetInfoEXT info;
    info.pPipelineInfo = &pipelineInfo;
    vk::IndirectExecutionSetCreateInfoEXT createInfo{
        .type = vk::IndirectExecutionSetInfoTypeEXT::ePipelines,
        .info = info,
    };
    auto createResult = vkDevice.createIndirectExecutionSetEXT(createInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create indirect execution set");
        return false;
    }
    m_indirectExecutionSet = createResult.value;

    oc::vector<vk::WriteIndirectExecutionSetPipelineEXT> writes;
    writes.reserve(variantCount);
    for (uint32 i = 0; i < variantCount; i++)
    {
        writes.push_back(vk::WriteIndirectExecutionSetPipelineEXT{
            .index = i,
            .pipeline = pipeline.getPipelineVariant(i),
        });
    }
    vkDevice.updateIndirectExecutionSetPipelineEXT(m_indirectExecutionSet, writes);
    return true;
}

void IndirectExecutionSet::destroy()
{
    if (m_indirectExecutionSet)
    {
        Globals::device.getDevice().destroyIndirectExecutionSetEXT(m_indirectExecutionSet);
        m_indirectExecutionSet = nullptr;
    }
}
