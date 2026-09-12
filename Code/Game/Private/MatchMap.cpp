module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Time; // real clock for the barrier pulse
import Core.Transform;
import Entity;
import RendererVK;
import Network;
import Nav;
import :Match;
import :Structures;

// The GENERATED TERRAIN (see Match.ixx, CoopMap): the co-op map and the PvP arenas share one cell
// grid, flood fill and rock spawn. Everything here is pure seeded math on the authority AND the
// clients, so every instance derives the identical layout.

// The PVP arenas (EPvpMap): rock-terrain layouts on a 5 m cell grid, each ringed by a 10 m rock
// border (the old borderwall.pre fence is gone). The Lane is the original corridor along X.
static constexpr float c_pvpWideHalfWidth = 40.0f;   // Wide lane / Chokepoints z extent
static constexpr float c_pvpChokeWallHalf = 5.0f;    // Chokepoints: the middle wall's x half-width
static constexpr float c_pvpCircleRadius = 65.0f;    // Circle: the open disc
static constexpr float c_pvpCircleInner = 28.0f;     // Circle: the central rock column
static constexpr float c_pvpCircleBaseRadius = 47.0f; // Circle: the Bases sit mid-ring
static constexpr float c_pvpCellSize = 5.0f;         // rock.pre (a 10 m cube) spawned at half scale
static constexpr int c_pvpBorderCells = 2;           // 10 m of rock around every arena
static constexpr float c_pvpBaseClear = 8.0f;        // rock-free radius around every Base cell
// The CO-OP grid (c_coopHalfSize / c_coopGroundEdge are shared, see Match.ixx).
static constexpr float c_coopCellSize = 10.0f; // terrain cell = one rock block (5x the 2 m grid)
static constexpr int c_coopCells = 54;         // cells per side
static_assert((float)c_coopCells * c_coopCellSize == c_coopHalfSize * 2.0f);
static constexpr float c_coopBaseClearRadius = 26.0f; // rock-free zone around the Base + starters
static constexpr float c_barrierStep = 20.0f;      // one barrier.pre segment (posts at centers)
// The INVISIBLE edge-wall ring just inside the ground rim: waves spawn in the band outside the
// barrier, where crowd pressure used to shove bodies off the world. Blocks everyone (edgewall.pre
// stays on the Default layer, like rock.pre). 100 m segments - nothing sees it, so long boxes keep
// the static body count at 6 a side instead of 30.
static constexpr float c_coopEdgeWall = 299.0f;
static constexpr float c_edgeWallStep = 100.0f;
static constexpr float c_edgeWallHalfThick = 0.5f; // MUST match edgewall.pre's HalfExtents z
static constexpr float c_edgeWallSpan = 300.0f;    // tangential reach of a side: the whole rim, so
                                                   // the four sides seal the corners between them
// A wave spawn point is clamped to +-c_coopGroundEdge, so this is the gap between the furthest a
// body can be placed and the wall's INNER FACE. It has to clear the largest unit's body radius -
// enemyTitan.pre, 2.0 m - or that unit spawns overlapping a static and gets kicked when it unparks.
static constexpr float c_edgeWallClearance = c_coopEdgeWall - c_edgeWallHalfThick - c_coopGroundEdge;
static_assert(c_edgeWallClearance >= 2.0f, "spawn clamp is too close to the edge wall: the biggest "
    "unit body would spawn inside it - move c_coopGroundEdge in or c_coopEdgeWall out");

// Deterministic map math: every instance must derive the IDENTICAL layout from the seed alone
// (the corridor-table contract), so the generator uses its own splitmix-style hash/RNG - never
// glm::linearRand, whose global engine state differs per instance.
static uint32 mapHash(uint32 x)
{
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    return x ^ (x >> 16);
}
struct MapRng
{
    uint32 state;
    uint32 next() { return state = mapHash(state); }
    float next01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};
