module RendererVK;

import Core;
import Core.glm;
import Settings;
import :TerrainResources;
import :Layout;

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
