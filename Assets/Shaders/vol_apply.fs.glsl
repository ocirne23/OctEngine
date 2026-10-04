#version 450

#extension GL_EXT_nonuniform_qualifier : enable // gi_probe.inc.glsl's volume lookup (only the sky SH is used here)

// Volumetric fog apply: fullscreen pass in the scene-color render pass (after the forward + sky draws,
// before TAA so the fog gets antialiased). Reconstructs each pixel's view depth from the scene depth
// and samples the integrated fog volume.
// Blended with (srcColor = ONE, dstColor = SRC_ALPHA): out = inScatter + sceneColor * transmittance.
//
// Everything beyond the froxel volume's far plane is added analytically by volFarField instead of by more
// slices, so Fog/Range is a near-field quality knob rather than a view distance.
//
// THE CLOUDS ARE COMPOSITED HERE while the fog is on (the separate cloud apply runs only with the fog off):
// fog laid over the finished clouds would fog them as if they stood at the scene depth - over far terrain,
// tens of km of fog in front of a cloud a few km away. With the cloud's in-scatter S and transmittance T at
// distance tc, fog (in-scatter, transmittance) to the scene F and to the cloud C:
//   out = T * F.rgb + C.a * S + (1 - T) * C.rgb  +  scene * (T * F.a)
// exact for a cloud at one distance (fog in front of it over it, the rest behind it), and linear in the
// scene, so the blend state stays the same.
// THE FAR-TREE VOLUME is a second such layer (TreeVolumePipeline; u_foliageParams4.w = it marched this frame), at
// its weighted mean distance - not in the scene depth, so fogged at the terrain BEHIND it the trees read twice as
// hazy as the billboards next to them. Two layers compose front to back by distance: with each layer's part
// P = (1 - T) F_layer.rgb + F_layer.a S and transmittance T, the nearer one's P + T x the farther one's.

#include "shared.inc.glsl"
#include "vol_fog.inc.glsl"
#include "cloud_upsample.inc.glsl"

layout (location = 0) in vec2 v_uv;
layout (binding = 1) uniform sampler2D u_depth;
layout (binding = 2) uniform sampler3D u_integrated;
layout (binding = 4, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };
layout (binding = 6) uniform sampler2D u_cloudColor; // the accumulated clouds (half res), this eye / slot
layout (binding = 7) uniform sampler2D u_cloudDepth;
layout (binding = 9) uniform sampler2D u_farTreesColor;  // the far-tree volume's march (full res): in-scatter, T
layout (binding = 10) uniform sampler2D u_farTreesDepth; // its weighted mean distance (m)

#define TERRAIN_HEIGHT_BINDING 3
#include "terrain_height.inc.glsl"
#define GI_GRID_DATA_NAME gi_gridData
#ifdef GI_VOLUME
layout (binding = 5) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES]; // the volume's copy of the sky SH (giEvalSkySH)
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
#include "gi_probe.inc.glsl"
// The cloud shadow map: the far field's sun term is shadowed by the clouds - the light shafts past the froxel
// volume (whose froxels shadow themselves, vol_scatter.cs.glsl).
#define CLOUD_SHADOW_BINDING 8
#include "cloud_shadow.inc.glsl"

layout (location = 0) out vec4 out_color;

// View index (0 = centre/desktop, 1 = left eye, 2 = right eye); selects the per-eye depth reconstruction.
// The fog volume itself is built once for the centre view, sampled here at each eye's reconstructed world pos.
layout (push_constant) uniform ViewPC { uint u_viewIndex; };

// (fog base world Y, terrain height) at worldXZ - the terrain-follow datum vol_scatter builds per froxel,
// with the macro altitude weighted by `follow` (0 = the layer sits at sea level).
vec2 volFarFieldGround(vec2 worldXZ, float follow)
{
    const vec4 d = terrainDataAt(worldXZ);
    return vec2(u_fogParams0.y + u_fogParams3.x * (u_fogParams5.w + max(d.w, 0.0) * follow), d.x);
}

