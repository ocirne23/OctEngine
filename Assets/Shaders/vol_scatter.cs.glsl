#version 460

#extension GL_EXT_ray_query : require

// Volumetric fog scatter: one invocation per froxel. Evaluates the participating-media density at the
// froxel (global height fog with wind-animated noise, plus local fog volume boxes) and the light scattered
// toward the camera through it: the sun (shadowed by a TLAS ray query or a cascade tap), GI probe ambient,
// and the local lights from the world-space light hash grid. The result is temporally blended against last
// frame's reprojected froxel (the Z sample point is jittered per frame, so the blend integrates the slice).
// vol_integrate.cs.glsl then accumulates the grid along Z.

#include "shared.inc.glsl"
#include "vol_fog.inc.glsl"

struct LightInfo
{
    vec3 pos;
    float range;
    vec3 color;
    float width;
    vec3 direction;
    float rotation;
};
struct FogVolume
{
    vec3 pos;
    float density;
    vec3 halfExtents;
    float edgeSoftness;
    vec3 albedo;
    float emissive;
};

layout (binding = 1, std430) readonly buffer InLightInfos { LightInfo in_lightInfos[]; };
layout (binding = 2, std430) readonly buffer InLightGrid  { uint in_gridData[]; };
layout (binding = 3, std430) readonly buffer InGridTable
{
    uint in_numGrids;
    uint in_gridDataCounter;
    uint in_tableSize;
    uint in_gridTable[];
};
layout (binding = 4) uniform accelerationStructureEXT u_tlas;
layout (binding = 5) uniform sampler2DArrayShadow u_shadowMap;
layout (binding = 6, std430) readonly buffer InFogVolumes
{
    uint in_numFogVolumes;
    uint _fvPad0;
    uint _fvPad1;
    uint _fvPad2;
    FogVolume in_fogVolumes[];
};
layout (binding = 7) uniform sampler3D u_history;
layout (binding = 8, rgba16f) uniform writeonly image3D u_outScatter;
layout (binding = 9, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };
#define TERRAIN_HEIGHT_BINDING 10
#include "terrain_height.inc.glsl"
// FFT ocean maps at binding 11 (declared by the include as u_uwOceanMaps): the underwater fog boundary
// samples the LIVE wave height so fog never pokes out of a trough, and the underwater sun term crosses
// the same surface (light shafts: caustic focus + Beer-Lambert; see underwater_light.inc.glsl).
#define UNDERWATER_OCEAN_BINDING 11
#include "underwater_light.inc.glsl"

// Light grid (read) + giSquareFalloff/giSunShadow from the shared lighting helpers.
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"
#include "lighting.inc.glsl"

// GI probe clipmap (read-only): fog picks up bounced/ambient light.
#define GI_GRID_DATA_NAME gi_gridData
#include "gi_probe.inc.glsl"

uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float hashToFloat(uint x) { return float(hashU(x) & 0x00FFFFFFu) / float(0x01000000u); }

float vnLattice(ivec3 p)
{
    const uint h = uint(p.x) * 1597334673u ^ uint(p.y) * 3812015801u ^ uint(p.z) * 2798796415u;
    return hashToFloat(h);
}

