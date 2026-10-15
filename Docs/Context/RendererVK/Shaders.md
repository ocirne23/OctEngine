# Shaders (RendererVK)

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Shaders

GLSL in `Assets/Shaders/`, **compiled at runtime with glslang — shader edits need no rebuild**, and F5
calls `reloadShaders()`.

* `*.inc.glsl` are includes.
* **Feature folders:** `Common` (shared/ubo + small utility includes), `Mesh` (instanced_indirect, shadow
  depth, skinning, debug lines), `Lighting` (light grid, shadows, RT shadow, RTAO), `GI`, `PostProcess`
  (composite, bloom, TAA, DLSS mvecs, eye adaptation, motion blur), `Sky` (sky, atmosphere, clouds,
  volumetric fog, rain occlusion), `Ocean`, `Terrain` (terrain, film, wetness, grass, rocks), `Clutter` (the ground
  clutter), `Trees`,
  `Particles` (+ decals), `Force`. Pipelines name the full path (`"Shaders/Sky/cloud_march.cs.glsl"`).
  **An `#include` is a bare file name**: the includer tries the includer's folder, the root shader's
  folder, then a file-name index of the whole `Shaders/` tree — so **file names must stay unique across
  folders.** `Local/Shaders/` mirrors the folders.
* **Compile in ONE `parse()` with the includer** (`Shader::GLSLtoSPV`), never `preprocess()` + a
  re-parse of its output. The SPIR-V carries full NonSemantic debug info (the `-gVS` equivalent), and
  one pass embeds the original text of the file and of every include at their true lines. The old
  re-parse embedded the flattened text, and its `#line N 0` include epilogues kept the last include's
  file name, so every function after an `#include` (`main` too) pointed into that include — Nsight
  reports that as "incorrect function definition locations ... glslangValidator 15.0.0".
  `Local/Shaders/*.spv` is the dump of each compile (`spirv-dis` it to check the debug info).
  The debug info may contain `OpExtInstWithForwardRefsKHR` (a debug type that refers to one declared later,
  e.g. a struct passed to a function), so `Device` enables `VK_KHR_shader_relaxed_extended_instruction`
  (`shaderRelaxedExtendedInstruction`) when the driver offers it.
