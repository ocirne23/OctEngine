// THE VOXEL BOX'S VERTEX INTERFACE: what instanced_indirect_tree_voxel.fs.glsl needs per tree, worked out once per box
// vertex (8 a tree) and handed over flat. Shared by the two vertex shaders that draw the box (instanced_indirect.vs.glsl
// TREE_VOXEL, tree_voxel_far.vs.glsl). Requires the UBO and mathutils.inc.glsl (shared.inc.glsl).
#ifndef TREE_VOXEL_BOX_INC_GLSL
#define TREE_VOXEL_BOX_INC_GLSL

// THE CAMERA UNDER THE TRUNK'S BEND (the tier that crossfades with the meshes: instanced_indirect.vs.glsl TREE_VOXEL).
// The box bends, the grid in it does not: what stands at the height h in the grid is drawn trunk x (h / ref)^2 to the
// side, as a mesh vertex is. The FS marches a STRAIGHT ray through the grid, so the bend is taken as a SHEAR about the
// height the ray enters at (h, this vertex's): its slope there, exact at h and off by trunk x (dh / ref)^2 a height dh
// away. A shear keeps a ray straight; sheared back, the camera at the height camHeight over the tree's base stands at
// camera - trunk x h (2 camHeight - h) / ref^2. With the camera as it is, a ray that looks DOWN into the crown drew
// everything at its entry height's offset (the mesh's own crown moved less beside it).
// Returns that factor of the trunk vector (TreeWind::trunk, world XZ: a tree turns about up only, so the camera is
// sheared in world space, before its one transform into the tree's axes).
float treeVoxelBoxShear(float localY, float instScale, float camHeight)
{
    const float height = max(localY, 0.0) * instScale;
    return height * (2.0 * camHeight - height) * (u_foliage_windInvRefHeight * u_foliage_windInvRefHeight);
}

// localPos = the box vertex (mesh-local), grid = its grid coordinate (x, z 0 / 1; y in steps where the box has rings up
// its height), boxSize = the box (mesh-local; Procedural billboardBox keeps every side > 0, and a tree's scale is > 0: no
// clamp here). toCam = the camera less the tree's origin (world; sheared by the caller where the bend must match).
// cam       = the camera in grid space
// worldSize = xyz the box in world metres, w = the cube root of its volume (x 1 / a level's voxels = that level's step)
// sunLod    = xyz the sun direction in grid units per metre, no zero component (the self-shadow march divides by it);
//             w = the level-of-detail scale: log2(w x the distance x level 0's voxels per axis) = the level whose voxel
//             is about a pixel, + "Trees/Voxel LOD bias"
void treeVoxelBoxInterface(vec3 localPos, vec3 grid, vec3 boxSize, vec3 toCam, float instScale, vec4 instQuat,
                           out vec3 cam, out vec4 worldSize, out vec4 sunLod)
{
    const vec4 toLocal = vec4(-instQuat.xyz, instQuat.w);
    const vec3 size = boxSize * instScale;
    const vec3 invSize = 1.0 / size; // one reciprocal: the camera and the sun both take it
    const vec3 boxMin = localPos - grid * boxSize;
    cam = (quat_transform(toCam, toLocal) - boxMin * instScale) * invSize;
    const float cubeRoot = pow(size.x * size.y * size.z, 1.0 / 3.0);
    worldSize = vec4(size, cubeRoot);
    const vec3 sun = quat_transform(u_sunDirection.xyz, toLocal) * invSize;
    // A pixel at 1 m spans 2 / u_mipPixelScale metres.
    sunLod = vec4(mix(vec3(1e-6), sun, greaterThan(abs(sun), vec3(1e-6))), 2.0 * exp2(u_foliage_voxelLodBias) / (u_mipPixelScale * cubeRoot));
}

#endif
