#version 460

#extension GL_EXT_ray_query : require

// The weather volume's RAIN OCCLUSION MAP (RainOcclusionPipeline): one texel = one vertical line through the
// top-down orthographic box of u_weather_rainOcclusionViewProj (its inverse in the push constants), from the near plane (the
// eye, "Rain occlusion pad" above the box) to the far plane (1 m below the box). Two ray queries down it:
//   SOLID   - opaque instances only (CullNoOpaque): the closest hit's ortho depth (t / range, standard Z; 1 = none).
//   FOLIAGE - the alpha-masked (non-opaque) instances above that hit (CullOpaque, candidates never confirmed): the
//             topmost one's depth and the layer count; each layer passes (1 - "Rain occlusion foliage block") of
//             the rain. No texture alpha test: a card counts as one layer wherever the line crosses it.
// ONE uint per texel (RAIN_OCCLUSION_* in particle_sim.cs.glsl decodes it):
//   bits 0..15 solid depth (unorm16), 16..27 top foliage depth (unorm12, 1 = no foliage), 28..31 pass fraction (/15).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform accelerationStructureEXT u_tlas;
layout (binding = 1, r32ui) uniform restrict writeonly uimage2D u_rainOcclusion;

// PC_DECL_ / PC_CONSTS: the lockable values (RainOcclusionPipeline::registerPushFields, PushFields.ixx).
layout (push_constant) uniform PC
{
    mat4 pc_invViewProj;
    uint pc_resolution;
    PC_DECL_layerBlock; // float: the rain one foliage layer stops (0..1)
};
PC_CONSTS

void main()
{
    const uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= pc_resolution || p.y >= pc_resolution)
        return;
    // Texel centre -> NDC as the rasterizer saw it (row 0 = NDC y -1, the sim's uv = ndc * 0.5 + 0.5).
    const vec2 ndc = (vec2(p) + 0.5) / float(pc_resolution) * 2.0 - 1.0;
    const vec4 nearPoint = pc_invViewProj * vec4(ndc, 0.0, 1.0);
    const vec4 farPoint = pc_invViewProj * vec4(ndc, 1.0, 1.0);
    const vec3 origin = nearPoint.xyz / nearPoint.w;
    const vec3 span = farPoint.xyz / farPoint.w - origin;
    const float range = length(span);
    const vec3 dir = span / range;

    rayQueryEXT solid;
    rayQueryInitializeEXT(solid, u_tlas, gl_RayFlagsCullNoOpaqueEXT, 0xFFu, origin, 0.0, dir, range);
    while (rayQueryProceedEXT(solid)) {}
    const float tSolid = rayQueryGetIntersectionTypeEXT(solid, true) == gl_RayQueryCommittedIntersectionTriangleEXT
        ? rayQueryGetIntersectionTEXT(solid, true) : range;

    uint layers = 0u;
    float tTop = range;
    rayQueryEXT foliage;
    rayQueryInitializeEXT(foliage, u_tlas, gl_RayFlagsCullOpaqueEXT, 0xFFu, origin, 0.0, dir, tSolid);
    while (rayQueryProceedEXT(foliage))
    {
        if (rayQueryGetIntersectionTypeEXT(foliage, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            ++layers;
            tTop = min(tTop, rayQueryGetIntersectionTEXT(foliage, false));
        }
    }

    const uint solidBits = uint(round(clamp(tSolid / range, 0.0, 1.0) * 65535.0));
    const uint foliageBits = layers > 0u ? uint(round(clamp(tTop / range, 0.0, 1.0) * 4095.0)) : 4095u;
    const float pass = pow(1.0 - clamp(pc_layerBlock, 0.0, 1.0), float(layers));
    const uint passBits = uint(round(pass * 15.0));
    imageStore(u_rainOcclusion, ivec2(p), uvec4(solidBits | (foliageBits << 16) | (passBits << 28), 0u, 0u, 0u));
}
