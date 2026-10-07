module RendererVK;

import Core;
import File;
import :Device;
import :CommandBuffer;
import :Layout;
import :TextureManager;
import :Texture;

namespace
{
    using namespace RendererVKLayout;

    // VkDrawIndexedIndirectCommand
    constexpr uint32 CLUTTER_COMMAND_SIZE = 5 * sizeof(uint32);

    DescriptorSetUpdateInfo storage(uint32 binding, const Buffer& buffer)
    {
        return DescriptorSetUpdateInfo{ .binding = binding, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = buffer.getBuffer(), .range = buffer.getSize() } } };
    }

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
}

void ClutterPipeline::buildLayouts(ComputePipelineLayout& cull, ComputePipelineLayout& prefix, ComputePipelineLayout& scatter)
{
    constexpr vk::ShaderStageFlags CS = vk::ShaderStageFlagBits::eCompute;
    const auto add = [](ComputePipelineLayout& layout, uint32 binding, vk::DescriptorType type) {
        layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = CS });
    };
    cull.computeShaderDebugFilePath = "Shaders/Clutter/clutter_cull.cs.glsl";
    cull.computeShaderText = FileSystem::readFileStr(cull.computeShaderDebugFilePath);
    add(cull, 0, vk::DescriptorType::eUniformBuffer);
    add(cull, 1, vk::DescriptorType::eCombinedImageSampler);
    for (uint32 binding = 2; binding <= 10; ++binding) // ground table, vertices, frame, types, meshes, flat, keys, counts, bucket counts
        add(cull, binding, vk::DescriptorType::eStorageBuffer);
    // The bindless texture array LAST (binding 11 = the highest: variable count) - the splat height maps, for the
    // tessellated relief under each object (clutterRelief).
    cull.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{ .binding = 11, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = m_maxTextures, .stageFlags = CS });
    // Binding 1 (the terrain-data cascades) is a ping-pong pair rewritten per frame by updateTerrainDescriptor.
    cull.descriptorBindingFlags.resize(cull.descriptorSetLayoutBindings.size());
    cull.descriptorBindingFlags[1] = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    cull.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount
        | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    prefix.computeShaderDebugFilePath = "Shaders/Clutter/clutter_prefix.cs.glsl";
    prefix.computeShaderText = FileSystem::readFileStr(prefix.computeShaderDebugFilePath);
    for (uint32 binding = 0; binding <= 5; ++binding) // frame, meshes, counts, bucket counts, bucket bases, commands
        add(prefix, binding, vk::DescriptorType::eStorageBuffer);

    scatter.computeShaderDebugFilePath = "Shaders/Clutter/clutter_scatter.cs.glsl";
    scatter.computeShaderText = FileSystem::readFileStr(scatter.computeShaderDebugFilePath);
    for (uint32 binding = 0; binding <= 4; ++binding) // counts, keys, flat, bucket bases, sorted
        add(scatter, binding, vk::DescriptorType::eStorageBuffer);
}

