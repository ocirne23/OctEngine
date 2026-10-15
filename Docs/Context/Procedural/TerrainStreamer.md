# TerrainStreamer and HeightMapBaker

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## `TerrainStreamer`

"Terrain*" tweaks. Render-only streaming of a **QUADTREE** of terrain nodes.

* **Quadtree nodes.** A node `(lod, coord at that lod)` covers 2^lod x 2^lod BASE chunks (`chunkSize` 256 m) with the
  SAME `lod0Res` (128) grid, so each level doubles the quad size. **The leaf rule** (`Ring::leafLodAt`, mirrored by the
  terrain VS): descend from `maxLod` (6) and stop at the first node whose band (`ringLodAt(nodeEdgeDist)`: `lodStep`
  1.1 chunks for the LOD0 band, each next band twice as wide, `fullResDist` 0.7) reaches its lod. **The wanted set**
  (`rebuildWanted`, on main when the ring moves): the leaves touching the (2R+1)^2 base chunks around the camera's chunk
  (`ringRadius` 128 = 32 km) and the generated bounds - **~100-200 nodes**, so every per-frame decision
  walks them directly on main (no scan jobs). A far node only changes when the camera crosses its own grid. A big node
  sticks out of the ring by up to its own size: the far terrain-data cascade grows to cover it
  (`m_wantedHalfExtent`). No skirts: edge stitching (below).
  A per-chunk LOD scheme (one fixed-size chunk per coordinate, regenerated whenever its band moved) could not keep up
  with fast flight; the quadtree regenerates a node only when its own grid moves.
* Generated on up to "Gen jobs" **self-continuing Low-priority pump jobs** (`"terrainChunkPump"`; `kickPump` CAS-claim +
  exit-recheck protocol, nearest-first). **The upload layout is built IN the pump** (`RenderMeshData::build`, pure) and
  `Renderer::createMesh` runs on the upload job (below), **so warm-tile mesh builds overlap cold V3 waits, which park
  fibers.**
* **Pre-emption points** (see Threading): the pump yields to higher-priority jobs between chunks,
  `generateChunk` after its `sampleGrid` and per vertex row, and `TerrainGenV3::sampleGrid` per row.
  **All of them sit OUTSIDE the V3 pipeline `JobMutex`** — the Normal collider job samples terrain
  and would park on that lock behind the very job holding it, so a point under the lock deadlocks.
  A cold-tile inference itself is never interrupted; it parks the fiber anyway.
