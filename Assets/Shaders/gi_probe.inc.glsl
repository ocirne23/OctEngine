// GI irradiance probes - a single persistent, world-space CASCADED CLIPMAP volume. GI_NUM_CASCADES nested
// probe grids, each GI_PROBE_DIM_X x _Y x _Z probes at a fixed power-of-two spacing (BASE_SPACING << cascade),
// centred on the scene focus lifted by GI_FOCUS_Y_OFFSET. Probes sit at ABSOLUTE lattice positions
// (lc * spacing) and are stored toroidally
// (slot = lc & (DIM-1)), so a probe that stays in range maps to the same storage slot every frame and its
// SH carries forward in place - no hash table, no copy, no prev/cur ping-pong. When the camera moves, the
// lattice coords that scroll out are silently overwritten by the new coords that wrap into their slots.
//
// Buffers / names the includer must define before including (read side):
//   GI_GRID_DATA_NAME   vec4[] per-probe data: cascade-major, slot-linear, GI_PROBE_STRIDE_V4 vec4s each
//                       (16-byte loads: a probe read is 6 wide loads, not 24 scalar ones).
// For the write side (trace) also define GI_PROBE_WRITE.
//
// Requires shared.inc.glsl (PI) (u_sceneFocus - the scene focus, which centers the cascades: the game's
// player, else the camera; see Renderer::setSceneFocus).

#ifndef GI_PROBE_INC_GLSL
#define GI_PROBE_INC_GLSL

// GI_SH_STRIDE, GI_NUM_CASCADES, GI_PROBE_DIM_X/Y/Z, GI_FOCUS_Y_OFFSET and GI_CASCADE_BASE_SPACING are
// injected by the engine from RendererVKLayout (Layout.ixx, the live g_giGrid - the "GI" grid tweaks
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

// The irradiance volume stores L1 as (L1 / L0) / GI_SQRT3: for a non-negative radiance every such ratio lies
// in [-sqrt(3), sqrt(3)] (a single direction gives 0.488603 / 0.282095), so the scaled ratio fills SNORM.
#define GI_SQRT3 1.7320508

vec4 shBasisL1(vec3 d)
{
    return vec4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x);
}

// cheb^GI_VIS_CHEB_POWER with the exponent baked: the fixed-count loop unrolls to POWER-1 multiplies.
// Fixed at 2, no tweak: near the occlusion threshold the weight is ~ 1 - p (delta / sigma)^2, so the
// power only rescales the variance floor there, and the weight floor cuts the tail it shapes.
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

// Integer lattice coord of the cascade's min corner, snapped so the focus (lifted by GI_FOCUS_Y_OFFSET -
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

// Min-corner lattice coord of the trace WAVE (4x4x4 probe block) that holds the probe at lattice coord lc,
// for the window at `origin`. Blocks are WORLD-aligned (lc >> 2), NOT window-relative: the window origin
// scrolls a cell at a time, and window-relative blocks re-partitioned every probe on every scroll - a probe
// being walked away from could land in a block whose centre was NEARER, and its rate went UP with distance;
// the nearest block centre also swung 0.5 .. 1.5 spacings within one cell of movement. Every dim is a
// multiple of 4, so a world block is also a 4x4x4 block of toroidal SLOT space (what the trace enumerates).
// The one block per axis that straddles the window's wrap seam has its two halves on opposite faces - both
// about half a window from the focus; the result is folded into the window so all 64 lanes agree.
ivec3 giWaveMin(ivec3 lc, ivec3 origin)
{
    return origin + (((lc & ~3) - origin) & GI_DIM_MASK);
}

