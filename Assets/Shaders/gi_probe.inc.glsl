// GI irradiance probes — a single persistent, world-space CASCADED CLIPMAP volume. GI_NUM_CASCADES nested
// probe grids, each GI_PROBE_DIM_X x _Y x _Z probes at a fixed power-of-two spacing (BASE_SPACING << cascade),
// centred on the scene focus lifted by GI_FOCUS_Y_OFFSET. Probes sit at ABSOLUTE lattice positions
// (lc * spacing) and are stored toroidally
// (slot = lc & (DIM-1)), so a probe that stays in range maps to the same storage slot every frame and its
// SH carries forward in place — no hash table, no copy, no prev/cur ping-pong. When the camera moves, the
// lattice coords that scroll out are silently overwritten by the new coords that wrap into their slots.
//
// Buffers / names the includer must define before including (read side):
//   GI_GRID_DATA_NAME   vec4[] per-probe data: cascade-major, slot-linear, GI_PROBE_STRIDE_V4 vec4s each
//                       (16-byte loads: a probe read is 6 wide loads, not 24 scalar ones).
// For the write side (trace) also define GI_PROBE_WRITE.
//
// Requires shared.inc.glsl (PI) (u_sceneFocus — the scene focus, which centers the cascades: the game's
// player, else the camera; see Renderer::setSceneFocus).

#ifndef GI_PROBE_INC_GLSL
#define GI_PROBE_INC_GLSL

// GI_SH_STRIDE, GI_NUM_CASCADES, GI_PROBE_DIM_X/Y/Z, GI_FOCUS_Y_OFFSET and GI_CASCADE_BASE_SPACING are
// injected by the engine from RendererVKLayout (Layout.ixx, the live g_giGrid — the "GI" grid tweaks
// reload every shader).
// Per-probe layout, in VEC4s (24 floats = 6 vec4; the CPU sizes the buffer in floats, GI_PROBE_STRIDE):
//   [0] = (c0.rgb, c1.r)  [1] = (c1.gb, c2.rg)  [2] = (c2.b, c3.rgb)   SH-L1 RGB irradiance
//   [3] = SH-L1 mean depth   [4] = SH-L1 mean depth^2
//   [5] = (backface-hit fraction, relocation offset xyz)
#define GI_PROBE_STRIDE_V4 ((uint(GI_SH_STRIDE) + 12u) / 4u)
#define GI_DEPTH_V4   3u
#define GI_DEPTH2_V4  4u
#define GI_MISC_V4    5u

#ifndef GI_NORMAL_BIAS
#define GI_NORMAL_BIAS 1.5 // push the sample point along the normal (world units) to limit self-leak
#endif
#ifndef GI_NORMAL_BIAS_SPACING
#define GI_NORMAL_BIAS_SPACING 0.25 // extra normal bias as a fraction of the cascade's probe spacing
#endif
// Probes whose gather rays mostly hit backfaces sit inside (or right behind) geometry; their near-black SH
// would darken every surface in their trilinear footprint. Fade them out of the lookup over this fraction
// range (dead probes renormalize away; the all-dead case falls through to a coarser cascade).
#ifndef GI_BACKFACE_DEAD_MIN
#define GI_BACKFACE_DEAD_MIN 0.15
#endif
#ifndef GI_BACKFACE_DEAD_MAX
#define GI_BACKFACE_DEAD_MAX 0.35
#endif
// Depth values are clamped to this many probe spacings before SH projection AND at lookup: visibility only
// matters within a trilinear cell (+ bias/offset headroom), and capping keeps miss rays (rayMax) from
// swamping the variance. The lookup queries at cap*0.95 max so fully-open probes always pass the test.
#ifndef GI_DEPTH_CAP_SPACING
#define GI_DEPTH_CAP_SPACING 3.0
#endif
// -----------------------------------------------------------------------------------------------------

#define GI_PROBE_DIMS      ivec3(GI_PROBE_DIM_X, GI_PROBE_DIM_Y, GI_PROBE_DIM_Z)
#define GI_CASCADE_PROBES  (GI_PROBE_DIM_X * GI_PROBE_DIM_Y * GI_PROBE_DIM_Z)
#define GI_DIM_MASK        (GI_PROBE_DIMS - 1)
#define GI_PROBE_DIM_MIN   min(min(GI_PROBE_DIM_X, GI_PROBE_DIM_Y), GI_PROBE_DIM_Z) // the fade bands scale with the narrowest axis

