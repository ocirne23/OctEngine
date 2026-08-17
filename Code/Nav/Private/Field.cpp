module Nav;

import Core;
import Core.glm;

namespace Nav
{

uint8 TeamField::costAt(const glm::ivec2& cell) const
{
    const Chunk* chunk = findChunk(cell);
    return chunk ? chunk->cost[cellIndex(cell)] : 0; // no chunk yet = open ground
}

void TeamField::rasterizeObstacles(oc::span<const NavObstacle> obstacles, uint8 clearanceCost)
{
    // Blocked cells first, then a clearance ring: cells touching a blocked one cost extra so the
    // descent hugs the middle of a gap instead of scraping the wall (units have a body radius).
    for (const NavObstacle& o : obstacles)
    {
        const glm::ivec2 lo = cellOf(o.min + 1e-3f);
        const glm::ivec2 hi = cellOf(o.max - 1e-3f);
        for (int z = lo.y; z <= hi.y; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const glm::ivec2 c(x, z);
                chunkAt(c).cost[cellIndex(c)] = Blocked;
            }
    }
    if (clearanceCost == 0)
        return;
    oc::vector<glm::ivec2> ring;
    m_chunks.forEach([&](uint64 key, const Chunk& chunk)
    {
        const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
        for (int i = 0; i < ChunkArea; ++i)
        {
            if (chunk.cost[i] != Blocked)
                continue;
            const glm::ivec2 c = base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx | dz)
                        ring.push_back(c + glm::ivec2(dx, dz));
        }
    });
    for (const glm::ivec2& c : ring)
    {
        uint8& cost = chunkAt(c).cost[cellIndex(c)];
        if (cost != Blocked)
            cost = glm::max(cost, clearanceCost);
    }
}

namespace
{
    struct HeapNode
    {
        uint32 dist;
        glm::ivec2 cell;
        uint16 src;
        bool operator>(const HeapNode& o) const { return dist > o.dist; }
    };
}

void TeamField::build(oc::span<const NavObstacle> obstacles, oc::span<const NavSource> sources,
    const BuildParams& params)
{
    ProfileScope scope("Nav build", EProfileCategory::Game);
    m_chunks.reset(64);
    m_sources.assign(sources.begin(), sources.end());
    m_cellsReached = 0;
    rasterizeObstacles(obstacles, params.clearanceCost);

    // Multi-source Dijkstra, 8-connected, lazy deletion. Distances in 1/8 m: an orthogonal step
    // is 16, a diagonal 23 (2.83 m); a cell's cost multiplies the step INTO it. Seeds cover each
    // source's footprint (blocked or not — the front leaves them into free neighbours only).
    const uint32 maxDist = uint32(glm::clamp(params.radius, 1.0f, 8000.0f) * DistScale);
    oc::priority_queue<HeapNode, oc::vector<HeapNode>, oc::greater<HeapNode>> heap;
    for (uint32 s = 0; s < uint32(m_sources.size()) && s < NoSource; ++s)
    {
        const NavSource& src = m_sources[s];
        const float half = glm::max(src.stopRadius, 0.01f);
        const glm::ivec2 lo = cellOf(glm::vec2(src.pos.x, src.pos.z) - half + 1e-3f);
        const glm::ivec2 hi = cellOf(glm::vec2(src.pos.x, src.pos.z) + half - 1e-3f);
        for (int z = lo.y; z <= hi.y; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const glm::ivec2 c(x, z);
                Chunk& chunk = chunkAt(c);
                const uint32 i = cellIndex(c);
                chunk.dist[i] = 0;
                chunk.src[i] = uint16(s);
                heap.push(HeapNode{ 0, c, uint16(s) });
            }
    }

    static constexpr glm::ivec2 c_offsets[8] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    static constexpr uint32 c_stepCost[8] = { 16, 16, 16, 16, 23, 23, 23, 23 };
    while (!heap.empty())
    {
        const HeapNode node = heap.top();
        heap.pop();
        {
            const Chunk* chunk = findChunk(node.cell);
            if (!chunk || chunk->dist[cellIndex(node.cell)] != node.dist)
                continue; // stale entry
        }
        ++m_cellsReached;
        for (int k = 0; k < 8; ++k)
        {
            const glm::ivec2 n = node.cell + c_offsets[k];
            const uint8 cost = costAt(n);
            if (cost == Blocked)
                continue;
            if (k >= 4) // no corner cutting: both orthogonal neighbours must be open too
            {
                if (costAt(node.cell + glm::ivec2(c_offsets[k].x, 0)) == Blocked
                    || costAt(node.cell + glm::ivec2(0, c_offsets[k].y)) == Blocked)
                    continue;
            }
            const uint32 d = node.dist + c_stepCost[k] * (1u + cost);
            if (d > maxDist || d >= Unreached)
                continue;
            Chunk& chunk = chunkAt(n);
            const uint32 i = cellIndex(n);
            if (d < chunk.dist[i])
            {
                chunk.dist[i] = uint16(d);
                chunk.src[i] = node.src;
                heap.push(HeapNode{ d, n, node.src });
            }
        }
    }
}

bool TeamField::hasData(const glm::vec2& xz) const
{
    const glm::ivec2 c = cellOf(xz);
    const Chunk* chunk = findChunk(c);
    return chunk && chunk->dist[cellIndex(c)] != Unreached;
}

TeamField::Sample TeamField::sample(const glm::vec2& xz, uint32 seed) const
{
    // 3x3 scan for the lowest neighbour (a per-cell hash of the seed adds < 1 dist unit of jitter,
    // so equal-distance ties resolve differently per unit). The centre wins only when nothing
    // around it is lower — at/inside a source's footprint.
    Sample out;
    const glm::ivec2 c = cellOf(xz);
    const glm::ivec2 chunkCoord = chunkOf(c);
    const Chunk* cached = m_chunks.find(chunkKey(chunkCoord));
    glm::ivec2 cachedCoord = chunkCoord;
    uint32 bestDist = UINT32_MAX;
    uint32 bestScore = UINT32_MAX;
    glm::ivec2 bestCell = c;
    uint16 bestSrc = NoSource;
    uint32 centreDist = Unreached;
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
            const uint32 d = cached->dist[i];
            if (d == Unreached)
                continue;
            if (dx == 0 && dz == 0)
                centreDist = d;
            const uint32 jitter = ((seed ^ (uint32(n.x) * 73856093u) ^ (uint32(n.y) * 19349663u)) >> 13) & 7u;
            const uint32 score = d * 8u + jitter;
            if (score < bestScore)
            {
                bestScore = score;
                bestDist = d;
                bestCell = n;
                bestSrc = cached->src[i];
            }
        }
    if (bestSrc == NoSource || bestSrc >= m_sources.size())
        return out;
    out.valid = true;
    out.srcIndex = bestSrc;
    out.dist = float(centreDist != Unreached ? centreDist : bestDist) / DistScale;
    if (bestCell == c || bestDist == 0)
    {
        const NavSource& src = m_sources[bestSrc];
        const glm::vec2 to = glm::vec2(src.pos.x, src.pos.z) - xz;
        const float len = glm::length(to);
        out.descentDir = len > 1e-3f ? to / len : glm::vec2(0.0f);
        return out;
    }
    const glm::vec2 to = cellCenter(bestCell) - xz;
    const float len = glm::length(to);
    out.descentDir = len > 1e-3f ? to / len : glm::vec2(0.0f);
    return out;
}

}
