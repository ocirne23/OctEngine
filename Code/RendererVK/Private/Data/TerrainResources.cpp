module RendererVK;

import Core;
import Core.glm;
import Settings;
import :TerrainResources;
import :TextureManager;
import :Layout;

namespace
{
    // Integer lattice hash -> [0, 1).
    float latticeHash(uint32 x, uint32 y, uint32 seed)
    {
        uint32 h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return (float)(h >> 8) * (1.0f / 16777216.0f);
    }

    // Tileable value noise at p (lattice cells, p >= 0) over a `period`-cell lattice: [0, 1], smoothstep fade - the shape
    // of terrain_splat.inc.glsl's terrainValueNoise, which the crag wander used before the texture.
    float tileValueNoise(glm::vec2 p, uint32 period, uint32 seed)
    {
        const glm::vec2 i = glm::floor(p);
        const glm::vec2 f = p - i;
        const glm::vec2 u = f * f * (3.0f - 2.0f * f);
        const uint32 x0 = (uint32)i.x % period, y0 = (uint32)i.y % period;
        const uint32 x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
        return glm::mix(glm::mix(latticeHash(x0, y0, seed), latticeHash(x1, y0, seed), u.x),
                        glm::mix(latticeHash(x0, y1, seed), latticeHash(x1, y1, seed), u.x), u.y);
    }

    // The gradient noise's 16 directions: (cos, sin) of k * 22.5 deg.
    constexpr uint32 GRAD_DIRS = 16;
    constexpr float GRAD_DIR[GRAD_DIRS][2] = {
        {  1.0f,         0.0f        }, {  0.92387953f,  0.38268343f }, {  0.70710678f,  0.70710678f }, {  0.38268343f,  0.92387953f },
        {  0.0f,         1.0f        }, { -0.38268343f,  0.92387953f }, { -0.70710678f,  0.70710678f }, { -0.92387953f,  0.38268343f },
        { -1.0f,         0.0f        }, { -0.92387953f, -0.38268343f }, { -0.70710678f, -0.70710678f }, { -0.38268343f, -0.92387953f },
        {  0.0f,        -1.0f        }, {  0.38268343f, -0.92387953f }, {  0.70710678f, -0.70710678f }, {  0.92387953f, -0.38268343f },
    };

    // Tileable gradient noise, ~[-0.7, 0.7], quintic fade: no lattice-aligned creases in the macro variation.
    float tileGradientNoise(glm::vec2 p, uint32 period, uint32 seed)
    {
        const glm::vec2 i = glm::floor(p);
        const glm::vec2 f = p - i;
        const glm::vec2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
        const uint32 x0 = (uint32)i.x % period, y0 = (uint32)i.y % period;
        const uint32 x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
        const auto corner = [&](uint32 x, uint32 y, glm::vec2 d) {
            const float* g = GRAD_DIR[(uint32)(latticeHash(x, y, seed) * GRAD_DIRS)];
            return g[0] * d.x + g[1] * d.y;
        };
        return glm::mix(glm::mix(corner(x0, y0, f), corner(x1, y0, f - glm::vec2(1.0f, 0.0f)), u.x),
                        glm::mix(corner(x0, y1, f - glm::vec2(0.0f, 1.0f)), corner(x1, y1, f - glm::vec2(1.0f, 1.0f)), u.x), u.y);
    }
}

