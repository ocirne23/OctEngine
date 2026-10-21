# Terrain, ocean, rivers and rocks

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Terrain and ocean integration

Procedural pushes the world-scale params in; the terrain state lives in `TerrainResources`
(`Data/TerrainResources.ixx`). The "Terrain/Water", "Terrain/Textures" and "Terrain/Tessellation" tweaks are
`Globals::settings.terrain` (`TerrainSettings`, Settings.Terrain), read by the UBO build every frame.

* **`setTerrainParams(meshRadius, seaLevel, lapseRate)`.** `meshRadius` is the radius inside which
  streamed chunks are guaranteed resident — **the fence for the ocean's land cull, and CPU-side ocean
  culling must fence on the SAME value its vertex shaders do**, or it deletes water the VS would have
  drawn (`getTerrainMeshRadius()`). `lapseRate` is the other half of the temperature reconstruction:
  the map bakes the SEA-LEVEL baseline and the shaders multiply this by the height THEY shade
  (`terrainTemperatureAt`) — **ONE value for the world, so no consumer can disagree with another.**
* **`setTerrainSplatMaterials`** registers the textures AND each material's climate box in one call,
  once, when the background DDS bake finishes. The box (`TerrainSplatMaterial::climate`) arrives with
  temperature already in t01 but **precipitation still in mm/yr: its divisor is the live tweak
  "Terrain/V3/Precip for full humidity"** (`v3PrecipFullHumidity`), which `buildUboTerrain` applies every frame, so
  that tweak reaches the shader without a texture re-upload. Materials lay out
  `[numGround][numRock][beach?][snow?]` as ONE contiguous range (climate boxes index the same slots),
  but **that is slot order, not draw order: the shader composites ground → beach → rock → snow**, so
  rock covers the beach. **Beach and snow are OVERLAYS, not materials the climate blend can pick** —
  beach paints over the waterline whatever the climate, and snow paints over everything else, ground
  AND rock. The contract is on `TerrainSplatMaterial` / `TerrainSplatCounts` (TerrainResources.ixx).
* **The beach also paints a river's BED**: `TerrainFields::river` (the river influence, 1 in a channel, 0 past its
  floodplain) over `TERRAIN_RIVER_BED_START..FULL` (0..1: the whole floodplain band), max'd with the shore band. It
  comes from the TERRAIN VERTEX's u (`positionU.w`; Procedural writes the river there - the terrain shades in world
  space and has no use for a UV): the terrain VS reads location 0 as a vec4 and passes `out_river` (location 4) through
  the tessellation stages to both FSs; the grass and clutter culls read it from the mesh with `grassGroundRiver`, over
  the same triangle as the height, so the bed's grass and clutter follow the ground's layers. The ocean's seabed
  splat sets it 0. The RT / GI hit shaders still read that u as a UV for the terrain material's diffuse texture,
  which the terrain does not have.
