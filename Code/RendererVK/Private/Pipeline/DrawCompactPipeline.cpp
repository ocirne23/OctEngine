module RendererVK;

import Core;
import File;

void DrawCompactPipeline::initialize(uint32 numLists)
{
    assert(numLists >= 1 && numLists <= MAX_LISTS);
    m_numLists = numLists;
    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
}

void DrawCompactPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("DrawCompactPipeline: shader reload failed, keeping previous pipeline\n");
}

void DrawCompactPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/draw_compact.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.defines.push_back({ "NUM_LISTS", oc::format("{}", m_numLists) });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = (uint32)((2 + m_numLists) * sizeof(vk::DeviceAddress)),
    });
}

void DrawCompactPipeline::record(vk::CommandBuffer cb, const Buffer& slotCount, oc::span<Buffer* const> lists, const Buffer& drawCounts)
{
    assert(lists.size() == m_numLists);
    {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }
    oc::array<vk::DeviceAddress, 2 + MAX_LISTS> push{};
    push[0] = slotCount.getDeviceAddress();
    push[1] = drawCounts.getDeviceAddress();
    for (uint32 k = 0; k < m_numLists; ++k)
        push[2 + k] = lists[k]->getDeviceAddress();
    cb.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cb.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0,
        (uint32)((2 + m_numLists) * sizeof(vk::DeviceAddress)), push.data());
    cb.dispatch(1, 1, 1);
}
