# TODO

A loose list of improvements, open issues and ideas. It is NOT a plan: the plans are in `Docs/` and in
the `Code/<Lib>/CONTEXT.md` files. Add an item when you think of one; remove it when it is done or
rejected. The links point to where the item is explained.

Index:
[Doc fixes](#doc-fixes) ·
[Game](#game) ·
[Entity](#entity) ·
[Rendering](#rendering) ·
[Terrain, water, weather](#terrain-water-weather) ·
[Trees](#trees) ·
[Rocks](#rocks) ·
[Grass](#grass) ·
[Clouds](#clouds) ·
[Force](#force) ·
[Nav](#nav) ·
[Physics](#physics) ·
[Spatial](#spatial) ·
[Threading](#threading) ·
[Core and profiling](#core-and-profiling) ·
[Network and multiplayer](#network-and-multiplayer) ·
[Script and DSL](#script-and-dsl) ·
[Audio](#audio) ·
[Particles](#particles) ·
[UI, Input, App](#ui-input-app) ·
[File](#file) ·
[Dead code](#dead-code) ·
[Hand-kept duplicates](#hand-kept-duplicates)

---

## Doc fixes

Text that looks out of date or contradicts itself.

* [Procedural](Code/Procedural/CONTEXT.md): TerrainGenV3 says "no unload API" (~L155) but L196
  describes `unloadModels()`.
* [Procedural](Code/Procedural/CONTEXT.md): Trees header status (~L833) still says G1 + CPU preview
  only; "bone palette" (~L845) was dropped; `tree_record.inc.glsl` "not read by a shader yet" (~L1170)
  is no longer true; the preview section (~L1324) describes G4 and "no RT" as future.
* [TreeRenderingPlan](Docs/TreeRenderingPlan.md): the status line (dated 2026-10-01) is behind the
  phase table.
* [RendererVK](Code/RendererVK/CONTEXT.md): far-tree March bullet (~L1544) still describes the old
  COOL sky term; far-tree defaults (~L1292) may conflict with the user's choice at ~L1323 (self
  shadow, normal, interior off).
* [Game](Code/Game/CONTEXT.md): Determinism (~L194) says a mid-trickle wave is not resumed; Save /
  load (~L1363) says it is.
* [Spatial](Code/Spatial/CONTEXT.md): "Not yet built" (~L414) mentions a leaf's dynamic list, which no
  longer exists (~L369).
* Memory: `game-iteration-roadmap` (tweak sync shipped), `procedural-trees-plan` (G1 "next", "refine
  plan before building"), `weather-roadmap` ("no wind tilt").

---

## Game

See [Game](Code/Game/CONTEXT.md).

* No win condition. No lose condition: the Base takes damage but is never destroyed (it would
  soft-lock the respawn).
* Thousands of dynamic bodies is the aim, not a measured budget.
* Player turrets target units only, not players.
* Unit selection and orders are authority-only; clients cannot select units.
* A loose unit that reaches an unshielded target with no player near stands frozen.
* Far tick: no combat, bubble, strain or health death check (only the void kill).
* "Enemies alive" also counts player-team barracks units and units at 0 hp.
* Shield-less unit push from the baked field is ~3 frames late; with the bake off there is no push.
* Save/load: shots, remote players and player state (health, energy, materials) are not saved.
* `energyCapacityOf` returns a 1.0 placeholder for the barracks.
* The "Base energy/s" tweak is unused, and `base.pre`'s `Output` no longer applies.
* The match does not read the terrain height: the flat ground plane and grid sit at y = 0 over the
  streamed terrain ([App](Code/App/CONTEXT.md)).
* The seeded world is single-player only; the lobby needs a world block and the seed in GMp.
* Client melee has no local flash visual.
* Clients never get the attacker-team damage tag, so the hurt flash is always neutral red.
* The nav feed's cull hash is one Rebuild interval stale.
* `HeightLimit < 0` is the hook for flying units, which do not exist yet.

## Entity

See [Entity](Code/Entity/CONTEXT.md).

* World radius queries from scripts in the parallel pass are a known open hole in the
  thread-safety list.
* SIM LOD gaps: an unselected unit never runs its death check; unselected entities are not rendered
  (no shadow / GI); a visible entity past the outer radius is not selected (top-down assumption).
* With the body disable off, a parked body woken by a contact moves without its entity.
* RootOnly cull bounds use the rest pose; a limb swing can go outside them.
* The Entity Editor Cull Mode combo respawns only the selected entity, not its children.
* No "children changed" hook in SceneComponent (`addChild` / `removeChild` / ... are the place).
* A bad `HumanoidAnimator` limb setup is only a warning; the model is silently absent.
* SceneAnimatorComponent is PARKED. To revive: the checklist in `SceneAnimatorComponent.ixx`, a
  batched bone submit instead of one `renderNode` per bone, a DSL surface, an editor section.

## Rendering

See [RendererVK](Code/RendererVK/CONTEXT.md).

**Performance ideas not yet tried**

* Sun shadow pass is vertex-bound (~0.37 ms, nearly all terrain): a coarser terrain shadow LOD, or
  cache the static casters' cascades.
* Overlap small chains (the ocean sim's mip blits).
* Lit FS is occupancy-bound; only one try was made (reverted).
* Register peaks: split a peak into its own pass, or a thread-per-ray trace. Shadowless film
  highlights would cut the film's peak (visual trade). TES `terrainLayers` climate walk per control
  point.
* Bisect: lit FS +8 registers (09-23 → 09-29); `vol_apply` and lit #0/#1 +16 B (09-29 → 10-04);
  the film's 64/16 cause. With every tweak lock on, ground #8 and the film get worse (parked).
* Measure: TLAS spikes from the 4 km grove (~20 ms "TLAS + probe trace", unconfirmed); rocks in the
  tree instance set; the terrain FS after the grass canopy term; POM silhouette `discard`; wet sky
  reflection and glints register cost; the film flow's second tap; a shoreline view (ocean lost
  early-Z on the tessellated seabed).

**Features / quality**

* DLSS: no frame generation, no Reflex. Motion blur is off while upscaling. A viewport that does not
  fit the swapchain skips the upscale (workaround).
* The ocean writes no motion vectors (dual-source blend); its waves blur only with the camera.
* Motion vectors not read yet by the cloud temporal pass and the particle collision.
* GI: sky-visibility occlusion not applied under the ocean; L1 ratio filter unweighted; no
  half-Lambert weight in the volume (leaks through thin geometry); sun from above is outside the
  cloud-dimming model; sun fraction is one scalar for RGB.
* Aerial perspective: no multiple scattering (shadowed haze too dark); no reflection fog on mirror
  rays; the fog-off path carries no air for the cloud and far-tree apply stages.
* Particles and transparents inside a cloud / fog get the opaque pixel's cloud.
* POM silhouettes (`TERRAIN_POM_SILHOUETTE`) do not work (holes behind ridges, a sky strip).
* Biplanar rock AO comes from the top plane only (stretched on steep faces).
* Tree wind: billboards do not wave in the shadow pass or motion vectors; the TLAS sees unswayed
  trees.
* Push blocks without lockable values are still hand-written with mirrored C++ structs.
* Tweak lock state is not saved (user decision).
* OpenXR's command pool and clear buffer have no debug names.
* Streamline's own legacy barrier in the DLSS evaluate is the one sync-validation hazard left.

## Terrain, water, weather

See [Procedural](Code/Procedural/CONTEXT.md), [RendererVK](Code/RendererVK/CONTEXT.md),
[Particle](Code/Particle/CONTEXT.md).

* Terrain overlay pass: grow it with snow, deformation (needs displaced geometry + depth writes) and
  other surface layers.
* Wetness injection from rain, particle hits and script splats (the planned sources).
* Weather: ripples on terrain wetness, then splashes (from the depth-collision hit / the volume's
  bottom exit). No terrain floor in the occlusion map. Optional raymarched rain in the fog pass.
* Rain shelter needs RT; bushes (no BLAS) and objects past the RT range do not shelter.
* The Underwater / AboveWater draw gate is sea level only.
* Rivers (the terrain-data flow bits are free for them).
* Film: blocky outline pop along the clipmap's 0.5 m contour; the scene mirror ray is off
  (`TERRAIN_FILM_RT_MIRROR`, register cost); reflection fog has no climate, noise or shadowing.
* Edge foam was removed; the likely cause (the ocean clipmap's stepping edge) is not fixed.
* Foam injection is not shore-weighted (stuck foam in calm shallows).
* Contingency: if the ocean goes past 80/32 registers, move foam + bubbles to an overlay pass.
* Seabed splat has no relief (linear layer borders).
* Fractional tessellation still slides vertices slowly past the freeze distance.
* `precise` crashes glslang (`PropagateNoContraction`).
* One frame without water after an ocean grid rebuild.
* Cache-only terrain: a missing tile is re-read from disk on every fetch.
* The streamer ring is limited by the 16-bit mesh index (~51k chunk meshes at 128 chunks).
* Per-pixel wind domain rotation in `ocean_wave.inc.glsl` is off (creases).

## Trees

See [TreeRenderingPlan](Docs/TreeRenderingPlan.md), Trees in [Procedural](Code/Procedural/CONTEXT.md),
far-tree volume in [RendererVK](Code/RendererVK/CONTEXT.md).

* W5: profile and tune `R_near`, density and `FOG_TERRAIN_RES`; measure / amortise the far rebake
  (double-buffer, splat 1/N chunks per frame).
* W4 wind tuning (fades, amplitudes) and a RelWithDebInfo profile.
* Far volume "not yet": per-tree species colour (not last writer), half res, the analytic tail (P7),
  a dithered fade-out at the overlap end (tried, removed by the user).
* Far volume: no guard on the `accum` 16-bit slice wrap; the hand-over shows noise with no temporal
  accumulation; the volume lags the camera at speed; band at angular resolution 3072 (cause
  unknown); pixel-skip copies arrive 1-3 frames late; under temporal "1 of 4" runs as "1 of 2".
* The far records pass adds no bushes past the detail distance.
* Inside the crossfade band the shadow pass is not dithered (both tiers cast).
* RT sees only whole-tree billboards; bushes and tree meshes have no BLAS.
* Fixed pools with no growth: 128 MB record pool, 600k-piece dynamic set. A ring-radius change
  restarts everything.
* Per-tree VS warp for near uniqueness (G4b), device-local expansion records, scaling past ~100k
  trees (per-tree GPU cull + compaction).
* Tree mesh LODs are off (GPU uses LOD 0); `lodError` / `Lod ErrorScale` unused.
* No BC7 encoder (billboard normals use BC3). Bark and leaf textures are procedural until authored.
* Texture parameters do not regenerate an existing PNG; no species hot reload; `TREE_GROVE_TYPES`
  is a hand-kept list.
* Option B: `tree_cull.inc.glsl` reads the 4-byte records directly (measure Option A first).
* Record benefits not used yet: felled-tree edits, trunk colliders, Nav / gameplay queries.
* Remove the scatter's `tree_small_02` rules "when this ships".
* Plan phases not started: P2 (clipmap, toroidal bake, max-height skip), P3 (stateless placement),
  P6 (T0↔T2 hand-over), P7 (T3 analytic canopy), P8 (shadows across tiers), P9 (seasons, species
  in T2, RT), G3 (live species editing), G8 (cluster ellipsoids, stats, trunk capsules).
* Leaf-only depth prepass (leaf overdraw is the expected main cost).
* Open questions Q1-Q13 in the plan.

## Rocks

See [RockRenderingPlan](Docs/RockRenderingPlan.md), Rocks in [Procedural](Code/Procedural/CONTEXT.md).

* R6: remove the `boulder` / `rocks` scatter rules and their assets.
* OWED: a profile of the tree culls with and without world rocks (does the LOD change cost trees?).
* Measure the far march register count after the rock lighting change (72 before).
* If big rocks read as soft blobs past 600 m: a per-type later hand-over ("Far start") or a
  per-material blob shrink (A/B with the user).
* Not built: `Tilt`, satellite `Cluster`s, dual contouring / locked fracture edges, per-column albedo
  sums in the far volume, a per-type `shadowDistance`.
* A rock has no billboard, so no far-volume hand-over: it draws wherever its chunk is in the set.
* The triplanar rock material's per-pixel cost is a guess.
* Small ground clutter (pebbles, scree, branches) is a later, separate system.
* Open questions Q1-Q4 in the plan (Nav blocking, `Huge` class, normal bake, climate box).

## Grass

* Far tier G2: the terrain FS blends toward the grass colour / BRDF so the range edge shows no ring.
* Interaction (trampling), clump / flower scatter.
* No VR. Roots do not sample the tessellated relief; the splat relief blend does not affect density.
* Blades: no RTAO, not in the TLAS, no scene-cascade shadows. Other objects do not read the near
  grass cascade.
* Mesh shaders only if a profile asks.

## Clouds

* Phase 4: RTX 2000 presets, empty-space skip, a cirrus layer, weather coupling.
* An aerial-perspective LUT.
* Dark band on the lit side of far clouds with a small step budget (workaround: 600 `Max steps`).
* The sun aureole is not in the cloud-shadowed sky-SH correction.
* Past the far shadow cascade the transmittance mean is not measured.
* Open: GPU budget (≤ 2 ms at 1440p on the 4090?) and the cloud "Type" slider default.

## Force

See [Force](Code/Force/CONTEXT.md).

* A map-scale emitter floods grid cells since the big-emitter bypass was removed (fallback: "Use
  grid" off).
* A merged group is always a sphere; cone Lances must opt out by hand.
* Merge cover scale < 1: member rims stick out. Group spheres are one frame behind.
* The "Force merge union" pass is serial.
* The baked field caps at 512 chunks; the gradient is XZ only; ~3 frames latent.
* `getEquilibriumRadius()` is an average and lags ~3 frames, but shield UI and damage rely on it.
* VR: no shell culling, no union march, no sampled-tier view clip.
* Small bubbles cannot use the sampled tier.
* No authored on/off for ForceComponent.
* A CPU field evaluator (`forceContribution` + pressure / equilibrium mirrors) would unblock a
  headless dedicated server.

## Nav

See [Nav](Code/Nav/CONTEXT.md).

* Lane re-plans lag a moving player badly outside the "Target track radius".
* A dirty obstacle set waits for every in-flight build, then re-kicks all builds.
* `floodSolveChunk` and the seed-path A* have no pre-emption points.
* Rebuilds are deferred out of physics-step frames (latency); a finished build publishes one frame
  later.
* A failed seed search is dropped silently; only the first 20 m of a plan is written.
* `clearArea` is unused.

## Physics

See [Physics](Code/Physics/CONTEXT.md).

* At most one step per update: below `stepHz` the sim runs slow instead of catching up.
* A kinematic or static body does not follow a gizmo drag ([Input](Code/Input/CONTEXT.md)).
* The `buoyancyStep` 16-bit wrap gives a rare spurious catch-up force.
* `enabled` is only written by `spawn()`; the setter is not there yet.
* Known race (left on purpose): `unpark` reads `suspended` while the owner rewrites the byte.

## Spatial

See [Spatial](Code/Spatial/CONTEXT.md).

* Script `query*` / `forEachIn*` traverse serially (a large script ball query would gain).
* No leaf-cell slicing in the fan-out (a narrow view over a few big cells).
* The ray tester goes lane by lane, not 8-wide.
* Only one `traverseParallel` at a time (`m_frontier`).
* An invalid view (first VR frame) skips the whole update.
* The stress harness is never driven by App.

## Threading

See [Threading](Code/Threading/CONTEXT.md).

* `JobMutex` barges (unfair).
* Main never helps with Low jobs; a main wait on a Low job spins.
* The first JobGraph run has no costs and promotes nothing; `JobCost` is lossy; ad-hoc submits have
  none.
* No permanent deep stress test; the stress harness is never driven by App.

## Core and profiling

See [Core](Code/Core/CONTEXT.md).

* The `oc::` container rule is not compiler-enforced.
* The EASTL aligned hook asserts on a non-zero `alignOffset`.
* Units, scripts and force emitters have no profile scope (ring lapping).
* Unregistered threads (startup pools, miniaudio) have no profiler track.
* Profile noise is 10-40 %; only ONE `-Tweak` works through `powershell -File`.
* Memory panel: VRAM mode does not sample churn; a VRAM hover allocates.
* `setFpsCeiling` never applies in VR.

## Network and multiplayer

See [Network](Code/Network/CONTEXT.md), Multiplayer in [Entity](Code/Entity/CONTEXT.md),
[NetFuzz](Code/NetFuzz/CONTEXT.md).

* No headless game server (`--headless` refuses `--game`: GPU field readbacks drive the sim).
* Event chunking past the caps (GSt ~79-100 structures, GSh 120 records).
* The key exchange is not authenticated (no MITM protection).
* IPv4 only. No reassembly for large Unreliable messages. Blocking DNS and `netGetExternalAddress`.
* NetFuzz: game mode has no ECDH (needs `--no-encrypt`); encrypted path is a smoke test only.
* Remote-owned entities: loss gaps fall through to the push correction for a frame.
* A Global entity cannot be handed over.
* A CLI `--game --connect` client in a menu-hosted lobby never readies.

## Script and DSL

See [Script](Code/Script/CONTEXT.md), [DslCompiler](Code/DslCompiler/CONTEXT.md).

* Fault recovery is manual only (F6). C++ exceptions and stack overflow stay fatal.
* The cooked build is all-or-nothing; adding a script needs a reconfigure.
* `SCRIPT_CTX_FUNCS` caps at 8 params (`audioTrigger` uses 7).
* The container-mutation check is conservative; the editor grey-out misses transitive mutation.
* DSL gaps: no `!`, one comparison per expression, no `a[i]`, no null, no `v * 2`, no
  `Entity` in `@data`, no array exposure; `self.light` cannot grow; `world.spawn` misses on the
  spawn frame; `entitiesInRadius` cannot use `ifexist`.
* `audioTrigger` always passes `overrideMask` 0.

## Audio

See [Audio](Code/Audio/CONTEXT.md).

* Only the HRTF direct path: no occlusion, reflections or reverb.
* Position is not integrated from `setVelocity`.

## Particles

See [Particle](Code/Particle/CONTEXT.md).

* Ocean spray uses only the first emitter (no look selection in the producer).
* Lit particles treat area and tube lights as point lights.
* `DECAL_FLAG_LIT` only approximates sun + GI.
* A "retired N non-finite particles" log means an upstream NaN that is still a bug.
* A rare race in `getEffectDesc` loads a `.pfx` twice.

## UI, Input, App

See [UI](Code/UI/CONTEXT.md), [Input](Code/Input/CONTEXT.md), [App](Code/App/CONTEXT.md).

* KNOWN ISSUE: EntityEditor `commitRespawn` draft builds run on the job and can import a container
  that is not cached yet.
* Entity Editor: physics, animator and script data are respawn-only.
* ImGui has no viewports.
* Input follow-up: check that drags leaving the window still work after the cross-thread
  cursor/capture change.
* CLI clients keep the auto-reconnect instead of returning to the menu.

## File

See [File](Code/File/CONTEXT.md).

* No cache version compatibility: any layout change recooks every scene.
* GC cannot collect caches whose `.oc` import options changed.
* Only cooked scenes provide a mesh stream source.

---

## Dead code

* `UI/Private/NodeEditor/` + the vendored imgui-node-editor, the NodeEditor audio nodes, the `.scr`
  compile path in ScriptHost.
* `World::buildSceneAnimatorSpawnInfo` (no caller while SceneAnimator is parked).
* `ForceQuery` (debug spawner only), `FlowField::clearArea`, `JoystickListener` dispatch.
* `borderwall.pre`, the retired `Connector` enum slot.
* `ParticleSystem::initialize()` is empty.

## Hand-kept duplicates

Values that must be kept in step by hand; a shared source would remove the risk.

* NetFuzz mirrors `GameProtocolId` / `GameNetVersion` (NetFuzz/main.cpp).
* `c_edgeWallHalfThick` ↔ `edgewall.pre` `HalfExtents`.
* `loadUnits` fallbacks ↔ the save's omitted-default keys.
* The Game `Gq*` chat filter repeats App's 256-byte cap.
