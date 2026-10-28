# The frame UBO, tweak locks and push blocks

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## The frame UBO and the tweak locks

ONE uniform buffer per frame slot (`RendererVKLayout::UBO_RANGE` bytes; every descriptor binds the whole range), built
by `buildFrameUbo` in the begin-frame job and uploaded whole. **No C++ struct mirrors it: its layout is REGISTERED**
(`UboBlock`, `UboBlock.ixx`) - each member a name, a type and an std140 offset in registration order.

**EVERY VALUE IS ONE LINE: its GLSL name, a lambda, and its SOURCES** - the sources alone decide live or lockable:
```cpp
list.add("fog_density", f.density);                                                  // a tweak as it is: lockable
list.add("fog_albedo", [&] { return f.albedo * f.albedoIntensity; }, f.albedo, f.albedoIntensity); // lockable
list.add("sunColor", [&s] { return s.sunColor * s.sunIntensity; }, UboLive);        // reads no-tweak state: live
list.addArray("views_mvp", NUM_UBO_VIEWS, [this](uint32 v) { return m_views[v].mvp; }, UboLive);
```
**Moving a value between live and baked is editing its sources** - `UboLive` for the tweaks it reads, or back. Its name
and its place stay: neither says live or lockable.

* **ONE function, by subject:** `Renderer::registerUboValues` (the end of `RendererUbo.cpp`) has a section per subject
  (the frame, Sky, Clouds, Shadows, Ray tracing, Fog, Ocean, Terrain, Terrain textures / tessellation / water, Rivers, Grass,
  Clutter, Trees, Rocks, LOD, Force, Particles, Weather, Post, present), and each section holds its subject's live and
  lockable values side by side, all named `<subject>_<field>` (`fog_density` lockable, `fog_waveBand` live).
* **LIVE** = everything that also reads the camera, the clock, the sun, the wind, a readback or a value the outside
  pushes per frame: the frame section (the views `u_views_mvp[3]` ... `u_views_viewPos[3]`, one array per view matrix,
  read through `ubo.inc.glsl`'s `u_mvp` / `u_viewPos` macros or `u_views_*[VIEW_CENTER]` - among them
  `u_views_rayFromNdc`, THE VIEW RAY: the inverse of `mvp`'s x / y / w rows, from the stored floats, in double
  (`writeView`), read as `normalize(vec3(ndc, 1) * u_rayFromNdc)`, plus `u_viewForward` in its column 3 - **use it
  instead of inverting the mvp per pixel**; the frustum, the screen, the time), the sun, the sun cascades, the terrain's
  streamer / splat / wetness state, the weather, and the per-frame values in the other sections.
* **LOCKABLE** = computed from tweaks alone, sources = EVERY settings variable the lambda reads.
* **THE FRAME STATE the lambdas read** comes from the `buildUbo*` steps that `buildFrameUbo` runs first - only what has
  a side effect, carries over from the last frame or comes out of one calculation with several results: `m_views`
  (prev mvps), `m_taaJitter`, `m_cameraVelocity`, `m_rainOcclusion`, `m_giFullBake` / `m_giUboPrevFocus`, the cloud
  wind / evolve offsets and shadow recentre (`m_cloudShadow*`), `m_sunCascadeViewProj` + `m_cascadeSunSizeTexels`, the
  force bake box (`m_forceBake*`), `m_wetTick`, `m_grassNear`, the foam field (`OceanSimulationPipeline::advanceFoamField`).
  Everything else a lambda computes itself from the params and getters. Then `evaluate()` runs every lambda once, in
  registration order (~600 virtual calls). CPU code that needs one of these values reads the frame state, never the
  block (`m_views[VIEW_CENTER].reprojClip`, `m_taaJitter`, `m_rainOcclusion`, `m_uboFrameIndex`).
* **The GLSL type comes from the lambda's return type** (`float`, `vec2/3/4`, `uint32` -> `uint`, `uvec4`, `mat4`;
  `bool` / `int` -> `float`, the way the shaders compare them). std140 packing in registration order; an array's element
  is a 16-byte multiple (vec4, uvec4, mat4). A lambda captures `[&]` over local references to members, or `this` -
  never a local value. A value two lambdas need comes from one helper (the anonymous namespace at the top of
  `RendererUbo.cpp`, or a Renderer member like `grassPatchSize` / `getOceanSwashAmp`), so they cannot drift.
