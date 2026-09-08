#version 460

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_gbufferNormal; // world-space normal (xyz)
layout (binding = 2) uniform sampler2D u_gbufferDepth;  // hardware depth
layout (binding = 3) uniform accelerationStructureEXT u_tlas;
layout (binding = 4, rgba16f) uniform restrict writeonly image2D u_aoOut; // rgb = bent normal, a = AO

// Alpha-masked rays are a compile-time variant (RTAO_ALPHA_TEST injected by the pipeline when the
// "RTAO/Alpha Test" tweak is on); the opaque path compiles out the geometry fetch + alpha test entirely.
#ifdef RTAO_ALPHA_TEST
struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
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
struct InMeshInstance
{
    uint renderNodeIdx;
    uint instanceOffsetIdx;
    uint meshIdxMaterialIdx;
    uint pipelineIdxAlphaMode;
};
// Geometry for the alpha-masked candidate test. Same buffers/layout the GI trace uses; rt_shadow.inc.glsl
// supplies the alpha test against them.
layout (binding = 5, std430) readonly buffer InVertices  { float in_vertices[]; }; // MeshVertex as 12 floats
layout (binding = 6, std430) readonly buffer InIndices   { uint in_indices[]; };
layout (binding = 7, std430) readonly buffer InMeshInfos { InMeshInfo in_meshInfos[]; };
layout (binding = 8, std430) readonly buffer InInstances { InMeshInstance in_instances[]; };
layout (binding = 9, std430) readonly buffer InMaterials { MaterialInfo in_materialInfos[]; };
layout (binding = 10) uniform sampler2D u_textures[]; // highest binding: variable descriptor count

#include "rt_shadow.inc.glsl"
#endif

layout (push_constant) uniform PC
{
    uint  numRays;
    float radius;     // world units
    float power;      // contrast curve
    float intensity;  // 0 = off, 1 = full
    uint  aoWidth;
    uint  aoHeight;
    uint  viewIndex;    // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
    float fadeStart;    // distance from camera where AO begins to fade out
    float maxDistance;  // distance at which AO is fully gone; 0 disables the falloff
    float normalBias;   // constant ray-origin offset along the surface normal (m)
    float distanceBias; // ray-origin offset toward the camera, per meter of view distance
} pc;

// Radical-inverse base-2 -> 2D Hammersley point set.
vec2 hammersley(uint i, uint n)
{
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
}

