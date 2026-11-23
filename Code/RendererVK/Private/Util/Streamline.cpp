module;

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <vulkan/vulkan_core.h>
#pragma warning(push, 0)
#include <sl/sl.h>
#include <sl/sl_consts.h>
#include <sl/sl_hooks.h>
#include <sl/sl_dlss.h>
#include <sl/sl_helpers_vk.h>
#include <sl/sl_security.h>
#pragma warning(pop)
#else
#include <dlfcn.h>
#include <unistd.h>
#include "ocngx.h"
#endif

module RendererVK;

import Core;
import Core.glm;
import :VK;
import :Streamline;

#ifndef _MSC_VER
import :Device;

// Linux: NVIDIA ships Streamline for Windows only. DLSS goes through the NGX SDK itself, in libocngx.so next to the
// executable (Linux/Include/ocngx.h: a GCC-built C shim, because the SDK needs libstdc++). NGX needs no swapchain
// proxies and no queues of its own: the swapchain calls go straight to the device, and the upscale is recorded into the
// frame's command buffer. Without the library, on another GPU or an old driver, every call behaves as the Windows build
// does without sl.interposer.dll.
namespace
{
    struct Ngx
    {
        PFN_ocngx_required_extensions requiredExtensions = nullptr;
        PFN_ocngx_init init = nullptr;
        PFN_ocngx_dlss_available dlssAvailable = nullptr;
        PFN_ocngx_dlss_optimal_size optimalSize = nullptr;
        PFN_ocngx_dlss_create create = nullptr;
        PFN_ocngx_dlss_evaluate evaluate = nullptr;
        PFN_ocngx_dlss_release release = nullptr;
        PFN_ocngx_shutdown shutdown = nullptr;
    };
    Ngx g_ngx;
    bool g_loadAttempted = false;
    bool g_loaded = false;      // the shim resolved: its extension lists are in
    bool g_initialized = false; // ocngx_init succeeded on the device
    bool g_verbose = false;
    oc::string g_exeDir;
    oc::vector<const char*> g_instanceExtensions;
    oc::vector<const char*> g_deviceExtensions;

    // What the feature was last created with: creation is only repeated on a change.
    struct AppliedOptions
    {
        int mode = -1;
        int preset = -1;
        glm::uvec2 renderSize{ 0 };
        glm::uvec2 outputSize{ 0 };
    };
    AppliedOptions g_applied;

    constexpr const char* PROJECT_ID = "5c0b6e1a-8f42-4d7e-9a31-b7d2e4c8f615"; // the Windows build's (Streamline)

    // NVSDK_NGX_PerfQuality_Value and NVSDK_NGX_DLSS_Feature_Flags (nvsdk_ngx_defs.h).
    int toNgxQuality(Streamline::DlssMode mode)
    {
        switch (mode)
        {
        case Streamline::DlssMode::DLAA:             return 5;
        case Streamline::DlssMode::Quality:          return 2;
        case Streamline::DlssMode::Balanced:         return 1;
        case Streamline::DlssMode::Performance:      return 0;
        case Streamline::DlssMode::UltraPerformance: return 3;
        default:                                     return -1;
        }
    }
    constexpr int NGX_FLAG_IS_HDR = 1 << 0;
    constexpr int NGX_FLAG_MV_LOW_RES = 1 << 1;     // motion vectors at render resolution
    constexpr int NGX_FLAG_DEPTH_INVERTED = 1 << 3; // reversed-Z
    constexpr int NGX_FLAG_AUTO_EXPOSURE = 1 << 6;  // the engine's exposure runs after the upscale

    void logMessage(const char* message)
    {
        if (g_verbose)
            printf("NGX: %s", message);
    }

    OcNgxImage toNgx(const Streamline::Image& image)
    {
        return OcNgxImage{
            .image = static_cast<VkImage>(image.image), .view = static_cast<VkImageView>(image.view),
            .format = (uint32_t)static_cast<VkFormat>(image.format), .width = image.size.x, .height = image.size.y,
            .aspectMask = (uint32_t)static_cast<VkImageAspectFlags>(image.aspect) };
    }

    // The previous frames may still be reading the feature's resources: it is only replaced or freed on an idle queue.
    void releaseFeature()
    {
        if (g_applied.mode < 0)
            return;
        (void)Globals::device.graphicsQueueWaitIdle();
        g_ngx.release();
        g_applied = AppliedOptions{};
    }
}

