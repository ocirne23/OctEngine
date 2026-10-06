export module RendererVK:RenderParams;

import Core;
import Core.glm;
// Every renderer settings type (FogParams, SkyParams, ...): re-exported, so RendererVK's importers keep seeing them.
export import Settings.Render;

// The pushed / reported parameter blocks that are not settings: OceanParams (Procedural::OceanGenerator builds it
// every frame) and the Stats snapshot.

// FFT/Tessendorf ocean: spectrum inputs for the GPU simulation (OceanSimulationPipeline) + water shading.
// The Renderer feeds these into the per-frame UBO (ocean* fields), which drives BOTH the compute simulation
// (the TMA spectrum is re-evaluated every frame, so all of it is live) and the surface shading. The grid
// geometry + Tweaks live in Procedural::OceanGenerator, which builds one of these each frame and hands it to
// Renderer::setOceanParams.
export struct OceanParams
{
    bool enabled = false;       // gates the per-frame FFT simulation + ocean draw

    // Spectrum (TMA = JONSWAP x Kitaigorodskii finite-depth attenuation, Hasselmann directional spreading;
    // Horvath 2015). Dispersion is finite-depth: w^2 = g k tanh(k D).
    glm::vec2 windDirection = glm::normalize(glm::vec2(0.8f, 0.35f)); // dominant wave travel direction (XZ)
    float windSpeed   = 10.5f;  // U10 wind speed (m/s): the main sea-state knob
    float fetchKm     = 300.0f; // fetch (km): distance the wind has blown over; longer = bigger swell
    float depth       = 100.0f; // ocean depth D (m): finite-depth dispersion + TMA shallow-water attenuation
                                // (shallow values like 35 visibly mute the long swell - by design)
    float horizonLevelOffset = -0.5f; // vertical shift (m, usually negative) of the HORIZON BAND only.
                                // The band is exempt from the land cull (its triangles are far larger
                                // than the cull's footprint bound), so it draws over distant terrain;
                                // sinking it a little keeps its crests under near-sea-level ground.
    float amplitude   = 1.0f;   // artistic scale on the spectrum amplitude (1 = physical)
    float choppiness  = 1.25f;   // horizontal displacement lambda (0 = heightfield only, higher = sharper crests)
    float normalStrength = 1.0f; // artistic scale on the shading slopes
    glm::vec3 cascadeSizes = glm::vec3(1536.0f, 188.0f, 25.0f); // FFT patch sizes (m); each TILES with its
                                // own size, so the largest sets how often the sea repeats - keep it many
                                // times the peak wavelength. Non-rational ratios keep the three from
                                // re-aligning; scale them as a SET (the band split ties cascade c+1's
                                // range to L_c, so growing one alone just moves the repetition down)
    float seaLevel    = 0.0f;   // world Y of the calm water plane
    float detailBias  = 0.0f;   // bias on the ring-matched vertex displacement mip (negative = finer;
                                // the clipmap rings carry their cell size per vertex, so 0 is motion-stable)

