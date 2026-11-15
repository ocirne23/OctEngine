module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Transform;
import Entity;
import RendererVK;
import Network;
import Nav;
import :Match;
import :Structures;

// THE PVP ARENAS (see Match.ixx, CoopMap): a rock cell grid, its flood fill and the rock spawn. Everything here is pure
// math on the authority AND the clients, so every instance derives the identical layout. (Co-op has no grid: its map is
// the procedural terrain.)

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
static constexpr float c_rockCellSize = 10.0f;       // rock.pre / terrainmark.pre at scale 1: a 10 m cell

// Deterministic map math: every instance must derive the IDENTICAL layout, so the rock heights use their own
// splitmix-style hash/RNG - never glm::linearRand, whose global engine state differs per instance.
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

// Flood fill from the seed cells (4-connected BFS = geodesic cell distance). Any open cell it
// never reaches becomes rock - so EVERY open cell is reachable from the Base by construction, and
// everything placed on open ground (nodes, move orders) is reachable too. Then the merged obstacle rects.
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

// Rocks under ONE root entity (removed whole on regeneration), then the arena's nodes. Deterministic: MapRng seeded
// off the arena index.
void GameMatch::spawnTerrain()
{
    MapRng rng{ mapHash(((uint32)m_coopMap.pvpMap + 17u) ^ 0xC0FFEEu) };
    const float rockScale = m_coopMap.cellSize / c_rockCellSize; // rock.pre/terrainmark.pre are 10 m cells
    // A Scene-only prefab, NOT createEmptyEntity: that one has no components at all, and an
    // entity without a SceneComponent refuses children (the pieces would end up unparented).
    m_terrainRoot = Globals::world.spawnAssetFile("Entities/Game/terrainroot.pre", Transform(), true);
    if (m_terrainRoot)
    {
        m_terrainRoot->setName("ArenaTerrain");
        Globals::world.addRootEntity(m_terrainRoot);
    }

    oc::vector<World::SpawnRequest> requests;
    requests.reserve(m_coopMap.reachable.size() / 2 + 128);
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < m_coopMap.cellsX * m_coopMap.cellsZ; ++i)
    {
        if (!m_coopMap.blocked[i])
            continue;
        // The rock is a cube (10 m × rockScale) sunk a random depth: exposed height 3..4.5 m for a 5 m arena block
        // (always above the player's jump, never floating), tops varying so a field of them reads as terrain, not tiling.
        const float exposed = 3.0f + rng.next01() * 1.5f;
        requests.push_back({ "Entities/Game/rock.pre",
            Transform(coopCellCenter(i) + glm::vec3(0.0f, exposed - 5.0f * rockScale, 0.0f), rockScale, identity) });
        // + the ground marker: a tinted plane wider than the block outlines the blocked cell
        requests.push_back({ "Entities/Game/terrainmark.pre", Transform(coopCellCenter(i), rockScale, identity) });
    }
    oc::vector<EntityPtr> spawned = Globals::world.spawnBatch(requests, /*addRoots*/ false);
    for (size_t i = 0; i < spawned.size(); ++i)
    {
        if (!spawned[i])
            continue;
        spawned[i]->setName((i & 1) ? "RockMark" : "Rock");
        if (m_terrainRoot)
            spawned[i]->reparentEntity(m_terrainRoot.get()); // root at origin: world pos == local
        else
            Globals::world.addRootEntity(oc::move(spawned[i]));
    }
    spawnPvpNodes();
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

void GameMatch::sendMapSeed()
{
    // [u8 the PvP arena index (EPvpMap)].
    uint8 buffer[4];
    NetWriter writer(buffer);
    writer.write<uint8>((uint8)m_coopMap.pvpMap);
    Globals::networkManager.fireNetworkEvent("GMp", writer.data());
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

// ---- the per-team Base anchors ----

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