// Update priority of one trace WAVE (min corner waveMin, from giWaveMin): a factor of the wave's update interval
// (giWaveUpdateInterval below), with NO upper and NO lower bound:
//     mult = (focusDist / priorityDist) ^ falloff / viewBoost
// * falloff - "GI/Priority Falloff", how hard the update rate drops with distance: 1 = the interval is
//   proportional to the distance, 2 = to its square (the far field all but stops), 0.5 = its root
//   (gentle), 0 = no distance term. The curve pivots on priorityDist - a block there keeps factor 1
//   whatever the exponent - so closer than it, a HIGHER falloff is a FASTER rate.
// * focusDist - distance from the SCENE FOCUS (the dominant term). The focus is what the cascades centre
//   on and what every other distance-based quality falloff measures from; in first person it IS the
//   camera. "GI/Priority Distance" is the NOMINAL-RATE distance of a block OUT of view: a block there has
//   factor 1 and so traces at exactly "GI/Update Interval Mult" frames; the interval is proportional to
//   the distance on BOTH sides of it, so a close block's factor < 1 CANCELS the global multiplier, down
//   to every frame. Linear growth is gentler than the ~1/d^2 fall of a probe cell's screen coverage.
// * viewBoost - "GI/Priority Frustum Weight" (>= 1) for a block IN the view frustum: its interval is
//   divided by it. The boost fades to 1 over priorityDist metres OUTSIDE the frustum, so a block
//   just off screen (one small turn from visible, and still lighting visible surfaces through the
//   bounce) keeps most of it, and one far behind has none. This is the big lever in first person, where
//   80% of the near cascades is behind or beside the camera: an unseen probe only has to be converged
//   WHEN it is turned into view, where it gets the boost at once; what it loses meanwhile is response
//   time to a lighting change nobody sees.
// The block's bounding sphere (FRUSTUM test only) carries one extra spacing (a probe lights its whole
// trilinear cell), which is also the guard band that keeps blocks at the screen edge in the in-view class.
// Identical for every lane of the wave, so the wave exits as a unit. Shared by the trace and the debug
// view's priority colour mode.
float giWavePriority(ivec3 waveMin, int spacing)
{
    const float s      = float(spacing);
    const vec3  center = (vec3(waveMin) + 1.5) * s;
    const float radius = 3.6 * s; // block half-diagonal (1.5 * sqrt(3)) + one spacing
    float outDist = 0.0;          // planes are normalized and point inward (Core.Frustum)
    for (int i = 0; i < 6; ++i)
        outDist = max(outDist, -dot(vec4(center, 1.0), u_frustumPlanes[i]) - radius);
    // To the block CENTRE, radius not subtracted: the radius is 3.6 spacings - 7 m in cascade 0, 58 m in
    // cascade 3 - so subtracting it gave the same world distance a different priority per cascade.
    const float focusDist = (distance(center, u_sceneFocus.xyz) - radius);// * (GI_CASCADE_BASE_SPACING / s);
    const float priorityDist = max(u_giPriorityDist, 1.0);
    const float viewBoost    = mix(max(u_giPriorityFrustumWeight, 1.0), 1.0, clamp(outDist / priorityDist, 0.0, 1.0));
    return pow(max(focusDist / priorityDist, 1e-4), max(u_giPriorityFalloff, 0.0)) / viewBoost; // pow(0, 0) is undefined
}

// Width of the cross-cascade fade band at a window's outer faces, in cells (a fraction of the narrowest dim).
#ifndef GI_CASCADE_FADE_BAND
#define GI_CASCADE_FADE_BAND 0.1
#endif
// Interval factor of a COVERED wave (giWaveCovered). Not a skip: the probes stay warm for the all-dead
// fall-through and for the moment the finer window scrolls off them.
#ifndef GI_COVERED_INTERVAL
#define GI_COVERED_INTERVAL 16.0
#endif

// True when NO lookup can reach the wave's probes except the all-dead fall-through: every point inside the
// trilinear support of its 4x4x4 probes (one spacing around the block) lies in the fade-free interior of the
// next FINER cascade's window, so evalProbeSHCoverage and giEvalBounce both return the finer cascade there.
// The priority distance is in CELLS, so without this the centre of every coarse cascade - exactly this
// region - got the cascade's highest update rate. In finer-cascade cells (spacing / 2), against the
// fade == 1 box of giCascadeFade (centred on the UNSNAPPED focus); the half cell covers the finer/coarser
// normal-bias difference (0.25 cells).
bool giWaveCovered(int cascade, ivec3 waveMin)
{
    if (cascade == 0)
        return false;
    const vec3  center = (u_sceneFocus.xyz + vec3(0.0, GI_FOCUS_Y_OFFSET, 0.0)) / float(giCascadeSpacing(cascade - 1));
    const vec3  inner  = vec3(GI_PROBE_DIMS / 2 - 2) - float(GI_PROBE_DIM_MIN) * GI_CASCADE_FADE_BAND - 0.5;
    const vec3  lo = vec3(2 * (waveMin - 1)), hi = vec3(2 * (waveMin + 4));
    return all(greaterThanEqual(lo, center - inner)) && all(lessThanEqual(hi, center + inner));
}

