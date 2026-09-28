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

    // variantMask: the pipeline variants the set holds (bit i = variant i); all of them must share one fragment
    // output interface. The others' slots stay unwritten.
    bool initialize(const GraphicsPipeline& pipeline, const char* debugName, uint32 variantMask = ~0u);
    void destroy();

    vk::IndirectExecutionSetEXT getHandle() const { return m_indirectExecutionSet; }
    // The variant the set was created with: it must be the BOUND pipeline when the set executes.
    uint32 getInitialVariant() const { return m_initialVariant; }

private:

    vk::IndirectExecutionSetEXT m_indirectExecutionSet;
    uint32 m_initialVariant = 0;
};
