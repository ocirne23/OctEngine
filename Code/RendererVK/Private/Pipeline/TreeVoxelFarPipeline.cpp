module RendererVK;

import Core;
import File;
import :Device;

namespace
{
    // MIRRORS tree_voxel_far.cs.glsl's push block (scalar layout).
    struct VoxelFarPush
    {
        vk::DeviceAddress records, map, types, chunks, farTypes, buckets, appended, state, commands, sorted;
        uint32 numChunks, numTypes, numFarTypes, numBuckets, worldSeed, mapSize;
        float chunkSize, endDistance;
        uint32 maxInstances, pad;
    };
    static_assert(sizeof(VoxelFarPush) == 120);

    constexpr uint32 COMMAND_SIZE = 5 * sizeof(uint32); // VkDrawIndexedIndirectCommand
    constexpr uint32 STATE_WORDS = 1 + 2 * TreeVoxelFarPipeline::MAX_BUCKETS;

    void computeBarrier(vk::CommandBuffer cmd)
    {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }

    void writeMapped(Buffer& buffer, oc::span<uint8> mapped, const void* data, size_t bytes)
    {
        if (bytes == 0)
            return;
        memcpy(mapped.data(), data, bytes);
        buffer.flushMappedMemory(bytes);
    }
}

void TreeVoxelFarPipeline::buildLayout(ComputePipelineLayout& layout, uint32 pass, bool shadow)
{
    layout.computeShaderDebugFilePath = "Shaders/Trees/tree_voxel_far.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.defines.push_back({ "FAR_PASS", oc::to_string(pass) });
    if (shadow)
        layout.defines.push_back({ "FAR_SHADOW", "1" });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(VoxelFarPush) });
}

void TreeVoxelFarPipeline::initLists(Lists& lists, uint32 maxInstances, const char* name)
{
    constexpr vk::BufferUsageFlags2 address = vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eStorageBuffer;
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        lists.chunks[i].initialize(MAX_CHUNKS * sizeof(ChunkGpu), address, vk::MemoryPropertyFlagBits::eHostVisible, false,
            oc::format("{}/Chunks", name).c_str(), BufferHostAccess::eSequentialWrite);
        lists.buckets[i].initialize(MAX_BUCKETS * sizeof(BucketGpu), address, vk::MemoryPropertyFlagBits::eHostVisible, false,
            oc::format("{}/Buckets", name).c_str(), BufferHostAccess::eSequentialWrite);
        lists.chunksMapped[i] = lists.chunks[i].mapMemory();
        lists.bucketsMapped[i] = lists.buckets[i].mapMemory();
        lists.appended[i].initialize(maxInstances * sizeof(InstanceGpu), address, vk::MemoryPropertyFlagBits::eDeviceLocal, false,
            oc::format("{}/Appended", name).c_str());
        lists.state[i].initialize(STATE_WORDS * sizeof(uint32), address | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal,
            false, oc::format("{}/State", name).c_str());
        lists.commands[i].initialize(MAX_BUCKETS * COMMAND_SIZE, address | vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, oc::format("{}/Commands", name).c_str());
        lists.sorted[i].initialize(maxInstances * sizeof(InstanceGpu), address | vk::BufferUsageFlagBits2::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, oc::format("{}/Sorted", name).c_str());
    }
}

void TreeVoxelFarPipeline::initialize()
{
    ComputePipelineLayout cull, shadowCull, prefix, scatter;
    buildLayout(cull, 0);
    buildLayout(shadowCull, 0, true);
    buildLayout(prefix, 1);
    buildLayout(scatter, 2);
    m_cull.initialize(cull);
    m_shadowCull.initialize(shadowCull);
    m_prefix.initialize(prefix);
    m_scatter.initialize(scatter);

    constexpr vk::BufferUsageFlags2 address = vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eStorageBuffer;
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_sets[i].initialize(m_cull.getDescriptorSetLayout(), "TreeVoxelFar");
        m_types[i].initialize(MAX_TYPES * sizeof(FarTypeGpu), address, vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeVoxelFar/Types",
            BufferHostAccess::eSequentialWrite);
        m_typesMapped[i] = m_types[i].mapMemory();
    }
    initLists(m_view, MAX_INSTANCES, "TreeVoxelFar");
    initLists(m_shadow, MAX_SHADOW_INSTANCES, "TreeVoxelFarShadow");
}

void TreeVoxelFarPipeline::reloadShaders()
{
    ComputePipelineLayout cull, shadowCull, prefix, scatter;
    buildLayout(cull, 0);
    buildLayout(shadowCull, 0, true);
    buildLayout(prefix, 1);
    buildLayout(scatter, 2);
    if (!m_cull.reloadShaders(cull) || !m_shadowCull.reloadShaders(shadowCull) || !m_prefix.reloadShaders(prefix) || !m_scatter.reloadShaders(scatter))
        printf("TreeVoxelFarPipeline: shader reload failed, keeping previous pipeline\n");
}

