# Force

> Library documentation for `Code/Force`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## The field

Forcefield bubbles. Emitters (Team 0–7, Output, Reach, Focus, Distribution, Width + direction)
project ANALYTIC fields with compact support spanning exactly `pos .. pos + dir*Reach`.

* Focus 0.5 = an exact sphere spanning the line; 0 and 1 = exact cones. Width is a pure lateral
  scale. `distGain` bumps density along the line.
* TOTAL output is invariant across shape params — a quadrature-cached budget fold is pre-applied to
  the uploaded Output, so pinching densifies instead of shedding power. Reach still scales the total.
* Gradients are finite-differenced.
* Same-team fields SUM (metaballs). The drawn surface is the equal-field equilibrium
  `phi_best - max(iso, phi_second) = 0`, so squish and pierce fall out of the math.
* Field math lives ONCE in `force_field.inc.glsl`; a CPU lobe mirror in System.cpp drives the debug
  rings.

### Shader rule for the per-team phi arrays

**NEVER use a dynamic-index STORE** (`phi[e.teamFlags.x] += c`).

The NVIDIA compiler miscompiles it on a 2-element private array: the add lands in BOTH elements for
index 1 and in NEITHER for 0, i.e. φ0 == φ1 everywhere at the co-op `NUM_FORCE_TEAMS` 2 — correct
SPIR-V, wrong SASS.

`forceAccumulate` / `forceAccumulateVisible` instead compare inside the unrolled per-team loop
(`phi[t] += team == t ? c : 0.0`), which compiles to predicated adds and is correct at every team
count. Reads by a loop induction variable unroll statically and are fine.

## Live team count

`ForceSystem::setNumTeams(2..8)` is a GAME-MODE setting (co-op = 2, PvP default 8), not a tweak.
`MAX_FORCE_TEAMS` (8) stays the CAP: the UBO colour array size, and the "outside every bubble"
sentinel.

The live count rides `ForceFieldParams::numTeams`. The renderer detects the change
(`setForceFieldParams`, one device idle — the `useGrid`-toggle pattern) and:

* recompiles every force shader with the per-pipeline `NUM_FORCE_TEAMS` define (all per-team loops
  and phi arrays size by it — 2 teams is a quarter of the 8-team accumulation);
* remakes the team-sized resources — the shell volume (ONE RGBA16F texture at ≤ 4 teams, two at 5+;
  deliberately never RG16F, since rg16f image stores need the `shaderStorageImageExtendedFormats`
  feature the engine does not enable — the second view slot is null and bindings fall back to view A)
  and the CPU-bake readback ((numTeams+3)/4 vec4s per sample, mirrored by `sampleBakedField` /
  `publishBake`).

Emitter and instance team values clamp below the live count (`buildEmitterGpu` + `setTeam` — shader
phi arrays are NUM-sized).

## `Globals::forceSystem`

* `initialize()` from main registers the "Force" tweaks → `ForceFieldParams` pushed every update.
* `update(renderer, dt)` runs main-thread after `world.update` and is the only writer of renderer
  force state.
* `createEmitter(...)` → RAII `ForceEmitter`. Setters are safe from the parallel pass (own slot
  only); `getAppliedForce()` / `getPressure()` are GPU readbacks, roughly 2 frames latent.
* `createQuery(pos)` → `ForceQuery` for territory:
  `Result{owningTeam, inside, ownField, opposingField, valid}`, same latency. `setPosition` is
  pass-safe, so a query can RIDE a moving unit. `MAX_FORCE_QUERIES` is 1024.

### The baked pressure field

"Force/Bake" tweaks, read with `sampleBakedField(pos, team)`.

1. `update()` selects at most `MAX_FORCE_BAKE_CHUNKS` (512) 16 m XZ chunks from the live emitters'
   and groups' support boxes (`buildBakeChunks` — conservative output-line AABBs plus
   transition/group spheres). Hitting the cap printfs once, bumps a stat, and dropped chunks read
   zero.