bool Streamline::loaded() { return g_loaded; }
bool Streamline::dlssAvailable() { return g_initialized && g_ngx.dlssAvailable(); }

bool Streamline::load(const oc::string&, bool verbose)
{
    if (g_loadAttempted)
        return g_loaded;
    g_loadAttempted = true;
    g_verbose = verbose;

    char exe[4096];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0)
        return false;
    exe[length] = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash)
        return false;
    *slash = '\0';
    g_exeDir = exe;
    const oc::string path = g_exeDir + "/libocngx.so";
    // RTLD_LOCAL: the shim's symbols stay out of the global scope (it carries its own hidden libstdc++).
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        printf("NGX: %s not loaded (%s), DLSS off\n", path.c_str(), dlerror());
        return false;
    }
    bool ok = true;
    auto resolve = [&](auto& fn, const char* name)
    {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(module, name));
        ok &= fn != nullptr;
    };
    PFN_ocngx_abi abi = nullptr;
    resolve(abi, "ocngx_abi");
    resolve(g_ngx.requiredExtensions, "ocngx_required_extensions");
    resolve(g_ngx.init, "ocngx_init");
    resolve(g_ngx.dlssAvailable, "ocngx_dlss_available");
    resolve(g_ngx.optimalSize, "ocngx_dlss_optimal_size");
    resolve(g_ngx.create, "ocngx_dlss_create");
    resolve(g_ngx.evaluate, "ocngx_dlss_evaluate");
    resolve(g_ngx.release, "ocngx_dlss_release");
    resolve(g_ngx.shutdown, "ocngx_shutdown");
    if (!ok || abi() != OCNGX_ABI)
    {
        printf("NGX: libocngx.so is missing entry points or has another ABI version, DLSS off\n");
        g_ngx = Ngx{};
        return false; // the module stays mapped: nothing it owns is live, and unmapping it buys nothing
    }

    uint32_t instanceCount = 0, deviceCount = 0;
    const char* const* instanceExtensions = nullptr;
    const char* const* deviceExtensions = nullptr;
    const int result = g_ngx.requiredExtensions(&instanceCount, &instanceExtensions, &deviceCount, &deviceExtensions);
    if (result != 0)
    {
        printf("NGX: the extension query failed (0x%08x), DLSS off\n", (unsigned)result);
        g_ngx = Ngx{};
        return false;
    }
    g_instanceExtensions.assign(instanceExtensions, instanceExtensions + instanceCount);
    g_deviceExtensions.assign(deviceExtensions, deviceExtensions + deviceCount);
    g_loaded = true;
    return true;
}

void Streamline::shutdown()
{
    if (g_initialized)
    {
        releaseFeature();
        g_ngx.shutdown();
    }
    g_initialized = false;
}

void Streamline::appendInstanceExtensions(oc::vector<const char*>& extensions)
{
    for (const char* name : g_instanceExtensions)
        if (oc::find_if(extensions.begin(), extensions.end(), [&](const char* have) { return strcmp(have, name) == 0; }) == extensions.end())
            extensions.push_back(name);
}

void Streamline::appendDeviceExtensions(oc::vector<const char*>& extensions)
{
    for (const char* name : g_deviceExtensions)
        if (oc::find_if(extensions.begin(), extensions.end(), [&](const char* have) { return strcmp(have, name) == 0; }) == extensions.end())
            extensions.push_back(name);
}

uint32 Streamline::extraGraphicsQueues() { return 0; }
uint32 Streamline::extraComputeQueues() { return 0; }
void Streamline::mergeDeviceFeatures(vk::PhysicalDeviceVulkan12Features&, vk::PhysicalDeviceVulkan13Features&) {}

