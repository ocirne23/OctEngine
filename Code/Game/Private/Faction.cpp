module Game;

import Core;
import Core.glm;
import Settings;
import :Faction;

namespace
{
    constexpr float c_valueScale = 257.0f; // the simulated value's steps for each byte step (65535 / 255)
    constexpr int c_boxMargin = 64;        // cells around the sources' rectangles the sweep covers: room for the bleed
}

void FactionLayer::reset(const glm::vec2& centre, float cellSize, uint8 startValue)
{
    m_cellSize = glm::max(cellSize, 0.5f);
    m_startValue = startValue;
    // Snapped to whole cells, so a reset at the same place gives the same grid.
    m_origin = glm::floor(centre / m_cellSize) * m_cellSize - glm::vec2((float)(Size / 2) * m_cellSize);
    m_value.assign((size_t)Size * Size, (uint16)((float)startValue * c_valueScale));
    m_source.assign((size_t)Size * Size, 0);
    m_bytes.assign((size_t)Size * Size, startValue);
    m_stamped.clear();
    m_box = glm::ivec4(0, 0, -1, -1);
    m_dirty = glm::ivec4(Size, Size, -1, -1);
    m_row = 0;
    m_rowCarry = 0.0f;
    m_stampDue = true;
    m_valid = true;
}

uint8 FactionLayer::sample(float x, float z) const
{
    if (!m_valid)
        return Neutral;
    const int cx = (int)std::floor((x - m_origin.x) / m_cellSize);
    const int cz = (int)std::floor((z - m_origin.y) / m_cellSize);
    if (cx < 0 || cz < 0 || cx >= Size || cz >= Size)
        return m_startValue;
    return m_bytes[(size_t)cz * Size + cx];
}

// The source field: every source's disc, (1 - d / radius)^2 x its strength, summed and held in -127..127 (a
// saturating add) - where the two sides overlap, the stronger and nearer one is left. Only the cells the last stamp
// and this one touch are written. It also sets the sweep's ACTIVE BOX: the stamped rectangles' bounds + the margin.
void FactionLayer::stampSources()
{
    for (const glm::ivec4& rect : m_stamped)
        for (int z = rect.y; z <= rect.w; ++z)
            memset(m_source.data() + (size_t)z * Size + rect.x, 0, (size_t)(rect.z - rect.x + 1));
    m_stamped.clear();
    glm::ivec4 box(Size, Size, -1, -1);
    for (const Source& source : m_sources)
    {
        if (source.radius <= 0.0f || source.strength == 0.0f)
            continue;
        const glm::vec2 local = (source.pos - m_origin) / m_cellSize;
        const float radiusCells = source.radius / m_cellSize;
        const int x0 = glm::max((int)std::floor(local.x - radiusCells), 0), x1 = glm::min((int)std::ceil(local.x + radiusCells), Size - 1);
        const int z0 = glm::max((int)std::floor(local.y - radiusCells), 0), z1 = glm::min((int)std::ceil(local.y + radiusCells), Size - 1);
        if (x1 < x0 || z1 < z0)
            continue;
        m_stamped.push_back(glm::ivec4(x0, z0, x1, z1));
        box = glm::ivec4(glm::min(box.x, x0), glm::min(box.y, z0), glm::max(box.z, x1), glm::max(box.w, z1));
        const float invRadius = 1.0f / radiusCells;
        const float scale = source.strength * 127.0f;
        for (int z = z0; z <= z1; ++z)
        {
            const float dz = (float)z + 0.5f - local.y;
            int8* row = m_source.data() + (size_t)z * Size;
            for (int x = x0; x <= x1; ++x)
            {
                const float dx = (float)x + 0.5f - local.x;
                const float d2 = (dx * dx + dz * dz) * invRadius * invRadius;
                if (d2 >= 1.0f)
                    continue;
                const float fall = 1.0f - std::sqrt(d2);
                row[x] = (int8)glm::clamp((int)row[x] + (int)std::lround(scale * fall * fall), -127, 127);
            }
        }
    }
    if (box.z >= box.x)
        box = glm::ivec4(glm::max(box.x - c_boxMargin, 0), glm::max(box.y - c_boxMargin, 0), glm::min(box.z + c_boxMargin, Size - 1),
            glm::min(box.w + c_boxMargin, Size - 1));
    m_box = box;
    m_row = box.y;
}

