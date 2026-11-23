#version 460

// THE SNOW DEFORMATION's two passes (SnowDeformPipeline; Docs/Context/RendererVK/Snow.md "The deformation"), on the two
// R32UI TOROIDAL windows (slot = lattice & (SNOW_DEFORM_RES - 1)): per texel hi16 = the offset from the UNDISTURBED snow
// surface over [SNOW_DEFORM_MIN, + SNOW_DEFORM_RANGE] m, lo16 = the snowfall (mm, mod 65536) when it was stamped.
//   pc_mode 0, THE MAINTENANCE (one thread per texel, y = the map): a texel outside the window the map held (pc_*Prev)
//            scrolled in - back to undisturbed.
//   pc_mode 1, THE STAMPS (one workgroup per deformer, x = its index): over its footprint in both windows, a CAPSULE (a
//            sphere swept between two FEET points: a unit's, a player's) or a CRATER (a bowl around an impact) PRESSES the
//            snow (the offset goes down to the deformer's bottom, never under the ground) and pushes a BERM up around it
//            (the berm share x the pressed depth, on undisturbed snow only). CAS loops: the stamps of one frame and of
//            different deformers meet in any order.
// The snow at a stamp: the ground = the site map's top (a roof is a ground: a foot a metre or more under it is inside the
// building and dents nothing), else the terrain-data map's height; the undisturbed depth = the pack there (once per stamp,
// at its centre: snow.inc.glsl snowGroundDepth + the water gate). The readers refill a track by the snow fallen since.

layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "ubo.inc.glsl"

layout (binding = 1, r32ui) uniform restrict uimage2D u_fine;
layout (binding = 2, r32ui) uniform restrict uimage2D u_coarse;
#define TERRAIN_HEIGHT_BINDING 3
#include "terrain_height.inc.glsl"
// The snow site map (snow.inc.glsl): bound directly, in the window it held before this frame's trace (the stamps run first).
layout (binding = 4) uniform sampler2D u_snowSiteTop;
layout (binding = 5) uniform sampler2D u_snowSitePass;
#define SNOW_SITE_TOP_SAMPLER u_snowSiteTop
#define SNOW_SITE_PASS_SAMPLER u_snowSitePass
#define SNOW_SITE_ORIGIN u_snow_siteReadOrigin
#include "snow.inc.glsl"

