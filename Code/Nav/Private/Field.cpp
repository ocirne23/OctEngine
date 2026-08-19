module Nav;

import Core;
import Core.glm;
import Threading;

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
    // MULTITHREADED (this runs inside the navFieldBuild jobs): every chunk any obstacle (inflated
    // by the one-cell ring) can touch is created SERIALLY first, so the parallel phases below never
    // mutate the map — their writes are single bytes where every racing writer writes the SAME
    // value (Blocked in phase 2, clearanceCost in phase 3), which is the sanctioned kind of race.
    for (const NavObstacle& o : obstacles)
    {
        const glm::ivec2 lo = chunkOf(cellOf(o.min - CellSize + 1e-3f));
        const glm::ivec2 hi = chunkOf(cellOf(o.max + CellSize - 1e-3f));
        for (int z = lo.y; z <= hi.y; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                m_chunks.getOrCreate(chunkKey(glm::ivec2(x, z)));
    }

    // Phase 2: stamp the blocked footprints, one obstacle per work item.
    struct Ctx { TeamField* self; oc::span<const NavObstacle> obstacles; uint8 clearanceCost; };
    Ctx ctx{ this, obstacles, clearanceCost };
    Globals::jobSystem.parallelFor(0u, uint32(obstacles.size()), 8u, [c = &ctx](uint32 begin, uint32 end)
    {
        ProfileScope scope("Nav raster blocked", EProfileCategory::Game);
        for (uint32 idx = begin; idx < end; ++idx)
        {
            const NavObstacle& o = c->obstacles[idx];
            const glm::ivec2 lo = cellOf(o.min + 1e-3f);
            const glm::ivec2 hi = cellOf(o.max - 1e-3f);
            for (int z = lo.y; z <= hi.y; ++z)
                for (int x = lo.x; x <= hi.x; ++x)
                {
                    const glm::ivec2 cell(x, z);
                    if (Chunk* chunk = c->self->m_chunks.find(chunkKey(chunkOf(cell)))) // pre-created
                        chunk->cost[cellIndex(cell)] = Blocked;
                }
        }
    });
    if (clearanceCost == 0)
        return;

    // Phase 3: the clearance ring, one chunk per work item; ring cells crossing into a neighbour
    // chunk write through find() — the inflated pre-creation guarantees it exists. The Blocked set
    // is FINAL after phase 2's barrier, so the != Blocked filter cannot race with a Blocked write.
    oc::vector<Chunk*> chunks;      // stack, not thread_local: this runs inside a build job and
    oc::vector<uint64> chunkKeys;   // the parallelFor parks the fiber (see FlowField::update)
    chunks.reserve(m_chunks.size());
    chunkKeys.reserve(m_chunks.size());
    for (uint32 slot = 0; slot < m_chunks.capacity(); ++slot)
        if (m_chunks.slotKey(slot) != InvalidChunkKey)
        {
            chunks.push_back(m_chunks.slotValue(slot));
            chunkKeys.push_back(m_chunks.slotKey(slot));
        }
    struct RingCtx { TeamField* self; oc::vector<Chunk*>* chunks; oc::vector<uint64>* keys; uint8 clearanceCost; };
    RingCtx ringCtx{ this, &chunks, &chunkKeys, clearanceCost };
    Globals::jobSystem.parallelFor(0u, uint32(chunks.size()), 1u, [c = &ringCtx](uint32 begin, uint32 end)
    {
        ProfileScope scope("Nav raster clearance", EProfileCategory::Game);
        for (uint32 idx = begin; idx < end; ++idx)
        {
            const Chunk& chunk = *(*c->chunks)[idx];
            const glm::ivec2 base = chunkFromKey((*c->keys)[idx]) * ChunkCells;
            for (int i = 0; i < ChunkArea; ++i)
            {
                if (chunk.cost[i] != Blocked)
                    continue;
                const glm::ivec2 cell = base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (!(dx | dz))
                            continue;
                        const glm::ivec2 n = cell + glm::ivec2(dx, dz);
                        Chunk* nc = c->self->m_chunks.find(chunkKey(chunkOf(n)));
                        if (!nc)
                            continue;
                        uint8& cost = nc->cost[cellIndex(n)];
                        if (cost != Blocked)
                            cost = glm::max(cost, c->clearanceCost);
                    }
            }
        }
    });
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
    // Direction = drop-weighted blend of every neighbour lower than the reference (the centre,
    // or the best neighbour when the centre is unreached): two equally lower neighbours pull
    // diagonally between them instead of snapping to one cell centre — a straight corridor reads
    // as a straight line, not a 45° stair. Blocked neighbours never enter (unreached).
    const uint32 refDist = centreDist != Unreached ? centreDist : bestDist + 1;
    glm::vec2 blend(0.0f);
    cachedCoord = chunkCoord;
    cached = m_chunks.find(chunkKey(chunkCoord));
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (!(dx | dz))
                continue;
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            const glm::ivec2 nc = chunkOf(n);
            if (nc != cachedCoord)
            {
                cachedCoord = nc;
                cached = m_chunks.find(chunkKey(nc));
            }
            if (!cached)
                continue;
            const uint32 d = cached->dist[cellIndex(n)];
            if (d == Unreached || d >= refDist)
                continue;
            const glm::vec2 to = cellCenter(n) - xz;
            const float len = glm::length(to);
            if (len > 1e-3f)
                blend += to / len * float(refDist - d);
        }
    const float bl = glm::length(blend);
    if (bl > 1e-3f)
        out.descentDir = blend / bl;
    else
    {
        const glm::vec2 to = cellCenter(bestCell) - xz;
        const float len = glm::length(to);
        out.descentDir = len > 1e-3f ? to / len : glm::vec2(0.0f);
    }
    return out;
}

