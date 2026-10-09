#ifndef RIVER_WAVE_INC_GLSL
#define RIVER_WAVE_INC_GLSL

// THE RIVER'S WAVES (river.vs.glsl: the dense near geometry; river.fs.glsl: the shading normal - one field, so the two
// agree): the OCEAN's FFT maps (OceanSimulationPipeline; per cascade c, layer c = displacement (Dx, h, Dz, -), layer
// OCEAN_CASCADES + c = gradients (dh/dx, dh/dz, ...)), which always run with the rivers. The field is scaled to the
// river: h(x) = H * h0(T x), so its slope is H * T * g0(T x) - "Wave tiling" T shortens the waves, "Wave height" H (x the
// whitewater's "Wave rapids") sets them. It is DRAGGED DOWNSTREAM by the flow with the two-phase flow map (each phase
// restarts every RIVER_WAVE_PERIOD seconds, the two half a period apart and cross-faded; the second samples elsewhere in
// the field, so the restart never shows). The FFT's own animation runs under it.
// The includer defines RIVER_WAVE_TEX(uvLayer) first: textureLod(u_oceanMaps, uvLayer, 0.0) in the VS, texture() in the FS.

layout (binding = 7) uniform sampler2DArray u_oceanMaps;

const float RIVER_WAVE_PERIOD = 2.0; // s

// The two phases' sample positions (in the tiled domain) and the first one's weight.
void riverWavePhases(vec2 worldXZ, vec2 flow, out vec2 q0, out vec2 q1, out float w0)
{
    const float t = u_timeSeconds / RIVER_WAVE_PERIOD;
    const float p0 = fract(t);
    const float p1 = fract(t + 0.5);
    w0 = 1.0 - abs(2.0 * p0 - 1.0);
    const float T = u_river_waveTiling;
    q0 = (worldXZ - flow * (p0 * RIVER_WAVE_PERIOD)) * T;
    q1 = (worldXZ - flow * (p1 * RIVER_WAVE_PERIOD)) * T + vec2(37.0, 61.0);
}

// The wave height (m) at worldXZ for the local flow (m/s) and whitewater amount.
float riverWaveHeight(vec2 worldXZ, vec2 flow, float whitewater)
{
    vec2 q0, q1;
    float w0;
    riverWavePhases(worldXZ, flow, q0, q1, w0);
    float h = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_ocean_cascadeSizes[c];
        h += RIVER_WAVE_TEX(vec3(q0 / L, float(c))).y * w0 + RIVER_WAVE_TEX(vec3(q1 / L, float(c))).y * (1.0 - w0);
    }
    return h * u_river_waveHeight * (1.0 + u_river_waveRapids * whitewater);
}

// The same field's slope (dh/dx, dh/dz).
vec2 riverWaveSlope(vec2 worldXZ, vec2 flow, float whitewater)
{
    vec2 q0, q1;
    float w0;
    riverWavePhases(worldXZ, flow, q0, q1, w0);
    vec2 g = vec2(0.0);
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_ocean_cascadeSizes[c];
        g += RIVER_WAVE_TEX(vec3(q0 / L, float(OCEAN_CASCADES + c))).xy * w0
           + RIVER_WAVE_TEX(vec3(q1 / L, float(OCEAN_CASCADES + c))).xy * (1.0 - w0);
    }
    return g * (u_river_waveHeight * (1.0 + u_river_waveRapids * whitewater) * u_river_waveTiling);
}

// The dense near cells' size (engine m) - Procedural RiverSystem's c_nearCell, keep in step. The light ribbon stays sunk
// out to the farthest a resident dense cell can reach (RiverSystem keeps cells whose centre is within the radius + one
// cell), then rises back over one more cell.
const float RIVER_NEAR_CELL = 64.0;

#endif