// THE update interval of a wave, in frames: every factor of the probe update rate as ONE product -
// "GI/Update Interval Mult" (the global factor, u_giTrace0.w; NOT a frame count on its own) x the priority
// factors above x GI_COVERED_INTERVAL for a covered wave - floored once, so the interval moves in single
// frames, and floored at ONE frame: the closest blocks trace every frame whatever the global factor. The
// trace's per-probe factors (GI_DEAD_INTERVAL) multiply this.
uint giWaveUpdateInterval(int cascade, ivec3 waveMin, int spacing)
{
    const float covered = giWaveCovered(cascade, waveMin) ? GI_COVERED_INTERVAL : 1.0;
    // The ceiling is numeric safety only (a float past 2^32 has no defined uint conversion), not a rate cap.
    return max(uint(min(max(u_giTrace0.w, 1.0) * giWavePriority(waveMin, spacing) * covered, 1.0e6)), 1u);
}

// THE trace schedule. The irradiance-volume bake re-bakes a voxel on exactly the frames a probe under it can
// change: the trace STAMPS each wave it visits (gi_waveStamp[giWaveWorkgroup], u_frameIndex + 1), and the
// bake tests the stamps plus giProbeFresh - it never re-evaluates the interval itself.
// * giWaveWorkgroup - the trace's workgroup index of a wave (its interleave phase). The trace enumerates
//   toroidal slot space in 4x4x4 blocks, so it is the wave's slot block, cascade-major. In the trace it
//   equals gl_WorkGroupID.x; the bake uses it to find a wave's stamp.
// * giWaveVisits    - the wave's regular visit: every giWaveUpdateInterval frames, interleaved by workgroup.
// * giProbeFresh    - the probe scrolled in since the last traced frame (u_giTrace1.xyz = that focus); a
//   fresh probe traces whatever its interval. The trace and the bake both call it.
uint giWaveWorkgroup(int cascade, ivec3 waveMin)
{
    const ivec3 b = (waveMin & GI_DIM_MASK) >> 2;
    const int blocksX = GI_PROBE_DIM_X / 4, blocksY = GI_PROBE_DIM_Y / 4;
    return uint(cascade) * uint(GI_CASCADE_PROBES / 64) + uint(b.x + (b.y + b.z * blocksY) * blocksX);
}
bool giWaveVisits(uint workgroup, uint updateInterval)
{
    return ((workgroup + u_frameIndex) % updateInterval) == 0u;
}
bool giProbeFresh(int cascade, ivec3 lc)
{
    const ivec3 prevOrigin = giCascadeOrigin(cascade, u_giTrace1.xyz);
    return any(lessThan(lc, prevOrigin)) || any(greaterThanEqual(lc, prevOrigin + GI_PROBE_DIMS));
}

// SH-L1 projections of the probe's hit distance and squared hit distance (misses counted as the depth
// cap). Directional visibility: reconstructing at the probe->surface direction gives the mean and second
// moment of the distance to geometry that way, for a Chebyshev occlusion test at lookup time.
void giReadDepthSH(uint cellBase, out vec4 dsh, out vec4 d2sh)
{
    dsh  = GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4];
    d2sh = GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4];
}

// Band-limited reconstruction of a scalar SH-L1 function at a direction (no cosine convolution - this is
// the raw function estimate, unlike irradiance).
float giEvalDepth(vec4 c, vec3 d) { return dot(c, shBasisL1(d)); }

// Fraction of the probe's gather rays that hit backfacing geometry (~1 = embedded in a wall/terrain).
float giProbeBackfaceFrac(uint cellBase) { return GI_GRID_DATA_NAME[cellBase + GI_MISC_V4].x; }

// Relocation offset: probes that escaped from inside geometry trace from (and are treated as sitting at)
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

// The SH-L1 basis at n with the cosine-lobe convolution (A0, A1) folded in: dot the coefficients with it
// to get the UNCLAMPED irradiance, which is LINEAR in the coefficients.
vec4 giIrradianceBasis(vec3 n)
{
    const float A0 = PI;
    const float A1 = 2.0 * PI / 3.0;
    return shBasisL1(n) * vec4(A0, A1, A1, A1);
}
vec3 giEvalSHLinear(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec4 Yk)
{
    return c0 * Yk.x + c1 * Yk.y + c2 * Yk.z + c3 * Yk.w;
}

