# Volumetric clouds

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Volumetric clouds (`CloudPipeline`, "Sky/Clouds" tweaks)

**A SANDBOX feature: full 3D clouds the camera flies through.** The game turns them off
(`setCloudsSuppressed`, GameMatch ctor/dtor) without touching the user's `Enabled` tweak. The sky
variant (`sky.fs.glsl`) draws NO clouds any more.

* **The shell:** altitudes `[Bottom, Top]` above world Y 0 on a sphere of `ATMOS_R_PLANET` whose centre
  lies straight under the camera (the terrain is flat; from altitude the deck still curves to the
  horizon). All cloud math is CAMERA-RELATIVE. **Precision:** `cloudAltitude` forms `(r² - R²) / (r + R)`
  and `cloudRaySphere` the stable quadratic, because float differences of two ~6.4e6 values are
  metres off.
* **TWO LAYERS** (2026-09-28): `cloudDensity` = the MAIN layer (its own band, "Bottom" / "Top",
  `u_clouds_mainBottom` / `mainInvHeight`) + the optional UPPER layer ("Sky/Clouds/Upper layer": its own band, coverage, type and
  density scale, `u_clouds_upperEnabled` / `upperDensity` / `upperBottom` / `upperInvHeight` / `upperCoverage` / `upperType`; default on, 6000-7500 m, type 0.1, density 0.1 = thin high
  sheets). Its "Coverage" and "Density" are MULTIPLIERS on the main layer's (2026-09-29; coverage default 0.6):
  the CPU sends `upperCoverage x coverage` (clamped 0..1) in
  `u_clouds_upperCoverage`, and the density was always `x upperDensity` before "Density (1/m)". **Its own column
  LIFT** ("Upper layer/Height variation", default 0.2, `u_clouds_upperHeightVariation`; 2026-09-29), the main layer's method:
  without it every sheet sat at the band's bottom, one altitude for the whole deck. Its field is the tower field
  NEGATED and offset at mip 2 (uncorrelated with the main lift and this layer's weather; still tiles). The upper layer's weather is the same map rotated -90° and offset (scale 1: still tiles), so its
  gaps do not follow the main layer's. The SHELL (`u_clouds_shellBottom` / `shellTop`, what the march and the shadow map cover)
  is the union of the bands; the gap between them is crossed by the coarse empty-space steps. Why: one tall
  shell stretched every column's profile into a peak - a taller "Top" made the towers thinner, not the sky
  layered. Both layers share `cloudLayerShape` (the noise threshold + detail erosion). **The upper layer does
  not cost the main layer quality** (2026-09-30): the SHADOW MAP marches the MAIN band only (`cloudMainIntervals`,
  `cloudMainDensity`) - the upper layer casts no map shadow (thin, low-density sheets; its own lighting comes from
  the sun march) - and the view march SOLVES its step growth over the main band's span (see the March).
  **COVERAGE 0 = CLEAR** (`cloudColumnCoverage`, both layers; 2026-09-29): a column's coverage is the base
  coverage + (weather.r - 0.5) x "Coverage variation", the variation faded in over the first 0.25 of the base
  coverage (`CLOUD_VARIATION_RAMP`). With a constant spread the weather map's peaks stayed clouds at coverage 0;
  from 0.25 up it is exactly the old sum. The upper layer ramps on its own effective coverage (upper x main).
  **EROSION CUTOFF** ("Erosion cutoff", default 0.1, `u_clouds_erosionCutoff`; 2026-09-29): the erosion's remap
  `(d - lo) / (1 - lo)` leaves a thin rest wherever the detail fBm (so `lo`) is low, and over kilometres of ray
  that rest read as haze in the open, eroded areas. `cloudLayerShape` ends with `(d - cut) / (1 - cut)`: the rest
  goes, the cores stay at 1. Monotonic, so the cheap shape still bounds the full one (the coarse test).
  **SHELVES** ("Shelf count" 0-3 (default 1), "Shelf strength", "Shelf thickness"; `u_clouds_shelfCount` / `shelfStrength` / `shelfHalfThickness`; 2026-09-28): the
  main layer's LAYERED look - stable layers (inversions) at fixed heights of the layer ("Shelf spacing" between them,
  default 0.2, the stack CENTRED in the layer: the CPU sends the lowest, `0.5 - (count - 1) / 2 x spacing`, in
  `u_clouds_shelfLowest` and the spacing in `u_clouds_shelfSpacing`; 2026-09-29 - before, count evenly spaced at i / (count + 1)), where every
  cloud that reaches one spreads out sideways into a flat tier (stratocumulus cumulogenitus; the anvil at a storm's
  top). The profile is raised past 1 around each shelf height (layer-relative hf, so all clouds spread at the same
  altitude), lowering the coverage threshold there - only where the cloud already exists, so the tiers follow the
  coverage of the clouds under them. **Tried and removed:** a MIDDLE layer (the main model over a second band, first
  with the main settings, then with its own copy): independent decks whose coverage did not follow the clouds below.
