module;

// The scene cooker: imports a model via Assimp once, then writes a .vsc binary snapshot (see
// CookedSceneData.ixx) plus BC-compressed .dds conversions of any non-DDS textures into
// Assets/Local/Cooked, so subsequent loads are a single fread and every texture can mip-stream.
//
// The stb implementations are compiled STATIC (internal linkage): RendererVK already owns the
// external-linkage stb_image implementation, and both libraries link into the same executable.
#pragma warning(push, 0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include <stb/stb_dxt.h>
#pragma warning(pop)
#include <dds/dds.h>
#include <meshoptimizer/meshoptimizer.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cfloat>

module File;

// File IS the filesystem library: its readers stream binary ranges directly (Core no longer
// exports these). Entry points call FileSystem::assertIoThread so the main-thread policy still holds.
import <filesystem>;
import <fstream>;

import Core;
import Core.glm;
import Core.AABB;
import Core.Log;

import :ISceneData;
import :IMeshData;
import :IMaterialData;
import :ITextureData;
import :INodeData;
import :CookedSceneData;
import :TextureConvert;

using namespace SceneCache;

namespace
{
    constexpr uint64 FNV_OFFSET = 0xcbf29ce484222325ull;
    constexpr uint64 FNV_PRIME = 0x100000001b3ull;

