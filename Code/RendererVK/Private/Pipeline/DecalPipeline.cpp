module RendererVK;

import Core;
import Core.glm;
import File;
import :DecalPipeline;
import :GraphicsPipeline;
import :TextureManager;
import :Device;
import :Sampler;
import :Layout;

using namespace RendererVKLayout;

static vk::DescriptorBufferInfo decalBufInfo(const Buffer& buffer)
{
    return vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() };
}
static vk::DescriptorImageInfo decalSampledRO(vk::Sampler sampler, vk::ImageView view)
{
    return vk::DescriptorImageInfo{ .sampler = sampler, .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
}

void DecalPipeline::buildLayout(GraphicsPipelineLayout& layout, uint32 maxTextures)
{
    layout.vertexShader.debugFilePath = "Shaders/decal.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/decal.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    // Cull FRONT faces and skip the depth test: the box's far/inside faces rasterize exactly once per
    // covered pixel even with the camera inside the volume; the fragment shader clips to the box.
    layout.cullMode = vk::CullModeFlagBits::eFront;
    layout.blendEnable = true; // premultiplied: out = src.rgb + dst * (1 - src.a)
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    // 20 = the set's highest binding number: required for eVariableDescriptorCount.
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 20, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound
        | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void DecalPipeline::initialize(vk::RenderPass sceneRenderPass, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount)
{
    m_viewCount = viewCount;
    m_textureSampler.initialize();
    GraphicsPipelineLayout layout;
    buildLayout(layout, maxTextures);
    m_pipeline.initialize(sceneRenderPass, layout);

    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_decalBuffers[i].initialize(MAX_DECALS * sizeof(DecalInfo),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "Decals", BufferHostAccess::eSequentialWrite);
        m_mappedDecals[i] = m_decalBuffers[i].mapMemory<DecalInfo>();

        m_indirectBuffers[i].initialize(sizeof(vk::DrawIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "DecalIndirect", BufferHostAccess::eSequentialWrite);
        m_mappedIndirect[i] = m_indirectBuffers[i].mapMemory<uint32>();
        m_mappedIndirect[i][0] = 36; // vertexCount: one cube per decal
        m_mappedIndirect[i][1] = 0;  // instanceCount
        m_mappedIndirect[i][2] = 0;
        m_mappedIndirect[i][3] = 0;
        m_indirectBuffers[i].flushMappedMemory(sizeof(vk::DrawIndirectCommand));

        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_sets[drawSlot(i, eye)].initialize(m_pipeline.getDescriptorSetLayout(), numTextureDescriptors);
    }
}

void DecalPipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    GraphicsPipelineLayout layout;
    buildLayout(layout, Globals::textureManager.getDescriptorCap());
    if (!m_pipeline.reloadShaders(sceneRenderPass, layout))
        printf("DecalPipeline: shader reload failed, keeping previous pipeline\n");
}

void DecalPipeline::upload(uint32 frameIdx, uint32 decalCount)
{
    if (decalCount > 0)
        m_decalBuffers[frameIdx].flushMappedMemory(decalCount * sizeof(DecalInfo));
    m_mappedIndirect[frameIdx][1] = decalCount;
    m_indirectBuffers[frameIdx].flushMappedMemory(sizeof(vk::DrawIndirectCommand));
}

void DecalPipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_sets[drawSlot(i, eye)].initialize(m_pipeline.getDescriptorSetLayout(), numTextureDescriptors);
}

void DecalPipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    const vk::DescriptorImageInfo imageInfo{
        .sampler = m_textureSampler.getSampler(),
        .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    oc::array<vk::WriteDescriptorSet, MAX_VIEWS> writes;
    for (uint32 eye = 0; eye < m_viewCount; ++eye)
        writes[eye] = vk::WriteDescriptorSet{ .dstSet = m_sets[drawSlot(frameIdx, eye)].getDescriptorSet(),
            .dstBinding = 20, .dstArrayElement = slotIdx, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(m_viewCount, writes.data(), 0, nullptr);
}

void DecalPipeline::recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 viewIndex = eyeToViewIndex(eye, m_viewCount);
    DescriptorSet& set = m_sets[drawSlot(frameIdx, eye)];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    oc::array<DescriptorSetUpdateInfo, 6> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { decalBufInfo(m_decalBuffers[frameIdx]) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { decalSampledRO(params.gbufferSampler, params.gbufferNormalView) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { decalBufInfo(params.giGridDataBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 20, .type = vk::DescriptorType::eCombinedImageSampler },
    };
    const size_t numTextures = Globals::textureManager.getNumTextures();
    updates[5].imageInfos.reserve(numTextures);
    for (uint16 texIdx = 0; texIdx < (uint16)numTextures; ++texIdx)
        updates[5].imageInfos.push_back(vk::DescriptorImageInfo{
            .sampler = m_textureSampler.getSampler(),
            .imageView = Globals::textureManager.getViewForDescriptor(texIdx), // freed slots -> fallback
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });

    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), 0, 1, sizeof(vk::DrawIndirectCommand));
}
