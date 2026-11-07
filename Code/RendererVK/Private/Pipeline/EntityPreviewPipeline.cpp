module RendererVK;

import Core;
import Core.glm;
import File;
import :Allocator;
import :Device;
import :EntityPreviewPipeline;
import :GraphicsPipeline;
import :Layout;
import :TextureManager;

using namespace RendererVKLayout;

static constexpr vk::Format c_colorFormat = vk::Format::eR8G8B8A8Unorm; // the fragment shader tone maps and encodes: ImGui shows it as it is
static constexpr vk::Format c_depthFormat = vk::Format::eD32Sfloat;
static constexpr uint32 c_initialFrameVec4s = 1 + 4 * 256; // the camera + a 256-bone palette

// The descriptor bindings, mirrored in Shaders/Editor/entity_preview.*.glsl. The texture array is the highest binding: its count is variable.
static constexpr uint32 c_bindingMaterials = 0;
static constexpr uint32 c_bindingSkinning = 1;
static constexpr uint32 c_bindingFrame = 2;
static constexpr uint32 c_bindingTextures = 3;

namespace
{
// The two push blocks, mirrored in Shaders/Editor/*.glsl.
struct MeshPush
{
    glm::mat4 viewProj;
    glm::vec4 posScale;
    glm::vec4 quat;
    glm::vec4 tintMaterial;
    glm::ivec4 flags; // x = highlighted, y = skinning index delta (influence index = gl_VertexIndex + y), z = palette base in vec4s (-1: bind pose)
};
static_assert(sizeof(MeshPush) == 128);

struct GridPush
{
    glm::mat4 viewProj;
    glm::vec4 cameraPosY;
    glm::vec4 extents;
};
}

EntityPreviewPipeline::~EntityPreviewPipeline()
{
    if (!isInitialized())
        return;
    for (Target& target : m_targets)
        destroyTarget(target);
    Globals::device.getDevice().destroyRenderPass(m_renderPass);
}

void EntityPreviewPipeline::destroyTarget(Target& target)
{
    if (target.framebuffer == vk::Framebuffer(nullptr))
        return;
    vk::Device device = Globals::device.getDevice();
    device.destroyFramebuffer(target.framebuffer);
    device.destroyImageView(target.colorView);
    device.destroyImageView(target.depthView);
    Globals::gpuAllocator.destroyImage(target.colorImage, target.colorMemory);
    Globals::gpuAllocator.destroyImage(target.depthImage, target.depthMemory);
    for (Buffer& buffer : target.frameData)
        buffer.destroy();
    target.framebuffer = vk::Framebuffer(nullptr);
}

void EntityPreviewPipeline::createRenderPass()
{
    const oc::array<vk::AttachmentDescription2, 2> attachments{
        vk::AttachmentDescription2{
            .format = c_colorFormat,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::AttachmentDescription2{
            .format = c_depthFormat,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        },
    };
    const vk::AttachmentReference2 colorRef{ .attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eColor };
    const vk::AttachmentReference2 depthRef{ .attachment = 1, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eDepth };
    const vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
        .pDepthStencilAttachment = &depthRef,
    };
    const oc::array<vk::SubpassDependency2, 2> dependencies{
        vk::SubpassDependency2{ // the previous frame's sampled read and this target's own last use -> this pass
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eLateFragmentTests,
            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
            .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead,
        },
        vk::SubpassDependency2{ // writes -> the ImGui draw's sampled read, later in the swapchain batch
            .srcSubpass = 0,
            .dstSubpass = vk::SubpassExternal,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        },
    };
    const vk::RenderPassCreateInfo2 info{
        .attachmentCount = (uint32)attachments.size(),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = (uint32)dependencies.size(),
        .pDependencies = dependencies.data(),
    };
    auto result = Globals::device.getDevice().createRenderPass2(info);
    if (result.result != vk::Result::eSuccess) { assert(false && "entity preview renderpass"); return; }
    m_renderPass = result.value;
    Globals::device.setDebugName(m_renderPass, "EntityPreview");
}

