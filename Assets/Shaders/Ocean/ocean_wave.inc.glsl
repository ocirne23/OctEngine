#ifndef OCEAN_WAVE_INC_GLSL
#define OCEAN_WAVE_INC_GLSL

// Sampling helpers for the FFT ocean maps (OceanSimulationPipeline / ocean_*.cs.glsl), per cascade c:
//   layer c                      : displacement (Dx, h, Dz, -)
//   layer   OCEAN_CASCADES + c   : slopes (dh/dx, dh/dz) + their second moments (dhx^2, dhz^2)
//   layer 2*OCEAN_CASCADES + c   : the fold Jacobian's terms (dDx/dx, dDz/dz, dDx/dz) + the vertical acceleration
//   layer 3*OCEAN_CASCADES + l   : the world-space foam field's level l (ocean_foam_field.inc.glsl)
// One layer per USE: the geometry reads the first, a normal the second, a fold / foam test the third alone.
// Shared by the displacement VS and ocean.fs.glsl, so the geometry and the shaded normals read the same field.
// Requires ubo.inc.glsl; the includer may set OCEAN_MAPS_BINDING / TERRAIN_HEIGHT_BINDING first.

#ifndef OCEAN_MAPS_BINDING
#define OCEAN_MAPS_BINDING 7
#endif
layout (binding = OCEAN_MAPS_BINDING) uniform sampler2DArray u_oceanMaps;

#ifdef TERRAIN_HEIGHT_BINDING
#include "terrain_height.inc.glsl"
#endif

// (terrain height, water surface level) at worldXZ; open-ocean bottom / sea level without terrain data. Depth = level -
// height; the clipmap lifts its vertices by level - sea level.
// Fades back to open ocean across the outer cascade's border: the clamp-to-edge sampler extends the border texel forever,
// so land there would flatten the sea along a DEAD-STRAIGHT seam to the horizon. Past the data the world is open sea.
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

// Per-texel wave-travel rotation. DISABLED (identity): rotating the sample domain pivots on the world origin, so the sea
// creases along every 8-bit angle contour - the baked flow steers the SIMULATION wind instead
// (OceanGenerator::steeredWindAngle). If revived: every wave-sampling pass must apply the same rotation, and the field
// TRAVELS AGAINST u_ocean_windDirection.
vec2 oceanFlowRotation(vec2 worldXZ)
{
    return vec2(1.0, 0.0);
}

// Rotate the FFT sampling position by -delta / a resulting horizontal vector back into world space.
vec2 oceanFlowSamplePos(vec2 worldXZ, vec2 fr) { return vec2(fr.x * worldXZ.x + fr.y * worldXZ.y, -fr.y * worldXZ.x + fr.x * worldXZ.y); }
vec2 oceanFlowToWorld(vec2 v, vec2 fr) { return vec2(fr.x * v.x - fr.y * v.y, fr.y * v.x + fr.x * v.y); }

// Land cull: true when the whole triangle footprint is buried under land (the VS then emits a NaN position). Decided by
// the vertex's own shoreHW alone: the footprint must be buried deeper than its own radius (a 45-degree slope bound -
// smoother terrain can't reach water at any co-triangle vertex). Cliffs steeper than 45 degrees can clip a sliver.
// ONLY where the whole footprint reads the NEAR data cascade at full weight: the far cascade is the generator's coarse
// stage, not the drawn mesh, and a cull from it removed water along every distant shore. Past the near cascade the depth
// test alone cuts the water against the ground.
// Both displacement passes MUST apply the same test or their geometry diverges.
bool oceanVertexCulled(vec2 worldXZ, float cellSize, vec2 shoreHW)
{
#ifdef TERRAIN_HEIGHT_BINDING
    const float margin = u_ocean_cullMargin;
    if (margin <= 0.0 || !terrainHeightMapPresent())
        return false; // no baked terrain data: open-ocean fallback, always water
    const float reach = 3.0 * cellSize; // after the CDLOD morph no co-triangle vertex lies further away

    // Never cull past the streamed terrain mesh: the cull is only invisible while a rendered mesh stands above the water.
    if (distance(worldXZ, u_viewPos.xz) + reach >= u_terrain_meshRadius)
        return false;

    const vec2 rel = abs(worldXZ - u_terrain_mapCentre);
    const float invNear = u_terrain_mapInvNearSize;
    if ((max(rel.x, rel.y) + reach) * invNear >= 0.42) // terrainDataAt's blend to the far cascade starts at 0.42
        return false;

    // Burial slack: the footprint's radius + half a near texel. The swash reach keeps the wet band above the waterline alive.
    const float err = reach + 0.5 / (float(textureSize(u_terrainHeight, 0).x) * invNear);
    return shoreHW.y - shoreHW.x < -(margin + u_ocean_swashReach + err);
#else
    return false;
#endif
}