// Value noise on the cell lattice: hashed corners, bilinear blend. Period in cells.
static float mapNoise(uint32 seed, float x, float z, float period)
{
    const float fx = x / period, fz = z / period;
    const int ix = (int)std::floor(fx), iz = (int)std::floor(fz);
    const float tx = fx - (float)ix, tz = fz - (float)iz;
    const auto corner = [&](int cx, int cz) {
        return (float)(mapHash(seed ^ mapHash((uint32)(cx * 73856093) ^ (uint32)(cz * 19349663))) >> 8)
            * (1.0f / 16777216.0f); };
    const float a = glm::mix(corner(ix, iz), corner(ix + 1, iz), tx);
    const float b = glm::mix(corner(ix, iz + 1), corner(ix + 1, iz + 1), tx);
    return glm::mix(a, b, tz);
}

glm::vec3 GameMatch::coopCellCenter(int index) const
{
    const int x = index % m_coopMap.cellsX, z = index / m_coopMap.cellsX;
    return glm::vec3(m_coopMap.origin.x + ((float)x + 0.5f) * m_coopMap.cellSize, 0.0f,
                     m_coopMap.origin.y + ((float)z + 0.5f) * m_coopMap.cellSize);
}

int GameMatch::coopCellAt(const glm::vec3& pos) const
{
    const int x = (int)std::floor((pos.x - m_coopMap.origin.x) / m_coopMap.cellSize);
    const int z = (int)std::floor((pos.z - m_coopMap.origin.y) / m_coopMap.cellSize);
    if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
        return -1;
    return z * m_coopMap.cellsX + x;
}

// The PvP arena layouts: pure functions of (arena, team count) - identical on every instance.
// A rock BORDER ring surrounds each open area; the flood fill from team 0's Base seals anything
// the layout cut off, and every Base cell gets a rock-free disc (a middle team's Base on the
// Chokepoints wall line carves its own gap).
void GameMatch::generatePvpGrid()
{
    CoopMap& map = m_coopMap;
    map.pvp = true;
    map.cellSize = c_pvpCellSize;
    glm::vec2 half(c_corridorHalfLength, c_corridorHalfWidth);
    switch (map.pvpMap)
    {
    case EPvpMap::WideLane:
    case EPvpMap::Chokepoints: half.y = c_pvpWideHalfWidth; break;
    case EPvpMap::Circle:      half = glm::vec2(c_pvpCircleRadius); break;
    default: break;
    }
    m_pvpInterior = half;
    map.cellsX = (int)std::lround(half.x * 2.0f / c_pvpCellSize) + 2 * c_pvpBorderCells;
    map.cellsZ = (int)std::lround(half.y * 2.0f / c_pvpCellSize) + 2 * c_pvpBorderCells;
    map.origin = -half - glm::vec2((float)c_pvpBorderCells * c_pvpCellSize);
    map.blocked.assign((size_t)map.cellsX * map.cellsZ, 0);
    for (int i = 0; i < map.cellsX * map.cellsZ; ++i)
    {
        const glm::vec3 c = coopCellCenter(i);
        bool open = glm::abs(c.x) < half.x && glm::abs(c.z) < half.y;
        if (map.pvpMap == EPvpMap::Circle)
        {
            const float r = glm::length(glm::vec2(c.x, c.z));
            open = r < c_pvpCircleRadius && r > c_pvpCircleInner;
        }
        else if (map.pvpMap == EPvpMap::Chokepoints && glm::abs(c.x) < c_pvpChokeWallHalf)
        {
            // the middle wall, pierced at the center and near each side: three 10 m openings
            const float az = glm::abs(c.z);
            open = az < 5.0f || (az > 20.0f && az < 30.0f);
        }
        map.blocked[i] = open ? 0 : 1;
    }
    oc::vector<int> seeds;
    for (uint8 t = 0; t < m_numTeams; ++t)
    {
        const glm::vec3 base = baseGroundPos(t);
        for (int i = 0; i < map.cellsX * map.cellsZ; ++i)
        {
            const glm::vec3 c = coopCellCenter(i);
            if (glm::length(glm::vec2(c.x - base.x, c.z - base.z)) < c_pvpBaseClear)
                map.blocked[i] = 0;
        }
        if (t == 0)
            seeds.push_back(coopCellAt(base));
    }
    finishGrid(seeds);
}