    uint64 fnv1a(const void* pData, size_t size, uint64 hash = FNV_OFFSET)
    {
        const uint8* pBytes = (const uint8*)pData;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ pBytes[i]) * FNV_PRIME;
        return hash;
    }
    // No pointers: for a const char* argument this template outranks the buffer overload above (exact
    // match vs pointer conversion) and would hash the 8 POINTER bytes - a different, ASLR-randomized
    // cache name every launch, so nothing was ever reused and stale files piled up.
    template <typename T> requires (!std::is_pointer_v<T>)
    uint64 fnv1a(const T& value, uint64 hash) { return fnv1a(&value, sizeof(T), hash); }

    // oc::string is not a std::filesystem::path source under either backing (and is a distinct
    // type entirely under EASTL), so the cooker crosses into <filesystem> through this one seam.
    std::filesystem::path toPath(oc::string_view path) { return std::filesystem::path(oc::toStd(path)); }

    uint64 fileMTimeTicks(const std::filesystem::path& path, std::error_code& ec)
    {
        return (uint64)std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    }

    // Mirror of ObjectContainer's LodN_ chain parsing: the cooker must not meshopt-generate chains for
    // meshes an artist already authored levels for.
    bool parseLodName(oc::string_view name, uint32& outLevel, oc::string_view& outLogicalName)
    {
        if (name.size() < 5 || (name[0] != 'L' && name[0] != 'l') || (name[1] != 'O' && name[1] != 'o') || (name[2] != 'D' && name[2] != 'd'))
            return false;
        size_t i = 3;
        if (name[i] < '0' || name[i] > '9')
            return false;
        uint32 level = 0;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9')
            level = level * 10 + (name[i++] - '0');
        if (i >= name.size() || name[i] != '_' || i + 1 >= name.size())
            return false;
        outLevel = level;
        outLogicalName = name.substr(i + 1);
        return true;
    }

    struct Blob
    {
        oc::vector<uint8> data;

        uint64 align16()
        {
            data.resize((data.size() + 15) & ~size_t(15));
            return data.size();
        }
        uint64 write(const void* pData, size_t size)
        {
            const uint64 offset = data.size();
            data.insert(data.end(), (const uint8*)pData, (const uint8*)pData + size);
            return offset;
        }
        uint64 writeAligned(const void* pData, size_t size)
        {
            align16();
            return write(pData, size);
        }
    };

    struct StringBlob
    {
        oc::vector<char> data;
        uint32 add(oc::string_view str)
        {
            const uint32 offset = (uint32)data.size();
            data.insert(data.end(), str.begin(), str.end());
            data.push_back('\0');
            return offset;
        }
    };

    // ---------------------------------------------------------------- texture conversion

    enum class ETexUsage : uint8 { Color, NormalMap, Data }; // diffuse (sRGB) / BC5 XY / linear BC1

    struct DecodedImage
    {
        oc::unique_ptr<uint8, void(*)(uint8*)> pixels{ nullptr, [](uint8*) {} };
        uint32 width = 0, height = 0;
    };

    DecodedImage decodeTexture(const ITextureData& texData, const oc::string& resolvedFile)
    {
        DecodedImage img;
        int w = 0, h = 0, comp = 0;
        const ITextureData::Pixel* pPixels = texData.getPixels();
        if (!pPixels)
        {
            stbi_uc* pDecoded = stbi_load(resolvedFile.c_str(), &w, &h, &comp, 4);
            if (!pDecoded)
                return img;
            img.pixels = { pDecoded, [](uint8* p) { stbi_image_free(p); } };
        }
        else if (texData.getHeight() == 0)
        {
            // Embedded compressed buffer (png/jpg/...); size rides in width.
            stbi_uc* pDecoded = stbi_load_from_memory((const stbi_uc*)pPixels, (int)texData.getWidth(), &w, &h, &comp, 4);
            if (!pDecoded)
                return img;
            img.pixels = { pDecoded, [](uint8* p) { stbi_image_free(p); } };
        }
        else
        {
            // Embedded raw rgba8888 (matches the Texture.cpp interpretation).
            w = (int)texData.getWidth();
            h = (int)texData.getHeight();
            uint8* pCopy = (uint8*)malloc((size_t)w * h * 4);
            if (!pCopy)
                return img;
            memcpy(pCopy, pPixels, (size_t)w * h * 4);
            img.pixels = { pCopy, [](uint8* p) { free(p); } };
        }
        img.width = (uint32)w;
        img.height = (uint32)h;
        return img;
    }

    TextureConvert::EBlockFormat toBlockFormat(dds::DXGI_FORMAT format)
    {
        switch (format)
        {
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM: return TextureConvert::EBlockFormat::BC1;
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM: return TextureConvert::EBlockFormat::BC3;
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC4_UNORM: return TextureConvert::EBlockFormat::BC4;
        default:                                      return TextureConvert::EBlockFormat::BC5;
        }
    }

    // Compress one RGBA8 mip into BC1/BC3/BC4/BC5 blocks (edge-clamped 4x4 fetch handles non-multiple-of-4 dims).
    void compressMip(const uint8* pRgba, uint32 width, uint32 height, dds::DXGI_FORMAT format, oc::vector<uint8>& out)
    {
        const TextureConvert::EBlockFormat blockFormat = toBlockFormat(format);
        const size_t base = out.size();
        out.resize(base + TextureConvert::compressedSize(width, height, blockFormat));
        TextureConvert::compressBlockRows(pRgba, width, height, blockFormat, 0, (height + 3) / 4, out.data() + base);
    }

    float alphaCoverage(const uint8* pRgba, size_t numPixels, float alphaScale, uint8 cutoff)
    {
        size_t covered = 0;
        const uint8* pA = pRgba + 3;
        for (size_t i = 0; i < numPixels; ++i, pA += 4)
            covered += oc::min(255.0f, (float)*pA * alphaScale) >= (float)cutoff;
        return (float)covered / (float)numPixels;
    }

    // Rescales a mip's alpha so the fraction of texels passing the alpha-test cutoff ("coverage")
    // matches the full-res image's: plain downsampling averages alpha toward the middle, which makes
    // alpha-tested foliage thin out and vanish in coarse mips - exactly the mips LOD selection and mip
    // streaming push distant vegetation onto. Coverage is monotonic in the scale, so a binary search
    // finds the factor that restores it.
    void preserveAlphaCoverage(uint8* pRgba, size_t numPixels, uint8 cutoff, float refCoverage)
    {
        float lo = 0.25f, hi = 8.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            if (alphaCoverage(pRgba, numPixels, mid, cutoff) < refCoverage)
                lo = mid;
            else
                hi = mid;
        }
        const float scale = 0.5f * (lo + hi);
        if (std::abs(scale - 1.0f) < 0.01f)
            return;
        uint8* pA = pRgba + 3;
        for (size_t i = 0; i < numPixels; ++i, pA += 4)
            *pA = (uint8)oc::min(255.0f, (float)*pA * scale + 0.5f);
    }

    // ---------------------------------------------------------------- roughness floor
    //
    // The lit / tree / foliage FS read the metal-roughness G as the GGX alpha with NO floor of their own: alpha
    // >= 0.01 is guaranteed here, in every block of every mip of a texture in a roughness slot. A BC1-style colour
    // block's G comes only from its two 565 endpoints (the palette interpolates them), so G6 >= 1 on both endpoints
    // (4/255 decoded) puts every texel of the block at >= 4/255 (~3.5/255 = 0.0137 after the hardware's palette
    // rounding). Filtering and mips only take convex combinations of those texels, so any sample holds the floor.

    constexpr uint16 BC_G_MASK = 0x07E0; // the 6 G bits of a 565 endpoint
    constexpr uint16 BC_G_ONE = 0x0020;
    constexpr uint8 ROUGHNESS_FLOOR_8 = 4; // = G6 1 expanded to 8 bits

    void applyChannelFloor(uint8* pRgba, size_t numPixels, const uint8* pChannelMin)
    {
        for (int c = 0; c < 4; ++c)
        {
            const uint8 minValue = pChannelMin[c];
            if (minValue == 0)
                continue;
            uint8* p = pRgba + c;
            for (size_t i = 0; i < numPixels; ++i, p += 4)
                *p = oc::max(*p, minValue);
        }
    }

    // A BC1 3-colour block that USES index 3 (black, or transparent black: G = 0) has no endpoint fix: its 16 texels
    // are decoded, G-floored and re-encoded as a 4-colour block (stb_dxt never writes 3-colour mode). Lossy, but only
    // authored files can hold such a block.
    void reencodeColourBlock(uint8* pBlock)
    {
        uint16 c[2];
        uint32 indices;
        memcpy(c, pBlock, 4);
        memcpy(&indices, pBlock + 4, 4);
        uint8 palette[4][4];
        for (int e = 0; e < 2; ++e)
        {
            const uint32 r = ((uint32)c[e] >> 11) & 31u, g = ((uint32)c[e] >> 5) & 63u, b = (uint32)c[e] & 31u;
            palette[e][0] = (uint8)((r << 3) | (r >> 2));
            palette[e][1] = (uint8)((g << 2) | (g >> 4));
            palette[e][2] = (uint8)((b << 3) | (b >> 2));
            palette[e][3] = 255;
        }
        for (int ch = 0; ch < 3; ++ch)
        {
            palette[2][ch] = (uint8)(((uint32)palette[0][ch] + palette[1][ch]) / 2);
            palette[3][ch] = 0;
        }
        palette[2][3] = 255;
        palette[3][3] = 255;
        uint8 texels[16 * 4];
        for (uint32 i = 0; i < 16; ++i)
        {
            memcpy(&texels[i * 4], palette[(indices >> (2 * i)) & 3u], 4);
            texels[i * 4 + 1] = oc::max(texels[i * 4 + 1], ROUGHNESS_FLOOR_8);
        }
        stb_compress_dxt_block(pBlock, texels, 0, STB_DXT_HIGHQUAL);
    }

    // Raises the G of both endpoints of one BC1-style colour block to G6 >= 1; every texel keeps its decoded colour
    // except for the raised G floor. fourColourAlways: the BC2 / BC3 colour block, interpolated in 4-colour mode
    // whatever the endpoint order. A BC1 block's MODE is its endpoint order (c0 > c1: 4 colours; c0 <= c1: 3 colours
    // + black), so when the raise flips the order the endpoints swap and the indices remap to the same entries.
    void raiseColourBlockGreen(uint8* pBlock, bool fourColourAlways)
    {
        uint16 c0, c1;
        uint32 indices;
        memcpy(&c0, pBlock, 2);
        memcpy(&c1, pBlock + 2, 2);
        if ((c0 & BC_G_MASK) && (c1 & BC_G_MASK))
            return;
        memcpy(&indices, pBlock + 4, 4);
        if (!fourColourAlways && c0 <= c1)
        {
            bool usesBlack = false;
            for (uint32 i = 0; i < 16; ++i)
                usesBlack |= ((indices >> (2 * i)) & 3u) == 3u;
            if (usesBlack)
            {
                reencodeColourBlock(pBlock); // now 4-colour, or c0 == c1 with every index 0
                memcpy(&c0, pBlock, 2);
                memcpy(&c1, pBlock + 2, 2);
                memcpy(&indices, pBlock + 4, 4);
                if ((c0 & BC_G_MASK) && (c1 & BC_G_MASK))
                    return;
            }
        }

        // A BC2 / BC3 block keeps its c0 > c1 order too: correct under either reading of its mode.
        const bool ordered = c0 > c1;
        uint16 r0 = (c0 & BC_G_MASK) ? c0 : (uint16)(c0 | BC_G_ONE);
        uint16 r1 = (c1 & BC_G_MASK) ? c1 : (uint16)(c1 | BC_G_ONE);
        if (ordered && r0 < r1)
        {
            // 4-colour palette {c0, c1, (2c0 + c1) / 3, (c0 + 2c1) / 3}: a swap maps index i to i ^ 1
            oc::swap(r0, r1);
            indices ^= 0x55555555u;
        }
        else if (ordered && r0 == r1)
            indices = 0; // the endpoints differed only in G 0 vs 1: ONE colour now (BC1 3-colour mode, index 0)
        else if (!ordered && !fourColourAlways && r0 > r1)
        {
            // BC1 3-colour palette {c0, c1, (c0 + c1) / 2, black}: a swap maps 0 <-> 1, 2 and 3 stay
            oc::swap(r0, r1);
            indices ^= ~(indices >> 1) & 0x55555555u;
        }
        memcpy(pBlock, &r0, 2);
        memcpy(pBlock + 2, &r1, 2);
        memcpy(pBlock + 4, &indices, 4);
    }

    // blockStride 8 (BC1) or 16 (BC2 / BC3: the colour block sits at byte 8).
    void raiseRoughnessFloor(uint8* pBlocks, size_t numBlocks, uint32 blockStride)
    {
        const bool bc1 = blockStride == 8;
        uint8* pColour = pBlocks + (bc1 ? 0 : 8);
        for (size_t i = 0; i < numBlocks; ++i, pColour += blockStride)
            raiseColourBlockGreen(pColour, !bc1);
    }

    // An authored .dds in a roughness slot: copied to outPath with the roughness floor in every colour block of every
    // mip / slice (all of them sit back to back after the header). False, with the reason, for a format this cannot
    // patch: BC7 and the rest would need a decoder, and an _SRGB format decodes the G through the sRGB curve.
    bool copyDdsWithRoughnessFloor(const oc::string& srcPath, const oc::string& outPath, oc::string& outReason)
    {
        std::error_code ec;
        const uint64 size = std::filesystem::file_size(toPath(srcPath), ec);
        if (ec || size < sizeof(dds::Header::magic) + sizeof(dds::Header::header))
        {
            outReason = "unreadable";
            return false;
        }
        oc::vector<uint8> fileData((size_t)size);
        {
            FILE* pFile = nullptr;
            fopen_s(&pFile, srcPath.c_str(), "rb");
            if (!pFile)
            {
                outReason = "unreadable";
                return false;
            }
            const bool ok = fread(fileData.data(), 1, fileData.size(), pFile) == fileData.size();
            fclose(pFile);
            if (!ok)
            {
                outReason = "unreadable";
                return false;
            }
        }

        const dds::Header header = dds::read_header(fileData.data(), fileData.size());
        if (!header.is_valid())
        {
            outReason = "not a valid DDS";
            return false;
        }
        uint32 blockStride = 0;
        switch (header.format())
        {
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC1_TYPELESS:
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM:
            blockStride = 8;
            break;
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC2_TYPELESS:
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM:
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC3_TYPELESS:
        case dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM:
            blockStride = 16;
            break;
        default:
            outReason = oc::format("DXGI format {}", (int)header.format());
            return false;
        }
        const size_t dataOffset = (size_t)header.data_offset();
        if (dataOffset >= fileData.size())
        {
            outReason = "no pixel data";
            return false;
        }
        raiseRoughnessFloor(fileData.data() + dataOffset, (fileData.size() - dataOffset) / blockStride, blockStride);

        FILE* pFile = nullptr;
        fopen_s(&pFile, outPath.c_str(), "wb");
        if (!pFile)
        {
            outReason = "output not writable";
            return false;
        }
        const bool ok = fwrite(fileData.data(), 1, fileData.size(), pFile) == fileData.size();
        fclose(pFile);
        if (!ok)
            outReason = "output not writable";
        return ok;
    }

    // coverageCutoff: alpha-test cutoff of the masked material(s) using this texture, or < 0 when the
    // texture is never alpha-tested (no coverage preservation).
    bool convertTextureToDds(const ITextureData& texData, const oc::string& resolvedFile, ETexUsage usage, float coverageCutoff, const oc::string& outPath)
    {
        const DecodedImage img = decodeTexture(texData, resolvedFile);
        if (!img.pixels || img.width == 0 || img.height == 0)
            return false;

        dds::DXGI_FORMAT format = dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        if (usage == ETexUsage::NormalMap)
            format = dds::DXGI_FORMAT::DXGI_FORMAT_BC5_UNORM; // XY only; the material flags Z reconstruction
        else if (usage == ETexUsage::Color)
        {
            const uint8* pA = img.pixels.get() + 3;
            const size_t numPixels = (size_t)img.width * img.height;
            for (size_t i = 0; i < numPixels; ++i, pA += 4)
                if (*pA < 250) { format = dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM; break; }
        }

        // Alpha-tested textures preserve their level-0 coverage per mip; only meaningful when the alpha
        // channel survives compression (BC3) and the full-res coverage isn't already all-or-nothing.
        float refCoverage = -1.0f;
        uint8 cutoff255 = 0;
        if (usage == ETexUsage::Color && coverageCutoff > 0.0f && coverageCutoff < 1.0f
            && format == dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM)
        {
            cutoff255 = (uint8)oc::clamp((int)(coverageCutoff * 255.0f + 0.5f), 1, 255);
            refCoverage = alphaCoverage(img.pixels.get(), (size_t)img.width * img.height, 1.0f, cutoff255);
            if (refCoverage <= 0.0f || refCoverage >= 1.0f)
                refCoverage = -1.0f;
        }

        const uint32 numMips = 1 + (uint32)std::floor(std::log2((float)oc::max(img.width, img.height)));
        oc::vector<uint8> fileData(sizeof(dds::Header));
        dds::write_header(fileData.data(), format, img.width, img.height, numMips);

        oc::vector<uint8> mipPixels;
        for (uint32 mip = 0; mip < numMips; ++mip)
        {
            const uint32 mipW = oc::max(1u, img.width >> mip);
            const uint32 mipH = oc::max(1u, img.height >> mip);
            const uint8* pSrc = img.pixels.get();
            if (mip > 0)
            {
                // Each level filters down from the full-res source (arbitrary ratios are fine for stbir).
                mipPixels.resize((size_t)mipW * mipH * 4);
                if (usage == ETexUsage::Color)
                    stbir_resize_uint8_srgb(img.pixels.get(), (int)img.width, (int)img.height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_RGBA);
                else
                    stbir_resize_uint8_linear(img.pixels.get(), (int)img.width, (int)img.height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_4CHANNEL);
                if (refCoverage >= 0.0f)
                    preserveAlphaCoverage(mipPixels.data(), (size_t)mipW * mipH, cutoff255, refCoverage);
                pSrc = mipPixels.data();
            }
            const size_t mipBegin = fileData.size();
            compressMip(pSrc, mipW, mipH, format, fileData);
            if (usage == ETexUsage::Data) // only the metal-roughness slot marks Data: the G is the GGX alpha
                raiseRoughnessFloor(fileData.data() + mipBegin, (fileData.size() - mipBegin) / 8, 8);
        }

        FILE* pFile = nullptr;
        fopen_s(&pFile, outPath.c_str(), "wb");
        if (!pFile)
            return false;
        const bool ok = fwrite(fileData.data(), 1, fileData.size(), pFile) == fileData.size();
        fclose(pFile);
        return ok;
    }

    // Mips + BC compression + .dds write for an already-decoded RGBA8 image (the shared tail of the
    // TextureConvert entry points; alpha-coverage preservation is a cooked-scene concern, not needed here).
    // pChannelMin (4 entries, or null): a per-channel floor re-applied after every mip resize - the caller
    // has already applied it to level 0.
    bool compressRgbaToDds(const uint8* pRgba, uint32 width, uint32 height, TextureConvert::EUsage usage, const char* outPath,
        const uint8* pChannelMin = nullptr)
    {
        dds::DXGI_FORMAT format = dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        if (usage == TextureConvert::EUsage::NormalMap || usage == TextureConvert::EUsage::TwoChannel)
            format = dds::DXGI_FORMAT::DXGI_FORMAT_BC5_UNORM;
        else if (usage == TextureConvert::EUsage::Height)
            format = dds::DXGI_FORMAT::DXGI_FORMAT_BC4_UNORM;
        else if (usage == TextureConvert::EUsage::ColorAlpha)
            format = dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM;
        else if (usage == TextureConvert::EUsage::Color)
        {
            const uint8* pA = pRgba + 3;
            const size_t numPixels = (size_t)width * height;
            for (size_t i = 0; i < numPixels; ++i, pA += 4)
                if (*pA < 250) { format = dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM; break; }
        }

        const uint32 numMips = 1 + (uint32)std::floor(std::log2((float)oc::max(width, height)));
        oc::vector<uint8> fileData(sizeof(dds::Header));
        dds::write_header(fileData.data(), format, width, height, numMips);

        oc::vector<uint8> mipPixels;
        for (uint32 mip = 0; mip < numMips; ++mip)
        {
            const uint32 mipW = oc::max(1u, width >> mip);
            const uint32 mipH = oc::max(1u, height >> mip);
            const uint8* pSrc = pRgba;
            if (mip > 0)
            {
                mipPixels.resize((size_t)mipW * mipH * 4);
                if (usage == TextureConvert::EUsage::Color)
                    stbir_resize_uint8_srgb(pRgba, (int)width, (int)height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_RGBA);
                else if (usage == TextureConvert::EUsage::ColorAlpha) // sRGB colour, linear alpha, NOT weighted by it
                    stbir_resize_uint8_srgb(pRgba, (int)width, (int)height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_RGBA_NO_AW);
                else
                    stbir_resize_uint8_linear(pRgba, (int)width, (int)height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_4CHANNEL);
                if (pChannelMin)
                    applyChannelFloor(mipPixels.data(), (size_t)mipW * mipH, pChannelMin);
                pSrc = mipPixels.data();
            }
            compressMip(pSrc, mipW, mipH, format, fileData);
        }

        FILE* pFile = nullptr;
        fopen_s(&pFile, outPath, "wb");
        if (!pFile)
            return false;
        const bool ok = fwrite(fileData.data(), 1, fileData.size(), pFile) == fileData.size();
        fclose(pFile);
        return ok;
    }

    // ---------------------------------------------------------------- cooking

    struct CookContext
    {
        Blob blob;
        StringBlob strings;
        oc::vector<CookedMesh> meshes;
        oc::vector<CookedMaterial> materials;
        oc::vector<CookedTexture> textures;
        oc::vector<CookedNode> nodes;
        oc::vector<uint32> meshRefs;
        oc::vector<CookedTexStamp> texStamps;
    };

    void cookMeshes(const ISceneData& scene, const SceneCookOptions& options, CookContext& ctx)
    {
        const uint32 numMeshes = scene.getNumMeshes();

        // Meshes that belong to an authored LodN_ chain (any level, or a plain "X" acting as level 0 of
        // an existing "LodN_X" chain) never get generated chains - mirror of ObjectContainer's pre-scan.
        oc::vector<bool> inAuthoredChain(numMeshes, false);
        {
            oc::unordered_map<oc::string_view, uint32> meshIdxByName;
            oc::vector<oc::string_view> chainLogicalNames;
            for (uint32 i = 0; i < numMeshes; ++i)
                meshIdxByName.try_emplace(scene.getMesh(i)->getName(), i);
            for (uint32 i = 0; i < numMeshes; ++i)
            {
                uint32 level;
                oc::string_view logicalName;
                if (parseLodName(scene.getMesh(i)->getName(), level, logicalName))
                {
                    inAuthoredChain[i] = true;
                    chainLogicalNames.push_back(logicalName);
                }
            }
            for (oc::string_view logicalName : chainLogicalNames)
                if (auto it = meshIdxByName.find(logicalName); it != meshIdxByName.end())
                    inAuthoredChain[it->second] = true;
        }

        const float reduction = oc::clamp(options.lodReduction, 0.05f, 0.75f);
        const int maxLevels = oc::min(options.lodLevels, (int)MAX_COOKED_LOD_LEVELS);
        const float decimation = oc::clamp(options.decimationFactor, 0.0f, 1.0f);

        ctx.meshes.resize(numMeshes);
        oc::vector<glm::vec3> zeroes;
        oc::vector<uint32> lodIndices;
        oc::vector<uint32> decimatedIndices, vertexRemap;
        oc::vector<glm::vec3> decimatedAttributes[5];
        for (uint32 meshIdx = 0; meshIdx < numMeshes; ++meshIdx)
        {
            const IMeshData& meshData = *scene.getMesh(meshIdx);
            CookedMesh& cooked = ctx.meshes[meshIdx];
            uint32 numVertices = meshData.getNumVertices();
            uint32 numIndices = meshData.getNumIndices();
            const uint32* pIndices = meshData.getIndices();
            const glm::vec3* attributes[5] = { meshData.getVertices(), meshData.getNormals(),
                meshData.getTangents(), meshData.getBitangents(), meshData.getTexCoords() };
            const bool isColMesh = oc::string_view(meshData.getName()).starts_with("Col_");

            // Decimation simplifies the base geometry itself (error-unbounded, target count wins) and
            // drops the vertices that fall out of use - everything downstream (LOD chains, collision
            // snapshots, mesh streaming) sees only the decimated mesh. Col_ proxies are authored
            // collision shapes and keep their exact geometry.
            if (decimation < 1.0f && !meshData.isSkinned() && !isColMesh && numIndices >= 12)
            {
                const size_t targetIndexCount = oc::max<size_t>(12, ((size_t)((float)numIndices * decimation)) / 3 * 3);
                decimatedIndices.resize(numIndices);
                float resultError = 0.0f;
                const size_t resultCount = meshopt_simplify(decimatedIndices.data(), pIndices, numIndices,
                    &attributes[0][0].x, numVertices, sizeof(glm::vec3), targetIndexCount, FLT_MAX, 0, &resultError);
                if (resultCount >= 3 && resultCount < numIndices)
                {
                    meshopt_optimizeVertexCache(decimatedIndices.data(), decimatedIndices.data(), resultCount, numVertices);
                    vertexRemap.resize(numVertices);
                    const size_t uniqueVertices = meshopt_optimizeVertexFetchRemap(vertexRemap.data(), decimatedIndices.data(), resultCount, numVertices);
                    meshopt_remapIndexBuffer(decimatedIndices.data(), decimatedIndices.data(), resultCount, vertexRemap.data());
                    for (int i = 0; i < 5; ++i)
                    {
                        if (!attributes[i])
                            continue;
                        decimatedAttributes[i].resize(uniqueVertices);
                        meshopt_remapVertexBuffer(decimatedAttributes[i].data(), attributes[i], numVertices, sizeof(glm::vec3), vertexRemap.data());
                        attributes[i] = decimatedAttributes[i].data();
                    }
                    numVertices = (uint32)uniqueVertices;
                    numIndices = (uint32)resultCount;
                    pIndices = decimatedIndices.data();
                }
            }

            cooked.nameOffset = ctx.strings.add(meshData.getName());
            cooked.materialIdx = meshData.getMaterialIndex();
            cooked.numVertices = numVertices;
            cooked.numIndices = numIndices;
            const AABB aabb = meshData.getAABB();
            memcpy(cooked.aabbMin, &aabb.min, sizeof(float) * 3);
            memcpy(cooked.aabbMax, &aabb.max, sizeof(float) * 3);

            zeroes.assign(numVertices, glm::vec3(0.0f));
            auto writeAttribute = [&](const glm::vec3* pData) {
                return ctx.blob.writeAligned(pData ? pData : zeroes.data(), (size_t)numVertices * sizeof(glm::vec3));
            };
            cooked.positionsOffset = writeAttribute(attributes[0]);
            cooked.normalsOffset = writeAttribute(attributes[1]);
            cooked.tangentsOffset = writeAttribute(attributes[2]);
            cooked.bitangentsOffset = writeAttribute(attributes[3]);
            cooked.texCoordsOffset = writeAttribute(attributes[4]);
            cooked.indicesOffset = ctx.blob.writeAligned(pIndices, (size_t)numIndices * sizeof(uint32));

            if (!options.generateLods || meshData.isSkinned() || isColMesh || inAuthoredChain[meshIdx]
                || numIndices < (uint32)options.lodMinIndices)
                continue;

            // Same policy as the runtime path in ObjectContainer::initializeMeshes, plus a vertex-cache
            // pass on each level (free at cook time). The simplify error is kept per level (scaled to
            // mesh-local units) - the renderer's screen-space-error selection projects it into pixels.
            const glm::vec3* pPositions = attributes[0];
            const float meshScale = meshopt_simplifyScale(&pPositions[0].x, numVertices, sizeof(glm::vec3));
            lodIndices.resize(numIndices);
            uint32 prevIndexCount = numIndices;
            float targetF = (float)numIndices;
            cooked.lodIndicesOffset = ctx.blob.align16();
            for (int level = 1; level <= maxLevels; ++level)
            {
                targetF *= reduction;
                const size_t targetIndexCount = oc::max<size_t>(12, ((size_t)targetF) / 3 * 3);
                if (targetIndexCount >= prevIndexCount)
                    break;
                const float targetError = 0.01f * (float)(1u << (level - 1));
                float resultError = 0.0f;
                const size_t resultCount = meshopt_simplify(lodIndices.data(), pIndices, numIndices,
                    &pPositions[0].x, numVertices, sizeof(glm::vec3), targetIndexCount, targetError, 0, &resultError);
                if (resultCount < 3 || resultCount >= (size_t)((float)prevIndexCount * 0.9f))
                    break;
                meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(), resultCount, numVertices);
                ctx.blob.write(lodIndices.data(), resultCount * sizeof(uint32));
                cooked.lodErrors[cooked.numLodLevels] = resultError * meshScale;
                cooked.lodIndexCounts[cooked.numLodLevels++] = (uint32)resultCount;
                prevIndexCount = (uint32)resultCount;
            }
        }
    }

    void cookTextures(const ISceneData& scene, const SceneCookOptions& options, const oc::string& sourcePath,
        const oc::string& texFolder, CookContext& ctx)
    {
        const uint32 numTextures = scene.getNumTextures();
        ctx.textures.resize(numTextures);

        // Usage per texture from the material slots the renderer actually samples (first mark wins).
        // Diffuse textures of alpha-MASKED materials also record the cutoff, so their mips can keep the
        // full-res alpha coverage (Blend materials want the true averaged alpha, so they don't).
        oc::vector<uint8> usage(numTextures, 0xFF);
        oc::vector<float> coverageCutoff(numTextures, -1.0f);
        oc::vector<uint8> feedsRoughness(numTextures, 0);
        auto markUsage = [&](uint32 texIdx, ETexUsage use) {
            if (texIdx < numTextures && usage[texIdx] == 0xFF)
                usage[texIdx] = (uint8)use;
        };
        for (uint32 i = 0; i < scene.getNumMaterials(); ++i)
        {
            const IMaterialData& material = *scene.getMaterial(i);
            markUsage(material.getDiffuseTexIdx(), ETexUsage::Color);
            markUsage(material.getNormalTexIdx(), ETexUsage::NormalMap);
            markUsage(material.getMetalRoughnessTexIdx(), ETexUsage::Data);
            if (material.getMetalRoughnessTexIdx() < numTextures)
                feedsRoughness[material.getMetalRoughnessTexIdx()] = 1;
            if (material.getAlphaMode() == IMaterialData::EAlphaMode::Mask)
            {
                const uint32 texIdx = material.getDiffuseTexIdx();
                if (texIdx < numTextures && coverageCutoff[texIdx] < 0.0f)
                    coverageCutoff[texIdx] = material.getAlphaCutoff();
            }
        }
        // A texture shared by a roughness slot and a diffuse / normal slot cooks for the OTHER slot (sRGB BC1/BC3 or
        // BC5): its G cannot carry the floor (a sRGB 4/255 decodes to 0.0012), so the shaders' roughness guarantee
        // does not hold for it.
        for (uint32 texIdx = 0; texIdx < numTextures; ++texIdx)
            if (feedsRoughness[texIdx] && usage[texIdx] != (uint8)ETexUsage::Data)
                Log::warning(oc::format("SceneCache: texture '{}' of '{}' is in a roughness slot AND a {} slot: no roughness floor for it",
                    scene.getTexture(texIdx)->getFileName(), sourcePath, usage[texIdx] == (uint8)ETexUsage::Color ? "diffuse" : "normal"));

        const oc::string modelFolder = oc::fromStd(toPath(sourcePath).parent_path().string());
        // A recook regenerates every conversion, so clear out the previous set first - otherwise .dds
        // files of textures the scene no longer references would pile up.
        std::error_code ecPurge;
        std::filesystem::remove_all(toPath(texFolder), ecPurge);
        bool texFolderCreated = false;
        for (uint32 texIdx = 0; texIdx < numTextures; ++texIdx)
        {
            const ITextureData& texData = *scene.getTexture(texIdx);
            const oc::string originalPath = texData.getFileName();
            ctx.textures[texIdx].pathOffset = ctx.strings.add(originalPath);
            if (!options.convertTextures || usage[texIdx] == 0xFF)
                continue;

            // Loose files resolve like Texture.cpp: next to the model first, then Assets-root-relative.
            oc::string resolvedFile;
            const bool isEmbedded = texData.getPixels() != nullptr;
            bool isAuthoredDds = false;
            if (!isEmbedded)
            {
                if (originalPath.empty())
                    continue;
                std::error_code ec;
                const std::filesystem::path nextToModel = toPath(modelFolder) / oc::toStd(originalPath);
                resolvedFile = std::filesystem::exists(nextToModel, ec) ? oc::fromStd(nextToModel.string()) : originalPath;
                isAuthoredDds = toPath(resolvedFile).extension() == ".dds";
                // Already streamable - except in a roughness slot, where a copy gets the roughness floor.
                if (isAuthoredDds && usage[texIdx] != (uint8)ETexUsage::Data)
                    continue;
                if (!std::filesystem::exists(toPath(resolvedFile), ec))
                {
                    Log::warning(oc::format("SceneCache: texture '{}' of '{}' not found; keeping original reference", originalPath, sourcePath));
                    continue;
                }
            }

            oc::string stem = oc::fromStd(toPath(originalPath).stem().string());
            if (stem.empty() || stem[0] == '*')
                stem = "embedded";
            for (char& c : stem)
                if (!isalnum((uint8)c) && c != '_' && c != '-')
                    c = '_';
            const oc::string cookedPath = texFolder + oc::to_string(texIdx) + "_" + stem + ".dds";

            if (!texFolderCreated)
            {
                std::error_code ec;
                std::filesystem::create_directories(toPath(texFolder), ec);
                texFolderCreated = true;
            }
            if (isAuthoredDds)
            {
                oc::string reason;
                if (!copyDdsWithRoughnessFloor(resolvedFile, cookedPath, reason))
                {
                    Log::warning(oc::format("SceneCache: roughness texture '{}' of '{}' kept as authored, NO roughness floor ({})", originalPath, sourcePath, reason));
                    continue;
                }
            }
            else if (!convertTextureToDds(texData, resolvedFile, (ETexUsage)usage[texIdx], coverageCutoff[texIdx], cookedPath))
            {
                Log::warning(oc::format("SceneCache: failed to convert texture '{}' of '{}'; keeping original reference", originalPath, sourcePath));
                continue;
            }

            ctx.textures[texIdx].pathOffset = ctx.strings.add(cookedPath);
            if (!isEmbedded) // embedded sources are covered by the model file's own stamp
            {
                std::error_code ec;
                CookedTexStamp& stamp = ctx.texStamps.emplace_back();
                stamp.sourcePathOffset = ctx.strings.add(resolvedFile);
                stamp.cookedPathOffset = ctx.strings.add(cookedPath);
                stamp.size = std::filesystem::file_size(toPath(resolvedFile), ec);
                stamp.mtime = fileMTimeTicks(toPath(resolvedFile), ec);
            }
        }
    }

    void cookMaterials(const ISceneData& scene, CookContext& ctx)
    {
        const uint32 numMaterials = scene.getNumMaterials();
        ctx.materials.resize(numMaterials);
        for (uint32 i = 0; i < numMaterials; ++i)
        {
            const IMaterialData& material = *scene.getMaterial(i);
            CookedMaterial& cooked = ctx.materials[i];
            cooked.nameOffset = ctx.strings.add(material.getName());
            const glm::vec3 baseColor = material.getBaseColor();
            const glm::vec3 emissiveColor = material.getEmissiveColor();
            const glm::vec3 specularColor = material.getSpecularColor();
            memcpy(cooked.baseColor, &baseColor, sizeof(float) * 3);
            memcpy(cooked.emissiveColor, &emissiveColor, sizeof(float) * 3);
            memcpy(cooked.specularColor, &specularColor, sizeof(float) * 3);
            cooked.roughness = material.getRoughnessFactor();
            cooked.metalness = material.getMetalnessFactor();
            cooked.opacity = material.getOpacity();
            cooked.alphaCutoff = material.getAlphaCutoff();
            cooked.emissiveIntensity = material.getEmissiveIntensity();
            cooked.refractiveIndex = material.getRefractiveIndex();
            cooked.alphaMode = (uint32)material.getAlphaMode();
            cooked.diffuseTexIdx = material.getDiffuseTexIdx();
            cooked.normalTexIdx = material.getNormalTexIdx();
            cooked.opacityTexIdx = material.getOpacityTexIdx();
            cooked.metalRoughnessTexIdx = material.getMetalRoughnessTexIdx();
        }
    }

    void cookNodes(const ISceneData& scene, CookContext& ctx)
    {
        // BFS so each node's children are contiguous; parent indices tracked to fill the recursive counts.
        oc::vector<uint32> parentIdx;
        oc::deque<oc::pair<oc::unique_ptr<INodeData>, uint32>> queue; // node -> its cooked index

        auto appendNode = [&](const INodeData& node, uint32 parent) {
            const uint32 idx = (uint32)ctx.nodes.size();
            CookedNode& cooked = ctx.nodes.emplace_back();
            parentIdx.push_back(parent);
            cooked.nameOffset = ctx.strings.add(node.getName());
            glm::vec3 pos, scale;
            glm::quat rot;
            node.getTransform(pos, scale, rot);
            memcpy(cooked.pos, &pos, sizeof(float) * 3);
            memcpy(cooked.scale, &scale, sizeof(float) * 3);
            cooked.rot[0] = rot.w; cooked.rot[1] = rot.x; cooked.rot[2] = rot.y; cooked.rot[3] = rot.z;
            cooked.firstMeshRef = (uint32)ctx.meshRefs.size();
            cooked.numMeshes = node.getNumMeshes();
            for (uint32 i = 0; i < cooked.numMeshes; ++i)
                ctx.meshRefs.push_back(node.getMeshIndex(i));
            return idx;
        };

        const uint32 rootIdx = appendNode(scene.getRootNode(), UINT32_MAX);
        queue.emplace_back(scene.getRootNode().clone(), rootIdx);
        while (!queue.empty())
        {
            auto [pNode, cookedIdx] = oc::move(queue.front());
            queue.pop_front();
            const uint32 numChildren = pNode->getNumChildren();
            ctx.nodes[cookedIdx].firstChild = (uint32)ctx.nodes.size();
            ctx.nodes[cookedIdx].numChildren = numChildren;
            for (uint32 i = 0; i < numChildren; ++i)
            {
                oc::unique_ptr<INodeData> pChild = pNode->getChild(i);
                const uint32 childIdx = appendNode(*pChild, cookedIdx);
                queue.emplace_back(oc::move(pChild), childIdx);
            }
        }

        // BFS order guarantees children have higher indices, so a reverse sweep accumulates subtree sizes.
        for (uint32 i = (uint32)ctx.nodes.size(); i-- > 1;)
            if (parentIdx[i] != UINT32_MAX)
                ctx.nodes[parentIdx[i]].numChildrenRecursive += 1 + ctx.nodes[i].numChildrenRecursive;
    }

    // Garbage-collects Assets/Local/Cooked whenever something cooks: a cache file is deleted (along with
    // its _tex folder) when its format version is outdated, its source model no longer exists, or the
    // source's mtime/size no longer match the cooked stamp. Caches whose source is intact but whose .oc
    // import options changed are indistinguishable from a second .oc variant of the same model, so those
    // are overwritten in place by their own recook instead.
    void gcStaleCaches()
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("Local/Cooked", ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".vsc")
                continue;
            bool stale = false;
            {
                FILE* pFile = nullptr;
                fopen_s(&pFile, entry.path().string().c_str(), "rb");
                if (!pFile)
                    continue;
                oc::unique_ptr<FILE, void(*)(FILE*)> file(pFile, [](FILE* p) { fclose(p); });
                CookedHeader header{};
                if (fread(&header, 1, sizeof(header), pFile) != sizeof(header) || header.magic != SCENE_CACHE_MAGIC)
                    continue; // not one of ours (or unreadable): leave it alone
                char sourcePath[1024] = {};
                if (header.version != SCENE_CACHE_VERSION || header.sourcePathOffset >= header.stringsSize)
                    stale = true;
                else if (_fseeki64(pFile, (int64)(header.stringsOffset + header.sourcePathOffset), SEEK_SET) == 0
                    && fread(sourcePath, 1, sizeof(sourcePath) - 1, pFile) > 0)
                {
                    const uint64 srcSize = std::filesystem::file_size(sourcePath, ec);
                    stale = ec || srcSize != header.sourceSize || fileMTimeTicks(toPath(sourcePath), ec) != header.sourceMTime || ec;
                }
            }
            if (stale)
            {
                std::filesystem::path texFolder = entry.path();
                texFolder.replace_extension();
                texFolder += "_tex";
                std::filesystem::remove(entry.path(), ec);
                std::filesystem::remove_all(texFolder, ec);
                Log::info(oc::format("SceneCache: removed stale cache '{}'", entry.path().string()));
            }
        }
    }

    bool cookScene(const ISceneData& scene, const oc::string& sourcePath, const oc::string& cachePath,
        const oc::string& texFolder, const SceneCookOptions& options,
        uint64 sourceMTime, uint64 sourceSize, uint64 optionsHash)
    {
        CookContext ctx;
        ctx.blob.data.resize(sizeof(CookedHeader)); // header patched in at the end

        cookMeshes(scene, options, ctx);
        cookTextures(scene, options, sourcePath, texFolder, ctx);
        cookMaterials(scene, ctx);
        cookNodes(scene, ctx);

        CookedHeader header{};
        header.magic = SCENE_CACHE_MAGIC;
        header.version = SCENE_CACHE_VERSION;
        header.sourceMTime = sourceMTime;
        header.sourceSize = sourceSize;
        header.optionsHash = optionsHash;
        header.sourcePathOffset = ctx.strings.add(sourcePath);
        header.numMeshes = (uint32)ctx.meshes.size();
        header.numMaterials = (uint32)ctx.materials.size();
        header.numTextures = (uint32)ctx.textures.size();
        header.numNodes = (uint32)ctx.nodes.size();
        header.numMeshRefs = (uint32)ctx.meshRefs.size();
        header.numTexStamps = (uint32)ctx.texStamps.size();
        header.meshesOffset = ctx.blob.writeAligned(ctx.meshes.data(), ctx.meshes.size() * sizeof(CookedMesh));
        header.materialsOffset = ctx.blob.writeAligned(ctx.materials.data(), ctx.materials.size() * sizeof(CookedMaterial));
        header.texturesOffset = ctx.blob.writeAligned(ctx.textures.data(), ctx.textures.size() * sizeof(CookedTexture));
        header.nodesOffset = ctx.blob.writeAligned(ctx.nodes.data(), ctx.nodes.size() * sizeof(CookedNode));
        header.meshRefsOffset = ctx.blob.writeAligned(ctx.meshRefs.data(), ctx.meshRefs.size() * sizeof(uint32));
        header.texStampsOffset = ctx.blob.writeAligned(ctx.texStamps.data(), ctx.texStamps.size() * sizeof(CookedTexStamp));
        header.stringsOffset = ctx.blob.writeAligned(ctx.strings.data.data(), ctx.strings.data.size());
        header.stringsSize = (uint32)ctx.strings.data.size();
        memcpy(ctx.blob.data.data(), &header, sizeof(header));

        std::error_code ec;
        std::filesystem::create_directories(toPath(cachePath).parent_path(), ec);
        FILE* pFile = nullptr;
        fopen_s(&pFile, cachePath.c_str(), "wb");
        if (!pFile)
        {
            Log::warning(oc::format("SceneCache: could not write '{}'", cachePath));
            return false;
        }
        const bool ok = fwrite(ctx.blob.data.data(), 1, ctx.blob.data.size(), pFile) == ctx.blob.data.size();
        fclose(pFile);
        return ok;
    }
}

