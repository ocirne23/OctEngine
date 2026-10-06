module RendererVK;

import Core;
import Core.Time;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :TreeVolumePipeline;

namespace
{
    constexpr vk::Format ACCUM_FORMAT = vk::Format::eR32Uint;
    constexpr vk::Format DENSITY_FORMAT = vk::Format::eR16Sfloat;
    constexpr vk::Format COLOUR_FORMAT = vk::Format::eR8G8B8A8Unorm;
    constexpr vk::Format OUT_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format FLOOR_BLOCK_FORMAT = vk::Format::eR32Sfloat;  // the block max
    constexpr vk::Format FLOOR_MAX_FORMAT = vk::Format::eR32G32Sfloat; // the grid: dilated, ahead
    constexpr uint32 FLOOR_MAX_BLOCK = 16; // tree_volume.inc.glsl's TV_FLOOR_MAX_BLOCK

    // tree_volume_floor_max.cs.glsl's push block.
    struct FloorMaxPC
    {
        glm::uvec2 srcSize;
        glm::uvec2 dstSize;
    };

    // Mirrors TreeVolumeParams (tree_volume.inc.glsl).
    struct VolumeParamsGpu
    {
        glm::vec2 centre;
        float rMin;
        float rMax;
        uint32 angularRes;
        uint32 radialRes;
        uint32 slices;
        float height;
        float densityScale;
        float pad0, pad1, pad2;
    };
    static_assert(sizeof(VolumeParamsGpu) == 48);
    // Mirror the push blocks of tree_volume_splat.cs.glsl / tree_volume_march.cs.glsl (scalar layout).
    struct SplatPC
    {
        vk::DeviceAddress pieces;  // TREE_SPLAT_RECORDS: the chunk map
        vk::DeviceAddress types;
        vk::DeviceAddress data;
        uint32 numPieces;    // TREE_SPLAT_RECORDS: the record chunks
        uint32 pad;          // TREE_SPLAT_RECORDS: the chunk map's size
        VolumeParamsGpu vol;
        // TREE_SPLAT_RECORDS only (numPieces: the records of the detail chunks, one workgroup each):
        vk::DeviceAddress records;
        vk::DeviceAddress detailChunks; // uvec4 per detail chunk: coord, its first record's pool word, its first workgroup
        vk::DeviceAddress recordTypes;
        float chunkSize;
        uint32 worldSeed;
        uint32 numDetailChunks;
        uint32 numRecordTypes;
        uint32 wgOffset;     // the first record's workgroup of this dispatch (the bake spreads them over frames)
        float rockExtinction; // a SOLID type's extinction (1/m) over its occupancy ("Far rock extinction")
    };
    static_assert(sizeof(SplatPC) == 128); // the guaranteed push-constant minimum: full
    struct MarchPC
    {
        VolumeParamsGpu vol;
        uint32 width;
        uint32 height;
        float stepScale;
        float startDistance;
        uint32 maxSteps;
        float ambient;
        float shrink;
        float overlap;
        float sunScale;
        float selfShadow;
        float normalStrength;
        float groundDark;
        float forwardScatter;
        float albedoScale;
        float interiorShadow;
        float interiorRadius;
        float saturation;   // "Far saturation scale" (the scale and the pixel skip are baked: TREE_MARCH_SCALE / TREE_MARCH_SKIP)
        uint32 pad1;
        glm::uvec2 fullSize;
    };
    static_assert(sizeof(MarchPC) == 128); // the guaranteed push-constant minimum: full
    // cloud_temporal.cs.glsl's push block under TREE_TEMPORAL (the scale and the checkerboard are baked).
    struct TemporalPC
    {
        uint32 viewIndex;
        uint32 width;
        uint32 height;
        float historyWeight;
        float maxDist;
    };
    // tree_volume_records.cs.glsl's / tree_volume_far.cs.glsl's (scalar layout).
    struct RecordsPC
    {
        vk::DeviceAddress records;
        vk::DeviceAddress chunks;
        vk::DeviceAddress types;
        uint32 numChunks;
        uint32 numTypes;
        float chunkSize;
        uint32 worldSeed;
        VolumeParamsGpu vol;
        float recordDetail;
        float rockExtinction; // a rock record's mass: its occupied volume x scale^3 x this
        uint32 chunkOffset;   // this dispatch's first chunk (numChunks: this dispatch's count)
        uint32 pad;           // the 8-byte alignment's tail (the shader's block ends at chunkOffset)
    };
    static_assert(sizeof(RecordsPC) == 104);
    // tree_volume_clear.cs.glsl's.
    struct ClearPC
    {
        uint32 sliceOffset;
    };
    // tree_volume_resolve.cs.glsl's (the column colour needs the columns' world positions).
    struct ResolvePC
    {
        VolumeParamsGpu vol;
        uint32 sliceOffset; // this dispatch's first slice
        uint32 columnPass;  // 1: the per-column colour pass (before any conversion); 0: convert the slices in place
    };
    struct FarPC
    {
        vk::DeviceAddress types;
        uint32 numTypes;
        uint32 mapSize;
        VolumeParamsGpu vol;
        vk::DeviceAddress records; // the chunks' ground
        vk::DeviceAddress map;
        float chunkSize;
        uint32 rowOffset; // this dispatch's first radial row
    };
    static_assert(sizeof(FarPC) == 88);
    // tree_volume_floor_smooth.cs.glsl's.
    struct FloorSmoothPC
    {
        uint32 angularRes;
        uint32 radialRes;
        int32 radius;
        uint32 radialAxis;
    };
    // tree_volume_upsample.cs.glsl's.
    struct UpsamplePC
    {
        glm::uvec2 size;
        float maxDist;
    };

    vk::DescriptorSetLayoutBinding binding(uint32 idx, vk::DescriptorType type, vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eCompute)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = idx, .descriptorType = type, .descriptorCount = 1, .stageFlags = stages };
    }
    auto storageInfo(vk::ImageView view) { return vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledGeneral(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }

    void writeSet(vk::DescriptorSet set, oc::span<DescriptorSetUpdateInfo> updates)
    {
        oc::small_vector<vk::WriteDescriptorSet, 8> writes;
        for (const DescriptorSetUpdateInfo& u : updates)
            writes.push_back(vk::WriteDescriptorSet{
                .dstSet = set,
                .dstBinding = u.binding,
                .dstArrayElement = u.startIdx,
                .descriptorCount = (uint32)oc::max(u.imageInfos.size(), u.bufferInfos.size()),
                .descriptorType = u.type,
                .pImageInfo = u.imageInfos.size() ? u.imageInfos.data() : nullptr,
                .pBufferInfo = u.bufferInfos.size() ? u.bufferInfos.data() : nullptr,
            });
        Globals::device.getDevice().updateDescriptorSets((uint32)writes.size(), writes.data(), 0, nullptr);
    }

    VolumeParamsGpu volumeParams(const FarTreeParams& s, glm::vec2 centre)
    {
        // A FIXED horizontal inner radius: the hand-over sits at horizontal "Far start" at any camera height (the
        // scaled start, Renderer::farTreesStart). A rebake distance (+ a crown) further in: the camera may move that
        // far from the bake centre, and the ring must still cover everything past the hand-over.
        const float rMin = oc::max(s.startDistance - s.rebakeDistance - 30.0f, 10.0f);
        return VolumeParamsGpu{ centre, rMin, oc::max(s.endDistance, rMin * 1.1f), s.angularRes, s.radialRes, s.slices,
            oc::max(s.height, 1.0f), s.densityScale, 0.0f, 0.0f, 0.0f };
    }

    // Everything the bake reads that is not per frame.
    bool sameBake(const FarTreeParams& a, const FarTreeParams& b)
    {
        return a.startDistance == b.startDistance && a.rebakeDistance == b.rebakeDistance && a.endDistance == b.endDistance && a.angularRes == b.angularRes
            && a.radialRes == b.radialRes && a.slices == b.slices && a.height == b.height && a.floorSmoothing == b.floorSmoothing
            && a.recordDetail == b.recordDetail && a.rockExtinction == b.rockExtinction;
    }

    void createImage(vk::ImageType type, vk::Format format, vk::Extent3D extent, vk::ImageUsageFlags usage,
        vk::Image& outImage, VmaAllocation& outMemory, vk::ImageView& outView, const char* debugName, vk::ImageCreateFlags flags = {})
    {
        vk::ImageCreateInfo info{
            .flags = flags,
            .imageType = type,
            .format = format,
            .extent = extent,
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, outImage, outMemory, debugName);
        vk::ImageViewCreateInfo viewInfo{
            .image = outImage,
            .viewType = type == vk::ImageType::e3D ? vk::ImageViewType::e3D : vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        outView = viewResult.value;
        Globals::device.setDebugName(outView, debugName);
    }

    // UNDEFINED -> GENERAL for every image given, then a clear to `value`.
    void initGeneral(oc::span<const vk::Image> images, const vk::ClearColorValue& value)
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary, "TreeVolume.init");
        vk::CommandBuffer cmd = init.begin(true);
        oc::small_vector<vk::ImageMemoryBarrier2, 8> bars;
        for (vk::Image image : images)
            bars.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            });
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
        const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        for (vk::Image image : images)
            cmd.clearColorImage(image, vk::ImageLayout::eGeneral, value, { range });
        const vk::MemoryBarrier2 toUse{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toUse });
        init.end();
        init.submitGraphics();
        (void)Globals::device.graphicsQueueWaitIdle();
    }
}

