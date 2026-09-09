module RendererVK;

import Core;
import Core.glm;
import Core.Tweaks;
import File;
import :Device;
import :TextureManager;
import :Texture;
import :GraphicsPipeline;
import :RenderPass;
import :Layout;

namespace
{
    struct TlasInstancePC
    {
        glm::vec3 viewPos;
        float maxRange; // instances whose origin is further out get TLAS mask 0
        uint32 numInstances;
    };
    struct TracePC
    {
        uint32 frameIndex;
        uint32 numRays;
        float temporalAlpha;
        float maxRayDist;
        glm::vec3 prevViewPos; float _pad0;
    };
    struct DebugPC
    {
        float  radius; // cube half-extent as a fraction of probe spacing
        uint32 mode;   // 0 = irradiance, 1 = cascade/LOD color
    };

    vk::DescriptorSetLayoutBinding storageBinding(uint32 b)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = b, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute };
    }
}

void GIProbePipeline::initialize(uint32 maxTlasInstances, uint32 maxTextures, uint32 numTextureDescriptors)
{
    m_textureSampler.initialize();

    resizeGrid();
    resizeTlasInstanceBuffers(maxTlasInstances);

    ComputePipelineLayout tlasLayout;  buildTlasInstanceLayout(tlasLayout);     m_tlasInstancePipeline.initialize(tlasLayout);
    ComputePipelineLayout traceLayout; buildTraceLayout(traceLayout, maxTextures); m_tracePipeline.initialize(traceLayout);

    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_tlasInstanceSets[i].initialize(m_tlasInstancePipeline.getDescriptorSetLayout());
        m_traceSets[i].initialize(m_tracePipeline.getDescriptorSetLayout(), numTextureDescriptors);
    }

    Tweak::intVar("GI", "Rays Per Probe", &m_giRaysPerProbe, 1, 128);
    Tweak::floatVar("GI", "Temporal Alpha", &m_giTemporalAlpha, 0.0f, 0.05f, 0.001f);
    Tweak::floatVar("GI", "Max Ray Distance", &m_giMaxRayDist, 0.0f, 128.0f);
    Tweak::floatVar("GI", "Strength", &m_giStrength, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("RT", "TLAS Range", &m_tlasRange, 16.0f, 8192.0f, 16.0f);
    Tweak::floatVar("GI", "Vis Variance Floor", &m_visVarianceFloor, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("GI", "Vis Cheb Power", &m_visChebPower, 1.0f, 6.0f, 0.1f);
    Tweak::floatVar("GI", "Vis Weight Floor", &m_visWeightFloor, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("GI", "Vis Mean Scale", &m_visMeanScale, 0.5f, 3.0f, 0.05f);
}

void GIProbePipeline::registerGridTweaks(const oc::function<void()>& onGridChanged)
{
    RendererVKLayout::GiGridConfig& grid = RendererVKLayout::g_giGrid;
    Tweak::intVar("GI", "Cascades", &grid.numCascades, 1, 8, 1.0f, onGridChanged);
    Tweak::intVar("GI", "Probes X (log2)", &grid.dimLog2X, 2, 6, 1.0f, onGridChanged);
    Tweak::intVar("GI", "Probes Y (log2)", &grid.dimLog2Y, 2, 6, 1.0f, onGridChanged);
    Tweak::intVar("GI", "Probes Z (log2)", &grid.dimLog2Z, 2, 6, 1.0f, onGridChanged);
    Tweak::floatVar("GI", "Focus Y offset (m)", &grid.focusOffsetY, -128.0f, 128.0f, 0.5f, onGridChanged);
}

void GIProbePipeline::resizeGrid()
{
    m_giGridData.initialize(RendererVKLayout::g_giGrid.gridDataBufferSize(),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal);
    doClear(); // fresh storage: the next GI frame zeroes it before the first trace
}

void GIProbePipeline::resizeTlasInstanceBuffers(uint32 maxTlasInstances)
{
    for (Buffer& instBuf : m_tlasInstanceBuffer)
        instBuf.initialize((vk::DeviceSize)maxTlasInstances * RendererVKLayout::GI_TLAS_INSTANCE_SIZE,
            vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal);
}

void GIProbePipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    // Variable-count texture binding: only the trace descriptor sets need re-allocating with the grown
    // count; the layout and pipeline declare the fixed device-limit cap and stay untouched.
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        m_traceSets[i].initialize(m_tracePipeline.getDescriptorSetLayout(), numTextureDescriptors);
}

void GIProbePipeline::reloadShaders(uint32 maxTextures)
{
    ComputePipelineLayout tlasLayout;  buildTlasInstanceLayout(tlasLayout);
    ComputePipelineLayout traceLayout; buildTraceLayout(traceLayout, maxTextures);
    bool ok = m_tlasInstancePipeline.reloadShaders(tlasLayout);
    ok = m_tracePipeline.reloadShaders(traceLayout) && ok;
    if (!ok)
        printf("GIProbePipeline: shader reload failed, keeping previous pipeline(s)\n");
}

void GIProbePipeline::buildTlasInstanceLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/gi_tlas_instances.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    for (uint32 b = 0; b <= 7; ++b)
        layout.descriptorSetLayoutBindings.push_back(storageBinding(b));
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TlasInstancePC) });
}

