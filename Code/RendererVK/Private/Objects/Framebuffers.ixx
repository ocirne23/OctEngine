export module RendererVK:Framebuffers;

import Core;
import :VK;
import :Allocator;

export class RenderPass;
export class SwapChain;

export class Framebuffers final
{
public:

    Framebuffers();
    ~Framebuffers();
    Framebuffers(const Framebuffers&) = delete;

    bool initialize(const RenderPass& renderPass, const SwapChain& swapChain);

    const oc::vector<vk::Framebuffer>& getFramebuffers() const { return m_framebuffers; }
    vk::Framebuffer getFramebuffer(int index) { return m_framebuffers[index]; }

private:

    oc::vector<vk::Framebuffer> m_framebuffers;
    oc::vector<vk::ImageView> m_imageViews;
    VmaAllocation m_depthImageMemory = nullptr;
    vk::Image m_depthImage;
    vk::ImageView m_depthImageView;
};