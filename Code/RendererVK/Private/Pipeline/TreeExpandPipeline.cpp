module RendererVK;

import Core;
import File;

import :TreeExpandPipeline;

namespace
{
    // Mirrors the push_constant block of tree_expand.cs.glsl (std430: the vec4 lands on offset 48).
    struct TreeExpandPush
    {
        vk::DeviceAddress pieces;
        vk::DeviceAddress types;
        vk::DeviceAddress instances;
        vk::DeviceAddress passMasks;
        vk::DeviceAddress lodStateBias;
        uint32 numPieces;
        uint32 baseInstance;
        glm::vec4 cameraPosScale;
        uint32 forceFar;
        uint32 stampedAll;
        uint32 stampedMain;
        uint32 stampedProxy;
        uint32 dummyNode;
        uint32 identityOffset;
    };
    static_assert(sizeof(TreeExpandPush) == 88);
    static_assert(offsetof(TreeExpandPush, cameraPosScale) == 48);

    constexpr uint32 GROUP_SIZE = 64;
}

void TreeExpandPipeline::initialize()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
}

void TreeExpandPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("TreeExpandPipeline: shader reload failed, keeping previous pipeline\n");
}

void TreeExpandPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_expand.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = (uint32)sizeof(TreeExpandPush),
    });
}

void TreeExpandPipeline::record(vk::CommandBuffer cb, vk::DeviceAddress instances, vk::DeviceAddress passMasks, vk::DeviceAddress lodStateBias,
    oc::span<const Dispatch> dispatches)
{
    cb.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    for (const Dispatch& d : dispatches)
    {
        if (d.numPieces == 0)
            continue;
        const TreeExpandPush push{
            .pieces = d.pieces,
            .types = d.types,
            .instances = instances,
            .passMasks = passMasks,
            .lodStateBias = lodStateBias,
            .numPieces = d.numPieces,
            .baseInstance = d.baseInstance,
            .cameraPosScale = d.cameraPosScale,
            .forceFar = d.forceFar,
            .stampedAll = d.stampedAll,
            .stampedMain = d.stampedMain,
            .stampedProxy = d.stampedProxy,
            .dummyNode = d.dummyNode,
            .identityOffset = d.identityOffset,
        };
        cb.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, (uint32)sizeof(push), &push);
        cb.dispatch((d.numPieces + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1);
    }
    // The written records / masks / biases -> every reader of the instance stream this frame: the culls
    // (compute), the TLAS-instance writer, recordPrevCopy (transfer).
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
    };
    cb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
}
