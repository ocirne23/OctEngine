module;

#include <cstdio> // fopen_s/_fseeki64/SEEK_SET for the narrowed DDS mip-range read

module RendererVK;

import Core;
import File;

import :VK;
import :Allocator;
import :Buffer;
import :StagingManager;
import :TextureStreamer;
import :stb_image;
import :DDS;

static vk::Format dxgiToVkFormat(dds::DXGI_FORMAT dxgiFormat, bool sRGB)
{
    using DF = dds::DXGI_FORMAT;
    switch (dxgiFormat)
    {
        // Block compressed
        case DF::DXGI_FORMAT_BC1_TYPELESS:
        case DF::DXGI_FORMAT_BC1_UNORM:        return sRGB ? vk::Format::eBc1RgbaSrgbBlock : vk::Format::eBc1RgbaUnormBlock;
        case DF::DXGI_FORMAT_BC1_UNORM_SRGB:   return vk::Format::eBc1RgbaSrgbBlock;
        case DF::DXGI_FORMAT_BC2_TYPELESS:
        case DF::DXGI_FORMAT_BC2_UNORM:        return sRGB ? vk::Format::eBc2SrgbBlock : vk::Format::eBc2UnormBlock;
        case DF::DXGI_FORMAT_BC2_UNORM_SRGB:   return vk::Format::eBc2SrgbBlock;
        case DF::DXGI_FORMAT_BC3_TYPELESS:
        case DF::DXGI_FORMAT_BC3_UNORM:        return sRGB ? vk::Format::eBc3SrgbBlock : vk::Format::eBc3UnormBlock;
        case DF::DXGI_FORMAT_BC3_UNORM_SRGB:   return vk::Format::eBc3SrgbBlock;
        case DF::DXGI_FORMAT_BC4_TYPELESS:
        case DF::DXGI_FORMAT_BC4_UNORM:        return vk::Format::eBc4UnormBlock;
        case DF::DXGI_FORMAT_BC4_SNORM:        return vk::Format::eBc4SnormBlock;
        case DF::DXGI_FORMAT_BC5_TYPELESS:
        case DF::DXGI_FORMAT_BC5_UNORM:        return vk::Format::eBc5UnormBlock;
        case DF::DXGI_FORMAT_BC5_SNORM:        return vk::Format::eBc5SnormBlock;
        case DF::DXGI_FORMAT_BC6H_TYPELESS:
        case DF::DXGI_FORMAT_BC6H_UF16:        return vk::Format::eBc6HUfloatBlock;
        case DF::DXGI_FORMAT_BC6H_SF16:        return vk::Format::eBc6HSfloatBlock;
        case DF::DXGI_FORMAT_BC7_TYPELESS:
        case DF::DXGI_FORMAT_BC7_UNORM:        return sRGB ? vk::Format::eBc7SrgbBlock : vk::Format::eBc7UnormBlock;
        case DF::DXGI_FORMAT_BC7_UNORM_SRGB:   return vk::Format::eBc7SrgbBlock;

        // Uncompressed
        case DF::DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DF::DXGI_FORMAT_R8G8B8A8_UNORM:   return sRGB ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        case DF::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return vk::Format::eR8G8B8A8Srgb;
        case DF::DXGI_FORMAT_R8G8B8A8_SNORM:   return vk::Format::eR8G8B8A8Snorm;
        case DF::DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DF::DXGI_FORMAT_B8G8R8A8_UNORM:   return sRGB ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm;
        case DF::DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return vk::Format::eB8G8R8A8Srgb;
        case DF::DXGI_FORMAT_R8G8_TYPELESS:
        case DF::DXGI_FORMAT_R8G8_UNORM:       return sRGB ? vk::Format::eR8G8Srgb : vk::Format::eR8G8Unorm;
        case DF::DXGI_FORMAT_R8G8_SNORM:       return vk::Format::eR8G8Snorm;
        case DF::DXGI_FORMAT_R8_TYPELESS:
        case DF::DXGI_FORMAT_R8_UNORM:         return sRGB ? vk::Format::eR8Srgb : vk::Format::eR8Unorm;
        case DF::DXGI_FORMAT_R8_SNORM:         return vk::Format::eR8Snorm;
        case DF::DXGI_FORMAT_A8_UNORM:         return vk::Format::eR8Unorm;
        case DF::DXGI_FORMAT_R16G16B16A16_FLOAT: return vk::Format::eR16G16B16A16Sfloat;
        case DF::DXGI_FORMAT_R16G16B16A16_UNORM: return vk::Format::eR16G16B16A16Unorm;
        case DF::DXGI_FORMAT_R16G16B16A16_SNORM: return vk::Format::eR16G16B16A16Snorm;
        case DF::DXGI_FORMAT_R16G16_FLOAT:     return vk::Format::eR16G16Sfloat;
        case DF::DXGI_FORMAT_R16G16_UNORM:     return vk::Format::eR16G16Unorm;
        case DF::DXGI_FORMAT_R16_FLOAT:        return vk::Format::eR16Sfloat;
        case DF::DXGI_FORMAT_R16_UNORM:        return vk::Format::eR16Unorm;
        case DF::DXGI_FORMAT_R32G32B32A32_FLOAT: return vk::Format::eR32G32B32A32Sfloat;
        case DF::DXGI_FORMAT_R32G32_FLOAT:     return vk::Format::eR32G32Sfloat;
        case DF::DXGI_FORMAT_R32_FLOAT:        return vk::Format::eR32Sfloat;
        case DF::DXGI_FORMAT_R10G10B10A2_UNORM: return vk::Format::eA2B10G10R10UnormPack32;
        case DF::DXGI_FORMAT_R11G11B10_FLOAT:  return vk::Format::eB10G11R11UfloatPack32;
        case DF::DXGI_FORMAT_B5G6R5_UNORM:     return vk::Format::eR5G6B5UnormPack16;
        case DF::DXGI_FORMAT_B5G5R5A1_UNORM:   return vk::Format::eA1R5G5B5UnormPack16;
        case DF::DXGI_FORMAT_B4G4R4A4_UNORM:   return vk::Format::eB4G4R4A4UnormPack16;

        default:
            assert(false && "unsupported DDS/DXGI texture format");
            return vk::Format::eUndefined;
    }
}

