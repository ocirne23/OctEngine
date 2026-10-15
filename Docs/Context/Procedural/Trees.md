# Trees

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Trees

"Trees" tweaks. **In development** (tiers T0..T3, phases G1..G8).
Status: G1 (species asset + piece generator) and a CPU composite PREVIEW.

### The model: a piece library, composited per tree

A tree is NEVER generated as a whole. A species generates once into a `TreeLibrary`
(`generateTreeLibrary`, TreeGenerator.cpp): **trunks** with attach **slots**, and **branch modules** (a main
branch with its sub-branch levels and leaves). `compositeTree(species, library, seed)` then picks a trunk and
fills its slots with modules: module +Y onto the slot direction, module +Z (its "up side") toward world up,
a roll jitter, a scale to the slot length. **The composite's only randomness is `treeHash` (PCG)** — the GPU
version (phase G4) must reproduce it bit for bit; keep the two in step.

* Pieces carry a per-vertex **bone** index (piece-local; bone 0 = the piece root, each level-1 sub-branch of a
  module gets its own) and `TreeBone` pivots/axes — the data the wind and the GPU bone palette will use.
* Modules are generated in a frame where world-up is `(0, cos φ, sin φ)` for the species' mean slot angle φ,
  so up-attraction and leaf facing are world-relative after the composite.
* Slot lengths follow the crown envelope (`Crown Shape`) — the envelope sets the silhouette, modules fill it.

### `.tree` — species asset (`Assets/Trees/*.tree`, `loadTreeSpecies`)

```
TreeSpecies <name>
	Seed n · Scale min max · Kind Tree|Bush (default Tree; a Bush is scattered around the grove's trees, never one)
	Climate <name> (a Bush grows around the trees of the same climate only, case-insensitive; a tree whose
	        climate no bush shares takes every bush)
	Weight n (a Bush, 1..8, default 1: how often its climate picks it - it is listed n times in each tree's bush list,
	        which the CPU and the far volume's GPU mirror both pick from uniformly; a tree's list holds at most 8 entries
	        in the far volume, TREE_RECORD_MAX_BUSHES)
	Undergrowth { Bushes (per tree, the fraction by chance) · Radius (m: out to this from the tree) } (a Tree; absent =
	        "Trees/Bushes per tree" and 0.75 x "Trees/Spacing (m)"; the far volume reads it per record type)
	Trunk   Count · Height min max · Radius · Taper (tip/base) · Flare · Sink (m below the base, default 0.5) · <shape>
	        Lobes n (ridges/buttresses around the trunk; 0 = round) · LobeDepth base top (radius fractions)
	        LobeHeight (height fraction over which the ridges fade to the top depth) · Twist (deg over the height)
	Crown   Shape Ellipsoid|Cone|Umbrella|Column · Start (fraction of height) · Radius (m reach)
	        Slots · Fill (probability) · Angle bottom top (deg from up) · AngleVar · Leader true|false
	        BranchRadius (max module base radius / trunk radius at the slot; caps the module scale)
	        Tiers count spread (slots grouped into `count` whorls up the crown, each spread over `spread` of
	        its band; envelope and angle follow the whorl's height - layered crowns; default 1 0.35)
	Module  Count · Length (m, 0 = crown radius) · Radius (base / length) · <shape>
	        Level { Count · Start · Length · LengthTaper · LengthVar (each child a random 0..this shorter,
	        0..0.95, default 0) · Angle value var · Rise (0..1: the start direction blended toward world-up -
	        strands that arch over before they hang; default 0) · Radius · <shape> }  (up to 3)
	Leaves  Size (m) · Aspect (width/length) · PerBranch (per average last-level branch; other branches
	        scale it by their length) · Levels (deepest tiers that carry leaves; 1 = last level only)
	        Type Single|Cluster · Cross true|false · Style Leaves|Needles|Pinnate · ClusterLeaves (Needles: side
	        shoots per branch; Pinnate: pinna pairs per compound leaf) · Shoots (Needles / Pinnate: branches /
	        compound leaves fanned from the card base, 1..5, default 1) · ClusterLeafSize (Needles: the needle
	        length; Pinnate: the leaflet length; 0.02..0.6) · NormalBend (0..1, default
	        0.7: card normals bent away from the module root, both faces)   (Cluster only, below)
	        Align (0..1, default 0: cards stick out of the branch facing up; 1 = they lie ALONG the branch,
	        facing out of it at random turns - a weeping strand's curtain)
	Lod     ErrorScale (x the per-level error; > 1 = coarser levels nearer the camera)
	Bake    Variants (baked whole trees per species, default 4)
	Billboard Distance (m, 0 = none; default 50) · Resolution (px, power of two, 512) · NormalBend (0..1, 0 - the user's pick, every species)
	          FadeWidth (m, the mesh <-> billboard crossfade band centred on Distance; 10)
          AlbedoScale (x the WHOLE-TREE billboards' colour, linear, 0..8; default 1 - applied after the cache, at
          load: a change needs no re-bake; the module billboards / branch cards and the far volume keep theirs)
          TopCardHeight (the WHOLE-TREE billboard's horizontal card, a fraction of the tree's height: 0 = bottom,
          1 = top; default 0.5 - its bake is orthographic from above, so only the card mesh moves: no re-bake)
	Bark    Plates lines rows (fissure lines per family around / horizontal-break rows along, per tile)
	        Crack (fissure width, ridge fraction) · Breakup (0 continuous lines .. 1 short segments) · Relief · Lichen
	Color   Bark r g b · Leaf r g b
	Placement (WORLD placement, TreeWorld; trees only - absent or Density 0 = never placed in the world)
	        Density (per ha in the ideal climate) · Temperature min max (C; the IDEAL range, one value = exactly that)
	        Precipitation min max (mm/yr; the ideal range - 2200 and up is the encoding's top, so a max there is no max)
	        ClimateWidth (sigma of the falloff OUTSIDE the ideal box, in the normalized climate space, 0.06)
	        Cluster size coverage (m, 0..1; size 0 = no patches) · MaxSlope (rise/run, 0.6)
	        Altitude min max (m above the local water level, 1.5 ..)

<shape> (TreeBranchShape, any of): Curve value var (deg, smooth bend) · UpAttract (negative droops - weakly:
        -1 over the length bends a horizontal branch only ~45° down)
        Droop (deg turned toward straight DOWN over the length, a fixed rate that stops at vertical: 400 = the
        strand hangs after a quarter of its length; 0..720, default 0). A species with ANY Droop grows each
        module for its own pitch (spread over the slot angles ± AngleVar, not the mean), and the composite
        places the nearest-pitch module AT that pitch with no roll jitter: the module's down is then exactly
        the world's (with the mean pitch and ±25° roll the strands hung up to ~50° off plumb).
        Wobble (deg random walk per segment) · Elbows (average count) · ElbowAngle value var (deg)
        ElbowUpBias (probability a downward elbow turn is mirrored upward; default 0.5)
        ElbowMinElevation (deg vs horizontal an elbow may turn down to; default -15; a branch that already
        points lower before the elbow is not lifted) · ElbowRange start end (length fractions; default 0 1)
        Stubs (probability an elbow carries a snapped-off stub) · Rings · Sides
```