2. `force_bake.cs` evaluates 16×16 samples per chunk — a corner-aligned world lattice at 1 m spacing,
   so unit-shield bubbles stay resolved, with ALL 8 team φ values per sample as two vec4s — at
   "Sample height" (1 m).
3. `publishBake` copies the slot's readback WITH the chunk list it was evaluated for
   (`ForceBakeReadback` pairing; per-slot lists are stored at upload). End to end this is ~3 frames
   latent.

`FieldSample{valid, inside, owningTeam, opposing, opposingGradient}` comes from ONE 2×2 bilinear
fetch. A missing chunk = zero field, correct by construction; the gradient is the analytic derivative
of the bilinear patch. It is worker-safe between updates, has unlimited consumers and NO per-consumer
GPU slot — this is the swarm units' emitter-interaction path.

## Renderer side (`RendererVK:ForceFieldPipeline`)

Emitter and query slots are main-thread with a free list and retirement; readbacks are slot-indexed,
so slots never re-pair with stale results.

### Shell draw culling

Upload-time CPU, desktop only — VR skips it. A drawable shell outside the center-view frustum, or
whose projected proxy radius is under "Force/Shell/Min screen radius (px)" (3), compacts into the
NON-drawn field partition: field, grid and readbacks are untouched, only the ray-march draw is
skipped (`ShellCull` in upload; the bounding sphere mirrors `forceEmitterBounds`; camera-inside never
size-culls).

### March LOD

The FS's step count tapers with the proxy's projected size below "Full-detail radius (px)" (160),
floor 8 (`forceParams2.w` = pixelScale/fullResPx; 0 = off).

### Sampled shell tier

"Force/Shell/Sampled tier radius (m)", 5; 0 = off.

Emitters whose VISIBLE bubble radius is at or above it march a BAKED FIELD VOLUME instead of the
analytic candidate loop. The metric is `forceEmitterVisibleRadius` (Layout.ixx = the shader's
`forceVisibleRadius`), the iso-shrunk draw box's bounding half-extent — NOT the authored Reach, which
the merge's group/transition spheres made a near-constant across all shield sizes. The SAME metric
drives the upload partition, the bake-volume fit and the union's ownership skip, all against
`forceBake0.w`.

* Two RGBA16F 3D textures hold all 8 team φ (`FORCE_SHELL_VOLUME_*` 128×48×128). ONE set, serialized
  by an acquire barrier, GENERAL for life.
* Written by `force_shellbake.cs` each frame (indirect dispatch, x = 0 when no emitter qualifies —
  the CB is cached) with the FULL analytic field, small-bubble deformation included.
* Refit each frame in `buildUboForce` over the union of the large DRAWABLE emitters' support boxes
  (`Ubo::forceBake0/1`: world min, 1/size, threshold, enabled). Fixed texels mean the resolution
  self-adjusts; clamp-to-border black = zero field outside, correct by construction.
* The FS's per-step `forceMarchSample` branches per instance. On the sampled tier the hit BISECTION
  and NORMALS also read the volume — the surface being refined IS the trilinear field, so its
  gradient matches exactly — while ownership dedup and the shading colour/alpha accumulate stay
  ANALYTIC, since they need per-emitter identity and shell alpha that the volume does not carry.
* The "camera inside a bubble" test is ONE CPU field evaluation per frame
  (`forceContributionCpu` mirror in `buildUboForce` → `forceBake2.w`), never a per-fragment origin
  re-sample.
* Trilinear iso-surface error is O(h²·curvature) — centimetres on large bubbles, which is why SMALL
  bubbles stay analytic.
* Draw set bindings 5/6 are the volumes.

### Union march

"Force/Shell/Union march" on, plus "Union step (m)" 1.5 and "Union max steps" 8. Desktop only, and
the density debug view forces it off.

The ANALYTIC tier becomes ONE march per pixel:

* Upload partitions drawables as `[sampled tier | analytic]`.
* The analytic proxies draw ONLY into the interval pass — its own RG16F render pass in the primary,
  before the scene stages, "Force intervals" GPU scope: shell VS + `force_interval.fs` MIN-blend
  (tEntry, −tExit) per box (`blendOp eMin` is a new `GraphicsPipelineLayout` field), cleared to
  fp16-max, ending SHADER_READ_ONLY.
* A fullscreen triangle (`composite.vs` + `force_union.fs`, indirect vertexCount 3/0) marches each
  covered pixel's union interval ONCE, with steps = length/step-size capped. The step GROWS with
  distance so far pixels never march finer than ~2 px (`forceBake2.z`).

That kills the overdraw × march term where small bubbles stack.

**Half resolution** — "Force/Shell/Union half res" is on by default and is a REBUILD-CLASS toggle
like Use grid: flipping it idles the device, re-sizes / creates or destroys the targets, and
re-injects `FORCE_UNION_UV_SCALE`.

* Interval and march targets are swapchain/2 (`resizeIntervalTarget` halves internally); both FS map
  `gl_FragCoord` back to full uv with the injected scale, and the primary passes halved viewport and
  scissor.
* The march renders into its own RGBA16F pass ("Force union march" GPU scope, cleared 0, no blend,
  ending SHADER_READ_ONLY), recorded right after the interval pass where gbuffer depth is still
  SHADER_READ_ONLY.
* The "Force union blend" scene stage (`force_union_upsample.fs`) composites it depth-aware: 2×2
  bilinear weights × relative depth similarity — each half texel's representative depth is the
  full-res depth at its own march uv, the exact value it clamped against — with a nearest-depth
  fallback when all four sit across a silhouette, then the premultiplied blend the full-res draw
  used.
* Toggle OFF is the pre-half-res path exactly: full-res interval target, the march draws directly
  into scene colour in the blend stage, and NO march framebuffer exists.
* VR is untouched — its union indirect is 0. In half-res mode the march pass still runs as clear-only
  so the march image's layout transition always happens.

