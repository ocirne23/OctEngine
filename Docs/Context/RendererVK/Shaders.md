# Shaders (RendererVK)

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Shaders

GLSL in `Assets/Shaders/`, **compiled at runtime with glslang — shader edits need no rebuild**, and F5
calls `reloadShaders()`.

* `*.inc.glsl` are includes.
* **Feature folders:** `Common` (shared/ubo + small utility includes), `Mesh` (instanced_indirect, shadow
  depth, skinning, debug lines, draw compaction), `Lighting` (light grid, shadows, RT shadow, RTAO + AO filters), `GI`,
  `PostProcess` (composite, bloom, TAA, DLSS mvecs, eye adaptation, motion blur), `Sky` (sky, atmosphere, clouds,
  volumetric fog, aerial LUT, rain occlusion), `Ocean`, `River`, `Terrain` (terrain, film, wetness, grass, rocks),
  `Clutter` (the ground clutter), `Trees`, `Particles` (+ decals), `Force`. Pipelines name the full path
  (`"Shaders/Sky/cloud_march.cs.glsl"`).
  **An `#include` is a bare file name**: the includer tries the includer's folder, the root shader's
  folder, then a file-name index of the whole `Shaders/` tree — so **file names must stay unique across
  folders.** `Local/Shaders/` mirrors the folders.
* **Compile in ONE `parse()` with the includer** (`Shader::GLSLtoSPV`), never `preprocess()` + a re-parse of its
  output. The SPIR-V carries full NonSemantic debug info (the `-gVS` equivalent), and one pass embeds the original
  text of the file and of every include at their true lines. A re-parse embeds the flattened text, and its `#line N 0`
  include epilogues keep the last include's file name, so every function after an `#include` points into that include
  (Nsight: "incorrect function definition locations"). `Local/Shaders/*.spv` is the dump of each compile (`spirv-dis`
  it to check the debug info). The debug info may contain `OpExtInstWithForwardRefsKHR` (a debug type that refers to
  one declared later, e.g. a struct passed to a function), so `Device` enables `VK_KHR_shader_relaxed_extended_instruction`
  (`shaderRelaxedExtendedInstruction`) when the driver offers it.