void EntityPreviewPipeline::createTarget(uint32 targetIdx)
{
    if (!isInitialized() || targetIdx >= PREVIEW_TARGET_COUNT || hasTarget(targetIdx))
        return;
    Target& target = m_targets[targetIdx];
    const char* colorName = targetIdx == 0 ? "EntityPreviewColor" : "AnimationPreviewColor";
    const char* depthName = targetIdx == 0 ? "EntityPreviewDepth" : "AnimationPreviewDepth";

    vk::Device device = Globals::device.getDevice();
    auto createImage = [&](vk::Format format, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect, const char* name, vk::Image& image, VmaAllocation& memory, vk::ImageView& view)
    {
        const vk::ImageCreateInfo imageInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = { TARGET_SIZE, TARGET_SIZE, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        if (!Globals::gpuAllocator.createImage(imageInfo, image, memory, name))
        {
            assert(false && "entity preview image");
            return;
        }
        const vk::ImageViewCreateInfo viewInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = { aspect, 0, 1, 0, 1 },
        };
        auto viewResult = device.createImageView(viewInfo);
        if (viewResult.result != vk::Result::eSuccess) { assert(false && "entity preview view"); return; }
        view = viewResult.value;
        Globals::device.setDebugName(view, name);
    };
    createImage(c_colorFormat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, vk::ImageAspectFlagBits::eColor,
        colorName, target.colorImage, target.colorMemory, target.colorView);
    createImage(c_depthFormat, vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth,
        depthName, target.depthImage, target.depthMemory, target.depthView);

    const oc::array<vk::ImageView, 2> views{ target.colorView, target.depthView };
    const vk::FramebufferCreateInfo framebufferInfo{
        .renderPass = m_renderPass,
        .attachmentCount = (uint32)views.size(),
        .pAttachments = views.data(),
        .width = TARGET_SIZE,
        .height = TARGET_SIZE,
        .layers = 1,
    };
    auto framebufferResult = device.createFramebuffer(framebufferInfo);
    if (framebufferResult.result != vk::Result::eSuccess) { assert(false && "entity preview framebuffer"); return; }
    target.framebuffer = framebufferResult.value;
    Globals::device.setDebugName(target.framebuffer, targetIdx == 0 ? "EntityPreview" : "AnimationPreview");

    for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        target.sets[i].initialize(m_meshPipeline.getDescriptorSetLayout(), "EntityPreview", m_numTextureDescriptors);
        target.writtenTextures[i] = UINT32_MAX;
    }
}

void EntityPreviewPipeline::buildMeshLayout(GraphicsPipelineLayout& layout, uint32 maxTextures)
{
    layout.vertexShader.debugFilePath = "Shaders/Editor/entity_preview.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/Editor/entity_preview.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.indirectBindable = false;
    layout.cullMode = vk::CullModeFlagBits::eNone; // the shader flips the normal: an asset's open back faces show
    layout.depthCompareOp = vk::CompareOp::eGreater; // reversed-Z, like the main view

    layout.vertexLayoutInfo.bindingDescriptions.push_back(vk::VertexInputBindingDescription{
        .binding = 0, .stride = sizeof(MeshVertex), .inputRate = vk::VertexInputRate::eVertex });
    auto& attributes = layout.vertexLayoutInfo.attributeDescriptions;
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 0, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(MeshVertex, positionU) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 1, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(MeshVertex, normalV) });
    attributes.push_back(vk::VertexInputAttributeDescription{ .location = 2, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(MeshVertex, tangent) });

    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = c_bindingMaterials, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = c_bindingSkinning, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = c_bindingFrame, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = c_bindingTextures, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound
        | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;

    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(MeshPush) });

    PipelineVariant wireframe;
    wireframe.polygonMode = vk::PolygonMode::eLine;
    wireframe.cullMode = vk::CullModeFlagBits::eNone;
    layout.additionalVariants.push_back(wireframe);
}

void EntityPreviewPipeline::buildGridLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/Editor/entity_preview_grid.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/Editor/entity_preview_grid.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.indirectBindable = false;
    layout.cullMode = vk::CullModeFlagBits::eNone;
    layout.blendEnable = true;
    layout.colorWriteAlpha = false; // ImGui reads the target's alpha: it stays opaque
    layout.depthCompareOp = vk::CompareOp::eGreater;
    layout.depthWriteEnable = false;
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(GridPush) });
}

void EntityPreviewPipeline::initialize(uint32 maxTextures, uint32 numTextureDescriptors)
{
    createRenderPass();
    m_textureSampler.initialize();
    m_colorSampler.initialize(vk::SamplerAddressMode::eClampToEdge, 1.0f);
    m_numTextureDescriptors = numTextureDescriptors;

    GraphicsPipelineLayout meshLayout;
    buildMeshLayout(meshLayout, maxTextures);
    m_meshPipeline.initialize(m_renderPass, meshLayout);

    GraphicsPipelineLayout gridLayout;
    buildGridLayout(gridLayout);
    m_gridPipeline.initialize(m_renderPass, gridLayout);
}

