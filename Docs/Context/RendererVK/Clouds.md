# Volumetric clouds

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Volumetric clouds (`CloudPipeline`, "Sky/Clouds" tweaks)

**A SANDBOX feature: full 3D clouds the camera flies through.** The game turns them off
(`setCloudsSuppressed`, `GameMatch` ctor/dtor) without touching the user's `Enabled` tweak. The sky
variant (`sky.fs.glsl`) draws NO clouds.

### Shell, layers and shape

* **The shell:** altitudes `[Bottom, Top]` above world Y 0 on a sphere of `CLOUD_R` = Earth's radius /
  "Sky/Clouds/Curvature" (`u_clouds_shellRadius`, 1..10; the terrain is flat, so this is the only horizon the far
  clouds sink under) whose centre lies straight under the camera. Only the shell's geometry uses it; the air
  (`cloudAerialScatter`, the sun's transmittance) keeps `ATMOS_R_PLANET`. The max of 10 keeps `cloudAltitude`'s
  first-order `r + R` within 1.4 % at 150 km. All cloud math is CAMERA-RELATIVE. **Precision:** `cloudAltitude` forms `(r² - R²) / (r + R)`
  and `cloudRaySphere` the stable quadratic, because float differences of two ~6.4e6 values are
  metres off.
* **TWO LAYERS:** `cloudDensity` = the MAIN layer ("Bottom (m)" / "Top (m)", `u_clouds_mainBottom` /
  `mainInvHeight`) + the optional UPPER layer ("Sky/Clouds/Upper layer": its own band, coverage, type, density
  and height variation; default on, 6000-7500 m, type 0.1, density 0.1 = thin high sheets). Its "Coverage" and
  "Density" are MULTIPLIERS on the main layer's: the CPU sends `upperCoverage x coverage` (clamped 0..1) in
  `u_clouds_upperCoverage`. Its own column LIFT ("Height variation", `u_clouds_upperHeightVariation`) uses the
  tower field NEGATED and offset at mip 2; its weather is the same map rotated -90° and offset (scale 1: still
  tiles), so its gaps do not follow the main layer's. The SHELL (`u_clouds_shellBottom` / `shellTop`, what the
  march covers) is the union of the bands; the gap between them is crossed by the coarse empty-space steps.
  Why two bands: one tall shell stretched every column's profile into a peak - a taller "Top" made the towers
  thinner, not the sky layered. Both layers share `cloudLayerShape` (the noise threshold + detail erosion).
  **The upper layer does not cost the main layer quality:** the SHADOW MAP marches the MAIN band only
  (`cloudMainIntervals`, `cloudMainDensity`; the upper layer casts no map shadow), and the view march solves its
  step growth over the main band's span (see the march).
* **COVERAGE 0 = CLEAR** (`cloudColumnCoverage`, both layers): a column's coverage is the base coverage +
  (weather.r - 0.5) x "Coverage variation", the variation faded in over the first `CLOUD_VARIATION_RAMP` (0.25)
  of the base coverage. With a constant spread the weather map's peaks stayed clouds at coverage 0. The upper
  layer ramps on its own effective coverage (upper x main).
* **MACRO COVERAGE** ("Macro size (km)" 100, "Macro variation" 1; `cloudColumnCoverage`): the weather map's
  coverage channel read over its OWN, much larger period (rotated, mip `CLOUD_MACRO_LOD` 2), added to the column
  coverage with the variation's ramp: whole regions clear or overcast. The weather period's features are the clouds'
  own size, so alone they scattered the clouds evenly everywhere. Its origin is world - wind (+ seed, axes swapped)
  wrapped by the macro period on the CPU, passed as `u_clouds_macroShift` = macro origin - noise origin, so the
  shader forms `(nxz + shift) / period` with one add (the eye offset included). Taking the origin and the eye offset
  separately cost the march 64 -> 72 registers.
* **FAR COVERAGE** ("Sky/Clouds/Far coverage", `u_clouds_farCoverage`): that column coverage (the variation's
  spread included) x a scale going 1 -> "Scale" over "Start" .. "End" (km, smoothstep) of the column's HORIZONTAL
  distance from the camera, `length(nxz - cloudNoiseOffset())` - not the sample's `camDist`, which the sun march and
  the shadow map do not pass, so the shadows and the air's cloud shadow (`cloudAirSunVis`) see the same clouds.
* **EROSION CUTOFF** ("Quality/Erosion cutoff", `u_clouds_erosionCutoff`): the erosion's remap
  `(d - lo) / (1 - lo)` leaves a thin rest wherever the detail fBm is low, which over kilometres of ray reads as
  haze. `cloudLayerShape` ends with `(d - cut) / (1 - cut)`: the rest goes, the cores stay at 1. Monotonic, so
  the cheap shape still bounds the full one (the coarse test).
