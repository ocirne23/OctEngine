module RendererVK;

import Core;
import Core.glm;
import File;
import :Device;
import :Layout;

SkinningComputePipeline::SkinningComputePipeline() {}
SkinningComputePipeline::~SkinningComputePipeline() {}

void SkinningComputePipeline::initialize(uint32 maxPaletteEntries, uint32 maxJobs)
{
    resizePaletteBuffer(maxPaletteEntries);
    resizeJobBuffer(maxJobs);

    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.dispatchArgsBuffer.initialize(sizeof(vk::DispatchIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "SkinningDispatchArgs", BufferHostAccess::eSequentialWrite);
        perFrame.mappedDispatchArgs = perFrame.dispatchArgsBuffer.mapMemory<vk::DispatchIndirectCommand>();
        perFrame.mappedDispatchArgs[0] = vk::DispatchIndirectCommand{ .x = 0, .y = 0, .z = 1 };
        perFrame.dispatchArgsBuffer.flushMappedMemory(sizeof(vk::DispatchIndirectCommand));
    }

    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    m_computePipeline.initialize(computePipelineLayout);
}

void SkinningComputePipeline::resizePaletteBuffer(uint32 maxPaletteEntries)
{
    m_maxPaletteEntries = maxPaletteEntries;
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.paletteBuffer.initialize(maxPaletteEntries * sizeof(glm::mat4),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "SkinningPalettes", BufferHostAccess::eSequentialWrite);
        perFrame.mappedPalettes = perFrame.paletteBuffer.mapMemory<glm::mat4>();
    }
}

void SkinningComputePipeline::resizeJobBuffer(uint32 maxJobs)
{
    m_maxJobs = maxJobs;
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.jobBuffer.initialize(RendererVKLayout::SKINNING_JOB_HEADER_BYTES + maxJobs * sizeof(RendererVKLayout::SkinningJob),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "SkinningJobs", BufferHostAccess::eSequentialWrite);
        const oc::span<uint8> bytes = perFrame.jobBuffer.mapMemory();
        perFrame.mappedJobCount = reinterpret_cast<uint32*>(bytes.data());
        perFrame.mappedJobs = oc::span<RendererVKLayout::SkinningJob>(
            reinterpret_cast<RendererVKLayout::SkinningJob*>(bytes.data() + RendererVKLayout::SKINNING_JOB_HEADER_BYTES), maxJobs);
        *perFrame.mappedJobCount = 0; // a frame recorded before its first upload skins nothing
        perFrame.jobBuffer.flushMappedMemory(RendererVKLayout::SKINNING_JOB_HEADER_BYTES);
    }
}

void SkinningComputePipeline::reloadShaders()
{
    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    if (!m_computePipeline.reloadShaders(computePipelineLayout))
        printf("SkinningComputePipeline: shader reload failed, keeping previous pipeline\n");
}

void SkinningComputePipeline::buildComputeLayout(ComputePipelineLayout& computePipelineLayout)
{
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/Mesh/skinning.cs.glsl";
    computePipelineLayout.computeShaderText = FileSystem::readFileStr(computePipelineLayout.computeShaderDebugFilePath);
    auto& bindings = computePipelineLayout.descriptorSetLayoutBindings;
    bindings.push_back(vk::DescriptorSetLayoutBinding{ // vertex buffer (read base, write output)
        .binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    bindings.push_back(vk::DescriptorSetLayoutBinding{ // skinning influences
        .binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    bindings.push_back(vk::DescriptorSetLayoutBinding{ // bone palette
        .binding = 2, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    bindings.push_back(vk::DescriptorSetLayoutBinding{ // skinning jobs
        .binding = 3, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void SkinningComputePipeline::finishUpload(uint32 frameIdx, uint32 paletteEnd, uint32 numJobs, uint32 numGroups)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    assert(paletteEnd <= frameData.mappedPalettes.size() && numJobs <= frameData.mappedJobs.size());
    if (paletteEnd > 0)
        frameData.paletteBuffer.flushMappedMemory(paletteEnd * sizeof(glm::mat4));
    *frameData.mappedJobCount = numJobs;
    frameData.jobBuffer.flushMappedMemory(RendererVKLayout::SKINNING_JOB_HEADER_BYTES + numJobs * sizeof(RendererVKLayout::SkinningJob));

    // Every job's own groups, packed: x first, then rows of y (an x past the device's guaranteed 65535 is not allowed).
    constexpr uint32 MAX_GROUPS_X = 32768;
    const uint32 x = oc::min(numGroups, MAX_GROUPS_X);
    const uint32 y = x == 0 ? 0 : (numGroups + x - 1) / x;
    frameData.mappedDispatchArgs[0] = vk::DispatchIndirectCommand{ .x = x, .y = y, .z = 1 };
    frameData.dispatchArgsBuffer.flushMappedMemory(sizeof(vk::DispatchIndirectCommand));
}

void SkinningComputePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];

    oc::array<DescriptorSetUpdateInfo, 4> updates
    {
        DescriptorSetUpdateInfo {
            .binding = 0, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.vertexBuffer.getBuffer(), .range = params.vertexBuffer.getSize() } } },
        DescriptorSetUpdateInfo {
            .binding = 1, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.skinningBuffer.getBuffer(), .range = params.skinningBuffer.getSize() } } },
        DescriptorSetUpdateInfo {
            .binding = 2, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = frameData.paletteBuffer.getBuffer(), .range = frameData.paletteBuffer.getSize() } } },
        DescriptorSetUpdateInfo {
            .binding = 3, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = frameData.jobBuffer.getBuffer(), .range = frameData.jobBuffer.getSize() } } },
    };

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipeline());
    commandBuffer.cmdUpdateDescriptorSets(m_computePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, descriptorSet, updates);
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    vkCommandBuffer.dispatchIndirect(frameData.dispatchArgsBuffer.getBuffer(), 0);

    // Skinned vertices are consumed as vertex input by the forward / shadow passes; last frame's positions
    // as a storage read by the scene vertex shader (the motion vectors).
    vk::MemoryBarrier2 memoryBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput | vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eShaderStorageRead,
    };
    vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
}
