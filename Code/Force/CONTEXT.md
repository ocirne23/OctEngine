# Force

> Library documentation for `Code/Force`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Forcefield bubbles. Links RendererVK and Threading.

**Nothing here is simulated.** Emitters project ANALYTIC influence fields; same-team fields SUM
(metaballs); a point belongs to the team whose field beats both the iso threshold and every other
team; and the drawn surface is the **equal-field equilibrium** between teams. Squish and focused-lobe
"pierce" fall out of that math.

## The field

An emitter's field has compact support spanning exactly `pos .. pos + dir * reach`.

| Parameter | Meaning |
|---|---|
| `Output` | The TOTAL field budget. |
| `Reach` | Hard falloff-to-zero extent. The VISIBLE bubble is smaller: `r = reach * sqrt(1 - sqrt(iso/output))`. |
| `Focus` [0,1] | Shape pinch. **0.5 = an exact sphere spanning the line; 0 = a cone pointed at the emitter; 1 = a cone pointed at the target.** |
| `Distribution` [0,1] | Where the density sits along the line, as a smooth budget-conserving bump. |
| `Width` | Pure LATERAL scale, reach untouched. 1 = round; below 1 pinches every shape narrower (cones keep a straight taper at a sharper angle, spheres go prolate). |
| `Team` | 0 .. live team count. |

**TOTAL output is invariant across focus, distribution and width.** A quadrature-cached shape budget
integral is pre-applied to the uploaded Output (`refreshDistributionScale`, factor
`referenceBudget / (shapeBudget * width²)`), so **pinching densifies instead of shedding power**.
Reach still scales the total. `getCenterDensityFactor()` converts a measured density back into Output
units (~1.11 for the default centred sphere).

Gradients are finite-differenced. Field math lives ONCE in `force_field.inc.glsl`; a CPU lobe mirror
in ForceSystem.cpp drives the debug rings and the "camera inside a bubble" test.

## Files

`Private/ForceSystem.ixx` / `.cpp` (`Force:ForceSystem`) is the system: instances, queries, the
upload, the bake, merging. `Private/Emitter.ixx` / `.cpp` (`Force:Emitter`) is the `ForceEmitter`
handle class alone — its bodies resolve the instance through `Globals::forceSystem`; the two CPU
field mirrors it needs (`forceDistributionGain`, `forceReferenceBudget`) are declared unexported in
ForceSystem.ixx. `ForceQuery` stays in ForceSystem.ixx.

