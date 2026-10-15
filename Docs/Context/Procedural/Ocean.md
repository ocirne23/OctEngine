# OceanGenerator

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## `OceanGenerator`

"Ocean*" tweaks. The CPU side of the FFT/Tessendorf water.

### World scale

**"Ocean/World scale" is the ocean's `metersPerPixel`**: every metre-valued ocean tweak is a MODEL
metre, and the sea is drawn at model × scale (1 = the model sea and **the default**; tuned by eye rather than tied
to the terrain's "Meters per pixel"). Applied in ONE place, `pushOceanParams` (with `oceanWorldScaled` in
Settings.Ocean), which fills the scaled `m_params` the renderer gets — **and the CPU buoyancy mirror reads `m_params`,
never the tweak members**, so the two cannot disagree about the scale.

| Quantity | Scaling | Why |
|---|---|---|
| fetch, depth, cascade patch sizes | × s | Froude similarity: with gravity untouched this is the ONE scaling of the JONSWAP/TMA inputs under which wavelengths AND heights both come out × s — the scaled sea is a shrunk copy, not the full-size spectrum aliased into small patches. |
| wind speed | × √s | The velocity half of the same similarity (`U²/(F g)` and the wave-age ratio stay invariant). |
| every other metre (shore depths, cull slack, RT ranges, horizon offset, steer range, bubble depth / blur, foam texel, **ring cell**) and the foam drift speed | × s | Same world, fewer metres. The ring cell shrinking keeps the clipmap's detail per wavelength and its reach per model kilometre. |
| absorption, SSS strength (per metre) | ÷ s | The same water column in fewer metres, so deep water stays deep-coloured. |
| dimensionless ratios (amplitude, choppiness, the approach-band fraction, swash amplitude, foam thresholds) | — | The break acceleration is a fraction of g, invariant under Froude scaling. |
| sea level | — | The world datum, owned by the terrain. |
| **outside the ocean tweaks:** the "Ocean/Spray *" metres and m/s, the spray `.pfx` size / gravity / turbulence and "Fog/Caustic shore fade (m)" | × s (spray rate per m² ÷ s², "Fog/Caustic depth fade" per m ÷ s) | The renderer reads `OceanParams::worldScale` (`Renderer::getOceanWorldScale`): the sea keeps its model PERIODS, so a speed or an acceleration scales like a length. Applied in the UBO value list (`Renderer::registerUboValues`, RendererUbo.cpp) and, for the emitter, in `ParticleSystem::update`. The underwater fog boundary's offset is NOT a scaled tweak: "Fog/Underwater wave offset" (a ratio) lowers it by × the deepest LIVE wave trough (`getWaveTrough`, world metres already): a higher sea needs a lower boundary, or the murk peeks through the troughs the fog's coarse froxels miss. |

**The periods stay the model sea's.** Froude scaling alone shortens them by √s, and a miniature sea at
real-sea speed reads as racing. So `OceanParams::timeScale` = √s slows the spectrum's clock
(`ocean_spectrum.cs.glsl`, the `e^{iωt}` evolution only) back to the model periods: the sea is the model
sea in slow motion, shrunk. The breaking-crest acceleration stays in spectrum time on purpose, so the
foam criterion (a fraction of g) keeps the model look at every scale.

### The geometry clipmap

Concentric square rings ("Ring cell (m)", "Ring resolution", "Rings"), **each with a FIXED world-space cell size that
doubles per ring.**

> Because a ring's cell size is constant, every world position inside it samples the displacement maps
> at a **FIXED mip regardless of camera distance — wave shapes do not morph with camera motion.** A radially graded
> grid, whose per-vertex mip follows distance, does.

**Ring transitions use a CDLOD-style vertex morph baked per vertex** (texcoord = ring cell size + morph
weight): over each ring's outer band, odd vertices collapse onto the next ring's coarser lattice and
the sampled mip blends +1, **so the boundary matches the next ring exactly — seamless by
construction.**

A FLAT "Horizon band" ring extends to the far plane so the sea always meets the horizon.

### Sectors

