module Nav;

import Core;
import Core.glm;
import Threading;

namespace Nav
{

void DensityField::initialize()
{
    if (!m_touch.isInitialized())
        m_touch.initialize();
}

void DensityField::splat(const glm::vec2& xz, uint16 amount)
{
    const glm::ivec2 c = cellOf(xz);
    const uint64 key = chunkKey(chunkOf(c));
    Chunk* chunk = m_chunks.find(key);
    if (!chunk)
    {
        if (m_touch.isInitialized())
            m_touch.local().push_back(key); // created by update() — one frame of missing spread
        return;
    }
    oc::atomic_ref<uint16> ref(chunk->v[m_write][cellIndex(c)]);
    uint16 cur = ref.load(oc::memory_order_relaxed);
    while (true)
    {
        const uint16 next = uint16(glm::min<uint32>(uint32(cur) + amount, 0xFFFFu));
        if (ref.compare_exchange_weak(cur, next, oc::memory_order_relaxed))
            break;
    }
    oc::atomic_ref<uint32>(chunk->touchedFrame).store(m_frame, oc::memory_order_relaxed);
}

uint16 DensityField::cellValue(const glm::ivec2& cell, uint32 buffer, const Chunk*& cached, glm::ivec2& cachedCoord) const
{
    const glm::ivec2 cc = chunkOf(cell);
    if (cc != cachedCoord)
    {
        cachedCoord = cc;
        cached = m_chunks.find(chunkKey(cc));
    }
    return cached ? cached->v[buffer][cellIndex(cell)] : 0;
}

glm::vec2 DensityField::gradient(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = readBuffer();
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    const float xp = cellValue(c + glm::ivec2(1, 0), buffer, cached, cachedCoord);
    const float xm = cellValue(c - glm::ivec2(1, 0), buffer, cached, cachedCoord);
    const float zp = cellValue(c + glm::ivec2(0, 1), buffer, cached, cachedCoord);
    const float zm = cellValue(c - glm::ivec2(0, 1), buffer, cached, cachedCoord);
    return glm::vec2(xp - xm, zp - zm) / (2.0f * CellSize);
}

float DensityField::valueAt(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const Chunk* chunk = m_chunks.find(chunkKey(chunkOf(c)));
    return chunk ? float(chunk->v[readBuffer()][cellIndex(c)]) : 0.0f;
}

void DensityField::update(uint32 keepFrames)
{
    ++m_frame;
    if (m_touch.isInitialized())
        m_touch.forEach([&](oc::vector<uint64>& keys)
        {
            for (const uint64 key : keys)
                m_chunks.getOrCreate(key).touchedFrame = m_frame;
            keys.clear();
        });
    m_write ^= 1u;
    const uint32 cutoff = m_frame > keepFrames ? m_frame - keepFrames : 0;
    m_chunks.eraseIf([&](uint64, Chunk& chunk)
    {
        if (chunk.touchedFrame < cutoff)
            return true;
        memset(chunk.v[m_write], 0, sizeof(chunk.v[m_write]));
        return false;
    });
}

void DensityField::clear()
{
    m_chunks.reset(64);
    if (m_touch.isInitialized())
        m_touch.forEach([](oc::vector<uint64>& keys) { keys.clear(); });
}

}
