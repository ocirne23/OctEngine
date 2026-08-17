export module File:TextureData;

import Core;
import :ITextureData;

export struct aiTexture;

export class TextureData final : public ITextureData
{
public:
	TextureData();
	~TextureData();
	TextureData(const TextureData&) = delete;
	TextureData(TextureData&&) = default;

	bool initialize(const char* filePath);
	bool initialize(const aiTexture* pEmbeddedTexture);

	const char* getFileName() const override;
	const Pixel* getPixels() const override;
	uint32 getWidth() const override;
	uint32 getHeight() const override;
	const char* getFormatInfo() const override;

private:

	oc::string      m_filePath;            // always set: embedded filename or loose file path
	const aiTexture* m_pTexture = nullptr;  // non-null only for embedded textures
};