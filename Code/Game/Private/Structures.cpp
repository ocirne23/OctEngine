module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Entity;
import Physics;
import Force;
import RendererVK;
import :Structures;

static constexpr const char* structurePrefabs[5] = {
    "Entities/Game/emitter.pre", "Entities/Game/generator.pre", "Entities/Game/transmitter.pre",
    "Entities/Game/extractor.pre", "Entities/Game/base.pre" };
static constexpr const char* structureNames[5] = { "Emitter", "Generator", "Transmitter", "Extractor", "Base" };
static constexpr float structureSpawnHeights[5] = { 1.5f, 0.75f, 2.0f, 0.4f, 1.5f }; // origin above the ground hit

const char* structureTypeName(EStructureType type)
{
    return structureNames[(int)type];
}

glm::vec3 StructureSystem::structureLabelAnchor(int index) const
{
    const Structure& s = m_structures[index];
    return s.pos + glm::vec3(0.0f, structureSpawnHeights[(int)s.type] + 0.7f, 0.0f);
}

static uint32 packColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

static void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments = 24)
{
    glm::vec3 prev = center + glm::vec3(radius, 0.0f, 0.0f);
    for (int i = 1; i <= segments; ++i)
    {
        const float a = float(i) / segments * glm::two_pi<float>();
        const glm::vec3 p = center + glm::vec3(std::cos(a) * radius, 0.0f, std::sin(a) * radius);
        Globals::rendererVK.addDebugLine(prev, p, color);
        prev = p;
    }
}

void StructureSystem::registerTweaks()
{
    Tweak::floatVar("Game/Economy", "Start minerals", &m_startMinerals, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Start fuel", &m_startFuel, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor snap radius", &m_extractorSnapRadius, 1.0f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Minerals/s per node", &m_mineralRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fuel/s per node", &m_fuelRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Emitter cost", &m_costs[0], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator cost", &m_costs[1], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Transmitter cost", &m_costs[2], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor cost", &m_costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel burn/s per generator", &m_fuelBurnRate, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Power per generator", &m_powerPerGenerator, 0.5f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Power per emitter", &m_powerPerEmitter, 0.1f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Power per extractor", &m_powerPerExtractor, 0.1f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Cable max length", &m_cableRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Place range", &m_placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &m_emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &m_emitterReach, 2.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &m_structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &m_structureDamageRate, 0.0f, 100.0f, 0.5f);
}

void StructureSystem::spawnNodes()
{
    m_minerals = m_startMinerals;
    m_fuel = m_startFuel;

    // Authored whitebox layout: player starts around (0, 140); nodes line the approach toward the
    // objective at the origin so pushing inward is what grows the economy.
    static constexpr struct { float x, z; ENodeType type; } layout[] = {
        {  40.0f,  115.0f, ENodeType::Mineral },
        { -45.0f,  110.0f, ENodeType::Fuel },
        {  85.0f,   60.0f, ENodeType::Fuel },
        { -90.0f,   50.0f, ENodeType::Mineral },
        {  55.0f,  -20.0f, ENodeType::Mineral },
        { -60.0f,  -30.0f, ENodeType::Fuel },
        {   0.0f,  -95.0f, ENodeType::Mineral },
    };
    for (const auto& n : layout)
    {
        Node node;
        node.type = n.type;
        node.pos = glm::vec3(n.x, 0.8f, n.z);
        node.entity = Globals::world.spawnAssetFile(
            n.type == ENodeType::Mineral ? "Entities/Game/mineralNode.pre" : "Entities/Game/fuelNode.pre",
            Transform(node.pos), true);
        if (!node.entity)
            continue;
        node.entity->setName(n.type == ENodeType::Mineral ? "MineralNode" : "FuelNode");
        Globals::world.addRootEntity(node.entity);
        m_nodes.push_back(std::move(node));
    }
}

void StructureSystem::clear()
{
    for (Structure& s : m_structures)
        if (s.entity)
            Globals::world.removeRootEntity(s.entity.get());
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_structures.clear();
    m_nodes.clear();
    m_links.clear();
    m_requests.clear();
    m_cables.clear();
    m_cableRequests.clear();
    m_demolishRequests.clear();
}

void StructureSystem::queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, nodeIndex });
}

