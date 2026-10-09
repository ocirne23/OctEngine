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
    // Motion blur tiles (MotionBlurPipeline): px per side, and the max blur RADIUS (a blur must stay inside the
    // 3x3 tile neighbourhood). The SUB-tile is the 8 x 8 workgroup that reduces it first - TAA's own workgroup
    // (taa.cs.glsl writes the sub-tiles); a power of two, and TILE a multiple of it.
    constexpr uint32 MOTION_BLUR_TILE = 32;
    constexpr uint32 MOTION_BLUR_SUBTILE = 8;
    static_assert(MOTION_BLUR_TILE % MOTION_BLUR_SUBTILE == 0);
    constexpr uint16 FALLBACK_DIFFUSE_TEX_IDX = 0;
	constexpr uint16 FALLBACK_NORMAL_TEX_IDX = 1;

    // Initial capacities only: the Renderer tracks the live capacities and grows the backing buffers
    // at runtime when they are exceeded (GPU idle + buffer recreate + command buffer re-record).
    constexpr uint32 INITIAL_RENDER_NODES = 64 * 1024;
    constexpr uint32 INITIAL_UNIQUE_MESHES = 2 * 1024; // MESH_MATERIAL_INDEX_LIMIT: the uint16 mesh index's whole range
    constexpr uint32 INITIAL_UNIQUE_MATERIALS = 8;
    constexpr uint32 INITIAL_INSTANCE_OFFSETS = 64;
    constexpr uint32 INITIAL_INSTANCE_DATA = 1024 * 1024; // the world's load does not grow it
    constexpr uint32 INITIAL_TEXTURES = 64; // TextureManager grows this, clamped to the device limit
    constexpr size_t INITIAL_LIGHT_GRID_BUFFER_SIZE = 10 * 1024 * 1024;
	constexpr size_t INITIAL_LIGHT_TABLE_NUM_ENTRIES = 4096; // power of 2 (doubling preserves this); 4x the CPU build's initial 1024-grid claim capacity, so a normal scene never grows


    // Debug line overlay (physics collider wireframes etc.): per-frame vertex capacity of the mapped
    // line buffer, allocated lazily on first use (16 bytes/vertex). Overflow drops the excess lines.
    constexpr uint32 MAX_DEBUG_LINE_VERTICES = 2 * 1024 * 1024;

    // GPU particle system (ParticlePipeline / particle_*.glsl). The particle POOL is persistent GPU
    // state (one copy, not per-frame-in-flight): a dead-index stack plus two alive lists ping-ponged by
    // frame parity. CPU work per frame is only the emitter table + a spawn map (mapped per-frame
    // buffers); emit/simulate run in compute with GPU-written indirect args, so spawning never
    // re-records anything. Sizing constants are injected into every shader compile (Shader.cpp).
    // Pool capacity. GPU cost is MAX_PARTICLES * (48 B pool + 4 B dead stack + 2 * 4 B alive lists) =
    // 30 MB at 512 K. The pool is SHARED by every emitter, and weather volumes hold their fill for the
    // whole session (rain 60 k, snow 40 k, dust 6.3 k, underwater 8.9 k, each x4 from its count tweak),
    // so the headroom above the volumes is what the ordinary effects and the GPU producers get.
    constexpr uint32 MAX_PARTICLES = 512 * 1024;             // persistent pool capacity (48 B each)
    constexpr uint32 MAX_PARTICLE_EMITTERS = 256;            // live emitter slots (table re-uploaded per frame)
    constexpr uint32 MAX_PARTICLE_SPAWNS_PER_FRAME = 16 * 1024; // spawn-map capacity (one uint per spawned particle)
    constexpr uint32 PARTICLE_SIM_GROUP_SIZE = 64;
    // GPU spawn path: any compute pass that runs BEFORE the particle sim in the frame appends
    // ParticleSpawnRequestGpu entries (particle_spawn.inc.glsl: particleRequestSpawn) to one shared
    // request buffer; the begin pass clamps + latches the count and the GPU emit dispatch (indirect,
    // GPU-sized) turns them into particles of the named emitter slot. Requests past the cap are dropped.
    constexpr uint32 MAX_PARTICLE_GPU_SPAWNS = 64 * 1024;
    // Ocean spray producer (ocean_spray.cs.glsl, an OceanSimulationPipeline step): a world-space grid of
    // this many cells per axis around the scene focus, each rolling for spray on breaking crests.
    constexpr uint32 OCEAN_SPRAY_GRID = 128;

    // One GPU spawn request (particle_spawn.inc.glsl mirror). w of velEmitter = the emitter slot as uint
    // bits; w of posSize is reserved (size scale; the emit pass ignores it today).
    struct alignas(16) ParticleSpawnRequestGpu
    {
        glm::vec4 posSize;    // xyz = world position, w reserved
        glm::vec4 velEmitter; // xyz = velocity (m/s), w = emitter slot (uint bits)
    };
    static_assert(sizeof(ParticleSpawnRequestGpu) == 32);

    // ParticleEmitterGpu::flags bits (mirrored in the particle shaders via the injected defines).
    constexpr uint32 PARTICLE_FLAG_LIT     = 1u << 0; // per-particle GI probe + sun lighting in the vertex shader
    constexpr uint32 PARTICLE_FLAG_COLLIDE = 1u << 1; // screen-space depth collision (previous frame's scene depth)
    constexpr uint32 PARTICLE_FLAG_KILL    = 1u << 2; // emitter destroyed: the sim retires its live particles
    constexpr uint32 PARTICLE_FLAG_VOLUME  = 1u << 3; // weather volume: box spawn (volumeParams), particles WRAP at the box faces and never age out
    constexpr uint32 PARTICLE_FLAG_OCCLUDE = 1u << 4; // volume only: a particle under the rain occlusion map's surface restarts at the box top
    constexpr uint32 PARTICLE_FLAG_WATER_FLOOR = 1u << 5; // the live ocean surface is a floor: a particle reaching it lands on it and fades out
    constexpr uint32 PARTICLE_FLAG_UNDERWATER  = 1u << 6; // volume only: lives below the live ocean surface (relocated under it when above; hidden while the camera is above sea level)
    constexpr uint32 PARTICLE_FLAG_ABOVE_WATER = 1u << 7; // volume only: the inverse - lives above the surface (relocated over it when below; hidden while the camera is under sea level)
    constexpr uint32 PARTICLE_FLAG_GROUND_FADE = 1u << 8; // alpha falls off exp(-height above the ground / spinParams.w) (dust hugging the ground)
    constexpr uint32 PARTICLE_TEX_NONE = 0xFFFFu;     // texIdx sentinel: procedural soft round sprite

    // Rain occlusion map (the weather volume's shelter test): ONE top-down orthographic depth view over
    // the volume, ray-traced against this frame's TLAS (RainOcclusionPipeline, R32UI packed) right after the GI
    // step, so the particle sim samples THIS frame's map.
    constexpr uint32 RAIN_OCCLUSION_RESOLUTION = 512;

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
        glm::vec4 spinParams{ 0.0f };              // x = max spin (rad/s, random sign), y = random initial rotation (0/1), z = lit emissive floor [0,1], w = ground fade height (m, PARTICLE_FLAG_GROUND_FADE)
        glm::uvec4 texFlags{ PARTICLE_TEX_NONE, 0u, 0u, 0u }; // x = texture idx (PARTICLE_TEX_NONE = procedural), y = PARTICLE_FLAG_* bits, z = flipbook cols | rows << 16 (0 = none), w = flipbook fps (float bits)
        glm::vec4 volumeParams{ 0.0f };            // PARTICLE_FLAG_VOLUME: xyz = box half extents (m) around posSpawnRadius.xyz, w = wind response (1/s: how fast the horizontal velocity relaxes onto the local wind)
        glm::vec4 cullParams{ 0.0f };              // x = fraction of this emitter's live particles the sim recycles THIS FRAME
                                                   // (a lowered count tweak on a weather volume: it never ages out, so there is
                                                   // nothing else to remove); ONE frame only, the CPU clears it again. yzw unused
    };
    static_assert(sizeof(ParticleEmitterGpu) == 224);

    // Projected box decals (DecalPipeline / decal.vs/fs.glsl), submitted per frame like lights
    // (Renderer::addDecal, lock-free). Drawn in the scene-color pass right after the opaque forward
    // draw: the box's fragments reconstruct the surface from the scene depth, project it into decal
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
    constexpr uint32 MAX_FORCE_TEAMS = 8;           // team CAP: sizes the UBO color array and is the
                                                    // "outside every bubble" sentinel. The LIVE count
                                                    // is ForceFieldParams::numTeams (shader define
                                                    // NUM_FORCE_TEAMS, 2 in co-op) - the force
                                                    // pipelines/bakes rebuild to fit it
    constexpr uint32 MAX_FORCE_QUERIES = 1024;      // persistent gameplay point-query slots
                                                    // (structures; units read the baked field)
    constexpr uint32 FORCE_SIM_GROUP_SIZE = 64;

    // ForceEmitterGpu::teamFlags.y bits.
    constexpr uint32 FORCE_FLAG_ACTIVE = 1u << 0;   // clear = destroyed/free slot: skipped everywhere
    // Evaluated by force_emitter.cs for its OWN slot-indexed readback but contributes NO field: a
    // member of a merge group (Force library) whose field the group's emitter carries. Compacted
    // past fe_count (so every field evaluation and the grid insert never see it) and never drawn.
    constexpr uint32 FORCE_FLAG_PASSIVE = 1u << 1;
    // force_emitter.cs integrates this slot's applied force / pressure (the ANALYTIC readback).
    // Clear = the thread exits at once: the Force library serves the readback from its CPU
    // pressure bake instead, which is the default for every ground consumer.
    constexpr uint32 FORCE_FLAG_READBACK = 1u << 2;

    // Force emitter hash grid (uniform 16 m cells, NOT camera-adaptive - gameplay queries happen
    // anywhere). Fixed per-cell emitter capacity; BUILT ON THE CPU (ForceFieldPipeline::buildGrid,
    // the renderer's grid job) with exact synchronous growth, like the light grid.
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

    // The emitter's VISIBLE size: the bounding half-extent of its drawn (iso-shrunk) box - the
    // actual bubble radius, not the authored Reach. CPU mirror of the shader's forceVisibleRadius
    // (forceVisibleBounds + the teamFlags.w pack, force_field.inc.glsl - keep in sync). THE
    // sampled-tier metric: the upload partition and the bake-volume fit classify with it, matching
    // the shell FS / union ownership tests against u_force_bakeThreshold.
    inline float forceEmitterVisibleRadius(const ForceEmitterGpu& e)
    {
        const float R = e.posReach.w;
        const float m = glm::abs(1.0f - 2.0f * e.dirFocus.w);
        float side = 0.5f * R * (1.0f + m) * e.outputParams.w * 1.03f;
        float forward = R * 1.02f;
        float back = R * 0.02f;
        const uint32 p = e.teamFlags.w;
        if (p != 0u)
        {
            const float lo = float(p & 0xFFu) * (1.0f / 255.0f);
            const float hi = float((p >> 8u) & 0xFFu) * (1.0f / 255.0f);
            side *= float(p >> 16u) * (1.0f / 65535.0f);
            forward = R * glm::min(hi + 0.02f, 1.02f);
            back = R * (0.02f - lo);
        }
        return glm::max(side, (forward + back) * 0.5f);
    }

    // CPU mirror of the shader's forceContribution (force_field.inc.glsl): this emitter's OWN
    // field at x. The renderer evaluates it once per frame at the camera ("camera inside a
    // bubble"); the Force library weights its bake-tap readback with it. Must stay in sync with
    // the shader.
    inline float forceContributionCpu(const glm::vec3& x, const ForceEmitterGpu& e)
    {
        const glm::vec3 d = x - glm::vec3(e.posReach);
        const float R = e.posReach.w;
        const float z = glm::dot(d, glm::vec3(e.dirFocus));
        if (z <= 0.0f || z >= R)
            return 0.0f;
        const float lat2 = glm::max(glm::dot(d, d) - z * z, 0.0f);
        const float X = z * (2.0f / R) - 1.0f;
        const float invW = 1.0f / e.outputParams.w;
        const float Y2 = lat2 * (4.0f / (R * R)) * (invW * invW);
        const float m = 1.0f - 2.0f * e.dirFocus.w;
        const float q = glm::clamp((1.0f - X) / (1.0f + X), 1e-4f, 1e4f);
        const float u2 = X * X + Y2 * (m == 0.0f ? 1.0f : std::pow(q, m));
        if (u2 >= 1.0f)
            return 0.0f;
        const float qq = 1.0f - u2;
        const float b = (z / R - e.outputParams.z) * 2.2222223f;
        return e.outputParams.x * qq * qq * (0.15f + std::exp(-b * b));
    }

    // GPU layout of the per-frame compacted emitter buffer: count header + live emitters (matches
    // the buffer block force_field.inc.glsl declares).
    struct alignas(16) ForceEmittersGpu
    {
        uint32 count;     // field-contributing emitters (grid insert, every field evaluation)
        uint32 evalCount; // count + the PASSIVE tail: what force_emitter.cs evaluates
        uint32 _pad0, _pad1;
        ForceEmitterGpu emitters[MAX_FORCE_EMITTERS];
    };
    constexpr size_t FORCE_EMITTER_HEADER_SIZE = sizeof(ForceEmittersGpu) - sizeof(ForceEmitterGpu) * MAX_FORCE_EMITTERS;

    // BAKED PRESSURE FIELD ("force bake"): a sparse set of XZ CHUNKS the CPU selects each frame
    // from the live emitters' support boxes, evaluated by force_bake.cs.glsl at ONE fixed gameplay
    // height (ALL team field values per sample) and read back host-visible - the CPU-side field
    // any number of consumers samples for force/exposure with NO per-consumer GPU slot. A chunk is
    // a 16 m square: 16x16 samples at 1 m spacing (unit-shield bubbles, reach 3, stay resolved),
    // CORNER-aligned to the world lattice (sample (i,j) of chunk (bx,bz) sits at (bx*16+i, bz*16+j)),
    // so bilinear taps cross chunk borders seamlessly and a missing chunk reads as zero field
    // (= outside every support).
    constexpr uint32 FORCE_BAKE_CHUNK_SAMPLES = 16;   // per axis (workgroup = one chunk, 16x16)
    constexpr float FORCE_BAKE_SAMPLE_SPACING = 1.0f; // m - also spelled in force_bake.cs.glsl
    constexpr uint32 MAX_FORCE_BAKE_CHUNKS = 512;     // 16 m chunks: 512 covers ~131k m^2 of field
    constexpr uint32 FORCE_BAKE_SAMPLES_PER_CHUNK = FORCE_BAKE_CHUNK_SAMPLES * FORCE_BAKE_CHUNK_SAMPLES;
    struct alignas(16) ForceBakeChunksGpu
    {
        uint32 count;
        float sampleY;    // the bake height (world y): where gameplay bodies live
        uint32 _pad0, _pad1;
        glm::ivec4 chunks[MAX_FORCE_BAKE_CHUNKS]; // xy = chunk coord (floor(world / 16 m)), zw unused
    };
    constexpr size_t FORCE_BAKE_HEADER_SIZE = sizeof(ForceBakeChunksGpu) - sizeof(glm::ivec4) * MAX_FORCE_BAKE_CHUNKS;
    // Output/readback: per sample (numTeams + 3) / 4 vec4s (every LIVE team's field value -
    // ONE vec4 with <= 4 teams, halving the readback + the CPU copy), chunk-major:
    // (chunk * 256 + localZ * 16 + localX) * vec4PerSample. The stride is the live team count's,
    // so the buffers are remade on a numTeams change (ForceFieldPipeline::setNumTeams).
    // The paired view of one frame slot's baked field: the data is ~2 frames old, so it comes WITH
    // the chunk list it was evaluated for (chunks.size() chunks x 512 vec4s, chunk-major).
    struct ForceBakeReadback
    {
        oc::span<const glm::ivec4> chunks;
        oc::span<const glm::vec4> data;
    };

    // SAMPLED SHELL TIER: a device-local 3D bake of EVERY team's field (two RGBA16F volumes =
    // phi[0..3]/phi[4..7]), refit each frame over the union of the LARGE drawable emitters'
    // support boxes (u_force_bakeMin / bakeInvSize carry the mapping, so the FIXED texel grid's resolution
    // self-adjusts to the active spread). Shell proxies whose reach exceeds the threshold march
    // these textures (two trilinear taps per sample) instead of the analytic candidate loop -
    // hits, normals and shading stay analytic. Written by force_shellbake.cs each frame.
    constexpr uint32 FORCE_SHELL_VOLUME_X = 128;
    constexpr uint32 FORCE_SHELL_VOLUME_Y = 48;
    constexpr uint32 FORCE_SHELL_VOLUME_Z = 128;
    constexpr uint32 FORCE_SHELL_VOLUME_GROUP = 8; // local_size per axis (dims are multiples)

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

    // Diffuse GI irradiance probes. A single persistent, world-space cascaded clipmap volume: numCascades
    // nested toroidal probe grids, each dimX x dimY x dimZ probes at a fixed power-of-two spacing
    // (GI_CASCADE_BASE_SPACING << cascade), centred on the scene focus (+ a Y offset). Probes live at
    // absolute lattice positions (lc * spacing) and are addressed toroidally (slot = lc & (DIM-1), so
    // every dim is a power of two), so irradiance carries forward in place across frames with no hash
    // table, copy, or ping-pong. SH-L1 RGB per probe.
    // The GI_* sizing values are injected into EVERY shader compile (Shader.cpp buildLayoutPreamble) as
    // #defines: the grid shape is a compile-time constant in the shaders (no per-sample uniform math),
    // and the "GI" grid settings (Globals::settings.gi.grid, GiGridConfig in Settings.Render) change it through the
    // Renderer's listener - GPU idle, the SH buffer re-allocated (resizeGrid), every shader reloaded, the clipmap
    // cleared. The probe buffer holds probesTotal() probes of GI_PROBE_STRIDE + one extra SH-L1 slot after the last
    // probe: the "virtual sky probe" (skyRadiance projected by the trace pass), evaluated as the out-of-field
    // fallback so it matches the probes by construction.
    constexpr uint32 GI_SH_STRIDE = 12;                                                  // SH-L1 RGB floats per probe
    constexpr uint32 GI_PROBE_STRIDE = GI_SH_STRIDE + 16;                                 // SH + SH-L1 depth + depth^2 + backface fraction + relocation offset xyz + sun DC luminance (+3 spare)
    constexpr uint32 GI_CASCADE_BASE_SPACING = 2;                                        // finest cascade probe spacing, world units (power of two)
    // Irradiance volume images per cascade, 20 B per voxel (gi_volume_bake.cs.glsl writes, giVolumeCascade reads):
    //   [0] B10G11R11_UFLOAT  L0 x W: the DC term, PREMULTIPLIED by the summed weight (a weight-correct trilinear
    //                         fetch); a small float, so the HDR range keeps ~1.5% relative steps
    //   [1] R8G8B8A8_SNORM    q1.rgb, q2.r      q = (L1 / L0) / sqrt(3): for a non-negative radiance each L1 / L0
    //   [2] R8G8B8A8_SNORM    q2.gb, q3.rg      ratio lies in [-sqrt(3), sqrt(3)], so it fits SNORM exactly
    //   [3] R16G16B16A16_SFLOAT q3.b, W, s x W  (W in fp16: L0 = fetch[0] / W needs its precision at small W;
    //                         s = the SUN FRACTION of the irradiance, premultiplied like L0 - the lookup dims that
    //                         part by the cloud shadow at the shaded point, see giIrradiance)
    // The volume is NOT accumulated (every bake writes a finished value from the fp32 probe history), so the
    // quantization never compounds. Then ONE more image at a fixed slot: the sky SH (the out-of-field fallback,
    // GI_VOLUME_SKY_TEXELS texels RGBA16F), copied from the probe buffer by the bake. Every consumer's sampler3D
    // array is sized for the cascade tweak's maximum + the sky, so no layout changes with the grid.
    constexpr uint32 GI_MAX_CASCADES = 8;
    constexpr uint32 GI_VOLUME_IMAGES_PER_CASCADE = 4;
    constexpr uint32 GI_VOLUME_SKY_TEXELS = 3; // the sky SH's 3 vec4s (the probe buffer's packing)
    inline constexpr vk::Format GI_VOLUME_FORMATS[GI_VOLUME_IMAGES_PER_CASCADE] = {
        vk::Format::eB10G11R11UfloatPack32, vk::Format::eR8G8B8A8Snorm, vk::Format::eR8G8B8A8Snorm, vk::Format::eR16G16B16A16Sfloat };
    constexpr uint32 GI_VOLUME_SKY_IMAGE = GI_MAX_CASCADES * GI_VOLUME_IMAGES_PER_CASCADE;
    constexpr uint32 GI_VOLUME_MAX_IMAGES = GI_VOLUME_SKY_IMAGE + 1;
    // The cloud feature toggles as BAKED shader defines (buildLayoutPreamble: CLOUDS, CLOUD_SHADOWS,
    // CLOUD_SELF_SHADOW_MAP, CLOUD_POWDER, CLOUD_DEBUG_MODE), copied from CloudParams by Renderer::syncCloudDefines;
    // a change reloads every shader.
    struct CloudShaderConfig
    {
        bool clouds = true;
        bool shadows = true;
        bool selfShadowFromMap = false;
        bool powder = false; // CLOUD_POWDER: the "Powder" strength is above 0
        bool checkerboard = true; // CLOUD_CHECKERBOARD: the march covers half the pixels per frame (also sizes its dispatch)
        int debugMode = 0; // CLOUD_DEBUG_MODE: 0 off, 1 step count, 2 density only, 3 history rejection

        bool operator==(const CloudShaderConfig&) const = default;
    };
    inline CloudShaderConfig g_cloudShaders;

    constexpr uint32 GI_INITIAL_TLAS_INSTANCES = 32 * 1024; // grown when the instance count exceeds it; the world's load does not
    constexpr size_t GI_TLAS_INSTANCE_SIZE = 64;                                         // sizeof(VkAccelerationStructureInstanceKHR)

    // FFT ocean simulation (OceanSimulationPipeline / ocean_*.cs.glsl). Injected into every shader compile.
    constexpr uint32 OCEAN_FFT_SIZE = 512; // FFT grid resolution per cascade (power of two)
    constexpr uint32 OCEAN_CASCADES = 3;   // spectral band-split cascades (different patch sizes)
    constexpr uint32 OCEAN_FOAM_LEVELS = 3; // world-space foam field clipmap levels (texel x 4 per level)
    constexpr uint32 MAX_TERRAIN_SPLAT_MATERIALS = 24; // UBO capacity for terrain splat materials (ground + rock + beach + snow)
    static_assert(MAX_TERRAIN_SPLAT_MATERIALS % 4 == 0, "terrainSplatHeightTex packs four slots per uvec4");

    // 3 x vec4 = 48 B, every member 16 B-aligned: the GLSL mirror (mesh_vertex.inc.glsl) is plain std430 and a
    // shader reads a member with one wide load. The texCoord rides the two .w components. Position stays at
    // offset 0 (the BLAS builds read it as R32G32B32 with this stride).
    struct MeshVertex
    {
        glm::vec4 positionU; // xyz = position, w = texCoord.x
        glm::vec4 normalV;   // xyz = normal,   w = texCoord.y
        glm::vec4 tangent;   // xyz = tangent,  w = bitangent sign

        void set(const glm::vec3& position, const glm::vec3& normal, const glm::vec4& tangentSign, const glm::vec2& texCoord)
        {
            positionU = glm::vec4(position, texCoord.x);
            normalV   = glm::vec4(normal, texCoord.y);
            tangent   = tangentSign;
        }
    };
    static_assert(sizeof(MeshVertex) == 48);
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
    // shader indexes with gl_WorkGroupID.y (one indirect dispatch covers all jobs - data-driven, no
    // re-record when instances spawn). Offsets are in element units (MeshVertex / SkinningVertex / mat4).
    struct SkinningJob
    {
        uint32 baseVertexOffset; // source geometry, MeshVertex units
        uint32 skinVertexOffset; // influences, SkinningVertex units
        uint32 outVertexOffset;  // destination, MeshVertex units
        uint32 vertexCount;
        uint32 paletteOffset;    // bone palette base, mat4 units
        // The output region is 2 x vertexCount: the deformed vertices, then last frame's positions (the motion
        // vectors; MeshInfo::prevVertexDelta). 0 on the job's first frame: the region holds no last frame yet,
        // so the skin writes this frame's positions there too.
        uint32 prevValid;
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

    // Initial mega-buffer sizes; MeshDataManager grows them on demand (GPU copy preserves contents). Sized so the world's
    // load does not grow them: 1.5 GiB of vertices, 0.5 GiB of indices.
    constexpr size_t INITIAL_VERTEX_DATA = 16 * 1024 * 1024 * sizeof(RendererVKLayout::MeshVertex);
    constexpr size_t INITIAL_INDEX_DATA = 64 * 1024 * 1024 * sizeof(RendererVKLayout::MeshIndex);
    constexpr size_t INITIAL_SKINNING_DATA = 64 * 1024 * sizeof(RendererVKLayout::SkinningVertex);
    constexpr uint32 INITIAL_SKINNING_PALETTE = 1024; // mat4 palette entries across all skinned instances
    constexpr uint32 INITIAL_SKINNING_JOBS = 64;      // skinned mesh instances (SkinningJob SSBO entries)

    // ---- THE FRAME UBO ------------------------------------------------------------------------------------------
    // ONE uniform buffer per frame slot, built by Renderer::buildFrameUbo (RendererUbo.cpp), read by every pass at
    // its UBO_BINDING. Its layout is REGISTERED, no C++ struct mirrors it (UboBlock.ixx): every value one line with its
    // sources, live or lockable, grouped by subject (Renderer::registerUboValues).
    // buildUboDeclaration makes the flat GLSL block from the same entries (the shader includer serves it as
    // "ubo.generated.glsl", which ubo.inc.glsl includes).
    constexpr uint32 VIEW_CENTER = 0;  // shared passes + desktop; the eyes are 1 (left) and 2 (right)
    constexpr uint32 NUM_UBO_VIEWS = 3;
    // Maps a per-eye index (0/1, also used for per-eye history slots) to the u_views_*[] index a per-eye pass should
    // read: desktop (viewCount 1) collapses to VIEW_CENTER; VR maps eye 0/1 -> 1/2.
    constexpr uint32 eyeToViewIndex(uint32 eye, uint32 viewCount) { return viewCount > 1 ? eye + 1 : VIEW_CENTER; }

    enum class EUboType : uint8 { Float, Uint, Vec2, Vec3, Vec4, Uvec4, Mat4 };

    // The frame UBO buffer's size: every descriptor binds the whole range; the registered block must fit it.
    constexpr uint32 UBO_RANGE = 16384;

    // The lock SECTIONS whose tweak rows the panel can lock (a TweakLock each, by this name). What bakes is a lockable
    // value (UboBlock) whose sources are all locked, wherever those rows live.
    struct UboLockSection
    {
        const char* name;
        oc::span<const oc::string_view> categories;
    };
    namespace UboLockSections
    {
        constexpr oc::string_view c_sky[] = { "Sky" };
        constexpr oc::string_view c_clouds[] = { "Sky/Clouds" };
        constexpr oc::string_view c_shadows[] = { "Shadows" };
        constexpr oc::string_view c_rt[] = { "RT", "RTAO", "GI" };
        constexpr oc::string_view c_fog[] = { "Fog" };
        constexpr oc::string_view c_ocean[] = { "Ocean" };
        constexpr oc::string_view c_terrainTex[] = { "Terrain/Textures" };
        constexpr oc::string_view c_terrainTess[] = { "Terrain/Tessellation" };
        constexpr oc::string_view c_terrainWater[] = { "Terrain/Water" };
        constexpr oc::string_view c_rivers[] = { "Terrain/Rivers/Surface" };
        constexpr oc::string_view c_grass[] = { "Grass" };
        constexpr oc::string_view c_clutter[] = { "Clutter" };
        constexpr oc::string_view c_foliage[] = { "Trees" };
        constexpr oc::string_view c_rock[] = { "Rocks" };
        constexpr oc::string_view c_lod[] = { "LOD" };
        constexpr oc::string_view c_force[] = { "Force" };
        constexpr oc::string_view c_particles[] = { "Particles" };
        constexpr oc::string_view c_post[] = { "TAA", "Post" };
    }
    inline constexpr UboLockSection c_uboLockSections[] = {
        { "Sky", UboLockSections::c_sky },
        { "Clouds", UboLockSections::c_clouds },
        { "Shadows", UboLockSections::c_shadows },
        { "Ray tracing", UboLockSections::c_rt },
        { "Fog", UboLockSections::c_fog },
        { "Ocean", UboLockSections::c_ocean },
        { "Terrain textures", UboLockSections::c_terrainTex },
        { "Terrain tessellation", UboLockSections::c_terrainTess },
        { "Terrain water", UboLockSections::c_terrainWater },
        { "Rivers", UboLockSections::c_rivers },
        { "Grass", UboLockSections::c_grass },
        { "Clutter", UboLockSections::c_clutter },
        { "Trees", UboLockSections::c_foliage },
        { "Rocks", UboLockSections::c_rock },
        { "LOD", UboLockSections::c_lod },
        { "Force", UboLockSections::c_force },
        { "Particles", UboLockSections::c_particles },
        { "Post", UboLockSections::c_post },
    };
    constexpr uint32 NUM_UBO_LOCK_SECTIONS = (uint32)(sizeof(c_uboLockSections) / sizeof(c_uboLockSections[0]));

    // THE declaration every shader compile includes (Shader.cpp's includer serves it as "ubo.generated.glsl");
    // Renderer::applyUboLocks rebuilds it (buildUboDeclaration, UboBlock.ixx). Main thread, like every shader compile.
    inline oc::string g_uboDeclaration;

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

    // Main cull output, read by the scene vertex shaders. The prev* fields feed the MOTION VECTORS: the
    // instance's transform last frame (prevScale 0 = the node did not move) and, for a skinned mesh, the
    // offset of its previous positions from the drawn vertex (0 = not skinned). See instanced_indirect.cs.glsl.
    struct OutMeshInstance
    {
        glm::vec3 translation;
        float scale;
        glm::vec4 quat;
        glm::vec3 prevTranslation;
        float prevScale;
        uint32 meshIdxMaterialIdx;
        uint32 prevVertexDelta;
        uint32 prevQuat[2]; // packSnorm2x16 (x, y), (z, w)
    };
    static_assert(sizeof(OutMeshInstance) == 64);

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
    // The shadow cull writes into the main cull's out-instance buffer (the shadow pass is done before the main cull).
    static_assert(sizeof(OutShadowMeshInstance) <= sizeof(OutMeshInstance));

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
        // A skinned output mesh: the distance (MeshVertex units) from a deformed vertex to its position LAST
        // frame (the skin writes both; SkinningJob::prevValid). 0 = not skinned.
        uint32 prevVertexDelta;
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
        Sky            = 4, // analytic sky + sun disc: drawn directly ("Sky/Enabled"), never on a material
        WireframeTransparent = 5, // tangent-debug color, line polygon mode, alpha-blended, no depth write (debug overlay)
        GizmoUI        = 6, // tangent-debug color, vertex shader forces NDC z=0 (nearest) so it draws on top of everything and nothing draws over it (world UI)
        GizmoWorld     = 7, // tangent-debug color, depth tested, alpha-blended, no depth write (world-space gizmo occluded by geometry)
        TerrainLit     = 8, // lit opaque, procedural height/slope albedo (no textures) for procedural terrain chunks
        Ocean          = 9, // GPU-animated Gerstner-spectrum water: vertex displacement + analytic-normal water shading
        LitMasked      = 10, // LitOpaque + the alpha-mask discard (ALPHA_MASK); ObjectContainer routes Mask materials here
        TerrainOverlay = 11, // the terrain chunks drawn AGAIN over the ground (surface-water film, later snow ...):
                             // never on a material - the main cull emits it for TerrainLit instances inside the
                             // wetness clipmap, into the mesh's (otherwise unused) transparent sequence
        LitFoliage     = 12, // LitMasked + the FOLIAGE card paths (FOLIAGE): the tree billboards' materials
                             // (MATERIAL_FLAG_BILLBOARD) only - kept out of LitMasked's register allocation
        LitRock        = 13, // the procedural rocks (instanced_indirect_rock.vs/.fs.glsl): lit opaque, no textures
                             // of its own - the CLIMATE's terrain bedrock material, world-space biplanar, plus
                             // the terrain's snow, a ground cover and a contact band. The material is not read.
        River          = 14, // river and lake water (River/river.vs/.fs.glsl; Procedural RiverSystem's meshes):
                             // the ocean's shading without its waves, dual-source composited like the ocean
    };
    // The TRANSPARENT FAMILY: the variants whose fragment shaders write colour location 0 only. The rest write
    // location 1 too (the motion target, masked where the variant does not use it). A DGC execution set needs
    // ONE fragment output interface, and a dual-source blend (ocean, overlay) may not write location 1, so the
    // cull routes a draw by this family - not by its alpha mode - into the opaque or the transparent sequence,
    // and each sequence executes with its own set (StaticMeshGraphicsPipeline). Injected into every shader as
    // PIPELINE_TRANSPARENT_MASK (one bit per EPipelineIndex).
    constexpr uint32 PIPELINE_TRANSPARENT_MASK = (1u << (uint32)EPipelineIndex::LitTransparent)
        | (1u << (uint32)EPipelineIndex::UnlitTransparent)
        | (1u << (uint32)EPipelineIndex::Ocean) | (1u << (uint32)EPipelineIndex::TerrainOverlay)
        | (1u << (uint32)EPipelineIndex::River);
    constexpr bool isTransparentPipeline(uint32 pipelineIdx) { return ((PIPELINE_TRANSPARENT_MASK >> pipelineIdx) & 1u) != 0; }

    // MaterialInfo::flags bits.
    constexpr uint32 MATERIAL_FLAG_NO_RAYTRACING = 1u << 31; // instance mask 0 in the TLAS: invisible to all rays
    constexpr uint32 MATERIAL_FLAG_LEAF = 1u << 30; // LitMasked: thin, light-transmitting leaves (the tree leaf
                                                    // clusters + billboards): the sun shines THROUGH them - a
                                                    // diffuse back term + a forward glow ("Trees/Foliage transmission")
    constexpr uint32 MATERIAL_FLAG_BC5_NORMAL = 1u << 29; // normal map is a two-channel BC5 texture (X/Y only):
                                                          // the shader reconstructs Z instead of reading .z
    constexpr uint32 MATERIAL_FLAG_OCEAN = 1u << 28; // ocean water (the Ocean variant's materials)
    constexpr uint32 MATERIAL_FLAG_TERRAIN = 1u << 27; // terrain chunk: colors procedurally (TERRAIN variant), the
                                                       // material's diffuse slot is a fallback - RT hits (ocean
                                                       // refraction) substitute the beach splat instead
    // DISTANCE FADE (LitMasked only): a dithered fade over a camera-distance band, packed into the low flag bits -
    // start in metres (bits 0..11), width in metres (bits 12..21). Without FADE_IN the surface fades OUT across
    // the band, with it IN; an out and an in material over the same band keep complementary pixels (the tree
    // mesh -> billboard crossfade). makeDistanceFadeFlags packs it.
    constexpr uint32 MATERIAL_FLAG_FADE_IN = 1u << 24;
    constexpr uint32 MATERIAL_FLAG_DISTANCE_FADE = 1u << 25;
    constexpr uint32 MATERIAL_FADE_BAND_MASK = 0x3FFFFFu; // bits 0..21
    constexpr uint32 makeDistanceFadeFlags(float startMetres, float widthMetres, bool fadeIn)
    {
        const uint32 start = (uint32)(startMetres < 0.0f ? 0.0f : startMetres > 4095.0f ? 4095.0f : startMetres);
        const uint32 width = (uint32)(widthMetres < 1.0f ? 1.0f : widthMetres > 1023.0f ? 1023.0f : widthMetres);
        return MATERIAL_FLAG_DISTANCE_FADE | (fadeIn ? MATERIAL_FLAG_FADE_IN : 0u) | start | (width << 12);
    }
    constexpr uint32 MATERIAL_FLAG_BILLBOARD = 1u << 26; // drawn on LitFoliage (its paths assume it): the sun shadow is NOT rejected by the geometric
                                                       // normal's facing (a flat card standing for a foliage clump,
                                                       // the tree billboards) - the normal-mapped normal decides
    constexpr uint32 MATERIAL_FLAG_NO_EDGE_FADE = 1u << 22; // with BILLBOARD: no edge-on fade (the mid tier's branch
                                                       // cards; outside the fade bits TreeSystem rewrites)
    constexpr uint32 MATERIAL_FLAG_BILLBOARD_TOP_CARD = 1u << 23; // with FOLIAGE: an upright whole-tree billboard with a
                                                       // HORIZONTAL card (its top-down view; the card whose normal
                                                       // points up) - the lit FS's crown frame runs along its normal

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
    // The aerial perspective LUT (aerial_lut.cs.glsl): the atmosphere's in-scatter + mean transmittance over the
    // centre view's frustum, Z = distance along the ray, quadratic out to "Fog/Aerial perspective/Max distance".
    constexpr uint32 AERIAL_LUT_X = 64;
    constexpr uint32 AERIAL_LUT_Y = 36;
    constexpr uint32 AERIAL_LUT_Z = 32; // = the shader's workgroup, ONE subgroup (its prefix sum): keep it at the warp size
    constexpr uint32 MAX_FOG_VOLUMES = 256;
    constexpr uint32 FOG_TERRAIN_RES = 512;     // fog terrain height map resolution per cascade (CPU-baked around
                                                // the camera; setFogTerrainHeightMap expects CASCADES*RES*RES floats)
    constexpr uint32 FOG_TERRAIN_CASCADES = 2;  // layer 0 = near/fine, layer 1 = far/coarse (same res, larger range)
    constexpr uint32 TERRAIN_WET_RES = 1024;    // terrain wetness clipmap texels per axis (power of two: toroidal
                                                // slot = lattice & (RES-1)); 512 m of coverage at the 0.5 m texel

    // PROCEDURAL GRASS (GrassPipeline; grass.inc.glsl). Patches of ranked blades: the cull writes one indexed draw per
    // visible patch, and the draw takes the FIRST K blades of the patch mesh (any prefix is evenly spread).
    constexpr uint32 GRASS_MAX_PATCHES = 65536;     // patch slots per frame (the grid around the camera is capped to it)
    constexpr uint32 GRASS_TABLE_DIM = 16;          // the ground-chunk table around the camera, chunks per axis
    constexpr uint32 GRASS_MAX_BLADES = 1024;       // blades per patch, max
    constexpr uint32 GRASS_BLADE_VERTEX_SHIFT = 5;  // a vertex id is blade << 5 | vertex in the blade (2 x 8 + 1 vertices)
    constexpr uint32 GRASS_LODS = 4;
    // grass.inc.glsl grassLodSegments. Each LOD HALVES the one before (they nest): grass.vs.glsl geomorphs the dropped rows.
    constexpr uint32 GRASS_LOD_SEGMENTS[GRASS_LODS] = { 8, 4, 2, 1 };
    constexpr uint32 GRASS_CULL_GROUP = 64;

    // One visible patch: the cull's output, read by the grass VS as INSTANCE-RATE attributes (firstInstance = slot).
    struct GrassPatchGpu
    {
        glm::vec2 origin;      // the patch's min corner, world XZ
        glm::vec2 chunkOrigin; // its terrain chunk's origin, world XZ
        uint32 firstVertex;    // the chunk mesh's first vertex in the vertex mega-buffer
        uint32 resLod;         // the chunk's grid cells per side (bits 0-15) | the blade LOD << 16
        uint32 density;        // the 4 corner densities, unorm8 (x0z0, x1z0, x0z1, x1z1)
        uint32 cellTemperature; // packHalf2x16(the chunk grid's cell (m), the patch's mean temperature (C): the cold tint)
    };
    static_assert(sizeof(GrassPatchGpu) == 32);

    // The per-frame GROUND TABLE (host-visible; grass_cull.cs.glsl): the patch grid and the terrain chunks under it.
    struct GrassFrameGpu
    {
        glm::vec2 gridOrigin;  // min corner of the patch grid, world XZ
        uint32 gridDim;        // patches per axis
        float patchSize;       // m
        glm::ivec2 tableMin;   // the chunk coordinate of table cell (0, 0)
        uint32 tableDim;       // cells per axis (<= GRASS_TABLE_DIM)
        float chunkSize;       // m
        glm::uvec2 chunks[GRASS_TABLE_DIM * GRASS_TABLE_DIM]; // x = the mesh's first vertex, y = grid cells per side (0 = none)
    };

    // GROUND CLUTTER (ClutterPipeline; clutter.inc.glsl, Docs/GroundClutterPlan.md): pebbles, branches, mushrooms and
    // flowers, placed on the GPU every frame. A grid of PATCHES around the camera; per patch and clutter TYPE a ranked
    // list of CLUTTER_CANDIDATES points (an R2 sequence, so every prefix is evenly spread): candidate k of a type is kept
    // where k + 0.5 < the type's density there x the patch area. One cull workgroup per patch, one thread per rank.
    // The kept objects go into BUCKETS - one per (mesh, LOD), the flowers' first - then a prefix pass writes one
    // indexed draw per bucket and a scatter pass sorts the records into the buckets' ranges (instance-rate attributes).
    constexpr uint32 CLUTTER_CANDIDATES = 256;     // ranked points per patch and type = the cull's workgroup (16 / m^2 at 4 m patches)
    constexpr uint32 CLUTTER_MAX_PATCHES = 6400;   // the cull's dispatch (80^2): the patch grid is capped to it
    constexpr uint32 CLUTTER_MAX_TYPES = 32;
    constexpr uint32 CLUTTER_MAX_MESHES = 256;     // variant meshes of every rigid type together
    constexpr uint32 CLUTTER_LODS = 3;             // mesh levels per rigid variant
    constexpr uint32 CLUTTER_FLOWER_LODS = 3;      // flower geometry levels (built in the vertex shader)
    constexpr uint32 CLUTTER_MAX_BUCKETS = CLUTTER_FLOWER_LODS + CLUTTER_MAX_MESHES * CLUTTER_LODS;
    constexpr uint32 CLUTTER_PREFIX_GROUP = 1024;  // the prefix pass: ONE workgroup, a thread per bucket
    static_assert(CLUTTER_MAX_BUCKETS <= CLUTTER_PREFIX_GROUP);
    constexpr uint32 CLUTTER_MAX_INSTANCES = 131072; // kept objects per frame (a bucket-local index fits 17 bits)
    constexpr uint32 CLUTTER_LOCAL_BITS = 17;
    static_assert(CLUTTER_MAX_INSTANCES <= (1u << CLUTTER_LOCAL_BITS) && CLUTTER_MAX_BUCKETS < (1u << (32 - CLUTTER_LOCAL_BITS)));
    constexpr uint32 CLUTTER_SCATTER_GROUP = 256;
    // THE FOREST FLOOR MAP (Procedural ClutterSystem, re-baked as the camera moves): rgba8 per texel - canopy, trunk
    // proximity, rock proximity, occupied (inside a trunk or a rock: nothing grows) - over CLUTTER_FLOOR_DIM^2 texels
    // of CLUTTER_FLOOR_TEXEL metres around its centre.
    constexpr uint32 CLUTTER_FLOOR_DIM = 384;
    constexpr float CLUTTER_FLOOR_TEXEL = 1.0f;
    // Flower geometry (clutter_flower.vs.glsl): per LOD the stem's segments and the petal slots (a quad each).
    constexpr uint32 CLUTTER_FLOWER_STEM_SEGMENTS[CLUTTER_FLOWER_LODS] = { 4, 2, 1 };
    constexpr uint32 CLUTTER_FLOWER_PETALS[CLUTTER_FLOWER_LODS] = { 16, 8, 4 };
    // The `Flow` placement term's measure (the clutter cull - RIVER_FLOW_SLOW / FAST defines - and Procedural's rock
    // rules): a river's speed (m/s, TerrainPoint::riverSpeed) from slow to fast. The Manning speeds run ~0.3 m/s in flat
    // water to ~3 m/s in a steep stream.
    constexpr float RIVER_FLOW_SLOW = 0.5f;
    constexpr float RIVER_FLOW_FAST = 2.5f;

    // The kinds of clutter (ClutterTypeGpu::info.z): how the cull places it and which draw and material it takes.
    enum class EClutterKind : uint32 { Pebble = 0, Branch = 1, Mushroom = 2, Flower = 3 };
    // A flower's head (clutter_flower.vs.glsl).
    enum class EFlowerHead : uint32 { Radial = 0, Spike = 1, Umbel = 2, Bell = 3, Reed = 4, Tuft = 5 };

    // One clutter TYPE (Procedural's .clutter file): where it grows and what it looks like. The density is the product
    // of the type's terms; each two-value term is mix(.x, .y, its measure 0..1). Colours are LINEAR.
    struct ClutterTypeGpu
    {
        glm::vec4 climate;   // the ideal climate box: x..y temperature (t01: (C + 25) / 75), z..w precipitation (01)
        glm::vec4 placement; // x density (per m^2), y 1 / climate width, z 1 / cluster size (0 = no clusters), w cluster coverage
        glm::vec4 terms0;    // xy Grass (the terrain's grass cover: bare .. full), zw Crag (bedrock showing: none .. full)
        glm::vec4 terms1;    // xy Beach (none .. full), zw Canopy (open .. under a full crown)
        glm::vec4 terms2;    // xy Trunk (far .. at a trunk), zw RockNear (far .. at a rock's foot)
        glm::vec4 terms3;    // xy Wet (dry .. wet: the climate's humidity), z max slope (rise / run), w min altitude above water (m)
        glm::vec4 terms4;    // xy River (none .. full: the terrain vertex's u), zw Flow (slow .. fast: its v, the river's speed)
        glm::vec4 terms5;    // x the River term's curve (its measure ^ this: > 1 = close to the water), yzw unused
        glm::vec4 ring;      // x ring radius (m, 0 = none), y ring width (m), z ring cell (m), w the chance a cell holds a ring
        glm::vec4 shape;     // x..y scale range, z range (m), w sink (fraction of the mesh height below the ground)
        glm::vec4 albedo0;   // rgb main colour (pebble: a tint on the climate's bedrock; flower: the petals), w roughness
        glm::vec4 albedo1;   // rgb second colour (end grain / the stem & gills / the flower's centre), w ground align 0..1
        glm::vec4 flower;    // x stem height (m at scale 1), y head size (m), z petal width (x the head size), w petal open angle (rad)
        glm::vec4 bound;     // x max density (per m^2, every term at its largest: the candidates evaluated), y spots (mushroom), zw Water (dry .. under the water)
        glm::uvec4 info;     // x first mesh, y variant meshes, z kind (EClutterKind), w flower head (EFlowerHead) | petals << 8
    };
    static_assert(sizeof(ClutterTypeGpu) == 15 * 16);

    // One rigid variant mesh: its levels in the clutter index / vertex buffers, and its bounds at scale 1.
    struct ClutterMeshGpu
    {
        glm::uvec4 lods[CLUTTER_LODS]; // x first index, y index count (0 = no such level), z vertex offset
        glm::vec4 bounds;              // x radius around (0, height / 2, 0), y height (the lowest point is y = 0)
    };
    static_assert(sizeof(ClutterMeshGpu) == 64);

    // A rigid clutter vertex (48 B, its own buffer).
    struct ClutterVertexGpu
    {
        glm::vec4 posAo;      // xyz position (scale 1, y up, the lowest point at 0), w ambient occlusion 0..1
        glm::vec4 normalPart; // xyz normal, w the PART: Branch 0 bark / 1 end grain; Mushroom 0 stem / 1 cap / 2 gills
        glm::vec4 uv;         // Branch bark: x along the wood, y around it (both m at scale 1); end grain: zw the point
                              // across the cut (m from its axis). Mushroom: x around (0..1), y up its profile (0..1)
    };
    static_assert(sizeof(ClutterVertexGpu) == 48);

    // One kept object (the cull's output, sorted into its bucket's range): instance-rate vertex attributes.
    struct ClutterInstanceGpu
    {
        glm::vec4 posScale;   // the ground contact point (world), the scale (already x the grow factor)
        glm::uvec4 data;      // xy the rotation quaternion (4 halves), z type | variant << 8 | lod << 16 | kind << 24, w its hash
        glm::uvec4 look;      // x albedo0 (rgba8: rgb, roughness), y albedo1 (rgba8: rgb, spots), z packHalf2x16(stem height, head size)
                              // - a rigid object: packHalf2x16(its height (m), 0) -, w head | petals << 8 | petal width (unorm8) << 16 | open angle (unorm8 of pi / 2) << 24
    };
    static_assert(sizeof(ClutterInstanceGpu) == 48);

    // The per-frame CLUTTER FRAME (host-visible; written in present when changed): the patch grid, the type / mesh counts,
    // the flower draws' index ranges and the forest floor map.
    struct ClutterFrameGpu
    {
        glm::vec2 floorOrigin; // world XZ of texel (0, 0)'s min corner
        float floorInvTexel;   // 1 / the texel (m)
        uint32 floorDim;       // texels per axis (0 = no map: the floor measures read 0)
        glm::vec2 gridOrigin;  // the patch grid's min corner
        uint32 gridDim;        // patches per axis (0 = no clutter)
        float patchSize;       // m
        uint32 numTypes;
        uint32 numMeshes;
        float range;           // m: the farthest type's range (the patch test)
        uint32 pad0;
        glm::uvec4 flowerLods[CLUTTER_FLOWER_LODS]; // x first index, y index count (the flower index buffer)
        uint32 floor[CLUTTER_FLOOR_DIM * CLUTTER_FLOOR_DIM];
    };

    // THE INLAND WATER MAP (host-visible per frame slot, written when it changes; Renderer::setRiverWaterMap): around the
    // camera, the river / lake water surface Y per texel (or RIVER_WATER_NONE) - the volumetric fog's underwater boundary
    // for the water the baked terrain map leaves out (it carries the sea only). Procedural RiverSystem bakes it. Its
    // span covers the fog's underwater near field (vol_scatter.cs.glsl fades that out by 300 m).
    constexpr uint32 RIVER_WATER_MAP_DIM = 512;
    constexpr float RIVER_WATER_MAP_TEXEL = 1.25f; // m: 640 m across
    constexpr float RIVER_WATER_NONE = -1.0e9f;
    struct RiverWaterMapGpu
    {
        glm::vec2 origin; // world XZ of texel (0, 0)'s min corner
        float invTexel;   // 1 / the texel (m)
        uint32 dim;       // texels per axis (0 = no map)
        float height[RIVER_WATER_MAP_DIM * RIVER_WATER_MAP_DIM];
    };

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