// The pure-math half: identical on every instance from (seed, fill, lanes) alone - MapRng only,
// never glm::linearRand (global engine state). Produces blocked cells, the BFS depth/reachable
// sets and the merged obstacle rects.
void GameMatch::generateCoopGrid()
{
    CoopMap& map = m_coopMap;
    const int n = c_coopCells;
    map.cellsX = map.cellsZ = n;
    map.cellSize = c_coopCellSize;
    map.origin = glm::vec2(-c_coopHalfSize);
    map.pvp = false;
    map.blocked.assign((size_t)n * n, 0);
    MapRng rng{ map.seed ? map.seed : 1u };
    const uint32 noiseA = rng.next(), noiseB = rng.next();

    // Two-octave value noise per cell, thresholded to EXACTLY the requested fill fraction (the
    // threshold is read off the sorted copy, so "Terrain fill" means what it says at any seed).
    oc::vector<float> value((size_t)n * n);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
            value[(size_t)z * n + x] = 0.65f * mapNoise(noiseA, (float)x + 0.5f, (float)z + 0.5f, 5.5f)
                                     + 0.35f * mapNoise(noiseB, (float)x + 0.5f, (float)z + 0.5f, 2.5f);
    const float fill = glm::clamp(map.fill, 0.0f, 0.6f);
    oc::vector<float> sorted = value;
    oc::sort(sorted.begin(), sorted.end());
    const float threshold = sorted[glm::clamp((int)((1.0f - fill) * (float)sorted.size()),
        0, (int)sorted.size() - 1)];
    for (size_t i = 0; i < value.size(); ++i)
        map.blocked[i] = fill > 0.0f && value[i] >= threshold ? 1 : 0;

    // The Base zone stays rock-free (Base footprint + starter node pair + breathing room).
    for (int i = 0; i < n * n; ++i)
    {
        const glm::vec3 c = coopCellCenter(i);
        if (glm::length(glm::vec2(c.x, c.z)) < c_coopBaseClearRadius)
            map.blocked[i] = 0;
    }

    // Carved ATTACK LANES: evenly spread directions (jittered) wobbling from the base ring out
    // past the barrier - they guarantee the edge ring connects to the Base (waves walk in through
    // the openings) and give the noise blobs natural chokepoints to defend.
    const int lanes = glm::clamp(map.lanes, 2, 12);
    const auto clearAt = [&](const glm::vec2& p) // ~1.5 cells wide: the cell + its 4 neighbours
    {
        const int cell = coopCellAt(glm::vec3(p.x, 0.0f, p.y));
        if (cell < 0)
            return;
        map.blocked[cell] = 0;
        const int x = cell % n, z = cell / n;
        if (x > 0)     map.blocked[cell - 1] = 0;
        if (x < n - 1) map.blocked[cell + 1] = 0;
        if (z > 0)     map.blocked[cell - n] = 0;
        if (z < n - 1) map.blocked[cell + n] = 0;
    };
    for (int i = 0; i < lanes; ++i)
    {
        const float a = ((float)i + rng.next01() * 0.7f) / (float)lanes * glm::two_pi<float>();
        const float phase = rng.next01() * glm::two_pi<float>();
        const glm::vec2 dir(std::cos(a), std::sin(a));
        const glm::vec2 perp(-dir.y, dir.x);
        for (float r = c_coopBaseClearRadius - 6.0f; r < c_coopHalfSize * 1.5f; r += c_coopCellSize * 0.35f)
        {
            const glm::vec2 p = dir * r + perp * (std::sin(r * 0.045f + phase) * 10.0f);
            if (glm::abs(p.x) > c_coopHalfSize || glm::abs(p.y) > c_coopHalfSize)
                break;
            clearAt(p);
        }
    }

    const int c0 = n / 2 - 1, c1 = n / 2;
    const int seeds[] = { c0 * n + c0, c0 * n + c1, c1 * n + c0, c1 * n + c1 };
    finishGrid(seeds);
}

