#version 460

// The ANALYTIC tier's UNION MARCH: one fullscreen march per covered pixel over the interval the
// small-emitter proxies rasterized (force_interval.fs.glsl) — where N small bubbles stack on a
// pixel, the field is marched ONCE instead of once per proxy, killing the overdraw term of the
// old per-proxy path. Marching and shading match force_shell.fs.glsl exactly (same crossing/wall
// machinery, same analytic refinement, shared shading include); the ownership discard is gone
// because ONE march owns every crossing — the only skip is a crossing whose dominant contributor
// is a SAMPLED-tier (large) emitter, which that emitter's own proxy draws. Premultiplied blend,
// manual reversed-Z depth clamp, no depth write.

#include "shared.inc.glsl"
#include "force_field.inc.glsl" // declares the emitter buffer at FORCE_EMITTERS_BINDING (1)

#ifndef FORCE_UNION_UV_SCALE
#define FORCE_UNION_UV_SCALE 1.0 // injected 2.0 when the march runs at half res
#endif

layout (binding = 2) uniform sampler2D u_gbufferDepth;
layout (binding = 5) uniform sampler2D u_shellInterval; // (tEntry, -tExit) per pixel, RG16F

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) out vec4 out_color;

#include "force_shell_shade.inc.glsl"

// A crossing dominated by a SAMPLED-tier emitter belongs to that emitter's own proxy draw.
bool forceSampledTierOwns(vec3 hitPos, uint team, out uint ownerIdx)
{
    ownerIdx = forceDominantEmitter(hitPos, team);
    if (ownerIdx == 0xFFFFFFFFu)
        return true; // no contributor (numerical fringe): draw nothing either way
    return u_forceBake1.w > 0.5 && fe_emitters[ownerIdx].posReach.w >= u_forceBake0.w;
}

#ifdef FORCE_GRID
// The EMPTY-CELL sample: a cell with no candidates holds NO SMALL emitters (big ones bypass the
// grid), so the field there is exactly the big list + the ambient term — a loop over fe_bigCount
// (typically 0-2) instead of nothing at all. This keeps the empty-cell fast path alive in scenes
// WITH big emitters, where it used to be disabled outright.
void forceSampleFieldBigOnly(vec3 x, float iso, uint fallbackTeam,
    out uint bestTeam, out float bestPhi, out float secondPhi, out float F)
{
    float phi[NUM_FORCE_TEAMS];
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        phi[t] = 0.0;
    for (uint k = 0u; k < fe_bigCount; ++k)
    {
        const ForceEmitterData e = fe_emitters[fe_bigIndices[k]];
        const float c = forceContribution(x, e);
        for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t) // predicated adds (see forceAccumulate)
            phi[t] += e.teamFlags.x == t ? c : 0.0;
    }
    const uint ambientTeam = min(uint(u_forceParams4.w), NUM_FORCE_TEAMS - 1u);
    const float ambient = forceAmbientField(x);
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        phi[t] += t == ambientTeam ? ambient : 0.0;
    bestTeam = 0u;
    bestPhi = phi[0];
    for (uint t = 1u; t < NUM_FORCE_TEAMS; ++t)
        if (phi[t] > bestPhi) { bestPhi = phi[t]; bestTeam = t; }
    secondPhi = 0.0;
    for (uint t = 0u; t < NUM_FORCE_TEAMS; ++t)
        if (t != bestTeam)
            secondPhi = max(secondPhi, phi[t]);
    F = bestPhi - forceOpposingBound(iso, secondPhi);
    if (bestPhi <= 0.0)
        bestTeam = fallbackTeam; // zero field: never a spurious team flip through empty space
}