// floorPass: 0 = the splat, 1 = the floor's coverage pass, 2 = the floor pass (TREE_FLOOR_PASS). records: the trees
// come from the world tree records (TREE_SPLAT_RECORDS).
void TreeVolumePipeline::buildSplatLayout(ComputePipelineLayout& layout, uint32 floorPass, bool records)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_splat.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    if (floorPass != 0)
        layout.defines.push_back(ShaderDefine{ "TREE_FLOOR_PASS", floorPass == 1 ? "1" : "2" });
    if (records)
        layout.defines.push_back(ShaderDefine{ "TREE_SPLAT_RECORDS", "1" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eCombinedImageSampler)); // terrain data (the records' ground)
    b.push_back(binding(2, vk::DescriptorType::eStorageImage));         // accum
    b.push_back(binding(3, vk::DescriptorType::eStorageImage));         // colour
    b.push_back(binding(4, vk::DescriptorType::eStorageImage));         // floor
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));         // the floor's coverage
    b.push_back(binding(6, vk::DescriptorType::eStorageImage));         // the rocks' share per column
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(SplatPC) });
}

void TreeVolumePipeline::buildResolveLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_resolve.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eCombinedImageSampler)); // terrain data (the columns' climate)
    b.push_back(binding(2, vk::DescriptorType::eStorageImage));         // accum (converted in place)
    b.push_back(binding(4, vk::DescriptorType::eStorageImage));         // the rocks' share per column
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));         // colour (back)
    b.push_back(binding(6, vk::DescriptorType::eStorageImage));         // floor (back)
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 7, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = ROCK_TEXTURES, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // the rock materials' diffuse
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ResolvePC) });
}

void TreeVolumePipeline::buildCopyLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_copy.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.descriptorSetLayoutBindings.push_back(binding(0, vk::DescriptorType::eStorageImage)); // accum (the float bits)
    layout.descriptorSetLayoutBindings.push_back(binding(1, vk::DescriptorType::eStorageImage)); // density
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ClearPC) });
}

void TreeVolumePipeline::buildClearLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_clear.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.descriptorSetLayoutBindings.push_back(binding(0, vk::DescriptorType::eStorageImage)); // accum
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ClearPC) });
}

void TreeVolumePipeline::buildFloorSmoothLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_floor_smooth.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eStorageImage)); // source floor
    b.push_back(binding(1, vk::DescriptorType::eStorageImage)); // destination
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(FloorSmoothPC) });
}

void TreeVolumePipeline::buildFloorMaxLayout(ComputePipelineLayout& layout, bool dilate)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_floor_max.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    if (dilate)
        layout.defines.push_back(ShaderDefine{ "TREE_FLOOR_MAX_DILATE", "1" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eStorageImage)); // the floor / the block max
    b.push_back(binding(1, vk::DescriptorType::eStorageImage)); // the block max / the dilated grid
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(FloorMaxPC) });
}

vk::Extent3D TreeVolumePipeline::floorMaxExtent() const
{
    return { (m_angularRes + FLOOR_MAX_BLOCK - 1) / FLOOR_MAX_BLOCK, (m_radialRes + FLOOR_MAX_BLOCK - 1) / FLOOR_MAX_BLOCK, 1 };
}

void TreeVolumePipeline::buildRecordsLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_records.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(2, vk::DescriptorType::eStorageImage)); // the mass per column
    b.push_back(binding(3, vk::DescriptorType::eStorageImage)); // the type per column
    b.push_back(binding(4, vk::DescriptorType::eStorageImage)); // colour
    b.push_back(binding(5, vk::DescriptorType::eStorageImage)); // the rocks' share per column
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(RecordsPC) });
}

void TreeVolumePipeline::buildFarLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_far.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eCombinedImageSampler)); // terrain data
    b.push_back(binding(2, vk::DescriptorType::eStorageImage));         // the mass per column
    b.push_back(binding(3, vk::DescriptorType::eStorageImage));         // the type per column
    b.push_back(binding(4, vk::DescriptorType::eStorageImage));         // floor
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));         // accum
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(FarPC) });
}

// The march, BAKED per setting: the temporal variant (TREE_TEMPORAL_OUT) at its scale, and the pixel skip (0 / 1 of 2 /
// 1 of 4 - the temporal variant only knows 1 of 2).
void TreeVolumePipeline::buildMarchLayout(ComputePipelineLayout& layout, bool temporalOut, uint32 scale, uint32 skip, bool handover)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_march.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    if (temporalOut)
        layout.defines.push_back(ShaderDefine{ "TREE_TEMPORAL_OUT", "1" });
    if (handover)
        layout.defines.push_back(ShaderDefine{ "TREE_HANDOVER", "1" });
    layout.defines.push_back(ShaderDefine{ "TREE_MARCH_SCALE", oc::to_string(scale) });
    layout.defines.push_back(ShaderDefine{ "TREE_MARCH_SKIP", oc::to_string(skip) });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eCombinedImageSampler)); // scene depth
    b.push_back(binding(2, vk::DescriptorType::eCombinedImageSampler)); // terrain data
    b.push_back(binding(3, vk::DescriptorType::eCombinedImageSampler)); // density
    b.push_back(binding(4, vk::DescriptorType::eCombinedImageSampler)); // colour
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));         // out
    b.push_back(binding(6, vk::DescriptorType::eStorageImage));         // out distance
    b.push_back(binding(7, vk::DescriptorType::eStorageImage));         // floor
    b.push_back(binding(8, vk::DescriptorType::eStorageImage));         // the previous slot's result (the checkerboard's copy)
    b.push_back(binding(9, vk::DescriptorType::eStorageImage));         // ... and its distance
    b.push_back(binding(10, vk::DescriptorType::eCombinedImageSampler)); // GI's sky map (the canopy's sky light)
    b.push_back(binding(11, vk::DescriptorType::eCombinedImageSampler)); // the cloud shadow map
    b.push_back(binding(12, vk::DescriptorType::eStorageImage));         // the max-floor grid
    // The hand-over's NEW bake (TREE_HANDOVER reads them; in every variant's layout, so one set serves all of them).
    b.push_back(binding(13, vk::DescriptorType::eCombinedImageSampler)); // density (the accumulation's R32F view)
    b.push_back(binding(14, vk::DescriptorType::eStorageImage));         // floor (back)
    b.push_back(binding(15, vk::DescriptorType::eCombinedImageSampler)); // colour (back)
    b.push_back(binding(16, vk::DescriptorType::eStorageImage));         // the max-floor grid (back)
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(MarchPC) });
}

bool TreeVolumePipeline::buildMarchPair(ComputePipeline& march, ComputePipeline& handover, bool temporalOut, uint32 scale, uint32 skip, bool reload)
{
    ComputePipelineLayout marchLayout;    buildMarchLayout(marchLayout, temporalOut, scale, skip, false);
    ComputePipelineLayout handoverLayout; buildMarchLayout(handoverLayout, temporalOut, scale, skip, true);
    if (!reload || !march.getPipeline())
        return march.initialize(marchLayout) && handover.initialize(handoverLayout);
    const bool marchOk = march.reloadShaders(marchLayout);
    return handover.reloadShaders(handoverLayout) && marchOk;
}

void TreeVolumePipeline::buildTemporalLayout(ComputePipelineLayout& layout, uint32 scale, bool checker)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_temporal.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.defines.push_back(ShaderDefine{ "TREE_TEMPORAL", "1" });
    layout.defines.push_back(ShaderDefine{ "TREE_TEMPORAL_SCALE", oc::to_string(scale) });
    layout.defines.push_back(ShaderDefine{ "TREE_TEMPORAL_CHECKER", checker ? "1" : "0" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    for (uint32 i = 1; i <= 4; ++i) // this frame's march colour + distances, the history colour + distances
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));
    b.push_back(binding(6, vk::DescriptorType::eStorageImage));
    b.push_back(binding(7, vk::DescriptorType::eCombinedImageSampler)); // scene depth (the clouds' checkerboard only)
    b.push_back(binding(8, vk::DescriptorType::eStorageImage));         // the fog apply's mean distance (R16F)
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TemporalPC) });
}

void TreeVolumePipeline::buildUpsampleLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/tree_volume_upsample.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    for (uint32 i = 1; i <= 3; ++i) // scene depth, the half-res colour + distances
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
    b.push_back(binding(4, vk::DescriptorType::eStorageImage)); // the full-res colour
    b.push_back(binding(5, vk::DescriptorType::eStorageImage)); // the full-res mean distance (R16F)
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(UpsamplePC) });
}

void TreeVolumePipeline::buildApplyLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/composite.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/tree_volume_apply.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    // out = inScatter + sceneColor * transmittance, like the cloud apply.
    layout.blendEnable = true;
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    layout.colorWriteAlpha = false; // scene colour alpha = TAA's ocean flag
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    layout.descriptorSetLayoutBindings.push_back(binding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment));
}

void TreeVolumePipeline::destroyImage(Image& image)
{
    if (image.view)
        Globals::device.getDevice().destroyImageView(image.view);
    Globals::gpuAllocator.destroyImage(image.image, image.memory);
    image = {};
}

