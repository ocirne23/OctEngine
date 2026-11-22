// THE GRASS BLADE'S SHAPE IN THE WIND, shared by the blades (grass.vs.glsl) and the flower stems (clutter_flower.vs.glsl),
// so a flower sways exactly as the grass around it. Needs grass.inc.glsl (grassValueNoise) and wind.inc.glsl first.

#ifndef GRASS_WIND_INC_GLSL
#define GRASS_WIND_INC_GLSL

const float GRASS_TWO_PI = 6.28318530718;

vec3 grassBezier(vec3 p0, vec3 p1, vec3 p2, float t)
{
    const float s = 1.0 - t;
    return s * s * p0 + 2.0 * s * t * p1 + t * t * p2;
}

// The wind's push on the tip at a point and time, in blade heights (XZ): THE shared wind (wind.inc.glsl - its direction,
// speed and gusts, the trees bend in the same) x "Bend" per m/s with a sway, plus a small-scale RIPPLE - value noise
// moving with the mean wind - x "Ripple" per m/s.
// In two parts, so a grass TUFT samples the field once for all its blades (grass_blade.inc.glsl): the FIELD at a point
// and time (xy = the wind's direction, z = its speed, w = the ripple noise), and a blade's PUSH from it (its own sway
// phase).
vec4 grassWindField(vec2 xz, float time)
{
    vec2 dir;
    const float speed = vegetationWind(xz, time, dir);
    const float ripple = grassValueNoise((xz - u_weather_windDirection * (u_weather_windTravelSpeed * time)) * u_grass_invRippleSize);
    return vec4(dir, speed, ripple);
}

vec2 grassWindPush(vec4 field, float time, float phase)
{
    const float sway = sin(time * u_grass_swayFrequency * GRASS_TWO_PI + phase);
    return field.xy * (field.z * (u_grass_windBend * (0.85 + 0.15 * sway) + u_grass_rippleBend * field.w * (0.8 + 0.2 * sway)));
}

vec2 grassWind(vec2 xz, float time, float phase)
{
    return grassWindPush(grassWindField(xz, time), time, phase);
}

// The same push in ONE step, from the wind VECTOR (the field's direction x its speed is that vector: no length, no
// divide). For a caller that keeps no field (the grass blades: grass_blade.inc.glsl).
// In two parts: the RIPPLE noise at a point and time (grassWindRipple), and the push with it (grassWindPushWith). A
// caller with two times (the blades: this frame and the last, for the motion vectors) takes ONE ripple tap for both.
float grassWindRipple(vec2 xz, float time)
{
    return grassValueNoise((xz - u_weather_windDirection * (u_weather_windTravelSpeed * time)) * u_grass_invRippleSize);
}

vec2 grassWindPushWith(float ripple, vec2 xz, float time, float phase)
{
    const float sway = sin(time * u_grass_swayFrequency * GRASS_TWO_PI + phase);
    return vegetationWindVector(xz, time) * (u_grass_windBend * (0.85 + 0.15 * sway) + u_grass_rippleBend * ripple * (0.8 + 0.2 * sway));
}

vec2 grassWindPushAt(vec2 xz, float time, float phase)
{
    return grassWindPushWith(grassWindRipple(xz, time), xz, time, phase);
}

// The tip of a blade of height h, from its root: lean + wind sideways (xz, capped at 0.9 h), the rest upward (y). The
// wind's push x h^2 / (h^2 + "Stiff height"^2): a short blade is stiffer (in blade heights it bent as far as a tall one).
// The control point stands the same y straight above the root, so the curve at t is
// root + (t^2 x, (2t - t^2) y, t^2 z): no control point needed (grass_blade.inc.glsl).
vec3 grassTipOffset(vec2 lean, vec2 wind, float h)
{
    const float h2 = h * h;
    vec2 offset = lean + wind * (h * h2 / max(h2 + u_grass_stiffHeight2, 1e-6));
    const float len = length(offset);
    if (len > 0.9 * h)
        offset *= 0.9 * h / len;
    return vec3(offset.x, sqrt(h2 - dot(offset, offset)), offset.y); // the radicand >= 0.19 h^2: offset <= 0.9 h
}

// The same as world points: the tip, and the control point (the flower stems).
vec3 grassTip(vec3 root, vec2 lean, vec2 wind, float h, out vec3 ctrl)
{
    const vec3 offset = grassTipOffset(lean, wind, h);
    ctrl = root + vec3(0.0, offset.y, 0.0);
    return root + offset;
}

#endif
