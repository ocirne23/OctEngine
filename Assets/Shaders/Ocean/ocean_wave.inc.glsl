#ifndef OCEAN_WAVE_INC_GLSL
#define OCEAN_WAVE_INC_GLSL

// Sampling helpers for the FFT ocean maps (OceanSimulationPipeline / ocean_*.cs.glsl), per cascade c:
//   layer c                      : displacement (Dx, h, Dz, dDx/dz)
//   layer   OCEAN_CASCADES + c   : gradients    (dh/dx, dh/dz, dDx/dx, dDz/dz)
//   layer 2*OCEAN_CASCADES + c   : slope second moments (dhx^2, dhz^2, accel, -)
//   layer 3*OCEAN_CASCADES + l   : the world-space foam field's level l (ocean_foam_field.inc.glsl)
// Shared by the displacement pass (instanced_indirect_ocean.vs.glsl) and the water shading
// (ocean.fs.glsl) so the drawn geometry and the shaded normals read the same field.
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
    float height = u_ocean_seaLevel - u_ocean_depth; // open-ocean bottom: sea level - depth D
    float level = u_ocean_seaLevel;                  // sea level
#ifdef TERRAIN_HEIGHT_BINDING
    if (terrainHeightMapPresent())
    {
        const float invOuter = u_terrain_mapInvFarSize > 0.0 ? u_terrain_mapInvFarSize : u_terrain_mapInvNearSize; // far cascade, else near
        const vec2 uv = abs((worldXZ - u_terrain_mapCentre) * invOuter); // 0.5 = the map's edge
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
// apply the identical rotation, and the field TRAVELS AGAINST u_ocean_windDirection.
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
    const float margin = u_ocean_cullMargin;
    if (margin <= 0.0)
        return false;
    const float reach = 3.0 * cellSize; // after the CDLOD morph no co-triangle vertex lies further away

    // Never cull past the streamed terrain mesh (u_terrain_meshRadius): the cull is only invisible while
    // a rendered mesh stands above the water - the clamp-to-edge-extended bakes report "land" forever.
    if (distance(worldXZ, u_viewPos.xz) + reach >= u_terrain_meshRadius)
        return false;

    float err = -1.0; // burial slack on top of margin + swash reach
#ifdef TERRAIN_HEIGHT_BINDING
    if (terrainHeightMapPresent())
    {
        const vec2 rel = worldXZ - u_terrain_mapCentre;
        const float cheb = max(abs(rel.x), abs(rel.y));
        const float invNear = u_terrain_mapInvNearSize;
        if ((cheb + reach) * invNear < 0.42) // full-weight near region (blend starts at 0.42)
            err = reach + 0.5 / (float(textureSize(u_terrainHeight, 0).x) * invNear);
        else
        {
            const float invFar = u_terrain_mapInvFarSize;
            if (u_ocean_farCullError <= 0.0 || invFar <= 0.0 || (cheb + reach) * invFar >= 0.48)
                return false; // far cull off / footprint reaches the far map's clamp-to-edge border
            err = u_ocean_farCullError;
        }
    }
#endif
    if (err < 0.0)
        return false; // no baked terrain data: open-ocean fallback, always water

    // Swash reach (u_ocean_swashReach) keeps the wet band above the waterline alive.
    return shoreHW.y - shoreHW.x < -(margin + u_ocean_swashReach + err);
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
    const float range = u_ocean_horizonDepthRange;
    if (range <= 0.0)
        return depth;
    const float t = smoothstep(range * 0.5, range, distance(worldXZ, u_viewPos.xz));
    return max(depth, u_ocean_horizonDepth * t);
}

// Vertex displacement mip: matches the ring's FIXED cell size (Nyquist band-limit, no camera-coupled
// morphing); `morph` blends +1 across the CDLOD boundary so adjacent rings meet exactly.
// Both displacement passes MUST use this same function.
float oceanVertexLod(float cellSize, float morph, float patchSize)
{
    return max(log2(max(cellSize, 1e-3) * float(OCEAN_FFT_SIZE) / patchSize) + morph + u_ocean_detailBias, 0.0);
}

// --- The shore, in three weights. depth = calm water level above the ground (negative = land height).
// The CPU buoyancy mirror (OceanGenerator) and underwaterLiveWaveY (underwater_light.inc) repeat them;
// the terrain's water film (instanced_indirect_terrain.fs) repeats the surface weight.

