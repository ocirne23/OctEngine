#ifndef DECAL_INC_GLSL
#define DECAL_INC_GLSL

// Projected box decals (DecalPipeline). DECAL_FLAG_* and PARTICLE_TEX_NONE are injected by the engine
// from RendererVKLayout (Layout.ixx). Struct must stay in sync with RendererVKLayout::DecalInfo (96 B).

struct Decal
{
    vec4 posOpacity;          // xyz = box center, w = overall opacity multiplier
    vec4 rotation;            // quat; local +Z projects onto the surface
    vec4 halfExtentsAngleFade;// xyz = box half extents, w = angle-fade cos cutoff (vs -Z)
    vec4 tint;                // rgb = color * intensity, a = base alpha
    vec4 emissiveFadeWidth;   // rgb = emissive radiance, w = angle-fade smoothstep width
    uvec4 params;             // x = diffuse texture idx (PARTICLE_TEX_NONE = solid tint), y = DECAL_FLAG_* bits
};

vec3 decalQuatRotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
// Inverse rotation (unit quat: conjugate).
vec3 decalQuatUnrotate(vec4 q, vec3 v)
{
    return decalQuatRotate(vec4(-q.xyz, q.w), v);
}

#endif