    // Optics: extinction drives Beer-Lambert absorption of the ray-traced refraction (1/m, Jerlov-ish
    // coastal water); scatter is the in-scattered radiance albedo (the water's "color" in deep water).
    glm::vec3 absorption   = glm::vec3(0.42f, 0.085f, 0.04f);
    glm::vec3 scatterColor = glm::vec3(0.012f, 0.08f, 0.085f);
    float scatterStrength  = 1.0f;
    float roughness        = 0.07f; // perceptual micro-roughness (widens the sun glint)
    float glintFilter      = 0.5f;  // 1 = full variance widening, 0 = none (raw sharp GGX)
    // Slope variance of the waves BELOW the finest cascade's Nyquist - the capillary band the FFT cannot
    // represent at any distance. The LEAN term only returns variance the MIP CHAIN filtered away, and it
    // is exactly 0 at mip 0, so near the camera the roughness used to collapse onto its 0.02 clamp and
    // the water mirrored the sky (plastic). Added to alpha^2 in the same form as LEAN (alpha^2 = 2 sigma^2)
    // and deliberately NOT scaled by glintFilter: this band is missing from the spectrum, not filtered
    // out of it. Dimensionless, so the world scale leaves it alone.
    float microRoughness   = 0.008f;
    // Rational soft limit on the shading slope, s /= 1 + k * |s| (ocean_wave.inc.glsl). It keeps the
    // near-fold division from exploding into dark creases, but it compresses exactly the steep crest
    // faces, which reads as blobby. 0 = no limit (sharpest crests, creases possible at folds).
    float crestSlopeLimit  = 0.0f;
    // Sub-band detail (oceanDetailSlope): the finest cascade's gradient field re-sampled at
    // detailScale x its patch size, in a domain rotated by detailRotation, added to the SHADING slope
    // only. Wave statistics at a shorter wavelength than the FFT band holds, for one fetch - no extra
    // memory, no extra FFT, and the displacement (so geometry, prepass and the CPU buoyancy mirror) is
    // untouched. Faded out past detailFadeDist because this band is absent from the LEAN moments.
    float detailStrength   = 0.35f; // 0 = off
    float detailScale      = 0.18f; // fraction of the finest cascade's patch size (smaller = finer)
    float detailFadeDist   = 60.0f; // m from the camera (a world metre: scaled); 0 = never fade
    float detailRotation   = 0.9f;  // radians; keeps the borrowed field off the parent's crest lines
    // Crest subsurface scattering (Sea of Thieves-style): back-lit wave crests glow the scatter color,
    // scaled by height above the calm water line. Power shapes the toward-the-sun view lobe.
    float sssStrength      = 0.75f;  // per meter of crest height; 0 disables (and the extra shadow rays with it)
    float sssPower         = 1.0f;
    float undersideTransmission = 1.0f; // scale on the sky seen through Snell's window from below (1 = Fresnel
                                        // transmission; less = more internal reflection, a darker ceiling)
    bool  hitLighting      = false; // evaluate the scene's grid lights at refraction/reflection ray hits
                                    // (OCEAN_HIT_LIGHTS shader variant; toggling reloads the pipeline)
    bool  rtReflections    = true;  // ray-traced mirror of the scene on the top side (OCEAN_RT_REFLECTIONS
                                    // shader variant; off = sky only, no mirror ray compiled in)
    int   debugMode        = 0;     // OCEAN_DEBUG_MODE shader variant (mode list: ocean.fs.glsl); 0 = off
    // Foam. ONE instant-foam response (oceanInstantFoam) both draws the per-pixel crest foam and injects
    // the world-space FOAM FIELD (the foam amount breaking leaves stuck to the water): white foam above
    // foamThreshold, the bubble cloud (and its roughness) from the same amount below it.
    glm::vec3 foamColor    = glm::vec3(0.88f, 0.92f, 0.94f);
    float foamBias         = 0.6f;  // fold threshold: Jacobian below this is folding (foaming)
    float foamBreakAccel   = 0.25f; // breaking threshold (Longuet-Higgins): downward crest acceleration
                                    // above this fraction of g is breaking - what makes LARGE waves foam
    float foamSoftness     = 0.5f;  // edge width of both thresholds (small = crisp crest lines)
    float bubbleDepth      = 3.0f;  // m under the surface: the bubble cloud's water absorbs the red both
                                    // ways, so deeper = darker and more turquoise (a world metre: scaled)
    float bubbleBrightness = 1.0f;  // the cloud's albedo, x foam color
    float bubbleBlur       = 4.0f;  // m (world): the cloud reads the foam field this blurred (a diffuse volume)    // The world-space foam field (ocean_foam.cs.glsl): SURFACE FOAM sticks to the water it formed on (the
    // rest lattice, so it rides the orbits and stays behind as the crest moves on) and drifts downwind.
    float foamSurfaceDecay    = 0.995f; // foam amount retention per frame
    float foamSurfaceStrength = 1.0f;   // display scale on the stuck foam (0 = only crest foam, as before)
    float foamTexel        = 0.5f;  // level 0 texel (m, world); level l = x 4^l, 512^2 texels per level
    // The stuck foam's coverage (oceanStuckFoamCoverage): a threshold on its density amount / Jacobian.
    float foamThreshold    = 0.5f;  // density where the foam turns on
    float foamEdge         = 0.06f; // threshold half-width: smaller = crisper foam edges
    float foamFineWaves    = 0.25f; // 0..1: the finest cascade's share in the Jacobian the foam reads (lower =
                                    // steadier foam shapes; the film's crest + shore foam too)
    float foamDetail       = 1.5f;  // scale on the sub-band detail slope in the foam's lighting normal (the
                                    // large waves' slope is eased by foamFlatten instead)
    float foamDriftSpeed   = 0.3f;  // m/s (world) along the swell's travel: the wind drift of the surface
    float foamFlatten      = 0.6f;  // 0..1: the foam's Lambert normal eased toward up (bent crests stop
                                    // going dark at grazing sun angles)
    bool  cameraUnderwater = false; // per frame, from the CPU mirror: the camera is below the live surface.
                                    // Gates the shader's underside path - a back face seen from above is a fold