void Streamline::onDeviceCreated(vk::Instance instance, vk::PhysicalDevice physicalDevice, vk::Device device,
    uint32, uint32, uint32, uint32)
{
    if (!g_loaded)
        return;
    const int result = g_ngx.init(PROJECT_ID, g_exeDir.c_str(), static_cast<VkInstance>(instance),
        static_cast<VkPhysicalDevice>(physicalDevice), static_cast<VkDevice>(device),
        reinterpret_cast<void*>(&vkGetInstanceProcAddr), reinterpret_cast<void*>(&vkGetDeviceProcAddr), g_verbose ? 1 : 0, &logMessage);
    if (result != 0)
    {
        printf("NGX: initialization failed (0x%08x), DLSS off\n", (unsigned)result);
        return;
    }
    g_initialized = true;
    printf(g_ngx.dlssAvailable() ? "NGX: DLSS available\n" : "NGX: DLSS is not supported on this device or driver\n");}

vk::Result Streamline::createSwapchain(vk::Device device, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out)
{
    auto result = device.createSwapchainKHR(info);
    out = result.value;
    return result.result;
}

void Streamline::destroySwapchain(vk::Device device, vk::SwapchainKHR swapchain)
{
    device.destroySwapchainKHR(swapchain);
}

vk::Result Streamline::getSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, oc::vector<vk::Image>& out)
{
    auto result = device.getSwapchainImagesKHR(swapchain);
    out = oc::fromStd(result.value);
    return result.result;
}

vk::Result Streamline::acquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64 timeoutNs, vk::Semaphore semaphore, uint32& imageIndex)
{
    auto result = device.acquireNextImageKHR(swapchain, timeoutNs, semaphore);
    imageIndex = result.value;
    return result.result;
}

vk::Result Streamline::queuePresent(vk::Queue queue, const vk::PresentInfoKHR& info)
{
    return queue.presentKHR(info);
}

glm::uvec2 Streamline::getDlssRenderSize(DlssMode mode, glm::uvec2 outputSize)
{
    if (!dlssAvailable() || mode == DlssMode::Off || outputSize.x == 0 || outputSize.y == 0)
        return outputSize;
    uint32_t width = 0, height = 0;
    if (g_ngx.optimalSize(toNgxQuality(mode), outputSize.x, outputSize.y, &width, &height) != 0 || width == 0 || height == 0)
        return outputSize;
    return glm::uvec2(width, height);
}

bool Streamline::evaluateDlss(vk::CommandBuffer cmd, const DlssFrame& frame)
{
    if (!dlssAvailable() || frame.mode == DlssMode::Off)
        return false;
    if (frame.outputSize.x < 32 || frame.outputSize.y < 32)
        return false; // NGX rejects a smaller output (InvalidParameter); a startup frame can be that small
    VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmd);

    if (g_applied.mode != (int)frame.mode || g_applied.preset != frame.preset
        || g_applied.renderSize != frame.renderSize || g_applied.outputSize != frame.outputSize)
    {
        releaseFeature();
        const int flags = NGX_FLAG_IS_HDR | NGX_FLAG_MV_LOW_RES | NGX_FLAG_DEPTH_INVERTED | NGX_FLAG_AUTO_EXPOSURE;
        const bool outputSubrect = frame.outputOffset != glm::uvec2(0);
        const int result = g_ngx.create(vkCmd, toNgxQuality(frame.mode), frame.preset, frame.renderSize.x, frame.renderSize.y,
            frame.outputSize.x, frame.outputSize.y, flags, outputSubrect ? 1 : 0);
        if (result != 0)
        {
            printf("NGX: creating the DLSS feature failed (0x%08x)\n", (unsigned)result);
            return false;
        }
        g_applied = AppliedOptions{ .mode = (int)frame.mode, .preset = frame.preset, .renderSize = frame.renderSize, .outputSize = frame.outputSize };
    }

    // The motion vectors are already render pixels (Windows tells Streamline 1 / renderSize, its unit being the screen).
    OcNgxEval eval{
        .colorIn = toNgx(frame.colorIn), .colorOut = toNgx(frame.colorOut), .depth = toNgx(frame.depth),
        .motion = toNgx(frame.motion), .bias = toNgx(frame.biasCurrentColor),
        .renderOffsetX = frame.renderOffset.x, .renderOffsetY = frame.renderOffset.y,
        .renderWidth = frame.renderSize.x, .renderHeight = frame.renderSize.y,
        .outputOffsetX = frame.outputOffset.x, .outputOffsetY = frame.outputOffset.y,
        .jitterX = frame.jitterPx.x, .jitterY = frame.jitterPx.y,
        .mvScaleX = 1.0f, .mvScaleY = 1.0f,
        .reset = frame.reset ? 1 : 0 };
    const int result = g_ngx.evaluate(vkCmd, &eval);
    if (result != 0)
        printf("NGX: evaluating DLSS failed (0x%08x)\n", (unsigned)result);
    return result == 0;
}

