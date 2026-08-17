export module RendererVK:ComputePipeline;

import Core;
import :VK;
import :Shader;

export struct ComputePipelineLayout
{
    oc::string computeShaderText;
    oc::string computeShaderDebugFilePath;
    oc::vector<ShaderDefine> defines;
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

    bool createPipeline(const ComputePipelineLayout& layout, vk::Pipeline& outPipeline, bool assertOnFailure);

    vk::Pipeline m_pipeline;
    vk::PipelineCache m_pipelineCache;
    vk::PipelineLayout m_pipelineLayout;
    vk::DescriptorSetLayout m_descriptorSetLayout;
};