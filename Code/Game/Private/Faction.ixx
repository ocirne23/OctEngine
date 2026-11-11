export module Game:Faction;

import Core;
import Core.glm;

// THE FACTION LAYER: one byte for each cell of a world-fixed grid says whose land it is - 0 = fully the enemy's (dark,
// corrupted), 255 = fully the players' (light, vibrant), 128 = neutral (the world as generated). Game state, owned
// here on the CPU; the renderer gets a copy as one 8-bit texture for the look (Renderer::setFactionMap). All land
// starts at one value ("Start value": 0 = the whole world is the enemy's).
//
// SOURCES push the land around them toward one side (setSources: a disc, + = the players, - = the enemy). They are
// stamped into a signed source field, and the SPREAD is a relaxation swept a few rows a frame:
//   v += dt x ( diffusion x (mean of the 4 neighbours - v) + |source| x (its side - v) / "Source time" )
// so land near a source goes to its side and the value bleeds out past the source's radius. LAND KEEPS ITS VALUE: there
// is no return over time - only a source of the other side takes land back.
//
// COST: the grid is 16 M cells, so nothing touches all of it after the reset. The sweep covers only the ACTIVE BOX (the
// sources' rectangles + a margin for the bleed), the stamp only the cells sources touch, and the renderer gets only the
// rectangle a sweep changed. All of it runs on the main thread (GameMatch::update).
export class FactionLayer final
{
public:
    static constexpr int Size = 4096; // cells for each axis (= the renderer's TerrainResources::FACTION_MAP_RES)
    static constexpr uint8 Neutral = 128;

    struct Source
    {
        glm::vec2 pos{ 0.0f }; // world x / z
        float radius = 0.0f;   // m: full at the centre, 0 at the radius
        float strength = 0.0f; // + = the players, - = the enemy; 1 = the full pull of "Source time"
    };

    // All land at startValue, the grid centred on centre (world x / z).
    void reset(const glm::vec2& centre, float cellSize, uint8 startValue);
    void clear() { m_valid = false; }
    bool valid() const { return m_valid; }

    // The frame's sources. They take effect at the next sweep's start (the source field is rebuilt there).
    void setSources(oc::span<const Source> sources) { m_sources.assign(sources.begin(), sources.end()); }

    // The spread: a slice of the active box's rows. Returns true on the frame a sweep ended that changed a byte; outDirty
    // is then the cell rectangle (x0, z0, x1, z1, inclusive) those changes lie in - what the renderer needs.
    bool tick(float deltaSec, glm::ivec4& outDirty);

    // 0..255 at a world point (the nearest cell; the start value outside the grid).
    uint8 sample(float x, float z) const;
    // -1 = fully the enemy's .. 0 = neutral .. 1 = fully the players'.
    float sampleSide(float x, float z) const { return ((float)sample(x, z) - (float)Neutral) / 127.0f; }

    const uint8* bytes() const { return m_bytes.data(); }
    glm::vec2 origin() const { return m_origin; } // world x / z of the grid's min corner
    float cellSize() const { return m_cellSize; }
    uint8 startValue() const { return m_startValue; }

private:
    void stampSources();

    bool m_valid = false;
    glm::vec2 m_origin{ 0.0f };
    float m_cellSize = 4.0f;
    uint8 m_startValue = 0;
    // 16 M cells: 2 + 1 + 1 bytes each (64 MB).
    oc::vector<uint16> m_value; // the simulated value, 0..65535 = 0..255 (a byte would stall: a slow step rounds to no change)
    oc::vector<int8> m_source;  // the signed source field, -127..127 = -1..1
    oc::vector<uint8> m_bytes;  // m_value rounded: what sample() and the renderer read
    oc::vector<Source> m_sources;
    oc::vector<glm::ivec4> m_stamped; // the cell rectangles (x0, z0, x1, z1) the last stamp wrote: all it has to clear
    glm::ivec4 m_box{ 0, 0, -1, -1 }; // this sweep's active box (x0, z0, x1, z1); empty = nothing to sweep
    glm::ivec4 m_dirty{ Size, Size, -1, -1 }; // the cells this sweep changed a byte of (empty: x1 < x0)
    int m_row = 0;              // the sweep's next row
    float m_rowCarry = 0.0f;    // the fraction of a row the frames have earned
    bool m_stampDue = true;     // the next tick starts a sweep: stamp the sources and set the box first
};