void TreeVolumePipeline::prepare(const FarTreeParams& settings)
{
    if (m_angularRes != settings.angularRes || m_radialRes != settings.radialRes || m_slices != settings.slices)
        createVolume(settings.angularRes, settings.radialRes, settings.slices);
    const uint32 scale = settings.halfRes ? 2u : 1u;
    const bool temporalChecker = settings.pixelSkip != 0; // the temporal pass knows 1 of 2 only
    if (settings.temporalPath() && (!m_hasTemporalImages || m_temporalScale != scale))
    {
        // First use: the three pipelines (compiled only now - off costs not even the startup compile), then the images.
        if (!m_temporalPipeline.getPipeline())
        {
            (void)buildMarchPair(m_marchTemporalPipeline, m_marchTemporalHandoverPipeline, true, scale, temporalChecker ? 1u : 0u, false);
            ComputePipelineLayout temporalLayout; buildTemporalLayout(temporalLayout, scale, temporalChecker);
            m_temporalPipeline.initialize(temporalLayout);
            ComputePipelineLayout upsampleLayout; buildUpsampleLayout(upsampleLayout);  m_upsamplePipeline.initialize(upsampleLayout);
            for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
            {
                m_temporalSets[f].initialize(m_temporalPipeline.getDescriptorSetLayout(), "TreeVolume.temporal");
                m_upsampleSets[f].initialize(m_upsamplePipeline.getDescriptorSetLayout(), "TreeVolume.upsample");
            }
            m_temporalBakedScale = scale;
            m_temporalBakedChecker = temporalChecker;
        }
        createTemporalImages(scale);
    }
    // The BAKED march / temporal variants follow the settings: rebuilt only when their define set changes (a toggle
    // drains the GPU once; a frame never runs a variant that does not match its dispatch).
    if (settings.temporalPath() && m_temporalPipeline.getPipeline()
        && (m_temporalBakedScale != scale || m_temporalBakedChecker != temporalChecker))
    {
        (void)Globals::device.graphicsQueueWaitIdle();
        ComputePipelineLayout temporalLayout; buildTemporalLayout(temporalLayout, scale, temporalChecker);
        // Both or neither: the baked values describe what the pipelines run (record() dispatches by them). The march
        // pair is the march and its hand-over twin.
        const bool marchOk = buildMarchPair(m_marchTemporalPipeline, m_marchTemporalHandoverPipeline, true, scale, temporalChecker ? 1u : 0u, true);
        if (marchOk && m_temporalPipeline.reloadShaders(temporalLayout))
        {
            m_temporalBakedScale = scale;
            m_temporalBakedChecker = temporalChecker;
        }
        else
        {
            printf("TreeVolumePipeline: temporal variant rebuild failed, keeping the previous one\n");
            if (marchOk) // put the march back in step with the kept temporal pass
                (void)buildMarchPair(m_marchTemporalPipeline, m_marchTemporalHandoverPipeline, true, m_temporalBakedScale,
                    m_temporalBakedChecker ? 1u : 0u, true);
        }
    }
    if (!settings.temporalPath() && m_plainBakedSkip != (uint32)settings.pixelSkip)
    {
        (void)Globals::device.graphicsQueueWaitIdle();
        if (buildMarchPair(m_marchPipeline, m_marchHandoverPipeline, false, 1, (uint32)settings.pixelSkip, true))
            m_plainBakedSkip = (uint32)settings.pixelSkip;
        else
            printf("TreeVolumePipeline: march variant rebuild failed, keeping the previous one\n");
    }
    else if (!settings.temporalPath() && m_hasTemporalImages)
    {
        (void)Globals::device.graphicsQueueWaitIdle(); // in-flight frames may still read them
        destroyTemporalImages();
    }
    // The plain pixel skip's latest-march images: only while the plain path skips pixels.
    const bool wantLatest = !settings.temporalPath() && settings.pixelSkip != 0;
    if (wantLatest && !m_hasLatest)
        createLatestImages();
    else if (!wantLatest && m_hasLatest)
    {
        (void)Globals::device.graphicsQueueWaitIdle();
        destroyLatestImages();
    }
}

void TreeVolumePipeline::createLatestImages()
{
    (void)Globals::device.graphicsQueueWaitIdle();
    destroyLatestImages();
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst;
    createImage(vk::ImageType::e2D, OUT_FORMAT, { m_width, m_height, 1 }, usage, m_latest.image, m_latest.memory, m_latest.view, "TreeVolume.latest");
    createImage(vk::ImageType::e2D, DENSITY_FORMAT, { m_width, m_height, 1 }, usage, m_latestDepth.image, m_latestDepth.memory, m_latestDepth.view, "TreeVolume.latestDepth");
    const vk::Image images[] = { m_latest.image, m_latestDepth.image };
    initGeneral(images, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }); // record() clears again at the restart
    m_hasLatest = true;
    m_lastPlainMarchFrame = UINT32_MAX; // a restart
}

void TreeVolumePipeline::destroyLatestImages()
{
    destroyImage(m_latest);
    destroyImage(m_latestDepth);
    m_hasLatest = false;
}

// At the MARCH resolution (the render size / scale): the raw march pair, and per slot the history distances - plus, at
// half res, the history colour (at full res the slot's own result image is the history).
void TreeVolumePipeline::createTemporalImages(uint32 scale)
{
    (void)Globals::device.graphicsQueueWaitIdle();
    destroyTemporalImages();
    m_temporalScale = scale;
    const vk::Extent3D extent{ (m_width + scale - 1) / scale, (m_height + scale - 1) / scale, 1 };
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    oc::small_vector<vk::Image, 8> images;
    createImage(vk::ImageType::e2D, OUT_FORMAT, extent, usage, m_raw.image, m_raw.memory, m_raw.view, "TreeVolume.raw");
    createImage(vk::ImageType::e2D, OUT_FORMAT, extent, usage, m_rawDepth.image, m_rawDepth.memory, m_rawDepth.view, "TreeVolume.rawDepth");
    images.push_back(m_raw.image);
    images.push_back(m_rawDepth.image);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        createImage(vk::ImageType::e2D, OUT_FORMAT, extent, usage, m_histDepth[f].image, m_histDepth[f].memory, m_histDepth[f].view, "TreeVolume.histDepth");
        images.push_back(m_histDepth[f].image);
        if (scale > 1)
        {
            createImage(vk::ImageType::e2D, OUT_FORMAT, extent, usage, m_histColour[f].image, m_histColour[f].memory, m_histColour[f].view, "TreeVolume.histColour");
            images.push_back(m_histColour[f].image);
        }
    }
    initGeneral(oc::span<const vk::Image>(images.data(), images.size()), vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } });
    m_hasTemporalImages = true;
    m_lastMarchFrame = UINT32_MAX; // the slots' history distances are new: no history this frame
}

void TreeVolumePipeline::destroyTemporalImages()
{
    destroyImage(m_raw);
    destroyImage(m_rawDepth);
    for (Image& image : m_histDepth)
        destroyImage(image);
    for (Image& image : m_histColour)
        destroyImage(image);
    m_hasTemporalImages = false;
}

void TreeVolumePipeline::createVolume(uint32 angularRes, uint32 radialRes, uint32 slices)
{
    (void)Globals::device.graphicsQueueWaitIdle(); // in-flight frames still read the old volume
    if (m_accumFloatView)
        Globals::device.getDevice().destroyImageView(m_accumFloatView);
    m_accumFloatView = nullptr;
    destroyImage(m_accum);
    destroyImage(m_density);
    for (uint32 i = 0; i < 2; ++i)
    {
        destroyImage(m_colour[i]);
        destroyImage(m_floor[i]);
        destroyImage(m_floorMax[i]);
    }
    destroyImage(m_floorBlock);
    destroyImage(m_floorCover);
    destroyImage(m_farAmount);
    destroyImage(m_farType);
    destroyImage(m_rockSum);
    m_job.active = false; // its images are gone
    m_angularRes = angularRes;
    m_radialRes = radialRes;
    m_slices = slices;
    const vk::ImageUsageFlags storageClear = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst;
    // MUTABLE: after the resolve it holds the new extinction's float bits, sampled through an R32F view (the hand-over).
    createImage(vk::ImageType::e3D, ACCUM_FORMAT, { angularRes, radialRes, slices }, storageClear | vk::ImageUsageFlagBits::eSampled,
        m_accum.image, m_accum.memory, m_accum.view, "TreeVolume.accum", vk::ImageCreateFlagBits::eMutableFormat);
    {
        const vk::ImageViewCreateInfo floatViewInfo{
            .image = m_accum.image,
            .viewType = vk::ImageViewType::e3D,
            .format = vk::Format::eR32Sfloat,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = Globals::device.getDevice().createImageView(floatViewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        m_accumFloatView = viewResult.value;
        Globals::device.setDebugName(m_accumFloatView, "TreeVolume.accumFloat");
    }
    createImage(vk::ImageType::e3D, DENSITY_FORMAT, { angularRes, radialRes, slices }, storageClear | vk::ImageUsageFlagBits::eSampled,
        m_density.image, m_density.memory, m_density.view, "TreeVolume.density");
    for (uint32 i = 0; i < 2; ++i)
    {
        createImage(vk::ImageType::e2D, COLOUR_FORMAT, { angularRes, radialRes, 1 }, storageClear | vk::ImageUsageFlagBits::eSampled,
            m_colour[i].image, m_colour[i].memory, m_colour[i].view, "TreeVolume.colour");
        createImage(vk::ImageType::e2D, ACCUM_FORMAT, { angularRes, radialRes, 1 }, storageClear, m_floor[i].image, m_floor[i].memory, m_floor[i].view, "TreeVolume.floor");
        createImage(vk::ImageType::e2D, FLOOR_MAX_FORMAT, floorMaxExtent(), storageClear, m_floorMax[i].image, m_floorMax[i].memory, m_floorMax[i].view, "TreeVolume.floorMax");
    }
    createImage(vk::ImageType::e2D, FLOOR_BLOCK_FORMAT, floorMaxExtent(), storageClear, m_floorBlock.image, m_floorBlock.memory, m_floorBlock.view, "TreeVolume.floorBlock");
    createImage(vk::ImageType::e2D, ACCUM_FORMAT, { angularRes, radialRes, 1 }, storageClear, m_floorCover.image, m_floorCover.memory, m_floorCover.view, "TreeVolume.floorCover");
    createImage(vk::ImageType::e2D, ACCUM_FORMAT, { angularRes, radialRes, 1 }, storageClear, m_farAmount.image, m_farAmount.memory, m_farAmount.view, "TreeVolume.farAmount");
    createImage(vk::ImageType::e2D, ACCUM_FORMAT, { angularRes, radialRes, 1 }, storageClear, m_farType.image, m_farType.memory, m_farType.view, "TreeVolume.farType");
    createImage(vk::ImageType::e2D, ACCUM_FORMAT, { angularRes, radialRes, 1 }, storageClear, m_rockSum.image, m_rockSum.memory, m_rockSum.view, "TreeVolume.rockSum");
    const vk::Image images[] = { m_accum.image, m_density.image, m_colour[0].image, m_colour[1].image, m_floor[0].image, m_floor[1].image,
        m_floorCover.image, m_farAmount.image, m_farType.image, m_rockSum.image, m_floorMax[0].image, m_floorMax[1].image, m_floorBlock.image };
    initGeneral(images, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }); // zero bits: also 0u for R32UI (an empty volume:
                                                                                                 // a max floor of 0 skips nothing that exists)
    m_baked = false;
}

void TreeVolumePipeline::recreateImages(uint32 width, uint32 height)
{
    for (Image& out : m_out)
        destroyImage(out);
    for (Image& out : m_outDepth)
        destroyImage(out);
    m_width = width;
    m_height = height;
    m_lastPlainMarchFrame = UINT32_MAX; // the new result images hold no march yet: no checkerboard copy source
    oc::array<vk::Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * 2> images;
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        createImage(vk::ImageType::e2D, OUT_FORMAT, { width, height, 1 }, usage, m_out[f].image, m_out[f].memory, m_out[f].view, "TreeVolume.out");
        createImage(vk::ImageType::e2D, DENSITY_FORMAT, { width, height, 1 }, usage, m_outDepth[f].image, m_outDepth[f].memory, m_outDepth[f].view, "TreeVolume.outDepth");
        images[f * 2] = m_out[f].image;
        images[f * 2 + 1] = m_outDepth[f].image;
    }
    initGeneral(images, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }); // no trees
    if (m_hasLatest)
        createLatestImages(); // at the new size (a restart)
    if (m_hasTemporalImages)
        createTemporalImages(m_temporalScale); // at the new size (no history)
}

