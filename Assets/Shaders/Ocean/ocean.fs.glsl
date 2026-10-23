#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable // the BRDF colour side is half (punctual_lights.inc.glsl)
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// FFT ocean surface shading (EPipelineIndex::Ocean). The VS passes the undisplaced world XZ in in_uv; the gradient maps
// rebuild a per-pixel wave normal + fold Jacobian, then: Fresnel split (Schlick, F0 = 0.02) between the RAY-TRACED
// refracted body (Snell n = 1.33, Beer-Lambert both ways, closed-form single-scatter; the water has TLAS mask 0, so rays
// pass through) and the RAY-TRACED mirror (sky fallback). GGX/Smith sun glint widened by spec AA + LEAN-filtered slope
// variance (Bruneton 2010: the elongated glitter at distance), Jacobian whitecap foam with the world-space stuck-foam
// field + its bubble cloud (ocean_foam.cs.glsl), one RT sun shadow ray. Ray budgets: "Ocean/RT" tweaks.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "ocean_wave.inc.glsl"
#include "ocean_bubbles.inc.glsl"
#include "ocean_foam_field.inc.glsl"
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl" // the edge fade's film surface: the pool level the wetness fills to

#define WATER_ABSORPTION u_ocean_absorption
#include "water_scene.inc.glsl" // the bindings, the BRDF terms, the sky lookups, traceScene / shadeHit

layout (location = 0) in vec3 in_pos;                       // displaced world position
layout (location = 4) in vec2 in_uv;                        // undisplaced world XZ
layout (location = 5) in flat uint in_meshIdxMaterialIdx;
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// DUAL-SOURCE composite: out = out_color + scene * out_factor, the alpha too. Opaque (factor 0, alpha 0 = TAA's ocean flag)
// except over the edge fade, where the ground and film show through.
layout (location = 0, index = 0) out vec4 out_color;
layout (location = 0, index = 1) out vec4 out_factor;

// --- Debug view ("Ocean/Debug mode" tweak). Paints the depth-keyed terms instead of shading, to find which one's boundary
// a visible line on the water follows.
//   1 = calm depth      black 0 -> white 4 m, green iso-lines every 0.5 m, RED = land (depth < 0)
//   2 = swash weight    the tongue weight (backflow): white = full, black = none
//   3 = surface weight  oceanSurfaceWeight: white = open water, darker = eased toward the swash amplitude
//   5 = shore foam band the surf band's nearShore target (u_ocean_shoreFoamDepth)
//   6 = mirror ray      GREEN = scene hit (pink-white near, pure green far), BLUE = fired, missed (TLAS has
//                       geometry), WHITE = hit only past "Reflection range", GREY = nothing hits anywhere
//                       (the TLAS is EMPTY - every RT effect is dead, not just this ray),
//                       RED = skipped as invisible (mirror weight <= 2%: nadir, foam, blur), YELLOW = skipped by "Reflection max rough",
//                       MAGENTA = skipped by "Ray cutoff dist", BLACK = OCEAN_RT_REFLECTIONS off
//                       UNDERSIDE: the same colours for the WINDOW ray (the scene above the water);
//                       outside Snell's window CYAN = TIR, mirror traced, DARK CYAN = TIR, mirror skipped
// Modes 1-5 are position-keyed and show on both sides of the surface.
// Set by the "Ocean/Debug mode" tweak (a define on this variant; toggling reloads the pipeline).
#ifndef OCEAN_DEBUG_MODE
#define OCEAN_DEBUG_MODE 0
#endif

// fp16 copies for the top side's shading block (GGX D is punctual_lights.inc.glsl's D_GGX_H).
float16_t V_SmithGGX_H(float16_t NoV, float16_t NoL, float16_t a)
{
    const float16_t a2 = a * a;
    const float16_t ggxV = NoL * sqrt(NoV * NoV * (float16_t(1.0) - a2) + a2);
    const float16_t ggxL = NoV * sqrt(NoL * NoL * (float16_t(1.0) - a2) + a2);
    return float16_t(0.5) / max(ggxV + ggxL, float16_t(1e-4));
}

float16_t F_SchlickH(float16_t u, float16_t f0)
{
    const float16_t x = float16_t(1.0) - u;
    const float16_t x2 = x * x;
    return f0 + (float16_t(1.0) - f0) * (x2 * x2 * x);
}