// The depth the WAVES and the surf band use: the baked depth, floored at "Ocean/Shore/Horizon depth" (0 = no floor)
//   - past "Horizon depth range" (0 = this part off), and
//   - wherever the FAR data cascade answers (1 - terrainNearWeight), at any distance: its depth is the generator's coarse
//     stage, which stands above the sea over real water along most shores. The depth test draws that water (the mesh is
//     under it), and a depth <= 0 there is a flat mirror covered in surf foam: a white sheet with the coarse contour's
//     straight edges, which comes and goes as the near cascade re-centres.
// Shallow is the dangerous direction: the shoal fade scales the slopes and (as fade^2) the LEAN variance, so the surface
// loses its roughness and mirrors the sky like wind 0, and the surf band's waterline term goes full. Too DEEP costs
// nothing visible.
// Only the assumed SEABED moves, never the surface, so no water over land. The land cull keeps reading the RAW depth.
float oceanEffectiveDepth(vec2 worldXZ, float depth)
{
    float t = 0.0;
    const float range = u_ocean_horizonDepthRange;
    if (range > 0.0)
        t = smoothstep(range * 0.5, range, distance(worldXZ, u_viewPos.xz));
#ifdef TERRAIN_HEIGHT_BINDING
    if (terrainHeightMapPresent())
        t = max(t, 1.0 - terrainNearWeight(worldXZ));
#endif
    return max(depth, u_ocean_horizonDepth * t);
}

// Vertex displacement mip: matches the ring's FIXED cell size (Nyquist band-limit, no camera-coupled morphing); `morph`
// blends +1 across the CDLOD boundary so adjacent rings meet exactly. Both displacement passes MUST use this function.
float oceanVertexLod(float cellSize, float morph, float patchSize)
{
    return max(log2(cellSize * float(OCEAN_FFT_SIZE) / patchSize) + morph + u_ocean_detailBias, 0.0);
}

// --- The shore, in three weights. depth = calm water level above the ground (negative = land height).
// The CPU buoyancy mirror (OceanGenerator) and underwaterLiveWaveY (underwater_light.inc) repeat them; the terrain's water
// film (instanced_indirect_terrain.fs) repeats the surface weight.

// Approach fade: 0 offshore, 1 at the waterline, over the wider of two swash reaches and "Shoal depth scale" x the mid
// cascade's patch size.
float oceanSwashFadeIn(float depth)
{
    return 1.0 - smoothstep(0.0, u_ocean_swashFadeBand, depth);
}

// Swash base: the fraction of the raw wave field that runs up the beach - "Swash amplitude" x the sea-connection fade
// (landlocked water, off sea level, gets no swell) x the land-height fade (dies one reach above the level).
float oceanSwashBase(float depth, float waterLevel)
{
    const float amp = u_ocean_swashAmp;
    const float seaFade = 1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_ocean_seaLevel));
    const float landFade = clamp(1.0 + min(depth, 0.0) * u_ocean_swashInvReach, 0.0, 1.0);
    return amp * seaFade * landFade;
}

// Swash weight: the base faded in across the approach band - gates the shore-only TONGUE behaviours (the backflow).
float oceanSwashWeight(float depth, float waterLevel)
{
    return oceanSwashBase(depth, waterLevel) * oceanSwashFadeIn(depth);
}

// THE surface weight: the fraction of the RAW cascade sum that stands at this depth, w = 1 - fadeIn * (1 - swashBase).
// ONE weight for every cascade, so the spectral detail holds all the way in (per-cascade fades left a visible line). Used
// identically by the displacement, the shading normal and the vertex normal.
float oceanSurfaceWeight(float depth, float waterLevel)
{
    return 1.0 - oceanSwashFadeIn(depth) * (1.0 - oceanSwashBase(depth, waterLevel));
}