// Cosine-convolved irradiance E(n) from SH-L1 coefficients. Diffuse exit radiance is albedo/PI * E(n).
vec3 giEvalSH(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 n)
{
    return max(giEvalSHLinear(c0, c1, c2, c3, giIrradianceBasis(n)), vec3(0.0));
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
// to in open space - instead of a differently-shaped cheap approximation that diverges at low sun angles
// (a single sky sample along the normal misses the bright horizon in-scatter band the probes gather).
// Returns cosine-convolved irradiance E(n), like evalProbeSHCoverage.
#define GI_SKY_SH_BASE (uint(GI_NUM_CASCADES) * uint(GI_CASCADE_PROBES) * GI_PROBE_STRIDE_V4) // vec4 index; 3 vec4s of SH
#ifdef GI_VOLUME_TEXTURES_NAME
// Volume mode: the bake's copy of the same 3 vec4s (the image at GI_VOLUME_SKY_IMAGE), so a consumer in volume
// mode reads nothing from the probe buffer.
vec3 giEvalSkySH(vec3 n)
{
    const vec4 p0 = texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(0, 0, 0), 0);
    const vec4 p1 = texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(1, 0, 0), 0);
    const vec4 p2 = texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(2, 0, 0), 0);
    return giEvalSH(p0.xyz, vec3(p0.w, p1.xy), vec3(p1.zw, p2.x), p2.yzw, n);
}
#else
vec3 giEvalSkySH(vec3 n) { return giEvalCell(GI_SKY_SH_BASE, n); }
#endif

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

// The Chebyshev test's two inputs for direction dir FROM the probe: the mean distance to geometry and its
// variance, both after the mean scale. Shared with the debug view's visibility mode.
// * Mean scale k (u_giVisParams.w, > 1) widens each probe's visible footprint: the blurry L1 reconstruction
//   underestimates distance sideways past a wall (x 1.4 .. 1.75 too short for a wall 0.5 .. 0.1 spacings
//   away), shrinking the un-occluded region around a probe; scaling pushes the boundary back out.
// * The scale applies to the DEPTH, so the second moment scales by k^2 and the variance by k^2. The old
//   code scaled the mean only: mean2 - (k mean)^2 was negative nearly everywhere, the variance was ALWAYS
//   the floor, and the stored second moment did nothing. Measured, the variance is large sideways past a
//   wall (the depth really spreads there: soft, mostly open) and at the floor toward the wall (sharp).
// * The variance floor (u_giVisParams.x, fraction of spacing) is the minimum edge softness: it covers the
//   L1 mean's own error and the per-visit ray jitter.
// Both moments are clamped at 0: toward a close wall the L1 reconstruction rings below it.
void giVisMoments(vec4 dsh, vec4 d2sh, vec3 dir, int s, out float mean, out float variance)
{
    const float k      = u_giVisParams.w;
    const float raw    = max(giEvalDepth(dsh, dir), 0.0);
    const float raw2   = max(giEvalDepth(d2sh, dir), 0.0);
    const float minDev = u_giVisParams.x * float(s);
    mean     = min(raw * k, GI_DEPTH_CAP_SPACING * float(s));
    variance = max((raw2 - raw * raw) * k * k, minDev * minDev);
}

// Trilinear irradiance from one cascade's 8 nearest probes, with DDGI-style backface weighting (probes
// behind the surface are faded out to limit light leaking through thin geometry). totalW returns the
// summed weight so the caller can detect the all-backfaced case and fall through to a coarser cascade.
// samplePos is the normal-BIASED query point (giBiasedSample), not the raw surface position.
// Returns the weight-normalized UNCLAMPED irradiance (giEvalSHLinear, Yk = giIrradianceBasis(n)). The
// max(0) is a nonlinearity - applied per probe (or per cascade) it leaves kinks that show up as ripples on
// flat surfaces and as a harder cascade seam - so the caller clamps ONCE, after the cross-cascade blend.
// Everything before the clamp is linear in the coefficients, so reducing each probe to 3 floats here is
// exactly the old "blend the 12 coefficients, evaluate once": 3 accumulators live instead of 12, and the
// first cascade holds 3 floats (not 12) while the second one samples - that span was the register peak.
void giSampleCascade(int c, int s, ivec3 base, vec3 frac, vec3 samplePos, vec3 n, vec4 Yk,
                     out vec3 eLin, out float totalW)
{
    eLin = vec3(0.0);
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

        // The misc vec4 (x = backface fraction, yzw = relocation offset) is loaded ONCE: the buffer is
        // not readonly under GI_PROBE_WRITE, so the compiler cannot merge the two accessor loads itself.
        const vec4 misc = GI_GRID_DATA_NAME[cellBase + GI_MISC_V4];
        // Reject probes embedded in geometry (mostly-backface gather) - their near-black SH is not signal.
        w *= 1.0 - smoothstep(GI_BACKFACE_DEAD_MIN, GI_BACKFACE_DEAD_MAX, misc.x);
        if (w <= 0.0)
            continue;

        // Directional terms use the probe's relocated position (where it actually traced from); the
        // trilinear weights above stay on the unmoved lattice.
        vec3 probeWorld = vec3(lc) * float(s) + misc.yzw;

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
            // variance bounds how likely it is still visible. No depth data yet (freshly cleared buffer)
            // -> don't occlude. That is tested on the DC coefficient, NOT on the reconstructed mean2: toward
            // a close wall the L1 reconstruction of d^2 rings to <= 0, and the old `mean2 > 1e-3` test read
            // that as "no data" and switched the occlusion OFF exactly where it matters most (a leak
            // through every wall a probe sits next to). The variance floor softens the blurry L1 reconstruction.
            vec4 dsh, d2sh;
            giReadDepthSH(cellBase, dsh, d2sh);
            float mean, variance;
            giVisMoments(dsh, d2sh, -dirToProbe, s, mean, variance);
            float d = min(len, GI_DEPTH_CAP_SPACING * float(s) * 0.95);
            if (d2sh.x > 1e-4 && d > mean)
            {
                // u_giVisParams.z = weight floor. The power is the GI_VIS_CHEB_POWER define: a chain of
                // multiplies instead of a pow per probe (16 probes a pixel).
                float delta    = d - mean;
                float cheb     = variance / (variance + delta * delta);
                w *= max(giChebPow(cheb), u_giVisParams.z);
            }
        }
        if (w <= 0.0)
            continue;

        vec3 c0, c1, c2, c3;
        giReadSH(cellBase, c0, c1, c2, c3);
        eLin += w * giEvalSHLinear(c0, c1, c2, c3, Yk);
        totalW += w;
    }
    if (totalW <= 1e-4)
        return;
    eLin *= 1.0 / totalW;
}