void StructureSystem::queueCableRequest(uint32 idA, uint32 idB)
{
    m_cableRequests.push_back(Cable{ idA, idB });
}

void StructureSystem::queueDemolishRequest(uint32 id)
{
    m_demolishRequests.push_back(id);
}

void StructureSystem::destroyStructureAt(size_t index)
{
    Structure& s = m_structures[index];
    if (s.nodeIndex >= 0)
        m_nodes[s.nodeIndex].extracted = false; // a removed extractor frees its node
    if (s.entity)
        Globals::world.removeRootEntity(s.entity.get());
    m_structures.erase(m_structures.begin() + index); // cables prune in the next rebuildGrids
}

void StructureSystem::applyDemolishRequest(uint32 id)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return; // already gone
    if (m_structures[index].type == EStructureType::Base)
        return; // the respawn anchor is not deletable
    Log::info(std::string(structureNames[(int)m_structures[index].type]) + " demolished");
    destroyStructureAt((size_t)index);
}

int StructureSystem::structureIndexById(uint32 id) const
{
    for (int i = 0; i < (int)m_structures.size(); ++i)
        if (m_structures[i].id == id)
            return i;
    return -1;
}

int StructureSystem::findConnectableNear(const glm::vec3& pos, float maxDist) const
{
    int best = -1;
    float bestDistSq = maxDist * maxDist;
    for (int i = 0; i < (int)m_structures.size(); ++i)
    {
        const glm::vec2 d = glm::vec2(m_structures[i].pos.x, m_structures[i].pos.z) - glm::vec2(pos.x, pos.z);
        const float distSq = glm::dot(d, d);
        if (distSq <= bestDistSq)
        {
            bestDistSq = distSq;
            best = i;
        }
    }
    return best;
}

bool StructureSystem::cableExists(uint32 idA, uint32 idB) const
{
    for (const Cable& c : m_cables)
        if ((c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA))
            return true;
    return false;
}

bool StructureSystem::cableAllowed(int indexA, int indexB) const
{
    if (indexA < 0 || indexB < 0 || indexA == indexB)
        return false;
    // Any structure pair cables (two emitters relay power fine); transmitters remain the CHEAP way
    // to span distance, not a topological requirement.
    const glm::vec3 d = m_structures[indexA].pos - m_structures[indexB].pos;
    return glm::dot(d, d) <= m_cableRange * m_cableRange;
}

void StructureSystem::applyCableRequest(uint32 idA, uint32 idB)
{
    // Validated at APPLY time (the MP validation seam): endpoints may have died, or a same-frame
    // duplicate may already have toggled the pair.
    if (cableExists(idA, idB))
    {
        std::erase_if(m_cables, [&](const Cable& c) {
            return (c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA); });
        Log::info("Cable removed");
        return;
    }
    const int a = structureIndexById(idA);
    const int b = structureIndexById(idB);
    if (!cableAllowed(a, b))
        return;
    m_cables.push_back(Cable{ idA, idB });
    Log::info("Cable connected");
}

int StructureSystem::findFreeNodeNear(const glm::vec3& groundPos, float maxDist) const
{
    int best = -1;
    float bestDistSq = maxDist * maxDist;
    for (int i = 0; i < (int)m_nodes.size(); ++i)
    {
        if (m_nodes[i].extracted)
            continue;
        const glm::vec2 d = glm::vec2(m_nodes[i].pos.x, m_nodes[i].pos.z) - glm::vec2(groundPos.x, groundPos.z);
        const float distSq = glm::dot(d, d);
        if (distSq <= bestDistSq)
        {
            bestDistSq = distSq;
            best = i;
        }
    }
    return best;
}

void StructureSystem::spawnBase(const glm::vec3& groundPos)
{
    Structure s;
    s.id = m_nextStructureId++;
    s.type = EStructureType::Base;
    s.pos = groundPos + glm::vec3(0.0f, structureSpawnHeights[(int)EStructureType::Base], 0.0f);
    s.health = m_structureHealthMax; // never drained — the Base is skipped in tickDamage
    s.entity = Globals::world.spawnAssetFile(structurePrefabs[(int)EStructureType::Base], Transform(s.pos), true);
    if (!s.entity)
        return;
    s.entity->setName("Base");
    Globals::world.addRootEntity(s.entity);
    m_structures.push_back(std::move(s)); // no query: invulnerable, no damage check needed
}

