// libocngx.so - see ocngx.h. Built by Linux/Linux.cmake with GCC (not the engine's clang + libc++), against the NGX SDK
// the flake pins (OC_NGX_SDK).
#include "ocngx.h"

#include <vulkan/vulkan.h>
#include "nvsdk_ngx_helpers_vk.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string>

#define OCNGX_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
    OcNgxLog g_log = nullptr;
    VkDevice g_device = VK_NULL_HANDLE;
    bool g_initialized = false;
    bool g_available = false;
    NVSDK_NGX_Parameter* g_params = nullptr; // the capability parameters; also the creation parameters, as NVIDIA's samples do
    NVSDK_NGX_Handle* g_dlss = nullptr;
    // Everything ocngx_init hands NGX outlives it: NGX may keep the pointers (the paths, the log callback) until shutdown.
    std::wstring g_selfDir;
    std::wstring g_dataPath;
    const wchar_t* g_searchPaths[1] = {};
    NVSDK_NGX_FeatureCommonInfo g_featureInfo{};

    void log(const char* message)
    {
        if (g_log)
            g_log(message);
    }

    void ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        log(message);
    }

    // UTF-8 to UTF-32 (wchar_t is 32-bit here). Invalid bytes become U+FFFD.
    std::wstring widen(const char* text)
    {
        std::wstring out;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : "");
        while (*p)
        {
            unsigned int c = *p++;
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (c >= 0x80 && c < 0xC0)
            {
                out.push_back(L'\xFFFD');
                continue;
            }
            c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
            for (; extra > 0 && (*p & 0xC0) == 0x80; --extra)
                c = (c << 6) | (*p++ & 0x3F);
            out.push_back(extra == 0 ? (wchar_t)c : L'\xFFFD');
        }
        return out;
    }

    std::wstring selfDirectory()
    {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(&selfDirectory), &info) || !info.dli_fname)
            return L".";
        std::string path = info.dli_fname;
        const size_t slash = path.rfind('/');
        return widen(slash == std::string::npos ? "." : path.substr(0, slash).c_str());
    }

    NVSDK_NGX_Resource_VK resource(const OcNgxImage& image, bool readWrite)
    {
        VkImageSubresourceRange range{};
        range.aspectMask = image.aspectMask;
        range.levelCount = 1;
        range.layerCount = 1;
        return NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)image.view, (VkImage)image.image, range,
            (VkFormat)image.format, image.width, image.height, readWrite);
    }
}

OCNGX_EXPORT int ocngx_abi(void)
{
    return OCNGX_ABI;
}

OCNGX_EXPORT int ocngx_required_extensions(uint32_t* instanceCount, const char* const** instanceExtensions,
                                           uint32_t* deviceCount, const char* const** deviceExtensions)
{
    unsigned int numInstance = 0, numDevice = 0;
    const char** instance = nullptr;
    const char** device = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(&numInstance, &instance, &numDevice, &device);
    if (NVSDK_NGX_FAILED(result))
        return (int)result;
    *instanceCount = numInstance;
    *instanceExtensions = instance;
    *deviceCount = numDevice;
    *deviceExtensions = device;
    return 0;
}

OCNGX_EXPORT int ocngx_init(const char* projectId, const char* dataPath, void* instance, void* physicalDevice, void* device,
                            void* getInstanceProcAddr, void* getDeviceProcAddr, int verbose, OcNgxLog logCallback)
{
    if (g_initialized)
        return 0;
    g_log = logCallback;
    g_selfDir = selfDirectory();
    g_searchPaths[0] = g_selfDir.c_str();

    g_featureInfo = NVSDK_NGX_FeatureCommonInfo{};
    g_featureInfo.PathListInfo.Path = g_searchPaths;
    g_featureInfo.PathListInfo.Length = 1;
    g_featureInfo.LoggingInfo.LoggingCallback = &ngxLog;
    g_featureInfo.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    g_featureInfo.LoggingInfo.DisableOtherLoggingSinks = true;

    g_dataPath = widen(dataPath);
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dataPath.c_str(),
        (VkInstance)instance, (VkPhysicalDevice)physicalDevice, (VkDevice)device,
        (PFN_vkGetInstanceProcAddr)getInstanceProcAddr, (PFN_vkGetDeviceProcAddr)getDeviceProcAddr, &g_featureInfo, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result))
        return (int)result;
    g_initialized = true;
    g_device = (VkDevice)device;

    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(result))
        return (int)result;
    int available = 0;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available)))
        available = 0;
    if (!available)
    {
        int needsDriver = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        char message[160];
        snprintf(message, sizeof(message), "DLSS is not available (needs a newer driver: %d, feature init result 0x%08x)\n",
            needsDriver, (unsigned)initResult);
        log(message);
    }
    g_available = available != 0;
    return 0;
}

