module;

#ifdef _MSC_VER
#include <vulkan/vulkan_core.h> // for vkCmdPushDescriptorSetKHR
#else
#include "LinuxGmfPrelude.h"
#endif

export module RendererVK:VK;

export import vulkan_hpp;
import Core;

#ifndef _MSC_VER
// clang: the C API is exported from here (MSVC reaches it through the GMF include above): Device / Instance name the
// Vk* debug-utils types directly.
export extern "C++"
{
#include <vulkan/vulkan_core.h>
}
#endif

#undef VK_NULL_HANDLE
export nullptr_t VK_NULL_HANDLE = nullptr;

#undef VK_QUEUE_FAMILY_IGNORED
export constexpr uint32 VK_QUEUE_FAMILY_IGNORED = (~0u);
#undef VK_WHOLE_SIZE
export constexpr uint64 VK_WHOLE_SIZE = (~0ULL);

export const char* VK_VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";

#undef VK_MAKE_API_VERSION
export constexpr uint32 VK_MAKE_API_VERSION(uint32 variant, uint32 major, uint32 minor, uint32 patch)
{
    return ((((uint32)(variant)) << 29U) | (((uint32)(major)) << 22U) | (((uint32)(minor)) << 12U) | ((uint32)(patch)));
}

#undef VK_MAKE_VERSION
export constexpr uint32 VK_MAKE_VERSION(uint32 major, uint32 minor, uint32 patch)
{
    return ((((uint32)(major)) << 22U) | (((uint32)(minor)) << 12U) | ((uint32)(patch)));
}

export PFN_vkCmdPushDescriptorSetKHR pfVkCmdPushDescriptorSetKHR = nullptr;
export extern "C" void vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites)
{
    pfVkCmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}

// VK_EXT_device_generated_commands entrypoints.
// vulkan-1.lib's static loader does not export these, so we forward the global C symbols
// (used by vulkan.hpp's static dispatcher) to function pointers loaded in Device::initialize.
export PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pfVkGetGeneratedCommandsMemoryRequirementsEXT = nullptr;
export extern "C" void vkGetGeneratedCommandsMemoryRequirementsEXT(VkDevice device, const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo, VkMemoryRequirements2* pMemoryRequirements)
{
    pfVkGetGeneratedCommandsMemoryRequirementsEXT(device, pInfo, pMemoryRequirements);
}

export PFN_vkCmdPreprocessGeneratedCommandsEXT pfVkCmdPreprocessGeneratedCommandsEXT = nullptr;
export extern "C" void vkCmdPreprocessGeneratedCommandsEXT(VkCommandBuffer commandBuffer, const VkGeneratedCommandsInfoEXT* pGeneratedCommandsInfo, VkCommandBuffer stateCommandBuffer)
{
    pfVkCmdPreprocessGeneratedCommandsEXT(commandBuffer, pGeneratedCommandsInfo, stateCommandBuffer);
}

export PFN_vkCmdExecuteGeneratedCommandsEXT pfVkCmdExecuteGeneratedCommandsEXT = nullptr;
export extern "C" void vkCmdExecuteGeneratedCommandsEXT(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed, const VkGeneratedCommandsInfoEXT* pGeneratedCommandsInfo)
{
    pfVkCmdExecuteGeneratedCommandsEXT(commandBuffer, isPreprocessed, pGeneratedCommandsInfo);
}

export PFN_vkCreateIndirectCommandsLayoutEXT pfVkCreateIndirectCommandsLayoutEXT = nullptr;
export extern "C" VkResult vkCreateIndirectCommandsLayoutEXT(VkDevice device, const VkIndirectCommandsLayoutCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkIndirectCommandsLayoutEXT* pIndirectCommandsLayout)
{
    return pfVkCreateIndirectCommandsLayoutEXT(device, pCreateInfo, pAllocator, pIndirectCommandsLayout);
}

export PFN_vkDestroyIndirectCommandsLayoutEXT pfVkDestroyIndirectCommandsLayoutEXT = nullptr;
export extern "C" void vkDestroyIndirectCommandsLayoutEXT(VkDevice device, VkIndirectCommandsLayoutEXT indirectCommandsLayout, const VkAllocationCallbacks* pAllocator)
{
    pfVkDestroyIndirectCommandsLayoutEXT(device, indirectCommandsLayout, pAllocator);
}

export PFN_vkCreateIndirectExecutionSetEXT pfVkCreateIndirectExecutionSetEXT = nullptr;
export extern "C" VkResult vkCreateIndirectExecutionSetEXT(VkDevice device, const VkIndirectExecutionSetCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkIndirectExecutionSetEXT* pIndirectExecutionSet)
{
    return pfVkCreateIndirectExecutionSetEXT(device, pCreateInfo, pAllocator, pIndirectExecutionSet);
}

