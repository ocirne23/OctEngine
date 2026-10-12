#ifndef POM_INC_GLSL
#define POM_INC_GLSL

// Silhouette Parallax Occlusion Mapping (Tatarchuk 2006). The height is the normal map's alpha: 1 = the top (no
// displacement), 0 = maximum depth; a normal map without height (alpha 1) takes the early out. The includer declares
// u_textures first.

#define POM_MIN_STEPS    8
#define POM_MAX_STEPS    32
#define POM_HEIGHT_SCALE 0.05

// The parallax-displaced uv. Discards the fragment at the silhouette and when back-facing.
vec2 spomDisplaceUV(uint normalTexIdx, vec3 V)
{
    const vec3 viewDirTS = normalize(transpose(in_tbn) * V);
    if (viewDirTS.z <= 0.001)
        discard;

    // Surface depth at the entry UV (0 = flat, 1 = maximum depth).
    float initSurfDepth = 1.0 - texture(u_textures[normalTexIdx], in_uv).a;
    if (initSurfDepth < 0.001)
        return in_uv;

    // More layers at grazing angles.
    int   numSteps  = int(mix(float(POM_MAX_STEPS), float(POM_MIN_STEPS), viewDirTS.z));
    float depthStep = 1.0 / float(numSteps);
    vec2 uvStep = (viewDirTS.xy / viewDirTS.z) * POM_HEIGHT_SCALE * depthStep;

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

    // A uv outside [0, 1]: the view ray left through the mesh boundary, so the outline follows the displaced surface.
    if (curUV.x < 0.0 || curUV.x > 1.0 || curUV.y < 0.0 || curUV.y > 1.0)
        discard;

    // Interpolate between the last layer above the surface and the first at / below it.
    float before = prevSurfDepth - prevLayerDepth; // > 0
    float after  = curSurfDepth  - curLayerDepth;  // <= 0
    float denom  = before - after;
    float weight = (abs(denom) > 1e-5) ? (before / denom) : 0.0;

    return mix(prevUV, curUV, weight);
}

#endif // POM_INC_GLSL