// Cross-cascade fade of cascade spacing s at the (biased) sample point p: 1 = this cascade alone, 0 = the
// next coarser one alone (for the outermost cascade the callers use it as COVERAGE). bandCells is the
// ramp width in cells.
// * CONTINUOUS in p. The old fade came from the integer cell index, so it was a staircase of one step per
//   cell - two hard steps over a 1.6-cell band.
// * Measured from the UNSNAPPED focus, not from the window: the window snaps a whole cell at a time, and a
//   band tied to it jumped a cell (2 m .. 16 m) along the entire seam whenever the focus crossed a cell
//   line. The snapped window always holds the 8-probe stencil of every point within DIM/2 - 2 cells of the
//   focus, so fade > 0 implies giCascadeFits. giWaveCovered uses the same box.
float giCascadeFade(int s, vec3 p, float bandCells)
{
    const vec3 center = u_sceneFocus.xyz + vec3(0.0, GI_FOCUS_Y_OFFSET, 0.0);
    const vec3 inside = vec3(GI_PROBE_DIMS / 2 - 2) - abs(p - center) / float(s); // cells inside the box, per axis
    return smoothstep(0.0, 1.0, min(min(inside.x, inside.y), inside.z) / bandCells);
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
// where no cascade covers the point). Callers blend their ambient fallback in by it - without the fade,
// leaving the probe field dropped the bounce light in a single step, which read as a bright square zone
// around the camera at the last cascade's window face.
// The walk tests the FADE only, not giCascadeFits: fade > 0 already implies the 8-probe stencil fits (see
// giCascadeFade), and the next coarser cascade of a blend always fits (its box is twice as wide around the
// same focus). Past the outermost box (fade 0) nothing is sampled: coverage 0 means every caller uses only
// its sky fallback, so a sample there would be thrown away.
vec3 evalProbeSHCoverage(vec3 worldPos, vec3 n, out float coverage)
{
    coverage = 1.0;
    const vec4 Yk = giIrradianceBasis(n);
    for (int c = 0; c < GI_NUM_CASCADES; ++c)
    {
        const int s = giCascadeSpacing(c);
        const vec3 p = giBiasedSample(worldPos, n, s);
        // Outermost cascade: there is nothing coarser to fade into, so the fade is the COVERAGE instead
        // (wider band than the inter-cascade one - this hands over to a fallback, not to more data).
        const bool  last = c == GI_NUM_CASCADES - 1;
        const float fade = giCascadeFade(s, p, float(GI_PROBE_DIM_MIN) * (last ? 0.2 : GI_CASCADE_FADE_BAND));
        if (fade <= 0.0)
            continue; // past the fade box: the coarser cascade alone (the window's snap slack is not sampled)

        // Weight/visibility terms measure from the biased point (DDGI surface bias): querying from the
        // raw surface point puts the Chebyshev direction exactly in the wall plane, where the blurry L1
        // depth reconstruction underestimates distance and false-occludes everything lateral to a probe
        // (bright probe-footprint circles on walls).
        const vec3 pf = p / float(s);
        const ivec3 base = ivec3(floor(pf));
        vec3 e0; float w0;
        giSampleCascade(c, s, base, pf - vec3(base), p, n, Yk, e0, w0);
        if (w0 <= 1e-4)
            continue; // every probe backfaced -> try a coarser (differently-aligned) cascade

        if (last)
            coverage = fade;
        else if (fade < 1.0)
        {
            const int  s2 = giCascadeSpacing(c + 1);
            const vec3 p2 = giBiasedSample(worldPos, n, s2);
            const vec3 pf2 = p2 / float(s2);
            const ivec3 base2 = ivec3(floor(pf2));
            vec3 e1; float w1;
            giSampleCascade(c + 1, s2, base2, pf2 - vec3(base2), p2, n, Yk, e1, w1);
            if (w1 > 1e-4)
                e0 = mix(e1, e0, fade);
        }
        return max(e0, vec3(0.0));
    }
    coverage = 0.0;
    return vec3(-1.0);
}

vec3 evalProbeSH(vec3 worldPos, vec3 n)
{
    float coverage;
    return evalProbeSHCoverage(worldPos, n, coverage);
}

#ifdef GI_VOLUME_TEXTURES_NAME
// THE IRRADIANCE VOLUME read side (baked by gi_volume_bake.cs.glsl). The includer declares a sampler3D array
// GI_VOLUME_TEXTURES_NAME[GI_VOLUME_MAX_IMAGES], cascade c at [c * GI_VOLUME_IMAGES_PER_CASCADE + k] in the
// RendererVKLayout::GI_VOLUME_FORMATS order: k = 0 L0 x W, k = 1..2 the L1 ratios, k = 3 (last ratio, W) -
// REPEAT addressing, linear filtering, no mips. One cascade: 4 filtered fetches (16 B per voxel) instead of
// 8 probes x 6 vec4 loads. Returns the summed weight (<= 1e-4 = every probe around the point is dead: the
// caller falls through to a coarser cascade, like giSampleCascade).
// The ratios filter unweighted while L0 filters weight-correct: where L0 changes fast between voxels the
// blended direction is slightly off, and a dead voxel's ratio 0 pulls the neighbours' L1 a little toward 0.
float giVolumeCascade(int c, int s, vec3 p, vec4 Yk, out vec3 eLin)
{
    // Voxel centres sit at (fine lattice coord + 0.5), which is exactly the texel-centre convention, and the
    // toroidal slot is the coord mod the dims, which is exactly REPEAT: the normalized coord is the fine
    // lattice position over the dims. fract keeps it small for the texture unit's fixed-point conversion.
    const vec3 uvw = fract(p * (float(GI_VOLUME_RES) / float(s)) / vec3(GI_PROBE_DIMS * GI_VOLUME_RES));
    const int  i   = c * GI_VOLUME_IMAGES_PER_CASCADE;
    // textureLod: the call sits in divergent control flow, where implicit derivatives are undefined.
    const vec3 l0W = textureLod(GI_VOLUME_TEXTURES_NAME[nonuniformEXT(i + 0)], uvw, 0.0).rgb;
    const vec4 qa  = textureLod(GI_VOLUME_TEXTURES_NAME[nonuniformEXT(i + 1)], uvw, 0.0); // q1.rgb, q2.r
    const vec4 qb  = textureLod(GI_VOLUME_TEXTURES_NAME[nonuniformEXT(i + 2)], uvw, 0.0); // q2.gb, q3.rg
    const vec2 tl  = textureLod(GI_VOLUME_TEXTURES_NAME[nonuniformEXT(i + 3)], uvw, 0.0).rg; // q3.b, W
    const float W  = tl.y;
    // c_k = q_k * sqrt(3) * c0, so the unclamped irradiance per channel is c0 * (Yk.x + sqrt(3) (q . Yk.yzw)).
    const vec3 c0 = W > 1e-4 ? l0W / W : vec3(0.0);
    const vec3 q1 = qa.xyz, q2 = vec3(qa.w, qb.xy), q3 = vec3(qb.zw, tl.x);
    eLin = c0 * (Yk.x + GI_SQRT3 * (q1 * Yk.y + q2 * Yk.z + q3 * Yk.w));
    return W;
}

// evalProbeSHCoverage against the volume: the same cascade walk (fade-only, see there), cross-cascade fade,
// coverage and dead fall-through, with giVolumeCascade in place of giSampleCascade. The normal still biases the
// sample point (per pixel); only the probe-direction half-Lambert weight is gone (see the bake).
vec3 evalProbeVolumeCoverage(vec3 worldPos, vec3 n, out float coverage)
{
    coverage = 1.0;
    const vec4 Yk = giIrradianceBasis(n);
    for (int c = 0; c < GI_NUM_CASCADES; ++c)
    {
        const int s = giCascadeSpacing(c);
        const vec3 p = giBiasedSample(worldPos, n, s);
        const bool  last = c == GI_NUM_CASCADES - 1;
        const float fade = giCascadeFade(s, p, float(GI_PROBE_DIM_MIN) * (last ? 0.2 : GI_CASCADE_FADE_BAND));
        if (fade <= 0.0)
            continue;

        vec3 e0;
        if (giVolumeCascade(c, s, p, Yk, e0) <= 1e-4)
            continue;

        if (last)
            coverage = fade;
        else if (fade < 1.0)
        {
            const int s2 = giCascadeSpacing(c + 1);
            vec3 e1;
            if (giVolumeCascade(c + 1, s2, giBiasedSample(worldPos, n, s2), Yk, e1) > 1e-4)
                e0 = mix(e1, e0, fade);
        }
        return max(e0, vec3(0.0));
    }
    coverage = 0.0;
    return vec3(-1.0);
}
#endif

#ifdef GI_PROBE_HALF
// ---- fp16 read side ----------------------------------------------------------------------------------
// The shading-side reads for includers whose shading is half (define GI_PROBE_HALF and enable
// GL_EXT_shader_explicit_arithmetic_types): half normal in, half irradiance out, so a half caller converts
// nothing. Irradiance here is sky + bounce light, single digits: well inside the half range.
// The CASCADE WALK stays 32-bit inside (evalProbeVolumeCoverage): measured on the RTX 4090, a half walk -
// half fetch results, half SH basis, half cascade blend, in every combination - cost the lit FS 16 B/thread
// of spills (64/32 -> 64/48) against the 32-bit walk with the result converted once. The sky SH read is half
// (measured neutral; the reflection fog shares it).
f16vec4 giIrradianceBasisH(f16vec3 n)
{
    const float16_t A0Y = float16_t(0.282095 * PI);
    const float16_t A1Y = float16_t(0.488603 * 2.0 * PI / 3.0);
    return f16vec4(A0Y, A1Y * n.y, A1Y * n.z, A1Y * n.x);
}
f16vec3 giEvalSHH(f16vec3 c0, f16vec3 c1, f16vec3 c2, f16vec3 c3, f16vec3 n)
{
    const f16vec4 Yk = giIrradianceBasisH(n);
    return max(c0 * Yk.x + c1 * Yk.y + c2 * Yk.z + c3 * Yk.w, f16vec3(0.0));
}
#ifdef GI_VOLUME_TEXTURES_NAME
f16vec3 giEvalSkySHH(f16vec3 n)
{
    const f16vec4 p0 = f16vec4(texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(0, 0, 0), 0));
    const f16vec4 p1 = f16vec4(texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(1, 0, 0), 0));
    const f16vec4 p2 = f16vec4(texelFetch(GI_VOLUME_TEXTURES_NAME[GI_VOLUME_SKY_IMAGE], ivec3(2, 0, 0), 0));
    return giEvalSHH(p0.xyz, f16vec3(p0.w, p1.xy), f16vec3(p1.zw, p2.x), p2.yzw, n);
}
#else
f16vec3 giEvalSkySHH(f16vec3 n) { return f16vec3(giEvalSkySH(vec3(n))); }
#endif
// The probe-field indirect irradiance / pi at a half-shaded point, faded to the sky SH over the field's edge:
// the whole block every half caller runs (computeLitColor, the ocean's hits, the film's mirror).
f16vec3 giIndirectOverPiH(vec3 worldPos, f16vec3 n)
{
    // 32-bit to the end, converted once (measured: a half coverage / sky blend here cost the lit FS 16 B).
    const vec3 n32 = vec3(n);
    float coverage;
#ifdef GI_VOLUME_TEXTURES_NAME
    const vec3 E = evalProbeVolumeCoverage(worldPos, n32, coverage);
#else
    const vec3 E = evalProbeSHCoverage(worldPos, n32, coverage);
#endif
    vec3 indirect = E.x >= 0.0 ? E * INV_PI : vec3(0.0);
    if (coverage < 1.0)
        indirect = mix(giEvalSkySH(n32) * INV_PI, indirect, coverage);
    return f16vec3(indirect);
}
#endif // GI_PROBE_HALF

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