    // Shore interaction: the baked terrain-data cascades (Renderer::setFogTerrainHeightMap, baked by the
    // terrain streamer) give the water its depth - open water eases to the swash amplitude across an
    // approach band at the shore (oceanSurfaceWeight, ocean_wave.inc.glsl), the swash tongue runs up the
    // beach and flows back, and a surf/foam band forms where the water column vanishes at the waterline.
    float shoalScale     = 0.005f; // approach band depth as a fraction of the mid cascade's patch size
                                // (floored at two swash reaches; scaled down with the cascade sizes)
    // Horizon depth: past horizonDepthRange the waves assume AT LEAST horizonDepth of water, whatever
    // the baked map says. Every distant depth error runs shallow - coarse texels average shore slopes
    // into the water, the generator reports depth exactly 0 for samples it could not resolve, and the
    // vertical scale compresses real shelves - and shallow is the ruinous direction: it fades the waves
    // out AND (via fade^2) the LEAN variance, leaving a mirror that reflects the sky exactly like wind 0.
    // Only the assumed seabed moves, never the surface, so it cannot put water over land; the land cull
    // still reads the raw map. 0 range = off.
    float horizonDepth      = 30.0f;
    float horizonDepthRange = 1500.0f;
    // Rate of the spectrum's clock relative to the frame clock (1 = real time). OceanGenerator sets
    // sqrt(world scale): its Froude-scaled inputs give a shrunk sea whose periods are x sqrt(s), and this
    // slows the evolution back to the model sea's periods so the miniature does not race. Only the
    // e^{iwt} evolution reads it - the breaking-crest acceleration stays in the spectrum's own time, so
    // the foam criterion (a fraction of g) keeps the model sea's look.
    float timeScale = 1.0f;
    // "Ocean/World scale" itself (s; 1 = the model sea). Nothing above re-applies it - they arrive in world
    // metres already. The spray and the ocean-bound fog metres read it: the model sea at model periods,
    // shrunk by s, so their lengths, speeds and accelerations all ride s.
    float worldScale = 1.0f;
    float shoreFoamDepth = 8.0f;  // water-column height (m) below which the waterline churns white; 0 = off
    float shoreFoamMax   = 0.75f; // surf band opacity cap: shore foam coverage never exceeds this, so the
                                  // refracted bottom stays visible through the lace (whitecaps unaffected)
    float swashAmp       = 0.5f;  // swash run-up: scale on the un-shoaled wave height riding through the
                                  // waterline and up the beach (waves crash and flow over; 0 = hard cutoff)
    float shoreFoamBias  = -0.33f;  // shifts the surf fold threshold: negative = sparser lace / more
                                  // transparent shore waves, positive = denser churn
    float swashFlow      = 0.33f;  // backflow: scale on the raw horizontal chop riding the swash weight -
                                  // the tongue visibly flows back seaward as the wave recedes (0 = off)
    float cullMargin     = 1.0f;  // land cull: clipmap triangles whose whole footprint is buried deeper
                                  // than this under the local water level are VS-culled (0 = off)
    float farCullError = 4.0f;    // land cull from the FAR terrain cascade (beyond the near cascade's
                                  // ~860 m): flat burial error allowance in METERS, covering how far
                                  // the far mesh LODs stray from the bake. Deliberately NOT scaled by
                                  // the far texel - that left everything under tens of meters of
                                  // terrain alive (a visible band of buried water past the near
                                  // handover). Narrow rivers the coarse point-sampled bake cannot
                                  // resolve may lose triangles out there (speed over accuracy);
                                  // 0 = never cull from far data

