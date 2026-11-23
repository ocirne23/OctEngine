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
  re-resolved through the same includer (so a new file that shadows an include also misses). The generated
  `ubo.generated.glsl` is checked through the UBO entries the shader's files name only (the per-shader reload in
  [FrameUbo.md](FrameUbo.md)), so a hit may embed an older declaration text in its debug info for entries it never
  reads. A changed file
  overwrites its entry; only a new define set adds a file. **Bump `SPV_CACHE_VERSION` when the `SpvOptions`, the
  target env, the entry format or the form `buildUboDeclaration` writes an entry in change.** A cache hit writes no `Local/Shaders/*.spv` dump. Delete
  `Local/ShaderCache/` to force a full recompile.
* **Per-pixel work hoisted to the UBO / per pixel:**
  * `u_sunTransmittance` is the CPU mirror of `atmosTransmittanceToLight(0, sun, up)` (`sunTransmittance` over
    `SkyAtmosphere`, RendererUbo.cpp; **keep it in sync with atmosphere.inc.glsl**) — no shader runs the Chapman
    function per pixel or per ray for it. The sky's multiple-scattering light is the same kind of value
    (`u_sky_multiScatterLow` / `High`, `buildUboSky`; "The atmosphere" in [Lighting.md](Lighting.md)).
    It is x the sun disc's VISIBLE share above the horizon (a smooth step over +- the disc radius, "Sun Angle Cos"):
    the Chapman form gives a large but finite depth below the horizon, and a dim red sun lit the world after sunset
    (an ocean glint where the sun would be). The clouds' own sun terms do not use it (they still glow after sunset).
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
  On a TREE record its three "last frame" fields carry the tree's wind ([Vegetation.md](Vegetation.md) "Tree wind"),
  and the mesh index's 16 bits its TYPE in the tree set ([Scene.md](Scene.md) "BAKED TREE RECORDS"). The LitTree
  fragment shader reads the record too (its first two fields, at the draw slot its VS hands on).
  They index it by `gl_InstanceIndex` (the cull writes each instance at its draw slot; "Draw-list compaction" in
  [Scene.md](Scene.md)).
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
* **Only `EPipelineIndex::LitMasked` / `LitTree` / `LitTreeVoxel` discard** among the static-mesh variants
  that write depth (the lit fragment compiled with `ALPHA_MASK`, instanced_indirect_tree.fs.glsl
  and instanced_indirect_tree_voxel.fs.glsl - the last also writes `gl_FragDepth`; the
  clutter's textured flowers have their own pipeline for the same reason). A
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
* **ONE light shadow trace** (`lightShadowVisibility`, punctual_lights.inc.glsl): the light kinds differ only in the
  point their ray aims at (`areaLightShadowTarget`, `beamLightShadowTarget`, the light's position), so they share one
  `traceLightVisibility` call. Each call site inlines the whole trace (the terrain march + the ray query with its
  alpha-test loop): three sites were three copies, ~27 KB of every lit fragment shader (lit FS 107 -> 80 KB).
  `PL_LIGHT_TRACE_TWO_SITES` (an includer's define, by its pipeline stats) keeps two sites - the tessellated ground: 72
  registers / 32 B with one site, 64 / 48 with two.
* **The lit core's includer defines** (instanced_indirect_lit.inc.glsl), each compiling a path OUT for a material that
  never takes it:
  * `LIT_NO_RTAO` - no RTAO read: the grass, the clutter and the tree voxels (a runtime flag kept the whole
    upsample compiled).
  * `LIT_NO_UNDERWATER` - no water-level fetch, no underwater sun at all: the tree leaves and voxels, the grass.
  * `LIT_LIGHTS_ROUGHNESS` (the includer's roughness as an expression of frame constants) - the light loop forms the
    view vector (from `u_viewPos`) and reads the roughness again per light, so neither is live across a light's
    shadow ray: the grass FS, 64 / 16 -> 56 / 32.
  * `LIT_CAUSTICS` (the one define that compiles a path IN) - the underwater CAUSTIC pattern
    (`underwaterSunTransmittance`): the terrain ground only. Every other material under the water takes the
    Beer-Lambert absorption alone (`underwaterSunAbsorption`, no ocean-map taps). The pattern is the surface's fold
    field projected down the sun's direction with no occlusion, which is right on the open seabed and wrong on a mesh.
  * `LIT_SNOW` (compiles a path IN; needs `SUN_SHADOW_FIRST`) - the snow's sun BRDF (`snow_brdf.inc.glsl`) as doSunLight's
    ONE BRDF and its facing exception, on the pixels `snowLitSetup` gives a snow share: the terrain ground, the rocks, the
    caps, the lit, tree and voxel FS (see The look in [Snow.md](Snow.md)). Without a call its three globals keep their
    constants (the blended lit variant). `SNOW_NO_GLINTS` (snow.inc.glsl) compiles the glints out (the voxel crowns).
    **Its register rules, each measured:** nothing of the snow may live across the AO or the light loop (the bright
    cavities' share across the AO cost the lit and tree FS 16 B, in every form); the diffuse is worked out BEFORE
    computeLitColor and enters as one half scalar (in doSunLight, or as a colour, it cost the lit FS 16 B); a branch of a
    second BRDF beside doLightH compiles both.
  * `LIT_DIELECTRIC` - no metalness: the specular colour is the ONE F0 scalar (`LIT_SPECULAR_F0`, default 0.04) across
    the light loop, the diffuse takes no (1 - metalness). The two tree shaders.
  * `LIT_LIGHTS_DIFFUSE` - the local lights' diffuse only (`doLightDiffuse`: the same radiance, falloff, cone, one-sided
    test and horizon fade per light kind; no view vector, no Fresnel, no GGX): the tree voxels, rough
    surfaces seen from tens of metres. The sun keeps the full BRDF. Voxel FS 64/32 -> 64/16.
  * `LIT_SUN_SHADOW_HARD` - the sun's cascade shadow as ONE hardware-PCF tap (`sampleSunShadowHard`, the ocean's form;
    not on the RT sun path): the tree voxels, whose pixels are stochastic coverage. Voxel FS 64/0 -> 56/32, 10 KB less
    code; the gain in time is small (~0.1 ms of a forest view: the PCSS search is not what a voxel pixel pays for).
* **`u_sunLit`** (a live UBO value, `registerUboValues`): 1 while the sun's radiance at the ground is above 0, 0 below
  the horizon (`u_sunTransmittance` is 0 there) and in a full eclipse - worked out once per frame on the CPU. Every
  reader of `sunShadowVisibility` multiplies it by that radiance, so the shaders that take their shadow with NO facing
  test gate the search by `u_sunLit > 0.5` at their call site: the tree leaves, the voxels (and the
  voxels' sun march), the flowers. At night each of their pixels paid the whole search for a light of 0. **Not inside
  `sunShadowVisibility`**: there the grass FS went from 56 / 32 B to 64 / 0, and every lit binary changed; **not at the
  grass's own call either** (56 / 32 B -> 64 / 16 B). The two SHADOW CULLS (instanced_indirect_shadow.cs.glsl, the far
  casters' tree_voxel_far.cs.glsl) emit no caster while it is 0: the shadow pass still runs and clears its maps (their
  layouts stay valid for the shaders that sample them x a radiance of 0), at 0.03 ms. A forest view at night: the GPU
  frame 9.8 -> 7.4 ms with all of it.
* **The tree fragment shaders hold nothing for the output across the lit core**: LitTree writes alpha 1 (no
  blending: the texture's alpha and the material's opacity were two values live to the end, across the light loop),
  and the voxel FS forms its depth and motion vector AFTER the lighting, from the position.
* **The tree VS variants** (instanced_indirect.vs.glsl with `TREE_ORIGIN` / `TREE_VOXEL`): `TREE_STATIC` -
  a tree piece never moves and is never skinned, so no previous-transform work, the motion is the wind's alone;
  `TREE_WIND_NO_FLUTTER` (the voxel box: no leaf-card TIP vertex); the voxel box passes payload 0 as a
  constant (the trunk bend only) and writes no normal / tangent. A tree's whole wind comes per TREE, not per vertex
  ([Vegetation.md](Vegetation.md) "Tree wind"). `TREE_ORIGIN` and `TREE_VOXEL` hand on the tree's DRAW SLOT
  (one flat uint) and nothing else per tree: see the next rule. `TREE_VOXEL` (the voxel march pass's near pipeline)
  draws no surface at all: a screen rectangle around the tree's box ([Scene.md](Scene.md) "THE VOXEL TIER").
* **What is the same for a whole TREE is a load in the fragment shader, not a flat interpolant** (each interpolated
  component is a plane equation per triangle, and the TRAM allocation is the tree ranges' launch limit). The fragment
  reads its tree's instance record at the draw slot (the origin, the rotation, the scale) and its type's entry of the
  tree set's table (the voxel grid's box), each where it is used; every pixel of the tree shares those loads. The
  vertex shader fetches nothing per tree. LitTree FS 34 -> 27 inputs at the same 64 / 16 B, its VS 44 -> 40 registers
  and 11.3 -> 8.7 KB ([Scene.md](Scene.md) "THE MESH LEAVES' SUN SELF-SHADOW"); the voxel tier likewise ("THE VOXEL
  TIER"). **A `flat` input costs the TRAM what an interpolated one does** (measured on that tier: 13 flat components
  out, its TRAM launch stall 85 -> 53 %). The far voxel trees have no draw slot, but their record is a buffer too
  (their sorted list, the set's binding 26): both voxel tiers hand on ONE flat word, the tree.
* **The TRAM pays per TRIANGLE: a proxy that only carries a ray is a screen rectangle, not a box.** The near voxel
  tier drew its grid's box (18 to 34 front triangles a tree) for the entry point alone; it draws 2 triangles and its
  fragment shader builds the pixel's ray from `gl_FragCoord` and intersects it with the box (FS 30 inputs -> 1; TRAM
  stall 54 -> 3 %). More triangles to save pixels lost: a box of 9 rings fitted to the tree raised that tier's TRAM
  stall from 54 to 75 %. The far voxel trees and the voxel shadow caster draw the same 2 triangles (the caster's
  rectangle lies in its cascade's clip space).
* **A march and a lit core in one fragment shader: split them.** The voxel tier's combined shader ran its whole march at
  the lit core's 64 registers (its register stall was 93 % once the TRAM was out of the way; warp occupancy x registers
  was constant across its versions), a ray that hit nothing paid them too, and a tree behind another one was lit and
  then covered. The MARCH is its own shader in its own pass (`Trees/tree_voxel_march.fs.glsl`, 40 / 0: depth + a hit
  word into an RG32UI target), the LIGHTING one full-screen draw over that target (64 / 0, once per final pixel).
  **The two shaders must agree on the hit to the bit**: one include builds the ray for both from the pixel and the
  tree's record, with `precise` values (`Trees/tree_voxel_ray.inc.glsl`); nothing interpolated is in it.
* **An affine proxy needs no loop over its corners.** The voxel shadow caster's rectangle (an orthographic cascade, a
  box with a shear) comes from the box's corner and three edges in clip space; the inverse of those edges maps clip
  space to the grid. With 8 projected corners and the cascade matrix's inverse its VS was 96 registers / 59 KB; it is
  54 / 39 KB.
* **A world position rebuilt in a lit-core fragment costs registers, whatever it is rebuilt from** (the clutter: from
  its camera-relative interpolant, 64 / 16 B -> 72 / 32 B; the voxel tier's box version: from its grid coordinate and
  the tree's record, 56 / 32 B -> 64 / 0). It stays an interpolant where there is one (the voxel lighting has none: a
  full-screen draw, 64 / 0). A value read only AFTER the lit core (the voxel tier's motion) is free to work out there.
* **Statement order sets a vertex shader's registers.** In instanced_indirect.vs.glsl the wind is worked out BEFORE the
  vertex's own transform: the transformed position and normal held across the wind were 15 registers of the generic
  lit VS (63 -> 48; unlit 62 -> 48, tree 48 -> 40). Same rule in a helper: `treeWindOfTree` evaluates the gusts first
  and returns no early zero, so nothing of the tree is held across them (the main cull 53 -> 50). And inside
  `treeWindVertex`: the vertex's own sway (`treeWindSway`) first, the trunk's part last. The sway output for the shadow
  lookup (`out_windSway`) as "offset minus the trunk's part" cost the tree VS one register more than this order; the
  unlit VS is 46 and 30.
* **The voxel tier's two vertex shaders hold no wind path at all** (ALU, not registers: binary size is the measure). The
  near tier reads the trunk's vector from its tree's record (no record test, no per-vertex `treeWindOfTree`
  fallback) and projects the box's corners for its rectangle (16 projections for 4 vertices a tree: 80 registers,
  11.5 KB - a vertex stage that is 0.2 % of the warps). The far voxel
  trees have no wind at all (they stand still): 8 projections, 48 registers, 5.1 KB, and their cull works out no
  trunk vector. Neither works out a value that is one per tree: the fragment shaders build those from the tree's
  record and the type's box.
* **A march whose loop sets the shader's register count reads its ray from the interpolants at each step.** An
  interpolant costs no register between two reads; a start point and a direction worked out before the loop are held
  across it. The voxel shadow caster (`shadow_depth.fs.glsl`, 40 registers) scales `in_voxelGrid + in_voxelDirG * t` to
  voxel coordinates inside the loop and tests the box on the sample; held as start + direction + exit distance it took
  48. Its slab test for the ray's entry sits BEFORE the loop and leaves one float (`t`) behind: still 40. Where another part sets the count (the voxel lighting's sun march: its light loop), hold them - fewer instructions a step.
* **Find what holds the count by stubbing, one part at a time** (the pipeline statistics, ~30 s a try): the voxel march
  shader was 48 with its loop stubbed AND with its ray setup's extra passes stubbed - two separate peaks, each to cut.
  **A second pass over the same inputs costs registers even when the passes run one after the other**: the inputs
  stay live across the first pass's temporaries (three slab tests of one ray: 48; one: 40 - leaner passes did not
  help, only fewer). A loop counter beside a distance limit, an end value beside the running one, and a value the
  compiler shares with a later use (the pixel's direction, used again after the loop) are each a register across the loop.
* **Do not factor shared terms out by hand.** The driver's compiler already shares a subexpression across inlined
  calls. Tried on the atmosphere (a march step's density and its two optical depths use the same two exponentials, the
  same radius and the same two square roots - one `AtmosPoint` struct for all three): the binaries shrank 2 %, the time
  did not, and holding the struct across the step RAISED the register counts (sky FS 56 -> 64, cloud_sky 64 -> 72) and
  slowed the cloud march 0.33 -> 0.36 ms. What pays is less WORK: an earlier test (the clutter cull's climate fit), a
  cull by what the view can see (the far shadow casters), fewer waits (the next rule).
* **The atmosphere marches** (atmosphere.inc.glsl `atmosMarch`, the aerial LUT, `cloudAerialScatter`), each measured:
  * **Two calls under an `if` are both worked out** (flattened). `atmosRayOD` is ONE Chapman evaluation with its
    direction and sign selected, and `AtmosRay` holds two constants, not four: aerial_lut 73 -> 64 registers, every
    sky binary 8 - 30 % smaller. A ray that only rises takes `atmosRayBeginRising` (one evaluation at the origin, not
    three).
  * **A term that needs a SECOND vector across the widest point of a step costs a register step.** The
    multiple-scattering light in the saturated sun march (the saturation needs the once-scattered light apart): sky
    FS 64. On the unsaturated sky-light march it adds into the same vector: 56.
  * **The light's transmittance comes FIRST in a step** (`atmosMarch`: its Chapman evaluation, then the ozone
    lengths, then the `exp`), before the sample's density and 1 / extinction. With the ozone length as one float worked
    out first and held across the rest of the step the sky FS was 64; in this order it is 56. The ozone's view part
    takes the ray's origin from `tLow` (`atmosOzoneSegment`), which the march holds already: the whole ray's ozone as a
    value of its own across the loop was gi_sky_map 56 -> 64.
  * **A light at the zenith needs no Chapman evaluation** (`atmosZenithOpticalDepth`: scale height x density, the
    exponentials are `atmosDensity`'s own). The aerial LUT's sky-radiance light: 63 -> 52 registers, and a third of the
    step's evaluations gone. Within 40 km of the camera the light's `up` is under 0.4 degrees from the sample's own
    zenith: under 0.2 % in the optical depth.
  * **A value from before the loop goes into one the loop holds already.** The aerial LUT's terrain shadow is marched
    FIRST (before the ray's atmosphere constants exist) and multiplied into the sun's two phases: 65 -> 63. In that
    shader the step's order is measured too, and the plain one is the lowest (52): the sun's optical depth first 54, the
    cloud shadow first 53, the optical depths added per species before `atmosTau` 53 - 55, the cloud shadow removed 61.
  * **Where the colour goes is not predictable: measure both.** `atmosMarch` colours its light IN the step (one sum:
    56; a sum per species, coloured after: 72). `cloudAerialScatter` is the opposite (sums per light and species,
    coloured after: 64; the colours in the step: 72 - the compiler held every colour x phase product across it).
* **A march that waits for each fetch takes TWO samples a wait.** Where the next sample's place does not depend on this
  one's result (only the march's END does), both taps go out together and are taken in turn: the same samples, about
  half the waits. The tree voxels' view and sun marches (`voxelOpacityPair`, tree_voxel.inc.glsl; "TWO STEPS A WAIT" in
  [Scene.md](Scene.md)). Four a wait was slower than two (about as slow as one): every tap past the end is wasted, and a march ends often.
* **The default static-mesh VS packs its interpolants into 4 locations**: `posU` (xyz + uv.x), `normalV` (xyz + uv.y),
  `tangent` (xyz + bitangent sign), flat `meshIdxMaterialIdx` at 3. The fragment shader rebuilds the bitangent. Every
  FS paired with that VS (lit, unlit, the gizmos, sky) uses this layout; the terrain, rock, ocean and river VS have
  their own. Beside them: `prevWorldDelta` at 4 (the motion vectors). LitTree adds three: its draw slot at 5
  (flat), the vertex's voxel-grid coordinate at 6 and `windSway` at 10 (the sun shadow's lookup before the wind's
  sway; Scene.md).
* **An interface on small triangles counts its components** (each one is a plane equation per triangle; the launch
  stall to read is the TRAM allocation). Two ways that cost no register: what only ONE kind of a shader's surfaces
  reads shares its slots with the others' (the clutter: a pebble's climate, a mushroom's local position and the other
  kinds' pattern coordinates in the same components - 28 -> 23 inputs), and flat values of a few bits pack into one
  word. **Not the world position**: rebuilt from another interpolant (the clutter's camera-relative position + the
  camera) the lit core held it across its light loop, 64 / 16 B -> 72 / 32 B.
* **The light grid build is split CPU / GPU, and the CPU part is NOT on the main thread**
  (`LightGridComputePipeline`):
  * **Inline in `addLightInfo`, on the adding thread (`addLight`):** the light's per-type bounds (point/spot sphere,
    spot cone sector, area front-half box, beam capsule) → its cull record; every 64^3 grid the box covers is claimed
    in a lock-free CPU hash table (the same `getPositionHash` as the shaders, bit for bit; bump-allocated slots, CAS on
    the table entry, a slot that loses the race for a grid stays DEAD with `cellSize 0`), the grid's distance-LOD cell
    size is picked at the claim, the grid's cell/large count is bumped (`atomic_ref`), and one (slot, light) touch is
    appended to a bounded array (one `fetch_add` per light for its whole block). A light with a reach over
    `GRID_SIZE / 2` (32 m) or spanning more than **"LOD/Light grid/Per-cell budget (cells)"** (32768: a whole 2 m grid,
    so by default only the reach decides) cells inside a grid is that grid's LARGE light (one entry evaluated by EVERY
    pixel of the grid, far from it too; `MAX_LARGE_LIGHTS_PER_GRID` 30 a grid, the rest DROPPED); any other is a per-cell
    light, evaluated only in the cells its range touches (`MAX_LIGHTCELL_LIGHTS` 24 a cell, the rest dropped). A light
    whose block or grid claim does not fit goes to an overflow list; the touch array grows for the next frame.
  * **THE SIZES FOLLOW THE GAME'S LIGHTS** (reaches of 4 - 46 m, most 8 - 25 m: unit hurt flashes 4 - 8, turret and
    cannon flashes 8 - 18, the cannon shell 14, the staff cone 18 - 22, field lights 2 x the field radius, the
    player 35, the Searchlight 46). Tried 32 m grids with 1 m cells and a 1024-cell budget: every light over ~5 m was
    LARGE, so a grid in a fight passed its 14 large-light slots and lights went dark at random.
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
    world units per cell (`dist` = to the grid's CENTRE). Min == max pins one resolution everywhere; the defaults
    (48 m, 32 m, 0.5, min 1, max 3): 2 m cells (32^3, 1.7 MB) in the grid the camera is in - its centre is up to 55 m
    away - then 4 m and 8 m. The tweak's minimum is 1 (2 m): 1 m cells make a 64 m grid 13.6 MB.
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
  sun visibility, 4 shadow-map texel size heat (green 5 cm → red 1 m), PCSS path only. 5 the terrain march's steps, on both
  paths (`terrainMarchDebugColor`, see [Terrain](Terrain.md)). `~GameMatch`'s shadow-preset restore leaves this field alone
  for the same reason.
* **"LOD/Light grid/Debug mode"** (`LightGridParams::debugMode`, a named list) is BAKED as the `LIGHT_GRID_DEBUG` define
  on the same lit fragments (its own listener; at Off every debug branch folds away) and overlays the forward pass's
  `computeLitColor` (`instanced_indirect_lit.inc.glsl`): Grids (a random colour per 64 m grid), Light count heat (the
  lights a point evaluates, its grid's LARGE lights included: green → red at the cell cap of 24, magenta = the point's
  grid is missing from the hash table), Light ranges (a step of blue per covering light). Not saved.
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

**Current numbers** (regs / local B per thread, RelWithDebInfo): lit FS 64/32 (all variants; its VS 48/0, the unlit
VS 30/0 and 46/0), tree FS 64/16
(its VS 40/0), the voxel trees' march FS 40/0 (its VS 80/0, the far trees' 48/0) and lighting FS 56/48 (the driver's
pick: its snow code out, it stays 56/48; the voxel shadow caster FS 40/0), the shadow VS 31/0 (the far casters 40/0, the
voxel pipeline 54/0), tree_volume_march 64/0 (handover 64/0),
tree_volume_splat 37..56/0, tree_voxel_far 33/0 (the shadow list 35/0), the main
cull 46/0, the shadow cull 39/0, grass FS 56/32 (the far rings' simple variant 56/16), grass VS 56/0 (53 before the blades' snow: the same 8-register allocation granule; the near casters 44/0), grass cull 48/0, clutter / flower FS
64/16, flower VS 70/0 (textured 72/0: 68 before the flowers' snow, the same allocation granule), terrain ground FS 64/32, tessellated ground 72/32 (64/64 with the snow state and
the wet-snow reflection out: the driver trades), its TCS 46/0 and TES 47/0, film 64/32, ocean 96/64, river FS 96/48, rock
64/16 (its VS 34/0), the snow caps FS 64/0 and VS 48/0, snow_cap_prepare 48/0, snow_deform 40/0, terrain_wetness 38/0,
sky.fs 56, aerial_lut 52, particle_sim 56, particle VS 56, rtao 54/0, vol_scatter 56/0,
vol_apply FS 64/16, cloud_march 72/0, cloud_sky 64/0, cloud_shadow 38/0 (54/0 with the
vortex on), cloud_temporal 48/0, gi_sky_map 56/0, gi_probe_trace 128/0, gi_volume_bake 55/0, clutter_cull 80/0,
decal 53/0, force_union FS 72/48 (8 teams; 56/32 with 2). The driver moves its
register/spill split by +-16 B on neutral changes, so re-measure before trusting a 16 B difference.

