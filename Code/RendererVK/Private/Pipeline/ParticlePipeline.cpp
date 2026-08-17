module RendererVK;

import Core;
import Core.glm;
import File;
import :ParticlePipeline;
import :ComputePipeline;
import :GraphicsPipeline;
import :TextureManager;
import :Device;
import :Sampler;
import :Layout;

using namespace RendererVKLayout;

// GPU counters block, see particle.inc.glsl: [0..15] sim dispatch args, [16..31] parity-0 draw args,
// [32..47] parity-1 draw args (instanceCount = alive count), [48..63] dead-stack top + padding.
static constexpr vk::DeviceSize COUNTERS_SIZE = 64;
static constexpr vk::DeviceSize drawArgsOffset(uint32 parity) { return 16 + parity * 16; }

static vk::DescriptorBufferInfo bufInfo(const Buffer& buffer)
{
    return vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() };
}
static vk::DescriptorImageInfo sampledRO(vk::Sampler sampler, vk::ImageView view)
{
    return vk::DescriptorImageInfo{ .sampler = sampler, .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
}

void ParticlePipeline::buildBeginLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/particle_begin.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void ParticlePipeline::buildEmitLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/particle_emit.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    for (uint32 binding = 0; binding <= 6; ++binding)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding,
            .descriptorType = binding == 0 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void ParticlePipeline::buildSimLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/particle_sim.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 binding = 2; binding <= 7; ++binding)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 8, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 9, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void ParticlePipeline::buildDrawLayout(GraphicsPipelineLayout& layout, uint32 maxTextures)
{
    layout.vertexShader.debugFilePath = "Shaders/particle.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/particle.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    // Premultiplied over the lit scene: out = src.rgb + dst * (1 - src.a). src.a already carries the
    // per-emitter additivity, so one pipeline covers alpha-blended smoke through additive fire.
    layout.blendEnable = true;
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    layout.depthTestEnable = true;   // occluded by opaque scene depth
    layout.depthWriteEnable = false; // transparent: never writes depth

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    // 20 = the set's highest binding number: required for eVariableDescriptorCount.
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 20, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound
        | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void ParticlePipeline::initialize(vk::RenderPass sceneRenderPass, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount)
{
    m_viewCount = viewCount;
    m_textureSampler.initialize();

    { ComputePipelineLayout layout; buildBeginLayout(layout); m_beginPipeline.initialize(layout); }
    { ComputePipelineLayout layout; buildEmitLayout(layout);  m_emitPipeline.initialize(layout); }
    { ComputePipelineLayout layout; buildSimLayout(layout);   m_simPipeline.initialize(layout); }
    { GraphicsPipelineLayout layout; buildDrawLayout(layout, maxTextures); m_drawPipeline.initialize(sceneRenderPass, layout); }

    m_poolBuffer.initialize(MAX_PARTICLES * 48ull, vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ParticlePool");
    for (uint32 i = 0; i < 2; ++i)
        m_aliveBuffers[i].initialize(MAX_PARTICLES * sizeof(uint32), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ParticleAliveList");
    m_deadListBuffer.initialize(MAX_PARTICLES * sizeof(uint32), vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ParticleDeadList");
    m_countersBuffer.initialize(COUNTERS_SIZE,
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ParticleCounters");

    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_emitterBuffers[i].initialize(MAX_PARTICLE_EMITTERS * sizeof(ParticleEmitterGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleEmitters", BufferHostAccess::eSequentialWrite);
        m_mappedEmitters[i] = m_emitterBuffers[i].mapMemory<ParticleEmitterGpu>();

        m_spawnBuffers[i].initialize(MAX_PARTICLE_SPAWNS_PER_FRAME * sizeof(uint32),
            vk::BufferUsageFlagBits2::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleSpawnMap", BufferHostAccess::eSequentialWrite);
        m_mappedSpawns[i] = m_spawnBuffers[i].mapMemory<uint32>();

        m_paramsBuffers[i].initialize(sizeof(FrameParams),
            vk::BufferUsageFlagBits2::eUniformBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleParams", BufferHostAccess::eSequentialWrite);
        m_mappedParams[i] = m_paramsBuffers[i].mapMemory<FrameParams>();

        m_beginDispatchBuffers[i].initialize(sizeof(vk::DispatchIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleBeginDispatch", BufferHostAccess::eSequentialWrite);
        m_mappedBeginDispatch[i] = m_beginDispatchBuffers[i].mapMemory<uint32>();

        m_emitDispatchBuffers[i].initialize(sizeof(vk::DispatchIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleEmitDispatch", BufferHostAccess::eSequentialWrite);
        m_mappedEmitDispatch[i] = m_emitDispatchBuffers[i].mapMemory<uint32>();

        m_readbackBuffers[i].initialize(COUNTERS_SIZE, vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ParticleCountersReadback");
        m_mappedReadback[i] = m_readbackBuffers[i].mapMemory<uint32>();
        memset(m_mappedReadback[i].data(), 0, COUNTERS_SIZE);

        m_beginSets[i].initialize(m_beginPipeline.getDescriptorSetLayout());
        m_emitSets[i].initialize(m_emitPipeline.getDescriptorSetLayout());
        m_simSets[i].initialize(m_simPipeline.getDescriptorSetLayout());
        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_drawSets[drawSlot(i, eye)].initialize(m_drawPipeline.getDescriptorSetLayout(), numTextureDescriptors);
    }
}

void ParticlePipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    ComputePipelineLayout beginLayout; buildBeginLayout(beginLayout);
    ComputePipelineLayout emitLayout;  buildEmitLayout(emitLayout);
    ComputePipelineLayout simLayout;   buildSimLayout(simLayout);
    GraphicsPipelineLayout drawLayout; buildDrawLayout(drawLayout, Globals::textureManager.getDescriptorCap());
    bool ok = m_beginPipeline.reloadShaders(beginLayout);
    ok = m_emitPipeline.reloadShaders(emitLayout) && ok;
    ok = m_simPipeline.reloadShaders(simLayout) && ok;
    ok = m_drawPipeline.reloadShaders(sceneRenderPass, drawLayout) && ok;
    if (!ok)
        printf("ParticlePipeline: shader reload failed, keeping previous pipeline(s)\n");
}

void ParticlePipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
        for (uint32 eye = 0; eye < m_viewCount; ++eye)
            m_drawSets[drawSlot(i, eye)].initialize(m_drawPipeline.getDescriptorSetLayout(), numTextureDescriptors);
}

void ParticlePipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    const vk::DescriptorImageInfo imageInfo{
        .sampler = m_textureSampler.getSampler(),
        .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    oc::array<vk::WriteDescriptorSet, MAX_VIEWS> writes;
    for (uint32 eye = 0; eye < m_viewCount; ++eye)
        writes[eye] = vk::WriteDescriptorSet{ .dstSet = m_drawSets[drawSlot(frameIdx, eye)].getDescriptorSet(),
            .dstBinding = 20, .dstArrayElement = slotIdx, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(m_viewCount, writes.data(), 0, nullptr);
}

void ParticlePipeline::update(uint32 frameIdx, oc::span<const ParticleEmitterGpu> emitters,
    oc::span<const oc::pair<uint16, uint16>> spawnRequests, float dt, bool collision, bool reset)
{
    if (!emitters.empty())
    {
        memcpy(m_mappedEmitters[frameIdx].data(), emitters.data(), emitters.size_bytes());
        m_emitterBuffers[frameIdx].flushMappedMemory(emitters.size_bytes());
    }

    uint32 spawnCount = 0;
    oc::span<uint32> spawnMap = m_mappedSpawns[frameIdx];
    for (const auto& [emitterIdx, count] : spawnRequests)
    {
        const uint32 n = oc::min<uint32>(count, MAX_PARTICLE_SPAWNS_PER_FRAME - spawnCount);
        for (uint32 i = 0; i < n; ++i)
            spawnMap[spawnCount + i] = emitterIdx;
        spawnCount += n;
    }
    if (spawnCount > 0)
        m_spawnBuffers[frameIdx].flushMappedMemory(spawnCount * sizeof(uint32));

    FrameParams& params = m_mappedParams[frameIdx][0];
    params.dt = dt;
    params.spawnCount = spawnCount;
    params.parity = frameIdx & 1u;
    params.reset = reset ? 1u : 0u;
    params.frameIndex = m_frameCounter++;
    params.collision = collision ? 1u : 0u;
    m_paramsBuffers[frameIdx].flushMappedMemory(sizeof(FrameParams));

    oc::span<uint32> beginDispatch = m_mappedBeginDispatch[frameIdx];
    beginDispatch[0] = reset ? (MAX_PARTICLES + 255) / 256 : 1;
    beginDispatch[1] = 1;
    beginDispatch[2] = 1;
    m_beginDispatchBuffers[frameIdx].flushMappedMemory(sizeof(vk::DispatchIndirectCommand));

    oc::span<uint32> emitDispatch = m_mappedEmitDispatch[frameIdx];
    emitDispatch[0] = (spawnCount + 63) / 64;
    emitDispatch[1] = 1;
    emitDispatch[2] = 1;
    m_emitDispatchBuffers[frameIdx].flushMappedMemory(sizeof(vk::DispatchIndirectCommand));
}

void ParticlePipeline::recordSim(CommandBuffer& commandBuffer, uint32 frameIdx, const SimParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 parity = frameIdx & 1u;

    const auto fullBarrier = [&](vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
    {
        vk::MemoryBarrier2 barrier{ .srcStageMask = srcStage, .srcAccessMask = srcAccess, .dstStageMask = dstStage, .dstAccessMask = dstAccess };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    };

    // The pool/lists/counters are shared across frames in flight: order this frame's compute writes
    // after the previous submission's draw (vertex storage reads + indirect reads) and make its own
    // sim writes visible (queue-global: barriers order across command buffers on the same queue).
    fullBarrier(vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);

    { // begin: pool reset or per-frame prepare
        DescriptorSet& set = m_beginSets[frameIdx];
        oc::array<DescriptorSetUpdateInfo, 3> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { bufInfo(m_paramsBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_countersBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_deadListBuffer) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_beginPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set.getDescriptorSet(), updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_beginPipeline.getPipeline());
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_beginPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.dispatchIndirect(m_beginDispatchBuffers[frameIdx].getBuffer(), 0);
    }

    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);

    { // emit: one thread per spawn
        DescriptorSet& set = m_emitSets[frameIdx];
        oc::array<DescriptorSetUpdateInfo, 7> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { bufInfo(m_paramsBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_poolBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_aliveBuffers[parity]) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_deadListBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_countersBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_spawnBuffers[frameIdx]) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_emitPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set.getDescriptorSet(), updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_emitPipeline.getPipeline());
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_emitPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.dispatchIndirect(m_emitDispatchBuffers[frameIdx].getBuffer(), 0);
    }

    // emit wrote the IN count + pool; the sim dispatch args (begin) are consumed by dispatchIndirect.
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eIndirectCommandRead);

    { // sim: integrate + compact survivors into the OUT list
        DescriptorSet& set = m_simSets[frameIdx];
        oc::array<DescriptorSetUpdateInfo, 10> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { bufInfo(m_paramsBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_poolBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_aliveBuffers[parity]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_aliveBuffers[1 - parity]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_deadListBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_countersBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.prevDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.prevNormalView) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_simPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set.getDescriptorSet(), updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_simPipeline.getPipeline());
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_simPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.dispatchIndirect(m_countersBuffer.getBuffer(), 0); // c_simGroups at offset 0
    }

    // sim writes -> billboard draw: OUT alive list read in the vertex shader, alive count read as the
    // indirect draw's instanceCount.
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eDrawIndirect,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead);

    { // Counters snapshot for CPU stats/debug (read back ~NUM_FRAMES_IN_FLIGHT frames later).
        vk::MemoryBarrier2 toCopy{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toCopy });
        const vk::BufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = COUNTERS_SIZE };
        cmd.copyBuffer(m_countersBuffer.getBuffer(), m_readbackBuffers[frameIdx].getBuffer(), 1, &region);
    }
}

void ParticlePipeline::recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 parity = frameIdx & 1u;
    const uint32 viewIndex = eyeToViewIndex(eye, m_viewCount);
    DescriptorSet& set = m_drawSets[drawSlot(frameIdx, eye)];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    oc::array<DescriptorSetUpdateInfo, 7> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_poolBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_aliveBuffers[1 - parity]) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_emitterBuffers[frameIdx]) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.giGridDataBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 20, .type = vk::DescriptorType::eCombinedImageSampler },
    };
    const size_t numTextures = Globals::textureManager.getNumTextures();
    updates[6].imageInfos.reserve(numTextures);
    for (uint16 texIdx = 0; texIdx < (uint16)numTextures; ++texIdx)
        updates[6].imageInfos.push_back(vk::DescriptorImageInfo{
            .sampler = m_textureSampler.getSampler(),
            .imageView = Globals::textureManager.getViewForDescriptor(texIdx), // freed slots -> fallback
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });

    commandBuffer.cmdUpdateDescriptorSets(m_drawPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_drawPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_drawPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_drawPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.drawIndirect(m_countersBuffer.getBuffer(), drawArgsOffset(1 - parity), 1, sizeof(vk::DrawIndirectCommand));
}
