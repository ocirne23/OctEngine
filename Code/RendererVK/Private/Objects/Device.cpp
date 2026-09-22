module RendererVK;

import Core.Log;
import File;
import :VK;
import :Instance;
import :Allocator;
import :OpenXRSession;

Device::Device() {}
Device::~Device()
{
    destroy();
}

bool Device::initialize()
{
    vk::Instance instance = Globals::instance.getInstance();
    const bool vrEnabled = Globals::openXR.isEnabled();
    if (vrEnabled)
    {
        // OpenXR dictates which physical device drives the HMD; honour it over the heuristic below.
        m_physicalDevice = Globals::openXR.getVulkanGraphicsDevice(instance);
    }
    else
    {
        // try to find the discrete GPU with the most memory
        auto enumResult = instance.enumeratePhysicalDevices();
        if (enumResult.result != vk::Result::eSuccess || enumResult.value.size() == 0)
        {
            assert(false && "Could not find any physical devices\n");
            return false;
        }
        for (vk::PhysicalDevice physDevice : enumResult.value)
        {
            if (!m_physicalDevice)
                m_physicalDevice = physDevice;
            else if (physDevice.getProperties().limits.maxMemoryAllocationCount > m_physicalDevice.getProperties().limits.maxMemoryAllocationCount
                && physDevice.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu
                || m_physicalDevice.getProperties().deviceType != vk::PhysicalDeviceType::eDiscreteGpu)
            {
                m_physicalDevice = physDevice;
            }
        }
    }
    printf("Device: %s\n", m_physicalDevice.getProperties().deviceName.data());
    m_nonCoherentAtomSize = m_physicalDevice.getProperties().limits.nonCoherentAtomSize;

    oc::vector<const char*> deviceExtensions{
        vk::KHRSwapchainExtensionName, vk::KHRPushDescriptorExtensionName, vk::KHRMaintenance5ExtensionName, vk::EXTDeviceGeneratedCommandsExtensionName,
        vk::KHRAccelerationStructureExtensionName, vk::KHRRayQueryExtensionName, vk::KHRDeferredHostOperationsExtensionName, vk::KHRShaderNonSemanticInfoExtensionName,
        vk::EXTShaderViewportIndexLayerExtensionName };

    // Device extensions the OpenXR runtime requires (none when VR is disabled). Kept alive for the
    // duration of this call; skip duplicates already in the list above.
    oc::vector<oc::string> xrExtensions;
    if (vrEnabled)
        xrExtensions = Globals::openXR.getRequiredVulkanDeviceExtensions();
    for (const oc::string& ext : xrExtensions)
    {
        bool alreadyAdded = false;
        for (const char* have : deviceExtensions)
            if (ext == have) { alreadyAdded = true; break; }
        if (!alreadyAdded)
            deviceExtensions.push_back(ext.c_str());
    }

    if (!supportsExtensions(deviceExtensions))
    {
        assert(false && "Physical device does not support the extension\n");
        return false;
    }

    // Optional: calibrated timestamps let the GpuProfiler place GPU pass timings on the CPU timeline
    // exactly (it falls back to submit-time anchoring without them).
    if (supportsExtensions({ vk::KHRCalibratedTimestampsExtensionName }))
    {
        deviceExtensions.push_back(vk::KHRCalibratedTimestampsExtensionName);
        m_supportsCalibratedTimestamps = true;
    }
    // Optional: per-pipeline driver statistics (register counts) for "Renderer/Log pipeline stats".
    const bool pipelineExecutableProperties = supportsExtensions({ vk::KHRPipelineExecutablePropertiesExtensionName });
    if (pipelineExecutableProperties)
        deviceExtensions.push_back(vk::KHRPipelineExecutablePropertiesExtensionName);

    m_graphicsQueueIndex = UINT32_MAX;
    oc::vector<vk::QueueFamilyProperties> queueFamilyProperties = oc::fromStd(m_physicalDevice.getQueueFamilyProperties());
    for (uint32 i = 0; i < queueFamilyProperties.size(); i++)
    {
        if (queueFamilyProperties[i].queueFlags & vk::QueueFlagBits::eGraphics)
        {
            m_graphicsQueueIndex = i;
            if (!(queueFamilyProperties[i].queueFlags & vk::QueueFlagBits::eTransfer))
            {
                assert(false && "Separate transfer queue not supported!");
                return false;
            }
            if (!(queueFamilyProperties[i].queueFlags & vk::QueueFlagBits::eCompute))
            {
                assert(false && "Separate compute queue not supported!");
                return false;
            }
            break;
        }
    }
    if (m_graphicsQueueIndex == UINT32_MAX)
    {
        assert(false && "Could not find a graphicsQueue family index\n");
        return false;
    }

    const float queuePriorities[] = { 1.0f };
    oc::array<vk::DeviceQueueCreateInfo, 1> deviceQueueCreateInfos{
        vk::DeviceQueueCreateInfo {.queueFamilyIndex = m_graphicsQueueIndex, .queueCount = 1, .pQueuePriorities = queuePriorities }
    };
    //const oc::vector<const char*>& enabledLayers = Globals::instance.getEnabledLayers();

    vk::PhysicalDeviceFeatures2 deviceFeatures;
    m_physicalDevice.getFeatures2(&deviceFeatures);
    deviceFeatures.features.samplerAnisotropy = vk::True;
    deviceFeatures.features.wideLines = vk::True; // debug lines and wireframe rasterize LINE_WIDTH px wide
    deviceFeatures.features.dualSrcBlend = vk::True; // the terrain overlay composites src0 + dst * src1
    vk::PhysicalDeviceVulkan11Features vk11Features
    {
        .pNext = &deviceFeatures,
        .storageBuffer16BitAccess = vk::True,
        .uniformAndStorageBuffer16BitAccess = vk::True,
        //.storageInputOutput16 = vk::True, // Not supported on NVIDIA!
        .multiview = vk::True, // sun shadow cascades render in a single multiview pass
    };
    vk::PhysicalDeviceVulkan12Features vk12Features
    {
        .pNext = &vk11Features,
        .storageBuffer8BitAccess = vk::True,
        .shaderFloat16 = vk::True,
		.shaderInt8 = vk::True,
        .shaderSampledImageArrayNonUniformIndexing = vk::True,
        // Lets the forward pass's cached draw CB keep its denoised-AO image descriptor refreshed each frame
        // (the AO image is ping-ponged per frame and recreated on resize) without re-recording the CB.
        .descriptorBindingSampledImageUpdateAfterBind = vk::True,
        .descriptorBindingPartiallyBound = vk::True, // texture arrays are written up to the live texture count only
        .descriptorBindingVariableDescriptorCount = vk::True,
        .runtimeDescriptorArray = vk::True,
        .scalarBlockLayout = vk::True,
        .timelineSemaphore = vk::True,
        .bufferDeviceAddress = vk::True, // required by VK_EXT_device_generated_commands (indirect/preprocess buffers are referenced by device address)
        .vulkanMemoryModel = vk::True,
        .vulkanMemoryModelDeviceScope = vk::True,
        .shaderOutputViewportIndex = vk::True,
        .shaderOutputLayer = vk::True,
    };
    vk::PhysicalDeviceVulkan13Features vk13Features{
        .pNext = &vk12Features,
        .shaderDemoteToHelperInvocation = vk::True,
        .synchronization2 = vk::True,
    };
    vk::PhysicalDeviceMaintenance5Features maintenance5Features{
        .pNext = &vk13Features,
        .maintenance5 = vk::True };
    // Hardware ray tracing via ray queries in compute (for GI probe tracing). rayQuery pulls in
    // acceleration structures; deferred_host_operations is a build-time dependency of both.
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures{
        .pNext = &maintenance5Features,
        .accelerationStructure = vk::True,
        // Lets the forward pass's cached draw CB keep its TLAS descriptor refreshed each frame (the TLAS
        // handle is recreated on capacity growth) without re-recording or invalidating the CB.
        .descriptorBindingAccelerationStructureUpdateAfterBind = vk::True
    };
    vk::PhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
        .pNext = &accelStructFeatures,
        .rayQuery = vk::True
    };
    vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgcFeatures{
        .pNext = &rayQueryFeatures,
        .deviceGeneratedCommands = vk::True
    };
    vk::PhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipelineExecutableFeatures{
        .pNext = &dgcFeatures,
        .pipelineExecutableInfo = vk::True
    };
    vk::DeviceCreateInfo deviceCreateInfo{
        .pNext = pipelineExecutableProperties ? (void*)&pipelineExecutableFeatures : (void*)&dgcFeatures,
        .queueCreateInfoCount = (uint32)deviceQueueCreateInfos.size(),
        .pQueueCreateInfos = deviceQueueCreateInfos.data(),
        .enabledLayerCount = (uint32)0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = (uint32)deviceExtensions.size(),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    auto createResult = m_physicalDevice.createDevice(deviceCreateInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert(false && "Could not create a device\n");
        return false;
    }
    m_device = createResult.value;

    // Instance-extension commands: loaded through the instance (vkGetDeviceProcAddr need not serve them).
    if (Globals::instance.isDebugUtilsEnabled())
    {
        m_pfnSetDebugName = (PFN_vkSetDebugUtilsObjectNameEXT)instance.getProcAddr("vkSetDebugUtilsObjectNameEXT");
        m_pfnBeginDebugLabel = (PFN_vkCmdBeginDebugUtilsLabelEXT)instance.getProcAddr("vkCmdBeginDebugUtilsLabelEXT");
        m_pfnEndDebugLabel = (PFN_vkCmdEndDebugUtilsLabelEXT)instance.getProcAddr("vkCmdEndDebugUtilsLabelEXT");
        if (!m_pfnBeginDebugLabel || !m_pfnEndDebugLabel) // only in pairs: a lone end is invalid
        {
            m_pfnBeginDebugLabel = nullptr;
            m_pfnEndDebugLabel = nullptr;
        }
    }
    setDebugName(m_device, "Device");

    vk::CommandPoolCreateInfo poolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = m_graphicsQueueIndex,
    };
    auto commandPoolResult = m_device.createCommandPool(poolCreateInfo);
    if (commandPoolResult.result != vk::Result::eSuccess)
    {
        assert(false && "Could not create a command pool\n");
        return false;
    }
    m_commandPool = commandPoolResult.value;
    setDebugName(m_commandPool, "Device.commandPool");

    const uint32 poolSize = 1024;
    vk::DescriptorPoolSize poolSizes[] =
    {
        { vk::DescriptorType::eSampler, poolSize * 4 },
        // Several sets bind a texture array whose capacity can grow at runtime up to TextureManager's
        // 16K soft cap (per frame in flight + re-allocations on growth), so provision generously.
        { vk::DescriptorType::eCombinedImageSampler, 256 * 1024 },
        { vk::DescriptorType::eSampledImage, poolSize },
        { vk::DescriptorType::eStorageImage, poolSize },
        { vk::DescriptorType::eUniformTexelBuffer, poolSize },
        { vk::DescriptorType::eStorageTexelBuffer, poolSize },
        { vk::DescriptorType::eUniformBuffer, poolSize },
        { vk::DescriptorType::eStorageBuffer, poolSize },
        { vk::DescriptorType::eUniformBufferDynamic, poolSize },
        { vk::DescriptorType::eStorageBufferDynamic, poolSize },
        { vk::DescriptorType::eInputAttachment, poolSize },
        { vk::DescriptorType::eAccelerationStructureKHR, poolSize } // GI trace TLAS binding
    };
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = poolSize * (uint32)(sizeof(poolSizes) / sizeof(poolSizes[0])),
        .poolSizeCount = (uint32)(sizeof(poolSizes) / sizeof(poolSizes[0])),
        .pPoolSizes = poolSizes,
    };
    auto descriptorPoolResult = m_device.createDescriptorPool(descriptorPoolCreateInfo);
    if (descriptorPoolResult.result != vk::Result::eSuccess)
    {
        assert(false && "Could not create a descriptor pool\n");
        return false;
    }
    m_descriptorPool = descriptorPoolResult.value;
    setDebugName(m_descriptorPool, "Device.descriptorPool");

    pfVkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)m_device.getProcAddr("vkCmdPushDescriptorSetKHR");

    vk::PhysicalDeviceProperties2 deviceProperties2{ .pNext = &m_deviceGeneratedCommandsProperties };
    m_physicalDevice.getProperties2(&deviceProperties2);

    pfVkGetGeneratedCommandsMemoryRequirementsEXT = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)m_device.getProcAddr("vkGetGeneratedCommandsMemoryRequirementsEXT");
    pfVkCmdPreprocessGeneratedCommandsEXT = (PFN_vkCmdPreprocessGeneratedCommandsEXT)m_device.getProcAddr("vkCmdPreprocessGeneratedCommandsEXT");
    pfVkCmdExecuteGeneratedCommandsEXT = (PFN_vkCmdExecuteGeneratedCommandsEXT)m_device.getProcAddr("vkCmdExecuteGeneratedCommandsEXT");
    pfVkCreateIndirectCommandsLayoutEXT = (PFN_vkCreateIndirectCommandsLayoutEXT)m_device.getProcAddr("vkCreateIndirectCommandsLayoutEXT");
    pfVkDestroyIndirectCommandsLayoutEXT = (PFN_vkDestroyIndirectCommandsLayoutEXT)m_device.getProcAddr("vkDestroyIndirectCommandsLayoutEXT");
    pfVkCreateIndirectExecutionSetEXT = (PFN_vkCreateIndirectExecutionSetEXT)m_device.getProcAddr("vkCreateIndirectExecutionSetEXT");
    pfVkDestroyIndirectExecutionSetEXT = (PFN_vkDestroyIndirectExecutionSetEXT)m_device.getProcAddr("vkDestroyIndirectExecutionSetEXT");
    pfVkUpdateIndirectExecutionSetPipelineEXT = (PFN_vkUpdateIndirectExecutionSetPipelineEXT)m_device.getProcAddr("vkUpdateIndirectExecutionSetPipelineEXT");

    if (pipelineExecutableProperties)
    {
        m_pfnGetPipelineExecutableProperties = (PFN_vkGetPipelineExecutablePropertiesKHR)m_device.getProcAddr("vkGetPipelineExecutablePropertiesKHR");
        m_pfnGetPipelineExecutableStatistics = (PFN_vkGetPipelineExecutableStatisticsKHR)m_device.getProcAddr("vkGetPipelineExecutableStatisticsKHR");        if (!m_pfnGetPipelineExecutableProperties)
            m_pfnGetPipelineExecutableStatistics = nullptr; // capturePipelineStatistics() checks only this one
    }

    if (m_supportsCalibratedTimestamps)
        pfVkGetCalibratedTimestampsKHR = (PFN_vkGetCalibratedTimestampsKHR)m_device.getProcAddr("vkGetCalibratedTimestampsKHR");

    pfVkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)m_device.getProcAddr("vkGetAccelerationStructureBuildSizesKHR");
    pfVkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)m_device.getProcAddr("vkCreateAccelerationStructureKHR");
    pfVkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)m_device.getProcAddr("vkDestroyAccelerationStructureKHR");
    pfVkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)m_device.getProcAddr("vkCmdBuildAccelerationStructuresKHR");
    pfVkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)m_device.getProcAddr("vkGetAccelerationStructureDeviceAddressKHR");
    pfVkCmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR)m_device.getProcAddr("vkCmdCopyAccelerationStructureKHR");
    pfVkCmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)m_device.getProcAddr("vkCmdWriteAccelerationStructuresPropertiesKHR");

    m_graphicsQueue = m_device.getQueue(m_graphicsQueueIndex, 0);
    if (!m_graphicsQueue)
    {
        assert(false && "Could not get the graphics queue\n");
        return false;
    }
    setDebugName(m_graphicsQueue, "GraphicsQueue");

    oc::vector<vk::Format> candidates = {
         vk::Format::eUndefined                                ,
         vk::Format::eR4G4UnormPack8                           ,
         vk::Format::eR4G4B4A4UnormPack16                      ,
         vk::Format::eB4G4R4A4UnormPack16                      ,
         vk::Format::eR5G6B5UnormPack16                        ,
         vk::Format::eB5G6R5UnormPack16                        ,
         vk::Format::eR5G5B5A1UnormPack16                      ,
         vk::Format::eB5G5R5A1UnormPack16                      ,
         vk::Format::eA1R5G5B5UnormPack16                      ,
         vk::Format::eR8Unorm                                  ,
         vk::Format::eR8Snorm                                  ,
         vk::Format::eR8Uscaled                                ,
         vk::Format::eR8Sscaled                                ,
         vk::Format::eR8Uint                                   ,
         vk::Format::eR8Sint                                   ,
         vk::Format::eR8Srgb                                   ,
         vk::Format::eR8G8Unorm                                ,
         vk::Format::eR8G8Snorm                                ,
         vk::Format::eR8G8Uscaled                              ,
         vk::Format::eR8G8Sscaled                              ,
         vk::Format::eR8G8Uint                                 ,
         vk::Format::eR8G8Sint                                 ,
         vk::Format::eR8G8Srgb                                 ,
         vk::Format::eR8G8B8Unorm                              ,
         vk::Format::eR8G8B8Snorm                              ,
         vk::Format::eR8G8B8Uscaled                            ,
         vk::Format::eR8G8B8Sscaled                            ,
         vk::Format::eR8G8B8Uint                               ,
         vk::Format::eR8G8B8Sint                               ,
         vk::Format::eR8G8B8Srgb                               ,
         vk::Format::eB8G8R8Unorm                              ,
         vk::Format::eB8G8R8Snorm                              ,
         vk::Format::eB8G8R8Uscaled                            ,
         vk::Format::eB8G8R8Sscaled                            ,
         vk::Format::eB8G8R8Uint                               ,
         vk::Format::eB8G8R8Sint                               ,
         vk::Format::eB8G8R8Srgb                               ,
         vk::Format::eR8G8B8A8Unorm                            ,
         vk::Format::eR8G8B8A8Snorm                            ,
         vk::Format::eR8G8B8A8Uscaled                          ,
         vk::Format::eR8G8B8A8Sscaled                          ,
         vk::Format::eR8G8B8A8Uint                             ,
         vk::Format::eR8G8B8A8Sint                             ,
         vk::Format::eR8G8B8A8Srgb                             ,
         vk::Format::eB8G8R8A8Unorm                            ,
         vk::Format::eB8G8R8A8Snorm                            ,
         vk::Format::eB8G8R8A8Uscaled                          ,
         vk::Format::eB8G8R8A8Sscaled                          ,
         vk::Format::eB8G8R8A8Uint                             ,
         vk::Format::eB8G8R8A8Sint                             ,
         vk::Format::eB8G8R8A8Srgb                             ,
         vk::Format::eA8B8G8R8UnormPack32                      ,
         vk::Format::eA8B8G8R8SnormPack32                      ,
         vk::Format::eA8B8G8R8UscaledPack32                    ,
         vk::Format::eA8B8G8R8SscaledPack32                    ,
         vk::Format::eA8B8G8R8UintPack32                       ,
         vk::Format::eA8B8G8R8SintPack32                       ,
         vk::Format::eA8B8G8R8SrgbPack32                       ,
         vk::Format::eA2R10G10B10UnormPack32                   ,
         vk::Format::eA2R10G10B10SnormPack32                   ,
         vk::Format::eA2R10G10B10UscaledPack32                 ,
         vk::Format::eA2R10G10B10SscaledPack32                 ,
         vk::Format::eA2R10G10B10UintPack32                    ,
         vk::Format::eA2R10G10B10SintPack32                    ,
         vk::Format::eA2B10G10R10UnormPack32                   ,
         vk::Format::eA2B10G10R10SnormPack32                   ,
         vk::Format::eA2B10G10R10UscaledPack32                 ,
         vk::Format::eA2B10G10R10SscaledPack32                 ,
         vk::Format::eA2B10G10R10UintPack32                    ,
         vk::Format::eA2B10G10R10SintPack32                    ,
         vk::Format::eR16Unorm                                 ,
         vk::Format::eR16Snorm                                 ,
         vk::Format::eR16Uscaled                               ,
         vk::Format::eR16Sscaled                               ,
         vk::Format::eR16Uint                                  ,
         vk::Format::eR16Sint                                  ,
         vk::Format::eR16Sfloat                                ,
         vk::Format::eR16G16Unorm                              ,
         vk::Format::eR16G16Snorm                              ,
         vk::Format::eR16G16Uscaled                            ,
         vk::Format::eR16G16Sscaled                            ,
         vk::Format::eR16G16Uint                               ,
         vk::Format::eR16G16Sint                               ,
         vk::Format::eR16G16Sfloat                             ,
         vk::Format::eR16G16B16Unorm                           ,
         vk::Format::eR16G16B16Snorm                           ,
         vk::Format::eR16G16B16Uscaled                         ,
         vk::Format::eR16G16B16Sscaled                         ,
         vk::Format::eR16G16B16Uint                            ,
         vk::Format::eR16G16B16Sint                            ,
         vk::Format::eR16G16B16Sfloat                          ,
         vk::Format::eR16G16B16A16Unorm                        ,
         vk::Format::eR16G16B16A16Snorm                        ,
         vk::Format::eR16G16B16A16Uscaled                      ,
         vk::Format::eR16G16B16A16Sscaled                      ,
         vk::Format::eR16G16B16A16Uint                         ,
         vk::Format::eR16G16B16A16Sint                         ,
         vk::Format::eR16G16B16A16Sfloat                       ,
         vk::Format::eR32Uint                                  ,
         vk::Format::eR32Sint                                  ,
         vk::Format::eR32Sfloat                                ,
         vk::Format::eR32G32Uint                               ,
         vk::Format::eR32G32Sint                               ,
         vk::Format::eR32G32Sfloat                             ,
         vk::Format::eR32G32B32Uint                            ,
         vk::Format::eR32G32B32Sint                            ,
         vk::Format::eR32G32B32Sfloat                          ,
         vk::Format::eR32G32B32A32Uint                         ,
         vk::Format::eR32G32B32A32Sint                         ,
         vk::Format::eR32G32B32A32Sfloat                       ,
         vk::Format::eR64Uint                                  ,
         vk::Format::eR64Sint                                  ,
         vk::Format::eR64Sfloat                                ,
         vk::Format::eR64G64Uint                               ,
         vk::Format::eR64G64Sint                               ,
         vk::Format::eR64G64Sfloat                             ,
         vk::Format::eR64G64B64Uint                            ,
         vk::Format::eR64G64B64Sint                            ,
         vk::Format::eR64G64B64Sfloat                          ,
         vk::Format::eR64G64B64A64Uint                         ,
         vk::Format::eR64G64B64A64Sint                         ,
         vk::Format::eR64G64B64A64Sfloat                       ,
         vk::Format::eB10G11R11UfloatPack32                    ,
         vk::Format::eE5B9G9R9UfloatPack32                     ,
         vk::Format::eD16Unorm                                 ,
         vk::Format::eX8D24UnormPack32                         ,
         vk::Format::eD32Sfloat                                ,
         vk::Format::eS8Uint                                   ,
         vk::Format::eD16UnormS8Uint                           ,
         vk::Format::eD24UnormS8Uint                           ,
         vk::Format::eD32SfloatS8Uint                          ,
         vk::Format::eBc1RgbUnormBlock                         ,
         vk::Format::eBc1RgbSrgbBlock                          ,
         vk::Format::eBc1RgbaUnormBlock                        ,
         vk::Format::eBc1RgbaSrgbBlock                         ,
         vk::Format::eBc2UnormBlock                            ,
         vk::Format::eBc2SrgbBlock                             ,
         vk::Format::eBc3UnormBlock                            ,
         vk::Format::eBc3SrgbBlock                             ,
         vk::Format::eBc4UnormBlock                            ,
         vk::Format::eBc4SnormBlock                            ,
         vk::Format::eBc5UnormBlock                            ,
         vk::Format::eBc5SnormBlock                            ,
         vk::Format::eBc6HUfloatBlock                          ,
         vk::Format::eBc6HSfloatBlock                          ,
         vk::Format::eBc7UnormBlock                            ,
         vk::Format::eBc7SrgbBlock                             ,
         vk::Format::eEtc2R8G8B8UnormBlock                     ,
         vk::Format::eEtc2R8G8B8SrgbBlock                      ,
         vk::Format::eEtc2R8G8B8A1UnormBlock                   ,
         vk::Format::eEtc2R8G8B8A1SrgbBlock                    ,
         vk::Format::eEtc2R8G8B8A8UnormBlock                   ,
         vk::Format::eEtc2R8G8B8A8SrgbBlock                    ,
         vk::Format::eEacR11UnormBlock                         ,
         vk::Format::eEacR11SnormBlock                         ,
         vk::Format::eEacR11G11UnormBlock                      ,
         vk::Format::eEacR11G11SnormBlock                      ,
         vk::Format::eAstc4x4UnormBlock                        ,
         vk::Format::eAstc4x4SrgbBlock                         ,
         vk::Format::eAstc5x4UnormBlock                        ,
         vk::Format::eAstc5x4SrgbBlock                         ,
         vk::Format::eAstc5x5UnormBlock                        ,
         vk::Format::eAstc5x5SrgbBlock                         ,
         vk::Format::eAstc6x5UnormBlock                        ,
         vk::Format::eAstc6x5SrgbBlock                         ,
         vk::Format::eAstc6x6UnormBlock                        ,
         vk::Format::eAstc6x6SrgbBlock                         ,
         vk::Format::eAstc8x5UnormBlock                        ,
         vk::Format::eAstc8x5SrgbBlock                         ,
         vk::Format::eAstc8x6UnormBlock                        ,
         vk::Format::eAstc8x6SrgbBlock                         ,
         vk::Format::eAstc8x8UnormBlock                        ,
         vk::Format::eAstc8x8SrgbBlock                         ,
         vk::Format::eAstc10x5UnormBlock                       ,
         vk::Format::eAstc10x5SrgbBlock                        ,
         vk::Format::eAstc10x6UnormBlock                       ,
         vk::Format::eAstc10x6SrgbBlock                        ,
         vk::Format::eAstc10x8UnormBlock                       ,
         vk::Format::eAstc10x8SrgbBlock                        ,
         vk::Format::eAstc10x10UnormBlock                      ,
         vk::Format::eAstc10x10SrgbBlock                       ,
         vk::Format::eAstc12x10UnormBlock                      ,
         vk::Format::eAstc12x10SrgbBlock                       ,
         vk::Format::eAstc12x12UnormBlock                      ,
         vk::Format::eAstc12x12SrgbBlock
    };

    for (vk::Format candidate : candidates)
    {
        auto imageFormatPropertiesresult = m_physicalDevice.getImageFormatProperties(candidate, vk::ImageType::e2D, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
        if (imageFormatPropertiesresult.result == vk::Result::eSuccess)
        {
            m_supported2DOptimalFormats.push_back(candidate);
        }
    }

    if (!Globals::gpuAllocator.initialize())
        return false;

    return true;
}

