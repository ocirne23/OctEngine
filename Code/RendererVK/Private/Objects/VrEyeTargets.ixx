export module RendererVK:VrEyeTargets;

import Core;
import :VK;
import :Allocator;
import :DescriptorSet;

// VR ONLY: the two per-eye LDR composite targets the tonemap pass writes and present() copies into the
// OpenXR eye swapchains. They are not part of the frame-slot rotation - one pair for the whole
// renderer - and they are re-created with the swapchain, so every surface/swapchain recreate calls
// create() again (a no-op outside VR).
//
// The depth image is SHARED between the eyes and never read: the swapchain render pass declares a depth
// attachment, so the framebuffers must provide one, but the composite draw does not use it.
export class VrEyeTargets final
{
public:
    // Allocates the descriptor sets once, at renderer init. create() may then run many times.
    void initialize(vk::DescriptorSetLayout compositeLayout);
    // Re-creates both eyes' color image/view/framebuffer and the shared depth for `extent`. Destroys
    // whatever was there first, so it is safe to call on every swapchain change.
    void create(vk::RenderPass swapchainRenderPass, vk::Extent2D extent, vk::Format colorFormat);
    void destroy();

    vk::Image getColorImage(uint32 eye) const { return m_colorImage[eye]; }
    vk::Framebuffer getFramebuffer(uint32 eye) const { return m_framebuffer[eye]; }
    DescriptorSet& getCompositeSet(uint32 eye) { return m_compositeSet[eye]; }

private:
    oc::array<vk::Image, 2> m_colorImage{};
    oc::array<VmaAllocation, 2> m_colorMem{};
    oc::array<vk::ImageView, 2> m_colorView{};
    oc::array<vk::Framebuffer, 2> m_framebuffer{};
    oc::array<DescriptorSet, 2> m_compositeSet;
    vk::Image m_depthImage;
    VmaAllocation m_depthMem = nullptr;
    vk::ImageView m_depthView;
};
