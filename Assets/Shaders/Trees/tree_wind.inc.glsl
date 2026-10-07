#ifndef TREE_WIND_INC_GLSL
#define TREE_WIND_INC_GLSL

#include "wind.inc.glsl"

// TREE WIND: a vertex-shader sway of the procedural trees in the shared VEGETATION wind (wind.inc.glsl: the weather
// wind + its gusts, the grass bends in the same), tuned by u_foliage_wind* (FoliageParams wind*, "Trees/Wind"). No
// textures and no per-tree data. Three layers:
//   TRUNK  - the whole tree leans downwind and sways, the offset growing with (height / ref height)^2 and the tree
//            dropping slightly so it keeps its length. From the mesh-local height alone, so the mesh, the merged branch
//            cards and the whole-tree billboard bend alike (the crossfades stay aligned).
//   BRANCH - meshes only: per-vertex weights + phases from the bake (Procedural TreeGenerator), each branch bending as one
//            unit; faded out before the mid tier's cards ("Branch fade").
//   LEAF   - leaf cards only: the card's tip flutters along its normal; faded out sooner ("Leaf fade").
// The gusts are wind.inc.glsl's (analytic: no field texture).
//
// THE PAYLOAD rides the tangent's w MAGNITUDE (RenderMeshData: texCoords.z), its sign staying the handedness:
//   |w| = 1          an ordinary mesh: no wind
//   |w| in (1, 2)    a tree mesh vertex: 1 + payload / 2^22, payload =
//                      bits 0..6   the module weight (0 at the module's root .. 127 at its tip, along the root bone)
//                      bits 7..12  the sub-branch weight (0 .. 63 along its level-1 bone)
//                      bits 13..16 the sub-branch phase, bits 17..19 the module placement's phase
//                      bit 20      a leaf card's TIP vertex (flutters), bit 21 = TREE_WIND_BIT (always set)
//   |w| >= 2         the tree billboards' / branch cards' strip and axis codes (lit FS): the trunk bend only
// Requires the UBO.

#define TREE_WIND_BIT (1u << 21)
#define TREE_WIND_TIP_BIT (1u << 20)
#define TREE_WIND_TAU 6.2831853

// False for a mesh without wind; payload 0 = the trunk bend only.
bool treeWindPayload(float tangentW, out uint payload)
{
    const float m = abs(tangentW);
    payload = 0u;
    if (m >= 2.0)
        return true;
    if (m <= 1.0)
        return false;
    payload = uint((m - 1.0) * 4194304.0 + 0.5);
    return (payload & TREE_WIND_BIT) != 0u;
}

float treeWindHash(uint h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h) * (1.0 / 4294967296.0);
}

// The world-space offset of a tree vertex at `time`. localPos = the mesh-local position (+Y up from the tree's base),
// worldNormal = the vertex normal in world space (the leaf flutter), dist = the tree's distance from the camera.
vec3 treeWindOffset(vec3 localPos, vec3 instPos, float instScale, vec3 worldNormal, uint payload, float dist, float time)
{
    vec2 dir;
    const float speed = vegetationWind(instPos.xz, time, dir);
    const vec2 side = vec2(-dir.y, dir.x);
    const uvec2 cell = floatBitsToUint(instPos.xz);
    const float treeRand = treeWindHash(cell.x * 0x8da6b343u ^ cell.y * 0xd8163841u);
    const float phase = treeRand * TREE_WIND_TAU;

    // TRUNK: lean + sway, x (height / ref height)^2; the drop keeps the length (to first order).
    const float height = max(localPos.y, 0.0) * instScale;
    const float t = height * u_foliage_windInvRefHeight;
    const float lean = u_foliage_windBend * speed * speed;
    const float sway = lean * u_foliage_windSway;
    const float swayAngle = time * TREE_WIND_TAU * u_foliage_windSwayHz * (0.85 + 0.3 * treeRand) + phase;
    const float downwind = (lean + sway * sin(swayAngle)) * t * t;
    const float sideways = sway * 0.4 * sin(swayAngle * 1.37 + 1.3) * t * t;
    vec3 offset = vec3(dir * downwind + side * sideways, 0.0).xzy;
    offset.y = -0.5 * (downwind * downwind + sideways * sideways) / max(height, 1.0);
    if (payload == 0u)
        return offset;

    // BRANCH: the module bends with its own phase, the sub-branch on top with its own (continuous at the joint: a
    // sub-branch's base has weight 0 and the module's weight there).
    const float branchFade = 1.0 - smoothstep(u_foliage_windBranchFadeStart, u_foliage_windBranchFadeEnd, dist);
    if (branchFade > 0.0)
    {
        const float rootW = float(payload & 127u) * (1.0 / 127.0);
        const float boneW = float((payload >> 7) & 63u) * (1.0 / 63.0);
        const float bonePhase = float((payload >> 13) & 15u) * (TREE_WIND_TAU / 16.0);
        const float modulePhase = float((payload >> 17) & 7u) * (TREE_WIND_TAU / 8.0);
        const float branchAngle = time * TREE_WIND_TAU * u_foliage_windBranchHz + phase;
        const float oscModule = sin(branchAngle + modulePhase);
        const float oscBone = sin(branchAngle * 1.7 + bonePhase);
        const float root2 = rootW * rootW;
        const float bone2 = boneW * boneW;
        const float amplitude = u_foliage_windBranch * speed * instScale * branchFade;
        const float push = root2 * (0.6 + 0.4 * oscModule) + bone2 * (0.3 + 0.3 * oscBone);
        const float bob = 0.35 * (root2 * oscModule + bone2 * oscBone);
        const float lateral = 0.3 * root2 * sin(branchAngle * 0.8 + modulePhase + 1.0);
        offset += vec3(dir * push + side * lateral, bob).xzy * amplitude;
    }

    // LEAF: the card's tip along its normal, a phase per card (its stem's payload).
    if ((payload & TREE_WIND_TIP_BIT) != 0u)
    {
        const float leafFade = 1.0 - smoothstep(u_foliage_windLeafFadeStart, u_foliage_windLeafFadeEnd, dist);
        if (leafFade > 0.0)
        {
            const float cardRand = treeWindHash((payload & 0xFFFFFu) ^ (cell.x * 0x27d4eb2du));
            const float flutter = sin(time * TREE_WIND_TAU * u_foliage_windLeafHz * (0.8 + 0.4 * cardRand) + cardRand * TREE_WIND_TAU);
            offset += worldNormal * (u_foliage_windLeaf * flutter * min(speed * 0.2, 1.0) * leafFade);
        }
    }
    return offset;
}