void EntityPreviewPipeline::reloadShaders(uint32 maxTextures)
{
    if (!isInitialized())
        return;
    GraphicsPipelineLayout meshLayout;
    buildMeshLayout(meshLayout, maxTextures);
    if (!m_meshPipeline.reloadShaders(m_renderPass, meshLayout))
        printf("EntityPreviewPipeline: mesh shader reload failed, keeping previous pipeline\n");
    GraphicsPipelineLayout gridLayout;
    buildGridLayout(gridLayout);
    if (!m_gridPipeline.reloadShaders(m_renderPass, gridLayout))
        printf("EntityPreviewPipeline: grid shader reload failed, keeping previous pipeline\n");
}

void EntityPreviewPipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    if (!isInitialized())
        return;
    m_numTextureDescriptors = numTextureDescriptors;
    for (Target& target : m_targets)
    {
        if (target.framebuffer == vk::Framebuffer(nullptr))
            continue;
        for (uint32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
        {
            target.sets[i].initialize(m_meshPipeline.getDescriptorSetLayout(), "EntityPreview", numTextureDescriptors);
            target.writtenTextures[i] = UINT32_MAX;
        }
    }
}

void EntityPreviewPipeline::invalidateTextures(uint32 frameIdx)
{
    for (Target& target : m_targets)
        target.writtenTextures[frameIdx] = UINT32_MAX;
}

void EntityPreviewPipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    if (!isInitialized())
        return;
    const vk::DescriptorImageInfo imageInfo{
        .sampler = m_textureSampler.getSampler(),
        .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    for (Target& target : m_targets)
    {
        if (target.framebuffer == vk::Framebuffer(nullptr) || target.writtenTextures[frameIdx] == UINT32_MAX)
            continue; // a full rewrite is pending anyway
        const vk::WriteDescriptorSet write{ .dstSet = target.sets[frameIdx].getDescriptorSet(),
            .dstBinding = c_bindingTextures, .dstArrayElement = slotIdx, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
        Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
    }
}

uint32 EntityPreviewPipeline::writeFrameData(Target& target, uint32 frameIdx, const EntityPreviewFrame& frame)
{
    const uint32 needed = 1 + 4 * (uint32)frame.palette.size();
    Buffer& buffer = target.frameData[frameIdx];
    if (target.frameDataVec4s[frameIdx] < needed)
    {
        // This slot's fence was waited: nothing reads the old buffer any more.
        const uint32 capacity = glm::max(needed + needed / 2, c_initialFrameVec4s);
        buffer.initialize(capacity * sizeof(glm::vec4), vk::BufferUsageFlagBits2::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible, false,
            "PreviewFrameData", BufferHostAccess::eSequentialWrite);
        target.frameDataVec4s[frameIdx] = capacity;
    }
    oc::span<glm::vec4> mapped = buffer.mapMemory<glm::vec4>();
    mapped[0] = glm::vec4(frame.cameraPos, 0.0f);
    if (!frame.palette.empty())
        memcpy(&mapped[1], frame.palette.data(), frame.palette.size() * sizeof(glm::mat4));
    buffer.flushMappedMemory(needed * sizeof(glm::vec4));
    return needed;
}

void EntityPreviewPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const EntityPreviewFrame& frame, oc::span<const MeshInfo> meshInfos,
    Buffer& vertexBuffer, Buffer& indexBuffer, Buffer& materialBuffer, Buffer& skinningBuffer)
{
    const uint32 targetIdx = (uint32)frame.target;
    if (targetIdx >= PREVIEW_TARGET_COUNT || !hasTarget(targetIdx))
        return;
    Target& target = m_targets[targetIdx];

    const uint32 width = glm::clamp(frame.size.x, 1u, TARGET_SIZE);
    const uint32 height = glm::clamp(frame.size.y, 1u, TARGET_SIZE);
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    vk::DescriptorSet vkSet = target.sets[frameIdx].getDescriptorSet();

    const uint32 frameVec4s = writeFrameData(target, frameIdx, frame);
    const uint32 paletteBones = (uint32)frame.palette.size();

    // The material table's and the skinning buffer's handles can change when they grow, and textures join the array without a capacity change.
    const uint32 numTextures = (uint32)Globals::textureManager.getNumTextures();
    const bool writeTextures = target.writtenTextures[frameIdx] != numTextures;
    oc::array<DescriptorSetUpdateInfo, 4> updates{
        DescriptorSetUpdateInfo{ .binding = c_bindingMaterials, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = materialBuffer.getBuffer(), .range = materialBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = c_bindingSkinning, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = skinningBuffer.getBuffer(), .range = skinningBuffer.getSize() } } },
        DescriptorSetUpdateInfo{ .binding = c_bindingFrame, .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = target.frameData[frameIdx].getBuffer(), .range = frameVec4s * sizeof(glm::vec4) } } },
        DescriptorSetUpdateInfo{ .binding = c_bindingTextures, .type = vk::DescriptorType::eCombinedImageSampler },
    };
    if (writeTextures)
    {
        updates[3].imageInfos.reserve(numTextures);
        for (uint32 texIdx = 0; texIdx < numTextures; ++texIdx)
            updates[3].imageInfos.push_back(vk::DescriptorImageInfo{
                .sampler = m_textureSampler.getSampler(),
                .imageView = Globals::textureManager.getViewForDescriptor((uint16)texIdx), // freed slots -> fallback
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
        target.writtenTextures[frameIdx] = numTextures;
    }
    commandBuffer.cmdUpdateDescriptorSets(m_meshPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet,
        oc::span<DescriptorSetUpdateInfo>(updates.data(), writeTextures ? 4 : 3));

    const oc::array<vk::ClearValue, 2> clears{
        vk::ClearValue{ vk::ClearColorValue{ std::array<float, 4>{ 0.085f, 0.09f, 0.1f, 1.0f } } },
        vk::ClearValue{ vk::ClearDepthStencilValue{ 0.0f, 0 } }, // reversed-Z
    };
    const vk::RenderPassBeginInfo begin{
        .renderPass = m_renderPass,
        .framebuffer = target.framebuffer,
        .renderArea = { vk::Offset2D{ 0, 0 }, vk::Extent2D{ width, height } },
        .clearValueCount = (uint32)clears.size(),
        .pClearValues = clears.data(),
    };
    cmd.beginRenderPass(begin, vk::SubpassContents::eInline);
    const vk::Viewport viewport{ .x = 0.0f, .y = 0.0f, .width = (float)width, .height = (float)height, .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ width, height } };
    cmd.setViewport(0, { viewport });
    cmd.setScissor(0, { scissor });

    if (!frame.meshes.empty())
    {
        const vk::PipelineLayout pipelineLayout = m_meshPipeline.getPipelineLayout();
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_meshPipeline.getPipelineVariant(frame.wireframe ? 1 : 0));
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &vkSet, 0, nullptr);
        const vk::Buffer vkVertexBuffer = vertexBuffer.getBuffer();
        const vk::DeviceSize vertexOffset = 0;
        cmd.bindVertexBuffers(0, 1, &vkVertexBuffer, &vertexOffset);
        cmd.bindIndexBuffer(indexBuffer.getBuffer(), 0, vk::IndexType::eUint32);

        for (const EntityPreviewMesh& mesh : frame.meshes)
        {
            uint32 indexCount = mesh.indexCount;
            uint32 firstIndex = mesh.firstIndex;
            int32 meshVertexOffset = mesh.vertexOffset;
            if (mesh.meshIdx != UINT16_MAX)
            {
                if (mesh.meshIdx >= meshInfos.size())
                    continue;
                const MeshInfo& info = meshInfos[mesh.meshIdx]; // a streamed-out mesh has indexCount 0: nothing until it is back
                indexCount = info.indexCount;
                firstIndex = info.firstIndex;
                meshVertexOffset = info.vertexOffset;
            }
            if (indexCount == 0)
                continue;
            // Skinned only with influences and a palette in range: otherwise the bind pose, like before skinning existed here.
            const bool skinned = mesh.skinVertexOffset != UINT32_MAX && mesh.paletteOffset != UINT32_MAX && mesh.paletteOffset < paletteBones;
            const MeshPush push{
                .viewProj = frame.viewProj,
                .posScale = glm::vec4(mesh.world.pos, mesh.world.scale),
                .quat = glm::vec4(mesh.world.quat.x, mesh.world.quat.y, mesh.world.quat.z, mesh.world.quat.w),
                .tintMaterial = glm::vec4(mesh.tint, float(mesh.materialIdx)),
                .flags = glm::ivec4(mesh.highlighted ? 1 : 0,
                    skinned ? int32(mesh.skinVertexOffset) - meshVertexOffset : 0,
                    skinned ? int32(1 + 4 * mesh.paletteOffset) : -1, 0),
            };
            cmd.pushConstants(pipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
            cmd.drawIndexed(indexCount, 1, firstIndex, meshVertexOffset, 0);
        }
    }

    if (frame.grid)
    {
        const float height0 = glm::abs(frame.cameraPos.y - frame.gridY);
        const float fadeDistance = glm::max(height0, 1.0f) * 40.0f;
        const GridPush push{
            .viewProj = frame.viewProj,
            .cameraPosY = glm::vec4(frame.cameraPos, frame.gridY),
            .extents = glm::vec4(fadeDistance, fadeDistance, 0.0f, 0.0f),
        };
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_gridPipeline.getPipeline());
        cmd.pushConstants(m_gridPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmd.draw(6, 1, 0, 0);
    }

    cmd.endRenderPass();
}
