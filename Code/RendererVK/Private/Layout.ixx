export module RendererVK:Layout;

import Core;
import Core.glm;
import Core.Frustum;
import Core.Transform;
import Core.Sphere;

import :VK;

export namespace RendererVKLayout
{
    constexpr uint32 NUM_FRAMES_IN_FLIGHT = 2;

    // HDR scene color target (linear radiance until the composite's exposure + tonemap).
    constexpr vk::Format SCENE_COLOR_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    constexpr uint16 FALLBACK_DIFFUSE_TEX_IDX = 0;
	constexpr uint16 FALLBACK_NORMAL_TEX_IDX = 1;

    // Initial capacities only: the Renderer tracks the live capacities and grows the backing buffers
    // at runtime when they are exceeded (GPU idle + buffer recreate + command buffer re-record).
    constexpr uint32 INITIAL_RENDER_NODES = 8;
    constexpr uint32 INITIAL_UNIQUE_MESHES = 4;
    constexpr uint32 INITIAL_UNIQUE_MATERIALS = 8;
    constexpr uint32 INITIAL_INSTANCE_OFFSETS = 64;
    constexpr uint32 INITIAL_INSTANCE_DATA = 4;
    constexpr uint32 INITIAL_TEXTURES = 64; // TextureManager grows this, clamped to the device limit
    constexpr size_t INITIAL_LIGHT_GRID_BUFFER_SIZE = 10 * 1024 * 1024;
	constexpr size_t INITIAL_LIGHT_TABLE_NUM_ENTRIES = 64; // must stay a power of 2 for hashing (doubling preserves this)


    // Debug line overlay (physics collider wireframes etc.): per-frame vertex capacity of the mapped
    // line buffer, allocated lazily on first use (16 bytes/vertex). Overflow drops the excess lines.
    constexpr uint32 MAX_DEBUG_LINE_VERTICES = 2 * 1024 * 1024;

    // GPU particle system (ParticlePipeline / particle_*.glsl). The particle POOL is persistent GPU
    // state (one copy, not per-frame-in-flight): a dead-index stack plus two alive lists ping-ponged by
    // frame parity. CPU work per frame is only the emitter table + a spawn map (mapped per-frame
    // buffers); emit/simulate run in compute with GPU-written indirect args, so spawning never
    // re-records anything. Sizing constants are injected into every shader compile (Shader.cpp).
    constexpr uint32 MAX_PARTICLES = 256 * 1024;             // persistent pool capacity (48 B each)
    constexpr uint32 MAX_PARTICLE_EMITTERS = 256;            // live emitter slots (table re-uploaded per frame)
    constexpr uint32 MAX_PARTICLE_SPAWNS_PER_FRAME = 16 * 1024; // spawn-map capacity (one uint per spawned particle)
    constexpr uint32 PARTICLE_SIM_GROUP_SIZE = 64;

    // ParticleEmitterGpu::flags bits (mirrored in the particle shaders via the injected defines).
    constexpr uint32 PARTICLE_FLAG_LIT     = 1u << 0; // per-particle GI probe + sun lighting in the vertex shader
    constexpr uint32 PARTICLE_FLAG_COLLIDE = 1u << 1; // screen-space depth collision (previous frame's G-buffer)
    constexpr uint32 PARTICLE_FLAG_KILL    = 1u << 2; // emitter destroyed: the sim retires its live particles
    constexpr uint32 PARTICLE_TEX_NONE = 0xFFFFu;     // texIdx sentinel: procedural soft round sprite

