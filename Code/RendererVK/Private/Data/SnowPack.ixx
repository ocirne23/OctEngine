export module RendererVK:SnowPack;

import Core;
import Core.glm;
import :Layout;

// THE SNOWPACK: the snow depth on the ground (m) per CLIMATE temperature at the shaded height and per CATCH (how much of
// the snowfall a place keeps: 1 = open flat ground), integrated on the CPU in game time from the weather now - and per
// cell the STATE of its surface (the grain size, the meltwater, the grime), which the snow's look reads.
//
// The weather is ONE global state (precipitation, the air temperature offset, the sun), so every place with the same
// climate temperature and the same catch has had the same history: this table IS the whole world's pack. Nothing is
// integrated on the GPU (no R16F rounding, no fixed tick), and there is no window to fill when the camera moves - the
// near and the far ground read the same function. The renderer copies the depths and the states into this frame slot's
// buffer (snow.inc.glsl reads them by device address, u_snow_table / u_snow_stateTable). Main thread.
export class SnowPack final
{
public:
    static constexpr uint32 TEMP_BINS = RendererVKLayout::SNOW_TABLE_TEMP_BINS;
    static constexpr uint32 CATCH_BINS = RendererVKLayout::SNOW_TABLE_CATCH_BINS;

    // The surface of one cell, as the GPU reads it (a vec4).
    struct CellState
    {
        float grainMm = 0.1f; // the optical grain diameter
        float liquid = 0.0f;  // the meltwater in the surface layer, 0..1
        float grime = 0.0f;   // the impurities, 0..1
        float pad = 0.0f;
    };
    static_assert(sizeof(CellState) == 16);

    // The weather over one step (Renderer::advanceWeather builds it from the "Weather" settings).
    struct Step
    {
        double gameHours = 0.0;        // the step's length in game time
        float precipitation = 0.0f;    // 0..1
        float airOffset = 0.0f;        // C added to the climate temperature (the offset + the day-night swing)
        float snowBelow = 0.5f;        // C of air: all snow below ...
        float rainAbove = 2.0f;        // ... all rain above (> snowBelow)
        float snowfallRate = 0.0f;     // m per game hour at precipitation 1 and catch 1
        float meltRate = 0.0f;         // m per game hour per C of air over 0
        float sunMelt = 0.0f;          // m per game hour x the sun height (faded out in cold air)
        float sunHeight = 0.0f;        // the sine of the sun's elevation
        float settlingPerHour = 0.0f;  // the depth settles by exp(-this x hours)
        float maxDepth = 3.0f;         // m
        // The state ("Weather/Snow": Fresh grain size .. Melt grime).
        float freshGrainMm = 0.1f;
        float grainGrowth = 0.05f;     // mm^2 per game day (dry)
        float wetGrainGrowth = 0.2f;   // mm^3 per game hour at full meltwater
        float surfaceLayer = 0.02f;    // m
        float meltHold = 0.005f;       // m
        float drainHours = 6.0f;
        float refreezeHours = 2.0f;
        float grimePerDay = 0.01f;
        float meltGrime = 0.5f;
    };
    void advance(const Step& step);
    void clear(float freshGrainMm);

    oc::span<const float> depths() const { return m_depthOut; }      // [c * TEMP_BINS + t]
    oc::span<const CellState> states() const { return m_state; }     // the same order
    float deepest() const { return m_deepest; }                      // the deepest cell (0 = no snow anywhere)

    // The table's axes, and the share of the precipitation that falls as snow at an air temperature.
    static float binTemperature(uint32 t);
    static float binCatch(uint32 c);
    static float snowFraction(float airC, float snowBelow, float rainAbove);

    // The surface's albedo (linear RGB) at a state: the spherical albedo of a weakly absorbing granular medium. The C++ twin
    // of snowAlbedo (snow.inc.glsl): the same constants. grimeAbs = grimeAbsorption's (u_snow_grimeAbs).
    static glm::vec3 surfaceAlbedo(float grainMm, float liquid, float grime, const glm::vec3& grimeAbs);
    // The grime's absorption per channel at full grime (m^-1; u_snow_grimeAbs): the dust's, x "Grime strength", toward a
    // neutral grey of the same mean by 1 - "Grime brownness".
    static glm::vec3 grimeAbsorption(float strength, float brownness);

private:
    oc::array<double, RendererVKLayout::SNOW_TABLE_CELLS> m_depth{};
    oc::array<float, RendererVKLayout::SNOW_TABLE_CELLS> m_depthOut{};
    // The state integrates in doubles (m_stateD); m_state is its float copy for the upload.
    struct StateD { double grainMm = 0.1, liquid = 0.0, grime = 0.0; };
    oc::array<StateD, RendererVKLayout::SNOW_TABLE_CELLS> m_stateD{};
    oc::array<CellState, RendererVKLayout::SNOW_TABLE_CELLS> m_state{};
    float m_deepest = 0.0f;
};
