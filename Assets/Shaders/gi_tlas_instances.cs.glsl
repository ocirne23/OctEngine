#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable

#include "shared.inc.glsl"

// Builds the TLAS instance array on the GPU, one VkAccelerationStructureInstanceKHR per mesh instance.
// Reuses the exact world-transform composition (renderNode * instanceOffset) from instanced_indirect.cs,
// so ray-traced geometry matches what the raster path draws. No frustum culling: GI needs off-screen
// geometry too.

struct RenderNodeTransform { vec4 posScale; vec4 quat; };
struct InMeshInstance      { uint renderNodeIdx; uint instanceOffsetIdx; uint meshIdxMaterialIdx; uint pipelineIdxAlphaMode; };
struct InMeshInstanceOffset{ vec4 posScale; vec4 quat; };
struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};

// Matches VkAccelerationStructureInstanceKHR (64 bytes): a row-major 3x4 transform, packed
// custom-index/mask and sbt-offset/flags words, then the 64-bit BLAS reference.
struct TlasInstance
{
    vec4 row0;
    vec4 row1;
    vec4 row2;
    uint instanceCustomIndexAndMask;
    uint sbtOffsetAndFlags;
    uint blasLo;
    uint blasHi;
};

layout (binding = 0, std430) readonly buffer InRenderNodeTransformsBuffer { RenderNodeTransform in_renderNodeTransforms[]; };
layout (binding = 1, std430) readonly buffer InMeshInstancesBuffer        { InMeshInstance in_instances[]; };
layout (binding = 2, std430) readonly buffer InMeshInstanceOffsetsBuffer  { InMeshInstanceOffset in_instanceOffsets[]; };
layout (binding = 3, std430) readonly buffer InBlasAddressesBuffer        { uvec2 in_blasAddresses[]; }; // uint64 split lo/hi per mesh
layout (binding = 4, std430) writeonly buffer OutTlasInstancesBuffer      { TlasInstance out_instances[]; };
layout (binding = 5, std430) readonly buffer InMaterialInfosBuffer        { MaterialInfo in_materialInfos[]; };
layout (binding = 6, std430) readonly buffer InNodePassMasksBuffer        { uint in_nodePassMasks[]; };
// mesh idx -> the mesh whose BLAS this one shares (identity when it owns its own): a LOD chain keeps a
// single BLAS at its RT level, so instances of every level reference that BLAS and carry its meshIdx
// (packed into sbtOffset below) for the hit shaders' attribute fetches.
layout (binding = 7, std430) readonly buffer InRtMeshAliasBuffer          { uint in_rtMeshAlias[]; };

layout (push_constant) uniform PushConstants
{
    vec3 viewPos;
    float maxRange; // instances whose origin is further out get mask 0 (bounds the TLAS build)
    uint numInstances;
} pc;

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
vec4 quat_multiply(vec4 q, vec4 p)
{
    vec4 c, r;
    c.xyz = cross(q.xyz, p.xyz);
    c.w = -dot(q.xyz, p.xyz);
    r = p * q.w + c;
    r.xyz = (q * p.w + r).xyz;
    return r;
}

layout(local_size_x = 64) in;

