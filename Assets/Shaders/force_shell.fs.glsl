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

// The shading helpers (pattern, forceShadeHit/Wall, heat gradient) are SHARED with the union-march
// FS (force_union.fs.glsl) — everything the instance identity provided rides their ownerIdx param.
#include "force_shell_shade.inc.glsl"

// The SAMPLED SHELL TIER's field volumes (force_shellbake.cs.glsl): every LIVE team's phi, baked
// over the fitted u_forceBake0/1 box, TEAM-SIZED (one texture holds up to 4 teams; the second
// exists only for 5+). Clamp-to-border transparent black = zero field outside.
layout (binding = 5) uniform sampler3D u_shellVolumeA; // phi[0..3]
#if NUM_FORCE_TEAMS > 4
layout (binding = 6) uniform sampler3D u_shellVolumeB; // phi[4..7]
#endif

// forceSampleField's semantics from the BAKED volume: one or two trilinear taps instead of the
// analytic candidate loop — the sampled tier's per-step cost is flat no matter how many emitters
// overlap. Only large emitters march this (their surfaces are far larger than a texel, so the
// trilinear reconstruction error is centimetres). The sampled tier's crossing REFINEMENT and
// NORMALS read the volume too (the surface being refined IS the trilinear field, so the baked
// gradient matches it exactly); only shading's color/ownership stay analytic — they need
// per-emitter identity and shell alpha, which the volume does not carry.
void forceReadBakedPhi(vec3 x, out float phi[NUM_FORCE_TEAMS])
{
    const vec3 uvw = (x - u_forceBake0.xyz) * u_forceBake1.xyz;
    const vec4 a = texture(u_shellVolumeA, uvw);
#if NUM_FORCE_TEAMS > 4
    const vec4 b = texture(u_shellVolumeB, uvw);
#endif
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
#if NUM_FORCE_TEAMS > 4
        phi[t] = t < 4u ? a[t] : b[t - 4u];
#else
        phi[t] = a[t];
#endif
}

void forceSampleFieldBaked(vec3 x, float iso, out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    float phi[NUM_FORCE_TEAMS];
    forceReadBakedPhi(x, phi);
    bestTeam = 0u;
    bestPhi = phi[0];
    for (uint t = 1u; t < NUM_FORCE_TEAMS; ++t)
        if (phi[t] > bestPhi) { bestPhi = phi[t]; bestTeam = t; }
    secondPhi = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != bestTeam)
            secondPhi = max(secondPhi, phi[t]);
    F = bestPhi - forceOpposingBound(iso, secondPhi);
}

// The tier-dispatched primitives the crossing logic runs on: bisection F, wall diff, the wall-fade
// team sample, and the two finite-difference normals. Baked = 1-2 texture taps per evaluation.
float shellSurfaceF(vec3 x, uint team, float iso, bool sampledTier)
{
    if (!sampledTier)
        return forceSurfaceForTeam(x, team, iso);
    float phi[NUM_FORCE_TEAMS];
    forceReadBakedPhi(x, phi);
    float opposing = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
    return phi[team] - forceOpposingBound(iso, opposing);
}

float shellTeamDiff(vec3 x, uint teamA, uint teamB, bool sampledTier)
{
    if (!sampledTier)
        return forceTeamDiff(x, teamA, teamB);
    float phi[NUM_FORCE_TEAMS];
    forceReadBakedPhi(x, phi);
    return phi[teamA] - phi[teamB];
}

void shellTeamSample(vec3 x, uint team, bool sampledTier, out float own, out float opposing)
{
    if (!sampledTier)
    {
        forceTeamSample(x, team, own, opposing);
        return;
    }
    float phi[NUM_FORCE_TEAMS];
    forceReadBakedPhi(x, phi);
    own = phi[team];
    opposing = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != team)
            opposing = max(opposing, phi[t]);
}

vec3 shellSurfaceNormal(vec3 x, uint team, float iso, float h, bool sampledTier)
{
    const vec2 k = vec2(1.0, -1.0);
    vec3 grad = k.xyy * shellSurfaceF(x + k.xyy * h, team, iso, sampledTier)
              + k.yyx * shellSurfaceF(x + k.yyx * h, team, iso, sampledTier)
              + k.yxy * shellSurfaceF(x + k.yxy * h, team, iso, sampledTier)
              + k.xxx * shellSurfaceF(x + k.xxx * h, team, iso, sampledTier);
    return -normalize(grad + vec3(0.0, 1e-6, 0.0));
}