**Elbows** are sharp turns on interior ring nodes, each rounded by replacing the corner with three points of
a quadratic Bezier (+2 rings per elbow); everything along a branch is addressed by its arc-length fraction
`u`, not by ring index. A **stub** continues the direction the branch had BEFORE the elbow (the branch that
snapped off), short and thick, with a flat broken end cap.

Placeholders: `Oak` (ellipsoid, cluster cards), `Pine` (cone + leader from 12 % of the height, 60 slots at 85 %, two
sub-branch levels (7 + 4) drooping, 1.1 × 0.83 m NEEDLE-spray cards of 3 fanned branches, 2 per branch, on all
three tiers (the module roots too: near the top of the cone the short modules are mostly root) -
reworked 2026-10-03 from single diamond leaves on one level, which read as bare poles), `Acacia` (umbrella:
14 slots, sub-branch levels 10 + 5 up-attracted, flat 0.9 m PINNATE cards, 4 per branch - 1 compound leaf of 4
pinna pairs, leaflets 0.07 of the card (~6 cm; true-to-life ~2 cm leaflets were invisible) - on the last two tiers; reworked
2026-10-03 from 0.1 m single diamonds), `Willow` (weeping: an 8.5-10.5 m trunk carrying 20 thin limbs in TWO whorls - `Tiers 2 0.4`,
Ellipsoid envelope (upper reach ~84 % of the lower; Cone gave ~33 %), Angle 55 30, trunk Taper 0.35 + BranchRadius
0.7: the thin upper trunk caps the upper modules to ~80 % - the upper layer's size knob -, arching over - Droop 80 -, 16 arcing branches each - Rise 0.3, Droop 120, leaved
too -, 7 strands per branch that rise, arch over the dome and hang plumb - Length 2.6, Rise 0.7, Droop 280 - of random length
(`LengthVar 0.65`: curtains ending at layered heights), narrow cluster cards along them, `Leaves Align 0.8`;
~1000+ cards per module, ~5x Oak).
Climates: `Oak` Temperate, `Pine` Boreal, `Acacia` Savanna, `Willow` Temperate. Undergrowth: Oak and Pine 4 bushes out
to 8 m (the global default's count); Acacia 6 out to 12 m (its trees stand ~29 m apart). **The open floor is filled by
WIDTH, not count** (2026-10-13; the user: "dense" = less open space, not more leaves): a bush's cover grows with its
radius squared, so one wide, low ground-cover bush per climate - `Weight 2`, half the climate's bushes - fills what three
round ones would, for one piece (one slot in the set, one cull entry, one far splat):
* `Bramble` (Temperate), `DwarfBirch` (Boreal), `Brush` (Savanna): crown `Radius` 2.6, `Scale` 0.8-1.2 (~4-6 m across,
  ~0.5-1.3 m tall), 14-16 near-flat slots from the ground, `Billboard Distance` 30. LEAVES ON EVERY LEVEL (`Levels 3`,
  `PerBranch 6`, 12 leaves per cluster) and THIN stems: `Module Radius` 0.005, trunk `Radius` 0.02. **`Module Radius` is a
  fraction of the module LENGTH** - with `Length 0` that is the crown radius, so the first draft's 0.022 x 2.6 m made
  ~11 cm stems: tree branches, bare between sparse clusters (the user, 2026-10-13: "branchy, not leafy").

The round bushes, two per climate:
* Temperate: `Shrub` (broadleaf, round, sparse cluster cards) and `Thicket` (denser and wide: crown radius 1.5 m,
  flat slot angles).
* Boreal: `Juniper` (a low SPREADING conifer, blue-green needle-spray cards, uncrossed - crossed read too dense; the first conifer bush, on
  single diamond leaves, was removed - the needle texture made it work) and `Bilberry` (knee-high, many thin upright
  stems with small dense leaf clusters).
* Savanna: `ThornScrub` (low, wide, umbrella crown, zig-zag grey stems, pinnate pads) and `Bushwillow` (an upright
  vase of stems, dry olive-yellow broadleaf clusters, up to ~2.5 m).

A bush is a
tree species with a short trunk (`Height` ~1 m), slots from the ground (`Crown Start 0`), a large `BranchRadius`
(the thin trunk would otherwise cap the module scale) and a short `Billboard Distance`.

### Piece mesh LODs

Every piece has `TREE_PIECE_LODS` (4) mesh levels, all from ONE skeleton: generation first records a
`PiecePlan` (branch paths, stubs, leaf placements — every random decision, in the old RNG order), then
`meshPiece` meshes each level from it (`PIECE_LODS` table in TreeGenerator.cpp):

| Level | Rings / sides | Branch levels | Leaves (every Nth, scaled √N: same area) | Stubs | Error (× length × ErrorScale) |
|---|---|---|---|---|---|
| 0 | 100 % / 100 % | all | 1 | yes | 0 |
| 1 | 60 % / 60 % | all | 2 | yes | 0.001 |
| 2 | 40 % / 45 % | deepest dropped (never below level 1) | 4 | no | 0.0025 |
| 3 | 25 % / 34 % | two deepest dropped (never below level 1) | 8 | no | 0.005 |

**The GPU uses LEVEL 0 ONLY (2026-10-03): no `createMeshLodChain` for tree pieces** (`uploadLodChain` uploads level
0 and leaves the chain empty). The trees switch through their own tiers (mid tier, billboard, far volume); the mesh
LOD steps underneath popped visibly (fewer, larger leaf cards per level, no crossfade). The generator meshes level 0
only (`MESHED_LODS` = 1 in TreeGenerator.cpp; the table below stays for a return of mesh LODs); `lodError` / `Lod
ErrorScale` are unused, and RendererVK allocates no hysteresis slots for tree records.

### Far representations (`Trees/Far mode`)

The SHARED pieces — modules AND trunks, and the baked whole-tree variants — get a billboard each (a trunk's +Y is
world up, so its billboard's "top" card stands vertical: two crossed vertical trunk cards; cache names
`<species>_trunkbillboard…`, modules keep the unprefixed ones), crossfaded in per placed piece beyond its distance ×
`Trees/Far distance scale` (`Trees/Force far` shows them all). Two modes, `Billboards` and `None`; changing the mode
reloads. (The octahedral-impostor fallback mode was removed 2026-10-04.) The bakes live in `TreeImpostor.cpp` (the
file name is historical): a small software rasterizer (2×2 supersampled, back faces culled, leaves alpha-tested);
`treeBakeHash` names the cache files by the bake's INPUTS: the species file's text, the piece's set and index,
`BAKE_VERSION` and the settings. **Not by the geometry** (it was, until 2026-10-07): the floats differ in their last
bits between Debug and the optimized builds (`/fp:fast`), so a position on a millimetre rounding edge renamed the file
at every config switch - each build deleted the other's cache and re-baked it. **A generator change that moves geometry
must bump `BAKE_VERSION`** (TreeImpostor.cpp), or press `Trees/Regenerate textures`.

* **Billboards** (default): two crossed cards along the module axis from its LOD-0 box — a vertical card
  (normal +X) and a horizontal card (normal +Z = world-up after the composite), their views stacked as strips
  of one square texture (`bakeBillboards`, `billboardMesh` in TreeImpostor.cpp). `Trees/Billboard views`
  (reloads): **2** (default) — a strip per card (side, top), the back face showing the front view through the
  card; **4** — each FACE its own strip, top to bottom: side from +X, side from −X, top, bottom. A back view
  keeps the card's right / up and negates only the view direction: stored as seen THROUGH the card, it reads
  the right way round on the back face, and its tangent-space normals match the back face's TBN (same tangent /
  bitangent, the handedness flips) — no shader change. Mips stop at 4 px per strip. Cache names carry the view
  count (`<species>_billboard<i>v<views>_<hash>`), so both settings keep their files. Both card PLANES pass through the branch axis (module x = 0 / z = 0),
  not the box centre, so the root lands at the trunk. **Whole trees** (the baked variants, whose +Y is up, so
  both cards above stand vertical) get a THIRD, horizontal card for the top-down view
  (`billboardHorizontalView`): across the axis at mid height, seen from above, u (+Z) spanning at least the
  tree's height so the lit FS's crown radius matches the vertical cards'. The material carries
  `MATERIAL_FLAG_BILLBOARD_TOP_CARD`; the lit FS finds the card by its up-facing normal (RendererVK). The
  edge-on fade hands over between the vertical cards and it. **Strip layout** (`billboardLayout`): 3 cards
  share the texture (3 strips with 2 views, 6 with 4), each strip's height rounded DOWN to a multiple of the
  largest power of two p that leaves ≥ 4 px per strip at mip log2(p) — so every strip boundary lands on a texel
  boundary at every mip the material gets and no mip mixes two cards. 512 / 3: p = 32, 160 rows a strip
  (+25% over a 4-strip layout), 32 spare rows at the bottom. 2 cards: 256 rows, mips to 4 px as before. **THE CROWN FIELD** (`buildCrownField`, TreeImpostor.cpp,
  2026-10-04): the piece's LOD-0 leaves (its bark when it has none) splatted by area into a grid of cubic cells (32
  along the longest side), Gaussian-blurred (sigma 2.5 cells, ~8 % of the longest side) and normalized to a peak of
  1 - a soft hull of whatever shape the leaves make (round oak, cone pine, flat acacia pad). Per texel, at its leaf's
  3D point (the card point + the baked depth): the **hull normal** = the field's outward gradient (the box centre
  where the field is flat), and the **interior** = the optical depth out along that normal / the field's CORE depth
  (from its peak out to the side, the mean of 4 horizontal directions), clamped to 0..1 - linear, so the lit FS's
  `Foliage interior depth start / end` remap it, and per-tree normalized, so a narrow pine and a wide oak both reach
  ~1 at their core. **VOLUME normals**: each texel's normal is bent by `Billboard NormalBend` toward the hull normal
  (was: "out of the box centre" - on a tall tree that baked normals pointing up at the top and down at the bottom) and
  keeps its sign, in each card face's tangent space. **The normal image is the CROWN layout**: RGB = that normal,
  **A = the interior** (RendererVK: remapped, x `Trees/Foliage interior shadow`); the mips are the bark's
  `buildNormalMips` (RGB renormalized, A averaged). The material carries
  `MATERIAL_FLAG_BILLBOARD`, so the lit FS does not reject the sun shadow by the flat card normal — without
  both, a whole card went dark whenever the sun was behind it. (The normal map's ALPHA was the texel's depth off the
  card until 2026-10-04 - read by no shader since the shadow pass's pixel depth offset was removed on 2026-10-02; the
  bake still computes it per texel, for the leaf's 3D point in the crown field.) The billboard materials and the leaf cluster material also carry
  `MATERIAL_FLAG_LEAF`: the sun shines through them (RendererVK leaf transmission on `LitTree` / `LitFoliage`,
  `Trees/Foliage transmission*` tweaks). The lit FS also blends their normals toward a view-ray CROWN normal (an ellipsoid of the
  card's proportions; `Trees/Foliage crown normal`), so the two cards stop shading differently at their crossing axis. This
  needs the card's +u axis to run from the instance origin (`billboardViews`: right = piece +Y). Real geometry on
  `LitFoliage` (RendererVK: LitTree + the card paths; a billboard material must use it): **they cast
  their own alpha-tested shadows**, so the mesh nodes stop drawing entirely.
* **Crossfade** (billboards only): a band `FadeWidth` wide, centred on the billboard distance × `Far distance
  scale`. Inside it (centre distance ± the module radius) the module draws its mesh as `barkFade` / `leavesFade`
  — the same LOD-chain meshes on `LitTree` with species materials DERIVED with a distance fade-OUT
  (`Renderer::deriveMaterial`, shared textures) — plus the billboard, whose material fades IN over the same
  band. The tree FSs' dither (tree_fade.inc.glsl) (RendererVK `MATERIAL_FLAG_DISTANCE_FADE`) gives each pixel to exactly one of them;
  TAA blends the per-frame dither. Outside the band only one side draws, the mesh on its normal (early-depth)
  materials. `applyFadeBands` writes the bands into the material flags (at reload and when the distance scale
  changes — no reload). Not dithered in the shadow pass: inside the band both cast. `Force far` keeps the
  hard switch. (Turning the cards about the
  branch axis toward the camera was tried and removed — the user did not like the look.) Cache `<species>_billboard<i>_<hash>.png` (+ `_normal`), mips down to 8 px.
* **RT (GI, RT shadows, RTAO, reflections) sees only the WHOLE-TREE billboards** of TREE species: every other tree
  mesh is created without a BLAS (`createMesh(..., raytraced = false)`) - bark / leaves / trunk / branches, the card
  meshes, the module / trunk billboards (the library rows) - and BUSHES never get one. Without
  billboards (Far mode None) the tree species' bark + leaves are raytraced instead (they stand in for the
  tree in shadow + GI there).
* **Branch cards — the MID tier** (GPU path, baked variants; `Trees/Branch card distance`, default 0.4 × each
  species' billboard distance, 0 = off; respawns): every baked variant keeps its composite (`TreePiece::placements`)
  and its bark SPLIT in two (`trunkBark` - the trunk alone - and `branchBark` - the modules'; `bark` stays whole for
  the bakes and the CPU path). `buildBillboards` stacks the module billboards into ONE CARD ATLAS per species (module
  m in rows [m, m + 1) x size; each mip the stack of the modules' own coverage-preserved levels) and merges per
  variant ONE CARD MESH: every module placement's cards in the variant's space, v into the module's block. Between
  the full mesh and the whole-tree billboard the tree draws the TRUNK (shared by both - it never fades at the mid
  band) plus the CARD mesh: over the mid band (`FadeWidth` wide) the branch bark (`branchMidFadeMaterial`) and the
  leaves (`leafMidFadeMaterial`) fade out while the cards fade in (`cardInMaterial`); over the far band the trunk
  (`barkFadeMaterial`) and the cards (`cardOutMaterial`) fade out while the whole billboard fades in (derived
  materials, bands in `applyFadeBands`). The cards draw on LitFoliage with the whole-tree billboards' shading and
  "Foliage ..." tweaks, but with `MATERIAL_FLAG_NO_EDGE_FADE`: no edge-on fade (the user's call). Their crown frame
  is still per MODULE: `billboardMesh(..., axisInZ)` puts each card's axis (its module's +Y line, as a texture v) into
  texCoords.z = 3 + v, which rides the tangent's w magnitude to the lit FS. MAIN pass only: shadows, GI and RT keep
  the whole billboard. 4 fixed record slots per tree, no pool (RendererVK "BAKED TREE RECORDS"). The CPU path (GPU
  expansion off) has no mid tier.
* **None**: mesh LODs only.

### Texture files (`Assets/Local/Trees/Textures`)

Generated output under `Assets/Local`, so NOT in git: a fresh checkout generates them on the first load.
The species textures are generated ONCE and saved as PNG (`File:ImageIO`): `<species>_bark.png`,
`<species>_bark_normal.png`, `<species>_leaves.png` (cluster species only). Later loads read them back and
only rebuild the mip chains (bark box-filtered, leaves coverage-preserving). **A file that exists wins —
changing `Bark` / `Color` / cluster parameters does NOT regenerate it**: press `Trees/Regenerate textures`
(overwrites all) or delete the file. The files are also where authored replacements go: square power of
two, RGBA8; the leaf atlas keeps the 2×2 cell layout (stem at the TOP edge of each cell, v = 0); the normal
map is tangent space with x along u (around) and y along v (along the branch, DOWN the image).

**GPU formats** (`Trees/Compress textures`, default on, reloads): the PNGs stay the RGBA8 cache; at load the
finished mip chains (coverage-preserving alpha, AlbedoScale applied) are BC-compressed in memory (File
`TextureConvert::compressBlockRows`, stb_dxt, every level's block rows a `parallelFor` task, "Tree texture BC"):

| Texture | Format |
|---|---|
| bark albedo | BC1 sRGB (opaque) |
| bark normal | BC5 (XY; MATERIAL_FLAG_BC5_NORMAL rebuilds Z) |
| leaf cluster atlas | BC3 sRGB (alpha-tested) |
| billboard + card-atlas albedo | BC3 sRGB |
| billboard + card-atlas normal | BC3 linear - RGB the full normal WITH its sign, A the baked interior: no two-channel format holds both, and there is no BC7 encoder |

4x less VRAM (BC1: 8x). Off = the RGBA8 chains as before (the A/B for compression artefacts). The module / trunk
billboards are uploaded only with `Show piece library` (they draw only there; that tweak reloads too).

### Bark texture

PROCEDURAL per species until authored textures exist (`TreeBarkTexture.cpp`, 1024²): tiles in both directions,
matching the bark UVs (u once around a branch, v along it per base circumference — so the pattern scales
with each branch). **Furrows, not cells** (a Worley plate pattern read as scales): two families of fissure
lines along the branch, each meandering by its own tileable fbm, cross and merge into braided ridges. All
noise is tileable GRADIENT noise — smoothstep value noise has zero slope on every lattice line, which the
normal map showed as evenly spaced horizontal bands. Every
line fades out where its own per-column noise is high (`Breakup` → segments with gaps) and varies in width
along its length; a third, 3× denser family of FINE cracks (mostly gaps, shallow) splits the ridges; a
fourth, 8× denser family of STRIATIONS (thin, very shallow, short-to-medium segments) runs along the ridges; rare
thin horizontal breaks (20 % per ridge column and row). **Low contrast in the cracks** (albedo 75 % of
the bark colour, fine cracks 85 %; fissure floor at 35 % of the ridge height): dark or deep outlines read as
cartoon ink; rounded ridge crowns with V fissures, ridge-scale
height/tint from SMOOTH noise (a per-segment hash keyed by row/column made a brightness grid), fine grain, along-the-branch fibres and lichen on the ridges. sRGB albedo plus a
LINEAR RGB tangent-space normal map from the same height field (x along u, y along v — the tube's tangent /
bitangent), each with a CPU box-filtered mip chain (normals renormalized). Uploaded through
`Renderer::createTextureMaterial(..., &normalMips)`, freed with the species.

### Leaf types

* **`Single`** — one double-sided diamond per leaf, the species' solid leaf colour, `LitOpaque`.
* **`Cluster`** — alpha-tested CARDS (`LitTree`), each showing a twig with `ClusterLeaves` leaves. `Size` /
  `Aspect` are then the card's, `PerBranch` counts cards, and a card's stem starts at the branch centre (the
  texture's twig grows out of the wood). `Cross true` adds a second card at 90° about the stem axis.
  The texture is PROCEDURAL until authored textures exist (`TreeLeafTexture.cpp`): per species a 1024² 2×2
  atlas of cluster variants (stem at v = 0, each card picks a cell), sRGB colours from `Color Leaf` / `Bark`,
  and its OWN mip chain — every level's alpha is rescaled (binary search) so the fraction of texels passing
  `TREE_LEAF_ALPHA_CUTOFF` matches level 0, otherwise the crown thins with distance. Uploaded through
  `Renderer::createTextureMaterial`, freed with the species (`TreeSystem::clearAll`).
  **`Style Needles`** (conifers, `drawNeedleCluster`): each cell is a conifer SPRAY instead of a leafy twig - `Shoots`
  needle branches fanned from the stem (the middle one along v, the others over up to ±18° and a little shorter;
  one card then reads as a whole spray, so the tree needs fewer branches and cards), each a main shoot plus
  `ClusterLeaves` side shoots (alternating, short, 24-38° off it - wider read as a flat fan), all densely set with
  thin tapered needles on both sides (two-segment, curving toward the shoot tip; shorter and steeper toward it; the
  far side darker) and a forward tuft at the tip. `ClusterLeafSize` is the needle length. Every leaf / needle /
  shoot is shortened until it fits inside its cell. **`Style Pinnate`** (acacia, `drawPinnateCluster`): `Shoots`
  BIPINNATE compound leaves fanned from a short woody twig (±28°), each a thin greenish rachis with `ClusterLeaves`
  pairs of LONG pinnae (0.55 × the rachis, forward, shorter toward the tip; the first pair close to the
  twig - a long bare stem read as a stalk). **Each of the 4 cells has its own CHARACTER** (`PinnateVariant`, the
  four read as copies with jitter only): a lean (±12°), a rachis curve (±0.16 × its length) and length (0.78-1),
  ±1 pinna pair, a pinna angle (30-50° + 0-18°) and length (× 0.8-1.12), the leaflet size (× 0.85-1.15), half of
  them ALTERNATE pinnae (the right-hand ones staggered outward) and a tint (× 0.88-1.12), every pinna a dense COMB of
  narrow oblong leaflets (`ClusterLeafSize` long, 0.4 as wide, 72-84° off the pinna, side by side with no gap,
  shorter in the pinna's last quarter; one half a little darker) - fern-like fronds, after a photo of a real
  acacia branch. (The first version - short pinnae nearly at right angles with sparse wide leaflets - read as
  scattered specks.) Every pinna fits its cell too. (The cell clips the drawing - the Oak's tip leaf lost its
  tip, 2026-10-03; the leafy twig also ends low enough for its tip leaf.)

### Baked tree variants (what the grove places)

The runtime no longer composites pieces per tree: at load every species bakes `Bake Variants` (4) whole trees
(`bakeTreeVariant`: the composite of a seed, all its pieces' bark merged into one mesh and all their leaves
into another, PER LOD LEVEL, at scale 1; each level's error = the largest piece error × its placement scale).
A variant is just one more "piece" (`Species::variants` / `variantMeshes`): LOD chains, a billboard (cache
infix `tree`), the crossfade — all the piece machinery applies unchanged. The grove places one
variant per tree with a seeded variant, scale (`Scale`) and yaw: **one GPU expansion piece = 4 records per
TREE** (was ~16 pieces × 3). A whole-tree billboard is two crossed VERTICAL cards through the trunk axis (a
tree's +Z card stands vertical too) — thin from straight above. Near-range uniqueness is planned as a per-tree
vertex-shader warp (bend / twist / lopsided crown / noise, seeded by position) that also carries the wind.
The pieces still exist (the bake input, the piece library rows).

**The WIND payload** (RendererVK "Tree wind", `tree_wind.inc.glsl` for the bit layout): `meshPiece` writes every bark
and leaf vertex's `texCoords.z = 1 + payload / 2^22` (`windPayload`): the module weight along the module's root bone
(a sub-branch's vertices take it at their bone's pivot), the level-1 sub-branch weight along its bone, the bone's phase,
a leaf card's TIP flag (corners 2 / 3; a diamond's all but the stem) - a card's weights from its STEM, so all four
corners share one phase. Trunk pieces: the wind bit only (the trunk bend). `bakeTreeVariant` ORs each module
placement's phase (3 bits) into its vertices. No vertex grows: the magnitude of the tangent's w was free.

### GPU expansion (G4)

With `Trees/GPU expansion` (default on), `spawnPreview` builds
ONE RendererVK tree instance set: a piece TYPE per library piece (its bark, leaves, the derived fade-out
materials and the billboard, plus the band), then every placed piece (transform, far centre + radius, type).
The set is uploaded once to device-local memory, and the culls make the per-piece decision and build the records
themselves, and the TLAS writer their RT instances (see RendererVK "BAKED TREE RECORDS"). **The pieces live in
the TERRAIN CHUNKS:** `spawnPreview` sorts them by the terrain chunk under their base (`Globals::terrain.chunkSize()`;
a chunk-size change respawns), one set chunk per terrain chunk, and hooks the set into the terrain
(`TerrainStreamer::setVegetation`, above ([TerrainStreamer.md](TerrainStreamer.md))): the terrain's render walk lists the chunks it draws with their passes,
and its sink calls `renderTreeInstanceSet` with that list - so the culls only see the plants of the visible
chunks (all passes) and of the shadow / GI sphere's chunks (shadow + GI). The sink runs on the walk's worker: it
reads the band settings through atomics. `update` binds the set (`bindTreeInstanceSet`, main) and submits
EVERY chunk itself when the walk does not carry it (terrain off, or the respawn frame - the new hooks start
with the next walk). Unhooking (`destroyTreeSet`) joins the walk. Every baked variant also carries its
FAR-TREE VOLUME grid (`bakeTreeDensity`, `TREE_DENSITY_RES` = 32³ extinction over its billboard box, from the
LOD-0 LEAF triangles only; cluster leaves count half opaque. The bark was dropped 2026-10-02: a trunk's whole area
in one thin voxel column drew dashed vertical streaks through the far blobs) and the species leaf colour — the leaf texture's
alpha-weighted mean, DECODED FROM sRGB (`meanLeafAlbedo`; the albedo textures upload as sRGB, so the raw
`leafColor` taken as linear read light yellow-green) — handed to the set as
`TreeInstanceType::density` / `albedo` (RendererVK "Far-tree volume"). With billboards, the BILLBOARD is the
piece's only shadow / GI / RT representation at every distance: the mesh draws in the MAIN pass only (both
paths). Off, it keeps the CPU path below (one
`RenderNode` per piece representation, pushed per frame). The set is destroyed on respawn / reload / disable.

### World records (`TreeWorld`, "Trees/World" tweaks; W1)

Every tree of the terrain ring as a **4-byte record per terrain chunk** (`TreeRecord`: x, z 12 bits each, chunk-local
on a 4096 lattice; the species type 8 bits = its index in the NAME-SORTED `.tree` list). No height (the terrain's), no
variant / scale / yaw: `treeRecordSeed(seed, chunk, record)` hashes the chunk and the QUANTIZED position, mirrored
in `Assets/Shaders/Trees/tree_record.inc.glsl` (not read by a shader yet). A member of TreeSystem, updated before its
`Enabled` check: it runs with the preview off.

* **Ring:** the terrain's (`ringRadius()`, `chunkSize()`, `generatedBounds()`), one chunk of hysteresis on eviction.
  On a camera chunk change the missing chunks are requested and the queue re-sorted nearest first; Low pump jobs
  (`Gen jobs`; 0, the default = one per job worker - `pumpCap`; they yield at every preemption point, so they only fill
  idle workers. It was 2: one or two busy workers while a fast camera's queue piled up) take the front, with lazy
  staleness as the terrain pumps.
* **THE BOOKKEEPING JOB** (`"TreeWorld update"`, Normal, `m_updateCounter`): merging the pumps' results, the ring
  rescan (a walk of every chunk + the (2R+1)² ring) and the next upload pick run OFF main. main.cpp kicks it
  (`TreeSystem::kickWorldUpdate`) right after `clutter.update` - the LAST reader of the records this frame
  (`cpuRecords`) - and the next `TreeWorld::update` joins it first, so nothing reads the chunks while it runs. It
  queues `m_pendingRemoves` (handles of the chunks that left the ring) and `m_pendingUploads` (picked chunk keys).
* **THE POOL JOB** (`"TreeWorld pool"`, Normal, `m_poolCounter`): the next `update` kicks it - the record POOL's removes
  and adds (`applyGpu`), then `updateTreeRecords`. The pool takes one thread at a time between beginFrame and
  `present`, which reads it: main.cpp joins it before `present` (`TreeSystem::joinWorldPool`), TreeSystem before
  `setTreeRecordTypes`, and `kickUpdate` before the next bookkeeping job. It writes `Chunk::gpu` ONLY, so TreeSystem and
  the clutter read the records beside it; the bookkeeping job drops the uploaded chunks' ground (and their CPU records
  outside the keep radius) first thing (`finishUploads`). One frame between a pick and its upload.
* **Main keeps** only the rare parts: the config check, a restart (it resets the pool), `loadSpecies` (IO), and the
  job inputs (camera chunk, ring radius, keep radius, upload budget) `update` snapshots.
* **Pure function** (`placeChunk`): one `sampleGrid` at Full detail, ~8 m step + halo, then one candidate per cell of
  a `Candidate cell (m)` (5) lattice, jittered over the cell. Per species (`Placement`) its CLIMATE FIT: 1 inside its
  IDEAL box (`Temperature` / `Precipitation` min max, normalized by `temperatureTo01` / `precipTo01`), outside a Gaussian of the distance
  to the box (per axis, sigma `ClimateWidth`), faded to 0 between `Climate fade start` and `end` (0.10 / 0.25 of the
  peak), 0 outside its slope / altitude band. (Until 2026-10-04 an attractor POINT: the density peaked at one climate
  and every climate around it thinned - a precipitation band where a minimum was meant.)
  Each species' density at the candidate: Density x fit x its cluster fbm x (fit / the BEST fit there) ^
  `Climate sharpness` (4). The candidate exists with probability (the summed densities) x the cell's hectares x
  `Density scale` (so at most one tree per cell: 400 / ha at 5 m); its species is picked by share. Species that share a
  climate ADD: a rare species in a dense one's box (Willow in Oak's) takes nothing away from it.
  (History: until 2026-10-04 the pick was by Density x fit with the Gaussian cut at 2 % - a dense species' tail
  outnumbered a sparse one's core, pines among the acacias, and lone trees far from any forest; until 2026-10-05 by
  fit ^ sharpness normalized by the SUM - an equal-fit species halved the other's density.) Bushes are not stored.
  **The existence roll goes FIRST** (exact, the same result): a tree candidate whose roll is above the species' summed
  `Density` x the cell's hectares, or a rock cell whose roll is above its type's bound (every rule's `Density` x its
  largest ground / forest / river / flow multipliers - every fit term is at most 1), skips its field sample and fits.
  The river grid (1 m) covers only what is read: the chunk plus the neighbours' rock reach + 2 m, not the terrain halo.
* **A new generation** (everything dropped, every chunk regenerated) on: Enabled / Seed / cell / density tweaks,
  `Reload species`, a new sampler, a chunk size or bounds change. The generation counter is monotonic, so a late pump
  result of an old config never merges.
* **GPU (W2):** each merged chunk joins an upload queue; `Upload KB per frame` (1024) of records per frame go to the
  renderer's pool (`Renderer::addTreeRecordChunk`, RendererVK "WORLD TREE RECORDS"; `GPU pool (MB)`, 128, fixed - a
  full pool refuses chunks and warns once), each with its GROUND: `placeChunk` also writes the chunk's 17² heights
  (16 m at 256 m chunks, bilinear in its Full 8 m grid, 16-bit above the minimum - TreeRecordPool's encoding), the
  ground the far volume stands the records' trees on (the camera-centred height map's far cascade, ~100 m texels,
  buried them on every peak). Every chunk uploads, trees or not. An evicted chunk's GPU range is freed (deferred
  reuse). Disabling frees the pool; a ring radius change restarts (the pool's chunk map is sized by it).
* **CPU keep radius (chunks)** (8): the CPU keeps a chunk's records only inside it (world mode's expansion reads
  them; at least its near radius + 1). Beyond it they are dropped once uploaded; a chunk coming back inside is generated again (same
  records - nothing re-uploads).
* `Log stats`: chunks (in flight, queued, to upload), trees, per chunk average / max; CPU and GPU MB; per species of the
  CPU-held records.
* Placeholder ideal climates: Oak 9..18 C / 900..2200 mm, 90 / ha; Pine -5..5 C / 600..2200 mm, 90 / ha; Acacia
  20..30 C / 300..900 mm, 12 / ha; Willow as Oak, 4 / ha; all `ClimateWidth` 0.06.
* **ROCK RECORDS** (R4, 2026-10-05, user-untested; `setRocks` - on with "Rocks/Enabled" +
  "Rocks/World/Enabled", handed over by TreeSystem every frame; a change is a new generation): the same 4-byte
  records in the same chunks. The record TYPES: the `.tree` list, then every `.rock` (name-sorted) from
  `firstRockType()` on - TreeWorld reads the `.rock` `Placement` blocks itself. `placeChunk` places the rocks FIRST:
  * **A rock type's lattice is in WORLD space** (`RockSpecies::cell` = 1.5 x its largest `Scale`, at least 6 m;
    anchored at the world origin, one rock per cell at most): a cell's rock is a pure function of (seed, type, cell),
    so a chunk computes the rocks of its NEIGHBOURS exactly as they do. It evaluates every cell that can put a rock
    within `ROCK_REACH` (11 m: half of `ROCK_MAX_SIZE`) of itself; its own rocks become records, the others only keep its trees out.
    For that the sample grid has a halo of `GRID_HALO` = 5 points (40 m: the rock's reach, the talus probe and the
    rugged measure's 2 cells) WHILE ROCK TYPES ARE PLACED - 43^2 samples per 256 m chunk, +51 % over the trees-only
    halo of 1 point (35^2), which a world without rocks keeps - plus the valley measure's coarse grid, 21^2 more.
    **The user does not mind chunk-generation cost much (2026-10-05); the per-frame cost is what counts** - all of
    this is in the pump jobs.
  * **A type has any number of RULES** (`RockSpecies::rules`: its `Placement` blocks) and **their densities ADD** -
    a rule for everywhere plus one for a climate the type is common in, each with its own ground (2026-10-05, the
    user: per-type placement, and climate controls - "the flat rocks common in the savanna with the acacias":
    `Slab.rock`'s second block). A rule's fit at the rock's (quantized) record position: its optional climate box (as
    a tree's), its `Altitude` band, x its `Slope` band (the slope UNDER the rock; soft edges with four values,
    `bandWeight` - a boulder rolls off a slope), x `mix(1, crag, Crag)` (the terrain shader's rock coverage - steep,
    or far above the macro altitude - at the terrain's DEFAULT texture tweaks: `CRAG_*`, a deliberate approximation),
    x `mix(1, talus, Talus)` (gentle ground with a steep slope `TALUS_PROBE` = 6 m uphill of it), x THE GROUND AROUND
    IT (below), x its cluster fbm (a noise per rule). The rock exists with probability (the rules' summed Density x
    fit) x the cell's hectares x "Rocks/World/Density scale". The record does not keep the rule: what a rule cannot
    set (`Scale`, `Sink`, `Align`, the shape) is the type's.
  * **THE GROUND AROUND A ROCK** (per rule; user-untested; the user, 2026-10-05: large types off the plains, to the
    cliffs and the hills; then "way more common in valleys, and also at cliff bottoms - rugged weighted to the lower
    end of the height"; then "boulders shouldn't sit on sloped terrain as much, they would roll off"). The slope
    under the rock cannot tell any of it (and with a `Slope` band below ~0.9 the crag fit's slope term is always 0,
    so a type spread evenly over the plains). ground = `mix(Plains, mix(Rugged high, Rugged low, lowEnd), ruggedness)`,
    then - when the rule has a `Valley` - `mix(ground, Valley, valley)`: IN A VALLEY THE MULTIPLIER IS `Valley`,
    whatever the others say (an override, not a product: Plains 0.3 x a product would need a Valley of 20 for a flat
    valley floor, and that x Rugged at a valley's cliff). Three measures, each only when a rule uses it:
    * `ruggedness` = smoothstep("Rocks/World/Rugged slope start", "full", the STEEPEST grid cell within
      `RUGGED_CELLS` = 2 cells of the rock) - 0.08..0.4 rise / run, 16-24 m. Each grid cell's slope, a separable
      square max (`windowRange`), read between the cell centres (`gridAt`).
    * `lowEnd` = smoothstep(0.25, 0.75, how far DOWN the rock sits between the highest and the lowest grid point
      within the same 2 points): ~1 at a cliff's bottom, ~0 on its top, 0.5 on an even hillside.
    * `valley` = smoothstep(`VALLEY_LOW_START` 0.5, `VALLEY_LOW_FULL` 0.9, the same over ~160 m) x
      smoothstep("Rocks/World/Valley relief start (m)", "full", that height range) - 15..60 m: LOW ground with higher
      ground within ~160 m - a valley's floor (flat ones too, up to ~300 m wide), the foot of a mountain. From its
      own COARSE grid (`VALLEY_STEP` 32 m, the lowest / highest point within `VALLEY_CELLS` = 5, a halo of 6: 21^2
      Full samples; on the chunks' common lattice, so the neighbours agree). The sampler's macro `altitude` cannot
      serve: it is the coarse stage's 7.68 km / px surface.
    `Plains 0` = never on flat ground (the Block). A first version was a WORLD rule by the rock's size
    (large rocks x0.1 on plains, x3 on rugged ground): removed, the user wants the control per type, not a smaller
    average size.
  * **FOREST** (per rule, `Forest open forest`; 2026-10-07, for dead wood): the density x mix(open, forest, the
    trees' own expected density there / `ROCK_FOREST_FULL` (60 per ha), clamped) - `speciesWeights`, the tree pass's
    own sum (every species' climate fit, faded, sharpened by the best fit, x its density and cluster patches, x "Trees/
    World/Density scale"), evaluated at the rock. The rocks are placed before the trees, so the measure is what the
    trees WILL be there, not the trees placed. `Forest 0 1`: only in forests, never in their clearings.
  * "Rocks/Reload types" makes TreeWorld read the Placement blocks again too (`RockSystem::typesRevision` through
    `setRocks`); "Trees/World/Reload species" does it WITHOUT regenerating the rock meshes - the fast way to iterate
    a Placement block.
  * **Rocks do not give way to each other** (boulders lie against boulders; a rejection order would chain across
    chunk borders). **The trees give way to every rock**: a tree candidate inside its FOOTPRINT is dropped - a
    CAPSULE along the record's yaw (hash 104, rock-local X turned to (cos, -sin) as the expansion turns it), its half
    length and radius `rockFootprint(type)` x `rockRecordScale(seed, Scale)`: a round rock's has no length and a radius
    of half its size (tree out inside 0.9 x that + 0.5 m, as before); a lying trunk's runs along the log with its
    thickness (+ 0.9 m: a disc over a 15 m log cleared the forest around it); a standing one's covers its base and
    roots. The clutter's floor map uses the same capsules (`FloorType::footprint`).
  * With rocks off the tree records are as before (the same hashes, the same field interpolation).
  * The far volume takes the rock records too (R5): see "World mode", THE WORLD'S ROCKS.

### World mode (TreeSystem, W3)

`Trees/Enabled` + `Trees/World/Enabled` + `GPU expansion` (+ terrain): the trees are TreeWorld's records, not the
preview grove. Switching it, or a TreeWorld restart (its `generation()`), respawns.

* **One DYNAMIC set** (RendererVK `createDynamicTreeInstanceSet`: every piece type, a fixed `Set capacity (pieces)`
  of slots, 600 k; no growth, a full set refuses chunks and warns once), hooked into the terrain like the preview
  (`hookTerrain`; chunk index capacity (2 x near + 3)^2 x 2).
* **Near radius (chunks)** (6, Chebyshev): every chunk inside it that TreeWorld holds CPU records for (TreeWorld's
  keep radius is raised to near + 1: `requireKeepRadius`) is EXPANDED on a Low job, nearest ring first, at most 4 in
  flight: per record its tree (`treeRecordSeed`: the preview's variant / scale / yaw rules, `Size variation`) on the
  ground of ONE Full `sampleGrid` at 2 m (the terrain's LOD 0 spacing) over the chunk plus the bushes' reach, plus its
  BUSHES (its species' `Undergrowth`, else `Bushes per tree` out to 0.75 x `Spacing`; the bush species of its climate
  by `Weight` - the preview's rule; `ExpandSpecies::bushesPerTree` / `bushRadius`, `bushUndergrowth`), none in
  WATER (at the grid's nearest point: under a river's / lake's surface or the sea, or a river bed past "Terrain/Rivers/
  Vegetation clear"; the far volume's GPU mirror has no water data and keeps them). The job
  reads only an immutable `ExpandContext` (types, scales, far centre / radius per variant, the record-type -> species
  map by name, the sampler) and a copy of the records.
* `Expand per frame` (2) finished chunks join the set per frame (`addTreeInstanceChunk`), the coordinate is mapped
  (`m_vegChunkOf`) and re-stamped in the terrain (`restampVegetation`). A chunk past near + 1 is removed
  (`removeTreeInstanceChunk`, deferred reuse) and re-stamped; a pending one's result is dropped.
* A respawn joins the jobs (`stopExpansion`: the generation bumps, the results are cleared).
* **THE WORLD'S ROCKS** (R4, 2026-10-05, user-untested): `spawnWorld` appends RockSystem's loaded types
  (`Globals::rocks.worldTypes()`) to the set - a GPU type per variant: ONE mesh, the LOD chain's level 0 in the `bark`
  slot on `LitRock`, no billboard (RendererVK: the culls pick its level, no wind) - and maps TreeWorld's rock record
  types to them by name. `expandChunk` turns a rock record into ONE piece (`placeRock`): variant and yaw from the seed
  as a tree's, the scale from `rockRecordScale`, laid ON THE GROUND by `RockSystem::groundTransform` (Rocks, "A rock on
  the ground" ([Rocks.md](Rocks.md)): it follows the ground's normal by `Align`). The
  set's types are fixed, so RockSystem's `worldGeneration()` (its meshes arrived, changed or went - it loads in the
  background) respawns the set; and before RockSystem frees its meshes it calls TreeSystem's mesh-user hook
  (`setMeshUser`), which destroys the set.
  **ROCKS IN THE FAR VOLUME** (R5, 2026-10-06, user-untested): rock meshes hand over to the far-tree volume at its start
  as a tree's billboard (RendererVK tree_cull.inc.glsl: a type without a billboard drops its mesh past `Far start` +
  `Far overlap` in the main pass; it keeps casting its sun shadow). A rock GPU type carries its variant's OCCUPANCY grid
  (`RockVariant::density`: `ROCK_DENSITY_RES` = 16^3 over its rock-local box, the field soft over one voxel; generated
  with the meshes) as the type's `density`, with `solid = true` and the SINK baked into the box (`densityMin/Max` - Sink
  x height; the volume stands a piece on the ground; the lean is left out). `spawnWorld` hands the renderer a
  `TreeRecordTypeGpu` per rock record type too (albedo.w = 0: a SOLID): its variants as the set's rock types (max 8),
  its Scale range with no size variation (= `rockRecordScale`), per variant its occupied volume at scale 1, the profile
  over the sunk box. **Keep `placeRock`'s variant / scale / yaw hashes and the volume's record expansion in step**
  (102 / 103 / 104, as a tree's). RendererVK "Far-tree volume", ROCKS, has the volume side.
* **The far-tree volume (W4)** takes EVERY tree from the RECORDS (RendererVK "WORLD TREE RECORDS"), never from the
  set: within `Trees/Far record detail (m)` the GPU expands each record exactly as `expandChunk` (variant, scale, yaw,
  its bushes - the same hashes; the terrain map's ground) and splats it in detail; beyond, its exact mass per column.
  So a chunk entering or leaving the near radius changes nothing in the volume. The spawn hands the renderer the
  record types (`setTreeRecordTypes`: per TreeWorld type its variants as the set's types, Scale range, size
  variation, a tree's bush types in `ExpandSpecies::bushes` order (weights as repeats), its OWN bushes per tree / radius, per-variant mass, the
  height profile); a respawn clears them. **Keep `expandChunk` and `tree_volume_splat.cs`'s `splatRecordPlant` /
  `main` (and `tree_volume_records.cs`'s variant / scale) in step.** At most 8 variants per species and 8 bush species
  per tree (a warning past that). Without `Trees/Enabled` (no species meshes, so no density) the records are not in
  the volume. It re-bakes at most every 30 frames while record chunks change.

### The preview (temporary)

`Trees/Enabled` loads every species (enable / `Reload species` re-reads the files — no hot reload yet),
uploads each piece as two `RenderMesh`es (bark + double-sided diamond leaf cards, solid-colour `LitOpaque`
materials), and spawns a `Grove size`² grove in front of the camera plus the piece library in rows behind it
(`Show piece library`; reloads - off, the module / trunk billboards are not uploaded). `Bushes per tree` (default 4; the fraction by chance) scatters the `Kind Bush` species
around each grove tree (1.5 m .. 0.75 × spacing out, area-uniform, a bush species by `Weight`, the same variant / scale /
yaw rules) - unless the tree's species has an `Undergrowth` block, which replaces both. `Bush shadow distance (m)` (default 100, GPU path): bushes farther than this from the shadow
cascades' centre cast no sun shadow (`TreeInstanceType::shadowDistance`). `Grove type`: Mixed (the TREE species alternate) or one species by its `TreeSpecies` name — the
names are a fixed list in Settings.Trees (`TREE_GROVE_TYPES`; a tweak enum registers before the species load), so a
new species needs its name added there; a missing one falls back to mixed with a warning. Trees sit on a
`Spacing` grid (default 11 m; `Grove size` up to 512², default 350² of `Grove type` Oak, for the far-tree volume),
each offset by a seeded random `Position jitter` × spacing (default 0.8; 1 = anywhere in its cell;
the old fixed 0.3 left the rows visible), and scaled by the species' `Scale` range × `Size variation` (2^±v,
log-uniform, default 0.6 = ×0.66..1.52). One `RenderNode` per placed piece mesh, pushed every frame. **This path is replaced
by the dedicated GPU tree pipeline (G4: own shaders, per-tree bone palettes, own shadow draw); trees in that
path are not in the RT scene at first, but the design keeps RT addable.**