TreeVolumePipeline::~TreeVolumePipeline()
{
    if (m_accumFloatView)
        Globals::device.getDevice().destroyImageView(m_accumFloatView);
    destroyImage(m_accum);
    destroyImage(m_density);
    for (uint32 i = 0; i < 2; ++i)
    {
        destroyImage(m_colour[i]);
        destroyImage(m_floor[i]);
        destroyImage(m_floorMax[i]);
    }
    destroyImage(m_floorBlock);
    destroyImage(m_floorCover);
    destroyImage(m_farAmount);
    destroyImage(m_farType);
    destroyImage(m_rockSum);
    for (Image& out : m_out)
        destroyImage(out);
    for (Image& out : m_outDepth)
        destroyImage(out);
    destroyTemporalImages();
    destroyLatestImages();
    if (m_linearSampler)
        Globals::device.getDevice().destroySampler(m_linearSampler);
    if (m_mipSampler)
        Globals::device.getDevice().destroySampler(m_mipSampler);
    if (m_screenSampler)
        Globals::device.getDevice().destroySampler(m_screenSampler);
}

void TreeVolumePipeline::initialize(uint32 width, uint32 height, vk::RenderPass sceneRenderPass)
{
    ComputePipelineLayout coverLayout;   buildSplatLayout(coverLayout, 1, false);  m_floorCoverPipeline.initialize(coverLayout);
    ComputePipelineLayout floorLayout;   buildSplatLayout(floorLayout, 2, false);  m_floorPipeline.initialize(floorLayout);
    ComputePipelineLayout splatLayout;   buildSplatLayout(splatLayout, 0, false);  m_splatPipeline.initialize(splatLayout);
    for (uint32 pass = 0; pass < 3; ++pass)
    {
        ComputePipelineLayout recordLayout; buildSplatLayout(recordLayout, pass, true); m_recordSplatPipelines[pass].initialize(recordLayout);
        for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
            m_recordSplatSets[pass * RendererVKLayout::NUM_FRAMES_IN_FLIGHT + f].initialize(m_recordSplatPipelines[pass].getDescriptorSetLayout(), "TreeVolume.recordSplat");
    }
    ComputePipelineLayout resolveLayout; buildResolveLayout(resolveLayout); m_resolvePipeline.initialize(resolveLayout);
    ComputePipelineLayout clearLayout;   buildClearLayout(clearLayout);     m_clearPipeline.initialize(clearLayout);
    for (DescriptorSet& set : m_clearSets)
        set.initialize(m_clearPipeline.getDescriptorSetLayout(), "TreeVolume.clear");
    ComputePipelineLayout copyLayout;    buildCopyLayout(copyLayout);       m_copyPipeline.initialize(copyLayout);
    for (DescriptorSet& set : m_copySets)
        set.initialize(m_copyPipeline.getDescriptorSetLayout(), "TreeVolume.copy");
    ComputePipelineLayout smoothLayout;  buildFloorSmoothLayout(smoothLayout); m_floorSmoothPipeline.initialize(smoothLayout);
    ComputePipelineLayout recordsLayout; buildRecordsLayout(recordsLayout); m_recordsPipeline.initialize(recordsLayout);
    ComputePipelineLayout farLayout;     buildFarLayout(farLayout);         m_farPipeline.initialize(farLayout);
    ComputePipelineLayout maxLayout;     buildFloorMaxLayout(maxLayout, false); m_floorMaxPipeline.initialize(maxLayout);
    ComputePipelineLayout dilateLayout;  buildFloorMaxLayout(dilateLayout, true); m_floorDilatePipeline.initialize(dilateLayout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_recordsSets[f].initialize(m_recordsPipeline.getDescriptorSetLayout(), "TreeVolume.records");
        m_farSets[f].initialize(m_farPipeline.getDescriptorSetLayout(), "TreeVolume.far");
        m_floorMaxSets[f * 2].initialize(m_floorMaxPipeline.getDescriptorSetLayout(), "TreeVolume.floorMax");
        m_floorMaxSets[f * 2 + 1].initialize(m_floorDilatePipeline.getDescriptorSetLayout(), "TreeVolume.floorDilate");
    }
    for (DescriptorSet& set : m_floorSmoothSets)
        set.initialize(m_floorSmoothPipeline.getDescriptorSetLayout(), "TreeVolume.floorSmooth");
    // The pixel skip baked at the default setting; prepare() rebuilds it if the setting differs.
    (void)buildMarchPair(m_marchPipeline, m_marchHandoverPipeline, false, 1, m_plainBakedSkip, false);
    GraphicsPipelineLayout applyLayout;  buildApplyLayout(applyLayout);     m_applyPipeline.initialize(sceneRenderPass, applyLayout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_floorCoverSets[f].initialize(m_floorCoverPipeline.getDescriptorSetLayout(), "TreeVolume.floorCover");
        m_floorSets[f].initialize(m_floorPipeline.getDescriptorSetLayout(), "TreeVolume.floor");
        m_splatSets[f].initialize(m_splatPipeline.getDescriptorSetLayout(), "TreeVolume.splat");
        m_resolveSets[f].initialize(m_resolvePipeline.getDescriptorSetLayout(), "TreeVolume.resolve");
        m_marchSets[f].initialize(m_marchPipeline.getDescriptorSetLayout(), "TreeVolume.march");
        m_applySets[f].initialize(m_applyPipeline.getDescriptorSetLayout(), "TreeVolume.apply");
    }
    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eRepeat, // the volume's u is the angle: it wraps
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    };
    auto samplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(samplerResult.result == vk::Result::eSuccess);
    m_linearSampler = samplerResult.value;
    Globals::device.setDebugName(m_linearSampler, "TreeVolume.linear");
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    auto screenSamplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(screenSamplerResult.result == vk::Result::eSuccess);
    m_screenSampler = screenSamplerResult.value;
    Globals::device.setDebugName(m_screenSampler, "TreeVolume.screen");
    samplerInfo.maxLod = 1000.0f; // VK_LOD_CLAMP_NONE: the rock textures' smallest mip
    auto mipSamplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(mipSamplerResult.result == vk::Result::eSuccess);
    m_mipSampler = mipSamplerResult.value;
    Globals::device.setDebugName(m_mipSampler, "TreeVolume.mip");
    recreateImages(width, height);
}

void TreeVolumePipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    ComputePipelineLayout coverLayout;   buildSplatLayout(coverLayout, 1, false);
    ComputePipelineLayout floorLayout;   buildSplatLayout(floorLayout, 2, false);
    ComputePipelineLayout splatLayout;   buildSplatLayout(splatLayout, 0, false);
    bool recordsOk = true;
    for (uint32 pass = 0; pass < 3; ++pass)
    {
        ComputePipelineLayout recordLayout; buildSplatLayout(recordLayout, pass, true);
        recordsOk = m_recordSplatPipelines[pass].reloadShaders(recordLayout) && recordsOk;
    }
    ComputePipelineLayout resolveLayout; buildResolveLayout(resolveLayout);
    ComputePipelineLayout clearLayout;   buildClearLayout(clearLayout);
    ComputePipelineLayout copyLayout;    buildCopyLayout(copyLayout);
    ComputePipelineLayout smoothLayout;  buildFloorSmoothLayout(smoothLayout);
    ComputePipelineLayout recordsLayout; buildRecordsLayout(recordsLayout);
    ComputePipelineLayout farLayout;     buildFarLayout(farLayout);
    GraphicsPipelineLayout applyLayout;  buildApplyLayout(applyLayout);
    ComputePipelineLayout maxLayout;     buildFloorMaxLayout(maxLayout, false);
    ComputePipelineLayout dilateLayout;  buildFloorMaxLayout(dilateLayout, true);
    bool ok = m_floorCoverPipeline.reloadShaders(coverLayout) && recordsOk;
    ok = m_floorMaxPipeline.reloadShaders(maxLayout) && ok;
    ok = m_floorDilatePipeline.reloadShaders(dilateLayout) && ok;
    ok = m_recordsPipeline.reloadShaders(recordsLayout) && ok;
    ok = m_farPipeline.reloadShaders(farLayout) && ok;
    ok = m_floorSmoothPipeline.reloadShaders(smoothLayout) && ok;
    ok = m_floorPipeline.reloadShaders(floorLayout) && ok;
    if (m_temporalPipeline.getPipeline()) // compiled at its first use (prepare)
    {
        ComputePipelineLayout temporalLayout;      buildTemporalLayout(temporalLayout, m_temporalBakedScale, m_temporalBakedChecker);
        ok = buildMarchPair(m_marchTemporalPipeline, m_marchTemporalHandoverPipeline, true, m_temporalBakedScale, m_temporalBakedChecker ? 1u : 0u, true) && ok;
        ok = m_temporalPipeline.reloadShaders(temporalLayout) && ok;
        ComputePipelineLayout upsampleLayout;      buildUpsampleLayout(upsampleLayout);
        ok = m_upsamplePipeline.reloadShaders(upsampleLayout) && ok;
    }
    ok = m_splatPipeline.reloadShaders(splatLayout) && ok;
    ok = m_resolvePipeline.reloadShaders(resolveLayout) && ok;
    ok = m_clearPipeline.reloadShaders(clearLayout) && ok;
    ok = m_copyPipeline.reloadShaders(copyLayout) && ok;
    ok = buildMarchPair(m_marchPipeline, m_marchHandoverPipeline, false, 1, m_plainBakedSkip, true) && ok;
    ok = m_applyPipeline.reloadShaders(sceneRenderPass, applyLayout) && ok;
    if (!ok)
        printf("TreeVolumePipeline: shader reload failed, keeping previous pipeline(s)\n");
    m_dirty = true; // a bake edit takes effect at once
}

// The bake's start: everything it reads that may change before it ends is SNAPSHOT here - the static sets' list, the
// record table and the chunk map (copied into the bake's own buffers; the record pool keeps the chunks they name alive
// until the bake ends: bakeHoldSince), and the detail chunks.
void TreeVolumePipeline::startBake(const RecordParams& params, uint32 frameNumber, glm::vec2 centre)
{
    const FarTreeParams& s = params.settings;
    BakeJob& job = m_job;
    job.active = true;
    job.stage = EBakeStage::Clear;
    job.progress = 0;
    job.startFrame = frameNumber;
    job.startSec = Globals::time.getElapsedSec();
    job.centre = centre;
    job.settings = s;
    job.sources.assign(params.sources.begin(), params.sources.end());
    job.records = params.records;
    job.records.chunksCpu = {};
    job.records.mapCpu = {};
    job.detailRecords = 0;
    job.numDetailChunks = 0;
    m_dirty = false;

    // Host-visible snapshot buffers: the last bake ended NUM_FRAMES_IN_FLIGHT frames ago or more (record()), so nothing
    // still reads them.
    auto upload = [](Buffer& buffer, const void* data, size_t bytes, const char* name)
    {
        if (bytes == 0)
            return;
        if (!buffer.getBuffer() || buffer.getSize() < bytes)
        {
            size_t capacity = oc::max<size_t>(buffer.getSize(), 16 * 1024);
            while (capacity < bytes)
                capacity *= 2;
            buffer.initialize(capacity, vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eHostVisible, false, name, BufferHostAccess::eSequentialWrite);
        }
        buffer.upload(bytes, data);
    };
    job.staticPieces = 0;
    for (const Source& source : job.sources)
        job.staticPieces += source.numPieces;
    if (params.records.numTypes == 0 || params.records.chunksCpu.empty() || params.records.mapCpu.empty())
        job.records.numTypes = 0; // no records this bake
    else
        snapshotRecords(params, upload);
    setBakeBudgets();
}

// The record table, the chunk map and the detail chunks into the bake's snapshot buffers.
void TreeVolumePipeline::snapshotRecords(const RecordParams& params, const oc::function<void(Buffer&, const void*, size_t, const char*)>& upload)
{
    BakeJob& job = m_job;
    const FarTreeParams& s = job.settings;
    upload(m_bakeTable, params.records.chunksCpu.data(), params.records.chunksCpu.size_bytes(), "TreeVolume.bakeTable");
    upload(m_bakeMap, params.records.mapCpu.data(), params.records.mapCpu.size_bytes(), "TreeVolume.bakeMap");
    job.records.chunks = m_bakeTable.getDeviceAddress();
    job.records.numChunks = (uint32)params.records.chunksCpu.size();
    job.records.map = m_bakeMap.getDeviceAddress();

    // THE DETAIL CHUNKS: the record chunks whose centre lies within "Far record detail" of the bake centre (the same
    // test as tree_volume_records.cs, which takes the rest) and whose reach touches the ring, each with its first
    // record's workgroup - the record splat runs one workgroup per RECORD and finds its chunk by a binary search.
    const VolumeParamsGpu vol = volumeParams(s, job.centre);
    m_detailScratch.clear();
    const float cs = params.records.chunkSize;
    const float reach = 0.7072f * cs + 30.0f; // the half diagonal + a crown or a bush's reach
    for (const TreeRecordChunkGpu& chunk : params.records.chunksCpu)
    {
        if (chunk.count == 0)
            continue;
        const float d = glm::length(glm::vec2(chunk.coord) * cs + 0.5f * cs - job.centre);
        if (d >= s.recordDetail || d + reach < vol.rMin || d - reach > vol.rMax)
            continue;
        m_detailScratch.push_back(glm::uvec4((uint32)chunk.coord.x, (uint32)chunk.coord.y, chunk.first, job.detailRecords));
        job.detailRecords += chunk.count;
    }
    job.numDetailChunks = (uint32)m_detailScratch.size();
    upload(m_bakeDetail, m_detailScratch.data(), m_detailScratch.size() * sizeof(glm::uvec4), "TreeVolume.bakeDetail");
}

// THE BUDGETS: every stage that grows with the resolution or the tree count spreads over its share of "Far bake frames" -
// the clear ~1/8, the three splat passes together ~1/2, the records' mass ~1/8, the far columns ~1/4 - so the whole bake
// stays about that long. (Until 2026-10-06 only the record splat was spread: the clear of the accumulation, the static
// sets' splats, the records' mass and the far columns each ran whole in one frame - a "Far trees" spike that grew with
// the resolution.)
void TreeVolumePipeline::setBakeBudgets()
{
    BakeJob& job = m_job;
    const uint32 frames = (uint32)oc::max(job.settings.bakeFrames, 1);
    auto share = [&](uint32 total, uint32 stageFrames) { return oc::max(1u, (total + stageFrames - 1u) / stageFrames); };
    job.clearPerFrame = share(m_slices, oc::max(1u, frames / 8u));
    job.perFrame = oc::max(256u, share(3u * (job.staticPieces + job.detailRecords), oc::max(1u, frames / 2u)));
    job.massPerFrame = share(job.records.numTypes > 0 ? job.records.numChunks : 1u, oc::max(1u, frames / 8u));
    job.rowsPerFrame = (share(m_radialRes, oc::max(1u, frames / 4u)) + 7u) & ~7u; // whole 8-row groups: no overlap
    job.resolvePerFrame = share(m_slices, oc::max(1u, frames / 8u));
    job.copyPerFrame = share(m_slices, oc::max(1u, frames / 8u));
}

void TreeVolumePipeline::trackCamera(glm::vec2 camera)
{
    const double now = Globals::time.getElapsedSec();
    if (m_lastCameraSec >= 0.0 && now <= m_lastCameraSec)
        return; // the same frame's reading
    if (m_lastCameraSec >= 0.0)
    {
        const float dt = (float)(now - m_lastCameraSec);
        const glm::vec2 velocity = (camera - m_lastCamera) / dt;
        // A jump (a teleport, a scenario load) is no motion to predict: start over.
        constexpr float MAX_SPEED = 3000.0f;
        m_cameraVelocity = glm::length(velocity) > MAX_SPEED ? glm::vec2(0.0f)
            : glm::mix(m_cameraVelocity, velocity, 1.0f - glm::exp(-dt / 0.25f)); // ~0.25 s smoothing
    }
    m_lastCamera = camera;
    m_lastCameraSec = now;
}

glm::vec2 TreeVolumePipeline::bakeLead(const FarTreeParams& s) const
{
    if (!s.bakeAhead)
        return glm::vec2(0.0f);
    // Capped at the ring's margin: a wrong guess (a stop, a turn) leaves the camera at most that far off the new centre,
    // and the re-bake test (on the predicted point, which then is the camera) follows at once.
    const glm::vec2 lead = m_cameraVelocity * (float)m_bakeDurationSec;
    const float cap = oc::max(s.rebakeDistance, 1.0f) + 30.0f;
    const float length = glm::length(lead);
    return length > cap ? lead * (cap / length) : lead;
}