void ClutterPipeline::initialize(uint32 maxTextures, uint32 numTextureDescriptors)
{
    m_maxTextures = maxTextures;
    m_textureSampler.initialize();
    ComputePipelineLayout cull, prefix, scatter;
    buildLayouts(cull, prefix, scatter);
    m_cullPipeline.initialize(cull);
    m_prefixPipeline.initialize(prefix);
    m_scatterPipeline.initialize(scatter);
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_cullSets[i].initialize(m_cullPipeline.getDescriptorSetLayout(), "ClutterCull", numTextureDescriptors);
        m_prefixSets[i].initialize(m_prefixPipeline.getDescriptorSetLayout(), "ClutterPrefix");
        m_scatterSets[i].initialize(m_scatterPipeline.getDescriptorSetLayout(), "ClutterScatter");
        m_frames[i].initialize(sizeof(ClutterFrameGpu), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "ClutterFrame", BufferHostAccess::eSequentialWrite);
        m_mappedFrames[i] = (ClutterFrameGpu*)m_frames[i].mapMemory().data();
        memset(m_mappedFrames[i], 0, sizeof(ClutterFrameGpu)); // gridDim 0: no clutter; floorDim 0: no map
        m_frames[i].flushMappedMemory(sizeof(ClutterFrameGpu));
        m_flat[i].initialize(CLUTTER_MAX_INSTANCES * sizeof(ClutterInstanceGpu), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterFlat");
        m_keys[i].initialize(CLUTTER_MAX_INSTANCES * sizeof(uint32), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterKeys");
        m_sorted[i].initialize(CLUTTER_MAX_INSTANCES * sizeof(ClutterInstanceGpu),
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterInstances");
        m_counts[i].initialize(16, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eIndirectBuffer
            | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterCounts");
        m_bucketCounts[i].initialize(CLUTTER_MAX_BUCKETS * sizeof(uint32), vk::BufferUsageFlagBits2::eStorageBuffer
            | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterBucketCounts");
        m_bucketBase[i].initialize(CLUTTER_MAX_BUCKETS * sizeof(uint32), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterBucketBase");
        m_commands[i].initialize(CLUTTER_MAX_BUCKETS * CLUTTER_COMMAND_SIZE, vk::BufferUsageFlagBits2::eStorageBuffer
            | vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterCommands");
        const uint32 zero[4] = {};
        m_counts[i].upload(sizeof(zero), zero);
        const oc::vector<uint32> noCommands(CLUTTER_MAX_BUCKETS * 5, 0u);
        m_commands[i].upload(noCommands.size() * sizeof(uint32), noCommands.data());
    }
    m_types.initialize(CLUTTER_MAX_TYPES * sizeof(ClutterTypeGpu), vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterTypes");
    m_meshes.initialize(CLUTTER_MAX_MESHES * sizeof(ClutterMeshGpu), vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterMeshes");
    setAssets({}, {}, {}, {});
    buildFlowerIndices();
}

// The near grass cascade's casters: the rigid / flower VS with CLUTTER_NEAR_SHADOW (the cascade's matrix, a single-view
// pass), depth only, the shadow pass's depth state, both faces. Its set: the UBO, and the vertex mega-buffer at 14 (the
// flower VS includes grass.inc.glsl, which reads the ground mesh through it).
void ClutterPipeline::buildNearShadowLayout(GraphicsPipelineLayout& layout, bool flowers)
{
    layout.vertexShader.debugFilePath = flowers ? "Shaders/Clutter/clutter_flower.vs.glsl" : "Shaders/Clutter/clutter.vs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.vertexShader.defines.push_back(ShaderDefine{ "CLUTTER_NEAR_SHADOW", "1" });
    layout.depthOnly = true;
    layout.depthCompareOp = vk::CompareOp::eLess;
    layout.depthBiasEnable = true;
    layout.depthBiasConstantFactor = 1.25f; // as ShadowMapGraphicsPipeline
    layout.depthBiasSlopeFactor = 2.5f;
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.indirectBindable = false;
    auto& bindings = layout.vertexLayoutInfo.bindingDescriptions;
    auto& attributes = layout.vertexLayoutInfo.attributeDescriptions;
    const uint32 instanceBinding = flowers ? 0 : 1;
    const uint32 firstInstanceLocation = flowers ? 0 : 2;
    if (!flowers)
    {
        bindings.push_back(vk::VertexInputBindingDescription{ .binding = 0, .stride = sizeof(ClutterVertexGpu), .inputRate = vk::VertexInputRate::eVertex });
        attributes.push_back(vk::VertexInputAttributeDescription{ .location = 0, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(ClutterVertexGpu, posAo) });
        attributes.push_back(vk::VertexInputAttributeDescription{ .location = 1, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(ClutterVertexGpu, normalPart) });
    }
    bindings.push_back(vk::VertexInputBindingDescription{ .binding = instanceBinding, .stride = sizeof(ClutterInstanceGpu), .inputRate = vk::VertexInputRate::eInstance });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = firstInstanceLocation, .binding = instanceBinding, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(ClutterInstanceGpu, posScale) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = firstInstanceLocation + 1, .binding = instanceBinding, .format = vk::Format::eR32G32B32A32Uint, .offset = offsetof(ClutterInstanceGpu, data) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = firstInstanceLocation + 2, .binding = instanceBinding, .format = vk::Format::eR32G32B32A32Uint, .offset = offsetof(ClutterInstanceGpu, look) });
    layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    if (flowers)
        layout.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = 14, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
}

void ClutterPipeline::initializeNearShadow(vk::RenderPass nearShadowRenderPass)
{
    m_nearShadowRenderPass = nearShadowRenderPass;
    GraphicsPipelineLayout rigid, flowers;
    buildNearShadowLayout(rigid, false);
    buildNearShadowLayout(flowers, true);
    m_nearShadowBuilt = m_nearShadowPipeline.initialize(m_nearShadowRenderPass, rigid);
    m_nearShadowFlowerBuilt = m_nearShadowFlowerPipeline.initialize(m_nearShadowRenderPass, flowers);
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_nearShadowSets[i].initialize(m_nearShadowPipeline.getDescriptorSetLayout(), "ClutterNearShadow");
        m_nearShadowFlowerSets[i].initialize(m_nearShadowFlowerPipeline.getDescriptorSetLayout(), "ClutterNearShadowFlowers");
    }
}

void ClutterPipeline::reloadShaders()
{
    ComputePipelineLayout cull, prefix, scatter;
    buildLayouts(cull, prefix, scatter);
    if (!m_cullPipeline.reloadShaders(cull))
        printf("ClutterPipeline: cull shader reload failed, keeping previous pipeline\n");
    if (!m_prefixPipeline.reloadShaders(prefix))
        printf("ClutterPipeline: prefix shader reload failed, keeping previous pipeline\n");
    if (!m_scatterPipeline.reloadShaders(scatter))
        printf("ClutterPipeline: scatter shader reload failed, keeping previous pipeline\n");
    if (!m_nearShadowRenderPass)
        return;
    const auto reloadNear = [this](GraphicsPipeline& pipeline, bool& built, bool flowers) {
        GraphicsPipelineLayout layout;
        buildNearShadowLayout(layout, flowers);
        if (!built)
            built = pipeline.initialize(m_nearShadowRenderPass, layout);
        else if (!pipeline.reloadShaders(m_nearShadowRenderPass, layout))
            printf("ClutterPipeline: near shadow shader reload failed, keeping previous pipeline\n");
    };
    reloadNear(m_nearShadowPipeline, m_nearShadowBuilt, false);
    reloadNear(m_nearShadowFlowerPipeline, m_nearShadowFlowerBuilt, true);
}

void ClutterPipeline::setAssets(oc::span<const ClutterTypeGpu> types, oc::span<const ClutterMeshGpu> meshes,
    oc::span<const ClutterVertexGpu> vertices, oc::span<const uint32> indices)
{
    m_numTypes = (uint32)oc::min(types.size(), (size_t)CLUTTER_MAX_TYPES);
    m_numMeshes = (uint32)oc::min(meshes.size(), (size_t)CLUTTER_MAX_MESHES);
    if (m_numTypes > 0)
        m_types.upload(m_numTypes * sizeof(ClutterTypeGpu), types.data());
    if (m_numMeshes > 0)
        m_meshes.upload(m_numMeshes * sizeof(ClutterMeshGpu), meshes.data());
    // At least one element each: the draws bind them whatever they hold.
    m_vertices.destroy();
    m_indices.destroy();
    m_vertices.initialize(oc::max(vertices.size(), (size_t)1) * sizeof(ClutterVertexGpu),
        vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterVertices");
    m_indices.initialize(oc::max(indices.size(), (size_t)1) * sizeof(uint32),
        vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterIndices");
    if (!vertices.empty())
        m_vertices.upload(vertices.size_bytes(), vertices.data());
    if (!indices.empty())
        m_indices.upload(indices.size_bytes(), indices.data());
}

// The flower topology, one flower per LOD: the stem's strip (2 vertices per row, S segments), the petal slots (a quad of
// 4 vertices each), the centre quad. The VS builds every vertex from its id (clutter_flower.vs.glsl flowerVertex).
void ClutterPipeline::buildFlowerIndices()
{
    oc::vector<uint32> indices;
    for (uint32 lod = 0; lod < CLUTTER_FLOWER_LODS; ++lod)
    {
        const uint32 first = (uint32)indices.size();
        const uint32 segments = CLUTTER_FLOWER_STEM_SEGMENTS[lod];
        for (uint32 row = 0; row < segments; ++row)
        {
            const uint32 l0 = 2 * row, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
            indices.insert(indices.end(), { l0, r0, l1, r0, r1, l1 });
        }
        const uint32 stemVerts = 2 * (segments + 1);
        for (uint32 quad = 0; quad <= CLUTTER_FLOWER_PETALS[lod]; ++quad) // the petals, then the centre
        {
            const uint32 b = stemVerts + 4 * quad;
            indices.insert(indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
        }
        m_flowerLods[lod] = glm::uvec4(first, (uint32)indices.size() - first, 0u, 0u);
    }
    m_flowerIndices.destroy();
    m_flowerIndices.initialize(indices.size() * sizeof(uint32), vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, false, "ClutterFlowerIndices");
    m_flowerIndices.upload(indices.size() * sizeof(uint32), indices.data());
}

void ClutterPipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    // The GPU is idle (BindlessTextures' capacity change): the variable-count sets are re-allocated; record() writes
    // every slot again.
    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
        m_cullSets[i].initialize(m_cullPipeline.getDescriptorSetLayout(), "ClutterCull", numTextureDescriptors);
}

void ClutterPipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    // A streamed texture's slot rewrite (UPDATE_AFTER_BIND: the cached cull is not re-recorded).
    vk::DescriptorImageInfo imageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = m_cullSets[frameIdx].getDescriptorSet(), .dstBinding = 11, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void ClutterPipeline::updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = m_cullSets[frameIdx].getDescriptorSet(), .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void ClutterPipeline::flushFrame(uint32 frameIdx, bool withFloor)
{
    m_frames[frameIdx].flushMappedMemory(withFloor ? sizeof(ClutterFrameGpu) : offsetof(ClutterFrameGpu, floor));
}

ClutterPipeline::Draw ClutterPipeline::getDraw(uint32 frameIdx)
{
    return Draw{ .instances = &m_sorted[frameIdx], .commands = &m_commands[frameIdx], .counts = &m_counts[frameIdx],
        .vertices = &m_vertices, .indices = &m_indices, .flowerIndices = &m_flowerIndices, .rigid = m_numMeshes > 0 };
}

void ClutterPipeline::recordClear(vk::CommandBuffer primary, uint32 frameIdx)
{
    // The slot's last draws read these (its fence was waited): no rigid draws, empty flower draws.
    primary.fillBuffer(m_counts[frameIdx].getBuffer(), 0, 4 * sizeof(uint32), 0u);
    primary.fillBuffer(m_commands[frameIdx].getBuffer(), 0, CLUTTER_FLOWER_LODS * CLUTTER_COMMAND_SIZE, 0u);
    const vk::MemoryBarrier2 toDraw{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
    };
    primary.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toDraw });
}

void ClutterPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    // The counts restart at 0; the slot's last draws (which read these buffers) retired with its fence.
    cmd.fillBuffer(m_counts[frameIdx].getBuffer(), 0, 4 * sizeof(uint32), 0u);
    cmd.fillBuffer(m_bucketCounts[frameIdx].getBuffer(), 0, CLUTTER_MAX_BUCKETS * sizeof(uint32), 0u);
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    // 1. THE CULL: a workgroup per patch slot (the ones past the grid return at once).
    {
        oc::vector<DescriptorSetUpdateInfo> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo->getBuffer(), .range = UBO_RANGE } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
            storage(2, *params.groundTable),
            storage(3, *params.vertexBuffer),
            storage(4, m_frames[frameIdx]),
            storage(5, m_types),
            storage(6, m_meshes),
            storage(7, m_flat[frameIdx]),
            storage(8, m_keys[frameIdx]),
            storage(9, m_counts[frameIdx]),
            storage(10, m_bucketCounts[frameIdx]),
        };
        // Every live texture slot (the splat height maps among them); streamed swaps arrive by updateTextureDescriptor.
        DescriptorSetUpdateInfo textures{ .binding = 11, .type = vk::DescriptorType::eCombinedImageSampler };
        for (uint16 texIdx = 0; texIdx < (uint16)Globals::textureManager.getNumTextures(); ++texIdx)
            textures.imageInfos.push_back(vk::DescriptorImageInfo{ .sampler = m_textureSampler.getSampler(),
                .imageView = Globals::textureManager.getViewForDescriptor(texIdx), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
        if (!textures.imageInfos.empty())
            updates.push_back(oc::move(textures));
        vk::DescriptorSet set = m_cullSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_cullPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_cullPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_cullPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.dispatch(CLUTTER_MAX_PATCHES, 1, 1);
    }
    computeBarrier(cmd);

    // 2. THE BUCKETS' RANGES and draws: one workgroup.
    {
        oc::array<DescriptorSetUpdateInfo, 6> updates{
            storage(0, m_frames[frameIdx]),
            storage(1, m_meshes),
            storage(2, m_counts[frameIdx]),
            storage(3, m_bucketCounts[frameIdx]),
            storage(4, m_bucketBase[frameIdx]),
            storage(5, m_commands[frameIdx]),
        };
        vk::DescriptorSet set = m_prefixSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_prefixPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_prefixPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_prefixPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.dispatch(1, 1, 1);
    }
    computeBarrier(cmd);

    // 3. THE RECORDS INTO THEIR BUCKETS: a thread per record slot (past the kept count they return).
    {
        oc::array<DescriptorSetUpdateInfo, 5> updates{
            storage(0, m_counts[frameIdx]),
            storage(1, m_keys[frameIdx]),
            storage(2, m_flat[frameIdx]),
            storage(3, m_bucketBase[frameIdx]),
            storage(4, m_sorted[frameIdx]),
        };
        vk::DescriptorSet set = m_scatterSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_scatterPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_scatterPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_scatterPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.dispatch(CLUTTER_MAX_INSTANCES / CLUTTER_SCATTER_GROUP, 1, 1);
    }

    // To the scene pass and the near cascade: the draws (indirect) and the sorted records (instance-rate attributes).
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexAttributeInput,
        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eVertexAttributeRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &after });
}

void ClutterPipeline::recordNearShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, Buffer& vertexBuffer)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const DescriptorSetUpdateInfo uboUpdate{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
        .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = UBO_RANGE } } };
    if (m_nearShadowBuilt && m_numMeshes > 0)
    {
        oc::array<DescriptorSetUpdateInfo, 1> updates{ uboUpdate };
        vk::DescriptorSet set = m_nearShadowSets[frameIdx].getDescriptorSet();
        commandBuffer.cmdUpdateDescriptorSets(m_nearShadowPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, set, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_nearShadowPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_nearShadowPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.bindVertexBuffers(0, { m_vertices.getBuffer(), m_sorted[frameIdx].getBuffer() }, { 0, 0 });
        cmd.bindIndexBuffer(m_indices.getBuffer(), 0, vk::IndexType::eUint32);
        cmd.drawIndexedIndirectCount(m_commands[frameIdx].getBuffer(), CLUTTER_FLOWER_LODS * CLUTTER_COMMAND_SIZE,
            m_counts[frameIdx].getBuffer(), sizeof(uint32), CLUTTER_MAX_BUCKETS - CLUTTER_FLOWER_LODS, CLUTTER_COMMAND_SIZE);
    }
    if (m_nearShadowFlowerBuilt)
    {
        oc::array<DescriptorSetUpdateInfo, 2> updates{ uboUpdate, storage(14, vertexBuffer) };
        vk::DescriptorSet set = m_nearShadowFlowerSets[frameIdx].getDescriptorSet();
        commandBuffer.cmdUpdateDescriptorSets(m_nearShadowFlowerPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, set, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_nearShadowFlowerPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_nearShadowFlowerPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.bindVertexBuffers(0, { m_sorted[frameIdx].getBuffer() }, { 0 });
        cmd.bindIndexBuffer(m_flowerIndices.getBuffer(), 0, vk::IndexType::eUint32);
        cmd.drawIndexedIndirect(m_commands[frameIdx].getBuffer(), 0, CLUTTER_FLOWER_LODS, CLUTTER_COMMAND_SIZE);
    }
}