oc::unique_ptr<ISceneData> ISceneData::loadCached(const char* filePath, bool mergeNodes, bool preTransformVertices, const SceneCookOptions& options)
{
    // Scene import/cook: main thread at spawn (cached afterwards) and on the terrain jobs.
    FileSystem::assertIoThread(/*allowMainThread*/ true);
    // Import options that shape the cooked data feed the header hash; the file NAME only hashes the
    // source path + Assimp options, so LOD/texture option changes re-cook in place instead of piling
    // up stale files (and two .oc's importing one model with different Assimp options don't thrash).
    uint64 optionsHash = fnv1a((const void*)filePath, strlen(filePath));
    optionsHash = fnv1a(mergeNodes, optionsHash);
    optionsHash = fnv1a(preTransformVertices, optionsHash);
    const uint64 nameHash = optionsHash;
    optionsHash = fnv1a(oc::clamp(options.decimationFactor, 0.0f, 1.0f), optionsHash);
    optionsHash = fnv1a(options.convertTextures, optionsHash);
    optionsHash = fnv1a(options.generateLods, optionsHash);
    optionsHash = fnv1a(options.lodLevels, optionsHash);
    optionsHash = fnv1a(options.lodReduction, optionsHash);
    optionsHash = fnv1a(options.lodMinIndices, optionsHash);

    // Through FileSystem: a source or cache inside a mounted .dat pack counts as present.
    FileSystem::FileStat sourceStat;
    const bool sourceMissing = !FileSystem::stat(filePath, sourceStat, true) || sourceStat.isDirectory;
    const uint64 sourceSize = sourceMissing ? 0 : sourceStat.size;
    const uint64 sourceMTime = sourceMissing ? 0 : (uint64)sourceStat.mtimeTicks;
    const bool cacheUsable = options.enableCache && !sourceMissing;

    oc::string stem = oc::fromStd(toPath(filePath).stem().string());
    for (char& c : stem)
        if (!isalnum((uint8)c) && c != '_' && c != '-')
            c = '_';
    const oc::string cacheTag = oc::format("{}_{:08x}", stem, (uint32)(nameHash ^ (nameHash >> 32)));
    const oc::string cachePath = "Local/Cooked/" + cacheTag + ".vsc";
    const oc::string texFolder = "Local/Cooked/" + cacheTag + "_tex/";

    if (cacheUsable)
    {
        auto cooked = oc::make_unique<CookedSceneData>();
        if (cooked->load(cachePath, filePath, sourceMTime, sourceSize, optionsHash))
        {
            Log::info(oc::format("SceneCache: '{}' served from '{}'", filePath, cachePath));
            return cooked;
        }
    }

    // Source gone (shipped build, moved asset): serve any cooked scene of this model, whatever its hash
    // suffix. Size/mtime 0 tells the reader there is nothing to validate against.
    if (sourceMissing && options.enableCache)
    {
        const auto isHash = [](const oc::string& s, size_t from)
        {
            if (s.size() != from + 8)
                return false;
            for (size_t i = from; i < s.size(); ++i)
                if (!isxdigit((uint8)s[i]))
                    return false;
            return true;
        };
        const oc::string prefix = stem + "_";
        oc::vector<FileSystem::DirEntry> cookedFiles;
        FileSystem::listDirectory("Local/Cooked", cookedFiles, true);
        for (const FileSystem::DirEntry& entry : cookedFiles)
        {
            if (entry.isDirectory || entry.extension != ".vsc")
                continue;
            const oc::string name = FileSystem::stem(entry.name);
            if (!oc::startsWith(name, prefix) || !isHash(name, prefix.size()))
                continue;
            auto cooked = oc::make_unique<CookedSceneData>();
            if (cooked->load(entry.path, filePath, 0, 0, 0))
            {
                Log::info(oc::format("SceneCache: source '{}' missing, served from '{}'", filePath, entry.path));
                return cooked;
            }
        }
    }

    oc::unique_ptr<ISceneData> imported = createAssimpLoader();
    if (!imported->initialize(filePath, mergeNodes, preTransformVertices) || !imported->isValid())
        return nullptr;

    // Skinned/animated scenes are served straight from the importer (the cache holds neither skeletons
    // nor clips); everything else cooks once and is re-read to validate the writer immediately.
    if (cacheUsable && !imported->getSkeleton() && imported->getNumAnimations() == 0)
    {
        gcStaleCaches(); // a cook happening means content changed: good moment to sweep dead caches
        const auto cookStart = std::chrono::steady_clock::now();
        if (cookScene(*imported, filePath, cachePath, texFolder, options, sourceMTime, sourceSize, optionsHash))
        {
            auto cooked = oc::make_unique<CookedSceneData>();
            if (cooked->load(cachePath, filePath, sourceMTime, sourceSize, optionsHash))
            {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cookStart).count();
                Log::info(oc::format("SceneCache: cooked '{}' -> '{}' in {} ms", filePath, cachePath, ms));
                return cooked;
            }
            Log::warning(oc::format("SceneCache: read-back of freshly cooked '{}' failed; serving direct import", cachePath));
        }
    }
    return imported;
}