bool TeamField::steerPoint(const glm::vec2& xz, int maxSteps, float radius, glm::vec2& outPoint) const
{
    // Greedy descent walk over cells (lowest 8-neighbour, no corner cutting), then the farthest
    // visible point wins — the pulled string hugs corners instead of the cell-centre stair.
    static constexpr glm::ivec2 c_offsets[8] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    glm::ivec2 cell = cellOf(xz);
    const Chunk* chunk = findChunk(cell);
    if (!chunk)
        return false;
    uint32 dist = chunk->dist[cellIndex(cell)];
    if (dist == Unreached)
    {
        // Standing inside a blocked/unreached cell: hop to the best reached neighbour first.
        uint32 best = Unreached;
        glm::ivec2 bestCell = cell;
        for (const glm::ivec2& o : c_offsets)
        {
            const glm::ivec2 n = cell + o;
            const Chunk* nc = findChunk(n);
            if (!nc)
                continue;
            const uint32 d = nc->dist[cellIndex(n)];
            if (d < best)
            {
                best = d;
                bestCell = n;
            }
        }
        if (best == Unreached)
            return false;
        outPoint = cellCenter(bestCell);
        return true;
    }
    oc::fixed_vector<glm::vec2, 256> path;
    uint16 src = chunk->src[cellIndex(cell)];
    bool reachedSource = false;
    for (int step = 0; step < maxSteps && step < 255; ++step)
    {
        if (dist == 0)
        {
            reachedSource = true;
            break;
        }
        uint32 bestD = dist;
        glm::ivec2 bestCell = cell;
        for (int k = 0; k < 8; ++k)
        {
            const glm::ivec2 n = cell + c_offsets[k];
            const Chunk* nc = findChunk(n);
            if (!nc)
                continue;
            const uint32 d = nc->dist[cellIndex(n)];
            if (d >= bestD)
                continue;
            if (k >= 4 && (costAt(cell + glm::ivec2(c_offsets[k].x, 0)) == Blocked
                || costAt(cell + glm::ivec2(0, c_offsets[k].y)) == Blocked))
                continue;
            bestD = d;
            bestCell = n;
        }
        if (bestCell == cell)
            break; // plateau
        cell = bestCell;
        dist = bestD;
        src = findChunk(cell)->src[cellIndex(cell)];
        path.push_back(cellCenter(cell));
    }
    if (reachedSource && src < m_sources.size())
    {
        const NavSource& s = m_sources[src];
        const glm::vec2 sp(s.pos.x, s.pos.z);
        if (lineOfSight(xz, sp, radius))
        {
            outPoint = sp;
            return true;
        }
    }
    for (int i = int(path.size()) - 1; i >= 0; --i)
        if (lineOfSight(xz, path[i], radius))
        {
            outPoint = path[i];
            return true;
        }
    if (path.empty())
        return false;
    outPoint = path[0];
    return true;
}