// Height fog over the ray segment [t0, t1] (t0 = the froxel volume's far plane), in the same
// (in-scatter, transmittance) form the integrated volume uses so the two compose.
//
// The ground is sampled at a few points and taken as linear between them, which keeps the optical depth
// exactly closed-form per sub-segment (a linear ground just subtracts its slope from the ray's). Nothing
// samples density, so there is no integration error: step count only sets how finely the km-scale ground
// profile is tracked.
//
// Terrain follow and the regional fields both FADE OUT along the ray. They are local properties, while the
// tail past the march is unbounded and inherits the last sub-segment - at full strength one sample governs
// an infinite integral, so a zero-fog region erases the horizon along its azimuth and a high macro altitude
// puts the base above the ray, filling the sky over that spot with a column of fog. Full weight at t0 keeps
// the seam with the froxel volume exact; by `reach` both have settled to the global medium at sea level.
// The clouds' mean sun transmittance over [a, b] along the ray: VOL_FAR_VIS_TAPS jittered taps of the cloud shadow
// map (past its far cascade it returns the layer's mean). The jitter moves every frame; the TAA resolves it.
#define VOL_FAR_VIS_TAPS 2
float volFarSunVis(vec3 dir, float a, float b, float jitter)
{
#ifdef CLOUD_SHADOWS
    if (u_cloudShadow4.x < 0.5)
        return 1.0;
    float v = 0.0;
    for (int j = 0; j < VOL_FAR_VIS_TAPS; ++j)
        v += cloudSunTransmittanceBilinear(u_viewPos + dir * mix(a, b, (float(j) + jitter) / float(VOL_FAR_VIS_TAPS)));
    return v / float(VOL_FAR_VIS_TAPS);
#else
    return 1.0;
#endif
}

// The SHAFT HAZE over [a, b] (vol_scatter.cs.glsl has why): its share of the sunlit in-scatter - behind the fog in
// front of it (fogTau) and its own haze in front (hazeTau; its OWN extinction bounds a level ray to the horizon, it
// does not dim the scene) - times the clouds' visibility over the piece. Flat-based, the fog's height base.
float volFarHazeStep(vec3 dir, float a, float b, float fogTau, inout float hazeTau, float vis)
{
    if (u_fogParams10.x <= 0.0)
        return 0.0;
    const float seg = volAnalyticOpticalDepth(u_viewPos, dir, a, b, u_fogParams0.y, u_fogParams10.y, u_fogParams10.x);
    const float w = exp(-fogTau - hazeTau) * (1.0 - exp(-seg)) * vis;
    hazeTau += seg;
    return w;
}

