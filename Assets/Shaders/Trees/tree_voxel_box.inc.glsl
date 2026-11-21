// THE VOXEL TIER'S RECTANGLE AND ITS HIT WORD, shared by its shaders. Requires the UBO and mathutils.inc.glsl (shared.inc.glsl).
// NO BOX IS DRAWN, in either tier: the vertex shader draws ONE SCREEN RECTANGLE around the tree's box (treeVoxelBoxRect:
// instanced_indirect.vs.glsl TREE_VOXEL, tree_voxel_far.vs.glsl) and hands on ONE flat word, the tree; the fragment shaders
// build the pixel's ray themselves and intersect it with the box (tree_voxel_ray.inc.glsl) - 2 triangles a tree and no
// interpolated value at all.
#ifndef TREE_VOXEL_BOX_INC_GLSL
#define TREE_VOXEL_BOX_INC_GLSL

// THE HIT WORD (the voxel hit target's first channel, RendererVK SceneColor; its second = the hit's distance from the
// camera, a float's bits). 0 = no voxel tree at this pixel. The vertex shaders hand on the low 24 bits (the tree).
const uint VOXEL_HIT_INDEX_MASK = 0x007FFFFFu; // the near tier: the draw slot (the main cull's instance); the far trees: the record in their sorted list
const uint VOXEL_HIT_FAR        = 0x00800000u; // a far voxel tree (tree_voxel_far.vs.glsl)
const uint VOXEL_HIT_LOD_SHIFT  = 24u;         // 4 bits: the grid level the view march ran at
const uint VOXEL_HIT_AGREE_SHIFT = 28u;        // 3 bits: how far the hit voxel's cover lets its mean normal be trusted (0..7)
const uint VOXEL_HIT_VALID      = 0x80000000u;

// THE SCREEN RECTANGLE AROUND A TREE'S BOX: the clip position of the rectangle's corner `corner` (0 / 1 per axis).
// boxMin / boxSize = the box (mesh-local). corners = 8: the box as it stands; 16: also the box moved by bendTop, its TOP's
// trunk bend (world) - the bend grows with the height, so everything of the bent tree lies between the two.
vec4 treeVoxelBoxRect(vec3 boxMin, vec3 boxSize, vec3 instPos, float instScale, vec4 instQuat, vec3 bendTop, uint corners, vec2 corner)
{
    vec2 ndcMin = vec2(1e30);
    vec2 ndcMax = vec2(-1e30);
    float nearest = 0.0; // reversed-Z: the largest
    bool behind = false;
    for (uint i = 0u; i < corners; ++i)
    {
        const vec3 at = vec3(float(i & 1u), float((i >> 1) & 1u), float((i >> 2) & 1u));
        vec3 world = quat_transform((boxMin + at * boxSize) * instScale, instQuat) + instPos;
        if (i >= 8u)
            world += bendTop;
        const vec4 clip = u_mvp * vec4(world, 1.0);
        behind = behind || clip.w < 1e-3;
        const vec3 ndc = clip.xyz / max(clip.w, 1e-3);
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        nearest = max(nearest, ndc.z);
    }
    // A corner behind the camera (the camera stands in or beside the box: "Trees/Force far"): the whole screen.
    if (behind)
    {
        ndcMin = vec2(-1.0);
        ndcMax = vec2(1.0);
    }
    const vec2 ndc = mix(ndcMin, ndcMax, corner);
    // At the box's NEAREST depth: the hit the fragment shader writes is never nearer (its depth_less), and early depth
    // rejects a tree the scene hides. The TAA jitter as every pass.
    return vec4(ndc + u_taaJitter.xy, min(nearest, 1.0), 1.0);
}

// THE TRUNK'S BEND AS A SHEAR OF THE RAY (the near tier, and the voxel shadow caster's one shear per tree; a far voxel
// tree stands still). The tree bends, the grid does not: what stands at the height h
// in the grid is drawn trunk x (h / ref)^2 to the side, as a mesh vertex is. The march is a STRAIGHT ray through the grid,
// so the bend is taken as a SHEAR about the height the ray enters at (h): its slope there, exact at h and off by
// trunk x (dh / ref)^2 a height dh away. A shear keeps a ray straight; sheared back, a point at the height y over the
// tree's base stands at point - trunk x h (2 y - h) / ref^2, and a direction d becomes d - trunk x (2 h / ref^2) x d.y.
// With the ray as it is, one that looks DOWN into the crown drew everything at its entry height's offset (the mesh's own
// crown moved less beside it). The trunk vector is world XZ (TreeWind::trunk): a tree turns about up only, so the ray is
// sheared in world space, before its one transform into the tree's axes.
// height = the entry point's height over the tree's base (world metres, >= 0).
// x = the factor of the trunk vector for a POINT at the height pointHeight, y = for a DIRECTION, per unit of its y.
vec2 treeVoxelBoxShear(float height, float pointHeight)
{
    const float k = u_foliage_windInvRefHeight * u_foliage_windInvRefHeight;
    return vec2(height * (2.0 * pointHeight - height), 2.0 * height) * k;
}

#endif
