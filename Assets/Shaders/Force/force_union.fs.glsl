#version 460

// The ANALYTIC tier's UNION MARCH: one fullscreen march per covered pixel over the interval the small-emitter proxies
// rasterized (force_interval.fs.glsl) - where N small bubbles stack on a pixel, the field is marched ONCE instead of once
// per proxy. Marching and shading match force_shell.fs.glsl (same crossing / wall logic, shared shading include). No
// ownership discard: ONE march owns every crossing, except a crossing whose dominant contributor is a SAMPLED-tier
// (large) emitter, which that emitter's own proxy draws. Premultiplied blend, manual reversed-Z depth clamp, no depth write.

#include "shared.inc.glsl"
#include "force_field.inc.glsl" // declares the emitter buffer at FORCE_EMITTERS_BINDING (1)

#ifndef FORCE_UNION_UV_SCALE
#define FORCE_UNION_UV_SCALE 1.0 // injected 2.0 when the march runs at half res
#endif

layout (binding = 2) uniform sampler2D u_sceneDepth;
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
    return u_force_bakeEnabled > 0.5 && forceVisibleRadius(fe_emitters[ownerIdx]) >= u_force_bakeThreshold;
}

void main()
{
    g_viewIndex = int(u_viewIndex);
    const float iso = u_force_isoThreshold;

    const vec2 interval = texelFetch(u_shellInterval, ivec2(gl_FragCoord.xy), 0).xy;
    float t0 = interval.x;
    float t1 = -interval.y;
    if (t0 > 6.0e4 || t1 <= t0)
        discard; // cleared (no analytic shell covers this pixel)

    // FORCE_UNION_UV_SCALE maps this pass's gl_FragCoord back to full-res uv (the interval target always matches this
    // pass's resolution, so its texelFetch above needs no scale).
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw * FORCE_UNION_UV_SCALE;
    const vec3 rayOrigin = u_viewPos;
    const vec3 rayDir = normalize(worldPosFromDepth(uv, 0.0) - rayOrigin);

    // Manual depth test: clamp the march to the opaque scene (once - not per proxy).
    const float sceneDepth = texture(u_sceneDepth, uv).r;
    float sceneDist = 1e30;
    if (sceneDepth > 0.0) // reversed-Z: 0 = sky/far
    {
        sceneDist = distance(rayOrigin, worldPosFromDepth(uv, sceneDepth));
        t1 = min(t1, sceneDist);
    }
    if (t1 <= t0)
        discard;

    // World-space step size, growing with DISTANCE so a far pixel never marches finer than ~2 px of world size
    // (u_force_unionStepPerDist), hard-capped: the union interval can span several disjoint bubbles, so the step count
    // follows its LENGTH instead of a fixed budget squeezed over it.
    const float stepSize = max(u_force_unionStepSize, t0 * u_force_unionStepPerDist);
    const int steps = clamp(int((t1 - t0) / stepSize), 4, int(u_force_unionMaxSteps));
    const float dt = (t1 - t0) / float(steps);

    uint bestTeam;
    float bestPhi, secondPhi, F;
    forceSampleField(rayOrigin + rayDir * t0, iso, bestTeam, bestPhi, secondPhi, F);
    // Camera-inside is ONE point per frame, evaluated on the CPU (buildUboForce) - see force_shell.fs.
    const bool cameraInsideField = t0 > 0.0 ? u_force_cameraInside > 0.5 : F > 0.0;
    uint prevTeam = bestTeam;
    float tPrev = t0;
    float fPrev = F;
    vec3 accumColor = vec3(0.0);
    float accumAlpha = 0.0;
    int numShaded = 0;
#ifdef FORCE_GRID
    // The cell's hash probe re-runs only when a step crosses a cell boundary.
    ivec3 cachedGridPos = ivec3(0x7FFFFFFF);
    uint cachedCell = FORCE_INVALID_CELL;
#endif
    // Static per-pixel phase jitter ("Force/Shell/Union jitter") breaks the step-count banding into spatial noise, so
    // larger "Union step (m)" settings stay presentable. No frame term: shells never shimmer with TAA off.
#ifdef FORCE_UNION_JITTER
    const float stepJitter = forceHash(vec3(gl_FragCoord.xy, 0.0));
#else
    const float stepJitter = 0.0; // constant-folds out of the loop below
#endif

    for (int i = 1; i <= steps && numShaded < 3 && accumAlpha < 0.98; ++i) // saturated: nothing behind shows
    {
        const float t = t0 + dt * (float(i) - stepJitter);
        bool sampledEmpty = false;
#ifdef FORCE_GRID
        // EMPTY-CELL FAST PATH: a cell with NO candidates provably has zero field (the grid insert covers every support).
        // After the crossing logic the index also JUMPS past the empty stretch.
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
            bestTeam = prevTeam; // zero field: never a spurious team flip through empty space
            bestPhi = 0.0;
            secondPhi = 0.0;
            F = -forceOpposingBound(iso, 0.0);
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
                // 5 refinements (not the shell FS's 6): the pixel's ONLY march has no cross-proxy hit error to match -
                // bracket/32 stays under the normal's finite-difference step at the union tier's step sizes.
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
                    // Same invisible-field reclassification as the shell FS.
                    float phiW[NUM_FORCE_TEAMS];
                    float phiVisW[NUM_FORCE_TEAMS];
                    forceAccumulateVisible(rayOrigin + rayDir * tWall, phiW, phiVisW);
                    const float visPrev = forcePhiAt(phiVisW, prevTeam) / max(forcePhiAt(phiW, prevTeam), 1e-6);
                    const float visBest = forcePhiAt(phiVisW, bestTeam) / max(forcePhiAt(phiW, bestTeam), 1e-6);
                    if (min(visPrev, visBest) < 0.05)
                    {
                        wallSkinTeam = visPrev > visBest ? prevTeam : bestTeam;
                        if (tHit >= 0.0)
                            tWall = -1.0;
                    }
                }
            }

            // Composite the step's events in ray order, skipping crossings owned by a sampled-tier emitter.
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
        // JUMP the INDEX past the empty cell's exit (uniform dt kept, so refinement brackets stay one step wide), and move
        // the bracket's start to the exit: a bracket spanning the zero stretch would cost the bisection its accuracy at
        // the next bubble's entry.
        if (sampledEmpty)
        {
            const vec3 p = rayOrigin + rayDir * t;
            const vec3 farBound = (floor(p / FORCE_GRID_CELL_SIZE) + step(vec3(0.0), rayDir)) * FORCE_GRID_CELL_SIZE;
            const vec3 safeDir = rayDir + vec3(equal(rayDir, vec3(0.0))) * 1e-8;
            const vec3 tBounds = (farBound - rayOrigin) / safeDir;
            const float tExit = min(min(min(tBounds.x, tBounds.y), tBounds.z), t1);
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
