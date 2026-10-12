#version 460

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : enable // the GI volume's per-cascade texture index

// Volumetric fog scatter: one invocation per froxel. The density (height fog with wind noise, underwater murk, local
// fog volume boxes) and the light scattered toward the camera: the sun (TLAS rays, a cascade tap or the terrain
// march), GI ambient and the light-grid lights. Temporally blended with last frame's reprojected froxel (the sample
// point is jittered per frame, so the blend integrates the froxel). vol_integrate.cs.glsl then accumulates along Z.

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
// FFT ocean maps (u_uwOceanMaps): the underwater fog boundary samples the LIVE wave height so fog never pokes out of a
// trough, and the underwater sun crosses the same surface (underwater_light.inc.glsl).
#define UNDERWATER_OCEAN_BINDING 11
#include "underwater_light.inc.glsl"

// THE INLAND WATER MAP (RendererVKLayout::RiverWaterMapGpu, baked by RiverSystem around the camera): the river / lake
// surface Y per texel, RIVER_WATER_NONE elsewhere (the terrain map's water channel is the sea's only). Nearest texel:
// a bilinear would mix in the "none" texels at a bank.
layout (binding = 14, std430) readonly buffer InRiverWaterMap
{
    vec2 rw_origin;
    float rw_invTexel;
    uint rw_dim;
    float rw_height[];
};

float riverWaterAt(vec2 worldXZ)
{
    if (rw_dim == 0u)
        return -1.0e9;

    const ivec2 t = ivec2(floor((worldXZ - rw_origin) * rw_invTexel));
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(int(rw_dim)))))
        return -1.0e9;
    return rw_height[uint(t.y) * rw_dim + uint(t.x)];
}

// The cloud shadow map: fog under (and in) a cloud is in shadow; the gaps cast the shafts.
#define CLOUD_SHADOW_BINDING 13
#include "cloud_shadow.inc.glsl"

// Light grid (read) + giSquareFalloff/giSunShadow from the shared lighting helpers.
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"
#include "lighting.inc.glsl"

// GI probe clipmap (read-only): fog picks up bounced/ambient light.
#define GI_GRID_DATA_NAME gi_gridData
#ifdef GI_VOLUME
layout (binding = 12) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES]; // the baked irradiance volume + sky SH
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
#include "gi_probe.inc.glsl"

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
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Two-octave value noise in [0,1], drifting with the wind ("Sky/Wind") and lifting a little: wispy density.
float fogNoise(vec3 worldPos)
{
    const vec3 wind = vec3(u_weather_windDirection.x, 0.12, u_weather_windDirection.y) * (u_weather_windSpeed * u_timeSeconds);
    const vec3 p = (worldPos - wind) * u_fog_noiseScale; // noise space = world - drift: the wisps travel WITH the wind
    return valueNoise(p) * 0.667 + valueNoise(p * 2.37 + vec3(17.3)) * 0.333;
}

// Closed-form mean of exp(-max(y - base, 0) * falloff) over one slice's ray segment. A point sample converges to it
// only after many frames; under camera motion the partly converged slices disagree, which reads as view-aligned
// density layers. The mean is exact every frame (only the lighting stays stochastic). MUST MATCH vol_fog.inc.glsl's
// far-field profile.
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