// The cross-fade's progress at this frame's real time (Time's unpaused clock: a pause must not hold a hand-over).
float TreeVolumePipeline::handoverFade() const
{
    const float swapTime = m_job.settings.swapTime;
    if (swapTime <= 0.0f)
        return 1.0f;
    return glm::clamp((float)(Globals::time.getElapsedSec() - m_job.handoverStartSec) / swapTime, 0.0f, 1.0f);
}

glm::vec4 TreeVolumePipeline::handoverUbo() const
{
    if (!handingOver())
        return glm::vec4(0.0f);
    const float fraction = m_job.stage == EBakeStage::Copy ? 1.0f : handoverFade();
    return glm::vec4(m_job.centre, fraction, 0.0f);
}

// This frame's share of the running bake (into the BACK floor / colour; the accumulation; the density at the end).
void TreeVolumePipeline::stepBake(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params)
{
    BakeJob& job = m_job;
    const FarTreeParams& s = job.settings;
    const uint32 back = 1u - m_front;
    const bool records = job.records.numTypes > 0;
    const VolumeParamsGpu vol = volumeParams(s, job.centre);
    uint32 budget = job.perFrame; // the record splat's workgroups this frame

    // The earlier frames' bake writes and the marches' reads of the volume (in queue order) before this frame's work -
    // and before this frame's march, which samples the new bake during the hand-over.
    const vk::MemoryBarrier2 stepStart{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite
            | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite
            | vk::AccessFlagBits2::eTransferWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &stepStart });

    if (job.stage == EBakeStage::Clear)
    {
        // The 2D images whole at the first frame (small); the 3D accumulation a slice range per frame (a compute clear:
        // vkCmdClearColorImage clears a 3D image whole). Nothing reads any of them before the splat. The next stage
        // starts the next frame (its opening barrier orders the clears before it).
        if (job.progress == 0)
        {
            const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
            cmd.clearColorImage(m_colour[back].image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.1f, 0.16f, 0.07f, 1.0f } }, { range });
            cmd.clearColorImage(m_floor[back].image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { range }); // 0u = no floor
            cmd.clearColorImage(m_floorCover.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { range });
            cmd.clearColorImage(m_rockSum.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { range });
            if (records)
            {
                cmd.clearColorImage(m_farAmount.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { range });
                cmd.clearColorImage(m_farType.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<uint32, 4>{ UINT32_MAX, 0u, 0u, 0u } }, { range }); // no type
            }
        }
        const uint32 slices = oc::min(job.clearPerFrame, m_slices - job.progress);
        if (slices > 0)
        {
            const vk::DescriptorSet set = m_clearSets[frameIdx].getDescriptorSet();
            oc::array<DescriptorSetUpdateInfo, 1> updates{
                DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_accum.view) } },
            };
            writeSet(set, updates);
            const ClearPC clearPc{ .sliceOffset = job.progress };
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_clearPipeline.getPipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_clearPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
            cmd.pushConstants(m_clearPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(clearPc), &clearPc);
            cmd.dispatch((m_angularRes + 7) / 8, (m_radialRes + 7) / 8, slices);
            job.progress += slices;
        }
        if (job.progress >= m_slices)
        {
            job.progress = 0;
            job.stage = EBakeStage::FloorCover;
        }
        return;
    }

    // The two FLOOR passes (the coverage per column, then the dominant tree's base), then the splat: the same shader,
    // the same per-tree dispatches. A pass walks ONE range - the static sets' pieces, then the detail records - within
    // this frame's budget (the static sets ran whole at a pass's first frame until 2026-10-06). True when the pass is done.
    // pass: 0 = the splat, 1 = the floor coverage, 2 = the floor (the record variants' index).
    auto splat = [&](ComputePipeline& pipeline, DescriptorSet& descriptorSet, uint32 pass)
    {
        auto bindSplat = [&](ComputePipeline& bound, DescriptorSet& boundSet)
        {
            const vk::DescriptorSet set = boundSet.getDescriptorSet();
            oc::array<DescriptorSetUpdateInfo, 7> updates{
                DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                    .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
                DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
                    .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
                DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_accum.view) } },
                DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_colour[back].view) } },
                DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floor[back].view) } },
                DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floorCover.view) } },
                DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_rockSum.view) } },
            };
            writeSet(set, updates);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, bound.getPipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, bound.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        };
        // The static sets: this frame's slice of each, one workgroup per tree (past 65535 the rows wrap into y).
        bool staticBound = false;
        uint32 setStart = 0;
        for (const Source& source : job.sources)
        {
            const uint32 setEnd = setStart + source.numPieces;
            if (budget > 0 && job.progress < setEnd && job.progress >= setStart)
            {
                const uint32 first = job.progress - setStart;
                const uint32 count = oc::min(budget, source.numPieces - first);
                if (!staticBound)
                {
                    bindSplat(pipeline, descriptorSet);
                    staticBound = true;
                }
                SplatPC pc{};
                pc.pieces = source.pieces;
                pc.types = source.types;
                pc.data = source.data;
                pc.numPieces = count;
                pc.vol = vol;
                pc.wgOffset = first;
                pc.rockExtinction = s.rockExtinction;
                cmd.pushConstants(pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
                cmd.dispatch(oc::min(count, 65535u), (count + 65534u) / 65535u, 1);
                job.progress += count;
                budget -= count;
            }
            setStart = setEnd;
        }
        // Then the detail chunks' records, one workgroup each.
        const uint32 recordsDone = job.progress - oc::min(job.progress, job.staticPieces);
        const uint32 count = job.progress >= job.staticPieces ? oc::min(budget, job.detailRecords - recordsDone) : 0u;
        if (count > 0)
        {
            ComputePipeline& recordPipeline = m_recordSplatPipelines[pass];
            bindSplat(recordPipeline, m_recordSplatSets[pass * RendererVKLayout::NUM_FRAMES_IN_FLIGHT + frameIdx]);
            SplatPC pc{};
            pc.pieces = job.records.map;
            pc.pad = job.records.mapSize;
            pc.types = job.records.volumeTypes;
            pc.data = job.records.volumeData;
            pc.numPieces = count;
            pc.vol = vol;
            pc.records = job.records.records;
            pc.detailChunks = m_bakeDetail.getDeviceAddress();
            pc.recordTypes = job.records.types;
            pc.chunkSize = job.records.chunkSize;
            pc.worldSeed = job.records.worldSeed;
            pc.numDetailChunks = job.numDetailChunks;
            pc.numRecordTypes = job.records.numTypes;
            pc.wgOffset = recordsDone;
            pc.rockExtinction = s.rockExtinction;
            cmd.pushConstants(recordPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
            cmd.dispatch(oc::min(count, 65535u), (count + 65534u) / 65535u, 1);
            job.progress += count;
            budget -= count;
        }
        if (job.progress < job.staticPieces + job.detailRecords)
            return false;
        job.progress = 0;
        return true;
    };
    const vk::MemoryBarrier2 floorToSplat{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    if (job.stage == EBakeStage::FloorCover && splat(m_floorCoverPipeline, m_floorCoverSets[frameIdx], 1))
    {
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat }); // coverage -> floor
        job.stage = EBakeStage::Floor;
    }
    if (job.stage == EBakeStage::Floor && budget > 0 && splat(m_floorPipeline, m_floorSets[frameIdx], 2))
    {
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat }); // floor -> smooth / splat
        job.stage = EBakeStage::Smooth;
    }
    if (job.stage == EBakeStage::Smooth)
    {
        job.stage = EBakeStage::Splat;
        if (s.floorSmoothing > 0)
        {
            // The floor's separable tent blur (tree_volume_floor_smooth.cs): along the angle into floorCover (free after
            // the floor pass), then along the radius back into the floor.
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_floorSmoothPipeline.getPipeline());
            for (uint32 axis = 0; axis < 2; ++axis)
            {
                const vk::DescriptorSet set = m_floorSmoothSets[frameIdx * 2 + axis].getDescriptorSet();
                oc::array<DescriptorSetUpdateInfo, 2> updates{
                    DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(axis == 0 ? m_floor[back].view : m_floorCover.view) } },
                    DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(axis == 0 ? m_floorCover.view : m_floor[back].view) } },
                };
                writeSet(set, updates);
                const FloorSmoothPC pc{ .angularRes = m_angularRes, .radialRes = m_radialRes, .radius = s.floorSmoothing, .radialAxis = axis };
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_floorSmoothPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
                cmd.pushConstants(m_floorSmoothPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
                cmd.dispatch((m_angularRes + 7) / 8, (m_radialRes + 7) / 8, 1);
                cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat });
            }
        }
    }
    if (job.stage == EBakeStage::Splat && budget > 0 && splat(m_splatPipeline, m_splatSets[frameIdx], 0))
    {
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat }); // the splat's colour writes
        job.stage = records ? EBakeStage::RecordMass : EBakeStage::Resolve;
        return; // the next stages take frames of their own
    }
    if (job.stage == EBakeStage::RecordMass)
    {
        // THE WORLD TREE RECORDS beyond the detail distance (the ones within it splatted with the trees above): their
        // mass per column, one workgroup per chunk (past 65535 chunks the rows wrap into y), then (the next frame) per
        // column the ground floor (where no tree floor is) and the slices from the type's height profile. After the tree
        // floors and their smoothing: a column a tree floored keeps that floor.
        // A chunk range per frame ("Far bake frames": job.massPerFrame).
        const uint32 count = oc::min(job.massPerFrame, job.records.numChunks - job.progress);
        const RecordsPC recordsPc{ .records = job.records.records, .chunks = job.records.chunks, .types = job.records.types,
            .numChunks = count, .numTypes = job.records.numTypes, .chunkSize = job.records.chunkSize,
            .worldSeed = job.records.worldSeed, .vol = vol, .recordDetail = s.recordDetail, .rockExtinction = s.rockExtinction,
            .chunkOffset = job.progress };
        const vk::DescriptorSet set = m_recordsSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 4> updates{
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_farAmount.view) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_farType.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_colour[back].view) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_rockSum.view) } },
        };
        writeSet(set, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_recordsPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_recordsPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.pushConstants(m_recordsPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(recordsPc), &recordsPc);
        if (count > 0)
            cmd.dispatch(oc::min(count, 65535u), (count + 65534u) / 65535u, 1);
        job.progress += count;
        if (job.progress >= job.records.numChunks)
        {
            job.progress = 0;
            job.stage = EBakeStage::FarColumns;
        }
        return;
    }
    if (job.stage == EBakeStage::FarColumns)
    {
        const vk::DescriptorSet set = m_farSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 6> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_farAmount.view) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_farType.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floor[back].view) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_accum.view) } },
        };
        writeSet(set, updates);
        // A range of radial rows per frame ("Far bake frames": job.rowsPerFrame, whole 8-row groups so the ranges never
        // overlap). Rows only: the ring a column checks for neighbouring mass reads the records' mass, finished above.
        const uint32 rows = oc::min(job.rowsPerFrame, m_radialRes - job.progress);
        const FarPC farPc{ .types = job.records.types, .numTypes = job.records.numTypes, .mapSize = job.records.mapSize, .vol = vol,
            .records = job.records.records, .map = job.records.map, .chunkSize = job.records.chunkSize, .rowOffset = job.progress };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_farPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_farPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.pushConstants(m_farPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(farPc), &farPc);
        if (rows > 0)
            cmd.dispatch((m_angularRes + 7) / 8, (rows + 7) / 8, 1);
        job.progress += rows;
        if (job.progress >= m_radialRes)
        {
            job.progress = 0;
            job.stage = EBakeStage::Resolve;
        }
        return;
    }
    if (job.stage == EBakeStage::Resolve)
    {
        // The sums -> the extinction IN PLACE (float bits in the accumulation: the hand-over's new volume), a slice range
        // per frame. At its first frame first the per-column pass: the colour with the ROCKS' share (the climate's
        // bedrock - the terrain's rock materials' mean colours, weighted by their climate boxes - and the rock fraction
        // in alpha), which sums each column's slices, so it runs before any slice converts.
        const vk::DescriptorSet set = m_resolveSets[frameIdx].getDescriptorSet();
        assert(!params.rockTextures.empty());
        DescriptorSetUpdateInfo rockTextures{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler };
        for (uint32 i = 0; i < ROCK_TEXTURES; ++i)
            rockTextures.imageInfos.push_back(vk::DescriptorImageInfo{ .sampler = m_mipSampler,
                .imageView = params.rockTextures[oc::min<size_t>(i, params.rockTextures.size() - 1)], .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
        oc::array<DescriptorSetUpdateInfo, 7> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_accum.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_rockSum.view) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_colour[back].view) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floor[back].view) } },
            oc::move(rockTextures),
        };
        writeSet(set, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_resolvePipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_resolvePipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        if (job.progress == 0)
        {
            const ResolvePC columnPc{ .vol = vol, .sliceOffset = 0, .columnPass = 1 };
            cmd.pushConstants(m_resolvePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(columnPc), &columnPc);
            cmd.dispatch((m_angularRes + 7) / 8, (m_radialRes + 7) / 8, 1);
            cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat }); // column sums -> conversion
        }
        const uint32 slices = oc::min(job.resolvePerFrame, m_slices - job.progress);
        if (slices > 0)
        {
            const ResolvePC resolvePc{ .vol = vol, .sliceOffset = job.progress, .columnPass = 0 };
            cmd.pushConstants(m_resolvePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(resolvePc), &resolvePc);
            cmd.dispatch((m_angularRes + 7) / 8, (m_radialRes + 7) / 8, slices);
            job.progress += slices;
        }
        if (job.progress >= m_slices)
        {
            job.progress = 0;
            job.stage = EBakeStage::FloorMax;
        }
        return;
    }
    if (job.stage == EBakeStage::FloorMax)
    {
        // The MAX-FLOOR GRID from the finished back floor (tree_volume_floor_max.cs: the block max, then its dilation) -
        // after the far columns, the last floor writes.
        const vk::Extent3D extent = floorMaxExtent();
        const FloorMaxPC maxPc{ .srcSize = glm::uvec2(m_angularRes, m_radialRes), .dstSize = glm::uvec2(extent.width, extent.height) };
        const FloorMaxPC dilatePc{ .srcSize = glm::uvec2(extent.width, extent.height), .dstSize = glm::uvec2(extent.width, extent.height) };
        for (uint32 pass = 0; pass < 2; ++pass)
        {
            ComputePipeline& pipeline = pass == 0 ? m_floorMaxPipeline : m_floorDilatePipeline;
            const vk::DescriptorSet set = m_floorMaxSets[frameIdx * 2 + pass].getDescriptorSet();
            oc::array<DescriptorSetUpdateInfo, 2> updates{
                DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(pass == 0 ? m_floor[back].view : m_floorBlock.view) } },
                DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(pass == 0 ? m_floorBlock.view : m_floorMax[back].view) } },
            };
            writeSet(set, updates);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.getPipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
            cmd.pushConstants(pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(FloorMaxPC), pass == 0 ? &maxPc : &dilatePc);
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            if (pass == 0)
                cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &floorToSplat }); // block max -> dilate
        }
        // THE HAND-OVER next (the first bake has nothing to hand over: straight to the copy). It starts counting at
        // this frame, whose UBO was built before it: the march takes the hand-over variant from the next frame on.
        job.stage = m_baked ? EBakeStage::Handover : EBakeStage::Copy;
        job.handoverStart = params.frameNumber;
        job.handoverStartSec = Globals::time.getElapsedSec();
        job.progress = 0;
        return;
    }
    if (job.stage == EBakeStage::Handover)
    {
        // Nothing to dispatch: the march cross-fades to the new bake by the UBO's fade ("Far swap time", the same
        // clock reading as this frame's UBO). At 1 every ray shows the new bake.
        if (handoverFade() >= 1.0f)
            job.stage = EBakeStage::Copy;
        return;
    }
    if (job.stage != EBakeStage::Copy)
        return;

    // THE COPY: the new extinction into the density, a slice range per frame. Meanwhile the march reads only the new
    // bake (the fraction is 1), through the accumulation: nothing reads the density.
    {
        const uint32 slices = oc::min(job.copyPerFrame, m_slices - job.progress);
        if (slices > 0)
        {
            const vk::DescriptorSet set = m_copySets[frameIdx].getDescriptorSet();
            oc::array<DescriptorSetUpdateInfo, 2> updates{
                DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_accum.view) } },
                DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_density.view) } },
            };
            writeSet(set, updates);
            const ClearPC copyPc{ .sliceOffset = job.progress };
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_copyPipeline.getPipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_copyPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
            cmd.pushConstants(m_copyPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(copyPc), &copyPc);
            cmd.dispatch((m_angularRes + 7) / 8, (m_radialRes + 7) / 8, slices);
            job.progress += slices;
        }
        if (job.progress < m_slices)
            return;
    }
    // THE SWAP: the back floor / colour / max floor become the front, the density holds the new bake.
    const vk::MemoryBarrier2 copyToMarch{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &copyToMarch }); // copy -> march
    m_front = back;
    m_centre = job.centre;
    m_bakedSettings = job.settings;
    m_baked = true;
    m_lastBakeEnd = params.frameNumber;
    m_bakeDurationSec = Globals::time.getElapsedSec() - job.startSec;
    job.active = false;
}

void TreeVolumePipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params)
{
    const FarTreeParams& s = params.settings;
    if (m_angularRes == 0 || m_angularRes != s.angularRes || m_radialRes != s.radialRes || m_slices != s.slices)
        return; // prepare() did not run for these settings yet
    // THE BAKE, spread over frames: a running one goes on (a geometry setting changed under it: dropped); a new one
    // starts when due and NUM_FRAMES_IN_FLIGHT frames after the last one swapped (its snapshot buffers and the old
    // front - the new back - may still be read until then).
    const glm::vec2 camera(params.cameraPos.x, params.cameraPos.z);
    trackCamera(camera);
    if (m_job.active && !sameBake(s, m_job.settings) && !handingOver())
        m_job.active = false; // a hand-over finishes: the march already shows part of it
    // BAKE AHEAD: the test and the new centre take where the camera will be when a bake started now swaps.
    const glm::vec2 ahead = camera + bakeLead(s);
    const bool due = m_dirty || !m_baked || !sameBake(s, m_bakedSettings) || glm::distance(ahead, m_centre) > oc::max(s.rebakeDistance, 1.0f);
    if (!m_job.active && due && (!m_baked || params.frameNumber >= m_lastBakeEnd + RendererVKLayout::NUM_FRAMES_IN_FLIGHT))
        startBake(params, params.frameNumber, ahead);
    if (m_job.active)
        stepBake(cmd, frameIdx, params);

    // TEMPORAL OFF (no blend, no half res) is the plain march: its own variant, its own barriers, no other image
    // touched. On: the march at the temporal images' scale, the temporal pass, and at half res the upsample.
    // The scale and the pixel skip are BAKED into the variants: dispatch by what they were compiled with (prepare()
    // rebuilds them when the settings change), never by the live settings.
    const bool temporal = s.temporalPath() && m_hasTemporalImages && m_temporalScale == (s.halfRes ? 2u : 1u)
        && m_temporalBakedScale == m_temporalScale;
    if (!temporal && m_plainBakedSkip != 0 && !m_hasLatest)
        return; // a failed variant rebuild left a skipping march without its images: no far trees this frame
    const uint32 scale = temporal ? m_temporalScale : 1u;
    // PIXEL SKIP: under the temporal pass only the checkerboard (the pass reconstructs; 1 of 4 runs as 1 of 2); on the
    // plain path 1 of 2 or 1 of 4, the skipped pixels filled from the persistent latest-march images.
    const uint32 marchWidth = (m_width + scale - 1) / scale, marchHeight = (m_height + scale - 1) / scale;
    const int skipMode = temporal ? (m_temporalBakedChecker ? 1 : 0) : (int)m_plainBakedSkip;
    const bool checker = skipMode == 1;
    const bool quad = skipMode == 2;
    const bool plainSkip = !temporal && skipMode != 0;
    // A RESTART of the plain skip (the first frame, a resize, a mode change, a frame without it): the latest images hold
    // nothing current - cleared to "no trees, distance 0", which fails every copy test (the marched neighbour fills in).
    const bool restart = plainSkip && (m_lastPlainMarchFrame == UINT32_MAX || params.frameNumber != m_lastPlainMarchFrame + 1
        || m_lastSkipMode != skipMode);
    m_lastPlainMarchFrame = plainSkip ? params.frameNumber : UINT32_MAX;
    m_lastSkipMode = skipMode;
    if (restart)
    {
        const vk::MemoryBarrier2 toClear{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toClear });
        const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        cmd.clearColorImage(m_latest.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, { range });
        cmd.clearColorImage(m_latestDepth.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { range });
    }
    // This slot's output was last read by its apply (fragment, frames back): WAR before this frame's writes. With the
    // temporal pass (this frame or last), also its compute reads: the raw march, the history of the previous slot.
    // The plain pixel skip READS and WRITES the latest images, last written by last frame's march (or the clear).
    const bool computeSrc = temporal || m_temporalLastFrame || plainSkip;
    const vk::MemoryBarrier2 inputBarrier{
        // The clear's stage whenever its TRANSFER_WRITE access is listed (VUID-VkMemoryBarrier2-srcAccessMask-03915).
        .srcStageMask = computeSrc ? vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader
                                       | (plainSkip ? vk::PipelineStageFlagBits2::eClear : vk::PipelineStageFlags2{})
                                   : vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = plainSkip ? vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite
                                       | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eTransferWrite
                                   : vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = plainSkip ? vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead
                                   : vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier });
    m_temporalLastFrame = temporal;

    // The history counts when last frame ran the temporal pass too (the previous slot holds its result + distances).
    const uint32 prevIdx = (frameIdx + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const float historyWeight = temporal && m_lastMarchFrame != UINT32_MAX && params.frameNumber == m_lastMarchFrame + 1
        ? glm::clamp(s.temporalBlend, 0.0f, 0.98f) : 0.0f;
    m_lastMarchFrame = temporal ? params.frameNumber : UINT32_MAX;
    Image& marchColour = temporal ? m_raw : m_out[frameIdx];
    Image& marchDepth = temporal ? m_rawDepth : m_outDepth[frameIdx];
    // THE HAND-OVER variant reads both bakes - from the frame after it began (this frame's UBO must carry its fraction).
    const bool handover = handingOver() && m_job.handoverStart < params.frameNumber;
    ComputePipeline& march = temporal ? (handover ? m_marchTemporalHandoverPipeline : m_marchTemporalPipeline)
                                      : (handover ? m_marchHandoverPipeline : m_marchPipeline);
    const uint32 back = 1u - m_front;

    // The march reads the BAKED geometry (centre + the bake's settings) with this frame's shading settings.
    VolumeParamsGpu vol = volumeParams(m_bakedSettings, m_centre);
    vol.densityScale = s.densityScale;
    const MarchPC pc{
        .vol = vol,
        .width = marchWidth,
        .height = marchHeight,
        .stepScale = oc::max(s.stepScale, 0.05f),
        .startDistance = params.startDistance, // scaled with the camera's height (Renderer::farTreesStart)
        .maxSteps = s.maxSteps,
        .ambient = s.ambient,
        .shrink = oc::max(s.blobShrink, 0.0f),
        .overlap = oc::max(s.overlap, 1.0f),
        .sunScale = s.sunScale,
        .selfShadow = s.selfShadow,
        .normalStrength = s.normalStrength,
        .groundDark = s.groundDarkening,
        .forwardScatter = glm::clamp(s.forwardScatter, -0.95f, 0.95f),
        .albedoScale = s.albedoScale,
        .interiorShadow = oc::max(s.interiorShadow, 0.0f),
        .interiorRadius = oc::max(s.interiorRadius, 0.0f), // 0: the taps sit on the sample - no darkening
        .saturation = oc::max(s.saturationScale, 0.0f),
        .pad1 = 0,
        .fullSize = glm::uvec2(m_width, m_height),
    };
    const vk::DescriptorSet set = m_marchSets[frameIdx].getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 17> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.sceneDepthSampler, .imageView = params.sceneDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        DescriptorSetUpdateInfo{ .binding = 12, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floorMax[m_front].view) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_density.view) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_colour[m_front].view) } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(marchColour.view) } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(marchDepth.view) } },
        DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floor[m_front].view) } },
        // The latest-march images (the plain pixel skip; without them, any matching images - never read then).
        DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_hasLatest ? m_latest.view : m_out[prevIdx].view) } },
        DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_hasLatest ? m_latestDepth.view : m_outDepth[prevIdx].view) } },
        DescriptorSetUpdateInfo{ .binding = 10, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.skyMapSampler, params.skyMapView) } },
        DescriptorSetUpdateInfo{ .binding = 11, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.cloudShadowSampler, params.cloudShadowView) } },
        // The NEW bake (the hand-over variant; the plain one never reads them): its extinction in the accumulation's
        // float view, and the back floor / colour / max floor.
        DescriptorSetUpdateInfo{ .binding = 13, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_accumFloatView) } },
        DescriptorSetUpdateInfo{ .binding = 14, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floor[back].view) } },
        DescriptorSetUpdateInfo{ .binding = 15, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_colour[back].view) } },
        DescriptorSetUpdateInfo{ .binding = 16, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_floorMax[back].view) } },
    };
    writeSet(set, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, march.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, march.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    cmd.pushConstants(march.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
    // One thread per block: a horizontal pair (1 of 2) or a 2x2 block (1 of 4).
    const uint32 marchColumns = checker || quad ? (marchWidth + 1) / 2 : marchWidth;
    const uint32 marchRows = quad ? (marchHeight + 1) / 2 : marchHeight;
    cmd.dispatch((marchColumns + 7) / 8, (marchRows + 7) / 8, 1);

    // The result -> this frame's apply (fragment).
    const vk::MemoryBarrier2 toApply{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    if (!temporal)
    {
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toApply });
        return;
    }
    // The raw march -> the temporal pass; this also covers last frame's temporal writes (the history, earlier in
    // the queue).
    const vk::MemoryBarrier2 toTemporal{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toTemporal });

    // The temporal result: at full res the slot's own result image (also last frame's = the history colour); at half
    // res the slot's half-res history colour, which the upsample then turns into the full-res result.
    Image& accumColour = scale > 1 ? m_histColour[frameIdx] : m_out[frameIdx];
    Image& prevColour = scale > 1 ? m_histColour[prevIdx] : m_out[prevIdx];
    { // -------- Temporal (read raw + the previous slot's result, write this slot's) --------
        const vk::DescriptorSet tset = m_temporalSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 9> tupdates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, m_raw.view) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, m_rawDepth.view) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, prevColour.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, m_histDepth[prevIdx].view) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(accumColour.view) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_histDepth[frameIdx].view) } },
            DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.sceneDepthSampler, .imageView = params.sceneDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
            DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_outDepth[frameIdx].view) } },
        };
        writeSet(tset, tupdates);
        const TemporalPC tpc{ .viewIndex = RendererVKLayout::VIEW_CENTER, .width = marchWidth, .height = marchHeight, .historyWeight = historyWeight,
            .maxDist = vol.rMax };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_temporalPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_temporalPipeline.getPipelineLayout(), 0, 1, &tset, 0, nullptr);
        cmd.pushConstants(m_temporalPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(tpc), &tpc);
        cmd.dispatch((marchWidth + 7) / 8, (marchHeight + 7) / 8, 1);
    }
    if (scale > 1)
    {
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toTemporal }); // temporal -> upsample
        // -------- Upsample (the half-res result -> the slot's full-res pair) --------
        const vk::DescriptorSet uset = m_upsampleSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 6> uupdates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.sceneDepthSampler, .imageView = params.sceneDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, accumColour.view) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_screenSampler, m_histDepth[frameIdx].view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_out[frameIdx].view) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storageInfo(m_outDepth[frameIdx].view) } },
        };
        writeSet(uset, uupdates);
        const UpsamplePC upc{ .size = glm::uvec2(m_width, m_height), .maxDist = vol.rMax };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_upsamplePipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_upsamplePipeline.getPipelineLayout(), 0, 1, &uset, 0, nullptr);
        cmd.pushConstants(m_upsamplePipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(upc), &upc);
        cmd.dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toApply });
}

void TreeVolumePipeline::recordApply(CommandBuffer& commandBuffer, uint32 frameIdx)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const vk::DescriptorSet set = m_applySets[frameIdx].getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 1> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_out[frameIdx].view) } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_applyPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, set, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    cmd.draw(3, 1, 0, 0);
}