vec3 shellWallNormal(vec3 x, uint teamA, uint teamB, float h, bool sampledTier)
{
    const vec2 k = vec2(1.0, -1.0);
    vec3 grad = k.xyy * shellTeamDiff(x + k.xyy * h, teamA, teamB, sampledTier)
              + k.yyx * shellTeamDiff(x + k.yyx * h, teamA, teamB, sampledTier)
              + k.yxy * shellTeamDiff(x + k.yxy * h, teamA, teamB, sampledTier)
              + k.xxx * shellTeamDiff(x + k.xxx * h, teamA, teamB, sampledTier);
    return normalize(grad + vec3(0.0, 1e-6, 0.0));
}

// The march's field sample: the sampled tier reads the baked volume, everything else accumulates
// analytically. Per-instance uniform branch (the whole fragment wave takes one side).
void forceMarchSample(bool sampledTier, vec3 x, float iso, out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    if (sampledTier)
        forceSampleFieldBaked(x, iso, bestTeam, bestPhi, secondPhi, F);
    else
        forceSampleField(x, iso, bestTeam, bestPhi, secondPhi, F);
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

    // SAMPLED TIER: large emitters march the baked field volume — two trilinear taps per sample
    // instead of the analytic candidate loop, so their cost stops scaling with emitter density.
    // Refinement/normals/shading below remain analytic (crisp rims, exact ownership).
    const bool sampledTier = u_forceBake1.w > 0.5 && e.posReach.w >= u_forceBake0.w;

    // March compositing up to two crossings of F (front shell + the surface behind it). The step
    // COUNT tapers with the proxy's projected size (u_forceParams2.w — see buildUboForce): a small
    // or distant bubble pays a handful of steps instead of the full budget; the floor of 8 keeps
    // thin shells from being stepped over entirely.
    int steps = int(u_forceParams1.w);
    if (u_forceParams2.w > 0.0)
    {
        const float lod = halfExtents.x / max(distance(rayOrigin, center), 1e-3) * u_forceParams2.w;
        steps = clamp(int(float(steps) * min(lod, 1.0)), 8, steps);
    }
    const float dt = (t1 - t0) / float(steps);
    uint bestTeam;
    float bestPhi, secondPhi, F;
    forceMarchSample(sampledTier, rayOrigin + rayDir * t0, iso, bestTeam, bestPhi, secondPhi, F);
    // "Inside a bubble" is a property of the CAMERA, not of this box's entry point: a proxy whose
    // box begins inside the merged field must style the exit it finds as a backface, not a dome.
    // The camera is ONE point per frame, evaluated on the CPU (buildUboForce) — never re-sampled here.
    const bool cameraInsideField = t0 > 0.0 ? u_forceBake2.w > 0.5 : F > 0.0;
    uint prevTeam = bestTeam;
    float tPrev = t0;
    float fPrev = F;
    vec3 accumColor = vec3(0.0);
    float accumAlpha = 0.0;
    int numShaded = 0;
    for (int i = 1; i <= steps && numShaded < 3; ++i)
    {
        const float t = t0 + dt * float(i);
        forceMarchSample(sampledTier, rayOrigin + rayDir * t, iso, bestTeam, bestPhi, secondPhi, F);
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
                    const float fm = shellSurfaceF(rayOrigin + rayDir * mid, hitTeam, iso, sampledTier);
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
                    if (shellTeamDiff(rayOrigin + rayDir * mid, prevTeam, bestTeam, sampledTier) > 0.0) lo = mid; else hi = mid;
                }
                tWall = (lo + hi) * 0.5;
                // The pane exists where BOTH pressed fields are above iso; fade it out toward the
                // rim analytically (also rejects spurious flips between weak far-apart fields).
                float wallOwn, wallOpposing;
                shellTeamSample(rayOrigin + rayDir * tWall, prevTeam, sampledTier, wallOwn, wallOpposing);
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
                    float phiW[NUM_FORCE_TEAMS];
                    float phiVisW[NUM_FORCE_TEAMS];
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
                        const float h = forceShadeNormalH(v_emitterIdx);
                        const vec3 wallPos = rayOrigin + rayDir * tWall;
                        const vec4 layer = asSkin
                            ? forceShadeHit(rayOrigin, rayDir, tWall, wallSkinTeam, cameraInsideField, sceneDist, v_emitterIdx,
                                shellSurfaceNormal(wallPos, wallSkinTeam, iso, h, sampledTier))
                            : forceShadeWall(rayOrigin, rayDir, tWall, prevTeam, bestTeam, wallFade, v_emitterIdx,
                                shellWallNormal(wallPos, prevTeam, bestTeam, h, sampledTier));
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
                        const vec4 layer = forceShadeHit(rayOrigin, rayDir, tHit, hitTeam, cameraInsideField, sceneDist, v_emitterIdx,
                            shellSurfaceNormal(rayOrigin + rayDir * tHit, hitTeam, iso, forceShadeNormalH(v_emitterIdx), sampledTier));
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