// Vertical FFT wave displacement at worldXZ (m, the cascades' sum, shoal-faded like the drawn surface; no chop: sub-
// froxel). The mip matches the froxel footprint, band-limiting the boundary to what the grid can represent.
float oceanWaveHeightAt(vec2 worldXZ, float waterDepth, float footprint)
{
    float h = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_ocean_cascadeSizes[c];
        const float fade = smoothstep(0.0, u_ocean_shoalFadeDepth[c], waterDepth);
        const float lod = max(log2(footprint * float(OCEAN_FFT_SIZE) / L), 0.0);
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

// Radiance scattered toward the camera from one grid light: the surface path's type encoding (point / spot / rect /
// tube) and falloff, with the HG phase in place of NdotL / BRDF.
vec3 volLightScatter(LightInfo light, vec3 pos, vec3 viewDir, float g, bool shadowRays)
{
    const vec3 toLight = light.pos - pos;
    const float dist = length(toLight);
    const vec3 L = toLight / max(dist, 1e-4);

    float shape = 1.0; // spot cone / rect facing factor; the falloff is applied after the early-out
    if (light.width < 0.0) // spot: rotation = cone half-angle, |direction| = edge softness
    {
        const float softness = length(light.direction);
        const float cosAngle = dot(-L, light.direction / softness);
        const float cosOuter = cos(light.rotation);
        const float cosInner = mix(cosOuter, 1.0, softness);
        shape = smoothstep(cosOuter, cosInner, cosAngle);
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
        shape = facing;
    }

    vec3 rad = light.color * (giSquareFalloff(dist, abs(light.range)) * shape);
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

    // Low-discrepancy temporal sampling: an R3 sequence strides each froxel's sample evenly through its FULL 3D footprint
    // over frames, offset per froxel by a spatial hash (neighbours decorrelated for the integrate's spatial filter).
    // Under camera motion the history is only a few frames, and a few white-noise samples boil where stratified ones do
    // not. The XY jitter also integrates the footprint, so wisps and shadow edges do not crawl as froxels slide.
    const uint spatialSeed = hashU(uint(cell.x) ^ hashU(uint(cell.y) * 7919u ^ hashU(uint(cell.z) * 31u)));
    const float fseq = float(u_frameIndex & 1023u);
    const float jx = fract(hashToFloat(spatialSeed)               + fseq * 0.8191725134);
    const float jy = fract(hashToFloat(spatialSeed ^ 0x68bc21ebu) + fseq * 0.6710436067);
    const float jz = fract(hashToFloat(spatialSeed ^ 0x02e5be93u) + fseq * 0.5497004779);

    const vec2 vpUv = (vec2(cell.xy) + vec2(jx, jy)) / vec2(VOL_FROXEL_X, VOL_FROXEL_Y);
    const float viewZ = volSliceToViewZ((float(cell.z) + jz) / float(VOL_FROXEL_Z));

    // View rays from u_rayFromNdc (ubo.inc.glsl): exact, translation-free, and not through the float32 invMvp, whose
    // error RE-ROLLS EVERY FRAME (wobbling rays de-sync the reprojection into shimmer). Unjittered (TAA jitter is a
    // raster thing). The temporal reprojection below uses the UNJITTERED froxel centre, not this jittered sample.
    const vec2 ndc = vec2(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0);
    const vec3 dir = normalize(vec3(ndc, 1.0) * u_rayFromNdc);
    const vec3 worldPos = u_viewPos + dir * (viewZ / dot(dir, u_viewForward));

    // ---- Media density + scattering albedo ----
    // Terrain-following height fog: the base rises by a fraction of the terrain height, so fog pools in valleys, reaches
    // partway up mountainsides and clears the peaks (follow < 1: the fog top rises slower than the ground).
    float heightBase = u_fog_heightBase;
    float heightFalloff = u_fog_heightFalloff;
    float regionMul = 1.0;   // baked regional fog-thickness modulation (terrain data map, packed channel B)
    float waterY = -1.0e9;   // local CALM water level (world Y); everything below the wave surface is fogged
    float waterDepth = 0.0;  // water level - terrain height (shoal fade for the wave sampling)
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(worldPos.xz); // .x = terrain height, .y = water level, .w = macro altitude
        waterY = td.y + u_fog_boundaryOffset; // the fog boundary, lowered by "Fog/Underwater wave offset" x the live wave trough
        waterDepth = td.y - td.x;       // wave shoal fade keys on the REAL depth, not the offset boundary

        // Terrain follow rides the MACRO ALTITUDE (A, m above sea level), not the raw height: ridge bumps do not drag
        // the layer up, and valleys carved below the macro surface sit INSIDE the fog. Clamped to sea level so fog
        // rests on the water instead of sinking over the seabed.
        heightBase += u_fog_terrainFollow * (u_terrain_mapSeaLevel + max(td.w, 0.0));
        {
            // Regional climate, eased in by Region strength: x = fog thickness, and the height-falloff multiplier
            // derived from the temperature at the GROUND (td.x), not the sea-level baseline .z: a mountain valley must
            // not get the coast's falloff.
            const vec4 climate = terrainClimateNearestAt(worldPos.xz);
            regionMul = mix(1.0, climate.x, u_fog_regionStrength);
            const float groundTemp = terrainTemperatureAt(climate, td.x);
            heightFalloff *= mix(1.0, fogFalloffFromTemperature(groundTemp), u_fog_regionStrength);
        }
    }

    // Height density = the analytic mean over this slice's segment of the sample ray, not a point sample.
    const float tScale = 1.0 / dot(dir, u_viewForward);
    const float yA = u_viewPos.y + dir.y * (volSliceToViewZ(float(cell.z) / float(VOL_FROXEL_Z)) * tScale);
    const float yB = u_viewPos.y + dir.y * (volSliceToViewZ(float(cell.z + 1) / float(VOL_FROXEL_Z)) * tScale);
    const float heightDensity = u_fog_density * heightFogMean(yA, yB, heightBase, heightFalloff) * regionMul;

    // Underwater: everything below the LOCAL water surface is ALWAYS fogged at the global density x "Fog/Underwater
    // density" (the height profile and region shape only the fog ABOVE it), whatever the local climate. The boundary is
    // the LIVE WAVE SURFACE: segments inside the waterline band (u_fog_waveBand, sized on the CPU from the trough
    // estimate, 0 = ocean off) sample the FFT height; outside it they are above / below any wave, so only a thin shell
    // pays for the wave taps. The submerged fraction is analytic per slice, like heightFogMean.
    const float y0 = min(yA, yB), y1 = max(yA, yB);
    float surfY = waterY;
    // A river or lake above the sea's level here (the inland water map) is the boundary instead: its calm surface (small
    // waves, no ocean taps), and its own colour below.
    const float inlandY = riverWaterAt(worldPos.xz);
    const bool inland = inlandY > waterY;
    if (inland)
        surfY = inlandY;
    else if (u_fog_waveBand > 0.0 && y0 < waterY + u_fog_waveBand && y1 > waterY - u_fog_waveBand)
        surfY += oceanWaveHeightAt(worldPos.xz, waterDepth, viewZ * (2.0 / float(VOL_FROXEL_Y)));

    // Underwater fog is NEAR-FIELD only: water absorbs everything within tens of metres, but the grid integrates
    // THROUGH the surface, and at range the coarse slices leak the dense murk behind it into the surface pixels
    // (blurry, shimmering distant water). Past the fade the water column carries the plain height fog.
    const float uwFade = 1.0 - smoothstep(100.0, 300.0, viewZ);
    const float underFrac = uwFade * clamp((surfY - y0) / max(y1 - y0, 1e-3), 0.0, 1.0);
    // Inland murk: "Fog/Lake underwater density", x "River underwater scale" for a river (the height's lowest bit:
    // 1 = a river channel - RiverTerrain::sampleInlandWaterGrid).
    const bool riverWater = inland && (floatBitsToUint(inlandY) & 1u) != 0u;
    const float inlandDensity = u_fog_inlandUnderwaterDensity * (riverWater ? u_fog_riverUnderwaterScale : 1.0);
    const float underDensity = u_fog_density * (inland ? inlandDensity : u_fog_underwaterDensity) * underFrac;

    // Density noise fades out where a wavelength drops under the froxel footprint (sub-froxel noise is aliasing that the
    // temporal blend turns into shimmer; its mean is 1), and is skipped where there is no medium to modulate.
    float noiseMul = 1.0;
    const float noiseWavelength = u_fog_noiseWavelength;
    const float noiseAmp = u_fog_noiseStrength * (1.0 - smoothstep(40.0 * noiseWavelength, 80.0 * noiseWavelength, viewZ));
    if (noiseAmp > 0.001 && (heightDensity + underDensity > 1e-7 || in_numFogVolumes > 0u))
        noiseMul = 1.0 + noiseAmp * (fogNoise(worldPos) * 2.0 - 1.0);
    float density = max(heightDensity, underDensity) * noiseMul;

    // Underwater the medium is water: the albedo blends toward the water's in-scatter colour by the submerged fraction.
    vec3 albedoWeighted = mix(u_fog_albedo, inland ? u_river_scatterColor : u_ocean_scatterColor, underFrac) * density;
    vec3 emissive = vec3(0.0);

    for (uint v = 0u; v < in_numFogVolumes; ++v)
    {
        const FogVolume fv = in_fogVolumes[v];
        const vec3 d = abs(worldPos - fv.pos);
        if (any(greaterThan(d, fv.halfExtents)))
            continue;

        // Soft box falloff over the outer edgeSoftness fraction of each half extent.
        const vec3 soft = fv.halfExtents * fv.edgeSoftness;
        const vec3 t = clamp((fv.halfExtents - d) / soft, vec3(0.0), vec3(1.0));
        const float falloff = t.x * t.y * t.z;
        const float dv = fv.density * falloff * noiseMul;
        density += dv;
        albedoWeighted += fv.albedo * dv;
        emissive += fv.albedo * (fv.emissive * dv);
    }

    // THE SHAFT HAZE ("Fog/Shaft haze"): a thin medium for the god rays alone. The height fog is nearly gone a few tens of
    // metres up, so cloud shafts would need a base density that fogs the world. The haze reaches the clouds (its own
    // scale height) and adds SUNLIT in-scatter only: no extinction, no ambient, so the lit / shadowed contrast is the
    // shaft (non-physical on purpose). Its sun visibility is the fog's where the fog computed one, else the clouds' alone.
    // REGISTERS: its density is taken HERE and its radiance joins `emissive` once the sun visibility is known, so the
    // slice bounds and the submerged fraction are not live across the shadow rays and the light loop.
    const float hazeSigma = underFrac < 1.0
        ? u_fog_hazeDensity * heightFogMean(yA, yB, u_fog_heightBase, u_fog_hazeInvHeight) * (1.0 - underFrac) : 0.0;
    const vec3 sunDir = u_sunDirection.xyz;
    const vec3 hazeRadiance = u_fog_albedo * u_sunTransmittance * u_sunColor.rgb
        * (volPhaseHG(dot(dir, sunDir), u_fog_anisotropy) * u_sunVisible * u_fog_sunScatter); // x hazeSigma x the sun visibility

    vec4 result = vec4(0.0);
    if (density > 1e-6)
    {
        const float g = u_fog_anisotropy;

        // Sun: TLAS shadow rays or one cascade tap (the surface shadow mode). The rays are jittered in a cone (sun
        // softness) per frame; with the temporal blend that is a smooth penumbra. Past the terrain shadow distance both
        // run out of data, so distant froxels march the terrain height cascades (cheaper, and mountains shadow far fog).
        float sunVis;
        if (viewZ > u_fog_terrainShadowDistance && terrainHeightMapPresent())
        {
            // Froxels float in air, so no self-shadow bias: a 25 m start / 10 steps (~12.8 km reach), unlike the lit
            // surfaces (u_shadow_terrainMarchStart).
            sunVis = terrainSunVisibility(worldPos, sunDir, 25.0, 10, u_fog_sunSoftness, 4.0);
        }
        else if (u_rt_sunShadow > 0.5)
        {
            const uint numRays = uint(u_fog_sunRays);
            const float softness = u_fog_sunSoftness;
            const vec3 refAxis = abs(sunDir.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            const vec3 t1 = normalize(cross(sunDir, refAxis));
            const vec3 t2 = cross(sunDir, t1);
            sunVis = 0.0;
            for (uint r = 0u; r < numRays; ++r)
            {
                // R2-sequence cone samples: stratified over the blend window, like the position jitter.
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

        const float cloudVis = cloudSunTransmittanceBilinear(worldPos);
        sunVis *= cloudVis;
        emissive += hazeRadiance * (hazeSigma * sunVis);

        // Light shafts: sunlight reaching an underwater froxel crossed the wavy surface (caustic focus + Beer-Lambert,
        // underwater_light.inc.glsl), weighted by the SUBMERGED fraction - a binary test at the jittered sample point
        // flips straddling froxels per frame, and the blend smears that flicker over distant water. Depth = the
        // submerged part's midpoint (jitter-free). Water forward-scatters far harder than haze: without the steeper
        // lobe no shafts read at any strength.
        vec3 sunTrans = vec3(1.0);
        float gSun = g;
        if (underFrac > 0.0)
        {
            const float depthMid = max((surfY - y0) - 0.5 * underFrac * (y1 - y0), 0.05);
            // "Fog/Shaft boost": non-physical gain (the physical shaft radiance is too faint at fog densities); its
            // sqrt also stretches the helper's REACH. calmY: the inland water's level, else the sea's (waterY carries
            // the fog boundary offset).
            const float calmY = inland ? inlandY : waterY - u_fog_boundaryOffset;
            sunTrans = mix(vec3(1.0), underwaterSunTransmittance(worldPos.xz, depthMid, viewZ * (2.0 / float(VOL_FROXEL_Y)), u_fog_shaftBoost,
                                                                 calmY - (surfY - depthMid), // calm column depth at the submerged midpoint
                                                                 calmY), underFrac);
            gSun = mix(g, 0.78, underFrac); // strong forward lobe: ~8x gain toward the sun
        }

        // "Fog/Sun scatter": a gain on the SUN term only - the shafts brighten, shadowed froxels keep their ambient,
        // the extinction is unchanged.
        vec3 inLight = u_sunTransmittance * u_sunColor.rgb * (volPhaseHG(dot(dir, sunDir), gSun) * sunVis * u_sunVisible * u_fog_sunScatter) * sunTrans;

        // Ambient: GI irradiance toward the camera (toggleable: the clipmap lookup is the next biggest cost after the
        // shadow rays), fading to the analytic sky over the probe field's outer band. For SH-L1 the isotropic in-scatter
        // is E_mean / PI; E(-dir) stands in for E_mean. u_ambientColor is an isotropic radiance (its phase integral is
        // itself). The sky SH's sunlit-ground part is cloud-shadowed here (giEvalSkySHCloud).
        inLight += u_ambientColor;
        if (u_rt_giStrength > 0.0)
            inLight += (u_fog_giAmbient > 0.5 ? giIrradiance(worldPos, -dir) : giEvalSkySHCloud(-dir, cloudVis)) * (u_rt_giStrength / PI);
        // rgb = in-scatter per metre (albedoWeighted = albedo x density), a = extinction (1/m). REGISTERS: folded BEFORE
        // the light loop, so only one accumulator is live across its shadow rays.
        result = vec4(albedoWeighted * inLight + emissive, density);

        // Local lights from the world-space hash grid cell containing this froxel.
        const bool lightShadows = u_fog_lightShadows > 0.5;
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
                // ONE loop over the large lights and the cell's: each volLightScatter inlines a shadow ray query.
                const uint numLargeLights = getLargeLightCount(gridIdx);
                const uint cellOffset     = calcCellOffset(gridIdx, gridMin, worldPos);
                const uint numLights      = numLargeLights + getNumLightsForCell(cellOffset);
                for (uint i = 0u; i < numLights; ++i)
                {
                    const uint lightId = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
                    result.rgb += albedoWeighted * volLightScatter(in_lightInfos[lightId], worldPos, dir, g, lightShadows);
                }
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }
    }
    else if (hazeSigma > 0.0)
        result.rgb = hazeRadiance * (hazeSigma * cloudSunTransmittanceBilinear(worldPos));

    // ---- Temporal blend against last frame's reprojected froxel ----
    // Reproject from the UNJITTERED froxel centre. REGISTERS: built here, not with the sample ray, so nothing of it is
    // live across the shadow rays.
    const ivec3 cellOut = ivec3(gl_GlobalInvocationID); // re-read, not kept live
    const vec2 centerUv = (vec2(cellOut.xy) + 0.5) / vec2(VOL_FROXEL_X, VOL_FROXEL_Y);
    const vec2 centerNdc = vec2(centerUv.x * 2.0 - 1.0, 1.0 - centerUv.y * 2.0);
    const vec3 centerDir = normalize(vec3(centerNdc, 1.0) * u_rayFromNdc);
    const float centerViewZ = volSliceToViewZ((float(cellOut.z) + 0.5) / float(VOL_FROXEL_Z));
    const vec3 centerPos = u_viewPos + centerDir * (centerViewZ / dot(centerDir, u_viewForward));
    float prevW;
    const vec2 prevFullUv = prevScreenUV(centerPos, prevW);
    const vec2 prevVpUv = (prevFullUv - u_viewportRect.xy) * u_viewportInvSize;
    // Accept history up to two froxels OUTSIDE the grid with a clamped lookup: the edge froxel is a close stand-in, and
    // rejecting makes the screen border flash noisy fresh samples on every small rotation. Larger overshoots reject.
    const vec2 margin = vec2(2.0 / float(VOL_FROXEL_X), 2.0 / float(VOL_FROXEL_Y));
    if (prevW > VOL_FOG_NEAR && all(greaterThanEqual(prevVpUv, -margin)) && all(lessThanEqual(prevVpUv, 1.0 + margin)))
    {
        const float prevSlice = volViewZToSlice(prevW);
        if (prevSlice <= 1.0)
        {
            const vec4 history = texture(u_history, vec3(clamp(prevVpUv, vec2(0.0), vec2(1.0)), prevSlice));
            result = mix(result, history, u_fog_temporalBlend);
        }
    }

    imageStore(u_outScatter, cellOut, result);
}