// THE BILLBOARD WAVES (the lit FS, FOLIAGE): a whole-tree billboard cannot bend its branches, so its texture lookup
// moves instead - two crossing waves over the card, growing toward the card's top (the outer crown; on the HORIZONTAL
// card toward its rim) from a still base, x the wind (speed / 8 m/s, at most 1.5) and "Billboard waves" (a fraction of
// the card). The texture coordinate stays inside the card's own strip. Only the whole-tree billboards (the strip code:
// |tangent w| in [2, 3)); the merged branch cards (the axis code) and everything else keep `uv`. The motion vectors do
// not carry it (a texture-space motion).
vec2 treeWindBillboardUv(vec2 uv, float tangentW, vec3 instanceOrigin, vec3 normal)
{
    const float code = abs(tangentW);
    if (u_foliage_windBillboardWaves <= 0.0 || code < 2.0 || code >= 3.0)
        return uv;
    const float dist = distance(instanceOrigin, u_views_viewPos[VIEW_CENTER].xyz);
    const float fade = u_foliage_windTrunkFadeEnd > 0.0 ? 1.0 - smoothstep(u_foliage_windTrunkFadeEnd - 100.0, u_foliage_windTrunkFadeEnd, dist) : 1.0;
    if (fade <= 0.0)
        return uv;
    const float stripV = code - 2.0;
    const float strip = floor(uv.y / stripV);
    const vec2 local = vec2(uv.x, uv.y / stripV - strip); // v: 0 at the card's top, 1 at its bottom
    const bool horizontal = abs(normal.y) > 0.7;
    const float reach = horizontal ? clamp(length(local - 0.5) * 2.0, 0.0, 1.0) : 1.0 - local.y;

    vec2 dir;
    const float speed = vegetationWind(instanceOrigin.xz, u_timeSeconds, dir);
    const uvec2 cell = floatBitsToUint(instanceOrigin.xz);
    const float phase = treeWindHash(cell.x * 0x8da6b343u ^ cell.y * 0xd8163841u) * TREE_WIND_TAU;
    const float t = u_timeSeconds * TREE_WIND_TAU * u_foliage_windBranchHz;
    const vec2 wave = vec2(sin(local.x * 9.0 + local.y * 5.0 - t + phase),
                           0.6 * sin(local.x * 5.0 - local.y * 11.0 - t * 1.37 + phase * 1.7));
    const vec2 offset = wave * (u_foliage_windBillboardWaves * reach * reach * min(speed * 0.125, 1.5) * fade);
    return vec2(clamp(uv.x + offset.x, 0.0, 1.0),
                clamp(uv.y + offset.y * stripV, strip * stripV, (strip + 1.0) * stripV - 1e-4));
}

// The whole wind for a vertex: `outPrevDelta` = last frame's offset minus this frame's (the motion vectors). 0 beyond
// "Trees/Wind/Trunk fade end" (a 100 m band).
vec3 treeWind(vec3 localPos, vec3 instPos, float instScale, vec3 worldNormal, uint payload, out vec3 outPrevDelta)
{
    outPrevDelta = vec3(0.0);
    const float dist = distance(instPos, u_views_viewPos[VIEW_CENTER].xyz);
    const float fade = u_foliage_windTrunkFadeEnd > 0.0 ? 1.0 - smoothstep(u_foliage_windTrunkFadeEnd - 100.0, u_foliage_windTrunkFadeEnd, dist) : 1.0;
    if (fade <= 0.0)
        return vec3(0.0);
    const vec3 now = treeWindOffset(localPos, instPos, instScale, worldNormal, payload, dist, u_timeSeconds) * fade;
    outPrevDelta = treeWindOffset(localPos, instPos, instScale, worldNormal, payload, dist, u_foliage_windPrevTime) * fade - now;
    return now;
}

#endif