// The seabed (water_scene.inc.glsl's bed hook). waterLevel returns the calm level the fields carry: the hit's column,
// one terrain fetch for both.
vec3 waterBedAlbedo(vec3 worldPos, vec3 geoN, float rayT, inout float waterLevel)
{
    TerrainFields f; // mild-climate fallbacks without a map, as the terrain VS
    f.altitude = worldPos.y - u_terrain_seaLevel;
    f.temperature = 12.5;
    f.humidity = 0.5;
    f.waterLevel = u_terrain_seaLevel;
    f.river = 0.0; // the seabed has no river bed (the mesh's river value is not read here)
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(worldPos.xz);
        f.altitude = td.w;
        f.waterLevel = td.y;
        // NEAREST climate texel (the terrain VS bilinears): through water at a ray-cone LOD the texel grid never shows,
        // and it is one decode instead of four.
        const vec4 climate = terrainClimateNearestAt(worldPos.xz);
        f.humidity = climate.w;
        f.temperature = terrainTemperatureAt(climate, worldPos.y);
    }
    waterLevel = f.waterLevel;

    g_seabedLod = min(log2(max(rayT, 1.0)) + 1.0, 7.0);
    f16vec3 albedo = terrainSplat(worldPos, geoN, geoN, f).albedo;
    // The seabed is fully wet: darken it as the terrain shader darkens ground at full wetness ("Wet darkening"), so the
    // sand under the water and the wet sand it just left match at the waterline.
    if (u_terrainWater_enabled > 0.5)
        albedo *= float16_t(u_terrainWater_darkening);
    return vec3(albedo);
}

// The water body along `dir` from `origin` (a surface point offset to the water side): the scene hit (TLAS; the baked
// terrain height field ONLY where the TLAS cannot answer), shaded, Beer-Lambert absorbed and blended into the in-scatter.
// Bounded at ~99% extinction and "Refraction range" (the bottom fades over its last 25%: a hard cutoff draws a contour).
// surfPos/shoreHW: the pixel's surface point and shore fetch, the height-field fallback's first estimate. The bottom is
// measured from the SURFACE point, not the calm level (the waterline band would reject otherwise), and never rejected when
// negative: terrain stood behind the water in the depth test, so a bottom exists (else a map-vs-mesh disagreement draws a
// deep-blue line along the shore).
vec3 traceWaterBody(vec3 origin, vec3 dir, vec3 surfPos, vec2 shoreHW, vec3 sunTint, float sunVis, vec3 L, vec3 inscatter, bool rtInRange)
{
    const vec3 sigmaT = u_ocean_absorption;
    const float range = u_ocean_rtRefractionRange;
    const float tMax = u_ocean_bodyTMax;
    SceneHit hit;
    // A ray the TLAS can answer is FINAL: a miss = no bottom within tMax = the in-scatter. The height field stands in only
    // where the TLAS cannot answer: rays off ("Ray cutoff dist"), or the reach leaves the TLAS range ("RT/TLAS Range").
    const bool traced = rtInRange && distance(surfPos, u_sceneFocus.xyz) + tMax < u_rt_giTlasRange;
    bool haveHit = rtInRange && traceScene(origin, dir, tMax, -1e30, hit);
    if (haveHit && hit.waterLevel < -1e29) // a non-terrain hit: one shore fetch for its column
        hit.waterLevel = oceanSampleShoreData(hit.pos.xz).y;
    if (!haveHit && !traced && dir.y < -0.02)
    {
        float bottom = surfPos.y - shoreHW.x;
        const float t = max(bottom, 0.02) / -dir.y;
        bottom = surfPos.y - oceanSampleShoreData(surfPos.xz + dir.xz * t).x; // one refinement for sloped shelves
        hit.t = max(bottom, 0.02) / -dir.y;
        // Past tMax the bottom is extinct: the in-scatter, not a hit pulled in to tMax.
        if (hit.t < tMax)
        {
            hit.pos = surfPos + dir * hit.t;
            hit.N = vec3(0.0, 1.0, 0.0);
            hit.albedo = waterBedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
            haveHit = true;
        }
    }
    if (!haveHit)
        return inscatter;

    const float sunPath = max(hit.waterLevel - hit.pos.y, 0.0) / max(L.y, 0.25); // column above the hit
    const vec3 hitRadiance = shadeHit(hit, dir, sunTint * sunVis * exp(-sigmaT * sunPath), L);
    const vec3 T = exp(-sigmaT * hit.t) * (1.0 - smoothstep(0.75 * range, range, hit.t));
    return hitRadiance * T + inscatter * (1.0 - T);
}

