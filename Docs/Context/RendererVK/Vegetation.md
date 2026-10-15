# Vegetation: grass, clutter, wind, far trees

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Procedural grass (`GrassPipeline`, "Grass" tweaks, `GrassParams`)

**Built 2026-10-03; defaults tuned by the user the same day** (1024 blades per 2 m patch, 1 m blades 0.15 m wide).
Desktop only (no VR). Blades near the camera (default range 145 m);
the far tiers (the terrain shading taking over the grass look) are not built yet.

* **Patches of RANKED blades** (`grass.inc.glsl`): a world grid of patches (`Patch size`, 2 m) around the camera. ONE
  static index buffer (`GrassPipeline::buildIndices`) holds, per LOD (8 / 4 / 2 / 1 segments - they NEST, for the geomorph; switches at `LOD 1/2/3 distance`, 10 / 30 / 60 m), `Blades per patch` blades in
  RANK order; blade k stands at the k-th point of an R2 low-discrepancy sequence over the patch (offset per patch), so
  EVERY PREFIX of the ranks is evenly spread. A patch draws the first K blades. The vertex shader keeps blade k where
  `(k + 0.5) / N < density x thinning` and shrinks it into the ground over the `Grow band` below that: ONE rule thins by
  density (climate, slope, clumps) AND by distance (`(Thinning start / d)^exponent`, faded to none over `Range fade`),
  so a blade fades by shrinking, never pops. Where the COVER fades (the corner densities: climate, slope, water, snow -
  not the clumps) the blades are also shorter and narrower by it (`Size by cover`, default 1), not only fewer. The
  CLUMPS (`grassClump`): value noise of `Clump size`, thresholded at `Bare fraction` (0.45: about that share of the
  ground is the bare / thin part) over +-0.2, mixed in by `Patchiness`. Fewer
  full-size blades read as sparse long stalks. The kept blades widen by `(1 / kept)^Width compensation` (capped) and are
  never narrower than `Min pixel width`.
