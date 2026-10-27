export module RendererVK:ComputePipeline;

import Core;
import :VK;
import :Shader;

export struct ComputePipelineLayout
{
    oc::string computeShaderText;
    oc::string computeShaderDebugFilePath;
    oc::vector<ShaderDefine> defines;
    oc::string pushDeclaration; // served as "push.generated.glsl" (PushBlock::declaration)
    oc::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    oc::vector<vk::DescriptorBindingFlags> descriptorBindingFlags; // parallel to descriptorSetLayoutBindings (optional)
    oc::vector<vk::PushConstantRange> pushConstantRanges;
};

export class ComputePipeline final
{
public:
    ComputePipeline() {};
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;

    bool initialize(const ComputePipelineLayout& layout);
    bool reloadShaders(const ComputePipelineLayout& layout);

    vk::Pipeline getPipeline() const { return m_pipeline; }
    vk::PipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }

private:

    bool createPipeline(const ComputePipelineLayout& layout, vk::Pipeline& outPipeline, ShaderStamp& outStamp, bool assertOnFailure);
    bool isCurrent(const ComputePipelineLayout& layout) const;

    vk::Pipeline m_pipeline;
    ShaderStamp m_stamp;
    bool m_capturedStatistics = false;
    vk::PipelineCache m_pipelineCache;
    vk::PipelineLayout m_pipelineLayout;
    vk::DescriptorSetLayout m_descriptorSetLayout;
};