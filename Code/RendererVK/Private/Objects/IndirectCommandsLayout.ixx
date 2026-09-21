export module RendererVK:IndirectCommandsLayout;

import Core;
import :VK;

export class IndirectCommandsLayout final
{
public:
    IndirectCommandsLayout();
    ~IndirectCommandsLayout();
    IndirectCommandsLayout(const IndirectCommandsLayout&) = delete;

    // shaderStages must cover every stage swapped by the execution set / touched by the draw.
    // useExecutionSet == false omits the EXECUTION_SET token (single-pipeline draws); required when the
    // generated commands run inside a multiview render pass, where an execution set is not permitted.
    bool initialize(const char* debugName, vk::PipelineLayout pipelineLayout, vk::ShaderStageFlags shaderStages, bool useExecutionSet = true);
    void destroy();

    vk::IndirectCommandsLayoutEXT getHandle() const { return m_layout; }
    uint32 getIndirectStride() const { return m_indirectStride; }

private:

    vk::IndirectCommandsLayoutEXT m_layout;
    uint32 m_indirectStride = 0;
};
