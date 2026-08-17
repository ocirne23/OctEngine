module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :Layout;

namespace
{
    constexpr vk::Format SPECTRUM_FORMAT = vk::Format::eR32G32B32A32Sfloat; // 2 complex values per texel
    constexpr vk::Format MAPS_FORMAT = vk::Format::eR16G16B16A16Sfloat;

    struct FftPC { uint32 axis; };        // 0 = horizontal (rows), 1 = vertical (columns)
    struct FoamPC { uint32 writeLayer; }; // foam ping/pong: write this layer, read the other

    vk::ImageSubresourceRange allSubresources(uint32 mipLevels, uint32 layers)
    {
        return { vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, layers };
    }
}

OceanSimulationPipeline::~OceanSimulationPipeline()
{
    destroyImages();
}

void OceanSimulationPipeline::destroyImages()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < 2; ++i)
    {
        if (m_spectrumView[i]) vkDevice.destroyImageView(m_spectrumView[i]);
        Globals::gpuAllocator.destroyImage(m_spectrumImage[i], m_spectrumMemory[i]);
        m_spectrumView[i] = nullptr; m_spectrumImage[i] = nullptr; m_spectrumMemory[i] = nullptr;
    }
    if (m_mapsView)     vkDevice.destroyImageView(m_mapsView);
    if (m_mapsMip0View) vkDevice.destroyImageView(m_mapsMip0View);
    Globals::gpuAllocator.destroyImage(m_mapsImage, m_mapsMemory);
    m_mapsView = nullptr; m_mapsMip0View = nullptr; m_mapsImage = nullptr; m_mapsMemory = nullptr;
    if (m_foamView) vkDevice.destroyImageView(m_foamView);
    Globals::gpuAllocator.destroyImage(m_foamImage, m_foamMemory);
    m_foamView = nullptr; m_foamImage = nullptr; m_foamMemory = nullptr;
}

void OceanSimulationPipeline::buildSpectrumLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ocean_spectrum.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void OceanSimulationPipeline::buildFftLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ocean_fft.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(FftPC) });
}

void OceanSimulationPipeline::buildAssembleLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ocean_assemble.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void OceanSimulationPipeline::buildFoamLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ocean_foam.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(FoamPC) });
}

