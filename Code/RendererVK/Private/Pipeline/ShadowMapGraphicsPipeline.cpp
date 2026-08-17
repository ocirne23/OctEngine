module RendererVK;

import Core;
import File;

import :CommandBuffer;
import :Device;
import :Layout;
import :Texture;
import :TextureManager;

ShadowMapGraphicsPipeline::ShadowMapGraphicsPipeline() {}
ShadowMapGraphicsPipeline::~ShadowMapGraphicsPipeline() {}

void ShadowMapGraphicsPipeline::buildPipelineLayout(GraphicsPipelineLayout& layout, uint32 maxTextures)
{
    layout.vertexShader.debugFilePath = "Shaders/shadow_depth.vs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    // Fragment stage discards alpha-masked (cutout) fragments so foliage casts correct shadows.
    layout.fragmentShader.debugFilePath = "Shaders/shadow_depth.fs.glsl";
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);

    layout.depthOnly = true;
    layout.depthCompareOp = vk::CompareOp::eLess; // shadow ortho projections stay standard-Z (not reversed)
    layout.depthBiasEnable = true;
    layout.depthBiasConstantFactor = 1.25f; // slope-scaled bias fights shadow acne on lit slopes
    layout.depthBiasSlopeFactor = 2.5f;
    layout.cullMode = vk::CullModeFlagBits::eFront;

    auto& bindings = layout.vertexLayoutInfo.bindingDescriptions;
    bindings.push_back(vk::VertexInputBindingDescription{ .binding = 0, .stride = sizeof(RendererVKLayout::MeshVertex), .inputRate = vk::VertexInputRate::eVertex });
    bindings.push_back(vk::VertexInputBindingDescription{ .binding = 2, .stride = sizeof(uint32), .inputRate = vk::VertexInputRate::eInstance });

    auto& attributes = layout.vertexLayoutInfo.attributeDescriptions;
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(RendererVKLayout::MeshVertex, position) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 3, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(RendererVKLayout::MeshVertex, texCoord) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 4, .binding = 2, .format = vk::Format::eR32Uint, .offset = 0 });

    auto& descriptorSetBindings = layout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // main UBO (cascade view-projs)
        .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // shared transformed instances
        .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // diffuse texture array (alpha-mask discard)
        .binding = 7, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    // The texture array (the set's highest binding) is variable-count: the layout declares the fixed
    // device-limit cap, the live size comes from the descriptor set allocation. UPDATE_AFTER_BIND so the
    // TextureStreamer can rewrite swapped slots without re-recording the cached draw CB.
    layout.descriptorBindingFlags.resize(descriptorSetBindings.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    // The cascade is selected per multiview view via gl_ViewIndex, so no push constant is needed.
}

void ShadowMapGraphicsPipeline::updateTextureDescriptor(vk::DescriptorSet descriptorSet, uint32 slotIdx, vk::ImageView view)
{
    // Streamed texture slot rewrite in the cached shadow-draw CB's set (binding 7 is UPDATE_AFTER_BIND).
    vk::DescriptorImageInfo imageInfo{ .sampler = m_sampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 7, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void ShadowMapGraphicsPipeline::buildIndirectState(uint32 maxUniqueMeshes)
{
    // The shadow pass renders all cascades in a single multiview render pass (non-zero viewMask), where DGC
    // forbids an Indirect Execution Set. We only ever use the one depth-only pipeline anyway, so the commands
    // layout omits the EXECUTION_SET token and we bind the pipeline explicitly + pass a null execution set.
    m_indirectCommandsLayout.initialize(m_graphicsPipeline.getPipelineLayout(),
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, /*useExecutionSet*/ false);

    createPreprocessBuffers(maxUniqueMeshes);
}

void ShadowMapGraphicsPipeline::resizeMeshCapacity(uint32 maxUniqueMeshes)
{
    createPreprocessBuffers(maxUniqueMeshes);
}

void ShadowMapGraphicsPipeline::createPreprocessBuffers(uint32 maxUniqueMeshes)
{
    // With no execution set, DGC needs the pipeline it will generate draws for supplied via pNext.
    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{ .pipeline = m_graphicsPipeline.getPipeline() };
    vk::GeneratedCommandsMemoryRequirementsInfoEXT memReqInfo{
        .pNext = &pipelineInfo,
        .indirectExecutionSet = VK_NULL_HANDLE,
        .indirectCommandsLayout = m_indirectCommandsLayout.getHandle(),
        .maxSequenceCount = maxUniqueMeshes,
        .maxDrawCount = maxUniqueMeshes,
    };
    vk::MemoryRequirements2 memReq;
    Globals::device.getDevice().getGeneratedCommandsMemoryRequirementsEXT(&memReqInfo, &memReq);
    m_preprocessSize = memReq.memoryRequirements.size;
    if (m_preprocessSize > 0)
    {
        for (Buffer& preprocess : m_preprocessBuffers)
            preprocess.initialize(m_preprocessSize,
                vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal);
    }
}

void ShadowMapGraphicsPipeline::initialize(ShadowMap& shadowMap, uint32 maxUniqueMeshes, uint32 maxTextures)
{
    m_renderPass = shadowMap.getRenderPass();
    m_sampler.initialize();
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout, maxTextures);
    m_graphicsPipeline.initialize(m_renderPass, layout);
    buildIndirectState(maxUniqueMeshes);
}

void ShadowMapGraphicsPipeline::reloadShaders(uint32 maxTextures)
{
    if (!m_renderPass)
        return;
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout, maxTextures);
    if (!m_graphicsPipeline.reloadShaders(m_renderPass, layout))
    {
        printf("ShadowMapGraphicsPipeline: shader reload failed, keeping previous pipeline\n");
        return;
    }
    // No execution set to rebuild: the multiview shadow pass binds its single pipeline directly.
}

void ShadowMapGraphicsPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params)
{
    oc::array<DescriptorSetUpdateInfo, 3> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = params.ubo.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.meshInstanceBuffer.getBuffer(), .range = params.meshInstanceBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler },
    };
    for (uint16 texIdx = 0; texIdx < (uint16)Globals::textureManager.getNumTextures(); ++texIdx)
    {
        updates[2].imageInfos.push_back(vk::DescriptorImageInfo{
            .sampler = m_sampler.getSampler(),
            .imageView = Globals::textureManager.getViewForDescriptor(texIdx), // freed slots -> fallback
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        });
    }

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    commandBuffer.cmdUpdateDescriptorSets(m_graphicsPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, descriptorSet, updates);
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipeline());
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);
    vkCommandBuffer.bindVertexBuffers(0, { params.vertexBuffer.getBuffer() }, { 0 });
    vkCommandBuffer.bindVertexBuffers(2, { params.instanceIdxBuffer.getBuffer() }, { 0 });
    vkCommandBuffer.bindIndexBuffer(params.indexBuffer.getBuffer(), 0, vk::IndexType::eUint32);

    // One multiview execute renders all cascades (gl_ViewIndex selects the layer/matrix).
    // With no execution set, the target pipeline must be supplied via pNext (matches the memory-requirements query).
    // Live sequence count read from the CPU-written mesh-count buffer (see StaticMeshGraphicsPipeline).
    const uint32 maxSequences = (uint32)(params.indirectCommandBuffer.getSize() / sizeof(RendererVKLayout::IndirectDrawSequence));
    vk::GeneratedCommandsPipelineInfoEXT pipelineInfo{ .pipeline = m_graphicsPipeline.getPipeline() };
    vk::GeneratedCommandsInfoEXT generatedCommandsInfo{
        .pNext = &pipelineInfo,
        .shaderStages = vk::ShaderStageFlagBits::eVertex,
        .indirectExecutionSet = VK_NULL_HANDLE, // single pipeline, bound explicitly above (multiview disallows an execution set)
        .indirectCommandsLayout = m_indirectCommandsLayout.getHandle(),
        .indirectAddress = params.indirectCommandBuffer.getDeviceAddress(),
        .indirectAddressSize = params.indirectCommandBuffer.getSize(),
        .preprocessAddress = m_preprocessSize > 0 ? m_preprocessBuffers[frameIdx].getDeviceAddress() : 0,
        .preprocessSize = m_preprocessSize,
        .maxSequenceCount = maxSequences,
        .sequenceCountAddress = params.meshCountBuffer.getDeviceAddress(),
        .maxDrawCount = maxSequences,
    };
    vkCommandBuffer.executeGeneratedCommandsEXT(vk::False, generatedCommandsInfo);
}
