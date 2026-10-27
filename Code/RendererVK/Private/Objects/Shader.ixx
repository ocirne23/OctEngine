export module RendererVK:Shader;

import Core;
import :VK;

export struct ShaderDefine
{
    oc::string name;
    oc::string value;
};

// What one compiled stage was built from: the SPIR-V cache key + the root source (keyHash), and the frame UBO entries
// its text names with their declaration state (uboRefs, uboHash). A pipeline keeps its stamps, so a reload after a
// lock change keeps every pipeline whose stages are all still current (Shader::isCurrent).
export struct ShaderStamp
{
    uint64 keyHash = 0;
    uint64 uboHash = 0;
    oc::vector<oc::string> uboRefs;
};

export class Shader final
{
public:
    Shader();
    ~Shader();
    Shader(const Shader&) = delete;

    bool initializeFromFile(vk::ShaderStageFlagBits stage, const oc::string& filePath, const oc::vector<ShaderDefine>& defines = {}, bool assertOnFailure = true);
    // pushDeclaration: the text the includer serves as "push.generated.glsl" (a pipeline's PushBlock; empty = none).
    // stamp (optional) receives what the compile was built from.
    bool initialize(vk::ShaderStageFlagBits stage, const oc::string& shaderStr, const oc::string& debugFilePath, const oc::vector<ShaderDefine>& defines = {},
        bool assertOnFailure = true, const oc::string& pushDeclaration = {}, ShaderStamp* stamp = nullptr);

    static bool GLSLtoSPV(const vk::ShaderStageFlagBits type, const oc::string& source, oc::vector<unsigned int>& spirv, const oc::string& debugFilePath,
        const oc::vector<ShaderDefine>& defines = {}, const oc::string& pushDeclaration = {}, ShaderStamp* stamp = nullptr);
    // True when a compile of these inputs gives the stamped result again, the INCLUDE FILES NOT CHECKED: only for a
    // reload that changes nothing on disk (s_uboOnlyReload).
    static bool isCurrent(const ShaderStamp& stamp, vk::ShaderStageFlagBits type, const oc::string& source, const oc::string& debugFilePath,
        const oc::vector<ShaderDefine>& defines, const oc::string& pushDeclaration);
    // FNV-1a 64 over bytes, chained from `hash` (the pipelines' fixed-state hashes).
    static uint64 hashBytes(uint64 hash, const void* data, size_t size);
    template<typename T> static uint64 hashValue(uint64 hash, const T& value) { return hashBytes(hash, &value, sizeof(T)); }

    // Set by Renderer::applyUboLocks around its reloadShaders: only the UBO declaration (and the push blocks) changed,
    // so a pipeline whose stamps are all current keeps its vk::Pipeline. The counters report the reload.
    static inline bool s_uboOnlyReload = false;
    static inline uint32 s_pipelinesKept = 0;
    static inline uint32 s_pipelinesRebuilt = 0;
    // The debug-utils name of a pipeline built from this file: the file name only ("taa.cs.glsl") - no
    // directory, no defines, so the name stays short.
    static oc::string debugName(const oc::string& debugFilePath);
    // "A B=1 C": the defines that make a variant (the pipeline stats' variant column). Appends to out, skipping
    // names already in it.
    static void appendDefineList(oc::string& out, const oc::vector<ShaderDefine>& defines);

    vk::ShaderModule getModule() const { return m_shaderModule; }

private:

    vk::ShaderModule m_shaderModule;
};