void OceanSimulationPipeline::createImages()
{
    vk::Device vkDevice = Globals::device.getDevice();

    for (uint32 i = 0; i < 2; ++i)
    {
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = SPECTRUM_FORMAT,
            .extent = { N, N, 1 },
            .mipLevels = 1,
            .arrayLayers = SPECTRUM_LAYERS,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, m_spectrumImage[i], m_spectrumMemory[i], "OceanSpectrum");
        vk::ImageViewCreateInfo viewInfo{
            .image = m_spectrumImage[i],
            .viewType = vk::ImageViewType::e2DArray,
            .format = SPECTRUM_FORMAT,
            .subresourceRange = allSubresources(1, SPECTRUM_LAYERS),
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        m_spectrumView[i] = viewResult.value;
    }

    m_mapsMipLevels = 1;
    for (uint32 s = N; s > 1; s >>= 1)
        ++m_mapsMipLevels;
    vk::ImageCreateInfo mapsInfo{
        .imageType = vk::ImageType::e2D,
        .format = MAPS_FORMAT,
        .extent = { N, N, 1 },
        .mipLevels = m_mapsMipLevels,
        .arrayLayers = MAPS_LAYERS,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(mapsInfo, m_mapsImage, m_mapsMemory, "OceanMaps");
    vk::ImageViewCreateInfo mapsViewInfo{
        .image = m_mapsImage,
        .viewType = vk::ImageViewType::e2DArray,
        .format = MAPS_FORMAT,
        .subresourceRange = allSubresources(m_mapsMipLevels, MAPS_LAYERS),
    };
    auto mapsViewResult = vkDevice.createImageView(mapsViewInfo);
    assert(mapsViewResult.result == vk::Result::eSuccess);
    m_mapsView = mapsViewResult.value;
    mapsViewInfo.subresourceRange.levelCount = 1;
    auto mip0Result = vkDevice.createImageView(mapsViewInfo);
    assert(mip0Result.result == vk::Result::eSuccess);
    m_mapsMip0View = mip0Result.value;

    // Persistent foam coverage mask over cascade 0's patch. Two layers = ping/pong: the foam pass reads
    // last frame's layer through a small diffusion tent and writes the other (frame slots alternate
    // strictly, so the layer roles just follow frameIdx & 1). The diffusion is what actually removes
    // texel structure from the mask — a sampling-side blur gets undone by the dissolve threshold.
    vk::ImageCreateInfo foamInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16Sfloat,
        .extent = { N, N, 1 },
        .mipLevels = 1,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(foamInfo, m_foamImage, m_foamMemory, "OceanFoam");
    vk::ImageViewCreateInfo foamViewInfo{
        .image = m_foamImage,
        .viewType = vk::ImageViewType::e2DArray,
        .format = vk::Format::eR16Sfloat,
        .subresourceRange = allSubresources(1, 2),
    };
    auto foamViewResult = vkDevice.createImageView(foamViewInfo);
    assert(foamViewResult.result == vk::Result::eSuccess);
    m_foamView = foamViewResult.value;

    // One-time layout init: the spectra live in GENERAL forever; the maps rest in SHADER_READ_ONLY
    // between frames so the draw passes can sample them even before the first simulation ran; the foam
    // accumulator is cleared to zero (its previous-frame read feeds back into itself).
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer cmd = init.begin(true);
        oc::array<vk::ImageMemoryBarrier2, 4> barriers;
        for (uint32 i = 0; i < 2; ++i)
        {
            barriers[i] = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = m_spectrumImage[i],
                .subresourceRange = allSubresources(1, SPECTRUM_LAYERS),
            };
        }
        barriers[2] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = m_mapsImage,
            .subresourceRange = allSubresources(m_mapsMipLevels, MAPS_LAYERS),
        };
        barriers[3] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = m_foamImage,
            .subresourceRange = allSubresources(1, 2),
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)barriers.size(), .pImageMemoryBarriers = barriers.data() });

        const vk::ClearColorValue zero{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }; // vk::ClearColorValue is spelled in std::array
        const vk::ImageSubresourceRange foamRange = allSubresources(1, 2);
        cmd.clearColorImage(m_foamImage, vk::ImageLayout::eTransferDstOptimal, &zero, 1, &foamRange);

        vk::ImageMemoryBarrier2 foamToGeneral{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = m_foamImage,
            .subresourceRange = allSubresources(1, 2),
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &foamToGeneral });
        init.end();
        init.submitGraphics();
        (void)Globals::device.getGraphicsQueue().waitIdle();
    }
}

void OceanSimulationPipeline::initialize()
{
    m_mapsSampler.initialize(); // repeat + trilinear + aniso: exactly what tiled, mipped water maps want
    createImages();

    // Displacement readback: per frame slot, GPU-written (transfer dst), host-read for buoyancy.
    // Coherent so the fence wait alone makes the copy visible (no invalidate path in Buffer).
    // Zeroed so pre-first-simulation reads decode as flat water instead of garbage waves.
    constexpr vk::DeviceSize readbackBytes = (vk::DeviceSize)READBACK_RES * READBACK_RES * CASCADES * 4 * sizeof(uint16);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        (void)m_readbackBuffers[i].initialize(readbackBytes, vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, false, "OceanReadback");
        m_readbackMapped[i] = m_readbackBuffers[i].mapMemory();
        memset(m_readbackMapped[i].data(), 0, m_readbackMapped[i].size());
    }

    ComputePipelineLayout spectrumLayout;
    buildSpectrumLayout(spectrumLayout);
    m_spectrumPipeline.initialize(spectrumLayout);
    ComputePipelineLayout fftLayout;
    buildFftLayout(fftLayout);
    m_fftPipeline.initialize(fftLayout);
    ComputePipelineLayout assembleLayout;
    buildAssembleLayout(assembleLayout);
    m_assemblePipeline.initialize(assembleLayout);
    ComputePipelineLayout foamLayout;
    buildFoamLayout(foamLayout);
    m_foamPipeline.initialize(foamLayout);

    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_spectrumSets[i].initialize(m_spectrumPipeline.getDescriptorSetLayout());
        m_fftHorizontalSets[i].initialize(m_fftPipeline.getDescriptorSetLayout());
        m_fftVerticalSets[i].initialize(m_fftPipeline.getDescriptorSetLayout());
        m_assembleSets[i].initialize(m_assemblePipeline.getDescriptorSetLayout());
        m_foamSets[i].initialize(m_foamPipeline.getDescriptorSetLayout());
    }
}