void TerrainResources::createNoiseTexture()
{
    constexpr uint32 N = NOISE_SIZE;
    oc::vector<glm::vec3> values(N * N);
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (uint32 y = 0; y < N; ++y)
        for (uint32 x = 0; x < N; ++x)
        {
            const glm::vec2 t = (glm::vec2((float)x, (float)y) + 0.5f) / (float)N; // texel centre, 0..1 over the tile
            glm::vec2 macro(0.0f);
            for (uint32 o = 0; o < 4; ++o)
            {
                const uint32 cells = NOISE_MACRO_CELLS << o;
                const float amp = 1.0f / (float)(1u << o);
                macro.x += amp * tileGradientNoise(t * (float)cells, cells, 1u + o);
                macro.y += amp * tileGradientNoise(t * (float)cells, cells, 101u + o);
            }
            float crag = 0.0f, norm = 0.0f;
            for (uint32 o = 0; o < 3; ++o)
            {
                const uint32 cells = NOISE_CRAG_CELLS << o;
                const float amp = 1.0f / (float)(1u << o);
                crag += amp * tileValueNoise(t * (float)cells, cells, 201u + o);
                norm += amp;
            }
            values[y * N + x] = glm::vec3(macro, crag / norm);
            lo = glm::min(lo, macro);
            hi = glm::max(hi, macro);
        }

    // The macro channels stretched to the full 0..1 (the shader centres them on 0.5); the crag channel keeps the fBm's
    // own range, so "Crag wander (m)" means what it did.
    oc::vector<oc::vector<uint8>> levels;
    levels.emplace_back(N * N * 4);
    const glm::vec2 invRange = 1.0f / glm::max(hi - lo, glm::vec2(1e-6f));
    for (uint32 i = 0; i < N * N; ++i)
    {
        const glm::vec2 m = (glm::vec2(values[i]) - lo) * invRange;
        uint8* px = &levels[0][i * 4];
        px[0] = (uint8)(glm::clamp(m.x, 0.0f, 1.0f) * 255.0f + 0.5f);
        px[1] = (uint8)(glm::clamp(m.y, 0.0f, 1.0f) * 255.0f + 0.5f);
        px[2] = (uint8)(glm::clamp(values[i].z, 0.0f, 1.0f) * 255.0f + 0.5f);
        px[3] = 255;
    }
    // 2x2 box mips down to 1x1 (smooth noise: a box is enough).
    for (uint32 size = N / 2; size >= 1; size /= 2)
    {
        const oc::vector<uint8>& src = levels.back();
        oc::vector<uint8> dst(size * size * 4);
        const uint32 srcSize = size * 2;
        for (uint32 y = 0; y < size; ++y)
            for (uint32 x = 0; x < size; ++x)
                for (uint32 c = 0; c < 4; ++c)
                {
                    const auto at = [&](uint32 sx, uint32 sy) { return (uint32)src[(sy * srcSize + sx) * 4 + c]; };
                    dst[(y * size + x) * 4 + c] = (uint8)((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        levels.push_back(oc::move(dst));
    }
    oc::vector<oc::span<uint8>> mips;
    for (oc::vector<uint8>& level : levels)
        mips.push_back(oc::span<uint8>(level.data(), level.size()));
    const uint16 idx = Globals::textureManager.uploadRgba8Mips(N, N, mips, false, "TerrainNoise");
    if (idx != UINT16_MAX)
        m_noiseTex = idx;
}

oc::vector<uint16> TerrainResources::setSplatMaterials(oc::span<const TerrainSplatMaterial> mats,
    const TerrainSplatCounts& counts, const SplatUpload& io)
{
    assert(mats.size() <= RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS);
    assert((size_t)counts.numGround + counts.numRock + (counts.hasBeach ? 1 : 0) + (counts.hasSnow ? 1 : 0) == mats.size()
        && "counts must describe the whole [ground][rock][beach?][snow?] span");
    // Replacing a live set: the old images may still be sampled in flight, so they go back to the
    // caller for its deferred free path. The old material slots are not recycled (nothing tracks their
    // range), but a set replacement is a rare config-refresh event.
    oc::vector<uint16> retired = oc::move(m_splatTextures);
    m_splatTextures.clear();

    oc::vector<RendererVKLayout::MaterialInfo> materialInfos;
    materialInfos.reserve(mats.size());
    oc::fill(oc::begin(m_splatHeightTex), oc::end(m_splatHeightTex), UINT16_MAX);
    oc::fill(oc::begin(m_splatTex), oc::end(m_splatTex), glm::uvec2(0u));
    for (const TerrainSplatMaterial& mat : mats)
    {
        RendererVKLayout::MaterialInfo& info = materialInfos.emplace_back();
        info.flags = 0;
        info.opacity = 1.0f;
        info.alphaMode = (uint16)RendererVKLayout::EAlphaMode::Opaque;
        info.diffuseTexIdx = RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX;
        info.normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
        info.metalRoughnessTexIdx = UINT16_MAX;

        const auto upload = [&](const oc::string& path, bool sRGB) -> uint16 {
            if (path.empty())
                return UINT16_MAX;
            const uint16 idx = io.uploadTexture(path, sRGB);
            if (idx != UINT16_MAX)
                m_splatTextures.push_back(idx);
            return idx;
        };
        if (const uint16 idx = upload(mat.diffuseDds, true); idx != UINT16_MAX)
            info.diffuseTexIdx = idx;
        if (const uint16 idx = upload(mat.normalDds, false); idx != UINT16_MAX)
        {
            info.normalTexIdx = idx;
            if (io.isBc5Normal(idx))
                info.flags |= RendererVKLayout::MATERIAL_FLAG_BC5_NORMAL;
        }
        // MaterialInfo has no free slot: the height + AO index rides the UBO per slot, like the climate box.
        const size_t slot = materialInfos.size() - 1;
        m_splatHeightTex[slot] = upload(mat.heightDds, false);
        // The splat's texture indices ride the UBO too (terrain_splat.inc.glsl), so its texture fetches do not
        // wait on a material-buffer load first. The materials stay registered for everyone else (their
        // roughness sits in the diffuse alpha, which only the splat knows: metalRoughnessTexIdx stays none).
        const uint32 bc5 = (info.flags & RendererVKLayout::MATERIAL_FLAG_BC5_NORMAL) != 0 ? 1u : 0u;
        m_splatTex[slot] = glm::uvec2(uint32(info.diffuseTexIdx) | (uint32(info.normalTexIdx) << 16), bc5);
    }

    m_splatBaseMaterial = (int32)io.addMaterials(materialInfos);
    m_splatCounts = counts;
    for (size_t i = 0; i < mats.size(); ++i)
    {
        m_splatClimate[i] = mats[i].climate;
        m_splatGrass[i] = mats[i].grass;
    }
    return retired;
}

TerrainResources::WetnessTick TerrainResources::advanceWetness(float simDeltaSec, const glm::vec3& focus)
{
    const TerrainSettings& s = Globals::settings.terrain;
    const float texel = terrainWetTexelSize(s);
    constexpr int32 res = (int32)RendererVKLayout::TERRAIN_WET_RES;
    const bool wasEnabled = m_wetWasEnabled;
    m_wetWasEnabled = s.wetEnabled;
    m_wetTickAccum = s.wetEnabled ? m_wetTickAccum + simDeltaSec : 0.0f;
    const float interval = 1.0f / glm::max(s.wetUpdateRate, 1.0f);
    m_wetTicking = s.wetEnabled && (!wasEnabled || m_wetTickAccum >= interval);

    WetnessTick tick;
    tick.ticking = m_wetTicking;
    tick.origin = m_wetPrevOrigin;
    tick.prevOrigin = m_wetPrevOrigin;
    if (m_wetTicking)
    {
        tick.dt = oc::min(m_wetTickAccum, 1.0f);
        m_wetTickAccum = 0.0f;
        tick.origin = glm::ivec2(glm::floor(glm::vec2(focus.x, focus.z) / texel)) - res / 2;
        tick.prevOrigin = wasEnabled ? m_wetPrevOrigin : tick.origin + res * 2;
        m_wetPrevOrigin = tick.origin;
        m_wetLayer = 1u - m_wetLayer; // write the other layer, read the last tick's
    }
    // Ping/pong layer: a tick writes the layer the last tick did not (the pass reads the other one);
    // between ticks this names the last written layer, which the terrain shader keeps sampling.
    tick.writeLayer = m_wetLayer;
    return tick;
}