void StructureSystem::placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex)
{
    if ((int)type >= NumPlaceableStructures)
        return; // the Base only enters through spawnBase
    const float cost = m_costs[(int)type];
    if (m_minerals < cost)
        return; // request raced the stock — silently refused (the ghost already showed red)
    if (type == EStructureType::Extractor)
    {
        // Validated HERE, not just at aim time: two same-frame requests for one node race, and this
        // is where server-side validation lands in multiplayer.
        if (nodeIndex < 0 || nodeIndex >= (int)m_nodes.size() || m_nodes[nodeIndex].extracted)
            return;
        m_nodes[nodeIndex].extracted = true;
    }

    Structure s;
    s.id = m_nextStructureId++;
    s.nodeIndex = type == EStructureType::Extractor ? nodeIndex : -1;
    s.type = type;
    s.pos = groundPos + glm::vec3(0.0f, structureSpawnHeights[(int)type], 0.0f);
    s.health = m_structureHealthMax;
    s.entity = Globals::world.spawnAssetFile(structurePrefabs[(int)type], Transform(s.pos), true);
    if (!s.entity)
    {
        if (s.nodeIndex >= 0)
            m_nodes[s.nodeIndex].extracted = false; // spawn failed — free the node again
        return;
    }
    s.entity->setName(structureNames[(int)type]);
    Globals::world.addRootEntity(s.entity);
    s.query = Globals::forceSystem.createQuery(s.pos);
    m_minerals -= cost;
    m_structures.push_back(std::move(s));
}

void StructureSystem::rebuildGrids()
{
    // Union-find over the MANUAL cables — structure counts are small, and a per-tick rebuild
    // self-heals after any destruction. m_links (index pairs) doubles as the debug-draw line list.
    const size_t n = m_structures.size();
    m_links.clear();
    std::vector<uint16> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = (uint16)i;
    const auto find = [&](uint16 i) {
        while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
        return i;
    };
    for (size_t c = 0; c < m_cables.size();)
    {
        const int a = structureIndexById(m_cables[c].idA);
        const int b = structureIndexById(m_cables[c].idB);
        if (a < 0 || b < 0)
        {
            m_cables.erase(m_cables.begin() + c); // an endpoint died — the cable goes with it
            continue;
        }
        m_links.emplace_back((uint16)a, (uint16)b);
        parent[find((uint16)a)] = find((uint16)b);
        ++c;
    }
    for (size_t i = 0; i < n; ++i)
        m_structures[i].gridId = find((uint16)i);
}

void StructureSystem::tickPower(float deltaSec)
{
    // Per grid: demand = emitters + extractors (each their tweak draw); generators run only while
    // there is Fuel AND demand to serve (no idle burn), supplying powerPerGenerator each.
    // Consumers draw supply in placement order — the tail goes unpowered first (emitters go dark,
    // extractors stop harvesting). Power-gating instead of placement-blocking keeps placement
    // always-responsive with no refund logic. An uncabled consumer sits alone in a generator-less
    // grid, so connection-required needs no extra rule.
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_powerPerEmitter
             : type == EStructureType::Extractor ? m_powerPerExtractor : 0.0f;
    };
    struct GridState { int generators = 0; float demand = 0.0f; float supply = 0.0f; float used = 0.0f; };
    std::unordered_map<uint16, GridState> grids;
    for (const Structure& s : m_structures)
    {
        GridState& g = grids[s.gridId];
        if (s.type == EStructureType::Generator) ++g.generators;
        g.demand += consumerDraw(s.type);
    }
    m_powerCapacity = 0.0f;
    m_powerDemand = 0.0f;
    int runningGenerators = 0;
    for (auto& [id, g] : grids)
    {
        int running = 0;
        if (g.demand > 0.0f && m_fuel > 0.0f && m_powerPerGenerator > 0.0f)
            running = glm::min(g.generators, (int)std::ceil(g.demand / m_powerPerGenerator));
        g.supply = running * m_powerPerGenerator;
        runningGenerators += running;
        if (m_fuel > 0.0f) // idle generators still count as capacity — headroom the HUD can show
            m_powerCapacity += g.generators * m_powerPerGenerator;
        m_powerDemand += g.demand;
    }
    m_fuel = glm::max(0.0f, m_fuel - runningGenerators * m_fuelBurnRate * deltaSec);

    m_powerUsed = 0.0f;
    for (Structure& s : m_structures)
    {
        const float draw = consumerDraw(s.type);
        if (draw <= 0.0f)
            continue;
        GridState& g = grids[s.gridId];
        s.powered = g.used + draw <= g.supply + 1e-4f;
        if (s.powered)
        {
            g.used += draw;
            m_powerUsed += draw;
        }
        if (s.type == EStructureType::Emitter)
            if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
            {
                fc->emitter.setOutput(s.powered ? m_emitterOutput : 0.0f);
                fc->emitter.setReach(m_emitterReach);
            }
    }
}