// Approach fade: 0 offshore, 1 at the waterline, over the wider of two swash reaches and "Shoal depth
// scale" x the mid cascade's patch size. The band across which open water becomes the shore.
float oceanSwashFadeIn(float depth)
{
    const float reach = max(u_ocean_swashReach, 0.01);
    return 1.0 - smoothstep(0.0, max(2.0 * reach, u_ocean_shoalScale * u_ocean_cascadeSizes.y), depth);
}

// Swash base: the fraction of the raw wave field that runs up the beach - "Swash amplitude" x the
// sea-connection fade (landlocked water, its baked level off sea level, gets none: no swell reaches it)
// x the land-height fade (dies one reach above the level).
float oceanSwashBase(float depth, float waterLevel)
{
    const float amp = u_ocean_swashAmp;
    if (amp <= 0.0)
        return 0.0;
    const float seaFade = 1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_ocean_seaLevel));
    const float reach = max(u_ocean_swashReach, 0.01);
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

// --- Sub-band detail --------------------------------------------------------------------------------
// The FFT band stops at the finest cascade's Nyquist, and the clipmap band-limits the displacement well
// above that, so near the camera the sea is a normal map on smooth geometry. This borrows the finest
// cascade's own gradient field at a FRACTION of its patch size: the same wave statistics at a shorter
// wavelength, for one fetch and no extra FFT work.
//   - ROTATED domain: an unrotated copy is the same field scaled - its crests run parallel to the
//     parent's and read as a fractal repeat. The slope comes back rotated into the sample domain.
//   - SHADING ONLY: never in oceanSampleDisplacement, so the geometry, the depth prepass and the CPU
//     buoyancy mirror (OceanGenerator::sampleDisplacement) are untouched.
//   - Implicit LOD: the smaller patch scales the uv derivatives, so the mip chain filters this band like
//     the cascades. It still fades out past "Detail fade (m)": its variance is not in the LEAN moments,
//     so filtered-away detail would vanish instead of becoming roughness ("Micro roughness" covers it).
// The terrain water film carries an inlined copy (instanced_indirect_terrain.fs.glsl) - keep them in step.
vec2 oceanDetailSlope(vec2 sampleXZ, vec2 worldXZ, float surfaceWeight)
{
    const float strength = u_ocean_detailStrength;
    if (strength <= 0.0)
        return vec2(0.0);
    float fade = surfaceWeight; // dies into the beach with every other wave term
    const float fadeDist = u_ocean_detailFadeDistance;
    if (fadeDist > 0.0)
        fade *= 1.0 - smoothstep(0.5 * fadeDist, fadeDist, distance(worldXZ, u_viewPos.xz));
    if (fade <= 0.0)
        return vec2(0.0);
    const int c = OCEAN_CASCADES - 1; // the finest cascade: the shortest waves there are to borrow
    const float L = max(u_ocean_cascadeSizes[c] * u_ocean_detailScale, 1e-3);
    const vec2 rot = vec2(cos(u_ocean_detailRotation), sin(u_ocean_detailRotation));
    const vec2 p = vec2(rot.x * sampleXZ.x + rot.y * sampleXZ.y, -rot.y * sampleXZ.x + rot.x * sampleXZ.y);
    const vec2 g = texture(u_oceanMaps, vec3(p / L, float(OCEAN_CASCADES + c))).xy; // (dh/dx, dh/dz)
    const vec2 s = g * (strength * fade);
    return vec2(rot.x * s.x - rot.y * s.y, rot.y * s.x + rot.x * s.y); // back into the sample domain
}

