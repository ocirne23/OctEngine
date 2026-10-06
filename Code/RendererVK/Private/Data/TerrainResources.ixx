export module RendererVK:TerrainResources;

import Core;
import Core.glm;
import :Layout;
import :RenderParams;
import :BakedWorldMap;

// Everything the renderer holds for the TERRAIN, which the outside (Procedural) pushes in rather than
// loading as a container: the world-scale params, the splat material set and its climate boxes, the
// CPU-baked height/water map the fog and ocean sample, and the fixed-tick wetness state machine (its
// tweaks are Globals::settings.terrain.wet*).
//
// The Renderer keeps buildUboTerrain (every field ends up in the frame UBO) and the record side; this
// owns the STATE those read, and the one piece of logic that is not a straight copy - the wetness tick.
// One terrain splat material: BC-compressed .dds paths (relative to Assets/) plus the climate box the
// ground/rock climate blend matches it against (ignored for beach/snow).
export struct TerrainSplatMaterial
{
    // THREE textures per material, packed so the splat makes one fetch fewer per layer (no ARM texture):
    oc::string diffuseDds; // sRGB albedo (RGB) + linear ROUGHNESS (A), BC3
    oc::string normalDds;  // tangent normal, BC5 (XY)
    oc::string heightDds;  // HEIGHT (R, 0..1, 1 = top; 0.5 = flat) + AO (G), BC5: the parallax march, the height blend
    // Metalness is always 0 (terrain is never metallic).
    // xy = temperature range, already t01; zw = precipitation range in mm/yr. Precipitation stays in real
    // units because its divisor is a live tweak ("Terrain/V3/Precip for full humidity"): buildUboTerrain
    // normalizes it every frame. The default is full width on both axes (matches any climate).
    glm::vec4 climate{ 0.0f, 1.0f, 0.0f, 1.0e6f };
    // How much GRASS grows where this texture shows (0..1; the grass cull weighs it by the same layer coverages and
    // climate picks the splat draws). Rock, beach and snow are 0.
    float grass = 0.0f;
};

// Layout of a registered set, in slot order [ground][rock][beach?][snow?] (the shader composites
// ground -> beach -> rock -> snow; see Renderer::setTerrainSplatMaterials). The beach and snow
// entries are OVERLAYS, not materials the climate blend can pick: beach paints over the waterline
// whatever the climate, and snow paints over everything else (ground AND rock) where it is cold
// enough and the slope is shallow enough to hold it.
export struct TerrainSplatCounts
{
    uint32 numGround = 0;
    uint32 numRock = 0;
    bool hasBeach = false;
    bool hasSnow = false;
};

export class TerrainResources final
{
public:
    void initialize()
    {
        // RGBA: terrain height, water level, fog thickness, spare.
        m_heightMap.initialize(RendererVKLayout::FOG_TERRAIN_RES, RendererVKLayout::FOG_TERRAIN_CASCADES, 4, "FogTerrainHeight");
    }

    // ---- Pushed in by Procedural ----
    void setParams(float meshRadius, float lapseRate, float seaLevel) { m_params = glm::vec4(meshRadius, glm::min(lapseRate, 0.0f), seaLevel, 0.0f); }
    const glm::vec4& getParams() const { return m_params; } // x = 0 disables the ocean land cull
    float getMeshRadius() const { return m_params.x; }

    // The splat set: a CONTIGUOUS material range plus the textures it owns. Registering a new set
    // returns the textures the old one owned, for the caller's deferred-free queue (they may still be
    // sampled in flight). addMaterials is the Renderer's material table; uploadTexture its texture one.
    struct SplatUpload
    {
        oc::function<uint32(const oc::vector<RendererVKLayout::MaterialInfo>&)> addMaterials;
        oc::function<uint16(const oc::string& path, bool sRGB)> uploadTexture;
        oc::function<bool(uint16 texIdx)> isBc5Normal;
    };
    // Returns the retired set's textures; the caller queues them for a deferred free.
    oc::vector<uint16> setSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts, const SplatUpload& io);

    int32 getSplatBaseMaterial() const { return m_splatBaseMaterial; } // -1 = no set (flat-colour fallback)
    const TerrainSplatCounts& getSplatCounts() const { return m_splatCounts; }
    oc::span<const uint16> getSplatTextures() const { return m_splatTextures; }
    const glm::vec4* getSplatClimate() const { return m_splatClimate; } // per slot, TerrainSplatMaterial::climate units
    const float* getSplatGrass() const { return m_splatGrass; }         // per slot, TerrainSplatMaterial::grass
    const uint16* getSplatHeightTex() const { return m_splatHeightTex; } // per slot, UINT16_MAX = no height map
    // Per slot: x = diffuse | normal << 16, y = 1 when the normal map is BC5.
    const glm::uvec2* getSplatTex() const { return m_splatTex; }

    // ---- The CPU-baked height/water map (fog's height base, the ocean's coarse shore fallback) ----
    BakedWorldMap& getHeightMap() { return m_heightMap; }
    const BakedWorldMap& getHeightMap() const { return m_heightMap; }
    // Last flip generation written into each frame slot's descriptor sets (0 = never).
    uint32 getDescGen(uint32 frameIdx) const { return m_descGen[frameIdx]; }
    void setDescGen(uint32 frameIdx, uint32 generation) { m_descGen[frameIdx] = generation; }

    // ---- The fixed-tick wetness clipmap ----
    // A texel keeps its wetness only if its coord was inside the LAST TICK's window, so a window that
    // was not live last frame (first enable, re-enable) parks the previous origin out of range and
    // every texel starts dry instead of inheriting a stale slot.
    //
    // The pass runs only when the accumulated sim delta reaches the tick interval, and integrates that
    // whole delta at once: per frame the change was below the R16F image's representable step at high
    // fps and rounded away, so wetness depended on the framerate. Between ticks the pass is skipped and
    // the reader keeps the last tick's layer + origin.
    struct WetnessTick
    {
        bool ticking = false;   // this frame runs the pass
        float dt = 0.0f;        // 0 on skipped frames: the rate terms are then unused
        glm::ivec2 origin{ 0 }; // this tick's window origin (lattice coord)
        glm::ivec2 prevOrigin{ 0 };
        uint32 writeLayer = 0;  // ping/pong: a tick writes the layer the last tick did not
    };
    // simDeltaSec is the SIM delta (frozen by the global pause, like the particles); the caller caps it.
    WetnessTick advanceWetness(float simDeltaSec, const glm::vec3& focus);
    bool isWetnessTicking() const { return m_wetTicking; } // the primary record reads this frame's decision

private:
    glm::vec4 m_params{ 0.0f };
    int32 m_splatBaseMaterial = -1;
    TerrainSplatCounts m_splatCounts;
    oc::vector<uint16> m_splatTextures; // for the per-frame streaming noteUse + replacement frees
    glm::vec4 m_splatClimate[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{};
    float m_splatGrass[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{};
    uint16 m_splatHeightTex[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{}; // read only once a set is registered
    glm::uvec2 m_splatTex[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{};    // see getSplatTex

    BakedWorldMap m_heightMap;
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_descGen{};

    glm::ivec2 m_wetPrevOrigin = glm::ivec2(0);
    bool m_wetWasEnabled = false; // the window was live last frame (else every texel starts dry)
    float m_wetTickAccum = 0.0f;
    bool m_wetTicking = false;
    uint32 m_wetLayer = 0;
};
