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
    // match vs pointer conversion) and would hash the 8 POINTER bytes — a different, ASLR-randomized
    // cache name every launch, so nothing was ever reused and stale files piled up.
    template <typename T> requires (!std::is_pointer_v<T>)
    uint64 fnv1a(const T& value, uint64 hash) { return fnv1a(&value, sizeof(T), hash); }

    uint64 fileMTimeTicks(const std::filesystem::path& path, std::error_code& ec)
    {
        return (uint64)std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    }

    // Mirror of ObjectContainer's LodN_ chain parsing: the cooker must not meshopt-generate chains for
    // meshes an artist already authored levels for.
    bool parseLodName(std::string_view name, uint32& outLevel, std::string_view& outLogicalName)
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
        std::vector<uint8> data;

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
        std::vector<char> data;
        uint32 add(std::string_view str)
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
        std::unique_ptr<uint8, void(*)(uint8*)> pixels{ nullptr, [](uint8*) {} };
        uint32 width = 0, height = 0;
    };

    DecodedImage decodeTexture(const ITextureData& texData, const std::string& resolvedFile)
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

    // Compress one RGBA8 mip into BC1/BC3/BC5 blocks (edge-clamped 4x4 fetch handles non-multiple-of-4 dims).
    void compressMip(const uint8* pRgba, uint32 width, uint32 height, dds::DXGI_FORMAT format, std::vector<uint8>& out)
    {
        const uint32 blocksX = (width + 3) / 4;
        const uint32 blocksY = (height + 3) / 4;
        const uint32 blockSize = (format == dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM) ? 8 : 16;
        const size_t base = out.size();
        out.resize(base + (size_t)blocksX * blocksY * blockSize);
        uint8* pDst = out.data() + base;

        uint8 blockRgba[16 * 4];
        uint8 blockRg[16 * 2];
        for (uint32 by = 0; by < blocksY; ++by)
        {
            for (uint32 bx = 0; bx < blocksX; ++bx)
            {
                for (uint32 y = 0; y < 4; ++y)
                {
                    const uint32 sy = std::min(by * 4 + y, height - 1);
                    for (uint32 x = 0; x < 4; ++x)
                    {
                        const uint32 sx = std::min(bx * 4 + x, width - 1);
                        const uint8* pSrc = pRgba + ((size_t)sy * width + sx) * 4;
                        memcpy(&blockRgba[(y * 4 + x) * 4], pSrc, 4);
                        blockRg[(y * 4 + x) * 2 + 0] = pSrc[0];
                        blockRg[(y * 4 + x) * 2 + 1] = pSrc[1];
                    }
                }
                switch (format)
                {
                case dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM: stb_compress_dxt_block(pDst, blockRgba, 0, STB_DXT_HIGHQUAL); break;
                case dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM: stb_compress_dxt_block(pDst, blockRgba, 1, STB_DXT_HIGHQUAL); break;
                default:                                      stb_compress_bc5_block(pDst, blockRg); break;
                }
                pDst += blockSize;
            }
        }
    }

    float alphaCoverage(const uint8* pRgba, size_t numPixels, float alphaScale, uint8 cutoff)
    {
        size_t covered = 0;
        const uint8* pA = pRgba + 3;
        for (size_t i = 0; i < numPixels; ++i, pA += 4)
            covered += std::min(255.0f, (float)*pA * alphaScale) >= (float)cutoff;
        return (float)covered / (float)numPixels;
    }

    // Rescales a mip's alpha so the fraction of texels passing the alpha-test cutoff ("coverage")
    // matches the full-res image's: plain downsampling averages alpha toward the middle, which makes
    // alpha-tested foliage thin out and vanish in coarse mips — exactly the mips LOD selection and mip
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
            *pA = (uint8)std::min(255.0f, (float)*pA * scale + 0.5f);
    }

    // coverageCutoff: alpha-test cutoff of the masked material(s) using this texture, or < 0 when the
    // texture is never alpha-tested (no coverage preservation).
    bool convertTextureToDds(const ITextureData& texData, const std::string& resolvedFile, ETexUsage usage, float coverageCutoff, const std::string& outPath)
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
            cutoff255 = (uint8)std::clamp((int)(coverageCutoff * 255.0f + 0.5f), 1, 255);
            refCoverage = alphaCoverage(img.pixels.get(), (size_t)img.width * img.height, 1.0f, cutoff255);
            if (refCoverage <= 0.0f || refCoverage >= 1.0f)
                refCoverage = -1.0f;
        }

        const uint32 numMips = 1 + (uint32)std::floor(std::log2((float)std::max(img.width, img.height)));
        std::vector<uint8> fileData(sizeof(dds::Header));
        dds::write_header(fileData.data(), format, img.width, img.height, numMips);

        std::vector<uint8> mipPixels;
        for (uint32 mip = 0; mip < numMips; ++mip)
        {
            const uint32 mipW = std::max(1u, img.width >> mip);
            const uint32 mipH = std::max(1u, img.height >> mip);
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
            compressMip(pSrc, mipW, mipH, format, fileData);
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
    bool compressRgbaToDds(const uint8* pRgba, uint32 width, uint32 height, TextureConvert::EUsage usage, const char* outPath)
    {
        dds::DXGI_FORMAT format = dds::DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        if (usage == TextureConvert::EUsage::NormalMap)
            format = dds::DXGI_FORMAT::DXGI_FORMAT_BC5_UNORM;
        else if (usage == TextureConvert::EUsage::Color)
        {
            const uint8* pA = pRgba + 3;
            const size_t numPixels = (size_t)width * height;
            for (size_t i = 0; i < numPixels; ++i, pA += 4)
                if (*pA < 250) { format = dds::DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM; break; }
        }

        const uint32 numMips = 1 + (uint32)std::floor(std::log2((float)std::max(width, height)));
        std::vector<uint8> fileData(sizeof(dds::Header));
        dds::write_header(fileData.data(), format, width, height, numMips);

        std::vector<uint8> mipPixels;
        for (uint32 mip = 0; mip < numMips; ++mip)
        {
            const uint32 mipW = std::max(1u, width >> mip);
            const uint32 mipH = std::max(1u, height >> mip);
            const uint8* pSrc = pRgba;
            if (mip > 0)
            {
                mipPixels.resize((size_t)mipW * mipH * 4);
                if (usage == TextureConvert::EUsage::Color)
                    stbir_resize_uint8_srgb(pRgba, (int)width, (int)height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_RGBA);
                else
                    stbir_resize_uint8_linear(pRgba, (int)width, (int)height, 0, mipPixels.data(), (int)mipW, (int)mipH, 0, STBIR_4CHANNEL);
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
        std::vector<CookedMesh> meshes;
        std::vector<CookedMaterial> materials;
        std::vector<CookedTexture> textures;
        std::vector<CookedNode> nodes;
        std::vector<uint32> meshRefs;
        std::vector<CookedTexStamp> texStamps;
    };

    void cookMeshes(const ISceneData& scene, const SceneCookOptions& options, CookContext& ctx)
    {
        const uint32 numMeshes = scene.getNumMeshes();

        // Meshes that belong to an authored LodN_ chain (any level, or a plain "X" acting as level 0 of
        // an existing "LodN_X" chain) never get generated chains — mirror of ObjectContainer's pre-scan.
        std::vector<bool> inAuthoredChain(numMeshes, false);
        {
            std::unordered_map<std::string_view, uint32> meshIdxByName;
            std::vector<std::string_view> chainLogicalNames;
            for (uint32 i = 0; i < numMeshes; ++i)
                meshIdxByName.try_emplace(scene.getMesh(i)->getName(), i);
            for (uint32 i = 0; i < numMeshes; ++i)
            {
                uint32 level;
                std::string_view logicalName;
                if (parseLodName(scene.getMesh(i)->getName(), level, logicalName))
                {
                    inAuthoredChain[i] = true;
                    chainLogicalNames.push_back(logicalName);
                }
            }
            for (std::string_view logicalName : chainLogicalNames)
                if (auto it = meshIdxByName.find(logicalName); it != meshIdxByName.end())
                    inAuthoredChain[it->second] = true;
        }

        const float reduction = std::clamp(options.lodReduction, 0.05f, 0.75f);
        const int maxLevels = std::min(options.lodLevels, (int)MAX_COOKED_LOD_LEVELS);
        const float decimation = std::clamp(options.decimationFactor, 0.0f, 1.0f);

        ctx.meshes.resize(numMeshes);
        std::vector<glm::vec3> zeroes;
        std::vector<uint32> lodIndices;
        std::vector<uint32> decimatedIndices, vertexRemap;
        std::vector<glm::vec3> decimatedAttributes[5];
        for (uint32 meshIdx = 0; meshIdx < numMeshes; ++meshIdx)
        {
            const IMeshData& meshData = *scene.getMesh(meshIdx);
            CookedMesh& cooked = ctx.meshes[meshIdx];
            uint32 numVertices = meshData.getNumVertices();
            uint32 numIndices = meshData.getNumIndices();
            const uint32* pIndices = meshData.getIndices();
            const glm::vec3* attributes[5] = { meshData.getVertices(), meshData.getNormals(),
                meshData.getTangents(), meshData.getBitangents(), meshData.getTexCoords() };
            const bool isColMesh = std::string_view(meshData.getName()).starts_with("Col_");

            // Decimation simplifies the base geometry itself (error-unbounded, target count wins) and
            // drops the vertices that fall out of use — everything downstream (LOD chains, collision
            // snapshots, mesh streaming) sees only the decimated mesh. Col_ proxies are authored
            // collision shapes and keep their exact geometry.
            if (decimation < 1.0f && !meshData.isSkinned() && !isColMesh && numIndices >= 12)
            {
                const size_t targetIndexCount = std::max<size_t>(12, ((size_t)((float)numIndices * decimation)) / 3 * 3);
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
            // mesh-local units) — the renderer's screen-space-error selection projects it into pixels.
            const glm::vec3* pPositions = attributes[0];
            const float meshScale = meshopt_simplifyScale(&pPositions[0].x, numVertices, sizeof(glm::vec3));
            lodIndices.resize(numIndices);
            uint32 prevIndexCount = numIndices;
            float targetF = (float)numIndices;
            cooked.lodIndicesOffset = ctx.blob.align16();
            for (int level = 1; level <= maxLevels; ++level)
            {
                targetF *= reduction;
                const size_t targetIndexCount = std::max<size_t>(12, ((size_t)targetF) / 3 * 3);
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

    void cookTextures(const ISceneData& scene, const SceneCookOptions& options, const std::string& sourcePath,
        const std::string& texFolder, CookContext& ctx)
    {
        const uint32 numTextures = scene.getNumTextures();
        ctx.textures.resize(numTextures);

        // Usage per texture from the material slots the renderer actually samples (first mark wins).
        // Diffuse textures of alpha-MASKED materials also record the cutoff, so their mips can keep the
        // full-res alpha coverage (Blend materials want the true averaged alpha, so they don't).
        std::vector<uint8> usage(numTextures, 0xFF);
        std::vector<float> coverageCutoff(numTextures, -1.0f);
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
            if (material.getAlphaMode() == IMaterialData::EAlphaMode::Mask)
            {
                const uint32 texIdx = material.getDiffuseTexIdx();
                if (texIdx < numTextures && coverageCutoff[texIdx] < 0.0f)
                    coverageCutoff[texIdx] = material.getAlphaCutoff();
            }
        }

        const std::string modelFolder = std::filesystem::path(sourcePath).parent_path().string();
        // A recook regenerates every conversion, so clear out the previous set first — otherwise .dds
        // files of textures the scene no longer references would pile up.
        std::error_code ecPurge;
        std::filesystem::remove_all(texFolder, ecPurge);
        bool texFolderCreated = false;
        for (uint32 texIdx = 0; texIdx < numTextures; ++texIdx)
        {
            const ITextureData& texData = *scene.getTexture(texIdx);
            const std::string originalPath = texData.getFileName();
            ctx.textures[texIdx].pathOffset = ctx.strings.add(originalPath);
            if (!options.convertTextures || usage[texIdx] == 0xFF)
                continue;

            // Loose files resolve like Texture.cpp: next to the model first, then Assets-root-relative.
            std::string resolvedFile;
            const bool isEmbedded = texData.getPixels() != nullptr;
            if (!isEmbedded)
            {
                if (originalPath.empty())
                    continue;
                std::error_code ec;
                const std::filesystem::path nextToModel = std::filesystem::path(modelFolder) / originalPath;
                resolvedFile = std::filesystem::exists(nextToModel, ec) ? nextToModel.string() : originalPath;
                if (std::filesystem::path(resolvedFile).extension() == ".dds")
                    continue; // already streamable
                if (!std::filesystem::exists(resolvedFile, ec))
                {
                    Log::warning(std::format("SceneCache: texture '{}' of '{}' not found; keeping original reference", originalPath, sourcePath));
                    continue;
                }
            }

            std::string stem = std::filesystem::path(originalPath).stem().string();
            if (stem.empty() || stem[0] == '*')
                stem = "embedded";
            for (char& c : stem)
                if (!isalnum((uint8)c) && c != '_' && c != '-')
                    c = '_';
            const std::string cookedPath = texFolder + std::to_string(texIdx) + "_" + stem + ".dds";

            if (!texFolderCreated)
            {
                std::error_code ec;
                std::filesystem::create_directories(texFolder, ec);
                texFolderCreated = true;
            }
            if (!convertTextureToDds(texData, resolvedFile, (ETexUsage)usage[texIdx], coverageCutoff[texIdx], cookedPath))
            {
                Log::warning(std::format("SceneCache: failed to convert texture '{}' of '{}'; keeping original reference", originalPath, sourcePath));
                continue;
            }

            ctx.textures[texIdx].pathOffset = ctx.strings.add(cookedPath);
            if (!isEmbedded) // embedded sources are covered by the model file's own stamp
            {
                std::error_code ec;
                CookedTexStamp& stamp = ctx.texStamps.emplace_back();
                stamp.sourcePathOffset = ctx.strings.add(resolvedFile);
                stamp.cookedPathOffset = ctx.strings.add(cookedPath);
                stamp.size = std::filesystem::file_size(resolvedFile, ec);
                stamp.mtime = fileMTimeTicks(resolvedFile, ec);
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
        std::vector<uint32> parentIdx;
        std::deque<std::pair<std::unique_ptr<INodeData>, uint32>> queue; // node -> its cooked index

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
            auto [pNode, cookedIdx] = std::move(queue.front());
            queue.pop_front();
            const uint32 numChildren = pNode->getNumChildren();
            ctx.nodes[cookedIdx].firstChild = (uint32)ctx.nodes.size();
            ctx.nodes[cookedIdx].numChildren = numChildren;
            for (uint32 i = 0; i < numChildren; ++i)
            {
                std::unique_ptr<INodeData> pChild = pNode->getChild(i);
                const uint32 childIdx = appendNode(*pChild, cookedIdx);
                queue.emplace_back(std::move(pChild), childIdx);
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
                std::unique_ptr<FILE, void(*)(FILE*)> file(pFile, [](FILE* p) { fclose(p); });
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
                    stale = ec || srcSize != header.sourceSize || fileMTimeTicks(sourcePath, ec) != header.sourceMTime || ec;
                }
            }
            if (stale)
            {
                std::filesystem::path texFolder = entry.path();
                texFolder.replace_extension();
                texFolder += "_tex";
                std::filesystem::remove(entry.path(), ec);
                std::filesystem::remove_all(texFolder, ec);
                Log::info(std::format("SceneCache: removed stale cache '{}'", entry.path().string()));
            }
        }
    }

    bool cookScene(const ISceneData& scene, const std::string& sourcePath, const std::string& cachePath,
        const std::string& texFolder, const SceneCookOptions& options,
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
        std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);
        FILE* pFile = nullptr;
        fopen_s(&pFile, cachePath.c_str(), "wb");
        if (!pFile)
        {
            Log::warning(std::format("SceneCache: could not write '{}'", cachePath));
            return false;
        }
        const bool ok = fwrite(ctx.blob.data.data(), 1, ctx.blob.data.size(), pFile) == ctx.blob.data.size();
        fclose(pFile);
        return ok;
    }
}

std::unique_ptr<ISceneData> ISceneData::loadCached(const char* filePath, bool mergeNodes, bool preTransformVertices, const SceneCookOptions& options)
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
    optionsHash = fnv1a(std::clamp(options.decimationFactor, 0.0f, 1.0f), optionsHash);
    optionsHash = fnv1a(options.convertTextures, optionsHash);
    optionsHash = fnv1a(options.generateLods, optionsHash);
    optionsHash = fnv1a(options.lodLevels, optionsHash);
    optionsHash = fnv1a(options.lodReduction, optionsHash);
    optionsHash = fnv1a(options.lodMinIndices, optionsHash);

    std::error_code ec;
    const uint64 sourceSize = std::filesystem::file_size(filePath, ec);
    uint64 sourceMTime = 0;
    if (!ec)
        sourceMTime = fileMTimeTicks(filePath, ec);
    const bool cacheUsable = options.enableCache && !ec;

    std::string stem = std::filesystem::path(filePath).stem().string();
    for (char& c : stem)
        if (!isalnum((uint8)c) && c != '_' && c != '-')
            c = '_';
    const std::string cacheTag = std::format("{}_{:08x}", stem, (uint32)(nameHash ^ (nameHash >> 32)));
    const std::string cachePath = "Local/Cooked/" + cacheTag + ".vsc";
    const std::string texFolder = "Local/Cooked/" + cacheTag + "_tex/";

    if (cacheUsable)
    {
        auto cooked = std::make_unique<CookedSceneData>();
        if (cooked->load(cachePath, filePath, sourceMTime, sourceSize, optionsHash))
        {
            Log::info(std::format("SceneCache: '{}' served from '{}'", filePath, cachePath));
            return cooked;
        }
    }

    std::unique_ptr<ISceneData> imported = createAssimpLoader();
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
            auto cooked = std::make_unique<CookedSceneData>();
            if (cooked->load(cachePath, filePath, sourceMTime, sourceSize, optionsHash))
            {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cookStart).count();
                Log::info(std::format("SceneCache: cooked '{}' -> '{}' in {} ms", filePath, cachePath, ms));
                return cooked;
            }
            Log::warning(std::format("SceneCache: read-back of freshly cooked '{}' failed; serving direct import", cachePath));
        }
    }
    return imported;
}