vec4 shBasisL1(vec3 d)
{
    return vec4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x);
}

// cheb^GI_VIS_CHEB_POWER with the exponent baked (integer define from RendererVKLayout::g_giGrid): the
// fixed-count loop unrolls to POWER-1 multiplies.
#ifndef GI_VIS_CHEB_POWER
#define GI_VIS_CHEB_POWER 2
#endif
float giChebPow(float x)
{
    float r = x;
    for (int i = 1; i < GI_VIS_CHEB_POWER; ++i)
        r *= x;
    return r;
}

// Probe spacing (world units) of a cascade. Cascade 0 is finest; each level doubles.
int giCascadeSpacing(int c) { return GI_CASCADE_BASE_SPACING << c; }

// Integer lattice coord of the cascade's min corner, snapped so the focus (lifted by GI_FOCUS_Y_OFFSET —
// a positive offset puts more probes above the ground than below) sits at its center.
ivec3 giCascadeOrigin(int c, vec3 focusPos)
{
    int s = giCascadeSpacing(c);
    vec3 center = focusPos + vec3(0.0, GI_FOCUS_Y_OFFSET, 0.0);
    return ivec3(floor(center / float(s))) - GI_PROBE_DIMS / 2;
}

// Toroidal slot (linear) for an absolute lattice coord. lc & mask is a true mod for power-of-two DIMs,
// correct for negative coords under two's complement.
uint giSlotLinear(ivec3 lc)
{
    ivec3 s = lc & GI_DIM_MASK;
    return uint(s.x + s.y * GI_PROBE_DIM_X + s.z * GI_PROBE_DIM_X * GI_PROBE_DIM_Y);
}

// vec4 offset into GI_GRID_DATA_NAME for the probe at lattice coord lc in cascade c.
uint giProbeBase(int c, ivec3 lc)
{
    return (uint(c) * uint(GI_CASCADE_PROBES) + giSlotLinear(lc)) * GI_PROBE_STRIDE_V4;
}

// SH-L1 projections of the probe's hit distance and squared hit distance (misses counted as the depth
// cap). Directional visibility: reconstructing at the probe->surface direction gives the mean and second
// moment of the distance to geometry that way, for a Chebyshev occlusion test at lookup time.
void giReadDepthSH(uint cellBase, out vec4 dsh, out vec4 d2sh)
{
    dsh  = GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4];
    d2sh = GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4];
}

// Band-limited reconstruction of a scalar SH-L1 function at a direction (no cosine convolution — this is
// the raw function estimate, unlike irradiance).
float giEvalDepth(vec4 c, vec3 d) { return dot(c, shBasisL1(d)); }

// Fraction of the probe's gather rays that hit backfacing geometry (~1 = embedded in a wall/terrain).
float giProbeBackfaceFrac(uint cellBase) { return GI_GRID_DATA_NAME[cellBase + GI_MISC_V4].x; }

// Relocation offset: probes embedded in / grazing geometry trace from (and are treated as sitting at)
// lattice position + offset. Trilinear weights stay on the unmoved lattice.
vec3 giProbeOffset(uint cellBase) { return GI_GRID_DATA_NAME[cellBase + GI_MISC_V4].yzw; }

// Raw SH-L1 RGB coefficients of one probe (three wide loads, unpacked per the layout above).
void giReadSH(uint cellBase, out vec3 c0, out vec3 c1, out vec3 c2, out vec3 c3)
{
    vec4 p0 = GI_GRID_DATA_NAME[cellBase];
    vec4 p1 = GI_GRID_DATA_NAME[cellBase + 1u];
    vec4 p2 = GI_GRID_DATA_NAME[cellBase + 2u];
    c0 = p0.xyz;
    c1 = vec3(p0.w, p1.xy);
    c2 = vec3(p1.zw, p2.x);
    c3 = p2.yzw;
}

// Cosine-convolved irradiance E(n) from SH-L1 coefficients. Diffuse exit radiance is albedo/PI * E(n).
vec3 giEvalSH(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 n)
{
    vec4 Y = shBasisL1(n);
    const float A0 = PI;
    const float A1 = 2.0 * PI / 3.0;
    vec3 E = A0 * c0 * Y.x + A1 * (c1 * Y.y + c2 * Y.z + c3 * Y.w);
    return max(E, vec3(0.0));
}

