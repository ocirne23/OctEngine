module RendererVK;

import Core;
import Core.glm;
import File;
import :ForceFieldPipeline;
import :GraphicsPipeline;
import :ComputePipeline;
import :Device;
import :Layout;

using namespace RendererVKLayout;

void ForceFieldPipeline::buildDrawLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/force_shell.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/force_shell.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    if (m_useGrid)
    {
        layout.vertexShader.defines.push_back({ "FORCE_GRID", "" });
        layout.fragmentShader.defines.push_back({ "FORCE_GRID", "" });
    }
    // Cull FRONT faces and skip the fixed-function depth test: the box's far/inside faces rasterize
    // exactly once per covered pixel even with the camera inside the volume; the fragment shader
    // marches within the box and depth-tests against the G-buffer depth itself.
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
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });

    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void ForceFieldPipeline::buildComputeLayout(ComputePipelineLayout& layout, const char* shaderPath)
{
    layout.computeShaderDebugFilePath = shaderPath;
    layout.computeShaderText = FileSystem::readFileStr(shaderPath);
    // force_grid.cs defines FORCE_GRID itself (it IS the grid pass); the others follow the toggle.
    if (m_useGrid)
        layout.defines.push_back({ "FORCE_GRID", "" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 binding : { 1u, 3u, 4u, 5u, 6u })
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void ForceFieldPipeline::createGridBuffers()
{
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        // Table header {numCells, dataCounter, tableSize, pad} is the CPU demand readback, so the
        // table stays DeviceLocal|HostVisible (light-grid contract).
        m_gridTableBuffers[i].initialize(16 + (size_t)m_tableEntries * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceGridTable");
        { // zero the demand header: checkForceGridCapacity reads it before the first GPU clear runs
            std::span<uint32> header = m_gridTableBuffers[i].mapMemory<uint32>(0, 16);
            memset(header.data(), 0, 16);
            m_gridTableBuffers[i].unmapMemory();
        }
        m_gridDataBuffers[i].initialize(m_gridDataSize,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ForceGridData");
    }
}

void ForceFieldPipeline::initialize(vk::RenderPass sceneRenderPass, uint32 viewCount)
{
    m_viewCount = viewCount;
    GraphicsPipelineLayout drawLayout;
    buildDrawLayout(drawLayout);
    m_pipeline.initialize(sceneRenderPass, drawLayout);

    ComputePipelineLayout gridLayout, forceLayout, queryLayout;
    buildComputeLayout(gridLayout, "Shaders/force_grid.cs.glsl");
    buildComputeLayout(forceLayout, "Shaders/force_emitter.cs.glsl");
    buildComputeLayout(queryLayout, "Shaders/force_query.cs.glsl");
    m_gridPipeline.initialize(gridLayout);
    m_emitterForcePipeline.initialize(forceLayout);
    m_queryPipeline.initialize(queryLayout);

    createGridBuffers();

    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_emitterBuffers[i].initialize(sizeof(ForceEmittersGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceEmitters", BufferHostAccess::eSequentialWrite);
        m_mappedEmitters[i] = m_emitterBuffers[i].mapMemory<ForceEmittersGpu>();
        m_mappedEmitters[i].data()->count = 0;
        m_mappedEmitters[i].data()->bigCount = 0;
        m_emitterBuffers[i].flushMappedMemory(FORCE_EMITTER_HEADER_SIZE);

        m_queryBuffers[i].initialize(sizeof(ForceQueriesGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceQueries", BufferHostAccess::eSequentialWrite);
        m_mappedQueries[i] = m_queryBuffers[i].mapMemory<ForceQueriesGpu>();
        m_mappedQueries[i].data()->count = 0;
        m_queryBuffers[i].flushMappedMemory(FORCE_QUERY_HEADER_SIZE);

        m_indirectBuffers[i].initialize(INDIRECT_UINTS * sizeof(uint32),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ForceIndirect", BufferHostAccess::eSequentialWrite);
        m_mappedIndirect[i] = m_indirectBuffers[i].mapMemory<uint32>();
        memset(m_mappedIndirect[i].data(), 0, INDIRECT_UINTS * sizeof(uint32));
        m_mappedIndirect[i][DRAW_CMD_OFFSET] = 36; // vertexCount: one cube per emitter
        m_mappedIndirect[i][GRID_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][GRID_DISPATCH_OFFSET + 2] = 1;
        m_mappedIndirect[i][EMITTER_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][EMITTER_DISPATCH_OFFSET + 2] = 1;
        m_mappedIndirect[i][QUERY_DISPATCH_OFFSET + 1] = 1;
        m_mappedIndirect[i][QUERY_DISPATCH_OFFSET + 2] = 1;
        m_indirectBuffers[i].flushMappedMemory(vk::WholeSize);

        // GPU-written, CPU-read ~2 frames later; zeroed so pre-first-frame reads decode as "no force
        // / never evaluated" instead of garbage (ocean readback pattern).
        m_forceReadbackBuffers[i].initialize(MAX_FORCE_EMITTERS * sizeof(glm::vec4),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "ForceReadback");
        m_mappedForceReadback[i] = m_forceReadbackBuffers[i].mapMemory<glm::vec4>();
        memset(m_mappedForceReadback[i].data(), 0, m_mappedForceReadback[i].size_bytes());

        m_queryReadbackBuffers[i].initialize(MAX_FORCE_QUERIES * sizeof(ForceQueryResult),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "ForceQueryReadback");
        m_mappedQueryReadback[i] = m_queryReadbackBuffers[i].mapMemory<ForceQueryResult>();
        memset(m_mappedQueryReadback[i].data(), 0, m_mappedQueryReadback[i].size_bytes());

        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_drawSets[drawSlot(i, eye)].initialize(m_pipeline.getDescriptorSetLayout());
        m_gridSets[i].initialize(m_gridPipeline.getDescriptorSetLayout());
        m_emitterForceSets[i].initialize(m_emitterForcePipeline.getDescriptorSetLayout());
        m_querySets[i].initialize(m_queryPipeline.getDescriptorSetLayout());
    }
}

void ForceFieldPipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    GraphicsPipelineLayout drawLayout;
    buildDrawLayout(drawLayout);
    if (!m_pipeline.reloadShaders(sceneRenderPass, drawLayout))
        printf("ForceFieldPipeline: shell shader reload failed, keeping previous pipeline\n");
    ComputePipelineLayout gridLayout, forceLayout, queryLayout;
    buildComputeLayout(gridLayout, "Shaders/force_grid.cs.glsl");
    buildComputeLayout(forceLayout, "Shaders/force_emitter.cs.glsl");
    buildComputeLayout(queryLayout, "Shaders/force_query.cs.glsl");
    if (!m_gridPipeline.reloadShaders(gridLayout))
        printf("ForceFieldPipeline: grid shader reload failed, keeping previous pipeline\n");
    if (!m_emitterForcePipeline.reloadShaders(forceLayout))
        printf("ForceFieldPipeline: emitter force shader reload failed, keeping previous pipeline\n");
    if (!m_queryPipeline.reloadShaders(queryLayout))
        printf("ForceFieldPipeline: query shader reload failed, keeping previous pipeline\n");
}

void ForceFieldPipeline::upload(uint32 frameIdx, std::span<const ForceEmitterGpu> slots,
    std::span<const ForceQueryGpu> querySlots, float bigReachThreshold)
{
    ForceEmittersGpu* dst = m_mappedEmitters[frameIdx].data();
    uint32 count = 0;
    uint32 bigCount = 0;
    const auto compact = [&](const ForceEmitterGpu& e, uint32 slot)
    {
        ForceEmitterGpu& out = dst->emitters[count];
        out = e;
        out.teamFlags.z = slot; // slot-indexed readback target
        // Big emitters bypass the grid into the globally-scanned list; when the list is full the
        // overflow inserts into the grid anyway (large footprint, but never a hole in the field).
        const float maxReach = e.posReach.w; // Reach IS the max extent (the bubble spans the output line)
        if (maxReach > bigReachThreshold && bigCount < MAX_FORCE_BIG_EMITTERS)
        {
            out.teamFlags.y |= FORCE_FLAG_BIG;
            dst->bigIndices[bigCount++] = count;
        }
        else
            out.teamFlags.y &= ~FORCE_FLAG_BIG;
        ++count;
    };
    // Two-pass partition: drawable shells first so the shell draw's instanceCount can stop before
    // the shellAlpha-0 emitters — an invisible world-scale emitter still contributes field (grid/
    // force/query dispatches use the FULL count) but never costs its full-screen ray march.
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if ((slots[slot].teamFlags.y & FORCE_FLAG_ACTIVE) && slots[slot].outputParams.y > 0.0f)
            compact(slots[slot], slot);
    const uint32 drawCount = count;
    for (uint32 slot = 0; slot < (uint32)slots.size(); ++slot)
        if ((slots[slot].teamFlags.y & FORCE_FLAG_ACTIVE) && slots[slot].outputParams.y <= 0.0f)
            compact(slots[slot], slot);
    dst->count = count;
    dst->bigCount = bigCount;
    m_emitterBuffers[frameIdx].flushMappedMemory(FORCE_EMITTER_HEADER_SIZE + count * sizeof(ForceEmitterGpu));

    ForceQueriesGpu* q = m_mappedQueries[frameIdx].data();
    const uint32 numQueries = (uint32)glm::min(querySlots.size(), (size_t)MAX_FORCE_QUERIES);
    if (numQueries > 0)
        memcpy(q->queries, querySlots.data(), numQueries * sizeof(ForceQueryGpu));
    q->count = numQueries;
    m_queryBuffers[frameIdx].flushMappedMemory(FORCE_QUERY_HEADER_SIZE + numQueries * sizeof(ForceQueryGpu));

    uint32* ind = m_mappedIndirect[frameIdx].data();
    ind[DRAW_CMD_OFFSET + 1] = drawCount; // instanceCount: drawable shells only (see partition above)
    ind[GRID_DISPATCH_OFFSET] = count; // single-thread workgroups (see force_grid.cs.glsl)
    ind[EMITTER_DISPATCH_OFFSET] = (count + FORCE_SIM_GROUP_SIZE - 1) / FORCE_SIM_GROUP_SIZE;
    ind[QUERY_DISPATCH_OFFSET] = (numQueries + FORCE_SIM_GROUP_SIZE - 1) / FORCE_SIM_GROUP_SIZE;
    m_indirectBuffers[frameIdx].flushMappedMemory(vk::WholeSize);
}

void ForceFieldPipeline::recordCompute(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo)
{
    vk::CommandBuffer vkCb = commandBuffer.getCommandBuffer();
    const auto bufInfo = [](const Buffer& buffer) {
        return vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() };
    };
    // One set shape for all three passes: 0 = UBO, 1 = emitters, 3/4 = grid table/data, 5/6 = per-pass IO.
    const auto makeUpdates = [&](const Buffer& io5, const Buffer& io6) {
        return std::array<DescriptorSetUpdateInfo, 6>{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridTableBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_gridDataBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(io5) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(io6) } },
        };
    };

    if (m_useGrid)
    {
        // Clear the table (header counters 0, tableSize, entries EMPTY) + the cell data.
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 0, 8, 0);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 8, 4, m_tableEntries);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 12, 4, 0);
        vkCb.fillBuffer(m_gridTableBuffers[frameIdx].getBuffer(), 16, vk::WholeSize, 0xFFFFFFFF);
        vkCb.fillBuffer(m_gridDataBuffers[frameIdx].getBuffer(), 0, vk::WholeSize, 0);
        {
            vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            };
            vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
        }
        vk::DescriptorSet gridSet = m_gridSets[frameIdx].getDescriptorSet();
        auto gridUpdates = makeUpdates(m_forceReadbackBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]); // 5/6 unused by the shader
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_gridPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_gridPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, gridSet, gridUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_gridPipeline.getPipelineLayout(), 0, 1, &gridSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), GRID_DISPATCH_OFFSET * sizeof(uint32));
        {
            vk::MemoryBarrier2 barrier{ // grid write -> force/query gather
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            };
            vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
        }
    }

    {
        vk::DescriptorSet forceSet = m_emitterForceSets[frameIdx].getDescriptorSet();
        auto forceUpdates = makeUpdates(m_forceReadbackBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]);
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_emitterForcePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_emitterForcePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, forceSet, forceUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_emitterForcePipeline.getPipelineLayout(), 0, 1, &forceSet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), EMITTER_DISPATCH_OFFSET * sizeof(uint32));
    }
    {
        vk::DescriptorSet querySet = m_querySets[frameIdx].getDescriptorSet();
        auto queryUpdates = makeUpdates(m_queryBuffers[frameIdx], m_queryReadbackBuffers[frameIdx]);
        vkCb.bindPipeline(vk::PipelineBindPoint::eCompute, m_queryPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_queryPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, querySet, queryUpdates);
        vkCb.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_queryPipeline.getPipelineLayout(), 0, 1, &querySet, 0, nullptr);
        vkCb.dispatchIndirect(m_indirectBuffers[frameIdx].getBuffer(), QUERY_DISPATCH_OFFSET * sizeof(uint32));
    }

    { // grid reads for the shell FS in the scene pass + readback visibility for the CPU
        std::array<vk::MemoryBarrier2, 2> barriers{
            vk::MemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            },
            vk::MemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            },
        };
        vkCb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = (uint32)barriers.size(), .pMemoryBarriers = barriers.data() });
    }
}