* **No vertex buffer:** the index buffer holds vertex IDS (`blade << GRASS_BLADE_VERTEX_SHIFT | vertex`), and
  `grass.vs.glsl` builds the blade: a quadratic Bezier from the root (control point above the root at the tip's height,
  tip pushed by a random lean + THE wind (see "The wind": its direction, speed and gusts x `Bend` per m/s with a sway,
  plus a value-noise ripple moving with it), its width
  narrowing to a one-vertex tip. **GEOMORPH:** over the last `LOD morph band` (fraction, 0.3) before the next LOD's
  distance, the rows the coarser LOD drops (the odd rows) move - positions, last frame's positions and normals - onto
  the midpoint of their neighbours, by the blade's OWN distance; at the switch the blade is the coarser one exactly. The
  patch's LOD comes from the nearest point of its box (with slack below the corners), so no blade of a coarser patch is
  nearer than the switch. **Two shading normals:** NEAR, the curve's own normal + the full edge tilt (the
  user's preferred look); it eases (per blade) between `LOD 2 distance` and `LOD 3 distance` into a normal LINEAR in t
  (root normal -> the upper-half chord's normal, edge tilt x (1 - t)), which every LOD interpolates the same. The
  curve normal (horizontal tangent at the tip) spread over the single-triangle LOD made a brightness step at its
  switch. Motion vectors: the same blade at LAST frame's time (`u_grass_prevTime`). The wind
  (not the lean) eases out between `Wind/Fade start` and `Fade end` (100 / 200 m): far blades moving read as grain.
* **Roots ON THE TERRAIN MESH:** the blades read the chunk's own vertices from the vertex mega-buffer (binding 14) and
  interpolate the cell's two triangles exactly as `TerrainGenerator.cpp` splits them (`grassGroundHeight`) - the baked
  height map (8 m texels) is far too coarse. Procedural's `TerrainStreamer::update` hands the BASE chunks within
  `groundRange()` (+ 16 m: the grass's AND the ground clutter's range) to `Renderer::setGrassGround` EVERY frame (coord,
  the chunk's first vertex in the finest drawn quadtree node over it, its cells per side, and the node meshes' row
  stride - UBO `present_groundStride`: a base chunk inside a bigger node is a sub-grid of it); the list lives ONE frame
  (a frame without it draws no grass: a disabled terrain or freed nodes are never read). `present` writes it into the slot's host-visible GROUND TABLE
  (`GrassFrameGpu`: the patch grid + a `GRASS_TABLE_DIM`^2 chunk table around the camera, the finest resident per cell;
  `uploadGrassFrame`) - also with the grass off while the clutter is on (no patch grid then): the clutter cull reads the
  same table (`m_groundTableValid`). The tessellated relief (`terrain_tess.tes.glsl`) is not sampled:
  the root sinks by `Root sink` + half the relief depth (the same fade and slope gate) and the blade grows by as much,
  so no blade floats over a displaced hollow.
* **The cull** (`grass_cull.cs.glsl`, a cached compute secondary "Grass cull" before the shadow cull): one thread per
  patch slot (`GRASS_MAX_PATCHES` = 65536; the range is capped so the grid fits), range and frustum tests, the 4 corner
  heights and DENSITIES - **the terrain textures' own logic**: `terrainLayers` (terrain_splat.inc.glsl, included with
  `TERRAIN_SPLAT_HEIGHT_ONLY`), fed as the terrain VS feeds it (the baked fields, the temperature at the height, the
  SMOOTH mesh normal from the chunk's vertex normals: `grassGroundSmoothNormal`). Density = what the beach, rock and
  snow layers leave of the GROUND x the grass amount of its climate pick's textures (`u_terrain_splatGrass` per slot,
  `TerrainSplatMaterial::grass`, set in Procedural's `TERRAIN_TEX_SOURCES`: grassland 1, savanna 0.6, cracked steppe / sand / scree 0)
  x (1 - `Cover/Canopy thinning` (0.3) x the FOREST FLOOR MAP's canopy (the ground clutter's: binding 8, the clutter
  frame; no map = no thinning): a closed canopy holds little grass. The ground's canopy shading under the blades
  (`grassGroundCanopy`) does not see the thinning.
  No grass until the terrain texture set is registered (the startup bake). The splat's relief height blend is not
  applied (linear coverages). Corners are shared, so the bilinear density is continuous across patches. Output per visible
  patch: a `GrassPatchGpu` record (32 B) and a `VkDrawIndexedIndirectCommand` (`firstInstance` = the slot - needs
  `drawIndirectFirstInstance`, enabled in Device.cpp), counted atomically.
* **The draw:** `m_grassPipeline` in `StaticMeshGraphicsPipeline` (built from the main layout like the tess terrain, so
  the SAME descriptor set binds: the lit core), `drawIndexedIndirectCount` after the tessellated ground and before the
  film; the record is an INSTANCE-RATE vertex attribute (binding 0). Two-sided, no discard (early depth), opaque
  family, writes motion. `grass.fs.glsl`: the lit core's `computeLitColor` **without RTAO** (`LIT_NO_RTAO` compiles
  the AO read out: traced from the depth image, a blade is a near-random normal a few cm over the ground, not in the
  TLAS - the rays hit or missed the terrain at random, heavy noise under trees; the root occlusion stands in); the sun shadow looked up from the blade's
  sun side; albedo root -> tip + per-blade variation + dry patches (sRGB tweaks, linearised in `buildUboGrass`);
  `Cold darkening` scales the albedo down as the patch's mean temperature (the cull's 4 corners, at their height; it
  rides the record's `cellTemperature` half with the cell size) falls from `Warm temperature` to `Cold temperature`; `Root
  occlusion` stands in for the blades shadowing each other (they are not in the TLAS and never cast into the scene
  cascades - the canopy and the near grass cascade, below, do that work); the
  sun shadow LOOKUP moves toward the sun by `Shadow bias` (0.5 m at the root, none at the tip): the sunk root sits
  under the ground's own shadow-map surface (a dark band at every blade's foot);
  `Transmission` lights a blade from behind; the normal blends toward the ground's with distance (`Ground normal
  blend`), with NO minimum N·V (the bright grazing light is wanted; clamps were tried and removed 2026-10-03/04). The CURVE
  normal (`grassBladeNormal`, near blades) is `cross(tangent, side)`, and the tangent at the TIP is the horizontal
  lean + wind offset: an upright blade (offset ~0) or one bending along its width (offset ∥ side) made it ~0 - noise
  or NaN on the tip vertex, spread over the top triangle. Where `|across| / |tangent|` is small (0.05-0.25) it blends
  to the root's normal `cross(up, side)` (2026-10-04). `Blades per patch` rebuilds the index buffer (GPU idle + re-record); everything else is UBO-live
  (`u_grass*`).

* **THE CANOPY: grass self-shadowing without a shadow map** (`Grass/Shadows/Canopy shadow`, 0.10; 0 = off) - a blade
  is far below a shadow-map texel. The grass layer is a thin VOLUME of blades (`grass.inc.glsl`): its top at the mean
  blade height (`grassCanopyHeight`), its extinction = `u_grass_canopyExtinction` (the CPU fold: Canopy shadow x blades per
  m^2 x half the blade width) x the COVER x the size by cover. Its RESULT (the sun term with the flecks, and the
  ground's canopy ambient) fades linearly with the blades' `Range fade` (`grassRangeFade`); fading the extinction
  kept exp(-extinction x path) nearly black until the band's last metres - an edge. Sun at a point `depth` below the
  top (`grassCanopySun`): `exp(-extinction x mean clumps x depth / max(sun.y, 0.1))`, the CLUMPS sampled at 4 points
  along the sun path (a clump shadows the bare ground beside it, stretched away from the sun), then SUN FLECKS: a
  2-octave noise at the path's ENTRY into the canopy turns T into lit / dark spots (`Fleck size` 0.08 m, `Fleck
  contrast` 0.7; the mean stays about T), STRETCHED along the sun's horizontal direction into streaks like blade
  shadows (x 1 + `Fleck stretch` (1) / tan(sun elevation), capped at 16), shared by every point of that path (a fleck lights a blade and the soil
  under it), faded out from half `Fleck fade distance` (40 m) to it. The BLADES: the VS passes depth + extinction (location
  5; the depth is linear along the blade), the FS multiplies the sun visibility (direct + transmission). THE GROUND
  under them (`instanced_indirect_terrain.fs.glsl`, both paths): `grassGroundCanopy` from the SAME `terrainLayers`
  the splat uses (the untessellated path now calls `terrainLayers` + `terrainSplatLayers` itself) - the sun at the full
  canopy depth into `g_sunVisMaterial` (the ground's sun only; the film keeps its own), and the blades' `Root
  occlusion` x the canopy's opacity on the ambient. Formed right after the layers, so only two halves stay live across
  the splat (the terrain FS's register count is not measured yet). The shadow map keeps the big casters (trees,
  terrain); the canopy multiplies them.
* **THE NEAR GRASS CASCADE** (`Grass/Shadows/Near shadows`, default ON; never under RT sun shadows): real blade
  shadows around the camera, the canopy beyond. It is an EXTRA LAYER of the sun shadow array (`ShadowMap::initialize(...,
  extraLayer)`: layer `NUM_SHADOW_CASCADES`, 2048^2, its own single-view render pass + framebuffer), so the lit shaders
  read it through the binding they already have (`u_shadowMap`). The cascades' multiview pass transitions the WHOLE
  array view (UNDEFINED -> read-only), so this layer is drawn right AFTER it in the primary ("Grass near shadow"; its
  pass's source dependency waits for that pass's depth writes). Its matrix (`u_grass_shadowViewProj`, `buildUboGrass`):
  an ortho box of +-`Near shadow range` (8 m: 0.8 cm texels) down the sun, the cascades' construction, texel-snapped,
  a 50 m up-sun slab, centred AHEAD of the camera (a centred box spent half its texels behind the view): where the
  view's bottom-centre ray meets the ground + range - 1 m along the horizontal view direction - so the receivers'
  full-weight disc starts 1 m behind the bottom of the frustum (the centre XZ: `u_grass_nearCentre`). That ground
  point is capped at HALF the range ahead (was 4 x the range: a low camera looking near the horizon meets the ground
  far ahead, and the box slid up to ~5 x the range forward - the near grass fell out; 2026-10-03). The ground is the
  terrain height under the camera (`Renderer::setCameraGround`, from Procedural's TerrainStreamer every frame; unknown:
  the camera's height - 2 m. The far trees' `setFarTreeCameraGround` exists only while trees are on, and its sea-level
  fallback put the box at its then 4 x range cap far ahead). Casters: the cull's SECOND list (count [1], binding 7; the patches within range x 1.5 + 2 m of that
  centre, in view or not), `grass.vs.glsl` with `GRASS_SHADOW` + `GRASS_NEAR_SHADOW`. Receivers (`grassNearShadow`,
  grass.inc.glsl, only where the lit core is included): the blades and the ground under them - also bare ground in the
  box - 4 bilinear PCF taps, the receiver moved toward the sun by `Near shadow bias` (0.15 m) and off its normal by a
  texel, x `Near shadow strength` (0.7); its weight fades over the box's edge (85-98 %) AND over 1.0-1.3 x the range from
  the box's centre (the box is square in light space: on the ground it reaches range / sin(sun elevation) along the
  sun, past the casters - an empty, lit band there showed as a gap) into the canopy term (which it REPLACES inside: real blades instead of
  the statistical volume). The ground clutter reads it too (its rigid and flower fragments). `GrassPipeline::recordNearShadow` draws the list (the same
  records and LOD as the main draw: counts [0] main, [1] near casters, [2] records), depth only, the shadow pass's depth
  bias, both faces, its own set (UBO + vertex mega-buffer); `ClutterPipeline::recordNearShadow` adds the clutter in the
  same secondary. **REMOVED 2026-10-03:** the blades' casting into the SCENE
  cascades (`Cast shadows` / `Shadow distance` / `Shadow cascades`, default off) - its cull list, multiview VS variant,
  pipeline and record call; the canopy and this cascade replace it.

## Ground clutter (`ClutterPipeline`, "Clutter" tweaks, `ClutterSettings`)

**Built 2026-10-07** (Procedural CONTEXT "Ground clutter" has the types, the
`.clutter` grammar and the FOREST FLOOR MAP). Pebbles, fallen branches, mushrooms and flowers, placed on the GPU every
frame - no CPU memory per object, no CPU work per frame but the clutter frame's header. Desktop only (as the grass).

* **THE MODEL** (`clutter_cull.cs.glsl`): a grid of PATCHES (`Patch size`, 4 m) around the camera (`uploadClutterFrame`:
  capped to `CLUTTER_MAX_PATCHES` = 80^2, so the range is at most ~156 m at 4 m), ONE WORKGROUP PER PATCH, ONE THREAD PER
  RANK (`CLUTTER_CANDIDATES` = 256: at most 16 / m^2 per type at 4 m patches; 128 capped the dense flowers). Per clutter TYPE (a `.clutter` Placement block), rank k stands at the k-th point of
  an R2 sequence over the patch (offset per patch and type) and is KEPT where `k + 0.5 < density x Density scale x
  patch area x range fade` - the grass's ranked prefix: a thinning density removes objects evenly, and one near the
  threshold shrinks into the ground over `Grow band` (no pops). Types are INDEPENDENT lists (no type swaps for another as
  the camera moves; two can overlap). A rank past the type's bound (`ClutterTypeGpu::bound.x`: every term at its
  largest) never tests.
* **THE DENSITY** of a type at a point = Density x the climate fit (1 inside the ideal box, a Gaussian outside, as a
  tree's) x per term `mix(a, b, measure)`: the terrain's GRASS cover, CRAG (bedrock), BEACH - a 5 x 5 grid over the
  patch (every 1 m at 4 m patches) computed by threads 0..24 into shared memory (the grass cull's `terrainLayers` feed),
  bilinear per candidate; the 4 corners alone aliased the bedrock's small-scale noise into grid-aligned diamonds, and the
  stones gathered in a regular pattern; the GRASS measure is the blades' own density: that cover x `grassClump` x the
  canopy thinning, so no flower stands in a bare spot; a flower also SHRINKS with the cover as a blade does
  (`grassCoverSize`, "Grass/Cover/Size by cover"): at the shore's beach band the few blades left are tiny -, the
  climate's humidity (WET), and the FOREST FLOOR MAP's CANOPY, TRUNK and ROCK proximity (binding 4, the clutter frame,
  `clutterFloorAt`, bilinear); x (1 - snow) x (1 - occupied); x its cluster noise (`Cluster`) and fairy rings (`Ring`);
  0 above `Slope` (the drawn mesh's facet under it) or below `Altitude` above the water.
* **A KEPT OBJECT**: its scale (x the grow factor), a yaw then the turn to the ground's normal by `Align` (flowers stay
  upright), ON THE DRAWN GROUND - the chunk mesh + the TESSELLATED RELIEF there (`clutterRelief`: the TES's own
  displacement - layers, fade, slope gate, depth, height mip - from the splat height maps through the bindless array at
  binding 11, a seventh consumer of BindlessTextures; over a rigid object's footprint the height lies between the mean and
  the lowest of 5 taps; without it the objects floated over the relief's hollows, up to half the 0.6 m depth) -, sunk
  by `Sink` x its height, a sphere test against the frustum (or within the near grass cascade's caster
  reach - an object just off screen still casts), the LOD by its projected size (`LOD 1 size` / `LOD 2 size`, radius /
  distance) - flowers by distance (`Flower LOD 1/2 distance`). The record (`ClutterInstanceGpu`, 48 B: position + scale,
  the quaternion in halves, type / variant / LOD / kind, a hash, and the LOOK: both colours as rgba8 of their square
  root, the flower's stem / head / petals / open angle) goes to a flat array with a BUCKET key.
* **THE BUCKETS**: one per (mesh, LOD), the flowers' `CLUTTER_FLOWER_LODS` first. Three dispatches in the "Clutter
  cull" secondary: the CULL (atomic bucket counts + bucket-local indices), the PREFIX (`clutter_prefix.cs.glsl`, ONE
  1024-thread workgroup: an exclusive scan of the counts, one `VkDrawIndexedIndirectCommand` per bucket with
  `firstInstance` = its range, and the rigid draw COUNT), the SCATTER (`clutter_scatter.cs.glsl`: each record to its
  bucket's range). At most `CLUTTER_MAX_INSTANCES` = 131072 objects per frame.
* **THE DRAWS** (`StaticMeshGraphicsPipeline`, with the grass: the main layout, so the lit core's set binds; after the
  grass, before the film): the RIGID meshes (`clutter.vs/fs.glsl`; `drawIndexedIndirectCount` over the (mesh, LOD)
  buckets: the meshes' own vertex / index buffers, `ClutterVertexGpu` at binding 0, the record instance-rate at
  binding 1) and the FLOWERS (`clutter_flower.vs/fs.glsl`; `drawIndexedIndirect` of the `CLUTTER_FLOWER_BUCKETS` = 12
  flower buckets - (textured, topology, LOD), bucket (textured x 2 + topology) x 3 + LOD - over the flower topology
  index buffer, the record at binding 0, no vertex buffer: the 6 untextured buckets through the flower pipeline, the 6
  textured ones through the TEXTURED FLOWER pipeline - the same shaders with `CLUTTER_TEXTURED` on the FS, the only one
  with a discard). Two-sided, the opaque family,
  `LIT_NO_RTAO` (not in the TLAS; no RT, no GI of their own), the near grass cascade received; the flowers write motion.
* **THE RIGID MATERIAL** per kind: PEBBLE - the climate's BEDROCK (the splat's rock entries, the top climate pick,
  triplanar at 4 x the rocks' uv scale) x the type's colour; BRANCH - bark with streaks along the wood (part 0), end
  grain (part 1); MUSHROOM - the cap (part 1) with optional SPOTS, the stem (0) and the gills (2). The vertex AO, and
  `Contact darkening` over `Contact height` at the foot.
* **THE FLOWERS** (`clutter_flower.vs.glsl`): the STEM is a grass blade - the same Bezier, lean and wind
  (`grass_wind.inc.glsl`, moved out of grass.vs.glsl: `grassBezier` / `grassWind` / `grassTip`), so a flower sways
  exactly as the grass around it; motion vectors from last frame's time. The HEAD sits at the tip in a frame along the
  stem's upper chord: RADIAL (petals around a centre disc, tilted up by `Open`), SPIKE (florets spiralling up the top
  of the stem), UMBEL (small flat florets over a disc), BELL (florets hanging from the arching top), and the plants of
  the same path - REED, TUFT, GRAIN (a tuft of stalks with ears and awns), FERN (saw-toothed fronds), ROSETTE / LEAFY
  (weeds; `flowerLeaf`: a two-quad leaf) - see Procedural CONTEXT, `.clutter`. The topology per LOD: the stem's strip
  (4 / 2 / 1 segments) + `CLUTTER_FLOWER_PETALS` (16 / 8 / 4) petal quads + a centre quad; a slot past the flower's petal
  count collapses (no area), fewer slots than petals widen them. TWO TOPOLOGIES (`clutterFlowerTopology`: the bucket is
  topology x 3 + LOD): 0 for every head but the fern, 1 = the LARGE one (`CLUTTER_FLOWER_BIG_PETALS` 32 / 18 / 8) for
  a fern's fronds - in one shared topology every flower would run the VS for 32 slots. The FS: the grass's root -> tip
  colours on the stem, the type's colours on the petals and the centre, transmission (`Flower transmission`). Its
  normal is turned to the VIEWER by N.V (the VS normal is one face, bent, not tied to the winding - a gl_FrontFacing
  flip left cards facing away, N.V = 0, and the sun's GGX lit them very bright at some view angles), then bent toward
  V up to the foliage's min N.V (`u_foliage_minNoV`), as the edge-on leaves.
* **TEXTURED FLOWERS** (2026-10-13; ferns, radial petals, grain ears, umbel / spike / bell florets): a flower type's texture is a bindless
  `u_textures` slot (`Renderer::createTexture` / `releaseTexture`: RGBA8 sRGB with the caller's mip chain, no material).
  Its index + 1 rides `ClutterTypeGpu::terms5.y` (0 = none); the cull copies it into the record's `data.x` (a flower has
  no quaternion) and picks a textured bucket. The textured part (part 1) becomes a strip of the part's full width, u
  across it (`out_flower.z`), v along: a FERN frond (4 / 3 / 2 quads), a RADIAL petal or an UMBEL / SPIKE / BELL floret
  (a rectangle, the outline in the texture), a GRAIN ear (ONE strip over the ear and its awns, `CLUTTER_GRAIN_STRIP_WIDEN` x the ear's width for the
  fanning awns: the awn quad collapses; its stalks are `CLUTTER_GRAIN_STALK_HALF_WIDTH` x the ear's width at the top -
  the texture's rachis width - not the flower stem's width). The FS samples part 1 only
  (`textureGrad`, the derivatives outside the branch), discards alpha < 0.5, and takes mix(Color, Color2, G) x R (G: a
  grain's awns, a fern's rachis).
* **NEAR SHADOWS**: the rigid and flower VS with `CLUTTER_NEAR_SHADOW` into the near grass cascade (the casters within
  its reach). Nothing casts into the scene cascades (the user's choice, 2026-10-07). No alpha test there: a textured
  strip casts narrowed to about its cover (a frond 0.55 x, an ear 0.6 x, a petal 0.65 x).
* Off (`Enabled`, no types, VR, or no ground table this frame): `recordClear` zeroes the counts and the flower commands.
  `setClutterAssets` (GPU idle, re-record) / `setClutterFloorMap` (each slot takes it in present: the clutter frame's
  floor part is flushed only then) are Procedural ClutterSystem's. The GPU cost is not measured yet.

## The wind (`WindParams`, "Sky/Wind", `u_weather`)

ONE wind for everything that moves with it: speed (m/s, default 2), direction (degrees the wind blows TOWARDS, from +X around
+Y; 113 = the sea's former heading), gust strength (m/s) and gust size (m). Its readers:

| Reader | How |
|---|---|
| Weather particles (rain, snow) | `weatherWindAt` (particle.inc.glsl) + the rain's own `Particles/Wind sheet *` |
| Trees, grass, flowers | `vegetationWind` (`wind.inc.glsl`; the flower stems through the grass's `grass_wind.inc.glsl`): the mean + a 2D GUST VECTOR, each component two analytic waves travelling downwind (1.2 / 0.46 x the gust size long). The gusts turn the wind rather than cancel it (a signed gust along the mean left ~40 % of the trees still at a 1 m/s mean, 5 m/s gusts). Grass adds its small-scale ripple (value noise moving with the mean wind), both "per m/s" (`Grass/Wind/Bend`, `Ripple`) |
| Fog | the noise drifts along the direction at the speed (`u_weather_windSpeed`), world - drift: WITH the wind |
| Clouds | direction, speed x `Sky/Clouds/Wind speed scale` (3.5) |
| Ocean (Procedural) | `Renderer::getWindParams()`: speed x `Ocean/Waves/Wind speed scale` (2.5) = the U10, the heading + π (the spectrum's convention) |

(Until 2026-10-05: separate Particles / Grass / Clouds / Fog / Ocean speeds and angles.)

## Tree wind (`tree_wind.inc.glsl`, "Trees/Wind" tweaks, `FoliageParams` wind*, `u_foliage_wind*`)

Vertex-shader sway of the procedural trees in THE wind (see "The wind" below). **No textures, no per-tree or
per-vertex memory**: the gusts are `wind.inc.glsl`'s, and the per-vertex data rides the free MAGNITUDE of the tangent's w (RenderMeshData:
texCoords.z in (1, 2) = a tree mesh vertex's payload; >= 2 = the billboard / card codes, which mean "tree" too;
1 = no wind). Three layers:

* **Trunk** - lean (`Bend` x speed²) + sway, x (local height / `Reference height`)², the vertex dropping so the tree
  keeps its length. From the mesh-local height alone, so the mesh, the merged branch cards and the whole-tree
  billboard bend alike (crossfades stay aligned). The only layer of the cards / billboards and of the shadow pass.
* **Branch** - meshes only, faded out by `Branch fade end` (70 m, before the mid tier): the module bends with the
  placement's phase, the level-1 sub-branch on top with its own (both weights 0 at their root: continuous joints).
* **Leaf** - the leaf cards' tip vertices along the card normal, a phase per card, faded out by `Leaf fade end`.
* **Billboard waves** (instanced_indirect_foliage.fs.glsl, `treeWindBillboardUv`) - a whole-tree billboard (strip code: |tangent w|
  in [2, 3)) cannot bend its branches, so its TEXTURE lookup moves: two crossing waves over the card, growing toward
  the card's top (on the horizontal card toward its rim) from a still base, x the wind and `Billboard waves` (0.012 of
  the card), clamped into the card's own strip. Albedo, alpha (the outline ripples too) and normal use the moved
  coordinate (`texUv`); the card frame's derivatives and the crown math keep `uv`. Not in the shadow pass and not in
  the motion vectors (a texture-space motion). The branch cards (axis code) keep still.

`instanced_indirect.vs` applies it to any vertex whose payload says so (`treeWindPayload`) and adds last frame's
offset minus this frame's (`u_foliage_windPrevTime`) to the motion vector. `shadow_depth.vs` applies the
trunk layer to casters the shadow cull marks as trees (bit 15 of `alphaTexIdxCascadeMask`; the cascade mask is the
low 15 bits). Both culls grow a tree's bound by `u_foliage_windReach` (the reach at the strongest gust, from the UBO
build). Beyond `Trunk fade end` (3 km, a 100 m band) nothing. **The TLAS stays static**: RT shadows / GI / reflections
see unswayed trees.

## Far-tree volume (`TreeVolumePipeline`, "Trees/Far ..." tweaks, `FarTreeParams`)

**PROTOTYPE (T2 / P1), desktop only** (on by default; the user's 2026-10-02 pick:
start 500 m, end 20 km, overlap 32 m, 2048 × 1024 × 10 slices over 22 m (angular: keep a power of two - at 3072 a
band showed along u = 1/3, cause not found), checkerboard on, blob shrink 0.433, step scale 0.85, 500 steps, ambient
1, sun 1, self shadow 4, normal strength 1, ground darkening 1, interior shadow 2 / radius 0.077, forward scatter
−0.1, albedo 1, rebake 64 m). The FAR trees —
beyond the billboards, `Far start` to `Far end` — as ONE marched volume:

* **The volume:** camera-centred (snapped: re-baked once the camera leaves `Far rebake distance` of the bake
  centre, or a tree set / a geometry setting changes), **POLAR** (`tree_volume.inc.glsl`): image x = the angle
  around the centre (`Far angular resolution`; it WRAPS — the sampler repeats u, and every integer wrap goes through
  `tvWrapAngle`: GLSL's `%` is UNDEFINED for a negative operand, and the `((x % n) + n) % n` wraps in the splat and
  the march broke the columns just below 0 - a line of strange streaks out from the bake centre along world −X, jumping
  with every rebake, 2026-10-03), image y = `log(r / rMin) /
  log(end / rMin)` (`Far radial resolution`), so nothing is stored inside the ring and the cells grow linearly
  with the distance (a constant angular size). Image z =
  the height above the column's TREE FLOOR, `[0, Far height]`: an R32UI 2D image, the base of the column's
  **DOMINANT** tree - the one whose tent covers it most (floor pass 1, `m_floorCover`: the largest coverage per
  column, atomic max; floor pass 2: the trees within 4 quantization steps of that coverage write their base, the
  lowest among them - NOT an exact match: the passes are separately compiled variants whose float math can round
  a tree's coverage a few bits apart, and with an exact match some columns got no floor at all and the splat
  skipped them: radial gaps in the volume, a moire changing with every rebake). The
  LOWEST base of any tree reaching the column put a cliff-top tree above the layer in its edge columns, where the
  splat moved it down - blobs out of line with their billboards at every cliff (2026-10-02; a larger `Far height`
  hid it, messily). A column only the rectangle's **ring** reaches (coverage 0) keeps the lowest base there
  (atomic max of inverted order-preserving bits;
  0 = no tree; the ring: the march filters the density bilinearly but reads the NEAREST column's floor, so a
  filter-reached column without a tree floor fell back to the height map and put the leaked density tens of
  metres off - thin hatched spikes above the crowns, 2026-10-02; and the march's PRIMARY sample,
  `densityAtColumns`, filters per column: the 4 columns around the point, each read at its own height above its own
  floor, blended bilinearly - the hardware filter put a neighbour's crown in at this column's height, a spike over
  every steep floor step at peaks and slopes; the lighting taps read the hardware filter at a height above the
  sample's SMOOTH floor - `densityAtColumns`' 4 column floors blended bilinearly. Measured from each tap's NEAREST
  column's floor, as first built, the shading jumped at every column edge between two different floors: bright /
  dark vertical lines through the blobs, finer but as many at a higher angular resolution - the user's A/B,
  2026-10-03: they went with self shadow + normal + interior off), written
  by the splat shader's `TREE_FLOOR_PASS` variant before the splat, then **SMOOTHED** (`tree_volume_floor_smooth.cs`,
  `Far floor smoothing` = the tent radius in columns, default 2, min 1: always dispatched; a rebake setting): a separable masked tent
  blur, along the angle into `floorCover` (free after the floor pass), then along the radius back. A column without a
  floor keeps none and adds nothing to its neighbours. The dominant tree's base jumps whole floor steps between
  neighbouring columns on a slope, and with everything stored and read relative to it those steps showed as faint
  vertical lines and a moire on slopes (2026-10-03); a tree the smoothed floor no longer fits moves down into the
  layer through the splat's `shift`, as for any floor below it. Splat and march both measure from it (the
  march: the nearest column; a column without a tree falls back to `terrainHeightAt`). **Not the height map**:
  its far cascade's ~132 m texels put the ground tens of metres off on mountains, and trees fell out of the
  15 m layer — missing far trees on mountainous terrain that popped in as billboards (2026-10-02; the user's A/B:
  a larger `Far height` brought them back). A tree that would reach past the layer's top (a lower tree set the
  column's floor) is moved down into it, as far as its base allows. The march's under-ground break uses the
  LOWER of floor and map, its above-layer skip the HIGHER, capped at 4 cells. R16F extinction (1/m) + an RGBA8
  colour map. 2048 × 512 × 16: ~100 MB (R32UI accum + R16F + colour). (A SEPARABLE per-axis warp was built first
  and dropped the same day: it must keep fine cells over the whole inner square, thousands of wasted texels per
  axis at a 5 km start.)
* **Bake** (`tree_volume_splat.cs` → `tree_volume_resolve.cs`): one workgroup per tree of every GPU tree set
  (`createTreeInstanceSet` uploads `TreeVolumePieceGpu` per piece + `TreeVolumeTypeGpu` per type + the types'
  float extinction mip chains — `TreeInstanceType::density`, box-filtered to 1³ on upload). Every texel the
  tree's box touches (+ one texel each way) samples the type volume at the mip matching the texel footprint, at
  the texel centre clamped into the box, × the tree's TENT weight horizontally (per polar axis, one cell wide
  each way, `tentWeight`) and the slice's box overlap vertically, and ADDS it. **The tent is load-bearing:** with
  the trilinear reconstruction it keeps a small tree's blob centred ON the tree whatever the grid's offset; the
  first (box-overlap) splat put it at the cell centre, up to half a cell (8-60 m) off, so every re-bake (a new
  grid centre) shifted every blob — "shaking" while flying. The splat ADDS with fixed-point `imageAtomicAdd` (crowns overlap → extinction sums). Past 65535 trees the
  dispatch wraps into y. The colour column takes the tree type's albedo (last writer wins).
* **WORLD TREE RECORDS** (W4, 2026-10-04; `RecordSource`, TreeRecordPool "WORLD TREE RECORDS"): while record types
  are set (`setTreeRecordTypes(types, chunkSize, worldSeed, volumeSet)`, from TreeSystem's world spawn), EVERY tree of
  the volume comes from the records, near and far, and the world's DYNAMIC set is never splatted (`recordFarTrees`
  skips dynamic sets; their adds / removes do not re-bake). One source: a chunk entering or leaving the set changes
  nothing in the volume. (First version, the same day: the set splatted its own chunks and the records the rest, with
  a per-chunk "expanded" flag - the swap moved the blobs (two different representations), and the set and the flag
  reached the bake on different frames: a chunk doubled for a frame on the way in, empty for a few on the way out.)
  `TreeRecordTypeGpu` per record type (TreeWorld's name-sorted species, bushes too) mirrors TreeSystem::expandChunk:
  the variants as the world set's volume types (max 8), the Scale range, `sizeVariation`, a tree's bush types in
  expandChunk's order (max 8), `bushesPerTree`, `bushRadius`; per variant its extinction integral at scale 1; the
  species' 32-bin height profile at the mean scale, crown radius (scale 1), leaf colour. Two regions by the chunk
  centre's distance from the bake centre against `Far record detail (m)` (4000; a rebake setting):
  * **Within it** the splat's `TREE_SPLAT_RECORDS` variants (all three passes; ONE WORKGROUP PER RECORD: the bake
    picks the detail chunks on the CPU from the table's CPU copy, `TreeRecordPool::tableCpu`, with each chunk's first
    workgroup, `m_bakeDetail`, and a workgroup finds its chunk by a binary search - a workgroup per
    chunk ran its ~3000 plants in sequence: 31 ms per bake, 2026-10-04): each record EXPANDS exactly as the CPU does - variant, scale, yaw from `treeRecordSeed`, its bushes - on
    its chunk's GROUND grid (`treeRecordGround`: the 17² heights TreeWorld took from the generator's own field;
    `terrainHeightAt` only where the chunk holds none) and runs the per-tree body (`splatPiece`) into the world set's
    volume types. Detailed floors as any tree. (The terrain map alone, ~100 m texels in its far cascade, put every
    peak's trees under the summit - hidden until the camera came within its near cascade, ~2 km.)
  * **Beyond it** two passes, because 15 slices x ~9M records of atomics per bake would cost tens of ms:
  * `tree_volume_records.cs`, one workgroup per chunk: each record adds its exact variant's mass x its exact scale² x
    weight / the column's area into a 2D R32UI image (`m_floorCover`'s memory since 2026-10-07 - the floor passes and the smoothing are done
    with it; the stage zeroes it at its first frame - fixed point `TV_AMOUNT_SCALE`; saves 17.6 MB at 3000 x 1500) over a TENT as
    wide as its crown (at least a cell each way, at most 8 texels; per-axis weights normalized, so the tree adds its
    whole mass - far out a tree is smaller than a column, near the detail distance a column can be smaller than a crown
    and a point splat made pillars); its nearest column takes its type (`m_farType`) and colour. No bushes.
  * `tree_volume_far.cs`, one thread per column: a column with mass, or next to one (the march's floor ring), that
    no tree floored gets the GROUND under its centre (its chunk's grid, `terrainHeightAt` where none); then mass x the
    type's profile (its mean over each slice) into the accumulation. A column without a type takes a neighbour's,
    else the first type with mass. The column's type is the LOWEST of its records' (`imageAtomicMin`, 2026-10-06; it
    was last writer wins): a tree's before a rock's, and the same every bake.
  Record table changes re-bake (at most every 30 frames). `farTreesActive` is true with record types alone.
* **ROCKS** (R5, 2026-10-06, user-untested; Procedural's world rocks): rock records are
  record types like trees' and go through the same passes. A rock type is a **SOLID** (`TreeInstanceType::solid` ->
  the volume type's `albedo.w` = 0; `TreeRecordTypeGpu::albedo.w` = 0): its grid is OCCUPANCY (0..1), and its extinction
  is **`Far rock extinction (1/m)`** (4; a rebake setting - before `Far density` and the blob shrink) at ANY size - a
  crown's extinction thins with its scale (`/ scale`: its leaves spread over a larger volume), a rock's does not. So the
  far mass of a rock record is its occupied volume x scale^3 x that (a tree's: x scale^2). **DEAD WOOD** (2026-10-07;
  `solidOwnColour` -> `albedo.w` = 0.5, the record type's too): a solid's extinction and mass, but a TREE's colour write
  (its own albedo: its bark texture's mean) and no rock sum - the shaders test `w < 0.75`
  (solid) and `w < 0.25` (the climate's bedrock). A rock writes NO colour: it
  adds its share to **`m_rockSum`** (R32UI 2D, cleared per bake), in the accumulation's units summed over the slices -
  the detail splat its voxel amounts, the far records pass mass / area / the slice height (what the far pass adds over
  the slices). **The resolve** (its z = 0 invocations) then turns it into the column's ROCK FRACTION (the sum / the
  column's accumulation over the slices) and mixes the CLIMATE'S BEDROCK into the colour by it: the terrain's rock splat
  materials' mean colours - each diffuse texture's smallest mip (binding 7, `ROCK_TEXTURES` = 8 combined samplers, a
  sampler with no LOD clamp; `RecordParams::rockTextures`, filled by Renderer from `TerrainResources`, the fallback
  diffuse past `numRock`) - weighted by their climate boxes at the column's climate (`terrainClimateAt` + the lapse at
  the column's floor), over EVERY rock entry (a far colour needs no top-three pick). **The colour map's ALPHA is 1 - the
  rock fraction** (the clear and every tree write keep 1). **The march**: by that fraction the sun term loses its phase
  (`Far forward scatter`: a surface does not glow backlit), takes the full N.L (`Far normal strength` toward 1, only
  while it is > 0), and the interior darkening drops to a quarter. The march is 64 registers with it (2026-10-06).
  The hand-over: see "Hand-over" below - a rock mesh leaves the main pass at the same distance as a billboard.
* **THE BAKE IS SPREAD OVER FRAMES** (`Far bake frames`, 12; 2026-10-04 - in one frame the world records cost 6-7 ms
  of GPU per bake, after the per-record dispatch took it down from 31): `startBake` SNAPSHOTS what it reads that can
  change before it ends - the static sets' list, the record table and the chunk map (copied into the bake's own
  host-visible buffers), the detail chunks - and the record pool keeps every chunk it saw alive until NUM_FRAMES_IN_FLIGHT
  frames after it ends (`bakeHoldSince` -> `TreeRecordPool::update`). `stepBake` runs one share per frame, EVERY stage
  that grows with the resolution or the tree count spread over its part of `Far bake frames` (`setBakeBudgets`):
  * the CLEAR ~1/8: the 2D images whole at its first frame (small), the 3D accumulation a slice range per frame
    (`tree_volume_clear.cs` - `vkCmdClearColorImage` clears a 3D image whole);
  * the three SPLAT passes (floor coverage, floor, splat) together ~1/2: each walks one range - the static sets' pieces,
    then the detail records - within the frame's budget (`SplatPC::wgOffset` for both);
  * the smoothing (2D, a frame); the RECORDS' MASS ~1/8 in chunk ranges (`RecordsPC::chunkOffset`); the FAR COLUMNS ~1/4
    in radial-row ranges of whole 8-row groups (`FarPC::rowOffset`);
  * the RESOLVE ~1/8, IN PLACE: the column pass (the rock colour; it sums each column's fixed-point slices, so it runs
    first, `ResolvePC::columnPass`), then texel-layer ranges turn the sums into the extinctions as HALF FLOATS in `accum`
    itself (`packHalf2x16`, two slices per texel - below); then the max-floor grid (2D, a frame);
  * **`accum` PACKS TWO SLICES PER TEXEL** (2026-10-06; `tree_volume.inc.glsl` `tvAccumTexel` / `tvAccumAmount`): R32UI,
    `(slices + 1) / 2` layers (`m_accumDepth`), slice s in layer s / 2, its low (even) or high (odd) 16 bits - 144 MB
    instead of 270 MB at 3000 x 1500 x 15. Image atomics are 32-bit only, so the two 16-bit fixed-point halves (x 1024)
    share one `imageAtomicAdd`. **The limit: a sum below 64 / m per slice and texel** - it is the texel's MEAN
    extinction (each add weighted by the tree's cover of the texel: trees side by side in one cell share it by area;
    only crowns / rocks occupying the same space add fully - a rock is `Far rock extinction`, 4), and a single add is
    clamped to its half. Past it the half wraps and carries into the other slice (not guarded: no saturating image
    atomic). The clear, the resolve and the copy walk the layers; the rock sum (2D) stays 32-bit;
  * the HAND-OVER CROSS-FADE (`Far swap time`, 0.5 s of REAL time - Time's unpaused clock, `handoverFade`; no GPU
    work of its own, so a time and not frames: the same at any frame rate, where the bake's frame budgets keep the
    per-frame cost the same instead): the march's `TREE_HANDOVER` variant binds
    BOTH bakes - the new one through `accum`'s RG16F view (MUTABLE_FORMAT, `m_accumFloatView`; the hardware filters
    across the columns, `densityTexelNew` blends the slice axis from two layer fetches; bindings 13-16 with the
    back floor / colour / max floor) - and per RAY a dither picks one: the new bake when the pixel's noise (IGN, stepped
    by the golden ratio per frame) is under the UBO's fade (`u_foliage_handoverFade`, `handoverUbo`). The temporal pass / TAA
    averages the picks into an image-space blend, so the opacity mixes correctly (a per-sample density mix would keep a
    dense crown opaque until late in the fade). The whole ray follows its pick - the ring, the skip, the ahead-break -
    so the variant costs what the normal march does. **Without any temporal accumulation (temporal off, no TAA / DLSS)
    the fade shows as noise.** (Until 2026-10-06 the first version switched 256 m world chunks one at a time by a hash:
    a per-chunk pop.) The variant is used from the frame AFTER the hand-over began (that frame's UBO was built before
    the stage changed);
  * the COPY ~1/8: at fraction 1 (every chunk on the new bake) `tree_volume_copy.cs` writes `accum` into `density` in
    slice ranges - nothing reads `density` then - and the SWAP follows. **No cross-fade** (`BakeJob::crossFade`
    false) for the first bake (nothing shown yet) and after a volume GEOMETRY change (`sameGeometry`: start, rebake
    distance, end, height, resolutions - the march reads both bakes through the SHOWN bake's ring and layer, so the new
    one would sample at the wrong radius / height): the copy then runs whole in ONE frame and the swap follows in it.
  (Until 2026-10-06 only the record splat was spread: the accumulation's clear, the static sets' splats, the records' mass
  and the far columns each ran whole in one frame - a "Far trees" spike growing with the resolution - and the resolve
  wrote `density` in the swap's frame, switching the whole volume at once.) The bake writes the BACK `floor` / `colour`
  (two of each, +36 MB) and the shared `accum`; the march reads the FRONT ones and `density` until the hand-over. No
  second density: `accum` holds the new one. A new bake starts only NUM_FRAMES_IN_FLIGHT frames after the last swap
  OR DROP (`m_bakeStartFrame`: the snapshots are rewritten - a dropped bake's dispatches may still be in flight, and a
  grown snapshot buffer is re-created - and after a swap the old front, the new back, may still be read). `markDirty` re-bakes after a
  running bake; `invalidate` (a set destroyed, the record types or the pool replaced) drops it and a geometry setting
  change drops it too - **except during the hand-over / copy** (the march already shows part of it: it finishes, and
  the next bake follows). **The volume lags the camera by the bake's length plus `Far swap time`**: no gap shows while
  speed x lag < 30 m + `Far overlap` (the billboards cover the ring's hole up to there; `Far rebake distance` cancels
  out). The bake's part is in frames, so it grows in seconds at a low frame rate. **BAKE AHEAD** (`Far bake ahead`, on)
  cancels it at a steady speed: `trackCamera` keeps the camera's horizontal velocity (real time, ~0.25 s smoothing; a
  jump past 3000 m/s resets it), `bakeLead` = that x the LAST bake's real start -> swap time (`m_bakeDurationSec`),
  capped at `Far rebake distance` + 30 m. Both the re-bake test and the new bake's centre use camera + lead - where the
  camera will be at the swap - so a bake starts early enough and centres where it is needed. A wrong guess (a stop, a
  turn) is bounded by the cap, and the test (camera + 0 lead then) re-bakes at once. Without it: lower `Far bake
  frames` / `Far swap time` if a gap shows at the hand-over.
* **The ring's inner radius is a FIXED `Far start` − `Far rebake distance` − 30 m** (horizontal), around the BAKE
  centre: the camera may move a rebake distance before the next bake, and the ring must still hold every tree
  past the hand-over.
* **The hand-over follows the camera's HEIGHT** (`Renderer::farTreesStart`): `sqrt(start² + h²)`, h = the camera's
  height above the ground under it (`setFarTreeCameraGround`, fed by TreeSystem from the terrain sampler; the
  baked sea level until set) — the 3D distance of a ground tree at horizontal `start`. The tree cull's
  volume start (`u_present_treeVolumeStart`, + overlap) and the march's start both use it, so from the air the hand-over stays at horizontal
  `Far start`, where the ring begins. (With the 3D `start` alone, the billboards vanished under a high camera
  with no volume there. Tried and dropped the same day: shrinking the RING with the height instead —
  `sqrt(start² − h²)` — which spread the log mapping's radial texels from ~10 m to the far end and smeared the
  crowns into stripes, also with an asinh mapping.)
* **The ray** is `vol_apply`'s `fogRay`: `u_rayFromNdc`, from `u_mvp`'s x / y / w rows (not `u_invMvp`, whose float32 error re-rolls
  every frame — kilometres out it moved the blobs) through this frame's TAA-JITTERED sub-pixel position, as every
  raster pass samples (an unjittered ray had TAA un-jitter content that never was jittered); the scene distance
  from `viewRelFromDepth` at the jittered uv. The `u_invMvp` ray was the visible "position jitter" (independent
  of resolution, step and TAA - the user's A/B, 2026-10-02); the jitter alignment is kept for correctness.
* **The canopy's colour and sky light** (2026-10-04): the leaf colour (linear, as `meanLeafAlbedo` decoded it; LINEAR
  in an RGBA8 UNORM image) is mixed toward its Rec. 709 luminance by `Far saturation scale` (0 grey .. 1 as baked .. 2;
  a push-constant word, was `pad0`), then x `Far albedo scale`. The SKY term is the real sky: pi x the cosine-weighted
  mean radiance of GI's sky map (binding 10, layer GI - the clouds included) over 5 taps (the zenith 0.4, a ring 45°
  down 4 x 0.15: one cloud overhead does not set it alone), baked once per frame by the sky-map bake into
  `SKY_MAP_TREE_SKY_TEXEL` (one `texelFetch` per ray, 2026-10-04), x `Far ambient` (1 = physical); it was a
  fixed cool blue tint x the sun's luminance. The sky map is baked in the GI stage, before "Far trees".
* **March cost rules** (2026-10-02, measured in the sandbox with the 350² grove, RelWithDebInfo: "Far trees" 0.98 →
  0.62 ms, the march 72 registers + 48 B spill → 56 registers, no spill): ONE polar lookup (`polarUv`: atan + log)
  per step, shared by the cell size, the floor and the primary sample; the 12 lighting taps per lit step take their
  uv to FIRST ORDER around the sample's (`polarJ`, d uv / d xz - the taps lie within ~10 m of a sample >= ~400 m out,
  < ~0.1 texel off), a purely vertical tap keeps the sample's column outright (`densityAbove`: no lookup, no floor
  read); the height map only above the layer (the skip) or deeper than half a layer under the floor (the break);
  the phase, the sun / sky factors and the slice height per ray; the pixel-skip block loops unrolled with arithmetic
  schedule / neighbour offsets (arrays indexed at run time sat in local memory). **A column WITHOUT a floor** inside
  the layer takes no sample and steps at least a whole cell: the splat floors every footprint plus one ring, so
  such a column lies 2+ columns from any density and all 4 `densityAtColumns` taps would read 0 (exact, not an
  approximation - keep the ring if the floor pass changes). It sits in the step formula and the sample's
  `hasFloor ?` - as a branch of its own it cost the march 8 registers (56 → 64, measured 2026-10-02).
* **THE MAX-FLOOR SKIP** (2026-10-05; `tree_volume_floor_max.cs`, the bake's last step - after the far columns, the last
  floor writes): the BACK floor -> the highest floor per block of `TV_FLOOR_MAX_BLOCK`² (16²) columns (R32F
  `m_floorBlock`), then that DILATED by one block each way (the angle wraps; past the ring's ends nothing) into
  `m_floorMax[back]` (front / back like the floor; march binding 12). A column without a floor counts as
  `TV_FLOOR_MAX_NONE` - it holds no density. In the march, a point higher than its block's value + `Far height` lies
  above every tree within one block in every direction, at least `r x skipReach` away (`min(sin(16 x 2pi / angularRes),
  1 - exp(-16 x logSpan / radialRes))`: the block's tangential width as a chord, or the inner neighbour block's radial
  depth), so the ray steps that far (a descending ray at most down to that height) before the floor read - only when
  that beats the regular step (`skip > cell`). Sky rays, the air over the canopy and treeless land take ~4x longer steps
  than the 4-cell cap of the old above-the-layer skip. Exact (it never steps over density). The grid is RG32F: y =
  **AHEAD**, the highest floor of the block's row and every row farther out within `TV_FLOOR_AHEAD_SECTORS` (4) blocks
  each way - a RISING ray above it + `Far height` ENDS. Valid while the ray moves outward (`dot(rel, dir.xz) >= 0`: past
  its closest approach to the bake centre, r only grows) and its polar angle cannot leave those sectors: from radius r
  a straight ray turns by at most asin(d / r), d <= the camera's distance from the centre, so the test needs
  `d < r x sin(4 blocks)` (85 m at r = 436 m). Measured (sandbox, 1440p): Far trees 0.96 -> 0.39..0.53 ms (the skip)
  -> 0.34 ms (+ the end); the march stayed at 72 registers (re-deriving the pixel-skip block's pixels after the march
  instead of holding them: still 72, reverted).
* **The march is 64 registers, no spill** (2026-10-06, RelWithDebInfo, every variant; it was 71 / 72): a segment's
  distance weights are not summed - `sum(T x alpha)` telescopes to `1 - T`, so the weighted mean is `distSum / (1 -
  T)` (an opaque segment normalises its sum before its T is zeroed). No timing change measurable through the run noise
  ("Far trees" ~0.43 ms either way). The interior taps also skip at `Far interior radius` 0 (they sat on the sample:
  `exp(0)`). The bake's share of the frame is its own GPU scope, "Far tree bake" (`updateBake` + `recordBake`); the
  march runs in the post-scene compute group (`marchPrepare` + `recordMarchPhase`; see "Barriers and overlap" ([CONTEXT.md](CONTEXT.md))) - its
  own scope "Far trees" only with "Renderer/Overlap compute" off.
* **March** (`tree_volume_march.cs`, full res, every pixel, no temporal): from `Far start` (camera distance; at
  least the ring's entry, exact circle roots; a vertical ray never enters) to the scene surface or `Far end`; the
  **Lighting tweaks:** `Far sun scale` (the direct factor), `Far self shadow` (× the sun taps' optical depth),
  `Far normal strength` (the sun term toward `max(N·L, 0)` with N = −∇density by forward differences — the
  volume's "normal map"; 3 more taps per lit step, only when > 0; a crown-scale gradient blended into it was
  tried and removed 2026-10-02 - not worth its 3 taps), `Far forward scatter` (Henyey-Greenstein g),
  `Far ground darkening` (the sky light's drop toward the ground), `Far albedo scale`, `Far ambient`, and
  `Far interior shadow` / `radius` (the volume's "Foliage interior shadow": `exp(−strength × the mean extinction of
  6 taps at radius × the cell size around the sample × that distance)` on the sun and the sky — a blob's core is
  dense on every side; 6 taps per lit step, only when both are > 0).
  The volume FADES IN over `Far overlap` ON THE RESULT - of the BAND only: the ray accumulates two segments split
  at the fade end (`Far start` + `Far overlap`), the band fades as a whole by its own weighted mean distance, and
  the segment behind it composites under it unfaded (an opaque band skips to the fade end unless it is already
  fully in). (Fading each sample's DENSITY thinned a blob's front, the ray reached its dark core, and a half-faded
  tree read too dark. ONE fade for the whole ray let a ray grazing a band tree's blob beside its billboard pull its
  mean distance into the band and fade out the far trees behind it too: a sky-coloured outline along the
  billboard silhouettes - 2026-10-02.) and `Far blob shrink` (1/m)
  comes off the baked extinction first, so a blob shrinks toward its dense core. Steps of one cell × `Far step
  scale` (default 0.85; so they grow like the cells), the WHOLE ray shifted by a per-pixel, per-frame fraction of
  its first step (TAA averages it — the steps are measured from the camera and slide through the volume as it
  moves; jittering only the first sample left the rest on that sliding grid and the blobs shimmered), the height
  gap above the volume's top. Per sample: `albedo / π ×` (the sun × 0.6 (a leaf's mean cosine) × its
  transmittance through the crown (3 taps out to 14 m) × the terrain's sun visibility (`terrainSunVisibility`,
  once per ray) × the cloud shadow (`cloudSunTransmittanceBilinear`, per lit step; binding 11) + a COOL sky term, `Far ambient` × a blue tint × the sun's luminance, darker near the ground) +
  `u_ambientColor`. (First version: `2 × L.y` full sun + a warm ambient from the sun colour - the crowns read flat
  and yellow.) Ends at transmittance 0.01. Out: in-scatter + transmittance (RGBA16F) and the transmittance-weighted
  mean distance (R16F, m); the temporal variant instead writes log2 distances in `cloud_temporal`'s format
  (RGBA16F: x = the first tree, y = the mean, z = the march limit).
* **Temporal** (`Far temporal blend`, the history's weight; **default 0 — and 0 costs NOTHING**: the plain march
  variant writes the slot's result (R16F linear distance) directly with the original barriers, the temporal
  images are freed and the two temporal pipelines are compiled only at the first use; the user tested: the
  full-res march is stable without it, also at larger steps - it is for half res / checkerboard later). On: the
  `TREE_TEMPORAL_OUT` march variant writes a raw pair (log2 distances), then `cloud_temporal.cs.glsl`'s
  **`TREE_TEMPORAL` instance** (history distances per slot; it also writes the R16F distance the fog apply reads) —
  the CLOUDS' pass, shared rather than copied (2026-10-02, the user's choice): full res, no checkerboard, no wind,
  the weight from the push constant. It reprojects the previous slot's result at the mean distance, rejects it
  when that distance leaves the neighbourhood's range or the march limit changed (a silhouette), clamps it to
  the neighbourhood and blends. The history counts only when the previous frame marched (`frameNumber`).
  **`Far pixel skip`** (Off / **1 of 2 (checkerboard), the default** / 1 of 4) **needs no pass on its own**: one
  thread per BLOCK - a horizontal pair, the marched pixel alternating per frame and row, or a 2x2 block, the marched
  pixel cycling through it over 4 frames - marches ONE pixel: the first of its schedule whose surface lies FARTHER
  than `Far start` (a nearer pixel's result is exactly "no trees" - the march would stop before the volume - so it
  is written directly; a block with no far pixel marches nothing). At a near crown's alpha-tested fringe (leaf and
  gap alternating per pixel and per frame) the background side is then marched this frame and donates to the
  block's other far pixels; marching the scheduled pixel blindly left the gaps only leaf-side copies - a bare-sky
  speckle. The march writes into the persistent **latest-march images**
  (`m_latest` / `m_latestDepth`, bindings 8 / 9: render size, not per slot - a skipped pixel was last marched up to 3
  frames ago; only while the plain path skips), and COPIES the block's other pixels from them - 1 to 3 frames late
  under camera motion (no reprojection). A RESTART (the first frame, a resize, a mode change, a frame without the
  plain skip: `m_lastPlainMarchFrame` / `m_lastSkipMode`) clears them to "no trees, distance 0", which fails every
  copy test. The copy must fit THIS frame's surface: the TAA jitter moves silhouettes by up to a pixel per
  frame, and a copy marched against the other side of an edge brought the bright outline back. With trees (T <
  0.999) the surface must lie behind their mean distance (×0.7); without, not farther than last frame's march
  reached (×1.5) - so the plain variant stores, for a tree-less pixel, its march LIMIT as the distance (the scene
  distance, sky = 65000 m; the composites read the distance only where trees are). **A failed test never
  marches** (a second march stalled the whole warp - with it the checkerboard cost MORE than off): trees in front of
  a nearer surface → "no trees" (exact); a surface farther than that march reached → MORE DONORS through the same
  test (`copyFits`): the block's marched pixel (this frame), then the 4 direct neighbours' latest marches, and
  only if none fits the marched pixel anyway. (Taking the marched pixel blindly gave a bare-sky halo along every
  near silhouette whenever it sat on the near side - worst at 1 of 4.) The wide tolerances keep the TAA jitter on grazing terrain (more than a
  few % per sub-pixel) from counting as a border. ONE `marchAt` call site (two inlined copies raise every warp's
  register count). The barrier before the march also orders last frame's latest-image writes (and a restart's
  clear) before this frame's reads. (Tried first: copying this frame's NEIGHBOUR - a bright outline at every
  silhouette against the volume; then marching the pair at depth edges - correct, but a warp with one edge pixel
  paid the double march, and the checkerboard saved little; then the previous SLOT as the copy source - fine for 1
  of 2, but it holds one frame only.) Under the temporal path the pass reconstructs instead, and 1 of 4 runs as 1
  of 2 (the pass knows the checkerboard only). **The scale and the pixel skip are BAKED** (`TREE_MARCH_SCALE` /
  `TREE_MARCH_SKIP` in the march, `TREE_TEMPORAL_SCALE` / `TREE_TEMPORAL_CHECKER` in the temporal pass), so the
  dead paths and their registers go: `prepare()` rebuilds a variant when its define set changes (one GPU drain per
  toggle; a failed rebuild keeps the old one), and `record()` dispatches by the BAKED values
  (`m_plainBakedSkip`, `m_temporalBakedScale` / `Checker`), never by the live settings. The march's push block is
  full (128 B; two pad words where the runtime flags were).
  **`Far half res`** (off) - or the blend - runs this path (`FarTreeParams::temporalPath`): the march at the temporal images' SCALE (2 = one ray per 2x2 block, its centre,
  to the block's FARTHEST surface) and/or this frame's checker parity only (the dispatch covers half the columns,
  as `cloud_march`). The temporal pass takes the scale, the checkerboard, the far end (the unmarched pixels' limit)
  and whether to write the R16F distance as push constants - the clouds keep their baked / UBO values through the
  same `TT_*` macros. The march's stored LIMIT is `min(scene distance, Far end)`, the rule the pass applies to an
  unmarched pixel. At half res the history colour is a half-res image per slot, and **`tree_volume_upsample.cs`**
  (`cloud_upsample`'s depth-aware 2x2 rules, copied) writes the slot's full-res pair, so the three composites
  read the same images in every mode. A far crown is ~1.5 px at 10 km: half res softens it.
* **Apply — FOG OFF:** without clouds, the "Far trees apply" scene stage: `colour + scene × T`. **With clouds,
  the cloud apply composites the trees itself** (`cloud_apply.fs`, bindings 4 / 5) - the two layers front to back
  by distance, `vol_apply`'s layering without the fog - and the tree stage is skipped. (Composited separately, the
  clouds came AFTER the trees: the volume writes no depth, so the clouds' march limit was the terrain behind the
  trees, and clouds between the two drew over them - 2026-10-02.)
  **FOG ON: the fog apply composites the trees itself** (`vol_apply.fs`, bindings 9 / 10; `u_foliage_farMarched` =
  it marched this frame — the fog apply is a cached secondary, so the flag rides the UBO), at the march's
  transmittance-weighted MEAN DISTANCE (`exp2` of the distances' y): over the finished trees, the fog fogged them at
  the scene depth — the terrain BEHIND them, km farther — and they read twice as hazy as the billboards beside
  them. Trees and clouds are two layers composed front to back by distance (the cloud formula per layer).
* **Hand-over:** the tree cull gets `u_present_treeVolumeStart` = `Far start` + `Far overlap`: a tree whose CENTRE lies
  past it drops its mesh AND its billboard from MAIN (the billboard stays the shadow caster). Over the overlap band
  both draw while the volume fades in — an overlap, not a seam; the billboards switch off at its end. (A dithered
  billboard fade-out over the band was tried and removed 2026-10-02 at the user's request.) A type WITHOUT a billboard
  (a rock, R5) drops its mesh at the same distance (`treeCullMainPiece`); its mesh stays the shadow caster.
* Not yet: per-tree species colour beyond "last writer", half res, the analytic tail (P7), placement
  beyond the grove, a dithered billboard fade-out at the overlap's end.
