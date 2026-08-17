module RendererVK;

import File.fwd;
import :TextureManager; // ?
import :TextureStreamer;
import :Device;
import :Layout;

TextureManager::~TextureManager()
{
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in TextureManager::~TextureManager");
    }
}

/*
uint16 TextureManager::upload(const oc::vector<ITextureData*>& textureData, bool generateMips)
{
    if (m_textures.size() + textureData.size() > UINT16_MAX)
    {
        assert(false && "Too many textures");
        return UINT16_MAX;
    }
    const uint16 startIdx = (uint16)m_textures.size();
    const size_t numTextures = textureData.size();
    m_textures.reserve(startIdx + numTextures);
    for (uint32 i = 0; i < textureData.size(); ++i)
    {
        m_textures.emplace_back();
        if (!m_textures[startIdx + i].initialize(*textureData[i], generateMips))
        {
            assert(false && "Failed to initialize texture");
            m_textures.resize(startIdx);
            return UINT16_MAX;
        }
    }

    return startIdx;
}*/

uint32 TextureManager::getDescriptorCap() const
{
	const uint32 deviceLimit = Globals::device.getPhysicalDevice().getProperties().limits.maxPerStageDescriptorSampledImages;
	return oc::min({ deviceLimit - 16u, (uint32)UINT16_MAX - 1u, 16u * 1024u });
}

uint16 TextureManager::upload(const ITextureData& textureData, bool generateMips, bool sRGB)
{
	return uploadImpl([&](Texture& texture) { return texture.initialize(textureData, generateMips, sRGB); });
}

uint16 TextureManager::upload(const char* filePath, bool generateMips, bool sRGB)
{
	return uploadImpl([&](Texture& texture) { return texture.initialize(filePath, generateMips, sRGB); });
}

uint16 TextureManager::uploadImpl(const oc::function<bool(Texture&)>& initialize)
{
	uint16 idx;
	if (!m_freeSlots.empty()) // slot freed by a destroyed ObjectContainer
	{
		idx = m_freeSlots.back();
		m_freeSlots.pop_back();
	}
	else
	{
		assert(m_textures.size() < UINT16_MAX && "Too many textures");
		if ((uint32)m_textures.size() >= m_maxTextures)
		{
			const uint32 maxCapacity = getDescriptorCap();
			if (m_maxTextures < maxCapacity)
			{
				m_maxTextures = oc::min(m_maxTextures * 2u, maxCapacity);
				m_generation++;
				printf("TextureManager: grew texture capacity to %u\n", m_maxTextures);
			}
			else
			{
				assert(false && "Texture capacity at device limit");
			}
		}
		idx = (uint16)m_textures.size();
		m_textures.emplace_back();
	}
	if (!initialize(m_textures[idx]))
	{
		assert(false && "Failed to initialize texture");
		if (idx == (uint16)(m_textures.size() - 1))
			m_textures.pop_back();
		else
			m_freeSlots.push_back(idx);
		return UINT16_MAX;
	}
	const uint64 allocatedBytes = m_textures[idx].getAllocatedBytes();
	if (oc::unique_ptr<StreamedTextureMeta> pMeta = m_textures[idx].takeStreamingMeta())
		Globals::textureStreamer.registerTexture(idx, oc::move(*pMeta), allocatedBytes);
	else
		Globals::textureStreamer.notePinned(allocatedBytes);
	// Refresh the slot's bindless entries in every frame slot (recycled slots point at the fallback or a
	// destroyed image until this applies; fresh slots pick the view up at the next full descriptor fill
	// anyway, but the queued write makes that independent of a re-record happening).
	Globals::textureStreamer.queueDescriptorWrite(idx);
	return idx;
}

void TextureManager::free(uint16 idx)
{
	assert(idx > RendererVKLayout::FALLBACK_NORMAL_TEX_IDX && idx < m_textures.size() && "freeing a fallback texture");
	Texture& texture = m_textures[idx];
	if (!texture.getImageView())
		return; // already freed
	Globals::textureStreamer.unregisterTexture(idx, texture.getAllocatedBytes());
	texture.destroy();
	m_freeSlots.push_back(idx);
	// Point the slot's bindless entries at the fallback until an upload recycles it.
	Globals::textureStreamer.queueDescriptorWrite(idx);
}