uint32 TextureConvert::blockBytes(EBlockFormat format)
{
    return format == EBlockFormat::BC1 || format == EBlockFormat::BC4 ? 8u : 16u;
}

size_t TextureConvert::compressedSize(uint32 width, uint32 height, EBlockFormat format)
{
    return (size_t)((width + 3) / 4) * ((height + 3) / 4) * blockBytes(format);
}

void TextureConvert::compressBlockRows(const uint8* pRgba, uint32 width, uint32 height, EBlockFormat format, uint32 rowBegin, uint32 rowEnd, uint8* pDstImage)
{
    const uint32 blocksX = (width + 3) / 4;
    const uint32 blockSize = blockBytes(format);
    uint8* pDst = pDstImage + (size_t)rowBegin * blocksX * blockSize;
    uint8 blockRgba[16 * 4];
    uint8 blockRg[16 * 2];
    uint8 blockR[16];
    for (uint32 by = rowBegin; by < rowEnd; ++by)
    {
        for (uint32 bx = 0; bx < blocksX; ++bx)
        {
            for (uint32 y = 0; y < 4; ++y)
            {
                const uint32 sy = oc::min(by * 4 + y, height - 1);
                for (uint32 x = 0; x < 4; ++x)
                {
                    const uint32 sx = oc::min(bx * 4 + x, width - 1);
                    const uint8* pSrc = pRgba + ((size_t)sy * width + sx) * 4;
                    memcpy(&blockRgba[(y * 4 + x) * 4], pSrc, 4);
                    blockRg[(y * 4 + x) * 2 + 0] = pSrc[0];
                    blockRg[(y * 4 + x) * 2 + 1] = pSrc[1];
                    blockR[y * 4 + x] = pSrc[0];
                }
            }
            switch (format)
            {
            case EBlockFormat::BC1: stb_compress_dxt_block(pDst, blockRgba, 0, STB_DXT_HIGHQUAL); break;
            case EBlockFormat::BC3: stb_compress_dxt_block(pDst, blockRgba, 1, STB_DXT_HIGHQUAL); break;
            case EBlockFormat::BC4: stb_compress_bc4_block(pDst, blockR); break;
            default:                stb_compress_bc5_block(pDst, blockRg); break;
            }
            pDst += blockSize;
        }
    }
}