void TreeVoxelFarPipeline::writeList(Lists& lists, uint32 frameIdx, oc::span<const ChunkGpu> chunks, oc::span<const BucketGpu> buckets)
{
    lists.numChunks[frameIdx] = (uint32)oc::min<size_t>(chunks.size(), MAX_CHUNKS);
    lists.numBuckets[frameIdx] = (uint32)oc::min<size_t>(buckets.size(), MAX_BUCKETS);
    writeMapped(lists.chunks[frameIdx], lists.chunksMapped[frameIdx], chunks.data(), lists.numChunks[frameIdx] * sizeof(ChunkGpu));
    writeMapped(lists.buckets[frameIdx], lists.bucketsMapped[frameIdx], buckets.data(), lists.numBuckets[frameIdx] * sizeof(BucketGpu));
}

void TreeVoxelFarPipeline::setFrame(uint32 frameIdx, oc::span<const ChunkGpu> chunks, oc::span<const FarTypeGpu> types, oc::span<const BucketGpu> buckets,
    oc::span<const ChunkGpu> shadowChunks, oc::span<const BucketGpu> shadowBuckets)
{
    m_numTypes[frameIdx] = (uint32)oc::min<size_t>(types.size(), MAX_TYPES);
    writeMapped(m_types[frameIdx], m_typesMapped[frameIdx], types.data(), m_numTypes[frameIdx] * sizeof(FarTypeGpu));
    writeList(m_view, frameIdx, chunks, buckets);
    writeList(m_shadow, frameIdx, shadowChunks, shadowBuckets);
}

void TreeVoxelFarPipeline::runPasses(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params, const ComputePipeline& cull, Lists& lists,
    uint32 maxInstances, vk::PipelineStageFlags2 drawStages)
{
    // The counts restart at 0; the slot's last draws (which read these buffers) retired with its fence.
    cmd.fillBuffer(lists.state[frameIdx].getBuffer(), 0, vk::WholeSize, 0u);
    {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }

    // The UBO (this slot's: written once per frame, the slot's previous use is done).
    const vk::DescriptorBufferInfo uboInfo{ .buffer = params.ubo.getBuffer(), .range = RendererVKLayout::UBO_RANGE };
    const vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    const vk::WriteDescriptorSet write{ .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer,
        .pBufferInfo = &uboInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);

    const VoxelFarPush push{
        .records = params.records, .map = params.map, .types = params.types,
        .chunks = lists.chunks[frameIdx].getDeviceAddress(), .farTypes = m_types[frameIdx].getDeviceAddress(),
        .buckets = lists.buckets[frameIdx].getDeviceAddress(), .appended = lists.appended[frameIdx].getDeviceAddress(),
        .state = lists.state[frameIdx].getDeviceAddress(), .commands = lists.commands[frameIdx].getDeviceAddress(),
        .sorted = lists.sorted[frameIdx].getDeviceAddress(),
        .numChunks = lists.numChunks[frameIdx], .numTypes = params.numTypes, .numFarTypes = m_numTypes[frameIdx],
        .numBuckets = lists.numBuckets[frameIdx], .worldSeed = params.worldSeed, .mapSize = params.mapSize,
        .chunkSize = params.chunkSize, .endDistance = params.endDistance, .maxInstances = maxInstances, .pad = 0,
    };
    const auto run = [&](const ComputePipeline& pipeline, uint32 groups)
    {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.pushConstants(pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(VoxelFarPush), &push);
        if (groups > 0)
            cmd.dispatch(groups, 1, 1);
    };
    run(cull, lists.numChunks[frameIdx]); // a workgroup per chunk
    computeBarrier(cmd);
    run(m_prefix, 1);
    computeBarrier(cmd);
    run(m_scatter, maxInstances / 64); // a thread per appended record (the rest return at once)

    const vk::MemoryBarrier2 toDraws{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | drawStages,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eVertexAttributeRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toDraws });
}

void TreeVoxelFarPipeline::clearList(vk::CommandBuffer cmd, Lists& lists, uint32 frameIdx)
{
    cmd.fillBuffer(lists.commands[frameIdx].getBuffer(), 0, vk::WholeSize, 0u);
    const vk::MemoryBarrier2 toDraws{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toDraws });
}

void TreeVoxelFarPipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params)
{
    runPasses(cmd, frameIdx, params, m_cull, m_view, MAX_INSTANCES, vk::PipelineStageFlagBits2::eVertexAttributeInput);
}

void TreeVoxelFarPipeline::recordClear(vk::CommandBuffer cmd, uint32 frameIdx)
{
    clearList(cmd, m_view, frameIdx);
}

void TreeVoxelFarPipeline::recordShadow(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params)
{
    runPasses(cmd, frameIdx, params, m_shadowCull, m_shadow, MAX_SHADOW_INSTANCES, vk::PipelineStageFlagBits2::eVertexAttributeInput);
}

void TreeVoxelFarPipeline::recordShadowClear(vk::CommandBuffer cmd, uint32 frameIdx)
{
    clearList(cmd, m_shadow, frameIdx);
}