glm::vec2 TeamField::avoid(const glm::vec2& xz, const glm::vec2& dir, float lookAhead, float radius, int& side) const
{
    const float len = glm::length(dir);
    if (len < 1e-4f)
        return dir;
    const glm::vec2 d = dir / len;
    if (lineOfSight(xz, xz + d * lookAhead, radius))
    {
        side = 0;
        return d;
    }
    static constexpr float c_angles[4] = { 30.0f, 60.0f, 90.0f, 120.0f };
    for (const float deg : c_angles)
    {
        const float a = glm::radians(deg);
        const float c = std::cos(a), sn = std::sin(a);
        const glm::vec2 l(d.x * c - d.y * sn, d.x * sn + d.y * c);
        const glm::vec2 r(d.x * c + d.y * sn, -d.x * sn + d.y * c);
        // Shorter whiskers on the wide angles: a sidestep, not a detour.
        const float reach = lookAhead * (deg <= 60.0f ? 1.0f : 0.6f);
        const bool lOk = lineOfSight(xz, xz + l * reach, radius);
        const bool rOk = lineOfSight(xz, xz + r * reach, radius);
        if (lOk && rOk)
        {
            if (side == 0)
            {
                // First choice: the side whose wider probe survives; else position parity so a
                // crowd splits. Latched into `side` — the walker keeps it until the way is clear.
                const bool lFar = lineOfSight(xz, xz + l * (reach * 1.5f), radius);
                const bool rFar = lineOfSight(xz, xz + r * (reach * 1.5f), radius);
                if (lFar != rFar)
                    side = lFar ? 1 : -1;
                else
                    side = ((int(std::floor(xz.x)) + int(std::floor(xz.y))) & 1) ? 1 : -1;
            }
            return side > 0 ? l : r;
        }
        if (lOk)
        {
            side = 1;
            return l;
        }
        if (rOk)
        {
            side = -1;
            return r;
        }
    }
    // Boxed in: back off along the reverse whisker if it is open, else hold.
    return lineOfSight(xz, xz - d * lookAhead * 0.5f, radius) ? -d : glm::vec2(0.0f);
}

int TeamField::chooseSide(const glm::vec2& xz, const glm::vec2& dir, const glm::vec2& target, float radius, float maxProbe) const
{
    const float len = glm::length(dir);
    if (len < 1e-4f)
        return 0;
    const glm::vec2 d = dir / len;
    const glm::vec2 left(-d.y, d.x);
    bool leftOpen = true, rightOpen = true;
    for (float s = CellSize; s <= maxProbe; s += CellSize)
    {
        // Probe points slightly FORWARD as well, so a wall we stand against still lets the
        // sideways walk pass along it.
        const glm::vec2 pl = xz + left * s + d * 0.5f;
        const glm::vec2 pr = xz - left * s + d * 0.5f;
        if (leftOpen && !lineOfSight(xz, pl, radius))
            leftOpen = false;
        if (rightOpen && !lineOfSight(xz, pr, radius))
            rightOpen = false;
        if (!leftOpen && !rightOpen)
            return 0;
        const bool lSees = leftOpen && lineOfSight(pl, target, radius);
        const bool rSees = rightOpen && lineOfSight(pr, target, radius);
        if (lSees != rSees)
            return lSees ? 1 : -1;
        if (lSees && rSees)
            return glm::distance(pl, target) <= glm::distance(pr, target) ? 1 : -1;
    }
    // Neither probe sees the target within reach: prefer the side still open, else the side
    // the target lies on.
    if (leftOpen != rightOpen)
        return leftOpen ? 1 : -1;
    const glm::vec2 to = target - xz;
    return (left.x * to.x + left.y * to.y) >= 0.0f ? 1 : -1;
}

