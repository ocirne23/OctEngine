#ifndef UBO_INC_GLSL
#define UBO_INC_GLSL

#ifndef UBO_BINDING
#define UBO_BINDING 0
#endif

// NUM_SHADOW_CASCADES is injected by the engine from RendererVKLayout (Layout.ixx)

#define VIEW_CENTER 0 // shared passes + desktop; the eyes are 1 (left) and 2 (right) in VR

// One camera view's matrices grouped contiguously (AoS), matching RendererVKLayout::ViewData. u_views[0] =
// centre/combined view (the only view on desktop; in VR sized to the union of both eyes' FOV), [1] = left
// eye, [2] = right eye (VR only). Selected by g_viewIndex (see below + the u_mvp/... convenience macros).
struct ViewData
{
    mat4 mvp;
    mat4 invMvp;     // inverse(mvp): reconstruct world pos from depth + screen uv
    mat4 prevMvp;    // previous frame's mvp: reproject world pos to last frame's screen
    mat4 prevInvMvp; // previous frame's inverse(mvp): reconstruct last frame's world pos
    mat4 reprojClip; // prevMvp * inverse(mvp) fused in double precision on the CPU: current NDC + depth
                     // -> last frame's clip, precision independent of the camera's world position
    vec4 viewPos;    // xyz = world position
};

