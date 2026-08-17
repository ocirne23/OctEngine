module RendererVK;

import :VK;
import :Device;

CommandBuffer::CommandBuffer() {}
CommandBuffer::~CommandBuffer()
{
    if (m_commandBuffer)
        Globals::device.getDevice().freeCommandBuffers(Globals::device.getCommandPool(), m_commandBuffer);
}

bool CommandBuffer::initialize(vk::CommandBufferLevel level)
{
    vk::CommandBufferAllocateInfo allocInfo
    {
        .commandPool = Globals::device.getCommandPool(),
        .level = level,
        .commandBufferCount = 1,
    };
    auto result = Globals::device.getDevice().allocateCommandBuffers(allocInfo);
    if (result.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to allocate command buffer");
        return false;
    }
    m_commandBuffer = result.value[0];
    if (!m_commandBuffer)
    {
        assert(false && "Failed to allocate command buffer");
        return false;
    }
    m_signalSemaphores.reserve(4);
    m_signalSemaphores.shrink_to_fit();

    m_waitSemaphores.reserve(4);
    m_waitSemaphores.shrink_to_fit();

    m_waitStages.reserve(4);
    m_waitStages.shrink_to_fit();
    return true;
}

void CommandBuffer::submitGraphics(vk::Fence fence)
{
    vk::SubmitInfo submitInfo{
        .waitSemaphoreCount = (uint32)m_waitSemaphores.size(),
        .pWaitSemaphores = m_waitSemaphores.data(),
        .pWaitDstStageMask = m_waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &m_commandBuffer,
        .signalSemaphoreCount = (uint32)m_signalSemaphores.size(),
        .pSignalSemaphores = m_signalSemaphores.data(),
    };
    auto result = Globals::device.getGraphicsQueue().submit(submitInfo, fence);
    if (result != vk::Result::eSuccess)
    {
        assert(false && "Failed to submit command buffer");
    }

    m_waitSemaphores.clear();
    m_waitStages.clear();
    m_signalSemaphores.clear();
    m_waitStage = vk::PipelineStageFlagBits2::eNone;
}

void CommandBuffer::cmdUpdateDescriptorSets(vk::PipelineLayout pipelineLayout, vk::PipelineBindPoint bindPoint, vk::DescriptorSet descriptorSet, const oc::span<DescriptorSetUpdateInfo>& updateInfo)
{
    vk::WriteDescriptorSet* descriptorWrites = (vk::WriteDescriptorSet*)_alloca(sizeof(vk::WriteDescriptorSet) * updateInfo.size());
    for (uint32 i = 0; i < updateInfo.size(); i++)
    {
        descriptorWrites[i] =
        {
            .dstSet = descriptorSet,
            .dstBinding = updateInfo[i].binding,
            .dstArrayElement = updateInfo[i].startIdx,
            .descriptorCount = (uint32)oc::max(updateInfo[i].imageInfos.size(), updateInfo[i].bufferInfos.size()),
            .descriptorType = updateInfo[i].type,
            .pImageInfo = updateInfo[i].imageInfos.size() ? updateInfo[i].imageInfos.data() : nullptr,
            .pBufferInfo = updateInfo[i].bufferInfos.size() ? updateInfo[i].bufferInfos.data() : nullptr,
        };
    }
    //m_commandBuffer.pushDescriptorSetKHR(bindPoint, pipelineLayout, 0, (uint32)updateInfo.size(), descriptorWrites);
    vk::Device device = Globals::device.getDevice();
    device.updateDescriptorSets((uint32)updateInfo.size(), descriptorWrites, 0, nullptr);
}

vk::CommandBuffer CommandBuffer::begin(bool once, vk::CommandBufferInheritanceInfo* pInheritanceInfo)
{
    if (m_hasRecorded)
        m_commandBuffer.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
    vk::CommandBufferBeginInfo beginInfo{
        .flags = once ? vk::CommandBufferUsageFlagBits::eOneTimeSubmit : (vk::CommandBufferUsageFlagBits)0,
        .pInheritanceInfo = pInheritanceInfo,
    };
    if (pInheritanceInfo && pInheritanceInfo->renderPass != VK_NULL_HANDLE)
    {
        beginInfo.flags |= vk::CommandBufferUsageFlagBits::eRenderPassContinue;
        //beginInfo.flags |= vk::CommandBufferUsageFlagBits::eSimultaneousUse;
    }
    auto result = m_commandBuffer.begin(beginInfo);
    if (result != vk::Result::eSuccess)
    {
        assert(false && "Failed to begin command buffer");
        return nullptr;
    }
    m_hasRecorded = true;

    return m_commandBuffer;
}

void CommandBuffer::end()
{
    auto result = m_commandBuffer.end();
    if (result != vk::Result::eSuccess)
    {
        assert(false && "Failed to end command buffer");
    }
}

void CommandBuffer::reset()
{
    m_hasRecorded = false;
    m_commandBuffer.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
}