// --- Sub-band detail. The FFT band stops at the finest cascade's Nyquist and the clipmap band-limits the displacement
// well above that, so near the camera the sea is a normal map on smooth geometry. This borrows the finest cascade's
// gradients at a FRACTION of its patch size: the same statistics at a shorter wavelength, for one fetch.
//   - ROTATED domain: an unrotated copy's crests run parallel to the parent's (a fractal repeat). The slope comes back
//     rotated into the sample domain.
//   - SHADING ONLY: never in oceanSampleDisplacement, so the geometry, the depth prepass and the CPU buoyancy mirror are
//     untouched.
//   - Implicit LOD: the smaller patch scales the uv derivatives, so the mips filter it like the cascades, and what they
//     filter out comes back as `variance` (the tap's own second moments): the band turns into roughness with distance,
//     no fade.
//   - "Detail octaves" (1..OCEAN_DETAIL_OCTAVES): octave o repeats the step at scale^(o + 1) and rotation x (o + 1).
//     u_ocean_detailOctaves[o] = (cos, sin, 1 / patch size, 1 while the octave is on).
// The terrain water film carries an inlined copy of the FIRST octave, with a distance fade and no variance
// (terrain_film.fs.glsl) - keep them in step.
#define OCEAN_DETAIL_OCTAVES 3
vec2 oceanDetailSlope(vec2 sampleXZ, float surfaceWeight, out float variance)
{
    variance = 0.0;
    const float gain = u_ocean_detailStrength * surfaceWeight; // dies into the beach with every other wave term
    vec2 slope = vec2(0.0);
    if (gain <= 0.0)
        return slope;

    const float layer = float(2 * OCEAN_CASCADES - 1); // the finest cascade's slopes: the shortest waves there are to borrow
    for (int o = 0; o < OCEAN_DETAIL_OCTAVES; ++o)
    {
        const vec4 oct = u_ocean_detailOctaves[o];
        if (oct.w <= 0.0)
            break;
        const vec2 p = vec2(oct.x * sampleXZ.x + oct.y * sampleXZ.y, -oct.y * sampleXZ.x + oct.x * sampleXZ.y);
        const vec4 g = texture(u_oceanMaps, vec3(p * oct.z, layer)); // (dh/dx, dh/dz, their squares)
        slope += vec2(oct.x * g.x - oct.y * g.y, oct.y * g.x + oct.x * g.y); // back into the sample domain
        variance += 0.5 * max(g.z + g.w - dot(g.xy, g.xy), 0.0);
    }
    variance *= gain * gain;
    return slope * gain;
}

// Cascade displacement sum at an undisplaced (morphed) world XZ. Choppy lambda applied here so it stays live (the maps
// store raw Dx/Dz). shoreHW = the vertex's (terrain height, water level), fetched once by the caller.
// The CPU buoyancy mirror (OceanGenerator::sampleDisplacement) MUST match this function change for change.
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

        // The raw field x ONE depth weight: oceanSurfaceWeight and oceanSwashWeight, their shared terms evaluated once.
        const float base = oceanSwashBase(depth, shoreHW.y);
        const float fadeIn = oceanSwashFadeIn(depth);
        disp = vec3(rawXZ.x * chop, rawY, rawXZ.y * chop) * (1.0 - fadeIn * (1.0 - base));
        sw = base * fadeIn;

        // Swash backflow: the raw chop slides the tongue seaward as the wave recedes. Gated by the tongue's thickness
        // above the sand (a buried surface must not keep sliding), soft-capped to ~the swash reach (the raw offset is
        // unbounded and would shear triangles into streaks).
        const float flowFade = smoothstep(0.0, 0.35, rawY * sw + depth);
        vec2 flowOff = rawXZ * (chop * u_ocean_swashFlow * sw * flowFade);
        const float flowCap = u_ocean_swashFlowCap;
        flowOff *= flowCap / (flowCap + length(flowOff));
        disp.xz += flowOff;
        disp.xz = oceanFlowToWorld(disp.xz, fr);
    }
    // No waterline floor: the DEPTH BUFFER cuts the wave against the sand per pixel - a trough under the seabed exposes
    // (wet) sand, which is what a receding swash looks like.
    return disp;
}

