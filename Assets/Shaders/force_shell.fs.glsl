#version 460

// Forcefield shell fragment shader: ray-marches the analytic team field through this instance's
// oriented reach box and composites up to TWO surface crossings of F = phi[best] - max(iso,
// phi[second]) front-to-back — the front shell plus the far/inner shell behind it (its visibility
// is the "Backface alpha" tweak; from inside a bubble the exit dome uses "Interior alpha" instead).
// Each crossing is shaded as a fresnel-rimmed energy shell (team color, world-anchored hex/noise
// pattern, contact glow where an opposing bubble presses in, soft glow where the shell meets scene
// geometry). Ownership discard keeps merged same-team bubbles single-shaded: a crossing is only
// shaded by the fragment whose instance is the DOMINANT contributor there; unowned crossings still
// advance the march (their owner's fragment draws them). Premultiplied blend over the lit scene,
// manual depth test against the G-buffer depth (reversed-Z), no depth write — particles and fog
// layer on top.

#include "shared.inc.glsl"
#include "force_field.inc.glsl" // declares the emitter buffer at FORCE_EMITTERS_BINDING (1)

layout (binding = 2) uniform sampler2D u_gbufferDepth;

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) in flat uint v_emitterIdx;

layout (location = 0) out vec4 out_color;

// ---- animated surface pattern ----