* **PEAKS - the tower field was the cause** (2026-09-28): the weather map's tower height (alpha, per-column top)
  was generated at 16 cells x 4 octaves per tile against the coverage's 6, so every cloud held several top
  maxima and its top followed them - a range of peaks, higher with a taller "Top". It is now 6 cells x 2 octaves
  (`cloud_noise.cs.glsl`): about one smooth top per cloud. The profile shape is three 0..1 tweaks in
  `u_clouds_towerVariation` / `topRoundness` / `baseSharpness`: **"Tower variation"** (how far a top may drop below the layer top; 0.55 = the old fixed
  [0.45, 1]), **"Top roundness"** (the top ramp as a superellipse `(1 - s^p)^(1/p)`, p = 1 + 5 x roundness: the
  profile - which raises the coverage threshold - holds near 1 and falls only at the ramp's end, a dome instead
  of a cone), **"Base sharpness"** (shortens the bottom ramp toward a flat base). Tried first and replaced: caps
  on the ramps in metres - barely visible, and awkward to tune.
  **"Tower core link"** (`u_clouds_towerCoreLink`, default 0.75; `cloudTowerSignal`): the broad tower field alone put
  most clouds on a slope of it, and their tops tilted alike - "ramps". The link ties the top to the cloud's OWN
  coverage (`coverage x (0.5 + tower)`): its core rises highest, its edges stay low, and the tower field scales
  that per cloud (some tower, some stay flat). 0 = the field-only tops.