float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.aoWidth) || px.y >= int(pc.aoHeight))
        return;

    const vec2 uv = (vec2(px) + 0.5) / vec2(pc.aoWidth, pc.aoHeight);

    const float depth = texture(u_gbufferDepth, uv).r;
    if (depth <= 0.0) { imageStore(u_aoOut, px, vec4(0.0, 0.0, 1.0, 1.0)); return; } // background (reversed-Z far = 0): no occlusion

    const vec3 nRaw = texture(u_gbufferNormal, uv).xyz;
    const float nLen = length(nRaw);
    if (nLen < 0.5) { imageStore(u_aoOut, px, vec4(0.0, 0.0, 1.0, 1.0)); return; }
    const vec3 N = nRaw / nLen;

    // The depth image is jittered (exact depth-prepass reuse); reconstruct at the surface's true
    // unjittered position so ray origins don't wobble sub-pixel with the jitter — see taaJitterUv.
    const vec3 worldPos = worldPosFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth);
    const float viewDist = length(u_viewPos - worldPos);          // geometric: the ray-origin bias below
    const float focusDist = length(u_sceneFocus.xyz - worldPos);  // quality falloff: from the scene focus (the game's player)

    // Past the AO max distance the result is always "no occlusion", so skip the ray loop entirely.
    // (bent normal = surface normal so the forward pass evaluates GI along the surface.) Measured from
    // the scene focus, like the fade below, so a top-down camera far above the ground does not fade
    // the AO out under the player.
    if (pc.maxDistance > 0.0 && focusDist >= pc.maxDistance)
    {
        imageStore(u_aoOut, px, vec4(N, 1.0));
        return;
    }

    // Ray-origin self-intersection bias. worldPos reconstructs from half-res depth, whose error lies
    // ALONG THE VIEW RAY and grows with distance — a fixed offset left distant slope pixels starting
    // under the RT surface (dark banding on sloped terrain). Compensate along V, toward the camera: it
    // cancels the depth error 1:1 and lifts off the surface plane by only offset*NoV, so grazing walls
    // are NOT pushed away from their own contact detail (a 1/NoV-scaled NORMAL offset did exactly that
    // and artifacted flat walls). The small constant normal offset handles triangle self-intersection.
    const vec3 V = (u_viewPos - worldPos) / max(viewDist, 1e-4);
    const vec3 rayOrigin = worldPos + N * pc.normalBias + V * min(viewDist * pc.distanceBias, pc.radius * 0.5);

    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    // Per-pixel + per-frame decorrelation (Cranley-Patterson). The frame term rotates the set so the
    // temporal pass (stage 3) accumulates fresh samples each frame.
    // Frame counter comes from the UBO (not the push constant) so this pass can be recorded once and still
    // rotate its sampling each frame (the push-constant frameIndex is left unused / stale).
    const float frameRot = float(u_frameIndex);
    vec2 jitter = vec2(ign(vec2(px) + frameRot * 1.6180339),
                       ign(vec2(px.yx) + frameRot * 3.1415926 + vec2(17.3, 5.1)));

    const uint n = max(pc.numRays, 1u);
    float occ = 0.0;
    vec3 bent = vec3(0.0); // sum of unoccluded ray directions -> bent normal (average open direction)
    for (uint i = 0u; i < n; ++i)
    {
        vec2 u = fract(hammersley(i, n) + jitter);
        float r = sqrt(u.x);                       // cosine-weighted hemisphere
        float phi = 6.2831853 * u.y;
        vec3 dir = T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u.x));

        rayQueryEXT rq;
#ifdef RTAO_ALPHA_TEST
        // Masked geometry is non-opaque in the TLAS; run the alpha test on candidates, hardware still
        // auto-commits opaque hits. Terminate-on-first-hit gives the nearest confirmed hit for falloff.
        rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, rayOrigin, 0.01, dir, pc.radius);
        while (rayQueryProceedEXT(rq))
        {
            if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
                && rtsCandidateBlocks(rq))
                rayQueryConfirmIntersectionEXT(rq);
        }
#else
        rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFFu, rayOrigin, 0.01, dir, pc.radius);
        while (rayQueryProceedEXT(rq)) {}
#endif
        if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
        {
            float t = rayQueryGetIntersectionTEXT(rq, true);
            occ += 1.0 - clamp(t / pc.radius, 0.0, 1.0); // closer hits darken more
        }
        else
        {
            bent += dir; // ray escaped: this direction is open
        }
    }

    float ao = clamp(1.0 - (occ / float(n)) * pc.intensity, 0.0, 1.0);
    ao = pow(ao, pc.power);
    // Distance falloff: far-away surfaces get noisy/low-quality AO (radius is fixed in world units, so it
    // shrinks in screen space with distance), so fade the occlusion back toward 1.0 (no AO) past fadeStart
    // — distance from the scene focus (see focusDist), matching the early-out above.
    if (pc.maxDistance > 0.0)
    {
        float fade = clamp((focusDist - pc.fadeStart) / max(pc.maxDistance - pc.fadeStart, 1e-3), 0.0, 1.0);
        ao = mix(ao, 1.0, fade);
    }
    // Bent normal = average unoccluded direction; fall back to the surface normal when fully occluded.
    vec3 bentN = (dot(bent, bent) > 1e-8) ? normalize(bent) : N;
    imageStore(u_aoOut, px, vec4(bentN, ao));
}
