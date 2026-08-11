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

static constexpr const char* structurePrefabs[7] = {
    "Entities/Game/emitter.pre", "Entities/Game/generator.pre", "Entities/Game/transmitter.pre",
    "Entities/Game/extractor.pre", "Entities/Game/battery.pre", "Entities/Game/fueltank.pre",
    "Entities/Game/base.pre" };
static constexpr const char* structureNames[7] = { "Emitter", "Generator", "Transmitter", "Extractor", "Battery", "Fuel tank", "Base" };
static constexpr float structureSpawnHeights[7] = { 1.5f, 0.75f, 2.0f, 0.4f, 1.0f, 1.4f, 1.5f }; // origin above the ground hit

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
    Tweak::floatVar("Game/Economy", "Battery cost", &m_costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &m_costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank capacity", &m_fuelTankCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Fuel burn/s per generator", &m_fuelBurnRate, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Energy gen/s per generator", &m_genEnergyPerSec, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Emitter energy/s", &m_emitterEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Extractor energy/s", &m_extractorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Battery capacity", &m_batteryCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &m_generatorFuelTank, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Cable max length", &m_cableRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Place range", &m_placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &m_emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &m_emitterReach, 2.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &m_structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &m_structureDamageRate, 0.0f, 100.0f, 0.5f);
}

void StructureSystem::spawnNodes()
{
    m_minerals = m_startMinerals; // fuel is grid-local: "Start fuel" seeds the Base tank instead

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
    s.charge = m_batteryCapacity;    // its battery-worth of storage starts full
    s.fuel = glm::min(m_startFuel, m_generatorFuelTank); // "Start fuel" seeds the Base tank
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
    // ENERGY model per grid: generators turn Fuel into energy/s, batteries (and the Base) store
    // it, consumers drain stored+generated energy in placement order — the tail goes unpowered
    // first (emitters dark, extractors idle). Generators run only against demand or an unfilled
    // battery (no idle Fuel burn) and quantize to whole running generators. Charge lives ON the
    // storage structures (equal split — all storage has the same capacity), so grid merges pool
    // it, splits carry it, and a destroyed battery loses it. A storage-less grid still works as
    // pure generator->consumer throughput. Power-gating instead of placement-blocking keeps
    // placement always-responsive; an uncabled consumer sits alone in a dead grid = unpowered.
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_emitterEnergyPerSec
             : type == EStructureType::Extractor ? m_extractorEnergyPerSec : 0.0f;
    };
    const auto energyCapacityOf = [&](EStructureType type) {
        return type == EStructureType::Battery || type == EStructureType::Base ? m_batteryCapacity : 0.0f;
    };
    struct GridState
    {
        int generators = 0;
        float energyCap = 0.0f, energyPool = 0.0f, demandRate = 0.0f;
        float fuelCap = 0.0f, fuelPool = 0.0f;
        float available = 0.0f; // energy pool + this tick's generation, drained by the consumer pass
    };
    std::unordered_map<uint16, GridState> grids;
    const float dt = glm::max(deltaSec, 1e-6f);
    for (const Structure& s : m_structures)
    {
        GridState& g = grids[s.gridId];
        if (s.type == EStructureType::Generator) ++g.generators;
        g.energyCap += energyCapacityOf(s.type);
        g.energyPool += s.charge;
        g.fuelCap += fuelCapacityOf(s.type);
        g.fuelPool += s.fuel;
        g.demandRate += consumerDraw(s.type);
        // Grid-local FUEL income: powered fuel extractors and the Base feed THEIR grid's tanks
        // (clamped to the tank capacity at write-back; a tank-less grid loses the fuel).
        if ((s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0
                && m_nodes[s.nodeIndex].type == ENodeType::Fuel)
            || s.type == EStructureType::Base)
            g.fuelPool += m_fuelRate * dt;
    }

    m_genRateTotal = 0.0f;
    for (auto& [id, g] : grids)
    {
        // Desired output = live demand + whatever refills the storage this tick; quantized up to
        // whole generators so the battery actually charges instead of asymptoting. Fuel caps the
        // running count: generators burn from THEIR grid's tanks and stop when they run dry.
        const float refillRate = glm::max(0.0f, g.energyCap - g.energyPool) / dt;
        const float desiredRate = g.demandRate + refillRate;
        int running = 0;
        if (desiredRate > 1e-4f && g.fuelPool > 0.0f && m_genEnergyPerSec > 0.0f)
        {
            running = glm::min(g.generators, (int)std::ceil(desiredRate / m_genEnergyPerSec));
            if (m_fuelBurnRate > 0.0f)
                running = glm::min(running, (int)(g.fuelPool / (m_fuelBurnRate * dt)));
        }
        g.fuelPool = glm::max(0.0f, g.fuelPool - running * m_fuelBurnRate * dt);
        g.available = g.energyPool + running * m_genEnergyPerSec * dt;
        m_genRateTotal += running * m_genEnergyPerSec;
    }

    m_useRateTotal = 0.0f;
    for (Structure& s : m_structures)
    {
        const float draw = consumerDraw(s.type);
        if (draw <= 0.0f)
            continue;
        GridState& g = grids[s.gridId];
        s.powered = g.available >= draw * dt - 1e-4f;
        if (s.powered)
        {
            g.available -= draw * dt;
            m_useRateTotal += draw;
        }
        if (s.type == EStructureType::Emitter)
            if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
            {
                fc->emitter.setOutput(s.powered ? m_emitterOutput : 0.0f);
                fc->emitter.setReach(m_emitterReach);
            }
    }

    // Write both pools back into their storage members proportional to capacity, clamped (surplus
    // over full storage is lost — but generators don't run for it, see desiredRate).
    m_gridEnergyTotal = 0.0f;
    m_gridCapacityTotal = 0.0f;
    m_fuelTotal = 0.0f;
    for (auto& [id, g] : grids)
    {
        g.energyPool = glm::min(g.available, g.energyCap);
        g.fuelPool = glm::min(g.fuelPool, g.fuelCap);
        m_gridEnergyTotal += g.energyPool;
        m_gridCapacityTotal += g.energyCap;
        m_fuelTotal += g.fuelPool;
    }
    for (Structure& s : m_structures)
    {
        const GridState& g = grids[s.gridId];
        const float energyCap = energyCapacityOf(s.type);
        if (energyCap > 0.0f)
            s.charge = g.energyCap > 0.0f ? g.energyPool * (energyCap / g.energyCap) : 0.0f;
        const float fuelCap = fuelCapacityOf(s.type);
        if (fuelCap > 0.0f)
            s.fuel = g.fuelCap > 0.0f ? g.fuelPool * (fuelCap / g.fuelCap) : 0.0f;
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

    // MINERAL income (global build currency; powered = last tick's power state — one tick of lag
    // is invisible at these rates). The Base provides one extractor's worth unconditionally.
    // FUEL income is grid-local and lives in tickPower.
    for (const Structure& s : m_structures)
    {
        if (s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0
            && m_nodes[s.nodeIndex].type == ENodeType::Mineral)
            m_minerals += m_mineralRate * deltaSec;
        else if (s.type == EStructureType::Base)
            m_minerals += m_mineralRate * deltaSec;
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
