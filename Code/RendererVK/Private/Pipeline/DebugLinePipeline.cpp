module RendererVK;

import Core;
import Core.glm;
import File;
import :DebugLinePipeline;
import :GraphicsPipeline;
import :Layout;

void DebugLinePipeline::buildLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/debug_line.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/debug_line.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.topology = vk::PrimitiveTopology::eLineList;
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.depthTestEnable = true;   // occluded by scene geometry
    layout.depthWriteEnable = false; // overlay: never affects the scene depth

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
}

void DebugLinePipeline::initialize(vk::RenderPass renderPass)
{
    m_renderPass = renderPass;
    GraphicsPipelineLayout layout; buildLayout(layout);
    m_pipeline.initialize(renderPass, layout);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        m_sets[i].initialize(m_pipeline.getDescriptorSetLayout());
}

void DebugLinePipeline::reloadShaders(vk::RenderPass renderPass)
{
    m_renderPass = renderPass;
    if (!m_renderPass)
        return;
    GraphicsPipelineLayout layout; buildLayout(layout);
    if (!m_pipeline.reloadShaders(m_renderPass, layout))
        printf("DebugLinePipeline: shader reload failed, keeping previous pipeline\n");
}

bool DebugLinePipeline::upload(uint32 frameIdx, oc::span<const LineVertex> verts)
{
    bool created = false;
    if (!m_buffersReady)
    {
        if (verts.empty())
            return false;
        for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        {
            m_vertexBuffers[i].initialize(RendererVKLayout::MAX_DEBUG_LINE_VERTICES * sizeof(LineVertex),
                vk::BufferUsageFlagBits2::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible, false, "DebugLineVertices", BufferHostAccess::eSequentialWrite);
            m_mappedVerts[i] = m_vertexBuffers[i].mapMemory<LineVertex>();

            m_indirectBuffers[i].initialize(sizeof(vk::DrawIndirectCommand),
                vk::BufferUsageFlagBits2::eIndirectBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible, false, "DebugLineIndirect", BufferHostAccess::eSequentialWrite);
            m_mappedIndirect[i] = m_indirectBuffers[i].mapMemory<uint32>();
            m_mappedIndirect[i][0] = 0; // vertexCount
            m_mappedIndirect[i][1] = 1; // instanceCount
            m_mappedIndirect[i][2] = 0;
            m_mappedIndirect[i][3] = 0;
            m_indirectBuffers[i].flushMappedMemory(sizeof(vk::DrawIndirectCommand));
        }
        m_buffersReady = true;
        created = true;
    }

    uint32 count = (uint32)verts.size();
    if (count > RendererVKLayout::MAX_DEBUG_LINE_VERTICES)
    {
        count = RendererVKLayout::MAX_DEBUG_LINE_VERTICES & ~1u;
        if (!m_overflowWarned)
        {
            printf("DebugLinePipeline: %zu line vertices exceed the %u capacity, excess dropped\n", verts.size(), RendererVKLayout::MAX_DEBUG_LINE_VERTICES);
            m_overflowWarned = true;
        }
    }
    if (count > 0)
    {
        memcpy(m_mappedVerts[frameIdx].data(), verts.data(), count * sizeof(LineVertex));
        m_vertexBuffers[frameIdx].flushMappedMemory(count * sizeof(LineVertex));
    }
    m_mappedIndirect[frameIdx][0] = count;
    m_indirectBuffers[frameIdx].flushMappedMemory(sizeof(vk::DrawIndirectCommand));
    return created;
}

void DebugLinePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo)
{
    DescriptorSet& set = m_sets[frameIdx];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    oc::array<DescriptorSetUpdateInfo, 2> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_vertexBuffers[frameIdx].getBuffer(), .range = m_vertexBuffers[frameIdx].getSize() } } },
    };

    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), 0, 1, sizeof(vk::DrawIndirectCommand));
}