The clipmap splits terrain-node style: ring 0 whole, each outer ring as 8 rectangular blocks around
its hole, the horizon band as its 4 sides. **Each is ONE `RenderMesh` + node (no `ObjectContainer`;
every sector shares `m_material`, `createMeshMaterial(Ocean, rayTraced = false)`, no LOD chain — the
clipmap is its own LOD) with its own SpatialIndex entry** (`SpatialLayer_Terrain`, userData =
`&sector.node | SpatialTerrainTag_Ocean`, `spawnVisible = false`), so the CPU visibility gate and the
GPU per-instance frustum cull drop off-screen water **instead of vertex-shading the whole multi-km
disc every frame.**

* **A grid never moves in memory while a list may name it.** The entries register only after
  `rebuildGrid` has built the whole vector (no more reallocation). A replaced or released grid is
  RETIRED (`retireGrid`: entries, nodes, meshes released at once; the vector kept in `m_retiredGrids`
  until `visibleCollectGeneration` moves past its value, freed at the top of `update`).
* All sectors share ONE transform, **snapped to a lattice multiple (8 cells) so vertices re-land on the same
  world positions** as the camera moves.
* **Both culls pad their bounds by `displacementExtent()` every frame.** The mesh they were built from
  is the UNDISPLACED lattice; the vertex shader then moves each vertex by the wave height and by the
  CHOPPY horizontal displacement, which scales with "Choppiness". A sector's bounding sphere absorbs
  some of that incidentally — its XZ half-diagonal exceeds its half-width — and **that spare slack is
  what a high choppiness runs out of**, dropping sectors with their crests still on screen. It shows at
  the SCREEN EDGES, where the frustum planes cut closest to a sector's own sphere.
  * `estimateWaveExtents` (the sparse readback re-scan, every 16th frame, F16C-decoded) also returns the
    crest and the largest RAW horizontal displacement. **Raw on purpose**: the maps store Dx/Dz before
    the choppiness lambda, so the live tweak scales the padding with no re-scan.
  * The CPU side re-registers `baseRadius + extent`; the GPU per-instance cull gets the same
    number through `u_ocean_displacementExtent` and adds it for `PIPELINE_IDX_OCEAN` instances
    (`instanced_indirect.cs.glsl`) — **frustum test only**, so LOD selection still sees real bounds.
* **Sector borders duplicate identical vertices, so splitting cannot open seams.**
* **Every triangle is emitted in BOTH windings** (`pushTri`), so the back-face-culled Ocean pipeline
  draws the surface from either side and the scene depth holds the nearest face from below too. (A
  cull-none variant shades every wave crossing along an underwater ray — stacked "water planes".)
  Only the index count doubles. **So `gl_FrontFacing` is always true on the water**; the ocean shader takes the side
  from the triangle's plane instead.
* The Spatial Main gate skips off-screen sectors (ocean is `PASS_MAIN` only), and "Ocean/Shore/Dry sector cull"
  skips sectors fully buried AND inside the streamed-mesh radius. **The dry test is the node's pass
  mask**, set per sector every `update` on main (`PASS_MAIN`, or 0 = the push skips it), so whoever
  pushes the node needs no ocean logic.
* **`update` never walks the grid for the push, and mostly not at all.** The re-centering of the
  sector entries (transform + `updateEntry`, padded by `displacementExtent()`) runs only when the
  snapped position, the sea level or the pad changed, and stays on main (an `updateEntry` on a worker could race the
  pool growth by a later registration in the frame). `update` only prepares the render job's
  input (`m_cameraPos`, `m_renderReady`); **`render(culled)`**, called by `TerrainStreamer::render`,
  kicks the job. The main-visible sectors come from the Spatial hand-over, collect slot 1: the
  TERRAIN's render job walks it once and pushes the sector nodes with the terrain nodes (from that worker;
  the sectors, masks and transforms hold until the next `update`, after the pre-present joins).
  Culling `Off` makes the ocean's own job push every sector instead. **A rebuilt grid draws from the
  next real stamp on** — one frame without water after a rebuild (rare: the ring tweaks, a far-plane
  change, re-enabling; frozen culling keeps it hidden until unfrozen), accepted over a second push
  path; the walk skips the old grid's destroyed nodes meanwhile.
* **The rest is a job** (`"oceanReadback"`, High, `m_renderCounter`; the culling-Off sector push has its
  own `"oceanRender"` scope inside; it reads `m_cameraPos` and captures only whether it pushes the sectors): the
  sector pushes only with culling Off, the displacement readback copy (`m_dispTile`;
  buoyancy samples it BEFORE the kick, and the readback slot is stable until present), the sparse
  wave-extent re-scan and the camera water-surface sample. `joinRender()` — main.cpp right before
  present; also `update`, `rebuildGrid` and the dtor — applies the three renderer stores (wave
  trough, displacement extent, camera water surface).
