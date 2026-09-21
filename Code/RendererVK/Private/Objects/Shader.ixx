export module RendererVK:Shader;

import Core;
import :VK;

export struct ShaderDefine
{
    oc::string name;
    oc::string value;
};

export class Shader final
{
public:
    Shader();
    ~Shader();
    Shader(const Shader&) = delete;

    bool initializeFromFile(vk::ShaderStageFlagBits stage, const oc::string& filePath, const oc::vector<ShaderDefine>& defines = {}, bool assertOnFailure = true);
    bool initialize(vk::ShaderStageFlagBits stage, const oc::string& shaderStr, const oc::string& debugFilePath, const oc::vector<ShaderDefine>& defines = {}, bool assertOnFailure = true);

    static bool GLSLtoSPV(const vk::ShaderStageFlagBits type, const oc::string& source, oc::vector<unsigned int>& spirv, const oc::string& debugFilePath, const oc::vector<ShaderDefine>& defines = {});
    // The debug-utils name of a pipeline built from this file: the file name only ("taa.cs.glsl") - no
    // directory, no defines, so the name stays short.
    static oc::string debugName(const oc::string& debugFilePath);

    vk::ShaderModule getModule() const { return m_shaderModule; }

private:

    vk::ShaderModule m_shaderModule;
};