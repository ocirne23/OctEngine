export module RendererVK:CommandBuffer;

import Core;
import :VK;

export struct DescriptorSetUpdateInfo
{
    uint32 binding;
    uint32 startIdx = 0;
    vk::DescriptorType type;
    oc::vector<vk::DescriptorBufferInfo> bufferInfos;
    oc::vector<vk::DescriptorImageInfo> imageInfos;
    //oc::variant<vk::DescriptorBufferInfo, vk::DescriptorImageInfo> info;
};

export class CommandBuffer final
{
public:
    CommandBuffer();
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = default;

    bool initialize(vk::CommandBufferLevel level);

    vk::CommandBuffer begin(bool once = false, vk::CommandBufferInheritanceInfo* pInheritanceInfo = nullptr);
    void end();
    bool hasRecorded() { return m_hasRecorded; }
    void reset();
    vk::CommandBuffer getCommandBuffer() { return m_commandBuffer; }

    void submitGraphics(vk::Fence fence = VK_NULL_HANDLE);
    void addWaitSemaphore(vk::Semaphore semaphore, vk::PipelineStageFlags waitStageFlags) { m_waitSemaphores.push_back(semaphore); m_waitStages.push_back(waitStageFlags); }
    void addSignalSemaphore(vk::Semaphore semaphore) { m_signalSemaphores.push_back(semaphore); }
    const oc::vector<vk::Semaphore>& getWaitSemaphores() const { return m_waitSemaphores; }
    const oc::vector<vk::Semaphore>& getSignalSemaphores() const { return m_signalSemaphores; }
    void setWaitStage(vk::PipelineStageFlags2 stage) { m_waitStage = stage; }
    void cmdUpdateDescriptorSets(vk::PipelineLayout pipelineLayout, vk::PipelineBindPoint bindPoint, vk::DescriptorSet descriptorSet, const oc::span<DescriptorSetUpdateInfo>& updateInfo);

private:

    vk::CommandBuffer m_commandBuffer;
    bool m_hasRecorded = false;
    oc::vector<vk::Semaphore> m_signalSemaphores;
    oc::vector<vk::Semaphore> m_waitSemaphores;
    oc::vector<vk::PipelineStageFlags> m_waitStages;
    vk::PipelineStageFlags2 m_waitStage = vk::PipelineStageFlagBits2::eNone;
};