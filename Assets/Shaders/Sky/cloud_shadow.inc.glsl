// Cloud shadows: the BEER SHADOW MAP lookup (CloudPipeline, written by cloud_shadow.cs.glsl). Requires ubo.inc.glsl.
// The includer defines CLOUD_SHADOW_BINDING (a sampler2DArray, one layer per cascade, RGBA32F).
//
// Two cascades around the camera, each a SNAPSHOT of the clouds (RendererState::buildUboClouds renders the next one a
// tile per frame in the cascade's other layer). A map is a HORIZONTAL grid at the main layer's middle, fixed to the
// clouds; a texel is the line through its grid point along the snapshot's sun direction:
//   x = the along-light coordinate a of the FIRST cloud met coming from the sun (a = 0 at the grid point),
//   y = the mean extinction (1/m) between that front and the last cloud on the line,
//   z = the line's whole optical depth.
// A receiver at along-light coordinate a: OD = min(y * max(x - a, 0), z) (Beer's law from the front). So one fetch
// serves a surface under the clouds, a point inside a cloud and a point above it (OD 0).
// THE LOOKUP IS THREE PLANES per cascade over a point p relative to the CENTRE view's camera (the CPU builds them):
//   u_clouds_shadowU / V: the map's uv = dot(p, xyz) + w - the grid point where TODAY's sun ray from p meets the
//                         layer's middle, the wind's drift in it; w = the CAMERA's own uv;
//   u_clouds_shadowA:     a = dot(p, xyz) + w - p's height over the middle along the snapshot's sun.
// u_clouds_shadowWindow: x = the snapshot's array layer, y = the half size of the lookup's WINDOW, z = 1 / its fade
// band. The window lies around the camera's place in the map, not around the map: a new snapshot (its middle where
// the camera was then) does not move the border between the cascades.

#ifndef CLOUD_SHADOW_INC_GLSL
#define CLOUD_SHADOW_INC_GLSL

layout (binding = CLOUD_SHADOW_BINDING) uniform sampler2DArray u_cloudShadow;

// A cascade's map uv of a camera-relative (CENTRE view) point, and the lookup's weight there: 1 inside the window, 0
// at its border (and at the map's own border, where a fast camera has left the margin).
vec2 cloudShadowUV(int cascade, vec3 rel, out float weight)
{
    const vec2 d = vec2(dot(rel, u_clouds_shadowU[cascade].xyz), dot(rel, u_clouds_shadowV[cascade].xyz)); // from the camera's place
    const vec2 uv = d + vec2(u_clouds_shadowU[cascade].w, u_clouds_shadowV[cascade].w);
    const vec2 edge = min(uv, 1.0 - uv);
    weight = clamp(min((u_clouds_shadowWindow[cascade].y - max(abs(d.x), abs(d.y))) * u_clouds_shadowWindow[cascade].z,
                       min(edge.x, edge.y) * 16.0), 0.0, 1.0);
    return uv;
}

// The along-light coordinate of that point.
float cloudShadowAlong(int cascade, vec3 rel)
{
    return dot(rel, u_clouds_shadowA[cascade].xyz) + u_clouds_shadowA[cascade].w;
}

// Optical depth toward the sun at a camera-relative (CENTRE view) point from one cascade; the window's weight in
// `weight`, 0 = outside.
float cloudShadowCascadeOD(int cascade, vec3 rel, out float weight)
{
    const vec2 uv = cloudShadowUV(cascade, rel, weight);
    if (weight <= 0.0)
        return 0.0;

    const vec4 t = textureLod(u_cloudShadow, vec3(uv, u_clouds_shadowWindow[cascade].x), 0.0);
    return min(t.y * max(t.x - cloudShadowAlong(cascade, rel), 0.0), t.z);
}