// Flood fill from the seed cells (4-connected BFS = geodesic cell distance). Any open cell it
// never reaches becomes rock - so EVERY open cell is reachable from the Base by construction, and
// everything placed on open ground (nodes, ambient spawns, move orders) is reachable too. Then the
// merged obstacle rects.
void GameMatch::finishGrid(oc::span<const int> seedCells)
{
    CoopMap& map = m_coopMap;
    const int n = map.cellsX, nz = map.cellsZ;
    map.depth.assign((size_t)n * nz, 0xFFFF);
    map.reachable.clear();
    map.maxDepth = 1;
    oc::vector<int> queue;
    queue.reserve((size_t)n * nz);
    for (const int seedCell : seedCells)
        if (seedCell >= 0 && !map.blocked[seedCell] && map.depth[seedCell] == 0xFFFF)
        {
            map.depth[seedCell] = 0;
            queue.push_back(seedCell);
        }
    for (size_t head = 0; head < queue.size(); ++head)
    {
        const int cell = queue[head];
        const int x = cell % n, z = cell / n;
        const uint16 d = (uint16)(map.depth[cell] + 1);
        const auto visit = [&](int next)
        {
            if (!map.blocked[next] && map.depth[next] == 0xFFFF)
            {
                map.depth[next] = d;
                queue.push_back(next);
            }
        };
        if (x > 0)     visit(cell - 1);
        if (x < n - 1) visit(cell + 1);
        if (z > 0)     visit(cell - n);
        if (z < nz - 1) visit(cell + n);
    }
    for (int i = 0; i < n * nz; ++i)
    {
        if (map.blocked[i])
            continue;
        if (map.depth[i] == 0xFFFF)
        {
            map.blocked[i] = 1; // sealed pocket: fill it in rather than leave unreachable ground
            continue;
        }
        map.reachable.push_back(i);
        map.maxDepth = map.depth[i] > map.maxDepth ? map.depth[i] : map.maxDepth;
    }

    // Merged obstacle rects (horizontal runs): one rect serves Nav, placement (cellsFree) and the
    // spawn probes - a few hundred instead of ~1300 per-cell entries.
    m_terrainRects.clear();
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < n; )
        {
            if (!map.blocked[z * n + x])
            {
                ++x;
                continue;
            }
            int end = x;
            while (end < n && map.blocked[z * n + end])
                ++end;
            m_terrainRects.push_back(glm::vec4(
                map.origin.x + (float)x * map.cellSize, map.origin.y + (float)z * map.cellSize,
                map.origin.x + (float)end * map.cellSize, map.origin.y + (float)(z + 1) * map.cellSize));
            x = end;
        }
}

