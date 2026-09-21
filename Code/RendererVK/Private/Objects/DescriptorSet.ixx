export module RendererVK:DescriptorSet;

import Core;
import :VK;

export class DescriptorSet final
{
public:

    DescriptorSet();
    ~DescriptorSet();
    DescriptorSet(const DescriptorSet&) = delete;

    // variableDescriptorCount: actual element count for a layout whose last binding has
    // eVariableDescriptorCount (0 = layout has no variable-count binding).
    bool initialize(vk::DescriptorSetLayout descriptorSetLayout, const char* debugName, uint32 variableDescriptorCount = 0);

    vk::DescriptorSet getDescriptorSet() const { return m_descriptorSet; }

private:

    vk::DescriptorSet m_descriptorSet;
};