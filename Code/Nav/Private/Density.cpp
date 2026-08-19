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

glm::vec2 FlowField::sample(const glm::vec2& xz, const TeamField* raster) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = readBuffer();
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    glm::vec2 sum(0.0f);
    float weight = 0.0f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            if (raster && raster->isBlocked(n))
                continue; // a wall is not "no flow here", it is not part of the neighbourhood
            const float w = (dx | dz) ? 0.5f : 1.0f; // centre weighs double
            weight += w;                             // an OPEN cell counts even with no chunk yet
            const glm::ivec2 nc = chunkOf(n);
            if (nc != cachedCoord)
            {
                cachedCoord = nc;
                cached = m_chunks.find(chunkKey(nc));
            }
            if (!cached)
                continue;
            const uint32 i = cellIndex(n);
            sum += glm::vec2(cached->vx[buffer][i], cached->vz[buffer][i]) * w;
        }
    return weight > 0.0f ? sum / (Scale * weight) : glm::vec2(0.0f);
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

// Per-step multiplier for a half-life: 0.5^(dt/halfLife) — the frame-rate-independent form of a
// "x% per frame" decay.
static float fadeStep(float deltaSec, float halfLifeSec)
{
    return std::exp2(-deltaSec / glm::max(halfLifeSec, 1e-3f));
}

void FlowField::beginStep(float deltaSec, uint32 keepFrames, float halfLifeSec, const TeamField* raster,
    float maxSpeed, oc::vector<StepItem>& outItems)
{
    ++m_frame;
    m_stepDecay = fadeStep(deltaSec, halfLifeSec);
    m_splatGain = glm::max(1.0f - m_stepDecay, 0.0005f);
    m_stepRaster = raster;
    m_stepMaxSpeed = maxSpeed;
    if (m_touch.isInitialized())
        m_touch.forEach([&](oc::vector<uint64>& keys)
        {
            for (const uint64 key : keys)
                m_chunks.getOrCreate(key).touchedFrame = m_frame;
            keys.clear();
        });
    // The buffer just written becomes the READ buffer; the next WRITE buffer starts as the read
    // buffer decayed, so a lane keeps its direction for a while after the units passed. Eviction
    // runs HERE (the step never writes touchedFrame, so before/after is the same set) — the map
    // must not mutate once the fan-out starts reading across chunks.
    m_write ^= 1u;
    const uint32 cutoff = m_frame > keepFrames ? m_frame - keepFrames : 0;
    m_chunks.eraseIf([&](uint64, Chunk& chunk) { return chunk.touchedFrame < cutoff; });
    for (uint32 slot = 0; slot < m_chunks.capacity(); ++slot)
        if (m_chunks.slotKey(slot) != InvalidChunkKey)
            outItems.push_back(StepItem{ this, m_chunks.slotKey(slot), m_chunks.slotValue(slot) });
}

void FlowField::stepChunk(uint64 key, Chunk& chunk)
{
    const TeamField* raster = m_stepRaster;
    const uint32 read = m_write ^ 1u;
    const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
    for (int i = 0; i < ChunkArea; ++i)
    {
        glm::vec2 v(chunk.vx[read][i], chunk.vz[read][i]);
        if (raster && raster->isBlocked(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits)))
        {
            // A wall cell holds NOTHING: whatever landed there before the cell was built over
            // is dropped, so nothing can ever carry momentum back out of a wall.
            chunk.vx[m_write][i] = 0;
            chunk.vz[m_write][i] = 0;
            continue;
        }
        v *= m_stepDecay;
        if (m_stepMaxSpeed > 0.0f) // a milling crowd must not out-shout a seeded lane
        {
            const float cap = m_stepMaxSpeed * Scale;
            const float len = glm::length(v);
            if (len > cap)
                v *= cap / len;
        }
        chunk.vx[m_write][i] = int16(glm::clamp(v.x, -32767.0f, 32767.0f));
        chunk.vz[m_write][i] = int16(glm::clamp(v.y, -32767.0f, 32767.0f));
    }
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