void GIProbePipeline::buildTraceLayout(ComputePipelineLayout& layout, uint32 maxTextures)
{
    layout.computeShaderDebugFilePath = "Shaders/gi_probe_trace.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // UBO
    b.push_back(storageBinding(1)); // lightInfos
    b.push_back(storageBinding(2)); // lightGrid
    b.push_back(storageBinding(3)); // lightTable
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // TLAS
    b.push_back(storageBinding(5)); // vertices
    b.push_back(storageBinding(6)); // indices
    b.push_back(storageBinding(7)); // meshInfos
    b.push_back(storageBinding(8)); // meshInstances
    b.push_back(storageBinding(9)); // materials
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 11, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // shadow map
    b.push_back(storageBinding(12)); // GI clipmap SH volume (read + write)
    // Texture array last (13 = the set's highest binding number, required for eVariableDescriptorCount):
    // the layout declares the fixed device-limit cap, the live size comes from the set allocation.
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 13, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // textures
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TracePC) });
}

void GIProbePipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    // Streamed texture slot rewrite. The trace set is refilled on every recordTrace anyway; this keeps the
    // set valid even on frames where the GI record early-outs (stale views must never dangle).
    vk::DescriptorImageInfo imageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = m_traceSets[frameIdx].getDescriptorSet(), .dstBinding = 13, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void GIProbePipeline::recordClearPersistent(CommandBuffer& commandBuffer)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // Zero the persistent clipmap SH so reads before the first trace are well-defined. The trace replaces
    // (alpha=1) every probe on its first visit anyway, so this is just initial-frame hygiene.
    cmd.fillBuffer(m_giGridData.getBuffer(), 0, vk::WholeSize, 0);

    vk::MemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
}

void GIProbePipeline::buildUpdateScratch()
{
    const auto buf = [](uint32 binding, vk::DescriptorType type = vk::DescriptorType::eStorageBuffer)
    {
        DescriptorSetUpdateInfo u{ .binding = binding, .type = type };
        u.bufferInfos.resize(1);
        return u;
    };
    for (uint32 b = 0; b < 8; ++b)
        m_tlasUpdates[b] = buf(b);

    m_traceUpdates.clear();
    m_traceUpdates.push_back(buf(0, vk::DescriptorType::eUniformBuffer)); // [0] UBO
    for (const uint32 b : { 1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 12u })           // [1..9] lightInfos, lightGrid, lightTable, vertices, indices, meshInfos, meshInstances, materialInfos, GI grid
        m_traceUpdates.push_back(buf(b));
    DescriptorSetUpdateInfo shadow{ .binding = 11, .type = vk::DescriptorType::eCombinedImageSampler }; // [10] shadow map
    shadow.imageInfos.resize(1);
    m_traceUpdates.push_back(oc::move(shadow));

    m_traceTexUpdate = DescriptorSetUpdateInfo{ .binding = 13, .type = vk::DescriptorType::eCombinedImageSampler };
    m_updateScratchBuilt = true;
}

