module RendererVK;

import Core;
import File;

import :CommandBuffer;
import :Device;
import :Layout;
import :TextureManager;
import :Texture;

void GBufferPipeline::buildPipelineLayout(GraphicsPipelineLayout& layout, uint32 maxTextures)
{
    layout.vertexShader.debugFilePath = "Shaders/gbuffer.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/gbuffer.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eBack;

    auto& bindings = layout.vertexLayoutInfo.bindingDescriptions;
    bindings.push_back(vk::VertexInputBindingDescription{ .binding = 0, .stride = sizeof(RendererVKLayout::MeshVertex), .inputRate = vk::VertexInputRate::eVertex });
    bindings.push_back(vk::VertexInputBindingDescription{ .binding = 2, .stride = sizeof(uint32), .inputRate = vk::VertexInputRate::eInstance });

    auto& attributes = layout.vertexLayoutInfo.attributeDescriptions;
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(RendererVKLayout::MeshVertex, position) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(RendererVKLayout::MeshVertex, normal) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 3, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(RendererVKLayout::MeshVertex, texCoord) }); // UV for the alpha-mask discard
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 4, .binding = 2, .format = vk::Format::eR32Uint, .offset = 0 });

    auto& descriptorSetBindings = layout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // UBO
        .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // transformed instances
        .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // material infos (sky flag in VS, alpha mask in FS)
        .binding = 2, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // FFT ocean maps (VS displaces MATERIAL_FLAG_OCEAN instances)
        .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // terrain-data cascades (VS: ocean water depth/level)
        .binding = 5, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    // Texture array (binding 6 = highest, eVariableDescriptorCount) for the fragment alpha-mask test.
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 6, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(descriptorSetBindings.size());
    // Baked terrain-data cascades (5): ping-pong image refreshed per frame without re-recording the
    // cached prepass CB.
    layout.descriptorBindingFlags[layout.descriptorBindingFlags.size() - 2] = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    // View index (u_views[viewIndex]); the prepass is rendered once per eye in VR (0 = centre on desktop).
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex, .offset = 0, .size = sizeof(uint32) });
}

void GBufferPipeline::initialize(const GBuffer& gbuffer, uint32 maxTextures)
{
    m_maxTextures = maxTextures;
    m_textureSampler.initialize();
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout, maxTextures);
    m_graphicsPipeline.initialize(gbuffer.getRenderPass(), layout);
}

void GBufferPipeline::updateTextureDescriptor(vk::DescriptorSet descriptorSet, uint32 slotIdx, vk::ImageView view)
{
    // Streamed texture slot rewrite in the cached prepass CB's set (binding 6 is UPDATE_AFTER_BIND).
    vk::DescriptorImageInfo imageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 6, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void GBufferPipeline::updateTerrainHeightDescriptor(vk::DescriptorSet descriptorSet, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    // Points the terrain-data cascade binding (5, UPDATE_AFTER_BIND) at the active ping-pong image;
    // refreshed every frame so a CPU re-bake swaps images without re-recording the cached prepass CB.
    // The ocean branch of the prepass VS reads it for water depth/level.
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 5, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void GBufferPipeline::reloadShaders(const GBuffer& gbuffer)
{
    GraphicsPipelineLayout layout;
    buildPipelineLayout(layout, m_maxTextures);
    if (!m_graphicsPipeline.reloadShaders(gbuffer.getRenderPass(), layout))
        printf("GBufferPipeline: shader reload failed, keeping previous pipeline\n");
}

void GBufferPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params, uint32 viewIndex)
{
    oc::vector<DescriptorSetUpdateInfo> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.meshInstanceBuffer.getBuffer(), .range = params.meshInstanceBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.materialInfoBuffer.getBuffer(), .range = vk::WholeSize } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, // FFT ocean maps (VS displacement)
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.oceanMapsSampler, .imageView = params.oceanMapsView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
    };
    // Texture array (binding 6) for the fragment alpha-mask test; same source as the forward/GI passes.
    DescriptorSetUpdateInfo texUpdate{ .binding = 6, .type = vk::DescriptorType::eCombinedImageSampler };
    for (uint16 texIdx = 0; texIdx < (uint16)Globals::textureManager.getNumTextures(); ++texIdx)
        texUpdate.imageInfos.push_back(vk::DescriptorImageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = Globals::textureManager.getViewForDescriptor(texIdx), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
    if (!texUpdate.imageInfos.empty())
        updates.push_back(oc::move(texUpdate));

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    commandBuffer.cmdUpdateDescriptorSets(m_graphicsPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, descriptorSet, updates);
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipeline());
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_graphicsPipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);
    vkCommandBuffer.bindVertexBuffers(0, { params.vertexBuffer.getBuffer() }, { 0 });
    vkCommandBuffer.bindVertexBuffers(2, { params.instanceIdxBuffer.getBuffer() }, { 0 });
    vkCommandBuffer.bindIndexBuffer(params.indexBuffer.getBuffer(), 0, vk::IndexType::eUint32);
    vkCommandBuffer.pushConstants(m_graphicsPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(uint32), &viewIndex);

    // Reuse the camera-cull IndirectDrawSequence buffer: skip the leading pipelineIndex uint (offset 4),
    // and draw one VkDrawIndexedIndirectCommand per mesh (stride = sizeof(IndirectDrawSequence) = 24).
    // The draw count comes from the CPU-written mesh-count buffer so registering meshes never re-records.
    const uint32 maxDrawCount = (uint32)(params.indirectCommandBuffer.getSize() / sizeof(RendererVKLayout::IndirectDrawSequence));
    vkCommandBuffer.drawIndexedIndirectCount(params.indirectCommandBuffer.getBuffer(), 4,
        params.meshCountBuffer.getBuffer(), 0, maxDrawCount, sizeof(RendererVKLayout::IndirectDrawSequence));
}