* **SHELVES** ("Shape/Shelf count" 1-3, "Shelf strength", "Shelf thickness", "Shelf spacing";
  `u_clouds_shelfCount` / `shelfStrength` / `shelfHalfThickness` / `shelfLowest` / `shelfSpacing`): the main
  layer's LAYERED look - stable layers (inversions) at fixed heights of the layer, the stack CENTRED in it (the
  CPU sends the lowest, `0.5 - (count - 1) / 2 x spacing`), where every cloud that reaches one spreads out
  sideways into a flat tier. The profile is raised past 1 around each shelf height (layer-relative hf, so all
  clouds spread at the same altitude), lowering the coverage threshold there - only where the cloud already
  exists, so the tiers follow the coverage of the clouds under them. **Tried and removed:** a MIDDLE layer -
  independent decks whose coverage did not follow the clouds below.
* **The density model is `clouds.inc.glsl`**, shared by every cloud pass: weather map (coverage, type,
  density, tower height) × height profile per type (stratus → cumulus → cumulonimbus) × Perlin-Worley
  base, eroded by curl-distorted Worley detail, plus a near-camera octave inside "Near detail radius (m)".
  **The profile raises the coverage THRESHOLD** (only the strongest noise passes near the top and base, so
  tops round into domes); a profile that only scaled the density against one fixed threshold cut every
  cloud at the same height.
* **Tops.** The weather map's tower field (alpha) is 6 cells x 2 octaves per tile (`cloud_noise.cs.glsl`):
  about one smooth top per cloud. A finer field (16 cells x 4 octaves) gave every cloud several top maxima - a
  range of peaks. The column top lies in `[1 - "Tower variation", 1]` of the layer. The profile shape is three
  0..1 tweaks: **"Tower variation"**, **"Top roundness"** (the top ramp as a superellipse `(1 - s^p)^(1/p)`,
  p = 1 + 5 x roundness: a dome instead of a cone), **"Base sharpness"** (shortens the bottom ramp toward a flat
  base). Ramp caps in metres were tried first: barely visible, awkward to tune.
  **"Tower core link"** (`cloudTowerSignal`): the broad tower field alone put most clouds on a slope of it, and
  their tops tilted alike ("ramps"). The link ties the top to the cloud's OWN coverage
  (`coverage x (0.5 + tower)`): its core rises highest, its edges stay low. 0 = the field-only tops.
* **Per-column LIFT** ("Base height variation" v, `u_clouds_baseVariation`): the WHOLE column rises by up to v
  of the layer - the profile and the base + detail noise with it (`noiseAlt`) - and its height shrinks to
  (1 - v), so each cloud keeps its own rounded base and only sits higher. The field is a second weather fetch:
  the tower field ROTATED 90° (uncorrelated with the tops; scale 1 still tiles) at MIP 2, so the height drifts
  over kilometres. **Tried and replaced:** a per-column BASE cut over the same profile - the cloud's low part
  became thin tendrils down to the shell bottom. 0 = no lift (and no extra fetch).

### Noise

* **The noise is world-anchored and tiles.** Base repeats "Base repeats" times per weather tile and detail
  "Detail repeats" times per base tile — integers, so `buildUboClouds` wraps (camera − wind) by the
  weather period ONCE, in double, and every texture stays continuous. The wind and the detail's vertical
  drift ("Evolve speed (m/s)") accumulate in double on the SIM clock. **"Seed"** (0 = none) adds a fixed offset to
  both - a hash of it as a fraction of the weather period (x / z; base and detail tile inside it) and of the detail
  period (the drift), `cloudSeedOffset` - so each seed is another cloud layout at the same settings, at no cost.
