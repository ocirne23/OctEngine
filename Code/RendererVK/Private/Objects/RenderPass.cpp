module RendererVK;

import :VK;
import :Device;
import :SwapChain;

RenderPass::RenderPass() {}
RenderPass::~RenderPass()
{
    if (m_renderPass)
        Globals::device.getDevice().destroyRenderPass(m_renderPass);
}

bool RenderPass::initialize(const SwapChain& swapChain)
{
    oc::array<vk::AttachmentDescription2, 2> attachments;
    attachments[0] = {
        .flags = vk::AttachmentDescriptionFlags(),
        .format = swapChain.getLayout().surfaceFormat.format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::ePresentSrcKHR,
    };
    attachments[1] = {
        .flags = vk::AttachmentDescriptionFlags(),
        .format = vk::Format::eD32Sfloat,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    };
    vk::AttachmentReference2 colorRef
    {
        .attachment = 0,
        .layout = vk::ImageLayout::eColorAttachmentOptimal,
    };
    vk::AttachmentReference2 depthRef{
        .attachment = 1,
        .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    };
    vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
        .pDepthStencilAttachment = &depthRef,
    };
    vk::PipelineStageFlags depthStages = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    oc::array<vk::SubpassDependency2, 2> dependencies
    {
        //vk::SubpassDependency2{
        //    .srcSubpass = vk::SubpassExternal,
        //    .dstSubpass = 0,
        //    .srcStageMask = vk::PipelineStageFlagBits::eComputeShader,
        //    .dstStageMask = vk::PipelineStageFlagBits::eDrawIndirect,
        //    .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        //    .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
        //},
         vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = depthStages,
            .dstStageMask = depthStages,
            .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        },
        vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,// | vk::PipelineStageFlagBits::eEarlyFragmentTests,
            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,// | vk::PipelineStageFlagBits::eEarlyFragmentTests,
            .srcAccessMask = vk::AccessFlags(),
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        }
    };
    vk::RenderPassCreateInfo2 renderPassInfo{
        .attachmentCount = static_cast<uint32>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = (uint32)dependencies.size(),
        .pDependencies = dependencies.data(),
    };

    auto createResult = Globals::device.getDevice().createRenderPass2(renderPassInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create renderpass\n");
        return false;
    }
    m_renderPass = createResult.value;

    return true;
}