#ifndef TREE_WIND_INC_GLSL
#define TREE_WIND_INC_GLSL

#include "wind.inc.glsl"

// TREE WIND: a vertex-shader sway of the procedural trees in the shared VEGETATION wind (wind.inc.glsl, the grass's
// too), tuned by u_foliage_wind* ("Trees/Wind"). No textures. What is the same for a whole tree (the gusts at its
// origin, the trunk's sway, the fades) is a TreeWind: once per tree in the main cull for a tree record, per vertex only
// for a caller without one. Three layers:
//   TRUNK  - the whole tree leans downwind and sways, the offset growing with (height / ref height)^2 and the tree
//            dropping slightly so it keeps its length. From the mesh-local height alone, so the mesh, the voxel box and
//            the shadow billboard bend alike (the crossfade and the shadow stay aligned).
//   BRANCH - meshes: per-vertex weights + phases from the bake (Procedural TreeGenerator), each branch bending as one
//            unit; faded out before the voxel tier ("Branch fade").
//   LEAF   - leaf cards only: the card's tip flutters along its normal; faded out sooner ("Leaf fade").
//
// THE PAYLOAD rides the tangent's w MAGNITUDE (RenderMeshData: texCoords.z), its sign staying the handedness:
//   |w| = 1          an ordinary mesh: no wind
//   |w| in (1, 2)    a tree mesh vertex: 1 + payload / 2^22, payload =
//                      bits 0..6   the module weight (0 at the module's root .. 127 at its tip, along the root bone)
//                      bits 7..12  the sub-branch weight (0 .. 63 along its level-1 bone)
//                      bits 13..16 the sub-branch phase, bits 17..19 the module placement's phase
//                      bit 20      a leaf card's TIP vertex (flutters), bit 21 = TREE_WIND_BIT (always set)
//   |w| >= 2         the voxel box (2.5, Procedural treeVoxelBoxMesh): the trunk bend only
// Requires the UBO and mathutils.inc.glsl (shared.inc.glsl).

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
    payload = uint((m - 1.0) * 4194304.0 + 0.5); // x 2^22
    return (payload & TREE_WIND_BIT) != 0u;
}

float treeWindHash(uint h) { return float(hashU(h)) * (1.0 / 4294967296.0); }

// The tree's random value in [0, 1), from its world position's bits.
float treeWindTreeRand(vec2 instXZ)
{
    const uvec2 cell = floatBitsToUint(instXZ);
    return treeWindHash(cell.x * 0x8da6b343u ^ cell.y * 0xd8163841u);
}

// 1 up to 100 m before "Trees/Wind/Trunk fade end" (>= 100 m on the CPU), 0 past it.
float treeWindTrunkFade(float dist)
{
    return 1.0 - smoothstep(u_foliage_windTrunkFadeEnd - 100.0, u_foliage_windTrunkFadeEnd, dist);
}

// A TREE'S WIND: everything that is the same for all of its vertices, at this frame's time and as the DIFFERENCE to
// last frame's (the motion vectors: a difference keeps its precision in the record's half floats). The main cull works
// it out once per tree and writes it into the tree's instance record (treeWindPack); a vertex then only scales it by
// its height and adds its branch's own sway.
struct TreeWind
{
    vec2 trunk;       // the trunk's offset at the reference height (m, XZ): the lean + the sway, x the species' scale x the trunk fade
    vec2 trunkDelta;  // last frame's minus this frame's
    vec2 wind;        // the wind vector at the tree (m/s, XZ)
    vec2 windDelta;   // last frame's minus this frame's
    float branchAmp;  // a branch tip's sway, m per m/s: "Branch sway" x the tree's scale x the species' x the trunk fade
    float branchFade; // the branches' fade with the distance ("Branch fade")
    float phase;      // the tree's own phase (rad)
};

// The trunk's offset at the reference height for a wind vector: the lean downwind (bend x speed^2) + the sway about it
// (x "Sway", a slower second wave across the wind). The direction x the speed is the wind vector itself.
vec2 treeWindTrunk(vec2 wind, float time, float treeRand)
{
    const float swayAngle = time * TREE_WIND_TAU * u_foliage_windSwayHz * (0.85 + 0.3 * treeRand) + treeRand * TREE_WIND_TAU; // + the tree's phase
    return (wind * (1.0 + u_foliage_windSway * sin(swayAngle)) + vec2(-wind.y, wind.x) * (u_foliage_windSway * 0.4 * sin(swayAngle * 1.37 + 1.3)))
         * (u_foliage_windBend * length(wind));
}