void OceanSimulationPipeline::reloadShaders()
{
    ComputePipelineLayout spectrumLayout;
    buildSpectrumLayout(spectrumLayout);
    if (!m_spectrumPipeline.reloadShaders(spectrumLayout))
        printf("OceanSimulationPipeline: spectrum shader reload failed, keeping previous pipeline\n");
    ComputePipelineLayout fftLayout;
    buildFftLayout(fftLayout);
    if (!m_fftPipeline.reloadShaders(fftLayout))
        printf("OceanSimulationPipeline: fft shader reload failed, keeping previous pipeline\n");
    ComputePipelineLayout assembleLayout;
    buildAssembleLayout(assembleLayout);
    if (!m_assemblePipeline.reloadShaders(assembleLayout))
        printf("OceanSimulationPipeline: assemble shader reload failed, keeping previous pipeline\n");
    ComputePipelineLayout foamLayout;
    buildFoamLayout(foamLayout);
    if (!m_foamPipeline.reloadShaders(foamLayout))
        printf("OceanSimulationPipeline: foam shader reload failed, keeping previous pipeline\n");
}

void OceanSimulationPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    const auto storageImage = [](vk::ImageView view) {
        return DescriptorSetUpdateInfo{ .type = vk::DescriptorType::eStorageImage,
            .imageInfos = { vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral } } };
    };

    // Compute -> compute execution barrier between the simulation stages (all resources stay GENERAL).
    // Sampled read included: the foam pass samples the maps' (previous-frame) mips through a sampler.
    const auto computeBarrier = [&cmd]() {
        vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    };

    { // Maps: sampled last frame -> compute-accessible (assemble writes mip 0, foam samples the mips).
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = m_mapsImage,
            .subresourceRange = allSubresources(m_mapsMipLevels, MAPS_LAYERS),
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier });
    }

    // ---- 1. Spectrum: TMA h0(k) + time evolution -> packed complex spectra (ping) ----
    {
        oc::array<DescriptorSetUpdateInfo, 2> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            storageImage(m_spectrumView[0]),
        };
        updates[1].binding = 1;
        vk::DescriptorSet set = m_spectrumSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_spectrumPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_spectrumPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_spectrumPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.dispatch(N / 8, N / 8, CASCADES);
    }
    computeBarrier();

    // ---- 2. Inverse FFT: horizontal (ping -> pong), then vertical (pong -> ping) ----
    {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_fftPipeline.getPipeline());

        oc::array<DescriptorSetUpdateInfo, 2> horizontal{ storageImage(m_spectrumView[0]), storageImage(m_spectrumView[1]) };
        horizontal[0].binding = 0; horizontal[1].binding = 1;
        vk::DescriptorSet hSet = m_fftHorizontalSets[frameIdx].getDescriptorSet();
        commandBuffer.cmdUpdateDescriptorSets(m_fftPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, hSet, horizontal);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_fftPipeline.getPipelineLayout(), 0, 1, &hSet, 0, nullptr);
        FftPC pc{ .axis = 0 };
        cmd.pushConstants(m_fftPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(FftPC), &pc);
        cmd.dispatch(N, SPECTRUM_LAYERS, 1); // one workgroup per row per layer
        computeBarrier();

        oc::array<DescriptorSetUpdateInfo, 2> vertical{ storageImage(m_spectrumView[1]), storageImage(m_spectrumView[0]) };
        vertical[0].binding = 0; vertical[1].binding = 1;
        vk::DescriptorSet vSet = m_fftVerticalSets[frameIdx].getDescriptorSet();
        commandBuffer.cmdUpdateDescriptorSets(m_fftPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vSet, vertical);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_fftPipeline.getPipelineLayout(), 0, 1, &vSet, 0, nullptr);
        pc.axis = 1;
        cmd.pushConstants(m_fftPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(FftPC), &pc);
        cmd.dispatch(N, SPECTRUM_LAYERS, 1);
        computeBarrier();
    }

    // ---- 3. Assemble: permute sign, unpack the 8 signals -> displacement/gradient/moments maps (mip 0) ----
    {
        oc::array<DescriptorSetUpdateInfo, 2> updates{ storageImage(m_spectrumView[0]), storageImage(m_mapsMip0View) };
        updates[0].binding = 0; updates[1].binding = 1;
        vk::DescriptorSet set = m_assembleSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_assemblePipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_assemblePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_assemblePipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        cmd.dispatch(N / 8, N / 8, CASCADES);
    }
    computeBarrier();

    // ---- 4. Foam: temporal whitecap COVERAGE accumulation (inject on Jacobian folding, decay, stash in
    // moments[c0].w). The injection Jacobian samples the maps mip-filtered (binding 3; image is GENERAL
    // here) so a 1.5m mask texel reads each cascade's AVERAGE local folding, not one aliased full-res
    // sample. The crisp foam edges come from noise erosion in the water shader, not from this mask. ----
    {
        oc::array<DescriptorSetUpdateInfo, 4> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
                .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            storageImage(m_mapsMip0View),
            storageImage(m_foamView),
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_mapsSampler.getSampler(), .imageView = m_mapsView, .imageLayout = vk::ImageLayout::eGeneral } } },
        };
        updates[1].binding = 1; updates[2].binding = 2;
        vk::DescriptorSet set = m_foamSets[frameIdx].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_foamPipeline.getPipeline());
        commandBuffer.cmdUpdateDescriptorSets(m_foamPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_foamPipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
        // Ping/pong: write layer frameIdx & 1, read (diffuse) the other — frame slots alternate strictly.
        FoamPC foamPc{ .writeLayer = frameIdx & 1u };
        cmd.pushConstants(m_foamPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(FoamPC), &foamPc);
        cmd.dispatch(N / 8, N / 8, 1);
    }

    // ---- 5. Mip chain: blit-downsample so distant water reads prefiltered displacement/slopes ----
    {
        vk::ImageMemoryBarrier2 toSrc{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = m_mapsImage,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, MAPS_LAYERS },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toSrc });

        for (uint32 mip = 1; mip < m_mapsMipLevels; ++mip)
        {
            vk::ImageMemoryBarrier2 toDst{
                .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
                .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = m_mapsImage,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, mip, 1, 0, MAPS_LAYERS },
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toDst });

            const int32 srcSize = (int32)(N >> (mip - 1));
            const int32 dstSize = (int32)(N >> mip);
            vk::ImageBlit2 blit{
                .srcSubresource = { vk::ImageAspectFlagBits::eColor, mip - 1, 0, MAPS_LAYERS },
                .dstSubresource = { vk::ImageAspectFlagBits::eColor, mip, 0, MAPS_LAYERS },
            };
            blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.srcOffsets[1] = vk::Offset3D{ srcSize, srcSize, 1 };
            blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.dstOffsets[1] = vk::Offset3D{ oc::max(dstSize, 1), oc::max(dstSize, 1), 1 };
            vk::BlitImageInfo2 blitInfo{
                .srcImage = m_mapsImage, .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
                .dstImage = m_mapsImage, .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = 1, .pRegions = &blit, .filter = vk::Filter::eLinear,
            };
            cmd.blitImage2(blitInfo);

            vk::ImageMemoryBarrier2 dstToSrc{
                .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = m_mapsImage,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, mip, 1, 0, MAPS_LAYERS },
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &dstToSrc });
        }

        // ---- CPU readback: displacement layers' mip READBACK_MIP -> this slot's host buffer (buoyancy).
        // The mip chain left every level TransferSrcOptimal, so the copy slots in before the final
        // transition; the host barrier pairs with beginFrame's fence wait on this slot. ----
        {
            // The mip-chain barriers only chained Blit->Blit; the buffer copy reads in the COPY stage.
            vk::ImageMemoryBarrier2 blitToCopy{
                .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = m_mapsImage,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, READBACK_MIP, 1, 0, CASCADES },
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &blitToCopy });

            const vk::BufferImageCopy region{
                .imageSubresource = { vk::ImageAspectFlagBits::eColor, READBACK_MIP, 0, CASCADES },
                .imageExtent = { READBACK_RES, READBACK_RES, 1 },
            };
            cmd.copyImageToBuffer(m_mapsImage, vk::ImageLayout::eTransferSrcOptimal,
                m_readbackBuffers[frameIdx].getBuffer(), 1, &region);
            vk::BufferMemoryBarrier2 toHost{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                .buffer = m_readbackBuffers[frameIdx].getBuffer(),
                .size = vk::WholeSize,
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &toHost });
        }

        vk::ImageMemoryBarrier2 toSampled{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = m_mapsImage,
            .subresourceRange = allSubresources(m_mapsMipLevels, MAPS_LAYERS),
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toSampled });
    }
}