Texture::Texture()
{
}

Texture::~Texture()
{
    destroy();
}

void Texture::destroy()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_imageView)
        vkDevice.destroyImageView(m_imageView);
    Globals::gpuAllocator.destroyImage(m_image, m_imageMemory);
    m_image = VK_NULL_HANDLE;
    m_imageMemory = VK_NULL_HANDLE;
    m_imageView = VK_NULL_HANDLE;
    m_width = m_height = m_numMipLevels = 0;
    m_pStreamingMeta.reset();
}

Texture::Texture(Texture&& move)
{
    m_width = move.m_width;
    m_height = move.m_height;
    m_numMipLevels = move.m_numMipLevels;
    m_format = move.m_format;
    m_image = move.m_image;
    m_imageMemory = move.m_imageMemory;
    m_imageView = move.m_imageView;
    m_pStreamingMeta = std::move(move.m_pStreamingMeta);
    move.m_image = VK_NULL_HANDLE;
    move.m_imageMemory = VK_NULL_HANDLE;
    move.m_imageView = VK_NULL_HANDLE;
}

uint64 Texture::getAllocatedBytes() const
{
    return Globals::gpuAllocator.getAllocationSize(m_imageMemory);
}

bool Texture::initialize(const char* filePath, bool generateMips, bool sRGB)
{
    const std::string extension = FileSystem::extension(filePath);
    std::vector<uint8> fileData;
    std::vector<std::span<uint8>> imgData; // pixel data per mip level

    uint32 width = 0;
    uint32 height = 0;
	vk::Format format = vk::Format::eUndefined;
	uint32 numMipLevels = 0;

    if (extension == ".dds")
    {
        FILE* pFile;
        fopen_s(&pFile, filePath, "rb");
        std::unique_ptr<FILE, void(*)(FILE*)> file(pFile, [](FILE* p) { if (p) fclose(p); });
        if (!file)
        {
            assert(false && "Failed to open file");
            return false;
        }
        // Texture loads run on the streaming/loader threads; the assert catches a main-thread slip.
        const size_t size = (size_t)FileSystem::fileSize(filePath);

        // Header first: it lays out the whole mip chain, so the pixel read below can be narrowed to
        // just the mips this image will actually contain.
        const size_t headerSize = std::min(size, sizeof(dds::Header));
        fileData.resize(headerSize);
        if (fread(fileData.data(), 1, headerSize, file.get()) != headerSize)
        {
            assert(false && "Failed to read file");
            return false;
        }
        const dds::Header header = dds::read_header(fileData.data(), headerSize);
        if (!header.is_valid())
        {
            assert(false && "Invalid DDS file");
            return false;
        }

        width = header.width();
        height = header.height();
        numMipLevels = header.mip_levels();
        format = dxgiToVkFormat(header.format(), sRGB);
        if (format == vk::Format::eUndefined)
            return false;

        // Plain 2D textures with a mip chain qualify for mip streaming: capture where each mip lives in
        // the file so the TextureStreamer can re-read arbitrary mip ranges later.
        if (numMipLevels > 1 && !header.is_cubemap() && !header.is_1d() && !header.is_3d() && header.array_size() == 1)
        {
            m_pStreamingMeta = std::make_unique<StreamedTextureMeta>();
            m_pStreamingMeta->filePath = filePath;
            m_pStreamingMeta->format = format;
            m_pStreamingMeta->fullWidth = (uint16)width;
            m_pStreamingMeta->fullHeight = (uint16)height;
            m_pStreamingMeta->mips.reserve(numMipLevels);
            for (uint32 i = 0; i < numMipLevels; i++)
            {
                m_pStreamingMeta->mips.push_back(TextureMipRange{
                    .fileOffset = header.mip_offset(i),
                    .byteSize = (uint32)header.mip_size(i),
                    .width = (uint16)std::max(1u, width >> i),
                    .height = (uint16)std::max(1u, height >> i),
                });
            }
        }

        // Tail-only initial load: upload just the always-resident low-res tail and let the
        // TextureStreamer bring higher mips in by priority. Unstreamable textures load everything.
        uint32 firstMip = 0;
        if (m_pStreamingMeta && Globals::textureStreamer.isStreamingEnabled())
        {
            firstMip = computeStreamTailTop(*m_pStreamingMeta, Globals::textureStreamer.tailMaxDim());
            m_pStreamingMeta->initialResidentTop = (uint8)firstMip;
        }

        // Mips of a slice are contiguous in the file: one seek + read covers [firstMip..numMipLevels).
        const uint64 dataOffset = header.mip_offset(firstMip);
        const uint64 dataSize = header.mip_offset(numMipLevels - 1) + header.mip_size(numMipLevels - 1) - dataOffset;
        assert(dataOffset + dataSize <= size && "DDS mip chain exceeds file size");
        fileData.resize(dataSize);
        if (_fseeki64(file.get(), (int64)dataOffset, SEEK_SET) != 0
            || fread(fileData.data(), 1, dataSize, file.get()) != dataSize)
        {
            assert(false && "Failed to read file");
            return false;
        }
        uint64 bufferOffset = 0;
        for (uint32 i = firstMip; i < numMipLevels; i++)
        {
            imgData.push_back(std::span(fileData.data() + bufferOffset, header.mip_size(i)));
            bufferOffset += header.mip_size(i);
        }
        width = std::max(1u, width >> firstMip);
        height = std::max(1u, height >> firstMip);
        numMipLevels -= firstMip;
    }
    else
    {
        assert(false && "reimplement non dds loading");
        /*
        std::unique_ptr<uint8, void(*)(uint8*)> pData(
            stbi_load(pFilePath, (int*)&m_width, (int*)&m_height, (int*)&m_numChannels, 4),
            [](uint8* p) { if (p) stbi_image_free(p); });

        const vk::DeviceSize imageSize = m_width * m_height * 4;
        if (!pData)
        {
            assert(false && "Failed to load image");
            return false;
        }
        */
    }

	// A streamed source's mip layout must stay exactly the file's: never synthesize a chain from it
	// (a partial-chain DDS whose tail is a single mip would otherwise hit the blit-generate path and
	// desync the image from the TextureStreamer's bookkeeping).
	return initialize(width, height, format, imgData, generateMips && !m_pStreamingMeta);
}