void Streamline::freeDlss()
{
    if (dlssAvailable())
        releaseFeature();
}
#else

namespace
{
    // Every entry point is a pointer, resolved from sl.interposer.dll (core API) or through
    // slGetFeatureFunction (the DLSS plugin) - nothing links the interposer.
    struct Api
    {
        HMODULE module = nullptr;
        PFun_slInit* init = nullptr;
        PFun_slShutdown* shutdown = nullptr;
        PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
        PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
        PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
        PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
        PFun_slSetConstants* setConstants = nullptr;
        PFun_slSetTagForFrame* setTagForFrame = nullptr;
        PFun_slEvaluateFeature* evaluateFeature = nullptr;
        PFun_slFreeResources* freeResources = nullptr;
        PFun_slSetVulkanInfo* setVulkanInfo = nullptr;
        PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

        PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;
        PFun_slDLSSSetOptions* dlssSetOptions = nullptr;

        // The manual hooking proxies (sl_hooks.h, Vulkan).
        PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
        PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
        PFN_vkGetSwapchainImagesKHR getSwapchainImages = nullptr;
        PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
        PFN_vkQueuePresentKHR queuePresent = nullptr;
    };
    Api g_sl;
    bool g_loadAttempted = false;
    bool g_initialized = false; // slInit succeeded: the proxies are mandatory from here on
    bool g_dlssSupported = false;
    sl::FeatureRequirements g_dlssRequirements;

    // What slDLSSSetOptions last got: it re-creates the feature, so it is only called on a change.
    struct AppliedOptions
    {
        int mode = -1;
        int preset = -1;
        glm::uvec2 outputSize{ 0 };
    };
    AppliedOptions g_applied;

    constexpr const char* PROJECT_ID = "5c0b6e1a-8f42-4d7e-9a31-b7d2e4c8f615";

    void logMessage(sl::LogType type, const char* msg)
    {
#ifdef _DEBUG
        if (type == sl::LogType::eInfo)
            return; // the SL log file already has them; the console only gets warnings and errors
        printf("Streamline: %s", msg);
#endif
    }

    sl::DLSSMode toSlMode(Streamline::DlssMode mode)
    {
        switch (mode)
        {
        case Streamline::DlssMode::DLAA:             return sl::DLSSMode::eDLAA;
        case Streamline::DlssMode::Quality:          return sl::DLSSMode::eMaxQuality;
        case Streamline::DlssMode::Balanced:         return sl::DLSSMode::eBalanced;
        case Streamline::DlssMode::Performance:      return sl::DLSSMode::eMaxPerformance;
        case Streamline::DlssMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
        default:                                     return sl::DLSSMode::eOff;
        }
    }

    // glm is column-major with column vectors, SL row-major with row vectors: the same 16 floats.
    sl::float4x4 toSl(const glm::mat4& m)
    {
        sl::float4x4 out;
        memcpy(&out, &m, sizeof(out));
        return out;
    }

    // `range` is chained to the resource, so it must live until the tag call.
    sl::Resource toSl(const Streamline::Image& image, sl::SubresourceRange& range)
    {
        range.aspectMask = (uint32_t)static_cast<VkImageAspectFlags>(image.aspect);
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        sl::Resource r(sl::ResourceType::eTex2d, (void*)static_cast<VkImage>(image.image), nullptr,
            (void*)static_cast<VkImageView>(image.view), (uint32_t)static_cast<VkImageLayout>(image.layout));
        r.width = image.size.x;
        r.height = image.size.y;
        r.nativeFormat = (uint32_t)static_cast<VkFormat>(image.format);
        r.mipLevels = 1;
        r.arrayLayers = 1;
        r.flags = 0;
        r.usage = (uint32_t)static_cast<VkImageUsageFlags>(image.usage);
        r.next = &range;
        return r;
    }

    void unload()
    {
        if (g_initialized && g_sl.shutdown)
            g_sl.shutdown();
        if (g_sl.module)
            FreeLibrary(g_sl.module);
        g_sl = Api{};
        g_initialized = false;
        g_dlssSupported = false;
        g_applied = AppliedOptions{};
    }
}