* **The rules:**
  * **A half sum that can CANCEL needs a floor before it is normalized**: `normalize(L + V)` at V = -L (looking at the
    light through the surface) underflows its squared length to 0 and gives inf / NaN, and NaN x a zero light term is
    still NaN - a bright point at the sun's position on the ocean after sunset. `halfVectorH` (punctual_lights.inc.glsl)
    floors it at 1e-4 IN HALF (an fp32 sum there moved the grass FS from 64/16 to 56/48) for the SUN's BRDF
    (`doLightH`, `doLightSplitH`); the ocean takes its fp32 floored form (no register change there). NOT in the area
    lights' specular (`doAreaLightSpecularH`, the light loop - the register peak): there it took the lit and tree
    FS from 64 to 72 registers, and V = -L against an area light is a rare view.
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
    vol_scatter's single loop (`volLightScatter` inlines a ray query) cut its registers. The GI trace's
    `giGatherDirect` (lighting.inc.glsl) likewise: 104 -> 89 KB, its 128 registers and its time as before.
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
* **The ocean spectrum / FFT images are RGBA16F** (`SPECTRUM_FORMAT`): the passes are memory-bound, so half storage
  pays here. The butterflies stay fp32 in shared memory.
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
  * gi_probe_trace's light loop without the terrain march: still 128/0, only 12 KB less code.
  * The film's lights in the lit core's loop (one loop, one shadow ray per light for both lobes; less code): 80/64. The
    film surface must then be resolved BEFORE that loop and its values stay live across the shadow ray query; with the
    film surface after the loop it spilled worse. Same register count as the film's own loop, so no occupancy gain, and
    the spill cost lands on EVERY terrain pixel while the saving is only on filmed ones.
  * The film's lights before its mirror (N / V then live across the lights' own ray-query peak).
  * The AO upsample's tap arrays as index bits (the lit FS got worse).
  * A compact half LightInfo (the compiler loads light fields at use); a D16 shadow map FOR SPEED (geometry-bound, D32 is
    compressed) - it is D16 now for its MEMORY (Lighting.md).
  * **Not worth it:** the GI probe buffer (the fp32 temporal HISTORY - fp16 would round small blend steps away),
    BakedWorldMap (heights), vertex attributes.