float forceHash(vec3 p)
{
    uvec3 q = floatBitsToUint(p) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = q.x ^ q.y ^ q.z;
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    // The xor spans the full 32 bits: normalize by 2^32 so the noise stays in [0, 1). An undersized
    // divisor here returned [0, 4) and drove the ridged crest shaping (1 - |2f - 1|)^3 hugely
    // NEGATIVE — the pattern then subtracted color in the premultiplied blend (black blotches).
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

// Flowing wave pattern: domain-warped 3D value noise shaped into soft ridged crests — drifting
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
    // meandering, non-repeating swirls instead of straight noise bands.
    const vec3 warp = vec3(
        forceValueNoise(p * 0.8 + vec3(0.0, t * 0.55, 0.0)),
        forceValueNoise(p * 0.8 + vec3(5.2, 1.3, -t * 0.45)),
        forceValueNoise(p * 0.8 + vec3(9.7, 4.1, t * 0.35))) - 0.5;
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
vec4 forceShadeHit(vec3 rayOrigin, vec3 rayDir, float tHit, uint hitTeam, bool cameraInsideField, float sceneDist)
{
    const vec3 hitPos = rayOrigin + rayDir * tHit;
    float phi[MAX_FORCE_TEAMS];
    float phiVis[MAX_FORCE_TEAMS]; // shell-alpha-weighted: invisible fields shape, never tint
    forceAccumulateVisible(hitPos, phi, phiVis);
    const float ownPhi = phi[hitTeam];
    if (ownPhi <= 0.0)
        return vec4(0.0);
    float opposingPhiVis = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
        if (t != hitTeam)
            opposingPhiVis = max(opposingPhiVis, phiVis[t]);

    const float iso = u_forceParams0.x;
    const float h = max(0.005 * fe_emitters[v_emitterIdx].posReach.w, 0.01);
    vec3 n = forceSurfaceNormal(hitPos, hitTeam, iso, h);
    const bool viewedFromInside = dot(n, rayDir) > 0.0;
    if (viewedFromInside)
        n = -n;

    // Field-weighted team color (sharpened phi^4 weights over the VISIBLE fields): isolated
    // bubbles keep their pure color, but toward a junction of two visible shells both sides
    // converge to the same mix — so whichever surface (or side) a pixel's march classified, it
    // shades the same there. Hard per-team lookups printed march-step-sized color jaggies along
    // the junction rim. Invisible fields (shell alpha 0) carry no color weight: a bubble pressed
    // by one keeps its pure team color instead of going junction-purple.
    vec3 teamColor = vec3(0.0);
    float weightSum = 0.0;
    for (uint t = 0u; t < MAX_FORCE_TEAMS; ++t)
    {
        float w = phiVis[t] * phiVis[t];
        w *= w;
        teamColor += u_forceTeamColors[t].rgb * w;
        weightSum += w;
    }
    teamColor /= max(weightSum, 1e-12);
    const float fresnel = pow(1.0 - clamp(dot(n, -rayDir), 0.0, 1.0), u_forceParams0.y);
    // Contact glow: the equilibrium seam lights up as the best VISIBLE opposing field approaches
    // our own — an invisible field pressing in doesn't light the whole rim as a seam.
    const float contact = smoothstep(1.0 - u_forceParams1.y, 1.0, opposingPhiVis / max(ownPhi, 1e-4));
    // Geometry glow: the shell surface fading into nearby opaque geometry along the view ray.
    const float geoGlow = u_forceParams1.z > 0.0
        ? 1.0 - clamp((sceneDist - tHit) / u_forceParams1.z, 0.0, 1.0) : 0.0;
    const float pattern = forcePattern(hitPos, n) * u_forceParams2.z;
    const float alphaMult = fe_emitters[v_emitterIdx].outputParams.y;

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
vec4 forceShadeWall(vec3 rayOrigin, vec3 rayDir, float tWall, uint teamA, uint teamB, float fade)
{
    const vec3 pos = rayOrigin + rayDir * tWall;
    const float h = max(0.005 * fe_emitters[v_emitterIdx].posReach.w, 0.01);
    vec3 n = forceWallNormal(pos, teamA, teamB, h);
    if (dot(n, rayDir) > 0.0)
        n = -n;
    const float fresnel = pow(1.0 - clamp(dot(n, -rayDir), 0.0, 1.0), u_forceParams0.y);
    const float pattern = forcePattern(pos, n) * u_forceParams2.z;
    const vec3 mixed = mix(u_forceTeamColors[teamA].rgb, u_forceTeamColors[teamB].rgb, 0.5);
    vec3 color = mix(mixed, vec3(1.0), 0.6) * u_forceParams1.x * (0.5 + 0.5 * pattern + fresnel);
    float alpha = clamp(u_forceParams3.z * (0.35 + 0.4 * fresnel + 0.25 * pattern), 0.0, 1.0) * fade;
    return vec4(color * alpha + color * 0.15 * fade, alpha);
}

void main()
{
    g_viewIndex = int(u_viewIndex);
    const ForceEmitterData e = fe_emitters[v_emitterIdx];
    const float iso = u_forceParams0.x;

    // View ray through this fragment (far-plane reconstruction; reversed-Z far = 0).
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw;
    const vec3 rayOrigin = u_viewPos;
    const vec3 rayDir = normalize(worldPosFromDepth(uv, 0.0) - rayOrigin);

    // Intersect the same oriented reach box the VS rasterized.
    float side, forward, back;
    forceEmitterBounds(e, side, forward, back);
    const mat3 basis = forceEmitterBasis(e.dirFocus.xyz);
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * (forward - back) * 0.5;
    const vec3 halfExtents = vec3(side, side, (forward + back) * 0.5);
    const vec3 localOrigin = transpose(basis) * (rayOrigin - center);
    const vec3 localDir = transpose(basis) * rayDir;
    const vec3 invDir = 1.0 / (localDir + vec3(equal(localDir, vec3(0.0))) * 1e-8);
    const vec3 tA = (-halfExtents - localOrigin) * invDir;
    const vec3 tB = ( halfExtents - localOrigin) * invDir;
    float t0 = max(max(min(tA.x, tB.x), min(tA.y, tB.y)), min(tA.z, tB.z));
    float t1 = min(min(max(tA.x, tB.x), max(tA.y, tB.y)), max(tA.z, tB.z));
    t0 = max(t0, 0.0);

    // Manual depth test: clamp the march to the opaque scene.
    const float sceneDepth = texture(u_gbufferDepth, uv).r;
    float sceneDist = 1e30;
    if (sceneDepth > 0.0) // reversed-Z: 0 = sky/far
    {
        sceneDist = distance(rayOrigin, worldPosFromDepth(uv, sceneDepth));
        t1 = min(t1, sceneDist);
    }
    if (t1 <= t0)
        discard;

    // DEBUG density view: heatmap of the STRONGEST team field along the ray instead of the shell —
    // tip concentration, lobe merging and the budget fold read directly; a white contour marks the
    // iso threshold (the bubble boundary). Overlapping proxies dedup via dominance at the peak.
    if (u_forceParams4.x > 0.5)
    {
        const int densitySteps = int(u_forceParams1.w);
        const float densityDt = (t1 - t0) / float(densitySteps);
        float maxPhi = 0.0;
        float tMax = t0;
        uint maxTeam = 0u;
        for (int i = 0; i <= densitySteps; ++i)
        {
            const float t = t0 + densityDt * float(i);
            uint bt;
            float bp, sp, Fd;
            forceSampleField(rayOrigin + rayDir * t, iso, bt, bp, sp, Fd);
            if (bp > maxPhi)
            {
                maxPhi = bp;
                tMax = t;
                maxTeam = bt;
            }
        }
        if (maxPhi <= 1e-4)
            discard;
        if (forceDominantEmitter(rayOrigin + rayDir * tMax, maxTeam) != v_emitterIdx)
            discard;
        vec3 heat = forceHeatColor(clamp(maxPhi / u_forceParams4.y, 0.0, 1.0));
        const float contour = 1.0 - smoothstep(0.0, 0.08, abs(maxPhi - iso) / iso);
        heat = mix(heat, vec3(1.0), contour * 0.8);
        out_color = vec4(heat * 0.8, 0.8);
        return;
    }

    // Fixed-step march compositing up to two crossings of F (front shell + the surface behind it).
    const int steps = int(u_forceParams1.w);
    const float dt = (t1 - t0) / float(steps);
    uint bestTeam;
    float bestPhi, secondPhi, F;
    forceSampleField(rayOrigin + rayDir * t0, iso, bestTeam, bestPhi, secondPhi, F);
    // "Inside a bubble" is a property of the CAMERA, not of this box's entry point: a proxy whose
    // box begins inside the merged field must style the exit it finds as a backface, not a dome.
    bool cameraInsideField = F > 0.0;
    if (t0 > 0.0 && cameraInsideField)
    {
        uint originTeam;
        float originBest, originSecond, originF;
        forceSampleField(rayOrigin, iso, originTeam, originBest, originSecond, originF);
        cameraInsideField = originF > 0.0;
    }
    uint prevTeam = bestTeam;
    float tPrev = t0;
    float fPrev = F;
    vec3 accumColor = vec3(0.0);
    float accumAlpha = 0.0;
    int numShaded = 0;
    for (int i = 1; i <= steps && numShaded < 3; ++i)
    {
        const float t = t0 + dt * float(i);
        forceSampleField(rayOrigin + rayDir * t, iso, bestTeam, bestPhi, secondPhi, F);
        const bool entryCrossing = F > 0.0;
        const bool surfaceCrossing = entryCrossing != (fPrev > 0.0);
        // Winning team flipped with at least one endpoint inside: the ray crossed the equilibrium
        // WALL between two pressed bubbles. F never changes sign there (it dips to 0 as best/second
        // swap), so the wall refines on the team difference, which does. One-endpoint-inside counts
        // because near the pane's RIM the skin crossing and the wall sit inside a single step —
        // requiring both endpoints inside made the pane's edge stair-step with the sampling.
        const bool teamFlip = bestTeam != prevTeam && (entryCrossing || fPrev > 0.0);
        if (surfaceCrossing || teamFlip)
        {
            float tHit = -1.0;
            uint hitTeam = 0u;
            if (surfaceCrossing)
            {
                // The bubble's team is the INSIDE end's strongest team; bisect on [tPrev, t] holding
                // it fixed. 6 refinements: neighbouring pixels can be shaded by DIFFERENT proxies
                // whose march intervals differ — the residual hit error must stay below the normal's
                // finite-difference step or the fresnel steps at merged shells' ownership boundary.
                hitTeam = entryCrossing ? bestTeam : prevTeam;
                float lo = tPrev, hi = t;
                for (int b = 0; b < 6; ++b)
                {
                    const float mid = (lo + hi) * 0.5;
                    const float fm = forceSurfaceForTeam(rayOrigin + rayDir * mid, hitTeam, iso);
                    if ((fm > 0.0) == entryCrossing) hi = mid; else lo = mid;
                }
                tHit = (lo + hi) * 0.5;
            }
            float tWall = -1.0;
            float wallFade = 0.0;
            uint wallSkinTeam = MAX_FORCE_TEAMS; // < MAX: wall shades as this team's SKIN, not a pane
            if (teamFlip)
            {
                float lo = tPrev, hi = t;
                for (int b = 0; b < 6; ++b)
                {
                    const float mid = (lo + hi) * 0.5;
                    if (forceTeamDiff(rayOrigin + rayDir * mid, prevTeam, bestTeam) > 0.0) lo = mid; else hi = mid;
                }
                tWall = (lo + hi) * 0.5;
                // The pane exists where BOTH pressed fields are above iso; fade it out toward the
                // rim analytically (also rejects spurious flips between weak far-apart fields).
                float wallOwn, wallOpposing;
                forceTeamSample(rayOrigin + rayDir * tWall, prevTeam, wallOwn, wallOpposing);
                wallFade = smoothstep(iso, iso * 1.3, min(wallOwn, wallOpposing));
                if (wallFade <= 0.001)
                    tWall = -1.0;
                if (tWall >= 0.0)
                {
                    // A wall against an INVISIBLE field (shell alpha ~0, e.g. a map-scale gameplay
                    // emitter) is not a contested pane — it IS the visible side's surface, pressed
                    // flat. Reclassify it as that team's skin: it then shades with the normal team
                    // look (phiVis already keeps the color pure) and, critically, is OWNED by the
                    // visible team's dominant emitter — the invisible side's proxy never
                    // rasterizes, which silently dropped the entry-side wall entirely.
                    float phiW[MAX_FORCE_TEAMS];
                    float phiVisW[MAX_FORCE_TEAMS];
                    forceAccumulateVisible(rayOrigin + rayDir * tWall, phiW, phiVisW);
                    const float visPrev = phiVisW[prevTeam] / max(phiW[prevTeam], 1e-6);
                    const float visBest = phiVisW[bestTeam] / max(phiW[bestTeam], 1e-6);
                    if (min(visPrev, visBest) < 0.05)
                    {
                        wallSkinTeam = visPrev > visBest ? prevTeam : bestTeam;
                        // A rim step can hold this wall AND the true skin crossing of the same
                        // visible surface — shading both would double the shell there.
                        if (tHit >= 0.0)
                            tWall = -1.0;
                    }
                }
            }
            // Composite this step's events in ray order (a rim step can hold skin AND wall), each
            // ownership-checked so exactly one proxy shades it; unowned events still advance the march.
            for (int ev = 0; ev < 2 && numShaded < 3; ++ev)
            {
                const bool wallFirst = tWall >= 0.0 && (tHit < 0.0 || tWall < tHit);
                if (wallFirst)
                {
                    const bool asSkin = wallSkinTeam != MAX_FORCE_TEAMS;
                    const uint ownTeam = asSkin ? wallSkinTeam : prevTeam;
                    if (forceDominantEmitter(rayOrigin + rayDir * tWall, ownTeam) == v_emitterIdx)
                    {
                        const vec4 layer = asSkin
                            ? forceShadeHit(rayOrigin, rayDir, tWall, wallSkinTeam, cameraInsideField, sceneDist)
                            : forceShadeWall(rayOrigin, rayDir, tWall, prevTeam, bestTeam, wallFade);
                        accumColor += (1.0 - accumAlpha) * layer.rgb;
                        accumAlpha += (1.0 - accumAlpha) * layer.a;
                        ++numShaded;
                    }
                    tWall = -1.0;
                }
                else if (tHit >= 0.0)
                {
                    if (forceDominantEmitter(rayOrigin + rayDir * tHit, hitTeam) == v_emitterIdx)
                    {
                        const vec4 layer = forceShadeHit(rayOrigin, rayDir, tHit, hitTeam, cameraInsideField, sceneDist);
                        accumColor += (1.0 - accumAlpha) * layer.rgb;
                        accumAlpha += (1.0 - accumAlpha) * layer.a;
                        ++numShaded;
                    }
                    tHit = -1.0;
                }
                else
                    break;
            }
        }
        prevTeam = bestTeam;
        tPrev = t;
        fPrev = F;
    }
    if (accumAlpha <= 0.002 && dot(accumColor, accumColor) < 1e-6)
        discard;

    out_color = vec4(accumColor, accumAlpha);
}