bool Streamline::loaded() { return g_initialized; }
bool Streamline::dlssAvailable() { return g_initialized && g_dlssSupported; }

bool Streamline::load(const oc::string& logDir, bool verbose)
{
    if (g_loadAttempted)
        return g_initialized;
    g_loadAttempted = true;

    // The interposer and its plugins sit next to App.exe (the App POST_BUILD copy).
    wchar_t dir[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash)
        return false;
    *slash = L'\0';
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, L"%s\\sl.interposer.dll", dir) < 0)
        return false;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    {
        printf("Streamline: sl.interposer.dll not found next to the executable, DLSS off\n");
        return false;
    }
    // Only ever load an NVIDIA-signed interposer (it loads the plugins, which it verifies itself).
    if (!sl::security::verifyEmbeddedSignature(path))
    {
        printf("Streamline: sl.interposer.dll is not signed by NVIDIA, DLSS off\n");
        return false;
    }
    g_sl.module = LoadLibraryW(path);
    if (!g_sl.module)
        return false;

    bool ok = true;
    auto resolve = [&](auto& fn, const char* name)
    {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(g_sl.module, name));
        ok &= fn != nullptr;
    };
    resolve(g_sl.init,                   "slInit");
    resolve(g_sl.shutdown,               "slShutdown");
    resolve(g_sl.isFeatureSupported,     "slIsFeatureSupported");
    resolve(g_sl.getFeatureRequirements, "slGetFeatureRequirements");
    resolve(g_sl.getFeatureFunction,     "slGetFeatureFunction");
    resolve(g_sl.getNewFrameToken,       "slGetNewFrameToken");
    resolve(g_sl.setConstants,           "slSetConstants");
    resolve(g_sl.setTagForFrame,         "slSetTagForFrame");
    resolve(g_sl.evaluateFeature,        "slEvaluateFeature");
    resolve(g_sl.freeResources,          "slFreeResources");
    resolve(g_sl.setVulkanInfo,          "slSetVulkanInfo");
    resolve(g_sl.getDeviceProcAddr,      "vkGetDeviceProcAddr");
    if (!ok)
    {
        printf("Streamline: sl.interposer.dll is missing entry points, DLSS off\n");
        unload();
        return false;
    }

    wchar_t logPath[MAX_PATH] = {};
    if (!logDir.empty() && MultiByteToWideChar(CP_UTF8, 0, logDir.c_str(), -1, logPath, MAX_PATH) == 0)
        logPath[0] = L'\0';

    const sl::Feature features[] = { sl::kFeatureDLSS };
    const wchar_t* pluginPaths[] = { dir };
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathToLogsAndData = logPath[0] ? logPath : nullptr;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = &logMessage;
    // Manual hooking: SL sees only the five swapchain calls (Streamline::createSwapchain ...), never the rest
    // of the API. No OTA: the DLSS model is the nvngx_dlss.dll shipped next to the exe, never a download.
    // Frame-based tagging: evaluateDlss tags with slSetTagForFrame (per frame token), which SL rejects without it.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking
        | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = PROJECT_ID;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result initResult = g_sl.init(pref, sl::kSDKVersion);
    if (initResult != sl::Result::eOk)
    {
        printf("Streamline: slInit failed (%d), DLSS off\n", (int)initResult);
        FreeLibrary(g_sl.module);
        g_sl = Api{};
        return false;
    }
    g_initialized = true;

    if (g_sl.getFeatureRequirements(sl::kFeatureDLSS, g_dlssRequirements) != sl::Result::eOk)
    {
        printf("Streamline: the DLSS plugin did not load, DLSS off\n");
        unload(); // nothing else to use SL for: drop it before the instance exists
        return false;
    }
    return true;
}

void Streamline::shutdown()
{
    unload();
}

void Streamline::appendInstanceExtensions(oc::vector<const char*>& extensions)
{
    if (!g_initialized)
        return;
    for (uint32 i = 0; i < g_dlssRequirements.vkNumInstanceExtensions; ++i)
    {
        const char* name = g_dlssRequirements.vkInstanceExtensions[i];
        if (oc::find_if(extensions.begin(), extensions.end(), [&](const char* have) { return strcmp(have, name) == 0; }) == extensions.end())
            extensions.push_back(name);
    }
}

