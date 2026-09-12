#ifndef OCEAN_WAVE_INC_GLSL
#define OCEAN_WAVE_INC_GLSL

// Sampling helpers for the FFT ocean maps (OceanSimulationPipeline / ocean_*.cs.glsl), per cascade c:
//   layer c                      : displacement (Dx, h, Dz, dDx/dz)
//   layer   OCEAN_CASCADES + c   : gradients    (dh/dx, dh/dz, dDx/dx, dDz/dz)
//   layer 2*OCEAN_CASCADES + c   : slope second moments (dhx^2, dhz^2, accel, turbulence [c=0 only])
// Shared by the displacement passes (instanced_indirect_ocean.vs.glsl, gbuffer.vs.glsl) and the water
// shading (ocean.fs.glsl) so prepass depth, drawn geometry and shaded normals read the same field.
// Requires ubo.inc.glsl; includer may set OCEAN_MAPS_BINDING / TERRAIN_HEIGHT_BINDING first.

#ifndef OCEAN_MAPS_BINDING
#define OCEAN_MAPS_BINDING 7
#endif
layout (binding = OCEAN_MAPS_BINDING) uniform sampler2DArray u_oceanMaps;

#ifdef TERRAIN_HEIGHT_BINDING
#include "terrain_height.inc.glsl"
#endif

// (terrain height, water surface level) at worldXZ; open-ocean bottom / sea level without terrain data.
// Depth derives live as level - height; the clipmap lifts its vertices by level - sea level.
//
// Fades back to open ocean across the outermost cascade's border. The sampler is clamp-to-edge, so
// past the map its border texel extends forever: where that texel is land, the surface weight drops
// to the swash amplitude and the sea reads flat, with a DEAD-STRAIGHT seam running to the horizon (the
// square map edge in perspective). Beyond the data the world model is open sea - which is what the
// horizon band exists to draw - so blend to it rather than trusting the clamp.
vec2 oceanSampleShoreData(vec2 worldXZ)
{
    float height = u_oceanParams2.w - u_oceanParams1.z; // open-ocean bottom: sea level - depth D
    float level = u_oceanParams2.w;                     // sea level
#ifdef TERRAIN_HEIGHT_BINDING
    if (terrainHeightMapPresent())
    {
        const float invOuter = u_fogParams5.z > 0.0 ? u_fogParams5.z : u_fogParams3.y; // far cascade, else near
        const vec2 uv = abs((worldXZ - u_fogParams5.xy) * invOuter); // 0.5 = the map's edge
        const float w = 1.0 - smoothstep(0.40, 0.49, max(uv.x, uv.y));
        if (w > 0.0)
        {
            const vec4 td = terrainDataAt(worldXZ); // .x = terrain height, .y = water level
            height = mix(height, td.x, w);
            level = mix(level, td.y, w);
        }
    }
#endif
    return vec2(height, level);
}

// Water depth (m) at worldXZ: local water surface minus terrain.
float oceanSampleShoreDepth(vec2 worldXZ)
{
    const vec2 hw = oceanSampleShoreData(worldXZ);
    return hw.y - hw.x;
}

// Per-texel wave-travel rotation. DISABLED (identity): rotating the sample domain pivots on the world
// origin, so the sea creases along every 8-bit angle contour - the baked flow steers the SIMULATION
// wind instead (OceanGenerator::steeredWindAngle). If ever revived: every wave-sampling pass must
// apply the identical rotation, and the field TRAVELS AGAINST u_oceanParams0.xy.
vec2 oceanFlowRotation(vec2 worldXZ)
{
    return vec2(1.0, 0.0);
}

// Rotate the FFT sampling position by -delta / a resulting horizontal vector back into world space.
vec2 oceanFlowSamplePos(vec2 worldXZ, vec2 fr) { return vec2(fr.x * worldXZ.x + fr.y * worldXZ.y, -fr.y * worldXZ.x + fr.x * worldXZ.y); }
vec2 oceanFlowToWorld(vec2 v, vec2 fr) { return vec2(fr.x * v.x - fr.y * v.y, fr.y * v.x + fr.x * v.y); }