* **Per material THREE textures, packed** (Procedural's bake; one fetch fewer per layer than an ARM texture):
  diffuse = sRGB albedo + linear ROUGHNESS in the alpha (BC3); normal (BC5 sets `MATERIAL_FLAG_BC5_NORMAL`);
  height = HEIGHT (R, 0.5 = flat) + AO (G) (BC5). **Metalness is always 0** (`metalRoughnessTexIdx` stays none).
  **`MaterialInfo` has no free slot, so the height + AO index rides the UBO per slot**
  (`u_terrain_splatHeightTex[s >> 2][s & 3]`, 0xFFFF = flat, AO 1), like the climate boxes. **The splat reads its
  other texture indices from the UBO too** (`u_terrain_splatTex[s >> 1]`, .xy even slot / .zw odd: diffuse |
  normal << 16, BC5 flag; `terrainSplatTex()`), not from `in_materialInfos`: no fetch waits on a storage-buffer
  load first. `sampleTerrainXZ` issues its three fetches back to back. The materials are still registered
  (`addMaterials`) for everything else.
* **Rock is BIPLANAR** (`sampleTerrainTriplanar`, kept its name): only the two world projections the normal
  leans on most are sampled, each weight being |n| MINUS the smallest |n| (0 for the plane being dropped where
  the second and third swap: no seam); a second plane under `BIPLANAR_WMIN` (5%) of the pair is skipped too. At most
  2 x 2 taps + the height/AO tap. Roughness rides each plane's diffuse alpha; height AND AO come from the top plane
  (uvY) alone, so on a steep face the AO is the top projection's, stretched (it multiplies only the ambient term).
* **The terrain NOISE texture** (`TerrainResources::createNoiseTexture`, made once in `initSharedBuffers`;
  `u_terrain_noiseTex`): 512² RGBA8, tileable, box mips. R, G = two gradient fBms (8 lattice cells per tile, 4 octaves,
  stretched to 0..1); B = a value fBm (16 cells, 3 octaves, 0..1).
  * **The crag wander reads B** (`textureLod` 0, `u_terrainTex_cragWanderUvScale` = one wavelength per cell), from
    every `terrainLayers` caller (the TES and the culls too). **The grass cull has no bindless array**, so it binds
    the texture alone (`TERRAIN_NOISE_SAMPLER`) - its coverages must match the ground's, or grass grows on the rock
    border.
  * **MACRO VARIATION** ("Terrain/Textures/Macro variation", "Macro hue", "Macro roughness", "Macro size (m)";
    `terrainMacroAt` + `terrainApplyMacro`): against the visible texture repeat. Two R/G taps, the second 3.71x larger
    and turned 34°, give a brightness and a warm/cool hue (~[-1, 1]); the roughness follows the brightness. Applied
    over ground, beach and rock, UNDER the snow (step 4 of `terrainSplatLayers`), and to the boulders
    (`instanced_indirect_rock.fs.glsl`, not dead wood) so a rock keeps the tone of its ground. The taps are in uniform
    flow (screen derivatives); the ocean's seabed uses `TERRAIN_MACRO_TEX` = `textureLod` 0.
* **"Renderer/Textures/Anisotropy"** (Off / 2x / 4x / 8x / 16x, default 4x - a user decision): the max anisotropy of
  `StaticMeshGraphicsPipeline::m_sampler`, the sampler of EVERY scene texture slot (materials + the splat). A
  change waits for the GPU, recreates the sampler (`recreateSampler`) and re-records; `record()` writes all
  texture slots with it. The terrain is seen at grazing angles, where it pays most.
* **Terrain relief** (`TERRAIN_SPLAT_RELIEF`, defined by the terrain FS only; "Terrain/Textures/Parallax*",
  "Height blend contrast"; `u_terrainTex_parallax*` / `heightBlendContrast`):
  * **Height blend:** `terrainMixInto` steepens each layer's coverage ramp and shifts it by the height
    difference, so borders follow the texture relief. Contrast 0 = a linear blend.
  * **Parallax occlusion mapping, displaced along the geometric normal** (mesh = top of the relief). The
    texture offset is the ray's TANGENTIAL part, `-(V - N NoV) / NoV * depth`, so ONE 3D offset serves
    every layer, each with its own UV scale. **Never march along world Y:** that ignores the base slope, and
    over the ray's sideways run the slope changes height by more than the relief (slopes broke).
    `terrainReliefAt` runs the SAME layer chain on the height maps alone (explicit gradients; one tap in a
    climate-box interior), and `terrainParallaxOffset` marches it: a linear search with steps ~ 1/NoV
    (capped by "Parallax steps"), `TERRAIN_POM_BISECT` (4) bisection steps, then a secant. Grazing angles need the
    bisection: a secant alone leaves the linear steps visible as slices. `NoV` is floored at 0.05.
  * **Silhouettes** (`TERRAIN_POM_SILHOUETTE`, a define in the include, **default 0 - it does not work**: holes in
    front of the terrain behind a ridge plus a strip on the sky, whatever the facet handling): curved relief mapping,
    `rayH(s) = 1 - s + c s^2` with `c` from the base surface's screen-derivative curvature; a ray that climbs back
    out over a crest is discarded, and the march frame eases to the FACET normal as the interpolated one turns
    edge-on. **It puts a `discard` in the ground pass**, which defers the depth write for every terrain pixel. The
    TLAS, the shadow map and the collider keep the mesh crest.
  * **Relief self-shadow** ("Parallax self-shadow"): `TERRAIN_POM_SHADOW_STEPS` (6) steps from the hit up toward the
    sun, written to the lit core's `g_sunVisMaterial`. It scales the ground's sun radiance in `doSunLight` only, not
    `g_sunVisSurface`, so the water film keeps its glint. Faded by camera distance and on steep slopes, skipped past
    the fade end. The coverages and the lighting stay at the mesh point. Rock uses its TOP plane's height only.
  * `pom.inc.glsl` (UV-space SPOM, height in the normal-map alpha, needs `in_tbn`/`in_uv`) does not fit the splat:
    several layers, no TBN, BC5 normals with no alpha.
  * All of it runs before `computeLitColor`; only the shifted XZ outlives the splat, so it should not move the lit
    core's register peak. **Measure it (pipeline stats + profile) before you extend it.**
  * The ocean's seabed splat has no relief (no derivatives at a ray hit): its layer borders stay linear.
* **Both relief switches are BAKED** (the `Renderer::attachSettingsListeners` listener, RendererSettings.cpp: GPU idle,
  reload, re-record):
  * "Terrain/Textures/Parallax" (default off) → `TERRAIN_POM` 0/1 on the terrain fragment shaders. With 0, the march,
    the self-shadow and their derivatives are compiled out; the height blend stays.
  * "Terrain/Tessellation/Enabled" (default on) → the cull's `TERRAIN_TESS_ROUTE` 0/1, and whether
    `m_terrainTessPipeline` is built and its draws recorded.
* **Terrain TESSELLATION** ("Terrain/Tessellation/*", `u_terrainTess_*`):
  * **It cannot live in the DGC execution set.** Every pipeline in an indirect execution set must have the
    initial pipeline's shader stages (VUID-vkUpdateIndirectExecutionSetPipelineEXT-11152), plus identical
    static state and fragment outputs. So `StaticMeshGraphicsPipeline` owns a second `GraphicsPipeline`,
    `m_terrainTessPipeline` (the ground only; the film is never tessellated), built by `buildTerrainTessLayout` from
    the main layout: the same bindings (identically defined set layout, so the SAME descriptor set binds), vertex
    input, push ranges and baked fragment defines. `indirectBindable = false`.
  * **Routing (the cull):** only a `TerrainLit` chunk whose bounding sphere reaches inside "Fade end (m)" (+1 m for
    the VR eyes, from the centre view) goes to the tess draws; a chunk wholly past it is the same surface (edge factor
    1, no displacement) and takes the plain DGC path. A routed instance still allocates its DGC slot (`atomicAdd`),
    but that sequence draws `indexCount = 0`; the real draw goes to `out_terrainTessCommands` (binding 16,
    `atomicMax(idx + 1)` like the film). Same 24 B per-mesh-slot layout. Relies on ONE instance per terrain mesh (one
    `RenderMesh` + node per chunk): the DGC entry is per mesh, the routing per instance.
  * **Draws:** `record()` draws them with `drawIndexedIndirectCount` (offset 4 skips `pipelineIndex`, stride 24)
    right after the opaque execute, then rebinds everything (a generated-commands execute leaves the bound state
    undefined). The count is the compacted one (draw count [2], see Draw-list compaction ([Scene.md](Scene.md))); with
    tessellation off the cull routes nothing there.
  * **Shaders:** `instanced_indirect_terrain.vs.glsl` with `TERRAIN_TESS` hands on control points (the baked fields
    still per vertex). `terrain_tess.tcs.glsl` computes an edge factor from the edge's two end points only (edge length
    / distance / "Target edge (px)", eased to 1 across the fade band by `1 - t^p`, "Factor falloff exponent"), which
    keeps the edges crack-free, and culls patches outside the frustum. `terrain_tess.tes.glsl` displaces along the
    interpolated normal by `terrainReliefAt` (splat include with `TERRAIN_SPLAT_HEIGHT_ONLY`), CENTRED (height 0.5 =
    the mesh), faded by distance ("Height falloff exponent", its OWN `1 - t^p`) and on slopes (`smoothstep(0.35, 0.6,
    N.y)`). Keep the height falloff no slower than the factor's: a factor dropping before the height leaves displaced
    relief on coarse triangles. The height falloff is read by the TES, the ground FS pixel normal, the film VS lift
    and the ocean's film estimate. The domain origin is LOWER_LEFT, so `ccw` follows the GL rules.
  * **Lighting uses the UNDISPLACED position** (`in_meshPos`, location 3; `TERRAIN_LIT_POS` in the terrain FS under
    `TERRAIN_TESS`). The shadow map and the TLAS hold the flat mesh, and the centred relief puts half the surface below
    it: lit from the displaced point, that half self-shadowed in bands. The splat samples the displaced `in_pos`.
    **Keep it a vec3 interpolant:** one float (the displacement) rebuilt in the FS cost the tessellated ground FS 8
    registers - the compiler kept the rebuilt point live, while a vec3 interpolant is re-read at each use. The FILM
    keeps the one-float form (`in_meshLift`): there it costs no register.
  * **RTAO** rebuilds its origins from that displaced depth but traces the flat TLAS, so a relief low would hit its
    own mesh's underside at once. A TERRAIN hit nearer than the relief depth, inside the tess fade, continues once past
    it (`rtao.cs.glsl`). The instance + material buffers (bindings 8, 9) are therefore declared and written in BOTH
    RTAO variants, not only the alpha-test one.
  * **Displaced normals = the PER-PIXEL height gradient** (ground pass): `terrainTessPixelNormal` is the TES's
    displacement function (the same layers, height composite, depth and falloff, at `in_meshPos`) differentiated per
    pixel by forward differences over the whole displacement range. **The step and the mip are the TES's footprint**
    (the target edge at `max(distance, freeze)`), not the pixel's: at a pixel step, mip 0 magnified the height map's
    texel grid and its BC4 steps into a pixelated contour pattern. The 3 points come from ONE walk of the layer chain
    (`terrainReliefAt3`). The coverages are computed ONCE, at `in_meshPos`, and shared with the splat
    (`terrainSplatLayers`; `terrainSplat` is its wrapper for everyone else). Tried and dropped: an interpolated TES
    normal (only roughly matched the geometry, two more height evaluations per vertex), the displaced facet normal
    (flat-shaded the coarse mid-range tessellation), and the relief's facet tilt (facets close up).
    **The normal maps are faded to half** toward that normal at full displacement (they carry the same relief
    again), and **the relief normal gates the sun** (`reliefSunGate`): N·L <= 0 on it → no ground sun
    (`g_sunVisMaterial`), whatever the normal map says. `in_normal` stays the SMOOTH normal (`coverN`); it drives the
    splat's layer coverages and the wet slope drain, so the bumps do not scatter rock or snow.
  * **No swimming near the camera:** the edge factor comes from the DISTANCE (edge length × projection scale /
    distance), not from projecting the end points: a projected length changes under a pure camera rotation, and every
    factor change slides the fractional vertices onto other heights. Closer than "Freeze distance (m)", the factor and
    the TES's height mip footprint hold at that distance, so nothing moves there.
  * **Crack-free vertices:** a vertex on an edge interpolates its two corners in a fixed (lexicographic) order, and a
    corner is the control point itself. The height mip footprint is a function of the position only, not of the patch.
  * **The CENTRE view decides every level of detail:** the TCS factor, the TES fade + mip footprint, the ground FS
    `terrainTessPixelNormal` and the film VS lift read `u_views_*[VIEW_CENTER]`, never the eye's `u_viewPos` /
    `u_mvp` - per-eye decisions give the two VR eyes different geometry. Only `gl_Position` projects with the eye.
  * **The TCS skips work the TES would not use:** edge factor 1 where both end points are past the slope gate
    (`TERRAIN_TESS_MIN_NORMAL_Y` 0.35: the TES displaces nothing there; per-edge and symmetric, so still crack-free);
    a patch whose three smooth corner normals all face away from the camera (`TERRAIN_TESS_BACKFACE_MARGIN` -0.3, a
    margin for the relief's tilt) is culled with the frustum cull.
  * **No `precise`**: the engine's glslang crashes (access violation in `PropagateNoContraction`) on it.
  * **Not tessellated:** the shadow map (a user decision: near relief casts the flat mesh's shadow), the TLAS, the
    collider.
* **The splat shaping defaults** ("Terrain/Textures/*"): slope is `1 - N.y`; 0.30 = 45°, about where soil genuinely
  stops holding, which is also about the steepest the diffusion model's 30 m/px field reaches - so on V3 terrain
  the CRAG test ("Crag relief start / full (m)") is what puts rock on mountains. Snow has its own slope ramp ("Snow
  slope start / full") and lies over ground AND rock. The snow temperature band sits well below 0 °C because **mean
  annual temperature below freezing is NOT permanent snow — Siberia averages −10 °C and is forest.** The crag wander
  exists because V3's macro altitude is nearly flat across one mountain, so **the raw crag test traces an elevation
  contour right across a range.** The crag thresholds are model-scale metres, scaled by `setTerrainCragScale`.
* **`setOceanParams`** — flipping `hitLighting` (`OCEAN_HIT_LIGHTS`), `rtReflections` (`OCEAN_RT_REFLECTIONS`, the
  scene mirror ray; "Ocean/RT/Reflections") or `debugMode` (`OCEAN_DEBUG_MODE`, "Ocean/Debug mode"; the mode legend is
  at the top of `ocean.fs.glsl`) rebuilds the ocean fragment variant (GPU idle + shader reload). The River variant gets
  the same defines minus the debug mode.
* **Ocean ray rules** (shared by the terrain film where it applies):
  * **Every scene ray is gated on its VISIBLE weight (2%)**, resolved before it is traced: the ocean's refraction on
    `(1 - F)(1 - milk)(1 - foam)`, its mirror on `F (1 - reflBlur)(1 - foam)`, the film's shadowed light walk on
    coverage x `(1 - foam)`, the underside's mirror (+ its sun shadow ray) on `1 - window transmission`, and an
    underside pixel the water path has absorbed to 0.1% traces nothing.
  * **The UNDERSIDE traces the scene above the water** through Snell's window (the refracted ray into the air; same
    define, "Reflection range (m)" and cutoff as the mirror ray, gated on the window's transmission > 2%); a miss keeps
    the sky + sun glitter. Hit and sky are both fogged over the LITERAL path (`applyRayFog` / `applyRayFogSky`): the
    mirror rule below needs a directly visible source, which an underwater viewer does not have. **The underside wears
    the surface FOAM too**: the foam terms are resolved before the side split (the stuck foam's edge takes screen
    derivatives), a foam patch is laid over the window + mirror as a backlit diffuse sheet, and it scales both
    underside rays' weights.
  * **Mirror rays are fogged, hits AND the reflected sky** (`reflection_fog.inc.glsl`, ocean + film).
    **THE RULE: a reflection carries the fog its SOURCE carries when seen directly** -
    `tau(camera -> what the ray shows) - tau(camera -> the water point)`, the second term being what the screen-space
    fog pass lays over the water pixel afterwards. NOT the literal path from the water point: height fog is densest at
    its base, so a grazing mirror ray stays in the densest metre while the camera's ray to the same shore runs a camera
    height above it - the literal path buried every reflection and no single fudge factor fit two fog settings. Closed
    form (`volAnalyticOpticalDepthLinear`), near medium, capped at the fog range, no fetch; terrain follow is
    approximated from the origin's and the hit's own height above sea level (half of it). No regional climate, no
    noise, unshadowed; lit like the far field (HG sun + the GI sky probe). "Ocean/RT/Reflection fog"
    (`u_ocean_rtReflectionFog`): 1 = the source's fog, 0 = off.
  * The mirror ray's "Reflection max rough" gate reads the roughness WITHOUT "Micro roughness" — that term is a
    constant floor, so inside the gate it would switch the mirror off on every pixel.
  * `setOceanWaveTrough` sizes the waterline band the fog scatter samples for the underwater fog boundary.
* **The Ocean variant is BACK-FACE CULLED.** The clipmap carries every triangle in both windings (see
  Sectors in [`Docs/Context/Procedural/Ocean.md`](../Procedural/Ocean.md)), so the underside draws from
  under water and the scene depth holds the nearest face from either side. Do not switch it to cull
  none: every wave crossing along an underwater ray would be rasterized, and shaded until a nearer
  one lands (there is no prepass early-Z to reject them). The underside shading (Snell's window, TIR, "Ocean/Shading/
  Underside transmission", `u_ocean_undersideTransmission`) lives in `ocean.fs.glsl`.
* **The particle GPU SPAWN PATH + ocean spray.** `ParticlePipeline` keeps one shared spawn-request
  buffer (`MAX_PARTICLE_GPU_SPAWNS` × `ParticleSpawnRequestGpu`) and a request counter in its counters
  block; a producer compute pass that runs BEFORE "Particle sim" binds both (`getCountersBuffer` /
  `getSpawnRequestBuffer`) and appends through `particle_spawn.inc.glsl` (the contract is in
  [Particle](../Particle/CONTEXT.md)). `ocean_spray.cs.glsl` is step 6 of `OceanSimulationPipeline::record` (after the
  mip chain; the maps' final barrier includes compute): an `OCEAN_SPRAY_GRID`² (128²) world grid around
  `u_sceneFocus` ("Ocean/Spray radius"), per cell the fold Jacobian + crest acceleration at explicit LOD (compute has
  no derivatives) through the water shader's own `oceanInstantFoam`, land skipped through the shore data (binding 2,
  the fog terrain map, UPDATE_AFTER_BIND + refreshed per frame like the wetness pass), and a hashed dice at "Spray
  rate" × cell area × dt × breaking. The emitter slot arrives through `setOceanSprayEmitter` (the Particle system's
  `Effects/ocean_spray.pfx` instance) in `u_ocean_sprayEmitter`; `UINT32_MAX` switches the step off in-shader, so the
  cached CB records once. Tweaks "Ocean/Spray rate / radius / threshold / kick / speed / forward offset / height
  offset"; "Particles/Ocean spray" switches it.

## River and lake water (`EPipelineIndex::River`, variant 14; "Terrain/Rivers/Surface")

Procedural's `RiverSystem` ([Rivers.md](../Procedural/Rivers.md)) builds ONE `RenderMesh` per river unit - a ribbon per
perennial segment at the carved water surface and a quad per lake row run at the lake's level - on the River
pipeline's one material (`createMeshMaterial(River, false)`: not in the TLAS), main pass only (`PASS_MAIN`), pushed
every frame like the ocean sectors. **The ocean is NOT extended for inland water** (a user decision): the river has its
own shaders, `River/river.vs.glsl` + `river.fs.glsl`, whose shading follows `ocean.fs.glsl` without the waves. **The
scene side is SHARED** through `Ocean/water_scene.inc.glsl` (the common bindings, the BRDF terms, the sky lookups,
`traceScene` / `shadeHit`); each includer defines `WATER_ABSORPTION` and its own bed hook `waterBedAlbedo` (the ocean
returns the calm level from its terrain fetch; the river keeps its own surface as the column) - Fresnel (F0 0.02), the
RT refraction to the bed (Beer-Lambert both ways, `u_river_absorption`; the bed is the terrain splat with the river
bed's beach layer, the baked height map where the TLAS cannot answer), the RT mirror (sky fallback, reflection fog),
the GGX glint, the light-grid lights. **Its RT toggles and ranges are the ocean's**: the variant gets the ocean's
`OCEAN_RT_REFLECTIONS` / `OCEAN_HIT_LIGHTS` defines (not its debug mode) and reads `u_ocean_rt*`.

* **The vertex** (river.vs.glsl): `positionU.w` = across the ribbon (-1..1; **2 = a lake**), `normalV.w` = the distance
  along the river (m), `tangent` = (flow x speed (m/s) in x / z, the whitewater amount in y). No displacement on the
  light ribbons.
* **THE WAVES are the OCEAN's FFT field** (`River/river_wave.inc.glsl`; the ocean always runs with the rivers - its
  maps, binding 7, are never stale while a river draws): per cascade the height (layer c) and slope (layer
  OCEAN_CASCADES + c) at "Wave tiling" x the ocean's frequency and "Wave height" x its height (x (1 + "Wave rapids" x the
  whitewater)), DRAGGED DOWNSTREAM by the vertex flow with a two-phase flow map (period 2 s, the second phase elsewhere
  in the field). The FS normal is that slope everywhere (far ribbons and lakes too, lakes x "Lake ripple") plus the noise
  ripples (below) as finer detail.
* **Geometry**: the DENSE near-cell ribbons (the vertex normal's x = 1; RiverSystem, within "Near radius (m)") are
  displaced by the same height in the VS, faded out over the last 30 % of the radius and the outer 40 % of the width to
  each bank, a TROUGH soft-limited to 70 % of the water column under the vertex (the normal's z: the channel's
  cross-section depth, 1e4 on a lake) so a shallow stream never shows its bed through the troughs; the dense LAKE grids
  take the same waves with the lakes' slow drift x "Lake ripple", to the shore. The LIGHT ribbons and flat lake quads
  sink "Near drop (m)" out to the radius + 2 near cells (`RIVER_NEAR_CELL` 64 m = RiverSystem's `c_nearCell`, **keep
  in step**) and rise back over one more; and **inside `u_river_nearCovered` they DISCARD**
  (`Renderer::setRiverNearCovered`, live: RiverSystem's distance around the camera within which every near cell is
  built) - sunk alone, a camera inside a deep channel saw the flat ribbon below as a second water layer. Where a cell is
  not built yet the sunk light surface still shows (no hole). The VS passes the dense mark as `out_dense` (location 3).
* **SMALL RIVERS**: the vertex normal's y carries the channel's centre depth (engine m, lakes 1e4); size = it / "Full
  size depth (m)" (at most 1). The waves scale by size (the VS height and the FS slope, `out_waveSize`, location 4: a
  fading stream head has none), the flow by mix("Small river flow", 1, size) (the ripples' drift and the waves' drag),
  and the TURBULENCE by size too (a trickle has too little water to churn, whatever its slope). From the full-size
  depth up a river behaves as set.
* **The ripples**: a two-phase FLOW MAP over the terrain noise texture's R / G gradient fBms (`u_terrain_noiseTex`),
  dragged downstream by the vertex flow ("Flow speed"), two scales, "Ripple size (m)" / "Ripple strength"; a lake
  drifts slowly in one direction and ripples at "Lake ripple".
* **TURBULENCE is a smooth gradient from calm water, not a kind of water** (a user decision): turb = the vertex
  whitewater amount (Procedural: a smooth function of the water's slope, blurred along the river) x "Foam strength" x
  size. As it rises: rougher (+0.12 x turb in alpha^2), a milkier body (aeration 0.6 x turb mixes the in-scatter toward
  the foam colour and the traced bed toward that in-scatter) and higher waves ("Wave rapids"). **No foam layer on top**
  (noise-threshold foam streaks were tried and removed).
* **In the transparent family** (`PIPELINE_TRANSPARENT_MASK`), composited DUAL-SOURCE like the ocean (`dualSourceAlpha`,
  depth write on, no back-face cull): a ribbon fades out over its last "Edge softness" of half-width, a lake over its
  last "Lake edge fade (m)" of water column (the baked height map) - **inside the NEAR data cascade only**
  (`nearMapWeight`): the far cascade is the generator's coarse stage, uncarved and smoothed, standing above most lakes;
  out there the lake is opaque and the terrain mesh cuts its shore (the body trace's height-map fallback is near-only
  too). **Writes alpha 0** - TAA's animated-surface flag, as the ocean (the ripples move without motion vectors).
* **The body trace starts 1 m BACK along the refracted ray** (`traceRiverBody`; the water path counts from the
  surface): in a shallow stream the TLAS bed (a coarser terrain LOD) can lie at or just above the drawn surface, and a
  ray from the surface misses it - a bright band along the banks.
* UBO section "Rivers" (`river_*`, RendererUbo.cpp), lock section "Rivers" (`c_uboLockSections`: the
  "Terrain/Rivers/Surface" rows; `river_nearCovered` is `UboLive`).
* **THE INLAND WATER MAP** (`Renderer::setRiverWaterMap`, `RiverWaterMapGpu`: `RIVER_WATER_MAP_DIM`² texels of
  `RIVER_WATER_MAP_TEXEL` m around the camera, the river / lake surface Y per texel, `RIVER_WATER_NONE` elsewhere; dim 0
  = no map). Baked by RiverSystem, copied into a per-frame-slot host-visible SSBO (`m_riverWaterMaps`). The
  terrain-data map's water channel is the sea's only, so the two consumers that need inland water read this:
  * **the underwater fog** (`vol_scatter.cs.glsl`, binding 14, `riverWaterAt`, nearest texel): where the inland surface
    lies above the sea's, it is the froxel's underwater boundary (no ocean wave taps there), with `u_river_scatterColor`
    as the albedo, "Fog/Lake underwater density" in place of "Fog/Underwater density" (lakes and rivers), x "Fog/River
    underwater scale" under a river (the map carries the kind in the height's LOWEST MANTISSA BIT: 1 = a river channel,
    0 = a lake - one float step), and the calm surface for the light shafts;
  * **the terrain wetness** (`terrain_wetness.cs.glsl`, binding 4): a wetness texel under inland water OR one map texel
    from it (the bank band) targets "Terrain/Rivers/Surface/Wetness" (`u_river_wetness`, default 0.6) at the base
    decay; the pass's sideways diffusion carries the wet fringe up the banks, so the terrain film forms there with no
    river-specific film code.
* **THE RIVER MIST PRODUCER** (`River/river_mist.cs.glsl`): the GPU spawn path's second producer, recorded by
  `ParticlePipeline::recordSim` at its HEAD (before the begin pass, which latches the spawn count; a compute barrier
  between). Its input is `setRiverMistSources` (RiverSystem: up to `MAX_RIVER_MIST_SOURCES` whitewater stretches near
  the camera, `RiverMistSourceGpu` = surface point + half-width, flow velocity + whitewater x the river's size + the
  stretch's length), copied in present into a per-frame-slot host-visible SSBO (`m_riverMistBuffers`). One thread per
  source slot: a hashed dice at `u_river_mistRate` x the stretch's area x dt x pow(smoothstep(threshold, 1,
  whitewater), `u_river_mistCurve`); hits spawn over the stretch (across it at |u|^(1 + `u_river_mistCentering`) x the
  half-width: gathered toward the centre line), drifting downstream ("Mist speed" x the flow) and kicked up ("Mist
  kick (m/s)"). UBO-gated by `u_river_mistEmitter` (`setRiverMistEmitter`, the Particle system's
  `Effects/river_mist.pfx`; `UINT32_MAX` = off), so the cached CB records once. Its tweaks are
  "Terrain/Rivers/Surface/Mist ..." (the "Rivers" lock section); "Particles/River mist" switches it. A NEGATIVE
  half-width marks a fall's plunge pool: it takes `u_river_plunge{Centering,Speed,Kick,Height}` ("Plunge mist ...") in
  place of the whitewater's four.