    // Ray tracing budget (ocean.fs.glsl traces the scene TLAS per pixel for refraction + reflection).
    float rtRefractionRange = 35.0f;  // max refracted-ray length (m): how far underwater geometry stays
                                       // visible through the surface (the ~99% Beer-Lambert extinction
                                       // bound still applies on top, so clear water is the case this caps)
    float rtReflectionRange = 3000.0f; // max mirror-ray length (m): how distant scenery still reflects
    float rtReflectionMaxRough = 0.25f; // filtered roughness above which the mirror ray is skipped and the
                                        // blurred sky stands in (a wide lobe can't be one mirror sample)
    float rtReflectionFog = 0.2f; // fog on mirror rays (ocean + terrain film): 1 = the reflected source's own fog, 0 = off
    float rtRayCutoffDist = 0.0f; // camera distance (m) beyond which NO scene rays are traced: refraction
                                   // falls back to the analytic baked-terrain bottom (the same path RT
                                   // misses take), reflections to the atmosphere. 0 = unlimited
};

// One live GPU allocation, as Renderer::forEachGpuAllocation reports it (the Memory panel's VRAM view).
export struct GpuAllocationInfo
{
    const char* name = nullptr; // the debug name (or nullptr), VMA's own copy: valid only inside the visit
    uint64 bytes = 0;           // the allocation (alignment included)
    uint64 blockBytes = 0;      // the VkDeviceMemory it lives in (== bytes when dedicated)
    uint64 usage = 0;           // VkImageUsageFlags, or the buffer's VkBufferUsageFlags2
    uint64 bufferSize = 0;      // buffer: the requested size
    uint32 memoryFlags = 0;     // VkMemoryPropertyFlags of its memory type
    uint32 format = 0;          // image: VkFormat
    uint32 width = 0, height = 0, depth = 0; // image extent
    uint16 mips = 0, layers = 0;
    uint8 samples = 0;
    bool image = false;
    bool deviceLocal = false;   // in a DEVICE_LOCAL heap (VRAM, ReBAR included)
    bool dedicated = false;     // its own VkDeviceMemory, not a VMA block
    bool mapped = false;
};

// Renderer::gpuEnumName: which Vulkan enum / flag set a GpuAllocationInfo value is.
export enum class GpuEnum : uint8 { Format, ImageUsage, BufferUsage, MemoryProperties };

export struct Stats
{
    uint32 numLights;
    uint32 maxLights;

    uint32 numMeshInstances;
    uint32 maxMeshInstances;

    uint32 numInstanceOffsets;
    uint32 maxInstanceOffsets;

    uint32 numMeshTypes;
    uint32 maxMeshTypes;

    uint32 numMaterials;
    uint32 maxMaterials;

    uint32 numRenderNodes;
    uint32 maxRenderNodes;

    uint32 numTextures;
    uint32 maxTextures;

    uint64 vertexDataUsedBytes;
    uint64 maxVertexDataBytes;

    uint64 indexDataUsedBytes;
    uint64 maxIndexDataBytes;

    uint32 numObjectContainers;

    uint32 numLightGrids;
    uint32 maxLightGrids;

    uint64 lightGridMemUsageBytes;
    uint64 maxLightGridMemUsageBytes;

    // Total GPU memory tracked by VMA across all heaps.
    uint64 gpuMemoryUsedBytes;     // bytes our live allocations occupy
    uint64 gpuMemoryReservedBytes; // bytes VMA has reserved in blocks (>= used)
    uint64 gpuMemoryBudgetBytes;   // device-local budget available to the process

    // Texture mip streaming (see TextureStreamer).
    uint64 textureBudgetBytes;
    uint64 textureResidentBytes;   // live allocations of streamable textures
    uint64 texturePinnedBytes;     // unstreamable textures, always fully resident
    uint64 textureDesiredBytes;    // what the priority pass wants resident
    uint64 textureTailBytes;       // always-resident mip tails (part of resident)
    uint32 numStreamableTextures;
    uint32 numStreamOpsInFlight;

    // Static BLAS memory (see AccelerationStructure; excludes the per-frame skinned BLASes).
    uint64 blasBytes;
    uint64 blasCompactionSavedBytes; // cumulative bytes reclaimed by copy-compaction

    // Mesh data streaming (see MeshStreamer).
    uint64 meshBudgetBytes;
    uint64 meshStreamableBytes; // registered mesh sets, resident or not
    uint64 meshResidentBytes;
    uint64 meshColdBytes;       // resident but unseen long enough to be eviction candidates
    uint32 numMeshSets;
    uint32 numEvictedMeshSets;

    // Mesh LOD (see MeshLodParams; selection runs in the GPU cull, counters read back a few frames late).
    uint32 numMeshLodGroups;
    uint32 lodInstanceCounts[5];   // VISIBLE LOD instances per selected level (MAX_MESH_LODS)
};