bool TextureConvert::convertToDds(const char* srcPath, EUsage usage, const char* outPath)
{
    int w = 0, h = 0, comp = 0;
    stbi_uc* pDecoded = stbi_load(srcPath, &w, &h, &comp, 4);
    if (!pDecoded)
    {
        Log::warning(oc::format("TextureConvert: could not decode '{}'", srcPath));
        return false;
    }
    const bool ok = compressRgbaToDds(pDecoded, (uint32)w, (uint32)h, usage, outPath);
    stbi_image_free(pDecoded);
    return ok;
}

bool TextureConvert::convertPackedToDds(const char* srcPathR, const char* srcPathG, const char* srcPathB, const char* outPath)
{
    const char* srcPaths[3] = { srcPathR, srcPathG, srcPathB };
    oc::vector<uint8> rgba;
    uint32 width = 0, height = 0;
    for (int channel = 0; channel < 3; ++channel)
    {
        if (!srcPaths[channel])
            continue;
        int w = 0, h = 0, comp = 0;
        stbi_uc* pGray = stbi_load(srcPaths[channel], &w, &h, &comp, 1);
        if (!pGray)
        {
            Log::warning(oc::format("TextureConvert: could not decode '{}'", srcPaths[channel]));
            return false;
        }
        if (rgba.empty())
        {
            width = (uint32)w;
            height = (uint32)h;
            rgba.resize((size_t)width * height * 4, 0);
            uint8* pA = rgba.data() + 3;
            for (size_t i = 0, n = (size_t)width * height; i < n; ++i, pA += 4)
                *pA = 255;
        }
        else if ((uint32)w != width || (uint32)h != height)
        {
            Log::warning(oc::format("TextureConvert: '{}' is {}x{}, expected {}x{} (all packed sources must match)", srcPaths[channel], w, h, width, height));
            stbi_image_free(pGray);
            return false;
        }
        uint8* pDst = rgba.data() + channel;
        for (size_t i = 0, n = (size_t)width * height; i < n; ++i, pDst += 4)
            *pDst = pGray[i];
        stbi_image_free(pGray);
    }
    if (rgba.empty())
        return false;
    return compressRgbaToDds(rgba.data(), width, height, EUsage::Data, outPath);
}

