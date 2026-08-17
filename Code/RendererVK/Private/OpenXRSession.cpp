module;

#include <vulkan/vulkan_core.h>
#include <cmath>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

module RendererVK;

import Core;
import Core.Camera;
import :VK;

namespace
{
    const char* xrResultStr(XrResult r)
    {
        switch (r)
        {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        default: return "XR_ERROR_<other>";
        }
    }

    // Returns true on success; logs and returns false otherwise.
    bool xrCheck(XrResult r, const char* what)
    {
        if (XR_SUCCEEDED(r))
            return true;
        printf("OpenXR: %s failed: %s (%d)\n", what, xrResultStr(r), (int)r);
        return false;
    }

    oc::vector<oc::string> tokenize(const oc::string& s)
    {
        oc::vector<oc::string> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t end = s.find(' ', start);
            if (end == oc::string::npos) end = s.size();
            if (end > start)
                out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }
}

bool OpenXRSession::initInstanceAndSystem()
{
    const char* enabledExtensions[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };

    auto copyFixed = [](char* dst, size_t cap, const char* src)
    {
        size_t i = 0;
        for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
        dst[i] = '\0';
    };

    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    copyFixed(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "App");
    copyFixed(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "OctEngine");
    createInfo.enabledExtensionCount = (uint32)(sizeof(enabledExtensions) / sizeof(enabledExtensions[0]));
    createInfo.enabledExtensionNames = enabledExtensions;

    XrResult r = xrCreateInstance(&createInfo, &m_instance);
    if (!XR_SUCCEEDED(r))
    {
        // No runtime installed / no HMD: this is an expected fallback, not a hard error.
        printf("OpenXR: xrCreateInstance failed (%s) - falling back to desktop-only.\n", xrResultStr(r));
        m_instance = XR_NULL_HANDLE;
        return false;
    }

    if (!loadKhrFunctions())
    {
        destroy();
        return false;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(m_instance, &systemInfo, &m_systemId);
    if (!XR_SUCCEEDED(r))
    {
        printf("OpenXR: xrGetSystem failed (%s) - is the headset connected?\n", xrResultStr(r));
        destroy();
        return false;
    }

    // The runtime requires the graphics-requirements query before the device is chosen / the session
    // is created (validation enforces it). The min/max Vulkan API version it reports is informational
    // here; the engine targets 1.3 which every desktop VR runtime supports.
    XrGraphicsRequirementsVulkanKHR gfxReq{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    if (!xrCheck(m_pfnGetVulkanGraphicsRequirements(m_instance, m_systemId, &gfxReq), "xrGetVulkanGraphicsRequirementsKHR"))
    {
        destroy();
        return false;
    }

    uint32 viewCount = 0;
    if (!xrCheck(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr), "xrEnumerateViewConfigurationViews(count)"))
    {
        destroy();
        return false;
    }
    if (viewCount != VIEW_COUNT)
    {
        printf("OpenXR: expected %u stereo views, runtime reports %u\n", VIEW_COUNT, viewCount);
        destroy();
        return false;
    }
    oc::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    if (!xrCheck(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()), "xrEnumerateViewConfigurationViews"))
    {
        destroy();
        return false;
    }
    m_viewConfig = views[0]; // both eyes share the recommended dimensions in practice

    XrSystemProperties sysProps{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &sysProps)))
        printf("OpenXR: system '%s', recommended eye target %ux%u\n", sysProps.systemName,
            m_viewConfig.recommendedImageRectWidth, m_viewConfig.recommendedImageRectHeight);

    m_enabled = true;
    return true;
}

