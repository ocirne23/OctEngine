module;

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

module RendererVK;

import Core;
import Core.glm;
import :VK;
import :Streamline;

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