// Rocks (+ the co-op barrier ring) under ONE root entity (removed whole on regeneration), then
// the resource nodes on reachable open cells. Deterministic: MapRng seeded off the map seed (a
// PvP arena has none - its rock heights roll from the arena index).
void GameMatch::spawnTerrain()
{
    MapRng rng{ mapHash((m_coopMap.pvp ? (uint32)m_coopMap.pvpMap + 17u : m_coopMap.seed) ^ 0xC0FFEEu) };
    const float rockScale = m_coopMap.cellSize / c_coopCellSize; // rock.pre/terrainmark.pre are 10 m cells
    // A Scene-only prefab, NOT createEmptyEntity: that one has no components at all, and an
    // entity without a SceneComponent refuses children (the pieces would end up unparented).
    m_terrainRoot = Globals::world.spawnAssetFile("Entities/Game/terrainroot.pre", Transform(), true);
    if (m_terrainRoot)
    {
        m_terrainRoot->setName(m_coopMap.pvp ? "ArenaTerrain" : "CoopTerrain");
        Globals::world.addRootEntity(m_terrainRoot);
    }

    oc::vector<World::SpawnRequest> requests;
    requests.reserve(m_coopMap.reachable.size() / 2 + 128);
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < m_coopMap.cellsX * m_coopMap.cellsZ; ++i)
    {
        if (!m_coopMap.blocked[i])
            continue;
        // The rock is a cube (10 m × rockScale) sunk a random depth: exposed height 3.5..6 m for
        // the 10 m co-op block, 3..4.5 m for a 5 m arena block (always above the player's jump,
        // never floating), tops varying so a field of them reads as terrain, not tiling.
        const float exposed = m_coopMap.pvp ? 3.0f + rng.next01() * 1.5f : 3.5f + rng.next01() * 2.5f;
        requests.push_back({ "Entities/Game/rock.pre",
            Transform(coopCellCenter(i) + glm::vec3(0.0f, exposed - 5.0f * rockScale, 0.0f), rockScale, identity) });
        // + the ground marker: a tinted plane wider than the block outlines the blocked cell
        requests.push_back({ "Entities/Game/terrainmark.pre", Transform(coopCellCenter(i), rockScale, identity) });
    }
    const size_t rockCount = requests.size();
    // The co-op barrier ring: 20 m segments on all four sides (E/W rotated 90° - the prefab's
    // long axis is X). Collider layer Barrier vs Player only: units and shots pass, capsules do
    // not. A PvP arena needs none - its rock border is solid for everyone.
    const glm::quat yaw90 = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    for (float o = -c_coopHalfSize + c_barrierStep * 0.5f; !m_coopMap.pvp && o < c_coopHalfSize; o += c_barrierStep)
    {
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(o, 10.0f, -c_coopHalfSize)) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(o, 10.0f, c_coopHalfSize)) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(-c_coopHalfSize, 10.0f, o), 1.0f, yaw90) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(c_coopHalfSize, 10.0f, o), 1.0f, yaw90) });
    }
    // The ground-edge ring, OUTSIDE the barrier: solid for everyone, so a wave shoved around in
    // its spawn band cannot be pushed off the world. Segments overlap at the corners - they are
    // static boxes, so that costs nothing.
    const size_t edgeStart = requests.size();
    for (float o = -c_edgeWallSpan + c_edgeWallStep * 0.5f; !m_coopMap.pvp && o < c_edgeWallSpan; o += c_edgeWallStep)
    {
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(o, 10.0f, -c_coopEdgeWall)) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(o, 10.0f, c_coopEdgeWall)) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(-c_coopEdgeWall, 10.0f, o), 1.0f, yaw90) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(c_coopEdgeWall, 10.0f, o), 1.0f, yaw90) });
    }
    oc::vector<EntityPtr> spawned = Globals::world.spawnBatch(requests, /*addRoots*/ false);
    for (size_t i = 0; i < spawned.size(); ++i)
    {
        if (!spawned[i])
            continue;
        spawned[i]->setName(i >= edgeStart ? "EdgeWall"
            : i >= rockCount ? "Barrier" : (i & 1) ? "RockMark" : "Rock");
        if (m_terrainRoot)
            spawned[i]->reparentEntity(m_terrainRoot.get()); // root at origin: world pos == local
        else
            Globals::world.addRootEntity(oc::move(spawned[i]));
    }

    if (m_coopMap.pvp)
    {
        spawnPvpNodes();
        return;
    }
    // RESOURCE NODES: the starter pair in the cleared Base zone, then a golden-angle spiral of
    // candidates SNAPPED to reachable open cells (skip when none nearby or too close to another
    // node) - even coverage that respects whatever the noise carved, and reachable by the flood
    // fill's guarantee.
    m_structures.spawnNode(10.0f, 4.0f, ENodeType::Mineral);
    m_structures.spawnNode(-4.0f, 10.0f, ENodeType::Fuel);
    oc::vector<glm::vec2> placed = { glm::vec2(10.0f, 4.0f), glm::vec2(-4.0f, 10.0f) };
    constexpr int c_nodeTarget = 48;
    int placedCount = 0;
    for (int i = 0; i < 120 && placedCount < c_nodeTarget; ++i)
    {
        const float t = ((float)i + 0.5f) / 120.0f;
        const float r = glm::mix(34.0f, c_coopHalfSize - 16.0f, std::sqrt(t)); // sqrt = uniform area
        const float a = (float)i * 2.3999632f; // the golden angle
        const glm::vec3 want(std::cos(a) * r, 0.0f, std::sin(a) * r);
        // Snap to the nearest reachable open cell around the candidate (2 rings).
        int bestCell = -1;
        float bestDistSq = FLT_MAX;
        const int wantCell = coopCellAt(want);
        if (wantCell < 0)
            continue;
        const int wx = wantCell % m_coopMap.cellsX, wz = wantCell / m_coopMap.cellsX;
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int x = wx + dx, z = wz + dz;
                if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
                    continue;
                const int cell = z * m_coopMap.cellsX + x;
                if (m_coopMap.blocked[cell]) // post-flood: open == reachable
                    continue;
                const glm::vec3 c = coopCellCenter(cell);
                const float distSq = glm::dot(glm::vec2(c.x - want.x, c.z - want.z),
                                              glm::vec2(c.x - want.x, c.z - want.z));
                if (distSq < bestDistSq)
                {
                    bestDistSq = distSq;
                    bestCell = cell;
                }
            }
        if (bestCell < 0)
            continue;
        const glm::vec3 center = coopCellCenter(bestCell);
        const glm::vec2 pos(center.x + (rng.next01() * 4.0f - 2.0f),
                            center.z + (rng.next01() * 4.0f - 2.0f));
        bool spaced = true;
        for (const glm::vec2& q : placed)
            if (glm::dot(pos - q, pos - q) < 13.0f * 13.0f)
            {
                spaced = false;
                break;
            }
        if (!spaced)
            continue;
        m_structures.spawnNode(pos.x, pos.y, (placedCount % 3) == 1 ? ENodeType::Fuel : ENodeType::Mineral);
        placed.push_back(pos);
        ++placedCount;
    }
}