bool Texture::initialize(const ITextureData& textureData, bool generateMips, bool sRGB)
{
    const ITextureData::Pixel* pPixels = textureData.getPixels();
    if (!pPixels)
    {
		const char* filePath = textureData.getFileName();
        if (!filePath || *filePath == '\0')
        {
			assert(false && "TextureData has no pixel data or file path");
			return false;
        }
        // Prefer the file relative to the scene file's folder; fall back to the asset-root-relative path.
        const std::string& rootFolder = textureData.getRootFolder();
        if (!rootFolder.empty())
        {
            const std::string candidate = FileSystem::join(rootFolder, filePath);
            if (FileSystem::exists(candidate))
                return initialize(candidate.c_str(), generateMips, sRGB);
        }
        return initialize(filePath, generateMips, sRGB);
    }
    std::string formatInfo = textureData.getFormatInfo();
    
    int width = textureData.getWidth();
    int height = textureData.getHeight();
    vk::Format format = vk::Format::eUndefined;

    std::vector<std::span<uint8>> imgData;
    
    if (height == 0)
    {
        if (formatInfo.compare("png") == 0)
        {
			const int bufferSize = textureData.getWidth();
            int numComponents = 0;
            int desiredComponents = 1;
			stbi_info_from_memory((stbi_uc*)textureData.getPixels(), bufferSize, &width, &height, &desiredComponents);
            desiredComponents = 4;
            stbi_uc* pixelData = stbi_load_from_memory((stbi_uc*)textureData.getPixels(), bufferSize, &width, &height, &numComponents, desiredComponents);
            if (!pixelData)
            {
                assert(false && "Failed to load png image");
                return false;
			}
            imgData.push_back(std::span((uint8*)pixelData, width * height * desiredComponents));
            switch (desiredComponents)
            {
            case 1:
                format = sRGB ? vk::Format::eR8Srgb : vk::Format::eR8Unorm;
                break;
            case 2:
				format = sRGB ? vk::Format::eR8G8Srgb : vk::Format::eR8G8Unorm;
				break;
			case 3:
				format = sRGB ? vk::Format::eR8G8B8Srgb : vk::Format::eR8G8B8Unorm;
				break;
			case 4:
				format = sRGB ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
				break;
            default:
                assert(false && "unsupported number of components in png");
				return false;
            }
        }
        else if (formatInfo.compare("jpg") == 0)
        {
            assert(false && "implement jpg loading");
            return false;
        }
        else
        {
            assert(false && "unsupported compressed texture format");
            return false;
        }
    }
    else
    {
        if (formatInfo.compare("rgba8888") == 0)
		{
			const size_t dataSize = (size_t)width * height * sizeof(ITextureData::Pixel);
			imgData.push_back(std::span((uint8*)textureData.getPixels(), dataSize));
			format = sRGB ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
		}
		else
		{
			assert(false && "unsupported uncompressed texture format");
			return false;
		}
    }

    return initialize(width, height, format, imgData, generateMips);
}