// The FAR cascade's optical depth, FILTERED ON THE RESULT: its texels are tens of metres, and the OD is non-linear in
// the stored terms, so a bilinear blend of the terms looks blocky. Instead each of the 2x2 texels gives its own
// transmittance and those are blended (PCF-style). u_clouds_shadowFarSoftness (texels) jitters the lookup per pixel
// and frame; TAA / the fog's temporal blend average it into a soft penumbra.
float cloudShadowFarODFiltered(vec3 rel, out float weight)
{
    const vec2 uv = cloudShadowUV(1, rel, weight);
    if (weight <= 0.0)
        return 0.0;

    const int res = textureSize(u_cloudShadow, 0).x;
    vec2 st = uv * float(res) - 0.5;
    {
        // World-anchored dither (vertex stages call this too: no gl_FragCoord), re-rolled every frame. 0 softness adds 0.
        const vec2 q = rel.xz * 3.0 + 5.588238 * float(u_frameIndex & 63u);
        const vec2 j = vec2(fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715)))),
                            fract(52.9829189 * fract(dot(q, vec2(0.00583715, 0.06711056)) + 0.5)));
        st += (j - 0.5) * u_clouds_shadowFarSoftness;
    }

    const ivec2 i0 = ivec2(floor(st));
    const vec2 f = st - vec2(i0);
    const float a = cloudShadowAlong(1, rel);
    const int layer = int(u_clouds_shadowWindow[1].x);
    float T = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        const vec4 t = texelFetch(u_cloudShadow, ivec3(clamp(i0 + o, ivec2(0), ivec2(res - 1)), layer), 0);
        const float w = (o.x == 0 ? 1.0 - f.x : f.x) * (o.y == 0 ? 1.0 - f.y : f.y);
        T += w * exp(-min(t.y * max(t.x - a, 0.0), t.z));
    }
    return -log(max(T, 1e-6));
}

// Optical depth toward the sun at a camera-relative (CENTRE view) point: x = the optical depth, y = how much of it the
// maps cover (1 inside, fading to 0 over the far cascade's border). The near cascade blends into the far one over its
// own border. filtered = the far cascade through cloudShadowFarODFiltered (4 fetches), else one hardware-bilinear
// fetch (a compile-time constant at every call site, so the other path compiles out).
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

// Sun transmittance through the clouds at a WORLD position (1 = no cloud, or cloud shadows off). Past the far cascade
// it fades to the layer's mean transmittance. CLOUD_SHADOWS is baked; u_clouds_shadowRendered is the runtime part (the
// map was rendered this frame: not suppressed by the game, the sun above the horizon).
// LOW SUN: the transmittance blends to the layer's mean by u_clouds_shadowLowSunAverage ("Low sun average start / end"):
// a low sun throws a cloud's shadow kilometres away, and its slow motion swept the shadows across the world - flicker.
// Fully averaged, the map is not read.
float cloudShadowLowSunMix(float T)
{
    return mix(T, u_clouds_shadowMeanTransmittance, u_clouds_shadowLowSunAverage);
}

float cloudSunTransmittanceImpl(vec3 worldPos, bool filtered)
{
#ifdef CLOUD_SHADOWS
    if (u_clouds_shadowRendered < 0.5)
        return 1.0;
    if (u_clouds_shadowLowSunAverage >= 1.0)
        return mix(1.0, u_clouds_shadowMeanTransmittance, u_clouds_shadowStrength);
    const vec2 s = cloudShadowSample(worldPos - u_views_viewPos[VIEW_CENTER].xyz, filtered);
    const float T = cloudShadowLowSunMix(mix(u_clouds_shadowMeanTransmittance, exp(-s.x), s.y));
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
    if (u_clouds_shadowLowSunAverage >= 1.0)
        return mix(1.0, u_clouds_shadowMeanTransmittance, u_clouds_shadowStrength);
    float w;
    const float od = cloudShadowCascadeOD(1, worldPos - u_views_viewPos[VIEW_CENTER].xyz, w);
    const float T = cloudShadowLowSunMix(mix(u_clouds_shadowMeanTransmittance, exp(-od), w));
    return mix(1.0, T, u_clouds_shadowStrength);
#else
    return 1.0;
#endif
}

#endif
