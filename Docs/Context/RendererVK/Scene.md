# Scene data: instances, meshes, streaming, LODs

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Per-frame instance flow

**`renderNode(node, passMask)` is LOCK-FREE**, callable from any job between `beginFrame` and
`present`: atomic cursor claims, monotonic.

> On an overflow frame the tail that no longer fits **drops for one frame**, `m_instanceOverflowStart`
> CAS-min records it, `present()` clamps (`clampInstanceCountToOverflow`), and capacity regrows at the
> next `beginFrame` (`checkFrameCapacities`). **NEVER roll a bump cursor back.**

`addLightInfo` / `addFogVolume` / `addPointLight` / `addAreaLight` / `addSpotLight` / `addDecal` are
lock-free too. **`addDebugLine` is `PerWorker`-staged**; `present()` hands the `PerWorker` to
`DebugLinePipeline::upload`, which copies each worker list straight into the slot's mapped vertex
buffer (one memcpy per list, no merged CPU copy) and clears it.

**Pass masks** `PASS_MAIN` / `PASS_SHADOW` / `PASS_GI` (Layout.ixx — the same bits Spatial uses). The
GPU word is the mask byte plus the PUSH FRAME above it (`InstanceStream::stampedPassMask`; the motion
vectors' "was this node pushed last frame" test), so a reader tests bits, never the whole word. Main
cull, shadow cull and the GI TLAS writer each early-out on their bit; TLAS also range-bounds by
`RT/TLAS Range`. **A node carries its own default mask** (`RenderNode::setPassMask`, a byte in the
padding, `PASS_ALL` unless set): `renderNode(node)` pushes with it, `renderNode(node, mask)` overrides
it. `renderNode` returns at once for a 0 mask and for a node with no instances (never spawned, or
destroyed — `freeRenderNode` clears them), so an owner can push a list of node pointers blindly.

**GPU stats atomics are debug-build only:** `buildLayoutPreamble` defines `SHADER_STATS` under
`#ifndef NDEBUG`, and the main cull's per-level LOD pick counter (`out_lodStats`) is written only under
it. The buffer stays bound in every build; the readout (`m_lodInstanceCounts`, snapshotted in
`beginFrame`) reads zeros in RelWithDebInfo / Release. Put any new per-instance stats atomic behind the same define.

**Transforms upload SPARSELY, inside `renderNode`:** write only through `RenderNode::setTransform`,
which is change-detected into the node's own dirty bits (one per frame in flight). `renderNode` copies
the transform into its slot's mapped buffer when that slot's bit is set, so there is no dirty list and
no merge in `present()`, and a node that moves while it is not pushed uploads nothing until its next
push. **A mutable `getTransform` would bypass the tracking.** One owner per node: `setTransform` and
`renderNode` of the same node must not run concurrently. A re-creation of the node buffers bumps
`InstanceStream`'s buffer generation (`getBufferGeneration`); a node with an older generation counts as
all-dirty at its next push.

`IndexRangeFreeList` recycles the contiguous slot ranges freed by destroyed ObjectContainers — mesh
infos, materials, instance offsets, skinning jobs. Ranges stay sorted and coalesced, and **allocation
is BEST-FIT so small requests do not shred the large holes.**

---

## Scene objects

`ObjectContainer` wraps one loaded `ISceneData`.

* `spawnNodeForIdx()` returns a move-only RAII `RenderNode`; `spawnSkinnedNode()` the GPU-skinned
  variant, where AnimatorComponent feeds the bone palette through `allocateSkinningPalette` /
  `setSkinningPalette`.
* **`RenderNode` is ONE CACHE LINE** (`static_assert`): transform slot, skinned bundle handle, LOD
  state base, the upload-state byte, the default pass-mask byte, local bounds and the mesh-instance vector. Everything else the
  push needs is derived from renderer tables: per-mesh instance counts from the instances themselves,
  an instance's LOD chain from `MeshLodRegistry::getGroupIdxForMesh` (instances reference the LOD0 mesh), the
  skinning palette from the bundle (`setSkinningPalette(node, palette)`). Do not add per-node side
  vectors.
* **Destroying a RenderNode recycles without GPU sync**: transform slots are free-listed, skinned
  bundles parked in place per container, and rebased sub-node offsets shared.
* **The per-entity spawn allocators are LOCK-FREE** (`Util/SlotAlloc.ixx`: `SlotStack` - a Treiber stack with
  a 32-bit ABA tag in the head; `PagedArray` - 4096-element pages that never move, only a new page takes a
  mutex; `SlotRangeAllocator` - a bump cursor plus one free stack per range size up to 64, never split or
  merged):
  * **transform slots** (`TransformSlots`, `Globals::renderNodeTransforms`): paged, so a spawn never
    reallocates under another worker's `setTransform`. The high-water mark sizes the node buffers.
  * **LOD state ranges** (`MeshLodRegistry::allocateStateRange`): NEVER grow mid-frame (a mid-frame growth
    waits for the GPU on a spawning worker). `renderNode` skips a node whose range is past the capacity, and
    `checkFrameCapacities` grows it (`growStateToPendingDemand`), like the transforms.
  * **emitter / query slots** (`RecycledSlotTable`): storage sized to the capacity once; a retire goes into its
    frame's bucket (`frame % (NUM_FRAMES_IN_FLIGHT + 4)`), and `beginFrame` (`recycleEmitters` /
    `recycleSlots`) moves the drained bucket onto the free stack.
  * **solid-colour tints**: a lock-free open-addressed mirror of the cache answers every hit; only a new
    colour locks - its own `m_solidColorMutex` (the 1×1 texture upload takes the TextureManager and staging
    mutexes under it), with `m_spawnMutex` only briefly inside `addMaterialInfos`.
  * `spawnNodeForIdx`'s rebased-offset cache is double-checked: only the first spawn of a sub-node locks.
  * **skinned despawns** (`releaseSkinnedBundle` -> `SkinnedMeshRegistry::queuePark`): the bundle handle goes
    on a pending `SlotStack`; `beginFrame` (and `removeObjectContainer`, first) parks them all under ONE lock
    (`drainParks`). Until then the dead bundle's skinning job and BLAS rebuild still run (≤ 1 frame, never
    drawn), and a same-frame respawn builds a fresh bundle instead of reusing it.

  So a static spawn takes NO lock after warm-up, and a despawn none at all. **Still under `m_spawnMutex`:**
  `spawnSkinnedNode` (the bundle acquire / build and the instance fill only), the shared tables, LOD groups.