// THE WAVE MARCH: a short march from a surface point along `dir` (unit, dir.y > 0) over the cascades' height field. The
// water is not in the TLAS, so no ray sees a wave: this is the waves' own shadow (dir = the sun) and their occlusion of a
// mirror ray. Returns (the visibility along dir past the waves in front, soft; the metres of WATER on the ray; how
// steeply the ray starts into the water - 0 on a face turned to dir).
// "Ocean/Shading/Wave march *": steps, range, softness.
//   - In the sampled field's own frame: the origin's height is the field's (not the mesh's band-limited one).
//   - The steps grow quadratically up to where the ray is above the highest crest (u_ocean_waveExcursion), or the range.
//   - EVERY sample, the origin's too, reads the cascades at ONE mip: the first step's length, never finer than the
//     pixel (footprint, m). A mip per step was tried: the origin and the first step then read two different fields, and
//     their difference alone, over the first step's centimetres of tolerance, put the ray "under water" in hard-edged
//     patches. Far water marches a smooth field, so the occlusion fades out where the waves go below a pixel.
//   - Each step's rest coordinate is its world XZ minus the choppy offset of the step before (one fixed-point turn of the
//     displacement's inverse; the displacement layer gives both in one tap).
//   - Soft: a step counts by how far the ray is under the surface, over the cone "Wave march softness" x the distance.
#define OCEAN_MARCH_START_SLOPE 0.35 // the surface's slope past the ray's at which `start` is full
vec3 oceanWaveMarch(vec2 restXZ, vec2 shoreHW, vec3 dir, float footprint)
{
    const float w = oceanSurfaceWeight(oceanEffectiveDepth(restXZ, shoreHW.y - shoreHW.x), shoreHW.y);
    const float crest = u_ocean_waveExcursion * w;
    if (dir.y <= 0.0 || crest <= 0.0)
        return vec3(1.0, 0.0, 0.0);
    const float chop = u_ocean_choppiness * w;
    const float invSteps = 1.0 / u_ocean_marchSteps;
    const float invRise = 1.0 / max(dir.y, 0.03);
    const float tFull = min(2.0 * crest * invRise, u_ocean_marchRange);
    // ONE mip per cascade for every sample of the march, the origin's too: the first step's length, never finer than the
    // pixel.
    vec3 lods;
    {
        const float lod = log2(max(tFull * invSteps * invSteps, footprint));
        for (int c = 0; c < OCEAN_CASCADES; ++c)
            lods[c] = max(lod + log2(float(OCEAN_FFT_SIZE) / u_ocean_cascadeSizes[c]), 0.0);
    }

    vec3 d = vec3(0.0);
    for (int c = 0; c < OCEAN_CASCADES; ++c)
        d += textureLod(u_oceanMaps, vec3(restXZ / u_ocean_cascadeSizes[c], float(c)), lods[c]).xyz;
    const float y0 = w * d.y;
    const float tEnd = min(tFull, (crest - y0) * invRise);
    if (tEnd <= 0.0)
        return vec3(1.0, 0.0, 0.0);
    vec2 off = chop * d.xz;
    const vec2 x0 = restXZ + off;

    // EVERY OUTPUT IS CONTINUOUS in the samples: a value that jumps between two pixels draws a line on the water.
    //   start : how steeply the ray starts into the water: the first step's depth over a WIDE cone
    //           (OCEAN_MARCH_START_SLOPE) - 0 on a face turned to dir, 1 only on a face well turned from it. Over the
    //           visibility's narrow cone it switched on at once, and the trough edge of every back face showed as a line.
    //   path  : ALL the water on the ray: per step, the part of it where the surface is above the ray, with the surface
    //           taken as linear between the two samples (the origin is on the surface: 0). Tried and dropped:
    //           - a sum of whole steps: a few values only, and what reads the path changes in bands;
    //           - the distance to the FIRST exit: it jumps where a ray comes out of the water for one sample and its
    //             neighbour does not (a hard edge across a back face).
    //           Water of ANOTHER wave counts too. `start` is what keeps a point that is only shadowed by one dark.
    float occl = 0.0, path = 0.0, tPrev = 0.0, start = 0.0, prevAbove = 0.0;
    const int steps = int(u_ocean_marchSteps);
    for (int k = 1; k <= steps; ++k)
    {
        const float f = float(k) * invSteps;
        const float t = tEnd * f * f;
        const float dt = t - tPrev;
        const vec2 q = x0 + dir.xz * t - off;
        d = vec3(0.0);
        for (int c = 0; c < OCEAN_CASCADES; ++c)
            d += textureLod(u_oceanMaps, vec3(q / u_ocean_cascadeSizes[c], float(c)), lods[c]).xyz;
        off = chop * d.xz;
        const float above = w * d.y - y0 - dir.y * t; // the surface above the ray (m): > 0 = the ray is under water
        const float under = clamp(above * u_ocean_marchInvSoftness / t, 0.0, 1.0); // the same, over the soft cone
        occl = max(occl, under);
        if (k == 1)
            start = clamp(above / (t * OCEAN_MARCH_START_SLOPE), 0.0, 1.0);
        {
            const float lo = min(prevAbove, above), hi = max(prevAbove, above);
            path += dt * clamp(hi / max(hi - lo, 1e-6), 0.0, 1.0); // lo >= 0: all of the step; hi <= 0: none of it
        }
        prevAbove = above;
        tPrev = t;
    }
    return vec3(1.0 - occl, path, start);
}