* **The density model is `clouds.inc.glsl`**, shared by every cloud pass: weather map (coverage, type,
  density, tower height) × height profile per type (stratus → cumulus → cumulonimbus) × Perlin-Worley
  base, eroded by curl-distorted Worley detail, plus a near-camera octave inside `Near detail radius`.
  **The profile raises the coverage THRESHOLD** (only the strongest noise passes near the top and base, so
  tops round into domes); a profile that only scaled the density against one fixed threshold cut every
  cloud at the same height. The tower height stretches the profile per column over `[0.45, 1]` of the column.
  **Per-column LIFT** ("Base height variation" v, `u_clouds_baseVariation`, default 0.03 - the user's pick; 2026-09-28): the WHOLE
  column rises by up to v of the shell - the profile and the base + detail noise with it (`noiseAlt`) - and its
  height shrinks to (1 - v), so each cloud keeps its own rounded base and only sits higher. The field is a
  second weather fetch: the tower field ROTATED 90° (uncorrelated with the tops; scale 1 still tiles with the
  weather period) at MIP 2, so the height drifts over kilometres. **Tried first and replaced:** a per-column
  BASE cut over the same profile (the field at twice the frequency, mip 0) - the cloud's low part became thin
  tendrils down to the shell bottom, a "peak" under every cloud. 0 = no lift (and no extra fetch).
* **The noise is world-anchored and tiles.** Base repeats `Base repeats` times per weather tile and detail
  `Detail repeats` times per base tile — integers, so `buildUboClouds` wraps (camera − wind) by the
  weather period ONCE, in double, and every texture stays continuous. The wind and the detail's vertical
  drift (`Evolve speed`) accumulate in double on the SIM clock. **A new noise lookup must use an integer
  multiple of these frequencies**, or it seams when the origin wraps.
* **The noise textures are generated on the GPU** (`cloud_noise.cs.glsl`) at initialize and on every
  shader reload, with a blit mip chain: base 128³, detail 32³, weather 512², curl 128², all RGBA8.
* **Three passes per eye:** the march (compute, half res, EVERY pixel EVERY frame — a fly-through has
  parallax at every depth, so a 1-in-16 update would smear), the temporal accumulation, and the apply.
  * **March** (`cloud_march.cs.glsl`, right after RTAO): up to the FARTHEST scene surface of the pixel's
    2x2 block; steps are `max(Near step, g * t)`, and **g is SOLVED PER RAY** so the schedule takes
    **"Steps per ray"** (default 300, capped at ~75 % of `Max steps`) over the ray's shell span `[tStart, tEnd]`
    (a fixed-point solve of `N(g) = max(0, n/g - tStart)/n + ln(tEnd / max(tStart, n/g))/g`): short steps near the
    camera, faster growth far away. (2026-09-29: it replaced the fixed "Step growth" g, with which a ray crossing
    the shell from below took `ln(top / bottom) / g` steps at any angle - the quality followed the layer's
    top / bottom RATIO, ~280 steps at 300-5000 m but ~70 at 1500-3000 m.) **The near step scales with the
    span:** `clamp(span / target, "Min step", "Near step")` (2026-09-30) - a fixed near step gave a SHORT span (up
    through a thin layer) fewer steps than the target; now it takes `target` uniform steps, and a long span keeps
    "Near step". "Min step (m)" (default 3, `u_clouds_minStep`) is the floor: finer steps only re-read the same noise.
    **With the upper layer on, the solve span is the MAIN band's** (`sStart` / `sEnd`, two more sphere tests): over
    the union shell the target was spread over the gap and the upper band, and the main layer lost steps. The
    upper layer takes its steps on top at the same growth; the gap uses the coarse empty-space steps; a ray that
    misses the main band solves over the shell. `tStart` / `tEnd` stay the shell's for the air term. The uniform floor (distance left / steps left) is
    only a safety net: as the rule it made a long ray's EVERY step long (400 m from the camera on, flat
    through the layer: one density sample per step, opaque at once - a grainy band at the camera's altitude).
    **Known issue, open:** with a small budget (200) a darker band remains on the LIT side of far clouds at
    the camera's altitude - lighting only ("Density only" is clean), gone with the sun march off, with
    "Max distance" 20 km, or with 600 steps. An in-cloud step cap of one mean free path did NOT remove it
    (tried and removed 2026-09-27). The default `Max steps` is 600: only long (flat) rays use the extra steps. **The march itself is `cloudRaymarch` (`cloud_raymarch.inc.glsl`), shared with
    the sky clouds.** **Empty-space skipping:** in clear air it takes 3x steps that test only the cheap
    shape (weather + base); the detail only erodes that shape, so a zero there is a zero of the full
    density. A hit backs up and marches finely; 4 empty fine steps return it to coarse. The sun march reads
    the detail noise on its first step only. **Past `Detail distance`** the detail erosion is off (its
    weight fades over the last 20 %; `cloudDensity` takes a detail WEIGHT) and the detail fetches are
    skipped. **The march ends at transmittance 0.02**, and below 0.3 its fine steps double. **The final
    transmittance is remapped `(T - 0.02) / 0.98`**, so the early-out level is fully opaque (2026-09-28): the
    composite passes scene x T, and the leftover 2 % let the SUN DISC (thousands of times the sky) shine through
    any cloud, however dense. **The aerial in-scatter in front of the cloud is in the clouds' shadow**: EACH of
    `cloudAerialScatter`'s 8 steps, placed at the ray's per-pixel / per-frame jitter (4 fixed points stamped the
    openings' lit air columns onto the clouds beside them as blue BLOBS; jittered, the temporal pass averages them
    into soft shafts), takes `cloudAirSunVis` (cloud_raymarch.inc.glsl; the function moved there from
    clouds.inc.glsl): the shadow map inside it, and PAST it the WEATHER MAP - the coverage of the column where the
    sun ray from the air point crosses the main layer's middle, `1 - smoothstep(0.35, 0.75, coverage)`. (2026-09-29:
    it was ONE visibility for the whole segment, the mean of two `cloudSunTransmittance` taps at 1/4 and 3/4 - and
    past the far cascade that function returns the sky-wide constant mean `mix(1, 0.3, coverage)`, so the 10-40 km of
    air toward far cloud bases lay half-lit under a dense deck: a BLUE Rayleigh SHEEN on the dark bases.) Its
    Mie forward peak otherwise drew a sun glow on top of dense cloud. Tried first and replaced: a ramp to the
    VIEW ray's transmittance within ~10 degrees of the sun - it cut the halo's centre only, a black hole in a
    ring. With cloud shadows off the air stays unshadowed.
  * **Checkerboard** (`CLOUD_CHECKERBOARD`, "Quality/Checkerboard", on): the march dispatch covers half the
    columns, each thread on this frame's parity (`marchPixel`) - full warps, half the rays. The parity flips
    every frame and so does the frame slot, so a slot's march image only ever holds ONE parity; the
    temporal pass reads only this frame's. A marched pixel clamps to its 4 diagonals; the others take the
    mean of their 4 side neighbours as the current value, last frame's march at the same pixel as history,
    and their own march limit from the scene depth (temporal binding 7). Per dense sample: a sun march (`Light steps` over `Light distance`), three Wrenninge
    multiple-scattering octaves — octave 0 on the HG + Draine phase (Jendersie & d'Eon 2023: a fit to Mie
    scattering on water droplets; the CPU turns `Droplet size (um)` into its four parameters,
    `u_clouds_hgG` / `draineG` / `draineAlpha` / `draineWeight`; **the HG part's g is capped by "Forward peak limit" (0.95)**: uncapped it is the droplets'
    diffraction peak, g ≈ 0.995 at 20 um - ~5400 / sr in a ~0.3 degree lobe, the size of the sun disc - and behind
    thin cloud that lobe clipped to white after the exposure: the sun looked bigger and brighter behind clouds
    than in clear sky), the multiple-scattering octaves ISOTROPIC (a flattened droplet phase kept ~half the energy
    in a g ≈ 0.66 forward lobe, and the side away from the sun went far too dark), summed over ALL octaves in
    CLOSED FORM: the first exactly, `a e^(-τa)`, plus the geometric tail `a²/(1-a) · e^(-τ a^(1+1/(1-a)))` through
    one effective extinction (constants per ray; per sample 2 exp, no loop), x "Multi-scatter strength"
    (`u_clouds_multiScatterStrength`, default 1, non-physical above 1). With the sun BEHIND the viewer the lit side is seen near
    180°, where the droplet phase is ~0, so its brightness is this term alone: the old two octaves (~0.14 of the
    sunlight) left thick sunlit clouds gray; all of them at a = 0.9 give ~0.7 (2026-09-29; an N-octave loop came
    between). Then a
    powder term, and an ambient: the sky map's CLEAR layer weighted by hf = the height within the sample's OWN
    LAYER (`cloudLayerHeightFraction`: main or upper band, not the union shell - a main-layer top would otherwise
    lose its sky light under a tall shell; the sun's bottom / top blend uses it too), plus the
    ground bounce falling off EXPONENTIALLY from below (its albedo `u_clouds_groundBounceAlbedo` = the sky's "Ground Albedo"
    COLOUR - its hue, not its intensity, which scales the sky-sphere ground plane and defaults to 0 - x the cloud
    "Ground albedo"), `exp(-h / depth)` with h = metres above the MAIN layer's base and depth =
    "Ground light depth (m)" (`u_clouds_invGroundLightDepth`, default 297 m) - it lights the undersides only (2026-09-28; the
    old linear `1 - hf` lit the whole cloud); the energy-conserving step integral.
    The sun at the cloud uses the atmosphere transmittance along the LOCAL up of the entry point (the
    sunset on distant clouds). The aerial perspective in front of the cloud is a 4-step single-scatter
    segment from the camera altitude (`cloudAerialScatter`; "Lighting/Aerial perspective strength",
    `u_clouds_aerialStrength`, 0-5, default 1: x the ADDED air light only - the cloud's dimming through the air stays
    physical; min 0.01: always on). The sun march runs only where the shadow map
    does not cover the sample (or "Self-shadow from map" is off - the default: the user keeps it off for
    quality; on, it measured march 0.63 -> 0.35 ms). **The sun march** (`cloudLightOpticalDepth`, "Light steps" 4,
    quadratic spacing) runs over a PER-SAMPLE reach: the way out of the sample's OWN layer toward the sun,
    `(layer top - altitude) / L.y`, capped by "Light distance (m)" (default 8000; 2026-09-29 - it was a fixed
    2 km: steps in empty air over a sample near the top, and a base sample stopped short of its tower). It reads each step's base noise at the mip of ITS OWN LENGTH (at least the view sample's) and
    stops past `odCut = 7 / min(msTailExt, ms)` (every multi-scattering octave under 0.1 %): march 0.63 ->
    0.56 ms, sky 0.14 -> 0.10 ms (2026-09-29). Output: in-scatter + transmittance, and
    log2 distances (first hit, transmittance-weighted, march limit).
  * **Temporal** (`cloud_temporal.cs.glsl`): reprojects the weighted cloud distance minus this frame's
    wind displacement, rejects a history whose cloud distance lies OUTSIDE the neighbourhood's range of
    this frame's distances (+-0.2 in log2), clamps its colour to the neighbourhood. **Not a per-pixel
    distance test:** a ray flat through the layer passes clouds at very different distances, so its one
    weighted distance jumps with the step jitter every frame - the per-pixel test rejected the history
    exactly where the raw march is noisiest (diagonal stripes of the old interleaved-gradient jitter; the
    jitter is now a per-pixel hash + golden-ratio step, grain instead of lines where history is missing).
    The history is the OTHER frame slot's accumulation; all images are cleared to "no cloud" at creation, so
    there is no history-valid flag. **The history's march LIMIT must also match this frame's** (±0.15 in
    log2): at a silhouette the bilinear history fetch pulled part of the sky neighbour's cloud into the texel
    over the near surface, the neighbourhood clamp let it through, and the feedback built it up - that texel's
    first-cloud distance is the surface's own, so the upsample kept it: a cloud-coloured outline on every edge
    against clouds, thicker with a higher blend. The checkerboard fill takes its front only from neighbours
    that hold cloud (an empty march stores its limit as the front - the same leak).
    **The opposite leak** (2026-09-29): a texel over a near surface holds "no cloud" (its march stopped under the
    clouds). The checkerboard fill's plain 4-neighbour average and the neighbourhood clamp's range both took it
    into the SKY texels beside it: a clear-sky-coloured outline around every object against clouds. So the fill
    weights its neighbours by how well their march limit matches its own (`1 / (1 + 16·Δ)`, as the upsample), and
    the clamp range takes only neighbours within `LIMIT_MATCH` (0.15 in log2) of the pixel's limit.
    **An ISOLATED fill pixel** (no side neighbour within `LIMIT_MATCH`: a thin leaf against the clouds, or a gap
    in one; 2026-10-12): its 4-neighbour value comes from the wrong surface, and with no matching neighbour the
    clamp range was that value alone - the history was thrown away, and the front was clamped to the leaf's limit,
    so the upsample drew the sky neighbours' cloud ON the leaf every other frame (flicker). Now it takes the
    history whole (last frame marched this pixel), no clamp and no range test (the limit test stays), and its
    front is the neighbours' own cloud front, NOT clamped to its limit: a cloud behind the leaf stays behind it.
  * **Apply** — the depth-aware 4-tap upsample is `cloud_upsample.inc.glsl` (a texel whose first cloud
    lies behind this pixel's surface counts as "no cloud"), used by TWO passes:
    * **Fog ON: the fog apply composites the clouds** (`vol_apply.fs.glsl`, bindings 6/7). Fog laid over
      finished clouds fogged them as if they stood at the scene depth (over far terrain: a grey band over
      every cloud). With the cloud (S, T) at its weighted distance tc and the fog F to the scene, C to tc:
      `out = T·F.rgb + C.a·S + (1−T)·C.rgb + scene·(T·F.a)` — exact for a cloud at one distance and linear
      in the scene, so the blend state is unchanged. `fogTo` (froxel volume + far field) runs twice on
      cloudy pixels - ONCE when the cloud and the scene both lie past the froxel volume and "Fog/Far
      Field/Max distance" (default 40 km, `u_fog_farFieldMaxDistance`; the far field adds no fog past it): both then
      see the same fog.
    * **The fog's sun term is cloud-shadowed everywhere**: the froxels per froxel (`vol_scatter.cs.glsl`), the
      FAR FIELD per sub-segment (`volFarField`: 2 jittered shadow-map taps each, weighted by the sub-segment's
      share of the in-scatter; the apply binds the cloud shadow map at 8). **"Fog/Sun scatter"** (`u_fog_sunScatter`,
      default 1): a non-physical gain on the SUN in-scatter only (froxels, far field, reflection fog) - sunlit fog
      and the shafts brighten, shadowed fog (ambient only) and the extinction do not: strong god rays through thin
      fog without fogging up the world.
    * **The fog's sky-SH ambient is cloud-shadowed too** (2026-10-04): the sky SH is ONE probe for every point,
      and holds the sunlit ground below the horizon (`skyGroundSun`, plus the "Ground Horizon" share
      above it), never cloud-shadowed. `giSkySunIrradiance` (gi_probe.inc.glsl) is that part in closed form, `giEvalSkySHCloud`
      subtracts its shadowed share: the froxels' GI-off ambient (the froxel's cloud tap), `giIrradiance`'s
      out-of-field fallback (its `cloudSunTransmittanceSoft`, so surfaces too), and the far field (weighted by
      `sunW`, no extra taps). The sun cone in the sky (aureole) is not part of it.
    * **"Fog/Shaft haze"** (Density (1/m), default 0.001, min 1e-5: always on; Height (m), default 200; `u_fog_hazeDensity` / `hazeInvHeight`): the fog is a
      HEIGHT fog, nearly gone a few tens of metres up, so shafts from the clouds showed only after cranking the base
      density. The haze is a separate thin medium reaching up to the clouds (its own scale height from the fog's
      height base) that adds SUNLIT in-scatter only (x "Sun scatter"): no extinction, no ambient - non-physical on
      purpose. Froxels: per froxel (`heightFogMean`), its visibility the fog's where the fog computed one, else the
      clouds' alone (terrain shadows skipped above the fog). Far field (`volFarHazeStep`): per sub-segment, with the
      same cloud-visibility taps, behind the fog in front of it and bounded by its OWN extinction (a level ray to
      the horizon stays finite; it does not dim the scene).
    * **Fog OFF: the "Cloud apply" scene stage** (`cloud_apply.fs.glsl`, before the fog apply slot, gated on
      `cloudsEnabled() && !fog`). `colorWriteAlpha = false`.
