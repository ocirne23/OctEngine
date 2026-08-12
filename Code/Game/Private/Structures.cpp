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

// One entry per EStructureType, INCLUDING Base last — static_asserted against Count so an enum
// change can never silently shift these again (reading past them handed std::string garbage).
static constexpr const char* structurePrefabs[] = {
    "Entities/Game/emitter.pre", "Entities/Game/generator.pre", "Entities/Game/transmitter.pre",
    "Entities/Game/extractor.pre", "Entities/Game/battery.pre", "Entities/Game/fueltank.pre",
    "Entities/Game/solar.pre", "Entities/Game/fabricator.pre", "Entities/Game/bastion.pre",
    "Entities/Game/lance.pre", "Entities/Game/barracks.pre", "Entities/Game/wall.pre",
    "Entities/Game/turret.pre", "Entities/Game/base.pre" };
static constexpr const char* structureNames[] = { "Emitter", "Generator", "Transmitter", "Extractor",
    "Battery", "Fuel tank", "Solar", "Fabricator", "Bastion", "Lance", "Barracks", "Wall", "Turret", "Base" };
static constexpr float structureSpawnHeights[] = { 1.5f, 0.75f, 2.0f, 0.4f, 1.0f, 1.4f, 1.0f, 1.0f, 2.0f, 1.0f, 1.2f, 1.2f, 1.2f, 1.5f }; // origin above the ground hit
static_assert(std::size(structurePrefabs) == (size_t)EStructureType::Count);
static_assert(std::size(structureNames) == (size_t)EStructureType::Count);
static_assert(std::size(structureSpawnHeights) == (size_t)EStructureType::Count);

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
    Tweak::floatVar("Game/Economy", "Base income mult", &m_baseIncomeMult, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Emitter cost", &m_costs[0], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator cost", &m_costs[1], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Transmitter cost", &m_costs[2], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor cost", &m_costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Battery cost", &m_costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &m_costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Solar cost", &m_costs[6], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fabricator cost", &m_costs[7], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion cost", &m_costs[8], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lance cost", &m_costs[9], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Barracks cost", &m_costs[10], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Wall cost (per segment)", &m_costs[11], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Bastion energy/s", &m_bastionEnergyPerSec, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Lance energy/s", &m_lanceEnergyPerSec, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Solar energy/s", &m_solarEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator energy/s", &m_fabricatorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator fuel/s", &m_fabricatorFuelPerSec, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Fabricator minerals/s", &m_fabricatorMineralsPerSec, 0.0f, 20.0f, 0.1f);
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
    Tweak::floatVar("Game/Economy", "Pipeline throughput", &m_cableThroughput[2], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &m_generatorFuelTank, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Cable max length", &m_cableRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Place range", &m_placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &m_emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &m_emitterReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Bastion output", &m_bastionOutput, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Bastion reach", &m_bastionReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Lance output", &m_lanceOutput, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Lance reach", &m_lanceReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter shrink time", &m_emitterShrinkTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter grow time", &m_emitterGrowTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter restart charge", &m_emitterRestartCharge, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &m_structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &m_structureDamageRate, 0.0f, 100.0f, 0.5f);
}

void StructureSystem::spawnNodes()
{
    m_minerals = m_startMinerals; // fuel is grid-local: "Start fuel" seeds the Base tank instead

    // Authored whitebox layout: the Base sits at the ORIGIN and the ambient enemy field grows with
    // distance from it in every direction. Two starter nodes sit inside the initial safe radius;
    // the rest ring outward in progressively deeper field, so expansion pays in all directions.
    static constexpr struct { float x, z; ENodeType type; } layout[] = {
        // starters (inside the ~30 m safe zone)
        {   14.0f,  -10.0f, ENodeType::Mineral },
        {  -12.0f,   14.0f, ENodeType::Fuel },
        // mid ring (~55-75 m)
        {   55.0f,   30.0f, ENodeType::Mineral },
        {  -60.0f,  -25.0f, ENodeType::Fuel },
        {   10.0f,   65.0f, ENodeType::Fuel },
        {  -35.0f,  -60.0f, ENodeType::Mineral },
        // outer ring (~100-140 m)
        {  105.0f,  -40.0f, ENodeType::Mineral },
        {  -95.0f,   60.0f, ENodeType::Mineral },
        {   40.0f, -115.0f, ENodeType::Fuel },
        {  -50.0f,  110.0f, ENodeType::Fuel },
        {  120.0f,   70.0f, ENodeType::Fuel },
        // far ring (~160-185 m)
        {  160.0f,  -90.0f, ENodeType::Mineral },
        { -150.0f, -110.0f, ENodeType::Mineral },
        {   90.0f,  165.0f, ENodeType::Fuel },
        { -170.0f,   40.0f, ENodeType::Mineral },
        {  175.0f,   15.0f, ENodeType::Fuel },
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

void StructureSystem::queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, facing, nodeIndex });
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

uint32 StructureSystem::randomTargetStructureId(const glm::vec3& nearPos) const
{
    // Keep the 4 nearest damageable structures (insertion into a tiny sorted array), then roll
    // among them — local harassment with some unpredictability, no cross-map beelines.
    constexpr int MaxCandidates = 4;
    struct Candidate { float distSq; uint32 id; };
    Candidate best[MaxCandidates];
    int count = 0;
    for (const Structure& s : m_structures)
    {
        if (s.type == EStructureType::Base)
            continue; // invulnerable — gnawing it would be a soft-lock treadmill
        const glm::vec2 d = glm::vec2(s.pos.x, s.pos.z) - glm::vec2(nearPos.x, nearPos.z);
        const Candidate c{ glm::dot(d, d), s.id };
        if (count < MaxCandidates)
            best[count++] = c;
        else if (c.distSq < best[MaxCandidates - 1].distSq)
            best[MaxCandidates - 1] = c;
        else
            continue;
        for (int i = glm::min(count, MaxCandidates) - 1; i > 0 && best[i].distSq < best[i - 1].distSq; --i)
            std::swap(best[i], best[i - 1]);
    }
    if (count == 0)
        return 0;
    return best[glm::clamp((int)glm::linearRand(0.0f, (float)count), 0, count - 1)].id;
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
            c.type = type; // retype in place — including cable <-> pipe (the medium switches)
            Log::info(type == ECableType::Pipe ? "Retyped to pipeline"
                : type == ECableType::Heavy ? "Retyped to heavy cable" : "Retyped to basic cable");
        }
        return;
    }
    const int a = structureIndexById(idA);
    const int b = structureIndexById(idB);
    if (!cableAllowed(a, b))
        return;
    m_cables.push_back(Cable{ idA, idB, type });
    Log::info(type == ECableType::Pipe ? "Pipeline connected"
        : type == ECableType::Heavy ? "Heavy cable connected" : "Cable connected");
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

