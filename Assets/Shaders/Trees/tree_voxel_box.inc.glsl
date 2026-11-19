// THE VOXEL BOX'S VERTEX INTERFACE: what instanced_indirect_tree_voxel.fs.glsl needs per tree, worked out once per box
// vertex (8 a tree) and handed over flat. Shared by the two vertex shaders that draw the box (instanced_indirect.vs.glsl
// TREE_VOXEL, tree_voxel_far.vs.glsl). Requires the UBO and mathutils.inc.glsl (shared.inc.glsl).
#ifndef TREE_VOXEL_BOX_INC_GLSL
#define TREE_VOXEL_BOX_INC_GLSL

// localPos = the box corner (mesh-local), grid = its grid coordinate (0 / 1 per axis), boxSize = the box (mesh-local;
// Procedural billboardBox keeps every side > 0, and a tree's scale is > 0: no clamp here).
// cam       = the camera in grid space (unbent by the wind)
// worldSize = xyz the box in world metres, w = the cube root of its volume (x 1 / a level's voxels = that level's step)
// sunLod    = xyz the sun direction in grid units per metre, no zero component (the self-shadow march divides by it);
//             w = the level-of-detail scale: log2(w x the distance x level 0's voxels per axis) = the level whose voxel
//             is about a pixel, + "Trees/Voxel LOD bias"
void treeVoxelBoxInterface(vec3 localPos, vec3 grid, vec3 boxSize, vec3 instPos, float instScale, vec4 instQuat,
                           out vec3 cam, out vec4 worldSize, out vec4 sunLod)
{
    const vec4 toLocal = vec4(-instQuat.xyz, instQuat.w);
    const vec3 boxMin = localPos - grid * boxSize;
    cam = (quat_transform(u_viewPos - instPos, toLocal) / instScale - boxMin) / boxSize;
    const vec3 size = boxSize * instScale;
    const float cubeRoot = pow(size.x * size.y * size.z, 1.0 / 3.0);
    worldSize = vec4(size, cubeRoot);
    const vec3 sun = quat_transform(u_sunDirection.xyz, toLocal) / size;
    // A pixel at 1 m spans 2 / u_mipPixelScale metres.
    sunLod = vec4(mix(vec3(1e-6), sun, greaterThan(abs(sun), vec3(1e-6))), 2.0 * exp2(u_foliage_voxelLodBias) / (u_mipPixelScale * cubeRoot));
}

#endif
