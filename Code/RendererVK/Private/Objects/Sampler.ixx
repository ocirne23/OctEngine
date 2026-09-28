export module RendererVK:Sampler;

import :VK;

export class Sampler final
{
public:
    Sampler();
    ~Sampler();
    Sampler(const Sampler&) = delete;

    // maxAnisotropy <= 1 = no anisotropic filtering. Calling it again replaces the sampler (the caller makes
    // sure the GPU no longer uses the old one).
    bool initialize(vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat, float maxAnisotropy = 16.0f, float mipLodBias = 0.0f);

    vk::Sampler getSampler() const { return m_sampler; }

private:

    vk::Sampler m_sampler;
};