void StructureSystem::tickDamage(float deltaSec)
{
    for (size_t i = 0; i < m_structures.size();)
    {
        Structure& s = m_structures[i];
        if (s.type == EStructureType::Base)
        {
            ++i; // invulnerable: a dead Base would soft-lock the respawn
            continue;
        }
        const ForceQuery::Result territory = s.query.getResult();
        if (territory.valid && territory.inside && territory.owningTeam != 0)
            s.health -= m_structureDamageRate * deltaSec;
        if (s.health <= 0.0f)
        {
            Log::info(std::string(structureNames[(int)s.type]) + " destroyed by the enemy field");
            destroyStructureAt(i); // grid rebuilds right after this pass
            continue;
        }
        ++i;
    }
}

void StructureSystem::tickAuthority(const glm::vec3&, float deltaSec)
{
    for (const PlaceRequest& req : m_requests)
        placeStructure(req.type, req.pos, req.nodeIndex);
    m_requests.clear();
    for (const Cable& req : m_cableRequests)
        applyCableRequest(req.idA, req.idB);
    m_cableRequests.clear();
    for (uint32 id : m_demolishRequests)
        applyDemolishRequest(id);
    m_demolishRequests.clear();

    // A node produces only through a POWERED extractor (powered = last tick's power state — one
    // tick of lag is invisible at these rates). The Base provides one extractor's worth of EACH
    // resource, unconditionally (it is self-powered).
    for (const Structure& s : m_structures)
    {
        if (s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0)
        {
            if (m_nodes[s.nodeIndex].type == ENodeType::Mineral) m_minerals += m_mineralRate * deltaSec;
            else                                                 m_fuel += m_fuelRate * deltaSec;
        }
        else if (s.type == EStructureType::Base)
        {
            m_minerals += m_mineralRate * deltaSec;
            m_fuel += m_fuelRate * deltaSec;
        }
    }

    // Damage BEFORE the grid rebuild: tickDamage erases from m_structures, and m_links carries raw
    // indices into it until the next rebuild — the windowed drawDebug reads m_links all next frame
    // (erasing after rebuildGrids left index N-1+1 dangling and crashed drawDebug). tickDamage only
    // reads the per-structure queries, so it has no grid dependency.
    tickDamage(deltaSec);
    rebuildGrids();
    tickPower(deltaSec);
}

void StructureSystem::drawDebug() const
{
    // Cables: a line between every connected structure pair.
    const uint32 linkColor = packColor(glm::vec3(0.9f, 0.9f, 0.3f));
    for (const auto& [a, b] : m_links)
        Globals::rendererVK.addDebugLine(m_structures[a].pos, m_structures[b].pos, linkColor);

    // Node rings: cyan = mineral, orange = fuel; dim while free (build an extractor here), full
    // color once extracted.
    for (const Node& node : m_nodes)
    {
        const glm::vec3 base = node.type == ENodeType::Mineral
            ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(1.0f, 0.6f, 0.15f);
        drawCircle(glm::vec3(node.pos.x, 0.3f, node.pos.z), m_extractorSnapRadius,
            packColor(base * (node.extracted ? 1.0f : 0.3f)));
    }

    // Unpowered consumers get a red marker ring so "why is there no bubble / no income" is
    // answerable at a glance (unconnected extractors show it too — cable them to a powered grid).
    const uint32 unpoweredColor = packColor(glm::vec3(1.0f, 0.25f, 0.2f));
    for (const Structure& s : m_structures)
        if ((s.type == EStructureType::Emitter || s.type == EStructureType::Extractor) && !s.powered)
            drawCircle(glm::vec3(s.pos.x, 0.3f, s.pos.z), 1.0f, unpoweredColor, 16);
}