bool TextureConvert::convertChannelsToDds(const PackChannel (&channels)[4], EUsage usage, const char* outPath)
{
    // Each distinct source decoded once (as RGBA: a grayscale source replicates into RGB).
    struct Decoded { const char* path; stbi_uc* pixels; };
    oc::small_vector<Decoded, 4> decoded;
    uint32 width = 0, height = 0;
    bool ok = true;
    const auto source = [&](const char* path) -> const stbi_uc* {
        for (const Decoded& d : decoded)
            if (strcmp(d.path, path) == 0)
                return d.pixels;
        int w = 0, h = 0, comp = 0;
        stbi_uc* pPixels = stbi_load(path, &w, &h, &comp, 4);
        if (!pPixels)
        {
            Log::warning(oc::format("TextureConvert: could not decode '{}'", path));
            return nullptr;
        }
        if (decoded.empty())
        {
            width = (uint32)w;
            height = (uint32)h;
        }
        else if ((uint32)w != width || (uint32)h != height)
        {
            Log::warning(oc::format("TextureConvert: '{}' is {}x{}, expected {}x{} (all packed sources must match)", path, w, h, width, height));
            stbi_image_free(pPixels);
            return nullptr;
        }
        decoded.push_back({ path, pPixels });
        return pPixels;
    };
    for (const PackChannel& channel : channels)
        if (channel.path && !source(channel.path))
            ok = false;

    if (ok && !decoded.empty())
    {
        const size_t numPixels = (size_t)width * height;
        oc::vector<uint8> rgba(numPixels * 4);
        for (int c = 0; c < 4; ++c)
        {
            uint8* pDst = rgba.data() + c;
            if (!channels[c].path)
            {
                for (size_t i = 0; i < numPixels; ++i, pDst += 4)
                    *pDst = channels[c].fill;
                continue;
            }
            const stbi_uc* pSrc = source(channels[c].path) + (channels[c].srcChannel & 3);
            const float scale = channels[c].scale;
            if (scale == 1.0f)
            {
                for (size_t i = 0; i < numPixels; ++i, pDst += 4, pSrc += 4)
                    *pDst = *pSrc;
                continue;
            }
            for (size_t i = 0; i < numPixels; ++i, pDst += 4, pSrc += 4)
                *pDst = (uint8)oc::clamp(128.0f + ((float)*pSrc - 128.0f) * scale + 0.5f, 0.0f, 255.0f);
        }
        const uint8 channelMin[4] = { channels[0].minValue, channels[1].minValue, channels[2].minValue, channels[3].minValue };
        const bool hasFloor = (channelMin[0] | channelMin[1] | channelMin[2] | channelMin[3]) != 0;
        if (hasFloor)
            applyChannelFloor(rgba.data(), numPixels, channelMin);
        ok = compressRgbaToDds(rgba.data(), width, height, usage, outPath, hasFloor ? channelMin : nullptr);
    }
    else
        ok = false;
    for (const Decoded& d : decoded)
        stbi_image_free(d.pixels);
    return ok;
}