// Multi-bounce lookup for the trace's gather HITS - the cheap cousin of evalProbeSHCoverage. The result
// is albedo-scaled and temporally blended at a few permille per visit, so noise is free: no Chebyshev
// visibility (skips the two depth-moment loads and the reconstruction per probe) and no cross-cascade
// fade. The walk starts at the FINEST cascade, like the shading lookup (a failed fit is arithmetic only):
// starting at the tracing probe's own cascade read the coarse probes UNDER a finer window, which
// giWaveCovered now lets go stale. 4 vec4 loads per probe (misc + SH) instead of 6, one cascade instead
// of up to two. Backface-dead rejection and the half-Lambert probe-direction fade stay - they are what
// keeps a wall from leaking into the bounce.
// coverage behaves like evalProbeSHCoverage's (1 inside, fading over the outermost window's edge band).
vec3 giEvalBounce(vec3 worldPos, vec3 n, out float coverage)
{
    coverage = 1.0;
    for (int c = 0; c < GI_NUM_CASCADES; ++c)
    {
        vec3  p = giBiasedSample(worldPos, n, giCascadeSpacing(c));
        ivec3 base, origin; int s; vec3 frac;
        if (!giCascadeFits(c, p, base, origin, s, frac))
            continue;
        // The outermost cascade's coverage BEFORE sampling: at 0 the caller scales the bounce by 0, so the
        // 8-probe loop would be thrown away. (Inner cascades walk by the fit test on purpose: this lookup has
        // no fade band and uses the whole window.)
        const bool last = c == GI_NUM_CASCADES - 1;
        const float lastCoverage = last ? giCascadeFade(s, p, float(GI_PROBE_DIM_MIN) * 0.2) : 1.0;
        if (lastCoverage <= 0.0)
            continue;

        vec3 a0 = vec3(0.0), a1 = vec3(0.0), a2 = vec3(0.0), a3 = vec3(0.0);
        float totalW = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            vec3 w3 = mix(1.0 - frac, frac, vec3(off));
            float w = w3.x * w3.y * w3.z;
            if (w <= 0.0)
                continue;
            ivec3 lc = base + off;
            uint cellBase = giProbeBase(c, lc);
            vec4 misc = GI_GRID_DATA_NAME[cellBase + GI_MISC_V4]; // x = backface fraction, yzw = offset
            w *= 1.0 - smoothstep(GI_BACKFACE_DEAD_MIN, GI_BACKFACE_DEAD_MAX, misc.x);
            if (w <= 0.0)
                continue;
            vec3 toProbe = vec3(lc) * float(s) + misc.yzw - p;
            float len = length(toProbe);
            if (len > 1e-4)
                w *= dot(n, toProbe / len) * 0.5 + 0.5;
            if (w <= 0.0)
                continue;
            vec3 c0, c1, c2, c3;
            giReadSH(cellBase, c0, c1, c2, c3);
            a0 += w * c0; a1 += w * c1; a2 += w * c2; a3 += w * c3;
            totalW += w;
        }
        if (totalW <= 1e-4)
            continue; // every probe backfaced -> try a coarser cascade

        if (last)
            coverage = lastCoverage;
        float inv = 1.0 / totalW;
        return giEvalSH(a0 * inv, a1 * inv, a2 * inv, a3 * inv, n);
    }
    coverage = 0.0;
    return vec3(-1.0);
}

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
// backface-hit fraction (the embedded-probe rejection signal), and store the relocation offset
// (unblended - the relocation logic is already iterative). The fraction and the offset share the misc
// vec4: one read + one write for both. prevBackfaceFrac = the stored fraction the caller already read.
void giBlendProbeStats(uint cellBase, vec4 dsh, vec4 d2sh, float prevBackfaceFrac, float backfaceFrac, vec3 offset, float alpha)
{
    GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4]  = mix(GI_GRID_DATA_NAME[cellBase + GI_DEPTH_V4],  dsh,  alpha);
    GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4] = mix(GI_GRID_DATA_NAME[cellBase + GI_DEPTH2_V4], d2sh, alpha);
    GI_GRID_DATA_NAME[cellBase + GI_MISC_V4] = vec4(mix(prevBackfaceFrac, backfaceFrac, alpha), offset);
}

#endif // GI_PROBE_WRITE

#endif // GI_PROBE_INC_GLSL