void Streamline::appendDeviceExtensions(oc::vector<const char*>& extensions)
{
    if (!g_initialized)
        return;
    for (uint32 i = 0; i < g_dlssRequirements.vkNumDeviceExtensions; ++i)
    {
        const char* name = g_dlssRequirements.vkDeviceExtensions[i];
        if (oc::find_if(extensions.begin(), extensions.end(), [&](const char* have) { return strcmp(have, name) == 0; }) == extensions.end())
            extensions.push_back(name);
    }
}

uint32 Streamline::extraGraphicsQueues() { return g_initialized ? g_dlssRequirements.vkNumGraphicsQueuesRequired : 0; }
uint32 Streamline::extraComputeQueues() { return g_initialized ? g_dlssRequirements.vkNumComputeQueuesRequired : 0; }

void Streamline::mergeDeviceFeatures(vk::PhysicalDeviceVulkan12Features& features12, vk::PhysicalDeviceVulkan13Features& features13)
{
    if (!g_initialized)
        return;
    // Every member after sType/pNext is a VkBool32 in both structs: OR SL's request into ours.
    const auto merge = [](auto& ours, const auto& theirs, size_t firstOffset)
    {
        static_assert(sizeof(ours) == sizeof(theirs));
        VkBool32* dst = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8*>(&ours) + firstOffset);
        const VkBool32* src = reinterpret_cast<const VkBool32*>(reinterpret_cast<const uint8*>(&theirs) + firstOffset);
        const size_t count = (sizeof(ours) - firstOffset) / sizeof(VkBool32);
        for (size_t i = 0; i < count; ++i)
            dst[i] = dst[i] | src[i];
    };
    const VkPhysicalDeviceVulkan12Features sl12 = sl::getVkPhysicalDeviceVulkan12Features(g_dlssRequirements.vkNumFeatures12, g_dlssRequirements.vkFeatures12);
    const VkPhysicalDeviceVulkan13Features sl13 = sl::getVkPhysicalDeviceVulkan13Features(g_dlssRequirements.vkNumFeatures13, g_dlssRequirements.vkFeatures13);
    merge(features12, sl12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
    merge(features13, sl13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
}

void Streamline::onDeviceCreated(vk::Instance instance, vk::PhysicalDevice physicalDevice, vk::Device device,
    uint32 graphicsFamily, uint32 firstSlGraphicsQueue, uint32 computeFamily, uint32 firstSlComputeQueue)
{
    if (!g_initialized)
        return;
    sl::VulkanInfo info{};
    info.device = static_cast<VkDevice>(device);
    info.instance = static_cast<VkInstance>(instance);
    info.physicalDevice = static_cast<VkPhysicalDevice>(physicalDevice);
    info.graphicsQueueFamily = graphicsFamily;
    info.graphicsQueueIndex = firstSlGraphicsQueue;
    info.computeQueueFamily = computeFamily;
    info.computeQueueIndex = firstSlComputeQueue;
    if (g_sl.setVulkanInfo(info) != sl::Result::eOk)
    {
        printf("Streamline: slSetVulkanInfo failed, DLSS off\n");
        unload(); // before the swapchain: the plain Vulkan path takes over cleanly
        return;
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device);
    g_sl.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(g_sl.getDeviceProcAddr(vkDevice, "vkCreateSwapchainKHR"));
    g_sl.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(g_sl.getDeviceProcAddr(vkDevice, "vkDestroySwapchainKHR"));
    g_sl.getSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(g_sl.getDeviceProcAddr(vkDevice, "vkGetSwapchainImagesKHR"));
    g_sl.acquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(g_sl.getDeviceProcAddr(vkDevice, "vkAcquireNextImageKHR"));
    g_sl.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(g_sl.getDeviceProcAddr(vkDevice, "vkQueuePresentKHR"));
    if (!g_sl.createSwapchain || !g_sl.destroySwapchain || !g_sl.getSwapchainImages || !g_sl.acquireNextImage || !g_sl.queuePresent)
    {
        printf("Streamline: the swapchain proxies are missing, DLSS off\n");
        unload();
        return;
    }

    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = info.physicalDevice;
    const sl::Result supported = g_sl.isFeatureSupported(sl::kFeatureDLSS, adapter);
    if (supported != sl::Result::eOk)
    {
        printf("Streamline: DLSS is not supported on this adapter (%d)\n", (int)supported);
        return; // SL stays loaded: the proxies keep serving the swapchain
    }
    void* getOptimal = nullptr;
    void* setOptions = nullptr;
    if (g_sl.getFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", getOptimal) != sl::Result::eOk
        || g_sl.getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", setOptions) != sl::Result::eOk
        || !getOptimal || !setOptions)
    {
        printf("Streamline: the DLSS plugin functions are missing, DLSS off\n");
        return;
    }
    g_sl.dlssGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(getOptimal);
    g_sl.dlssSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(setOptions);
    g_dlssSupported = true;
    printf("Streamline: DLSS available\n");
}

vk::Result Streamline::createSwapchain(vk::Device device, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out)
{
    if (g_sl.createSwapchain)
    {
        VkSwapchainKHR swapchain = nullptr;
        const VkResult result = g_sl.createSwapchain(static_cast<VkDevice>(device), reinterpret_cast<const VkSwapchainCreateInfoKHR*>(&info), nullptr, &swapchain);
        out = vk::SwapchainKHR(swapchain);
        return vk::Result(result);
    }
    auto result = device.createSwapchainKHR(info);
    out = result.value;
    return result.result;
}

void Streamline::destroySwapchain(vk::Device device, vk::SwapchainKHR swapchain)
{
    if (g_sl.destroySwapchain)
        g_sl.destroySwapchain(static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), nullptr);
    else
        device.destroySwapchainKHR(swapchain);
}