// Land cull: true when the whole triangle footprint is buried under land - the VS then emits a NaN
// position, discarding the primitives. Decided by the vertex's own shoreHW alone (no fetches): safety
// comes from the burial requirement, not sampling density - near-cascade footprints must be buried
// deeper than their own radius (a 45-degree slope bound: smoother terrain can't reach water anywhere a
// co-triangle vertex sits), far/blend-band data uses the flat "Far cull error (m)" (0 = no far cull;
// mistakes out there are sub-pixel). Cliffs steeper than 45 degrees can clip a sliver at their base.
// Both displacement passes MUST apply the same test or their geometry diverges.
bool oceanVertexCulled(vec2 worldXZ, float cellSize, vec2 shoreHW)
{
    const float margin = u_oceanParams7.x;
    if (margin <= 0.0)
        return false;
    const float reach = 3.0 * cellSize; // after the CDLOD morph no co-triangle vertex lies further away

    // Never cull past the streamed terrain mesh (u_terrainParams.x): the cull is only invisible while
    // a rendered mesh stands above the water - the clamp-to-edge-extended bakes report "land" forever.
    if (distance(worldXZ, u_viewPos.xz) + reach >= u_terrainParams.x)
        return false;

    float err = -1.0; // burial slack on top of margin + swash reach
#ifdef TERRAIN_HEIGHT_BINDING
    if (terrainHeightMapPresent())
    {
        const vec2 rel = worldXZ - u_fogParams5.xy;
        const float cheb = max(abs(rel.x), abs(rel.y));
        const float invNear = u_fogParams3.y;
        if ((cheb + reach) * invNear < 0.42) // full-weight near region (blend starts at 0.42)
            err = reach + 0.5 / (float(textureSize(u_terrainHeight, 0).x) * invNear);
        else
        {
            const float invFar = u_fogParams5.z;
            if (u_oceanParams6.x <= 0.0 || invFar <= 0.0 || (cheb + reach) * invFar >= 0.48)
                return false; // far cull off / footprint reaches the far map's clamp-to-edge border
            err = u_oceanParams6.x;
        }
    }
#endif
    if (err < 0.0)
        return false; // no baked terrain data: open-ocean fallback, always water

    // Swash reach (u_oceanParams7.w) keeps the wet band above the waterline alive.
    return shoreHW.y - shoreHW.x < -(margin + u_oceanParams7.w + err);
}

// The water depth the WAVES use: the baked depth, floored at "Ocean/Shore/Horizon depth" once past
// "Horizon depth range" (0 = off, always take the map literally).
//
// At range the depth field cannot be trusted to be deep enough. The far cascade point-samples a
// coarse surface, its texels average shore slopes into the water, TerrainGenV3 returns height ==
// water level (depth EXACTLY 0) for any sample it could not resolve, and the vertical scale
// compresses real shelves by metersPerPixel/30 - every one of those errs SHALLOW. That is the
// dangerous direction: depth drives the shoal fade, which scales the wave slopes and (as fade^2) the
// LEAN variance standing in for filtered-out slopes, so a shallow reading strips the surface of
// microfacet roughness, collapses alpha to its 0.02 floor and mirror-reflects the sky - pixel for
// pixel what wind 0 looks like. Reading too DEEP out there costs nothing visible.
//
// Only the assumed SEABED moves, never the surface, so this cannot put water over land. The land cull
// deliberately keeps reading the RAW depth: culling is about what is buried, not about what the waves
// should look like.
float oceanEffectiveDepth(vec2 worldXZ, float depth)
{
    const float range = u_oceanParams4.x;
    if (range <= 0.0)
        return depth;
    const float t = smoothstep(range * 0.5, range, distance(worldXZ, u_viewPos.xz));
    return max(depth, u_oceanParams3.y * t);
}