void GIProbePipeline::recordTlasInstances(CommandBuffer& commandBuffer, uint32 frameIdx, TlasInstanceParams& params)
{
    if (params.numInstances == 0)
        return;
    if (!m_updateScratchBuilt)
        buildUpdateScratch();

    DescriptorSet& set = m_tlasInstanceSets[frameIdx];
    vk::DescriptorSet vkSet = set.getDescriptorSet();

    auto bufInfo = [](Buffer& buf) { return vk::DescriptorBufferInfo{ .buffer = buf.getBuffer(), .range = buf.getSize() }; };
    m_tlasUpdates[0].bufferInfos[0] = bufInfo(params.renderNodeTransforms);
    m_tlasUpdates[1].bufferInfos[0] = bufInfo(params.meshInstances);
    m_tlasUpdates[2].bufferInfos[0] = bufInfo(params.instanceOffsets);
    m_tlasUpdates[3].bufferInfos[0] = bufInfo(params.blasAddresses);
    m_tlasUpdates[4].bufferInfos[0] = bufInfo(m_tlasInstanceBuffer[frameIdx]);
    m_tlasUpdates[5].bufferInfos[0] = bufInfo(params.materialInfos);
    m_tlasUpdates[6].bufferInfos[0] = bufInfo(params.nodePassMasks);
    m_tlasUpdates[7].bufferInfos[0] = bufInfo(params.rtMeshAlias);
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_tlasInstancePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, m_tlasUpdates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_tlasInstancePipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_tlasInstancePipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    TlasInstancePC pc{ .viewPos = params.viewPos, .maxRange = m_tlasRange, .numInstances = params.numInstances };
    cmd.pushConstants(m_tlasInstancePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
    cmd.dispatch((params.numInstances + 63) / 64, 1, 1);
}

void GIProbePipeline::recordTrace(CommandBuffer& commandBuffer, uint32 frameIdx, TraceParams& params)
{
    if (!m_updateScratchBuilt)
        buildUpdateScratch();
    DescriptorSet& set = m_traceSets[frameIdx];
    vk::DescriptorSet vkSet = set.getDescriptorSet();
    auto bufInfo = [](Buffer& buf) { return vk::DescriptorBufferInfo{ .buffer = buf.getBuffer(), .range = buf.getSize() }; };

    // Patch this frame's handles into the persistent scratch (index map in buildUpdateScratch).
    m_traceUpdates[0].bufferInfos[0] = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };
    m_traceUpdates[1].bufferInfos[0] = bufInfo(params.lightInfos);
    m_traceUpdates[2].bufferInfos[0] = bufInfo(params.lightGrid);
    m_traceUpdates[3].bufferInfos[0] = bufInfo(params.lightTable);
    m_traceUpdates[4].bufferInfos[0] = bufInfo(params.vertexBuffer);
    m_traceUpdates[5].bufferInfos[0] = bufInfo(params.indexBuffer);
    m_traceUpdates[6].bufferInfos[0] = bufInfo(params.meshInfos);
    m_traceUpdates[7].bufferInfos[0] = bufInfo(params.meshInstances);
    m_traceUpdates[8].bufferInfos[0] = bufInfo(params.materialInfos);
    m_traceUpdates[9].bufferInfos[0] = bufInfo(m_giGridData);
    m_traceUpdates[10].imageInfos[0] = vk::DescriptorImageInfo{ .sampler = params.shadowMapSampler, .imageView = params.shadowMapView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };

    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_tracePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, m_traceUpdates);

    // The whole texture array, every frame (streamed slot changes ride the pending-write path too, but
    // this keeps the set complete). Its list keeps its capacity across frames; skipped when empty (a
    // zero-count write is invalid).
    m_traceTexUpdate.imageInfos.clear();
    for (uint16 texIdx = 0; texIdx < (uint16)Globals::textureManager.getNumTextures(); ++texIdx)
        m_traceTexUpdate.imageInfos.push_back(vk::DescriptorImageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = Globals::textureManager.getViewForDescriptor(texIdx), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
    if (!m_traceTexUpdate.imageInfos.empty())
        commandBuffer.cmdUpdateDescriptorSets(m_tracePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, oc::span<DescriptorSetUpdateInfo>(&m_traceTexUpdate, 1));

    // The acceleration-structure descriptor (binding 4) needs a pNext'd write the buffer/image helper
    // does not support; write it directly.
    vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &params.tlas };
    vk::WriteDescriptorSet asWrite{ .pNext = &asInfo, .dstSet = vkSet, .dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
    Globals::device.getDevice().updateDescriptorSets(1, &asWrite, 0, nullptr);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_tracePipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_tracePipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);

    // Trace tuning (passed via push constants, runtime-tweakable). Rays are amortized over frames via the
    // temporal blend.
    TracePC pc{
        .frameIndex = params.frameIndex,
        .numRays = (uint32)oc::max(m_giRaysPerProbe, 1),
        .temporalAlpha = m_giTemporalAlpha,
        .maxRayDist = m_giMaxRayDist,
        .prevViewPos = params.prevViewPos,
    };
    cmd.pushConstants(m_tracePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);

    cmd.dispatch((RendererVKLayout::g_giGrid.traceThreads() + 63) / 64, 1, 1);
}