bool OpenXRSession::loadKhrFunctions()
{
    auto load = [&](const char* name, PFN_xrVoidFunction* out) -> bool
    {
        XrResult r = xrGetInstanceProcAddr(m_instance, name, out);
        return xrCheck(r, name);
    };
    bool ok = true;
    ok &= load("xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&m_pfnGetVulkanInstanceExtensions);
    ok &= load("xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&m_pfnGetVulkanDeviceExtensions);
    ok &= load("xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&m_pfnGetVulkanGraphicsDevice);
    ok &= load("xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&m_pfnGetVulkanGraphicsRequirements);
    return ok;
}

oc::vector<oc::string> OpenXRSession::getRequiredVulkanInstanceExtensions() const
{
    uint32 size = 0;
    if (!xrCheck(m_pfnGetVulkanInstanceExtensions(m_instance, m_systemId, 0, &size, nullptr), "xrGetVulkanInstanceExtensionsKHR(size)"))
        return {};
    oc::string buffer(size, '\0');
    if (!xrCheck(m_pfnGetVulkanInstanceExtensions(m_instance, m_systemId, size, &size, buffer.data()), "xrGetVulkanInstanceExtensionsKHR"))
        return {};
    if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return tokenize(buffer);
}

oc::vector<oc::string> OpenXRSession::getRequiredVulkanDeviceExtensions() const
{
    uint32 size = 0;
    if (!xrCheck(m_pfnGetVulkanDeviceExtensions(m_instance, m_systemId, 0, &size, nullptr), "xrGetVulkanDeviceExtensionsKHR(size)"))
        return {};
    oc::string buffer(size, '\0');
    if (!xrCheck(m_pfnGetVulkanDeviceExtensions(m_instance, m_systemId, size, &size, buffer.data()), "xrGetVulkanDeviceExtensionsKHR"))
        return {};
    if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return tokenize(buffer);
}

vk::PhysicalDevice OpenXRSession::getVulkanGraphicsDevice(vk::Instance vkInstance) const
{
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!xrCheck(m_pfnGetVulkanGraphicsDevice(m_instance, m_systemId, (VkInstance)vkInstance, &physDevice), "xrGetVulkanGraphicsDeviceKHR"))
        return nullptr;
    return vk::PhysicalDevice(physDevice);
}

int64_t OpenXRSession::selectSwapchainFormat() const
{
    uint32 count = 0;
    if (!xrCheck(xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats(count)"))
        return 0;
    oc::vector<int64_t> formats(count);
    if (!xrCheck(xrEnumerateSwapchainFormats(m_session, count, &count, formats.data()), "xrEnumerateSwapchainFormats"))
        return 0;

    // Composite output is LDR post-tonemap, so prefer an 8-bit sRGB format.
    const int64_t preferred[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB };
    for (int64_t want : preferred)
        for (int64_t have : formats)
            if (have == want)
                return have;
    return formats.empty() ? 0 : formats[0];
}

bool OpenXRSession::createSession(vk::Instance vkInstance, vk::PhysicalDevice vkPhysicalDevice, vk::Device vkDevice,
    uint32 graphicsQueueFamily, uint32 graphicsQueueIndex)
{
    m_vkDevice = (VkDevice)vkDevice;
    m_graphicsQueueFamily = graphicsQueueFamily;
    vkGetDeviceQueue(m_vkDevice, graphicsQueueFamily, graphicsQueueIndex, &m_vkQueue);

    XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance = (VkInstance)vkInstance;
    binding.physicalDevice = (VkPhysicalDevice)vkPhysicalDevice;
    binding.device = m_vkDevice;
    binding.queueFamilyIndex = graphicsQueueFamily;
    binding.queueIndex = graphicsQueueIndex;

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;
    if (!xrCheck(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession"))
        return false;

    // App/world space: prefer STAGE (the room/standing universe) so runtime-level space adjustments — SteamVR
    // space drag, recenter — move the rendered view. STAGE origin sits on the floor at the play-area centre.
    // Fall back to LOCAL (head-seeded, world-locked) where STAGE isn't offered.
    XrReferenceSpaceType appSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    {
        uint32 spaceCount = 0;
        if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(m_session, 0, &spaceCount, nullptr)))
        {
            oc::vector<XrReferenceSpaceType> spaces(spaceCount);
            if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(m_session, spaceCount, &spaceCount, spaces.data())))
                for (XrReferenceSpaceType type : spaces)
                    if (type == XR_REFERENCE_SPACE_TYPE_STAGE) { appSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; break; }
        }
    }
    m_appSpaceIsStage = (appSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE);

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = appSpaceType;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f; // identity
    if (!xrCheck(xrCreateReferenceSpace(m_session, &spaceInfo, &m_space), "xrCreateReferenceSpace(app)"))
        return false;
    printf("OpenXR: app reference space = %s\n", m_appSpaceIsStage ? "STAGE" : "LOCAL");

    XrReferenceSpaceCreateInfo viewSpaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation.w = 1.0f; // identity
    if (!xrCheck(xrCreateReferenceSpace(m_session, &viewSpaceInfo, &m_viewSpace), "xrCreateReferenceSpace(VIEW)"))
        return false;

    m_swapchainFormat = selectSwapchainFormat();

    // One single-layer swapchain per eye: the SteamVR Vulkan runtime silently ignores arraySize>1.
    for (uint32 eye = 0; eye < VIEW_COUNT; ++eye)
    {
        XrSwapchainCreateInfo scInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        scInfo.format = m_swapchainFormat;
        scInfo.sampleCount = 1;
        scInfo.width = m_viewConfig.recommendedImageRectWidth;
        scInfo.height = m_viewConfig.recommendedImageRectHeight;
        scInfo.faceCount = 1;
        scInfo.arraySize = 1;
        scInfo.mipCount = 1;
        if (!xrCheck(xrCreateSwapchain(m_session, &scInfo, &m_swapchains[eye]), "xrCreateSwapchain"))
            return false;

        uint32 imageCount = 0;
        if (!xrCheck(xrEnumerateSwapchainImages(m_swapchains[eye], 0, &imageCount, nullptr), "xrEnumerateSwapchainImages(count)"))
            return false;
        m_swapchainImages[eye].assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
        if (!xrCheck(xrEnumerateSwapchainImages(m_swapchains[eye], imageCount, &imageCount,
            (XrSwapchainImageBaseHeader*)m_swapchainImages[eye].data()), "xrEnumerateSwapchainImages"))
            return false;
    }

    // Own command pool + a single reusable clear buffer (decoupled from the engine's pool). M1
    // serializes with vkQueueWaitIdle after each submit, so one buffer is enough.
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    if (vkCreateCommandPool(m_vkDevice, &poolInfo, nullptr, &m_vkCommandPool) != VK_SUCCESS)
    {
        printf("OpenXR: failed to create command pool\n");
        return false;
    }
    VkCommandBufferAllocateInfo cbInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbInfo.commandPool = m_vkCommandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_vkDevice, &cbInfo, &m_clearCommandBuffer) != VK_SUCCESS)
    {
        printf("OpenXR: failed to allocate command buffers\n");
        return false;
    }

    printf("OpenXR: session created, 2x%ux%u eye swapchains\n",
        m_viewConfig.recommendedImageRectWidth, m_viewConfig.recommendedImageRectHeight);
    return true;
}

void OpenXRSession::pollEvents()
{
    if (!m_enabled)
        return;

    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (true)
    {
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult r = xrPollEvent(m_instance, &event);
        if (r == XR_EVENT_UNAVAILABLE)
            break;
        if (!XR_SUCCEEDED(r))
            break;

        switch (event.type)
        {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            m_exitRequested = true;
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            auto& stateEvent = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            m_sessionState = stateEvent.state;
            switch (m_sessionState)
            {
            case XR_SESSION_STATE_READY:
            {
                XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xrCheck(xrBeginSession(m_session, &beginInfo), "xrBeginSession"))
                    m_sessionRunning = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrCheck(xrEndSession(m_session), "xrEndSession");
                m_sessionRunning = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                m_exitRequested = true;
                m_sessionRunning = false;
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}

bool OpenXRSession::beginFrame()
{
    m_frameActive = false;
    m_shouldRender = false;
    m_headPoseValid = false;
    if (!m_sessionRunning)
        return false;

    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    if (!xrCheck(xrWaitFrame(m_session, &waitInfo, &frameState), "xrWaitFrame"))
        return false;

    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!xrCheck(xrBeginFrame(m_session, &beginInfo), "xrBeginFrame"))
        return false;

    m_predictedDisplayTime = frameState.predictedDisplayTime;
    m_shouldRender = (frameState.shouldRender == XR_TRUE);
    m_frameActive = true;

    // Per-eye views (pose + fov) for the composition layer.
    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = m_predictedDisplayTime;
    locateInfo.space = m_space;
    XrViewState viewState{ XR_TYPE_VIEW_STATE };
    for (uint32 i = 0; i < VIEW_COUNT; ++i) m_views[i] = { XR_TYPE_VIEW };
    uint32 located = 0;
    xrCheck(xrLocateViews(m_session, &locateInfo, &viewState, VIEW_COUNT, &located, m_views), "xrLocateViews");

    // Head pose (VIEW space relative to LOCAL) for the mono camera.
    XrSpaceLocation headLoc{ XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_space, m_predictedDisplayTime, &headLoc))
        && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
        && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    {
        m_headPose = headLoc.pose;
        m_headPoseValid = true;
    }

    return true;
}

void OpenXRSession::getHeadView(const glm::vec3& playSpacePos, const glm::quat& baseOrientation, glm::mat4& outViewMatrix, glm::vec3& outPosition) const
{
    glm::quat orientation(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 localPos(0.0f);
    if (m_headPoseValid)
    {
        orientation = glm::quat(m_headPose.orientation.w, m_headPose.orientation.x, m_headPose.orientation.y, m_headPose.orientation.z);
        localPos = glm::vec3(m_headPose.position.x, m_headPose.position.y, m_headPose.position.z);
    }
    // Rigid play-space transform (locomotion + base orientation: camera rotation * play yaw); the headset's
    // tracked pose rides on top. world = T(pos) * R(base) * headLocal. Keeping headLocal inside R(base) is
    // deliberate: head tracking AND the STAGE/space-drag offset must rotate with the yawed world. Turning in
    // place is handled in the controller (it pivots about the head by adjusting the locomotion origin).
    const glm::mat4 playSpace = glm::translate(glm::mat4(1.0f), playSpacePos) * glm::mat4_cast(baseOrientation);
    const glm::mat4 headWorld = playSpace * glm::translate(glm::mat4(1.0f), localPos) * glm::mat4_cast(orientation);
    outPosition = glm::vec3(headWorld[3]);
    outViewMatrix = glm::inverse(headWorld);
}

void OpenXRSession::getEyeView(uint32 eye, const glm::vec3& playSpacePos, const glm::quat& baseOrientation, glm::mat4& outViewMatrix, glm::vec3& outPosition) const
{
    const XrPosef& pose = m_views[eye].pose;
    const glm::quat orientation(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    const glm::vec3 localPos(pose.position.x, pose.position.y, pose.position.z);
    // Same rigid play-space transform as getHeadView; the eye pose adds rotation + the per-eye offset (IPD).
    const glm::mat4 playSpace = glm::translate(glm::mat4(1.0f), playSpacePos) * glm::mat4_cast(baseOrientation);
    const glm::mat4 eyeWorld = playSpace * glm::translate(glm::mat4(1.0f), localPos) * glm::mat4_cast(orientation);
    outPosition = glm::vec3(eyeWorld[3]);
    outViewMatrix = glm::inverse(eyeWorld);
}

glm::mat4 OpenXRSession::getEyeProjection(uint32 eye, float nearZ, float farZ) const
{
    // OpenXR gives asymmetric half-angles (angleLeft/Down are negative). Build the off-centre frustum
    // the same way glm::perspective is built in the mono path so the depth/handedness convention matches.
    const XrFovf& fov = m_views[eye].fov;
    const float l = std::tan(fov.angleLeft) * nearZ;
    const float r = std::tan(fov.angleRight) * nearZ;
    const float b = std::tan(fov.angleDown) * nearZ;
    const float t = std::tan(fov.angleUp) * nearZ;
    return reverseZProjection(glm::frustum(l, r, b, t, nearZ, farZ), nearZ, farZ);
}

glm::mat4 OpenXRSession::getCombinedProjection(float nearZ, float farZ) const
{
    // Widest extent each side across both eyes (angleLeft/Down are negative), with a small margin so the
    // head-centred frustum also covers the eyes' horizontal offset (IPD) for near geometry. This guarantees
    // any point either eye can see reprojects inside the centre view's [0,1] (no fog-volume edge clamp).
    XrFovf u = m_views[0].fov;
    for (uint32 i = 1; i < VIEW_COUNT; ++i)
    {
        u.angleLeft  = oc::min(u.angleLeft,  m_views[i].fov.angleLeft);
        u.angleRight = oc::max(u.angleRight, m_views[i].fov.angleRight);
        u.angleDown  = oc::min(u.angleDown,  m_views[i].fov.angleDown);
        u.angleUp    = oc::max(u.angleUp,    m_views[i].fov.angleUp);
    }
    constexpr float margin = 1.1f;
    const float l = std::tan(u.angleLeft)  * nearZ * margin;
    const float r = std::tan(u.angleRight) * nearZ * margin;
    const float b = std::tan(u.angleDown)  * nearZ * margin;
    const float t = std::tan(u.angleUp)    * nearZ * margin;
    return reverseZProjection(glm::frustum(l, r, b, t, nearZ, farZ), nearZ, farZ);
}

void OpenXRSession::endFrame(vk::Image leftSource, vk::Image rightSource, vk::Extent2D sourceExtent, vk::ImageLayout sourceLayout)
{
    if (!m_frameActive)
        return;
    m_frameActive = false;

    XrCompositionLayerProjectionView projViews[VIEW_COUNT]{};
    XrCompositionLayerProjection projLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    uint32 layerCount = 0;

    if (m_shouldRender && leftSource && rightSource)
    {
        uint32 imageIndex[VIEW_COUNT] = {};
        bool acquired[VIEW_COUNT] = { false, false };
        bool ok = true;
        for (uint32 eye = 0; eye < VIEW_COUNT && ok; ++eye)
        {
            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (!xrCheck(xrAcquireSwapchainImage(m_swapchains[eye], &acquireInfo, &imageIndex[eye]), "xrAcquireSwapchainImage"))
            {
                ok = false;
                break;
            }
            acquired[eye] = true;
            XrSwapchainImageWaitInfo scWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            scWaitInfo.timeout = XR_INFINITE_DURATION;
            if (!xrCheck(xrWaitSwapchainImage(m_swapchains[eye], &scWaitInfo), "xrWaitSwapchainImage"))
                ok = false;
        }

        if (ok)
        {
            const VkImage eyeSrc[VIEW_COUNT] = { (VkImage)leftSource, (VkImage)rightSource };
            VkCommandBuffer cb = m_clearCommandBuffer;
            VkCommandBufferBeginInfo cbBegin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &cbBegin);

            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;

            for (uint32 eye = 0; eye < VIEW_COUNT; ++eye)
            {
                const VkImage src = eyeSrc[eye];
                const VkImage dst = m_swapchainImages[eye][imageIndex[eye]].image;

                // This eye's composited LDR image -> TRANSFER_SRC (it is re-cleared next frame, so no restore).
                VkImageMemoryBarrier srcToRead{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                srcToRead.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                srcToRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                srcToRead.oldLayout = (VkImageLayout)sourceLayout;
                srcToRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                srcToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                srcToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                srcToRead.image = src;
                srcToRead.subresourceRange = range;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &srcToRead);

                VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toDst.srcAccessMask = 0;
                toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.image = dst;
                toDst.subresourceRange = range;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &toDst);

                // Blit (scales the render-resolution eye image to the runtime's eye target).
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = { (int32_t)sourceExtent.width, (int32_t)sourceExtent.height, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = { (int32_t)m_viewConfig.recommendedImageRectWidth, (int32_t)m_viewConfig.recommendedImageRectHeight, 1 };
                vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toColor.image = dst;
                toColor.subresourceRange = range;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &toColor);
            }

            vkEndCommandBuffer(cb);
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            vkQueueSubmit(m_vkQueue, 1, &submit, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_vkQueue); // M2: simple serialization; replaced with proper sync later

            for (uint32 eye = 0; eye < VIEW_COUNT; ++eye)
            {
                projViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                projViews[eye].pose = m_views[eye].pose;
                projViews[eye].fov = m_views[eye].fov;
                projViews[eye].subImage.swapchain = m_swapchains[eye];
                projViews[eye].subImage.imageArrayIndex = 0;
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = {
                    (int32_t)m_viewConfig.recommendedImageRectWidth,
                    (int32_t)m_viewConfig.recommendedImageRectHeight };
            }
            projLayer.space = m_space;
            projLayer.viewCount = VIEW_COUNT;
            projLayer.views = projViews;
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer);
            layerCount = 1;
        }

        // Release whatever we acquired (required before xrEndFrame references the swapchains).
        for (uint32 eye = 0; eye < VIEW_COUNT; ++eye)
            if (acquired[eye])
            {
                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrCheck(xrReleaseSwapchainImage(m_swapchains[eye], &releaseInfo), "xrReleaseSwapchainImage");
            }
    }

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = m_predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    xrCheck(xrEndFrame(m_session, &endInfo), "xrEndFrame");

    m_frameCounter++;
}

void OpenXRSession::destroy()
{
    if (m_vkDevice != VK_NULL_HANDLE && m_vkCommandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_vkDevice, m_vkCommandPool, nullptr);
        m_vkCommandPool = VK_NULL_HANDLE;
    }
    m_clearCommandBuffer = VK_NULL_HANDLE;
    for (uint32 eye = 0; eye < VIEW_COUNT; ++eye)
    {
        if (m_swapchains[eye] != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(m_swapchains[eye]);
            m_swapchains[eye] = XR_NULL_HANDLE;
        }
        m_swapchainImages[eye].clear();
    }
    if (m_viewSpace != XR_NULL_HANDLE)
    {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_space != XR_NULL_HANDLE)
    {
        xrDestroySpace(m_space);
        m_space = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE)
    {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE)
    {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }
    m_enabled = false;
    m_sessionRunning = false;
}