struct SnowDeformer // RendererVKLayout::SnowDeformer
{
    vec3 a;
    float radius;
    vec3 b;
    uint packed;
};
layout (buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SnowDeformerList { SnowDeformer d[]; };
#include "push.generated.glsl"

#define BERM_WIDTH 0.6 // x the radius: the ring outside the press that rises
#define DEFORM_SEED_FINE 1u // pc_flags (SnowDeformPipeline::record)
#define DEFORM_REBASE 2u

uint deformPack(float offset, uint stamp)
{
    const uint o = uint(round(clamp((offset - SNOW_DEFORM_MIN) / SNOW_DEFORM_RANGE, 0.0, 1.0) * 65535.0));
    return (o << 16) | (stamp & 0xFFFFu);
}
float deformOffset(uint texel)
{
    return float(texel >> 16) * (SNOW_DEFORM_RANGE / 65535.0) + SNOW_DEFORM_MIN;
}
// The offset as the readers see it now: a track refills by the snow fallen since its stamp (snow.inc.glsl snowDeformDecode).
float deformNow(uint texel)
{
    float offset = deformOffset(texel);
    if (offset < 0.0)
        offset = min(0.0, offset + mod(u_snow_fallNow - float(texel & 0xFFFFu), 65536.0) * 0.001);
    return offset;
}

uint mapLoad(bool coarse, ivec2 slot) { return coarse ? imageLoad(u_coarse, slot).r : imageLoad(u_fine, slot).r; }
uint mapSwap(bool coarse, ivec2 slot, uint compare, uint value)
{
    return coarse ? imageAtomicCompSwap(u_coarse, slot, compare, value) : imageAtomicCompSwap(u_fine, slot, compare, value);
}

// The top surface under the snow at xz: the site map's (a roof, a rock, the terrain) inside its window, else the terrain.
float stampGround(vec2 xz)
{
    if (u_snow_sitePresent > 0.5)
    {
        const vec2 lattice = xz * (1.0 / SNOW_SITE_TEXEL);
        const vec2 rel = (lattice - SNOW_SITE_ORIGIN) * (1.0 / float(SNOW_SITE_RES)) - 0.5;
        if (max(abs(rel.x), abs(rel.y)) < 0.48)
        {
            const float top = textureLod(SNOW_SITE_TOP_SAMPLER, lattice * (1.0 / float(SNOW_SITE_RES)), 0.0).r;
            if (top > SNOW_SITE_NO_TOP * 0.5)
                return top;
        }
    }
    return terrainHeightAt(xz);
}

shared float s_depth;

void main()
{
    if (pc_mode == 0u)
    {
        // THE MAINTENANCE: a texel that scrolled in goes back to undisturbed - a FINE one takes the coarse texel under it
        // (DEFORM_SEED_FINE: what was trodden there while the fine window was elsewhere stays; the raw texel, so its refill
        // clock goes on). Safe in one dispatch: the coarse window clears only strips >= ~110 m from the focus, the fine
        // window lies within ~36 m (and a fresh start, which clears all of the coarse one, does not seed).
        // DEFORM_REBASE: every held texel whose track has refilled goes back to undisturbed, before the 65.5 m clock wraps
        // and shows it again.
        const bool coarse = gl_WorkGroupID.y == 1u;
        const uint i = gl_GlobalInvocationID.x;
        const ivec2 rel = ivec2(i % uint(SNOW_DEFORM_RES), i / uint(SNOW_DEFORM_RES));
        const ivec2 lc = (coarse ? pc_coarseOrigin : pc_fineOrigin) + rel;
        const ivec2 prevRel = lc - (coarse ? pc_coarsePrev : pc_finePrev);
        const ivec2 slot = lc & ivec2(SNOW_DEFORM_RES - 1);
        uint value = SNOW_DEFORM_UNDISTURBED;
        if (all(greaterThanEqual(prevRel, ivec2(0))) && all(lessThan(prevRel, ivec2(SNOW_DEFORM_RES))))
        {
            if ((pc_flags & DEFORM_REBASE) == 0u)
                return;
            const uint held = mapLoad(coarse, slot);
            if (deformOffset(held) >= 0.0 || deformNow(held) < 0.0)
                return; // undisturbed, a berm, or a track still open
        }
        else if (!coarse && (pc_flags & DEFORM_SEED_FINE) != 0u)
        {
            const ivec2 under = ivec2(floor((vec2(lc) + 0.5) * (SNOW_DEFORM_FINE_TEXEL / SNOW_DEFORM_COARSE_TEXEL)));
            value = imageLoad(u_coarse, under & ivec2(SNOW_DEFORM_RES - 1)).r;
        }
        if (coarse)
            imageStore(u_coarse, slot, uvec4(value));
        else
            imageStore(u_fine, slot, uvec4(value));
        return;
    }

    // THE STAMPS
    const SnowDeformer d = SnowDeformerList(pc_deformers).d[gl_WorkGroupID.x];
    const bool crater = (d.packed & SNOW_DEFORMER_CRATER) != 0u;
    const float bermShare = float(d.packed & 0xFFFFu) * (1.0 / 65535.0);
    const float r = d.radius;
    const vec3 centre = crater ? d.a : 0.5 * (d.a + d.b);
    if (gl_LocalInvocationIndex == 0u)
    {
        // The undisturbed depth, once per stamp at its centre (the pack varies over kilometres, the ground per texel), as
        // the VS lifts it: x the slope rule (the terrain's normal over +-1 m, about the near mesh's step) and the water gate.
        float depth = 0.0;
        if (snowPackLive() && terrainHeightMapPresent())
        {
            const float ground = stampGround(centre.xz);
            const vec4 climate = terrainClimateAt(centre.xz);
            const float hx = terrainHeightAt(centre.xz + vec2(1.0, 0.0)) - terrainHeightAt(centre.xz - vec2(1.0, 0.0));
            const float hz = terrainHeightAt(centre.xz + vec2(0.0, 1.0)) - terrainHeightAt(centre.xz - vec2(0.0, 1.0));
            const float normalY = 2.0 / sqrt(hx * hx + hz * hz + 4.0);
            depth = snowGroundDepth(terrainTemperatureAt(climate, ground), vec3(centre.x, ground, centre.z))
                * snowGroundHolds(normalY) * snowGroundDry(ground, terrainDataAt(centre.xz).y, 0.0);
        }
        s_depth = depth;
    }
    barrier();
    const float depth = s_depth;
    if (depth <= 0.002)
        return; // no snow to press

    // The wall of a track deeper than its radius slopes out at "Track wall slope" (rise per metre): at most depth / slope wider.
    const float wallSlope = max(u_snow_trackWallSlope, 0.1);
    const float reach = r * (1.0 + BERM_WIDTH) + (crater ? 0.0 : depth / wallSlope);
    const vec2 lo = (crater ? d.a.xz : min(d.a.xz, d.b.xz)) - reach;
    const vec2 hi = (crater ? d.a.xz : max(d.a.xz, d.b.xz)) + reach;
    const vec2 seg = d.b.xz - d.a.xz;
    const float segLen2 = dot(seg, seg);
    const uint stampNow = uint(u_snow_fallNow) & 0xFFFFu;

    for (int m = 0; m < 2; ++m)
    {
        const bool coarse = m == 1;
        const float texel = coarse ? SNOW_DEFORM_COARSE_TEXEL : SNOW_DEFORM_FINE_TEXEL;
        const ivec2 origin = coarse ? pc_coarseOrigin : pc_fineOrigin;
        const ivec2 l0 = max(ivec2(floor(lo / texel)), origin);
        const ivec2 l1 = min(ivec2(ceil(hi / texel)), origin + ivec2(SNOW_DEFORM_RES - 1));
        if (any(greaterThan(l0, l1)))
            continue;
        const ivec2 size = l1 - l0 + 1;
        const uint count = uint(size.x * size.y);
        for (uint i = gl_LocalInvocationIndex; i < count; i += 64u)
        {
            const ivec2 lc = l0 + ivec2(int(i % uint(size.x)), int(i / uint(size.x)));
            const vec2 xz = (vec2(lc) + 0.5) * texel;
            const float ground = stampGround(xz);
            const float surface = ground + depth;

            // The deformer's bottom at xz, its distance from the axis, and how deep it presses at its axis.
            float dist, pressed;
            float target = 1.0; // the offset it presses to (> 0 = no press here)
            float bermIn = r;   // where the berm's ring starts (the press's outer edge)
            if (crater)
            {
                dist = length(xz - d.a.xz);
                pressed = min(d.b.x, depth);
                if (d.a.y < ground - r || d.a.y > surface + r) // not in the snow (in the air, a roof under it, a cave): the
                    continue;                                   // colliders hold no snow, so a shot lands on the ground under it
                if (dist < r)
                    target = -pressed * (1.0 - (dist / r) * (dist / r));
            }
            else
            {
                const float t = segLen2 > 1e-6 ? clamp(dot(xz - d.a.xz, seg) / segLen2, 0.0, 1.0) : 0.0;
                const vec3 c = mix(d.a, d.b, t);
                dist = length(xz - c.xz);
                pressed = clamp(surface - c.y, 0.0, depth);
                if (c.y < ground - 1.0 || c.y > surface + 0.05) // under a roof (inside), or over the snow (in the air)
                    continue;
                if (dist < r)
                    target = c.y + r - sqrt(r * r - dist * dist) - surface; // the bottom of the sphere at its feet
                // THE WALL: where the feet are deeper than the radius under the snow, the sphere's edge leaves a vertical step
                // of that depth in one texel, which the tessellation's vertex grid samples into teeth along the track. The
                // snow slopes from the press's edge up to the undisturbed surface instead (packed snow holds a steep wall).
                const float edge = c.y + r - surface; // the press at the sphere's edge (< 0: below the surface)
                if (edge < 0.0)
                {
                    target = min(target, edge + max(dist - r, 0.0) * wallSlope);
                    bermIn = r - edge / wallSlope;
                }
            }
            target = max(target, -depth); // never under the ground
            float berm = 0.0;
            const float bermOut = bermIn + r * BERM_WIDTH;
            if (target >= -0.002 && dist >= bermIn && dist < bermOut)
            {
                const float u = (dist - bermIn) / (bermOut - bermIn);
                berm = bermShare * pressed * 4.0 * u * (1.0 - u);
                if (berm < 0.002)
                    continue;
            }
            else if (target >= -0.002)
                continue;

            const ivec2 slot = lc & ivec2(SNOW_DEFORM_RES - 1);
            uint old = mapLoad(coarse, slot);
            for (int k = 0; k < 8; ++k)
            {
                const float now = deformNow(old);
                uint want;
                if (berm > 0.0)
                {
                    if (now < 0.0 || berm <= now + 0.001)
                        break; // a pressed texel keeps its press; a higher berm stays
                    want = deformPack(berm, old & 0xFFFFu);
                }
                else
                {
                    if (target >= now - 0.001)
                        break; // already that deep
                    want = deformPack(target, stampNow);
                }
                const uint seen = mapSwap(coarse, slot, old, want);
                if (seen == old)
                    break;
                old = seen;
            }
        }
    }
}