void main()
{
    const uint id = gl_GlobalInvocationID.x;
    if (id >= pc.numInstances)
        return;

    const InMeshInstance inst = in_instances[id];
    const uint meshIdx = inst.meshIdxMaterialIdx & 0x0000FFFFu;

    const vec4 quat   = quat_multiply(in_renderNodeTransforms[inst.renderNodeIdx].quat, in_instanceOffsets[inst.instanceOffsetIdx].quat);
    const vec4 rnPS   = in_renderNodeTransforms[inst.renderNodeIdx].posScale;
    const vec4 ioPS   = in_instanceOffsets[inst.instanceOffsetIdx].posScale;
    const vec3 pos    = rnPS.xyz + quat_transform(ioPS.xyz * rnPS.w, in_renderNodeTransforms[inst.renderNodeIdx].quat);
    const float scale = rnPS.w * ioPS.w;

    // Rotation matrix columns (object basis vectors rotated into world), scaled uniformly.
    const vec3 col0 = quat_transform(vec3(1.0, 0.0, 0.0), quat);
    const vec3 col1 = quat_transform(vec3(0.0, 1.0, 0.0), quat);
    const vec3 col2 = quat_transform(vec3(0.0, 0.0, 1.0), quat);

    TlasInstance o;
    o.row0 = vec4(scale * col0.x, scale * col1.x, scale * col2.x, pos.x);
    o.row1 = vec4(scale * col0.y, scale * col1.y, scale * col2.y, pos.y);
    o.row2 = vec4(scale * col0.z, scale * col1.z, scale * col2.z, pos.z);
    // The RT mesh: whose BLAS geometry this instance traces against (the chain's shared level).
    const uint rtMeshIdx = meshIdx < uint(in_rtMeshAlias.length()) ? in_rtMeshAlias[meshIdx] : meshIdx;

    // Flags: TriangleFacingCullDisable (0x01) always; ForceOpaque (0x04) for everything except
    // alpha-masked instances, which stay non-opaque so shadow rays can run their alpha test
    // (rt_shadow.inc.glsl). Opaque-flagged rays (RTAO, GI gather) still treat masked geometry as solid.
    // The sbtOffset bits (unused by ray queries) carry the RT meshIdx so hit shaders fetch the indices
    // matching the BLAS geometry instead of the raster-selected LOD level's.
    const uint alphaMode = inst.pipelineIdxAlphaMode >> 16;
    const uint instFlags = 0x01u | (alphaMode == ALPHA_MODE_MASK ? 0x00u : 0x04u);
    o.sbtOffsetAndFlags          = (instFlags << 24) | (rtMeshIdx & 0x00FFFFFFu);

    // Guard the address lookup: meshIdx can be up to 65535, but the BLAS address buffer only has
    // length() entries. An out-of-bounds read returns a foreign 64-bit value that, used as a BLAS
    // reference, makes ray traversal chase a wild pointer -> MMU fault. Clamp to a null reference instead.
    const bool meshInRange = rtMeshIdx < uint(in_blasAddresses.length());
    const uvec2 addr = meshInRange ? in_blasAddresses[rtMeshIdx] : uvec2(0u);
    o.blasLo = addr.x;
    o.blasHi = addr.y;

    // Instances that must not be traced become INACTIVE (acceleration structure reference 0, below): the
    // TLAS build skips an inactive instance entirely, so it costs nothing in the build or in traversal,
    // where a mask-0 instance would still be a node in the tree. Excluded is any instance that cannot
    // be safely traversed:
    //  - no real BLAS (zeroed/out-of-range address) -> would chase a null/garbage pointer, and
    //  - a non-finite transform (NaN/Inf from an out-of-range renderNode/instanceOffset index) -> makes the
    //    driver's TLAS bounds garbage, which also MMU-faults traversal on NVIDIA.
    const bool hasBlas    = (addr.x != 0u || addr.y != 0u);
    const bool finiteXform = !any(isnan(o.row0)) && !any(isinf(o.row0))
                          && !any(isnan(o.row1)) && !any(isinf(o.row1))
                          && !any(isnan(o.row2)) && !any(isinf(o.row2));
    // Mask 0 also excludes debug/gizmo geometry flagged NO_RAYTRACING on its material (rasterized only,
    // never blocks shadow rays / GI / AO).
    const uint materialIdx = inst.meshIdxMaterialIdx >> 16;
    const bool noRT = materialIdx < uint(in_materialInfos.length())
                   && (in_materialInfos[materialIdx].flags & MATERIAL_FLAG_NO_RAYTRACING) != 0u;
    // GI or shadow relevance keeps the instance hittable: probes/RTAO trace the GI set, and the
    // rt-sun-shadow mode needs shadow-relevant casters present too.
    const bool inRtSet = (in_nodePassMasks[inst.renderNodeIdx] & (PASS_GI | PASS_SHADOW)) != 0u;
    // Range bound: rays never reach past the GI clipmap + max ray distance, so distant geometry
    // only bloats the TLAS build (origin-distance test: cheap, conservative via the RT/GI tweak).
    const bool inRange = distance(pos, pc.viewPos) <= pc.maxRange;
    const bool traceable = hasBlas && finiteXform && !noRT && inRtSet && inRange;
    if (!traceable)
    {
        o.blasLo = 0u; // reference 0 = inactive: not built, not traversed
        o.blasHi = 0u;
    }
    o.instanceCustomIndexAndMask = (id & 0x00FFFFFFu) | (0xFFu << 24);      // custom = instance idx

    out_instances[id] = o;
}