    // Per-emitter GPU config, uploaded per frame for every live slot (small). Particles reference their
    // emitter slot each frame, so live param edits retroactively drive already-spawned particles.
    // Keep in sync with particle.inc.glsl.
    struct alignas(16) ParticleEmitterGpu
    {
        glm::vec4 posSpawnRadius{ 0.0f };          // xyz = world position, w = spawn radius (m)
        glm::vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f }; // quat; the spawn cone axis is local +Y
        glm::vec4 velocityInherit{ 0.0f };         // xyz = emitter velocity (m/s), w = inherit factor [0,1]
        glm::vec4 spawnParams{ 0.0f, 1.0f, 1.0f, 0.0f }; // x = cone angle (rad), y = speed min, z = speed max, w = spawn on shell (0 = solid, 1 = surface)
        glm::vec4 lifeParams{ 1.0f, 1.0f, 0.0f, 0.0f };  // x = life min (s), y = life max, z = gravity (m/s^2, along -Y), w = drag (1/s)
        glm::vec4 noiseParams{ 0.0f, 0.25f, 0.0f, 0.3f };// x = turbulence accel (m/s^2), y = turbulence frequency (1/m), z = turbulence scroll (m/s), w = collision bounce [0,1]
        glm::vec4 sizeParams{ 0.1f, 0.1f, 0.0f, 0.0f };  // x = size start (m), y = size end, z = size variance [0,1], w = velocity stretch (s, 0 = round billboard)
        glm::vec4 colorStart{ 1.0f };              // rgb = linear color * intensity, a = start alpha
        glm::vec4 colorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
        glm::vec4 fadeParams{ 0.1f, 0.7f, 0.0f, 0.25f }; // x = fade-in end (life frac), y = fade-out start, z = additivity [0,1], w = soft-particle fade distance (m)
        glm::vec4 spinParams{ 0.0f };              // x = max spin (rad/s, random sign), y = random initial rotation (0/1), z = lit emissive floor [0,1], w unused
        glm::uvec4 texFlags{ PARTICLE_TEX_NONE, 0u, 0u, 0u }; // x = texture idx (PARTICLE_TEX_NONE = procedural), y = PARTICLE_FLAG_* bits, z = flipbook cols | rows << 16 (0 = none), w = flipbook fps (float bits)
    };
    static_assert(sizeof(ParticleEmitterGpu) == 192);

    // Projected box decals (DecalPipeline / decal.vs/fs.glsl), submitted per frame like lights
    // (Renderer::addDecal, lock-free). Drawn in the scene-color pass right after the opaque forward
    // draw: the box's fragments reconstruct the surface from the G-buffer depth, project it into decal
    // space and blend over the lit scene (premultiplied), so decals wrap any static or skinned surface.
    // Local +Z is the projection direction; the texture maps across local XY.
    constexpr uint32 MAX_DECALS = 4096;

    constexpr uint32 DECAL_FLAG_LIT = 1u << 0; // sun N.L + GI probe irradiance modulate the decal color

    struct alignas(16) DecalInfo
    {
        glm::vec3 pos{ 0.0f };                     // box center
        float opacity = 1.0f;                      // overall multiplier (CPU-side lifetime fade)
        glm::vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f }; // quat; local +Z projects onto the surface
        glm::vec3 halfExtents{ 0.5f };             // box half size (z = projection depth)
        float angleFadeCos = -0.1f;                // fade out surfaces whose normal faces away from the
                                                   // projection (cos of the cutoff angle vs -Z; -1 = never)
        glm::vec4 tint{ 1.0f };                    // rgb = color * intensity, a = base alpha
        glm::vec3 emissive{ 0.0f };                // radiance added regardless of lighting
        float normalFadeWidth = 0.2f;              // angle-fade smoothstep band width (cos units)
        glm::uvec4 params{ PARTICLE_TEX_NONE, 0u, 0u, 0u }; // x = diffuse texture idx (PARTICLE_TEX_NONE = solid tint), y = DECAL_FLAG_* bits, zw unused
    };
    static_assert(sizeof(DecalInfo) == 96);

    // Forcefield bubbles (Force library / ForceFieldPipeline / force_*.glsl). Emitters are analytic
    // influence-field sources with COMPACT SUPPORT (field is exactly 0 beyond the directional reach):
    // same-team fields sum (metaball merging), the bubble surface is the equal-field equilibrium
    // against the strongest opposing team (or the iso threshold, uncontested). The live slot table is
    // compact-uploaded per frame; per-emitter applied forces and point queries are GPU-computed and
    // read back slot-indexed ~2 frames latent. Sizing constants are injected into every shader compile.
    constexpr uint32 MAX_FORCE_EMITTERS = 8192;     // live emitter slots (64 B each)
    constexpr uint32 MAX_FORCE_TEAMS = 8;           // fixed team count (per-point field accumulators)
    constexpr uint32 MAX_FORCE_QUERIES = 1024;      // persistent gameplay point-query slots
    constexpr uint32 MAX_FORCE_BIG_EMITTERS = 64;   // reach above the tweak threshold bypasses the grid
    constexpr uint32 FORCE_SIM_GROUP_SIZE = 64;

    // ForceEmitterGpu::teamFlags.y bits.
    constexpr uint32 FORCE_FLAG_ACTIVE = 1u << 0;   // clear = destroyed/free slot: skipped everywhere
    constexpr uint32 FORCE_FLAG_BIG = 1u << 1;      // reach above the threshold: global list, not the grid

    // Force emitter hash grid (uniform 32 m cells, NOT camera-adaptive — gameplay queries happen
    // anywhere). Fixed per-cell emitter capacity; cells bump-allocate from the data buffer with the
    // light grid's overflow-inflated-counter growth contract (checkForceGridCapacity).
    constexpr uint32 FORCE_CELL_MAX_EMITTERS = 64;  // packed uint16 indices per occupied cell (must stay even)
    constexpr uint32 INITIAL_FORCE_TABLE_ENTRIES = 4096; // power of two (doubling preserves this)
    constexpr size_t INITIAL_FORCE_GRID_DATA_SIZE = 1024 * 1024;

    // Per-emitter GPU config, re-uploaded (compacted) every frame for every live slot.
    // Keep in sync with force_field.inc.glsl.
    struct alignas(16) ForceEmitterGpu
    {
        glm::vec4 posReach{ 0.0f, 0.0f, 0.0f, 1.0f };   // xyz = world position, w = Reach (m): the bubble
                                                         // spans EXACTLY pos .. pos + dir * Reach
        glm::vec4 dirFocus{ 0.0f, 1.0f, 0.0f, 0.5f };   // xyz = normalized direction, w = focus [0,1]
                                                         // (0.5 = sphere, 0 = cone point at emitter, 1 = at target)
        glm::vec4 outputParams{ 1.0f, 1.0f, 0.5f, 1.0f };// x = Output (distribution budget fold pre-applied),
                                                         // y = shell alpha mult, z = distribution [0,1]
                                                         // (axial density bump position), w = width
                                                         // (lateral scale: 1 = round, < 1 = narrower)
        glm::uvec4 teamFlags{ 0u, 0u, 0u, 0u };         // x = team [0, MAX_FORCE_TEAMS), y = FORCE_FLAG_* bits, zw unused
    };
    static_assert(sizeof(ForceEmitterGpu) == 64);

    // GPU layout of the per-frame compacted emitter buffer: count header + big-emitter index list +
    // live emitters (matches the buffer block force_field.inc.glsl declares). Big emitters (max
    // directional reach above the tweak threshold) bypass the grid — every evaluation scans the big
    // list linearly with a sphere reject, so a 300 m dome doesn't flood thousands of cells.
    struct alignas(16) ForceEmittersGpu
    {
        uint32 count;
        uint32 bigCount;
        uint32 _pad0, _pad1;
        uint32 bigIndices[MAX_FORCE_BIG_EMITTERS]; // compact indices of grid-bypassing emitters
        ForceEmitterGpu emitters[MAX_FORCE_EMITTERS];
    };
    constexpr size_t FORCE_EMITTER_HEADER_SIZE = sizeof(ForceEmittersGpu) - sizeof(ForceEmitterGpu) * MAX_FORCE_EMITTERS;

    // One registered point query (mapped per frame) and its GPU-written result (read back).
    struct alignas(16) ForceQueryGpu
    {
        glm::vec4 posActive{ 0.0f }; // xyz = world position, w = 1 active / 0 inactive slot
    };
    // Per-frame mapped query input buffer (count header + slot array; matches force_query.cs.glsl).
    struct alignas(16) ForceQueriesGpu
    {
        uint32 count;
        uint32 _pad0, _pad1, _pad2;
        ForceQueryGpu queries[MAX_FORCE_QUERIES];
    };
    constexpr size_t FORCE_QUERY_HEADER_SIZE = sizeof(ForceQueriesGpu) - sizeof(ForceQueryGpu) * MAX_FORCE_QUERIES;
    struct alignas(16) ForceQueryResult
    {
        uint32 owningTeam;       // strongest team; MAX_FORCE_TEAMS = outside every bubble
        float ownField;          // that team's field at the point
        float bestOpposingField; // strongest other team's field
        uint32 frameStamp;       // m_frameCounter when computed (0 = never: slot not yet evaluated)
    };
    static_assert(sizeof(ForceQueryResult) == 16);

    // Mesh/material indices are stored as uint16 in InMeshInstance, so growth clamps to this.
    constexpr uint32 MESH_MATERIAL_INDEX_LIMIT = USHRT_MAX
        - 1;
    // Max levels in a mesh LOD chain (level 0 = full resolution), authored or meshopt-generated.
    constexpr uint32 MAX_MESH_LODS = 5;
    constexpr uint32 INITIAL_MESH_LOD_GROUPS = 256;  // GPU MeshLodGroup array capacity, grown on demand
    constexpr uint32 INITIAL_LOD_STATE_SLOTS = 1024; // per-instance LOD hysteresis state slots, grown on demand
    constexpr size_t MAX_LIGHTS = USHRT_MAX - 1;
    // Sun shadow cascaded shadow maps.
    constexpr uint32 NUM_SHADOW_CASCADES = 4;
    constexpr uint32 SHADOW_MAP_RESOLUTION = 2048;

    // Per-render-node pass visibility bits (in_nodePassMasks, written at renderNode() push time):
    // the main indirect cull, shadow cull, and TLAS-instance writer each early-out on their bit, so
    // one pushed instance list feeds three independently culled passes. Bit order must match the
    // Spatial library's SpatialPass_* mask (Entity static_asserts it).
    constexpr uint32 PASS_MAIN   = 1u << 0;
    constexpr uint32 PASS_SHADOW = 1u << 1;
    constexpr uint32 PASS_GI     = 1u << 2;
    constexpr uint32 PASS_ALL    = PASS_MAIN | PASS_SHADOW | PASS_GI;

    // Diffuse GI irradiance probes. A single persistent, world-space cascaded clipmap volume: GI_NUM_CASCADES
    // nested toroidal probe grids, each GI_CASCADE_PROBE_DIM^3 probes at a fixed power-of-two spacing
    // (GI_CASCADE_BASE_SPACING << cascade), camera-centered. Probes live at absolute lattice positions
    // (lc * spacing) and are addressed toroidally (slot = lc & (DIM-1)), so irradiance carries forward in
    // place across frames with no hash table, copy, or ping-pong. SH-L1 RGB per probe.
    // The GI_* sizing constants are injected into every shader compile (Shader.cpp buildLayoutPreamble).
    constexpr uint32 GI_SH_STRIDE = 12;                                                  // SH-L1 RGB floats per probe
    constexpr uint32 GI_PROBE_STRIDE = GI_SH_STRIDE + 12;                                 // SH + SH-L1 depth + depth^2 + backface fraction + relocation offset xyz
    constexpr uint32 GI_NUM_CASCADES = 4;                                                // nested clipmap levels
    constexpr uint32 GI_CASCADE_PROBE_DIM = 32;                                          // probes per axis per cascade (power of two)
    constexpr uint32 GI_CASCADE_BASE_SPACING = 2;                                        // finest cascade probe spacing, world units (power of two)
    constexpr uint32 GI_CASCADE_PROBES = GI_CASCADE_PROBE_DIM * GI_CASCADE_PROBE_DIM * GI_CASCADE_PROBE_DIM;
    constexpr uint32 GI_PROBES_TOTAL = GI_NUM_CASCADES * GI_CASCADE_PROBES;

    // + one extra SH-L1 slot after the last probe: the "virtual sky probe" (skyRadiance projected by the
    // trace pass), evaluated as the out-of-field fallback so it matches the probes by construction.
    constexpr size_t GI_GRID_DATA_BUFFER_SIZE = ((size_t)GI_PROBES_TOTAL * GI_PROBE_STRIDE + GI_SH_STRIDE) * sizeof(uint32);
    constexpr uint32 GI_TRACE_THREADS = GI_PROBES_TOTAL + 64;                            // one invocation per probe + one workgroup projecting the sky SH

    constexpr uint32 GI_INITIAL_TLAS_INSTANCES = 256; // grown when the instance count exceeds it
    constexpr size_t GI_TLAS_INSTANCE_SIZE = 64;                                         // sizeof(VkAccelerationStructureInstanceKHR)

    // FFT ocean simulation (OceanSimulationPipeline / ocean_*.cs.glsl). Injected into every shader compile.
    constexpr uint32 OCEAN_FFT_SIZE = 512; // FFT grid resolution per cascade (power of two)
    constexpr uint32 OCEAN_CASCADES = 3;   // spectral band-split cascades (different patch sizes)
    constexpr uint32 MAX_TERRAIN_SPLAT_MATERIALS = 24; // UBO capacity for terrain splat materials (ground + rock + beach + snow)

    struct MeshVertex
    {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec4 tangent;
        glm::vec2 texCoord;
    };
    using MeshIndex = uint32;

    // Per-vertex skinning influences for skeletal meshes. Stored as a parallel stream (not folded into
    // MeshVertex) so the rendered/BLAS vertex format stays 48 bytes. boneIndices reference the scene
    // Skeleton's bone array; weights are normalized to sum 1. The GPU skinning compute reads this plus a
    // bone-matrix palette and writes a deformed MeshVertex (same format) into a per-instance output region.
    struct SkinningVertex
    {
        glm::uvec4 boneIndices;
        glm::vec4 boneWeights;
    };

    // Per-skinned-instance job for the skinning compute, uploaded each frame into a std430 SSBO the
    // shader indexes with gl_WorkGroupID.y (one indirect dispatch covers all jobs — data-driven, no
    // re-record when instances spawn). Offsets are in element units (MeshVertex / SkinningVertex / mat4).
    struct SkinningJob
    {
        uint32 baseVertexOffset; // source geometry, MeshVertex units
        uint32 skinVertexOffset; // influences, SkinningVertex units
        uint32 outVertexOffset;  // destination, MeshVertex units
        uint32 vertexCount;
        uint32 paletteOffset;    // bone palette base, mat4 units
    };
    // Per-skinned-mesh source data captured when an ObjectContainer loads (bind-pose geometry + skinning
    // influences + material/pipeline). Owned by the renderer (like MeshInfo) and referenced by a base index
    // per container; spawnSkinnedNode() turns each into a unique per-instance output region + MeshInfo.
    struct SkinnedMeshSource
    {
        uint32 baseVertexOffset; // bind-pose geometry, MeshVertex units
        uint32 skinVertexOffset; // influences, SkinningVertex units
        uint32 vertexCount;
        uint32 indexCount;
        uint32 firstIndex;
        uint16 materialLocalIdx;
        uint16 pipelineIdx;
        uint16 alphaMode;
        Sphere bounds;
        // meshopt-generated LOD chain (bind pose, index-only): the skinning compute preserves the source
        // vertex ordering in every instance's deformed output region, so these index ranges can draw any
        // instance at any level. lod*[j] = level j+1; errors in mesh-local units like MeshLodGroup.
        uint8  numLodLevels = 0;
        uint32 lodFirstIndex[MAX_MESH_LODS - 1] = {};
        uint32 lodIndexCount[MAX_MESH_LODS - 1] = {};
        float  lodError[MAX_MESH_LODS - 1] = {};
    };

    constexpr uint32 SKINNING_THREADS_PER_GROUP = 64;

    // Initial mega-buffer sizes; MeshDataManager grows them on demand (GPU copy preserves contents).
    constexpr size_t INITIAL_VERTEX_DATA = 1024 * 1024 * sizeof(RendererVKLayout::MeshVertex);
    constexpr size_t INITIAL_INDEX_DATA = 4 * 1024 * 1024 * sizeof(RendererVKLayout::MeshIndex);
    constexpr size_t INITIAL_SKINNING_DATA = 64 * 1024 * sizeof(RendererVKLayout::SkinningVertex);
    constexpr uint32 INITIAL_SKINNING_PALETTE = 1024; // mat4 palette entries across all skinned instances
    constexpr uint32 INITIAL_SKINNING_JOBS = 64;      // skinned mesh instances (SkinningJob SSBO entries)

    // A camera view's matrices grouped contiguously (AoS): one view's data in a single block, so on desktop
    // only views[0] is touched. views[0] = centre/combined view (the sole desktop view; in VR sized to the
    // union of both eyes' FOV so the shared world-space passes cover everything either eye sees);
    // views[1] = left eye, views[2] = right eye (VR only). Shaders select via g_viewIndex.
    struct alignas(16) ViewData
    {
        glm::mat4 mvp;
        glm::mat4 invMvp;     // inverse(mvp): reconstruct world pos from depth + screen uv
        glm::mat4 prevMvp;    // previous frame's mvp: reproject world pos to last frame's screen
        glm::mat4 prevInvMvp; // previous frame's inverse(mvp): reconstruct last frame's world pos
        glm::mat4 reprojClip; // prevMvp * inverse(mvp), fused on the CPU in double precision: reprojects
                              // current NDC + depth straight to last frame's clip space. Near-identity at
                              // any camera position, unlike the world-space round trip through invMvp +
                              // prevMvp whose float32 error grows with distance from the world origin
                              // (pixel-scale by ~500 units = temporal-history jitter).
        glm::vec4 viewPos;    // xyz = world position
    };
    constexpr uint32 VIEW_CENTER = 0;  // shared passes + desktop; the eyes are 1 (left) and 2 (right)
    constexpr uint32 NUM_UBO_VIEWS = 3;
    // Maps a per-eye index (0/1, also used for per-eye history slots) to the u_views[] matrix index a
    // per-eye pass should read: desktop (viewCount 1) collapses to VIEW_CENTER; VR maps eye 0/1 -> 1/2.
    constexpr uint32 eyeToViewIndex(uint32 eye, uint32 viewCount) { return viewCount > 1 ? eye + 1 : VIEW_CENTER; }

    struct alignas(16) Ubo
    {
        ViewData views[NUM_UBO_VIEWS];
        Frustum frustum;         // centre/combined frustum (culling, shadow cascade fit)
        glm::vec3 viewPad_;      // (the centre viewPos moved into views[]) keeps betaMie in a std140 16-byte slot
        float betaMie;           // Mie scattering coefficient at sea level (1/m), drives sky + indirect sky light
        glm::vec3 sunDirection;  // xyz = normalized direction towards the sun, w unused
        float sunAngularCos;     // cos of the sun disc radius (1 = point, smaller = bigger disc)
        glm::vec3 sunColor;      // rgb = color * intensity (sun irradiance; also sources atmosphere scattering)
        float sunGlow;           // glow falloff exponent (0 = no glow); larger = tighter

        glm::vec3 betaRayleigh;  // Rayleigh scattering coefficients at sea level (1/m), drives sky + indirect
        float rolloffKnee;       // sky highlight roll-off: luminance where compression starts (at full roll-off)
        glm::vec3 skyRadianceColor; // directional atmosphere light (moonlight/space light) radiance, along skyUp; GI-only
        float rtSkyRadiance;        // > 0.5: one sky-visibility ray per GI probe gates the sky radiance injection
        glm::vec3 ambientColor;   // flat, non-physical minimum ambient radiance (applied once at final shading)
        uint32 frameIndex;        // monotonic frame counter (RNG / temporal-rotation source so passes that read
                                  // it can be recorded once instead of baking it into a push constant)
        glm::vec3  skyUp;         // sky "up" axis (normalized); need not be world +Y (e.g. planet surface normal)
        float rtSunShadow;        // > 0.5: ray-traced sun shadows instead of PCSS cascades
        // Each cascadeViewProj has a structurally-zero bottom row ([0,0,0,1] for ortho*lookAt), so the
        // per-cascade far distance is stashed in m[0][3] and the world texel size in m[1][3]. Readers
        // restore the bottom row to [0,0,0,1] before using the matrix (see the shaders' cascadeMatrix).
        glm::mat4 cascadeViewProj[NUM_SHADOW_CASCADES];
        glm::vec3 shadowParams; // x = depth bias, y = normal bias (texels), z = 1/resolution
        float sunShadowRays;    // RT sun shadow rays per pixel (1 = single jittered ray)

        float rtLightShadows;   // > 0.5: ray-traced shadows for punctual/area/tube lights
        float timeSeconds;      // elapsed app time (cloud wind / sky animation)
        float cloudCoverage;    // 0 = clear sky, 1 = overcast
        float cloudThickness;   // cloud slab thickness (m)

        glm::vec4 cloudParams0; // x = layer height (m), y = noise scale, z = wind speed (noise units/s), w = wind angle (rad)
        glm::vec4 cloudParams1; // x = edge softness, y = sun shading strength, z = unused, w = unused
        glm::vec4 cloudParams2; // x = density (extinction), y = sharpness, z = base/top height variation, w = moon brightness
        glm::vec4 skySunParams; // x = atmosphere scatter boost (in-scatter only), y = Mie anisotropy g, z = sky highlight roll-off, w = star density

        glm::vec4 screenSize;   // xy = full render-target resolution (px); zw = 1/xy (screen-space AO lookup)
        glm::vec4 viewportRect; // xy = viewport min, zw = viewport size, both normalized to [0,1] of the full
                                // render target. The scene renders through this sub-rect (editor viewport panel),
                                // so screen-space reconstruction must map full-frame UV through it.
        glm::vec4 taaJitter;    // xy = this frame's TAA sub-pixel jitter in NDC (0 when TAA disabled), zw =
                                // LAST frame's. ALL raster passes apply xy in clip space — the G-buffer
                                // prepass included, so the forward pass's depth-prepass reuse tests its
                                // depth DIRECTLY, bound read-only (conservative unjittered prefills could
                                // not cover sub-pixel slits/grazing silhouettes). mvp/invMvp/prevMvp stay
                                // unjittered; TAA/AO-temporal/RTAO compensate sampled depth analytically
                                // (taaJitterUv in shared.inc.glsl), zw compensating the PREV depth image.

        // Volumetric fog (packing documented in vol_fog.inc.glsl)
        glm::vec4 fogParams0; // x = global density (1/m), y = height base, z = height falloff (1/m), w = range (m)
        glm::vec4 fogParams1; // rgb = fog albedo * intensity (> 1 = non-physical gain), w = phase anisotropy g
        glm::vec4 fogParams2; // x = noise scale (1/m), y = noise strength, z = wind speed (m/s), w = temporal blend
        glm::vec4 fogParams3; // x = terrain follow fraction, y = 1 / near terrain map world size (0 = no map), z = enabled, w = light shadow rays
        glm::vec4 fogParams4; // x = sun shadow rays, y = spatial filter, z = GI ambient, w = sun shadow softness (rad)
        glm::vec4 fogParams5; // xy = terrain height map world center XZ (shared by both cascades), z = 1 / far cascade world size (0 = near only), w = baked terrain sea level
        glm::vec4 fogParams6; // x = slice power (Z distribution exponent), y = terrain shadow distance (m),
                              // z = regional fog strength, w = underwater density multiplier
        glm::vec4 fogParams7; // x = underwater sun in-scatter gain (fog light shafts; 1 = physical),
                              // y = waterline band half-height gating the fog's FFT wave taps
                              // (m; 0 = ocean off; sized from the readback trough estimate),
                              // z = underwater caustic strength (0 = off), w = caustic depth fade (1/m)
        glm::vec4 fogParams8; // x = underwater fog boundary offset off the local water surface (m),
                              // y = caustic shore fade depth (m; 0 = off), zw unused
        glm::vec4 fogParams9; // far field (past the froxel volume; vol_apply): x = enabled, y = density
                              // scale, z = multiplier on the near field's height falloff, w = ground samples

        glm::vec4 moonParams; // xyz = normalized direction towards the moon, w = cos of the moon disc radius

        glm::vec4 starParams;   // x = star size, y = size variation, z = brightness, w = color variation
        glm::vec4 nebulaParams; // x = intensity, y = noise scale, z = band width, w = dust lane strength
        glm::vec4 nebulaAxis;   // xyz = normalized milky-way band pole (band lies on its great circle), w unused

        glm::vec4 eclipseParams; // x = visible sun fraction (solar eclipse; sunColor is pre-multiplied by it,
                                 // y = sky highlight roll-off headroom (stops the shoulder absorbs),
                                 // the sky divides it back out for the unoccluded disc/corona), zw unused

        glm::vec4 atmosParams;   // x = Rayleigh scale height (m), y = Mie scale height (m), z = Mie extinction ratio, w = ozone strength
        glm::vec4 groundParams;  // rgb = ground albedo * intensity, w = horizon terrain fraction (fallback ambient)
        glm::vec4 aoParams;      // x = RTAO enabled (0/1), y = GI strength, z = RTAO max distance (m; the
                                 // forward pass skips its AO upsample past it; 0 = no falloff), w unused
        glm::vec4 giVisParams;   // x = Chebyshev variance floor (fraction of spacing), y = Chebyshev power, z = probe weight floor, w = mean scale (footprint widening)

        // Ocean (FFT/Tessendorf water; OceanSimulationPipeline + ocean_*.cs.glsl / ocean.fs.glsl)
        glm::vec4 oceanParams0;    // xy = wind direction (unit), z = spectrum amplitude scale, w = choppiness lambda
        glm::vec4 oceanParams1;    // x = wind speed U10 (m/s), y = fetch (m), z = ocean depth D (m), w = normal strength
        glm::vec4 oceanParams2;    // xyz = cascade FFT patch sizes L0/L1/L2 (m), w = sea level (world Y)
        glm::vec4 oceanAbsorption; // rgb = water extinction sigma_t (1/m, Beer-Lambert), w = perceptual roughness
        glm::vec4 oceanScatter;    // rgb = in-scatter albedo color, w = scatter intensity
        glm::vec4 oceanFoam;       // rgb = foam albedo, w = Jacobian foam bias (higher = more whitecaps)
        glm::vec4 oceanParams3;    // x = horizon band level offset (m, usually negative: sinks ONLY the
                                   //     horizon-band ring, which is exempt from the land cull and so
                                   //     would otherwise draw over distant near-sea-level terrain),
                                   // y = horizon depth (m): the minimum water depth the waves assume
                                   //     past oceanParams4.x (only the seabed moves, not the surface),
                                   // z = turbulence decay/frame,
                                   // w = vertex displacement mip bias (Detail bias; the clipmap rings carry
                                   // their cell size per vertex, so the mip itself is baked into the mesh)
        glm::vec4 oceanParams4;    // x = horizon depth RANGE (m): camera distance past which the waves
                                   //     assume at least oceanParams3.y of water whatever the map says
                                   //     (distant depth readings all err shallow; 0 = take it literally),
                                   // y = turbulence spread (diffusion/frame), z = shoal depth scale (per-
                                   // cascade shoaling: waves fade below depth = scale * patch size),
                                   // w = instant-foam edge width (both thresholds' smoothstep)
        glm::vec4 oceanParams5;    // x = foam boost (turbulence -> fold-threshold relaxation),
                                   // y = turbidity (entrained-bubble milkiness + roughness),
                                   // z = shore foam depth (m; surf band width at the waterline, 0 = off),
                                   // w = breaking-crest foam threshold (downward crest accel in g units)
        glm::vec4 oceanParams6;    // x = far-cascade land-cull error allowance (m; flat burial slack the
                                   //     cull demands when only far terrain data covers the footprint,
                                   //     0 = never cull from far data — moved here from the removed params10),
                                   // y = glint variance filter scale (spec AA + LEAN roughness),
                                   // z = crest SSS strength (0 = off), w = crest SSS forward-lobe power
        glm::vec4 oceanParams7;    // x = land cull margin (m): clipmap triangles whose whole footprint is
                                   // buried deeper than this under the local water level are VS-culled
                                   // (oceanVertexCulled; 0 = off),
                                   // y = shore foam max coverage (surf band opacity cap),
                                   // z = swash amplitude (0 = off), w = swash reach (m, CPU estimate)
        glm::vec4 oceanParams8;    // x = swash drawdown burial (m below the seabed when receding),
                                   // y = shore foam threshold bias (negative = sparser surf),
                                   // z = swash backflow (horizontal chop scale on the tongue),
                                   // w = RT ray cutoff distance (m from the camera; beyond it the water
                                   //     shader traces no scene rays, 0 = unlimited)
        glm::vec4 oceanParams9;    // x = trough margin (m): how far above the seabed the wave trough is
                                   // held, tapered in with depth so the waterline itself does not lift,
                                   // y = RT refraction ray range (m: underwater visibility),
                                   // z = RT reflection ray range (m),
                                   // w = RT reflection roughness cutoff (rougher = sky fallback)
        glm::vec4 oceanParams10;   // x = wave height limit as a fraction of water depth (breaking limit;
                                   //     scales the shoaled cascade sum, 0 = unbounded), yzw unused
        glm::vec4 terrainParams;   // x = streamed terrain mesh coverage radius (m, radial from camera XZ;
                                   // 0 = no terrain mesh up — fences the ocean land cull),
                                   // y = temperature lapse rate, C per WORLD metre above sea level (<= 0;
                                   //     pairs with the map's baked sea-level baseline — terrainTemperatureAt),
                                   // z = sea level (world Y, live from the streamer), w unused
        glm::vec4 terrainShadowParams; // long-range sun shadows marched off the terrain height cascades,
                                   // for the ground the cascades and the TLAS cannot reach (ShadowParams):
                                   // x = camera distance where the march fades in (m; 0 = off),
                                   // y = first sample distance (m) = self-shadow bias vs the map's texels,
                                   // z = penumbra growth per metre along the sun ray, w unused

        // TERRAIN variant texture splatting (Renderer::setTerrainSplatMaterials; keep in sync with
        // ubo.inc.glsl). Materials are CONTIGUOUS in the material buffer, in the order the shader
        // composites them bottom-up: [base .. +numGround) climate-blended ground, [.. +numRock) the
        // bedrock exposed by slope/crag, then the optional single beach entry, then the optional single
        // snow entry.
        glm::vec4 terrainTexParams0; // x = base material idx (< 0 = no texture set: flat-color fallback),
                                     // y = ground entry count, z = rock entry count,
                                     // w = climate kernel sigma (Gaussian falloff OUTSIDE a climate box)
        glm::vec4 terrainTexParams1; // x = ground uv scale (1/m), y = rock uv scale (1/m),
                                     // z = slope where rock fades in, w = slope where rock is full
        glm::vec4 terrainTexParams2; // x = crag relief start (m above macro altitude), y = crag relief full,
                                     // z = beach band height (m above water level), w = snow uv scale (1/m)
        glm::vec4 terrainTexParams3; // x = beach entry present (0/1; index base + numGround + numRock),
                                     // y = snow entry present (0/1; the LAST material, after the beach),
                                     // z = temperature (C) at/below which snow cover is complete,
                                     // w = temperature (C) at/above which there is no snow
        glm::vec4 terrainTexParams4; // x = slope where snow starts sliding off, y = slope where none remains,
                                     // z = humidity at/below which cold ground stays bare (polar desert),
                                     // w = unused
        glm::vec4 terrainTexParams5; // x = crag wander amplitude (m; 0 = off), y = crag wander frequency
                                     // (1/m), zw unused. The wander breaks the rock boundary off the
                                     // elevation contour the crag test would otherwise trace — see terrainSplat.
        glm::vec4 terrainSplatClimate[MAX_TERRAIN_SPLAT_MATERIALS]; // ground/rock CLIMATE BOX in the
                                     // (t01, h01) space: xy = temperature range, zw = humidity range.
                                     // Weight is 1 inside the box and Gaussian-decays outside it, so a
                                     // full-width range on an axis means "this axis does not matter here"
                                     // (snow-line rock is cold at ANY humidity). Unused for beach/snow.

        // GPU mesh LOD selection (indirect + shadow cull; keep in sync with ubo.inc.glsl)
        glm::vec4 lodParams0; // x = screen-space error threshold (px, bias pre-applied), y = hysteresis band,
                              // z = fallback full-res pixels (authored chains), w = mipPixelScale (px per unit/dist)
        glm::vec4 lodParams1; // x = force LOD level (< 0 = off), y = fallback-metric level bias, z = enabled (0/1), w unused

        // Forcefield bubbles (Force library / ForceFieldPipeline; keep in sync with ubo.inc.glsl)
        glm::vec4 forceTeamColors[MAX_FORCE_TEAMS]; // rgb = linear team color, w unused
        glm::vec4 forceParams0; // x = iso threshold, y = rim power, z = rim intensity, w = shell base alpha
        glm::vec4 forceParams1; // x = contact glow intensity, y = contact glow width (opposing/own ratio band),
                                // z = geometry glow distance (m), w = march steps
        glm::vec4 forceParams2; // x = pattern scale (1/m), y = pattern scroll speed, z = pattern intensity,
                                // w = unused (the force-gain readback scale moved CPU-side)
        glm::vec4 forceParams3; // x = interior alpha (shell opacity floor seen from inside),
                                // y = backface alpha (far/inner surface visibility from outside),
                                // z = contact wall alpha (interior equilibrium pane),
                                // w = junction smoothing (smooth-max width as a fraction of iso)
        glm::vec4 forceParams4; // x = density debug view (0/1: heatmap of peak field along the ray),
                                // y = density range (field value mapping to white),
                                // z = ambient field slope (strength/m; <= 0 disables), w = ambient team
        glm::vec4 forceParams5; // ambient field: xy = planar world center, z = safe radius (m),
                                // w = max strength
    };

    struct alignas(16) RenderNodeTransform : Transform {};
    struct alignas(16) MeshInstanceOffset {
        Transform transform;
    };

    struct alignas(16) InMeshInstance
    {
        uint32 renderNodeIdx;
        uint32 instanceOffsetIdx;
        uint16 meshIdx;
        uint16 materialIdx;
        uint16 pipelineIndex; // can use less bits probably
        uint16 alphaMode;     // can use less bits probably
    };
	static_assert(sizeof(InMeshInstance) == 16);

    struct OutMeshInstance
    {
        glm::vec3 translation;
        float scale;
        glm::vec4 quat;
        uint32 meshIdxMaterialIdx;
        uint32 _padding1;
        uint32 _padding2;
        uint32 _padding3;
    };

    // Shadow cull output: like OutMeshInstance but its trailing uint packs the alpha-mask texture index
    // (high 16 bits, 0xFFFF = opaque/no mask) and the cascade overlap bitmask (low 16 bits). Resolving
    // the material in the cull keeps the material buffer out of the depth vertex/fragment shaders.
    // Padded to the std430 array stride (48) so the GPU-side stride matches this allocation size.
    struct alignas(16) OutShadowMeshInstance
    {
        glm::vec3 translation;
        float scale;
        glm::vec4 quat;
        uint32 alphaTexIdxCascadeMask;
        uint32 _pad0;
        uint32 _pad1;
        uint32 _pad2;
    };
    static_assert(sizeof(OutShadowMeshInstance) == 48);

    struct IndirectDrawSequence
    {
        uint32 pipelineIndex;
        uint32 indexCount;
        uint32 instanceCount;
        uint32 firstIndex;
        int32  vertexOffset;
        uint32 firstInstance;
    };
    static_assert(sizeof(IndirectDrawSequence) == 24);

    struct alignas(16) MeshInfo
    {
        glm::vec3 center;
        float radius = 1.0f;
        uint32 indexCount;
        uint32 firstIndex;
        int32  vertexOffset;
        uint32 firstInstance;
    };

    // GPU copy of a MeshLodGroup for the cull shaders' per-instance LOD selection: the chain's global
    // mesh indices packed pairwise (level 0 low half of mesh01) + per-level simplify errors for levels
    // 1..4 (level 0's is always 0). Keep in sync with mesh_lod.inc.glsl.
    struct alignas(16) GpuMeshLodGroup
    {
        uint32 numLods;
        uint32 mesh01; // level 1 << 16 | level 0
        uint32 mesh23; // level 3 << 16 | level 2
        uint32 mesh4;  // level 4 in the low half
        float errors1_4[4];
    };
    static_assert(sizeof(GpuMeshLodGroup) == 32);

    enum class EAlphaMode : uint16
    {
        Opaque = 0,
        Mask   = 1,
        Blend  = 2,
    };

    enum class EPipelineIndex : uint16
    {
        LitOpaque      = 0,
        LitTransparent = 1,
        UnlitOpaque    = 2,
        UnlitTransparent = 3,
        Sky            = 4, // analytic sky + sun disc (sky sphere interior)
        WireframeTransparent = 5, // tangent-debug color, line polygon mode, alpha-blended, no depth write (debug overlay)
        GizmoUI        = 6, // tangent-debug color, vertex shader forces NDC z=0 (nearest) so it draws on top of everything and nothing draws over it (world UI)
        GizmoWorld     = 7, // tangent-debug color, depth tested, alpha-blended, no depth write (world-space gizmo occluded by geometry)
        TerrainLit     = 8, // lit opaque, procedural height/slope albedo (no textures) for procedural terrain chunks
        Ocean          = 9, // GPU-animated Gerstner-spectrum water: vertex displacement + analytic-normal water shading
    };

    // MaterialInfo::flags bits.
    constexpr uint32 MATERIAL_FLAG_NO_RAYTRACING = 1u << 31; // instance mask 0 in the TLAS: invisible to all rays
    constexpr uint32 MATERIAL_FLAG_SKY = 1u << 30; // sky sphere: skipped in the G-buffer prepass so its depth
                                                   // stays at the far plane (TAA reprojects it parallax-free)
    constexpr uint32 MATERIAL_FLAG_BC5_NORMAL = 1u << 29; // normal map is a two-channel BC5 texture (X/Y only):
                                                          // the shader reconstructs Z instead of reading .z
    constexpr uint32 MATERIAL_FLAG_OCEAN = 1u << 28; // ocean water: the G-buffer prepass vertex shader Gerstner-
                                                     // displaces these instances so depth/normal match the forward pass
    constexpr uint32 MATERIAL_FLAG_TERRAIN = 1u << 27; // terrain chunk: colors procedurally (TERRAIN variant), the
                                                       // material's diffuse slot is a fallback — RT hits (ocean
                                                       // refraction) substitute the beach splat instead

    struct alignas(16) MaterialInfo
    {
		uint32 flags;
        float opacity;
        uint16 diffuseTexIdx;
        uint16 normalTexIdx;
        uint16 metalRoughnessTexIdx;
        uint16 alphaMode;
    };

    // Volumetric fog froxel grid (view-frustum-aligned 3D textures).
    constexpr uint32 VOL_FROXEL_X = 160;
    constexpr uint32 VOL_FROXEL_Y = 90;
    constexpr uint32 VOL_FROXEL_Z = 128;
    constexpr uint32 MAX_FOG_VOLUMES = 256;
    constexpr uint32 FOG_TERRAIN_RES = 512;     // fog terrain height map resolution per cascade (CPU-baked around
                                                // the camera; setFogTerrainHeightMap expects CASCADES*RES*RES floats)
    constexpr uint32 FOG_TERRAIN_CASCADES = 2;  // layer 0 = near/fine, layer 1 = far/coarse (same res, larger range)

    // Local participating-media box, submitted per frame like lights (Renderer::addFogVolume). Density adds
    // to the global fog inside the box, fading out over the outer edgeSoftness fraction of each half extent.
    struct alignas(16) FogVolumeInfo
    {
        glm::vec3 pos;
        float density;          // extinction added inside the box (1/m)
        glm::vec3 halfExtents;
        float edgeSoftness;     // 0..1 fraction of each half extent that fades out
        glm::vec3 albedo;       // scattering tint of this volume's media
        float emissive;         // self-lit glow (radiance per meter at full density)
    };
    static_assert(sizeof(FogVolumeInfo) == 48);

    // GPU layout of the per-frame fog volume buffer: count header + array (matches vol_scatter.cs.glsl).
    struct alignas(16) FogVolumes
    {
        uint32 count;
        uint32 _pad0, _pad1, _pad2;
        FogVolumeInfo volumes[MAX_FOG_VOLUMES];
    };
    constexpr size_t FOG_VOLUME_HEADER_SIZE = sizeof(RendererVKLayout::FogVolumes) - sizeof(RendererVKLayout::FogVolumeInfo) * RendererVKLayout::MAX_FOG_VOLUMES;

    // Unified light record for both point and rectangular area lights. width == 0 marks a point
    // light (direction/rotation unused); width > 0 marks an area light whose quad height is encoded
    // in the length of direction and which is rotated by rotation around that direction.
    struct alignas(16) LightInfo
    {
        glm::vec3 pos;
        float radius;
        glm::vec3 color; // above 1.0f for higher intensity light
        float width;
        glm::vec3 direction;
        float rotation;
    };
    static_assert(sizeof(LightInfo) == 48);
}