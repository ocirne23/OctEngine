module RendererVK;

import Core;
import Core.SDL;
import Core.Window;

import :VK;
import :Device;
import :OpenXRSession;

Instance::Instance() {}
Instance::~Instance()
{
    Globals::device.destroy();
    destroy();
}

static vk::Bool32 debugReportCallback(vk::DebugReportFlagsEXT flags,
    vk::DebugReportObjectTypeEXT objectType,
    uint64_t object,
    size_t location,
    int32_t messageCode,
    const char* pLayerPrefix,
    const char* pMessage,
    void* pUserData)
{
	const char* severity = "UNKNOWN";
    if (flags & vk::DebugReportFlagBitsEXT::eWarning)
        severity = "WARNING";
	else if (flags & vk::DebugReportFlagBitsEXT::eInformation)
		severity = "INFO";
	else if (flags & vk::DebugReportFlagBitsEXT::eError)
		severity = "ERROR";
	else if (flags & vk::DebugReportFlagBitsEXT::eDebug)
		severity = "DEBUG";
	else if (flags & vk::DebugReportFlagBitsEXT::ePerformanceWarning)
		severity = "PERFORMANCE WARNING";

    printf("VK%s: %s", severity, pMessage);

    return false;
}

static vk::Bool32 debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT       messageSeverity,
    vk::DebugUtilsMessageTypeFlagsEXT              messageTypes,
    const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    if (pCallbackData->messageIdNumber == 0x76589099)
    {
        printf(pCallbackData->pMessage + 94);
        return vk::False;
    }
    if (pCallbackData->messageIdNumber == 0x92394c89)
        return vk::False;
    if (pCallbackData->messageIdNumber == 0xa5625282)
        return vk::False;
    if (pCallbackData->messageIdNumber == 0xd90fc835) //vkCmdExecuteGeneratedCommandsEXT() : command can't be executed on a secondary command buffer.
        return vk::False;
    // The SteamVR OpenXR runtime creates its swapchain images (inside xrCreateSwapchain) with external
    // D3D11 memory and a PREINITIALIZED initialLayout, which the spec forbids for external-memory images.
    // It's the runtime's vkCreateImage, not ours, so suppress this known false positive.
    if (pCallbackData->pMessageIdName && strstr(pCallbackData->pMessageIdName, "VUID-VkImageCreateInfo-pNext-01443"))
        return vk::False;
    const char* severity = "UNKNOWN";
    
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
        severity = "ERROR";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
        severity = "WARNING";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
        severity = "INFO";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose)
        severity = "VERBOSE";
    printf("VKV%s: %s\n", severity, pCallbackData->pMessage);
    bool* pBreakOnValidationLayerError = reinterpret_cast<bool*>(pUserData);
    if (*pBreakOnValidationLayerError && (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError))
    {
        __debugbreak();
    }
    return vk::False;
}

