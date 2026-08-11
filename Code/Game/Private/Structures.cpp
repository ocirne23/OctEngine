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
    Tweak::floatVar("Game/Economy", "Emitter energy/s @ pressure 1", &m_emitterPressureDraw, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Extractor energy/s", &m_extractorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Battery capacity", &m_batteryCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Internal buffer", &m_internalBuffer, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Generator buffer", &m_generatorBuffer, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Cable throughput", &m_cableThroughput[0], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Heavy cable throughput", &m_cableThroughput[1], 0.5f, 200.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &m_generatorFuelTank, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Cable max length", &m_cableRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Place range", &m_placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &m_emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &m_emitterReach, 2.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter shrink time", &m_emitterShrinkTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter grow time", &m_emitterGrowTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter restart charge", &m_emitterRestartCharge, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &m_structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &m_structureDamageRate, 0.0f, 100.0f, 0.5f);
}

void StructureSystem::spawnNodes()
{
    m_minerals = m_startMinerals; // fuel is grid-local: "Start fuel" seeds the Base tank instead

    // Authored whitebox layout: the Base sits near (0, 146) and the ambient enemy field grows with
    // distance from it — nodes further out sit in progressively deeper field, so expanding the
    // safe zone is what grows the economy.
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

void StructureSystem::queueCableRequest(uint32 idA, uint32 idB, ECableType type)
{
    m_cableRequests.push_back(Cable{ idA, idB, type });
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

uint32 StructureSystem::randomTargetStructureId() const
{
    uint32 candidates[256];
    int count = 0;
    for (const Structure& s : m_structures)
    {
        if (s.type == EStructureType::Base)
            continue; // invulnerable — gnawing it would be a soft-lock treadmill
        candidates[count++] = s.id;
        if (count == 256)
            break;
    }
    if (count == 0)
        return 0;
    return candidates[glm::clamp((int)glm::linearRand(0.0f, (float)count), 0, count - 1)];
}

void StructureSystem::damageStructure(uint32 id, float amount)
{
    const int index = structureIndexById(id);
    if (index < 0 || m_structures[index].type == EStructureType::Base)
        return;
    m_structures[index].health -= amount; // <= 0 despawns in the next tickDamage sweep
    m_structures[index].lastHitByAttack = true;
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

void StructureSystem::applyCableRequest(uint32 idA, uint32 idB, ECableType type)
{
    // Validated at APPLY time (the MP validation seam): endpoints may have died, or a same-frame
    // duplicate may already have toggled the pair.
    for (size_t i = 0; i < m_cables.size(); ++i)
    {
        Cable& c = m_cables[i];
        if (!((c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA)))
            continue;
        if (c.type == type)
        {
            m_cables.erase(m_cables.begin() + i);
            Log::info("Cable removed");
        }
        else
        {
            c.type = type; // retype in place (upgrade/downgrade)
            Log::info(type == ECableType::Heavy ? "Cable upgraded to heavy" : "Cable downgraded to basic");
        }
        return;
    }
    const int a = structureIndexById(idA);
    const int b = structureIndexById(idB);
    if (!cableAllowed(a, b))
        return;
    m_cables.push_back(Cable{ idA, idB, type });
    Log::info(type == ECableType::Heavy ? "Heavy cable connected" : "Cable connected");
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
        m_links.push_back(Link{ (uint16)a, (uint16)b, (uint16)c });
        parent[find((uint16)a)] = find((uint16)b);
        ++c;
    }
    for (size_t i = 0; i < n; ++i)
        m_structures[i].gridId = find((uint16)i);
}

void StructureSystem::tickPower(float deltaSec)
{
    // ENERGY FLOW SIM: every structure holds LOCAL charge; each cable moves energy toward equal
    // fill fractions of its endpoints, capped by the cable type's throughput — energy propagates
    // hop by hop, so transport capacity (not a global pool) decides what stays powered.
    // Generators produce into their OWN buffer while it has space (full buffer = export-limited =
    // no fuel burn); consumers drain their internal battery. FUEL stays grid-pooled (union-find).
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_emitterEnergyPerSec
             : type == EStructureType::Extractor ? m_extractorEnergyPerSec : 0.0f;
    };
    const float dt = glm::max(deltaSec, 1e-6f);

    // ---- fuel: grid-local pools (tanks: generators / fuel tanks / Base) + extractor income ----
    struct GridFuel { float cap = 0.0f, pool = 0.0f; };
    std::unordered_map<uint16, GridFuel> fuel;
    for (const Structure& s : m_structures)
    {
        GridFuel& g = fuel[s.gridId];
        g.cap += fuelCapacityOf(s.type);
        g.pool += s.fuel;
        if ((s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0
                && m_nodes[s.nodeIndex].type == ENodeType::Fuel)
            || s.type == EStructureType::Base)
            g.pool += m_fuelRate * dt;
    }

    // ---- generators: produce into their own buffer, burning grid fuel proportionally ----
    m_genRateTotal = 0.0f;
    for (Structure& s : m_structures)
    {
        if (s.type != EStructureType::Generator || m_genEnergyPerSec <= 0.0f)
            continue;
        GridFuel& g = fuel[s.gridId];
        const float space = energyCapacityOf(s.type) - s.charge;
        float want = glm::min(m_genEnergyPerSec * dt, glm::max(space, 0.0f));
        if (want <= 0.0f)
            continue;
        const float fuelNeeded = m_fuelBurnRate * want / m_genEnergyPerSec;
        if (fuelNeeded > 1e-9f)
        {
            const float fuelTaken = glm::min(fuelNeeded, g.pool);
            want *= fuelTaken / fuelNeeded;
            g.pool -= fuelTaken;
        }
        s.charge += want;
        m_genRateTotal += want / dt;
    }

    // ---- cable flows: equalize fill fractions, capped by the cable type's throughput. The exact
    // ---- equalizing transfer is (fillA - fillB) * capA*capB/(capA+capB) — never overshoots, so
    // ---- the sim is stable at any dt; the clamp is what makes thin cables a real bottleneck. ----
    for (const Link& l : m_links)
    {
        Cable& c = m_cables[l.cableIndex];
        Structure& a = m_structures[l.a];
        Structure& b = m_structures[l.b];
        const float capA = energyCapacityOf(a.type);
        const float capB = energyCapacityOf(b.type);
        c.lastFlowRate = 0.0f;
        if (capA <= 0.0f || capB <= 0.0f)
            continue;
        const float tEq = (a.charge / capA - b.charge / capB) * (capA * capB / (capA + capB));
        const float maxT = m_cableThroughput[(int)c.type] * dt;
        const float T = glm::clamp(tEq, -maxT, maxT);
        a.charge -= T;
        b.charge += T;
        c.lastFlowRate = T / dt;
    }

    // ---- consumers drain their internal battery. Emitters pay EXTRA per unit of pressure on
    // ---- their field, LATCH OFF at empty until the battery refills to "Emitter restart charge"
    // ---- (no per-tick flicker), and their bubble output ramps smoothly: fading out over the
    // ---- shrink time when starved, regrowing over the grow time on restart. A tiny sentinel
    // ---- output keeps the pressure sensor alive while dark. ----
    m_useRateTotal = 0.0f;
    float totalDemand = 0.0f;
    for (Structure& s : m_structures)
    {
        if (s.type == EStructureType::Emitter)
        {
            ForceComponent* fc = getComponent<ForceComponent>(s.entity.get());
            const float draw = m_emitterEnergyPerSec
                + (fc ? fc->emitter.getPressure() * m_emitterPressureDraw : 0.0f)
                + s.unitLoad; // enemy units leaning on the bubble (deposited by NpcSystem)
            s.unitLoad = 0.0f;
            totalDemand += draw;
            if (s.emitterDown && s.charge >= glm::min(m_emitterRestartCharge, m_internalBuffer))
                s.emitterDown = false;
            bool paid = false;
            if (!s.emitterDown)
            {
                paid = s.charge >= draw * dt - 1e-4f;
                if (paid)
                {
                    s.charge -= draw * dt;
                    m_useRateTotal += draw;
                }
                else
                    s.emitterDown = true;
            }
            s.powered = paid;
            const float target = paid ? 1.0f : 0.0f;
            const float rampTime = glm::max(target > s.outputFrac ? m_emitterGrowTime : m_emitterShrinkTime, 0.01f);
            s.outputFrac = glm::clamp(s.outputFrac + (target > s.outputFrac ? dt : -dt) / rampTime, 0.0f, 1.0f);
            if (fc)
            {
                fc->emitter.setOutput(glm::max(m_emitterOutput * s.outputFrac, 0.01f));
                fc->emitter.setReach(m_emitterReach);
            }
            continue;
        }
        const float draw = consumerDraw(s.type);
        if (draw <= 0.0f)
            continue;
        totalDemand += draw;
        s.powered = s.charge >= draw * dt - 1e-4f;
        if (s.powered)
        {
            s.charge -= draw * dt;
            m_useRateTotal += draw;
        }
    }

    // ---- totals + fuel write-back (fuel pools clamp to tank capacity, split proportionally) ----
    m_gridEnergyTotal = 0.0f;
    m_gridCapacityTotal = 0.0f;
    for (const Structure& s : m_structures)
    {
        m_gridEnergyTotal += s.charge;
        m_gridCapacityTotal += energyCapacityOf(s.type);
    }
    m_fuelTotal = 0.0f;
    for (auto& [id, g] : fuel)
    {
        g.pool = glm::min(g.pool, g.cap);
        m_fuelTotal += g.pool;
    }
    for (Structure& s : m_structures)
    {
        const float fuelCap = fuelCapacityOf(s.type);
        if (fuelCap > 0.0f)
        {
            const GridFuel& g = fuel[s.gridId];
            s.fuel = g.cap > 0.0f ? g.pool * (fuelCap / g.cap) : 0.0f;
        }
    }
    // Fuel-collapse tripwire: the usual death spiral is fuel dry -> generators stopped -> emitters
    // dark -> waves eat everything. Make the FIRST domino loud.
    const bool fuelDry = m_fuelTotal <= 0.01f && totalDemand > 0.0f;
    if (fuelDry && !m_wasFuelDry)
        Log::warning("FUEL DRY — generators stopped, buffers draining; emitters go dark next");
    m_wasFuelDry = fuelDry;
}

void StructureSystem::tickDamage(float deltaSec)
{
    // Field damage needs BOTH: the query says the point is enemy-owned (so a friendly bubble
    // overhead protects), AND the analytic AMBIENT field is meaningfully present there (exact CPU
    // mirror of forceAmbientField — it is a closed-form distance field). The second condition
    // keeps roaming enemy UNITS' bubbles from field-draining structures by mere proximity: units
    // hurt structures only by actually attacking (their proximity gnaw), never by walking past.
    const ForceFieldParams& fp = Globals::forceSystem.getParams();
    const auto ambientAt = [&](const glm::vec3& p) {
        if (fp.ambientSlope <= 0.0f)
            return 0.0f;
        const float d = glm::distance(glm::vec2(p.x, p.z), fp.ambientCenter);
        return glm::clamp((d - fp.ambientSafeRadius) * fp.ambientSlope, 0.0f, fp.ambientMaxStrength);
    };
    for (size_t i = 0; i < m_structures.size();)
    {
        Structure& s = m_structures[i];
        if (s.type == EStructureType::Base)
        {
            ++i; // invulnerable: a dead Base would soft-lock the respawn
            continue;
        }
        const ForceQuery::Result territory = s.query.getResult();
        if (territory.valid && territory.inside && territory.owningTeam != 0
            && ambientAt(s.pos) > fp.isoThreshold)
        {
            s.health -= m_structureDamageRate * deltaSec;
            s.lastHitByAttack = false;
        }
        if (s.health <= 0.0f)
        {
            Log::info(std::string(structureNames[(int)s.type])
                + (s.lastHitByAttack ? " destroyed by enemy units" : " destroyed by the enemy field"));
            destroyStructureAt(i); // grid rebuilds right after this pass
            continue;
        }
        ++i;
    }
}

void StructureSystem::tickAuthority(const glm::vec3&, float deltaSec)
{
    m_time += deltaSec;
    for (const PlaceRequest& req : m_requests)
        placeStructure(req.type, req.pos, req.nodeIndex);
    m_requests.clear();
    for (const Cable& req : m_cableRequests)
        applyCableRequest(req.idA, req.idB, req.type);
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
    // Cables: base hue by TYPE (yellow basic, cyan heavy), brightness by UTILIZATION (idle cables
    // are dim), plus a pulse mark travelling in the flow direction — speed scales with load.
    for (const Link& l : m_links)
    {
        const Cable& c = m_cables[l.cableIndex];
        const glm::vec3 posA = m_structures[l.a].pos;
        const glm::vec3 posB = m_structures[l.b].pos;
        const float throughput = glm::max(m_cableThroughput[(int)c.type], 1e-3f);
        const float util = glm::clamp(glm::abs(c.lastFlowRate) / throughput, 0.0f, 1.0f);
        const glm::vec3 hue = c.type == ECableType::Heavy
            ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        Globals::rendererVK.addDebugLine(posA, posB, packColor(hue * (0.25f + 0.75f * util)));
        if (util > 0.02f)
        {
            float t = glm::fract(m_time * (0.4f + 1.2f * util));
            if (c.lastFlowRate < 0.0f)
                t = 1.0f - t; // pulse travels WITH the energy (A->B positive)
            const glm::vec3 p = glm::mix(posA, posB, t);
            Globals::rendererVK.addDebugLine(p, p + glm::vec3(0.0f, 0.6f, 0.0f), packColor(hue));
        }
    }

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
