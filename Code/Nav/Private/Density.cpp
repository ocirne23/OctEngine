module Nav;

import Core;
import Core.glm;
import Threading;

namespace Nav
{

void FlowField::initialize()
{
    if (!m_touch.isInitialized())
        m_touch.initialize();
}

static void atomicAddSat16(int16& value, int amount)
{
    oc::atomic_ref<int16> ref(value);
    int16 cur = ref.load(oc::memory_order_relaxed);
    while (true)
    {
        const int16 next = int16(glm::clamp(int(cur) + amount, -32767, 32767));
        if (ref.compare_exchange_weak(cur, next, oc::memory_order_relaxed))
            return;
    }
}

void FlowField::splat(const glm::vec2& xz, const glm::vec2& velocity)
{
    const glm::ivec2 c = cellOf(xz);
    const uint64 key = chunkKey(chunkOf(c));
    Chunk* chunk = m_chunks.find(key);
    if (!chunk)
    {
        if (m_touch.isInitialized())
            m_touch.local().push_back(key);
        return;
    }
    const uint32 i = cellIndex(c);
    atomicAddSat16(chunk->vx[m_write][i], int(velocity.x * m_splatGain * Scale));
    atomicAddSat16(chunk->vz[m_write][i], int(velocity.y * m_splatGain * Scale));
    oc::atomic_ref<uint32>(chunk->touchedFrame).store(m_frame, oc::memory_order_relaxed);
}

glm::vec2 FlowField::sample(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = readBuffer();
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    glm::vec2 sum(0.0f);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            const glm::ivec2 nc = chunkOf(n);
            if (nc != cachedCoord)
            {
                cachedCoord = nc;
                cached = m_chunks.find(chunkKey(nc));
            }
            if (!cached)
                continue;
            const uint32 i = cellIndex(n);
            const float w = (dx | dz) ? 0.5f : 1.0f; // centre weighs double
            sum += glm::vec2(cached->vx[buffer][i], cached->vz[buffer][i]) * w;
        }
    return sum / (Scale * 5.0f); // 1 + 8*0.5 weights
}

glm::vec2 FlowField::sampleArea(const glm::vec2& xz, int radiusCells, const TeamField* raster) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = readBuffer();
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    glm::vec2 sum(0.0f);
    int count = 0;
    for (int dz = -radiusCells; dz <= radiusCells; ++dz)
        for (int dx = -radiusCells; dx <= radiusCells; ++dx)
        {
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            if (raster && raster->isBlocked(n))
                continue;
            const glm::ivec2 nc = chunkOf(n);
            if (nc != cachedCoord)
            {
                cachedCoord = nc;
                cached = m_chunks.find(chunkKey(nc));
            }
            ++count;
            if (!cached)
                continue;
            const uint32 i = cellIndex(n);
            sum += glm::vec2(cached->vx[buffer][i], cached->vz[buffer][i]);
        }
    return count > 0 ? sum / (Scale * float(count)) : glm::vec2(0.0f);
}

void FlowField::update(uint32 keepFrames, float decay, const TeamField* raster, float wallBounce)
{
    ++m_frame;
    m_splatGain = glm::max(1.0f - decay, 0.0005f);
    if (m_touch.isInitialized())
        m_touch.forEach([&](oc::vector<uint64>& keys)
        {
            for (const uint64 key : keys)
                m_chunks.getOrCreate(key).touchedFrame = m_frame;
            keys.clear();
        });
    // The buffer just written becomes the READ buffer; the next WRITE buffer starts as the read
    // buffer decayed, so a lane keeps its direction for a while after the units passed.
    m_write ^= 1u;
    const uint32 read = m_write ^ 1u;
    const uint32 cutoff = m_frame > keepFrames ? m_frame - keepFrames : 0;
    static constexpr glm::ivec2 c_n4[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    m_chunks.eraseIf([&](uint64 key, Chunk& chunk)
    {
        if (chunk.touchedFrame < cutoff)
            return true;
        const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
        for (int i = 0; i < ChunkArea; ++i)
        {
            glm::vec2 v(chunk.vx[read][i], chunk.vz[read][i]);
            if (raster && (v.x != 0.0f || v.y != 0.0f))
            {
                const glm::ivec2 c = base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits);
                for (const glm::ivec2& o : c_n4)
                {
                    if (!raster->isBlocked(c + o))
                        continue;
                    const glm::vec2 n(float(o.x), float(o.y)); // into the wall
                    const float into = glm::dot(v, n);
                    if (into > 0.0f)
                        v -= n * (into * (1.0f + wallBounce));
                }
            }
            chunk.vx[m_write][i] = int16(glm::clamp(v.x * decay, -32767.0f, 32767.0f));
            chunk.vz[m_write][i] = int16(glm::clamp(v.y * decay, -32767.0f, 32767.0f));
        }
        return false;
    });
}