bool FactionLayer::tick(float deltaSec, glm::ivec4& outDirty)
{
    if (!m_valid)
        return false;
    const GameFactionSettings& s = Globals::settings.game.faction;
    const float sweepSec = glm::max(s.sweepSeconds, 0.05f);
    if (m_stampDue)
    {
        stampSources();
        m_stampDue = false;
    }
    if (m_box.z < m_box.x) // no source on the grid: nothing moves. Look again next frame (a stamp of no sources is free)
    {
        m_stampDue = true;
        return false;
    }
    const int boxRows = m_box.w - m_box.y + 1;
    m_rowCarry += (float)boxRows * glm::max(deltaSec, 0.0f) / sweepSec;
    int rows = (int)m_rowCarry;
    m_rowCarry -= (float)rows;
    rows = glm::min(rows, m_box.w + 1 - m_row);

    // One explicit step of a cell's sweep interval; both rates are clamped, so a long interval cannot overshoot.
    const float diffuse = glm::clamp(s.diffusion * sweepSec, 0.0f, 0.9f);
    const float pull = sweepSec / glm::max(s.sourceSeconds, 0.01f) * (1.0f / 127.0f);
    const int x0 = m_box.x, x1 = m_box.z;
    for (int z = m_row; z < m_row + rows; ++z)
    {
        const uint16* up = m_value.data() + (size_t)glm::max(z - 1, 0) * Size;
        const uint16* down = m_value.data() + (size_t)glm::min(z + 1, Size - 1) * Size;
        uint16* row = m_value.data() + (size_t)z * Size;
        const int8* sourceRow = m_source.data() + (size_t)z * Size;
        uint8* byteRow = m_bytes.data() + (size_t)z * Size;
        int changedMin = Size, changedMax = -1;
        for (int x = x0; x <= x1; ++x)
        {
            const uint32 here = row[x];
            const uint32 sum = (uint32)row[glm::max(x - 1, 0)] + (uint32)row[glm::min(x + 1, Size - 1)] + (uint32)up[x] + (uint32)down[x];
            const int source = sourceRow[x];
            // Land at rest: no source on it and the same value all around. Most of the box, most of the time.
            if (source == 0 && sum == here * 4u)
                continue;
            float v = (float)here;
            v += ((float)sum * 0.25f - v) * diffuse;
            if (source != 0)
                v += ((source > 0 ? 65535.0f : 0.0f) - v) * glm::min((float)glm::abs(source) * pull, 1.0f);
            const uint16 next = (uint16)glm::clamp(v + 0.5f, 0.0f, 65535.0f);
            row[x] = next;
            const uint8 byte = (uint8)((next + 128u) / 257u);
            if (byte != byteRow[x])
            {
                byteRow[x] = byte;
                changedMin = glm::min(changedMin, x);
                changedMax = x;
            }
        }
        if (changedMax >= 0)
            m_dirty = glm::ivec4(glm::min(m_dirty.x, changedMin), glm::min(m_dirty.y, z), glm::max(m_dirty.z, changedMax), glm::max(m_dirty.w, z));
    }
    m_row += rows;
    if (m_row <= m_box.w)
        return false;
    m_rowCarry = 0.0f;
    m_stampDue = true;
    const bool changed = m_dirty.z >= m_dirty.x; // a sweep that moved no byte sends nothing
    outDirty = m_dirty;
    m_dirty = glm::ivec4(Size, Size, -1, -1);
    return changed;
}
