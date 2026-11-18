module RendererVK;

import Core;
import Core.glm;
import File;
import :BeamCardPipeline;
import :GraphicsPipeline;
import :Layout;

void BeamCardPipeline::buildLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/Sky/beam_card.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/Sky/beam_card.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.blendEnable = true; // the glow adds to the scene
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eOne;
    layout.colorWriteAlpha = false; // scene colour alpha = TAA's ocean flag
    layout.depthTestEnable = false;  // the shader cuts the glow at the scene depth itself
    layout.depthWriteEnable = false;
    constexpr vk::ShaderStageFlags both = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = both });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = both });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = both, .offset = 0, .size = sizeof(uint32) });
}

void BeamCardPipeline::initialize(vk::RenderPass renderPass, uint32 viewCount)
{
    m_viewCount = oc::clamp(viewCount, 1u, 2u);
    GraphicsPipelineLayout layout; buildLayout(layout);
    m_pipeline.initialize(renderPass, layout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        for (uint32 e = 0; e < m_viewCount; ++e)
            m_sets[slot(f, e)].initialize(m_pipeline.getDescriptorSetLayout(), "BeamCards");
        m_cardBuffers[f].initialize(MAX_CARDS * sizeof(Card), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "BeamCards", BufferHostAccess::eSequentialWrite);
        m_mappedCards[f] = m_cardBuffers[f].mapMemory<Card>();
        m_indirectBuffers[f].initialize(sizeof(vk::DrawIndirectCommand), vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "BeamCardIndirect", BufferHostAccess::eSequentialWrite);
        m_mappedIndirect[f] = m_indirectBuffers[f].mapMemory<uint32>();
        m_mappedIndirect[f][0] = 0; // vertexCount
        m_mappedIndirect[f][1] = 1; // instanceCount
        m_mappedIndirect[f][2] = 0;
        m_mappedIndirect[f][3] = 0;
        m_indirectBuffers[f].flushMappedMemory(sizeof(vk::DrawIndirectCommand));
    }
}

void BeamCardPipeline::reloadShaders(vk::RenderPass renderPass)
{
    if (!renderPass)
        return;
    GraphicsPipelineLayout layout; buildLayout(layout);
    if (!m_pipeline.reloadShaders(renderPass, layout))
        printf("BeamCardPipeline: shader reload failed, keeping previous pipeline\n");
}

void BeamCardPipeline::upload(uint32 frameIdx, oc::span<const RendererVKLayout::LightInfo> beams)
{
    // A BeamLight: pos = the centre, direction = the axis x the length, width = the core radius, -radius = the range,
    // rotation = the tip fade (Light.ixx).
    uint32 count = 0;
    Card* const mapped = m_mappedCards[frameIdx].data();
    for (const RendererVKLayout::LightInfo& beam : beams)
    {
        if (count >= MAX_CARDS)
            break;
        const float length = glm::length(beam.direction);
        if (!(length >= 1e-4f) || !(beam.radius < -1e-5f) || !(beam.width > 0.0f))
            continue;
        const glm::vec3 axis = beam.direction / length;
        mapped[count++] = Card{ glm::vec4(beam.pos - beam.direction * 0.5f, beam.width), glm::vec4(axis, length),
            glm::vec4(beam.color, -beam.radius), glm::vec4(beam.rotation, 0.0f, 0.0f, 0.0f) };
    }
    if (count > 0)
        m_cardBuffers[frameIdx].flushMappedMemory(count * sizeof(Card));
    m_counts[frameIdx] = count;
    m_mappedIndirect[frameIdx][0] = count * 6u;
    m_indirectBuffers[frameIdx].flushMappedMemory(sizeof(vk::DrawIndirectCommand));
}

void BeamCardPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params)
{
    DescriptorSet& set = m_sets[slot(frameIdx, oc::min(eye, m_viewCount - 1))];
    vk::DescriptorSet vkSet = set.getDescriptorSet();
    const auto sampledGeneral = [](vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; };
    oc::array<DescriptorSetUpdateInfo, 5> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = RendererVKLayout::UBO_RANGE } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_cardBuffers[frameIdx].getBuffer(), .range = m_cardBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.sceneDepthSampler, .imageView = params.sceneDepthView, .imageLayout = params.sceneDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.cloudSampler, params.cloudColorView) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.cloudSampler, params.cloudDepthView) } },
    };
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_viewCount);
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), 0, 1, sizeof(vk::DrawIndirectCommand));
}