void GIProbePipeline::buildDebugLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/gi_probe_debug.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/gi_probe_debug.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone; // procedural cube, winding not guaranteed
    if (m_debugDepthReadOnly) // depth-prepass reuse: read-only scene depth, no writes allowed
        layout.depthWriteEnable = false;

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex }); // UBO (mvp + viewPos)
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex }); // clipmap SH volume
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eVertex, .offset = 0, .size = sizeof(DebugPC) });
}

void GIProbePipeline::initializeDebug(vk::RenderPass renderPass)
{
    m_debugRenderPass = renderPass;
    GraphicsPipelineLayout layout; buildDebugLayout(layout);
    m_debugPipeline.initialize(renderPass, layout);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        m_debugSets[i].initialize(m_debugPipeline.getDescriptorSetLayout());
}

void GIProbePipeline::reloadDebugShaders(vk::RenderPass renderPass)
{
    // sceneColor's render pass is recreated on window resize, so refresh the cached handle rather than
    // reloading against the (possibly dangling) one captured at initializeDebug().
    m_debugRenderPass = renderPass;
    if (!m_debugRenderPass)
        return;
    GraphicsPipelineLayout layout; buildDebugLayout(layout);
    if (!m_debugPipeline.reloadShaders(m_debugRenderPass, layout))
        printf("GIProbePipeline: debug shader reload failed, keeping previous pipeline\n");
}

void GIProbePipeline::recordDebugDraw(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, float radius, uint32 mode)
{
    DescriptorSet& set = m_debugSets[frameIdx];
    vk::DescriptorSet vkSet = set.getDescriptorSet();
    auto bufInfo = [](Buffer& buf) { return vk::DescriptorBufferInfo{ .buffer = buf.getBuffer(), .range = buf.getSize() }; };

    oc::array<DescriptorSetUpdateInfo, 2> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(m_giGridData) } },
    };

    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    commandBuffer.cmdUpdateDescriptorSets(m_debugPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_debugPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_debugPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    DebugPC pc{ .radius = radius, .mode = mode };
    cmd.pushConstants(m_debugPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(pc), &pc);
    // One instanced cube (36 verts) per clipmap probe across all cascades.
    cmd.draw(36, RendererVKLayout::g_giGrid.probesTotal(), 0, 0);
}