* **Per-pixel work hoisted to the UBO / per pixel:** `u_sunTransmittance` is the CPU mirror of
  `atmosTransmittanceToLight(0, sun, up)` (`buildUboSky`; keep the constants in sync with
  atmosphere.inc.glsl) — the lit sun term never runs the Chapman function per pixel; `u_sunDirection` and `u_skyUp` are
  normalized on the CPU, so shaders use them raw; PCSS takes 6 blocker GATHERS (`textureGather`: 2x2 texels
  each, 24 texels, since 2026-09-28 for the L1TEX latency; 12 single-texel taps before, 16 before that) + 12
  filter taps. The gathers stay spread over the Vogel disk; the four texels of one gather are neighbours.
  **Umbra early-out** (`pcssCascade`, not the border path): all 24 search texels occluded AND one
  hardware-PCF tap at the centre = 0 → return 0, the
  filter skipped. The centre tap guards a small hole over the pixel that the sparse 15-texel search misses
  (a close grate / canopy gap the filter's small disk would see). Static meshes 1.635 → 1.606 ms (sandbox,
  3-4 runs each, no overlap); lit FS 64/48 → 72/16, terrain unchanged. The PCSS Vogel disk rotates its compile-time tap angles
  by ONE per-pixel `(cos, sin)` (`rotSC`) instead of a sincos per tap (keep the tap loops fully
  unrolled: 4-tap batches with a run-time-indexed offset table measured 93 registers against 83); `u_cascadeSunSizeTexels` holds
  the per-cascade PCSS penumbra scale (`buildUboSunShadow`, from the matrices' bottom-row scalars);
  divide-by-PI is `* INV_PI`; the AO bilateral weights use `exp2` of the squared distance.
* **The GI probe buffer is `vec4[]`** (every includer declares it so; layout table at the top of
  gi_probe.inc.glsl): a probe is 6 wide loads — SH in 3, depth moments in 2, backface + relocation
  offset packed in 1 — not 24 scalar ones; the write side packs the same way. The Chebyshev weight
  exponent is the fixed `GI_VIS_CHEB_POWER` define (2, gi_probe.inc.glsl; no tweak), unrolled to
  multiplies.
* **`shared.inc.glsl` structs must stay in sync with `Private/Layout.ixx`** (the UBO's declaration is generated
  from it, see "The frame UBO and the tweak locks" ([FrameUbo.md](FrameUbo.md))). So must the
  main cull's `OutMeshInstance` (64 B): it is declared in the cull and in the three scene vertex shaders
  (lit, terrain, ocean) - a shader that declares fewer fields reads with the wrong stride.
* **`mathutils.inc.glsl`** (Common/, included by shared.inc.glsl; a shader without shared.inc.glsl includes it
  itself): `quat_transform`, `quat_multiply`, the `hashU` and `pcg3d` integer hashes. Never redefine one locally.
* `motion_vector.inc.glsl`: the fragment side of the motion vectors (`fragPrevClip`, `motionVector`);
  fragment shaders only (`gl_FragCoord`). The readers' side is in shared.inc.glsl (`prevScreenUVMotion`).
* **THE SUN SHADOW FIRST** (`SUN_SHADOW_FIRST`: the lit FS, instanced_indirect.fs.glsl, and the terrain GROUND,
  instanced_indirect_terrain.fs.glsl - not the film, terrain_film.fs.glsl, which has its own hard tap). `sunShadowVisibility`
  (the shadow + the terrain march + the eclipse + the clouds) is split out of `doSunLight`; these shaders call it
  at the top of main (LitMasked: right after its discard), before the material, from the GEOMETRIC normal (the
  lit mesh normal; the terrain's smooth mesh normal `coverN`, also when tessellated: the shadow is evaluated at the
  flat mesh, which the shadow map and the TLAS hold, and the displaced faces turned away from the sun are gated
  by `reliefSunGate`), and
  `doSunLight` reads `g_sunShadowFirst`. So a normal map never bends the shadow's offset; `doSunLight`'s facing
  test on the shading normal still applies. The shadow search is the lit FS's register peak (Nsight live
  registers: 70 in `pcssCascade`, with the whole surface live across it). Measured 2026-09-28: lit FS 72/16 ->
  **64/48**; game mode (64 units) Static meshes 0.281/0.284 -> 0.257/0.263 ms, GPU frame 1.553/1.562 ->
  1.508/1.515. Terrain: speed-neutral (#8 72/16 either way, tessellated ground 64/48 -> 72/16) - there it is a
  quality choice. The same day, rebuilding the world position in the lit FS instead of interpolating it was tried
  and reverted (see "Motion vectors" ([PostProcess.md](PostProcess.md))). Later the same day: the terrain ground's shadow moved BEFORE
  `terrainLayers` (so the layers and the tessellated pixel normal are not live across it), and `V` moved down to
  just before `computeLitColor` (Nsight: 69 live at the tessellated ground's `terrainLayers` call). Also
  `ClimatePick` (terrain_splat.inc.glsl) packs its three indices into one uint (8 bits each), and `pickClimate`
  keeps its four running weights in HALF (user-checked: no visible artifacts). Together: both ground variants
  72 -> 64 regs (next item).
* **The terrain ground FS is 64 regs** since 2026-09-28 (pipeline stats): untessellated #8 72/16 -> **64/32**,
  tessellated 72/16 -> **64/48**. From three changes measured together: the shadow before `terrainLayers` (from
  `coverN`), `V` computed just before `computeLitColor`, and `ClimatePick`'s packed indices + half `pickClimate`
  weights (see "SUN SHADOW FIRST" above). The film (#11, one untessellated variant, flow on everywhere) is 56/48.
  The frame time is not measured yet. Before that, 64 LIVE at most yet 72 allocated: the step needed a peak a
  few registers lower, not a different place. Tried earlier the same day, no change to #8: the shadow first (above; kept for its quality), `terrainFbm` not unrolled, and the wet
  section's two noises (drying fBm, glints) hoisted above the splat (reverted). **Kept: `terrainFbm` is
  `[[dont_unroll]]`** (terrain_splat.inc.glsl; its includers enable GL_EXT_control_flow_attributes): unrolled,
  the 12 corner hashes overlapped - the tessellated terrain's TES dropped 70 -> 49 registers with it (the
  then tessellated film FS 64/16 -> 56/48; everything else unchanged. The film is no longer tessellated).
* **Only `EPipelineIndex::LitMasked` / `LitTree` / `LitFoliage` discard** (the lit fragment compiled with `ALPHA_MASK`,
  instanced_indirect_tree.fs.glsl and instanced_indirect_foliage.fs.glsl). A
  `discard` anywhere in a pipeline's shader costs it early depth WRITES, and with no prepass that is
  every opaque pixel's overdraw. `ObjectContainer` sends a Mask material that resolved to `LitOpaque`
  (after the `.oc` overrides) to `LitMasked`. Keep `discard` out of `LitOpaque`.
* **The RT shadow toggles are BAKED into the lit-core fragments** (lit, masked, transparent, terrain):
  `LIT_RT_SUN_SHADOW` / `LIT_RT_LIGHT_SHADOWS`, always defined 0/1 from `RTParams::effectiveSunShadow()`
  / `effectiveLightShadows()` (master AND toggle — the same expression as the UBO flags). The lit
  core `#error`s without them. Why: register allocation covers every compiled path, so the PCSS
  search and the RT sun loop in one shader cost the occupancy of the larger one. "RT/Enable RT",
  "RT Sun" and "RT Lights" reload the pipeline; the setter runs BEFORE the idle test, because an override
  fires at registration and `initialize()` must build with it. The ocean, fog and GI still read
  the uniforms. `computeLitColor` evaluates the sun FIRST, so no AO/GI value is live across the shadow
  search.
* **The default static-mesh VS packs its interpolants into 4 locations** (was 6): `posU` (xyz + uv.x),
  `normalV` (xyz + uv.y), `tangent` (xyz + bitangent sign), flat `meshIdxMaterialIdx` at 3. The
  fragment shader rebuilds the bitangent. Every FS paired with that VS (lit, unlit, the gizmos, sky)
  uses this layout; the terrain and ocean VS have their own.
* **The light grid build is split CPU / GPU, and the CPU part is NOT on the main thread**
  (`LightGridComputePipeline`):
  * **Inline in `addLightInfo`, on the adding thread (`addLight`):** the light's per-type bounds
    (point/spot sphere, spot cone sector, area front-half box, tube capsule) → its cull record;
    every 32^3 grid the box covers is claimed in a lock-free CPU hash table (the same
    `getPositionHash` as the shaders, bit for bit; bump-allocated slots, CAS on the table entry,
    a slot that loses the race for a grid stays DEAD with `cellSize 0`), the grid's distance-LOD
    cell size is picked at the claim, the grid's cell/large count is bumped (`atomic_ref`), and
    one (slot, light) touch is appended to a bounded array (one `fetch_add` per light for its
    whole block). A light with `reach > 16 m` or spanning more than **"Per-cell budget (cells)"**
    (1024) cells inside a grid is that grid's LARGE light (`MAX_LARGE_LIGHTS_PER_GRID` 14, one
    entry evaluated by every pixel of the grid). A light whose block or grid claim does not fit
    goes to an overflow list; the touch array grows for the next frame.
  * **The merge job (`build`, `Renderer::kickGridBuilds` from the App loop right after the
    force update — the frame's last light source; `present()` kicks as a fallback):** re-walks the
    overflow lights serially (reading the mapped, write-combined light buffer — a non-goal path),
    prefix-sums the per-slot counts into per-grid list ranges, data offsets and workgroup counts,
    scatters the touches (claim order, a thread race — it only matters past the per-cell cap, a
    non-goal), lays out the workgroups, and — when the capacity fits — `upload`s.
    O(grids + touches).
  * **`joinGridBuilds`** (present, before the staging update) is the ONLY main-thread part: the
    wait, plus the rare exact-fit growth. The `Demand` counts FAILED claims too, so
    `growLightGridBuffers` (GPU idle, table / data / host buffers recreated, re-record) fits an
    overflowed burst; the upload then runs on the main thread for that frame. No frame drops a
    light except an overflow light that ALSO meets an exhausted grid capacity in the re-walk.
    The force grid follows the same contract (`ForceFieldPipeline::buildGrid`, its own job on the
    same counter; the claim table + touch array live in `Util/GridClaim.ixx`, shared by both).
  * **`upload`** writes the grid jobs, the workgroup list (one per 64 cells of a grid), the light
    list, the hash table (header `{numGrids, gridDataUints, tableSize}` + slots — the readers'
    probe loop is unchanged) and the indirect dispatch.
  * **THE CLAIM TABLE IS THE GPU TABLE** (light and force alike): same hash, same linear probing,
    no deletions, SAME SIZE — so the light upload is one sequential pass writing
    `claim[i] == EMPTY ? EMPTY : dataOffset[claim[i]]` (grid data sizes vary with the LOD), and
    the force upload is a plain `memcpy`: its records are fixed-size, one per slot, so the table
    keeps SLOT indices and `forceFindCell` does `slot * FORCE_CELL_UINTS`. No hashing, no probing,
    no read of the mapped memory. That is why the table entry count is tied to the claim capacity
    (`getTableEntries()` = 4 x grid capacity; the renderer's `m_lightTableEntries` follows it on
    growth; the force pipeline's claim capacity is `m_tableEntries / 4`), and why
    `GridClaim::resize` REHASHES the live slots: a growth lands between a build and its upload.
  * **A light added after the kick is missed for that frame.** New light sources go before
    `kickGridBuilds` in the App loop.
  * **GPU (`light_grid.cs.glsl`, GATHER):** one thread per cell loops its grid's candidate list
    (box test + the range sphere for point/spot) and writes the cell's count + packed ids in one
    go. No atomics, no spin, no clear: every header and cell of every grid is written. Counts are
    written CAPPED (the debug heat view shows the cap as saturation); readers use them raw.
    `light_grid.inc.glsl` is read-only API now; the table wrappers need `TABLE_SIZE_NAME` +
    `GRID_TABLE_NAME`, the gather shader does without them.
  * **The distance LOD** is `LightGridParams` under "Graphics/LOD/Light grid", read by the CPU
    build every frame (no reload): `level = floor(pow(max(dist - start, 0) / step, power))`,
    `cellSize = clamp(minCell << level, minCell, maxCell)` in world units per cell. Min cell 0
    (log2) = the full GRID_SIZE cells per axis; min == max pins one resolution everywhere; the
    defaults (0 m, 16 m, 0.5, 0, 2) follow the old sqrt ramp but stop at 4 m cells.
  * The three grid constants (`GRID_SIZE`, the two caps) and the header layout are mirrored in
    `LightGridComputePipeline.cpp`: **change both.**
* **Debug overlays are `#define`-driven, NEVER uniforms** — a debug switch rebuilds its pipeline
  (GPU idle + reload, the wireframe pattern), so the release shader carries no debug branch at all.
  The three today: `SHADOW_DEBUG` and `LIGHT_GRID_DEBUG` (below, both on the lit fragments) and
  `FORCE_DENSITY_VIEW` ("Force/Debug/Density view", `ForceFieldPipeline::setDensityView` through
  `setForceFieldParams`'s rebuild branch). Keep new ones on the same pattern.
* **"Shadows/Debug mode"** (`ShadowParams::debugMode`) is BAKED as the `SHADOW_DEBUG` define into the
  lit and terrain fragment variants (`StaticMeshGraphicsPipeline::setShadowDebugMode`; the tweak callback
  in `Renderer` does the GPU-idle + reload, the wireframe pattern), so the release shader carries none of
  it. `shadowDebugOverlay` in `shadows.inc.glsl`: 1 cascade index tint, 2 the cross-fade band (white),
  3 the raw sun visibility, 4 shadow-map texel size heat (green 5 cm → red 1 m). PCSS path only.
  `~GameMatch`'s shadow-preset restore leaves this field alone for the same reason.
* **"Graphics/LOD/Light grid/Debug Mode"** (`LightGridParams::debugMode`) is BAKED as the
  `LIGHT_GRID_DEBUG` define on the same lit fragments (its own reload callback; at 0 every debug branch
  folds away) and overlays the forward pass's `computeLitColor` (`instanced_indirect_lit.inc.glsl`):
  1 light grid cells (random colour per grid), 2 per-cell light count heat (green → red at the cell
  cap, magenta = the point's grid is missing from the hash table), 3 light ranges (a step of blue
  per covering light). Not saved. The sun cascade view lives in the baked "Shadows/Debug mode" above.
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

## Half floats (fp16) - measured on the RTX 4090, 2026-09-21/22

The shading of the lit, terrain and ocean fragment shaders is **fp16 math in whole blocks**; positions, UVs,
depths, ray origins / directions, ray-query and hit-attribute data stay fp32. Measure with the pipeline
stats (registers x 4 + local memory per thread); list the fp16 <-> fp32 conversions with
**`Tools/fp16conv.ps1 <shader>.glsl`** (every OpFConvert by source line - glslang never warns about an
implicit `float16_t -> float` widening). Totals (regs/local, B/thread), start -> now:
lit 96/32 (416) -> 64/32 (288), terrain 96/80 (464) -> 80/32 (352), ocean 80/48 (368) -> 80/16 (336).

* **The rules that came out of it:**
  * A half that is only STORED (math still fp32) gains nothing: it is not packed, it takes a full 32-bit
    register, and the conversions add temporaries. Only whole blocks of fp16 MATH pay.
  * A value must not be live in BOTH precisions across a peak (a ray query, PCSS, the light loop): convert
    once, before it, and keep only the half copy (e.g. `computeLitColor` takes the surface half).
  * **Fold before the peak.** The ocean top side and the terrain film are LINEAR in their traced radiances:
    `final = body * bodyWeight + mirror * mirrorWeight + C`, so the glint, SSS, the bubble cloud, the blur's
    sky share and the foam fold into one half `C` BEFORE the traces (see both shaders). The bubble cloud is
    `ocean_bubbles.inc.glsl`, shared by both; the ocean's parallax tap for its coverage runs
    before the sun shadow ray, so only one float of it is live across that query.
  * **One light loop per shader** (large + cell lights in one loop): every `doLightShadowed` call inlines
    all light types plus the shadow ray query. Merging the ocean's and the film's double loops cut 36-40 KB
    of code each.
  * The scene colour target is RGBA16F, so radiance may be half where it is bounded; a light's radiance is
    not, so it is clamped to 65504 as it lands in a half accumulator.
  * The driver moves its register/spill split by +-16 B on neutral changes (e.g. the order of two
    independent blocks) - a 16 B difference needs a bisect before it means anything.
  * **Packing halves by hand does NOT work on this driver:** it folds `packFloat2x16` / `unpackFloat2x16`
    pairs away - packing the lit core's 13 surface halves across the light loop gave a BYTE-IDENTICAL
    binary. Each live half costs a full 32-bit register, so fewer live VALUES is the only lever.
  * **fp32 stays, for a certain reason, where:** positions, UVs, depths and ray data (precision); absolute
    heights (a 1 m band at 500 m altitude is 1 half step); a distance squared (overflows half past 256 m);
    E[x^2] - E[x]^2 (catastrophic cancellation - the LEAN variance); a value that underflows half's normal
    range (0.02^4, the water's base alpha^2); hash inputs (`fract` of large products); the GI cascade walk
    (measured +16 B in half).
* **The direct-light BRDF** (`punctual_lights.inc.glsl`, the `*H` copies beside the fp32 functions): N, V,
  L, H, the dots, GGX D + visibility, Fresnel and the colour factors; each light's factor widens once, at
  the multiply with its fp32 radiance. GGX D is Filament's fp16 form: `1 - NoH^2` as `|N x H|^2` (no
  cancellation near the peak) and the square after the divide (alpha^4 never forms), clamped to 65504.
  **Alpha must be >= 0.01** (the lit FS, the splat and the film clamp it). Back-facing shading normals (N·V < 0, N·V
  clamped to 0) KEEP their specular on purpose: the grazing sheen on the grass comes from it (a gate on N·V <= 0 was
  tried 2026-10-04 and removed - it took that sheen away). Only the ANTI-LIGHT cone loses it (`antiLightFade`,
  `doLightH`: V within ~3-14° of −L, faded over |L + V| 0.25 → 0.05): there only back-facing normals are lit, and the
  almost zero `L + V` normalized in half made H noise, Fresnel → 1 and GGX at its peak - a bright DOT around the sun
  seen through the grass (the user's A/B: it stayed with the grass transmission at 0).
* **The lit core:** `computeLitColor(worldPos, V, f16vec3 N, f16vec3 albedo, float16_t alpha, metalness,
  AO)`, a half accumulator, half AO / bent normal; the light loop puts each light's result into half BEFORE
  its shadow ray (`lightShadowVisibility`), so no 32-bit light result is live across the query. `g_sunVisSurface` (one half scalar; the film rebuilds the
  surface sun radiance with `sunSurfaceRadiance()`) replaced two fp32 vec3 globals that were live across the
  whole light loop.
* **The terrain splat** (`terrain_splat.inc.glsl`) is half: samples, coverages, blend weights, normals.
  The climate match and the crag fBm hash stay fp32.
* **GI:** `GI_PROBE_HALF` (defined by the lit core and the ocean) adds the half interface
  `giIndirectOverPiH(worldPos, f16vec3 n)` and `giEvalSkySHH`. **The cascade walk inside stays fp32:** a half
  walk (half fetch results, half SH basis, half cascade blend - every combination tried) cost the lit FS
  16 B/thread against the fp32 walk converted once. The reflection fog's blend is half on `giEvalSkySHH`.
* **The ocean spectrum / FFT images are RGBA16F** (`SPECTRUM_FORMAT`): 512^2 x 9 layers streamed ~6x per
  frame, memory-bound; Ocean sim 0.216 -> 0.179 ms. The butterflies stay fp32 in shared memory.
* **Where the peaks are now:** the terrain's ground FS is 72/16 since the film moved to the TERRAIN
  OVERLAY pass (64/16 itself; it was 80/32 inside the ground's shader). The film's scene mirror ray is
  disabled (`TERRAIN_FILM_RT_MIRROR`). The ocean's seabed splat costs 16 B. The film surface (wave taps,
  slopes, Jacobians, foam) and the terrain's wetness block are half math.
* **Bisected 2026-09-22 - NO single peak left** (regs/local, "demand" = regs + local / 4):
  * Lit core (lit 64/32, terrain ground 72/16, demand ~72-76): the terrain splat alone is 54 (63
    tessellated), so the peak is `computeLitColor`. Removing the PCSS search (64/48), the light shadow rays
    (72/0), the underwater sun transmittance (64/48), the relief sun gate (72/16) or the two-cascade border
    PCSS (a dithered cascade pick: 64/48) - EACH ONE alone - leaves the demand the same. Several sites
    reach it; the terrain pays ~4 registers over lit.
  * Ocean (80/16, demand ~84): no underside path 72/32, no light loop 72/32, no scene rays 64/64, no rays AND
    no seabed splat 64/48. Same pattern.
  * `gi_probe_trace` (96/0): the sun ray, the light loop and the bounce each add ~16 over a ~80 base; the
    hit path whole is 72. `[[dont_unroll]]` on the ray loop: 96. Shared-memory accumulators instead of the
    per-thread ones: 109 (RMW) / 108 (`atomicAdd`, VK_EXT_shader_atomic_float).
  * The AO upsample's tap arrays as index bits: terrain 72/0 but lit 72/48; fully unrolled: terrain 72/32.
  * Rebuilding `specularColor` / `diffuseColOverPi` from albedo + metalness per light (4 live values, not
    6): a BYTE-IDENTICAL binary - the compiler hoists the loop-invariant values back out.
  * What is left is structural: split a peak into its own pass (the film's overlay precedent), or a
    thread-per-ray trace.
* **Measured 2026-09-23** (the surface-water rework; regs/local):
  * **A dual-source OUTPUT written early costs registers.** The ocean wrote `out_factor = vec4(0)` at the
    top of main as the default for its early returns. With the edge fade's dynamic store at the end, that
    kept the output live through the whole shader: 80/32 against 80/16. Write every output AT each return,
    never as an up-front default. Constant stores at both ends fold, so the bug only shows once one store
    becomes dynamic.
  * The ocean's edge fade (`oceanEdgeCover`: terrain height + the wetness pool level, 5 taps) costs nothing
    once it reads no value of main's. A fallback to `shoreHW.x` alone kept that value live across the
    traces and the light loop. It sits after the underside path with its early-out: 80/16.
  * **The film's peak is its light loop's shadow rays**: `doLight` in place of `doLightShadowed` measured
    56/0 against 56/48 (demand 56 vs 68); without the loop, 48/0. Lights-first ordering (the RT mirror is
    off now) and accumulating straight into `color` measured the same 56/48. Shadowless film highlights
    would be the lever, a visual trade (a light behind a wall would glint on a puddle).
  * The ground's wet sky reflection costs the TESSELLATED ground 16 B (72/16 -> 72/32; untessellated
    unchanged): its inputs stay live across `computeLitColor`. Every split (weight before, R.xz across,
    the whole reflection before) measured the same or worse; see the comment at the reflection.
  * The terrain TES is 70/0 for both the ground and the film: the relief taps are 6 of it (64 without),
    the rest is `terrainLayers` (the climate walk, per tessellated vertex). Its per-layer indices cannot be
    interpolated across a patch, so moving it per control point is not a small change.
* **Measured 2026-09-29** (regs/local): lit FS **72/32** (#10 variant 72/48; it was 64/32 on 09-22), ocean
  **80/32** (was 80/16), film 56/48 (same), terrain ground 64/48 and 56/48, cloud_march / cloud_sky 64/0,
  cloud_shadow 38/0, cloud_temporal 48/0, vol_scatter 72/0, vol_apply FS 56/16. Bisected against that day's
  changes: the lit core's filtered cloud lookup (bilinear: still 72/32), the ocean's filtered cloud lookup, its
  SH blur share and its SH ambient (each removed: still 80/32) - NONE is the cause. The lit +8 regs came in
  between 09-23 and 09-29 (GI cloud-shadow fixes 09-27, motion vectors / terrain opt 09-28): not bisected yet.
* **Measured 2026-09-30** (the foam / bubble rework; regs/local, Debug exe - the FS stats match RelWithDebInfo):
  ocean **80/32** (the rework had taken it to 80/48), film **64/16** (was 56/48: the same demand, 68).
  * The ocean's +16 B was the bubble cloud's THREE half-vec3 `exp`s (sun path down, sky path down, path up)
    in the middle of the top side. The two DOWN paths and the albedo are per-frame constants: `buildUboOcean`
    folds them into `u_ocean_bubbleSun` / `bubbleSky`, and `oceanBubbleRadianceFrame` keeps the one per-pixel `exp` (up).
    The film's depth is per pixel (capped at its water), so it keeps the full `oceanBubbleRadiance`.
  * The ocean's blurred bubble tap (up to 16 B-spline taps) moved to the top of main: NEUTRAL, but kept there
    (the cheapest point for it). It refracts through the level plane since.
  * `foamSlope` -> `foamNoL` (one half across the sun ray query, not two; the film's struct likewise): neutral.
  * Film, each stubbed, NO change: the bubble radiance (the frame form), the blurred milk tap, the sharp
    foam-field tap with the stuck foam. Its 64/16 is not this session's foam; its known peak is the light
    loop's shadow rays (below).
* **Measured 2026-10-04** (the GI sky's observer-averaged clouds + the sky SH's cloud-dimmed ground sun; RelWithDebInfo):
  cloud_sky 64/0, gi_sky_map 68/0, vol_scatter 72/0, vol_apply FS **56/32** (was 56/16 on 09-29), lit FS 72/48
  (#0, #1), 64/64 (#10), ocean 80/32, film 64/16, terrain ground 56/48 (#8) and 64/32, decal 55/0, particle VS 60/0.
  A/B with the sky-SH dimming removed (`giIrradiance`, the far field): every number the same. Lit #10 and #12 trade
  72/32 and 64/64 BETWEEN RUNS of unchanged source: the `#n` suffix follows the pipeline creation order, which is not
  fixed - compare those two as a pair. vol_apply's +16 B and lit #0/#1's +16 B came in between 09-29 and 10-04 (the
  far-tree layer in vol_apply 10-02, ...): not bisected.
  Then `u_sunTransmittance` for the six per-pixel / per-ray `atmosTransmittanceToLight(0, sun, up)` left (fog scatter
  x2, fog far field, decal, particle VS, `skyGroundSun` - per GI miss ray and in the sky-map bake) and the GI-off
  skip of the fog / decal / particle GI lookups: no register change.
* **Measured 2026-10-11** (RelWithDebInfo; regs/local): **vol_scatter 70/0 -> 62/0**, **river FS 72/64 -> 64/32**.
  * vol_scatter: ONE light loop over the large + cell lights (each `volLightScatter` inlines a ray query: 70 -> 66,
    code 50.7 -> 43.4 KB, with the next item); the shaft haze's density taken before the sun term and its radiance added
    into `emissive` (yA / yB / underFrac dead across the rays); then the scatter folded into `result` BEFORE the light loop
    (the loop adds `albedoWeighted x light`: one accumulator live, not inLight + emissive), and the reprojection ray,
    the camera forward and the cell index rebuilt at the end (66 -> 62).
  * river FS: the body trace FIRST, the glint and the sky share after it (`color` not live across its ray query), and
    the in-scatter + the aeration mix built INSIDE `traceRiverBody` after the trace (`riverInscatter`). Tried and
    reverted: the lights before the mirror trace (72/16), the sun tint as one scalar rebuilt from the UBO per use (64/48).
* **Measured 2026-10-12** (RelWithDebInfo; regs/local): the shared `water_scene.inc.glsl` left river FS 64/32 and ocean
  FS 80/32 unchanged. cloud_temporal's checkerboard fill pixels in ONE neighbour loop (the clamp range gathered with
  the weighted average): 48/0 and 9216 B before and after, cloud and TREE_TEMPORAL (scale 2, checker) variants alike -
  the driver had already merged the duplicate fetches. clutter_cull 80/16 after its relief view terms left the loop.
* **Measured 2026-10-12, the CPU-guarantee rounds** (UBO guards, then data guards; regs/local): lit FS 72/48 -> **64/32**
  (all 3 variants), terrain FS 72/16 -> 64/32, sky.fs 64 -> 56, aerial_lut 79 -> 73, particle_sim 62 -> 56, ocean 80/32
  -> 80/16, film 64/48 -> 64/32, rock 64/16 -> 64/16 (64/32 between the rounds). Worse by 16 B: the tessellated terrain
  FS (64/48) and clutter_cull (80/32) - not bisected (the driver's +-16 B). **The light loops' `min(count, MAX)` stay**
  although the counts are written capped, in `lighting.inc.glsl` and `particle.vs.glsl`: without them gi_probe_trace went
  96 -> 125 and the particle VS 56 -> 61 (an unbounded loop). The lit / ocean / river / film / vol_scatter loops read the
  counts raw at no cost.
* **Tried and dropped: the film's lights in the lit core's loop** (one loop, one shadow ray per light for
  both lobes; code 184 -> 145 KB): 80/64. The film surface must then be resolved BEFORE that loop and its
  values stay live across the shadow ray query; packing them did nothing (the driver folds it), and a
  variant with the film surface after the loop (lobe on the unrippled base normal) measured 80/144 - the
  scheduler's placement of the film's independent texture taps is not controllable from GLSL. Same
  register count as the film's own loop, so no occupancy gain, and the spill cost lands on EVERY terrain
  pixel while the saving is only on filmed ones. NVIDIA returns no internal representations (no SASS) through
  `VK_KHR_pipeline_executable_properties`, so bisecting is the only way to locate a peak.
* **Tried and dropped:** a compact half LightInfo (the compiler loads light fields at use); a D16 shadow map
  (geometry-bound, D32 is compressed); the film's lights before its mirror (N / V then live across the
  lights' own ray-query peak: 368 vs 352 B). **Not worth it:** the GI probe buffer (the fp32 temporal
  HISTORY - fp16 would round small blend steps away), BakedWorldMap (heights), vertex attributes.
