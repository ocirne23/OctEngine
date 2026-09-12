// Forcefield shell SHADING, shared by the per-proxy shell FS (force_shell.fs.glsl) and the
// analytic-tier UNION march FS (force_union.fs.glsl). Everything an instance identity used to
// provide rides the `ownerIdx` parameter instead (the shading emitter: v_emitterIdx per-proxy,
// the crossing's dominant emitter in the union pass) - it only scales the normal's
// finite-difference step (posReach.w) and the per-emitter shell alpha (outputParams.y).
// The consumer includes force_field.inc.glsl first.

#ifndef FORCE_SHELL_SHADE_INC_GLSL
#define FORCE_SHELL_SHADE_INC_GLSL

// ---- animated surface pattern ----

float forceHash(vec3 p)
{
    uvec3 q = floatBitsToUint(p) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = q.x ^ q.y ^ q.z;
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    // The xor spans the full 32 bits: normalize by 2^32 so the noise stays in [0, 1). An undersized
    // divisor here returned [0, 4) and drove the ridged crest shaping (1 - |2f - 1|)^3 hugely
    // NEGATIVE - the pattern then subtracted color in the premultiplied blend (black blotches).
    return float((n >> 22u) ^ n) * (1.0 / 4294967296.0);
}

float forceValueNoise(vec3 p)
{
    const vec3 i = floor(p);
    vec3 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(forceHash(i + vec3(0, 0, 0)), forceHash(i + vec3(1, 0, 0)), f.x),
                   mix(forceHash(i + vec3(0, 1, 0)), forceHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(forceHash(i + vec3(0, 0, 1)), forceHash(i + vec3(1, 0, 1)), f.x),
                   mix(forceHash(i + vec3(0, 1, 1)), forceHash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// Flowing wave pattern: domain-warped 3D value noise shaped into soft ridged crests - drifting
// energy swells with no repeating tiling. Purely WORLD-anchored and per-emitter-free: moving
// emitters slide through a stable pattern and merged shells shade seamlessly across their ownership
// boundary (any per-emitter term here would print a seam where the dominant owner flips). The
// normal is unused (3D noise needs no triplanar projection), kept for signature stability.
float forcePattern(vec3 worldPos, vec3 n)
{
    const float scale = u_forceParams2.x;
    const float t = u_timeSeconds * u_forceParams2.y;
    const vec3 p = worldPos * scale;
    // Low-frequency warp fields drifting at different rates: these bend the wave bands into
    // meandering, non-repeating swirls instead of straight noise bands. LOD: past ~60 m the warp's
    // meander is sub-pixel, so its 3 noise octaves (24 hashes) fade out and are skipped entirely.
    const float warpFade = 1.0 - smoothstep(60.0, 100.0, distance(worldPos, u_viewPos));
    vec3 warp = vec3(0.0);
    if (warpFade > 0.0)
        warp = (vec3(
            forceValueNoise(p * 0.8 + vec3(0.0, t * 0.55, 0.0)),
            forceValueNoise(p * 0.8 + vec3(5.2, 1.3, -t * 0.45)),
            forceValueNoise(p * 0.8 + vec3(9.7, 4.1, t * 0.35))) - 0.5) * warpFade;
    // Main wave crests: ridged shaping of a warped mid-frequency field (bright meandering bands).
    const float f1 = forceValueNoise(p * 1.7 + warp * 3.0 + vec3(0.0, 0.0, t * 0.25));
    float crest = 1.0 - abs(f1 * 2.0 - 1.0);
    crest *= crest * crest;
    // Finer counter-drifting shimmer riding the same warp.
    const float f2 = forceValueNoise(p * 3.6 - warp * 2.0 + vec3(t * 0.4, 0.0, 0.0));
    return crest * (0.8 + 0.5 * f2) + f2 * f2 * 0.25;
}

// Shades one refined surface crossing. Returns PREMULTIPLIED rgb + alpha, ready to composite
// front-to-back. Layer styling: front surfaces (outward normal toward the camera) are the standard
// shell; surfaces seen from their inside are the interior dome (camera within a bubble, "Interior
// alpha" floor) or the far/inner backface seen through the front ("Backface alpha" scale).
// The OUTWARD surface normal is the CALLER's (`normal`, unflipped): the shell FS supplies the
// baked-volume gradient on the sampled tier and the analytic gradient otherwise, the union FS
// always the analytic one - so the 4-tap finite difference pays the caller's cheapest field.
// forceShadeNormalH is the shared finite-difference step for computing it.
float forceShadeNormalH(uint ownerIdx) { return max(0.005 * fe_emitters[ownerIdx].posReach.w, 0.01); }

vec4 forceShadeHit(vec3 rayOrigin, vec3 rayDir, float tHit, uint hitTeam, bool cameraInsideField,
    float sceneDist, uint ownerIdx, vec3 normal)
{
    const vec3 hitPos = rayOrigin + rayDir * tHit;
    float phi[NUM_FORCE_TEAMS];
    float phiVis[NUM_FORCE_TEAMS]; // shell-alpha-weighted: invisible fields shape, never tint
    forceAccumulateVisible(hitPos, phi, phiVis);
    const float ownPhi = phi[hitTeam];
    if (ownPhi <= 0.0)
        return vec4(0.0);
    float opposingPhiVis = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != hitTeam)
            opposingPhiVis = max(opposingPhiVis, phiVis[t]);

    const float iso = u_forceParams0.x;
    vec3 n = normal;
    const bool viewedFromInside = dot(n, rayDir) > 0.0;
    if (viewedFromInside)
        n = -n;

    // Field-weighted team color (sharpened phi^4 weights over the VISIBLE fields): isolated
    // bubbles keep their pure color, but toward a junction of two visible shells both sides
    // converge to the same mix - so whichever surface (or side) a pixel's march classified, it
    // shades the same there. Hard per-team lookups printed march-step-sized color jaggies along
    // the junction rim. Invisible fields (shell alpha 0) carry no color weight: a bubble pressed
    // by one keeps its pure team color instead of going junction-purple.
    vec3 teamColor = vec3(0.0);
    float weightSum = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
    {
        float w = phiVis[t] * phiVis[t];
        w *= w;
        teamColor += u_forceTeamColors[t].rgb * w;
        weightSum += w;
    }
    teamColor /= max(weightSum, 1e-12);
    const float fresnel = pow(1.0 - clamp(dot(n, -rayDir), 0.0, 1.0), u_forceParams0.y);
    // Contact glow: the equilibrium seam lights up as the best VISIBLE opposing field approaches
    // our own - an invisible field pressing in doesn't light the whole rim as a seam.
    const float contact = smoothstep(1.0 - u_forceParams1.y, 1.0, opposingPhiVis / max(ownPhi, 1e-4));
    // Geometry glow: the shell surface fading into nearby opaque geometry along the view ray.
    const float geoGlow = u_forceParams1.z > 0.0
        ? 1.0 - clamp((sceneDist - tHit) / u_forceParams1.z, 0.0, 1.0) : 0.0;
    const float pattern = forcePattern(hitPos, n) * u_forceParams2.z;
    const float alphaMult = fe_emitters[ownerIdx].outputParams.y;

    const float rimI = u_forceParams0.z;
    vec3 color = teamColor * (rimI * fresnel + pattern * (0.25 + 0.75 * fresnel));
    color += mix(teamColor, vec3(1.0), 0.6) * contact * u_forceParams1.x;
    color += teamColor * geoGlow * rimI * 0.5;

    float alpha = u_forceParams0.w * alphaMult * (0.2 + 0.8 * fresnel);
    float layerScale = 1.0;
    if (viewedFromInside)
    {
        if (cameraInsideField)
        {
            // Interior dome: pattern-forward opacity floor, so the shell stays visible looking out
            // from within (rims still brighten toward grazing angles via fresnel).
            const float interior = u_forceParams3.x * alphaMult;
            color += teamColor * (0.3 + pattern) * interior;
            alpha = max(alpha, interior * (0.5 + 0.35 * pattern));
        }
        else
        {
            layerScale = u_forceParams3.y; // far/inner surface seen from outside, through the front
        }
    }
    alpha += contact * 0.25 + geoGlow * 0.1;
    alpha = clamp(alpha, 0.0, 1.0) * layerScale;
    // Premultiplied + a slight additive lift so rims/glow bloom over the scene.
    return vec4(color * alpha + color * 0.15 * layerScale, alpha);
}

// Heat gradient for the density debug view: blue -> cyan -> green -> yellow -> red -> white.
vec3 forceHeatColor(float t)
{
    const vec3 c0 = vec3(0.0, 0.0, 0.3);
    const vec3 c1 = vec3(0.0, 0.4, 1.0);
    const vec3 c2 = vec3(0.0, 1.0, 0.3);
    const vec3 c3 = vec3(1.0, 1.0, 0.0);
    const vec3 c4 = vec3(1.0, 0.2, 0.0);
    const vec3 c5 = vec3(1.0);
    if (t < 0.2) return mix(c0, c1, t * 5.0);
    if (t < 0.4) return mix(c1, c2, (t - 0.2) * 5.0);
    if (t < 0.6) return mix(c2, c3, (t - 0.4) * 5.0);
    if (t < 0.8) return mix(c3, c4, (t - 0.6) * 5.0);
    return mix(c4, c5, (t - 0.8) * 5.0);
}

// Shades the equilibrium WALL between two opposing pressed bubbles (phi_A == phi_B, both above
// iso): a white-hot energy pane blending both team colors, its own visibility on "Contact wall
// alpha". fade in [0,1] dissolves the pane at its rim (where min(phi_A, phi_B) approaches iso), so
// the edge is analytic instead of stair-stepping with the march sampling. Premultiplied rgb + alpha.
vec4 forceShadeWall(vec3 rayOrigin, vec3 rayDir, float tWall, uint teamA, uint teamB, float fade,
    uint ownerIdx, vec3 normal) // normal from the caller - see forceShadeHit
{
    const vec3 pos = rayOrigin + rayDir * tWall;
    vec3 n = normal;
    if (dot(n, rayDir) > 0.0)
        n = -n;
    const float fresnel = pow(1.0 - clamp(dot(n, -rayDir), 0.0, 1.0), u_forceParams0.y);
    const float pattern = forcePattern(pos, n) * u_forceParams2.z;
    const vec3 mixed = mix(u_forceTeamColors[teamA].rgb, u_forceTeamColors[teamB].rgb, 0.5);
    vec3 color = mix(mixed, vec3(1.0), 0.6) * u_forceParams1.x * (0.5 + 0.5 * pattern + fresnel);
    float alpha = clamp(u_forceParams3.z * (0.35 + 0.4 * fresnel + 0.25 * pattern), 0.0, 1.0) * fade;
    return vec4(color * alpha + color * 0.15 * fade, alpha);
}

#endif