vk::Result Streamline::getSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, oc::vector<vk::Image>& out)
{
    if (g_sl.getSwapchainImages)
    {
        uint32_t count = 0;
        VkResult result = g_sl.getSwapchainImages(static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), &count, nullptr);
        if (result != VK_SUCCESS)
            return vk::Result(result);
        oc::vector<VkImage> images(count);
        result = g_sl.getSwapchainImages(static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), &count, images.data());
        out.clear();
        for (uint32 i = 0; i < count; ++i)
            out.push_back(vk::Image(images[i]));
        return vk::Result(result);
    }
    auto result = device.getSwapchainImagesKHR(swapchain);
    out = oc::fromStd(result.value);
    return result.result;
}

vk::Result Streamline::acquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64 timeoutNs, vk::Semaphore semaphore, uint32& imageIndex)
{
    if (g_sl.acquireNextImage)
        return vk::Result(g_sl.acquireNextImage(static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), timeoutNs,
            static_cast<VkSemaphore>(semaphore), nullptr, &imageIndex));
    auto result = device.acquireNextImageKHR(swapchain, timeoutNs, semaphore);
    imageIndex = result.value;
    return result.result;
}

vk::Result Streamline::queuePresent(vk::Queue queue, const vk::PresentInfoKHR& info)
{
    if (g_sl.queuePresent)
        return vk::Result(g_sl.queuePresent(static_cast<VkQueue>(queue), reinterpret_cast<const VkPresentInfoKHR*>(&info)));
    return queue.presentKHR(info);
}

glm::uvec2 Streamline::getDlssRenderSize(DlssMode mode, glm::uvec2 outputSize)
{
    if (!dlssAvailable() || mode == DlssMode::Off || outputSize.x == 0 || outputSize.y == 0)
        return outputSize;
    sl::DLSSOptions options{};
    options.mode = toSlMode(mode);
    options.outputWidth = outputSize.x;
    options.outputHeight = outputSize.y;
    sl::DLSSOptimalSettings settings{};
    if (g_sl.dlssGetOptimalSettings(options, settings) != sl::Result::eOk || settings.optimalRenderWidth == 0 || settings.optimalRenderHeight == 0)
        return outputSize;
    return glm::uvec2(settings.optimalRenderWidth, settings.optimalRenderHeight);
}