* **The SPIR-V cache** (`Shader::GLSLtoSPV`): every compile result is stored as
  `Local/ShaderCache/<path>.<key hash>.spvc` and served on the next compile of the same variant, so startup and F5
  only parse the shaders that changed. The KEY holds everything that is not a file: `SPV_CACHE_VERSION`, glslang's
  version, the stage, the file path, the whole preamble (the layout constants, the live settings baked into it, the
  variant's defines, `SHADER_STATS` in Debug) and the push block. It is stored in full and compared; its hash only
  names the file. The FILES are checked by content hash: the root source, and every include the compile resolved,
  re-resolved through the same includer (so a new file that shadows an include also misses). A changed file
  overwrites its entry; only a new define set adds a file. **Bump `SPV_CACHE_VERSION` when the `SpvOptions`, the
  target env or the entry format change.** A cache hit writes no `Local/Shaders/*.spv` dump. Delete
  `Local/ShaderCache/` to force a full recompile.
* **Per-pixel work hoisted to the UBO / per pixel:**
  * `u_sunTransmittance` is the CPU mirror of `atmosTransmittanceToLight(0, sun, up)` (`buildUboSky`; **keep the
    constants in sync with atmosphere.inc.glsl**) — no shader runs the Chapman function per pixel or per ray for it.
  * `u_sunDirection` and `u_skyUp` are normalized on the CPU, so shaders use them raw.
  * PCSS takes `PCSS_BLOCKER_GATHERS` (6) blocker GATHERS (`textureGather`: 2x2 texels each, 24 texels) +
    `PCSS_FILTER_SAMPLES` (12) filter taps. The gathers stay spread over the Vogel disk; the four texels of one gather
    are neighbours. **Umbra early-out** (`pcssCascade`, not the border path): all 24 search texels occluded AND one
    hardware-PCF tap at the centre = 0 → return 0, the filter skipped. The centre tap guards a small hole over the
    pixel that the sparse search misses (a close grate / canopy gap the filter's small disk would see).
  * The PCSS Vogel disk rotates its compile-time tap angles by ONE per-pixel `(cos, sin)` (`rotSC`) instead of a
    sincos per tap. **Keep the tap loops fully unrolled:** 4-tap batches with a run-time-indexed offset table cost ~10
    registers more.
  * `u_cascadeSunSizeTexels` holds the per-cascade PCSS penumbra scale (`buildUboSunShadow`, from the matrices'
    bottom-row scalars); divide-by-PI is `* INV_PI`; the AO bilateral weights use `exp2` of the squared distance.
* **The GI probe buffer is `vec4[]`** (every includer declares it so; layout table at the top of gi_probe.inc.glsl): a
  probe is 6 wide loads — SH in 3, depth moments in 2, backface + relocation offset packed in 1 — not 24 scalar ones;
  the write side packs the same way. The Chebyshev weight exponent is the fixed `GI_VIS_CHEB_POWER` define (2, no
  tweak), unrolled to multiplies.
* **`shared.inc.glsl` structs must stay in sync with `Private/Layout.ixx`** (the UBO's declaration is generated
  from it, see "The frame UBO and the tweak locks" ([FrameUbo.md](FrameUbo.md))). So must the main cull's
  `OutMeshInstance` (64 B): it is declared in the cull (`instanced_indirect.cs.glsl`) and in every scene vertex shader
  that reads it (lit, terrain, ocean, rock, river) - a shader that declares fewer fields reads with the wrong stride.
* **`mathutils.inc.glsl`** (Common/, included by shared.inc.glsl; a shader without shared.inc.glsl includes it
  itself): `quat_transform`, `quat_multiply`, the `hashU` and `pcg3d` integer hashes. Never redefine one locally.
* `motion_vector.inc.glsl`: the fragment side of the motion vectors (`fragPrevClip`, `motionVector`); fragment shaders
  only (`gl_FragCoord`). The readers' side is in shared.inc.glsl (`prevScreenUVMotion`).
* **THE SUN SHADOW FIRST** (`SUN_SHADOW_FIRST`: the lit-core fragment shaders - lit, foliage, tree, the terrain GROUND,
  rock, grass, clutter - not the film, terrain_film.fs.glsl, which has its own hard tap). `sunShadowVisibility` (the
  shadow + the terrain march + the eclipse + the clouds) is split out of `doSunLight`; these shaders call it at the top
  of main (LitMasked: right after its discard), before the material, from the GEOMETRIC normal (the lit mesh normal;
  the terrain's smooth mesh normal `coverN`, also when tessellated: the shadow is evaluated at the flat mesh, which the
  shadow map and the TLAS hold, and the displaced faces turned away from the sun are gated by `reliefSunGate`), and
  `doSunLight` reads `g_sunShadowFirst`. So a normal map never bends the shadow's offset; `doSunLight`'s facing test on
  the shading normal still applies. The shadow search is the lit FS's register peak (`pcssCascade`, with the whole
  surface live across it): taking it first freed registers in the lit FS; on the terrain it is a quality choice. The
  terrain ground takes its shadow BEFORE `terrainLayers` (so the layers and the tessellated pixel normal are not live
  across it), and computes `V` just before `computeLitColor`.
* **Terrain register savers:** `ClimatePick` (terrain_splat.inc.glsl) packs its three indices into one uint (8 bits
  each), and `pickClimate` keeps its running weights in HALF. **`terrainFbm` is `[[dont_unroll]]`** (its includers
  enable GL_EXT_control_flow_attributes): unrolled, the 12 corner hashes overlapped and cost the TES ~20 registers.
* **Only `EPipelineIndex::LitMasked` / `LitTree` / `LitFoliage` discard** among the static-mesh variants that write
  depth (the lit fragment compiled with `ALPHA_MASK`, instanced_indirect_tree.fs.glsl and
  instanced_indirect_foliage.fs.glsl; the clutter's textured flowers have their own pipeline for the same reason). A
  `discard` anywhere in a pipeline's shader costs it early depth WRITES, and with no prepass that is every opaque
  pixel's overdraw. `ObjectContainer` sends a Mask material that resolved to `LitOpaque` (after the `.oc` overrides) to
  `LitMasked`. Keep `discard` out of `LitOpaque`.
* **The RT shadow toggles are BAKED into the lit-core fragments** (lit, masked, transparent, terrain, rock, ...):
  `LIT_RT_SUN_SHADOW` / `LIT_RT_LIGHT_SHADOWS`, always defined 0/1 from `RTParams::effectiveSunShadow()` /
  `effectiveLightShadows()` (master AND toggle — the same expression as the UBO flags). The lit core `#error`s without
  them. Why: register allocation covers every compiled path, so the PCSS search and the RT sun loop in one shader cost
  the occupancy of the larger one. "RT/Enable RT", "RT Sun" and "RT Lights" reload the pipeline; the setter runs
  BEFORE the init / idle test, because an override fires at registration and `initialize()` must build with it. The
  ocean, fog and GI still read the uniforms. `computeLitColor` evaluates the sun FIRST, so no AO/GI value is live
  across the shadow search.
* **The default static-mesh VS packs its interpolants into 4 locations**: `posU` (xyz + uv.x), `normalV` (xyz + uv.y),
  `tangent` (xyz + bitangent sign), flat `meshIdxMaterialIdx` at 3. The fragment shader rebuilds the bitangent. Every
  FS paired with that VS (lit, unlit, the gizmos, sky) uses this layout; the terrain, rock, ocean and river VS have
  their own.
* **The light grid build is split CPU / GPU, and the CPU part is NOT on the main thread**
  (`LightGridComputePipeline`):
  * **Inline in `addLightInfo`, on the adding thread (`addLight`):** the light's per-type bounds (point/spot sphere,
    spot cone sector, area front-half box, tube capsule) → its cull record; every 32^3 grid the box covers is claimed
    in a lock-free CPU hash table (the same `getPositionHash` as the shaders, bit for bit; bump-allocated slots, CAS on
    the table entry, a slot that loses the race for a grid stays DEAD with `cellSize 0`), the grid's distance-LOD cell
    size is picked at the claim, the grid's cell/large count is bumped (`atomic_ref`), and one (slot, light) touch is
    appended to a bounded array (one `fetch_add` per light for its whole block). A light with a reach over
    `GRID_SIZE / 2` (16 m) or spanning more than **"LOD/Light grid/Per-cell budget (cells)"** (1024) cells inside a grid
    is that grid's LARGE light (`MAX_LARGE_LIGHTS_PER_GRID` 14, one entry evaluated by every pixel of the grid). A light
    whose block or grid claim does not fit goes to an overflow list; the touch array grows for the next frame.
  * **The merge job (`build`, `Renderer::kickGridBuilds` from the App loop right after the force update — the frame's
    last light source; `present()` kicks as a fallback):** re-walks the overflow lights serially (reading the mapped,
    write-combined light buffer — a non-goal path), prefix-sums the per-slot counts into per-grid list ranges, data
    offsets and workgroup counts, scatters the touches (claim order, a thread race — it only matters past the per-cell
    cap, a non-goal), lays out the workgroups, and — when the capacity fits — `upload`s. O(grids + touches).
  * **`joinGridBuilds`** (present, before the staging update) is the ONLY main-thread part: the wait, plus the rare
    exact-fit growth (`FrameSubmission::applyLightGridGrowth`: GPU idle, table / data / host buffers recreated,
    re-record, then the upload on the main thread for that frame). The `Demand` counts FAILED claims too, so the growth
    fits an overflowed burst. No frame drops a light except an overflow light that ALSO meets an exhausted grid
    capacity in the re-walk. The force grid follows the same contract (`ForceFieldPipeline::buildGrid`, its own job on
    the same counter, `growGridBuffers`; the claim table + touch array live in `Util/GridClaim.ixx`, shared by both).
  * **`upload`** writes the grid jobs, the workgroup list (one per 64 cells of a grid), the light list, the hash table
    (header `{numGrids, gridDataCounter, tableSize}` + slots) and the indirect dispatch.
  * **THE CLAIM TABLE IS THE GPU TABLE** (light and force alike): same hash, same linear probing, no deletions, SAME
    SIZE — so the light upload is one sequential pass writing `claim[i] == EMPTY ? EMPTY : dataOffset[claim[i]]` (grid
    data sizes vary with the LOD), and the force upload is a plain `memcpy`: its records are fixed-size, one per slot,
    so the table keeps SLOT indices and `forceFindCell` does `slot * FORCE_CELL_UINTS`. No hashing, no probing, no read
    of the mapped memory. That is why the table entry count is tied to the claim capacity (`getTableEntries()` = 4 x
    grid capacity; the renderer's `m_lightTableEntries` follows it on growth; the force pipeline's claim capacity is
    `m_tableEntries / 4`), and why `GridClaim::resize` REHASHES the live slots: a growth lands between a build and its
    upload.
  * **A light added after the kick is missed for that frame.** New light sources go before `kickGridBuilds` in the App
    loop.
  * **GPU (`light_grid.cs.glsl`, GATHER):** one thread per cell loops its grid's candidate list (box test + the range
    sphere for point/spot) and writes the cell's count + packed ids in one go. No atomics, no spin, no clear: every
    header and cell of every grid is written. Counts are written CAPPED (the debug heat view shows the cap as
    saturation). `light_grid.inc.glsl` is read-only API; the table wrappers need `TABLE_SIZE_NAME` +
    `GRID_TABLE_NAME`, the gather shader does without them.
  * **The distance LOD** is `LightGridParams` under "LOD/Light grid", read by the CPU build every frame (no reload):
    `level = floor(pow(max(dist - start, 0) / step, power))`, `cellSize = clamp(minCell << level, minCell, maxCell)` in
    world units per cell. Min cell 0 (log2) = the full GRID_SIZE cells per axis; min == max pins one resolution
    everywhere; the defaults (0 m, 16 m, 0.5, min 0, max 1) stop at 2 m cells.
  * The three grid constants (`GRID_SIZE`, the two caps) and the header layout are mirrored in
    `LightGridComputePipeline.cpp`: **change both.**