float valueNoise(vec3 p)
{
    const ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    const float n000 = vnLattice(i + ivec3(0, 0, 0)), n100 = vnLattice(i + ivec3(1, 0, 0));
    const float n010 = vnLattice(i + ivec3(0, 1, 0)), n110 = vnLattice(i + ivec3(1, 1, 0));
    const float n001 = vnLattice(i + ivec3(0, 0, 1)), n101 = vnLattice(i + ivec3(1, 0, 1));
    const float n011 = vnLattice(i + ivec3(0, 1, 1)), n111 = vnLattice(i + ivec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Two-octave wind-drifted value noise in [0,1]; modulates the density for a dusty, wispy look.
float fogNoise(vec3 worldPos)
{
    const vec3 wind = vec3(1.0, 0.12, 0.55) * (u_fogParams2.z * u_timeSeconds);
    const vec3 p = (worldPos + wind) * u_fogParams2.x;
    return valueNoise(p) * 0.667 + valueNoise(p * 2.37 + vec3(17.3)) * 0.333;
}

// Mean of the exponential height profile exp(-max(y - base, 0) * falloff) over one slice's ray segment,
// in closed form. A point sample (even jittered + temporally blended) only converges to this mean after
// many frames - while the camera moves the history is short, and the partially-converged per-slice means
// disagree, which reads as view-aligned density layers sweeping with the camera. The analytic mean is
// exact every frame, so the height fog profile is layer-free even with zero history (only the lighting
// stays stochastic).
float heightFogMean(float yA, float yB, float base, float falloff)
{
    const float y0 = min(yA, yB) - base;
    const float y1 = max(yA, yB) - base;
    if (falloff * max(y1, 0.0) < 1e-4)
        return 1.0; // whole segment at/below the base (or no falloff): full density
    const float dy = y1 - y0;
    if (dy < 1e-3)
        return exp(-max(y0, 0.0) * falloff); // near-horizontal segment: point sample is exact
    const float a0 = max(y0, 0.0), a1 = max(y1, 0.0);
    const float belowFrac = clamp(-y0 / dy, 0.0, 1.0); // fraction of the segment below the base (density 1)
    return belowFrac + (exp(-a0 * falloff) - exp(-a1 * falloff)) / (falloff * dy);
}

// Vertical FFT wave displacement at worldXZ (m, sum of the cascades, shoal-faded like the drawn surface;
// chop and river flow rotation are skipped - sub-froxel for a fog boundary). The mip matches the froxel's
// world footprint, band-limiting the boundary to what the froxel grid can represent anyway.
float oceanWaveHeightAt(vec2 worldXZ, float waterDepth, float footprint)
{
    float h = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_oceanParams2[c];
        const float fade = smoothstep(0.0, max(u_oceanParams4.z * L, 0.01), waterDepth);
        const float lod = max(log2(max(footprint, 1e-3) * float(OCEAN_FFT_SIZE) / L), 0.0);
        h += textureLod(u_uwOceanMaps, vec3(worldXZ / L, float(c)), lod).y * fade;
    }
    return h;
}

// Hard visibility ray against opaque TLAS geometry (alpha-masked detail is irrelevant at fog frequency).
float volRayVisibility(vec3 origin, vec3 dir, float tMax)
{
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.02, dir, tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT ? 0.0 : 1.0;
}

// Radiance scattered toward the camera from one grid light: same type encoding (point/spot/rect/tube)
// and falloff as the surface path, but with the HG phase in place of the NdotL/BRDF.
vec3 volLightScatter(LightInfo light, vec3 pos, vec3 viewDir, float g, bool shadowRays)
{
    const vec3 toLight = light.pos - pos;
    const float dist = length(toLight);
    const vec3 L = toLight / max(dist, 1e-4);

    vec3 rad = light.color * giSquareFalloff(dist, abs(light.range));
    if (light.width < 0.0) // spot: rotation = cone half-angle, |direction| = edge softness
    {
        const float softness = length(light.direction);
        const float cosAngle = dot(-L, light.direction / max(softness, 1e-4));
        const float cosOuter = cos(light.rotation);
        const float cosInner = mix(cosOuter, 1.0, softness);
        rad *= smoothstep(cosOuter, cosInner, cosAngle);
    }
    else if (light.width > 0.0 && light.range >= 0.0) // rect area: one-sided along its facing normal
    {
        const float height = length(light.direction);
        const vec3 up     = light.direction / height;
        const vec3 ref    = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 right0 = normalize(cross(up, ref));
        const vec3 right  = right0 * cos(light.rotation) + cross(up, right0) * sin(light.rotation);
        const float facing = dot(cross(up, right), -L);
        if (facing <= 0.0)
            return vec3(0.0);
        rad *= facing;
    }
    if (dot(rad, rad) < 1e-10)
        return vec3(0.0);
    if (shadowRays)
        rad *= volRayVisibility(pos, L, max(dist - 0.05, 0.02));
    return rad * volPhaseHG(dot(viewDir, L), g);
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    const ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(cell, ivec3(VOL_FROXEL_X, VOL_FROXEL_Y, VOL_FROXEL_Z))))
        return;

    // Low-discrepancy temporal sampling: an R3 sequence (generalized golden ratio) strides each froxel's
    // sample point evenly through its FULL 3D footprint over frames, offset per froxel by a spatial hash
    // so neighbors stay decorrelated for the integrate pass's spatial filter. Per-frame white noise here
    // converged fine standing still, but boiled under camera motion: reprojection resampling cuts the
    // effective history to a few frames, and a few white-noise samples have high variance where a few
    // stratified ones don't. The XY jitter additionally integrates the froxel's whole footprint, so
    // world-frequency detail (noise wisps, shadow edges) stops crawling as froxel centers slide with
    // the camera.
    const uint spatialSeed = hashU(uint(cell.x) ^ hashU(uint(cell.y) * 7919u ^ hashU(uint(cell.z) * 31u)));
    const float fseq = float(u_frameIndex & 1023u);
    const float jx = fract(hashToFloat(spatialSeed)               + fseq * 0.8191725134);
    const float jy = fract(hashToFloat(spatialSeed ^ 0x68bc21ebu) + fseq * 0.6710436067);
    const float jz = fract(hashToFloat(spatialSeed ^ 0x02e5be93u) + fseq * 0.5497004779);

    const vec2 vpUv = (vec2(cell.xy) + vec2(jx, jy)) / vec2(VOL_FROXEL_X, VOL_FROXEL_Y);
    const float viewZ = volSliceToViewZ((float(cell.z) + jz) / float(VOL_FROXEL_Z));

    // View rays from u_mvp's x/y/w ROWS (the sky.fs pattern): for a world direction d, ndc.xy =
    // (r0.d, r1.d) / (rw.d), so solving the 3x3 system {r0.d = ndc.x, r1.d = ndc.y, rw.d = 1} gives the
    // exact ray through a pixel from O(1)-magnitude rotation/projection terms - no camera translation,
    // no depth-convention dependence, and no float32 invMvp (whose error grows with the camera's
    // distance from the origin and RE-ROLLS EVERY FRAME: the previous two-point unprojection through it
    // wobbled the ray directions per frame, de-syncing the temporal reprojection into fog shimmer -
    // reversed-Z shrank the unproject baseline and amplified it). mvp is unjittered (the TAA jitter only
    // applies at rasterization). The UNJITTERED froxel-center ray feeds the temporal reprojection below -
    // reprojecting the jittered sample point would smear the per-frame jitter into the history lookup.
    const mat3 rayFromNdc = inverse(mat3(
        vec3(u_mvp[0][0], u_mvp[1][0], u_mvp[2][0]),
        vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]),
        vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3])));
    const vec2 ndc = vec2(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0);
    const vec3 dir = normalize(vec3(ndc, 1.0) * rayFromNdc);
    const vec3 camFwd = normalize(vec3(0.0, 0.0, 1.0) * rayFromNdc);
    const vec3 worldPos = u_viewPos + dir * (viewZ / max(dot(dir, camFwd), 1e-3));

    const vec2 centerUv = (vec2(cell.xy) + 0.5) / vec2(VOL_FROXEL_X, VOL_FROXEL_Y);
    const vec2 centerNdc = vec2(centerUv.x * 2.0 - 1.0, 1.0 - centerUv.y * 2.0);
    const vec3 centerDir = normalize(vec3(centerNdc, 1.0) * rayFromNdc);

    // ---- Media density + scattering albedo ------------------------------------------------------------
    // Terrain-following height fog: raise the base by a fraction of the local terrain height, so fog pools
    // in valleys, reaches partway up mountainsides and clears the peaks (follow < 1 keeps the fog top
    // rising slower than the ground under it). The height is clamped up to the baked sea level so fog
    // rests on the water surface instead of sinking over the seabed.
    float heightBase = u_fogParams0.y;
    float heightFalloff = u_fogParams0.z;
    float regionMul = 1.0;   // baked regional fog-thickness modulation (terrain data map, packed channel B)
    float waterY = -1.0e9;   // local CALM water level (world Y); everything below the wave surface is fogged
    float waterDepth = 0.0;  // water level - terrain height (shoal fade for the wave sampling)
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(worldPos.xz); // .x = terrain height, .y = water level, .w = macro altitude
        waterY = td.y + u_fogParams8.x; // "Fog/Underwater offset (m)" raises/lowers the fog boundary
        waterDepth = td.y - td.x;       // wave shoal fade keys on the REAL depth, not the offset boundary
        // Terrain follow rides the MACRO ALTITUDE channel (A, m above sea level), not the raw height:
        // the fog base tracks the smooth macro landscape, so ridge bumps don't drag the layer up with
        // them and valleys carved below the macro surface sit INSIDE the fog (the generator's
        // valley-fog thickness keys on the same carve depth). Clamped to sea level so fog rests on
        // the water instead of sinking over the seabed.
        if (u_fogParams3.x > 0.0)
            heightBase += u_fogParams3.x * (u_fogParams5.w + max(td.w, 0.0));
        if (u_fogParams6.z > 0.0)
        {
            // Regional climate: x = fog thickness (density multiplier), and the height-falloff multiplier
            // (fog hugs the ground in one region, towers in another) DERIVED from temperature - it is a
            // pure function of it, so the slot it used to be baked into is now free entirely.
            // Both eased in by Region strength.
            // Temperature is evaluated at the GROUND (td.x), not at this froxel: the fog's character comes
            // from the air over the terrain, and .z on its own is only the sea-level baseline. Using the
            // baseline directly would give a mountain valley the coast's falloff - the altitude signal is
            // the entire point of the knob.
            const vec4 climate = terrainClimateNearestAt(worldPos.xz);
            regionMul = mix(1.0, climate.x, u_fogParams6.z);
            const float groundTemp = terrainTemperatureAt(climate, td.x);
            heightFalloff *= mix(1.0, fogFalloffFromTemperature(groundTemp), u_fogParams6.z);
        }
    }
    // Height density = the analytic mean over this slice's segment of the sample ray (see heightFogMean),
    // not a point sample at worldPos.
    const float tScale = 1.0 / max(dot(dir, camFwd), 1e-3);
    const float yA = u_viewPos.y + dir.y * (volSliceToViewZ(float(cell.z) / float(VOL_FROXEL_Z)) * tScale);
    const float yB = u_viewPos.y + dir.y * (volSliceToViewZ(float(cell.z + 1) / float(VOL_FROXEL_Z)) * tScale);
    const float heightDensity = u_fogParams0.x * heightFogMean(yA, yB, heightBase, heightFalloff) * regionMul;

    // Underwater: everything at/below the LOCAL water surface is ALWAYS fogged at the global density x
    // "Fog/Underwater density" (u_fogParams6.w) - the height profile and the regional thickness only
    // shape the fog ABOVE the surface, so dipping the camera below the waterline reads as murky depth
    // regardless of the local climate (thick murk under thin haze at > 1, off at 0). The boundary is the
    // LIVE WAVE SURFACE: froxel segments inside the waterline band (u_fogParams7.y - sized CPU-side from
    // the readback's trough estimate, 0 = ocean off) sample the FFT displacement for the real wave height,
    // so fog neither pokes out of troughs nor recedes under crests; segments outside the band are
    // trivially above/below any possible wave, so only a thin shell pays for the wave taps.
    // Analytic per-slice fraction (mean of the step profile over the segment), like heightFogMean.
    const float y0 = min(yA, yB), y1 = max(yA, yB);
    float surfY = waterY;
    if (u_fogParams7.y > 0.0 && y0 < waterY + u_fogParams7.y && y1 > waterY - u_fogParams7.y)
        surfY += oceanWaveHeightAt(worldPos.xz, waterDepth, viewZ * (2.0 / float(VOL_FROXEL_Y)));
    // Underwater fog is a NEAR-FIELD effect: water absorbs everything within tens of meters, so distant
    // underwater froxels can never be legitimately seen - but the froxel grid integrates THROUGH the
    // water surface, and at range the coarse Z slices + XY bilinear leaked the dense tinted murk from
    // behind the surface into the surface pixels (blurry, shimmering distant water). Fading the whole
    // underwater treatment out by view distance removes the discontinuity the leak fed on; beyond the
    // fade the water column just carries the plain (continuous) height fog, as it did before.
    const float uwFade = 1.0 - smoothstep(100.0, 300.0, viewZ);
    const float underFrac = uwFade * clamp((surfY - y0) / max(y1 - y0, 1e-3), 0.0, 1.0);
    const float underDensity = u_fogParams0.x * u_fogParams6.w * underFrac;

    // Density noise fades out where one noise wavelength drops under the froxel footprint (sub-froxel
    // noise is pure aliasing the temporal blend turns into shimmer; its mean is 1) and is skipped
    // entirely where there is no medium to modulate - sky/above-fog froxels pay nothing.
    float noiseMul = 1.0;
    const float noiseWavelength = 1.0 / max(u_fogParams2.x, 1e-4);
    const float noiseAmp = u_fogParams2.y * (1.0 - smoothstep(40.0 * noiseWavelength, 80.0 * noiseWavelength, viewZ));
    if (noiseAmp > 0.001 && (heightDensity + underDensity > 1e-7 || in_numFogVolumes > 0u))
        noiseMul = max(1.0 + noiseAmp * (fogNoise(worldPos) * 2.0 - 1.0), 0.0);
    float density = max(heightDensity, underDensity) * noiseMul;
    // Underwater the medium is water, not air: blend the fog albedo toward the ocean's in-scatter color
    // by how submerged the slice is, so the murk reads blue-green instead of atmospheric gray.
    vec3 albedoWeighted = mix(u_fogParams1.rgb, u_oceanScatter.rgb, underFrac) * density;
    vec3 emissive = vec3(0.0);

    for (uint v = 0u; v < in_numFogVolumes; ++v)
    {
        const FogVolume fv = in_fogVolumes[v];
        const vec3 d = abs(worldPos - fv.pos);
        if (any(greaterThan(d, fv.halfExtents)))
            continue;
        // Soft box falloff over the outer edgeSoftness fraction of each half extent.
        const vec3 soft = max(fv.halfExtents * max(fv.edgeSoftness, 1e-3), vec3(1e-3));
        const vec3 t = clamp((fv.halfExtents - d) / soft, vec3(0.0), vec3(1.0));
        const float falloff = t.x * t.y * t.z;
        const float dv = fv.density * falloff * noiseMul;
        density += dv;
        albedoWeighted += fv.albedo * dv;
        emissive += fv.albedo * (fv.emissive * dv);
    }

    vec4 result = vec4(0.0);
    if (density > 1e-6)
    {
        const vec3 albedo = albedoWeighted / density;
        const float g = u_fogParams1.w;

        // Sun, with either TLAS shadow rays or a single cascade tap (matching the surface shadow mode).
        // Multiple rays are jittered in a cone (sun softness) per froxel per frame; together with the
        // temporal blend this turns the binary visibility into a smooth penumbra instead of blotches.
        // Beyond the terrain shadow distance both sources run out of data (TLAS range bound / cascade
        // extent) - distant froxels march the terrain height cascades instead: cheaper than the ray,
        // and mountains actually shadow far fog.
        const vec3 sunDir = normalize(u_sunDirection.xyz);
        float sunVis;
        if (viewZ > u_fogParams6.y && terrainHeightMapPresent())
        {
            // Froxel receivers float in air, so they need none of the surface path's self-shadow bias:
            // its own tuned 25 m start / 10 steps (~12.8 km reach), kept as-is now that the march is
            // shared with the lit surfaces (which start much further out - see u_terrainShadowParams).
            sunVis = terrainSunVisibility(worldPos, sunDir, 25.0, 10, max(u_fogParams4.w, 0.015), 4.0);
        }
        else if (u_rtSunShadow > 0.5)
        {
            const uint numRays = max(uint(u_fogParams4.x), 1u);
            const float softness = u_fogParams4.w;
            const vec3 refAxis = abs(sunDir.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            const vec3 t1 = normalize(cross(sunDir, refAxis));
            const vec3 t2 = cross(sunDir, t1);
            sunVis = 0.0;
            for (uint r = 0u; r < numRays; ++r)
            {
                // R2-sequence cone samples (same reasoning as the position jitter: stratified over the
                // temporal blend window, so the penumbra stays smooth under camera motion).
                const uint rSeed = hashU(spatialSeed ^ (r * 0x68bc21ebu));
                const float ang = fract(hashToFloat(rSeed) + fseq * 0.7548776662) * 2.0 * PI;
                const float rad = sqrt(fract(hashToFloat(rSeed ^ 0x9e3779b9u) + fseq * 0.5698402910)) * softness;
                const vec3 rayDir = normalize(sunDir + (t1 * cos(ang) + t2 * sin(ang)) * rad);
                sunVis += volRayVisibility(worldPos, rayDir, 1.0e4);
            }
            sunVis /= float(numRays);
        }
        else
            sunVis = giSunShadow(worldPos, vec3(0.0));

        // Light shafts: sunlight reaching an underwater froxel crossed the wavy surface - caustic focus
        // + Beer-Lambert absorption (underwater_light.inc.glsl) - weighted by the slice's SUBMERGED
        // fraction, never a binary test at the jittered sample point: straddling froxels flipped between
        // air and water lighting per frame, and the temporal blend smeared that flicker across every
        // distant water surface. Depth = the submerged part's midpoint (y0/y1 are slice bounds, jitter-
        // free). The sun phase also steepens underwater: water forward-scatters far harder than haze
        // (g -> ~0.65) - without the forward lobe the focused columns barely brighten toward the sun
        // and no shafts read at any strength.
        vec3 sunTrans = vec3(1.0);
        float gSun = g;
        if (underFrac > 0.0)
        {
            const float depthMid = max((surfY - y0) - 0.5 * underFrac * (y1 - y0), 0.05);
            // "Fog/Shaft boost" (u_fogParams7.x): non-physical gain on the underwater sun in-scatter -
            // at fog-scale densities the physically correct shaft radiance is too faint to read. Its
            // sqrt also feeds the helper's REACH, so boosting brightness stretches shaft length too.
            sunTrans = mix(vec3(1.0),
                underwaterSunTransmittance(worldPos.xz, depthMid, viewZ * (2.0 / float(VOL_FROXEL_Y)),
                    u_fogParams7.x,
                    (waterY - u_fogParams8.x) - (surfY - depthMid), // calm column depth at the submerged midpoint
                    waterY - u_fogParams8.x                          // the calm level itself (waterY carries the fog boundary offset)
                    ), underFrac);
            gSun = mix(g, 0.78, underFrac); // strong forward lobe: ~8x gain toward the sun
        }
        vec3 inLight = atmosTransmittanceToLight(0.0, sunDir, u_skyUp) * u_sunColor.rgb * (volPhaseHG(dot(dir, sunDir), gSun) * sunVis * u_eclipseParams.x) * sunTrans;

        // Ambient: GI probe irradiance toward the camera (toggleable; the clipmap lookup is the next
        // biggest cost after the shadow rays), fading to the analytic sky over the probe field's outer
        // band (coverage) instead of stepping at its boundary. For an SH-L1 field the isotropically
        // in-scattered radiance is E_mean / PI; E(-dir) is the single-sample stand-in for E_mean.
        // u_ambientColor is an isotropic radiance, so its phase integral is just itself.
        float giCov = 0.0;
        vec3 amb = vec3(0.0);
        if (u_fogParams4.z > 0.5)
        {
            amb = evalProbeSHCoverage(worldPos, -dir, giCov);
            if (amb.x < 0.0)
                amb = vec3(0.0);
        }
        if (giCov < 1.0)
            amb = mix(giEvalSkySH(-dir), amb, giCov);
        inLight += amb * u_aoParams.y / PI + u_ambientColor;

        // Local lights from the world-space hash grid cell containing this froxel.
        const bool lightShadows = u_fogParams3.w > 0.5;
        const ivec3 gridPos = getGridPos(worldPos);
        uint tableIdx = getTableIdx(gridPos);
        while (true)
        {
            const uint gridIdx = getGridIdx(tableIdx);
            if (gridIdx == EMPTY_ENTRY)
                break;
            const ivec3 gridMin = getGridMin(gridIdx);
            if (gridMin == gridPos)
            {
                const uint numLargeLights = getLargeLightCount(gridIdx);
                for (uint i = 0u; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
                    inLight += volLightScatter(in_lightInfos[getLargeLightId(gridIdx, i)], worldPos, dir, g, lightShadows);

                const uint cellOffset = calcCellOffset(gridIdx, gridMin, worldPos);
                const uint numLights  = getNumLightsForCell(cellOffset);
                for (uint i = 0u; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                    inLight += volLightScatter(in_lightInfos[getLightId(cellOffset, i)], worldPos, dir, g, lightShadows);
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }

        // rgb = in-scattered radiance per meter, a = extinction (1/m); integrated analytically per slice.
        result = vec4(albedo * density * inLight + emissive, density);
    }

    // ---- Temporal blend against last frame's reprojected froxel ---------------------------------------
    // Reproject from the UNJITTERED froxel center (centerDir, not the jittered sample ray).
    const float centerViewZ = volSliceToViewZ((float(cell.z) + 0.5) / float(VOL_FROXEL_Z));
    const vec3 centerPos = u_viewPos + centerDir * (centerViewZ / max(dot(centerDir, camFwd), 1e-3));
    float prevW;
    const vec2 prevFullUv = prevScreenUV(centerPos, prevW);
    const vec2 prevVpUv = (prevFullUv - u_viewportRect.xy) / u_viewportRect.zw;
    // Accept history up to a couple of froxels OUTSIDE the grid with a clamped lookup: the edge froxel is
    // a close stand-in, and rejecting outright made the screen border flash fresh (noisy) samples during
    // every small rotation. Larger overshoots still reject (stretching the edge across the screen is worse).
    const vec2 margin = vec2(2.0 / float(VOL_FROXEL_X), 2.0 / float(VOL_FROXEL_Y));
    if (prevW > VOL_FOG_NEAR
        && all(greaterThanEqual(prevVpUv, -margin)) && all(lessThanEqual(prevVpUv, 1.0 + margin)))
    {
        const float prevSlice = volViewZToSlice(prevW);
        if (prevSlice <= 1.0)
        {
            const vec4 history = texture(u_history, vec3(clamp(prevVpUv, vec2(0.0), vec2(1.0)), prevSlice));
            result = mix(result, history, clamp(u_fogParams2.w, 0.0, 0.97));
        }
    }

    imageStore(u_outScatter, cell, result);
}