// The arena node tables: the lane's 180°-symmetric set (every node on a SIDE, the center empty,
// each side's forward fuel node the exposed prize), widened with an outer band on the wide
// arenas; the Circle rings its nodes between the Bases. A node whose cell is rock is skipped.
void GameMatch::spawnPvpNodes()
{
    const auto nodeIfOpen = [this](float x, float z, ENodeType type)
    {
        const int cell = coopCellAt(glm::vec3(x, 0.0f, z));
        if (cell >= 0 && !m_coopMap.blocked[cell])
            m_structures.spawnNode(x, z, type);
    };
    if (m_coopMap.pvpMap == EPvpMap::Circle)
    {
        for (int k = 0; k < 6; ++k)
        {
            const float a = ((float)k + 0.5f) * glm::pi<float>() / 3.0f; // between the two-team Bases
            nodeIfOpen(std::cos(a) * 38.0f, std::sin(a) * 38.0f, (k & 1) ? ENodeType::Fuel : ENodeType::Mineral);
            nodeIfOpen(std::cos(a) * 56.0f, std::sin(a) * 56.0f, (k & 1) ? ENodeType::Mineral : ENodeType::Fuel);
        }
        return;
    }
    static constexpr struct { float x, z; ENodeType type; } c_laneNodes[] = {
        { -45.0f,   8.0f, ENodeType::Mineral }, {  45.0f,  -8.0f, ENodeType::Mineral },
        { -37.0f, -10.0f, ENodeType::Fuel },    {  37.0f,  10.0f, ENodeType::Fuel },
        { -25.0f,  12.0f, ENodeType::Mineral }, {  25.0f, -12.0f, ENodeType::Mineral },
        { -14.0f, -10.0f, ENodeType::Fuel },    {  14.0f,  10.0f, ENodeType::Fuel },
    };
    static constexpr struct { float x, z; ENodeType type; } c_wideNodes[] = {
        { -40.0f,  30.0f, ENodeType::Fuel },    {  40.0f, -30.0f, ENodeType::Fuel },
        { -18.0f,  32.0f, ENodeType::Mineral }, {  18.0f, -32.0f, ENodeType::Mineral },
        { -50.0f, -28.0f, ENodeType::Mineral }, {  50.0f,  28.0f, ENodeType::Mineral },
    };
    for (const auto& n : c_laneNodes)
        nodeIfOpen(n.x, n.z, n.type);
    if (m_coopMap.pvpMap != EPvpMap::Lane)
        for (const auto& n : c_wideNodes)
            nodeIfOpen(n.x, n.z, n.type);
}

void GameMatch::rebuildPvpMap(EPvpMap map)
{
    if (m_coopMap.built && m_coopMap.pvp && m_coopMap.pvpMap == map)
        return; // the join-replay re-broadcast / a duplicate GMp
    if (m_terrainRoot)
    {
        Globals::world.removeRootEntity(m_terrainRoot.get());
        m_terrainRoot = {};
    }
    m_structures.clearNodes();
    m_pvpMap = map;
    m_coopMap.pvpMap = map;
    generatePvpGrid();
    m_coopMap.built = true;
    spawnTerrain();
    m_wallObstacles.clear();
    for (const glm::vec4& r : m_terrainRects)
        m_wallObstacles.push_back(Nav::NavObstacle{ glm::vec2(r.x, r.y), glm::vec2(r.z, r.w) });
    m_structures.setTerrainBlocked(m_terrainRects);
    m_structures.setPlacementBounds(-m_pvpInterior, m_pvpInterior); // inside the rock border
    Log::info(oc::format("PvP arena: {}, {} rock rects, {} open cells",
        pvpMapName(map), m_terrainRects.size(), m_coopMap.reachable.size()));
}

