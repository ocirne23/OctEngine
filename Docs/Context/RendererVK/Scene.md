# Scene data: instances, meshes, streaming, LODs

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Per-frame instance flow

**`renderNode(node, passMask)` is LOCK-FREE**, callable from any job between `beginFrame` and
`present`: atomic cursor claims, monotonic.

> On an overflow frame the tail that no longer fits **drops for one frame**, `m_instanceOverflowStart`
> CAS-min records it, `present()` clamps (`clampInstanceCountToOverflow`), and capacity regrows at the
> next `beginFrame` (`checkFrameCapacities`). **NEVER roll a bump cursor back.**

**A job that pushes many nodes uses a `RenderBatch`** (Renderer.ixx; the entity pass job, the terrain walk's fan-out
chunks and sphere pass, the rock preview): `push` does the per-node work at once (`writeNodePush`:
the streaming notes, the skinned bundle's push stamp, the sparse transform upload, the pass mask) and stages the
instances (up to 128) locally; `flush` (and the destructor) makes ONE claim and ONE copy into the mapped instance buffer,
writes the LOD nodes' `NodeState`s, and adds the per-mesh counts once per distinct mesh. A batch holds no node pointer and
no thread-local, so a fiber wait inside its life is harmless; it must flush before `present`. A node with more instances
than the batch holds goes through `renderNode`. Bone models push every bone through the entity job's batch.

**The per-mesh instance counts** (the cull's bucket sizes, `InstanceStream::noteMeshInstances`) take one atomic add per
mesh and push (per run of one mesh in a node; per mesh and flush in a batch), never one per instance - a crowd of one
mesh adds to ONE counter from every worker. The count arrays cover every 16-bit mesh index and never resize (a mesh
spawned on a worker during the pushes cannot reallocate them), and a mesh's first add in a frame lists it in
`touchedMeshes`: `present()` gives buckets to the listed meshes only, and `beginFrame` zeroes only them.

**`NodeState`** (`InstanceStream`, per render node, 8 bytes): the stamped pass mask and the LOD state slot bias
(`stateBase - startIdx`; only a node with a LOD chain writes it, in ONE write with its mask once its instances are
placed). One record, so a push writes one partial line of the write-combined mapping and the cull reads both in one load.
`recordPrevCopy` copies the records whole into the previous set (the motion vectors read `.x`).

`addLightInfo` (a `PointLight` / `SpotLight` / `AreaLight` / `BeamLight`) / `addFogVolume` / `addDecal` are lock-free too. **`addDebugLine` is `PerWorker`-staged**; `present()` hands the `PerWorker` to
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
    (`drainParks`). Until then the dead bundle stays allocated (never pushed, so never skinned), and a same-frame
    respawn builds a fresh bundle instead of reusing it.
  * **skinning runs for PUSHED bundles only**: `renderNode` stamps the node's bundle (`SkinnedMeshRegistry::notePush`),
    and `present()` (`writeFrame`, under `m_spawnMutex`) copies only the stamped bundles' palettes into the slot's mapped
    buffer, packs their live jobs (each with its `firstGroup`, behind a job-count header) and makes this frame's BLAS
    build list with the other bundles' entries inert. The dispatch is each job's own groups (`x` ≤ 32768, then `y`
    rows; `skinning.cs.glsl` finds its job by a binary search on `firstGroup`). `prevValid` = the bundle was skinned
    LAST frame too (`m_skinnedFrames`), so a node back on screen draws one frame without skinned motion, never with a
    stale pose.

  So a static spawn takes NO lock after warm-up, and a despawn none at all. **Still under `m_spawnMutex`:**
  `spawnSkinnedNode` (the bundle acquire / build and the instance fill only), the shared tables, LOD groups.
* **Every shared-table RELEASE takes `m_spawnMutex`**, like the adds (`freeMeshInfoRange`,
  `removeObjectContainer`, a `RenderMaterial`'s free, `freeMeshLodGroup`): an unlocked release races a spawning job's
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
* `RenderMaterial::createForMesh(pipeline, rayTraced)` (below): one material per SYSTEM, shared by its meshes
  (fallback textures, opaque, `MATERIAL_FLAG_OCEAN` / `_TERRAIN` by pipeline, `NO_RAYTRACING` unless ray traced, BC5 flag
  from the fallback normal's format — exactly what a container override produces). Declare it before the nodes that draw it.
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
* `createMeshIndices(indices)` → a move-only RAII `RenderMeshIndices`, and `createTerrainMesh(data, indices)` → a
  `RenderMesh` of 12-byte `TerrainVertex` data over that shared range, not raytraced, no index range of its own (Procedural's terrain
  nodes; "The terrain vertex" in [Terrain.md](Terrain.md)). The ranges outlive the meshes that draw them.
* `spawnMeshNode(mesh, material, pipeline, transform)` → a plain `RenderNode` with one instance on a
  shared identity instance offset (`m_identityInstanceOffsetIdx`, created at the first spawn). The
  instance's alpha mode is the MATERIAL's (the TLAS writer's opacity flag reads it).
* **WORLD TREE RECORDS** (`TreeRecordPool`, Data/; the API types - `TreeRecordTypeGpu`, the ground layout, `TreeRecordStats` - in
  `Public/TreeRecords.ixx`; `resetTreeRecords` / `addTreeRecordChunk` / `removeTreeRecordChunk` /
  `updateTreeRecords`, main thread after beginFrame; Procedural TreeWorld): every
  terrain chunk's 4-byte tree records - right after its GROUND (`TREE_RECORD_GROUND_RES`² = 17² heights over the chunk,
  16-bit above its minimum, `TREE_RECORD_GROUND_WORDS`; every chunk has one, trees or not) - in ONE fixed-size
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
* **BAKED TREE RECORDS** (the `RenderTreeSet` handle - Public, move-only RAII; `create` / `render` / destroy, its bodies the
  RendererState helpers `createTreeSet` / `renderTreeSet` / `destroyTreeSet` in RendererTrees.cpp - + `tree_cull.inc.glsl`;
  its value types are nested: `Rep`, `Type`, `Piece`, `ChunkDraw`): a SET of placed procedural tree pieces sharing a table of piece
  TYPES (bark / bark-fade / leaves / leaves-fade / voxel / shadow-billboard representations + the crossfade band), each
  chunk uploaded ONCE to DEVICE-LOCAL buffers (`TreeCullPieceGpu` 64 B: transform + band centre/radius + type;
  `TreeCullTypeGpu` 64 B: the six records, the distance / band and a `shadowDistance`: beyond it from
  the cascades' centre the shadow cull drops the piece from every cascade, before the transform loads - the
  bushes, a texel or less in the far cascades). Trees do not move, so nothing about them is written per frame: no render
  nodes, no stream entries, no pass of their own (**do not rewrite tree records into the host-visible stream per
  frame**: an expand pass that did cost ~7.5 ms of GPU). **A set is split into CHUNKS**
  (contiguous piece ranges - Procedural: one per terrain chunk).
  * **The chunks change at run time** (`RenderTreeSet::create(types, capacity)` / `addChunk` / `removeChunk`, main after
    beginFrame; Procedural's world mode): the types fixed, a FIXED capacity of
    piece slots in blocks of 64 (no growth, no drain; a full set refuses the chunk), each chunk written once through
    the staging ring (`writeTreeChunk`: RT-capable first, bucket sizes); a removed chunk's slots
    are reused NUM_FRAMES_IN_FLIGHT frames later, its index from the next frame on. `m_treeSetMutex` covers the chunk
    table: main adds / removes while the terrain walk lists (`render`). The far volume never splats a set's pieces: the
    world's trees reach it through their records (WORLD TREE RECORDS above), expanded into the set's volume TYPES.
  * `bind()` (main, every frame) picks the set the recorded culls bind (a change re-records).
    `render(chunks, scale, forceFar)` - **ANY THREAD**, once per frame between beginFrame and
    present (Procedural calls it from the terrain's render walk) - takes the frame's drawn chunks with their PASS bits,
    writes the frame slot's host-visible LIST (`piece | passMask << 28` per listed piece, binding 22 / 15 / 11 in the
    main cull / shadow culls / TLAS writer), claims ONE instance range (3 FIXED records per LISTED piece -
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
    voxels / none decision from the distance to the centre view) or
    `treeCullShadowPiece` (shadow + rain-shelter culls: the SHADOW BILLBOARD is the caster at every distance; a type
    without one casts its mesh) — and runs the per-instance cull for each record in `[kBegin, kEnd)` (record k at its first
    record + k). ONE call site of that body (`cullInstance` / `cullCaster`, the loop serves the plain instance with
    k = 0): each call site is a full inlined copy. **Both culls are `local_size_x = 64`**
    (`IndirectCullComputePipeline::update` dispatches ceil(threads / 64) groups; one thread per workgroup leaves 31 of
    32 lanes idle). A tree whose whole sphere is off screen returns before its records.
  * **THE THREE SLOTS**: 0 = bark, 1 = leaves, 2 = the far representation. MAIN pass: the meshes before the type's
    band, its VOXEL record after it, both inside it (the meshes on their fade-out materials, the voxels' material
    fades in); a type with no voxel record draws its meshes at every distance. SHADOW passes: slot 2 alone - the
    type's `shadowBillboard` record, or with "Trees/Voxel shadows" its voxel record; a type with neither casts slots
    0 and 1. **The shadow billboard never enters the main pass**: it has no albedo and no normals (below,
    `MATERIAL_FLAG_BILLBOARD`). Fixed slots, **no per-frame pool** (a per-frame atomic pool ran out with many trees +
    bushes, and an overflowing tree drew undithered).
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
    bark record for a type with neither shadow billboard nor leaves. In the main pass a rock hands over to the far-tree
    volume at its start, as every type does (`treeCullMainPiece`); it keeps casting its sun shadow.
  * **`Type::farRing`** (off for a bush): only such a type gets an entry in the far voxel trees' tables
    (`farTypes` / `farBuckets`, "THE FAR VOXEL TREES" below) - a bush has a voxel tier in the set, but far out it is a
    few metres tall, and its voxel boxes would use up the bucket limit.
  * The buffers are bound at bindings 20/21 (main cull) and 13/14 (shadow culls) — descriptors written at RECORD
    time, so the culls bind ONE set (`m_treeCullSet`, the set rendered; changing it or destroying it re-records; a
    second set in one frame asserts). With no set, `m_treeCullDummy` is bound.
  * **In the TLAS** (GI, RTAO, RT shadows, the ocean / film reflections): the TLAS slots are the stream outside the
    tree range + ONE per tree of the list's **RT SECTION** (`treeCullTlasInstance`, the section length a push
    constant; `u_present_giTlasNumInstances` = stream + section, and the TLAS capacity grows by that demand,
    `m_giTlasDemand`). `writeTreeChunk` puts each chunk's RT-CAPABLE pieces first (a type whose RT
    representation has a BLAS - `RayTracingScene::hasStaticBlas`; bushes have none) with a sphere around them;
    `render` lists those of the GI / shadow chunks whose sphere reaches into `Trees/RT range (m)`
    FIRST, then everything else, which takes no TLAS slot. `gi_tlas_instances` (bindings 9 / 10 / 11) builds a tree's
    instance from the same static data with `treeCullRtPiece` - the shadow billboard (a type without one: its leaves only),
    off beyond **`Trees/RT range (m)`** (default 1000 m, `u_foliage_rtRange`, 0 = only "RT/TLAS Range") from the scene
    focus: every tree in the TLAS is an overlapping box every GI / shadow ray traverses. A mesh created without a
    BLAS (bushes) is inactive anyway. Since the range's stream entries are never written, it puts the MATERIAL in the
    custom index: **bit 23 set = a tree, bits 0..15 = its material** (a stream instance's custom index stays its
    index, below 2^23). Every ray-query consumer reads only the material from `in_instances[custom]`, so each takes it
    from the custom index when bit 23 is set (`rt_shadow.inc`, `gi_probe_trace`, `rtao`, `ocean.fs`,
    `terrain_film.fs`) - no extra binding anywhere. The geometry comes from the sbt-offset RT mesh index as for any
    instance; the billboards keep RTAO's mask 0x02. The three that SHADE a hit (`gi_probe_trace`, the water's
    `water_scene.inc`, `terrain_film.fs`) take a billboard's albedo from its two colours and its normal from the crown
    shape (`tree_card.inc.glsl`, `MATERIAL_FLAG_BILLBOARD` below): it has no albedo texture and no normals of use.
  * Destroying a set drains the GPU (rare: respawn / reload), so it must die before the meshes and materials its types
    name. Texture / mesh streaming notes are skipped: tree textures are pinned and `RenderMesh`es never stream.
  * `setTreeRecordTypes` takes the world set as a `const RenderTreeSet&` (a friend of its state reads the id).
* **A `RenderMesh` with LOD levels:** `createMesh(levels, errors, raytraced)` - level 0 first, at most MAX_MESH_LODS (the
  levels end at the first empty one), errors in mesh-local units (0 for level 0; nonzero → the screen-space-error selector).
  Every level goes into ONE vertex range and ONE index range, with one MeshInfo per level at its own offsets (level k is
  `m_meshIdx + k`); 2+ levels also register the GPU LOD chain (`addMeshLodGroup`, `m_lodGroup`). The one-level `createMesh`
  is the same call with one level. `spawnMeshNode` places level 0 and allocates the node's hysteresis slot for a mesh with a
  chain; the cull redirects each instance like a container chain. `destroy` frees the chain first, then the ranges and the
  MeshInfos (Procedural's rocks: `Variant::mesh`).
  * Each raytraced level gets its own BLAS, and `addMeshLodGroup` aliases every level to the "RT/BLAS LOD level" one,
    as for a container chain: the other levels' BLASes are built and unused.
* **Materials and bare textures are their own API, not the Renderer's: `RenderMaterial` / `RenderTexture`** (Public,
  move-only RAII like `RenderMesh`; the bodies are RendererState helpers, `addMeshMaterial` / `addTextureMaterial` / ...).
  * `RenderMaterial::createTextured(TextureDesc)` — a material with its OWN generated sRGB diffuse from a caller-built mip
    chain (`TextureManager::uploadMips`; no blit-generated mips, so the caller can keep alpha-test coverage per level).
    `TextureDesc` names every input: `width` / `height`, `albedoMips`, the optional `normalMips` (a LINEAR RGB
    tangent-space normal map, x along U, y along V), `albedoEncoding` / `normalEncoding` (`ETextureEncoding`, default Rgba8:
    BC1 / BC3 / BC5 when the caller compressed the levels itself - Procedural's trees; a BC5 normal sets
    MATERIAL_FLAG_BC5_NORMAL), `alphaCutoff` (> 0 → `EAlphaMode::Mask` with the cutoff in `opacity`; draw it on
    `LitMasked`), `extraFlags`, `debugName` (its textures are `<name>/albedo` and `<name>/normal`: the VRAM view splits
    names on '/', so name a material `<group>/<item>` for one box per item).
  * `RenderMaterial::derive(srcIndex, flags)` copies a material (any index: the solid-colour tint too) sharing its textures
    (the procedural tree leaf cards and bark fades).
  * `RenderTexture::create(w, h, mips, name)` — a bare bindless texture (sRGB RGBA8).
  * `index()` is what `spawnMeshNode`, `RenderTreeSet::Rep` and the GPU structs take. Destroying a material frees its slot,
    and a textured one also queues its textures' free (retired once no frame in flight can sample them); a mesh or derived material frees
    the slot only. So **a derived material dies before its source** (declare it after), and every node drawing a material
    before it. `getFlags` / `setFlags` rewrite the flags in place (no re-record). `RenderMaterial::getOrCreateSolidColor`
    is a plain index: cached, shared, never freed.
* `~RenderMesh` frees the ranges and neutralizes the `MeshInfo` slot (`freeMeshInfoRange`), like
  `removeObjectContainer` for an unstreamed mesh. **Destroy its nodes first** (owners declare the
  mesh before the node).

### Tree materials and foliage shading

**THE TREE VARIANTS**: the tree code lives in its own fragment shaders, so it never sets the register count of
LitOpaque / LitMasked (instanced_indirect.fs.glsl, which has no tree code at all). A tree draws as its MESHES or as its
VOXELS - two variants: **`LitTree`** (variant 14,
`instanced_indirect_tree.fs.glsl`, the lit VS with `ALPHA_MASK` + `TREE_ORIGIN`): the procedural tree MESHES
(TreeSystem: the leaf clusters and every bark / leaf crossfade copy) - LitMasked's path + the distance fade + the leaf
terms (below). **`LitTreeVoxel`** (variant 15, `instanced_indirect_tree_voxel.fs.glsl`, the lit VS with
`TREE_VOXEL`): the VOXEL TIER (below). The distance fade is shared (`tree_fade.inc.glsl`); **keep the rest of the
`main()`s in step.** All their values live in `Globals::settings.foliage` (`FoliageParams`, Settings.Render; registered
by `Settings::registerFoliage` as the "Foliage ...", "Leaf ..." and "Voxel ..." rows in the "Trees" tweak category) and
ride `u_foliage_*`. A tree's SHADOW BILLBOARD is no lit variant: it draws in the shadow passes only
(`MATERIAL_FLAG_BILLBOARD` below).

* **THE VOXEL TIER** (`LitTreeVoxel`; Procedural "Trees/Voxel impostors", the data in [Trees.md](../Procedural/Trees.md)
  "The voxel tier"): a `RenderTreeSet::Type::voxel` record is the tree in the MAIN pass past its type's band
  (`treeCullMainPiece` in tree_cull.inc.glsl: slot 2; its material carries the band's fade-in);
  the shadows, the rain shelter, GI and RT take the shadow billboard.
  * The mesh is the variant's GRID BOX (8 corners: normal = the corner's grid coordinate, tangent = the box size,
    `|tangent w|` 2.5 = the trunk bend). The `TREE_VOXEL` VS hands on the camera in grid space, the face point's grid
    coordinate, the instance rotation, and - everything that is ONE value per tree, worked out per box vertex and not
    per pixel (`treeVoxelBoxInterface`, tree_voxel_box.inc.glsl, shared with `tree_voxel_far.vs.glsl`) - the box's
    world size + the cube root of its volume (a level's step = that / the level's voxels), the SUN's direction in grid
    units per metre (no zero component), the LOD scale (`log2(scale x distance x level 0's voxels)` = the level, "Voxel
    LOD bias" inside) and the camera's distance to the tree's origin (the distance fade). The box's sides and a tree's
    scale are > 0 on the CPU (Procedural `billboardBox`): the shaders do not clamp them. Front faces only: the camera
    never stands inside a tree at the tier's distances (with "Trees/Force far" next to one, that tree is missing).
  * The FS marches from the face along the view ray at the level whose voxel is about a pixel
    (`u_mipPixelScale`; "Trees/Voxel LOD bias"; dithered between two levels per pixel), one crossing per step - the
    stored opacity's length, so a step multiplies the transmittance by `1 - opacity`. It stops where the coverage passes
    the pixel's noise threshold (STOCHASTIC coverage, resolved by TAA / DLSS) and discards when the ray leaves the box.
    **The marches run in the level's VOXEL coordinates** (the grid coordinate x the level's voxels: tree_voxel.inc.glsl
    takes `c`): a step is one multiply-add of a start and a direction scaled once, and it ends on ONE exit distance
    (`voxelBoxExit`), not a box test on every sample. A sample's brick and voxel are plain floors of `c - 0.5` held just
    under the last voxel centre (`voxelCentreCoord`) - no clamp after them. `VOXEL_BRICK_ORIGIN` (this shader's define):
    the march's brick holds its place in the atlas, worked out once per brick. The hit lies on the pixel's own view
    ray, so ONE view vector serves the position and the lighting.
    **TWO STEPS A WAIT** (the view march and the sun march): the tier's cost is its chain of fetches, each one waited for
    before the next sample is known. Where the next step's sample lies in the same stored brick (`voxelInBrick`), both
    first taps go out together (`voxelOpacityPair`) and are taken in turn - the same samples, the same result as one a
    step (the opaque pass 5.2 -> 4.7 ms in a forest view). A march builds its step from `voxelSampleBrick` /
    `voxelSliceUv`; the hit's place is formed from `t` after the loop, not held in it. **The marches are bound by their
    step count and their per-step math, not by what stands before them** (measured: the setup before the loop ~0, the
    jumps over the empty bricks before a crown 0.13 ms of the view march's 1.6 ms, the sun march 0.7 ms; a fifth less
    math per step = a tenth less time). So the ray is held as the voxel CENTRE coordinate (`voxelCentreClamp`), and the
    end is one compare of the transmittance. **Three quarters of the view march are samples that find NOTHING**: the air
    between the leaves INSIDE the stored bricks, before the first cover (1.2 ms; the march through the cover itself is
    0.36 ms). That air lies within a voxel or two of cover, so no skip structure removes it (the two tried below); only
    a coarser level does: "Voxel LOD bias" 0.5 = 0.25 ms less, 1.0 = 0.6 ms less. The level's pixel is the OUTPUT pixel
    (`u_mipPixelScale` is the viewport's, not the render rect's): with DLSS Quality a bias of 0.58 is one voxel per
    RENDERED pixel.
    Tried, each dropped: FOUR samples a wait (slower than two, about as slow as one: the taps past a hit are wasted);
    the material + the grid's header read in the vertex shader and handed over flat (no gain); a HALF-BRICK mask in the
    index's alpha so a march jumps the empty halves of stored bricks (+0.2 ms: the test per sample and the jump steps
    cost more than the zero samples they save); a PROBE on the next coarser level before the fine march (+0.2 ms: the
    coarse level finds cover almost at once, and the fine march then walks the same air); STRIDES of two voxels with the
    opacity corrected for the length (-0.44 ms, but half the rays miss a feature one voxel thick - a trunk goes half
    transparent - where the LOD bias gives the same saving from prefiltered data); the near set's chunks listed nearest first for early depth (no gain: the
    boxes of neighbouring trees pass through each other, so a far tree's face is rarely behind a near tree's hit).
    The grid is SPARSE BRICKS over three one-level textures ([Trees.md](../Procedural/Trees.md) "SPARSE BRICKS"): the
    material's albedo slot = the opacity atlas, whose header names the index texture (`voxelGrid`: the resolution, the
    levels and the atlas columns from the index's header texel); its normal slot = the surface atlas. A step reads the
    index (one `texelFetch`) only when it enters ANOTHER brick (`VoxelBrick`: the march keeps its last one) and, in a
    stored brick, one filtered opacity tap - the second slice only where its blend weight is above 0 (with "Voxel
    sharpness" most samples lie on one slice); the colour, normal and interior are read once, at the hit, from the
    march's own brick (`voxelSurface`). The whole surface is formed BEFORE the cascades' shadow search, and the depth
    and the motion vector after the lighting, so few values cross the lit core.
  * **It writes `gl_FragDepth`** (the hit; `layout (depth_less)`: never nearer than the face, so early depth still
    rejects a hidden box) and its motion vector at that depth. **A pipeline that writes FragDepth cannot join the DGC
    execution set** (every pipeline of a set must match its first one in its use of FragDepth): the main cull routes
    `PIPELINE_IDX_LIT_TREE_VOXEL` into its own list (binding 23, count `[4]`), drawn with `drawIndexedIndirectCount` right
    after the opaque execute; the variant is out of the opaque set's mask.
  * Shading: the voxel's mean leaf normal, toward the crown's outward direction where it is short ("Trees/Voxel normal
    hull"); the INTERIOR (the surface atlas's colour A) as the ambient's AO through the "Foliage interior ..." remap; the sun's
    SELF-SHADOW marched through the tree's own voxels one level coarser ("Trees/Voxel self shadow"; the transmission
    takes "Foliage transmission self shadow" of it; "Voxel self shadow coarser levels", 2, = how many levels coarser
    than the view march, 1..3: about half the march's steps per level. Against 1, 2 takes 0.3 ms off the opaque pass of
    a forest view and 3 0.4 ms; the picture at 2 is that of 1, at 3 the crowns are a little brighter); the cascades' shadow looked up where that sun path LEAVES the box,
    past "Billboard shadow shift" when the billboard casts - the tree's own caster lies in the box and would shadow it
    twice. With "Voxel self shadow" 0 there is no march and the cascades are read at the hit itself: the tree takes its
    own caster's shadow (its billboard, shifted, or its voxel caster). **That cascade lookup is
    ONE hardware-PCF tap** (`LIT_SUN_SHADOW_HARD`, `sampleSunShadowHard`: no blocker search, no penumbra, no foliage
    shadow length; the cascade cross-fade dithered) - a voxel pixel is noise that TAA / DLSS resolves anyway. With it the
    FS is 56 registers / 32 B (64 / 0 with the PCSS search). At NIGHT (`u_sunLit` 0, see [Shaders.md](Shaders.md)) neither the sun march nor
    the lookup runs. The transmission's BACK
    term follows the voxel's leaves, not its normal alone: blended by the mean normal's length from the average of
    two-sided leaves facing every way, `0.25 x (1 - V.L)`, to `-N.L` (after the edge-on bend N faces the viewer: no
    flip to the seen side) - from the outward mean normal alone only the crown's underside glowed under a high sun. The length is trusted by the hit voxel's cover
    (`smoothstep(0.05, 0.5, opacity)`: a thin tip's voxel holds a leaf or two, a long mean though the tip faces every way)
    and x (1 - "Voxel transmission spread"). Then the leaf terms of the mesh leaves (N.V bend, transmission, sky wrap, the leaf patches) and no RTAO.
    **The leaf-only terms go on by the voxel's LEAF SHARE** (the surface atlas's normal A; 0 = bark only): the leaf
    PATCHES ("Leaf patch variation"), the sun TRANSMISSION (back term + glow) and the SKY WRAP. The trunk and the bare
    branches keep their bark colour, let no sun through and take the ambient by their own normal. The transmission and
    the wrap take the share BEFORE the cascades' shadow search, so the share is not one more value held across it.
    The atlas reads are `tree_voxel.inc.glsl`'s (shared with the shadow casters), and so is the EMPTY-SPACE SKIPPING
    (`voxelBrickSkip`, the view march, the self-shadow's sun march and the shadow casters): a sample whose brick the
    index marks EMPTY jumps the ray to that brick's far side - the box is much larger than the crown, and a ray through
    its empty corners marched every step.
  * **THE FAR VOXEL TREES** (`TreeVoxelFarPipeline`, `tree_voxel_far.cs.glsl` + `tree_voxel_far.vs.glsl`; "Trees/Voxel
    far trees", on; live): the world set draws only the NEAR chunks ("Trees/World/Near radius"); every OTHER resident record
    chunk is expanded on the GPU into voxel boxes. Procedural TreeSystem hands the set's chunk coordinates over every frame
    (`Renderer::setTreeNearChunks`) - only those it held the frame before too: the terrain walk that lists the set's drawn
    chunks was kicked before the update, so a chunk excluded the frame it joined left its trees undrawn for a frame (a dark
    flash at the near radius); now it draws twice for that frame; `present` (`updateTreeVoxelFar`) lists this slot's record table minus those, within
    the END distance horizontally - the far volume's hand-over (`farTreesStart` + "Far overlap", as the near set's trees
    leave the main pass; 4300 m at the defaults) or, with the far volume off, "Voxel far end (m)" (4000). Procedural TreeWorld's
    record ring reaches the farther of the two that is on. Three passes, inline in the primary after the clutter cull (the chunk count
    changes per frame; off: `recordClear` empties the draws):
    * CULL, a workgroup per chunk: per record its tree exactly as `splatRecordPlant` / Procedural `expandChunk` place it
      (variant, scale, yaw from its seed; its chunk's ground grid), its variant's set type -> `FarTypeGpu` (bucket = its
      voxel mesh, material, trunk-bend scale, box centre + radius at scale 1; built in `initTreeSetTypes`), the end
      distance and the frustum on the box sphere, appended with its bucket and its index in it (`MAX_INSTANCES` 2^20:
      ~200 trees per 256 m chunk and a quarter of the ring in view make ~1M at 20 km - 262144 overflowed at ~9 km). The
      chunks are listed NEAREST FIRST, so past the cap the farthest drop (in the table's order the overflow hit trees
      anywhere far out, different ones every frame). No bushes (`Type::farRing` off), no rocks (no voxel tier).
    * PREFIX, one thread: each bucket's first instance and its `VkDrawIndexedIndirectCommand` (`MAX_BUCKETS` 64, the
      unused ones 0 instances). SCATTER: each record into its bucket's range.
    * The draw: `StaticMeshGraphicsPipeline`'s far voxel pipeline (the main layout, the box's MeshVertex at binding 0, the
      32 B record instance-rate at binding 1 - position + scale, the yaw, material, trunk-bend scale, bucket << 26 | index
      - the voxel tier's FS), `drawIndexedIndirect` over the 64 buckets right after
      the near voxel list. Every buffer reaches the passes as a device address; the UBO is the set's one binding.
    * **THE FAR SHADOW CASTERS**: the same chunks within the cascades' reach (horizontally from the scene focus: the
      shadow distance + "Foliage shadow cascade margin" + "Far height") cast as their SHADOW BILLBOARDS, with the sun cascades on
      (not under RT sun shadow). The same three passes into their own buffers (`Lists m_shadow`, `MAX_SHADOW_INSTANCES`
      2^19), the CULL with `FAR_SHADOW`: per tree its type's billboard bucket (`FarTypeGpu::shadowBucket`, one per
      billboard mesh, `TreeInstanceSet::farShadowBuckets`) and alpha mask (`shadowTex`), the cascade mask of the shadow
      cull's tree rule (`farCascadeMask`: each cascade's planes + its split + the margin, the sphere grown by the
      billboard shift) instead of the view's distance test. **ONLY THE CASTERS THE VIEW CAN SEE THE SHADOW OF**: the
      cascades lie all around the scene focus, so most of the ring's trees stand beside or behind the camera. A caster is
      kept inside the VIEW FRUSTUM grown by its shadow's length (2 x its radius x the sun's cotangent, at most
      `FAR_SHADOW_MAX_LENGTH` 400 m) - the shadow draw 1.09 -> 0.72 ms and this cull 0.10 -> 0.06 ms in a forest view.
      The main shadow cull does the same for every other caster ("Sun shadow draw cost" in [Lighting.md](Lighting.md)).
      Inline in the primary BEFORE the shadow cull ("Far tree shadows");
      `ShadowMapGraphicsPipeline`'s FAR pipeline (`shadow_depth.vs.glsl` with `TREE_FAR_SHADOW`: the record instance-rate at
      binding 1 builds the packed word the shadow cull writes - a billboard, swaying, its trunk bend - the main alpha-test FS)
      draws them with `drawIndexedIndirect` over the 64 buckets after the voxel casters. Billboards also with "Voxel shadows"
      on (the near set only casts voxels).
    * No GI or RT.
  * **THE VOXEL SHADOW CASTERS** ("Trees/Voxel shadows", off; live): the shadow cull's slot 2 takes the type's voxel record
    instead of its billboard (`treeCullShadowFar`), at every distance, and routes it into a SECOND list - the main
    cull's voxel list, borrowed (the shadow cull + draw run first), count `[1]` of the shadow cull's two. Its alpha index is
    the grid's opacity atlas (its header names the brick index). `ShadowMapGraphicsPipeline`'s VOXEL pipeline (`shadow_depth.vs/.fs` with `TREE_VOXEL`, FRONT
    faces culled as the main shadow pipeline - the cascade matrices mirror the winding, so that keeps the faces where
    the light ENTERS the box; a back-face cull kept the leaving ones and nothing cast - plain `drawIndexedIndirectCount`
    after the DGC execute: it writes FragDepth) marches along the light from the face at the level whose voxel is about
    a cascade texel + "Voxel shadow LOD bias" (+1: the near cascade's texels are a few cm, so every caster marched the
    finest level there and the shadow draw cost exploded; ONE fetch per step, the nearest slice -
    `voxelOpacityNearest`: the threshold is binary), to where the coverage
    reaches "Voxel shadow coverage" (0.5; a FIXED threshold: a shadow map gets no TAA), and writes that depth +
    0.15 m (`layout (depth_greater)`; the rasterizer's bias does not reach a written depth). No billboard shift. The TLAS
    keeps the billboard.

* **`MATERIAL_FLAG_BILLBOARD`** (bit 26): a tree's **SHADOW BILLBOARD** (Procedural TreeSystem; `RenderTreeSet::Type::
  shadowBillboard`) - three crossed cards that stand for the tree in the SHADOW passes, the rain shelter and the TLAS.
  **Never in the main pass: no lit variant draws it**, and it has no albedo and no normal map.
  * **Its albedo slot is a ONE-CHANNEL MASK** (BC4 / R8). `addTextureMaterial` shows a one-channel albedo in all four
    channels (the image view's component mapping, `Texture::initialize`'s `redAsAlpha`), so every alpha reader -
    `shadow_depth.fs`, the far shadow casters, `rtsCandidateBlocks` - reads it through `.a` as any texture's. **The value is the
    COVER up to `TREE_CARD_BARK`** (0.75, Layout.ixx, a shader define), so the 0.5 test runs on a cover the texture
    filter blended, as on any alpha; from there to 1 - a texel that is solid for certain - it is the BARK SHARE
    (`TREE_CARD_BARK` = leaves only .. 1 = bark only). **No step may sit AT the test**: with a solid texel stored at its
    kind's value (cover 0.51 -> 1), the filtered test was solid over most of the way to a neighbour of cover 0.45 - a
    speckled, too dense caster that flickered with every sub-texel move (the wind, the sun), and PCSS's fixed per-pixel
    disc turned the speckle into grain.
  * **Its normal slot is a 2 x 1 texture of its two colours**, bark then leaves, as their square roots. A ray that
    SHADES a hit (`gi_probe_trace`, `water_scene.inc`, `terrain_film.fs`) tests the flag and calls `treeCardAlbedo`
    (`tree_card.inc.glsl`): one tap at u = 0.75 - 0.5 x the bark share, squared. An EMPTY or thin texel takes the leaf
    colour - those rays run with the opaque flag, so the whole card is solid to them - and a filtered tap between leaves
    and a gap stays below `TREE_CARD_BARK`: a crown's edge never reads as bark.
  * **A shaded hit's NORMAL is the CROWN SHAPE's, not the card's** (`treeCardNormal`): three flat cards shade as three
    planes (one faces the sun, the next is edge-on to it). The normal is the shape's where the RAY enters it (the ray in
    object space, from the ray query), so it depends on the ray alone and the three cards agree. The shape: an ellipsoid
    above its centre, a vertical cylinder below (a trunk and a crown's lower half face sideways, not down); a ray that
    passes the shape takes its closest point (the silhouette). Centre = the card mesh's bound centre (`InMeshInfo::
    center`, on the trunk axis), radii = the cards' half extents, which ride every card vertex's TANGENT (Procedural
    `billboardMesh`; a card has no normal map). The normal faces the ray, so **a card hit is never a backface** for the
    GI probes' embedded test.
  * The shadow cull knows it by the flag (bit 7 of the packed word: the shift below); the TLAS writer gives it mask
    0x02 (no RTAO, below). **No per-leaf pixel depth offset** in the caster (`gl_FragDepth` in `shadow_depth.fs`): it
    cost ~15 % of the shadow draw. `OutShadowMeshInstance` keeps its 48-byte stride with pads.
* **Shadow length**: a flat caster leaves a HARD edge in the crown it stands for (half the crown sits behind a card,
  and the map treats every leaf as opaque). `foliageTransmit` (shadows.inc.glsl, in `pcssCascade` / `pcssBorder`)
  keeps only `1 - exp(-gap / length)` of a foliage receiver's shadow, `gap` = metres to the PCSS average blocker
  (`cascadeDepthRange`). **The voxel tier** (`SHADOW_FOLIAGE_BIAS` / `g_shadowFoliage`: also the constant depth bias
  only, no slope scaling and no normal offset - a voxel hit lies a small depth step from its own caster) takes
  `length` = `Trees/Foliage shadow length (m)` (`u_foliage_shadowLength`, default 3, min 0.05). **The tree MESH leaves**
  (the LitTree FS, `SHADOW_LEAF_TRANSMIT` / `g_shadowLeafLength`, set per leaf pixel; the normal bias stays) take
  `Trees/Leaf shadow length (m)` (`u_foliage_leafShadowLength`, default 3, 0 = off): their only caster is their own
  shadow billboard, whose top card at `TopCardHeight` put every leaf under it in full shadow and every leaf above it
  in none - now a leaf just under the card is barely shaded, one deep under it fully. Bark keeps the full shadow.
* **Billboard shadow shift**: `Trees/Billboard shadow shift (m)` (`u_foliage_billboardShadowShift`, default 3; < 0 =
  away from the sun) moves a tree BILLBOARD's shadow caster along the sun direction in `shadow_depth.vs` (the shadow
  cull's bit 7: a swaying caster whose material has `MATERIAL_FLAG_BILLBOARD`; its cull radius grows by the shift).
  The flat cards stand at the crown's middle, so the caster can be moved toward the crown's sun side.
* **The leaf look** (`leaf_look.inc.glsl`, the LitTree leaves and the voxel tier): the LitTree leaves take `Trees/Leaf
  roughness` (0.35, a waxy sheen; the cluster normal map gives every leaf its own facing, its slope x `Trees/Leaf
  normal strength` (1; 0 = flat cards)) instead of 0.65; both
  variants tint the leaf colour by world-space PATCHES (`leafPatchTint`: two value noises at `Leaf patch size (m)`, 1 -
  ONE lattice, `leafValueNoise2`: each corner hash's low / high 16 bits, 8 hashes, not 16 -
  brightness x 1 -+ 0.6 x `Leaf patch variation` (1), and toward a yellow-green or a deeper blue-green by it): a crown
  in groups of lighter / yellower and darker / greener leaves. No per-card data reaches the FS, so it is world space.
  **Leaf specular curve** (`LEAF_SPEC_NORMAL` + `g_leafSpecN` / `g_leafSpecOn` in `doSunLight`, LitTree only,
  `Trees/Leaf specular curve` 3): a leaf's SUN SPECULAR comes from its own normal - the raw normal map's slope x that,
  the same edge-on bend - while the diffuse keeps the `Leaf normal strength` normal: ONE `doLightSplitH`
  (punctual_lights.inc.glsl: one H, one Fresnel, one GGX, the diffuse N·L from the other normal; two `doLightH` calls ran
  the GGX twice, and Fresnel with F0 = 0 is not 0 at grazing). A more curved leaf sweeps the highlight over a wider
  range, so some spot on it meets the half vector (a glint); from the shading normal it was a faint even sheen.
  **Leaf specular** (`Trees/Leaf specular` 0.06, LitTree leaves only): the leaves' F0 - the includer's `LIT_SPECULAR_F0`
  (`g_specularF0`, set before `computeLitColor`) in place of the lit core's 0.04: a waxy cuticle's brighter, white highlight.
  **The front sun on the facing card face only** (`g_sunVisMaterial` = smoothstep(0, 0.1) of the interpolated vertex
  normal . L, leaves only, as the terrain's relief gate): the seen face of a leaf lit from behind had half of every leaf
  tilted into the sun by the normal map (x the specular curve), and toward a low sun Fresnel -> 1 turned that half white.
  The back side keeps only the transmission. With `NormalBend` the gate follows the bent vertex normal (both faces lit as
  the crown's shape, as intended).
  **Leaf sky wrap** (`LEAF_SKY_WRAP` + `g_leafSkyWrap` in `computeLitColor`, `Trees/Leaf sky wrap` 0.5): a leaf whose
  (bent) normal faces DOWN takes the GI irradiance of that normal mirrored toward the sky by that much - a thin leaf
  is lit through from the sky behind it; at its own side only it read the dark ground: dark leaves all through the
  crowns. One lookup still.
* **Leaf transmission** (`MATERIAL_FLAG_LEAF`, bit 30, **LitTree / LitTreeVoxel only**; LitMasked ignores the flag):
  the FS adds the sun THROUGH the leaf, tinted by its colour DEEPENED (`leafTransmitColor`: the colour ^ `Foliage
  transmission saturation` (1 = the leaf colour itself), x a yellow by `Foliage transmission warmth` (0 = none; at 1 red up to
  x 1.8, blue down to x 0.45), back at its own luminance - light through a leaf passes its pigment about twice and its red edge lets more
  red through: a yellow-green glow (the squared colour alone came out a deeper green); formed only where light comes
  through - a sun-facing leaf seen with the sun behind skips its per-channel pow; on the MESH leaves also x the leaf's THICKNESS: 1 - `Leaf
  transmission centre` (0.4) at the midrib, 1 at the rim, from the leaf normal map's A (the RIM, Procedural
  TreeLeafTexture). Tried: the rim from the normal's tilt - each leaf's own random tilt cancels its cup on one half,
  which then read as all centre) — `saturate(-N·L)` (lit from behind) +
  `saturate(V·-L)^focus × glow` (the backlit rim looking toward the sun), × `Trees/Foliage transmission`. Its
  visibility = `mix(1, sun shadow, Foliage transmission shadow)`: a leaf seen from
  the shaded side is in its own crown's shadow, which would leave no glow. `u_foliage_transmission` /
  `transmissionFocus` / `transmissionGlow` / `transmissionShadow`.
* **Leaves seen edge-on:** their N is bent toward the viewer until N·V reaches `Trees/Foliage min N.V` (default 0.04,
  `u_foliage_minNoV`, 0 = off) - the leaf cards' bent normals (`Leaves NormalBend`) let N·V approach 0 with N·L > 0, and the sun's GGX at
  grazing (Fresnel → 1, the Smith term growing) lights one leaf near the sun hundreds of times brighter than its
  diffuse: a blinding spot with bloom.
* **The leaf terms are formed BEFORE `computeLitColor`** as one half colour: the light loop is the lit FS's register
  peak, and terms formed after it keep the shadow and the surface colour live across it. The
  transmitted sun (radiance) then goes IN as `g_litExtraLight` (`LIT_EXTRA_LIGHT`), added to the sum right after the
  sun, before the GI and the light loop: kept live across the loop and added after it, it cost the tree FS
  16 B of spill (64 / 48 -> 64 / 32). The terrain FS adds its far-grass blade light the same way (Vegetation.md).
  LitTree's leaf and plain shadows share ONE `sunShadowVisibility` call (`sunShadowFirstTree`): two call sites inline
  the shadow search twice. A LEAF pixel's shadow is looked up from its SUN side (the bias normal flipped toward the
  sun) - the facing reject gives every leaf facing away from the sun 0, exactly the ones the back term lights.
  LitMasked uses the plain `sunShadowFirst` (after its discard).
* **No RTAO from the shadow billboards**: a `MATERIAL_FLAG_BILLBOARD` instance gets TLAS mask 0x02
  (`gi_tlas_instances.cs`) and RTAO traces with cull mask 0x01, so its opaque rays do not hit whole card rectangles
  (every other ray uses 0xFF and still hits them); and the voxel FS compiles without the AO read (`LIT_NO_RTAO`) —
  traced from depth, it is the occlusion of a point inside a crown; the baked interior is its AO.
* **`MATERIAL_FLAG_DISTANCE_FADE`** (bit 25, LitTree / LitTreeVoxel only - `tree_fade.inc.glsl`; +
  `MATERIAL_FLAG_FADE_IN`, bit 24): a dithered fade over a camera-distance band packed into the LOW flag bits (start m
  in bits 0..11, width m in 12..21; `makeDistanceFadeFlags`). Fade-out keeps `dither < 1 − f`, fade-in `dither ≥ 1 − f`,
  so an out/in pair over the same band shares no pixel; interleaved gradient noise shifted per frame for TAA.
  **The distance is the INSTANCE ORIGIN's** (the tree's base: the VS hands the camera's distance to it on, flat, with `TREE_ORIGIN` - LitTree's
  VS - and `TREE_VOXEL`), so a whole tree dissolves evenly over the band. By each pixel's own distance the band's sphere
  cut a tree in two - the mesh on its near half, the next tier on its far half - and the voxel tier showed its crown's
  dark inside along the cut. `MaterialInfo` stays 16 B (it is redeclared in ~10 shaders).

### Per-entity tint

`RenderMaterial::getOrCreateSolidColor(color)` mints a material whose diffuse is a 1×1 solid-colour texture,
**cached per quantized colour so repeat calls are free.** Pass the index to
`RenderNode::setMaterialOverride`.

### Emissive

**Emissive is a STRENGTH on a solid colour, drawn by the UNLIT pipeline only.** `getOrCreateSolidColor(color,
emissive)` with a strength above 0 (in sixteenths, up to 15.9; it is part of the cache key) mints the colour's
material with `EAlphaMode::Emissive` and the strength in `MaterialInfo::opacity`. `instanced_indirect_unlit.fs.glsl`
then outputs colour × strength × a facet term (full where the surface faces the eye, 55 % at a grazing angle, so a
faceted shape stays readable with no lighting): HDR, and the bloom takes it.

* **The lit shaders are untouched** (no emissive term, no register): a lit pipeline draws such a material as its
  plain colour. So a glowing piece must be a node of a container with `PipelineIdx UnlitOpaque` (`eyes.oc`,
  `gemsGlow.oc`), authored `Color r g b` + `Emissive s` in its `Component Render`.
* No other pass tests for the Emissive alpha mode: they see an opaque material. A glowing piece is OUT OF RAY TRACING
  and casts no sun shadow - a look, not a light: the emissive solid colour carries `MATERIAL_FLAG_NO_RAYTRACING` itself,
  because the tint REPLACES the container's material (and with it the container's `ExcludeFromRayTracing`). A plain
  tint keeps its shadows. A light placed inside a glowing piece (the cannon shell's) is therefore not blocked by it.
  `getOrCreateSolidColorMaterial` builds the row in ONE initializer. Tried: `material.flags |= NO_RAYTRACING` inside an
  `if` after the plain fields - in the RelWithDebInfo build the flag never reached the table (the alpha mode and the
  strength from the same `if` did), seen with a runtime probe.

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
Their initial sizes (`INITIAL_VERTEX_DATA` 700 MiB, about 15 M `MeshVertex`, `INITIAL_INDEX_DATA` 45 M indices = 180 MiB), the
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
the chain's full count (`present()`, from the touched LOD0 mesh; a member pushed directly gets no bucket of its own),
**hysteresis lives in per-instance GPU slots (benign races)**, shadow cull
selects stateless 4× / +2 coarser, and GI needs no selection at all (one chain BLAS). A mesh with no instances this
frame keeps a STALE first instance: the culls read it only for an instance they place in that bucket.

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
mesh, LOD levels and streamed-out meshes included (`out_indirectCommands[meshIdx]`, an atomic add on its
`instanceCount`). Walking them all costs one front-end draw per slot and list (tens of thousands of draws with
0 instances).

* **THE BUCKET CLAIM** (main and shadow cull, `bucketClaim` / `casterClaim`): ONE atomic per (list, mesh) and
  subgroup, not per instance. A loop takes the first remaining lane's key, its lanes rank themselves by ballot, the
  elected lane adds their count, and the others go round again. Correct for any set of active lanes (a lane leaves
  only after its claim): divergence costs rounds, never a wrong index. A crowd of one mesh no longer serializes on one
  address.
* **THE DRAW SLOT**: a placed instance goes to `firstInstance + idx` (its bucket's start + its index) in the
  out-instance buffer, and the scene vertex shaders (lit, terrain, ocean, rock, river, shadow depth) read it by
  `gl_InstanceIndex` - the draw's `firstInstance` puts it there. No index list, no instance-rate attribute, no vertex
  binding 2. The buffer is sized to the instance capacity, which `present()` keeps above the LOD-expanded bucket total.

* Right after each cull's dispatch, ONE workgroup (1024 threads, subgroup ballots) moves the entries with
  `instanceCount > 0 && indexCount > 0` to the front of the SAME buffer and writes one count per list:
  main cull `[0]` opaque, `[1]` transparent, `[2]` tess ground, `[3]` terrain film, `[4]` the trees' voxel tier;
  shadow / rain cull one count. The DGC executes take them as `sequenceCountAddress`, the plain draws (tess ground,
  film, voxel tier) as the `countBuffer`.
* **Slot order is kept**, so it is the same every frame: the transparent list is not depth sorted, and a
  per-frame order change would flicker its overlaps. That is why it is one workgroup and not a global
  atomic append.
* In place is safe: a kept entry moves to an index <= its own slot, the chunk's reads finish before its
  writes (barrier), and later chunks are never written. Past the count the entries are stale; the cull's
  full-capacity fill clears them next frame. **Nothing may read these buffers by meshIdx after the
  compaction.**
* No descriptor set: the buffers go in as device addresses (push constants). `meshCount[0]` (the CPU's
  registered mesh count) is the slot count it walks.
* Tried: a sort of the opaque list by pipeline. Dropped because a co-op battle measured 128 draws with 35 pipeline
  changes after compaction: slot order already keeps short runs, and the ~25 binds it saves are not worth a second walk.
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
  * the shadow cull writes the MAIN cull's out-instance buffer (`OutShadowMeshInstance` at its draw slot; its
    48 B fit the main 64 B stride, static_assert in Layout.ixx) and its OPAQUE list (binding 8). Only the compacted
    count is the shadow cull's own. No shadow capacity of its own to resize.
  * the shadow execute uses the static mesh pass's OPAQUE DGC scratch (`getOpaquePreprocessBuffer`), sized for both:
    `ShadowMapGraphicsPipeline::getPreprocessRequirement` -> `StaticMeshGraphicsPipeline::reserveOpaquePreprocess`,
    called by the Renderer after every shadow-pipeline initialize / reload / mesh-capacity growth. The transparent
    scratch stays separate (that execute shares a render pass with the opaque one).
  * ONE barrier after the shadow draw: its indirect / vertex / preprocess reads before the main cull's clear +
    writes and the scene execute's preprocess. The main cull therefore waits for the shadow draw (the cull is ~15 µs).

  Saves the shadow's out instances + indices (~52 MB at 1M instances), its command list and its scratch, per frame in
  flight.