layout (binding = UBO_BINDING, std140) uniform UBO
{
    ViewData u_views[3];
    vec4 u_frustumPlanes[6];
    vec3 u_viewPad;        // padding (centre viewPos lives in u_views[]); keeps u_betaMie 16-byte aligned
    float u_betaMie;       // Mie scattering coefficient at sea level (1/m), drives sky + indirect sky light

    vec3 u_sunDirection;   // xyz = normalized direction towards the sun, w unused
    float u_sunAngularCos; // cos of the sun disc radius (1 = point, smaller = bigger disc)
    vec3 u_sunColor;       // rgb = color * intensity (sun irradiance; also sources atmosphere scattering)
    float u_sunGlow;       // glow falloff exponent (0 = no glow); larger = tighter

    vec3  u_betaRayleigh;  // Rayleigh scattering coefficients at sea level (1/m), drives sky + indirect
    float u_rolloffKnee;   // sky highlight roll-off: luminance where compression starts (at full roll-off)
    vec3  u_skyRadianceColor; // directional atmosphere light (moonlight/space light) radiance, along u_skyUp; GI-only
    float u_rtSkyRadiance;    // > 0.5: one sky-visibility ray per GI probe gates the sky radiance injection
    vec3  u_ambientColor;  // flat, non-physical minimum ambient radiance (applied once at final shading)
    uint  u_frameIndex;    // monotonic frame counter (RNG / temporal-rotation source)
    vec3  u_skyUp;         // sky "up" axis (normalized); need not be world +Y (e.g. planet surface normal)
    float u_rtSunShadow;   // > 0.5: ray-traced sun shadows instead of PCSS cascades

    mat4 u_cascadeViewProj[NUM_SHADOW_CASCADES];
    vec3 u_shadowParams; // x = depth bias, y = normal bias (texels), z = 1/resolution
    float u_sunShadowRays; // RT sun shadow rays per pixel (1 = single jittered ray)

    float u_rtLightShadows; // > 0.5: ray-traced shadows for punctual/area/tube lights
    float u_timeSeconds;    // elapsed app time (cloud wind / sky animation)
    float u_cloudCoverage;  // 0 = clear sky, 1 = overcast
    float u_cloudThickness; // cloud slab thickness (m)

    vec4 u_cloudParams0;    // x = layer height (m), y = noise scale, z = wind speed (noise units/s), w = wind angle (rad)
    vec4 u_cloudParams1;    // x = edge softness, y = sun shading strength, z = silver lining, w = unused
    vec4 u_cloudParams2;    // x = density (extinction), y = sharpness, z = base/top height variation, w = moon brightness
    vec4 u_skySunParams;    // x = atmosphere scatter boost (in-scatter only), y = Mie anisotropy g, z = sky highlight roll-off, w = star density

    vec4 u_screenSize;   // xy = full render-target resolution (px); zw = 1/xy
    vec4 u_viewportRect; // xy = viewport min, zw = viewport size, normalized to [0,1] of the full render target
    vec4 u_taaJitter;    // xy = this frame's TAA sub-pixel jitter in NDC, zw = LAST frame's. ALL raster
                         // passes apply xy in clip space (prepass included: the forward early-Z tests its
                         // depth directly, read-only); mvp/invMvp/prevMvp stay unjittered and geometric
                         // consumers of sampled depth compensate via taaJitterUv (shared.inc.glsl)

    // Volumetric fog (packing documented in vol_fog.inc.glsl)
    vec4 u_fogParams0;   // x = global density (1/m), y = height base, z = height falloff (1/m), w = range (m)
    vec4 u_fogParams1;   // rgb = fog albedo * intensity (> 1 = non-physical gain), w = phase anisotropy g
    vec4 u_fogParams2;   // x = noise scale (1/m), y = noise strength, z = wind speed (m/s), w = temporal blend
    vec4 u_fogParams3;   // x = terrain follow fraction, y = 1 / near terrain map world size (0 = no map), z = enabled, w = light shadow rays
    vec4 u_fogParams4;   // x = sun shadow rays, y = spatial filter, z = GI ambient, w = sun shadow softness (rad)
    vec4 u_fogParams5;   // xy = terrain height map world center XZ (shared by both cascades), z = 1 / far cascade world size (0 = near only), w = baked terrain sea level
    vec4 u_fogParams6;   // x = slice power (Z distribution exponent), y = terrain shadow distance (m),
                         // z = regional fog strength, w = underwater density multiplier
    vec4 u_fogParams7;   // x = underwater sun in-scatter gain (fog light shafts; 1 = physical),
                         // y = waterline band half-height gating the fog's FFT wave taps (m;
                         // 0 = ocean off; sized from the readback trough estimate),
                         // z = underwater caustic strength (0 = off), w = caustic depth fade (1/m)
    vec4 u_fogParams8;   // x = underwater fog boundary offset off the local water surface (m),
                         // y = caustic shore fade depth (m; contrast ramps in over this much water, 0 = off), zw unused
    vec4 u_fogParams9;   // far field (past the froxel volume; vol_apply): x = enabled, y = density scale,
                         // z = multiplier on the near field's height falloff (fogParams0.z), w = ground samples

    vec4 u_moonParams;   // xyz = normalized direction towards the moon, w = cos of the moon disc radius

    vec4 u_starParams;   // x = star size, y = size variation, z = brightness, w = color variation
    vec4 u_nebulaParams; // x = intensity, y = noise scale, z = band width, w = dust lane strength
    vec4 u_nebulaAxis;   // xyz = normalized milky-way band pole (band lies on its great circle), w unused

    vec4 u_eclipseParams; // x = visible sun fraction (solar eclipse; u_sunColor is pre-multiplied by it,
                          // the sky divides it back out for the unoccluded disc/corona),
                          // y = sky highlight roll-off headroom (stops the shoulder absorbs), zw unused

    vec4 u_atmosParams;   // x = Rayleigh scale height (m), y = Mie scale height (m), z = Mie extinction ratio, w = ozone strength

    vec4 u_groundParams;  // rgb = ground albedo * intensity (sky-sphere ground plane + the skyRadiance
                          // ground-bounce tint for downward GI/fog rays), w = horizon terrain fraction
                          // (virtual sky probe projection: fraction of above-horizon sky treated as ground)
    vec4 u_aoParams;      // x = RTAO enabled (0/1), y = GI strength, z = RTAO max distance (m; past it the
                          // AO image is exactly (N, 1) so the upsample is skipped; 0 = no falloff), w unused
    vec4 u_giVisParams;   // x = Chebyshev variance floor (fraction of spacing), y = Chebyshev power, z = probe weight floor, w = mean scale (footprint widening)

    // Ocean (FFT/Tessendorf water; ocean_*.cs.glsl simulation + ocean.fs.glsl shading)
    vec4 u_oceanParams0;    // xy = wind direction (unit), z = spectrum amplitude scale, w = choppiness lambda
    vec4 u_oceanParams1;    // x = wind speed U10 (m/s), y = fetch (m), z = ocean depth D (m), w = normal strength
    vec4 u_oceanParams2;    // xyz = cascade FFT patch sizes L0/L1/L2 (m), w = sea level (world Y)
    vec4 u_oceanAbsorption; // rgb = water extinction sigma_t (1/m, Beer-Lambert), w = perceptual roughness
    vec4 u_oceanScatter;    // rgb = in-scatter albedo color, w = scatter intensity
    vec4 u_oceanFoam;       // rgb = foam albedo, w = Jacobian foam bias (higher = more whitecaps)
    vec4 u_oceanParams3;    // x = horizon band level offset (m, usually negative: sinks only the
                            // horizon band so it stays under distant near-sea-level terrain),
                            // y = horizon depth (m): minimum water depth the waves assume past
                            // u_oceanParams4.x (only the seabed moves, not the surface),
                            // z = turbulence decay per frame,
                            // w = vertex displacement mip bias (Detail bias; ring cell size rides per vertex)
    vec4 u_oceanParams4;    // x = horizon depth RANGE (m): camera distance past which the waves assume
                            // at least u_oceanParams3.y of water, whatever the map says (0 = literal),
                            // y = turbulence spread (diffusion/frame),
                            // z = shoal depth scale (waves fade below depth = scale * cascade patch size),
                            // w = instant-foam edge width (both thresholds' smoothstep)
    vec4 u_oceanParams5;    // x = foam boost (turbulence -> fold-threshold relaxation),
                            // y = turbidity (entrained-bubble milkiness + roughness),
                            // z = shore foam depth (m; surf band width at the waterline, 0 = off),
                            // w = breaking-crest foam threshold (downward crest accel in g units)
    vec4 u_oceanParams6;    // x = far-cascade land-cull error allowance (m; flat burial slack the cull
                            //     demands when only far terrain data covers the footprint, 0 = never cull
                            //     from far data — moved here from the removed params10),
                            // y = glint variance filter scale (spec AA + LEAN roughness),
                            // z = crest SSS strength (0 = off), w = crest SSS forward-lobe power
    vec4 u_oceanParams7;    // x = land cull margin (m): clipmap triangles whose whole footprint is
                            // buried deeper than this under the local water level are VS-culled
                            // (oceanVertexCulled; 0 = off),
                            // y = shore foam max coverage (surf band opacity cap),
                            // z = swash amplitude (scale on the un-shoaled wave height running up the
                            // beach, 0 = off), w = swash reach (m; CPU estimate of max run-up height —
                            // sizes the land sampling band and relaxes the vertex cull)
    vec4 u_oceanParams8;    // x = swash drawdown burial (m below the seabed a receding surface sinks;
                            // deeper = steeper, cleaner cut against the sand),
                            // y = shore foam threshold bias (shifts the surf fold threshold:
                            // negative = sparser/more transparent surf),
                            // z = swash backflow (scale on the raw horizontal chop riding the swash
                            // weight: the tongue flows back seaward as the wave recedes),
                            // w = RT ray cutoff distance (m from the camera; beyond it the water shader
                            // traces no scene rays at all, 0 = unlimited)
    vec4 u_oceanParams9;    // x = trough margin (m): how far above the seabed the wave trough is held,
                            // tapered in with depth so the waterline itself does not lift,
                            // y = RT refraction ray range (m: underwater visibility of traced geometry),
                            // z = RT reflection ray range (m),
                            // w = RT reflection roughness cutoff (rougher pixels skip the mirror ray)
    vec4 u_oceanParams10;   // x = wave height limit as a fraction of water depth (breaking limit;
                            // scales the shoaled cascade sum, 0 = unbounded), yzw unused
    vec4 u_terrainParams;   // x = streamed terrain mesh coverage radius (m, radial from camera XZ;
                            // 0 = no terrain mesh up — fences the ocean land cull),
                            // y = temperature lapse rate, C per WORLD metre above sea level (<= 0; pairs
                            //     with the climate pack's baked sea-level baseline — terrainTemperatureAt),
                            // z = sea level (world Y, live from the streamer), w unused
    vec4 u_terrainShadowParams; // long-range sun shadows marched off the terrain height cascades, for the
                            // ground the cascades and the TLAS cannot reach:
                            // x = camera distance where the march fades in (m; 0 = off),
                            // y = first sample distance (m) = self-shadow bias vs the map's texels,
                            // z = penumbra growth per metre along the sun ray, w unused

    // TERRAIN variant texture splatting (keep in sync with RendererVKLayout::Ubo). Materials are
    // contiguous, in the order the shader composites them bottom-up:
    //   [base .. +numGround) climate-blended ground, [.. +numRock) bedrock exposed by slope/crag,
    //   then the optional beach entry, then the optional snow entry (always last).
    vec4 u_terrainTexParams0; // x = base material idx (< 0 = no texture set: flat-color fallback),
                              // y = ground entry count, z = rock entry count,
                              // w = climate kernel sigma (Gaussian falloff OUTSIDE a climate box)
    vec4 u_terrainTexParams1; // x = ground uv scale (1/m), y = rock uv scale (1/m),
                              // z = slope where rock fades in, w = slope where rock is full
    vec4 u_terrainTexParams2; // x = crag relief start (m above macro altitude), y = crag relief full,
                              // z = beach band height (m above water level), w = snow uv scale (1/m)
    vec4 u_terrainTexParams3; // x = beach entry present (0/1; index base + numGround + numRock),
                              // y = snow entry present (0/1; the LAST material, after the beach),
                              // z = temperature (C) at/below which snow cover is complete,
                              // w = temperature (C) at/above which there is no snow
    vec4 u_terrainTexParams4; // x = slope where snow starts sliding off, y = slope where none remains,
                              // z = humidity at/below which cold ground stays bare (polar desert), w unused
    vec4 u_terrainTexParams5; // x = crag wander amplitude (m; 0 = off), y = crag wander frequency (1/m),
                              // zw unused
    vec4 u_terrainSplatClimate[MAX_TERRAIN_SPLAT_MATERIALS]; // ground/rock CLIMATE BOX: xy = t01 range,
                              // zw = h01 range. Weight is 1 inside and Gaussian-decays outside, so a full
                              // 0..1 range on an axis means "this axis does not matter for this entry".
                              // Unused for the beach/snow overlays.

    // GPU mesh LOD selection (indirect + shadow cull; keep in sync with RendererVKLayout::Ubo)
    vec4 u_lodParams0; // x = screen-space error threshold (px, bias pre-applied), y = hysteresis band,
                       // z = fallback full-res pixels (authored chains), w = mipPixelScale (px per unit/dist)
    vec4 u_lodParams1; // x = force LOD level (< 0 = off), y = fallback-metric level bias, z = enabled (0/1), w unused

    // Forcefield bubbles (keep in sync with RendererVKLayout::Ubo; force_field.inc.glsl consumes these)
    vec4 u_forceTeamColors[MAX_FORCE_TEAMS]; // rgb = linear team color, w unused
    vec4 u_forceParams0; // x = iso threshold, y = rim power, z = rim intensity, w = shell base alpha
    vec4 u_forceParams1; // x = contact glow intensity, y = contact glow width (opposing/own ratio band),
                         // z = geometry glow distance (m), w = march steps
    vec4 u_forceParams2; // x = pattern scale (1/m), y = pattern scroll speed, z = pattern intensity,
                         // w = unused (the force-gain readback scale moved CPU-side)
    vec4 u_forceParams3; // x = interior alpha (shell opacity floor seen from inside),
                         // y = backface alpha (far/inner surface visibility from outside),
                         // z = contact wall alpha (interior equilibrium pane),
                         // w = junction smoothing (smooth-max width as a fraction of iso)
    vec4 u_forceParams4; // x = density debug view (0/1: heatmap of peak field along the ray),
                         // y = density range (field value mapping to white),
                         // z = ambient field slope (strength/m; <= 0 disables), w = ambient team
    vec4 u_forceParams5; // ambient field: xy = planar world center, z = safe radius (m), w = max strength
};

// View index selecting which u_views[] entry the convenience macros / reconstruction helpers read. Defaults
// to VIEW_CENTER (0): shared world-space passes and the whole desktop path leave it there. The per-eye
// screen-space passes set it once at the top of main() from a push constant (1 = left eye, 2 = right eye).
int g_viewIndex = 0;

// Convenience accessors so call sites read the current view's matrices without spelling out u_views[..].
// Shared passes get the centre view for free (g_viewIndex == VIEW_CENTER). A pass that needs a specific
// view regardless of g_viewIndex (e.g. fog apply sampling the centre-built froxel volume) must index
// u_views[] explicitly instead of using these.
#define u_mvp        u_views[g_viewIndex].mvp
#define u_invMvp     u_views[g_viewIndex].invMvp
#define u_prevMvp    u_views[g_viewIndex].prevMvp
#define u_prevInvMvp u_views[g_viewIndex].prevInvMvp
#define u_reprojClip u_views[g_viewIndex].reprojClip
#define u_viewPos    (u_views[g_viewIndex].viewPos.xyz)

#endif