## The rock material (`EPipelineIndex::LitRock`, variant 13; `RockParams`, "Rocks/Material")

The procedural rocks (Procedural `RockSystem`, [Rocks.md](../Procedural/Rocks.md)). A rock has NO textures and its
material is not read: **the surface comes from the climate it stands in, through the terrain splat's own materials and
rules** - a boulder under a sandstone cliff is that sandstone.

* **`instanced_indirect_rock.vs.glsl`** (the terrain VS's shape: position + normal only): the terrain-data fields PER
  VERTEX - the ground height under the vertex (`terrainDataAt().x`), the temperature at the vertex's own height
  (`terrainTemperatureAt`: the top of a tall boulder is colder than its foot), the humidity, the water level - as one
  interpolant. Without a map: the instance origin as the ground, a mild climate. Plus the vertex's baked **CAVITY**:
  a rock mesh has no uv, so `MeshVertex`'s u (`positionU.w`) carries it (Procedural `rockCavity`: 1 = open, 0 = deep
  in a crevice).
* **`instanced_indirect_rock.fs.glsl`**: the lit core (`SUN_SHADOW_FIRST`, `g_waterLevelOverride`) +
  `terrain_splat.inc.glsl` WITHOUT `TERRAIN_SPLAT_RELIEF` (linear blends, no parallax). Coverages first, then bottom-up:
  1. **Bedrock** - `pickClimate` over the splat's ROCK entries (the cliff's pick), `sampleTerrainTriplanar` (biplanar)
     in WORLD space at the terrain's rock uv scale x "UV scale (x terrain rock)" (4: a boulder shows finer grain than a
     cliff). Rocks are static: world space does not swim, and every instance of a shape shows its own piece of the
     texture.
  2. **The ground material** (the climate's top two GROUND entries, biplanar): a patchy COVER on up-facing faces
     ("Ground cover" x smoothstep of normal.y over "Cover start / full (normal y)", x a world fBm of "Cover patch size
     (m)") and in the crevices ("Cavity cover" x (1 - cavity), on faces that do not look down), and the
     **CONTACT BAND** at the foot (1 at the ground, 0 "Contact height (m)" above it; "Contact blend" of ground material
     - it hides the cut line - and "Contact darkening" on the AO). The ground height is the data map's, metres-wide
     texels near the camera and far coarser beyond: the band fades out by "Contact fade distance (m)".
  3. **Snow** - the terrain's rule (cold x holds x humid) with the face's own slope, world-XZ projection.
  The AO = the contact darkening x the cavity ("Cavity AO"). Then the terrain's wetness (`terrainWetnessAt`: the
  ground's darkening + gloss thresholds and slope drain; no drying pattern, glints or sky reflection). Before a splat
  set is registered: plain grey stone.
* **The pipeline**: a variant like the terrain's (opaque, back-face culled, static: the motion target stays masked).
  `isLitCoreFragment` in `buildPipelineLayout` gives it the lit core's baked defines (`LIT_RT_*`, the debug modes).
* **Meshes**: a regular LOD chain per shape (`createMeshLodChain`; the culls pick the level per instance), raytraced
  through the chain's one BLAS. **No tessellation** (a user decision).
* **In the tree instance set** (the world's rocks): see "BAKED TREE RECORDS" ([Scene.md](Scene.md)) - a rock record
  gets the LOD pick and no wind.
* **DEAD WOOD** (Procedural `Surface Wood`: fallen logs, stumps, snags - the same pipeline, the same records). The mesh
  marks it: its **v** (`normalV.w`) is 1 + the vertex's BARK cover (1 the outer skin, 0 a cut or broken face), 0 for a
  rock; its **TANGENT is not a direction but the vertex's WOOD COORDINATES** (Procedural RockMesh: along the part it
  belongs to - the trunk, a stub, a root - in units of its base circumference, and across it from that part's axis,
  nominal units). The VS passes along as is and across in metres (x the instance scale), with the instance's material
  index. The FS replaces the bedrock (step 1) with `woodSurface`: the MATERIAL is a tree species' BARK TEXTURE
  (RockSystem: BC1 albedo with the type's tint baked in + a BC5 normal map), mapped as on a tree branch - u = the angle
  around the part (once around), v = along. 2 taps (`textureGrad`: at u's wrap the derivatives come from a copy that
  wraps on the far side, so the seam keeps its mip) and the normal map through a cotangent frame from the screen
  derivatives (the part's axis is not known in the FS). The cut faces (bark cover 0) show END GRAIN: the same tap
  paled and greyed, growth rings ~5 mm around the axis (faded out before they alias), a darker core. Steps 2 and 3 and
  the wetness lie on top unchanged: litter or moss on the top, the contact band, snow. Without a splat set the wood
  still shows. GI and the RT hits sample the same bark texture (at the mesh's uv: its mean colour, roughly).
  **Nothing else in the renderer may read a rock mesh's tangent as a direction** (the GI / RTAO hits read no tangents).
* **CACTUS** (Procedural `Surface Cactus`: the saguaro). The same wood coordinates in the tangent, its **v** = 3 + the
  rib count / 100 (the FS: `in_wood.w > 2.5`, the count = round((w - 3) x 100)). The FS replaces the bedrock with
  `cactusSurface`: the MATERIAL is a 1 x 2 texture - the skin (row 0), the spines (row 1), two `texelFetch`es, no
  sampling - and `cactusSkin` (`cactus.inc.glsl`, shared with the clutter's cacti) puts an AREOLE on every rib crest:
  the angle around the part (`cactusAround`: atan of the across coordinates, a seam-safe pixel width) x the rib count
  finds the crests (Procedural `ribIndent`'s convention: the ribs are the mesh's), along x the rib count spaces them -
  0.9 rib widths apart, every other rib half a step on. Each a greyish felt pad with a star of 7 pale spines (a spine
  thinner than a pixel draws a pixel wide, as faint as it is thin); the valleys a little darker, each rib its own shade;
  the whole pattern fades to its mean past ~1/4 rib per pixel. Roughness 0.55 (waxy), duller on the pads and spines.
  **No ground cover** (no sand on the crown or in the rib valleys); the contact band, snow and the wetness as on a
  rock. GI and the RT hits sample the 1 x 2 texture at the mesh's uv (v = 3.x: mostly the skin row).

## Terrain surface water

ONE wetness field feeds ONE water surface ("Terrain/Water" tweaks, `Globals::settings.terrain.wet*`,
`u_terrainWater_*` / `u_terrain_wet*`). It is both the rain puddles on the ground and the continuation of the ocean onto
the sand, so the two can never disagree.

* **The field** is the wetness clipmap (`TerrainWetnessPipeline`, below): rain everywhere, 1 under the live ocean
  surface (its swash tongue included), decaying back with "Dry time (s)" and spreading with "Diffusion (1/s)".
  "Slope drain" applies on both sides: the compute pass divides wet-in and rain by it (map gradient), the terrain FS
  raises the decay by it per pixel (mesh normal).
* **The LEVEL** (`terrainPoolLevel`, `terrain_wetness.inc.glsl`) is how far that wetness fills the splat RELIEF - the
  same height composite the tessellation displaces by: 0 = the relief's low points, 1 = its top. "Fill start" / "Fill
  full" bracket it in wetness, "Fill curve" shapes the rise. **The SLOPE sinks it** ("Film max slope (deg)" / "Film
  slope fade (deg)", `u_terrainWater_slopeCut` / `slopeFull` as smooth-normal y): full below max - fade, 0 at the max,
  so a pool on a steepening slope recedes into the relief's low points instead of alpha-fading. The live-ocean part
  of the film is NOT slope-limited. Clamped to 1: the level is a height INSIDE the relief, and an unclamped one lifts
  the film off the ground.
* **The SURFACE: the film is NEVER tessellated** (a user decision: it follows the water level, not the relief). The
  terrain VS under `TERRAIN_OVERLAY_PASS` lifts each vertex along the normal to `max(level, live ocean surface)` in the
  relief band (`terrainFilmLevel`), inside the tessellation range only, with the TES's own distance fade and slope gate
  and the GROUND relief depth (the rock's is a per-pixel coverage). A pool's surface is flat across a crevice, and at
  the waterline the film rides the ocean's own live displaced surface (the baked water level + `underwaterLiveWaveY`).
  Capped at the relief top - above that the ocean draws its own surface and a film lifted to the same height would
  z-fight it. Elsewhere (and with tessellation off) there is no lift. **Depth test GREATER_OR_EQUAL**
  (`PipelineVariant::depthGreaterOrEqual`): with no lift the film is bit-identical to the flat ground
  (`invariant gl_Position`), and the displaced ground hides it where it stands higher. The VS hands the lift on
  (`out_meshLift`, location 3, one float): the film FS rebuilds the mesh point from it (`TERRAIN_LIT_POS`) and lights
  and measures the relief from there. The wetness clipmap (18), the ocean maps (7) and the terrain data (19) are bound
  to the vertex stage for this.
* **The COVERAGE** is how much water stands over a pixel: `(level - relief height) x relief depth` METRES (or the live
  ocean over the ground where deeper, see the hand-over below), faded over the last "Edge fade (m)". The film therefore
  always dies exactly where its surface meets the terrain, per pixel and shaped by the texture relief - not on the
  geometry's per-triangle outline or the clipmap's texel contour, both of which pop in blocky.
* The film re-draws the chunk's whole index range (the chunks have no skirts - "Edge stitching" in
  [`Docs/Context/Procedural/TerrainStreamer.md`](../Procedural/TerrainStreamer.md)).
* **The GROUND** under it: albedo x "Wet darkening" and the wet gloss, each with its own wetness threshold ("Darkening
  threshold" 0.3, "Roughness threshold" 1.0; `u_terrainWater_darkeningThreshold` / `roughnessThreshold`). Each amount
  is `smoothstep(0, threshold, wet)`: a plateau while the ground is soaked, a smooth fade to dry below the threshold.
  Tried and dropped: log10 ramps with "reach" tweaks, a gain on the wetness (a hard edge where it saturated), an
  exponential curve.
* **THE DRYING PATTERN** (the ground pass's darkening + gloss): ground dries in metre-scale ISLANDS, not uniformly.
  * P (0..1, low = holds its water longest) is a world-anchored value fBm (`terrainFbm`) of "Drying pattern size (m)"
    (`invDryingPatternSize`), stretched from its central bunching toward 0..1 by "Drying pattern contrast"
    (`dryingContrastHalf`, clamped: higher = stronger islands). "Drying pattern relief" (`dryingPatternRelief`) of
    `surf.height` is mixed in, for fine breakup at the island edges.
  * Each wet amount (above) becomes a LEVEL through P. Ground below it is wet, and above it is a dry island. The
    islands grow as the ground dries. Darkening and gloss share P, so with a higher roughness threshold an island
    loses its gloss first, then its darkness.
  * Each level has its own soft band: "Darkening edge" (`darkeningEdge`) and "Roughness edge" (`roughnessEdge`), both
    0.5 by default (narrow = crisp islands). "Drying pattern" (`dryingPattern`, 0..1) mixes from uniform drying to the
    patterned look. The pattern fades out where its cells shrink below a few pixels (`fwidth`): the noise would
    shimmer there.
  * Where the field is at 1 (under the swash), the level is above all of P: all wet.
  * Tried and dropped: the relief ALONE as the pattern (it tiles at the splat texture's scale: speckle, not
    patches), and a faster-drying power on the peaks' wetness (no threshold, so no islands).
* **The wet SPECULAR** (the ground pass, with the gloss weight `glossW`):
  * The lit core has NO environment specular (diffuse GI + the lights' GGX lobes only), so wet ground with one sun
    highlight reads as merely darker. (Not an fp16 problem: the half GGX's `|N x H|^2` error at the wet alpha is ~1e-6
    against a^2 = 0.0064.)
  * "Wet normal scale" (`wetNormalScale`): with the gloss, the normal map's tilt off the shading base is scaled
    (1 = unchanged). Below 1, water fills the micro relief and the highlight stays sharp. Above 1, the tilt is
    exaggerated. It is kept on the base's side of the horizon.
  * The wet SKY REFLECTION (no tweak, always on): after `computeLitColor` (past the light loop's register peak), the
    film's sky model: the baked mirror sky along R, water Fresnel (F0 0.02), the film's roughness-to-blur rule (rough
    wet ground mirrors nothing), the texture AO as specular occlusion, and the mirror fog rule. Gated by `aboveLive`
    and off under the film (`underFilm`, below), so the two never both mirror the sky. Two sky-map fetches on
    reflecting pixels only.
* **GLINTS** ("Glint size (m)" `u_terrainWater_invGlintSize`, "Glint coverage" `glintCoverage` (min 0.01), "Glint
  roughness" `glintRoughness`): beaded water and flat wet grains catch the sun in small sharp points.
  * Sparse world-anchored patches: a single-octave `terrainValueNoise` over glint-size cells, its peaks thresholded
    (`smoothstep(1 - coverage, 1 - coverage / 2, n)`). They drop the roughness to a near-mirror alpha, on the wet gloss
    only (x `glossW`, not under the film or the live ocean).
  * They fade out where a cell shrinks below ~0.3-0.7 pixel (`fwidth`, shared with the drying pattern), where they
    would only shimmer.
* **Three ROUGHNESSES:**
  * "Water roughness" (`u_terrainWater_waterRoughness`, perceptual) is the FILM's base, in the ocean's microfacet model.
  * "Wet roughness" (`wetRoughness`, GGX alpha like the splat's `rough`) is the wet ground's roughness. It is full at
    the "Roughness threshold" and is broken into islands by the drying pattern.
  * "Underwater roughness" (`underwaterRoughness`, GGX alpha) is the ground's roughness under WATER: the live ocean
    (`1 - aboveLive`, shows through the ocean's edge fade) and the FILM (`underFilm`). `underFilm` is the film's own
    coverage recomputed in the ground pass (the pool level through `surf.height`, over its "Edge fade (m)", with the
    ground relief depth as the metres) - not a wetness gate, so the wet gloss stops exactly at the film's outline.
  * The darkening stays under the ocean, because the ocean's traced seabed is darkened the same.
* **The HAND-OVER to the ocean** (two tweaks, so the seam is neither the film's nor the ocean's hard edge):
  * "Ocean blend (m)" (`u_terrainWater_oceanBlend`): `aboveLive` fades from 1 to 0 over this much LIVE water over the
    GROUND (`g_liveDepthBelow` at the tested point + that point's height over the relief ground). The film draws from
    its OWN list BEFORE the transparent execute (below), so the ocean always covers it and only the ocean's fade band
    shows the film. The film must still be gone before its surface sinks under the ocean's. **Keep the blend under the
    relief depth.**
  * The film's coverage depth is `max(pool depth, live water over the ground)`, so the film always covers the ocean's
    thin edge, also where the wetness has not caught up yet. The Beer-Lambert tint runs on the same value.
  * "Ocean edge fade (m)" (`u_terrainWater_oceanEdgeFade`, min 0.01): **the ocean** (`ocean.fs.glsl`) fades out over
    this much water column, so its mesh does not end in a hard line where it cuts the ground; the film under it carries
    the water. A real blend: the Ocean variant composites DUAL-SOURCE (out = ocean x a + scene x (1 - a)), and **the
    cull puts ocean instances in the TRANSPARENT sequence** so they draw after the ground. The ALPHA blends too
    (`PipelineVariant::dualSourceAlpha`): out.a = 0 + dst.a x (a > 0.5 ? 0 : 1), the TAA ocean flag where the ocean is
    most of the pixel. Top side only; a fully faded pixel returns before the shading. The band is an ease-out,
    cover = 1 - (1 - t)^2 with t = column / fade: always 0 at the bottom (the mesh edge stays hidden), mostly opaque
    above it.
    **The column is to the FILM's water surface**, where the ocean hands over to it: `terrainHeightAt(in_pos.xz)` +
    (`terrainPoolLevel(`the wetness clipmap`)` - 0.5) x the ground/beach relief depth, with the tessellation's
    distance falloff (0 past it or with tessellation off). The ocean therefore samples the wetness clipmap (binding 18,
    `terrain_wetness.inc.glsl`), at the pixel's own position, not `in_uv`'s undisplaced lattice point. The pool level's
    slope term takes the ground normal's y from the baked map's forward gradient over 4 m (two more taps). The whole
    fade is `oceanEdgeCover`.
    * **Edge foam was tried and REMOVED**: foam across the fade band made the fading edge opaque and showed the
      ocean's own LOD'd, clipmap-snapped edge stepping as the clipmap scrolls.
    * **Not a dither:** an IGN discard (TAA-resolved) stayed as flickering noise, because TAA caps the history on ocean
      pixels.
    * **Cost:** the ocean does not early-Z reject the tessellated seabed under it; that ground is shaded, then covered.
* **The FLOW** ("Film flow speed (m/s)" `u_terrainWater_flowSpeed`, "Film flow min slope (deg)"
  `u_terrainWater_flowSlopeHalfTan` / `flowSlopeTan`, "Film flow cycle (s)" `flowCycle`; `terrainFilmSurface`): the
  inland ripples run DOWNHILL.
  * Direction: `+N.xz` of the smooth mesh normal (a normal leans toward the DOWNHILL side), exact per pixel. Not the
    terrain-data map's flow bits (`terrainFlowEncAt`): 8-bit angles on ~8 m NEAREST texels, which would step.
  * Speed: the tweak (at 45 degrees) x sqrt(tan slope), x a slope GATE: none below half of "Film flow min slope",
    full from it (smoothstep on tan; default 8 degrees), so pools and flats lie still. sqrt(tan) alone moved gentle
    beaches too much; a tan^2 curve stilled them but slowed the moderate slopes with them. Off the shore only
    (x (1 - shore)): the shore band carries the ocean's own waves.
  * A two-phase flow map on the finest cascade's slope tap and the detail tap. Each is sampled again half a cycle apart
    and crossfaded by a triangle, so neither phase's reset shows. The finest cascade's phase 1 is one slope-only tap
    after the cascade loop. The moments and the Jacobian (shore foam) do not flow.
* **The LOOK** is the ocean's own (`terrainFilmSurface` / `terrainFilmShade`): "Waviness", "Normal scale", "Wind
  ripples". The film's Beer-Lambert TINT runs on the SAME water depth as the coverage - a puddle deepens toward its
  middle.
* **The film is the TERRAIN OVERLAY pass** (`EPipelineIndex::TerrainOverlay`, variant 11): the terrain chunks
  overlapping the wetness clipmap drawn a SECOND time over the ground - the main cull (`instanced_indirect.cs.glsl`)
  emits it for `TerrainLit` instances into its OWN list (binding 17, draw count [3]; same instance list, count raised
  with `atomicMax` so every overlapping instance is inside the drawn range). `record()` draws it with
  `drawIndexedIndirectCount` and variant 11 bound normally (it stays in the transparent execution set but is never
  routed there). **Draw order: opaque execute → tessellated ground → grass → clutter → film → sky → transparent
  execute** (ocean, river, blended meshes), so the ocean always blends over the film.
  * **The film has its own FS, `terrain_film.fs.glsl`**; the part it shares with the ground's
    `instanced_indirect_terrain.fs.glsl` (the wetness, the mirror sky, the splat include, `terrainFields`,
    `terrainWetness`) is `terrain_common.inc.glsl`. The pipeline's terrain define loops (lit debug, `LIT_RT_*`,
    `TERRAIN_POM`) match both FS paths (`isTerrainFragment`).
  * Depth test GREATER_OR_EQUAL / write off, `early_fragment_tests`; uncovered pixels discard, DRY pixels EARLY -
    before the layer walk and the relief taps, from the raw wetness's pool level and the live ocean depth at the
    deepest relief (an exact bound). The FS composites DUAL-SOURCE (`PipelineVariant::dualSourceBlend`: out = K +
    ground * factor per channel, the film being linear in the ground colour) - it never reads the scene colour. The
    overlay resolves its own sun visibility: ONE hard shadow tap (one ray with the RT sun), as the ocean.
  * **Why a separate pass:** the film set the terrain FS's register allocation for every pixel, filmed or not.
    **The overlay is meant to grow** (snow, deformation, other surface layers): any layer that is a K + factor composite
    fits as is; deformation would need displaced geometry and depth writes, which it does not do.
  * **The film reflects the SKY only** (the baked mirror sky, fogged). Its scene mirror ray (`terrainFilmMirror`, the
    ocean's ray under the ocean's gates) is DISABLED behind `TERRAIN_FILM_RT_MIRROR` (a `constexpr false` in
    StaticMeshGraphicsPipeline): a ray query sets the film's register allocation for every pixel.
  * **The film's base normal is the LEVEL water plane, not the ground normal** (sun, sky and lights match the ocean on
    sloped ground); it eases to the ground normal as the view flattens onto the plane (`V.y` 0.35 -> 0.05), because a
    hard switch drew a line at eye height.
  * The overlay runs the film in two stages (`terrainFilmSurface`, `terrainFilmShade` -> K + ground factor) with its
    OWN grid-light walk (specular only, full light shapes); the ground's wet gloss is off under it. (Merging the
    film's lights into the lit core's loop was tried and dropped, see "Half floats" ([Shaders.md](Shaders.md)).)
  * **Inland** (above the swash run-up, where the FFT depth weight is 0) the film takes wind ripples: the finest
    cascade keeps a slope weight of its own there ("Terrain/Water/Wind ripples", `u_terrainWater_rippleStrength`) - the
    film loop's existing taps, no extra fetch. The film also carries the ocean's sub-band DETAIL slope (the
    `oceanDetailSlope` math inlined, "Ocean/Shading/Detail ...", one extra tap), its crest foam (`oceanInstantFoam`
    inlined, shore-gated) and its own normal knob ("Normal scale", `u_terrainWater_normalScale`, x the ocean's normal
    strength); no foam inland, and the amplitude follows the ocean wind through the spectrum.
* **THE WETNESS CLIPMAP** (`TerrainWetnessPipeline`, `terrain_wetness.cs.glsl` / `.inc.glsl`; the tick state is
  `TerrainResources::advanceWetness`): ONE persistent R16F image, `TERRAIN_WET_RES`² (1024²) texels of "Texel size (m)",
  stored **toroidally around the scene focus** exactly like the GI probe clipmap (slot = lattice & (RES−1); the CPU
  packs this tick's and the last tick's window origin into `u_terrain_wetOrigin` / `wetPrevOrigin`, and a texel whose
  coord was outside the last window starts dry). The pass decays `exp(−dt/dryTime)` (sharpened on warm ground from the
  map's own climate, "Dry temp sensitivity"), then **sets ground under the LIVE ocean surface to 1** — the same
  calm-depth + swash residual predicate the lit core uses for underwater sunlight, so the wet tongue is where the water
  was drawn, and permanently submerged seabed stays 1 until a drawdown exposes it — plus a uniform rain term and the
  inland water target. GENERAL for life, two layers ping/ponged (the diffusion tent reads neighbours; "Terrain/Water/
  Diffusion on" is the baked `WET_DIFFUSION` define). The TERRAIN fragment shader (binding 18, UPDATE_AFTER_BIND)
  manual-bilinears it (never across the wrap seam) and derives everything above from it. **Disabled = the pass is
  skipped and the presence flag is 0**; re-enabling parks the previous origin out of range so nothing stale shows.
  * **The pass runs on a FIXED TICK** ("Terrain/Water/Update rate (Hz)", default 20), not per frame: the UBO build
    accumulates the sim delta and ticks when it reaches the interval, integrating the whole accumulated delta at once;
    the primary executes the pass (before the forward pass) on tick frames only (`isWetnessTicking`). **Why:** the
    image is R16F, whose step is ~0.0005 at wetness 0.5 — a per-frame change at 165 fps is below half a step and ROUNDS
    AWAY in `imageStore`, so rain and drying stalled and wetness depended on the framerate. A tick writes the ping/pong
    layer the last tick did not (`m_wetLayer`, NOT the frame-slot parity); between ticks the UBO keeps naming that
    layer and the last tick's window origin, so the reader stays consistent with the data. The origin therefore
    scrolls per tick.

## Rain occlusion map

**`setRainOcclusionVolume`** — the RAIN OCCLUSION MAP for the weather particle volumes (`PARTICLE_FLAG_OCCLUDE`, see
[Particle](../Particle/CONTEXT.md)): ONE top-down orthographic view (`u_weather_rainOcclusionViewProj`, a plain matrix),
**RAY-TRACED** (`RainOcclusionPipeline`, `rain_occlusion.cs.glsl`) into ONE R32_UINT image (`RAIN_OCCLUSION_RESOLUTION`²
= 512², 1 MB, always GENERAL).

* Per texel, two ray queries from the near plane straight down against THIS frame's TLAS: SOLID (CullNoOpaque) - the
  closest opaque hit's ortho depth (t / range, standard Z; a miss = 1); FOLIAGE (CullOpaque, up to that hit, candidates
  never confirmed) - the alpha-masked instances' top depth and layer count, each layer stopping "Particles/Rain
  occlusion foliage block" (0.7) of the rain (no texture alpha test; in the TLAS a tree is its whole-tree billboard).
  Packed: bits 0..15 solid depth, 16..27 top foliage depth (1 = none), 28..31 the pass fraction ×15.
* The sim (`rainSheltered`, usampler2D, nearest, its own bounds check = open sky outside) shelters a drop below the
  solid surface, and one below the foliage top whose FIXED draw (a hash of its seed, which changes only at a restart -
  no compounding over frames) is at or above the pass fraction.
* **ONE image for both frame slots**: the trace's opening barrier (src = compute reads) covers every earlier read on
  the queue, the previous frame's sim included.
* **Allocated only while active**: `present` switches it (GPU idle, re-record) when this frame's UBO asks for the map
  (a rain / snow volume with `Occlude`, "Particles/Rain occlusion" on, RT on); the compute pipeline is built on the
  first activation; inactive = a 1×1 open-sky placeholder. Bushes (no BLAS) and anything outside "RT/TLAS Range" /
  "Trees/RT range (m)" do not shelter. **Do not go back to a raster depth pass**: a second shadow cull + depth pipeline
  sized its cull output for the WHOLE instance capacity (~400 MB at 4M instances) for a map a few metres around the
  camera.
* It runs in the primary right after the GI step (the TLAS build's barrier covers compute), and the PARTICLE SIM runs
  behind it (`recordRainAndParticleSim`, both primaries), so the sim samples THIS frame's map (binding 10). Needs RT:
  with "RT/Enable RT" off the UBO turns the shelter test off.
* The particle system requests the box every frame (main thread after the begin-frame join, the scene-focus pattern);
  `present` latches it for the NEXT frame's `buildUboRainOcclusion` (eye "Rain occlusion pad" m above the box top,
  footprint padded 25 % for the one-frame lag), and the primary executes the pass only when the UBO it was built with
  says so (`u_weather_rainOcclusionPresent`), so the pass and the sim's test never disagree. No request or "Particles/
  Rain occlusion" off = the params are zero and the sim's shelter test is off.
* Tweaks under "Particles": Rain occlusion (on by default), Rain occlusion pad, Rain occlusion bias (the depth below
  the surface that counts as sheltered), Rain occlusion foliage block.