vec4 volFarField(vec3 dir, float t0, float t1)
{
    const float density = u_fogParams0.x * u_fogParams9.y;
    if ((density <= 1e-7 && u_fogParams10.x <= 0.0) || t1 <= t0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    const float falloff = u_fogParams0.z * u_fogParams9.z;
    const float tauOpaque = 12.0; // transmittance < 1e-5
    const bool followsTerrain = terrainHeightMapPresent() && u_fogParams3.x > 0.0;
    // THE SUN IS SHADOWED BY THE CLOUDS per sub-segment (the light shafts): each adds its share of the in-scatter,
    // T before it x (1 - its own T), times the clouds' sun visibility over it. The ambient stays one constant.
    const float visJitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(u_frameIndex & 63u), vec2(0.06711056, 0.00583715))));
    const float visTail = 20000.0; // the tail's taps span this much past the march (the shadow map covers far less)
    float sunW = 0.0;
    float hazeW = 0.0, hazeTau = 0.0; // the shaft haze's sunlit share, and its own optical depth so far

    float tau;
    if (!followsTerrain)
    {
        // Split so the visibility can vary along the ray; each piece stays closed-form.
        const float tEnd = min(t1, t0 + visTail);
        const int steps = max(int(u_fogParams9.w), 1);
        float tPrev = t0;
        tau = 0.0;
        for (int i = 1; i <= steps; ++i)
        {
            const float tNext = mix(t0, tEnd, float(i) / float(steps));
            const float seg = volAnalyticOpticalDepth(u_viewPos, dir, tPrev, tNext, u_fogParams0.y, falloff, density);
            const float vis = volFarSunVis(dir, tPrev, tNext, visJitter);
            sunW += exp(-tau) * (1.0 - exp(-seg)) * vis;
            hazeW += volFarHazeStep(dir, tPrev, tNext, tau, hazeTau, vis);
            tau += seg;
            if (tau > tauOpaque)
                break;
            tPrev = tNext;
        }
        if (t1 > tEnd && tau <= tauOpaque)
        {
            const float seg = volAnalyticOpticalDepth(u_viewPos, dir, tEnd, t1, u_fogParams0.y, falloff, density);
            const float vis = volFarSunVis(dir, tEnd, min(t1, tEnd + visTail), visJitter);
            sunW += exp(-tau) * (1.0 - exp(-seg)) * vis;
            hazeW += volFarHazeStep(dir, tEnd, t1, tau, hazeTau, vis);
            tau += seg;
        }
    }
    else
    {
        // Only march where the cascades hold ground data; beyond that the map clamps to its edge, so the
        // remainder is constant-ground and solves in one step (this is also what keeps sky rays, t1 =
        // VOL_FAR_INFINITY, from spreading their steps across 10,000 km).
        const float reach = (u_fogParams5.z > 0.0) ? 0.5 / u_fogParams5.z : 0.5 / u_fogParams3.y;
        const float tEnd = min(t1, t0 + reach);
        const int steps = max(int(u_fogParams9.w), 1);

        vec2 gPrev = volFarFieldGround(u_viewPos.xz + dir.xz * t0, 1.0);
        float tPrev = t0;
        tau = 0.0;
        float dens = density; // last sub-segment's medium; the tail continues on it
        float k = falloff;

        for (int i = 1; i <= steps; ++i)
        {
            const float tNext = mix(t0, tEnd, float(i) / float(steps));
            const vec2 gNext = volFarFieldGround(u_viewPos.xz + dir.xz * tNext,
                                                 1.0 - smoothstep(0.25 * reach, reach, tNext - t0));
            const float len = tNext - tPrev;

            const float midT = 0.5 * (tPrev + tNext);
            const float wRegion = u_fogParams6.z * (1.0 - smoothstep(0.0, reach, midT - t0));
            dens = density;
            k = falloff;
            if (wRegion > 0.001) // regional fields, as vol_scatter applies them per froxel
            {
                const vec4 climate = terrainClimateNearestAt(u_viewPos.xz + dir.xz * midT);
                dens *= mix(1.0, climate.x, wRegion);
                k *= mix(1.0, fogFalloffFromTemperature(terrainTemperatureAt(climate, 0.5 * (gPrev.y + gNext.y))), wRegion);
            }

            const float seg = volAnalyticOpticalDepthLinear(u_viewPos.y + dir.y * tPrev - gPrev.x,
                                                            dir.y - (gNext.x - gPrev.x) / max(len, 1e-3),
                                                            len, k, dens);
            const float vis = volFarSunVis(dir, tPrev, tNext, visJitter);
            sunW += exp(-tau) * (1.0 - exp(-seg)) * vis;
            hazeW += volFarHazeStep(dir, tPrev, tNext, tau, hazeTau, vis);
            tau += seg;
            if (tau > tauOpaque)
                break;

            tPrev = tNext;
            gPrev = gNext;
        }

        if (t1 > tEnd && tau <= tauOpaque)
        {
            const float seg = volAnalyticOpticalDepthLinear(u_viewPos.y + dir.y * tEnd - gPrev.x, dir.y, t1 - tEnd, k, dens);
            const float vis = volFarSunVis(dir, tEnd, min(t1, tEnd + visTail), visJitter);
            sunW += exp(-tau) * (1.0 - exp(-seg)) * vis;
            hazeW += volFarHazeStep(dir, tEnd, t1, tau, hazeTau, vis);
            tau += seg;
        }
    }

    const float T = exp(-tau);
    if (T >= 0.9999 && hazeW <= 1e-5)
        return vec4(0.0, 0.0, 0.0, 1.0);

    // The ambient is constant over the segment, so its single-scatter integral collapses: d(tau)/dt is the
    // extinction, hence integral(rho * exp(-tau)) == 1 - T. The sun's is sunW (above): shadowed by the CLOUDS
    // only - the froxel volume's terrain shadow march is a sparse min() that only holds up filtered and
    // temporally blended.
    const vec3 sunDir = normalize(u_sunDirection.xyz);
    const vec3 sunLight = u_sunTransmittance * u_sunColor.rgb
        * (volPhaseHG(dot(dir, sunDir), u_fogParams1.w) * u_eclipseParams.x * u_fogParams8.w); // x "Sun scatter", as the froxels
    // Virtual sky probe only: the GI probe field ends well inside the froxel volume, so evalProbeCoverage
    // would report zero coverage out here and hand over to exactly this. Its sunlit-ground part
    // (giSkySunIrradiance) is cloud-shadowed like the sun: weighted by sunW, which equals 1 - T unshadowed.
    // GI off (u_aoParams.y 0): no sky SH lookup.
    vec3 ambient = u_ambientColor * (1.0 - T);
    if (u_aoParams.y > 0.0)
    {
        const vec3 skyE = giEvalSkySH(-dir);
        const vec3 groundSunE = min(giSkySunIrradiance(-dir), skyE);
        ambient += ((skyE - groundSunE) * (1.0 - T) + groundSunE * sunW) * (u_aoParams.y / PI);
    }

    return vec4(u_fogParams1.rgb * (sunLight * (sunW + hazeW) + ambient), T);
}

