// Cloud shadows: the BEER SHADOW MAP lookup (CloudPipeline, cloud_shadow.cs.glsl). Requires ubo.inc.glsl.
// The includer defines CLOUD_SHADOW_BINDING (a sampler2DArray, one layer per cascade, RGBA32F).
//
// Two sun-aligned orthographic cascades around the camera (u_cloudShadow0/1: the centre relative to the
// CENTRE view's camera + 1 / extent; the light-space axes e0 / e1 in u_cloudShadow2/3, L = u_sunDirection).
// A texel is one line along L through the cloud shell:
//   x = the along-light coordinate a = dot(p - centre, L) of the FIRST cloud met coming from the sun,
//   y = the mean extinction (1/m) between that front and the last cloud on the line,
//   z = the line's whole optical depth.
// A receiver at along-light coordinate a sits under (x - a) metres of cloud, capped at the whole depth:
// OD = min(y * max(x - a, 0), z) - Beer's law from the front (Hillaire / Frostbite). So one fetch serves a
// surface under the shell, a point inside a cloud (a mountain, a fog froxel, a cloud sample) and a point
// above it (OD 0).

#ifndef CLOUD_SHADOW_INC_GLSL
#define CLOUD_SHADOW_INC_GLSL

layout (binding = CLOUD_SHADOW_BINDING) uniform sampler2DArray u_cloudShadow;

// Optical depth toward the sun at a camera-relative (CENTRE view) point from one cascade; the edge fade
// weight (1 inside, 0 at the border) in `weight`, 0 = outside.
float cloudShadowCascadeOD(int cascade, vec3 rel, out float weight)
{
    const vec4 c = cascade == 0 ? u_cloudShadow0 : u_cloudShadow1;
    const vec3 s = rel - c.xyz;
    const vec2 uv = vec2(dot(s, u_cloudShadow2.xyz), dot(s, u_cloudShadow3.xyz)) * c.w + 0.5;
    const vec2 edge = min(uv, 1.0 - uv);
    weight = clamp(min(edge.x, edge.y) * 6.0, 0.0, 1.0); // fades over the outer ~17 %: the cascades differ in resolution
    if (weight <= 0.0)
        return 0.0;
    const vec4 t = textureLod(u_cloudShadow, vec3(uv, float(cascade)), 0.0);
    return min(t.y * max(t.x - dot(s, u_sunDirection), 0.0), t.z);
}

// Optical depth toward the sun at a camera-relative (CENTRE view) point: x = the optical depth, y = how
// much of it the maps cover (1 inside, fading to 0 over the far cascade's border). The near cascade blends
// into the far one over its own border (it lies well inside the far one).
vec2 cloudShadowSample(vec3 rel)
{
    float wNear, wFar;
    const float odNear = cloudShadowCascadeOD(0, rel, wNear);
    if (wNear >= 1.0)
        return vec2(odNear, 1.0);
    const float odFar = cloudShadowCascadeOD(1, rel, wFar);
    if (wNear > 0.0)
        return vec2(mix(odFar, odNear, wNear), 1.0);
    return vec2(odFar, wFar);
}

// Sun transmittance through the clouds at a WORLD position (1 = no cloud, or cloud shadows off). Past the
// far cascade it fades to the mean transmittance of the layer, so distant ground does not turn fully lit.
// CLOUD_SHADOWS (baked: "Sky/Clouds" + "Shadows" enabled) compiles it in; u_cloudShadow4.x is the runtime
// part the define cannot hold - the map was not rendered this frame (the game suppresses the clouds, or
// the sun is at the horizon).
float cloudSunTransmittance(vec3 worldPos)
{
#ifdef CLOUD_SHADOWS
    if (u_cloudShadow4.x < 0.5)
        return 1.0;
    const vec2 s = cloudShadowSample(worldPos - u_views[VIEW_CENTER].viewPos.xyz);
    const float T = mix(u_cloudShadow3.w, exp(-s.x), s.y);
    return mix(1.0, T, u_cloudShadow2.w);
#else
    return 1.0;
#endif
}

#endif