* **The GLSL block is FLAT**: shaders read `u_<subject>_<field>` (`u_fog_density`, `u_fog_waveBand`), never a
  struct member - so ONE value can be a constant while its neighbours stay block reads. Every member carries its byte
  offset as an explicit `layout(offset = N)`; there is no struct type at all. `buildUboDeclaration`
  (`UboDeclaration.cpp`) makes the GLSL from the block's entries; the shader includer serves that text as
  **`ubo.generated.glsl`** (no file), which `ubo.inc.glsl` includes. A copy for reading,
  `Assets/Local/Shaders/ubo.generated.glsl`, is written only by the first shader that really compiles after the
  declaration changed (a SPIR-V cache miss; `g_uboDeclarationCopyStale`), so a run served from the cache writes none.
* **THE CPU GUARANTEES VALID VALUES; SHADERS DO NOT RE-GUARD THEM.** A lambda (or a push fill / lockable) returns unit
  directions (`u_sunDirection`, `u_skyUp`, the wind), floors and clamps (also the ones a tweak range already implies:
  the panel can type past a range), ordered start/end pairs, and DERIVED values for a guarded form (`<subject>_inv<X>`
  reciprocals, `force_junctionInvK`) when 0 stays legal for the raw value's other readers. **A numeric tweak whose
  default is above 0 has a min above 0**, and its lambda floors at that min - the GPU has no "0 = off / unlimited" branch
  for it. Explicit toggles (bools, mode enums) keep their branches.
  No GPU `normalize` / `max(u_x, eps)` / bound on a UBO or push value; a wrong value may hard-crash the GPU.
* **`u_present_*`** is what only `present()` knows (the tree range, the TLAS live count): source `UboPresent`, registered
  together (contiguous), skipped by `evaluate()`; present() runs `evaluatePresent()` and uploads that byte range alone
  (`presentBegin` / `presentSize`) after the begin-frame upload.