void Device::destroy()
{
    if (m_device)
    {
        auto waitResult = m_device.waitIdle();
        if (waitResult != vk::Result::eSuccess)
        {
            assert(false && "Failed to wait for device idle");
        }
        Globals::gpuAllocator.destroy();
        m_device.destroyCommandPool(m_commandPool);
        m_device.destroyDescriptorPool(m_descriptorPool);
        m_device.destroy();
    }
    m_device = nullptr;
}

void Device::setDebugName(vk::ObjectType type, uint64 handle, const char* name) const
{
    if (!m_pfnSetDebugName || !handle || !name)
        return;
    // Short names read better in tool UIs. Over the cap, keep the TAIL - for a path that is the file name.
    constexpr size_t MAX_DEBUG_NAME_LENGTH = 63;
    const size_t length = strlen(name);
    if (length > MAX_DEBUG_NAME_LENGTH)
    {
        name += length - MAX_DEBUG_NAME_LENGTH;
        while ((*name & 0xC0) == 0x80) // never start inside a UTF-8 sequence
            ++name;
    }
    const VkDebugUtilsObjectNameInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = (VkObjectType)type,
        .objectHandle = handle,
        .pObjectName = name,
    };
    m_pfnSetDebugName((VkDevice)m_device, &info);
}