* **Debug overlays are `#define`-driven, NEVER uniforms** — a debug switch rebuilds its pipeline (GPU idle + reload, the
  wireframe pattern), so the release shader carries no debug branch at all. Examples: `SHADOW_DEBUG` and
  `LIGHT_GRID_DEBUG` (below, both on the lit fragments), `OCEAN_DEBUG_MODE`, the cloud debug mode, and
  `FORCE_DENSITY_VIEW` ("Force/Debug/Density view", `ForceFieldPipeline::setDensityView` through
  `setForceFieldParams`'s rebuild branch). Keep new ones on the same pattern.
* **"Shadows/Debug mode"** (`ShadowParams::debugMode`) is BAKED as the `SHADOW_DEBUG` define into the lit and terrain
  fragment variants (`StaticMeshGraphicsPipeline::setShadowDebugMode`; the listener in `Renderer` does the GPU-idle +
  reload). `shadowDebugOverlay` in `shadows.inc.glsl`: 1 cascade index tint, 2 the cross-fade band (white), 3 the raw
  sun visibility, 4 shadow-map texel size heat (green 5 cm → red 1 m). PCSS path only. `~GameMatch`'s shadow-preset
  restore leaves this field alone for the same reason.
* **"LOD/Light grid/Debug Mode"** (`LightGridParams::debugMode`) is BAKED as the `LIGHT_GRID_DEBUG` define on the same
  lit fragments (its own listener; at 0 every debug branch folds away) and overlays the forward pass's
  `computeLitColor` (`instanced_indirect_lit.inc.glsl`): 1 light grid cells (random colour per grid), 2 per-cell light
  count heat (green → red at the cell cap, magenta = the point's grid is missing from the hash table), 3 light ranges
  (a step of blue per covering light). Not saved.
* SPIR-V plus source dumps land in `Assets/Local/` for Aftermath crash analysis.

## Pipeline statistics (register counts)

`VK_KHR_pipeline_executable_properties` (optional, enabled when offered). **"Renderer/Log pipeline
stats"** (off): pipelines created while it is on carry `CAPTURE_STATISTICS`, and
`Device::logPipelineStatistics` writes one tab-separated line per stage - `<debug name> <VS|FS|CS>
Register Count=.. Binary Size=.. Local Memory Size=.. ...` - to the log AND to
`Assets/Local/pipeline_stats.txt` (appended since startup; F5 re-creates the pipelines and appends a new
block). Unattended: `App.exe --quit-after 20 --tweak "Renderer/Log pipeline stats=1"`, then read the file.

* **Register Count sets the occupancy**; a non-zero **Local Memory Size** is spilled registers or
  dynamically indexed local arrays (both go through L1TEX). Integers print their low 32 bits: the NVIDIA
  driver reports Local Memory Size as 2^36 + bytes.
* Measure **RelWithDebInfo** for real numbers: Debug adds `SHADER_STATS` to the cull shaders.
* Compute and graphics pipelines (every variant) report under their debug name; ImGui's do not.
* **The `#n` suffix of a variant follows the pipeline creation order, which is not fixed** - compare the lit
  variants as a group.
* NVIDIA returns no internal representations (no SASS) through `VK_KHR_pipeline_executable_properties`, so
  bisecting (stub a block, re-measure) is the only way to locate a peak.

## Half floats (fp16)

Measured on an RTX 4090. The shading of the lit, terrain, film, ocean and river fragment shaders is **fp16 math in
whole blocks**; positions, UVs, depths, ray origins / directions, ray-query and hit-attribute data stay fp32. Measure
with the pipeline stats (registers x 4 + local memory per thread); list the fp16 <-> fp32 conversions with
**`Tools/fp16conv.ps1 <shader>.glsl`** (every OpFConvert by source line - glslang never warns about an implicit
`float16_t -> float` widening).

**Current numbers** (regs / local B per thread, RelWithDebInfo): lit FS 64/32 (all variants), terrain ground FS 64/32,
tessellated ground 64/48, film 64/32, ocean 80/16, river FS 64/32, rock 64/16, sky.fs 56, aerial_lut 73,
particle_sim 56, particle VS 56, vol_scatter 62/0, vol_apply FS 56/32, cloud_march / cloud_sky 64/0, cloud_shadow
38/0, cloud_temporal 48/0, gi_sky_map 68/0, gi_probe_trace 96/0, clutter_cull 80/32, decal 55/0. The driver moves its
register/spill split by +-16 B on neutral changes, so re-measure before trusting a 16 B difference.

* **The rules:**
  * A half that is only STORED (math still fp32) gains nothing: it is not packed, it takes a full 32-bit register, and
    the conversions add temporaries. Only whole blocks of fp16 MATH pay.
  * A value must not be live in BOTH precisions across a peak (a ray query, PCSS, the light loop): convert once,
    before it, and keep only the half copy (e.g. `computeLitColor` takes the surface half).
  * **Fold before the peak.** The ocean top side and the terrain film are LINEAR in their traced radiances:
    `final = body * bodyWeight + mirror * mirrorWeight + C`, so the glint, SSS, the bubble cloud, the blur's sky share
    and the foam fold into one half `C` BEFORE the traces (see both shaders). The bubble cloud is
    `ocean_bubbles.inc.glsl`, shared by both; the ocean's parallax tap for its coverage runs before the sun shadow ray,
    so only one float of it is live across that query. Per-frame constants go to the UBO instead
    (`u_ocean_bubbleSun` / `bubbleSky`: the ocean keeps one per-pixel `exp` in `oceanBubbleRadianceFrame`; the film's
    depth is per pixel, so it keeps the full `oceanBubbleRadiance`).
  * **One light loop per shader** (large + cell lights in one loop): every `doLightShadowed` call inlines all light
    types plus the shadow ray query. Merging a double loop cut 36-40 KB of code each in the ocean and the film, and
    vol_scatter's single loop (`volLightScatter` inlines a ray query) cut its registers.
  * **Write a dual-source OUTPUT at each return, never as an up-front default.** An `out_factor = vec4(0)` at the top
    of main kept the output live through the whole shader once one store became dynamic (+16 B on the ocean).
  * **Keep the accumulators few across a ray query:** fold the result into one accumulator before the loop / trace,
    and rebuild cheap values (a reprojection ray, the camera forward, a cell index) after it, rather than holding them
    (vol_scatter, the river's `traceRiverBody` builds its in-scatter after the trace).
  * The scene colour target is RGBA16F, so radiance may be half where it is bounded; a light's radiance is not, so it
    is clamped to 65504 as it lands in a half accumulator.
  * **Packing halves by hand does NOT work on this driver:** it folds `packFloat2x16` / `unpackFloat2x16` pairs away
    (a BYTE-IDENTICAL binary). Each live half costs a full 32-bit register, so fewer live VALUES is the only lever.
    Rebuilding loop-invariant values per iteration does not help either: the compiler hoists them back out.
  * **The light loops' `min(count, MAX)` in `lighting.inc.glsl` and `particle.vs.glsl` stay**, although the counts are
    written capped: without them gi_probe_trace went 96 -> 125 registers and the particle VS 56 -> 61 (an unbounded
    loop). The lit / ocean / river / film / vol_scatter loops read the counts raw at no cost.
  * **fp32 stays, for a certain reason, where:** positions, UVs, depths and ray data (precision); absolute heights (a
    1 m band at 500 m altitude is 1 half step); a distance squared (overflows half past 256 m); E[x^2] - E[x]^2
    (catastrophic cancellation - the LEAN variance); a value that underflows half's normal range (0.02^4, the water's
    base alpha^2); hash inputs (`fract` of large products); the GI cascade walk (+16 B in half).
* **The direct-light BRDF** (`punctual_lights.inc.glsl`, the `*H` copies beside the fp32 functions): N, V, L, H, the
  dots, GGX D + visibility, Fresnel and the colour factors; each light's factor widens once, at the multiply with its
  fp32 radiance. GGX D is Filament's fp16 form: `1 - NoH^2` as `|N x H|^2` (no cancellation near the peak) and the
  square after the divide (alpha^4 never forms), clamped to 65504. **Alpha must be >= 0.01** (the lit FS, the splat and
  the film clamp it). Back-facing shading normals (N·V < 0, N·V clamped to 0) KEEP their specular on purpose: the
  grazing sheen on the grass comes from it (a gate on N·V <= 0 took that sheen away). Only the ANTI-LIGHT cone loses it
  (`antiLightFade`, `doLightH`: V within ~3-14° of −L, faded over |L + V| 0.25 → 0.05): there only back-facing normals
  are lit, and the almost zero `L + V` normalized in half made H noise, Fresnel → 1 and GGX at its peak - a bright DOT
  around the sun seen through the grass.
* **The lit core:** `computeLitColor(worldPos, V, f16vec3 N, f16vec3 albedo, float16_t alpha, metalness, AO)`, a half
  accumulator, half AO / bent normal; the light loop puts each light's result into half BEFORE its shadow ray
  (`lightShadowVisibility`), so no 32-bit light result is live across the query. `g_sunVisSurface` is one half scalar
  (the film rebuilds the surface sun radiance with `sunSurfaceRadiance()`), not fp32 vec3 globals live across the
  light loop.
* **The terrain splat** (`terrain_splat.inc.glsl`) is half: samples, coverages, blend weights, normals. The climate
  match and the fBm hashes stay fp32.
* **GI:** `GI_PROBE_HALF` (defined by the lit core and the ocean) adds the half interface
  `giIndirectOverPiH(worldPos, f16vec3 n)` and `giEvalSkySHH`. **The cascade walk inside stays fp32:** a half walk
  (half fetch results, half SH basis, half cascade blend - every combination tried) cost the lit FS 16 B/thread against
  the fp32 walk converted once. The reflection fog's blend is half on `giEvalSkySHH`.
* **The ocean spectrum / FFT images are RGBA16F** (`SPECTRUM_FORMAT`): 512^2 x 9 layers streamed ~6x per frame,
  memory-bound, so half storage pays here. The butterflies stay fp32 in shared memory.
* **Where the peaks are (bisected):** there is NO single peak left in the lit core or the ocean - removing the PCSS
  search, the light shadow rays, the underwater sun transmittance, the relief sun gate or the two-cascade border PCSS,
  EACH ONE alone, leaves the demand (regs + local / 4) the same; the ocean likewise for its underside path, light loop
  and scene rays. The terrain splat alone is ~54 registers (63 tessellated). In gi_probe_trace the sun ray, the light
  loop and the bounce each add ~16 over a ~80 base; `[[dont_unroll]]` on the ray loop and shared-memory accumulators
  (RMW or `atomicAdd`) were no better. **The film's peak is its light loop's shadow rays**: shadowless film highlights
  would be the lever, a visual trade (a light behind a wall would glint on a puddle). The terrain TES's cost is the
  relief taps plus `terrainLayers` (the climate walk per tessellated vertex), whose per-layer indices cannot be
  interpolated across a patch. What is left is structural: split a peak into its own pass (the film's overlay
  precedent), or a thread-per-ray trace.
* **Tried and dropped:**
  * The film's lights in the lit core's loop (one loop, one shadow ray per light for both lobes; less code): 80/64. The
    film surface must then be resolved BEFORE that loop and its values stay live across the shadow ray query; with the
    film surface after the loop it spilled worse. Same register count as the film's own loop, so no occupancy gain, and
    the spill cost lands on EVERY terrain pixel while the saving is only on filmed ones.
  * The film's lights before its mirror (N / V then live across the lights' own ray-query peak).
  * The AO upsample's tap arrays as index bits (the lit FS got worse).
  * A compact half LightInfo (the compiler loads light fields at use); a D16 shadow map (geometry-bound, D32 is
    compressed).
  * **Not worth it:** the GI probe buffer (the fp32 temporal HISTORY - fp16 would round small blend steps away),
    BakedWorldMap (heights), vertex attributes.