**Empty-space skipping** — a force-grid cell with no candidates PROVABLY holds ZERO field (every
emitter inserts into every cell its support overlaps), so the sample is a constant and the index
jumps past the empty stretch with the bracket start moved there. (A bracket spanning the skip would
cost the bisection its accuracy at the next bubble's entry.)

**Union march extras**

* The cell hash probe is cached per cell segment (`forceSampleFieldCell` re-probes only when a step
  crosses a 16 m boundary).
* The march phase carries a STATIC per-pixel jitter — "Force/Shell/Union jitter", OFF by default,
  rebuild-class; the `FORCE_UNION_JITTER` define compiles it out when off. Spatial noise instead of
  step banding, so a larger "Union step (m)" stays presentable, with no temporal term so nothing
  shimmers with TAA off. Step counts are already tweaks: "Force/March steps" for the proxy march,
  "Union step (m)" + "Union max steps" for the union.
* Both marches break on `accumAlpha ≥ 0.98`.
* Its bisections run 5 iterations; the shell FS keeps 6 for cross-proxy hit matching.

### Draw-box shrink

"Force/Shell/Visible bounds iso frac", 1.0; 0 = off.

`packVisibleBounds` (System.cpp, inside `buildEmitterGpu`) packs each emitter's closed-form OWN iso
extent — evaluated at iso × the frac, the merge slack for two sub-iso fields summing past iso — into
`teamFlags.w`. `forceVisibleBounds` (force_field.inc) shrinks the proxy VS, interval FS and shell FS
march boxes to it, so a weak bubble in a full-size reach box costs its actual size.

The FIELD keeps the full support everywhere: grid insert, bake fits and CPU mirrors all stay
`forceEmitterBounds`.

### Shading and the draw

* Shading is the SHARED include `force_shell_shade.inc.glsl` (`forceShadeHit` / `forceShadeWall` take
  an `ownerIdx` param — `v_emitterIdx` per proxy, or the crossing's dominant emitter in the union
  pass).
* The union pass needs NO ownership discard (one march owns every crossing); it only skips crossings
  whose dominant emitter is sampled-tier, since those proxies draw themselves.
* With the union off (VR or tweak) the proxy draw spans both partitions — the old path exactly.
* Rendering is one instanced draw of unit cubes (the decal pattern, after debug lines and before
  particles) whose FS ray-marches the field through the emitter's reach box, bisection-refines, and
  does a manual reversed-Z depth test. OWNERSHIP DISCARD dedups merged bubbles.

### Compute (`recordForceCompute`, after the light grid)

* A uniform emitter hash grid per frame at `FORCE_GRID_CELL_SIZE` 16 m — the FORCE grid's OWN size,
  finer than the shared `hash_grid` `GRID_SIZE` 32 the light grid keeps, because gather cost scales
  with small emitters per cell and a swarm packs dozens into a 32 m cell
  (`force_grid.inc.glsl` + `force_grid.cs.glsl`). EVERY emitter inserts — there is no big-emitter
  bypass list any more, so an empty cell provably holds zero field.
* `force_emitter.cs` integrates opposing pressure with a 13-sample integral (a center-only tap reads
  zero when big bubbles press rims).
* `force_query.cs` evaluates points. Readbacks are host-visible.
* "Force/Use grid" off is the brute-force A/B toggle.

## `ForceComponent`

`Component Force` in a `.pre`: `Team` / `Output` / `Reach` / `Focus` / `Distribution` / `Width` /
`Centered` / `Mergeable`, plus local `Direction` / `Offset`. Demo: `Entities/SphereField.pre`.

* It keeps its emitter on the entity's world transform, and is not frozen-gated — placement, not
  simulation.
* `Centered` (default true) pulls the emitter back Reach/2 so the bubble centres on the entity.
* Reach is in world units, unscaled by the entity.
* There is no authored on/off — scripts use `setOutput(0)`.

### Active gate

`ForceEmitter::setActive` / `isActive`, `EmitterInstance::active`, default on; pass-safe like
`setOutput`.

Off means the slot and every parameter stay, but the emitter uploads with flags 0 — skipped by grid,
draw and compute, the same upload a Merged member without readback makes. It also skips the bake
boxes, reads zero force and pressure, and has no bubble (`refreshBubbleBounds` zeroes `bubbleRadius`
and the candidate and drops the bounds cache), so the member sweep evicts it and any transition state
is dropped at upload.

The World's SIM LOD drives it: "Game/Sim LOD/Force bubbles max tier" (default 1) — active only while
the entity's tier is at or below it, on every selected entity with a ForceComponent, structures
included. See [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

Script access is through the `entityGetForceComponent` / `forceGet*` / `forceSet*` handle ABI, plus
the NodeEditor Get/Set Force nodes.

## `ForceEmitter::getEquilibriumRadius()`

A pressure-aware estimate of the CURRENT bubble radius: the closed-form iso profile at the widest
station with the threshold raised to `max(iso, pressure readback)`.

It is an AVERAGE — the true surface sits closer on the enemy-facing side — and about 2 frames latent.
The Game player's shield UI and damage logic read it.

## Emitter merging

`ForceSystem::updateMerging`, "Force/Merge" tweaks, main-thread inside `update()` before the upload.

### What merges

Emitters that are mergeable and have a bubble above iso cluster per TEAM into persistent
`MergeGroup`s.

> Mergeable is DEFAULT ON for every emitter. `ForceEmitter::setMergeable(false)` or
> `Component Force` → `Mergeable false` opts out. A merged group is always a SPHERE, so a cone-shaped
> Lance that merges loses its shape — opt such prefabs out.

Each group is carried by ONE GPU sphere emitter (focus 0.5, axis up) whose iso radius covers every
member's bounding sphere (`refreshBubbleBounds`: the closed-form iso profile sampled at 16 stations).

* `memberCover` = "Cover spread scale" × |cᵢ − centre| + "Cover radius scale" × rᵢ.
* Group radius = "Cover scale" × the max over members + "Cover margin".
* Defaults 1 / 1 / 0.85, margin 0.2 m, "Max group radius" 4 m, "Max members" 255,
  "Summed output fraction" 0.5. 1/1/1 = exact cover; lower hugs the crowd tighter, letting member
  rims poke out at the edge. The SAME formula feeds the target, the Merged-member floor, the Joining
  completion test and the max-radius checks, so the guarantees stay consistent with whatever scaling
  is chosen.
* Output = max(sum × "Summed output fraction", the densest member's centre density), so the merged
  bubble is never fainter than a member. Reach is solved from the sphere's closed form, so the
  visible radius equals the cover exactly.

### Hysteresis

* A pair JOINS when the bubble centres are within "Join distance (x radii)" (0.5) × (rᵢ+rⱼ).
* A member LEAVES when NO other member is within "Leave distance" (0.85) × (rᵢ+rⱼ) — below 1.0, so
  the member's own bubble reappears while it still overlaps the group's cover and a unit is never
  exposed on the split frame.
* It also leaves on a team change, a lost bubble (collapsed shield output 0.01), a cleared flag, or
  drifting past "Max group radius" × 1.1.

### Off the main thread, pipelined a frame

* `update()` kicks the whole `updateMerging` as ONE job ("Force merge") AFTER the upload, over this
  frame's post-sim emitter state.
* `ForceSystem::joinMerge()` (main.cpp loop top, right after the UI join and before input or
  entity-change drains can create or destroy emitters; also once after the loop) joins it.
* This is the UI-job pattern: it runs during present and the fence wait, where main has nothing to
  overlap it with otherwise. Kicking after `world.update` and joining in `update()` measured as a
  pure 0.06 ms main-thread wait — main had nothing between them.
* The group spheres and transitions the upload uses are therefore ONE FRAME behind the emitters —
  centimetres at unit speeds, inside the cover margin plus hysteresis.
* The job NEVER touches the renderer (`present()` reads the force slot vector in that window):
  `createGroup` leaves `rendererSlot` UINT32_MAX and `update()` mints it on main;
  `dissolveGroup` pushes the slot to `m_retiredGroupSlots`, which `update()` destroys.

### The passes

Each per-item pass is `runPass` = inline under a small count (256 emitters / 8 groups / 128
candidates — a parallelFor's submit + wake + join costs more than the items) and a `parallelFor`
above it. The work is PARALLEL AND SCALED BY CANDIDATES, NOT EMITTERS.

| Pass | Kind | What it does |
|---|---|---|
| "Force merge bounds" | per emitter | Cached 16-station profile, re-evaluated only when a shape param or iso changed. A CANDIDATE = mergeable + bubble + own cover term under "Max group radius", staged `PerWorker` with a CAS-max of the largest join radius. |
| "Force merge leave" | one job per group | Touches only its own members. |
| "Force merge cells" | serial | Candidates → (cellKey, idx) sorted by key. Cell = 2 × the largest join radius; 21-bit biased coords packed in a uint64. |
| "Force merge neighbours" | per candidate | Binary-search the 27 surrounding cells, exact test `joinK × (rᵢ+rⱼ)`, pairs i<j staged `PerWorker`. |
| "Force merge union" | serial | Ungrouped pair → new group; lone + group → join if it fits "Max members" / "Max group radius"; two groups → merge smaller into larger. Serial because of the renderer slot API and cross-group membership moves. Its cost is the PAIR count. |
| "Force merge cover" | per group | Target + displayed sphere, `dissolve` flag. |
| "Force merge dissolve" | serial | The sweep. |

> "Force merge neighbours" REPLACED a `SpatialIndex::querySphere` per emitter, which walked the
> entity hierarchy's render-entry-filled cells and cost 1.6 ms/frame on main for ~100 emitters. The
> private list costs 0.04 ms.

Main-thread `update()` order: "Force merge join" (Wait; only if the loop-top join did not happen) →
"Force prepare" (params push + the job's retired group slots) → "Force upload" (per-emitter
parallelFor: distinct renderer slots, read-only readback span, PerWorker debug lines) →
"Force groups upload" → "Force queries" → "Force merge kick" (the job submit).

Groups under "Min members" dissolve; `Enabled` off dissolves everything.

### Smoothing and membership

`recomputeCover` writes a TARGET sphere. The DISPLAYED one first FOLLOWS the members' own motion 1:1
— the output-weighted mean displacement of the members already in the group last frame, from
`prevBubbleCenter`; a sphere eased on a time constant trailed a marching crowd, and the floor then
inflated it forward, leaving a void behind — and then eases the residual (membership jumps) over
"Smooth time" (0.3 s; `smoothGroup`). It is floored by the cover of the Merged members at the
displayed centre: they have no field of their own, so that floor is the coverage guarantee.

Membership is a state machine **Own → Joining → Merged → Leaving → Own**.

* While Joining or Leaving, the member uploads an ACTIVE sphere of its OWN output whose centre and
  visible radius lerp (smoothstep over "Blend time" 0.5 s) between its own bubble and the group's
  displayed sphere.
* Leaving starts between the own bubble and the group sphere per "Leave from group sphere" (0.5;
  1 = a full ghost copy of the group sphere shrinking onto the unit, which read as an empty bubble
  left behind) and slides to the unit.
* Joining starts at the unit's bubble and only completes (→ Merged / PASSIVE) once the displayed
  group sphere actually covers it. A linear lerp of centre + radius keeps the unit inside the sphere
  at every step.
* A new group grows out of its larger founder's bubble. A dissolving group (undersize, group-group
  merge, Enabled off) starts a Leaving — or Joining into the absorbing group — sphere for every
  member from where its sphere stood. A reversal mid-transition restarts from the sphere uploaded
  last frame (`blendCenter` / `blendRadius`).

### A merged member projects no field

* With "Member readback" (default on) it uploads `FORCE_FLAG_PASSIVE`, compacted into a tail past
  `fe_count` (header `evalCount`) that only `force_emitter.cs` evaluates — so its `getAppliedForce` /
  `getPressure` are its OWN position's truth against the group's field, which is what the unit shield
  logic wants.
* Off means the member's slot is skipped entirely and the group's readback is split by output share
  (cheapest).
* Every getter and setter keeps working while merged (`isMerged()` tells).

Stats: "Groups (stat)" / "Merged emitters (stat)". "Force/Debug/Draw merge groups" draws the group
sphere plus spokes to the members; merged members' own rings are hidden from "Draw emitters".

## `ForceEmitter::setShellAlpha(alpha)`

`outputParams.y`, default 1 — shell opacity.

* 0 SKIPS the ray-marched shell draw entirely while the field stays live: it still deforms bubbles
  and still serves force, pressure and query readbacks.
* Invisible fields also never TINT other shells. Shading weighs team colour and contact glow by
  alpha-scaled fields (`forceAccumulateVisible`), and an equilibrium wall against an invisible side
  reclassifies as the visible team's SKIN — shaded and owned by that team — because the invisible
  proxy never rasterizes, so pane-classified walls would silently drop.
* `ForceFieldPipeline::upload` classifies every ACTIVE slot ONCE into the partition
  `[sampled-tier drawable | analytic drawable | non-drawn field | PASSIVE tail]` — one sweep, one
  `shellVisible` test per emitter — and sets the draw `instanceCount`s from the bucket sizes; compute
  dispatches keep the full counts.

This is how a big invisible emitter avoids a full-screen march. Note that a map-scale emitter now
floods grid cells (the big-emitter bypass list was removed); "Force/Use grid" off is the fallback for
such authoring extremes.