void FlowField::clearArea(const glm::vec2& xz, float radius)
{
    const glm::ivec2 lo = cellOf(xz - radius), hi = cellOf(xz + radius);
    const float r2 = radius * radius;
    for (int z = lo.y; z <= hi.y; ++z)
        for (int x = lo.x; x <= hi.x; ++x)
        {
            const glm::ivec2 c(x, z);
            const glm::vec2 d = cellCenter(c) - xz;
            if (glm::dot(d, d) > r2)
                continue;
            Chunk* chunk = m_chunks.find(chunkKey(chunkOf(c)));
            if (!chunk)
                continue;
            const uint32 i = cellIndex(c);
            chunk->vx[0][i] = chunk->vz[0][i] = 0;
            chunk->vx[1][i] = chunk->vz[1][i] = 0;
        }
}

void FlowField::clear()
{
    m_chunks.reset(64);
    if (m_touch.isInitialized())
        m_touch.forEach([](oc::vector<uint64>& keys) { keys.clear(); });
}

}

namespace Nav
{

void PressureField::initialize()
{
    if (!m_touch.isInitialized())
        m_touch.initialize();
}

void PressureField::inject(const glm::vec2& xz, float amount)
{
    const glm::ivec2 c = cellOf(xz);
    const uint64 key = chunkKey(chunkOf(c));
    Chunk* chunk = m_chunks.find(key);
    if (!chunk)
    {
        if (m_touch.isInitialized())
            m_touch.local().push_back(key);
        return;
    }
    oc::atomic_ref<float> ref(chunk->p[m_write][cellIndex(c)]);
    float cur = ref.load(oc::memory_order_relaxed);
    while (!ref.compare_exchange_weak(cur, cur + amount, oc::memory_order_relaxed)) {}
    oc::atomic_ref<uint32>(chunk->touchedFrame).store(m_frame, oc::memory_order_relaxed);
}

float PressureField::cellValue(const glm::ivec2& cell, uint32 buffer, const Chunk*& cached, glm::ivec2& cachedCoord) const
{
    const glm::ivec2 cc = chunkOf(cell);
    if (cc != cachedCoord)
    {
        cachedCoord = cc;
        cached = m_chunks.find(chunkKey(cc));
    }
    return cached ? cached->p[buffer][cellIndex(cell)] : 0.0f;
}

float PressureField::value(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const Chunk* chunk = m_chunks.find(chunkKey(chunkOf(c)));
    return chunk ? chunk->p[m_write][cellIndex(c)] : 0.0f;
}

glm::vec2 PressureField::gradient(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = m_write;
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    const float xp = cellValue(c + glm::ivec2(1, 0), buffer, cached, cachedCoord);
    const float xm = cellValue(c - glm::ivec2(1, 0), buffer, cached, cachedCoord);
    const float zp = cellValue(c + glm::ivec2(0, 1), buffer, cached, cachedCoord);
    const float zm = cellValue(c - glm::ivec2(0, 1), buffer, cached, cachedCoord);
    return glm::vec2(xp - xm, zp - zm) / (2.0f * CellSize);
}

bool PressureField::lowestNearby(const glm::vec2& xz, int radiusCells, const TeamField* raster, glm::vec2& outCentre) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = m_write;
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    float best = FLT_MAX;
    bool found = false;
    for (int dz = -radiusCells; dz <= radiusCells; ++dz)
        for (int dx = -radiusCells; dx <= radiusCells; ++dx)
        {
            if (!(dx | dz))
                continue;
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            if (raster && raster->isBlocked(n))
                continue;
            const glm::ivec2 nc = chunkOf(n);
            if (nc != cachedCoord)
            {
                cachedCoord = nc;
                cached = m_chunks.find(chunkKey(nc));
            }
            // Slight preference for nearer cells on ties (an absent chunk = zero pressure).
            const float v = (cached ? cached->p[buffer][cellIndex(n)] : 0.0f) + 0.001f * float(dx * dx + dz * dz);
            if (v < best)
            {
                best = v;
                outCentre = cellCenter(n);
                found = true;
            }
        }
    return found;
}

