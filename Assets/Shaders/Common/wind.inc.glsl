#ifndef WIND_INC_GLSL
#define WIND_INC_GLSL

// THE VEGETATION WIND, shared by the trees (tree_wind.inc.glsl) and the grass (grass.vs.glsl): "Sky/Wind", one direction,
// strength and set of passing gusts for everything that bends. The mean velocity + a 2D GUST VECTOR x the gust strength,
// as the particles' weather wind: each component two waves travelling downwind (1.2 and 0.46 x the gust size long, the
// second skewed). A 2D vector, not a signed gust along the mean, so the flurries TURN the wind rather than cancel it (a
// gust stronger than the mean wind would stop the trees dead). Analytic, a few ALU per call. Requires the UBO.

// The wind at `xz` at `time`: its speed (m/s) and direction (unit, XZ).
float vegetationWind(vec2 xz, float time, out vec2 dir)
{
    const vec2 meanDir = u_weather_windDirection;
    const float along = dot(xz, meanDir);
    const float across = dot(xz, vec2(-meanDir.y, meanDir.x));
    const float travel = time * u_weather_windTravelSpeed;
    const float k1 = 5.236 * u_weather_invGustSize;  // 2 pi / (1.2 x the gust size)
    const float k2 = 13.66 * u_weather_invGustSize;  // 2 pi / (0.46 x the gust size)
    const vec2 gust = vec2(0.6 * sin((along - travel) * k1) + 0.4 * sin((along * 0.8 + across * 0.6 - travel) * k2),
                           0.6 * sin((along - travel) * k1 + 2.1) + 0.4 * sin((along * 0.7 - across * 0.7 - travel) * k2 * 0.85 + 4.2));
    const vec2 wind = u_weather_windVelocity.xz + gust * u_weather_gustStrength;
    const float speed = length(wind);
    dir = speed > 1e-3 ? wind / speed : meanDir;
    return speed;
}

#endif
