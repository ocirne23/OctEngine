#version 460

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : enable

// THE SNOW SITE MAP (SnowSitePipeline; Docs/Context/RendererVK/Snow.md): one texel = one vertical line from pc_topY down
// pc_range, at the centre of its LATTICE cell (world-snapped: the window origin pc_origin + the invocation; stored
// toroidally at lattice & (SNOW_SITE_RES - 1)). Against THIS frame's TLAS with RT_CULL_SNOW (no movers, no bark):
//   SOLID   - the terrain (marched: not in the TLAS), then the opaque instances above it: the top solid surface's world Y
//             (SNOW_SITE_NO_TOP when the line meets nothing);
//   FOLIAGE - the alpha-masked instances above that surface, ALPHA-TESTED (rtsCandidateBlocks): each layer that blocks
//             stops pc_layerBlock of the snow ("Weather/Snow/Foliage block"); the highest one's height over the solid top
//             (/ SNOW_SITE_FOLIAGE_RANGE), so a point above the crown (a branch over the shadow card) is not under it.
// Only the texels a tick needs: outside the last window (scrolled in), all (pc_full), or the tick's row slice or a texel
// marked SNOW_SITE_RETRACE (the invalidation) - those only where the camera's frustum holds the texel: the TLAS holds only
// what was pushed this frame, and an off-screen building (no shadow or RT relevance) is not in it - such a texel keeps the
// last value it was seen with.
// THE CLASS of a traced texel (binding 8, written fresh: the invalidation's bits clear): a committed solid hit is an OBJECT,
// or a ROCK when it is a tree-set piece (custom bit 23: with bark culled and cards / leaves never opaque, only a rock, dead
// wood or a cactus) or a stream instance on the rock pipeline (a preview rock); no hit = the terrain.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform accelerationStructureEXT u_tlas;
layout (binding = 1, r32f) uniform restrict image2D u_siteTop;
layout (binding = 2, rg8) uniform restrict writeonly image2D u_sitePass;

#define UBO_BINDING 9 // the frame UBO: the centre view's frustum
#include "ubo.inc.glsl"

#include "mesh_vertex.inc.glsl"

struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};
struct InMeshInstance
{
    uint renderNodeIdx;
    uint instanceOffsetIdx;
    uint meshIdxMaterialIdx;
    uint pipelineIdxAlphaMode;
};
struct InMeshInfo
{
    vec3 center;
    float radius;
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _padding;
};
// The alpha test's geometry and materials (rt_shadow.inc.glsl): the GI trace's buffers.
layout (binding = 3, std430) readonly buffer InVertices  { MeshVertex in_vertices[]; };
layout (binding = 4, std430) readonly buffer InIndices   { uint in_indices[]; };
layout (binding = 5, std430) readonly buffer InMeshInfos { InMeshInfo in_meshInfos[]; };
layout (binding = 6, std430) readonly buffer InInstances { InMeshInstance in_instances[]; };
layout (binding = 7, std430) readonly buffer InMaterials { MaterialInfo in_materialInfos[]; };
layout (binding = 8, r8ui) uniform restrict uimage2D u_siteClass;
layout (binding = 10) uniform sampler2D u_textures[]; // highest binding: variable descriptor count

// The push block: SnowSitePipeline's SitePush (pc_layerBlock: lockable; pc_terrainRay: no frame UBO here).
layout (buffer_reference) buffer TerrainRayTable;
#include "push.generated.glsl"
#define TERRAIN_RAY_TABLE pc_terrainRay
#include "rt_shadow.inc.glsl"

void main()
{
    const ivec2 rel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(rel, ivec2(SNOW_SITE_RES))))
        return;
    const ivec2 lc = pc_origin + rel;
    const ivec2 prevRel = lc - pc_prevOrigin;
    const bool scrolledIn = any(lessThan(prevRel, ivec2(0))) || any(greaterThanEqual(prevRel, ivec2(SNOW_SITE_RES)));
    const ivec2 slot = lc & ivec2(SNOW_SITE_RES - 1);
    const vec2 xz = (vec2(lc) + 0.5) * SNOW_SITE_TEXEL;
    if (pc_full == 0u && !scrolledIn)
    {
        if ((uint(lc.y) & (SNOW_SITE_SLICES - 1u)) != pc_slice && (imageLoad(u_siteClass, slot).r & SNOW_SITE_RETRACE) == 0u)
            return;
        // The refresh only where this frame pushed every shelter: the frustum, at the column's last top (a sphere of a
        // storey's height around it).
        const float lastTop = imageLoad(u_siteTop, slot).r;
        const vec3 probe = vec3(xz.x, lastTop > SNOW_SITE_NO_TOP * 0.5 ? lastTop : u_views_viewPos[0].y, xz.y); // [0]: the centre view
        for (int i = 0; i < 6; ++i)
            if (dot(vec4(probe, 1.0), u_frustumPlanes[i]) + 4.0 < 0.0)
                return;
    }

    const vec3 origin = vec3(xz.x, pc_topY, xz.y);
    const vec3 dir = vec3(0.0, -1.0, 0.0);

    // The terrain first (not in the TLAS): the solid trace stops at it.
    const float tTerrain = terrainRayMarch(origin, dir, 0.0, pc_range);
    rayQueryEXT solid;
    rayQueryInitializeEXT(solid, u_tlas, gl_RayFlagsCullNoOpaqueEXT, RT_CULL_SNOW, origin, 0.0, dir, tTerrain);
    while (rayQueryProceedEXT(solid)) {}
    const bool hit = rayQueryGetIntersectionTypeEXT(solid, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    const float tSolid = hit ? rayQueryGetIntersectionTEXT(solid, true) : tTerrain;
    uint cls = 0u;
    if (hit)
    {
        const uint custom = rayQueryGetIntersectionInstanceCustomIndexEXT(solid, true);
        const bool rock = (custom & RT_CUSTOM_TREE_BIT) != 0u || (in_instances[custom].pipelineIdxAlphaMode & 0xFFFFu) == PIPELINE_IDX_LIT_ROCK;
        cls = rock ? SNOW_SITE_CLASS_ROCK : SNOW_SITE_CLASS_OBJECT;
    }

    // The foliage above it: every candidate the card's alpha says blocks is one layer (never confirmed: the query goes on),
    // and the highest (the candidates come in no order: the min t).
    uint layers = 0u;
    float tFoliage = tSolid;
    rayQueryEXT foliage;
    rayQueryInitializeEXT(foliage, u_tlas, gl_RayFlagsCullOpaqueEXT, RT_CULL_SNOW, origin, 0.0, dir, tSolid);
    while (rayQueryProceedEXT(foliage))
    {
        if (rayQueryGetIntersectionTypeEXT(foliage, false) == gl_RayQueryCandidateIntersectionTriangleEXT && rtsCandidateBlocks(foliage))
        {
            ++layers;
            tFoliage = min(tFoliage, rayQueryGetIntersectionTEXT(foliage, false));
        }
    }

    const float top = tSolid < pc_range ? origin.y - tSolid : SNOW_SITE_NO_TOP;
    const float pass = layers == 0u ? 1.0 : pow(1.0 - pc_layerBlock, float(layers)); // pow(0, 0) is undefined
    const float foliageOver = layers == 0u ? 0.0 : clamp((tSolid - tFoliage) / SNOW_SITE_FOLIAGE_RANGE, 0.0, 1.0);
    imageStore(u_siteTop, slot, vec4(top, 0.0, 0.0, 0.0));
    imageStore(u_sitePass, slot, vec4(pass, foliageOver, 0.0, 0.0));
    imageStore(u_siteClass, slot, uvec4(cls));
}
