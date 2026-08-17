export module RendererVK:Texture;

import Core;
import File.fwd;

import :VK;
import :Device;
import :Allocator;
import :TextureStreamer;

export class Texture final
{
public:
    Texture();
    ~Texture();
    Texture(const Texture& copy) = delete;
    Texture(Texture&& move);

    // Destroys the vk image/view and resets to the empty state (slot recycling by TextureManager::free;
    // the caller guarantees no frame in flight still samples the image). Safe on an empty texture.
    void destroy();

    bool initialize(const char* filePath, bool generateMips, bool sRGB = false);
    // sRGB: the pixel data is sRGB-encoded color (albedo/emissive) — sample through an Srgb format so
    // the hardware linearizes it. Data textures (normals, metal/roughness) must stay Unorm.
    bool initialize(const ITextureData& textureData, bool generateMips, bool sRGB = false);
    bool initialize(uint32 width, uint32 height, vk::Format format, const oc::vector<oc::span<uint8>>& imageDataMips, bool generateMips);

    vk::ImageView getImageView() const { return m_imageView; }
    vk::Image getImage() const { return m_image; }
    vk::Format getFormat() const { return m_format; }
    uint64 getAllocatedBytes() const;

    // Streaming metadata built while parsing the DDS header; TextureManager::upload moves it into the
    // TextureStreamer. Null for unstreamable textures (procedural/embedded/non-DDS/cubemap/array/3D).
    oc::unique_ptr<StreamedTextureMeta> takeStreamingMeta() { return oc::move(m_pStreamingMeta); }

    // Residency change by the TextureStreamer: take ownership of a replacement image (a different mip
    // range of the same source) and hand back the old handles, which the streamer destroys once no
    // frame in flight can still be sampling them.
    void adoptStreamedImage(vk::Image image, VmaAllocation memory, vk::ImageView view, uint32 numMips,
        vk::Image& outOldImage, VmaAllocation& outOldMemory, vk::ImageView& outOldView)
    {
        outOldImage = m_image;
        outOldMemory = m_imageMemory;
        outOldView = m_imageView;
        m_image = image;
        m_imageMemory = memory;
        m_imageView = view;
        m_numMipLevels = numMips;
    }

private:

    // Dims of the live image's mip 0. For streamed textures this is the resident top mip, not the
    // source's full resolution (that lives in the StreamedTextureMeta).
    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_numMipLevels = 0;
    vk::Format m_format;
    vk::Image m_image;
    VmaAllocation m_imageMemory = nullptr;
    vk::ImageView m_imageView;
    oc::unique_ptr<StreamedTextureMeta> m_pStreamingMeta;
};