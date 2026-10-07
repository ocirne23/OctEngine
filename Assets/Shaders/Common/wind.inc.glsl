#ifndef WIND_INC_GLSL
#define WIND_INC_GLSL

// THE VEGETATION WIND, shared by the trees (tree_wind.inc.glsl) and the grass (grass.vs.glsl): THE wind ("Sky/Wind",
// u_weather_windVelocity: the mean velocity, .gustStrength, .invGustSize: 1 / the gust size) - one direction, strength
// and set of passing gusts for everything that bends. The mean + a 2D GUST VECTOR x the gust strength, as the
// particles' weather wind: each component two waves travelling downwind (1.2 and 0.46 x the gust size long - 60 m
// and 23 m at the default 50 m - the second skewed; the components apart in phase). A 2D vector, not a signed gust
// along the mean: the flurries TURN the wind rather than cancel it (a signed gust stronger than the mean wind stopped
// the trees dead, in bands drifting downwind). Analytic: no texture, a few ALU per call.
// Requires the UBO.

// The wind at `xz` at `time`: its speed (m/s) and direction (unit, XZ).
float vegetationWind(vec2 xz, float time, out vec2 dir)
{
    const float meanSpeed = length(u_weather_windVelocity.xz);
    const vec2 meanDir = meanSpeed > 1e-3 ? u_weather_windVelocity.xz / meanSpeed : u_weather_windDirection;
    const float along = dot(xz, meanDir);
    const float across = dot(xz, vec2(-meanDir.y, meanDir.x));
    const float travel = time * max(meanSpeed, 1.0);
    const float k1 = 5.236 * u_weather_invGustSize;  // 2 pi / (1.2 x the gust size)
    const float k2 = 13.66 * u_weather_invGustSize;  // 2 pi / (0.46 x the gust size)
    const vec2 gust = vec2(0.6 * sin((along - travel) * k1) + 0.4 * sin((along * 0.8 + across * 0.6 - travel) * k2),
                           0.6 * sin((along - travel) * k1 + 2.1) + 0.4 * sin((along * 0.7 - across * 0.7 - travel) * k2 * 0.85 + 4.2));
    const vec2 wind = u_weather_windVelocity.xz + gust * u_weather_gustStrength;
    const float speed = length(wind);
    dir = speed > 1e-3 ? wind / speed : meanDir;
    return speed;
}

// The mean wind's direction (unit, XZ) and speed (m/s) alone: what small-scale detail travels with.
vec2 vegetationWindMeanDir()
{
    const float meanSpeed = length(u_weather_windVelocity.xz);
    return meanSpeed > 1e-3 ? u_weather_windVelocity.xz / meanSpeed : u_weather_windDirection;
}

#endif
