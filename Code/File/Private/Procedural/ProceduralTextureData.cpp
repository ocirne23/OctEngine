module File;

import Core;

import :ProceduralTextureData;

ProceduralTextureData::ProceduralTextureData()
{
}

ProceduralTextureData::~ProceduralTextureData()
{
}

bool ProceduralTextureData::initialize(EProceduralTextureType type, uint32 width, uint32 height)
{
	m_width  = width;
	m_height = height;
	m_pixels.resize(width * height);

	switch (type)
	{
	case EProceduralTextureType::Checkerboard:
	{
		m_fileName = "procedural_checkerboard";
		constexpr uint32 tileSize = 8;
		for (uint32 y = 0; y < height; y++)
		{
			for (uint32 x = 0; x < width; x++)
			{
				const bool isLight = ((x / tileSize) + (y / tileSize)) % 2 == 0;
				const char v = isLight ? (uint8)0x00 : (uint8)0xFF;
				Pixel& p = m_pixels[y * width + x];
				p.r = (uint8)0xFF;
				p.g = v;
				p.b = (uint8)0xFF;
				p.a = (uint8)0xFF;
			}
		}
		break;
	}
	case EProceduralTextureType::White:
	{
		m_fileName = "procedural_white";
		for (Pixel& p : m_pixels)
		{
			p.a = (uint8)0xFF;
			p.r = (uint8)0xFF;
			p.g = (uint8)0xFF;
			p.b = (uint8)0xFF;
		}
		break;
	}
	case EProceduralTextureType::FlatNormal:
	{
		m_fileName = "procedural_flat_normal";
		// Flat normal map: (0.5, 0.5, 1.0) in RGB => (128, 128, 255)
		for (Pixel& p : m_pixels)
		{
			p.r = (uint8)0x80; // 128
			p.g = (uint8)0x80; // 128
			p.b = (uint8)0xFF; // 255
			p.a = (uint8)0xFF;
		}
		break;
	}
	case EProceduralTextureType::SkyGradient:
	{
		m_fileName = "procedural_sky_gradient";
		// v = 0 (top row) maps to the sky sphere's zenith, v = 1 to the bottom. Zenith -> horizon over
		// the upper half, horizon -> ground over the lower half, with a sqrt ramp like skyRadiance().
		const float zenith[3]  = { 0.80f, 0.75f, 0.85f };
		const float horizon[3] = { 0.80f, 0.55f, 0.40f };
		const float ground[3]  = { 0.10f, 0.09f, 0.08f };
		for (uint32 y = 0; y < height; y++)
		{
			const float v = (height > 1) ? (float)y / (float)(height - 1) : 0.0f;
			const float t = v * 2.0f - 1.0f; // -1 = zenith, 0 = horizon, +1 = ground
			float rgb[3];
			for (int c = 0; c < 3; c++)
			{
				rgb[c] = (t <= 0.0f) ? horizon[c] + (zenith[c] - horizon[c]) * std::sqrt(-t)
				                     : horizon[c] + (ground[c] - horizon[c]) * oc::min(t * 2.0f, 1.0f);
			}
			for (uint32 x = 0; x < width; x++)
			{
				Pixel& p = m_pixels[y * width + x];
				p.r = (uint8)(rgb[0] * 255.0f + 0.5f);
				p.g = (uint8)(rgb[1] * 255.0f + 0.5f);
				p.b = (uint8)(rgb[2] * 255.0f + 0.5f);
				p.a = (uint8)0xFF;
			}
		}
		break;
	}
	default:
		return false;
	}

	return true;
}

bool ProceduralTextureData::initializeFromPixels(const Pixel* pixels, uint32 width, uint32 height, const char* name)
{
	if (!pixels || width == 0 || height == 0)
		return false;
	m_fileName = name ? name : "procedural_pixels";
	m_width = width;
	m_height = height;
	m_pixels.assign(pixels, pixels + (size_t)width * height);
	return true;
}

const char*  ProceduralTextureData::getFileName()   const { return m_fileName.c_str(); }
const ProceduralTextureData::Pixel* ProceduralTextureData::getPixels() const { return m_pixels.data(); }
uint32       ProceduralTextureData::getWidth()      const { return m_width; }
uint32       ProceduralTextureData::getHeight()     const { return m_height; }
const char*  ProceduralTextureData::getFormatInfo() const { return "rgba8888"; }

oc::unique_ptr<ITextureData> ITextureData::createFallbackDiffuseTexture()
{
	auto pTex = oc::make_unique<ProceduralTextureData>();
	pTex->initialize(EProceduralTextureType::Checkerboard, 8, 8);
	return pTex;
}

oc::unique_ptr<ITextureData> ITextureData::createFallbackWhiteTexture()
{
	auto pTex = oc::make_unique<ProceduralTextureData>();
	pTex->initialize(EProceduralTextureType::White, 8, 8);
	return pTex;
}

oc::unique_ptr<ITextureData> ITextureData::createFallbackNormalTexture()
{
	auto pTex = oc::make_unique<ProceduralTextureData>();
	pTex->initialize(EProceduralTextureType::FlatNormal, 8, 8);
	return pTex;
}