* **Every shared-table RELEASE takes `m_spawnMutex`**, like the adds (`freeMeshInfoRange`,
  `removeObjectContainer`, `releaseMaterial`, `freeMeshLodGroup`): an unlocked release races a spawning job's
  add on the free-list vector.
* `~ObjectContainer` frees ALL renderer resources (`Renderer::removeObjectContainer`).
* The container keeps its own copy of the source `Skeleton`, **so animators can retarget against it at
  spawn.**

### Single meshes (`RenderMesh`)

For generated geometry that lives and dies one mesh at a time with a SHARED material (terrain chunks,
ocean sectors), a container is pure overhead: its own material slot, node tables, name strings and
path map per mesh. `RendererVK:RenderMesh` is the lean path (main thread):

* `RenderMeshData::build(MeshGeometryDesc)` — pure, from any job: the final `MeshVertex` array (the
  container's tangent-handedness formula and null-attribute defaults), the indices, the bounds.
* `createMeshMaterial(pipeline, rayTraced)` — one permanent material per SYSTEM (fallback textures,
  opaque, `MATERIAL_FLAG_OCEAN` / `_TERRAIN` by pipeline, `NO_RAYTRACING` unless ray traced, BC5 flag
  from the fallback normal's format — exactly what a container override produces).
* `createMesh(data, raytraced = true)` → a move-only RAII `RenderMesh`: the vertex/index uploads and ONE `MeshInfo`
  (through `addMeshInfos`, so the BLAS registers like any mesh). No LOD chain, no stream set. Offsets
  and counts are stored as 32-bit element units. `raytraced` false builds NO BLAS (`RayTracingScene`'s
  `m_noStaticBlas`, the skip the skinned output regions use too) - any TLAS instance of it is inactive (no BLAS
  address). For meshes RT never has to see: the trees' meshes / cards, the bushes (Procedural TreeSystem).
  **"No BLAS address" = the entry reads 0, and that holds only because the BLAS address buffers are ZEROED at creation
  and on growth** (`AccelerationStructure::zeroBlasAddresses`): an entry is written only by a BLAS build / free /
  eviction, and fresh buffer memory is undefined (recycled allocator memory after a mesh-capacity growth holds stale
  bytes). Unzeroed, the TLAS writer takes the garbage for a BLAS reference -> DEVICE LOST in the TLAS build /
  traversal, reported by the next `vkQueueWaitIdle` on any thread. A streamed-out mesh before its first stream-in has
  the same exposure.
* `spawnMeshNode(mesh, material, pipeline, transform)` → a plain `RenderNode` with one instance on a
  shared identity instance offset (`m_identityInstanceOffsetIdx`, created at the first spawn). The
  instance's alpha mode is the MATERIAL's (the TLAS writer's opacity flag reads it).
* **WORLD TREE RECORDS** (`TreeRecordPool`, Data/; `resetTreeRecords` / `addTreeRecordChunk` / `removeTreeRecordChunk` /
  `updateTreeRecords`, main thread after beginFrame; Procedural TreeWorld): every
  terrain chunk's 4-byte tree records - right after its GROUND (`TREE_RECORD_HEIGHT_RES`² = 17² heights over the chunk,
  16-bit above its minimum, `TREE_RECORD_HEIGHT_WORDS`; every chunk has one, trees or not) - in ONE fixed-size
  HOST-VISIBLE sequential-write pool (ReBAR when the device has it; `Trees/World/GPU pool (MB)`, 128; 64-record
  blocks, best-fit `IndexRangeFreeList` + a bump top) and a TABLE of the resident chunks (`TreeRecordChunkGpu`: coord,
  first, count; host-visible per frame slot, rewritten for the current slot when changed). **The pool never grows**: a
  chunk that does not fit is refused (`stats().refused`); only a size change drains the GPU. A chunk's range is
  written once by a direct memcpy into blocks no frame reads - **NOT through the staging ring**: a full ring submits
  and waits a fence under the staging mutex, and the TreeWorld pool job runs on main whenever main helps at a join
  (spikes of tens of ms). A removed chunk's blocks are reused NUM_FRAMES_IN_FLIGHT frames later, when every frame whose
  table listed it is done (later while a far-volume bake that started before the removal still runs: it works from a
  table snapshot). The far-tree volume's bake reads them ("Far-tree volume" ([Vegetation.md](Vegetation.md)) / WORLD
  TREE RECORDS), and the pool holds the record types (`setTreeRecordTypes`, drains the GPU - a world spawn). The CHUNK
  MAP (`TreeRecordMapGpu`, per frame slot with the table): a toroidal mapSize² grid (a power of two over the ring's
  diameter; `resetTreeRecords(bytes, ringRadius)`), indexed by the chunk coordinate & (mapSize - 1), holding the
  coordinate (checked) and its ground's pool word: any position -> its ground (`treeRecordGround`,
  tree_record.inc.glsl).
