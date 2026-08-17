export module RendererVK:Instance;

import Core;
import Core.fwd;
import :VK;

export class Instance final
{
public:
    Instance();
    ~Instance();
    Instance(const Instance&) = delete;

    bool initialize(Window& window, bool enableValidationLayers);
    void destroy();

    void setBreakOnValidationLayerError(bool value) { m_breakOnValidationLayerError = value; }
    vk::Instance getInstance() const { return m_instance; }
    const oc::vector<const char*>& getEnabledLayers() const { return m_enabledLayers; }
    bool supportsLayer(const char* pLayerName) const;
    bool supportsExtension(const char* pExtensionName) const;
    uint32 getApiVersion() const { return m_apiVersion; }

private:

    vk::Instance m_instance;
    uint32 m_apiVersion = 0;
    bool m_breakOnValidationLayerError = false;
    oc::vector<const char*> m_enabledLayers;

    oc::vector<vk::ExtensionProperties> m_supportedExtensions;
    oc::vector<vk::LayerProperties> m_supportedLayers;
    vk::DebugUtilsMessengerEXT m_debugMessenger;
	vk::DebugReportCallbackEXT m_debugReportCallback;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_INSTANCE)
    Instance instance;
}