vec3 giEvalCell(uint cellBase, vec3 n)
{
    vec3 c0, c1, c2, c3;
    giReadSH(cellBase, c0, c1, c2, c3);
    return giEvalSH(c0, c1, c2, c3, n);
}

// Virtual sky probe: the trace pass projects skyRadiance (sky + analytic sunlit ground) into one extra
// SH-L1 slot stored after the last probe. Out-of-field surfaces evaluate it exactly like a real probe
// with no geometry hits, so leaving the probe field hands over to the same integral the probes converge
// to in open space — instead of a differently-shaped cheap approximation that diverges at low sun angles
// (a single sky sample along the normal misses the bright horizon in-scatter band the probes gather).
// Returns cosine-convolved irradiance E(n), like evalProbeSHCoverage.
#define GI_SKY_SH_BASE (uint(GI_NUM_CASCADES) * uint(GI_CASCADE_PROBES) * GI_PROBE_STRIDE_V4) // vec4 index; 3 vec4s of SH
vec3 giEvalSkySH(vec3 n) { return giEvalCell(GI_SKY_SH_BASE, n); }

// True when cascade c's 8-probe stencil around p fully fits inside its toroidal window (so the slots are
// the ones actually resident for this camera position, and we never interpolate across the wrap seam).
bool giCascadeFits(int c, vec3 p, out ivec3 base, out ivec3 origin, out int s, out vec3 frac)
{
    s      = giCascadeSpacing(c);
    origin = giCascadeOrigin(c, u_sceneFocus.xyz);
    vec3 pf = p / float(s);
    base   = ivec3(floor(pf));
    frac   = pf - vec3(base);
    return !(any(lessThan(base, origin)) || any(greaterThanEqual(base + 1, origin + GI_PROBE_DIMS)));
}

// Trilinear irradiance from one cascade's 8 nearest probes, with DDGI-style backface weighting (probes
// behind the surface are faded out to limit light leaking through thin geometry). totalW returns the
// summed weight so the caller can detect the all-backfaced case and fall through to a coarser cascade.
// samplePos is the normal-BIASED query point (giBiasedSample), not the raw surface position.
vec3 giSampleCascade(int c, int s, ivec3 base, vec3 frac, vec3 samplePos, vec3 n, out float totalW)
{
    // Blend the raw SH-L1 coefficients (not per-probe irradiance) and clamp once at the end. The cosine
    // convolution's max(0) is a per-probe nonlinearity; applying it after interpolation avoids the kinks
    // between probes that show up as ripples on flat surfaces.
    vec3 a0 = vec3(0.0), a1 = vec3(0.0), a2 = vec3(0.0), a3 = vec3(0.0);
    totalW = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        vec3 w3 = mix(1.0 - frac, frac, vec3(off));
        float w = w3.x * w3.y * w3.z;
        if (w <= 0.0)
            continue;
        ivec3 lc = base + off;
        uint cellBase = giProbeBase(c, lc);

        // Reject probes embedded in geometry (mostly-backface gather) — their near-black SH is not signal.
        w *= 1.0 - smoothstep(GI_BACKFACE_DEAD_MIN, GI_BACKFACE_DEAD_MAX, giProbeBackfaceFrac(cellBase));
        if (w <= 0.0)
            continue;

        // Directional terms use the probe's relocated position (where it actually traced from); the
        // trilinear weights above stay on the unmoved lattice.
        vec3 probeWorld = vec3(lc) * float(s) + giProbeOffset(cellBase);

        vec3 toProbe = probeWorld - samplePos;
        float len = length(toProbe);
        if (len > 1e-4)
        {
            vec3 dirToProbe = toProbe / len;

            // Smooth backface fade (linear in the half-angle term, not squared): still suppresses probes
            // behind the surface to limit leaking, but with gentler position-dependent modulation so flat
            // surfaces don't ripple as the per-probe direction sweeps across them.
            w *= dot(n, dirToProbe) * 0.5 + 0.5;

            // Directional visibility (DDGI-style Chebyshev on SH-L1 depth): reconstruct the probe's mean
            // and second-moment distance toward the surface; when the surface lies beyond the mean, the
            // variance bounds how likely it is still visible. mean2 == 0 means no depth data yet (freshly
            // cleared buffer) -> don't occlude. The variance floor softens the blurry L1 reconstruction.
            vec4 dsh, d2sh;
            giReadDepthSH(cellBase, dsh, d2sh);
            float cap   = GI_DEPTH_CAP_SPACING * float(s);
            // Mean scale (u_giVisParams.w, > 1) widens each probe's visible footprint: the blurry L1
            // reconstruction underestimates distance at grazing angles, shrinking the un-occluded region
            // around a probe; scaling the mean pushes the occlusion boundary back out (more overlap).
            float mean  = clamp(giEvalDepth(dsh, -dirToProbe) * u_giVisParams.w, 0.0, cap);
            float mean2 = giEvalDepth(d2sh, -dirToProbe);
            float d = min(len, cap * 0.95);
            if (mean2 > 1e-3 && d > mean)
            {
                // u_giVisParams: x = variance floor (fraction of spacing), z = weight floor. The power
                // is the GI_VIS_CHEB_POWER define ("GI/Vis Cheb Power", integer): a chain of multiplies
                // instead of a pow per probe (16 probes a pixel).
                float minDev   = u_giVisParams.x * float(s);
                float variance = max(mean2 - mean * mean, minDev * minDev);
                float delta    = d - mean;
                float cheb     = variance / (variance + delta * delta);
                w *= max(giChebPow(cheb), u_giVisParams.z);
            }
        }
        if (w <= 0.0)
            continue;

        vec3 c0, c1, c2, c3;
        giReadSH(cellBase, c0, c1, c2, c3);
        a0 += w * c0; a1 += w * c1; a2 += w * c2; a3 += w * c3;
        totalW += w;
    }
    if (totalW <= 1e-4)
        return vec3(0.0);
    float inv = 1.0 / totalW;
    return giEvalSH(a0 * inv, a1 * inv, a2 * inv, a3 * inv, n);
}