// Conservative bounding sphere of a big emitter's support (mirrors the CPU ShellCull sphere):
// the ray-entry distance into it bounds where that emitter's field can begin along the ray.
float forceBigSupportEntry(vec3 rayOrigin, vec3 rayDir, ForceEmitterData e)
{
    float side, forward, back;
    forceEmitterBounds(e, side, forward, back);
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * ((forward - back) * 0.5);
    const float radius = length(vec3(side, side, (forward + back) * 0.5));
    const vec3 oc = rayOrigin - center;
    const float b = dot(oc, rayDir);
    const float disc = b * b - (dot(oc, oc) - radius * radius);
    if (disc <= 0.0)
        return 1e30; // the ray never reaches this emitter's support
    return -b - sqrt(disc); // may be negative (inside/behind): caller clamps against t
}
#endif

void main()
{
    g_viewIndex = int(u_viewIndex);
    const float iso = u_forceParams0.x;

    const vec2 interval = texelFetch(u_shellInterval, ivec2(gl_FragCoord.xy), 0).xy;
    float t0 = interval.x;
    float t1 = -interval.y;
    if (t0 > 6.0e4 || t1 <= t0)
        discard; // cleared (no analytic shell covers this pixel)

    // FORCE_UNION_UV_SCALE (injected, 2.0 in the half-res mode; absent = full res): maps this
    // pass's gl_FragCoord back to full-res uv (the interval texelFetch below stays in this pass's
    // own texels — its target always matches this resolution).
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw * FORCE_UNION_UV_SCALE;
    const vec3 rayOrigin = u_viewPos;
    const vec3 rayDir = normalize(worldPosFromDepth(uv, 0.0) - rayOrigin);

    // Manual depth test: clamp the march to the opaque scene (once — not per proxy).
    const float sceneDepth = texture(u_gbufferDepth, uv).r;
    float sceneDist = 1e30;
    if (sceneDepth > 0.0) // reversed-Z: 0 = sky/far
    {
        sceneDist = distance(rayOrigin, worldPosFromDepth(uv, sceneDepth));
        t1 = min(t1, sceneDist);
    }
    if (t1 <= t0)
        discard;

    // World-space step size (u_forceBake2.x), growing with DISTANCE so a far pixel never marches
    // finer than ~2 px of world size (u_forceBake2.z = px per radius/dist), hard-capped
    // (u_forceBake2.y): the union interval can span several disjoint bubbles, so the step count
    // follows its LENGTH instead of being a fixed budget squeezed over it.
    const float stepSize = max(u_forceBake2.x, 2.0 * t0 / max(u_forceBake2.z, 1.0));
    const int steps = clamp(int((t1 - t0) / stepSize), 4, int(u_forceBake2.y));
    const float dt = (t1 - t0) / float(steps);
    uint bestTeam;
    float bestPhi, secondPhi, F;
    forceSampleField(rayOrigin + rayDir * t0, iso, bestTeam, bestPhi, secondPhi, F);
    // Camera-inside is ONE point per frame, evaluated on the CPU (buildUboForce forceBake2.w) —
    // never re-sampled per fragment.
    const bool cameraInsideField = t0 > 0.0 ? u_forceBake2.w > 0.5 : F > 0.0;
    uint prevTeam = bestTeam;
    float tPrev = t0;
    float fPrev = F;
    vec3 accumColor = vec3(0.0);
    float accumAlpha = 0.0;
    int numShaded = 0;
#ifdef FORCE_GRID
    // Per-fragment hoists: the current cell's hash probe re-runs only when a step crosses a 16 m
    // cell boundary, and each big emitter's support-sphere ray ENTRY (for the empty-stretch jump
    // clamp) is a constant of the ray — computed once, not per empty sample.
    ivec3 cachedGridPos = ivec3(0x7FFFFFFF);
    uint cachedCell = FORCE_INVALID_CELL;
    float bigEntry[8];
    const uint numBigsHoisted = min(fe_bigCount, 8u);
    for (uint k = 0u; k < numBigsHoisted; ++k)
        bigEntry[k] = forceBigSupportEntry(rayOrigin, rayDir, fe_emitters[fe_bigIndices[k]]);
#endif
    // Static per-pixel phase jitter breaks the march's step-count banding into spatial noise —
    // larger "Union step (m)" settings stay presentable. Purely spatial (no frame term), so shells
    // never shimmer with TAA off; crossings still bisect to the exact surface either way.
    const float stepJitter = forceHash(vec3(gl_FragCoord.xy, 0.0));
    for (int i = 1; i <= steps && numShaded < 3 && accumAlpha < 0.98; ++i) // saturated: nothing behind shows
    {
        const float t = t0 + dt * (float(i) - stepJitter);
        bool sampledEmpty = false;
#ifdef FORCE_GRID
        // EMPTY-CELL FAST PATH: a cell with NO candidates holds no SMALL emitters (the grid
        // insert covers every support) — provable, not heuristic — so the sample reduces to the
        // big list + ambient (forceSampleFieldBigOnly, typically 0-2 emitters). After the
        // crossing logic below the index also JUMPS past the empty stretch where that is safe.
        const vec3 samplePos = rayOrigin + rayDir * t;
        const ivec3 gridPos = forceGridPos(samplePos);
        if (any(notEqual(gridPos, cachedGridPos)))
        {
            cachedGridPos = gridPos;
            cachedCell = forceFindCell(gridPos);
        }
        if (cachedCell == FORCE_INVALID_CELL || forceCellCount(cachedCell) == 0u)
        {
            sampledEmpty = true;
            forceSampleFieldBigOnly(samplePos, iso, prevTeam, bestTeam, bestPhi, secondPhi, F);
        }
        else
            forceSampleFieldCell(samplePos, cachedCell, iso, bestTeam, bestPhi, secondPhi, F);
#else
        forceSampleField(rayOrigin + rayDir * t, iso, bestTeam, bestPhi, secondPhi, F);
#endif
        const bool entryCrossing = F > 0.0;
        const bool surfaceCrossing = entryCrossing != (fPrev > 0.0);
        const bool teamFlip = bestTeam != prevTeam && (entryCrossing || fPrev > 0.0);
        if (surfaceCrossing || teamFlip)
        {
            float tHit = -1.0;
            uint hitTeam = 0u;
            if (surfaceCrossing)
            {
                hitTeam = entryCrossing ? bestTeam : prevTeam;
                float lo = tPrev, hi = t;
                // 5 refinements (not the shell FS's 6): this is the pixel's ONLY march, so there
                // is no cross-proxy hit-error matching to satisfy — bracket/32 stays under the
                // normal's finite-difference step at the union tier's step sizes.
                for (int b = 0; b < 5; ++b)
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
                for (int b = 0; b < 5; ++b)
                {
                    const float mid = (lo + hi) * 0.5;
                    if (forceTeamDiff(rayOrigin + rayDir * mid, prevTeam, bestTeam) > 0.0) lo = mid; else hi = mid;
                }
                tWall = (lo + hi) * 0.5;
                float wallOwn, wallOpposing;
                forceTeamSample(rayOrigin + rayDir * tWall, prevTeam, wallOwn, wallOpposing);
                wallFade = smoothstep(iso, iso * 1.3, min(wallOwn, wallOpposing));
                if (wallFade <= 0.001)
                    tWall = -1.0;
                if (tWall >= 0.0)
                {
                    // Same invisible-field reclassification as the shell FS: a wall against a
                    // shell-alpha-0 field IS the visible side's pressed-flat surface.
                    float phiW[NUM_FORCE_TEAMS];
                    float phiVisW[NUM_FORCE_TEAMS];
                    forceAccumulateVisible(rayOrigin + rayDir * tWall, phiW, phiVisW);
                    const float visPrev = phiVisW[prevTeam] / max(phiW[prevTeam], 1e-6);
                    const float visBest = phiVisW[bestTeam] / max(phiW[bestTeam], 1e-6);
                    if (min(visPrev, visBest) < 0.05)
                    {
                        wallSkinTeam = visPrev > visBest ? prevTeam : bestTeam;
                        if (tHit >= 0.0)
                            tWall = -1.0;
                    }
                }
            }
            // Composite the step's events in ray order. The only skip: a crossing owned by a
            // sampled-tier emitter (its own proxy draws it) — everything else is ours, exactly
            // once, because this is the pixel's ONLY analytic march.
            for (int ev = 0; ev < 2 && numShaded < 3; ++ev)
            {
                const bool wallFirst = tWall >= 0.0 && (tHit < 0.0 || tWall < tHit);
                if (wallFirst)
                {
                    const bool asSkin = wallSkinTeam != MAX_FORCE_TEAMS;
                    const uint ownTeam = asSkin ? wallSkinTeam : prevTeam;
                    uint ownerIdx;
                    if (!forceSampledTierOwns(rayOrigin + rayDir * tWall, ownTeam, ownerIdx))
                    {
                        const float h = forceShadeNormalH(ownerIdx);
                        const vec3 wallPos = rayOrigin + rayDir * tWall;
                        const vec4 layer = asSkin
                            ? forceShadeHit(rayOrigin, rayDir, tWall, wallSkinTeam, cameraInsideField, sceneDist, ownerIdx,
                                forceSurfaceNormal(wallPos, wallSkinTeam, iso, h))
                            : forceShadeWall(rayOrigin, rayDir, tWall, prevTeam, bestTeam, wallFade, ownerIdx,
                                forceWallNormal(wallPos, prevTeam, bestTeam, h));
                        accumColor += (1.0 - accumAlpha) * layer.rgb;
                        accumAlpha += (1.0 - accumAlpha) * layer.a;
                        ++numShaded;
                    }
                    tWall = -1.0;
                }
                else if (tHit >= 0.0)
                {
                    uint ownerIdx;
                    if (!forceSampledTierOwns(rayOrigin + rayDir * tHit, hitTeam, ownerIdx))
                    {
                        const vec4 layer = forceShadeHit(rayOrigin, rayDir, tHit, hitTeam, cameraInsideField, sceneDist, ownerIdx,
                            forceSurfaceNormal(rayOrigin + rayDir * tHit, hitTeam, iso, forceShadeNormalH(ownerIdx)));
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
#ifdef FORCE_GRID
        // JUMP the INDEX past the empty cell's exit (uniform dt preserved, so the refinement
        // brackets stay one step wide), and move the bracket's start to the exit — the skipped
        // stretch is provably zero, and a bracket spanning it would cost the bisection its
        // accuracy at the next bubble's entry. Valid only with the ambient field off (it varies
        // everywhere), and the target additionally clamps to each big emitter's support ENTRY
        // (bounding sphere, conservative) — a big field beginning mid-cell must still be sampled.
        // Many bigs make the per-step clamp loop cost more than the jump saves: then just no jump.
        if (sampledEmpty && u_forceParams4.z <= 0.0 && fe_bigCount <= 8u)
        {
            const vec3 p = rayOrigin + rayDir * t;
            const vec3 farBound = (floor(p / FORCE_GRID_CELL_SIZE)
                + step(vec3(0.0), rayDir)) * FORCE_GRID_CELL_SIZE;
            const vec3 safeDir = rayDir + vec3(equal(rayDir, vec3(0.0))) * 1e-8;
            const vec3 tBounds = (farBound - rayOrigin) / safeDir;
            float tExit = min(min(min(tBounds.x, tBounds.y), tBounds.z), t1);
            for (uint k = 0u; k < numBigsHoisted && tExit > t; ++k)
                tExit = min(tExit, bigEntry[k]); // hoisted ray-constant support entries
            if (tExit > t)
            {
                // ++i lands on the first (jittered) sample past the exit.
                i = max(i, int((tExit - t0) / dt + stepJitter));
                tPrev = tExit; // still inside the empty zero-field stretch: F there is the same known negative
            }
        }
#endif
    }
    if (accumAlpha <= 0.002 && dot(accumColor, accumColor) < 1e-6)
        discard;

    out_color = vec4(accumColor, accumAlpha);
}