export PFN_vkDestroyIndirectExecutionSetEXT pfVkDestroyIndirectExecutionSetEXT = nullptr;
export extern "C" void vkDestroyIndirectExecutionSetEXT(VkDevice device, VkIndirectExecutionSetEXT indirectExecutionSet, const VkAllocationCallbacks* pAllocator)
{
    pfVkDestroyIndirectExecutionSetEXT(device, indirectExecutionSet, pAllocator);
}

export PFN_vkUpdateIndirectExecutionSetPipelineEXT pfVkUpdateIndirectExecutionSetPipelineEXT = nullptr;
export extern "C" void vkUpdateIndirectExecutionSetPipelineEXT(VkDevice device, VkIndirectExecutionSetEXT indirectExecutionSet, uint32_t executionSetWriteCount, const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites)
{
    pfVkUpdateIndirectExecutionSetPipelineEXT(device, indirectExecutionSet, executionSetWriteCount, pExecutionSetWrites);
}

// VK_KHR_calibrated_timestamps entrypoint (optional extension - GpuProfiler's GPU<->CPU timeline
// alignment). Loaded in Device::initialize only when the extension is supported; guarded by
// Device::supportsCalibratedTimestamps() before any call.
export PFN_vkGetCalibratedTimestampsKHR pfVkGetCalibratedTimestampsKHR = nullptr;
export extern "C" VkResult vkGetCalibratedTimestampsKHR(VkDevice device, uint32_t timestampCount, const VkCalibratedTimestampInfoKHR* pTimestampInfos, uint64_t* pTimestamps, uint64_t* pMaxDeviation)
{
    return pfVkGetCalibratedTimestampsKHR(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
}

// VK_KHR_acceleration_structure entrypoints. Like the DGC functions above, vulkan-1.lib's static
// loader does not export these, so forward the global C symbols (called by vulkan.hpp's static
// dispatcher) to function pointers loaded in Device::initialize.
export PFN_vkGetAccelerationStructureBuildSizesKHR pfVkGetAccelerationStructureBuildSizesKHR = nullptr;
export extern "C" void vkGetAccelerationStructureBuildSizesKHR(VkDevice device, VkAccelerationStructureBuildTypeKHR buildType, const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo, const uint32_t* pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo)
{
    pfVkGetAccelerationStructureBuildSizesKHR(device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
}

export PFN_vkCreateAccelerationStructureKHR pfVkCreateAccelerationStructureKHR = nullptr;
export extern "C" VkResult vkCreateAccelerationStructureKHR(VkDevice device, const VkAccelerationStructureCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkAccelerationStructureKHR* pAccelerationStructure)
{
    return pfVkCreateAccelerationStructureKHR(device, pCreateInfo, pAllocator, pAccelerationStructure);
}

export PFN_vkDestroyAccelerationStructureKHR pfVkDestroyAccelerationStructureKHR = nullptr;
export extern "C" void vkDestroyAccelerationStructureKHR(VkDevice device, VkAccelerationStructureKHR accelerationStructure, const VkAllocationCallbacks* pAllocator)
{
    pfVkDestroyAccelerationStructureKHR(device, accelerationStructure, pAllocator);
}

export PFN_vkCmdBuildAccelerationStructuresKHR pfVkCmdBuildAccelerationStructuresKHR = nullptr;
export extern "C" void vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos, const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos)
{
    pfVkCmdBuildAccelerationStructuresKHR(commandBuffer, infoCount, pInfos, ppBuildRangeInfos);
}

export PFN_vkGetAccelerationStructureDeviceAddressKHR pfVkGetAccelerationStructureDeviceAddressKHR = nullptr;
export extern "C" VkDeviceAddress vkGetAccelerationStructureDeviceAddressKHR(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo)
{
    return pfVkGetAccelerationStructureDeviceAddressKHR(device, pInfo);
}

export PFN_vkCmdCopyAccelerationStructureKHR pfVkCmdCopyAccelerationStructureKHR = nullptr;
export extern "C" void vkCmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureInfoKHR* pInfo)
{
    pfVkCmdCopyAccelerationStructureKHR(commandBuffer, pInfo);
}

export PFN_vkCmdWriteAccelerationStructuresPropertiesKHR pfVkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
export extern "C" void vkCmdWriteAccelerationStructuresPropertiesKHR(VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount, const VkAccelerationStructureKHR* pAccelerationStructures, VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery)
{
    pfVkCmdWriteAccelerationStructuresPropertiesKHR(commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);
}