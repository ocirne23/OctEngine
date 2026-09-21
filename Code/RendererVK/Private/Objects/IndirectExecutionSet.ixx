export module RendererVK:IndirectExecutionSet;

import Core;
import :VK;

class GraphicsPipeline;

export class IndirectExecutionSet final
{
public:
    IndirectExecutionSet();
    ~IndirectExecutionSet();
    IndirectExecutionSet(const IndirectExecutionSet&) = delete;

    bool initialize(const GraphicsPipeline& pipeline, const char* debugName);
    void destroy();

    vk::IndirectExecutionSetEXT getHandle() const { return m_indirectExecutionSet; }

private:

    vk::IndirectExecutionSetEXT m_indirectExecutionSet;
};