void Device::beginDebugLabel(vk::CommandBuffer cmd, const char* name) const
{
    if (!m_pfnBeginDebugLabel)
        return;
    const VkDebugUtilsLabelEXT label{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pLabelName = name,
    };
    m_pfnBeginDebugLabel((VkCommandBuffer)cmd, &label);
}

void Device::endDebugLabel(vk::CommandBuffer cmd) const
{
    if (m_pfnEndDebugLabel)
        m_pfnEndDebugLabel((VkCommandBuffer)cmd);
}

void Device::logPipelineStatistics(vk::Pipeline pipeline, const char* name)
{
    if (!capturePipelineStatistics() || !pipeline)
        return;
    const VkPipelineInfoKHR pipelineInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = (VkPipeline)pipeline };
    uint32 numExecutables = 0;
    if (m_pfnGetPipelineExecutableProperties((VkDevice)m_device, &pipelineInfo, &numExecutables, nullptr) != VK_SUCCESS || numExecutables == 0)
        return;
    oc::vector<VkPipelineExecutablePropertiesKHR> executables(numExecutables,
        VkPipelineExecutablePropertiesKHR{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
    if (m_pfnGetPipelineExecutableProperties((VkDevice)m_device, &pipelineInfo, &numExecutables, executables.data()) != VK_SUCCESS)
        return;

    // One line per stage: <pipeline>\t<stage>\t<statistic>=<value>... (tab-separated, easy to diff).
    oc::string text;
    oc::vector<VkPipelineExecutableStatisticKHR> statistics;
    for (uint32 i = 0; i < numExecutables; ++i)
    {
        const VkPipelineExecutableInfoKHR executableInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = (VkPipeline)pipeline, .executableIndex = i };
        uint32 numStatistics = 0;
        if (m_pfnGetPipelineExecutableStatistics((VkDevice)m_device, &executableInfo, &numStatistics, nullptr) != VK_SUCCESS)
            continue;
        statistics.assign(numStatistics, VkPipelineExecutableStatisticKHR{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
        if (m_pfnGetPipelineExecutableStatistics((VkDevice)m_device, &executableInfo, &numStatistics, statistics.data()) != VK_SUCCESS)
            continue;
        oc::string line = oc::format("{}\t{}", name, executables[i].name);
        for (const VkPipelineExecutableStatisticKHR& statistic : statistics)
        {
            // Integers print their LOW 32 bits: the NVIDIA driver reports "Local Memory Size" as 2^36 + bytes
            // on every stage (0, 32, 80, ... in the low word); every other statistic is far below 2^32.
            switch (statistic.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: line += oc::format("\t{}={}", statistic.name, statistic.value.b32 != 0); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:  line += oc::format("\t{}={}", statistic.name, (uint32)statistic.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: line += oc::format("\t{}={}", statistic.name, (uint32)statistic.value.u64); break;
            default:                                                 line += oc::format("\t{}={}", statistic.name, statistic.value.f64); break;
            }
        }
        Log::info(line);
        text += line + "\n";
    }

    // Pipelines are created on main (startup, F5, tweak reloads); the mutex only keeps the text whole if
    // one is ever created elsewhere.
    std::lock_guard<std::mutex> lock(m_pipelineStatsMutex);
    m_pipelineStatsText += text;
    (void)FileSystem::writeFileStr("Local/pipeline_stats.txt", m_pipelineStatsText, /*allowMainThread*/ true);
}

bool Device::supportsExtensions(oc::vector<const char*> extensions)
{
    auto enumResult = m_physicalDevice.enumerateDeviceExtensionProperties();
    if (enumResult.result != vk::Result::eSuccess)
    {
        assert(false && "Could not enumerate device extension properties\n");
        return false;
    }
    oc::vector<vk::ExtensionProperties> extensionProperties = oc::fromStd(enumResult.value);
    for (const char* deviceExtension : extensions)
    {
        bool found = false;
        for (vk::ExtensionProperties extensionProperty : extensionProperties)
        {
            if (strcmp(deviceExtension, extensionProperty.extensionName) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

uint32 Device::findMemoryType(uint32 typeBits, vk::MemoryPropertyFlags requirementsMask) const
{
    vk::PhysicalDeviceMemoryProperties memoryProperties = m_physicalDevice.getMemoryProperties();
    uint32 typeIndex = uint32(~0);
    for (uint32 i = 0; i < memoryProperties.memoryTypeCount; i++)
    {
        if ((typeBits & 1) && ((memoryProperties.memoryTypes[i].propertyFlags & requirementsMask) == requirementsMask))
        {
            typeIndex = i;
            break;
        }
        typeBits >>= 1;
    }
    assert(typeIndex != uint32(~0));
    return typeIndex;
}