// Trilinear probe irradiance with cross-cascade blending. Walks coarse-to-fine and uses the finest cascade
// that both fits and has front-facing probes; near that cascade's outer window faces it cross-fades into
// the next coarser cascade so the LOD/spacing change is gradual instead of a hard, wavy seam. Returns
// vec3(-1) when no cascade covers the point (caller falls back to ambient).
// Normal-biased sample point for a cascade. The bias grows with the cascade's probe spacing so distant,
// coarse lookups sit cleanly between probes (and off the surface) instead of grazing it and picking up
// contaminated / in-geometry probes.
vec3 giBiasedSample(vec3 worldPos, vec3 n, int s)
{
    return worldPos + n * (GI_NORMAL_BIAS + GI_NORMAL_BIAS_SPACING * float(s));
}

// coverage = 1 deep inside the probe field, falling to 0 over the OUTERMOST cascade's edge band (and 0
// where no cascade covers the point). Callers blend their ambient fallback in by it — without the fade,
// leaving the probe field dropped the bounce light in a single step, which read as a bright square zone
// around the camera at the last cascade's window face.
vec3 evalProbeSHCoverage(vec3 worldPos, vec3 n, out float coverage)
{
    coverage = 1.0;
    for (int c = 0; c < GI_NUM_CASCADES; ++c)
    {
        vec3  p = giBiasedSample(worldPos, n, giCascadeSpacing(c));
        ivec3 base, origin; int s; vec3 frac;
        if (!giCascadeFits(c, p, base, origin, s, frac))
            continue;

        float w0;
        // Weight/visibility terms measure from the biased point (DDGI surface bias): querying from the
        // raw surface point puts the Chebyshev direction exactly in the wall plane, where the blurry L1
        // depth reconstruction underestimates distance and false-occludes everything lateral to a probe
        // (bright probe-footprint circles on walls).
        vec3 E0 = giSampleCascade(c, s, base, frac, p, n, w0);
        if (w0 <= 1e-4)
            continue; // every probe backfaced -> try a coarser (differently-aligned) cascade

        // Fade toward the next cascade over the outer `band` cells of this window (1 = interior, 0 = face).
        vec3  cellInWin = vec3(base - origin);
        vec3  distCells = min(cellInWin, vec3(GI_PROBE_DIMS - 2) - cellInWin);
        float edge = min(min(distCells.x, distCells.y), distCells.z);
        if (c == GI_NUM_CASCADES - 1)
        {
            // Outermost cascade: there is nothing coarser to fade into, so fade the COVERAGE instead
            // (wider band than the inter-cascade one — this hands over to a fallback, not to more data).
            coverage = clamp(edge / (float(GI_PROBE_DIM_MIN) * 0.2), 0.0, 1.0);
            return E0;
        }
        float band = float(GI_PROBE_DIM_MIN) * 0.1;
        float fade = clamp(edge / band, 0.0, 1.0);
        if (fade >= 1.0)
            return E0;

        vec3  p2 = giBiasedSample(worldPos, n, giCascadeSpacing(c + 1));
        ivec3 base2, origin2; int s2; vec3 frac2;
        if (!giCascadeFits(c + 1, p2, base2, origin2, s2, frac2))
            return E0;
        float w1;
        vec3 E1 = giSampleCascade(c + 1, s2, base2, frac2, p2, n, w1);
        if (w1 <= 1e-4)
            return E0;
        return mix(E1, E0, fade);
    }
    coverage = 0.0;
    return vec3(-1.0);
}