// instPos / instScale = the tree's origin and scale, scale = the species' `Wind` (x the trunk lean + sway, y the branch
// sway; the leaf flutter keeps its own). The trunk and the branch amplitude are zero beyond "Trunk fade end".
// The gusts FIRST, and no early return for a tree beyond the fade: the distance and the fade held across the gusts were
// two more registers in the main cull (and the trees that reach it stand inside the fade's range).
TreeWind treeWindOfTree(vec3 instPos, float instScale, vec2 scale)
{
    TreeWind tree;
    tree.wind = vegetationWindVector(instPos.xz, u_timeSeconds);
    const vec2 prevWind = vegetationWindVector(instPos.xz, u_foliage_windPrevTime);
    tree.windDelta = prevWind - tree.wind;
    const float dist = distance(instPos, u_views_viewPos[VIEW_CENTER].xyz);
    const float fade = treeWindTrunkFade(dist);
    const float treeRand = treeWindTreeRand(instPos.xz);
    tree.trunk = treeWindTrunk(tree.wind, u_timeSeconds, treeRand) * (scale.x * fade);
    tree.trunkDelta = treeWindTrunk(prevWind, u_foliage_windPrevTime, treeRand) * (scale.x * fade) - tree.trunk;
    tree.branchAmp = u_foliage_windBranch * instScale * scale.y * fade;
    tree.branchFade = 1.0 - smoothstep(u_foliage_windBranchFadeStart, u_foliage_windBranchFadeEnd, dist);
    tree.phase = treeRand * TREE_WIND_TAU;
    return tree;
}

// A TREE RECORD'S WIND (the main cull's OutMeshInstance, instanced_indirect.cs.glsl): a tree never moves and is never
// skinned, so the three "last frame" fields of its record are free, and carry
//   prevPosScale    xy = trunk, z = branchAmp, w = -(1 + branchFade + 2 x the phase's step of 1024): <= -1 marks a tree
//                   record (0 = a node that did not move, > 0 = last frame's scale)
//   prevQuat        the wind and its difference, two halves each
//   prevVertexDelta the trunk's difference, two halves
#define TREE_WIND_PHASE_STEPS 1024.0
void treeWindPack(TreeWind tree, out vec4 prevPosScale, out uvec2 prevQuat, out uint prevVertexDelta)
{
    const float phaseStep = min(floor(tree.phase * (TREE_WIND_PHASE_STEPS / TREE_WIND_TAU)), TREE_WIND_PHASE_STEPS - 1.0);
    prevPosScale = vec4(tree.trunk, tree.branchAmp, -(1.0 + tree.branchFade + 2.0 * phaseStep));
    prevQuat = uvec2(packHalf2x16(tree.wind), packHalf2x16(tree.windDelta));
    prevVertexDelta = packHalf2x16(tree.trunkDelta);
}

bool treeWindIsRecord(vec4 prevPosScale) { return prevPosScale.w <= -0.5; }

TreeWind treeWindUnpack(vec4 prevPosScale, uvec2 prevQuat, uint prevVertexDelta)
{
    const float code = -1.0 - prevPosScale.w; // branchFade (0..1) + 2 x the phase's step
    const float phaseStep = floor(code * 0.5);
    return TreeWind(prevPosScale.xy, unpackHalf2x16(prevVertexDelta), unpackHalf2x16(prevQuat.x), unpackHalf2x16(prevQuat.y),
                    prevPosScale.z, code - 2.0 * phaseStep, phaseStep * (TREE_WIND_TAU / TREE_WIND_PHASE_STEPS));
}

// A branch's offset for a wind vector, per m of amplitude: the module bends with its own phase, the sub-branch on top
// with its own (continuous at the joint: a sub-branch's base has weight 0 and the module's weight there). root2 / bone2
// = the squared weights along the module and its sub-branch. Downwind and across in XZ; the bob takes the wind's speed.
vec3 treeWindBranch(vec2 wind, float angle, float root2, float bone2, float modulePhase, float bonePhase)
{
    const float oscModule = sin(angle + modulePhase);
    const float oscBone = sin(angle * 1.7 + bonePhase);
    const float push = root2 * (0.6 + 0.4 * oscModule) + bone2 * (0.3 + 0.3 * oscBone);
    const float bob = 0.35 * (root2 * oscModule + bone2 * oscBone);
    const float lateral = 0.3 * root2 * sin(angle * 0.8 + modulePhase + 1.0);
    return vec3(wind * push + vec2(-wind.y, wind.x) * lateral, length(wind) * bob).xzy;
}

// A vertex's share of the trunk's bend, from its height (localY = mesh-local, +Y up from the tree's base): x = (height /
// ref height)^2, x the tree's trunk vector = its XZ offset; y x the vector's squared length = its drop, which keeps the
// trunk's length (to first order).
vec2 treeWindTrunkShare(float localY, float instScale)
{
    const float height = max(localY, 0.0) * instScale;
    const float t = height * u_foliage_windInvRefHeight;
    const float t2 = t * t;
    return vec2(t2, -0.5 * t2 * t2 / max(height, 1.0));
}

