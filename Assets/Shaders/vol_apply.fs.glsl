#version 450

// Volumetric fog apply: fullscreen pass in the scene-color render pass (after the forward + sky draws,
// before TAA so the fog gets antialiased). Reconstructs each pixel's view depth from the G-buffer depth
// and samples the integrated fog volume.
// Blended with (srcColor = ONE, dstColor = SRC_ALPHA): out = inScatter + sceneColor * transmittance.
//
// Everything beyond the froxel volume's far plane is added analytically by volFarField instead of by more
// slices, so Fog/Range is a near-field quality knob rather than a view distance.

#include "shared.inc.glsl"
#include "vol_fog.inc.glsl"

layout (location = 0) in vec2 v_uv;
layout (binding = 1) uniform sampler2D u_depth;
layout (binding = 2) uniform sampler3D u_integrated;
layout (binding = 4, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

#define TERRAIN_HEIGHT_BINDING 3
#include "terrain_height.inc.glsl"
#define GI_GRID_DATA_NAME gi_gridData
#include "gi_probe.inc.glsl"

layout (location = 0) out vec4 out_color;

// View index (0 = centre/desktop, 1 = left eye, 2 = right eye); selects the per-eye depth reconstruction.
// The fog volume itself is built once for the centre view, sampled here at each eye's reconstructed world pos.
layout (push_constant) uniform ViewPC { uint u_viewIndex; };

// (fog base world Y, terrain height) at worldXZ — the terrain-follow datum vol_scatter builds per froxel,
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
// tail past the march is unbounded and inherits the last sub-segment — at full strength one sample governs
// an infinite integral, so a zero-fog region erases the horizon along its azimuth and a high macro altitude
// puts the base above the ray, filling the sky over that spot with a column of fog. Full weight at t0 keeps
// the seam with the froxel volume exact; by `reach` both have settled to the global medium at sea level.
vec4 volFarField(vec3 dir, float t0, float t1)
{
    const float density = u_fogParams0.x * u_fogParams9.y;
    if (density <= 1e-7 || t1 <= t0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    const float falloff = u_fogParams0.z * u_fogParams9.z;
    const float tauOpaque = 12.0; // transmittance < 1e-5
    const bool followsTerrain = terrainHeightMapPresent() && u_fogParams3.x > 0.0;

    float tau;
    if (!followsTerrain)
    {
        tau = volAnalyticOpticalDepth(u_viewPos, dir, t0, t1, u_fogParams0.y, falloff, density);
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

            tau += volAnalyticOpticalDepthLinear(u_viewPos.y + dir.y * tPrev - gPrev.x,
                                                 dir.y - (gNext.x - gPrev.x) / max(len, 1e-3),
                                                 len, k, dens);
            if (tau > tauOpaque)
                break;

            tPrev = tNext;
            gPrev = gNext;
        }

        if (t1 > tEnd && tau <= tauOpaque)
            tau += volAnalyticOpticalDepthLinear(u_viewPos.y + dir.y * tEnd - gPrev.x, dir.y, t1 - tEnd, k, dens);
    }

    const float T = exp(-tau);
    if (T >= 0.9999)
        return vec4(0.0, 0.0, 0.0, 1.0);

    // Lighting is constant over the segment, so the single-scatter integral collapses: d(tau)/dt is the
    // extinction, hence integral(rho * exp(-tau)) == 1 - T. Sun unshadowed: the froxel volume's terrain
    // shadow march is a sparse min() that only holds up filtered and temporally blended.
    const vec3 sunDir = normalize(u_sunDirection.xyz);
    vec3 inLight = atmosTransmittanceToLight(0.0, sunDir, u_skyUp) * u_sunColor.rgb
        * (volPhaseHG(dot(dir, sunDir), u_fogParams1.w) * u_eclipseParams.x);
    // Virtual sky probe only: the GI probe field ends well inside the froxel volume, so evalProbeSHCoverage
    // would report zero coverage out here and hand over to exactly this.
    inLight += giEvalSkySH(-dir) * u_aoParams.y / PI + u_ambientColor;

    return vec4(u_fogParams1.rgb * inLight * (1.0 - T), T);
}

void main()
{
    g_viewIndex = int(u_viewIndex);
    const float depth = texture(u_depth, v_uv).r;
    // Reconstruct this eye's world position (current view), then reproject it through the shared CENTRE view
    // that the froxel volume was built in, so the lookup lands at the correct froxel regardless of which eye
    // is sampling (on desktop the centre view IS this view, so vpUv collapses back to v_uv). Sampling the
    // centre-built volume at the eye's own screen UV would offset the fog by the eye/head parallax.
    const vec3 worldPos = worldPosFromDepth(v_uv, depth);
    const vec4 centerClip = u_views[VIEW_CENTER].mvp * vec4(worldPos, 1.0);
    const float viewZ = centerClip.w;
    const vec2 ndc = centerClip.xy / centerClip.w;
    const vec2 vpUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    // Outside the centre view (a sliver an eye can see past the volume's coverage): emit no fog rather than
    // letting the clamp sampler smear the edge froxels across the screen.
    if (centerClip.w <= 0.0 || any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
    {
        out_color = vec4(0.0, 0.0, 0.0, 1.0); // inScatter 0, transmittance 1 -> scene unchanged
        return;
    }
    // Texel z of the integrated volume stores the fog state at slice z's FAR edge, so the matching
    // texture coordinate sits half a slice below the continuous slice coordinate (texel centers are at
    // +0.5). Sampling without that shift read fog from half a slice too deep and anchored the linear
    // reconstruction between slices wrong — one source of view-aligned fog banding.
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
        // Ray from u_mvp's x/y/w ROWS, not from worldPos: u_invMvp is a float32 CPU inverse whose error
        // re-rolls every frame, and a sky pixel's far field is a pure function of this ray, so that wobble
        // would be unfilterable flicker. Same derivation as sky.fs.glsl / vol_scatter.cs.glsl. u_mvp is
        // unjittered while the depth image was rasterized jittered, hence -u_taaJitter (the NDC form of
        // shared.inc.glsl's taaJitterUv subtraction).
        const vec2 rayVpUv = (v_uv - u_viewportRect.xy) / u_viewportRect.zw;
        const vec2 rayNdc = vec2(rayVpUv.x * 2.0 - 1.0, 1.0 - rayVpUv.y * 2.0) - u_taaJitter.xy;
        const mat3 rayFromNdc = inverse(mat3(
            vec3(u_mvp[0][0], u_mvp[1][0], u_mvp[2][0]),
            vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]),
            vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3])));
        const vec3 dir = normalize(vec3(rayNdc, 1.0) * rayFromNdc);
        const vec3 camFwd = normalize(vec3(0.0, 0.0, 1.0) * rayFromNdc);
        // The volume is bounded by view-Z, the far field integrates along the ray.
        const float invCos = 1.0 / max(dot(dir, camFwd), 1e-3);
        const float t0 = volFogFar() * invCos;
        // Sky runs to infinity, which the closed form handles: a level ray saturates at the horizon, an
        // upward one converges on a finite optical depth and stays see-through.
        const float t1 = (depth <= 0.0) ? VOL_FAR_INFINITY : viewZ * invCos;
        if (t1 > t0)
        {
            const vec4 far = volFarField(dir, t0, t1);
            fog = vec4(fog.rgb + fog.a * far.rgb, fog.a * far.a);
        }
    }
    out_color = vec4(fog.rgb, fog.a);
}