bool Texture::initialize(uint32 width, uint32 height, vk::Format format, const std::vector<std::span<uint8>>& imageDataMips, bool generateMips)
{
    vk::Device vkDevice = Globals::device.getDevice();
	m_width = width;
	m_height = height;
	m_format = format;
    // Only synthesize a full mip chain when generating from a single source level (the blit path below).
    // When the source already carries its own mips (e.g. DDS), use them as-is so the image's mipLevels and
    // the upload loop stay in lockstep — otherwise the loop reads past imageDataMips and upper levels of the
    // image are left undefined.
    const bool generateFromSingle = generateMips && imageDataMips.size() == 1;
    m_numMipLevels = generateFromSingle ? (uint32)(std::floor(std::log2(std::max(width, height)))) + 1 : (uint32)imageDataMips.size();

    vk::ImageCreateInfo imageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = m_format,
        .extent = { m_width, m_height, 1 },
        .mipLevels = m_numMipLevels,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined
    };

	auto imageFormatPropertiesresult = Globals::device.getPhysicalDevice().getImageFormatProperties(m_format, imageCreateInfo.imageType, imageCreateInfo.tiling, imageCreateInfo.usage);
    if (imageFormatPropertiesresult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get image format properties");
        return false;
	}
	auto imageFormatProperties = imageFormatPropertiesresult.value;

    if (!Globals::gpuAllocator.createImage(imageCreateInfo, m_image, m_imageMemory, "Texture"))
        return false;

    vk::ImageViewCreateInfo imageViewInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2D,
        .format = imageCreateInfo.format,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = vk::RemainingMipLevels,
            .baseArrayLayer = 0,
            .layerCount = imageCreateInfo.arrayLayers
        }
    };

    auto createImageViewResult = vkDevice.createImageView(imageViewInfo);
    if (createImageViewResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create image view");
        return false;
    }
    m_imageView = createImageViewResult.value;

    if (generateFromSingle)
    {
        Globals::stagingManager.uploadImageAndGenerateMipMaps(m_image, m_width, m_height, m_numMipLevels, imageDataMips[0].size(), imageDataMips[0].data());
    }
    else
    {
        for (uint32 i = 0; i < m_numMipLevels; i++)
        {
            Globals::stagingManager.uploadImage(m_image, std::max(1u, m_width >> i), std::max(1u, m_height >> i), imageDataMips[i].size(), imageDataMips[i].data(), i);
        }
    }

    return true;
}
