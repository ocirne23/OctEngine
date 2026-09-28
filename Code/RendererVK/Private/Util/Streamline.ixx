export module RendererVK:Streamline;

import Core;
import Core.glm;
import :VK;

// NVIDIA Streamline (DLSS Super Resolution), MANUAL HOOKING. sl.interposer.dll is resolved at runtime,
// never linked (the Aftermath pattern): without the DLLs, on a non-NVIDIA GPU or when the signature check
// fails, every call below is a no-op or the plain Vulkan path and App.exe runs as before.
//
// Order (Renderer::initDeviceAndSwapchain):
//   load()              BEFORE the Vulkan instance: verify + load the interposer, slInit, read the DLSS
//                       requirements (extensions, 1.2/1.3 features, extra queues)
//   append*/merge*      the instance / device creation add what DLSS asks for
//   onDeviceCreated()   slSetVulkanInfo, the swapchain proxies, the DLSS support check
//   shutdown()          Device::destroy, BEFORE the device goes (slShutdown)
//
// The five swapchain calls go through the SL proxies whenever SL is loaded (the manual hooking contract:
// SL's presentCommon must run every frame), see the swapchain wrappers below.
export namespace Streamline
{
    // "Post/DLSS/Mode" order.
    enum class DlssMode : int { Off, DLAA, Quality, Balanced, Performance, UltraPerformance };

    // logDir: absolute folder for SL's log file (sl.log, plus what NGX writes); empty = console only.
    // verbose: SL's verbose log level (read once, at slInit).
    bool load(const oc::string& logDir, bool verbose);
    bool loaded();
    void shutdown();

    void appendInstanceExtensions(oc::vector<const char*>& extensions);
    void appendDeviceExtensions(oc::vector<const char*>& extensions);
    // SL's queues come AFTER the host's in the graphics / compute family.
    uint32 extraGraphicsQueues();
    uint32 extraComputeQueues();
    void mergeDeviceFeatures(vk::PhysicalDeviceVulkan12Features& features12, vk::PhysicalDeviceVulkan13Features& features13);
    void onDeviceCreated(vk::Instance instance, vk::PhysicalDevice physicalDevice, vk::Device device,
        uint32 graphicsFamily, uint32 firstSlGraphicsQueue, uint32 computeFamily, uint32 firstSlComputeQueue);

    // The swapchain calls: SL's proxies when loaded, else the Vulkan loader.
    vk::Result createSwapchain(vk::Device device, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out);
    void destroySwapchain(vk::Device device, vk::SwapchainKHR swapchain);
    vk::Result getSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, oc::vector<vk::Image>& out);
    vk::Result acquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64 timeoutNs, vk::Semaphore semaphore, uint32& imageIndex);
    vk::Result queuePresent(vk::Queue queue, const vk::PresentInfoKHR& info);

    // ---- DLSS Super Resolution ----
    bool dlssAvailable(); // loaded, the device supports it, and the plugin functions resolved
    // The render size DLSS wants for `mode` at `outputSize`. Off / unavailable: outputSize.
    glm::uvec2 getDlssRenderSize(DlssMode mode, glm::uvec2 outputSize);

    struct Image
    {
        vk::Image image;
        vk::ImageView view;
        vk::ImageLayout layout;  // the layout the image is in when evaluate() records
        vk::Format format;
        glm::uvec2 size;         // the whole image
        vk::ImageUsageFlags usage;
        // SL hands NGX a COLOR subresource unless told otherwise: a depth image must say so (else NGX fails the
        // evaluate with InvalidParameter).
        vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
    };
    struct DlssFrame
    {
        DlssMode mode;
        int preset;              // sl::DLSSPreset value (0 = default)
        uint32 frameIndex;       // monotonic
        Image colorIn;           // linear HDR, render resolution
        Image colorOut;          // output resolution, storage
        Image depth;             // hardware depth (reversed-Z)
        Image motion;            // RG16F, px at render resolution, current -> previous
        Image biasCurrentColor;  // R16F, render resolution: lerp(history, current, bias). Null image = not tagged
        glm::uvec2 renderOffset; // the rendered sub-rect of colorIn / depth / motion
        glm::uvec2 renderSize;
        glm::uvec2 outputOffset; // the written sub-rect of colorOut
        glm::uvec2 outputSize;
        glm::vec2 jitterPx;      // this frame's sub-pixel jitter in render pixels
        bool reset;              // no history: first frame, a cut, a size change
        glm::mat4 viewToClip;    // unjittered
        glm::mat4 clipToPrevClip;
        glm::vec3 cameraPos;
        glm::vec3 cameraUp;
        glm::vec3 cameraRight;
        glm::vec3 cameraFwd;
        float cameraNear;
        float cameraFar;
        float cameraFovY;        // radians
        float cameraAspect;
    };
    // Tags the images, sets the constants and records the upscale into `cmd` (compute work, outside a render
    // pass). The caller orders the inputs before and the output after it. Returns false when nothing was
    // recorded (the caller then falls back to its own resolve).
    bool evaluateDlss(vk::CommandBuffer cmd, const DlssFrame& frame);
    // Frees the DLSS feature's own resources (mode Off, a lost device); the next evaluate re-creates them.
    void freeDlss();
}