* **THE CLOUD SHADOW MAP** (a Beer shadow map: `cloud_shadow.cs.glsl` writes, `cloud_shadow.inc.glsl`
  reads). Two sun-aligned ortho cascades in ONE 2-layer RGBA32F array (`SHADOW_RESOLUTION` 1024), recorded
  straight into the primary after the pre-scene stages (before GI, fog and the scene).
  * A texel = one line along the sun through the MAIN layer's band (not the union shell: the upper layer casts
    no map shadow, 2026-09-30): x = the along-light coordinate `dot(p - centre, L)`
    of the first cloud coming from the sun, y = the mean extinction to the last cloud, z = the whole optical
    depth. A receiver's optical depth is `min(y * max(x - a, 0), z)`, so ONE fetch serves the ground, a
    mountain or fog froxel inside a cloud, and the clouds' own samples. Because x is a coordinate and not a
    distance from a start plane, the map needs no per-cascade start distance.
  * **PROGRESSIVE UPDATES, one buffer, no toroidal addressing.** Each frame a cascade renders 1 / split of
    its texels ("Near update split" 1/4, "Far update split" 1/64 since 2026-09-29 - it was 1/16; the far
    cascade did ~2/3 of the samples, and a 64-frame cycle moves the clouds ~11 m at 10 m/s, under a third of
    its ~39 m texel; Cloud shadow 0.175 -> ~0.09 ms): one texel of each 2x2 / 4x4 / 8x8 block, the
    rotating phase's (`shadowTexel` in cloud_shadow.cs.glsl), one thread per rendered texel - a constant cost
    per frame instead of a spike every Nth frame (the old "Far update interval"). Neighbouring texels differ in
    age by a few frames, well under a texel of cloud motion. (Tried and reverted, 2026-10-05: the frozen centre
    travelling WITH the wind, so a texel's age stops showing as cloud motion - no artefact was observed that it
    fixed, and it made a still camera re-centre in wind: one full cascade render every extent / 8 of travel.) That needs ONE texel-to-world mapping for all of
    them, so `buildUboClouds` FREEZES each centre (snapped to whole texels in light space, double, world
    space) and re-centres only when the camera is 1/8 of the extent away (125 m near, 1 km far), or the sun,
    the extent or the enable changed: that frame renders the whole cascade (a rare spike; a CONTINUOUSLY
    moving sun re-renders every frame). The lookup is unchanged: only the frozen centre's camera-relative
    offset is rebuilt each frame. **Steps per cascade** ("Near steps", "Far steps", `u_clouds_shadowNearSteps` /
    `shadowFarSteps`). **So the map is ONE image, not per frame slot.** It is cleared to "no cloud" at creation and
    stays bound while the clouds are off (`u_clouds_shadowRendered = 0` makes the lookup return 1).
  * **THE FAR CASCADE IS FILTERED ON THE RESULT** (`cloudShadowFarODFiltered`): its texels are tens of
    metres, and the OD is non-linear in the stored terms, so the sampler's bilinear blend of (x, y, z) stayed
    blocky. Instead it `texelFetch`es the 2x2 texels, computes each one's transmittance and blends those
    bilinearly (PCF-style), then returns `-log(T)`. **"Far softness"** (`u_clouds_shadowFarSoftness`, texels, default
    1.5) jitters the lookup per pixel and frame (a world-anchored hash: vertex-stage consumers have no
    gl_FragCoord); TAA and the fog's temporal blend average it into a penumbra. The near cascade keeps its
    one hardware-bilinear fetch, and so does `cloudSunTransmittanceSoft` (GI).
    **Only SURFACES take the filter** (`cloudSunTransmittance`: lit core, terrain, ocean, film, decals).
    **`cloudSunTransmittanceBilinear`** (the far cascade's one bilinear fetch) serves what already averages over
    space and time: the fog froxels + shaft haze, the far field's jittered taps (up to 10 per pixel), the clouds'
    air term and particles. Measured 2026-09-29: the far field on the filter cost the fog apply ~0.025 ms.
  * Past the far cascade the transmittance fades to a rough mean from the coverage (not measured).
  * **Consumers** multiply their sun term by `cloudSunTransmittance(worldPos)`: the lit core (lit, masked,
    transparent, terrain; forward set binding 22), the ocean (`sunTint`, so glint, body, foam and SSS), the
    terrain wet film (its own `sunTint` copy: in-scatter, reflection fog, mirror hits; AND its own sun
    visibility tap in main, `g_sunVisSurface` -> `sunSurfaceRadiance()`: glint, whitewater and the bubble
    cloud - both lacked the cloud term until 2026-09-29, and the film's foam glowed brighter than the ocean
    under cloud shadow. `g_sunVisSurface` already HOLDS the cloud and the eclipse, so never multiply it by
    `sunTint`: that squares the cloud shadow - the film's bubbles and foam did, darker than the ocean's, 2026-09-30.
    The tap sits at the WATER SURFACE (`in_pos`, up normal), not the ground under it (`TERRAIN_LIT_POS`,
    ground-normal gate), since 2026-09-30),
    the fog scatter (13 — the shafts through cloud gaps), LIT particles
    (draw set 11, vertex stage), LIT decals (6), and the cloud march's self-shadow (9; outside the map it
    falls back to the sun march).
  * **GI under cloud shadows: dimmed at the LOOKUP, not in the trace.** The trace's gather-hit sun has NO
    cloud shadow (the trace does not bind the map). Cloud-shadowed hits made each probe lag a moving cloud
    shadow by its update interval, so the GI flashed under shadows that fall AWAY from the camera (long
    intervals; a cloud straight overhead did not show it). Instead every probe keeps its **sun DC luminance**
    (probe vec4 `[6].x`, `GI_SUN_V4`: the direct sun at gather hits + the sun share of the multi-bounce +
    the sunlit-ground term `skyGroundSun` of every DOWNWARD MISS (past `rayMax` or the TLAS range: the sky
    below the horizon is a sunlit ground; left out of the sun part, each visit's random downward misses
    added undimmed sun under a cloud shadow - probe flashes at shadow borders, worst with a low sun) + its
    "Ground Horizon" share w on the UPWARD misses, blended at the SH's alpha),
    **A MISS IS THE SKY SH's RADIANCE** (`giSkySHRadiance`, 2026-10-04), not the sky map: a probe in open space
    then gathers exactly what the out-of-field fallback (`giEvalSkySHCloud`) evaluates, cloud dimming included -
    under clouds the probe field was darker than the area past it. From the sky map, upward misses in a cone
    around the sun (15° -> 30°) counted as sun, and the lookup dimmed the aureole under a cloud that the averaged
    GI layer had already counted in; the fallback never did. The miss's sun part is the STEP (G below, w G above),
    not its L1 form (negative toward the zenith for w < 0.2): only the sun's DC is kept, and both have the same.
    Volume mode reads last frame's sky SH (the bake's copy); probe mode reads the slot this dispatch rewrites
    (a frame-to-frame difference only). The bake stores the **sun fraction s × W** in the tail image's `.b` (RGBA16F),
    and **each cascade lookup turns it into the sun's SHARE AT THE NORMAL** (`giSunShare`, 2026-10-04): a probe keeps
    only the sun's DC, so its direction is MODELLED - the sunlit ground at the gather hits, light from the lower
    hemisphere, whose L1 irradiance is its DC term × (1 − n·up). One DC fraction for every normal dimmed the sky
    light on up-facing ground with the ground's sun under a cloud: the field was darker than the sky-SH area past
    it (with the cloud shadows off the two matched). Sun from ABOVE (the "Ground Horizon" share w of the upward
    misses, a sunlit wall overhead) is outside the model. The probe data and the volume images are unchanged.
    `giIrradiance` returns `E × (1 − share (1 − T))` with `T = cloudSunTransmittanceSoft` — the FAR cascade
    only (~8 m texels, one fetch): the bounce comes from tens of metres around, so the soft value fits.
    Indoors s ≈ 0, so a roof is not dimmed by the cloud above it. `evalProbeCoverage` stays UNDIMMED (it
    returns s as well): the trace's multi-bounce needs the undimmed value. The dimming compiles in only
    when the includer has `cloud_shadow.inc.glsl` BEFORE `gi_probe.inc.glsl` and `CLOUD_SHADOWS` is set.
    s is the DC fraction (not per direction) and one scalar for RGB: the warm sun bounce and the blue sky
    part are dimmed alike. **Measured (2026-09-27): no register or local-memory change** in the lit, terrain,
    ocean, decal, particle, fog scatter, trace or bake pipelines.