OCNGX_EXPORT int ocngx_dlss_available(void)
{
    return g_initialized && g_available ? 1 : 0;
}

OCNGX_EXPORT int ocngx_dlss_optimal_size(int perfQuality, uint32_t outputWidth, uint32_t outputHeight,
                                         uint32_t* renderWidth, uint32_t* renderHeight)
{
    if (!g_available)
        return (int)NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    // NGX_DLSS_GET_OPTIMAL_SETTINGS (nvsdk_ngx_helpers.h), without that header's D3D and CUDA helpers.
    void* callback = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_params, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
    if (!callback)
        return (int)NVSDK_NGX_Result_FAIL_OutOfDate;
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_Width, outputWidth);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_Height, outputHeight);
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_PerfQualityValue, perfQuality);
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_RTXValue, 0);
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(g_params);
    if (NVSDK_NGX_FAILED(result))
        return (int)result;
    unsigned int width = 0, height = 0;
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_OutWidth, &width);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_OutHeight, &height);
    *renderWidth = width;
    *renderHeight = height;
    return 0;
}

OCNGX_EXPORT void ocngx_dlss_release(void)
{
    if (g_dlss)
        NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
    g_dlss = nullptr;
}

OCNGX_EXPORT int ocngx_dlss_create(void* commandBuffer, int perfQuality, int preset, uint32_t renderWidth, uint32_t renderHeight,
                                   uint32_t outputWidth, uint32_t outputHeight, int flags, int outputSubrects)
{
    if (!g_available)
        return (int)NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    ocngx_dlss_release();
    const char* presetNames[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    };
    for (const char* name : presetNames)
        NVSDK_NGX_Parameter_SetUI(g_params, name, (unsigned int)preset);

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = renderWidth;
    create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = outputWidth;
    create.Feature.InTargetHeight = outputHeight;
    create.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)perfQuality;
    create.InFeatureCreateFlags = flags;
    create.InEnableOutputSubrects = outputSubrects != 0;
    const NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, (VkCommandBuffer)commandBuffer, 1, 1, &g_dlss, g_params, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        g_dlss = nullptr;
        return (int)result;
    }
    return 0;
}

OCNGX_EXPORT int ocngx_dlss_evaluate(void* commandBuffer, const OcNgxEval* eval)
{
    if (!g_dlss)
        return (int)NVSDK_NGX_Result_FAIL_NotInitialized;
    NVSDK_NGX_Resource_VK colorIn = resource(eval->colorIn, false);
    NVSDK_NGX_Resource_VK colorOut = resource(eval->colorOut, true);
    NVSDK_NGX_Resource_VK depth = resource(eval->depth, false);
    NVSDK_NGX_Resource_VK motion = resource(eval->motion, false);
    NVSDK_NGX_Resource_VK bias = resource(eval->bias, false);

    NVSDK_NGX_VK_DLSS_Eval_Params params{};
    params.Feature.pInColor = &colorIn;
    params.Feature.pInOutput = &colorOut;
    params.pInDepth = &depth;
    params.pInMotionVectors = &motion;
    params.pInBiasCurrentColorMask = eval->bias.image ? &bias : nullptr;
    params.InJitterOffsetX = eval->jitterX;
    params.InJitterOffsetY = eval->jitterY;
    params.InRenderSubrectDimensions = { eval->renderWidth, eval->renderHeight };
    params.InReset = eval->reset;
    params.InMVScaleX = eval->mvScaleX;
    params.InMVScaleY = eval->mvScaleY;
    params.InColorSubrectBase = { eval->renderOffsetX, eval->renderOffsetY };
    params.InDepthSubrectBase = { eval->renderOffsetX, eval->renderOffsetY };
    params.InMVSubrectBase = { eval->renderOffsetX, eval->renderOffsetY };
    params.InBiasCurrentColorSubrectBase = { eval->renderOffsetX, eval->renderOffsetY };
    params.InOutputSubrectBase = { eval->outputOffsetX, eval->outputOffsetY };
    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT((VkCommandBuffer)commandBuffer, g_dlss, g_params, &params);
    return NVSDK_NGX_FAILED(result) ? (int)result : 0;
}

OCNGX_EXPORT void ocngx_shutdown(void)
{
    if (!g_initialized)
        return;
    ocngx_dlss_release();
    if (g_params)
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
    g_params = nullptr;
    NVSDK_NGX_VULKAN_Shutdown1(g_device);
    g_initialized = false;
    g_available = false;
    g_device = VK_NULL_HANDLE;
}