bool TextureConvert::convertToDds(const char* srcPath, EUsage usage, const char* outPath)
{
    int w = 0, h = 0, comp = 0;
    stbi_uc* pDecoded = stbi_load(srcPath, &w, &h, &comp, 4);
    if (!pDecoded)
    {
        Log::warning(std::format("TextureConvert: could not decode '{}'", srcPath));
        return false;
    }
    const bool ok = compressRgbaToDds(pDecoded, (uint32)w, (uint32)h, usage, outPath);
    stbi_image_free(pDecoded);
    return ok;
}

bool TextureConvert::convertPackedToDds(const char* srcPathR, const char* srcPathG, const char* srcPathB, const char* outPath)
{
    const char* srcPaths[3] = { srcPathR, srcPathG, srcPathB };
    std::vector<uint8> rgba;
    uint32 width = 0, height = 0;
    for (int channel = 0; channel < 3; ++channel)
    {
        if (!srcPaths[channel])
            continue;
        int w = 0, h = 0, comp = 0;
        stbi_uc* pGray = stbi_load(srcPaths[channel], &w, &h, &comp, 1);
        if (!pGray)
        {
            Log::warning(std::format("TextureConvert: could not decode '{}'", srcPaths[channel]));
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
            Log::warning(std::format("TextureConvert: '{}' is {}x{}, expected {}x{} (all packed sources must match)", srcPaths[channel], w, h, width, height));
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