void StructureSystem::placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing)
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
    // Lance orientation (its prefab's Force axis is entity -Z): the AIMED facing from the
    // two-click placement when given, else auto — away from the Base, into the enemy field.
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (type == EStructureType::Lance)
    {
        glm::vec2 dir(0.0f);
        if (glm::dot(glm::vec2(facing.x, facing.z), glm::vec2(facing.x, facing.z)) > 1e-4f)
            dir = glm::normalize(glm::vec2(facing.x, facing.z));
        else
            for (const Structure& other : m_structures)
                if (other.type == EStructureType::Base)
                {
                    const glm::vec2 d = glm::vec2(s.pos.x, s.pos.z) - glm::vec2(other.pos.x, other.pos.z);
                    if (glm::dot(d, d) > 1e-4f)
                        dir = glm::normalize(d);
                    break;
                }
        if (glm::dot(dir, dir) > 0.5f)
            rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    s.entity = Globals::world.spawnAssetFile(structurePrefabs[(int)type], Transform(s.pos, 1.0f, rot), true);
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

void StructureSystem::rebuildLinks()
{
    // Resolve every cable to index pairs for this tick's flow sim + debug draw; a per-tick rebuild
    // self-heals after any destruction. No grid/union-find: BOTH resources are per-structure flow
    // networks now, so connectivity is only ever consumed edge by edge.
    m_links.clear();
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
        ++c;
    }
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
             : type == EStructureType::Extractor ? m_extractorEnergyPerSec
             : type == EStructureType::Fabricator ? m_fabricatorEnergyPerSec : 0.0f;
    };
    const float dt = glm::max(deltaSec, 1e-6f);

    // ---- fuel income: powered fuel extractors fill their OWN tank, the Base its own — a full
    // ---- tank stalls production (export-limited), the same rule generators obey for energy ----
    for (Structure& s : m_structures)
    {
        if (s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0
            && m_nodes[s.nodeIndex].type == ENodeType::Fuel)
            s.fuel = glm::min(s.fuel + m_fuelRate * dt, fuelCapacityOf(s.type));
        else if (s.type == EStructureType::Base)
            s.fuel = glm::min(s.fuel + m_fuelRate * m_baseIncomeMult * dt, fuelCapacityOf(s.type));
    }

    // ---- producers: generators burn from their OWN tank, solar panels trickle for free — both
    // ---- into their OWN energy buffer only (full buffer = export-limited = no fuel burn) ----
    m_genRateTotal = 0.0f;
    for (Structure& s : m_structures)
    {
        if (s.type == EStructureType::Solar)
        {
            const float add = glm::min(m_solarEnergyPerSec * dt,
                glm::max(energyCapacityOf(s.type) - s.charge, 0.0f));
            s.charge += add;
            m_genRateTotal += add / dt;
            continue;
        }
        if (s.type != EStructureType::Generator || m_genEnergyPerSec <= 0.0f)
            continue;
        const float space = energyCapacityOf(s.type) - s.charge;
        float want = glm::min(m_genEnergyPerSec * dt, glm::max(space, 0.0f));
        if (want <= 0.0f)
            continue;
        const float fuelNeeded = m_fuelBurnRate * want / m_genEnergyPerSec;
        if (fuelNeeded > 1e-9f)
        {
            const float fuelTaken = glm::min(fuelNeeded, s.fuel);
            want *= fuelTaken / fuelNeeded;
            s.fuel -= fuelTaken;
        }
        s.charge += want;
        m_genRateTotal += want / dt;
    }

    // ---- cable flows: equalize fill fractions, capped by the cable type's throughput. The exact
    // ---- equalizing transfer is (fillA - fillB) * capA*capB/(capA+capB) — never overshoots, so
    // ---- the sim is stable at any dt; the clamp is what makes thin lines a real bottleneck.
    // ---- Basic/Heavy cables move ENERGY between energy buffers; PIPES move FUEL between tanks. ----
    for (const Link& l : m_links)
    {
        Cable& c = m_cables[l.cableIndex];
        Structure& a = m_structures[l.a];
        Structure& b = m_structures[l.b];
        const bool isPipe = c.type == ECableType::Pipe;
        const float capA = isPipe ? fuelCapacityOf(a.type) : energyCapacityOf(a.type);
        const float capB = isPipe ? fuelCapacityOf(b.type) : energyCapacityOf(b.type);
        c.lastFlowRate = 0.0f;
        if (capA <= 0.0f || capB <= 0.0f)
            continue;
        float& storeA = isPipe ? a.fuel : a.charge;
        float& storeB = isPipe ? b.fuel : b.charge;
        const float tEq = (storeA / capA - storeB / capB) * (capA * capB / (capA + capB));
        const float maxT = m_cableThroughput[(int)c.type] * dt;
        const float T = glm::clamp(tEq, -maxT, maxT);
        storeA -= T;
        storeB += T;
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
        if (isEmitterType(s.type))
        {
            ForceComponent* fc = getComponent<ForceComponent>(s.entity.get());
            const float draw = emitterDrawOf(s.type)
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
                fc->emitter.setOutput(glm::max(emitterOutputOf(s.type) * s.outputFrac, 0.01f));
                fc->emitter.setReach(emitterReachOf(s.type));
            }
            continue;
        }
        const float draw = consumerDraw(s.type);
        if (draw <= 0.0f)
            continue;
        totalDemand += draw;
        // Fabricators burn fuel alongside energy (piped into their own tank) — both must be there.
        const float fuelDraw = s.type == EStructureType::Fabricator ? m_fabricatorFuelPerSec : 0.0f;
        s.powered = s.charge >= draw * dt - 1e-4f && s.fuel >= fuelDraw * dt - 1e-4f;
        if (s.powered)
        {
            s.charge -= draw * dt;
            s.fuel -= fuelDraw * dt;
            m_useRateTotal += draw;
        }
    }

    // ---- totals (both resources live per-structure now; nothing to write back) ----
    m_gridEnergyTotal = 0.0f;
    m_gridCapacityTotal = 0.0f;
    m_fuelTotal = 0.0f;
    for (const Structure& s : m_structures)
    {
        m_gridEnergyTotal += s.charge;
        m_gridCapacityTotal += energyCapacityOf(s.type);
        m_fuelTotal += s.fuel;
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
        placeStructure(req.type, req.pos, req.nodeIndex, req.facing);
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
        else if (s.type == EStructureType::Fabricator && s.powered)
            m_minerals += m_fabricatorMineralsPerSec * deltaSec; // energy -> minerals converter
        else if (s.type == EStructureType::Base)
            m_minerals += m_mineralRate * m_baseIncomeMult * deltaSec; // trickle, not an extractor
    }

    // Damage BEFORE the grid rebuild: tickDamage erases from m_structures, and m_links carries raw
    // indices into it until the next rebuild — the windowed drawDebug reads m_links all next frame
    // (erasing after rebuildGrids left index N-1+1 dangling and crashed drawDebug). tickDamage only
    // reads the per-structure queries, so it has no grid dependency.
    tickDamage(deltaSec);
    rebuildLinks();
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
        const glm::vec3 hue = c.type == ECableType::Pipe ? glm::vec3(1.0f, 0.55f, 0.15f) // fuel orange
            : c.type == ECableType::Heavy ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
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
        if ((isEmitterType(s.type) || s.type == EStructureType::Extractor
            || s.type == EStructureType::Fabricator) && !s.powered)
            drawCircle(glm::vec3(s.pos.x, 0.3f, s.pos.z), 1.0f, unpoweredColor, 16);
}