// Vertex displacement mip: matches the ring's FIXED cell size (Nyquist band-limit, no camera-coupled
// morphing); `morph` blends +1 across the CDLOD boundary so adjacent rings meet exactly.
// Both displacement passes MUST use this same function.
float oceanVertexLod(float cellSize, float morph, float patchSize)
{
    return max(log2(max(cellSize, 1e-3) * float(OCEAN_FFT_SIZE) / patchSize) + morph + u_oceanParams3.w, 0.0);
}

// --- The shore, in three weights. depth = calm water level above the ground (negative = land height).
// The CPU buoyancy mirror (OceanGenerator) and underwaterLiveWaveY (underwater_light.inc) repeat them;
// the terrain's water film (instanced_indirect_terrain.fs) repeats the surface weight.

// Approach fade: 0 offshore, 1 at the waterline, over the wider of two swash reaches and "Shoal depth
// scale" x the mid cascade's patch size. The band across which open water becomes the shore.
float oceanSwashFadeIn(float depth)
{
    const float reach = max(u_oceanParams7.w, 0.01);
    return 1.0 - smoothstep(0.0, max(2.0 * reach, u_oceanParams4.z * u_oceanParams2.y), depth);
}

// Swash base: the fraction of the raw wave field that runs up the beach - "Swash amplitude" x the
// sea-connection fade (landlocked water, its baked level off sea level, gets none: no swell reaches it)
// x the land-height fade (dies one reach above the level).
float oceanSwashBase(float depth, float waterLevel)
{
    const float amp = u_oceanParams7.z;
    if (amp <= 0.0)
        return 0.0;
    const float seaFade = 1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_oceanParams2.w));
    const float reach = max(u_oceanParams7.w, 0.01);
    const float landFade = clamp(1.0 + min(depth, 0.0) / reach, 0.0, 1.0);
    return amp * seaFade * landFade;
}

// Swash weight: the base, faded in across the approach band - what gates the TONGUE behaviours (the
// backflow), which belong to the shore alone.
float oceanSwashWeight(float depth, float waterLevel)
{
    return oceanSwashBase(depth, waterLevel) * oceanSwashFadeIn(depth);
}

// THE surface weight: what fraction of the RAW cascade sum stands at this depth,
//     w = 1 - fadeIn * (1 - swashBase)
// 1 in open water, easing to the swash base across the approach band. ONE weight for every cascade, so
// the spectral detail is preserved all the way in (per-cascade fades stepped the chop down across a
// band a few texels wide - a visible line at that depth). Used identically by the displacement, the
// shading normal and the vertex normal.
float oceanSurfaceWeight(float depth, float waterLevel)
{
    return 1.0 - oceanSwashFadeIn(depth) * (1.0 - oceanSwashBase(depth, waterLevel));
}