* **BAKED TREE RECORDS** (`createTreeInstanceSet` / `renderTreeInstanceSet` / `destroyTreeInstanceSet`,
  RendererTrees.cpp + `tree_cull.inc.glsl`): a SET of placed procedural tree pieces sharing a table of piece
  TYPES (bark / bark-fade / leaves / leaves-fade / billboard representations + the crossfade band), uploaded
  ONCE to DEVICE-LOCAL buffers (`TreeCullPieceGpu` 64 B: transform + band centre/radius + type;
  `TreeCullTypeGpu` 96 B, with the mid tier's records + distance / band and a `shadowDistance`: beyond it from
  the cascades' centre the shadow cull drops the piece from every cascade, before the transform loads - the
  bushes, a texel or less in the far cascades). Trees do not move, so nothing about them is written per frame: no render
  nodes, no stream entries, no pass of their own (**do not rewrite tree records into the host-visible stream per
  frame**: an expand pass that did cost ~7.5 ms of GPU). **A set is split into CHUNKS**
  (`TreeInstanceChunk`: contiguous piece ranges - Procedural: one per terrain chunk).
  * **DYNAMIC sets** (`createDynamicTreeInstanceSet(types, capacity)` / `addTreeInstanceChunk` /
    `removeTreeInstanceChunk`, main after beginFrame; Procedural's world mode): the types fixed, a FIXED capacity of
    piece slots in blocks of 64 (no growth, no drain; a full set refuses the chunk), each chunk written once through
    the staging ring (`writeTreeChunk`: RT-capable first, bucket sizes, the volume's pieces); a removed chunk's slots
    are reused NUM_FRAMES_IN_FLIGHT frames later (its volume pieces go to the SENTINEL type at once - the volume types
    end with a res-0 one), its index from the next frame on. `m_treeSetMutex` covers the chunk table: main adds /
    removes while the terrain walk lists (`renderTreeInstanceSet`). The far volume never splats a dynamic set: the
    world's trees reach it through their records (WORLD TREE RECORDS above).
  * `bindTreeInstanceSet` (main, every frame) picks the set the recorded culls bind (a change re-records).
    `renderTreeInstanceSet(set, chunks, scale, forceFar)` - **ANY THREAD**, once per frame between beginFrame and
    present (Procedural calls it from the terrain's render walk) - takes the frame's drawn chunks with their PASS bits,
    writes the frame slot's host-visible LIST (`piece | passMask << 28` per listed piece, binding 22 / 15 / 11 in the
    main cull / shadow culls / TLAS writer), claims ONE instance range (4 FIXED records per LISTED piece -
    tree_cull.inc.glsl's slot layouts) and notes each listed chunk's precomputed bucket sizes (per distinct level-0
    mesh) — nothing per piece on the CPU but the list entry. The culls skip a listed tree without their pass bit (a
    chunk only in the shadow / GI sphere draws no main records; the TLAS writer wants `PASS_GI | PASS_SHADOW`). A
    second call in a frame asserts and is dropped (`m_treeCullTaken`). Destroying the bound set after this frame's
    claim keeps the range claimed with NO trees (`u_present_treeCount` = 0): the threads skip the range whole.
  * The UBO carries the range (`u_present_treeRangeBase` / `treeRangeLength`) and `u_present_treeFarScale` /
    `treeForceFar` / `treeVolumeStart` (the far-tree volume's start, 0 = no volume): **`UboPresent` values, uploaded
    in `present()` by `evaluatePresent`**, because the UBO uploads in beginFrame, before the claim (a 0 / 0 range made
    the culls read never-written entries as instances: garbage mesh indices, out-of-bounds bucket writes).
  * **The culls build the records, ONE THREAD PER TREE**: the dispatch covers the stream below the range, one thread
    per LISTED piece (`u_present_treeCount` = the list length), then the stream above it (`u_present_treeThreads` =
    the thread count; `treeCullThreadInstance` maps a thread to its instance, and a tree thread through the list to
    its piece and pass bits). A tree thread decides once — `treeCullMainPiece` (main cull: the mesh / crossfade /
    billboard / none decision from the distance to the centre view — the same rules as the CPU preview) or
    `treeCullShadowPiece` (shadow + rain-shelter culls: the BILLBOARD is the caster at every distance; a type without
    one casts its mesh) — and runs the per-instance cull for each record in `[kBegin, kEnd)` (record k at its first
    record + k). ONE call site of that body (`cullInstance` / `cullCaster`, the loop serves the plain instance with
    k = 0): each call site is a full inlined copy. **Both culls are `local_size_x = 64`**
    (`IndirectCullComputePipeline::update` dispatches ceil(threads / 64) groups; one thread per workgroup leaves 31 of
    32 lanes idle). A tree whose whole sphere is off screen returns before its records.
  * **THE MID TIER (main pass only; shadows / GI / RT keep the billboard):** a type with `midDistance` > 0 is a baked
    variant with its bark SPLIT - `trunk` / `trunkFade` (the trunk alone) and `bark` / `barkFade` (the module
    branches) - and a CARD mesh (`cardsIn` / `cardsOut`: every module placement's billboard cards merged into ONE mesh
    per variant over the species' card ATLAS, LitFoliage + `MATERIAL_FLAG_NO_EDGE_FADE`, bit 22). Over the mid band
    the branch bark and the leaves fade out while the card mesh fades in; the trunk stands; over the far band the
    trunk and the cards fade out while the billboard fades in. At most 4 draw at once, so a tree has 4 FIXED slots
    (mid: trunk, branches, leaves-then-billboard, cards; otherwise bark, leaves, -, billboard) - **no per-frame pool**
    (a per-frame atomic card pool ran out with a large grove + bushes, and an overflowing tree drew undithered). A
    merged card's crown frame cannot come from the instance origin (the tree's): its axis rides the tangent's w
    (RenderMeshData, `foliageCrownFrame` in the foliage FS).
  * **Far trees out of the near cascades:** the shadow cull keeps a tree record in cascade c only while
    `distance(centre, u_sceneFocus) − radius ≤ split(c) + margin` (`Trees/Foliage shadow cascade margin (m)`, 64,
    `u_present_treeShadowMargin`): a cascade's box runs far up-sun and takes in thousands of distant trees; the margin
    keeps the long shadows of trees just up-sun of its range. Tree meshes have NO LOD chains (Procedural uploads
    level 0 only): both culls skip the LOD group lookup for tree records, and no hysteresis slots are allocated;
    prev transform = none (w 0 — trees never move).
  * **A ROCK of the set** (Procedural's world rocks): a type with ONE mesh in the `bark` slot on `LitRock` - both
    culls know it by the record's pipeline index (`PIPELINE_IDX_LIT_ROCK`, a compare on a loaded word: no extra load
    for a tree record) and give it what a tree record lacks, the mesh LOD pick (STATELESS - `lodSelectLevel` with no
    last level: the record has no hysteresis slot), and withhold what a tree gets: the wind (the bound's sway growth,
    the shadow VS's bit 15 trunk bend). `treeCullRtPiece` and `initTreeSetTypes`' RT-capable test fall back to the
    bark record for a type with neither billboard nor leaves. A type without a billboard has no far-volume hand-over
    (`treeCullMainPiece` tests the volume's start for billboard types only): a rock draws wherever its chunk is in
    the set.
  * The buffers are bound at bindings 20/21 (main cull) and 13/14 (shadow culls) — descriptors written at RECORD
    time, so the culls bind ONE set (`m_treeCullSet`, the set rendered; changing it or destroying it re-records; a
    second set in one frame asserts). With no set, `m_treeCullDummy` is bound.
  * **In the TLAS** (GI, RTAO, RT shadows, the ocean / film reflections): the TLAS slots are the stream outside the
    tree range + ONE per tree of the list's **RT SECTION** (`treeCullTlasInstance`, the section length a push
    constant; `u_present_giTlasNumInstances` = stream + section, and the TLAS capacity grows by that demand,
    `m_giTlasDemand`). `createTreeInstanceSet` puts each chunk's RT-CAPABLE pieces first (a type whose RT
    representation has a BLAS - `RayTracingScene::hasStaticBlas`; bushes have none) with a sphere around them;
    `renderTreeInstanceSet` lists those of the GI / shadow chunks whose sphere reaches into `Trees/RT range (m)`
    FIRST, then everything else, which takes no TLAS slot. `gi_tlas_instances` (bindings 9 / 10 / 11) builds a tree's
    instance from the same static data with `treeCullRtPiece` - the billboard (a type without one: its leaves only),
    off beyond **`Trees/RT range (m)`** (default 600 m, `u_foliage_rtRange`, 0 = only "RT/TLAS Range") from the scene
    focus: every tree in the TLAS is an overlapping box every GI / shadow ray traverses. A mesh created without a
    BLAS (bushes) is inactive anyway. Since the range's stream entries are never written, it puts the MATERIAL in the
    custom index: **bit 23 set = a tree, bits 0..15 = its material** (a stream instance's custom index stays its
    index, below 2^23). Every ray-query consumer reads only the material from `in_instances[custom]`, so each takes it
    from the custom index when bit 23 is set (`rt_shadow.inc`, `gi_probe_trace`, `rtao`, `ocean.fs`,
    `terrain_film.fs`) - no extra binding anywhere. The geometry comes from the sbt-offset RT mesh index as for any
    instance; the billboards keep RTAO's mask 0x02.
  * Destroying a set drains the GPU (rare: respawn / reload). Texture / mesh streaming notes are skipped: tree
    textures are pinned and `RenderMesh`es never stream.
* `createMeshLodChain(levels, errors)` — a GPU LOD chain (`addMeshLodGroup`) over `createMesh`'d meshes,
  level 0 first, errors in mesh-local units (0 for level 0; nonzero → the screen-space-error selector).
  `spawnMeshNode` on a chain's level 0 allocates the node's hysteresis slot itself, and the cull redirects
  each instance like a container chain. `freeMeshLodChain` BEFORE destroying the meshes.
* `createTextureMaterial(w, h, mips, alphaCutoff, name)` — a material with its OWN generated sRGB RGBA8
  diffuse from a caller-built mip chain (`TextureManager::uploadRgba8Mips`; no blit-generated mips, so the
  caller can keep alpha-test coverage per level). Its textures are named `<name>/albedo` and `<name>/normal`: the
  VRAM view splits names on '/', so name a material `<group>/<item>` for one box per item. `ETextureEncoding` albedo /
  normal (default Rgba8): BC1 / BC3 / BC5 when the caller compressed the levels itself (Procedural's trees,
  `TextureManager::uploadMips`); a BC5 normal sets MATERIAL_FLAG_BC5_NORMAL. `alphaCutoff > 0` → `EAlphaMode::Mask`
  with the cutoff in `opacity` (the Mask discard's threshold); draw it on `LitMasked`. An optional `normalMips` chain
  uploads a LINEAR RGB tangent-space normal map (x along U, y along V); `extraFlags` adds material flags.
  `deriveMaterial(src, flags)` copies a material sharing its textures (free with `releaseMaterial`, the slot only);
  `get/setMaterialFlags` rewrite flags in place (no re-record). `destroyTextureMaterial` queues the textures' free and
  releases the material slot. Used by the procedural tree leaf cards and bark.
* `~RenderMesh` frees the ranges and neutralizes the `MeshInfo` slot (`freeMeshInfoRange`), like
  `removeObjectContainer` for an unstreamed mesh. **Destroy its nodes first** (owners declare the
  mesh before the node).

### Tree materials and foliage shading

**THE TREE VARIANTS**: the tree code lives in its own fragment shaders, so it never sets the register count of
LitOpaque / LitMasked (instanced_indirect.fs.glsl, which has no tree code at all). **`LitTree`** (variant 15,
`instanced_indirect_tree.fs.glsl`, the lit VS with `ALPHA_MASK`): the procedural tree MESHES (TreeSystem: the leaf
clusters and every bark / leaf crossfade copy) - LitMasked's path + the distance fade + the leaf terms (below).
**`LitFoliage`** (variant 12, `instanced_indirect_foliage.fs.glsl`, the lit VS with `FOLIAGE`): LitTree's path + the
billboard card paths. The distance fade is shared (`tree_fade.inc.glsl`); **keep the rest of the three `main()`s in
step.** All foliage values live in `Globals::settings.foliage` (`FoliageParams`, Settings.Render; registered by
`Settings::registerFoliage` as "Foliage ..." rows in the "Trees" tweak category) and ride `u_foliage_*`.

* **`MATERIAL_FLAG_BILLBOARD`** (bit 26; drawn on **`LitFoliage`**, whose card paths assume the flag): the FS's early
  sun shadow is NOT rejected by the geometric normal's facing (`sunShadowFirstFoliage`: sampled from the sun side,
  the normal-mapped normal's facing test in `doSunLight` decides) — for flat cards standing for a foliage clump. Its
  shadow lookup uses the CONSTANT depth bias only (`SHADOW_FOLIAGE_BIAS` / `g_shadowFoliage` in shadows.inc.glsl: no
  slope scaling, no normal offset): a crossed billboard's back half sits in the crossing card's shadow by a small
  depth step, which the slope-scaled bias and normal push erase. The lookup stays ON the card plane. **No per-leaf
  pixel depth offset** in the caster (`gl_FragDepth` in `shadow_depth.fs`): it cost ~15 % of the shadow draw, and the
  shadow length below hides the crossing line well enough. `OutShadowMeshInstance` keeps its 48-byte stride with pads.
* **Shadow length**: flat cards leave a HARD edge on the crossing axis (half the crown sits behind the crossing card,
  and the map treats every leaf as opaque). `foliageTransmit` (shadows.inc.glsl, in `pcssCascade` / `pcssBorder`)
  keeps only `1 - exp(-gap / length)` of a foliage receiver's shadow, `gap` = metres to the PCSS average blocker
  (`cascadeDepthRange`), `length` = `Trees/Foliage shadow length (m)` (`u_foliage_shadowLength`, default 1.5, min
  0.05). ~0 on the axis, deepening into the crown; far blockers shadow fully. **The tree MESH leaves** (the LitTree FS,
  `SHADOW_LEAF_TRANSMIT` / `g_shadowLeafLength`, set per leaf pixel; the normal bias stays) take the same fade with
  `Trees/Leaf shadow length (m)` (`u_foliage_leafShadowLength`, default 1.5, 0 = off): their only caster is their own
  billboard, whose top card at `TopCardHeight` put every leaf under it in full shadow and every leaf above it in none -
  now a leaf just under the card is barely shaded, one deep under it fully. Bark keeps the full shadow.
* **Billboard shadow shift**: `Trees/Billboard shadow shift (m)` (`u_foliage_billboardShadowShift`, default 2; < 0 =
  away from the sun) moves a tree BILLBOARD's shadow caster along the sun direction in `shadow_depth.vs` (the shadow
  cull's bit 7: a swaying caster whose material has `MATERIAL_FLAG_BILLBOARD`; its cull radius grows by the shift).
  The flat cards stand at the crown's middle, so the caster can be moved toward the crown's sun side.
* **The billboard normal map is the CROWN layout** (Procedural TreeImpostor `bakeBillboards`): RGB = the full
  tangent-space normal, SIGN KEPT (a normal may face away, toward a sun behind the tree), **A = the BAKED INTERIOR**
  (linear: 0 on the crown's surface .. 1 at about the crown's core depth, from the piece's own blurred leaf field -
  any crown shape). Do not rebuild z from RG to free a channel: every normal then faces the viewer and a back-lit
  tree goes flat.
* **Interior**: darkens the sun visibility AND the ambient (`computeLitColor`'s `texAO`) by `1 - Foliage interior
  shadow (u_foliage_interiorShadow) x smoothstep(Foliage interior depth start, end, A)` (`u_foliage_interiorStart` /
  `u_foliage_interiorEnd`, default 0.5 / 1), so a fully lit crown is not flat. **On the SUN** (direct and
  transmitted) that interior FADES OUT as the view lines up with the sun (x `Foliage interior view fade` (1) x |V·L|;
  the ambient keeps it): from the front the leaves seen through the gaps are lit through those same gaps.
* **CROWN SELF-SHADOW**: the sun (direct + transmitted) x `exp(-Foliage self shadow (5) x chord)`, the chord from the
  visible LEAF along L to where the crown ellipsoid (below) ends, in lateral radii (0 for a leaf outside it). The leaf
  lies between the view ray's ENTRY on the ellipsoid and the card point (the plane through the centre), at the baked
  interior x (1 - max(V·L, 0)) (from the front: at the entry). The flat cards cannot shade themselves in the shadow
  map, so without it a crown seen toward the sun is lit everywhere but at its axis. The TRANSMITTED sun takes only
  `Foliage transmission self shadow` (0.12, `u_foliage_transmissionSelfShadow`) of that exponent (light scattered
  forward through the leaves loses less): at the full share a crown seen toward the sun hides `Foliage transmission`
  entirely. On a whole tree's horizontal card the interior term is raised to `Foliage interior shadow top card scale`
  (`u_foliage_interiorTopCardScale`, default 2; an exponent: > 1 darker), then × `|V.y|` (the view's steepness;
  plain `V.y` goes negative from below and turns the card black).
* **Crown normal**: each crossed card's baked normals shade the crown side ITS bake view saw, so the shading splits
  hard where card A gives way to card B on screen. `foliageCrownNormal` blends the shading normal toward the view ray's
  hit on an ELLIPSOID on the card's axis — the line from the instance origin along +u, centred at the card's u centre,
  radii half the u length ALONG the axis and half the card's WIDTH ACROSS it (`|d(pos)/dv|` x the card's strip height
  in texture v, which rides the tangent's w magnitude as `2 + strip` - Procedural `TREE_CARD_STRIP_CODE`; d(pos)/dv
  alone spans a whole texture unit, ~3x the card; cards without the code - the merged branch cards, axis code >= 3 -
  keep a sphere of half the u length). The ellipsoid follows the card's own proportions (a sphere of half the tree
  height points a pine's normals up at its top and down at its bottom). It depends on the ray only, so both cards
  agree. The weight is 1 at the axis (where the cards disagree most) and falls to `Trees/Foliage crown normal`
  (`u_foliage_crownNormal`, default 0.3) at half the LATERAL radius from it, so the outer crown keeps its baked
  detail. **The baked normal is ROTATED, not replaced**: it turns by the rotation from the card's facing normal to the
  crown normal, x the weight as an angle - both cards agree on the mean direction, the leaf normals keep their
  scatter. Replacing it with the one smooth crown normal lights the axis strip brighter than the rest of the card (a
  smooth normal facing the light outshines the mean of scattered ones): a bright vertical band through every
  billboard.
* On a whole tree's HORIZONTAL card (`MATERIAL_FLAG_BILLBOARD_TOP_CARD`, bit 23, on the card whose normal points up -
  `foliageTopCard`; whole trees stand upright) `foliageCrownFrame` takes the card normal as the axis and the card's
  own point on it as the centre (its u spans at least the height, its v the crown width). The LitFoliage variant's VS
  (defined `FOLIAGE`) adds the instance origin as flat location 5.
* **The leaf look** (`leaf_look.inc.glsl`, LitTree / LitFoliage leaves): the LitTree leaves take `Trees/Leaf
  roughness` (0.44, a waxy sheen; the cluster normal map now gives every leaf its own facing, its slope x `Trees/Leaf
  normal strength` (1; 0 = flat cards)) instead of 0.65; both
  variants tint the leaf colour by world-space PATCHES (`leafPatchTint`: two value noises at `Leaf patch size (m)`, 2 -
  ONE lattice, `leafValueNoise2`: each corner hash's low / high 16 bits, 8 hashes, not 16 -
  brightness x 1 -+ 0.6 x `Leaf patch variation` (0.4), and toward a yellow-green or a deeper blue-green by it): a crown
  in groups of lighter / yellower and darker / greener leaves. No per-card data reaches the FS, so it is world space.
  **Leaf specular curve** (`LEAF_SPEC_NORMAL` + `g_leafSpecN` / `g_leafSpecOn` in `doSunLight`, LitTree only,
  `Trees/Leaf specular curve` 3): a leaf's SUN SPECULAR comes from its own normal - the raw normal map's slope x that,
  the same edge-on bend - while the diffuse keeps the `Leaf normal strength` normal: ONE `doLightSplitH`
  (punctual_lights.inc.glsl: one H, one Fresnel, one GGX, the diffuse N·L from the other normal; two `doLightH` calls ran
  the GGX twice, and Fresnel with F0 = 0 is not 0 at grazing). A more curved leaf sweeps the highlight over a wider
  range, so some spot on it meets the half vector (a glint); from the shading normal it was a faint even sheen.
  **Leaf sky wrap** (`LEAF_SKY_WRAP` + `g_leafSkyWrap` in `computeLitColor`, `Trees/Leaf sky wrap` 0.5): a leaf whose
  (bent) normal faces DOWN takes the GI irradiance of that normal mirrored toward the sky by that much - a thin leaf
  is lit through from the sky behind it; at its own side only it read the dark ground: dark leaves all through the
  crowns. One lookup still.
* **Leaf transmission** (`MATERIAL_FLAG_LEAF`, bit 30, **LitTree / LitFoliage only**; LitMasked ignores the flag):
  the FS adds the sun THROUGH the leaf, tinted by its colour DEEPENED (`leafTransmitColor`: the colour ^ `Foliage
  transmission saturation` (2), x a yellow by `Foliage transmission warmth` (0.5: red up to x 1.8, blue down to
  x 0.45), back at its own luminance - light through a leaf passes its pigment about twice and its red edge lets more
  red through: a yellow-green glow (the squared colour alone came out a deeper green); formed only where light comes
  through - a sun-facing leaf seen with the sun behind skips its per-channel pow; on the MESH leaves also x the leaf's THICKNESS: 1 - `Leaf
  transmission centre` (0.4) at the midrib, 1 at the rim, from the leaf normal map's A (the RIM, Procedural
  TreeLeafTexture). Tried: the rim from the normal's tilt - each leaf's own random tilt cancels its cup on one half,
  which then read as all centre) — `saturate(-N·L)` (lit from behind) +
  `saturate(V·-L)^focus × glow` (the backlit rim looking toward the sun), × `Trees/Foliage transmission`. Its
  visibility = `mix(1, sun shadow, Foliage transmission shadow)` × (LitFoliage) the interior term: a leaf seen from
  the shaded side is in its own crown's shadow, which would leave no glow. `u_foliage_transmission` /
  `transmissionFocus` / `transmissionGlow` / `transmissionShadow`.
* **Leaves seen edge-on:** their N is bent toward the viewer until N·V reaches `Trees/Foliage min N.V` (default 0.25,
  `u_foliage_minNoV`, 0 = off) - the cards' bent normals let N·V approach 0 with N·L > 0, and the sun's GGX at
  grazing (Fresnel → 1, the Smith term growing) lights one leaf near the sun hundreds of times brighter than its
  diffuse: a blinding spot with bloom.
* **The leaf terms are formed BEFORE `computeLitColor`** as one half colour: the light loop is the lit FS's register
  peak, and terms formed after it keep the shadow, the interior term and the surface colour live across it. The
  transmitted sun (radiance) then goes IN as `g_litExtraLight` (`LIT_EXTRA_LIGHT`), added to the sum right after the
  sun, before the GI and the light loop: kept live across the loop and added after it, it cost the tree and foliage FS
  16 B of spill (64 / 48 -> 64 / 32).
  LitTree's leaf and plain shadows share ONE `sunShadowVisibility` call (`sunShadowFirstTree`): two call sites inline
  the shadow search twice. A LEAF pixel's shadow is looked up from its SUN side (the bias normal flipped toward the
  sun) - the facing reject gives every leaf facing away from the sun 0, exactly the ones the back term lights.
  LitMasked uses the plain `sunShadowFirst` (after its discard).
* **No RTAO on foliage**: a FOLIAGE instance gets TLAS mask 0x02 (`gi_tlas_instances.cs`) and RTAO traces with cull
  mask 0x01, so its opaque rays do not hit whole card rectangles (every other ray uses 0xFF and still hits them); and
  the lit FS skips the AO read on a card (`FOLIAGE_NO_RTAO` / `g_noRtao`) — traced from depth, it is the flat card's
  occlusion by the crossing card and the ground.
* **Edge-on fade**: a foliage card FADES OUT EDGE-ON (dithered, `smoothstep(start, end, |N·V|)` on the geometric
  normal, its own decorrelated pattern so it composes with the distance fade): a grazing card smears its texture into
  bright streaks, and the crossed card faces the view right then. `Trees/Foliage edge fade start` / `end` (default
  0.1 / 0.6; `u_foliage_edgeFadeStart` / `edgeFadeEnd`); near the crossing axis both are × `Foliage edge fade centre
  scale` (default 1.33, `edgeFadeCentreScale`, back to ×1 at half the crown radius, `foliageAxisDistance`), and on a
  whole tree's horizontal card also × `Foliage edge fade top card scale` (`u_foliage_edgeFadeTopCardScale`, default
  1). **Not from below:** the fade blends out as the view turns upward (`smoothstep(0, 0.2, -V.y)`, the first ~11°
  below the pixel) - looking up into a crown, the edge-on cards are what fills it.
* **`MATERIAL_FLAG_DISTANCE_FADE`** (bit 25, LitTree / LitFoliage only - `tree_fade.inc.glsl`; + `MATERIAL_FLAG_FADE_IN`,
  bit 24): a dithered fade over a camera-distance band packed into the LOW flag bits (start m in bits 0..11, width m
  in 12..21; `makeDistanceFadeFlags`). Fade-out keeps `dither < 1 − f`, fade-in `dither ≥ 1 − f`, so an out/in pair
  over the same band shares no pixel; interleaved gradient noise shifted per frame for TAA. `MaterialInfo` stays 16 B
  (it is redeclared in ~10 shaders).

### Per-entity tint

`getOrCreateSolidColorMaterial(color)` mints a material whose diffuse is a 1×1 solid-colour texture,
**cached per quantized colour so repeat calls are free.** Pass the index to
`RenderNode::setMaterialOverride`.

---

## Streaming

### Texture mips (`TextureStreamer`, "Texture Streaming" tweaks)

DDS textures load **tail-only** (mips at or below `Tail max dim`, default 128 px) and stream against a
VRAM budget (`Budget (MB)`) by camera priority (`Mip bias`, `Texel ratio`).

**Residency changes recreate the image and rewrite the slot in all 7 bindless texture arrays** —
UPDATE_AFTER_BIND, applied per frame slot after the fence wait. Disk reads run on a worker thread, and
**old images retire after `NUM_FRAMES_IN_FLIGHT`.**

Rate limits: `Max ops in flight`, `Max MB/frame`, plus `Demote hysteresis frames` and `Decay frames`
so a briefly-unseen texture is not immediately demoted. `GPU mip copies` moves the mip promotion onto
the GPU.

**Non-DDS, procedural and embedded textures are pinned at full res.**

Both streamers' `update()` allocate nothing per frame once warm: the completion queue is swapped into
a KEPT twin deque (a fresh deque per frame allocates even when empty), and the solver's grant heap,
retention order, promotion order and eviction candidates are kept member scratch.

### Mesh data (`MeshStreamer`, "Mesh Streaming" tweaks)

Cooked scenes register mesh sets — source mesh, LOD levels and `.vsc` byte ranges (see
`MeshStreamSource` in [`Docs/Context/File/CONTEXT.md`](../File/CONTEXT.md)). Vertex and index mega-buffers are
`BitRangeAllocator<true>` (lock-free) free lists in `MeshDataManager` (see "The graphics-queue mutex" ([CONTEXT.md](CONTEXT.md)) for its grow lock).
A full pool grows by **1.1x** (at least the request, rounded up to whole buckets), not 2x: these are the largest
device-local buffers, and a doubling leaves up to half of one unused. Each grow is a GPU drain + a full GPU copy.
Their initial sizes (`INITIAL_VERTEX_DATA` 16 M vertices = 768 MiB, `INITIAL_INDEX_DATA` 64 M indices = 256 MiB), the
mesh instance capacity (`INITIAL_INSTANCE_DATA` 1048576), the render node capacity (`INITIAL_RENDER_NODES` 65536) and
the GI TLAS instance capacity (`GI_INITIAL_TLAS_INSTANCES` 32768) are set so the world's load does not grow them (each
grow idles the GPU). The unique mesh capacity starts at `INITIAL_UNIQUE_MESHES` (2048) and grows up to
`MESH_MATERIAL_INDEX_LIMIT` (65534, the uint16 mesh index).

Over `Mesh budget (MB)`, least-recently-referenced sets unseen for `Mesh cold frames` **evict**:

* MeshInfos get `indexCount 0`, **so DGC, shadow and TLAS become no-ops with NO re-record**, and
  static BLASes persist as geometry snapshots.
* Ranges free after `NUM_FRAMES_IN_FLIGHT`.
* Re-reference re-streams from the `.vsc` on a worker (`Mesh max ops`, `Mesh max MB/frame`), and **the
  LOD selector renders the nearest resident level meanwhile.**

**Only cooked static meshes stream.** With Spatial culling everything visible stays referenced, so only
despawned containers and `Col_` proxies go cold.

---

## Mesh LODs ("LOD" tweaks)

`MAX_MESH_LODS` is 5.

* **`LodN_<name>` meshes and nodes form authored chains**; level > 0 is never instanced, like `Col_`.
* Static meshes without authored LODs get **meshopt-generated index-only chains, pre-baked by the scene
  cache** (`Generate LODs`, `Generated levels`, `Generated reduction`, `Min indices`).
* Skinned meshes get bind-pose-generated chains too (per-instance `MeshLodGroups`, recycled with the
  bundle). **Skinned output MeshInfos are excluded from static BLAS builds and compaction.**

### Selection is per-instance ON THE GPU

`mesh_lod.inc.glsl` in the main and shadow cull; params live through the UBO. Both culls include
`instance_cull.inc.glsl`: their shared bindings (1-5, 7, 8, 10-12), the instance transform and `lodResidentLevel` (the
nearest resident level while the picked one is streamed out). The shadow cull's cascade test reads `u_cascadePlanes`,
normalized once per frame on the CPU (`buildUboSunShadow`).

**Instances push LOD0 and the cull redirects the draw.** The CPU sizes every chain member's bucket to
the chain's full count, **hysteresis lives in per-instance GPU slots (benign races)**, shadow cull
selects stateless 4× / +2 coarser, and GI needs no selection at all (one chain BLAS).

**The metric is screen-space error**: the meshopt simplify error of each level, in mesh-local units,
projected to pixels against `Max error (px)`. **Authored chains carry no error data**, so those fall
back to projected diameter vs `Full-res pixels (Authored Lods)`.

Also: `Bias`, `Hysteresis`, `Force LOD` (−1 = off).

`renderNode` keeps **every level of a referenced chain warm** (`noteUse`, once per frame per chain),
and the cull falls back to the nearest resident level during a re-stream.

### RT

**A LOD chain shares ONE BLAS at `RT/BLAS LOD level`** (an alias table; the aliased RT meshIdx rides
the TLAS instance `sbtOffset` for hit-shader fetches, and materials stay per-instance). Static BLASes
**copy-compact after build** (`RT/BLAS compaction`, ~30–50 % back).

---

## Draw-list compaction (`DrawCompactPipeline`, `draw_compact.cs.glsl`)

The culls (main, shadow, rain) accumulate into **PER-MESH-SLOT** sequences: one 24 B entry per registered
mesh, LOD levels and streamed-out meshes included (`out_indirectCommands[meshIdx]`, `atomicAdd` on its
`instanceCount`). Walking them all costs one front-end draw per slot and list (tens of thousands of draws with
0 instances).

* Right after each cull's dispatch, ONE workgroup (1024 threads, subgroup ballots) moves the entries with
  `instanceCount > 0 && indexCount > 0` to the front of the SAME buffer and writes one count per list:
  main cull `[0]` opaque, `[1]` transparent, `[2]` tess ground, `[3]` terrain film; shadow / rain cull one
  count. The DGC executes take them as `sequenceCountAddress`, the tess draws as the `countBuffer`.
* **Slot order is kept**, so it is the same every frame: the transparent list is not depth sorted, and a
  per-frame order change would flicker its overlaps. That is why it is one workgroup and not a global
  atomic append.
* In place is safe: a kept entry moves to an index <= its own slot, the chunk's reads finish before its
  writes (barrier), and later chunks are never written. Past the count the entries are stale; the cull's
  full-capacity fill clears them next frame. **Nothing may read these buffers by meshIdx after the
  compaction.**
* No descriptor set: the buffers go in as device addresses (push constants). `meshCount[0]` (the CPU's
  registered mesh count) is the slot count it walks.
* **THE SKY DRAW**: no entity, no mesh, no cull. The `Sky/Enabled` tweak (`SkyParams::enabled`, default
  on; a change re-records) gates ONE `draw(3)` in `StaticMeshGraphicsPipeline::record` on the Sky variant: a fullscreen
  triangle (`sky.vs.glsl`) at z = 0, the reversed-Z far plane and the depth clear value, depth test GREATER_OR_EQUAL,
  no depth write. It draws after the opaque execute, the tessellated ground, the grass, the clutter and the film -
  after every depth writer - so early depth rejects each pixel the scene covers. `sky.fs.glsl` reads no interpolant
  (the ray comes from `gl_FragCoord`), so the geometry is irrelevant. The Sky variant is kept OUT of the opaque DGC
  execution set (`createExecutionSets`), and no material can select it (`parsePipeline` has no "Sky").
* **THE DGC PREPROCESS SCRATCH** (`createPreprocessBuffers` in the static mesh and the shadow pipelines) is sized
  by `vkGetGeneratedCommandsMemoryRequirementsEXT`, and that requirement depends on the execution set's PIPELINES,
  not only on the mesh capacity: `reloadShaders` asks again after it rebuilds them (e.g. "Editor/Wireframe" adds line
  pipelines; a stale size fails VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071). Grow-only: a reload whose
  requirement the buffers already cover keeps them.
* **THE SHADOW PASS BORROWS THE MAIN PASS'S MEMORY**: the shadow cull + draw run BEFORE the main cull
  (`recordPrimaryPreScene`), so everything they write is dead again before the main cull and the scene need it:
  * the shadow cull writes the MAIN cull's out-instance buffer (`OutShadowMeshInstance` at index `instanceIdx`; its
    48 B fit the main 64 B stride, static_assert in Layout.ixx), its instance-index buffer and its OPAQUE list (binding
    8). Only the compacted count is the shadow cull's own. No shadow capacity of its own to resize.
  * the shadow execute uses the static mesh pass's OPAQUE DGC scratch (`getOpaquePreprocessBuffer`), sized for both:
    `ShadowMapGraphicsPipeline::getPreprocessRequirement` -> `StaticMeshGraphicsPipeline::reserveOpaquePreprocess`,
    called by the Renderer after every shadow-pipeline initialize / reload / mesh-capacity growth. The transparent
    scratch stays separate (that execute shares a render pass with the opaque one).
  * ONE barrier after the shadow draw: its indirect / vertex / preprocess reads before the main cull's clear +
    writes and the scene execute's preprocess. The main cull therefore waits for the shadow draw (the cull is ~15 µs).

  Saves the shadow's out instances + indices (~52 MB at 1M instances), its command list and its scratch, per frame in
  flight.