float TeamField::freeDistance(const glm::vec2& a, const glm::vec2& dir, float maxLen, float radius) const
{
    // Grid DDA along a direction; returns the parametric distance to the first blocked cell.
    const auto march = [&](const glm::vec2& start, const glm::vec2& d, float len) -> float
    {
        glm::ivec2 cell = cellOf(start);
        const glm::vec2 dl = d * len;
        const glm::ivec2 step(dl.x > 0.0f ? 1 : -1, dl.y > 0.0f ? 1 : -1);
        const glm::vec2 invAbs(glm::abs(dl.x) > 1e-6f ? 1.0f / glm::abs(dl.x) : FLT_MAX,
                               glm::abs(dl.y) > 1e-6f ? 1.0f / glm::abs(dl.y) : FLT_MAX);
        const glm::vec2 cellMin = glm::vec2(cell) * CellSize;
        glm::vec2 tMax(
            (dl.x > 0.0f ? cellMin.x + CellSize - start.x : start.x - cellMin.x) * invAbs.x,
            (dl.y > 0.0f ? cellMin.y + CellSize - start.y : start.y - cellMin.y) * invAbs.y);
        const glm::vec2 tDelta = CellSize * invAbs;
        float t = 0.0f;
        for (int guard = 0; guard < 4096; ++guard)
        {
            if (costAt(cell) == Blocked)
                return t * len;
            if (tMax.x < tMax.y)
            {
                if (tMax.x > 1.0f) return len;
                t = tMax.x;
                cell.x += step.x;
                tMax.x += tDelta.x;
            }
            else
            {
                if (tMax.y > 1.0f) return len;
                t = tMax.y;
                cell.y += step.y;
                tMax.y += tDelta.y;
            }
        }
        return len;
    };
    float best = march(a, dir, maxLen);
    if (radius > 0.0f)
    {
        const glm::vec2 side(-dir.y * radius, dir.x * radius);
        best = glm::min(best, glm::min(march(a + side, dir, maxLen), march(a - side, dir, maxLen)));
    }
    return best;
}

// Push AWAY from every blocked cell whose nearest point lies within `range` of xz, weighted by
// proximity (1 at contact, 0 at range) — the raster-side "keep your distance from walls" term. In
// a one-cell gap the two side walls cancel (the walker centres itself); at a corner the single
// wall swings it wide.
glm::vec2 TeamField::wallPush(const glm::vec2& xz, float range) const
{
    const glm::ivec2 c = cellOf(xz);
    const int r = int(std::ceil(range / CellSize)) + 1;
    glm::vec2 push(0.0f);
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx)
        {
            const glm::ivec2 n = c + glm::ivec2(dx, dz);
            if (costAt(n) != Blocked)
                continue;
            const glm::vec2 mn = glm::vec2(n) * CellSize, mx = mn + CellSize;
            const glm::vec2 closest = glm::clamp(xz, mn, mx);
            const glm::vec2 away = xz - closest;
            const float d = glm::length(away);
            if (d >= range || d < 1e-4f)
                continue;
            push += away / d * (1.0f - d / range);
        }
    return push;
}

namespace
{
    struct AStarNode
    {
        glm::ivec2 cell;
        uint32 parent;
        uint32 g;
        uint32 h;
        bool closed;
    };
    struct AStarOpen
    {
        uint32 f;
        uint32 index;
        bool operator>(const AStarOpen& o) const { return f > o.f; }
    };
    inline uint64 packCell(const glm::ivec2& c) { return (uint64(uint32(c.x)) << 32) | uint64(uint32(c.y)); }
    inline uint32 octile(const glm::ivec2& a, const glm::ivec2& b)
    {
        const uint32 dx = uint32(glm::abs(a.x - b.x)), dz = uint32(glm::abs(a.y - b.y));
        const uint32 mn = glm::min(dx, dz), mx = glm::max(dx, dz);
        return mn * 23 + (mx - mn) * 16; // same 1/8 m units as the field build
    }
}