// THE EDGE FADE ("Terrain/Water/Ocean edge fade (m)"; 0 = off, returns 1): over its last centimetres of water column the
// ocean composites over the ground and the film drawn before it (the cull puts the ocean in the transparent sequence), so
// its mesh does not end in a hard line - the film carries the water on.
// The column is to the FILM's water surface under this pixel: the film's tessellated surface (terrain_tess.tes.glsl)
// without the splat relief - terrain height + the pool level the wetness fills the relief band to (terrainPoolLevel), in
// the ground relief depth with the tessellation's distance falloff (0 past it, or with tessellation off). At the pixel's
// own position (in_pos.xz), not in_uv's lattice point. Ease-out 1 - (1 - t)^2: transparent at the bottom, mostly opaque
// above. Computed from nothing main has, so no value of main's stays live for it (the no-map fallback is
// oceanSampleShoreData's open-ocean floor, re-derived from the UBO).
float oceanEdgeCover()
{
    float filmY = u_ocean_seaLevel - u_ocean_depth;
    if (terrainHeightMapPresent())
    {
        filmY = terrainHeightAt(in_pos.xz);
        float reliefDepth = 0.0;
        if (u_terrainTess_enabled > 0.5 && u_terrain_splatBase >= 0.0 && u_terrain_numGround >= 1.0)
        {
            // The film's lift (instanced_indirect_terrain.vs.glsl): the centre view and the HEIGHT falloff.
            const float tf = clamp((distance(in_pos, u_views_viewPos[VIEW_CENTER].xyz) - u_terrainTess_fadeStart)
                * u_terrainTess_invFadeWidth, 0.0, 1.0);
            reliefDepth = u_terrainTess_depthGround * (1.0 - pow(tf, u_terrainTess_heightFalloff));
        }
        // The pool level sinks on slopes ("Film max slope"), as the film's: the normal's y from the baked map's forward
        // gradient over 4 m (the film reads its smooth mesh normal; the map is what the ocean has).
        const vec2 grad = (vec2(terrainHeightAt(in_pos.xz + vec2(4.0, 0.0)), terrainHeightAt(in_pos.xz + vec2(0.0, 4.0))) - filmY) * 0.25;
        const float normalY = inversesqrt(1.0 + dot(grad, grad));
        filmY += (terrainPoolLevel(terrainWetnessAt(in_pos.xz), normalY) - 0.5) * reliefDepth;
    }

    const float s = 1.0 - clamp((in_pos.y - filmY) / u_terrainWater_oceanEdgeFade, 0.0, 1.0);
    return 1.0 - s * s;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    // out_factor (the dual-source dst multiplier) is written AT each return, never as a default up here: an early store
    // plus the edge fade's dynamic one kept the output live through the whole shader (80/16 -> 80/32). Every early return
    // (the debug views, the underside) writes it opaque: vec4(0).
    const vec3 up = u_skyUp;
    const vec3 L  = u_sunDirection.xyz;
    const vec3 toCam = u_viewPos - in_pos;
    const float viewDist = length(toCam);
    const vec3 V = toCam / max(viewDist, 1e-4);

    // Entrained bubbles: the cloud's coverage where the refracted view ray reaches "Bubble depth", so the cloud slides
    // under the surface with the view. Blurred by "Bubble blur (m)" (a diffuse volume). The refracted ray is never flatter
    // than the critical angle, so the parallax stays below 1.52x the depth.
    // FIRST in main, where almost nothing is live: its B-spline taps (up to 16) inside the top side's shading set the
    // register peak (80/32 -> 80/48); here only the one result stays live. So it refracts through the LEVEL plane, not
    // the rippled normal (metres of blur anyway), and the underside pixels pay the tap for nothing.
    const float milkF = oceanSampleFoamField(u_oceanMaps,
        in_uv + refract(-V, vec3(0.0, 1.0, 0.0), 1.0 / 1.33).xz * (u_ocean_bubbleDepth / sqrt(1.0 - (1.0 - V.y * V.y) / (1.33 * 1.33))),
        length(fwidth(in_uv)), u_ocean_bubbleBlur);

    // Per-frame atmosTransmittanceToLight x the cloud shadow at the surface (the glint, the body, the foam).
    const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * (u_sunVisible * cloudSunTransmittance(in_pos));
    // "Ray cutoff dist" (0 = unlimited): beyond it no scene rays - the body uses the analytic bottom, reflection the sky.
    const bool rtInRange = u_ocean_rtRayCutoff <= 0.0 || viewDist < u_ocean_rtRayCutoff;

    // shoreHW = (terrain height, water level) here, reused by the surf band and SSS below.
    vec2 slope, slopeVar, shoreHW, detailSlope;
    float jacobian, jacobianRaw, accel, foamJacobian;
    oceanSampleSurface(in_uv, slope, jacobian, jacobianRaw, slopeVar, accel, shoreHW, detailSlope, foamJacobian);
    const float ns = u_ocean_normalStrength;
    vec3 N = normalize(vec3(-slope.x * ns, 1.0, -slope.y * ns));

    // The FOAM's lighting slope: foam is a rough, thick sheet, so "Foam flatten" eases the large waves' slope toward flat
    // (bent crest facets fall to NoL 0 in dark blotches), while the sub-band detail stays x "Foam detail" (the foam's
    // relief, as the film's foam wears its detailed normal). From the slope, so it always faces up. Resolved to its N.L
    // here: else the slope (two values) stays live across the sun shadow ray query.
    float16_t foamNoL;
    {
        const f16vec2 foamSlope = f16vec2(((slope - detailSlope) * (1.0 - u_ocean_foamFlatten) + detailSlope * u_ocean_foamDetail) * ns);
        const f16vec3 foamN = normalize(f16vec3(-foamSlope.x, float16_t(1.0), -foamSlope.y));
        foamNoL = max(dot(foamN, f16vec3(L)), float16_t(0.0));
    }
    // Screen derivatives up front: the underside returns early, and derivatives are undefined in non-uniform control flow.
    const vec3 faceN = cross(dFdx(in_pos), dFdy(in_pos));
    const float uvFootprint = length(fwidth(in_uv));

    // Foam, BEFORE the side split: both sides wear it, and the stuck foam's edge takes screen derivatives, which the
    // underside branch cannot. The foam field at this pixel's REST position (ocean_foam_field.inc.glsl); the crest foam is
    // the live Jacobian's. The density's screen derivative widens the stuck foam's edge (AA).
    const float foamAmount = oceanSampleFoamField(u_oceanMaps, in_uv, uvFootprint, 0.0);
    const float stuckDensity = oceanStuckFoamDensity(foamAmount * u_ocean_foamStrength, foamJacobian);
    const float stuckFoam = oceanStuckFoamCoverage(stuckDensity, 0.5 * fwidth(stuckDensity));
    const float foam = max(oceanInstantFoam(jacobian, accel), stuckFoam);

    // Shoreline surf band: coverage target from the breaking bore front + the waterline, realized through the RAW
    // (un-shoaled) fold Jacobian, so it reads as filaments along the swell, not a painted gradient.
    float shoreFoam = 0.0;
    const float shoreFoamDepth = u_ocean_shoreFoamDepth;
    if (shoreFoamDepth > 0.0)
    {
        // Same floored depth the waves use: a distant too-shallow reading would drive the waterline lace term across whole
        // bays as a white wash.
        const float shoreDepth = oceanEffectiveDepth(in_uv, shoreHW.y - shoreHW.x);
        const float waveH = in_pos.y - shoreHW.y;               // surface height above the LOCAL calm water level
        const float column = max(shoreDepth, 0.0) + waveH;      // instantaneous water column at this pixel
        const float nearShore = 1.0 - smoothstep(shoreFoamDepth, 4.0 * shoreFoamDepth, shoreDepth);
        const float bore = max(waveH, 0.0) / max(column, 0.05); // crest fraction of the column (bore front)
        float target = nearShore * smoothstep(0.4, 0.8, bore);
        target = max(target, 1.0 - smoothstep(0.0, 0.35 * shoreFoamDepth, column));

        if (target > 0.001)
        {
            // The target fades the lace in through its THRESHOLD only (as a coverage multiplier it is a grey veil). At the
            // band's outer edge only folds 0.8 past "Fold bias" foam (a subset of the crest foam: no seam); toward the
            // waterline it rises to 1.45 and the lace fills in, its strands solid white throughout.
            const float b = mix(u_ocean_foamBias - 0.8, 1.45, target) + u_ocean_shoreFoamBias; // "Shore foam bias"
            shoreFoam = 1.0 - smoothstep(b - 0.4, b + 0.4, jacobianRaw);
            // "Shore foam max": keep the bottom visible through the lace. A soft knee, not a min() (a hard clamp makes a
            // plateau with an edge). NORMALISED so full lace (1) reaches the cap exactly (the bare knee
            // fm (1 - e^(-x / fm)) gives only 1 - 1/e at full lace: a grey veil next to the offshore foam).
            shoreFoam = u_ocean_shoreFoamMax * (1.0 - exp(-shoreFoam * u_ocean_shoreFoamInvMax)) * u_ocean_shoreFoamKneeNorm;
        }
    }
    const float foamW = max(foam, shoreFoam);

#if OCEAN_DEBUG_MODE != 0 && OCEAN_DEBUG_MODE != 6
    // The depth-keyed modes are a function of the position alone: before the side split, so the underside shows them too.
    {
        const float depthDbg = oceanEffectiveDepth(in_uv, shoreHW.y - shoreHW.x);
        const float swDbg = oceanSwashWeight(depthDbg, shoreHW.y);
        vec3 dbg = vec3(0.0);
#if OCEAN_DEBUG_MODE == 1
        dbg = depthDbg < 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(clamp(depthDbg / 4.0, 0.0, 1.0));
        if (depthDbg >= 0.0 && fract(depthDbg * 2.0) < 0.05)
            dbg.g = 1.0;
#elif OCEAN_DEBUG_MODE == 2
        dbg = vec3(swDbg / u_ocean_swashAmp);
#elif OCEAN_DEBUG_MODE == 3
        dbg = vec3(oceanSurfaceWeight(depthDbg, shoreHW.y));
#elif OCEAN_DEBUG_MODE == 5
        dbg = vec3(u_ocean_shoreFoamDepth > 0.0 ? 1.0 - smoothstep(u_ocean_shoreFoamDepth, 4.0 * u_ocean_shoreFoamDepth, depthDbg) : 0.0);
#endif
        out_color = vec4(dbg, 1.0);
        out_factor = vec4(0.0);
        return;
    }
#endif

    // Underside (camera on the water side of this triangle): through Snell's window the scene above or the sky, outside it
    // (TIR) the mirrored water body, surface foam over both. The side comes from the triangle's plane, not gl_FrontFacing:
    // the clipmap carries both windings under back-face culling, so the survivor is always front-facing. Only while the
    // camera is under water (u_ocean_cameraUnderwater, the CPU mirror): a back face seen from above is a FOLD (choppiness
    // overturns the sheet) and shades as the top side (grazing flip; foam lights on the un-flipped, flattened normal).
    if (u_ocean_cameraUnderwater > 0.5 && dot(faceN, V) * dot(faceN, N) < 0.0)
    {
        // Keep the detail normal inside the camera's hemisphere at grazing (a flip would open the window at TIR angles).
        const float nv = dot(N, V);
        if (nv > -1e-3)
            N = normalize(N - V * (nv + 1e-3));

        // The underwater fog carries the water column only within its 100-300 m fade (vol_scatter); the path beyond is
        // absorbed here, on the same curve. Past 0.1% nothing this pixel traces can show (not 2%, the rays' own cut: the
        // window's sun glitter is ~30x the sky).
        const vec3 pathAbsorb = exp(-u_ocean_absorption * (viewDist * smoothstep(100.0, 300.0, viewDist)));
        if (max(pathAbsorb.r, max(pathAbsorb.g, pathAbsorb.b)) < 0.001)
        {
            out_color = vec4(0.0, 0.0, 0.0, 1.0);
            out_factor = vec4(0.0);
            return;
        }

        const vec3 ambientSkyU = skyAmbientUp(up);
        const vec3 inscatterU = u_ocean_scatterColor * u_ocean_scatterStrength * (ambientSkyU + sunTint * max(L.y, 0.0) / PI);
        // The window's transmission: Schlick on the transmitted (air-side) cosine, 0 at the critical angle, x "Underside
        // transmission". It splits the pixel between the window (trans) and the mirror (1 - trans).
        const vec3 refrUp = refract(-V, -N, 1.33);
        const bool inWindow = dot(refrUp, refrUp) > 1e-6;
        const vec3 tDir = inWindow ? normalize(refrUp) : vec3(0.0, 1.0, 0.0);
        const float trans = inWindow ? (1.0 - F_Schlick(clamp(dot(tDir, N), 0.0, 1.0), 0.02)) * u_ocean_undersideTransmission : 0.0;
        // Surface foam covers both from below (laid over at the end), so it scales both rays' weights; under 2% a ray is skipped.
        const float clearW = 1.0 - foamW;
        const bool traceMirror = (1.0 - trans) * clearW > 0.02;
#if OCEAN_DEBUG_MODE == 6
        {
            // The underside's scene ray is the WINDOW ray; outside the window (TIR) there is only the mirror.
            vec3 dbg = vec3(0.0);
            if (!inWindow)
                dbg = traceMirror ? vec3(0.0, 1.0, 1.0) : vec3(0.0, 0.3, 0.3);
#ifdef OCEAN_RT_REFLECTIONS
            else if (trans * clearW <= 0.02)
                dbg = vec3(1.0, 0.0, 0.0);
            else if (!rtInRange)
                dbg = vec3(1.0, 0.0, 1.0);
            else
            {
                SceneHit dbgHit;
                if (traceScene(in_pos + N * 0.05, tDir, u_ocean_rtReflectionRange, -1e30, dbgHit))
                    dbg = vec3(0.0, 1.0, 0.0) + vec3(0.8, 0.0, 0.8) * exp(-dbgHit.t * 0.05);
                else if (rtShadowVisibility(in_pos + N * 0.05, tDir, 0.05, 100000.0) < 0.5)
                    dbg = vec3(1.0);
                else if (rtShadowVisibility(u_viewPos, vec3(0.0, -1.0, 0.0), 0.05, 100000.0) < 0.5)
                    dbg = vec3(0.0, 0.0, 1.0);
                else
                    dbg = vec3(0.3);
            }
#endif
            out_color = vec4(dbg, 1.0);
            out_factor = vec4(0.0);
            return;
        }
#endif

        float sunVisU = 0.0; // for the mirrored seabed and the foam's backlight
        if (L.y > 0.0 && (traceMirror || foamW > 0.003))
            sunVisU = u_rt_sunShadow > 0.5 ? rtShadowVisibility(in_pos + N * 0.1, L, 0.05, 10000.0) : sampleSunShadowHard(in_pos, N);

        // The mirror: the seabed and submerged shore - the same traced water body the top side refracts into.
        vec3 color = inscatterU;
        if (traceMirror)
            color = traceWaterBody(in_pos - N * 0.05, reflect(-V, N), in_pos, shoreHW, sunTint, sunVisU, L, inscatterU, rtInRange);
        if (inWindow)
        {
            // The window: the scene above along the refracted ray, else the sky with the sun's glitter. The top side's
            // mirror ray but for the fog: the viewer is under water, so the mirror rule has no direct view to match and
            // the LITERAL path is fogged.
            vec3 above;
            bool aboveHit = false;
#ifdef OCEAN_RT_REFLECTIONS
            if (trans * clearW > 0.02 && rtInRange)
            {
                SceneHit hit;
                aboveHit = traceScene(in_pos + N * 0.05, tDir, u_ocean_rtReflectionRange, -1e30, hit); // "Reflection range"
                if (aboveHit)
                    above = applyRayFog(shadeHit(hit, tDir, sunTint, L), in_pos, tDir, hit.t, sunTint, L, ambientSkyU);
            }
#endif
            if (!aboveHit)
            {
                above = reflectedSkyRadiance(tDir);
                const float sunDot = max(dot(tDir, L), 0.0);
                above += sunTint * (pow(sunDot, 600.0) * 30.0 + pow(sunDot, 24.0) * 0.6);
                above = applyRayFogSky(above, in_pos, tDir, sunTint, L, ambientSkyU);
            }
            color = mix(color, above, trans);
        }
        // Foam from below: a backlit diffuse sheet - about half of the top side's whitewater light comes through.
        if (foamW > 0.003)
        {
            const vec3 foamBelow = u_ocean_foamColor * (0.5 * (sunTint * (max(L.y, 0.0) * sunVisU) / PI + ambientSkyU + u_ambientColor));
            color = mix(color, foamBelow, foamW);
        }
        color *= pathAbsorb;
        out_color = vec4(color, 0.0); // alpha 0 = TAA's ocean flag (see the end of main)
        out_factor = vec4(0.0);
        return;
    }

    // The edge fade (oceanEdgeCover); fully faded: nothing to shade.
    const float cover = oceanEdgeCover();
    if (cover <= 0.0)
    {
        out_color = vec4(0.0);
        out_factor = vec4(1.0);
        return;
    }

    if (dot(N, V) < 0.0) // grazing: keep the shading hemisphere consistent
        N = -N;

    // Microfacet roughness = base + spec AA + LEAN slope variance (both x "Glint filtering") + the sub-grid capillary band
    // + the churn's micro-roughness (the foam amount). The variance terms stretch the sun glitter toward the horizon.
    // "Micro roughness": the slope variance BELOW the finest cascade's Nyquist. LEAN returns only what the mips removed
    // (zero at mip 0), so without it the near field falls onto the 0.02 alpha clamp: plastic water up close. Not x "Glint
    // filtering" (this band was never in the spectrum). Enters as alpha^2 = 2 sigma^2, like the LEAN term.
    const float perceptualRough = u_ocean_roughness;
    const float slopeVariance = 0.5 * (slopeVar.x + slopeVar.y) * (ns * ns);
    const float microVariance = u_ocean_microRoughness * (ns * ns);
    // alphaGateSq: every term except the micro band (the "Reflection max rough" gate ignores it).
    const float alphaGateSq = perceptualRough * perceptualRough * perceptualRough * perceptualRough
        + (normalVariance(N) + 2.0 * slopeVariance) * u_ocean_glintFilter + foamAmount * 0.35;
    const float alphaSq = alphaGateSq + 2.0 * microVariance;

    // From here the top side shades in HALF math: vectors, dots, roughness, ray weights and colours (RGBA16F target, sun
    // intensity single digits). Positions, ray origins and traced directions stay 32-bit (widened from the half N / V);
    // each traced result converts to half ONCE. The scene lights accumulate in 32-bit (a light's radiance has no bound).
    const float16_t alphaF = float16_t(clamp(sqrt(alphaSq), 0.02, 1.0));
    // "Reflection max rough"'s gate value (see the mirror below), resolved here so alphaGateSq dies before the body trace.
    const float alphaGate = sqrt(alphaGateSq);
    const f16vec3 Nh = f16vec3(N);
    const f16vec3 Vh = f16vec3(V);
    const f16vec3 Lh = f16vec3(L);
    // The half vector in FLOAT, its length floored: looking straight at the sun (V = -L, e.g. a set sun behind the waves)
    // L + V is ~0, its square underflowed in half and normalize gave NaN - and NaN x a zero sun term is still NaN: a
    // flickering bright point on the water at the sun's position.
    const vec3 lv = L + V;
    const f16vec3 Hh = f16vec3(lv * inversesqrt(max(dot(lv, lv), 1e-12)));
    const float16_t NoV = clamp(dot(Nh, Vh), float16_t(1e-3), float16_t(1.0));
    const float16_t NoL = max(dot(Nh, Lh), float16_t(0.0));
    const float16_t NoH = max(dot(Nh, Hh), float16_t(0.0));
    const float16_t LoH = dot(Lh, Hh); // = |L + V| / 2 >= 0
    const float16_t foamH = float16_t(foamW);
    const f16vec3 sunTintH = f16vec3(sunTint);


    // Sun visibility: one RT shadow ray (or PCSS fallback). The crest SSS (always on, >= 0.01 on the CPU) needs it too.
    const bool sunUp = L.y > 0.0;
    const float16_t sunVis = float16_t(!sunUp ? 0.0
        : (u_rt_sunShadow > 0.5 ? rtShadowVisibility(in_pos + vec3(Nh) * 0.1, L, 0.05, 10000.0)
                               : sampleSunShadowHard(in_pos, vec3(Nh)))); // one tap: the moving water hides a penumbra
    const f16vec3 ambientSky = f16vec3(skyAmbientUp(up));
    // The GI's sky visibility on the SKY reflections (the blurred share in C, the mirror's sky fallback): water under a
    // roof mirrors no sky. A traced mirror hit is geometry and keeps its full weight.
    const float16_t skyVis = float16_t(giSkyVisibility(in_pos, up));
    const f16vec3 whitewater = f16vec3(u_ocean_foamColor) * (sunTintH * (foamNoL * sunVis * float16_t(INV_PI)) + ambientSky + f16vec3(u_ambientColor));

    const f16vec3 inscatter = f16vec3(u_ocean_scatterColor * u_ocean_scatterStrength) * (ambientSky + sunTintH * float16_t(max(L.y, 0.0) * INV_PI));

    const float16_t F = F_SchlickH(NoV, float16_t(0.02));

    // Each scene ray's weight in the final pixel, resolved before it is traced: foam covers both, the bubble cloud
    // replaces the body, Fresnel splits the rest, the roughness blur hands part of the mirror to the average sky. Under 2%
    // the ray is skipped.
    const float16_t milk = float16_t(milkF); // entrained bubbles (the foam amount)
    const float16_t reflBlur = clamp(alphaF * float16_t(2.0) - float16_t(0.05), float16_t(0.0), float16_t(0.6));
    const float16_t clearW = float16_t(1.0) - foamH;
    const float16_t bodyWeight = (float16_t(1.0) - F) * (float16_t(1.0) - milk) * clearW;
    const float16_t mirrorWeight = F * (float16_t(1.0) - reflBlur) * clearW;

    // The pixel is LINEAR in the two traced radiances: body * bodyWeight + mirror * mirrorWeight + C.
    //   color = mix(mix(body, bubbles, milk) + sss, mix(mirror, ambientSky, reflBlur), F) + glint
    //   final = mix(color, whitewater, foam)
    // Everything not traced - the glint, the crest SSS, the bubble cloud, the blur's sky share and the foam - folds into C
    // BEFORE the traces, so only C, the weights and the light inputs stay live across them (the ray queries are the
    // register peak).
    f16vec3 C = f16vec3(0.0);
    if (milk > float16_t(0.003))
        C = oceanBubbleRadianceFrame(NoV, sunTintH * sunVis, ambientSky + f16vec3(u_ambientColor), inscatter) * milk;

    // Crest SSS: sun through back-lit crests glows the scatter color, x height above the calm line. In the transmitted
    // body, so Fresnel fades it at grazing like all subsurface light.
    {
        const float16_t waveH = float16_t(max(in_pos.y - shoreHW.y, 0.0));
        const float16_t towardSun = pow(clamp(dot(Vh, -Lh), float16_t(0.0), float16_t(1.0)), float16_t(u_ocean_sssPower));
        const float16_t backSlope = float16_t(0.5) - float16_t(0.5) * dot(Lh, Nh);
        C += f16vec3(u_ocean_scatterColor) * sunTintH * (float16_t(u_ocean_sssStrength) * waveH * towardSun * backSlope * backSlope * backSlope * sunVis);
    }

    // Sun glint: Cook-Torrance specular, shadow-gated. D * (Vis * NoL) stays <= ~2e4 at the 0.02 alpha floor; the tinted
    // glint is clamped to the half range.
    const float16_t D  = D_GGX_H(alphaF, NoH, cross(Nh, Hh));
    const float16_t Vv = V_SmithGGX_H(NoV, NoL, alphaF);
    const float16_t glint = D * (Vv * NoL) * (sunVis * F_SchlickH(LoH, float16_t(0.02)));

    // The blur's sky share: the sky SH's cosine lobe (E(n) / pi) toward the reflection's side - a rough reflection sees the
    // lit clouds toward the sun, not the average. Centred between R and up: around R a grazing lobe is half below the
    // horizon (the SH's black ground), and the rough ocean goes darker than the film.
    f16vec3 blurSky = ambientSky;
    if (UBO_LIVE_rt_giStrength > 0.0)
    {
        vec3 Rb = reflect(-vec3(Vh), vec3(Nh));
        Rb.y = max(Rb.y, 0.0);
        blurSky = max(giEvalSkySHH(f16vec3(normalize(normalize(Rb) + up))) * float16_t(INV_PI), f16vec3(0.0));
    }
    C = clearW * ((float16_t(1.0) - F) * C + (F * reflBlur * skyVis) * blurSky + min(sunTintH * glint, f16vec3(MEDIUMP_FLT_MAX)))
      + foamH * whitewater;

    // Refracted body: the traced water column (Beer-Lambert both ways).
    f16vec3 body = inscatter;
    if (bodyWeight > float16_t(0.02))
    {
        // Air into water never totally reflects (N faces V): refract always returns a direction.
        const vec3 refrDir = refract(-vec3(Vh), vec3(Nh), 1.0 / 1.33);
        body = f16vec3(traceWaterBody(in_pos + vec3(Nh) * 0.05, refrDir, in_pos, shoreHW, sunTint, float(sunVis), L, vec3(inscatter), rtInRange));
    }
    f16vec3 color = C + body * bodyWeight;

    // Reflection: ray-traced mirror, sky fallback, roughness-blurred toward the average sky.
    vec3 R = reflect(-vec3(Vh), vec3(Nh));
    R.y = max(R.y, 0.02); // keep grazing reflections just above the horizon
    R = normalize(R);
    vec3 reflColor;
    bool mirrorHit = false;
    // "Reflection max rough": a wide lobe can't be one mirror sample. The gate reads the roughness WITHOUT "Micro
    // roughness" (a constant floor on every pixel would switch the mirror off everywhere).
#ifdef OCEAN_RT_REFLECTIONS // "Ocean/RT/Reflections"
    if (mirrorWeight > float16_t(0.02) && alphaGate < u_ocean_rtReflectionMaxRough && rtInRange)
    {
        SceneHit hit;
        mirrorHit = traceScene(in_pos + vec3(Nh) * 0.05, R, u_ocean_rtReflectionRange, -1e30, hit); // "Reflection range"
        if (mirrorHit)
            reflColor = applyReflectionFog(shadeHit(hit, R, sunTint, L), in_pos, R, hit.t, sunTint, L, vec3(ambientSky));
    }
#endif
    // Sky fallback, fogged too (the baked mirror sky carries none). Only on a miss: a hit replaces it whole.
    if (!mirrorHit)
        reflColor = applyReflectionFogSky(reflectedSkyRadiance(R), in_pos, R, sunTint, L, vec3(ambientSky)) * float(skyVis);
#if OCEAN_DEBUG_MODE == 6
    {
        vec3 dbg = vec3(0.0);
#ifdef OCEAN_RT_REFLECTIONS
        SceneHit dbgHit;
        if (mirrorWeight <= float16_t(0.02))
            dbg = vec3(1.0, 0.0, 0.0);
        else if (alphaGate >= u_ocean_rtReflectionMaxRough)
            dbg = vec3(1.0, 1.0, 0.0);
        else if (!rtInRange)
            dbg = vec3(1.0, 0.0, 1.0);
        else if (traceScene(in_pos + vec3(Nh) * 0.05, R, u_ocean_rtReflectionRange, -1e30, dbgHit))
            dbg = vec3(0.0, 1.0, 0.0) + vec3(0.8, 0.0, 0.8) * exp(-dbgHit.t * 0.05);
        else if (rtShadowVisibility(in_pos + vec3(Nh) * 0.05, R, 0.05, 100000.0) < 0.5)
            dbg = vec3(1.0);           // WHITE: a hit exists, but past "Reflection range"
        else if (rtShadowVisibility(u_viewPos, vec3(0.0, -1.0, 0.0), 0.05, 100000.0) < 0.5)
            dbg = vec3(0.0, 0.0, 1.0); // BLUE: this ray missed, but the TLAS holds geometry (straight below the camera)
        else
            dbg = vec3(0.3);           // GREY: no ray hits anything - the TLAS is empty or not the bound one
#endif
        out_color = vec4(dbg, 1.0);
        out_factor = vec4(0.0);
        return;
    }
#endif
    // The mirror's share (its blurred sky share is in C).
    color += f16vec3(reflColor) * mirrorWeight;

    // Scene lights (shared light-grid walk, RT-shadowed): dielectric specular + in-scatter "diffuse"; foam responds as
    // lambertian whitewater instead.
    vec3 outColor = vec3(color);
    {
        const f16vec3 matColOverPi = mix(f16vec3(u_ocean_scatterColor * u_ocean_scatterStrength), f16vec3(u_ocean_foamColor), foamH) * float16_t(INV_PI);
        const float16_t lightRough = mix(alphaF, float16_t(0.85), foamH);
        const f16vec3 waterSpec = f16vec3(0.02);

        const ivec3 gridPos = getGridPos(in_pos);
        uint tableIdx = getTableIdx(gridPos);
        while (true)
        {
            const uint gridIdx = getGridIdx(tableIdx);
            if (gridIdx == EMPTY_ENTRY)
                break;
            const ivec3 gridMin = getGridMin(gridIdx);
            if (gridMin == gridPos)
            {
                // ONE loop over the large lights, then the cell's (as the lit core): each loop inlines doLightShadowed
                // whole, light types plus shadow ray queries.
                const uint numLargeLights = getLargeLightCount(gridIdx);
                const uint cellOffset     = calcCellOffset(gridIdx, gridMin, in_pos);
                const uint numLights      = numLargeLights + getNumLightsForCell(cellOffset);
                for (uint i = 0; i < numLights; ++i)
                {
                    const uint lightId    = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
                    const LightInfo light = in_lightInfos[lightId];
                    outColor += doLightShadowed(light, in_pos, Vh, Nh, waterSpec, matColOverPi, 0.0, lightRough);
                }
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }
    }

    // Scene colour ALPHA = TAA's animated-surface flag: 0 marks ocean (the waves move without motion vectors, so
    // taa.cs.glsl caps the history weight). Other opaque surfaces write their material alpha (> 0); the stages layered over
    // the scene write RGB only. The blend composites the alpha too (out.a = 0 + dst.a * out_factor.a): the flag where the
    // ocean is the larger part of the pixel, the ground's own alpha where the fade shows mostly the ground.
    out_color = vec4(outColor * cover, 0.0);
    out_factor = vec4(vec3(1.0 - cover), cover > 0.5 ? 0.0 : 1.0);
}