bool Streamline::evaluateDlss(vk::CommandBuffer cmd, const DlssFrame& frame)
{
    if (!dlssAvailable() || frame.mode == DlssMode::Off)
        return false;
    const sl::ViewportHandle viewport(0u);

    if (g_applied.mode != (int)frame.mode || g_applied.preset != frame.preset || g_applied.outputSize != frame.outputSize)
    {
        sl::DLSSOptions options{};
        options.mode = toSlMode(frame.mode);
        options.outputWidth = frame.outputSize.x;
        options.outputHeight = frame.outputSize.y;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        // The engine's exposure is computed AFTER the upscale (eye adaptation reads the output).
        options.useAutoExposure = sl::Boolean::eTrue;
        const sl::DLSSPreset preset = (sl::DLSSPreset)frame.preset;
        options.dlaaPreset = preset;
        options.qualityPreset = preset;
        options.balancedPreset = preset;
        options.performancePreset = preset;
        options.ultraPerformancePreset = preset;
        options.ultraQualityPreset = preset;
        if (g_sl.dlssSetOptions(viewport, options) != sl::Result::eOk)
            return false;
        g_applied = AppliedOptions{ .mode = (int)frame.mode, .preset = frame.preset, .outputSize = frame.outputSize };
    }

    sl::FrameToken* token = nullptr;
    const uint32_t frameIndex = frame.frameIndex;
    if (g_sl.getNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token)
        return false;

    sl::Constants constants{};
    constants.cameraViewToClip = toSl(frame.viewToClip);
    constants.clipToCameraView = toSl(glm::inverse(frame.viewToClip));
    constants.clipToLensClip = toSl(glm::mat4(1.0f));
    constants.clipToPrevClip = toSl(frame.clipToPrevClip);
    constants.prevClipToClip = toSl(glm::inverse(frame.clipToPrevClip));
    constants.jitterOffset = sl::float2(frame.jitterPx.x, frame.jitterPx.y);
    constants.mvecScale = sl::float2(1.0f / (float)frame.renderSize.x, 1.0f / (float)frame.renderSize.y);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(frame.cameraPos.x, frame.cameraPos.y, frame.cameraPos.z);
    constants.cameraUp = sl::float3(frame.cameraUp.x, frame.cameraUp.y, frame.cameraUp.z);
    constants.cameraRight = sl::float3(frame.cameraRight.x, frame.cameraRight.y, frame.cameraRight.z);
    constants.cameraFwd = sl::float3(frame.cameraFwd.x, frame.cameraFwd.y, frame.cameraFwd.z);
    constants.cameraNear = frame.cameraNear;
    constants.cameraFar = frame.cameraFar;
    constants.cameraFOV = frame.cameraFovY;
    constants.cameraAspectRatio = frame.cameraAspect;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    if (g_sl.setConstants(constants, *token, viewport) != sl::Result::eOk)
        return false;

    sl::SubresourceRange ranges[5];
    sl::Resource colorIn = toSl(frame.colorIn, ranges[0]);
    sl::Resource colorOut = toSl(frame.colorOut, ranges[1]);
    sl::Resource depth = toSl(frame.depth, ranges[2]);
    sl::Resource motion = toSl(frame.motion, ranges[3]);
    sl::Resource bias = toSl(frame.biasCurrentColor, ranges[4]);
    const sl::Extent renderExtent{ .top = frame.renderOffset.y, .left = frame.renderOffset.x, .width = frame.renderSize.x, .height = frame.renderSize.y };
    const sl::Extent outputExtent{ .top = frame.outputOffset.y, .left = frame.outputOffset.x, .width = frame.outputSize.x, .height = frame.outputSize.y };
    // Valid until evaluate: the evaluate below is recorded right after, into the same command buffer.
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent),
        sl::ResourceTag(&bias, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent),
    };
    const uint32_t numTags = frame.biasCurrentColor.image ? 5u : 4u; // the optional tag is last
    sl::CommandBuffer* slCmd = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmd));
    if (g_sl.setTagForFrame(*token, viewport, tags, numTags, slCmd) != sl::Result::eOk)
        return false;

    const sl::BaseStructure* inputs[] = { &viewport };
    return g_sl.evaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, slCmd) == sl::Result::eOk;
}

void Streamline::freeDlss()
{
    if (!dlssAvailable() || g_applied.mode < 0)
        return;
    g_sl.freeResources(sl::kFeatureDLSS, sl::ViewportHandle(0u));
    g_applied = AppliedOptions{};
}
#endif