vec3 evalProbeSH(vec3 worldPos, vec3 n)
{
    float coverage;
    return evalProbeSHCoverage(worldPos, n, coverage);
}

// Debug visualization: hue = cascade (red=0 finest .. yellow=coarsest), brightness checkerboarded per probe.
vec3 giDebugColor(vec3 worldPos, vec3 n)
{
    vec3 p = worldPos + n * GI_NORMAL_BIAS;
    for (int c = 0; c < GI_NUM_CASCADES; ++c)
    {
        int   s      = giCascadeSpacing(c);
        ivec3 origin = giCascadeOrigin(c, u_sceneFocus.xyz);
        ivec3 base   = ivec3(floor(p / float(s)));
        if (any(lessThan(base, origin)) || any(greaterThanEqual(base + 1, origin + GI_PROBE_DIMS)))
            continue;

        vec3 lod;
        if      (c == 0) lod = vec3(1.0, 0.2, 0.2);
        else if (c == 1) lod = vec3(0.2, 1.0, 0.2);
        else if (c == 2) lod = vec3(0.3, 0.5, 1.0);
        else             lod = vec3(1.0, 1.0, 0.2);
        float checker = (((base.x + base.y + base.z) & 1) == 0) ? 1.0 : 0.55;
        return lod * checker;
    }
    return vec3(0.0);
}

#ifdef GI_PROBE_WRITE

void giStoreCell(uint cellBase, vec3 c0, vec3 c1, vec3 c2, vec3 c3)
{
    GI_GRID_DATA_NAME[cellBase]      = vec4(c0, c1.r);
    GI_GRID_DATA_NAME[cellBase + 1u] = vec4(c1.gb, c2.rg);
    GI_GRID_DATA_NAME[cellBase + 2u] = vec4(c2.b, c3);
}

// Temporally blend this frame's freshly-projected coefficients into a probe (lerp toward the new value).
// alpha == 1 fully replaces the stored value (used for probes that just scrolled into the clipmap).
void giBlendCell(uint cellBase, vec3 c0, vec3 c1, vec3 c2, vec3 c3, float alpha)
{
    GI_GRID_DATA_NAME[cellBase]      = mix(GI_GRID_DATA_NAME[cellBase],      vec4(c0, c1.r),     alpha);
    GI_GRID_DATA_NAME[cellBase + 1u] = mix(GI_GRID_DATA_NAME[cellBase + 1u], vec4(c1.gb, c2.rg), alpha);
    GI_GRID_DATA_NAME[cellBase + 2u] = mix(GI_GRID_DATA_NAME[cellBase + 2u], vec4(c2.b, c3),     alpha);
}

// Temporally blend the probe's SH-L1 depth moments (the Chebyshev visibility estimate) and its
// backface-hit fraction (the embedded-probe rejection signal; shares the misc vec4 with the offset).
void giBlendProbeStats(uint cellBase, vec4 dsh, vec4 d2sh, float backfaceFrac, float alpha)
{
    GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4]  = mix(GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4],  dsh,  alpha);
    GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4] = mix(GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4], d2sh, alpha);
    vec4 misc = GI_GRID_DATA_NAME[cellBase + GI_MISC_V4];
    misc.x = mix(misc.x, backfaceFrac, alpha);
    GI_GRID_DATA_NAME[cellBase + GI_MISC_V4] = misc;
}

// Store the relocation offset (written unblended — the relocation logic is already iterative).
void giStoreProbeOffset(uint cellBase, vec3 offset)
{
    vec4 misc = GI_GRID_DATA_NAME[cellBase + GI_MISC_V4];
    misc.yzw = offset;
    GI_GRID_DATA_NAME[cellBase + GI_MISC_V4] = misc;
}

#endif // GI_PROBE_WRITE

#endif // GI_PROBE_INC_GLSL