// THE TRUNK'S BEND ALONE, this frame's: the shadow casters (no payload attribute in that pass, no motion vectors).
// trunkScale = the species' trunk scale. The same bend treeWindOfTree + treeWindVertex give a payload-0 vertex.
vec3 treeWindTrunkOffset(vec3 localPos, vec3 instPos, float instScale, float trunkScale)
{
    const float fade = treeWindTrunkFade(distance(instPos, u_views_viewPos[VIEW_CENTER].xyz));
    if (fade <= 0.0)
        return vec3(0.0);
    const vec2 wind = vegetationWindVector(instPos.xz, u_timeSeconds);
    const float treeRand = treeWindTreeRand(instPos.xz);
    const vec2 trunk = treeWindTrunk(wind, u_timeSeconds, treeRand) * (trunkScale * fade);
    const vec2 share = treeWindTrunkShare(localPos.y, instScale);
    return vec3(trunk * share.x, share.y * dot(trunk, trunk)).xzy;
}

// The world-space offset of a tree vertex this frame; outPrevDelta = last frame's offset minus it (the motion vectors).
// localPos = the mesh-local position, worldNormal = the vertex normal in world space (the leaf flutter).
vec3 treeWindVertex(TreeWind tree, vec3 localPos, vec3 instPos, float instScale, vec3 worldNormal, uint payload, out vec3 outPrevDelta)
{
    // TRUNK: the tree's vector x the vertex's share.
    const vec2 share = treeWindTrunkShare(localPos.y, instScale);
    const vec2 prevTrunk = tree.trunk + tree.trunkDelta;
    const float trunk2 = dot(tree.trunk, tree.trunk);
    vec3 offset = vec3(tree.trunk * share.x, share.y * trunk2).xzy;
    outPrevDelta = vec3(tree.trunkDelta * share.x, share.y * (dot(prevTrunk, prevTrunk) - trunk2)).xzy;
    if (payload == 0u)
        return offset;

    // BRANCH (treeWindBranch), this frame's and last frame's.
    const float amplitude = tree.branchAmp * tree.branchFade;
    const vec2 prevWind = tree.wind + tree.windDelta;
    if (amplitude > 0.0)
    {
        const float rootW = float(payload & 127u) * (1.0 / 127.0);
        const float boneW = float((payload >> 7) & 63u) * (1.0 / 63.0);
        const float bonePhase = float((payload >> 13) & 15u) * (TREE_WIND_TAU / 16.0);
        const float modulePhase = float((payload >> 17) & 7u) * (TREE_WIND_TAU / 8.0);
        const float root2 = rootW * rootW;
        const float bone2 = boneW * boneW;
        const float rate = TREE_WIND_TAU * u_foliage_windBranchHz;
        const vec3 now = treeWindBranch(tree.wind, u_timeSeconds * rate + tree.phase, root2, bone2, modulePhase, bonePhase) * amplitude;
        offset += now;
        outPrevDelta += treeWindBranch(prevWind, u_foliage_windPrevTime * rate + tree.phase, root2, bone2, modulePhase, bonePhase) * amplitude - now;
    }

    // LEAF: the card's tip along its normal, a phase per card (its stem's payload). TREE_WIND_NO_FLUTTER: an includer
    // whose meshes carry no TIP vertex (the voxel box) compiles without it.
#ifndef TREE_WIND_NO_FLUTTER
    if ((payload & TREE_WIND_TIP_BIT) != 0u)
    {
        const float dist = distance(instPos, u_views_viewPos[VIEW_CENTER].xyz);
        float leafFade = 1.0 - smoothstep(u_foliage_windLeafFadeStart, u_foliage_windLeafFadeEnd, dist);
        // The trunk fade too, only where its band can reach into the flutter's range (constants while their rows are locked).
        if (u_foliage_windTrunkFadeEnd - 100.0 < u_foliage_windLeafFadeEnd)
            leafFade *= treeWindTrunkFade(dist);
        if (leafFade > 0.0)
        {
            const float cardRand = treeWindHash((payload & 0xFFFFFu) ^ (floatBitsToUint(instPos.x) * 0x27d4eb2du));
            const float rate = TREE_WIND_TAU * u_foliage_windLeafHz * (0.8 + 0.4 * cardRand);
            const float cardPhase = cardRand * TREE_WIND_TAU;
            const float now = sin(u_timeSeconds * rate + cardPhase) * min(length(tree.wind) * 0.2, 1.0);
            const float prev = sin(u_foliage_windPrevTime * rate + cardPhase) * min(length(prevWind) * 0.2, 1.0);
            offset += worldNormal * (u_foliage_windLeaf * leafFade * now);
            outPrevDelta += worldNormal * (u_foliage_windLeaf * leafFade * (prev - now));
        }
    }
#endif
    return offset;
}

// The whole wind for a vertex of a caller WITHOUT a record (the far voxel trees: a few vertices a tree): the tree's wind
// worked out here, per vertex.
vec3 treeWind(vec3 localPos, vec3 instPos, float instScale, vec3 worldNormal, uint payload, vec2 scale, out vec3 outPrevDelta)
{
    return treeWindVertex(treeWindOfTree(instPos, instScale, scale), localPos, instPos, instScale, worldNormal, payload, outPrevDelta);
}

#endif