* **THE SKY CLOUDS** (`cloud_sky.cs.glsl`, `CloudPipeline::recordSky`, in the primary after the shadow map
  and before GI): one invocation per sky-map texel, `cloudRaymarch` with 64 steps, written as (in-scatter,
  transmittance) into a lat-long image on the sky map's own texel grid; the sky-map bake composites it
  into its GI and mirror layers. So the ocean / wet-film reflections, the GI miss rays, the sky SH and the
  forward pass's sky ambient all see the clouds (overcast darkens the ground's sky light). **TEMPORAL**
  (2026-09-29): 64 steps are far too coarse near the horizon (the steps grow to kilometres; the ocean reflects
  mostly those directions), and the samples sit relative to the camera, so a FAST camera slid them through the
  noise and the reflected sky changed colour every frame. Each frame now marches with a new jitter (per-texel
  hash + golden ratio) and blends into the texel's own last value (the image is read-write, cleared to "no
  cloud" at creation). The history weight is FRAME-TIME based: `u_clouds_skyHistory = exp(-3 dt / T)` with T =
  "Sky/Clouds/Quality/Sky map history (s)" (default 1; the GI layer has its own, see TWO CLOUD LAYERS below; 95 % of a change after T seconds at any frame rate; real
  time, so it still converges while the sim is paused; 0 = no history). **A FLOOR under it** (both layers):
  "Sky map min samples" N (default 12; 0 = time only) - the weight is at least (N - 1) / (N + 1), an average of N
  marches. Time alone averaged ~5 marches at 30 fps (~20 at 120): the ocean mirrored the march noise as a per-frame
  flicker at low frame rates (2026-10-05). **PROGRESSIVE:** each frame marches
  ONE texel of every 2x2 block (rotating phase, the shadow map's order; `CloudPipeline::SKY_UPDATE_FRAMES` = 4,
  so the CPU's dt spans 4 frames), and only the UPPER hemisphere is dispatched (rows [0, H/2); the lower half
  stays "no cloud" from the clear). 4096 threads cannot fill the GPU, so the pass is now bound by its longest
  ray's serial chain, not its thread count - so `recordSky` ends WITHOUT a barrier: the sky-map bake's input barrier
  (`GIProbePipeline::recordSkyMap`, compute writes -> compute sampled reads) orders it before its one reader, and the
  GI prep's compute in between (the TLAS instance write) runs alongside it (~0.01 ms, 2026-10-05). **The observer
  stands on the GROUND under the camera** (`ATMOS_OBSERVE_HEIGHT`), not at the camera: every reader of the
  sky map sits under the clouds, even while the camera flies above them. The ambient reads the sky map's
  CLEAR layer from LAST frame (no feedback loop through the clouds' own image). **The clear layer (2) holds per-frame
  CONSTANTS, one texel each** (2026-10-04; `SKY_MAP_*_TEXEL` in atmosphere.inc.glsl), written by invocation (0, 0, 0)
  of the bake, whose dispatch covers only layers 0 and 1: the clouds' ambient (the clear sky over the zenith + four
  directions at 30°, averaged) and the far trees' sky light (the GI layer, see Far trees ([Vegetation.md](Vegetation.md))). The clear layer was a full
  256x128 skyRadiance layer for 5 values, and each reader did 5 fetches per ray; now one `texelFetch`.
  **TWO CLOUD LAYERS: the GI sky is AVERAGED OVER OBSERVERS** (2026-10-04; `CloudPipeline::SKY_LAYERS`, a 2-layer
  array, the dispatch's z). From one observer the cloud overhead and the cloud in front of the sun ARE that
  observer's cloud shadow, and every GI-layer reader (sky SH -> fog / ocean / out-of-field ambient, far trees, GI
  misses) lit the whole world with it: fog, ocean and trees under a cloud brightened when the camera moved into the
  sun. Layer 0 = from under the camera, composited into the MIRROR layer (reflections show the real clouds);
  layer 1 = each march from a new observer in a disc around the camera (R2 over the disc per texel, uniform by
  area: "Sky/Clouds/Quality/GI sky observer radius (m)", default 4 km, `u_clouds_giSkyObserverRadius`), its history the average
  over them, composited into the GI layer. A binary cloud / gap per march needs a longer history: "GI sky history
  (s)" (default 4, `u_clouds_giSkyHistory`).
  **THE GI LAYER IS LOW-RES** (2026-10-04): since the probe misses read the sky SH and the far trees a constant
  texel, its only readers were the sky-SH projection (64 directions + −up) and the GI-off zenith ambient (terrain,
  ocean, film) - yet the clouds' layer 1 marched and the sky map composited the full 256x128 grid. Now both live in
  the `SKY_MAP_GI_WIDTH` x `SKY_MAP_GI_HEIGHT` (64x32) CORNER of their images, on their own lat-long grid (1/16 of
  the marches / `skyRadiance` texels; the rest of the dispatch's threads exit), read with `skyMapGISample`
  (atmosphere.inc.glsl: 4 `texelFetch`es, U wrapped inside the corner) - never `textureLod` + `skyMapUV`. The zenith
  readers take `SKY_MAP_GI_ZENITH_TEXEL` (a constant texel, like the trees'). No new images or bindings. The corner's
  lower half is rewritten as "no cloud" every pass, so a shader reload leaves no full-res texels in it. Measured
  after (RelWithDebInfo sandbox): "Cloud sky" 0.126 ms GPU.
  **THE MIRROR LAYER'S CLEAR SKY IS CACHED** (2026-10-04; layer 3, `SKY_MAP_LAYER_MIRROR_CACHE`, `SKY_MAP_LAYERS` 4):
  the 12-step `mirrorSkyRadiance` changes only with the sun and the sky tweaks, so each frame re-marches ONE texel
  of every 2x2 block (rotating phase, the sky clouds' order) into the cache and composites this frame's clouds over
  the cached value for the rest - 1/4 of the march. A moving sun lags by up to 3 frames. The image is cleared to 0
  at creation; alpha 0 = not baked yet, marched at once. The bake's entry barrier carries the RAW from last frame's
  cache writes; the dispatch stays 2 layers (`SKY_MAP_DISPATCH_LAYERS`).
* **The visible sky is seen from the camera altitude** (`sky.fs.glsl`, `atmosphereScatter`'s observer
  height): the air above thins, and the horizon dips (`cosHorizon`). Below the dipped horizon the march ends
  at the ground, so the pixel is the haze in front of it plus the lit ground through the march's
  transmittance; the sun disc, moon and stars are left out there. The view optical depth is
  `atmosSegmentOD` / `atmosRayBegin` + `atmosRayOD` (atmosphere.inc.glsl; the per-ray form keeps the
  origin's Chapman values, so a march pays one evaluation per step), stable for descending rays (see the cloud haze fix: a "to space"
  depth through the planet is float noise). The sky MAP and every indirect path keep the ground observer.
* **Known limitation:** particles and transparents draw before the apply, so one inside a cloud gets the
  cloud of the opaque pixel behind it — the fog apply has the same limitation.
* **The three toggles are BAKED defines**, injected into every shader compile by `buildLayoutPreamble` from
  `RendererVKLayout::g_cloudShaders` (`Renderer::syncCloudDefines`): `CLOUDS` ("Sky/Clouds/Enabled"),
  `CLOUD_SHADOWS` (+ "Shadows/Enabled"), `CLOUD_SELF_SHADOW_MAP` (+ "Self-shadow from map", off by
  default), `CLOUD_POWDER` ("Lighting/Powder" above 0; the strength stays in the UBO), `CLOUD_CHECKERBOARD`
  ("Quality/Checkerboard"; also sizes the march dispatch, which the reload re-records) and `CLOUD_DEBUG_MODE`
  ("Quality/Debug mode": the march / temporal debug views compile out at 0). A change of a DEFINE waits for
  the GPU and reloads every shader, like the GI grid tweaks; `syncCloudDefines` returns whether one changed,
  so dragging the Powder slider reloads only when it crosses 0.
* **The aerial perspective's distance is NOT the weighted cloud distance:** the loop also sums
  `absorbed * exp(-kAir * t)` (kAir = the ray's mean air extinction), and the haze is taken where the air
  transmittance equals that in-scatter-weighted mean. A flat ray at the camera's altitude holds the fog
  around the camera AND clouds tens of km out; at the weighted distance the near cloud's light was dimmed by
  the far cloud's air - a dark band at the camera's altitude (gone with "Max distance" 20 km, absent in
  "Density only").
* **Register budget** (pipeline stats, 2026-09-27; registers / local bytes): march 56/0, sky clouds 48/16
  (the driver's +-16 B split; a 32k-thread pass),
  temporal 43/0, shadow map 36/0, cloud apply 39/0; the fog apply 48/0 with clouds (44 without). The cloud
  lookup adds nothing to the lit, terrain, ocean, fog scatter, GI trace, particle and decal shaders. Two rules
  came out of it: **the march applies its four light colours AFTER the loop** (the in-scatter is linear in
  them, so the loop sums three scalar weights: 56 -> 48 on the sky pass), and **the fog apply folds the
  cloud part before the scene's fog** (4 live values across the second `fogTo`, not 8: 56 -> 48). What a define
  cannot hold stays a runtime UBO flag: `u_clouds_enabled` = the march ran this frame (the game suppresses
  it), `u_clouds_shadowRendered` = the map was rendered this frame (suppressed, or the sun at the horizon).
* **Debug mode** ("Sky/Clouds/Quality"): step count heat, density only, history rejection.