* **The horizon band is exempt from BOTH under-terrain culls** — a negated cell size flags its
  vertices, because its huge triangles break the vertex cull's footprint assumption.

### What runs where

Everything else is on the GPU: `OceanSimulationPipeline` (in RendererVK) simulates the spectrum and
IFFT into displacement and gradient maps **each frame, so every parameter is live**; the Ocean pipeline
variant displaces this mesh and shades it (RT refraction, Beer-Lambert). Params push through
`Renderer::setOceanParams`; **the mesh rebuilds only when ring params change.**

**The ocean bakes NOTHING itself** — the shore's surface weight, surf, swash and the land cull all read
the streamer's terrain-data map, and the GPU passes read the SAME bake the CPU copy comes from, **so
the drawn water and the simulated water agree by construction.**

**The shore is ONE depth weight on the raw cascade sum** (`oceanSurfaceWeight`, ocean_wave.inc.glsl):
1 in open water, easing to "Swash amplitude" (× sea-connection × land-height fades) across an approach
band sized by "Shoal depth scale". Every cascade is scaled alike, so the wave's spectral detail is
preserved into the beach; there is no per-cascade shoaling, no breaking limit and no waterline floor
— the surface is the wave, and the depth buffer cuts it against the sand. The swash weight
(the same base, faded in across the band) gates only the tongue's backflow.

### `sampleWaterHeight` — the buoyancy field

CPU-evaluated from the GPU displacement readback, and **a full CPU MIRROR of the clipmap vertex
shader**: the raw cascade sum times the shore's surface weight, plus the swash backflow. ~2 frames
latent — invisible for physics. The App wires it into `PhysicsWorld::setWaterSurface`, taking the higher of it and the
inland water (`TerrainStreamer::sampleInlandWaterHeight`, [Rivers.md](Rivers.md)).

> **The shader is what you see, the mirror is what floats on it. Changing either without the other is a
> silent bug** — bodies sink through drawn waves, or float on dry sand.

It returns `-FLT_MAX` where there is no water: ocean disabled, readback not primed, or land beyond the
swash run-up band. **Inside that band it returns the live tongue surface, which SINKS BELOW the terrain
as the wave recedes — so bodies beach themselves**, and callers want a plain surface-vs-point test
rather than a separate dry check.

`hasWater()` (or `TerrainStreamer::hasInlandWater()`) is the App's global gate for the buoyancy pass, **so no water
costs physics nothing** — the pass otherwise sweeps the whole broadphase every step.

### Wind

The sea's wind is THE wind (RendererVK `WindParams`, "Sky/Wind"): its direction, and its speed x "Ocean/Waves/Wind
speed scale" (2.5) = the MODEL U10 (`windSpeed()`; the surf band, foam drift and calm cap all read it). The dominant
spectrum term travels AGAINST `windDirection`, so the sim takes the heading the wind blows FROM (`baseWindAngle()` =
"Sky/Wind" direction + π) and the swell heading is `swellTravelAngle()` = the wind's own direction. `steeredWindAngle`
slews the SIM wind toward the baked shore flow so waves roll inland ("Ocean/Waves/Flow steers wind"). The app hands
`swellTravelAngle()` to the terrain's flow bake every frame (`setFlowWindAngle`), so a wind change reaches the next bake.

> Per-pixel domain rotation is **disabled** in `ocean_wave.inc.glsl` — it creases. Read the comment
> before reviving it.

### Near-field detail: three shading knobs

The FFT band ends at the finest cascade's Nyquist, and the mesh band-limits the displacement well
above that, so what is left near the camera is a normal map on a smooth surface — the "plastic" look.

* **"Ocean/Shading/Micro roughness"** — the slope variance of everything BELOW that Nyquist (the
  capillary band), added straight into the GGX `alpha²` as `2σ²`. **The LEAN term cannot cover it**:
  LEAN returns the variance the MIP CHAIN removed, which is exactly 0 at mip 0, so without it the near field falls
  back on the capped screen-derivative AA and then onto the 0.02 alpha clamp — a sky mirror. Deliberately **not**
  scaled by "Glint filtering": that knob trades away filtered variance, and this band was never in the spectrum to
  filter. 0 restores the mirror.