bool TeamField::findPath(const glm::vec2& from, const glm::vec2& to, uint32 maxExpand, float radius,
    oc::vector<glm::vec2>& outPath) const
{
    outPath.clear();
    const glm::ivec2 start = cellOf(from), goal = cellOf(to);
    if (start == goal)
    {
        outPath.push_back(from);
        outPath.push_back(to);
        return true;
    }
    // Straight shot first: most orders need no search at all.
    if (lineOfSight(from, to, radius))
    {
        outPath.push_back(from);
        outPath.push_back(to);
        return true;
    }
    thread_local oc::vector<AStarNode> nodes;
    thread_local oc::unordered_map<uint64, uint32> index;
    thread_local oc::priority_queue<AStarOpen, oc::vector<AStarOpen>, oc::greater<AStarOpen>> open;
    nodes.clear();
    index.clear();
    while (!open.empty())
        open.pop();

    static constexpr glm::ivec2 c_offsets[8] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    static constexpr uint32 c_stepCost[8] = { 16, 16, 16, 16, 23, 23, 23, 23 };
    nodes.push_back(AStarNode{ start, UINT32_MAX, 0, octile(start, goal), false });
    index[packCell(start)] = 0;
    open.push(AStarOpen{ nodes[0].h, 0 });
    uint32 reached = UINT32_MAX;
    uint32 expanded = 0;
    while (!open.empty() && expanded < maxExpand)
    {
        const AStarOpen top = open.top();
        open.pop();
        if (nodes[top.index].closed)
            continue;
        nodes[top.index].closed = true;
        ++expanded;
        if (nodes[top.index].cell == goal)
        {
            reached = top.index;
            break;
        }
        for (int k = 0; k < 8; ++k)
        {
            const glm::ivec2 n = nodes[top.index].cell + c_offsets[k];
            const uint8 cost = costAt(n);
            if (cost == Blocked && n != goal) // the goal cell may be a structure: enterable as the end
                continue;
            if (k >= 4 && (costAt(nodes[top.index].cell + glm::ivec2(c_offsets[k].x, 0)) == Blocked
                || costAt(nodes[top.index].cell + glm::ivec2(0, c_offsets[k].y)) == Blocked))
                continue; // no corner cutting
            const uint32 g = nodes[top.index].g + c_stepCost[k] * (1u + (cost == Blocked ? 0u : cost));
            const uint64 key = packCell(n);
            const auto it = index.find(key);
            if (it == index.end())
            {
                const uint32 idx = uint32(nodes.size());
                nodes.push_back(AStarNode{ n, top.index, g, octile(n, goal), false });
                index[key] = idx;
                open.push(AStarOpen{ g + nodes[idx].h, idx });
            }
            else if (!nodes[it->second].closed && g < nodes[it->second].g)
            {
                nodes[it->second].g = g;
                nodes[it->second].parent = top.index;
                open.push(AStarOpen{ g + nodes[it->second].h, it->second });
            }
        }
    }
    if (reached == UINT32_MAX)
        return false;

    // Reconstruct start -> goal, then STRING PULL: keep a point only where the line from the last
    // kept point to the one after it would clip the raster.
    thread_local oc::vector<glm::vec2> cells;
    cells.clear();
    for (uint32 i = reached; i != UINT32_MAX; i = nodes[i].parent)
        cells.push_back(cellCenter(nodes[i].cell));
    oc::reverse(cells.begin(), cells.end());
    cells.front() = from;
    cells.back() = to;
    outPath.push_back(cells.front());
    size_t anchor = 0;
    for (size_t i = 1; i + 1 < cells.size(); ++i)
        if (!lineOfSight(cells[anchor], cells[i + 1], radius))
        {
            outPath.push_back(cells[i]);
            anchor = i;
        }
    outPath.push_back(cells.back());
    return true;
}

bool TeamField::lineOfSight(const glm::vec2& a, const glm::vec2& b, float radius) const
{
    if (radius > 0.0f)
    {
        const glm::vec2 d = b - a;
        const float len = glm::length(d);
        if (len > 1e-3f)
        {
            const glm::vec2 side = glm::vec2(-d.y, d.x) / len * radius;
            if (!lineOfSight(a + side, b + side) || !lineOfSight(a - side, b - side))
                return false;
        }
    }
    // Grid DDA from a to b, testing every crossed cell for Blocked (absent chunk = open).
    glm::ivec2 cell = cellOf(a);
    const glm::ivec2 end = cellOf(b);
    const glm::vec2 d = b - a;
    const glm::ivec2 step(d.x > 0.0f ? 1 : -1, d.y > 0.0f ? 1 : -1);
    const glm::vec2 invAbs(glm::abs(d.x) > 1e-6f ? 1.0f / glm::abs(d.x) : FLT_MAX,
                           glm::abs(d.y) > 1e-6f ? 1.0f / glm::abs(d.y) : FLT_MAX);
    const glm::vec2 cellMin = glm::vec2(cell) * CellSize;
    glm::vec2 tMax(
        (d.x > 0.0f ? cellMin.x + CellSize - a.x : a.x - cellMin.x) * invAbs.x,
        (d.y > 0.0f ? cellMin.y + CellSize - a.y : a.y - cellMin.y) * invAbs.y);
    const glm::vec2 tDelta = CellSize * invAbs;
    for (int guard = 0; guard < 4096; ++guard)
    {
        if (cell == end)
            return true; // the END cell may be blocked (a structure target): visible if all before it are open
        if (costAt(cell) == Blocked)
            return false;
        if (tMax.x < tMax.y)
        {
            if (tMax.x > 1.0f) return true;
            cell.x += step.x;
            tMax.x += tDelta.x;
        }
        else
        {
            if (tMax.y > 1.0f) return true;
            cell.y += step.y;
            tMax.y += tDelta.y;
        }
    }
    return false;
}

}