void FlowField::seedPath(oc::span<const glm::vec2> path, float speed, float radius, const TeamField* raster)
{
    if (path.size() < 2 || speed <= 0.0f)
        return;
    const auto write = [&](const glm::vec2& p, const glm::vec2& v)
    {
        const glm::ivec2 c = cellOf(p);
        if (raster && raster->isBlocked(c))
            return; // never seed flow into a wall — nothing can stand there to follow it
        Chunk& chunk = m_chunks.getOrCreate(chunkKey(chunkOf(c)));
        chunk.touchedFrame = m_frame;
        const uint32 i = cellIndex(c);
        // Max magnitude, EXCEPT when the plan disagrees with what is there: re-seeding the same
        // route is idempotent and a strong live lane is never weakened, but a re-plan that goes a
        // DIFFERENT way must win even where a milling crowd's own splats sum higher than the lane
        // speed — otherwise a group's periodic re-seed silently writes nothing exactly where the
        // group is standing.
        for (uint32 b = 0; b < 2; ++b)
        {
            const glm::vec2 cur(chunk.vx[b][i], chunk.vz[b][i]);
            const glm::vec2 next = v * Scale;
            const float curLen = glm::length(cur);
            const bool stronger = glm::dot(next, next) > glm::dot(cur, cur);
            const bool disagrees = curLen > 1e-3f && glm::dot(next, cur) < 0.5f * glm::length(next) * curLen;
            if (stronger || disagrees) // 0.5 = more than 60 deg apart
            {
                chunk.vx[b][i] = int16(glm::clamp(next.x, -32767.0f, 32767.0f));
                chunk.vz[b][i] = int16(glm::clamp(next.y, -32767.0f, 32767.0f));
            }
        }
    };
    // Segment directions up front: the WRITTEN vector near a junction is a blend of the two
    // segments meeting there, easing over c_turnBlend metres on each side. The polyline itself is
    // untouched (a rounded path could cut a wall corner) — only the flow VECTORS turn gradually,
    // so a 90 deg corner in the route reads as an arc to anything following it.
    static constexpr float c_turnBlend = 2.0f;
    oc::fixed_vector<glm::vec2, 64> dirs;
    oc::fixed_vector<float, 64> lens;
    for (size_t s = 0; s + 1 < path.size(); ++s)
    {
        const glm::vec2 seg = path[s + 1] - path[s];
        const float len = glm::length(seg);
        dirs.push_back(len > 1e-3f ? seg / len : glm::vec2(0.0f));
        lens.push_back(len);
    }
    const int lateral = int(glm::max(radius, 0.0f) / CellSize);
    for (size_t s = 0; s + 1 < path.size() && s < dirs.size(); ++s)
    {
        const float len = lens[s];
        if (len < 1e-3f)
            continue;
        const glm::vec2 a = path[s], dir = dirs[s];
        for (float t = 0.0f; t <= len; t += CellSize * 0.5f)
        {
            // Half-weight toward the neighbouring segment AT the junction, fading to none
            // c_turnBlend metres away — the two sides of a corner meet at the bisector.
            glm::vec2 v = dir;
            if (s > 0 && t < c_turnBlend)
                v += dirs[s - 1] * (0.5f * (1.0f - t / c_turnBlend));
            if (s + 2 < path.size() && len - t < c_turnBlend)
                v += dirs[s + 1] * (0.5f * (1.0f - (len - t) / c_turnBlend));
            const float vl = glm::length(v);
            v = vl > 1e-3f ? v / vl : dir;
            const glm::vec2 p = a + dir * t;
            // NEVER write a vector that aims at a wall. At a one-cell gap the junction blend points
            // diagonally, i.e. at the wall BESIDE the opening, and the flow term is strong enough
            // that units then follow it into that wall instead of through the gap. Fall back to the
            // pure segment direction, then to the straight line to the segment's end.
            if (raster && raster->isBlocked(cellOf(p + v * CellSize)))
            {
                if (!raster->isBlocked(cellOf(p + dir * CellSize)))
                    v = dir;
                else
                {
                    const glm::vec2 toEnd = path[s + 1] - p;
                    const float el = glm::length(toEnd);
                    if (el > 1e-3f && !raster->isBlocked(cellOf(p + toEnd / el * CellSize)))
                        v = toEnd / el;
                }
            }
            const glm::vec2 side(-v.y, v.x);
            // CONSTRICTION BOOST: where the lane squeezes between walls the surrounding cells hold
            // no flow to support it and a crowd waiting at the mouth is splatting its own, so the
            // one cell that matters is written harder than the open stretches.
            const bool squeeze = raster && (raster->isBlocked(cellOf(p + side * CellSize))
                || raster->isBlocked(cellOf(p - side * CellSize)));
            // FOCUS: blocked lateral cells hand their share to the surviving ones, so a wide lane
            // squeezing past a wall concentrates its strength on the centre instead of thinning
            // out (capped — a one-cell gap must not write an absurd magnitude).
            int open = 0;
            for (int l = -lateral; l <= lateral; ++l)
                if (!(raster && raster->isBlocked(cellOf(p + side * (float(l) * CellSize)))))
                    ++open;
            const float focus = open > 0
                ? glm::min(float(2 * lateral + 1) / float(open), 2.0f) : 1.0f;
            const float write_speed = speed * (squeeze ? 1.6f : 1.0f) * focus;
            for (int l = -lateral; l <= lateral; ++l)
                write(p + side * (float(l) * CellSize), v * write_speed);
        }
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

glm::vec2 PressureField::gradient(const glm::vec2& xz, const TeamField* raster) const
{
    const glm::ivec2 c = cellOf(xz);
    const uint32 buffer = m_write;
    glm::ivec2 cachedCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(cachedCoord));
    const float here = cellValue(c, buffer, cached, cachedCoord);
    const auto at = [&](const glm::ivec2& o)
    {
        const glm::ivec2 n = c + o;
        return (raster && raster->isBlocked(n)) ? here : cellValue(n, buffer, cached, cachedCoord);
    };
    const float xp = at(glm::ivec2(1, 0)), xm = at(glm::ivec2(-1, 0));
    const float zp = at(glm::ivec2(0, 1)), zm = at(glm::ivec2(0, -1));
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

void PressureField::beginStep(float deltaSec, const TeamField* raster, float diffusionPerSec,
    float halfLifeSec, uint32 keepFrames, float propagationFloor, oc::vector<StepItem>& outItems)
{
    ++m_frame;
    m_stepDecay = fadeStep(deltaSec, halfLifeSec);
    m_stepDiffusion = glm::clamp(diffusionPerSec * deltaSec * 60.0f, 0.0f, 0.25f); // Jacobi stability
    m_stepFloor = propagationFloor;
    m_stepRaster = raster;
    if (m_touch.isInitialized())
        m_touch.forEach([&](oc::vector<uint64>& keys)
        {
            for (const uint64 key : keys)
                m_chunks.getOrCreate(key).touchedFrame = m_frame;
            keys.clear();
        });
    // Evict FIRST — one frame later than evicting on this step's result, invisible at any sane
    // keep window — so the map never mutates while the fan-out reads across chunks.
    const uint32 cutoff = m_frame > keepFrames ? m_frame - keepFrames : 0;
    m_chunks.eraseIf([&](uint64, Chunk& chunk) { return chunk.touchedFrame < cutoff; });
    // SERIAL snapshot for the quiet-skip: during the parallel step a neighbour's peak/touchedFrame
    // are being rewritten by its own task (and would be post-step values anyway).
    for (uint32 slot = 0; slot < m_chunks.capacity(); ++slot)
        if (m_chunks.slotKey(slot) != InvalidChunkKey)
        {
            Chunk* chunk = m_chunks.slotValue(slot);
            chunk->prevActive = (chunk->peak > 0.0f || chunk->touchedFrame == m_frame) ? 1 : 0;
            outItems.push_back(StepItem{ this, m_chunks.slotKey(slot), chunk });
        }
}

void PressureField::stepChunk(uint64 key, Chunk& chunk, const CellVisit* onActiveCell)
{
    static constexpr float c_eps = 0.02f;
    static constexpr glm::ivec2 c_n4[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    const TeamField* raster = m_stepRaster;
    // The buffer units wrote this frame (injections landed on top of last frame's diffused state)
    // is the SOURCE; the step writes the other buffer, which endStep() then makes current.
    const uint32 src = m_write;
    const uint32 dst = m_write ^ 1u;
    const glm::ivec2 coord = chunkFromKey(key);
    const glm::ivec2 base = coord * ChunkCells;
    // Everything this chunk can touch, resolved ONCE: its four neighbour chunks and the five
    // matching raster chunks (the raster uses the same cell/chunk lattice, so a cell's cost is
    // a plain array index).
    Chunk* nbr[4];
    const TeamField::Chunk* rasterNbr[4];
    for (int d = 0; d < 4; ++d)
    {
        const uint64 nKey = chunkKey(coord + c_n4[d]);
        nbr[d] = m_chunks.find(nKey);
        rasterNbr[d] = raster ? raster->chunks().find(nKey) : nullptr;
    }
    const TeamField::Chunk* rasterSelf = raster ? raster->chunks().find(key) : nullptr;

    // QUIET SKIP: a chunk that held nothing last step, took no injection this frame and has no
    // active neighbour cannot produce anything — zero its output and move on. Most chunks of a
    // large field are quiet most of the time, which is what makes the whole pass cheap.
    bool quiet = !chunk.prevActive;
    for (int d = 0; d < 4 && quiet; ++d)
        if (nbr[d] && nbr[d]->prevActive)
            quiet = false;
    if (quiet)
    {
        memset(chunk.p[dst], 0, sizeof(chunk.p[dst]));
        chunk.peak = 0.0f;
        return;
    }

    // Neighbour resolution by INDEX: inside the chunk it is i+-1 / i+-16, only the border
    // steps into the neighbour chunk fetched above.
    struct Neighbour { const Chunk* p; const TeamField::Chunk* r; int i; };
    const auto neighbourAt = [&](int d, int i, int cx, int cz) -> Neighbour
    {
        switch (d)
        {
        case 0: return cx < ChunkCells - 1 ? Neighbour{ &chunk, rasterSelf, i + 1 }
                                           : Neighbour{ nbr[0], rasterNbr[0], cz << ChunkBits };
        case 1: return cx > 0 ? Neighbour{ &chunk, rasterSelf, i - 1 }
                              : Neighbour{ nbr[1], rasterNbr[1], (cz << ChunkBits) | (ChunkCells - 1) };
        case 2: return cz < ChunkCells - 1 ? Neighbour{ &chunk, rasterSelf, i + ChunkCells }
                                           : Neighbour{ nbr[2], rasterNbr[2], cx };
        default: return cz > 0 ? Neighbour{ &chunk, rasterSelf, i - ChunkCells }
                               : Neighbour{ nbr[3], rasterNbr[3], ((ChunkCells - 1) << ChunkBits) | cx };
        }
    };

    float peak = 0.0f;
    for (int i = 0; i < ChunkArea; ++i)
    {
        const int cx = i & (ChunkCells - 1), cz = i >> ChunkBits;
        const float here = chunk.p[src][i];
        if (rasterSelf && rasterSelf->cost[i] == TeamField::Blocked)
        {
            chunk.p[dst][i] = 0.0f; // walls hold no pressure (and reflect it: neighbours read
            continue;               // their own value where the neighbour is a wall)
        }
        float nv[4];
        float lap = 0.0f;
        for (int d = 0; d < 4; ++d)
        {
            const Neighbour n = neighbourAt(d, i, cx, cz);
            const bool wall = n.r && n.r->cost[n.i] == TeamField::Blocked;
            nv[d] = wall ? here : (n.p ? n.p->p[src][n.i] : 0.0f); // no-flux: a wall reads as us
            if (wall || glm::abs(nv[d]) < m_stepFloor)
                continue; // below the floor: too faint to propagate, and it may not drain us
            lap += nv[d] - here;
        }
        const float v = (here + m_stepDiffusion * lap) * m_stepDecay;
        chunk.p[dst][i] = glm::abs(v) < c_eps * 0.1f ? 0.0f : v; // abs: troughs are negative
        peak = glm::max(peak, glm::abs(v));
        if (glm::abs(v) <= c_eps)
            continue;
        if (onActiveCell) // the gradient is already in hand — see the header
            (*onActiveCell)(cellCenter(base + glm::ivec2(cx, cz)),
                glm::vec2(nv[0] - nv[1], nv[2] - nv[3]) / (2.0f * CellSize));
        // Growth: pressure at a border cell wants to cross into a chunk that may not exist.
        // Queued through the worker-safe touch queue; the chunk exists before the next step.
        if (m_touch.isInitialized())
        {
            if (cx == 0 && !nbr[1]) m_touch.local().push_back(chunkKey(coord + c_n4[1]));
            if (cx == ChunkCells - 1 && !nbr[0]) m_touch.local().push_back(chunkKey(coord + c_n4[0]));
            if (cz == 0 && !nbr[3]) m_touch.local().push_back(chunkKey(coord + c_n4[3]));
            if (cz == ChunkCells - 1 && !nbr[2]) m_touch.local().push_back(chunkKey(coord + c_n4[2]));
        }
    }
    chunk.peak = peak;
    if (peak > c_eps)
        chunk.touchedFrame = m_frame;
}

void PressureField::seedPath(oc::span<const glm::vec2> path, float amount, float radius, const TeamField* raster,
    float squeezeGain)
{
    if (path.size() < 2 || amount <= 0.0f)
        return;
    // A GAP is walls on BOTH sides ACROSS the lane; a lane merely running along one wall is open
    // ground and gets the plain depth. Probing out to c_gapProbe cells each way also grades it: the
    // tighter the channel, the deeper the trough (a one-cell gap is the deepest point of a route).
    static constexpr int c_gapProbe = 2;
    const auto dig = [&](const glm::vec2& p, const glm::vec2& along)
    {
        const glm::ivec2 c = cellOf(p);
        if (raster && raster->isBlocked(c))
            return;
        float depth = amount;
        if (raster && squeezeGain > 0.0f)
        {
            const glm::ivec2 sideCell = glm::abs(along.x) > glm::abs(along.y) ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
            int left = 0, right = 0; // open cells before hitting a wall, each capped at c_gapProbe
            while (left < c_gapProbe && !raster->isBlocked(c + sideCell * (left + 1)))
                ++left;
            while (right < c_gapProbe && !raster->isBlocked(c - sideCell * (right + 1)))
                ++right;
            if (left < c_gapProbe && right < c_gapProbe) // walls on BOTH sides: a channel
                depth *= 1.0f + squeezeGain * float(2 * c_gapProbe - left - right);
        }
        Chunk& chunk = m_chunks.getOrCreate(chunkKey(chunkOf(c)));
        chunk.touchedFrame = m_frame;
        float& cell = chunk.p[m_write][cellIndex(c)];
        cell = glm::min(cell, -depth);
    };
    for (size_t s = 0; s + 1 < path.size(); ++s)
    {
        const glm::vec2 a = path[s], b = path[s + 1];
        const glm::vec2 seg = b - a;
        const float len = glm::length(seg);
        if (len < 1e-3f)
            continue;
        const glm::vec2 dir = seg / len;
        const glm::vec2 side(-dir.y, dir.x);
        const int lateral = int(glm::max(radius, 0.0f) / CellSize);
        for (float t = 0.0f; t <= len; t += CellSize * 0.5f)
            for (int l = -lateral; l <= lateral; ++l)
                dig(a + dir * t + side * (float(l) * CellSize), dir);
    }
}

void PressureField::clear()
{
    m_chunks.reset(64);
    if (m_touch.isInitialized())
        m_touch.forEach([](oc::vector<uint64>& keys) { keys.clear(); });
}

}
