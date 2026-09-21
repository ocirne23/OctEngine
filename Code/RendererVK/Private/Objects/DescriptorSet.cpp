module RendererVK;

import :Device;

DescriptorSet::DescriptorSet() {}
DescriptorSet::~DescriptorSet() 
{
    if (m_descriptorSet)
    {
        vk::Device vkDevice = Globals::device.getDevice();
        vkDevice.freeDescriptorSets(Globals::device.getDescriptorPool(), m_descriptorSet);
        m_descriptorSet = nullptr;
    }
}
bool DescriptorSet::initialize(vk::DescriptorSetLayout descriptorSetLayout, const char* debugName, uint32 variableDescriptorCount)
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_descriptorSet) // re-initialize (e.g. the variable descriptor count grew): release the old set back to the pool
    {
        vkDevice.freeDescriptorSets(Globals::device.getDescriptorPool(), m_descriptorSet);
        m_descriptorSet = nullptr;
    }
    vk::DescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo
    {
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableDescriptorCount,
    };
    vk::DescriptorSetAllocateInfo allocInfo
    {
        .pNext = variableDescriptorCount > 0 ? &variableCountInfo : nullptr,
        .descriptorPool = Globals::device.getDescriptorPool(),
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptorSetLayout,
    };
    auto allocateResult = vkDevice.allocateDescriptorSets(allocInfo);
    if (allocateResult.result != vk::Result::eSuccess || !allocateResult.value[0])
    {
        assert(false && "Failed to allocate descriptor set");
        return false;
    }
    m_descriptorSet = allocateResult.value[0];
    Globals::device.setDebugName(m_descriptorSet, debugName);
    return true;
}