* **"Ocean/Shading/Crest slope limit"** — `k` in the shading slope's soft limit `s /= 1 + k·|s|`
  (`oceanSampleSurface`). It keeps the near-fold division from exploding into dark creases, but it compresses exactly
  the steep crest faces, so **lower = sharper crests**, 0 = no limit. Only the SHADING slope: `oceanSampleDisplacement`
  has no such limit, so the CPU buoyancy mirror is untouched by this knob.
* **"Ocean/Shading/Detail strength / scale / fade (m) / rotation (rad)"** — `oceanDetailSlope`: the FINEST
  cascade's own gradient field re-sampled at `scale ×` its patch size and added to the shading slope.
  Wave statistics at a shorter wavelength than the FFT band holds, for **one fetch — no extra memory,
  no extra FFT**.
  * The domain is **rotated**, because an unrotated copy is the same field scaled: its crest lines run
    parallel to the parent's at every point and read as a fractal repeat rather than as ripples. The
    slope is rotated back before `oceanFlowToWorld`.
  * It is **shading only** — never in `oceanSampleDisplacement` — so geometry and
    the CPU buoyancy mirror are untouched, and the shader/mirror rule is not broken.
  * Implicit LOD: a smaller patch makes the uv derivatives correspondingly larger, so the mip chain
    filters this band the way it filters the cascades. It still **fades out with distance**, because
    this band is absent from the LEAN moments — what the mips remove would simply vanish instead of
    becoming roughness. "Micro roughness" is what covers that band.
  * It is scaled by the shore surface weight, so it dies into the beach with every other wave term.

The first two are dimensionless (a variance and a slope ratio) and so is the detail strength, scale and
rotation — `pushOceanParams` world-scales only "Detail fade (m)".

### Foam and bubbles

* **"Ocean/Foam/Bubble depth (m) / Bubble brightness / Bubble blur (m)"** — the entrained-bubble cloud
  (`ocean_bubbles.inc.glsl`). Its coverage IS the foam field's amount (clamped; it also adds
  roughness, amount × 0.35). The cloud is a high-albedo layer "Bubble depth" UNDER the surface, so the water over it
  absorbs the red both ways and it reads **turquoise, not grey** (a mix toward a whitewater colour reads grey). The
  ocean reads its coverage where the refracted view ray reaches that depth (one parallax tap); the terrain film uses
  the same function with the depth capped at its own water, so the hand-over keeps one colour. "Bubble depth"
  is a metre (× s); the brightness is a scale (a separate turbidity scale would be redundant with it).
* **The surf band scales with the WIND** ("Ocean/Foam/Foam wind full (m/s)", `pushOceanParams`): its width
  "Ocean/Shore/Shore foam depth (m)" × smoothstep(0, full, MODEL U10) - off in a calm, full from "full" up. Its cap
  ("Shore foam max") scales only in a near-calm: × smoothstep(0.5, 2 m/s, model U10). The fold / breaking thresholds do
  not scale (shifting them with the wind was tried and dropped).