void GameMatch::rebuildCoopMap(uint32 seed, float fill, int lanes)
{
    if (m_coopMap.built && !m_coopMap.pvp && m_coopMap.seed == seed && m_coopMap.fill == fill && m_coopMap.lanes == lanes)
        return; // the join-replay re-broadcast / a duplicate GMp
    if (m_terrainRoot)
    {
        Globals::world.removeRootEntity(m_terrainRoot.get()); // rocks + barrier die with it
        m_terrainRoot = {};
    }
    m_structures.clearNodes();
    m_coopMap.seed = seed;
    m_coopMap.fill = fill;
    m_coopMap.lanes = lanes;
    m_ambientSpawn.remaining = 0; // an ambient group anchored on the old map is void
    generateCoopGrid();
    m_coopMap.built = true;
    spawnTerrain();
    // The rock rects are the co-op "walls": Nav obstacles (feedNav re-reads m_wallObstacles every
    // frame) and placement refusals (cellsFree - ghosts turn red on rock, unit spawns skip it).
    m_wallObstacles.clear();
    for (const glm::vec4& r : m_terrainRects)
        m_wallObstacles.push_back(Nav::NavObstacle{ glm::vec2(r.x, r.y), glm::vec2(r.z, r.w) });
    m_structures.setTerrainBlocked(m_terrainRects);
    m_structures.setPlacementBounds(glm::vec2(-c_coopHalfSize), glm::vec2(c_coopHalfSize));
    Log::info(oc::format("Co-op map: seed {}, {} rock rects, {} reachable cells",
        seed, m_terrainRects.size(), m_coopMap.reachable.size()));
}

void GameMatch::sendMapSeed()
{
    // [u8 mode: 0 co-op | 1 PvP] then the co-op inputs (seed/fill/lanes) or the PvP arena index.
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint8>(m_coopMap.pvp ? 1 : 0);
    if (m_coopMap.pvp)
        writer.write<uint8>((uint8)m_coopMap.pvpMap);
    else
    {
        writer.write<uint32>(m_coopMap.seed);
        writer.write<float>(m_coopMap.fill);
        writer.write<uint8>((uint8)m_coopMap.lanes);
    }
    Globals::networkManager.fireNetworkEvent("GMp", writer.data());
}

// An ambient body position: a random point in the disc around the group anchor that lands on OPEN
// ground (rejection-sampled), then slid at least a body's width off any neighbouring rock edge -
// continuous coverage across cell borders, so nothing lines up on the lattice.
glm::vec3 GameMatch::ambientPointNear(const glm::vec3& center, float radius) const
{
    glm::vec3 p = center;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float a = glm::linearRand(0.0f, glm::two_pi<float>());
        const float r = radius * std::sqrt(glm::linearRand(0.0f, 1.0f));
        const glm::vec3 q = center + glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
        const int cell = coopCellAt(q);
        if (cell >= 0 && !m_coopMap.blocked[cell])
        {
            p = q;
            break;
        }
    }
    const int cell = coopCellAt(p);
    if (cell >= 0)
    {
        constexpr float c_keep = 1.2f; // body radius + a gap from the rock face
        const int x = cell % m_coopMap.cellsX, z = cell / m_coopMap.cellsX;
        const glm::vec3 c = coopCellCenter(cell);
        const float half = m_coopMap.cellSize * 0.5f;
        if (x == 0 || m_coopMap.blocked[cell - 1])
            p.x = glm::max(p.x, c.x - half + c_keep);
        if (x == m_coopMap.cellsX - 1 || m_coopMap.blocked[cell + 1])
            p.x = glm::min(p.x, c.x + half - c_keep);
        if (z == 0 || m_coopMap.blocked[cell - m_coopMap.cellsX])
            p.z = glm::max(p.z, c.z - half + c_keep);
        if (z == m_coopMap.cellsZ - 1 || m_coopMap.blocked[cell + m_coopMap.cellsX])
            p.z = glm::min(p.z, c.z + half - c_keep);
    }
    return glm::vec3(p.x, 1.0f, p.z);
}

