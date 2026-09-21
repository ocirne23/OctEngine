export module RendererVK:Device;

import Core;
import :VK;

export class Device final
{
public:
    Device();
    ~Device();
    Device(const Device&) = delete;

    bool initialize();
    void destroy();

    uint32 findMemoryType(uint32 typeBits, vk::MemoryPropertyFlags requirementsMask) const;

    vk::PhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    vk::Device getDevice() const { return m_device; }
    vk::CommandPool getCommandPool() const { return m_commandPool; }
    vk::DescriptorPool getDescriptorPool() const { return m_descriptorPool; }
    uint32 getGraphicsQueueIndex() const { return m_graphicsQueueIndex; }
    vk::Queue getGraphicsQueue() const { return m_graphicsQueue; }
    // vkQueueSubmit/vkQueueWaitIdle/vkQueuePresentKHR need external synchronization on the queue, and
    // StagingManager's overflow submit can run off the main thread - so EVERY queue call takes this
    // mutex: submits via CommandBuffer::submitGraphics, presents via SwapChain::present, idles via
    // graphicsQueueWaitIdle(). Never call getGraphicsQueue().submit()/waitIdle() raw. (Known holes,
    // both main-thread-only by construction: the ImGui backend holds the raw queue for its
    // texture-upload submits - post-join window, no worker uploads run there - and OpenXR submits
    // internally at xrEndFrame - the VR path keeps beginFrame/present synchronous on main.)
    std::mutex& getGraphicsQueueMutex() { return m_graphicsQueueMutex; }
    vk::Result graphicsQueueWaitIdle() { std::lock_guard<std::mutex> lock(m_graphicsQueueMutex); return m_graphicsQueue.waitIdle(); }
    bool supportsExtensions(oc::vector<const char*> extensions);
    bool supportsCalibratedTimestamps() const { return m_supportsCalibratedTimestamps; }
    vk::DeviceSize getNonCoherentAtomSize() const { return m_nonCoherentAtomSize; }
    const vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT& getDeviceGeneratedCommandsProperties() const { return m_deviceGeneratedCommandsProperties; }

    // VK_EXT_debug_utils: the names Nsight / RenderDoc / validation messages show, and the labelled
    // command ranges (the GpuProfiler scopes) Nsight shows as markers. No-ops when the instance did not
    // enable the extension. Naming needs external sync on the object, so name it where it is created;
    // the driver copies the string, so a temporary is fine. Names over 63 characters keep their tail.
    template<typename T>
    void setDebugName(T handle, const char* name) const
    {
        if (m_pfnSetDebugName && handle && name)
            setDebugName(T::objectType, reinterpret_cast<uint64>(static_cast<typename T::CType>(handle)), name);
    }
    void setDebugName(vk::ObjectType type, uint64 handle, const char* name) const;
    void beginDebugLabel(vk::CommandBuffer cmd, const char* name) const;
    void endDebugLabel(vk::CommandBuffer cmd) const;

    // VK_KHR_pipeline_executable_properties: per-stage driver statistics (register count - what sets the
    // occupancy - instruction count, ...). "Renderer/Log pipeline stats" (not Saved; the Renderer registers
    // it before the device exists, and F5 re-creates the pipelines): pipelines created while it is on carry
    // CAPTURE_STATISTICS, and logPipelineStatistics writes one line per stage to the log and to
    // Local/pipeline_stats.txt (appended since startup; a reload appends a new block). (Internal
    // representations: the NVIDIA driver returns none - tried 2026-09-22 - so there is no SASS dump here.)
    static inline bool s_logPipelineStats = false;
    bool capturePipelineStatistics() const { return s_logPipelineStats && m_pfnGetPipelineExecutableStatistics; }
    void logPipelineStatistics(vk::Pipeline pipeline, const char* name);

private:

    vk::PhysicalDevice m_physicalDevice;
    vk::Device m_device;
    vk::CommandPool m_commandPool;
    vk::DescriptorPool m_descriptorPool;
    uint32 m_graphicsQueueIndex;
    vk::Queue m_graphicsQueue;
    std::mutex m_graphicsQueueMutex;
    vk::DeviceSize m_nonCoherentAtomSize;
    bool m_supportsCalibratedTimestamps = false;
    vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT m_deviceGeneratedCommandsProperties;
    // Members, not function-local statics: set once in initialize (main thread) before any job runs.
    PFN_vkSetDebugUtilsObjectNameEXT m_pfnSetDebugName = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT m_pfnBeginDebugLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT m_pfnEndDebugLabel = nullptr;
    PFN_vkGetPipelineExecutablePropertiesKHR m_pfnGetPipelineExecutableProperties = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR m_pfnGetPipelineExecutableStatistics = nullptr;    std::mutex m_pipelineStatsMutex;
    oc::string m_pipelineStatsText;

    oc::vector<vk::Format> m_supported2DOptimalFormats;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DEVICE)
    Device device;
}