bool Instance::initialize(Window& window, bool enableValidationLayers)
{
    auto enumerateExtensionsResult = vk::enumerateInstanceExtensionProperties();
    if (enumerateExtensionsResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to enumerate instance extension properties");
        return false;
    }
    m_supportedExtensions = enumerateExtensionsResult.value;
    auto enumerateLayersResult = vk::enumerateInstanceLayerProperties();
    if (enumerateLayersResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to enumerate instance layer properties");
        return false;
    }
    m_supportedLayers = enumerateLayersResult.value;
    m_apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

    uint32 numExtensions = 0;
    const char* const* ppExtensions = SDL_Vulkan_GetInstanceExtensions(&numExtensions);
    oc::vector<const char*> extensions;

    for (uint32 i = 0; i < numExtensions; i++)
        if (ppExtensions[i] && supportsExtension(ppExtensions[i]))
            extensions.push_back(ppExtensions[i]);

    // Instance extensions the OpenXR runtime requires (none when VR is disabled). Kept alive for the
    // duration of this call; skip any SDL already added.
    oc::vector<oc::string> xrExtensions;
    if (Globals::openXR.isEnabled())
        xrExtensions = Globals::openXR.getRequiredVulkanInstanceExtensions();
    for (const oc::string& ext : xrExtensions)
    {
        bool alreadyAdded = false;
        for (const char* have : extensions)
            if (ext == have) { alreadyAdded = true; break; }
        if (!alreadyAdded && supportsExtension(ext.c_str()))
            extensions.push_back(ext.c_str());
    }

    vk::ApplicationInfo appInfo{ .pApplicationName = "App", .applicationVersion = VK_MAKE_VERSION(1, 0, 0), .pEngineName = "OctEngine", .engineVersion = VK_MAKE_VERSION(1, 0, 0), .apiVersion = m_apiVersion };
    vk::InstanceCreateInfo createInfo{ .pApplicationInfo = &appInfo };

    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = debugCallback,
        .pUserData = &m_breakOnValidationLayerError,
    };
	vk::DebugReportCallbackCreateInfoEXT debugReportCreateInfo{
		.flags = vk::DebugReportFlagBitsEXT::eError | vk::DebugReportFlagBitsEXT::eWarning | vk::DebugReportFlagBitsEXT::eInformation | vk::DebugReportFlagBitsEXT::ePerformanceWarning | vk::DebugReportFlagBitsEXT::eDebug,
		.pfnCallback = debugReportCallback,
	};

    oc::vector<vk::ValidationFeatureEnableEXT> validationFeaturesList =
    {
    //    vk::ValidationFeatureEnableEXT::eDebugPrintf
    };
    vk::ValidationFeaturesEXT validationFeatures{
        .pNext = &debugCreateInfo,
        .enabledValidationFeatureCount = (uint32)validationFeaturesList.size(),
        .pEnabledValidationFeatures = validationFeaturesList.data(),
	};

    if (enableValidationLayers)
    {
        if (supportsLayer(VK_VALIDATION_LAYER_NAME))
            m_enabledLayers.push_back(VK_VALIDATION_LAYER_NAME);
        if (supportsExtension(vk::EXTDebugUtilsExtensionName))
            extensions.push_back(vk::EXTDebugUtilsExtensionName);
        //if (supportsExtension(vk::EXTDebugReportExtensionName))
        //    extensions.push_back(vk::EXTDebugReportExtensionName);
        createInfo.pNext = &validationFeatures;
    }
    createInfo.enabledLayerCount = (uint32)m_enabledLayers.size();
    createInfo.ppEnabledLayerNames = m_enabledLayers.data();

    createInfo.enabledExtensionCount = static_cast<uint32>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    auto createInstanceResult = vk::createInstance(createInfo);
    if (createInstanceResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create instance");
        return false;
    }
    m_instance = createInstanceResult.value;

    if (enableValidationLayers)
    {
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo2 = debugCreateInfo;
        VkDebugUtilsMessengerEXT debugMessenger;
        auto createMessengerFunc = (PFN_vkCreateDebugUtilsMessengerEXT)m_instance.getProcAddr("vkCreateDebugUtilsMessengerEXT");
        createMessengerFunc(m_instance, &debugCreateInfo2, nullptr, &debugMessenger);
        m_debugMessenger = debugMessenger;

		VkDebugReportCallbackCreateInfoEXT debugReportCreateInfo2 = debugReportCreateInfo;
        VkDebugReportCallbackEXT debugReportCallback;
		auto createDebugReportFunc = (PFN_vkCreateDebugReportCallbackEXT)m_instance.getProcAddr("vkCreateDebugReportCallbackEXT");
        if (createDebugReportFunc)
        {
            createDebugReportFunc(m_instance, &debugReportCreateInfo2, nullptr, &debugReportCallback);
            m_debugReportCallback = debugReportCallback;
        }
    }

    return true;
}

void Instance::destroy()
{
    if (m_instance)
    {
        if (m_debugMessenger)
        {
            auto destroyMessengerFunc = (PFN_vkDestroyDebugUtilsMessengerEXT)m_instance.getProcAddr("vkDestroyDebugUtilsMessengerEXT");
            destroyMessengerFunc(m_instance, m_debugMessenger, nullptr);
        }
		if (m_debugReportCallback)
		{
			auto destroyDebugReportFunc = (PFN_vkDestroyDebugReportCallbackEXT)m_instance.getProcAddr("vkDestroyDebugReportCallbackEXT");
			destroyDebugReportFunc(m_instance, m_debugReportCallback, nullptr);
		}
        m_instance.destroy();
    }
    m_instance = nullptr;
}

bool Instance::supportsExtension(const char* pExtensionName) const
{
    for (auto extension : m_supportedExtensions)
        if (strcmp(pExtensionName, extension.extensionName) == 0)
            return true;
    return false;
}

bool Instance::supportsLayer(const char* pLayerName) const
{
    for (auto layer : m_supportedLayers)
        if (strcmp(pLayerName, layer.layerName) == 0)
            return true;
    return false;
}