* **Push constants** keep what sizes, offsets or gates a dispatch (extents, view index, `mbEnabled`, buffer
  addresses). Tweak values ride the UBO so a lock can bake them (RTAO's, TAA's, the motion blur's, the composite's and
  the far-tree march's shading among them). A tweak value that must stay a push constant is a LOCKABLE PUSH VALUE
  (below): the eye-adaptation histogram's bloom threshold / knee / exposure rule, the rain occlusion's foliage block,
  the GI probe debug's radius / mode (passes that bind no frame UBO), and the far-tree volume's geometry and bake
  settings (a bake reads the settings it STARTED with over many frames; the march the shown bake's - not the frame's).

## The locks (`RendererUboBake.cpp`)

**THE UNIT IS ONE VALUE, THE LOCK IS ONE TWEAK.** Settings' `TweakLock` (`Settings.Tweaks`) gives every tweak row its
own locked state; `c_uboLockSections` (Layout.ixx) only names the lock SECTIONS whose rows can be locked ("Ray tracing"
covers RT, RTAO and GI, "Post" covers TAA and Post). **Only a row that is a SOURCE of a UBO entry or of a push value is
lockable** (`registerUboLocks` calls `TweakRegistry::markLockable` for every source): the TweakPanel draws an `L` /
`U` button in front of each such row (a locked row is read-only) and a section toggle on the fold ("Lock" / "Partly
locked" / "Locked") that sets all of them. A row in a section that feeds nothing baked (a CPU setting, a resource
size, a define with its own listener) has no button and is never read-only. `ETweakFlags::Runtime` rows have no lock
(the sun direction / colour, the ambient, the up axis, the wind, the "Sky/Sun cycle" tweaks - which keep `sky_radiance`
live).

* **An entry's SOURCES are addresses** (its `list.add` arguments after the lambda), and every one is a
  `Globals::settings` member: Settings resolves it to the tweak row whose variable holds it
  (`TweakRegistry::sourceState`). **The entries read the settings directly, never a pushed copy**: the ocean through
  `oceanWorldScaled(settings.ocean)` (Settings.Ocean, the one world-scale conversion OceanGenerator also pushes
  with), the terrain textures / wetness from `settings.terrain` (`terrainWetTexelSize`), the force field from
  `settings.force`. GI's private tweaks: `GIProbePipeline::registerUboFields`.
* **An entry is baked while every source is locked.** A row no lock covers counts as locked (nothing unlocks it: a
  change just re-bakes); a Runtime row or an unlocked row makes it live. A source that resolves to no registered tweak
  logs a warning and counts as locked.
* **Baked = the value is a GLSL `const`** in every shader (the block keeps its slot as `u_<name>Baked`, unread), so
  the compiler folds it: dead branches go, loops over constant counts unroll (`ao_spatial`'s radius, the composite's
  gather samples, the march steps), a single tonemapper path stays. Unbaked = the block member again. A change
  rebuilds ONLY the pipelines whose shaders name a changed entry (below).
* **EVENT-DRIVEN, nothing is polled.** `registerUboLocks` (before `initPipelines`) registers the locks, the
  fields, and bakes ONCE, so the first compile is already baked. After that two events set `m_uboLocksDirty`:
  * a lock click (the lock's `onChange`; it also sets `m_uboResolveDirty`, so the entries re-resolve);
  * a value change of a bakeable row (`addChangeListener` + `TweakRegistry::isVarBakeable`): the panel flush, an
    override, a synced value, `Tweak::notifyChanged`. Renderer setters that store lockable values (`setShadowParams`,
    `setFogParams`, `setPostParams`, `setSkyParams`, `setSkyRadiance`, `setTerrainCragScale`) set it themselves.
* `applyUboLocks` runs on main at the top of `kickBeginFrameJob` (VR too) and returns at once unless dirty. Dirty:
  re-resolve if asked, evaluate the list, write the consts (`bakeUboValues`), and `reloadShaders` only when the
  generated declaration changed. A slider drag over a LOCKED row therefore rebuilds per changed frame; unlock the
  row to tune it live.
* **THE RELOAD IS PER SHADER.** `g_uboEntryStates` (Layout.ixx, `buildUboEntryStates`) holds one hash per entry of
  what its declaration lines say (offset, type, baked + value). A compile records the entries its FILES name (every
  `u_<name>` / `UBO_LIVE_<name>` token, comments and macro bodies included - a superset only rebuilds more; the
  generated texts are not scanned), and depends on those entries' states only: in the SPIR-V cache (the
  `ubo.generated.glsl` dep) and in the `ShaderStamp` every `ComputePipeline` / `GraphicsPipeline` keeps per stage.
  `applyUboLocks` runs `reloadShaders` with `Shader::s_uboOnlyReload`: a pipeline whose stamps are all current (cache
  key, root source, named entries) and whose fixed-function state is the same keeps its `vk::Pipeline`; the log line
  `UBO locks: rebuilt N pipelines, kept M` counts them. The include FILES are not re-checked in that mode, so a shader
  edit still needs F5. **A shader that reads a UBO value through a pasted name (`##`) would be missed - spell every
  `u_` name out.**
* A value that is not finite keeps the entry live (a const cannot spell it).
* **`UBO_LIVE_<name>`** (generated for every entry: the block member under either name) reads ONE value from the
  block even while it is baked - for a site where the constant costs more than it saves (pipeline stats, every lock
  against none, then a single-lock bisect): `u_rt_giStrength` in the ocean (`water_scene.inc`, `ocean.fs`) and in
  `reflection_fog` (baked, the GI branches went away and the register count rose), `u_rt_giRaysPerProbe` in the GI
  trace (a constant count unrolled the ray loop; `[[dont_unroll]]` instead cost the unlocked trace more). **Most other
  register moves are the allocator trading registers for spill at an equal demand** - they flip with whichever lock is
  on, so do not chase them.
* The pipeline stats' last column (`Defines=`) names a variant by its defines, so a bisect matches variants exactly.
* **Every row is LOCKED by default, and the state is not saved** (a panel click lasts for the run);
  `--tweak "@lock/Fog=0"` unlocks a section for a run, `--tweak "@lock/Fog/Global Density=0"` one row (a profile run's
  `-Tweaks` file can unlock them all for an A/B). A startup compiles the shaders ONCE, already baked.
* The reload reads the shader files on main: it opens `FileSystem::AllowMainThreadIO`, like F5.

## Push blocks (`PushBlock.ixx`)

The UBO's registered pattern for a push block, used by every push block with LOCKABLE values (the eye-adaptation
histogram, rain occlusion, the GI probe debug, the far-tree splat / records / far / resolve / floor smooth / march /
temporal / upsample). The others are still hand-written blocks with mirrored C++ structs.

