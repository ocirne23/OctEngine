#ifndef POM_INC_GLSL
#define POM_INC_GLSL

// Silhouette Parallax Occlusion Mapping (SPOM)
//
// The height field is encoded in the alpha channel of the normal map:
//   alpha = 1.0  ->  surface is at the top  (no displacement)
//   alpha = 0.0  ->  surface is at maximum depth
//
// A standard normal map that carries no height data (all alpha = 1) produces
// zero surface depth, so the early-out fires and no marching is done.
//
// This file references the global `u_textures` sampler array that must be
// declared in the including shader before this file is included.
//
// References:
//   Tatarchuk 2006 - "Practical Parallax Occlusion Mapping with Self-Shadowing
//                     for Detailed Surface Rendering"

#define POM_MIN_STEPS    8
#define POM_MAX_STEPS    32
#define POM_HEIGHT_SCALE 0.05

// Returns parallax-displaced texture coordinates.
// Discards the fragment at silhouette / back-facing boundaries.
//
//   normalTexIdx  - index into u_textures[]; height stored in the alpha channel
vec2 spomDisplaceUV(uint normalTexIdx, vec3 V)
{
    const vec3 viewDirTS = normalize(transpose(in_tbn) * V);

    // Silhouette: back-facing or extremely grazing incidence -> discard.
    if (viewDirTS.z <= 0.001)
        discard;

    // Surface depth at the entry UV (0 = flat, 1 = maximum depth).
    float initSurfDepth = 1.0 - texture(u_textures[normalTexIdx], in_uv).a;
    if (initSurfDepth < 0.001)
        return in_uv; // Flat surface; no marching needed.

    // Adaptive layer count: fewer layers when looking head-on (large z),
    // more layers at grazing angles where many layers are needed.
    int   numSteps  = int(mix(float(POM_MAX_STEPS), float(POM_MIN_STEPS), viewDirTS.z));
    float depthStep = 1.0 / float(numSteps);

    // UV delta per depth layer: project the view ray onto the tangent plane.
    // As we trace deeper, the visible UV shifts toward the camera's foot point.
    vec2 uvStep = (viewDirTS.xy / viewDirTS.z) * POM_HEIGHT_SCALE * depthStep;

    // ---- Linear ray march ----
    float curLayerDepth = 0.0;
    vec2  curUV         = in_uv;
    float curSurfDepth  = initSurfDepth;

    float prevLayerDepth = 0.0;
    vec2  prevUV         = in_uv;
    float prevSurfDepth  = initSurfDepth;

    for (int i = 0; i < POM_MAX_STEPS; ++i)
    {
        if (i >= numSteps || curLayerDepth >= curSurfDepth)
            break;

        prevLayerDepth = curLayerDepth;
        prevUV         = curUV;
        prevSurfDepth  = curSurfDepth;

        curUV         += uvStep;
        curLayerDepth += depthStep;
        curSurfDepth   = 1.0 - texture(u_textures[normalTexIdx], curUV).a;
    }

    // ---- SPOM silhouette clipping ----
    // If the displaced UV has walked outside [0, 1] the view ray exited through
    // a mesh boundary; this fragment is part of the geometric silhouette and
    // must be discarded so the mesh outline matches the displaced surface.
    if (curUV.x < 0.0 || curUV.x > 1.0 || curUV.y < 0.0 || curUV.y > 1.0)
        discard;

    // ---- Linear interpolation refinement ----
    // prevLayer: ray is above the surface  (prevSurfDepth > prevLayerDepth)
    // curLayer:  ray is at/below surface   (curSurfDepth  <= curLayerDepth)
    float before = prevSurfDepth - prevLayerDepth; // > 0
    float after  = curSurfDepth  - curLayerDepth;  // <= 0
    float denom  = before - after;
    float weight = (abs(denom) > 1e-5) ? (before / denom) : 0.0;

    return mix(prevUV, curUV, weight);
}

#endif // POM_INC_GLSL