// The ray for this pixel from u_mvp's x/y/w ROWS, not from a reconstructed position: u_invMvp is a float32 CPU
// inverse whose error re-rolls every frame, and a sky pixel's far field is a pure function of this ray, so that
// wobble would be unfilterable flicker. Same derivation as sky.fs.glsl / vol_scatter.cs.glsl. u_mvp is
// unjittered while the depth image was rasterized jittered, hence -u_taaJitter (the NDC form of
// shared.inc.glsl's taaJitterUv subtraction). invCos: the volume is bounded by view-Z, the far field
// integrates along the ray.
void fogRay(out vec3 dir, out float invCos)
{
    const vec2 rayVpUv = (v_uv - u_viewportRect.xy) / u_viewportRect.zw;
    const vec2 rayNdc = vec2(rayVpUv.x * 2.0 - 1.0, 1.0 - rayVpUv.y * 2.0) - u_taaJitter.xy;
    const mat3 rayFromNdc = inverse(mat3(
        vec3(u_mvp[0][0], u_mvp[1][0], u_mvp[2][0]),
        vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]),
        vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3])));
    dir = normalize(vec3(rayNdc, 1.0) * rayFromNdc);
    const vec3 camFwd = normalize(vec3(0.0, 0.0, 1.0) * rayFromNdc);
    invCos = 1.0 / max(dot(dir, camFwd), 1e-3);
}