* **A handle struct per block IS the layout**: `PushValue<T>` members that bind themselves where they are declared
  (`PushGroup`): `PushValue<float> radius = g("radius");`, a buffer reference
  `PushValue<uint64> pieces = g("pieces", "PieceList");` (an address names its GLSL type), a nested group with a prefix
  (`TvVolPush vol{ { g.block, "vol_" } };` -> `pc_vol_*`). Scalar layout, explicit offsets, 128 bytes at most. A
  variant can bind other names (`SplatPush::recordsVariant`: `pc_pieces` / `pc_map`).
* **The GLSL is generated per pipeline**: the layout carries `block.declaration()` as its `pushDeclaration`, and the
  shader writes `#include "push.generated.glsl"` where its block was (the includer serves that compile's text; the
  preamble enables `GL_EXT_scalar_block_layout` for every shader). An instance-less block of `pc_<name>` members; a
  baked one is `pc_<name>Baked` (the slot stays) and a `const` named like the member follows. `PC_LIVE_<name>` reads the
  member in both modes. The push range is `block.size()`.
* **A dispatch fills a `PushData`** (a stack buffer, so recording stays thread-safe): `PushData pc(block);
  pc.set(h.size, v); cmd.pushConstants(layout, stages, 0, pc.size(), pc.data());`. The LOCKABLE values are written by
  the `PushData` itself (`writeLockables`, the registered lambdas), so the fill site never computes them a second time.
* **Lockable** = a bound handle plus `block.lockable(h, lambda, sources...)` / `block.lockable(h, variable)`, after the
  handles are bound (the pipeline's `registerPushFields`, before its first build).
* The far-tree passes build `TreeVolumeParams` from the flat members with `TV_PUSH_VOL` (tree_volume.inc.glsl), so the
  helpers fold the baked ones.
* **Same events as the UBO**: a lock click marks every block dirty with a re-resolve, a bakeable change marks it dirty.
  `update()` re-resolves if asked, evaluates and bakes; true = a const changed.
* **Who updates when** (`Renderer::m_pushBlocks`, filled by `Renderer::registerPushFields`): a block with an `onRebake`
  is re-baked in `applyUboLocks` and only its passes reload (eye adaptation + re-record, rain occlusion, GI probe debug +
  re-record) - a UBO change in the same frame reloads everything instead. A block without one updates itself at the
  moment its values belong to:
  * the far-tree BAKE blocks (the splat pair, records, far columns, resolve, floor smoothing) take the LIVE settings - a
    running bake with other settings is dropped anyway (`recordBake`: `sameBake`), so the values a dispatch writes are
    the bake's;
  * the far-tree MARCH blocks (the march pairs, the temporal pass, the upsample) take `marchSettings()`: the shown
    bake's, or in the frame a bake without a cross-fade swaps, the new one's (its copy and swap run in that frame's
    `recordBake`, before the march; FloorMax's end marks the blocks dirty). The density scale is live;
  * all in `TreeVolumePipeline::prepare` (before any recording; GPU idle), `reloadBakePasses` / `reloadMarchPasses`.
* Code that writes a lockable push value calls `Tweak::notifyChanged`.
* Not lockable yet: the eye adaptation's `GpuParams` (a mapped per-frame UBO: "Post/Adapt *"), the bloom chain's
  per-level weights, the cloud shadow split - values that also depend on runtime state.