* **A node is ONE `RenderMesh` + its `RenderNode`, no `ObjectContainer`** (see RendererVK "Single
  meshes"): no per-node material (every node shares `m_material`, `createMeshMaterial(TerrainLit)`),
  names or node tables, and no LOD chain (the quadtree IS the LOD). A `Resident` declares its `mesh`
  FIRST so it is destroyed AFTER the `node` that draws it; a retired node frees its mega-buffer
  ranges and MeshInfo slot, **so residency stays bounded across a session.**
* Each resident registers a `SpatialEntry` on `SpatialLayer_Terrain` with **`spawnVisible = false`** —
  nodes stream in off-screen constantly, and the guard would pin each one in the main pass until it
  first entered the frustum. **Its userData is the `Resident*`**, so the hand-over push needs no
  lookup and reaches the node AND its vegetation. Its sphere is the node's bounds grown for the
  vegetation (a plant up to 64 m tall, its crown up to 16 m past the node's edge). That address must
  outlive every list that holds it: residents are heap-held (`m_residents` maps to a `unique_ptr`), and an evicted /
  cleared one is **retired** (`retireResident`: culling entry, node and mesh released at once; the memory kept in
  `m_retired` until `visibleCollectGeneration` moves past its value, freed at the top of `update`). The walk meets its
  destroyed node and `renderNode` skips it.
* **The push is `render(renderer, ocean)`, not `update`.** main.cpp calls it right after
  `ocean.update` (which may rebuild the sector grid). It kicks `ocean.render(gate)` first, then its
  own job, whose ONE walk of the hand-over pushes every value: a tagged ocean sector's `RenderNode*`
  or a node's `Resident*` (its node, and its vegetation - below), each with its node's own pass mask
  (nodes `PASS_ALL`, sectors `PASS_MAIN` or 0 when dry). Nothing is resolved on main. It runs with the terrain disabled
  too (the walk still pushes the ocean's sectors; `m_renderReady` = "update ran enabled" gates the node-only parts).
* **The render push never walks the ring.** Two sets:
  1. **Main-visible nodes** come from the cull job's Main stamp through the Spatial visible-set
     hand-over, collect slot 1 (`setVisibleCollect(SpatialLayer_Terrain, 1, ECollect::UserData)` in
     `initialize`, `visibleUserData(1)` in `render`) → `PASS_ALL`. The slot holds userData VALUES
     read in the cull job (zeros dropped there), so the walk reads no pool row; a node evicted since the stamp is a
     retired, destroyed node, skipped by the push.
  2. **Main-culled nodes keep shadow + GI** (the ground behind the camera must stay in the TLAS and
     the sun cascades), but only inside a `forEachInSphere` around the renderer's scene focus with
     radius `max(shadow maxDistance + casterPad, giTlasRange())` — the range past which the GPU
     shadow cull and the TLAS range bound drop the push anyway; farther ground gets its sun shadow
     from the terrain march over the baked height map. `MainOnly` skips this set; a main-stamped hit
     is skipped by the query itself (`forEachInSphere(..., SpatialPassBit_Main)`), tagged sectors and
     zero entries by the emit. Culling `Off` keeps the plain walk over every registered resident.

  **The walk and the pushes run on workers** (`"terrainRenderPush"`, High, `m_renderCounter`); main.cpp calls
  `joinRender()` right before `present`, and `update` / `clearResidents` join it before they touch `m_residents`. The
  job fans the hand-over list out over a High `parallelFor` (`"terrainRenderPushChunk"`, auto-grain `m_renderPushCost`)
  and runs the sphere query BESIDE it as a job of its own (`"terrainRenderPushSphere"`), then merges the vegetation and
  hands the main-drawn nodes to `RiverSystem::drawChunks` (see [Rivers.md](Rivers.md)). **The two sets are disjoint**
  (the query skips main-stamped entries, and the list holds each entry once), which `renderNode` needs: it writes the
  node's transform upload state. Culling `Off`'s walk over `m_residents` stays serial (a debug mode).
* **THE VEGETATION IS STORED IN THE NODES** (`setVegetation(lookup, sink, numChunks)`, TreeSystem's GPU
  tree set; its chunks are the BASE chunks): every resident holds, per base chunk it covers (4^lod atomics + a count the
  walk skips empty nodes by; only nodes near the camera carry any - `Trees/Near radius`), the index of that chunk's
  vegetation (`lookup`, -1 = none; re-stamped on every resident by `setVegetation`, which joins the walk first). The
  walk notes them with the node's pass mask (main-stamped `PASS_ALL`, the shadow/GI sphere `PASS_SHADOW | PASS_GI`),
  merges a chunk drawn twice (a swap's old + new node: the masks OR'ed), and hands the frame's
  list to `sink` at its end, on the walk's worker. The pushes note it from several workers: each mask is
  OR'ed through an `atomic_ref`, and the push that first sets a chunk's mask lists it in `m_vegTouched`
  (one slot per vegetation chunk, `m_vegTouchedCount`); the list order does not matter to the tree culls.
  `vegetationRouted()` = this frame's walk carries it (the nodes draw and a sink is set); otherwise the owner submits
  its vegetation itself.
  **`restampVegetation(coord)`** (TreeSystem's world mode adds / removes one base chunk): re-stamps that chunk's entry
  in every resident node over it (retired ones to -1) from `lookup` WITHOUT joining the walk - the stamp is an atomic,
  the walk sees the old or the new index, and the owner keeps a removed index unused until the next frame.
* **THE GRASS STANDS ON THE NODE MESHES** (RendererVK "Procedural grass"): at the end of every enabled `update`, every
  BASE chunk within `renderer.groundRange()` (a lookup per LOD each, no walk) goes to `Renderer::setGrassGround` with
  the finest DRAWN node over it: the chunk's first vertex in that node's mesh and its cells per side (`lod0Res >> lod`),
  plus the node row stride (`lod0Res + 1`, UBO `present_groundStride`) - a base chunk inside a bigger node is a sub-grid
  of it. The GPU reads their vertices. The renderer keeps the list ONE frame, so a disabled terrain simply sends none.
* **Requests:** when the ring moved (or a request ended without a resident: `m_wantedDirty`), every wanted leaf
  neither resident nor pending is appended to the pump pool, after publishing the ring the pump judges staleness by
  (`m_pumpRing`: no longer a wanted leaf = dropped at dequeue; nearest = smallest node edge distance).
* **Residency** (`updateResidency`, every frame on main, after the adopt):
  1. **Retire** a resident that is no longer a wanted leaf once its area is DRAWN by wanted ones - its wanted ancestor
     (a coarsening) or every wanted leaf under it (a refinement, down several levels if the bands say so), each
     registered and, with the culling gate, main-visible OR culled by a Main stamp since its registration
     (`registeredAt` vs `visibleCollectGeneration`: it is off screen, no hole can show there) - or when the old node is
     not on screen either; or at once when it left the ring or never drew (held). **Requiring every replacement
     main-visible is wrong**: one old node is replaced by many leaves, so a big on-screen node stayed forever, pinned
     the draw camera and mis-stitched every edge. While the culling is FROZEN no stamp comes and the old node just stays.
  2. **Move the draw camera** (edge stitching, below).
  3. **Register** (draw) the wanted leaves the draw camera allows. A refinement's leaves wait for their whole group -
     every wanted leaf under the drawn node they replace resident - and register together, so an old node and its
     replacements overlap no longer than the one-frame stamp latency. Adoption itself never registers.

* **The mesh upload is a job too** (`"terrainUpload"`, Normal, `m_uploadCounter`).
  `Renderer::createMesh` copies the node into the staging ring. Every node is ~1.2 MB (see "Node memory" below), and a
  wrap of the 100 MB ring waits a staging fence, so uploads on main cost tens of ms. `update` only PICKS the batch into
  `m_uploads` (cap "Uploads/frame" AND "Upload MB/frame", 48 MB default; the first node always goes; the picks stay in
  `m_pending`). **main.cpp kicks it right AFTER `present`** (`kickUploads`), so it runs through the frame-pacing
  wait, and **joins it before the next frame's "Frame kicks"** (`joinUploads`): `createMesh` grows the
  MeshInfo / per-mesh tables that the begin-frame job and the entity pass's `renderNode` read.
  Everything it calls is thread-safe (the MeshDataManager alloc mutex, the staging mutex, the renderer
  `m_spawnMutex`), like parallel entity spawning. **The batch fans out across workers**: a grain-1
  `parallelFor` (`"terrainUploadMesh"`), a node per task. That only pays because the staging memcpy runs OUTSIDE the
  staging mutex (RendererVK, `StagingManager`). The next `update` adopts the batch (`"adoptUploads"`:
  re-validate, `spawnMeshNode`, the vegetation stamp — all cheap, all on main; `updateResidency` registers). One more
  frame of latency. `clearResidents` joins it and drops the batch.

  Everything else runs on main: the config/model polling, the terrain param setters, the fog
  height-map handover (the bake itself is already a job — `HeightMapBaker::update` polls it), the wanted set, the
  requests, the upload pick + adopt, `updateResidency` (the retire releases GPU residency into the renderer and
  unregisters the culling entry - not allowed inside the cull job's kick/join window anyway) and `RiverSystem::update`.

### Node memory

Every node is the same `lod0Res` grid: `(lod0Res+1)²` vertices and `6·lod0Res²` indices, no skirt (edge stitching,
below) - ~1.2 MB (48-byte `MeshVertex`, 4-byte index). The wanted set is roughly 25 LOD0 nodes, then ~15 per coarser
level (each band is about one node wide in its own node size). A retired node's ranges free at once
(`retireResident` -> `RenderMesh::destroy`), so residency follows the camera; the mega-buffers never shrink - the
freed ranges are holes for later uploads.

### Edge stitching (no skirts)

Two neighbouring nodes can be different LODs; the finer one has extra vertices along the shared edge that the coarser
one does not (T-junctions: cracks). Instead of a skirt, **the terrain VS snaps the finer side's edge onto the coarser
side's straight edge**. Every node has the same grid, so a node one level coarser has a vertex on every 2nd vertex of
the finer one's edge (aligned: a node's edge starts on its coarser neighbour's lattice).

* **The data** (`generateChunk`, `TerrainChunkMesh::stitch`, uploaded in `MeshVertex::tangent` - the terrain shades
  without tangents): per vertex, xyz = its height on the straight edge of a neighbour 1 / 2 / 3 LODs coarser
  (`TERRAIN_STITCH_LEVELS` = 3; the own height off the edges and on the coarse lattice), w = the node's LOD. Shared edge
  points are sampled at the same world points by both nodes, so they match exactly.
* **The shader** (`instanced_indirect_terrain.vs.glsl`, `terrainStitchedHeight`): a vertex on one edge (found by its
  local x / z = 0 or the node size; a corner is on every lattice) takes the two BASE chunks on either side of it (the
  same pair from both nodes), computes the quadtree leaf LOD over each (`terrainLeafLod`, at the draw camera), and takes
  `stitch[edgeLod - ownLod - 1]` when the coarser of the two is above its own. `terrainLeafLod` / `terrainRingLod` /
  `terrainNodeEdgeDist` mirror `Ring::leafLodAt` / `ringLodAt` / `nodeEdgeDist` **bit for bit** (the same float
  operations in the same order - `ringLodAt` is a threshold loop, no log2): both nodes of an edge must agree. UBO
  `terrain_stitch` (base chunk size, 0 = off; the draw camera) and `terrain_stitchBands` (`Renderer::setTerrainStitch`,
  every enabled update). Tweak `Terrain/Edge stitching` (off = the cracks, a debug view).
* **The DRAW CAMERA - why the shader cannot use the ring camera.** A node's drawn LOD differs from the wanted tree until
  its replacement lands (seconds on a cold tile). Both sides compute the same edge LOD only if no drawn node is
  COARSER than the leaf the shader computes over it (a finer one has every vertex it needs). So the streamer keeps
  `m_drawCam` (a `Ring`: camera + bands) with the **invariant: a resident is registered (drawn) only while it lies
  inside a leaf of the draw camera's tree** (`lod <= m_drawCam.leafLodAt(firstBase)`):
  * a wanted leaf too coarse for it (a coarsening behind the camera) waits unregistered (held); refinements always fit;
  * a retire needs REGISTERED replacements (a held one cannot take over, on screen or off);
  * the draw camera FOLLOWS the ring camera as far as the registered residents allow: `updateResidency` tries
    `DRAW_CAM_STEPS` (8) points from it to the ring camera (quarter-chunk lattice, the ring's bands) against every
    registered resident and takes the farthest that keeps the invariant, before registering. So refinement ahead is
    never delayed, and in fast flight the draw camera trails only by the generation lag (the pending refinements
    nearest the ring camera block the last steps) - not by the flight length.
  * **The UBO lags one frame** (the begin-frame job builds it before `terrain.update`), and a node registered in an
    update draws in the main pass only from the next frame (its first stamp) - so this frame's drawn set always keeps the
    invariant for the camera the UBO carries. Culling `Off`'s walk skips held nodes.
* **Not stitched:** the shadow pass (`shadow_depth.vs`), the ray-tracing BLAS, the grass and clutter (they read the
  unsnapped vertices) - all off by the tiny T-junction error at a LOD edge. **Tessellation:** every edge to a coarser
  node is >= `fullResDist + lodStep` chunks (~460 m) away, past the tessellation fade end (75 m), so the displaced
  relief only meets LOD0 against LOD0.

### It owns THE world datum

Handed out, never duplicated:

| Accessor | Consumer |
|---|---|
| `seaLevel()` | **The ocean floats on this rather than owning a second copy** — the generator builds its heights around it (moving it regenerates nodes), and the swash gate compares the two, so a fork switches the swash off everywhere. Valid even while terrain is disabled. |
| `activeClimateMaps()` | The collider, the trees, the rocks, the clutter. **nullptr while terrain is disabled** — consumers treat that as "no terrain" rather than sampling a field that is not drawn. |
| `activeWaterReach()` / `activeFlowField()` | The ocean's shore bake, **so the two bakes cannot drift apart.** |
| `activeTerrainData()` | The ocean's CPU buoyancy and wind-steering votes. **A re-bake swaps in a NEW object**, so a consumer holding the old `shared_ptr` keeps a coherent snapshot. |
| `sampleInlandWaterHeight()` / `hasInlandWater()` | The App's buoyancy (rivers and lakes; see [Rivers.md](Rivers.md)). |

### The terrain-data map

Baked around the camera and shipped to the GPU through `Renderer::setFogTerrainHeightMap`.

**Two cascades, RGBA32F, cascade-major** (`BakedTerrainData`; the layout matches
`terrain_height.inc.glsl`). "Data map range (m)" (4096) near, "Data map far range (m)" (8192) far, same texel count:

| Channel | Contents |
|---|---|
| R | terrain height (world Y) |
| G | water surface level |
| B | 4×8 packed bits — fog thickness, flow direction (bits 8–15), sea-level temperature, humidity. **Bit-cast, never float arithmetic** (some packs are NaN bit patterns) |
| A | macro altitude |

Consumed by fog terrain-follow, ocean depth/level (GPU through `terrain_height.inc.glsl` plus the CPU
copy), terrain colouring and the terrain sun march.

It also bakes the splat textures, PACKED into THREE per set (one texture fetch fewer per splat layer than the
four sources): `diffr` = albedo + ROUGHNESS in the alpha (the ARM's G; BC3; floored to
>= 3/255 in every mip - **the splat shader relies on it**, no GPU floor; the floor is in the file name,
`<stem>_diffr_f3.dds`, so a change re-bakes, and a bake deletes the unfloored `<stem>_diffr.dds`), `nor` (BC5),
`hao` = HEIGHT (the `disp` source's R, 0.5 = flat without one - the terrain relief's input: the parallax +
height blend in "Terrain/Textures", the displacement in "Terrain/Tessellation"; see
[`Docs/Context/RendererVK/Terrain.md`](../RendererVK/Terrain.md)) + AO (the ARM's R; BC5). A set's `heightScale`
(`TerrainTexSource`, default 1) scales its baked height about 0.5 - the relief of that set alone (Moss002: 0.25); the
scale is part of the file name (`<stem>_hao_h25.dds`), so a change re-bakes. The ARM's metalness is dropped (terrain is
never metallic). A packed output is stale when EITHER of its sources is newer. Compressed to `Assets/Local/TerrainTex`
— a background job kicked ONCE, at startup while the terrain is enabled or from `updateTerrainTextures` when it is
enabled later (`kickTexBake`; a disabled terrain never reads the source image sets); **the TERRAIN
shader falls back to flat colours until that bake finishes.** Each entry's climate box registers with
its textures (`TerrainSplatMaterial::climate`: temperature as t01, precipitation in mm/yr); the renderer divides the
precipitation by the live "Terrain/V3/Precip for full humidity" (`buildUboTerrain`). Each GROUND entry
also carries a `.grass` amount (0..1, `TerrainSplatMaterial::grass`): the grass grows where the splat draws that
texture, by that amount (RendererVK "Procedural grass").

The streamer also registers the **"Terrain/Water" tweaks** - ONE wetness field (rain, the ocean's swash,
submersion) driving ONE water surface: the field (texel size, update rate, diffusion, rain, dry time +
temperature sensitivity, wet-in, slope drain), the surface (fill start / full / curve = how far the wetness
fills the splat relief, the film max slope + slope fade that sink it on slopes, the film flow speed + min slope + cycle
(ripples running downhill), edge fade), the hand-over to the ocean (ocean blend = the film fading into the
ocean's water, ocean edge fade = the ocean blending out at its edge over the film) and the look (waviness, normal
scale, wind ripples, water (film) / wet ground / underwater ground roughness, wet darkening, darkening / roughness
thresholds, wet normal scale, glint size / coverage / roughness, the drying pattern = dry islands from a world fBm:
strength, darkening / roughness edge, size, relief share, contrast). **The renderer reads them straight from
`Globals::settings.terrain`**; the streamer pushes only `setTerrainCragScale` (V3's world scale) from
`updateTerrainTextures`. The clipmap itself - swash injection, decay, the toroidal window - is the renderer's
(`TerrainWetnessPipeline`; see Terrain surface water in [`Docs/Context/RendererVK/Terrain.md`](../RendererVK/Terrain.md)).

## `HeightMapBaker`

The async snapshot bake driver: **ONE Low job in flight** (JobCounter-polled), re-baking on camera
drift past range/4 or a config change — **configs carry `operator==` so tweaks stay live**.

**Two passes for a NEW sampler.** The near cascade at Full detail is every full-detail tile under the
near range — at 4 km and sub-metre mpp ~1000 V3 tiles, ~1.5 s each cold, serialised on the one
inference lock — and the map ships all-or-nothing. A sampler the baker has not shipped yet (first
map, reseed, config rebuild) therefore gets a **quick pass first — both cascades at Coarse detail,
seconds** — then a Full-near re-bake at the same centre replaces it. Coarse and Full share the fitted
climate baseline, so the textures land with the quick pass; only the near heights refine later. A drift
or rule change on a known sampler goes straight to Full (its tiles are what the mesh streamed).
**A bake whose inputs go stale while it runs is cancelled** (an atomic checked between 16-row bands of
`sampleGrid`, so at most one band of tile fetches is wasted), and teardown cancels before it waits.

**Centres snap to the COARSEST cascade's texel lattice**, so no cascade's features ever swim as the
camera moves. Both cascades measure the same world distance at their own texel size, **so near and far
agree.**

**The water-level channel bakes EXACTLY what the sampler reports** (no land-burial dive): the ocean clipmap's coarse
outer rings vertex-sample it, and a triangle bridging a flat water texel to a sunk land texel sags the far sea.

### `applyWaterReach`

> **"Sea level everywhere is a lie the ocean believes."** A generator that models no lakes (V3)
> reports the ocean's level at EVERY point on the planet, so every scrap of terrain within a metre of
> it — an inland hollow, a river flat — reads as shoreline and the ocean runs swash up it.

**The swash already has the right gate**: it fades where the baked water level departs from sea level,
which is how landlocked water is meant to be excluded. **So the fix is not a new rule, it is telling
the truth** — a chamfer distance-to-ocean pass sinks the baked water level under any ground the ocean
cannot reach ("Ocean reach (m)", "Ocean reach feather (m)", "Ocean reach drop (m)", "Ocean swash depth"), and the
existing gate does the rest. The terrain shader's beach overlay keys on the same field, so inland sand goes with it.

* **"Can reach" is a DISTANCE question, and the bake is the only place it is cheap**: it owns the whole
  camera-centred height grid, so one chamfer pass gives every texel its distance to real ocean in world
  metres. A per-point generator query would have to search its own field per sample, and the Coarse
  level has no fine elevation to search at all.
* **It applies to SUBMERGED ground as much as dry land.** A plateau at −0.05 m is under water by
  definition, so a land-only rule could never drain one — and those are exactly what V3 produces,
  kilometres of sea-level film with no ocean in reach. The clipmap rides the baked level, so sinking it
  here **actually removes the water** rather than merely muting its swash.
* **A real shelving bay is kept wet by the RADIUS**: its apron is too shallow to seed itself, but it
  sits close to water that does. A radius shorter than the apron makes the sea retreat off it.
* **LAKES are never touched** — a lake's surface already differs from sea level.

### `applyFlowField`

The 8-bit per-texel flow direction: **toward land through the surf zone, downhill elsewhere**, eased
back into the offshore `windAngle` the app pushes in each frame (`setFlowWindAngle`).

**It runs AFTER `applyWaterReach` on purpose** — reach decides what is water at all. Baked into both
maps' flow bits, and handed out through `activeFlowField()` so the ocean's shore map bakes the SAME
directions: otherwise the wave travel direction would turn where one map hands over to the other.
