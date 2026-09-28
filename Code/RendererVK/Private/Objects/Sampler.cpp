module RendererVK;

import :VK;
import :Device;

Sampler::Sampler()
{
}

Sampler::~Sampler()
{
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

bool Sampler::initialize(vk::SamplerAddressMode addressMode, float maxAnisotropy, float mipLodBias)
{
    if (m_sampler)
    {
        Globals::device.getDevice().destroySampler(m_sampler);
        m_sampler = nullptr;
    }
    vk::SamplerCreateInfo samplerInfo = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = addressMode,
        .addressModeV = addressMode,
        .addressModeW = addressMode,
        .mipLodBias = mipLodBias,
        .anisotropyEnable = maxAnisotropy > 1.0f ? vk::True : vk::False,
        .maxAnisotropy = maxAnisotropy > 1.0f ? maxAnisotropy : 1.0f,
        .compareEnable = vk::False,
        .compareOp = vk::CompareOp::eNever,
        .minLod = 0.0f,
        .maxLod = vk::LodClampNone,
        .borderColor = vk::BorderColor::eFloatOpaqueBlack,
        .unnormalizedCoordinates = vk::False
    };
    auto createResult = Globals::device.getDevice().createSampler(samplerInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create sampler");
        return false;
    }
    m_sampler = createResult.value;
    Globals::device.setDebugName(m_sampler,
        addressMode == vk::SamplerAddressMode::eRepeat ? "Sampler.repeat" :
        addressMode == vk::SamplerAddressMode::eClampToEdge ? "Sampler.clamp" :
        addressMode == vk::SamplerAddressMode::eMirroredRepeat ? "Sampler.mirror" : "Sampler.border");

    return true;
}