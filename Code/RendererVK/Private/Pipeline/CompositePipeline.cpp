module RendererVK;

import Core;
import File;

import :CommandBuffer;
import :Device;
import :Layout;
import :SceneColor; // SCENE_DEPTH_SAMPLED_LAYOUT (the motion blur gather's depth)

void CompositePipeline::buildPipelineLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/PostProcess/composite.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/PostProcess/composite.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    // Fullscreen HDR->display op: depth is meaningless here (it's the first draw of the swapchain pass and
    // relied on always-passing the old eLess-vs-cleared-1.0 test; under reversed-Z it would fail instead).
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ // bloom level 0
        .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    for (uint32 binding = 3; binding <= 5; ++binding) // motion blur: velocity, neighbour max, scene depth
        layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = binding, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ // the frame UBO (the gather's viewport + frame index)
        .binding = 6, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(CompositePC) });
}

void CompositePipeline::initialize(const RenderPass& renderPass)
{
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout);
    m_graphicsPipeline.initialize(renderPass, layout);
}

void CompositePipeline::reloadShaders(const RenderPass& renderPass)
{
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout);
    if (!m_graphicsPipeline.reloadShaders(renderPass, layout))
        printf("CompositePipeline: shader reload failed, keeping previous pipeline\n");
}

void CompositePipeline::record(CommandBuffer& commandBuffer, const RecordParams& params)
{
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    const auto sampledGeneral = [&](vk::ImageView view) { return vk::DescriptorImageInfo{ .sampler = params.mbSampler, .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; };
    oc::array<DescriptorSetUpdateInfo, 7> updates{
        DescriptorSetUpdateInfo{
            .binding = 0,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = {
                vk::DescriptorImageInfo{
                    .sampler = params.sampler,
                    .imageView = params.resolvedView,
                    .imageLayout = params.resolvedLayout
    } } },
        DescriptorSetUpdateInfo{
            .binding = 1,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.exposureBuffer, .range = vk::WholeSize } } },
        DescriptorSetUpdateInfo{
            .binding = 2,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.bloomSampler, .imageView = params.bloomView, .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.mbVelocityView) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.mbNeighborMaxView) } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.mbSampler, .imageView = params.mbDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo, .range = RendererVKLayout::UBO_RANGE } } } };

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_graphicsPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, descriptorSet, updates);
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipeline());
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);
    const CompositePC pc{ .bloomUv = params.bloomUv };
    vkCommandBuffer.pushConstants(m_graphicsPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
    vkCommandBuffer.draw(3, 1, 0, 0);
}
