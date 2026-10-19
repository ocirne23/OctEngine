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
vec2 grassWind(vec2 xz, float time, float phase)
{
    vec2 dir;
    const float speed = vegetationWind(xz, time, dir);
    const float sway = sin(time * u_grass_swayFrequency * GRASS_TWO_PI + phase);
    const float ripple = grassValueNoise((xz - u_weather_windDirection * (u_weather_windTravelSpeed * time)) * u_grass_invRippleSize);
    return dir * (speed * (u_grass_windBend * (0.85 + 0.15 * sway) + u_grass_rippleBend * ripple * (0.8 + 0.2 * sway)));
}

// The tip and the control point of a blade of height h: lean + wind sideways (capped at 0.9 h), the rest upward. The
// wind's push x h^2 / (h^2 + "Stiff height"^2): a short blade is stiffer (in blade heights it bent as far as a tall one).
vec3 grassTip(vec3 root, vec2 lean, vec2 wind, float h, out vec3 ctrl)
{
    const float h2 = h * h;
    vec2 offset = lean + wind * (h * h2 / max(h2 + u_grass_stiffHeight2, 1e-6));
    const float len = length(offset);
    if (len > 0.9 * h)
        offset *= 0.9 * h / len;
    const float y = sqrt(h * h - dot(offset, offset)); // the radicand >= 0.19 h^2: offset <= 0.9 h
    ctrl = root + vec3(0.0, y, 0.0);
    return root + vec3(offset.x, y, offset.y);
}

#endif