* **The WORLD-SPACE FOAM FIELD** (`ocean_foam.cs.glsl`, `ocean_foam_field.inc.glsl`,
  `OceanSimulationPipeline::advanceFoamField`). A turbulence term stashed in a cascade's moments is too coarse (metre
  texels over a km patch) and only shows by relaxing the live fold threshold, so aged foam stays on the crests; this
  field replaces it.
  * `OCEAN_FOAM_LEVELS` (3) camera-centred clipmap levels of 512², texel "Foam texel (m)" × 4^level (default 0.4 /
    1.6 / 6.4 model m: ~205 m / 820 m / 3.3 km). They are the ocean MAPS' last layers (`3 × CASCADES + level`, mipped
    by the blit), so any maps sampler reads them - no new binding. Ping/pong state: an R16F image, layer
    = level × 2 + slot.
  * ONE channel: the FOAM AMOUNT (`max(prev × "Foam decay", instant)`, no spread), injected by
    `oceanInstantFoam`. Above "Foam threshold" (its density, below) it draws white foam; the same amount is
    the bubble cloud and the roughness. So decaying foam fades into the turquoise glow. (A second turbulence channel
    with its own decay / spread, and a turbulence-relaxed fold threshold, were redundant and are gone.)
  * The bubble cloud reads the amount BLURRED ("Bubble blur (m)", default 2, × s): a diffuse volume, so
    the separate breaking events merge into clouds instead of texel-sized spots. That read picks a coarse
    mip and reconstructs it with two mips' B-splines (8 taps); a magnified coarse mip read bilinearly
    shows its texels as diamonds.
  * Indexed in DRIFTED REST coordinates, q = rest XZ − drift. The rest (undisplaced) lattice is where a
    parcel sits between orbits, so foam on it rides the waves and STAYS as a crest passes on. The drift
    ("Foam drift (% wind)", along the swell's travel, over the SIM delta) moves the whole frame, never
    resamples, so it never blurs.
  * The CPU state scrolls each level by whole texels (the UBO's `zw` = the compute's read offset). A texel
    change or a frame with the ocean off resets the field. **A texel that SCROLLS IN starts from the next
    coarser level's last-frame state** (bilinear), not empty: at a long "Foam decay" an empty strip takes
    seconds to fill and shows each level's edge as a line trailing the camera. The outermost level
    starts empty (it fades out anyway).
  * The ocean draws `max(crest foam, surface foam × "Surface foam")`; the film does the same inside its
    shore gate. The film samples at its own position (near the shore it is within a displacement of the
    rest lattice).
  * **Coverage is DATA-DRIVEN** (`oceanStuckFoamDensity` / `oceanStuckFoamCoverage`): a threshold
    ("Foam threshold", "Foam edge") on the stuck foam's surface density, `amount / J`. The live fold
    Jacobian J is the displaced area per rest area, so the foam packs where the water converges and
    tears where it stretches. Every cascade is in J, so the edges get detail down to centimetres with no
    pattern. The ocean widens the edge by `fwidth(density)` (AA); the film has no derivatives there and
    relies on its mip-filtered Jacobian taps. The J it reads is `foamJacobian` (`oceanSampleSurface`): the
    finest cascade scaled by "Foam fine waves" (0.5) - at full weight its sub-second waves reshape the foam every frame.
    The film's crest fold and shoreline lace read the same damped sums. **No procedural foam pattern** (a ridged-noise
    lace was rejected: the foam must be data-driven). The field is read through a cubic B-spline where magnified
    (`oceanFoamBicubic`).
  * **The foam's lighting normal** (ocean): its own slope, `macro × (1 − "Foam flatten") + detail ×
    "Foam detail"`. `oceanSampleSurface` returns the sub-band detail slope separately for this. The flatten
    eases only the large waves, whose bent crest facets otherwise go dark, and the detail keeps the foam's
    relief below the geometry (a bump from a lace gradient was rejected).
    **The film's whitewater is the ocean's exact formula** on the same slope rule (`foamSlope` in
    `terrain_film.fs.glsl`, at the OCEAN's normal strength, not the film's normal scale or waviness) with
    `sunSurfaceRadiance()` as the sun, so the two foams meet in one tone.
  * **The surf band's cap** ("Ocean/Shore/Shore foam max") is a soft knee NORMALISED so full lace reaches
    the cap (ocean + film); the bare knee `fm (1 − e^(−x/fm))` stops at ≈ 0.63 even at cap 1 and reads as a grey veil
    beside the fully covering stuck foam. The band's TARGET (nearShore × bore, the waterline term)
    fades the lace in through its fold THRESHOLD only, `b = mix(Fold bias − 0.8, 1.45, target) + Shore
    foam bias`, **never as a coverage multiplier** (multiplied, the band's fade is a half-transparent veil). At
    target → 0 only folds 0.8 past "Fold bias" foam, a subset of the crest foam, so the hand-over has no
    seam. The film keeps `shore` (the land gate) as a multiplier: inland its raw Jacobian is still the full
    open-sea fold field.
  * Not shore-weighted: injection is open-ocean math, so the stuck foam and the milk can show in the calm
    shallows where the waves were damped.
* **`OceanParams::cameraUnderwater`** (`u_ocean_cameraUnderwater`) — set each frame in `pushOceanParams` from
  `sampleWaterHeight` at the camera. `ocean.fs.glsl` takes its UNDERSIDE path only while it is set: a back
  face seen from above is a FOLD (high "Choppiness" overturns the sheet), and it must shade as the top side, not as the
  underside with "foam from below".

"Ocean/RT" tweaks budget per-pixel scene rays.