* **BASE BREAKUP** ("Base breakup (km)", `u_clouds_baseBreakup`, peak to peak; `cloudLayerShape`): the base tile is
  only weather / "Base repeats" wide (3.3 km at the defaults), so from altitude the same shapes stood in rows along X
  and Z. The base lookup is warped by the weather map's type channel at mip `CLOUD_BREAKUP_LOD` (4), read twice at
  different rotations: smooth (a gentle stretch), and it tiles with the weather period, so the wrap stays continuous.
  Each base tile then shows a different part of the noise. The detail is not warped (too fine to read as rows). Two
  coarse 2D fetches per density sample; the march stays at 64 registers, no spill. Default 0: it moves the clouds but
  not the gaps (the even scatter is the coverage's scale - the macro coverage handles that).
  **A new noise lookup must use an integer
  multiple of these frequencies**, or it seams when the origin wraps.
* **The noise textures are generated on the GPU** (`cloud_noise.cs.glsl`) at initialize and on every
  shader reload, with a blit mip chain: base 128³, detail 32³, weather 512², curl 128², all RGBA8.

### The passes

**Three passes per eye:** the march (compute, half res, EVERY pixel EVERY frame — a fly-through has
parallax at every depth, so a 1-in-16 update would smear), the temporal accumulation, and the apply.

* **March** (`cloud_march.cs.glsl`; the loop is `cloudRaymarch` in `cloud_raymarch.inc.glsl`, shared with the
  sky clouds): up to the FARTHEST scene surface of the pixel's 2x2 block.
  * **Step schedule:** steps are `max(near step, g * t)`, and **g is SOLVED PER RAY** so the ray takes
    **"Steps per ray"** (capped at 75 % of "Max steps") over its shell span `[tStart, tEnd]` (a fixed-point solve
    of `N(g) = max(0, n/g - tStart)/n + ln(tEnd / max(tStart, n/g))/g`): short steps near the camera, faster
    growth far away. A fixed growth made the quality follow the layer's top / bottom RATIO. **The near step
    scales with the span:** `clamp(span / target, "Min step (m)", "Near step (m)")`, so a short span (up
    through a thin layer) still takes `target` steps. **With the upper layer on, the solve span is the MAIN
    band's** (`sStart` / `sEnd`): over the union shell the target was spread over the gap and the upper band.
    The upper layer takes its steps on top at the same growth; a ray that misses the main band solves over the
    shell. The uniform floor (distance left / steps left) is only a safety net: as the rule it made a long ray's
    EVERY step long (a grainy band at the camera's altitude).
  * **Empty-space skipping:** in clear air it takes 3x steps (`CLOUD_COARSE_MULT`) that test only the cheap
    shape (weather + base); the detail only erodes that shape, so a zero there is a zero of the full density. A
    hit backs up and marches finely; `CLOUD_COARSE_AFTER` (4) empty fine steps return it to coarse. **Past
    "Detail distance (km)"** the detail fetches are skipped (the weight fades over the last 20 %; `cloudDensity`
    takes a detail WEIGHT) and the view samples erode by the detail's MEAN instead (`CLOUD_DETAIL_MEAN_ERODE` 0.5; a
    weight up to `CLOUD_DETAIL_FETCH_MIN` = the mean alone, as `d * x - y` from `u_clouds_meanErosion`, multiplied out
    on the CPU). Without it the far clouds were uneroded and thinned as the camera came near. Weight 0 = no erosion:
    the cheap test (it must bound the full shape) and the sun march (its first step gets the weight only above the
    fetch threshold). The general remap there, or the mean on the sun march, cost the march 64 -> 72 registers.
  * **The march ends at transmittance 0.02** (`CLOUD_T_END`), and below 0.3 (`CLOUD_T_THICK`) its fine steps
    double. **The final transmittance is remapped `(T - 0.02) / 0.98`**, so the early-out level is fully
    opaque: the composite passes scene x T, and a leftover 2 % let the SUN DISC shine through any cloud.
  * **Known issue, open:** with a small step budget a darker band remains on the LIT side of far clouds at the
    camera's altitude - lighting only ("Density only" is clean), gone with the sun march off, a shorter
    "Max distance (km)", or more steps. An in-cloud step cap of one mean free path did NOT remove it.
  * **Checkerboard** (`CLOUD_CHECKERBOARD`, "Quality/Checkerboard", on): the march dispatch covers half the
    columns, each thread on this frame's parity (`marchPixel`) - full warps, half the rays. The parity flips
    every frame and so does the frame slot, so a slot's march image only ever holds ONE parity; the
    temporal pass reads only this frame's. A marched pixel clamps to its 4 diagonals; the others take the
    mean of their 4 side neighbours as the current value, last frame's march at the same pixel as history,
    and their own march limit from the scene depth (temporal binding 7).
  * **Lighting per dense sample:** a sun march, three Wrenninge multiple-scattering terms in closed form, a
    powder term, and an ambient.
    * Octave 0 uses the HG + Draine phase (Jendersie & d'Eon 2023: a fit to Mie scattering on water droplets;
      the CPU turns "Droplet size (um)" into `u_clouds_hgG` / `draineG` / `draineAlpha` / `draineWeight`).
      **The HG part's g is capped by "Forward peak limit"**: uncapped it is the droplets' diffraction peak
      (g ≈ 0.995 at 20 um, a lobe the size of the sun disc), which clipped to white behind thin cloud.
    * The multiple-scattering octaves are ISOTROPIC (a flattened droplet phase kept half the energy forward,
      and the side away from the sun went far too dark), summed over ALL octaves in CLOSED FORM: the first
      exactly, `a e^(-τa)`, plus the geometric tail `a²/(1-a) · e^(-τ a^(1+1/(1-a)))` through one effective
      extinction (constants per ray; per sample 2 exp, no loop), x "Multi-scatter strength" (non-physical above
      1). With the sun BEHIND the viewer the lit side is seen near 180°, where the droplet phase is ~0, so its
      brightness is this term alone - two octaves left thick sunlit clouds gray.
    * The ambient: the sky map's clear-sky value weighted by hf = the height within the sample's OWN LAYER
      (`cloudLayerHeightFraction`: main or upper band, not the union shell; the sun's bottom / top blend uses it
      too), plus the ground bounce falling off EXPONENTIALLY from below: albedo `u_clouds_groundBounceAlbedo` =
      the sky's "Ground Albedo" COLOUR (its hue, not its intensity) x the cloud "Ground albedo",
      `exp(-h / depth)` with h = metres above the MAIN layer's base and depth = "Ground light depth (m)" - it
      lights the undersides only.
    * The sun at the cloud uses the atmosphere transmittance along the LOCAL up of the entry point (the
      sunset on distant clouds).
    * **The sun march** (`cloudLightOpticalDepth`, "Light steps", quadratic spacing) runs only where the shadow
      map does not cover the sample (or "Shadows/Self-shadow from map" is off - the default, for quality). Its
      reach is PER SAMPLE: the way out of the sample's OWN layer toward the sun, `(layer top - altitude) / L.y`,
      capped by "Light distance (m)". It reads each step's base noise at the mip of ITS OWN LENGTH (at least
      the view sample's) and stops past `u_clouds_sunOdCut` = 7 / min(tail extinction, ms) (every
      multiple-scattering octave under 0.1 %).
  * **The aerial perspective in front of the cloud** (`cloudAerialScatter`): an 8-step single-scatter segment
    from the camera altitude, x "Lighting/Aerial perspective strength" (`u_clouds_aerialStrength`, min 0.01:
    scales the ADDED air light only; the dimming has its own tweak below). Two lights, as the
    terrain's aerial LUT: the sun, and the SKY LIGHT ("Sky Radiance" from the local up, not cloud-shadowed - most of
    the blue; without it the air in the clouds' shadow added nothing). The air thins with height, so a cloud gets
    less of it than terrain at the same distance. **"Aerial dimming"** scales the air's optical depth on the
    cloud's own light (per colour: warmer and darker far out; 1 = physical). **"Aerial fade"** takes the air's MEAN
    optical depth off the cloud's opacity (in-scatter and alpha together): far clouds fade into the sky behind them. **Each step is in the
    clouds' shadow** (`cloudAirSunVis`, `cloud_raymarch.inc.glsl`): the shadow map inside it, and PAST it the
    WEATHER MAP - the coverage of the column where the sun ray from the air point crosses the main layer's
    middle, `1 - smoothstep(0.35, 0.75, coverage)`. The steps sit at the ray's per-pixel / per-frame jitter:
    fixed points stamped the openings' lit air columns onto the clouds as blobs; jittered, the temporal pass
    averages them into soft shafts. One visibility for the whole segment left the air toward far cloud bases
    half-lit under a dense deck (a blue sheen). A ramp to the view ray's transmittance near the sun was tried
    and replaced: it cut the halo's centre only. With cloud shadows off the air stays unshadowed.
  * **The aerial perspective's distance is NOT the weighted cloud distance:** the loop also sums
    `absorbed * exp(-kAir * t)` (kAir = the ray's mean air extinction), and the haze is taken where the air
    transmittance equals that in-scatter-weighted mean. A flat ray at the camera's altitude holds the fog
    around the camera AND clouds tens of km out; at the weighted distance the near cloud's light was dimmed by
    the far cloud's air - a dark band at the camera's altitude.
  * Output: in-scatter + transmittance, and log2 distances (first hit, transmittance-weighted, march limit).
* **Temporal** (`cloud_temporal.cs.glsl`): reprojects the weighted cloud distance minus this frame's
  wind displacement, rejects a history whose cloud distance lies OUTSIDE the neighbourhood's range of
  this frame's distances (padded 0.2 in log2), clamps its colour to the neighbourhood ("Quality/Temporal
  blend"). **Not a per-pixel distance test:** a ray flat through the layer passes clouds at very different
  distances, so its one weighted distance jumps with the step jitter every frame - a per-pixel test rejects the
  history exactly where the raw march is noisiest. The march jitter is a per-pixel hash + golden-ratio step
  (grain, not lines, where history is missing). The history is the OTHER frame slot's accumulation; all images
  are cleared to "no cloud" at creation, so there is no history-valid flag. **Silhouette rules** (each fixed a
  cloud- or sky-coloured outline around objects against clouds):
  * **The history's march LIMIT must match this frame's** (`LIMIT_MATCH`, 0.15 in log2): the bilinear
    history fetch at a silhouette pulls the sky neighbour's cloud over the near surface, and the feedback
    builds it up.
  * The checkerboard fill takes its front only from neighbours that hold cloud (an empty march stores its
    limit as the front).
  * The fill weights its neighbours by how well their march limit matches its own (`1 / (1 + 16·Δ)`, as the
    upsample), and the clamp range takes only neighbours within `LIMIT_MATCH` of the pixel's limit - else a
    "no cloud" texel over a near surface leaks into the sky texels beside it.
  * **An ISOLATED fill pixel** (no side neighbour within `LIMIT_MATCH`: a thin leaf against the clouds, or a
    gap in one) takes the history whole (last frame marched this pixel), no clamp and no range test (the limit
    test stays), and its front is the neighbours' own cloud front, NOT clamped to its limit: a cloud behind the
    leaf stays behind it. Clamping it flickered the sky's cloud onto the leaf every other frame.
* **Apply** — the depth-aware 4-tap upsample is `cloud_upsample.inc.glsl` (a texel whose first cloud
  lies behind this pixel's surface counts as "no cloud"), used by TWO passes:
  * **Fog ON: the fog apply composites the clouds** (`vol_apply.fs.glsl`, bindings 6/7). Fog laid over
    finished clouds fogged them as if they stood at the scene depth. With the cloud (S, T) at its weighted
    distance tc and the fog F to the scene, C to tc:
    `out = T·F.rgb + C.a·S + (1−T)·C.rgb + scene·(T·F.a)` — exact for a cloud at one distance and linear
    in the scene, so the blend state is unchanged. `fogTo` (froxel volume + far field) runs twice on
    cloudy pixels - ONCE when the cloud and the scene both lie past the froxel volume and "Fog/Far
    Field/Max distance (km)" (`u_fog_farFieldMaxDistance`; the far field adds no fog past it): both then
    see the same fog.
  * **Fog OFF: the "Cloud apply" scene stage** (`cloud_apply.fs.glsl`, before the fog apply slot, gated on
    `cloudsEnabled() && !fog`). `colorWriteAlpha = false`.
* **Known limitation:** particles and transparents draw before the apply, so one inside a cloud gets the
  cloud of the opaque pixel behind it — the fog apply has the same limitation.

### Clouds in the fog

* **The fog's sun term is cloud-shadowed everywhere**: the froxels per froxel (`vol_scatter.cs.glsl`), the
  FAR FIELD per sub-segment (`volFarField`: 2 jittered shadow-map taps each, weighted by the sub-segment's
  share of the in-scatter; the apply binds the cloud shadow map at 8). **"Fog/Sun scatter"** (`u_fog_sunScatter`):
  a non-physical gain on the SUN in-scatter only (froxels, far field, reflection fog) - sunlit fog and the
  shafts brighten, shadowed fog and the extinction do not: strong god rays without fogging up the world.
* **The fog's sky-SH ambient is cloud-shadowed too**: the sky SH is ONE probe for every point, and holds the
  sunlit ground below the horizon (`skyGroundSun`, plus the "Sky/Ground Horizon" share above it), never
  cloud-shadowed. `giSkySunIrradiance` (gi_probe.inc.glsl) is that part in closed form; `giEvalSkySHCloud`
  subtracts its shadowed share: the froxels' GI-off ambient, `giIrradiance`'s out-of-field fallback (its
  `cloudSunTransmittanceSoft`, so surfaces too), and the far field (weighted by `sunW`, no extra taps). The sun
  cone in the sky (aureole) is not part of it.
* **"Fog/Shaft haze"** ("Density (1/m)", min 1e-5: always on; "Height (m)"; `u_fog_hazeDensity` /
  `hazeInvHeight`): the fog is a HEIGHT fog, nearly gone a few tens of metres up, so shafts from the clouds
  showed only after cranking the base density. The haze is a separate thin medium reaching up to the clouds
  (its own scale height from the fog's height base) that adds SUNLIT in-scatter only (x "Sun scatter"): no
  extinction, no ambient - non-physical on purpose. Froxels: per froxel (`heightFogMean`), its visibility the
  fog's where the fog computed one, else the clouds' alone. Far field (`volFarHazeStep`): per sub-segment, with
  the same cloud-visibility taps, behind the fog in front of it and bounded by its OWN extinction (a level ray
  to the horizon stays finite; it does not dim the scene).

### The cloud shadow map

A Beer shadow map: `cloud_shadow.cs.glsl` writes, `cloud_shadow.inc.glsl` reads. Two sun-aligned ortho
cascades ("Sky/Clouds/Shadows": "Near cascade (km)", "Far cascade (km)") in ONE 2-layer RGBA32F array
(`SHADOW_RESOLUTION` 1024), recorded straight into the primary after the pre-scene stages (before GI, fog and
the scene).

* A texel = one line along the sun through the MAIN layer's band (not the union shell): x = the along-light
  coordinate `dot(p - centre, L)` of the first cloud coming from the sun, y = the mean extinction to the last
  cloud, z = the whole optical depth. A receiver's optical depth is `min(y * max(x - a, 0), z)`, so ONE fetch
  serves the ground, a mountain or fog froxel inside a cloud, and the clouds' own samples. Because x is a
  coordinate and not a distance from a start plane, the map needs no per-cascade start distance.
* **PROGRESSIVE UPDATES, one buffer, no toroidal addressing.** Each frame a cascade renders 1 / split of
  its texels ("Near update split", default 1/4; "Far update split", default 1/64): one texel of each 2x2 /
  4x4 / 8x8 block, the rotating phase's (`shadowTexel` in cloud_shadow.cs.glsl), one thread per rendered texel
  - a constant cost per frame instead of a spike every Nth frame. A 64-frame cycle moves the clouds ~11 m at
  10 m/s, under a third of the far cascade's ~39 m texel. That needs ONE texel-to-world mapping for all of
  them, so `buildUboClouds` FREEZES each centre (snapped to whole texels in light space, double, world space)
  and re-centres only when the camera is 1/8 of the extent away (125 m near, 5 km far at the defaults), or the
  sun, the extent or the enable changed: that frame renders the whole cascade (a rare spike; a CONTINUOUSLY
  moving sun re-renders every frame). The lookup is unchanged: only the frozen centre's camera-relative
  offset is rebuilt each frame. **Tried and reverted:** a frozen centre travelling WITH the wind - it fixed
  no visible artefact, and made a still camera re-centre in wind. **Steps per cascade** ("Near steps", "Far
  steps"). **So the map is ONE image, not per frame slot.** It is cleared to "no cloud" at creation and stays
  bound while the clouds are off (`u_clouds_shadowRendered = 0` makes the lookup return 1).
* **THE FAR CASCADE IS FILTERED ON THE RESULT** (`cloudShadowFarODFiltered`): its texels are tens of
  metres, and the OD is non-linear in the stored terms, so the sampler's bilinear blend of (x, y, z) stays
  blocky. Instead it `texelFetch`es the 2x2 texels, computes each one's transmittance and blends those
  bilinearly (PCF-style), then returns `-log(T)`. **"Far softness"** (`u_clouds_shadowFarSoftness`, texels)
  jitters the lookup per pixel and frame (a world-anchored hash: vertex-stage consumers have no
  gl_FragCoord); TAA and the fog's temporal blend average it into a penumbra. The near cascade keeps its
  one hardware-bilinear fetch.
  **Only SURFACES take the filter** (`cloudSunTransmittance`: lit core, terrain, ocean, film, decals).
  **`cloudSunTransmittanceBilinear`** (the far cascade's one bilinear fetch) serves what already averages over
  space and time: the fog froxels + shaft haze, the far field's jittered taps, the clouds' air term and
  particles.
* Past the far cascade the transmittance fades to a rough mean from the coverage
  (`u_clouds_shadowMeanTransmittance`).
* **LOW SUN** ("Shadows/Low sun average start / end (deg)", 15 / 3): the transmittance also blends to that mean by
  `u_clouds_shadowLowSunAverage` = 1 - smoothstep over the sun's elevation (live; `cloudShadowLowSunMix`), and fully
  averaged the map is not read. A cloud's shadow lies height / tan(elevation) away (~17 km at 5 degrees for a 1.5 km
  cloud), so a slowly moving low sun (the sun cycle) swept the shadows across the world: fast flicker. The clouds' own
  lighting (`cloud_raymarch`) still reads the map.
* **Consumers** multiply their sun term by the cloud transmittance (each includer sets
  `CLOUD_SHADOW_BINDING`): the lit core (lit, masked, transparent, terrain; forward set binding 22), the ocean
  (22; `sunTint`, so glint, body, foam and SSS), the terrain wet film, the fog scatter (13 — the shafts through
  cloud gaps), the fog apply (8), LIT particles (draw set 11, vertex stage), LIT decals (6), the far-tree
  volume march (11), the aerial LUT (2), the sky clouds (3) and the cloud march's self-shadow (9; outside the
  map it falls back to the sun march).
  * **The wet film** has its own `sunTint` copy (in-scatter, reflection fog, mirror hits) AND its own sun
    visibility tap in main, `g_sunVisSurface` -> `sunSurfaceRadiance()` (glint, whitewater, the bubble cloud).
    **`g_sunVisSurface` already HOLDS the cloud and the eclipse, so never multiply it by `sunTint`**: that
    squares the cloud shadow. The tap sits at the WATER SURFACE (`in_pos`, up normal), not the ground under it
    (`TERRAIN_LIT_POS`).

### GI under cloud shadows

**Dimmed at the LOOKUP, not in the trace.** The trace's gather-hit sun has NO cloud shadow (the trace does not
bind the map): cloud-shadowed hits made each probe lag a moving cloud shadow by its update interval, so the GI
flashed under shadows.

* Every probe keeps its **sun DC luminance** (probe vec4 `[6].x`, `GI_SUN_V4`): the direct sun at gather hits
  + the sun share of the multi-bounce + the sunlit-ground term `skyGroundSun` of every DOWNWARD MISS (left out,
  random downward misses added undimmed sun under a cloud shadow - probe flashes at shadow borders) + its
  "Ground Horizon" share w on the UPWARD misses, blended at the SH's alpha.
* **A MISS IS THE SKY SH's RADIANCE** (`giSkySHRadiance`), not the sky map: a probe in open space then gathers
  exactly what the out-of-field fallback (`giEvalSkySHCloud`) evaluates, cloud dimming included. The miss's sun
  part is the STEP (G below, w G above), not its L1 form (negative toward the zenith for w < 0.2): only the
  sun's DC is kept, and both have the same. Volume mode reads last frame's sky SH (the bake's copy); probe mode
  reads the slot this dispatch rewrites.
* The bake stores the **sun fraction s × W** in the tail image's `.b` (RGBA16F), and **each cascade lookup turns
  it into the sun's SHARE AT THE NORMAL** (`giSunShare`): a probe keeps only the sun's DC, so its direction is
  MODELLED - the sunlit ground at the gather hits, light from the lower hemisphere, whose L1 irradiance is its
  DC term × (1 − n·up). One DC fraction for every normal over-dimmed up-facing ground. Sun from ABOVE (the
  "Ground Horizon" share of the upward misses, a sunlit wall overhead) is outside the model.
* `giIrradiance` returns `E × (1 − share (1 − T))` with `T = cloudSunTransmittanceSoft` — the FAR cascade
  only (one bilinear fetch): the bounce comes from tens of metres around, so the soft value fits.
  Indoors s ≈ 0, so a roof is not dimmed by the cloud above it. `evalProbeCoverage` stays UNDIMMED (it
  returns s as well): the trace's multi-bounce needs the undimmed value. **The dimming compiles in only
  when the includer has `cloud_shadow.inc.glsl` BEFORE `gi_probe.inc.glsl` and `CLOUD_SHADOWS` is set.**
  s is the DC fraction (not per direction) and one scalar for RGB: the warm sun bounce and the blue sky
  part are dimmed alike. It adds no registers or local memory to the pipelines that use it.

### The sky clouds

`cloud_sky.cs.glsl`, `CloudPipeline::recordSky`, in the primary after the shadow map and before GI: one
invocation per sky-map texel, `cloudRaymarch` with `SKY_CLOUD_STEPS` (64) steps, written as (in-scatter,
transmittance) into a lat-long image on the sky map's own texel grid; the sky-map bake composites it into its
GI and mirror layers. So the ocean / wet-film reflections, the GI miss rays, the sky SH and the forward pass's
sky ambient all see the clouds (overcast darkens the ground's sky light).

* **TEMPORAL:** 64 steps are far too coarse near the horizon (the ocean reflects mostly those directions), and
  the samples sit relative to the camera, so a fast camera slid them through the noise. Each march takes a new
  jitter (per-texel hash + golden ratio) and blends into the texel's own last value (the image is read-write,
  cleared to "no cloud" at creation). The history weight is FRAME-TIME based: `u_clouds_skyHistory =
  exp(-3 dt / T)` with T = "Quality/Sky map history (s)" (95 % of a change after T seconds; real time, so it
  still converges while the sim is paused; 0 = no history). **A FLOOR under it** (both layers): "Sky map min
  samples" N (0 = time only) - the weight is at least (N - 1) / (N + 1), an average of N marches. Time alone
  averaged too few marches at low frame rates, and the ocean mirrored the march noise as flicker.
* **PROGRESSIVE:** each frame marches ONE texel of every 2x2 block (rotating phase, the shadow map's order;
  `CloudPipeline::SKY_UPDATE_FRAMES` = 4, so the CPU's dt spans 4 frames), and only the UPPER hemisphere is
  dispatched (the lower half stays "no cloud" from the clear). The pass is bound by its longest ray's serial
  chain, not its thread count, so `recordSky` ends WITHOUT a barrier: the sky-map bake's input barrier
  (`GIProbePipeline::recordSkyMap`) orders it before its one reader, and the GI prep's compute in between runs
  alongside it.
* **The observer stands on the GROUND under the camera** (`ATMOS_OBSERVE_HEIGHT`), not at the camera: every
  reader of the sky map sits under the clouds, even while the camera flies above them. The ambient reads the
  sky map's CLEAR layer from LAST frame (no feedback loop through the clouds' own image).
* **The clear layer (2) holds per-frame CONSTANTS, one texel each** (`SKY_MAP_*_TEXEL` in atmosphere.inc.glsl),
  written by invocation (0, 0, 0) of the bake, whose dispatch covers only layers 0 and 1
  (`SKY_MAP_DISPATCH_LAYERS`): the clouds' ambient (the clear sky over the zenith + four directions at 30°,
  averaged) and the far trees' sky light (the GI layer, see "Far-tree volume" ([Vegetation.md](Vegetation.md))).
  Each reader does one `texelFetch`.
* **TWO CLOUD LAYERS: the GI sky is AVERAGED OVER OBSERVERS** (`CloudPipeline::SKY_LAYERS`, a 2-layer array,
  the dispatch's z). From one observer the cloud overhead and the cloud in front of the sun ARE that observer's
  cloud shadow, and every GI-layer reader (sky SH -> fog / ocean / out-of-field ambient, far trees, GI misses)
  lit the whole world with it. Layer 0 = from under the camera, composited into the MIRROR layer (reflections
  show the real clouds); layer 1 = each march from a new observer in a disc around the camera (R2 over the disc
  per texel, uniform by area: "Quality/GI sky observer radius (m)", `u_clouds_giSkyObserverRadius`), its
  history the average over them, composited into the GI layer. A binary cloud / gap per march needs a longer
  history: "Quality/GI sky history (s)" (`u_clouds_giSkyHistory`).
* **THE GI LAYER IS LOW-RES:** its only readers are the sky-SH projection and the GI-off zenith ambient
  (terrain, ocean, film), so the clouds' layer 1 and the sky map's GI layer live in the `SKY_MAP_GI_WIDTH` x
  `SKY_MAP_GI_HEIGHT` (64x32) CORNER of their images, on their own lat-long grid (the rest of the dispatch's
  threads exit), read with `skyMapGISample` (atmosphere.inc.glsl: 4 `texelFetch`es, U wrapped inside the
  corner) - **never `textureLod` + `skyMapUV`**. The zenith readers take `SKY_MAP_GI_ZENITH_TEXEL`. The
  corner's lower half is rewritten as "no cloud" every pass, so a shader reload leaves no full-res texels in it.
* **THE MIRROR LAYER'S CLEAR SKY IS CACHED** (layer 3, `SKY_MAP_LAYER_MIRROR_CACHE`, `SKY_MAP_LAYERS` 4):
  the 12-step `mirrorSkyRadiance` changes only with the sun and the sky tweaks, so each frame re-marches ONE
  texel of every 2x2 block (rotating phase) into the cache and composites this frame's clouds over the cached
  value for the rest. A moving sun lags by up to 3 frames. The image is cleared to 0 at creation; alpha 0 = not
  baked yet, marched at once. The bake's entry barrier carries the RAW from last frame's cache writes.

### The visible sky

**The visible sky is seen from the camera altitude** (`sky.fs.glsl`, `atmosphereScatter`'s observer
height): the air above thins, and the horizon dips (`cosHorizon`). Below the dipped horizon the march ends
at the ground, so the pixel is the haze in front of it plus the lit ground through the march's
transmittance; the sun disc, moon and stars are left out there. The view optical depth is
`atmosSegmentOD` / `atmosRayBegin` + `atmosRayOD` (atmosphere.inc.glsl; the per-ray form keeps the
origin's Chapman values, so a march pays one evaluation per step), stable for descending rays (a "to space"
depth through the planet is float noise). The sky MAP and every indirect path keep the ground observer.

### Shader defines and the register budget

* **The cloud toggles are BAKED defines**, injected into every shader compile by `buildLayoutPreamble`
  (Shader.cpp) from `RendererVKLayout::g_cloudShaders` (`Renderer::syncCloudDefines`): `CLOUDS`
  ("Sky/Clouds/Enabled"), `CLOUD_SHADOWS` (+ "Shadows/Enabled"), `CLOUD_SELF_SHADOW_MAP` (+ "Shadows/Self-shadow
  from map", off by default), `CLOUD_POWDER` (`CloudParams::powder` above 0; its tweak is not registered, so it
  stays at 0 and the define off), `CLOUD_CHECKERBOARD` ("Quality/Checkerboard"; also sizes the march dispatch,
  which the reload re-records) and `CLOUD_DEBUG_MODE` ("Quality/Debug mode": the march / temporal debug views
  compile out at 0). A change of a DEFINE waits for the GPU and reloads every shader, like the GI grid tweaks;
  `syncCloudDefines` returns whether one changed.
* What a define cannot hold stays a runtime UBO flag: `u_clouds_enabled` = the march ran this frame (the game
  suppresses it), `u_clouds_shadowRendered` = the map was rendered this frame (suppressed, or the sun at the
  horizon).
* **Register rules:** **the march applies its four light colours AFTER the loop** (the in-scatter is linear in
  them, so the loop sums three scalar weights), and **the fog apply folds the cloud part before the scene's
  fog** (4 live values across the second `fogTo`, not 8). Each saved 8 registers. The cloud lookup adds nothing
  to the lit, terrain, ocean, fog scatter, GI trace, particle and decal shaders.
* **Debug mode** ("Sky/Clouds/Quality/Debug mode"): step count heat, density only, history rejection.
