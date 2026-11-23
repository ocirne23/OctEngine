// libocngx.so: NVIDIA's NGX SDK (DLSS Super Resolution) behind a plain C interface. Linux only; Streamline.cpp is its one
// user (it dlopens the library next to the executable, the place the Windows build copies sl.interposer.dll to).
//
// Why a separate library: the SDK's libnvsdk_ngx.a is built against libstdc++, and the engine runs on libc++. The shim is
// compiled with GCC and links libstdc++ statically with hidden symbols, so the two C++ runtimes never meet in one symbol
// scope (cuDNN, which meets the engine's libc++ there, shows what that costs). Nothing but these C functions crosses.
//
// Vulkan handles cross as void* (every handle here is a pointer on 64-bit), so this header needs no Vulkan include.
// Every int result is 0 on success, else the NVSDK_NGX_Result code.
#pragma once

#include <stdint.h>

#define OCNGX_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*OcNgxLog)(const char* message);

typedef struct OcNgxImage
{
    void* image;         // VkImage
    void* view;          // VkImageView
    uint32_t format;     // VkFormat
    uint32_t width;      // the whole image
    uint32_t height;
    uint32_t aspectMask; // VkImageAspectFlags
} OcNgxImage;

typedef struct OcNgxEval
{
    OcNgxImage colorIn;  // render resolution, in a shader-readable layout
    OcNgxImage colorOut; // output resolution, VK_IMAGE_LAYOUT_GENERAL
    OcNgxImage depth;
    OcNgxImage motion;   // pixels at render resolution, current -> previous
    OcNgxImage bias;     // optional (image null): lerp(history, current, bias)
    uint32_t renderOffsetX, renderOffsetY;
    uint32_t renderWidth, renderHeight;
    uint32_t outputOffsetX, outputOffsetY;
    float jitterX, jitterY; // render pixels
    float mvScaleX, mvScaleY;
    int reset;
} OcNgxEval;

int ocngx_abi(void);
// The instance / device extensions NGX needs; valid before ocngx_init. The arrays belong to the library.
int ocngx_required_extensions(uint32_t* instanceCount, const char* const** instanceExtensions,
                              uint32_t* deviceCount, const char* const** deviceExtensions);
// After the device exists. getInstanceProcAddr / getDeviceProcAddr: PFN_vkGetInstanceProcAddr / PFN_vkGetDeviceProcAddr.
// The DLSS model (libnvidia-ngx-dlss.so.*) is searched next to this library.
int ocngx_init(const char* projectId, const char* dataPath, void* instance, void* physicalDevice, void* device,
               void* getInstanceProcAddr, void* getDeviceProcAddr, int verbose, OcNgxLog log);
int ocngx_dlss_available(void);
// perfQuality: NVSDK_NGX_PerfQuality_Value.
int ocngx_dlss_optimal_size(int perfQuality, uint32_t outputWidth, uint32_t outputHeight, uint32_t* renderWidth, uint32_t* renderHeight);
// Records the feature's creation into `commandBuffer`; replaces a feature that exists. flags: NVSDK_NGX_DLSS_Feature_Flags.
// preset: NVSDK_NGX_DLSS_Hint_Render_Preset, for every quality mode.
int ocngx_dlss_create(void* commandBuffer, int perfQuality, int preset, uint32_t renderWidth, uint32_t renderHeight,
                      uint32_t outputWidth, uint32_t outputHeight, int flags, int outputSubrects);
int ocngx_dlss_evaluate(void* commandBuffer, const OcNgxEval* eval);
// Frees the feature (the GPU must be done with it).
void ocngx_dlss_release(void);
void ocngx_shutdown(void);

typedef int (*PFN_ocngx_abi)(void);
typedef int (*PFN_ocngx_required_extensions)(uint32_t*, const char* const**, uint32_t*, const char* const**);
typedef int (*PFN_ocngx_init)(const char*, const char*, void*, void*, void*, void*, void*, int, OcNgxLog);
typedef int (*PFN_ocngx_dlss_available)(void);
typedef int (*PFN_ocngx_dlss_optimal_size)(int, uint32_t, uint32_t, uint32_t*, uint32_t*);
typedef int (*PFN_ocngx_dlss_create)(void*, int, int, uint32_t, uint32_t, uint32_t, uint32_t, int, int);
typedef int (*PFN_ocngx_dlss_evaluate)(void*, const OcNgxEval*);
typedef void (*PFN_ocngx_dlss_release)(void);
typedef void (*PFN_ocngx_shutdown)(void);

#ifdef __cplusplus
}
#endif
