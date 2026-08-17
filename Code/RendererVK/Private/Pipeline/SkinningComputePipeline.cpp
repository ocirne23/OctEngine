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
        perFrame.jobBuffer.initialize(maxJobs * sizeof(RendererVKLayout::SkinningJob),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "SkinningJobs", BufferHostAccess::eSequentialWrite);
        perFrame.mappedJobs = perFrame.jobBuffer.mapMemory<RendererVKLayout::SkinningJob>();
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
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/skinning.cs.glsl";
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

void SkinningComputePipeline::update(uint32 frameIdx, oc::span<const glm::mat4> palettes, oc::span<const RendererVKLayout::SkinningJob> jobs)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    if (!palettes.empty())
    {
        assert(palettes.size() <= frameData.mappedPalettes.size());
        memcpy(frameData.mappedPalettes.data(), palettes.data(), palettes.size() * sizeof(glm::mat4));
        frameData.paletteBuffer.flushMappedMemory(palettes.size() * sizeof(glm::mat4));
    }

    uint32 maxGroups = 0;
    if (!jobs.empty())
    {
        assert(jobs.size() <= frameData.mappedJobs.size());
        memcpy(frameData.mappedJobs.data(), jobs.data(), jobs.size() * sizeof(RendererVKLayout::SkinningJob));
        frameData.jobBuffer.flushMappedMemory(jobs.size() * sizeof(RendererVKLayout::SkinningJob));
        for (const RendererVKLayout::SkinningJob& job : jobs)
            maxGroups = oc::max(maxGroups, (job.vertexCount + RendererVKLayout::SKINNING_THREADS_PER_GROUP - 1) / RendererVKLayout::SKINNING_THREADS_PER_GROUP);
    }
    frameData.mappedDispatchArgs[0] = vk::DispatchIndirectCommand{ .x = maxGroups, .y = (uint32)jobs.size(), .z = 1 };
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

    // Skinned vertices are consumed as vertex input by the G-buffer / forward / shadow passes.
    vk::MemoryBarrier2 memoryBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput | vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eShaderStorageRead,
    };
    vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
}