// Combined surface data for the water FS (implicit LOD: the mip chain prefilters distant slopes):
//   slope        : chop-corrected slope (Tessendorf: grad h / (1 + lambda dD))
//   jacobian     : horizontal fold J (< ~0.5 = folding crest)
//   jacobianRaw  : the same before the shore's depth weight (the surf band reads folds off it)
//   slopeVar     : LEAN term (Bruneton 2010) - slope variance lost to mip filtering, as microfacet roughness
//   accel        : vertical acceleration (breaking-crest foam driver)
//   shoreHW      : the (terrain height, water level) fetch, returned for the caller to reuse
//   detail       : the sub-band detail slope's share of `slope` (world domain) - the foam lights on it
//   foamJacobian : `jacobian` with the finest cascade x "Foam fine waves" - the stuck foam's density reads it, so the
//                  short, fast waves do not reshape it every frame
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
        const vec4 g = texture(u_oceanMaps, vec3(uv, float(OCEAN_CASCADES + c)));     // slopes, their second moments
        const vec4 j = texture(u_oceanMaps, vec3(uv, float(2 * OCEAN_CASCADES + c))); // dDx/dx, dDz/dz, dDx/dz, accel
        slopeSum += g.xy;
        varSum += max(g.zw - g.xy * g.xy, vec2(0.0));
        sxx += j.x; szz += j.y; sxz += j.z;
        accel += j.w;
        if (c == OCEAN_CASCADES - 1)
            fine = j.xyz;
    }
    jacobianRaw = (1.0 + chop * sxx) * (1.0 + chop * szz) - chop * sxz * chop * sxz;

    // Buried under land past the swash band: flat calm surface (the run-up band still shades; its lace reads the raw folds).
    if (depth <= -u_ocean_swashReach)
    {
        slope = vec2(0.0);
        jacobian = 1.0;
        slopeVar = vec2(0.0);
        accel = 0.0;
        return;
    }

    // The displacement's one depth weight, so shading tracks the geometry: slopes and the Jacobian scale with the height,
    // the variance with its square.
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

    // Floor + rational soft limit: near folds the raw division explodes the slope into dark creases. "Crest slope limit"
    // also compresses the steep crest faces: lower sharpens crests at the risk of creases; 0 = off.
    slope = slopeSum / max(vec2(jxx, jzz), vec2(0.6));
    slope /= 1.0 + u_ocean_crestSlopeLimit * length(slope);
    float detailVar;
    const vec2 detailSample = oceanDetailSlope(sampleXZ, w, detailVar);
    slope = oceanFlowToWorld(slope + detailSample, fr);
    detail = oceanFlowToWorld(detailSample, fr);
    slopeVar = varSum + vec2(detailVar);
}

// Instant crest foam from the fold Jacobian + downward acceleration (Longuet-Higgins). ONE function shared by the water
// shader (the crest foam it draws), ocean_foam.cs.glsl (what it injects into the foam field) and the spray.
float oceanInstantFoam(float jacobian, float accel)
{
    const float softness = u_ocean_foamSoftness;
    const float bias = u_ocean_foamBias;
    const float fold = 1.0 - smoothstep(bias - softness, bias, jacobian);
    const float breaking = smoothstep(u_ocean_foamBreakAccel, u_ocean_foamBreakAccel + softness, -accel / 9.81);
    return max(fold, breaking);
}

#endif