void PressureField::update(const TeamField* raster, float diffusion, float decay, uint32 keepFrames)
{
    ++m_frame;
    if (m_touch.isInitialized())
        m_touch.forEach([&](oc::vector<uint64>& keys)
        {
            for (const uint64 key : keys)
                m_chunks.getOrCreate(key).touchedFrame = m_frame;
            keys.clear();
        });
    // The buffer units wrote this frame (injections landed on top of last frame's diffused
    // state) is the SOURCE of this step; the step writes the other buffer, which then becomes
    // the read buffer for the coming entity pass AND the base the next injections add onto.
    const uint32 src = m_write;
    const uint32 dst = m_write ^ 1u;
    static constexpr float c_eps = 0.02f;
    oc::vector<uint64> grow;
    m_chunks.forEachMutable([&](uint64 key, Chunk& chunk)
    {
        const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
        float peak = 0.0f;
        for (int i = 0; i < ChunkArea; ++i)
        {
            const glm::ivec2 c = base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits);
            const float here = chunk.p[src][i];
            if (raster && raster->isBlocked(c))
            {
                chunk.p[dst][i] = 0.0f; // walls hold no pressure (and reflect it: neighbours read
                continue;               // their own value where the neighbour is a wall)
            }
            float lap = 0.0f;
            glm::ivec2 cachedCoord = chunkOf(c);
            const Chunk* cached = &chunk;
            static constexpr glm::ivec2 c_n4[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const glm::ivec2& o : c_n4)
            {
                const glm::ivec2 n = c + o;
                const bool wall = raster && raster->isBlocked(n);
                lap += (wall ? here : cellValue(n, src, cached, cachedCoord)) - here;
            }
            const float v = (here + diffusion * lap) * decay;
            chunk.p[dst][i] = v < c_eps * 0.1f ? 0.0f : v;
            peak = glm::max(peak, v);
            // Growth: pressure at a border cell wants to cross into a chunk that may not exist.
            if (v > c_eps)
            {
                const int cx = i & (ChunkCells - 1), cz = i >> ChunkBits;
                if (cx == 0) grow.push_back(chunkKey(chunkOf(c) + glm::ivec2(-1, 0)));
                if (cx == ChunkCells - 1) grow.push_back(chunkKey(chunkOf(c) + glm::ivec2(1, 0)));
                if (cz == 0) grow.push_back(chunkKey(chunkOf(c) + glm::ivec2(0, -1)));
                if (cz == ChunkCells - 1) grow.push_back(chunkKey(chunkOf(c) + glm::ivec2(0, 1)));
            }
        }
        chunk.peak = peak;
        if (peak > c_eps)
            chunk.touchedFrame = m_frame;
    });
    for (const uint64 key : grow)
        m_chunks.getOrCreate(key).touchedFrame = m_frame; // fresh chunk: zeros in both buffers
    m_write = dst; // ONE current buffer: units read it AND inject into it next pass
    const uint32 cutoff = m_frame > keepFrames ? m_frame - keepFrames : 0;
    m_chunks.eraseIf([&](uint64, Chunk& chunk) { return chunk.touchedFrame < cutoff; });
}

void PressureField::clear()
{
    m_chunks.reset(64);
    if (m_touch.isInitialized())
        m_touch.forEach([](oc::vector<uint64>& keys) { keys.clear(); });
}

}