glm::vec3 GameMatch::clampToOpenGround(const glm::vec3& pos) const
{
    if (!m_coopMap.built)
        return pos;
    const int cell = coopCellAt(pos);
    if (cell < 0 || !m_coopMap.blocked[cell])
        return pos;
    // Inside a rock: the nearest open cell center within 3 rings (post-flood, open == reachable).
    const int cx = cell % m_coopMap.cellsX, cz = cell / m_coopMap.cellsX;
    int best = -1;
    float bestDistSq = FLT_MAX;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx)
        {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
                continue;
            const int c = z * m_coopMap.cellsX + x;
            if (m_coopMap.blocked[c])
                continue;
            const glm::vec3 center = coopCellCenter(c);
            const glm::vec2 d(center.x - pos.x, center.z - pos.z);
            if (glm::dot(d, d) < bestDistSq)
            {
                bestDistSq = glm::dot(d, d);
                best = c;
            }
        }
    return best >= 0 ? coopCellCenter(best) : pos;
}

// The barrier's "energy fence": pulsing lines strung between the posts at three heights, a slow
// wave running along each side. Cheap (a few hundred debug lines) and unmistakably "do not pass".
void GameMatch::drawCoopBarrier()
{
    const float t = (float)Globals::time.getElapsedSec(); // real clock: the fence hums while paused
    constexpr float c_heights[3] = { 1.4f, 3.0f, 4.6f };
    const glm::vec3 base(1.0f, 0.5f, 0.15f);
    const int segs = (int)(c_coopHalfSize * 2.0f / c_barrierStep);
    for (int side = 0; side < 4; ++side)
    {
        for (int i = 0; i < segs; ++i)
        {
            const float a0 = -c_coopHalfSize + (float)i * c_barrierStep;
            const float a1 = a0 + c_barrierStep;
            const uint32 color = packColor(base * (0.55f + 0.45f
                * std::sin(t * 2.2f + (float)(i + side * segs) * 0.7f)));
            for (const float h : c_heights)
            {
                const glm::vec3 p0 = side < 2 ? glm::vec3(a0, h, side == 0 ? -c_coopHalfSize : c_coopHalfSize)
                                              : glm::vec3(side == 2 ? -c_coopHalfSize : c_coopHalfSize, h, a0);
                const glm::vec3 p1 = side < 2 ? glm::vec3(a1, h, side == 0 ? -c_coopHalfSize : c_coopHalfSize)
                                              : glm::vec3(side == 2 ? -c_coopHalfSize : c_coopHalfSize, h, a1);
                Globals::rendererVK.addDebugLine(p0, p1, color);
            }
        }
    }
}

// ---- the per-team Base anchors ------------------------------------------------------------------

glm::vec3 GameMatch::baseGroundPos(uint8 team) const
{
    // Circle: the Bases sit evenly around the ring, team 0 at the west. The lane arenas: two
    // teams = the corridor's ends (the original layout); more spread EVENLY along the x axis
    // between them on the corridor's center line, so every team keeps the same node symmetry
    // and the middle teams sit between two neighbours (generatePvpGrid clears rock around each).
    if (m_pvpMap == EPvpMap::Circle)
    {
        const float a = glm::pi<float>() + glm::two_pi<float>() * (float)team / (float)glm::max((int)m_numTeams, 1);
        return glm::vec3(std::cos(a), 0.0f, std::sin(a)) * c_pvpCircleBaseRadius;
    }
    if (m_numTeams <= 2)
        return team == 0 ? m_basePos : m_enemyBasePos;
    const float t = (float)glm::min((int)team, (int)m_numTeams - 1) / (float)(m_numTeams - 1);
    return glm::mix(m_basePos, m_enemyBasePos, t);
}

glm::vec3 GameMatch::teamStartPos(uint8 team) const
{
    if (m_coop)
        return m_playerStart; // one shared Base - everyone spawns/respawns beside it
    // One Base per playable team; the spawn sits just beside it. Prefer the LIVE Base of that
    // team (on a client the mirrored one - it never learned the lobby's team count), else the
    // layout formula.
    const glm::vec3 offset(0.0f, 1.0f, -6.0f);
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) == team)
            return glm::vec3(m_structures.structurePos(i).x, 0.0f, m_structures.structurePos(i).z) + offset;
    return baseGroundPos(team) + offset;
}
