#ifndef MESH_LOD_INC_GLSL
#define MESH_LOD_INC_GLSL

// GPU mesh LOD selection, shared by the main and the shadow cull: screen-space error when the chain carries per-level
// simplify errors, the projected diameter otherwise. The instance's stored meshIdx is always level 0; the cull
// redirects. Matches RendererVKLayout::GpuMeshLodGroup.

struct MeshLodGroup
{
    uint numLods;
    uint mesh01;    // level 1 << 16 | level 0
    uint mesh23;    // level 3 << 16 | level 2
    uint mesh4;     // level 4 in the low half
    vec4 errors1_4; // per-level simplify error, mesh-local units (level 0 = 0); all-zero = authored chain
};

uint lodMeshAt(MeshLodGroup group, int level)
{
    uint packed = level < 2 ? group.mesh01 : (level < 4 ? group.mesh23 : group.mesh4);
    return (level & 1) != 0 ? (packed >> 16) : (packed & 0xFFFFu);
}

float lodErrorAt(MeshLodGroup group, int level)
{
    return level == 1 ? group.errors1_4.x : (level == 2 ? group.errors1_4.y : (level == 3 ? group.errors1_4.z : group.errors1_4.w));
}

// Coarsest level whose geometric deviation from LOD0 still projects below threshold pixels.
int lodPickByError(MeshLodGroup group, float errScale, float threshold)
{
    int picked = 0;
    while (picked + 1 < int(group.numLods) && lodErrorAt(group, picked + 1) * errScale <= threshold)
        ++picked;
    return picked;
}

// dist = camera distance to the instance's bounds surface, radius = scaled LOD0 bounds radius, errorThreshold /
// passBias = the pass's coarseness (shadow: x4 / +2 levels). lastLevel < 0 = no hysteresis state: the conservative pick.
int lodSelectLevel(MeshLodGroup group, float dist, float radius, float instanceScale, float errorThreshold, float passBias, int lastLevel)
{
    const float forceLod = u_lod_forceLod;
    if (forceLod >= 0.0)
        return min(int(forceLod), int(group.numLods) - 1);

    const float pixelsPerUnit = u_mipPixelScale / dist; // both callers pass dist >= 0.01
    const float hysteresis = u_lod_hysteresis;
    int level;
    if (group.errors1_4.x > 0.0)
    {
        const float errScale = instanceScale * pixelsPerUnit;
        if (lastLevel < 0)
            return lodPickByError(group, errScale, errorThreshold);

        // Hold the previous level while it sits between the conservative and permissive picks: no flip-flop.
        const int finest = lodPickByError(group, errScale, errorThreshold / (1.0 + hysteresis));
        const int coarsest = lodPickByError(group, errScale, errorThreshold * (1.0 + hysteresis));
        level = clamp(lastLevel, finest, coarsest);
    }
    else
    {
        // No per-level errors (authored LodN_ chains): each halving of the on-screen diameter below fullResPixels drops
        // one level.
        const float projPixels = max(1.0, radius * pixelsPerUnit);
        const float lodF = log2(max(1.0, u_lod_fullResPixels / projPixels)) + u_lod_bias + passBias;
        level = int(lodF);

        if (lastLevel >= 0)
        {
            if (level > lastLevel && lodF < float(lastLevel) + 1.0 + hysteresis)
                level = lastLevel;
            else if (level < lastLevel && lodF > float(lastLevel) - hysteresis)
                level = lastLevel;
        }
        level = clamp(level, 0, int(group.numLods) - 1);
    }
    return level;
}

#endif