// Cascade displacement sum at an undisplaced (morphed) world XZ. Choppy lambda applied here so it
// stays live (the maps store raw Dx/Dz). shoreHW = the vertex's (terrain height, water level),
// fetched once by the caller. The CPU buoyancy mirror (OceanGenerator::sampleDisplacement) MUST
// match this function change for change.
vec3 oceanSampleDisplacement(vec2 worldXZ, float cellSize, float morph, vec2 shoreHW)
{
    const float chop = u_oceanParams0.w;
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    vec3 disp = vec3(0.0);
    float sw = 0.0;
    // Buried deeper than the swash band: the surface weight is zero - skip the fetches (bit-identical).
    if (depth > -u_oceanParams7.w)
    {
        const vec2 fr = oceanFlowRotation(worldXZ);
        const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
        float rawY = 0.0;
        vec2 rawXZ = vec2(0.0);
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_oceanParams2[c];
            const vec4 d = textureLod(u_oceanMaps, vec3(sampleXZ / L, float(c)), oceanVertexLod(cellSize, morph, L));
            rawY += d.y;
            rawXZ += d.xz;
        }
        // The whole raw field, scaled by ONE depth weight (oceanSurfaceWeight): 1 in open water, easing
        // to the swash amplitude across the approach band. Every cascade is scaled alike, so the
        // spectral detail is preserved all the way in.
        // (oceanSurfaceWeight and oceanSwashWeight, with their shared base and fade-in evaluated once.)
        const float base = oceanSwashBase(depth, shoreHW.y);
        const float fadeIn = oceanSwashFadeIn(depth);
        disp = vec3(rawXZ.x * chop, rawY, rawXZ.y * chop) * (1.0 - fadeIn * (1.0 - base));
        sw = base * fadeIn;
        // Swash backflow: the raw chop slides the tongue seaward as the wave recedes. Gated by the
        // tongue's thickness above the sand (a buried surface must not keep sliding), soft-capped to
        // ~the swash reach (the raw offset is unbounded and would shear triangles into streaks).
        const float flowFade = smoothstep(0.0, 0.35, rawY * sw + depth);
        vec2 flowOff = rawXZ * (chop * u_oceanParams8.z * sw * flowFade);
        const float flowCap = clamp(0.5 * u_oceanParams7.w, 0.25, 1.0);
        flowOff *= flowCap / (flowCap + length(flowOff));
        disp.xz += flowOff;
        disp.xz = oceanFlowToWorld(disp.xz, fr);
    }
    // No waterline floor: the surface is the wave, and the DEPTH BUFFER cuts it against the sand per
    // pixel - a trough that dips under the seabed exposes (wet) sand, which is what a receding swash
    // looks like.
    return disp;
}

// Combined surface data for the water fragment shader (implicit-LOD: the mip chain prefilters
// distant slopes):
//   slope    : chop-corrected slope (Tessendorf: grad h / (1 + lambda dD))
//   jacobian : horizontal fold J (< ~0.5 = folding crest)
//   slopeVar : LEAN term (Bruneton 2010) - slope variance lost to mip filtering, returned as
//              microfacet roughness (the elongated sun glitter at distance)
//   accel    : vertical acceleration (breaking-crest foam driver)
//   shoreHW  : the (terrain height, water level) fetch, returned for the caller to reuse
void oceanSampleSurface(vec2 worldXZ, out vec2 slope, out float jacobian, out vec2 slopeVar, out float accel, out vec2 shoreHW)
{
    const float chop = u_oceanParams0.w;
    shoreHW = oceanSampleShoreData(worldXZ);
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    // Buried under land: flat calm surface (with swash on, the run-up band still shades).
    if (depth <= (u_oceanParams7.z > 0.0 ? -u_oceanParams7.w : 0.0))
    {
        slope = vec2(0.0);
        jacobian = 1.0;
        slopeVar = vec2(0.0);
        accel = 0.0;
        return;
    }
    const vec2 fr = oceanFlowRotation(worldXZ);
    const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
    vec2 slopeSum = vec2(0.0);
    vec2 varSum = vec2(0.0);
    float sxx = 0.0, szz = 0.0, sxz = 0.0;
    accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const vec2 uv = sampleXZ / u_oceanParams2[c];
        const vec4 g = texture(u_oceanMaps, vec3(uv, float(OCEAN_CASCADES + c)));
        const vec4 d = texture(u_oceanMaps, vec3(uv, float(c)));
        const vec4 m = texture(u_oceanMaps, vec3(uv, float(2 * OCEAN_CASCADES + c)));
        slopeSum += g.xy;
        varSum += max(m.xy - g.xy * g.xy, vec2(0.0));
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += m.z;
    }
    // The one depth weight the displacement used (oceanSurfaceWeight), so shading tracks the geometry:
    // slopes and the Jacobian scale with the height, the variance with its square.
    const float w = oceanSurfaceWeight(depth, shoreHW.y);
    slopeSum *= w;
    varSum *= w * w;
    sxx *= w; szz *= w; sxz *= w;
    accel *= w;
    const float jxx = 1.0 + chop * sxx;
    const float jzz = 1.0 + chop * szz;
    const float jxz = chop * sxz;
    jacobian = jxx * jzz - jxz * jxz;
    // Floor + rational soft limit: near folds the raw division explodes the slope into dark creases.
    slope = slopeSum / max(vec2(jxx, jzz), vec2(0.6));
    slope /= 1.0 + 0.2 * length(slope);
    slope = oceanFlowToWorld(slope, fr);
    slopeVar = varSum;
}

