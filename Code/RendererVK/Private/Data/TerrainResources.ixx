export module RendererVK:TerrainResources;

import Core;
import Core.glm;
import :Layout;
import :Settings;
import :BakedWorldMap;

// Everything the renderer holds for the TERRAIN, which the outside (Procedural) pushes in rather than
// loading as a container: the world-scale params, the splat material set and its climate boxes, the
// two tweak blocks, the CPU-baked height/water map the fog and ocean sample, and the fixed-tick
// wetness state machine.
//
// The Renderer keeps buildUboTerrain (every field ends up in the frame UBO) and the record side; this
// owns the STATE those read, and the one piece of logic that is not a straight copy - the wetness tick.
// One terrain splat material: BC-compressed .dds paths (relative to Assets/) plus the climate box the
// ground/rock climate blend matches it against (ignored for beach/snow).
export struct TerrainSplatMaterial
{
    oc::string diffuseDds;
    oc::string normalDds;
    oc::string armDds; // packed AO (R) / roughness (G) / metalness (B); sampled linear
    oc::string heightDds; // optional BC4 height (R, 0..1, 1 = top): the parallax march + the height blend
    // xy = temperature range, already t01; zw = precipitation range in mm/yr. Precipitation stays in real
    // units because its divisor is a live tweak (TerrainTexTweaks::precipFullMm): buildUboTerrain
    // normalizes it every frame. The default is full width on both axes (matches any climate).
    glm::vec4 climate{ 0.0f, 1.0f, 0.0f, 1.0e6f };
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

// Terrain splat shaping (UBO terrainTexParams0..4); tweak-backed, owned by TerrainStreamer
// (Terrain/Textures category) and pushed here every frame.
export struct TerrainTexTweaks
{
    float uvScaleGround = 0.20f;  // 1/m: ~5 m texture repeat on flat ground
    float uvScaleRock = 0.08f;    // 1/m: rock features read larger on cliffs
    float uvScaleSnow = 0.12f;    // 1/m
    float climateBlend = 0.09f;   // Gaussian sigma OUTSIDE a climate box, in (t01,h01) units
    // Slope here is 1 - N.y, so these read as angles: 0.30 = 45 deg, 0.55 = 63 deg. Soil genuinely
    // stops holding around 45, which is also about the steepest the diffusion model's 30 m/px field
    // reaches - a threshold set for a sharper procedural field simply never fires on it.
    float slopeRockStart = 0.30f;
    float slopeRockFull = 0.55f;
    float cragStart = 12.0f;
    float cragFull = 50.0f;
    float beachBand = 2.5f;
    // Snow cover. It is a layer ON TOP of the ground/rock, not a climate entry competing with them:
    // real snow buries soil and bedrock alike and slides off anything steep, which is what makes a
    // cold mountain read as white with bare rock on its faces. It sheds EARLIER than rock appears
    // (0.18 = 35 deg vs 0.30 = 45), so a steepening slope loses its snow first and only then goes to
    // bedrock, rather than flipping between the two at one shared angle.
    // Mean annual temperature below freezing is NOT permanent snow - Siberia averages -10 C and is
    // forest. Permanent cover needs roughly -8 C and colder, so these sit well below 0: measured on the
    // model's own climate, a -2 C snow line covers 15.8% of land and a +2 C one covers 25.4%.
    float snowTempFull = -11.0f;  // C at/below which cover is complete
    float snowTempNone = -3.0f;   // C at/above which there is none
    float snowSlopeStart = 0.18f; // slope where it starts sliding off (~35 deg)
    float snowSlopeFull = 0.45f;  // slope where none remains (~57 deg)
    float snowAridity = 0.10f;    // humidity at/below which cold ground stays bare (polar desert)
    // Crag wander. The crag test measures the surface against the generator's macro altitude, which for
    // V3 is a 7.68 km surface - nearly flat across one mountain - so crag ~= height - constant and the
    // rock boundary traces an elevation contour right across a range. This wanders it. Scaled by the
    // local relief in the shader, so it can only modulate relief that exists and never rocks a plain.
    // TerrainStreamer scales these by V3's world scale, like the crag thresholds.
    float cragWanderAmp = 150.0f;      // metres at the model's true scale; 0 = off
    float cragWanderWavelength = 2000.0f; // metres at the model's true scale
    // Relief from the splat HEIGHT maps (terrain_splat.inc.glsl). Parallax occlusion mapping: ONE march in
    // world space over the height-blended composite of the visible layers, near the camera only. Relief is in
    // metres of the height range 0..1, the mesh at the top (1).
    float parallaxDepthGround = 0.12f; // m: ground, beach and snow
    float parallaxDepthRock = 0.35f;   // m
    float parallaxFadeStart = 15.0f;   // m from the camera: full parallax inside
    float parallaxFadeEnd = 30.0f;     // m: none past it (the march is skipped); 0 = parallax off
    float parallaxSteps = 24.0f;       // linear search steps at a grazing view (~1/NoV, a quarter looking straight on)
    float parallaxShadow = 1.0f;       // relief self-shadow from the sun: 0 = off, 1 = full
    // Height blend: a layer laid over another with coverage w shows where it stands HIGHER, so the borders
    // follow the texture relief (sand in the gaps between rocks) instead of a linear cross-fade. 0 = linear.
    float heightBlendContrast = 3.0f;
    // TESSELLATION (StaticMeshGraphicsPipeline's terrain tess pipeline; terrain_tess.tcs/.tes.glsl): the
    // ground and the overlay pass subdivide near the camera and DISPLACE along the vertex normal by the same
    // height composite the parallax march uses, CENTRED on the mesh (height 0.5 = the mesh), so the flat
    // mesh the TLAS, the collider and the shadow map still see is the relief's mean surface.
    bool  tessEnabled = true;
    float tessMaxFactor = 16.0f;    // per edge; LOD0 is 2 m, so 16 = ~12 cm triangles
    float tessTargetPx = 7.0f;      // screen length of a subdivided edge: smaller costs FS helper lanes
    float tessFadeStart = 15.0f;    // m from the camera: full displacement inside
    float tessFadeEnd = 100.0f;     // m: no displacement and no subdivision past it
    // Shape of the fade between start and end, for the displacement AND the tess factor: strength = 1 - t^p
    // (t = 0..1 across the band). 1 = linear, 2 = quadratic (holds, drops late), 0.5 = square root (drops early).
    float tessFalloffExponent = 1.0f;
    // Closer than this the factor and the height mip use it instead of the camera distance: the vertices and
    // their heights stop changing as the camera approaches (they swam and breathed without it).
    float tessFreezeDistance = 15.0f;
    float tessDepthGround = 0.5f;   // m of relief (height 0..1): ground, beach, snow
    float tessDepthRock = 0.5f;     // m
    // mm/yr that reads as humidity 1.0: the divisor for TerrainSplatMaterial::climate's precipitation.
    // Mirrors the generator's live "Terrain/V3/Precip for full humidity" tweak; if the two drift, the
    // whole climate table slides along the humidity axis.
    float precipFullMm = 2200.0f;
};

// Terrain wetness clipmap (TerrainWetnessPipeline): the decaying memory of where water touched the
// ground - the ocean swash tongue, permanently submerged seabed, rain - read by the TERRAIN shader
// to darken and gloss it. A TERRAIN_WET_RES^2 toroidal window of texelSize metres around the scene
// focus. Pushed every frame by the terrain streamer (mirrors its "Terrain/Wetness" tweaks).
export struct TerrainWetTweaks
{
    bool enabled = false;
    float texelSize = 0.5f;      // m; 1024 texels = 512 m of coverage
    float dryTime = 90.0f;       // s for wetness to decay to 1/e on cool ground
    float dryTempSens = 0.00f;   // extra decay rate per C above 15 C (warm sand dries faster); 0 = uniform
    float dryRate = 0.005f;      // 1/s: the CONSTANT part of the drain, next to the proportional dry time -
                                 // d(wet)/dt = rain - dryRate - wet / dryTime, so rain below the rate never
                                 // keeps ground wet and above it settles at dryTime x (rain - dryRate)
    float rain = 0.0f;           // wetness added per second everywhere (0 = no rain)
    float wetInTime = 0.4f;      // s for ground under water to reach full wetness (0 = instant)
    float filmDepth = 0.03f;     // m of water over which the wetting target ramps 0 -> 1 (softens the tongue edge)
    float diffusionRate = 15.0f; // 1/s: sideways spread through the 3x3 tent (packed per frame as
                                 // 1 - exp(-rate * dt), so it is framerate independent); the toggle is
                                 // the pipeline's own "Diffusion" tweak (a baked define)
    float updateRate = 20.0f;    // Hz: the pass runs on a FIXED TICK with the accumulated sim delta, not
                                 // every frame - at high fps a per-frame change is below the R16F image's
                                 // step (0.0005 at wetness 0.5) and rounds away, so rain and drying stalled
                                 // there and wetness depended on the framerate. Keep it well under the fps.
    // Two darkening layers (the terrain shader): DAMP = soaked ground everywhere, a plateau above the
    // damp knee that fades smoothly to dry below it; FILM = standing water on top - the whole surface
    // just after a wave (above the spike start) and the pools once it drains. Fully wet = both.
    float albedoScale = 0.55f;   // FILM albedo multiplier (on top of damp)
    float dampAlbedoScale = 0.75f; // DAMP albedo multiplier
    float dampKnee = 0.25f;      // wetness below which damp fades to dry (0.25 = ~1.4 dry times of plateau)
    float spikeStart = 0.7f;     // wetness above which the whole surface carries the film darkening
    float roughness = 0.15f;     // roughness at full wetness
    // Pooling: as the ground dries, the film retreats into the low spots of a world-anchored noise
    // (the crevices), so gloss breaks up into blobs instead of fading uniformly.
    float poolScale = 3.0f;      // 1/m noise scale (~30 cm pools; 0 = off, uniform film)
    float poolSoftness = 0.15f;  // noise band around the wetness that half-pools (edge softness)
    float dampGloss = 0.3f;      // fraction of the roughness drop the damp ground between pools keeps
    float poolHold = 2.0f;       // >= 1: pool threshold = wet^(1/hold), so the crevices keep their
                                 // water long after the surface between them has drained
    float slopeDrain = 4.0f;     // steep ground sheds water: decay rate x (1 + slope * drain) in the
                                 // terrain shader (per pixel, mesh normal) AND wet-in / rain rate
                                 // / (1 + slope * drain) in the compute pass (map gradient, 8 m);
                                 // slope = 1 - N.y (a 45-degree face ~2.2x at 4, a wall 5x); 0 = off
    // Surface water: where the WETNESS is still near full the terrain shader draws the ground AS
    // water - the ocean shader's Fresnel sky reflection and dielectric sun glint over the lit ground
    // - so the ocean's depth-buffer intersection with the sand lands on ground that already looks
    // like water. Keyed on the smooth wetness field, not the pooled noise (that drew water blobs);
    // it ramps over [threshold - softness, threshold + softness] so it blends out, never edges.
    float surfaceThreshold = 0.7f; // wetness at which the water look is half in
    float surfaceSoftness = 0.25f; // half-width of the ramp
    float surfaceWaviness = 0.5f;  // film normal: 0 = the level water plane, 1 = the live FFT wave normal
    float surfaceDepth = 0.15f;    // m of virtual water the ground is tinted through (the ocean's
                                   // absorption + in-scatter), so the colours match at the waterline
    float rippleStrength = 0.1f;   // inland film: the finest ocean cascade's slope weight where the shore
                                   // weight is 0 (0 = off). Amplitude follows the ocean's wind via the spectrum
    float surfaceNormalScale = 1.0f; // film wave normal strength, on top of the ocean's "Normal strength"
    float liveMargin = 0.08f;      // m: the film + gloss stay on ground up to this far BELOW the estimated
                                   // live surface (the estimate sits under the drawn ocean edge: no choppy
                                   // XZ, no tongue thickness - without the margin a bare band shows above
                                   // the waterline)
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

    void setTexTweaks(const TerrainTexTweaks& params) { m_texTweaks = params; }
    const TerrainTexTweaks& getTexTweaks() const { return m_texTweaks; }
    void setWetTweaks(const TerrainWetTweaks& params) { m_wetTweaks = params; }
    const TerrainWetTweaks& getWetTweaks() const { return m_wetTweaks; }

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
    const uint16* getSplatHeightTex() const { return m_splatHeightTex; } // per slot, UINT16_MAX = no height map

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
    uint16 m_splatHeightTex[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{}; // read only once a set is registered
    TerrainTexTweaks m_texTweaks;
    TerrainWetTweaks m_wetTweaks;

    BakedWorldMap m_heightMap;
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_descGen{};

    glm::ivec2 m_wetPrevOrigin = glm::ivec2(0);
    bool m_wetWasEnabled = false; // the window was live last frame (else every texel starts dry)
    float m_wetTickAccum = 0.0f;
    bool m_wetTicking = false;
    uint32 m_wetLayer = 0;
};