> ### SHADER RULE: never a dynamic-index STORE into the per-team phi arrays
>
> `phi[e.teamFlags.x] += c` is **miscompiled by the NVIDIA compiler** on a 2-element private array:
> the add lands in BOTH elements for index 1 and in NEITHER for 0, i.e. φ0 == φ1 everywhere at the
> co-op `NUM_FORCE_TEAMS` 2. Correct SPIR-V, wrong SASS.
>
> `forceAccumulate` / `forceAccumulateVisible` instead compare inside the unrolled per-team loop
> (`phi[t] += team == t ? c : 0.0`), which compiles to predicated adds and is correct at every team
> count ([force_field.inc.glsl:183](../../Assets/Shaders/force_field.inc.glsl#L183)). Reads by a loop
> induction variable unroll statically and are fine.

## `Globals::forceSystem`

[ForceSystem.ixx](Private/ForceSystem.ixx).

* `initialize()` from main before world spawns — registers the "Force" tweaks, which live on
  `ForceFieldParams` and are pushed to the renderer every update.
* **`update(renderer, dt)` runs main-thread after `world.update`** ([main.cpp:808](../App/main.cpp#L808))
  and is the ONLY place renderer force state is written.
* `joinMerge()` at the main-loop top ([main.cpp:575](../App/main.cpp#L575)) — see Merging.

### Handles

Both are move-only RAII, and **their setters are safe from the parallel entity pass** (one writer per
instance, own slot only). Create and destroy take `m_createMutex`, since they run concurrently from
spawn jobs; `m_emitters` is RESERVED to `MAX_FORCE_INSTANCES` and `m_queries` to its renderer cap at
initialize, so growth never reallocates under a concurrent handle resolve.

> ### An INSTANCE is not a GPU SLOT
>
> `MAX_FORCE_INSTANCES` (**32768**, ForceSystem.ixx) caps the CPU instances; `MAX_FORCE_EMITTERS`
> (**8192**) caps the renderer slots. **`createEmitter` claims only an instance** — a renderer slot
> is minted by the first `update()` that sees the emitter ACTIVE and handed back the frame it is
> gated off. Every unit on a 25k-unit co-op map can therefore carry a bubble while only the ones
> near a player cost a GPU slot; without this the far ambient spawns exhausted the 8192 slots.
>
> Hitting either cap prints once (the slot one re-arms when the starvation ends), and the
> `Emitters` / `GPU slots` stat tweaks under `Force` show both counts live. A starved ACTIVE
> emitter simply projects no field that frame and retries the next.

**`ForceEmitter`** — `createEmitter(team, pos, dir, output, reach, focus, distribution, width)`.

* Setters: `setTransform` / `setPosition` / `setOutput` / `setReach` / `setFocus` /
  `setDistribution` / `setWidth` / `setShellAlpha` / `setActive` / `setMergeable` /
  `setAnalyticReadback`; `setTeam` is
  main-thread (rare).
* Authored getters mirror all of them and read the LIVE instance state, valid immediately.
* Readbacks: `getAppliedForce()` (the opposing teams' field pressure integrated over this emitter's
  own bubble) and `getPressure()` (the mean opposing field strength). **The DEFAULT source is the
  CPU pressure bake** (`bakedReadback`, run inside the upload jobs): the `force_emitter.cs`
  integral mirrored as a centre tap plus a planar ring of 8 (16 above 20 m reach) bilinear taps at
  0.35 × reach at the bake height, each weighted by the emitter's own normalized field
  (`forceContributionCpu`), force = Output × mean(w × −∇), pressure = mean(opposing) — same units,
  same "Force gain", ~3 frames latent. **No GPU work per emitter, and a merged member needs no
  upload at all.** `setAnalyticReadback(true)` (`AnalyticReadback true` in a `Component Force`)
  keeps the GPU integral for bubbles that leave the ground band — the projectile prefabs — by
  setting `FORCE_FLAG_READBACK`, the only slots `force_emitter.cs` does not exit on at once. The
  bake being disabled or unpublished falls everyone back to the GPU path.
* `getEquilibriumRadius()` — the pressure-aware CURRENT bubble radius: the closed-form iso profile at
  the widest station with the threshold raised from iso to `max(iso, pressure)`. **An AVERAGE** — the
  true surface sits closer on the enemy-facing side — and it inherits the readback latency. 0 = no
  bubble survives. The Game player's shield UI and damage logic read it.

**`ForceQuery`** — `createQuery(pos)`, `MAX_FORCE_QUERIES` 1024. `setPosition` is pass-safe, so a
query can RIDE a moving unit. **No game consumer holds one any more** — the player's territory /
density readout and the structures' territory drain are `sampleBakedField` taps (its `field` is the
strongest team's φ, the density readout). The testbed's debug query spawner is the remaining user.

`Result{ owningTeam, inside, ownField, opposingField, valid }`. **`ownField` is written even when the
point is inside no bubble**, so it doubles as the density readout — `density()` returns it, and the
debug density view heat-maps it.

### Active gate

`setActive(false)` keeps the INSTANCE and every parameter but projects NO field this frame: **its
renderer slot goes back** (skipped by the grid, draw, compute and bake), no readback (force and
pressure read zero), evicted from merging and never a merge candidate. The next `update()` that sees
it active mints a fresh slot and uploads it in the same frame. Pass-safe like `setOutput` — the
setter only writes the bool, and the slot churn itself runs serially in `update()`.

**The World's SIM LOD drives it by tier** — "Game/Sim LOD/Force bubbles max tier", default 1. See
[`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

## Live team count

`setNumTeams(2..8)` is a **GAME-MODE setting, not a tweak** (co-op = 2, PvP default 8). Call it
before the mode's world spawns.

`MAX_FORCE_TEAMS` (8) stays the CAP: the UBO colour array size, and the "outside every bubble"
sentinel.

The live count rides `ForceFieldParams::numTeams`, and the renderer detects the change in
`setForceFieldParams` — one device idle, the `useGrid`-toggle pattern — then:

* recompiles every force shader with the per-pipeline `NUM_FORCE_TEAMS` define, so all per-team loops
  and phi arrays size by it (**2 teams = a quarter of the 8-team accumulation**);
* remakes the team-sized resources: the shell volume (ONE RGBA16F 3D texture at ≤ 4 teams, two at 5+)
  and the CPU-bake readback.

> The shell volume is **deliberately never RG16F**: `rg16f` image stores need the
> `shaderStorageImageExtendedFormats` feature the engine does not enable. At ≤ 4 teams the second
> view slot is null and bindings fall back to view A.

Emitter and query team values clamp below the live count.

## The baked pressure field

"Force/Bake" tweaks. **THE readback path for every ground consumer**: swarm units, the player's and
structures' territory, and — through `bakedReadback` — every emitter's own force/pressure. Any
number of consumers sample field force and exposure with a plain bilinear tap and NO per-consumer
GPU slot; `force_emitter.cs` only integrates `AnalyticReadback` emitters (projectiles).

1. `buildBakeChunks` selects at most `MAX_FORCE_BAKE_CHUNKS` (512) **16 m XZ chunks** from the live
   emitters' and groups' support boxes — conservative output-line AABBs plus transition and group
   spheres. 512 chunks cover ~131k m². Hitting the cap printfs once, bumps the `Chunks (stat)` tweak,
   and dropped chunks read zero.
2. `force_bake.cs` evaluates **16×16 samples per chunk** on a corner-aligned world lattice at 1 m
   spacing — so unit-shield bubbles stay resolved — holding ALL team φ values per sample, at
   "Sample height" (default 1 m, where bodies live).
3. `publishBake` copies the slot's readback **WITH the chunk list it was evaluated for**
   (`ForceBakeReadback` pairing; per-slot lists are stored at upload).

`sampleBakedField(pos, team)` → `FieldSample{ valid, inside, owningTeam, field, opposing, opposingGradient }`
from ONE 2×2 bilinear fetch. The gradient is the analytic derivative of the bilinear patch, and it is
planar (XZ).

**A position outside every chunk reads as ZERO field — correct by construction**, since the chunks
cover every support box. Worker-safe between updates; ~3 frames latent end to end.

## `update()` — the main-thread pass order

| Scope | Work |
|---|---|
| `"Force merge join"` (Wait) | Normally already joined at the loop top; a guarantee, not the expected path. |
| `"Force prepare"` | Params push, plus destroying the merge job's retired group slots. |
| `"Force upload"` | A per-emitter `parallelFor` (grain `c_uploadGrain` = 64): distinct renderer slots, read-only readback span, the renderer's per-worker debug lines. An emitter whose ACTIVE gate flipped only STAGES its index in its chunk's `SlotChurn` slot (owner-sliced, see below). |
| `"Force slots"` | **Serial.** Retires the staged slots, then mints slots for the newly active ones and uploads them, then mints the slots of groups founded on the merge job — the renderer's create/destroy grow vectors the upload jobs index, so they cannot run inside them. |
| `"Force groups upload"` | A per-group `runPass` (grain 16, inline under 32): own slot, own readback, and in shared-readback mode its OWN members (an emitter belongs to at most one group). The sphere fold is `forceSphereFold()` — a namespace-scope constant, since a function-local static is a race under `/Zc:threadSafeInit-`. |
| `"Force queries"` | Query positions up, results latched. |
| `"Force bake"` | `"Force bake boxes"`: a per-emitter `runPass` (grain 256, inline under 512) rasterizing support boxes into a `PerWorker` **stamp-cleared open-addressing key set** (`BakeKeySet`: no per-frame clear, no sort — a first-seen key lands in the slot's `unique` list); then a serial merge of the slots' unique lists through one more such set, capped at `MAX_FORCE_BAKE_CHUNKS` by keeping the chunks nearest the covered set's CENTROID (**never the sorted tail: the key packs `bx` as uint32, so negative X sorts last and a tail cut would blank a whole half-plane — units there stopped being pushed**). `"Force bake publish"`: the paired readback copy fans out per chunk (`"Force bake copy"`) into a buffer sized ONCE to the cap; the lookup is a **dense uint16 grid over the chunks' bounding box** (O(1) per bilinear corner; a sorted-key binary search past `MAX_BAKE_GRID_CELLS`), built in O(chunks). |
| `"Force merge kick"` | Submits the merge job. |

> **"Force gain" applies on the CPU here, not on the GPU.** The compute writes the raw integral, so
> changing the gain does not need a recompute.

## Renderer side (`RendererVK:ForceFieldPipeline`)

`MAX_FORCE_EMITTERS` is 8192 (64 B each) — **held by ACTIVE emitters only**, see the Active gate.
Emitter and query slots are main-thread with a free list and retirement; readbacks are slot-indexed,
so slots never re-pair with stale results. **That retirement window is what makes the gate's slot
recycling safe:** a slot released this frame cannot be re-handed out until every frame that could
still deliver its readback has drained.

### The upload partition

`ForceFieldPipeline::upload` classifies every ACTIVE slot ONCE, in one sweep with one `shellVisible`
test per emitter, into:

```
[ sampled-tier drawable | analytic drawable | non-drawn field | PASSIVE tail ]
```

and sets the draw `instanceCount`s from the bucket sizes. **Compute dispatches keep the full
counts** — a non-drawn emitter still has a field.

### Shell draw culling

Upload-time CPU, **desktop only — VR skips it**, because the centre frustum is the wrong eye's.

A drawable shell outside the centre-view frustum, or whose projected proxy radius is under
"Min screen radius (px)" (3), compacts into the non-drawn partition: field, grid and readbacks
untouched, only the ray-march draw skipped. The bounding sphere mirrors `forceEmitterBounds`, and
camera-inside never size-culls.

### March LOD

The FS's step count tapers linearly with the proxy's projected size below "Full-detail radius (px)"
(160), floor 8. 0 = off.

### Sampled shell tier

"Sampled tier radius (m)", default 5; 0 = off.

Emitters whose **VISIBLE bubble radius** is at or above it march a BAKED FIELD VOLUME instead of the
analytic candidate loop.

> The metric is `forceEmitterVisibleRadius` — the iso-shrunk draw box's bounding half-extent — **NOT
> the authored Reach**, which the merge's group and transition spheres made a near-constant across
> all shield sizes. The SAME metric drives the upload partition, the bake-volume fit and the union's
> ownership skip.

* Two RGBA16F 3D textures, `FORCE_SHELL_VOLUME` 128×48×128, holding all team φ. ONE set, serialized
  by an acquire barrier, GENERAL for life.
* Written by `force_shellbake.cs` each frame — indirect dispatch, x = 0 when no emitter qualifies, so
  the CB is cached — with the FULL analytic field, small-bubble deformation included.
* Refit each frame in `buildUboForce` over the union of the large DRAWABLE emitters' support boxes,
  **CLIPPED in XZ to the camera's view footprint** — the four corner rays hit the union's height
  band, their XZ box plus "Volume view margin" (10 m; 0 = unclipped) bounds the fit. **Fixed texels
  mean the resolution self-adjusts**, and with the clip it follows the ZOOM rather than the spread
  of every large bubble in the world (one 7 m bubble 100 m off-screen used to halve a 40 m shell's
  resolution — which is why raising the tier threshold looked like a smoothing control). Outside
  the fit the volume reads clamp-to-border black = zero field; that boundary lies outside the view
  by construction. A corner ray that misses the band (free-fly camera at the horizon) or VR leaves
  the union unclipped.
* On this tier the hit BISECTION and NORMALS also read the volume — **the surface being refined IS
  the trilinear field, so its gradient matches exactly** — while ownership dedup and the shading
  colour/alpha accumulate stay ANALYTIC, since they need per-emitter identity and shell alpha the
  volume does not carry.
* **Trilinear iso-surface error is O(h²·curvature)**: centimetres on large bubbles, which is why
  SMALL bubbles stay analytic — the fixed-size volume cannot resolve them.
* Draw set bindings 5/6 are the volumes.

### Union march

"Union march" on by default, with "Union step (m)" 1.5 and "Union max steps" 8. Desktop only, and the
density debug view forces it off.

**The ANALYTIC tier becomes ONE march per pixel:**

1. The analytic proxies draw ONLY into an interval pass — its own RG16F render pass in the primary
   before the scene stages ("Force intervals"): shell VS + `force_interval.fs` **MIN-blend**
   `(tEntry, −tExit)` per box (`blendOp eMin` is a `GraphicsPipelineLayout` field), cleared to
   fp16-max, ending SHADER_READ_ONLY.
2. A fullscreen triangle (`composite.vs` + `force_union.fs`, indirect vertexCount 3 or 0) marches
   each covered pixel's union interval ONCE, `steps = length/stepSize` capped. **The step GROWS with
   distance so far pixels never march finer than ~2 px.**

That kills the overdraw × march term where small bubbles stack.

**Empty-space skipping:** a force-grid cell with no candidates PROVABLY holds ZERO field — every
emitter inserts into every cell its support overlaps — so the sample is a constant and the index
jumps past the empty stretch **with the bracket start moved there** (a bracket spanning the skip
would cost the bisection its accuracy at the next bubble's entry).

**Half resolution** — "Union half res", on by default, a REBUILD-CLASS toggle like Use grid: flipping
it idles the device, re-sizes / creates or destroys the targets, and re-injects
`FORCE_UNION_UV_SCALE`.

* Interval and march targets are swapchain/2; both FS map `gl_FragCoord` back to full uv with the
  injected scale, and the primary passes halved viewport and scissor.
* The march renders into its own RGBA16F pass ("Force union march", cleared 0, no blend, ending
  SHADER_READ_ONLY), recorded right after the interval pass where gbuffer depth is still
  SHADER_READ_ONLY.
* The `"Force union blend"` scene stage (`force_union_upsample.fs`) composites it **depth-aware**:
  2×2 bilinear weights × relative depth similarity, where each half texel's representative depth is
  the full-res depth at its own march uv — the exact value it clamped against — with a nearest-depth
  fallback when all four sit across a silhouette.
* OFF is the pre-half-res path exactly: full-res interval target, the march draws directly into scene
  colour, and NO march framebuffer exists.
* **VR is untouched** (its union indirect is 0). In half-res mode the march pass still runs as
  clear-only, so the march image's layout transition always happens.

**Other union details:** the cell hash probe is cached per cell segment (`forceSampleFieldCell`
re-probes only when a step crosses a 16 m boundary); "Union jitter" (OFF by default, rebuild-class —
the define compiles it out) turns step banding into static per-pixel spatial noise, with no temporal
term so nothing shimmers with TAA off; both marches break on `accumAlpha ≥ 0.98`; and the union's
bisections run 5 iterations where the shell FS keeps 6 for cross-proxy hit matching.

### Draw-box shrink

"Visible bounds iso frac", default 1.0; 0 = off.

`packVisibleBounds` (ForceSystem.cpp, inside `buildEmitterGpu`) packs each emitter's closed-form OWN iso
extent — evaluated at iso × the frac, the merge slack for two sub-iso fields summing past iso — into
`teamFlags.w`. `forceVisibleBounds` then shrinks the proxy VS, interval FS and shell FS march boxes,
**so a weak bubble in a full-size reach box costs its actual size.**

**The FIELD keeps the full support everywhere** — grid insert, bake fits and CPU mirrors all stay
`forceEmitterBounds`.

### The draw and the compute

Rendering is one instanced draw of unit cubes (the decal pattern) whose FS ray-marches the field
through the emitter's reach box, bisection-refines, and does a manual reversed-Z depth test.
**OWNERSHIP DISCARD dedups merged bubbles.**

Shading is the shared include `force_shell_shade.inc.glsl`; `forceShadeHit` / `forceShadeWall` take
an `ownerIdx` (per-proxy `v_emitterIdx`, or the crossing's dominant emitter in the union pass). **The
union pass needs NO ownership discard** — one march owns every crossing — it only skips crossings
whose dominant emitter is sampled-tier, since those proxies draw themselves. With the union off
(VR or tweak) the proxy draw spans both partitions, exactly the old path.

`recordForceCompute`, after the light grid:

* A uniform emitter hash grid per frame at `FORCE_GRID_CELL_SIZE` **16 m** — the FORCE grid's OWN
  size, finer than the shared `hash_grid` `GRID_SIZE` 32 the light grid keeps, because gather cost
  scales with small emitters per cell and a swarm packs dozens into a 32 m cell. **EVERY emitter
  inserts** — there is no big-emitter bypass list any more, which is what makes "an empty cell holds
  zero field" provable.
* `force_emitter.cs` integrates opposing pressure with a **13-sample integral**; a centre-only tap
  reads zero when big bubbles press rims.
* `force_query.cs` evaluates the query points. Readbacks are host-visible.
* "Use grid" off = brute-force scan of every emitter per evaluation — the A/B correctness check.

## Emitter merging

`ForceSystem::updateMerging`, "Force/Merge" tweaks.

### What merges

Emitters that are **mergeable and have a bubble above iso** cluster per TEAM into persistent
`MergeGroup`s, each carried by ONE GPU sphere emitter (focus 0.5, axis up).

> **Mergeable is DEFAULT ON for every emitter.** `setMergeable(false)`, or `Mergeable false` in a
> `Component Force`, opts out. **A merged group is always a SPHERE**, so a cone-shaped Lance that
> merges loses its shape — opt such prefabs out.

Cover math:

```
memberCover(m, centre) = spreadScale·|m.bubbleCentre − centre| + radiusScale·m.bubbleRadius
groupRadius            = coverScale · max over members + coverMargin
```

The SAME formula feeds the target, the Merged-member floor, the Joining completion test and the
max-radius checks, so the guarantees stay consistent with whatever scaling is chosen. 1/1/1 covers
every member bubble exactly; below 1 hugs the crowd tighter at the price of member rims sticking out
near the edge.

Group output = `max(largest member's centre density, sum × sumFraction)`, so **the merged bubble is
never fainter than a member**, and reach is solved from the sphere's closed form (`sphereReach`) so
the visible radius equals the cover exactly.

The 16-station bubble profile is CACHED and only re-evaluated when a shape parameter or the iso
threshold changed — **a moving unit just translates the centre.**

### Hysteresis

* **Join** when the bubble centres are within `joinDistance × (rᵢ+rⱼ)`.
* **Leave** when NO other member is within `leaveDistance × (rᵢ+rⱼ)`. Below 1.0 on purpose, so the
  member's own bubble reappears while it still overlaps the group's cover — **a unit is never exposed
  on the split frame.**
* Also leaves on a team change, a lost bubble, a cleared mergeable flag, or drifting past
  `maxRadius × 1.1`.

### Off the main thread, pipelined a frame

`update()` kicks the whole `updateMerging` as ONE Normal job AFTER the upload, over this frame's
post-sim emitter state. `joinMerge()` joins it at the main-loop top, **right after the UI join and
before input or entity-change drains can create or destroy emitters.**

This is the UI-job pattern: it runs during present and the fence wait, where main has nothing to
overlap it with otherwise. (Kicking after `world.update` and joining in `update()` measured as a pure
0.06 ms main-thread wait — main had nothing between them.)

**Consequence:** the group spheres and member transitions the next upload uses are ONE FRAME behind
the emitters. That is centimetres at unit speeds, inside the cover margin, and every membership rule
has hysteresis.

**The job NEVER touches the renderer** (`present()` reads the force slot vector in that window):
`createGroup` leaves `rendererSlot` UINT32_MAX and `update()` mints it on main; `dissolveGroup` pushes
the slot to `m_retiredGroupSlots`, which `update()` destroys.

### The passes

`runPass(count, grain, minParallel, profile, fn)` is inline under `minParallel` items — **a
parallelFor's submit + wake + join costs more than a handful of items** — and a `parallelFor` above
it. The work is **PARALLEL AND SCALED BY CANDIDATES, NOT EMITTERS**.

**Staging is OWNER-SLICED, not `PerWorker`:** a pass that stages results gets ONE SLOT PER CHUNK
(`passSlots(count, grain, minParallel)` = 1 inline, else `JobSystem::numChunks`; `prepareSlots`
grows the slot vector with capacity kept and clears the slots in use), `fn` indexes its slot by
`begin / grain`, and the serial drain walks exactly that many. Memory therefore scales with the
pass's item count, not the machine's context count, and no thread-local state is involved — the
merge passes run inside the merge job, whose `parallelFor` joins park the fiber. The one
`PerWorker` left is the bake `BakeKeySet` (a fixed 4096-entry hash table per slot — few, full slots
dedupe best; see ForceSystem.ixx).

| Pass | Kind | minParallel | What |
|---|---|---|---|
| `"Force merge bounds"` | per emitter | 256 | Refresh the cached profile. A CANDIDATE = mergeable + has a bubble + own cover term under `maxRadius`, staged in the chunk's slot with a CAS-max of the largest join radius (float bits — positive floats order as uints). |
| `"Force merge leave"` | per group | 8 | Touches only its own members. |
| `"Force merge cells"` | **serial** | — | Candidates → `(cellKey, idx)` sorted by key. **Cell = 2 × the largest join radius, so a 3×3×3 neighbourhood holds every possible partner.** |
| `"Force merge neighbours"` | per candidate | 128 | Binary-search the 27 cells, exact test `joinDistance × (rᵢ+rⱼ)`, pairs `i<j` staged in the chunk's slot. |
| `"Force merge union"` | **serial** | — | Ungrouped pair → new group; lone + group → join if it fits; two groups → merge smaller into larger. Serial because of the renderer slot API and cross-group membership moves. **Its cost is the PAIR count.** |
| `"Force merge cover"` | per group | 8 | `recomputeCover` targets plus the `dissolve` flag. |
| `"Force merge dissolve"` | **serial** | — | The sweep. |

> `"Force merge neighbours"` REPLACED a `SpatialIndex::querySphere` per emitter, which walked the
> entity hierarchy's render-entry-filled cells and cost **1.6 ms/frame on main for ~100 emitters**.
> The private candidate cell list costs 0.04 ms.

Groups under `minMembers` dissolve; `Enabled` off dissolves everything.

### Smoothing

`recomputeCover` writes a TARGET sphere. The DISPLAYED one:

1. **FOLLOWS the members' own motion 1:1** — the output-weighted mean displacement of the members
   already in the group last frame, from `prevBubbleCenter`. A sphere merely eased on a time constant
   trailed a marching crowd, and the Merged-member floor then inflated it forward, leaving a void
   behind.
2. Then eases the residual — membership jumps — over `smoothTime`.
3. **Floored by the cover of the Merged members at the displayed centre.** They have no field of
   their own, so that floor IS the coverage guarantee.

### Membership state machine

**Own → Joining → Merged → Leaving → Own.**

* While **Joining or Leaving** the member uploads an ACTIVE sphere of its OWN output whose centre and
  visible radius lerp (smoothstep over `blendTime`) between its own bubble and the group's displayed
  sphere. A linear lerp of centre and radius keeps the unit inside the sphere at every step.
* **Leaving** starts between the own bubble and the group sphere per `leaveFromGroup` — 1 = a full
  ghost copy of the group sphere shrinking onto the unit, which read as an empty bubble left behind —
  and slides to the unit.
* **Joining** starts at the unit's bubble and only completes (→ Merged / PASSIVE) once the displayed
  group sphere actually covers it.
* A new group grows out of its larger founder's bubble. A dissolving group starts a Leaving — or
  Joining into the absorbing group — sphere for every member from where its sphere stood. A reversal
  mid-transition restarts from `blendCenter`/`blendRadius`, the sphere uploaded last frame.

### A Merged member projects no field

* A BAKE-READ member (the default) uploads nothing (flags 0, compacted out): its taps read its own
  position against the group's field, which is what unit shield logic wants.
* An ANALYTIC member with `memberReadback` (default on) uploads `FORCE_FLAG_PASSIVE | READBACK`,
  compacted into a tail past `fe_count` (header `evalCount`) that **only `force_emitter.cs`
  evaluates**, for the same own-position truth.
* `memberReadback` off = the group sphere integrates (`FORCE_FLAG_READBACK` on the group) and its
  readback is split by output share among the ANALYTIC Merged members; bake-read members are
  untouched.
* **Every getter and setter keeps working while merged**; `isMerged()` tells.

## `ForceEmitter::setShellAlpha`

`outputParams.y`, default 1. **0 SKIPS the ray-marched shell draw entirely while the field stays
live** — it still deforms other bubbles and serves force, pressure and query readbacks. The intended
use is a huge invisible field whose proxy box would otherwise become a full-screen march.

**Invisible fields also never TINT other shells.** Shading weighs team colour and contact glow by
alpha-scaled fields (`forceAccumulateVisible`), and an equilibrium wall against an invisible side
reclassifies as the visible team's SKIN — shaded and owned by that team — because the invisible proxy
never rasterizes, so pane-classified walls would silently drop.

> Note a map-scale emitter now floods grid cells, since the big-emitter bypass list was removed.
> "Use grid" off is the fallback for such authoring extremes.

## `ForceComponent` (lives in Entity)

`Component Force` in a `.pre`: `Team` / `Output` / `Reach` / `Focus` / `Distribution` / `Width` /
`Centered` / `Mergeable` / `AnalyticReadback`, plus local `Direction` / `Offset`. Demo:
`Entities/SphereField.pre`.

* Keeps its emitter on the entity's world transform. **Not frozen-gated** — placement, not
  simulation.
* `Centered` (default true) pulls the emitter back `Reach/2` so the bubble centres on the entity.
* **Reach is in world units, unscaled by the entity.**
* No authored on/off — scripts use `setOutput(0)`.
* Script access is the `entityGetForceComponent` / `forceGet*` / `forceSet*` handle ABI, exposed in
  the DSL as `self.force.*`.

## Tweaks

| Group | Entries |
|---|---|
| `Force` | Enabled, Iso threshold (0.15), March steps (10), Use grid, Force gain (5), Emitters + GPU slots (stats) |
| `Force/Bake` | Enabled, Sample height (1 m), Chunks (stat) |
| `Force/Shell` | Alpha (0.5), Min screen radius (3 px), Full-detail radius (160 px), Sampled tier radius (5 m), Volume view margin (10 m), Union march, Union half res, Union jitter, Union step (1.5 m), Union max steps (8), Visible bounds iso frac (1.0), Interior alpha, Backface alpha, Rim power (3), Rim intensity (1.5), Junction smoothing (0.5) |
| `Force/Glow` | Contact intensity (0.33), Contact width (0.15), Contact wall alpha (0.5), Geometry distance (0.5 m) |
| `Force/Pattern` | Scale (0.6 /m), Scroll speed (0.3), Intensity (0.5) |
| `Force/Teams` | Per-team shell colour |
| `Force/Merge` | Enabled, Join distance (0.5), Leave distance (0.85), Cover spread/radius scale (1/1), Cover scale (0.85), Cover margin (0.2 m), Max group radius (**8 m**), Max members (255), Min members (2), Summed output fraction (0.5), Member readback, Smooth time (0.3 s), Blend time (0.5 s), Leave from group sphere (0.5), Groups + Merged emitters (stats) |
| `Force/Debug` | Draw emitters, Draw merge groups, Draw queries, Density view, Log tier classification, Density range (2) |

`junctionSmoothing` is a smooth-max width rounding the crease where shells meet the equilibrium wall.
**Queries use the same function, so the gameplay inside-test always matches the drawn surface.**