// Cascade displacement sum at an undisplaced (morphed) world XZ. Choppy lambda applied here so it
// stays live (the maps store raw Dx/Dz). shoreHW = the vertex's (terrain height, water level),
// fetched once by the caller. The CPU buoyancy mirror (OceanGenerator::sampleDisplacement) MUST
// match this function change for change.
vec3 oceanSampleDisplacement(vec2 worldXZ, float cellSize, float morph, vec2 shoreHW)
{
    const float chop = u_ocean_choppiness;
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    vec3 disp = vec3(0.0);
    float sw = 0.0;
    // Buried deeper than the swash band: the surface weight is zero - skip the fetches (bit-identical).
    if (depth > -u_ocean_swashReach)
    {
        const vec2 fr = oceanFlowRotation(worldXZ);
        const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
        float rawY = 0.0;
        vec2 rawXZ = vec2(0.0);
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_ocean_cascadeSizes[c];
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
        vec2 flowOff = rawXZ * (chop * u_ocean_swashFlow * sw * flowFade);
        const float flowCap = clamp(0.5 * u_ocean_swashReach, 0.25, 1.0);
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
//   jacobianRaw : the same before the shore's depth weight (the surf band reads folds off it)
//   slopeVar : LEAN term (Bruneton 2010) - slope variance lost to mip filtering, returned as
//              microfacet roughness (the elongated sun glitter at distance)
//   accel    : vertical acceleration (breaking-crest foam driver)
//   shoreHW  : the (terrain height, water level) fetch, returned for the caller to reuse
//   detail   : the sub-band detail slope's share of `slope` (world domain) - the foam lights on it
//   foamJacobian : `jacobian` with the finest cascade scaled by "Foam fine waves" (u_ocean_foamFineWaves) -
//              the stuck foam's density reads it, so the short, fast waves do not reshape it every frame
void oceanSampleSurface(vec2 worldXZ, out vec2 slope, out float jacobian, out float jacobianRaw, out vec2 slopeVar, out float accel, out vec2 shoreHW, out vec2 detail, out float foamJacobian)
{
    detail = vec2(0.0);
    foamJacobian = 1.0;
    vec3 fine = vec3(0.0); // the finest cascade's (dDx/dx, dDz/dz, dDx/dz)
    const float chop = u_ocean_choppiness;
    shoreHW = oceanSampleShoreData(worldXZ);
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    const vec2 fr = oceanFlowRotation(worldXZ);
    const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
    vec2 slopeSum = vec2(0.0);
    vec2 varSum = vec2(0.0);
    float sxx = 0.0, szz = 0.0, sxz = 0.0;
    accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const vec2 uv = sampleXZ / u_ocean_cascadeSizes[c];
        const vec4 g = texture(u_oceanMaps, vec3(uv, float(OCEAN_CASCADES + c)));
        const vec4 d = texture(u_oceanMaps, vec3(uv, float(c)));
        const vec4 m = texture(u_oceanMaps, vec3(uv, float(2 * OCEAN_CASCADES + c)));
        slopeSum += g.xy;
        varSum += max(m.xy - g.xy * g.xy, vec2(0.0));
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += m.z;
        if (c == OCEAN_CASCADES - 1)
            fine = vec3(g.z, g.w, d.w);
    }
    jacobianRaw = (1.0 + chop * sxx) * (1.0 + chop * szz) - chop * sxz * chop * sxz;
    // Buried under land: flat calm surface (with swash on, the run-up band still shades, and its surf
    // lace still reads the raw folds).
    if (depth <= (u_ocean_swashAmp > 0.0 ? -u_ocean_swashReach : 0.0))
    {
        slope = vec2(0.0);
        jacobian = 1.0;
        slopeVar = vec2(0.0);
        accel = 0.0;
        return;
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
    {
        const vec3 f = fine * ((u_ocean_foamFineWaves - 1.0) * w * chop); // the finest cascade's change
        const float fxx = jxx + f.x, fzz = jzz + f.y, fxz = jxz + f.z;
        foamJacobian = fxx * fzz - fxz * fxz;
    }
    // Floor + rational soft limit: near folds the raw division explodes the slope into dark creases.
    // "Crest slope limit" (u_ocean_crestSlopeLimit) is the limit's strength - it also compresses the steep
    // crest faces, so lowering it sharpens crests at the risk of creases; 0 = off.
    slope = slopeSum / max(vec2(jxx, jzz), vec2(0.6));
    slope /= 1.0 + u_ocean_crestSlopeLimit * length(slope);
    const vec2 detailSample = oceanDetailSlope(sampleXZ, worldXZ, w);
    slope = oceanFlowToWorld(slope + detailSample, fr);
    detail = oceanFlowToWorld(detailSample, fr);
    slopeVar = varSum;
}

// Instant crest foam from the fold Jacobian + downward acceleration (Longuet-Higgins). ONE function
// shared by the water shader (the crest foam it draws), ocean_foam.cs.glsl (what it injects into the foam
// field) and the spray.
float oceanInstantFoam(float jacobian, float accel)
{
    const float softness = max(u_ocean_foamSoftness, 0.02);
    const float bias = u_ocean_foamBias;
    const float fold = 1.0 - smoothstep(bias - softness, bias, jacobian);
    const float breaking = smoothstep(u_ocean_foamBreakAccel, u_ocean_foamBreakAccel + softness, -accel / 9.81);
    return max(fold, breaking);
}

#endif