// Instant crest foam from the fold Jacobian + downward acceleration (Longuet-Higgins). ONE function
// shared by the water shader (display, biasBoost = turbulence * "Foam boost") and ocean_foam.cs.glsl
// (injection, biasBoost = 0 - only genuine breaking adds energy, no feedback loop).
float oceanInstantFoam(float jacobian, float accel, float biasBoost)
{
    const float softness = max(u_oceanParams4.w, 0.02);
    const float bias = u_oceanFoam.w + biasBoost;
    const float fold = 1.0 - smoothstep(bias - softness, bias, jacobian);
    const float breaking = smoothstep(u_oceanParams5.w, u_oceanParams5.w + softness, -accel / 9.81);
    return max(fold, breaking);
}

// Accumulated turbulence (moments[c0].w): the decaying memory of breaking. Magnified hard near the
// camera, so mip 0 gets a 3x3 tent of bilinear taps (reconstruction AA); crossfades to plain
// trilinear once the pixel footprint reaches mip 1. footprint = world size of one pixel.
float oceanSampleTurbulence(vec2 worldXZ, float footprint)
{
    const float L0 = u_oceanParams2.x;
    const float layer = float(2 * OCEAN_CASCADES);
    const float size = float(OCEAN_FFT_SIZE);

    const float lod = log2(max(footprint * size / L0, 1e-3));
    const float trilinear = texture(u_oceanMaps, vec3(worldXZ / L0, layer)).w;
    if (lod >= 1.0)
        return trilinear;

    const vec2 uv = worldXZ / L0;
    const vec2 spacing = vec2(1.0 / size);
    float filtered = 0.0;
    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            const float w = float((2 - abs(i)) * (2 - abs(j)));
            filtered += textureLod(u_oceanMaps, vec3(uv + vec2(float(i), float(j)) * spacing, layer), 0.0).w * w;
        }
    }
    filtered *= 1.0 / 16.0;

    return mix(filtered, trilinear, clamp(lod, 0.0, 1.0));
}

// Vertex-shader shading normal with explicit LOD (the G-buffer prepass writes this as the reference
// normal). shoreHW = the vertex's (terrain height, water level), fetched once by the caller.
vec3 oceanSampleNormalLod(vec2 worldXZ, float cellSize, float morph, vec2 shoreHW)
{
    const float chop = u_oceanParams0.w;
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    if (depth <= 0.0)
        return vec3(0.0, 1.0, 0.0); // buried under land: flat
    const vec2 fr = oceanFlowRotation(worldXZ);
    const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
    vec2 slopeSum = vec2(0.0);
    vec2 sxx_szz = vec2(0.0);
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_oceanParams2[c];
        const vec4 g = textureLod(u_oceanMaps, vec3(sampleXZ / L, float(OCEAN_CASCADES + c)), oceanVertexLod(cellSize, morph, L));
        slopeSum += g.xy;
        sxx_szz += g.zw;
    }
    const float w = oceanSurfaceWeight(depth, shoreHW.y); // the displacement's depth weight
    slopeSum *= w;
    sxx_szz *= w;
    vec2 slope = slopeSum / max(vec2(1.0) + chop * sxx_szz, vec2(0.6));
    slope /= 1.0 + 0.2 * length(slope); // same fold-over soft limit as oceanSampleSurface
    slope = oceanFlowToWorld(slope, fr);
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

#endif
