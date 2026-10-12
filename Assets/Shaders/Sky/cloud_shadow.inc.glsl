// Cloud shadows: the BEER SHADOW MAP lookup (CloudPipeline, written by cloud_shadow.cs.glsl). Requires
// ubo.inc.glsl. The includer defines CLOUD_SHADOW_BINDING (a sampler2DArray, one layer per cascade, RGBA32F).
//
// Two sun-aligned orthographic cascades around the camera (u_clouds_shadowCascade[]: xyz = the centre relative
// to the CENTRE view's camera, w = 1 / extent; the light-space axes in u_clouds_shadowAxis0 / 1, L =
// u_sunDirection). A texel is one line along L through the main cloud layer:
//   x = the along-light coordinate a = dot(p - centre, L) of the FIRST cloud met coming from the sun,
//   y = the mean extinction (1/m) between that front and the last cloud on the line,
//   z = the line's whole optical depth.
// A receiver at along-light coordinate a: OD = min(y * max(x - a, 0), z) (Beer's law from the front). So one
// fetch serves a surface under the clouds, a point inside a cloud and a point above it (OD 0).

#ifndef CLOUD_SHADOW_INC_GLSL
#define CLOUD_SHADOW_INC_GLSL

layout (binding = CLOUD_SHADOW_BINDING) uniform sampler2DArray u_cloudShadow;

// Optical depth toward the sun at a camera-relative (CENTRE view) point from one cascade; the edge fade weight
// (1 inside, 0 at the border) in `weight`, 0 = outside.
float cloudShadowCascadeOD(int cascade, vec3 rel, out float weight)
{
    const vec4 c = u_clouds_shadowCascade[cascade];
    const vec3 s = rel - c.xyz;
    const vec2 uv = vec2(dot(s, u_clouds_shadowAxis0), dot(s, u_clouds_shadowAxis1)) * c.w + 0.5;
    const vec2 edge = min(uv, 1.0 - uv);
    weight = clamp(min(edge.x, edge.y) * 6.0, 0.0, 1.0); // fades over the outer ~17 %: the cascades differ in resolution
    if (weight <= 0.0)
        return 0.0;
    const vec4 t = textureLod(u_cloudShadow, vec3(uv, float(cascade)), 0.0);
    return min(t.y * max(t.x - dot(s, u_sunDirection), 0.0), t.z);
}

// The FAR cascade's optical depth, FILTERED ON THE RESULT: its texels are tens of metres, and the OD is
// non-linear in the stored terms, so a bilinear blend of the terms looks blocky. Instead each of the 2x2 texels
// gives its own transmittance and those are blended (PCF-style). u_clouds_shadowFarSoftness (texels) jitters the
// lookup per pixel and frame; TAA / the fog's temporal blend average it into a soft penumbra.
float cloudShadowFarODFiltered(vec3 rel, out float weight)
{
    const vec3 s = rel - u_clouds_shadowCascade[1].xyz;
    const vec2 uv = vec2(dot(s, u_clouds_shadowAxis0), dot(s, u_clouds_shadowAxis1)) * u_clouds_shadowCascade[1].w + 0.5;
    const vec2 edge = min(uv, 1.0 - uv);
    weight = clamp(min(edge.x, edge.y) * 6.0, 0.0, 1.0);
    if (weight <= 0.0)
        return 0.0;
    const int res = textureSize(u_cloudShadow, 0).x;
    vec2 st = uv * float(res) - 0.5;
    if (u_clouds_shadowFarSoftness > 0.0)
    {
        // World-anchored dither (vertex stages call this too: no gl_FragCoord), re-rolled every frame.
        const vec2 p = rel.xz * 3.0 + 5.588238 * float(u_frameIndex & 63u);
        const vec2 j = vec2(fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))),
                            fract(52.9829189 * fract(dot(p, vec2(0.00583715, 0.06711056)) + 0.5)));
        st += (j - 0.5) * u_clouds_shadowFarSoftness;
    }
    const ivec2 i0 = ivec2(floor(st));
    const vec2 f = st - vec2(i0);
    const float a = dot(s, u_sunDirection);
    float T = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        const vec4 t = texelFetch(u_cloudShadow, ivec3(clamp(i0 + o, ivec2(0), ivec2(res - 1)), 1), 0);
        const float w = (o.x == 0 ? 1.0 - f.x : f.x) * (o.y == 0 ? 1.0 - f.y : f.y);
        T += w * exp(-min(t.y * max(t.x - a, 0.0), t.z));
    }
    return -log(max(T, 1e-6));
}

// Optical depth toward the sun at a camera-relative (CENTRE view) point: x = the optical depth, y = how much of
// it the maps cover (1 inside, fading to 0 over the far cascade's border). The near cascade blends into the far
// one over its own border. filtered = the far cascade through cloudShadowFarODFiltered (4 fetches), else one
// hardware-bilinear fetch (a compile-time constant at every call site, so the other path compiles out).
vec2 cloudShadowSample(vec3 rel, bool filtered)
{
    float wNear, wFar;
    const float odNear = cloudShadowCascadeOD(0, rel, wNear);
    if (wNear >= 1.0)
        return vec2(odNear, 1.0);
    const float odFar = filtered ? cloudShadowFarODFiltered(rel, wFar) : cloudShadowCascadeOD(1, rel, wFar);
    if (wNear > 0.0)
        return vec2(mix(odFar, odNear, wNear), 1.0);
    return vec2(odFar, wFar);
}

// Sun transmittance through the clouds at a WORLD position (1 = no cloud, or cloud shadows off). Past the far
// cascade it fades to the layer's mean transmittance. CLOUD_SHADOWS is baked; u_clouds_shadowRendered is the
// runtime part (the map was rendered this frame: not suppressed by the game, the sun above the horizon).
float cloudSunTransmittanceImpl(vec3 worldPos, bool filtered)
{
#ifdef CLOUD_SHADOWS
    if (u_clouds_shadowRendered < 0.5)
        return 1.0;
    const vec2 s = cloudShadowSample(worldPos - u_views_viewPos[VIEW_CENTER].xyz, filtered);
    const float T = mix(u_clouds_shadowMeanTransmittance, exp(-s.x), s.y);
    return mix(1.0, T, u_clouds_shadowStrength);
#else
    return 1.0;
#endif
}

// Surfaces: the far cascade filtered (a lit surface shows its texels).
float cloudSunTransmittance(vec3 worldPos)
{
    return cloudSunTransmittanceImpl(worldPos, true);
}

// Media and averaged taps (fog froxels, the far field's jittered taps, the clouds' air term, particles): one
// bilinear fetch - they already average over space and time.
float cloudSunTransmittanceBilinear(vec3 worldPos)
{
    return cloudSunTransmittanceImpl(worldPos, false);
}

// From the FAR cascade only (bilinear, one fetch): a soft value over tens of metres. The GI lookup dims the
// probes' sun part by it (giIrradiance).
float cloudSunTransmittanceSoft(vec3 worldPos)
{
#ifdef CLOUD_SHADOWS
    if (u_clouds_shadowRendered < 0.5)
        return 1.0;
    float w;
    const float od = cloudShadowCascadeOD(1, worldPos - u_views_viewPos[VIEW_CENTER].xyz, w);
    const float T = mix(u_clouds_shadowMeanTransmittance, exp(-od), w);
    return mix(1.0, T, u_clouds_shadowStrength);
#else
    return 1.0;
#endif
}

#endif
