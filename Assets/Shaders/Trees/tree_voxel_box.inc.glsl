// THE VOXEL TIER'S RAY SETUP, shared by its shaders. Requires the UBO and mathutils.inc.glsl (shared.inc.glsl).
// NO BOX IS DRAWN, in either tier: the vertex shader draws ONE SCREEN RECTANGLE around the tree's box (treeVoxelBoxRect:
// instanced_indirect.vs.glsl TREE_VOXEL, tree_voxel_far.vs.glsl), and the fragment shader intersects the pixel's ray with
// the box itself (instanced_indirect_tree_voxel.fs.glsl voxelTreeRay) - 2 triangles a tree and no interpolated entry point.
// NOTHING that is one value per tree is worked out per vertex: the fragment shader takes the box from the tree set's type
// table and the tree's origin, rotation and scale from its record (the near tier) or from two flat values (the far trees).
#ifndef TREE_VOXEL_BOX_INC_GLSL
#define TREE_VOXEL_BOX_INC_GLSL

// THE SCREEN RECTANGLE AROUND A TREE'S BOX: the clip position of the rectangle's corner `corner` (0 / 1 per axis), and
// rayPoint = a world point on that corner's view ray, about as far as the tree (linear over the rectangle: the fragment
// shader takes the pixel's direction from the camera to it, and anchors the hit on it).
// boxMin / boxSize = the box (mesh-local). corners = 8: the box as it stands; 16: also the box moved by bendTop, its TOP's
// trunk bend (world) - the bend grows with the height, so everything of the bent tree lies between the two.
vec4 treeVoxelBoxRect(vec3 boxMin, vec3 boxSize, vec3 instPos, float instScale, vec4 instQuat, vec3 bendTop, uint corners, vec2 corner,
    out vec3 rayPoint)
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
    // The ray is camera-relative and linear in the NDC position (shared.inc.glsl viewRelFromDepth's numerator: the eye
    // cancels, so it keeps its precision far from the world origin).
    const mat4 invM = u_invMvp;
    const vec3 ray = invM[0].xyz * ndc.x + invM[1].xyz * ndc.y + (invM[3].xyz - u_viewPos * invM[3].w);
    rayPoint = u_viewPos + ray * distance(u_viewPos, instPos);
    // At the box's NEAREST depth: the hit the fragment shader writes is never nearer (its depth_less), and early depth
    // rejects a tree the scene hides. w = 1: the ray interpolates linearly over the screen. The TAA jitter as every pass.
    return vec4(ndc + u_taaJitter.xy, min(nearest, 1.0), 1.0);
}

// THE TRUNK'S BEND AS A SHEAR OF THE RAY (the near tier only: a far voxel tree stands still). The tree bends, the grid does not: what stands at the height h
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