// Fog (in-scatter, transmittance) from the camera to worldPos: the froxel volume, then the far field out to
// t1 along dir (VOL_FAR_INFINITY for the sky; < 0 = worldPos itself, from its view-Z).
vec4 fogTo(vec3 worldPos, float t1, vec3 dir, float invCos)
{
    // Reproject through the shared CENTRE view that the froxel volume was built in, so the lookup lands at the
    // correct froxel regardless of which eye is sampling (on desktop the centre view IS this view). Sampling
    // the centre-built volume at the eye's own screen UV would offset the fog by the eye/head parallax.
    const vec4 centerClip = u_views[VIEW_CENTER].mvp * vec4(worldPos, 1.0);
    const float viewZ = centerClip.w;
    const vec2 ndc = centerClip.xy / centerClip.w;
    const vec2 vpUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    // Outside the centre view (a sliver an eye can see past the volume's coverage): emit no fog rather than
    // letting the clamp sampler smear the edge froxels across the screen.
    if (centerClip.w <= 0.0 || any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
        return vec4(0.0, 0.0, 0.0, 1.0); // inScatter 0, transmittance 1 -> scene unchanged
    // Texel z of the integrated volume stores the fog state at slice z's FAR edge, so the matching
    // texture coordinate sits half a slice below the continuous slice coordinate (texel centers are at
    // +0.5). Sampling without that shift read fog from half a slice too deep and anchored the linear
    // reconstruction between slices wrong - one source of view-aligned fog banding.
    const float s = volViewZToSlice(viewZ) * float(VOL_FROXEL_Z); // continuous slice index
    // Per-pixel interleaved-gradient dither (+-half a slice, rotated per frame): breaks the residual
    // Mach banding of the piecewise-linear reconstruction into noise below TAA's threshold.
    const vec2 px = gl_FragCoord.xy + 5.588238 * float(u_frameIndex & 63u);
    const float ign = fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
    const float w = clamp((s - 0.5 + (ign - 0.5)) / float(VOL_FROXEL_Z), 0.0, 1.0);
    vec4 fog = texture(u_integrated, vec3(vpUv, w));
    // Inside the first slice there is no "before fog" texel to lerp from: fade the fog in linearly so
    // very near geometry isn't over-fogged by the first slice's full accumulation.
    if (s < 1.0)
        fog = mix(vec4(0.0, 0.0, 0.0, 1.0), fog, max(s, 0.0));

    if (u_fogParams9.x > 0.5) // far field: picks up exactly where the volume's last slice ends
    {
        const float t0 = volFogFar() * invCos;
        // t1 < 0: the point itself (the scene surface). Sky runs to infinity, which the closed form handles:
        // a level ray saturates at the horizon, an upward one converges on a finite optical depth.
        // "Far Field/Max distance" (u_fogParams8.z) bounds it: past it no fog is added.
        const float tEnd = min(t1 < 0.0 ? viewZ * invCos : t1, u_fogParams8.z);
        if (tEnd > t0)
        {
            const vec4 far = volFarField(dir, t0, tEnd);
            fog = vec4(fog.rgb + fog.a * far.rgb, fog.a * far.a);
        }
    }
    return fog;
}

void main()
{
    g_viewIndex = int(u_viewIndex);
    const float depth = texture(u_depth, v_uv).r;
    vec3 dir;
    float invCos;
    fogRay(dir, invCos);

    // The cloud part FIRST, folded into (partial in-scatter, cloud transmittance) = 4 live values across the
    // scene's fog evaluation below; the other order kept the scene fog AND the cloud (8) live across the
    // second fogTo (+12 registers). CLOUDS is baked ("Sky/Clouds/Enabled"); u_cloudShape0.w = the cloud
    // march ran this frame (the game suppresses it at runtime).
    vec4 cloudPart = vec4(0.0, 0.0, 0.0, 1.0); // rgb = C.a * S + (1 - T) * C.rgb, a = T
    bool sameFog = false; // the cloud sees the scene's own fog: cloudPart.rgb still holds S, folded below
    float tCloud = 1e30;  // the cloud layer's distance (orders it against the far trees)
#ifdef CLOUDS
    if (u_cloudShape0.w > 0.5)
    {
        const float logScene = depth > 0.0
            ? log2(max(length(viewRelFromDepth(v_uv - taaJitterUv(u_taaJitter.xy), depth)), 1.0))
            : 1e30;
        float logCloudDist;
        const vec4 cloud = cloudUpsample(u_cloudColor, u_cloudDepth, gl_FragCoord.xy, logScene, logCloudDist);
        cloudPart = cloud;
        if (cloud.a < 0.999)
        {
            // The fog in front of the cloud: the same two parts, to the cloud's weighted distance - unless the
            // cloud AND the scene lie past both the froxel volume and the far field's max distance: then both
            // see the same (whole) fog, and the scene's evaluation below serves the cloud too.
            tCloud = exp2(logCloudDist);
            const float fogEnd = max(u_fogParams8.z, u_fogParams0.w * invCos);
            sameFog = min(logCloudDist, logScene) >= log2(fogEnd);
            if (!sameFog)
            {
                const vec4 fogCloud = fogTo(u_viewPos + dir * tCloud, tCloud, dir, invCos);
                cloudPart.rgb = fogCloud.a * cloud.rgb + (1.0 - cloud.a) * fogCloud.rgb;
            }
        }
    }
#endif
    const vec4 fog = fogTo(worldPosFromDepth(v_uv, depth), depth <= 0.0 ? VOL_FAR_INFINITY : -1.0, dir, invCos);
    if (sameFog)
        cloudPart.rgb = fog.a * cloudPart.rgb + (1.0 - cloudPart.a) * fog.rgb;
    vec4 layers = cloudPart;
    if (u_foliageParams4.w > 0.5)
    {
        const vec4 trees = texelFetch(u_farTreesColor, ivec2(gl_FragCoord.xy), 0);
        if (trees.a < 0.999)
        {
            const float tTrees = texelFetch(u_farTreesDepth, ivec2(gl_FragCoord.xy), 0).r;
            const vec4 fogTrees = fogTo(u_viewPos + dir * tTrees, tTrees, dir, invCos);
            const vec3 treePart = fogTrees.a * trees.rgb + (1.0 - trees.a) * fogTrees.rgb;
            layers = tTrees <= tCloud
                ? vec4(treePart + trees.a * cloudPart.rgb, trees.a * cloudPart.a)
                : vec4(cloudPart.rgb + cloudPart.a * treePart, cloudPart.a * trees.a);
        }
    }
    out_color = vec4(layers.a * fog.rgb + layers.rgb, layers.a * fog.a);
}