ForceFieldPipeline::GridDemand ForceFieldPipeline::getGridDemand(uint32 frameIdx)
{
    std::span<GridDemand> span = m_gridTableBuffers[frameIdx].mapMemory<GridDemand>(0, sizeof(GridDemand));
    const GridDemand demand = *span.data();
    m_gridTableBuffers[frameIdx].unmapMemory();
    return demand;
}

void ForceFieldPipeline::growGridBuffers(size_t neededDataBytes, uint32 neededTableEntries)
{
    while (m_gridDataSize < neededDataBytes)
        m_gridDataSize *= 2;
    while (m_tableEntries < neededTableEntries)
        m_tableEntries *= 2; // stays a power of 2 for the hash
    createGridBuffers(); // per-frame GPU scratch, rebuilt every frame: nothing to preserve
    printf("ForceFieldPipeline: grew grid buffers to %zu bytes / %u table entries\n", m_gridDataSize, m_tableEntries);
}

void ForceFieldPipeline::recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 viewIndex = eyeToViewIndex(eye, m_viewCount);
    DescriptorSet& set = m_drawSets[drawSlot(frameIdx, eye)];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    std::array<DescriptorSetUpdateInfo, 5> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_emitterBuffers[frameIdx].getBuffer(), .range = m_emitterBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridTableBuffers[frameIdx].getBuffer(), .range = m_gridTableBuffers[frameIdx].getSize() } } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = m_gridDataBuffers[frameIdx].getBuffer(), .range = m_gridDataBuffers[frameIdx].getSize() } } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_indirectBuffers[frameIdx].getBuffer(), 0, 1, sizeof(vk::DrawIndirectCommand));
}
