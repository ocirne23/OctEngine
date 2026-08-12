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
    "Entities/Game/lance.pre", "Entities/Game/barracks.pre", "Entities/Game/barracksBrute.pre",
    "Entities/Game/barracksRunner.pre", "Entities/Game/barracksSpitter.pre", "Entities/Game/wall.pre",
    "Entities/Game/turret.pre", "Entities/Game/mineralstorage.pre", "Entities/Game/constructor.pre",
    "Entities/Game/base.pre" };
static constexpr const char* structureNames[] = { "Emitter", "Generator", "Connector", "Extractor",
    "Battery", "Fuel tank", "Solar", "Fabricator", "Bastion", "Lance", "Barracks", "Brute barracks",
    "Runner barracks", "Spitter barracks", "Wall", "Turret", "Mineral silo", "Constructor", "Base" };
// Grid-aligned sizes: 1x1-cell buildings are 2 m cubes (half 1), 2x2 ones 4 m (half 2) — the
// spawn height IS the half height, so every box sits flush on the ground.
static constexpr float structureSpawnHeights[] = { 1.0f, 1.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 2.0f, 2.0f, 1.0f, 3.0f }; // origin above the ground hit
                                                // (Solar sits at 1: a flat 4x4x2 panel)
static constexpr glm::vec3 c_blueprintColor(0.45f, 0.55f, 0.7f); // ghost tint until built
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
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped(ETweakFlags::Saved | ETweakFlags::Synced);
    Tweak::floatVar("Game/Combat", "Projectile structure damage", &m_projectileStructDamage, 0.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Pressure draw tension", &m_pressureDrawTension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Start minerals", &m_startMinerals, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Start fuel", &m_startFuel, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor snap radius", &m_extractorSnapRadius, 1.0f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Minerals/s per node", &m_mineralRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fuel/s per node", &m_fuelRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base income mult", &m_baseIncomeMult, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Emitter cost", &m_costs[0], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator cost", &m_costs[1], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Connector cost", &m_costs[2], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor cost", &m_costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Battery cost", &m_costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &m_costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Solar cost", &m_costs[6], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fabricator cost", &m_costs[7], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion cost", &m_costs[8], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lance cost", &m_costs[9], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Barracks cost", &m_costs[10], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Brute barracks cost", &m_costs[11], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Runner barracks cost", &m_costs[12], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Spitter barracks cost", &m_costs[13], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo cost", &m_costs[16], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Constructor cost", &m_costs[17], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Constructor range", &m_constructorRange, 2.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Constructor build rate", &m_constructorBuildRate, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Structures", "Constructor boost rate", &m_constructorBoostRate, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Constructor boost materials/s", &m_constructorBoostMaterials, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Constructor boost energy/s", &m_constructorBoostEnergy, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Mineral base capacity", &m_mineralBaseCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo capacity", &m_mineralSiloCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Wall cost (per segment)", &m_costs[14], 0.0f, 100.0f, 0.5f);
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
    Tweak::floatVar("Game/Economy", "Conveyor throughput", &m_cableThroughput[3], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Connector link range", &m_connectorRange, 5.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &m_generatorFuelTank, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Cable max length", &m_cableRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Place range", &m_placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &m_emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &m_emitterReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Bastion output", &m_bastionOutput, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Bastion reach", &m_bastionReach, 2.0f, 46.0f, 0.5f);
    // The lance's Width 0.2 + Focus 0.85 concentrate a CONSERVED total (~25x+ local density), so
    // its useful output range sits far below the other emitters' — hence the tiny floor.
    Tweak::floatVar("Game/Structures", "Lance output", &m_lanceOutput, 0.01f, 8.0f, 0.01f);
    Tweak::floatVar("Game/Structures", "Lance reach", &m_lanceReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter shrink time", &m_emitterShrinkTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter grow time", &m_emitterGrowTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter restart charge", &m_emitterRestartCharge, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &m_structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &m_structureDamageRate, 0.0f, 100.0f, 0.5f);
}

void StructureSystem::spawnNodes()
{
    // No global seed: "Start minerals" lands in each Base's mineral STORE at spawnBase — spendable
    // minerals are the Silo+Base stores, recomputed per tick. Fuel likewise seeds the Base tank.

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
    const auto spawnNode = [this](float x, float z, ENodeType type)
    {
        Node node;
        node.type = type;
        node.pos = glm::vec3(x, 0.8f, z);
        node.entity = Globals::world.spawnAssetFile(
            type == ENodeType::Mineral ? "Entities/Game/mineralNode.pre" : "Entities/Game/fuelNode.pre",
            Transform(node.pos), true);
        if (!node.entity)
            return;
        node.entity->setName(type == ENodeType::Mineral ? "MineralNode" : "FuelNode");
        Globals::world.addRootEntity(node.entity);
        m_nodes.push_back(std::move(node));
    };
    if (m_pvp)
    {
        // The CORRIDOR arena (GameMatch::spawnCorridorWalls: x -65..65, z -20..20, bases at
        // x = -55 / +55): mirrored starters at each end, contested resources in the middle.
        static constexpr struct { float x, z; ENodeType type; } corridor[] = {
            { -45.0f,   8.0f, ENodeType::Mineral }, {  45.0f,  -8.0f, ENodeType::Mineral },
            { -37.0f, -10.0f, ENodeType::Fuel },    {  37.0f,  10.0f, ENodeType::Fuel },
            { -20.0f,   0.0f, ENodeType::Mineral }, {  20.0f,   0.0f, ENodeType::Mineral },
            {   0.0f,  14.0f, ENodeType::Fuel },    {   0.0f, -14.0f, ENodeType::Fuel },
        };
        for (const auto& n : corridor)
            spawnNode(n.x, n.z, n.type);
        return; // the radial PvE layout stays outside the walls
    }
    for (const auto& n : layout)
        spawnNode(n.x, n.z, n.type);
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
    const glm::vec3& facing, uint8 team)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, facing, nodeIndex, team });
}

void StructureSystem::queueCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team)
{
    m_cableRequests.push_back(Cable{ idA, idB, type, 0.0f, team });
}

void StructureSystem::queueDemolishRequest(uint32 id, uint8 team)
{
    m_demolishRequests.push_back({ id, team });
}

void StructureSystem::destroyStructureAt(size_t index)
{
    Structure& s = m_structures[index];
    const uint32 id = s.id;
    if (s.nodeIndex >= 0)
        m_nodes[s.nodeIndex].extracted = false; // a removed extractor frees its node
    if (s.entity)
        Globals::world.removeRootEntity(s.entity.get());
    m_structures.erase(m_structures.begin() + index); // cables prune in the next rebuildLinks
    if (onStructureRemoved)
        onStructureRemoved(id);
}

void StructureSystem::applyDemolishRequest(uint32 id, uint8 team)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return; // already gone
    if (m_structures[index].type == EStructureType::Base)
        return; // the respawn anchor is not deletable
    if (m_structures[index].team != team)
        return; // PvP: only the owning team demolishes its structures
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

uint32 StructureSystem::randomTargetStructureId(const glm::vec3& nearPos, uint8 attackerTeam) const
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
        if (s.team == attackerTeam)
            continue; // never march on your own team's buildings
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

bool StructureSystem::cableAllowed(int indexA, int indexB, ECableType type, int ignoreCableIndex) const
{
    if (indexA < 0 || indexB < 0 || indexA == indexB)
        return false;
    const int medium = cableMedium(type);
    // Building-to-building links are fine again, but BOTH endpoints must actually USE the medium
    // (capacity > 0) — a link that could never flow is refused instead of dangling dead.
    const auto capOf = [&](EStructureType t) {
        return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
    if (capOf(m_structures[indexA].type) <= 0.0f || capOf(m_structures[indexB].type) <= 0.0f)
        return false;
    // Range: links touching a Connector reach further — they are the long-haul relays.
    const bool viaConnector = m_structures[indexA].type == EStructureType::Connector
        || m_structures[indexB].type == EStructureType::Connector;
    const float range = viaConnector ? m_connectorRange : m_cableRange;
    const glm::vec3 d = m_structures[indexA].pos - m_structures[indexB].pos;
    if (glm::dot(d, d) > range * range)
        return false;
    // A Connector carries exactly ONE medium: every other link touching it must match the new
    // link's medium (power / fuel / minerals). Buildings may mix freely.
    for (const int endpoint : { indexA, indexB })
    {
        if (m_structures[endpoint].type != EStructureType::Connector)
            continue;
        const uint32 id = m_structures[endpoint].id;
        for (int c = 0; c < (int)m_cables.size(); ++c)
            if (c != ignoreCableIndex && (m_cables[c].idA == id || m_cables[c].idB == id)
                && cableMedium(m_cables[c].type) != medium)
                return false;
    }
    return true;
}

void StructureSystem::spendMinerals(uint8 team, float amount)
{
    // Silos drain first; the Base last — its trickle refills, and keeping silo stock rotating
    // makes conveyor supply lines visibly matter.
    for (const EStructureType pass : { EStructureType::MineralSilo, EStructureType::Base })
        for (Structure& s : m_structures)
        {
            if (amount <= 0.0f)
                return;
            if (s.type != pass || s.team != team)
                continue;
            const float take = glm::min(amount, s.mineralStore);
            s.mineralStore -= take;
            amount -= take;
        }
}

void StructureSystem::applyStructureTint(Structure& s)
{
    if (!s.entity)
        return;
    RenderComponent* rc = getComponent<RenderComponent>(s.entity.get());
    if (!rc || !rc->node.isValid())
        return;
    glm::vec3 color = c_blueprintColor;
    if (!s.blueprint)
    {
        color = glm::vec3(1.0f); // fallback: authored tint missing
        if (const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(s.entity.get()); info && info->color.x >= 0.0f)
            color = info->color;
    }
    rc->node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(color));
}

float StructureSystem::investMaterials(Structure& s, float amount)
{
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp — the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT.
    if (amount <= 0.0f || (int)s.type >= NumPlaceableStructures)
        return 0.0f;
    const float materialsPerHp = glm::max(m_costs[(int)s.type], 0.01f) / glm::max(m_structureHealthMax, 1e-3f);
    const float heal = glm::min(amount / materialsPerHp, m_structureHealthMax - s.health);
    if (heal <= 0.0f)
        return 0.0f;
    s.health += heal;
    if (s.blueprint && s.health >= m_structureHealthMax - 1e-3f)
    {
        s.blueprint = false;
        applyStructureTint(s); // back to the authored color
        Log::info(std::string(structureNames[(int)s.type]) + " constructed");
    }
    return heal * materialsPerHp;
}

float StructureSystem::takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount)
{
    float taken = 0.0f;
    for (Structure& s : m_structures)
    {
        if (amount - taken <= 0.0f)
            break;
        if (s.blueprint || s.team != team
            || (s.type != EStructureType::MineralSilo && s.type != EStructureType::Base))
            continue;
        if (glm::distance(glm::vec2(s.pos.x, s.pos.z), glm::vec2(pos.x, pos.z)) > radius)
            continue;
        const float take = glm::min(amount - taken, s.mineralStore);
        s.mineralStore -= take;
        taken += take;
    }
    return taken;
}

float StructureSystem::fundNearbyBlueprint(const glm::vec3& pos, float radius, uint8 team, float amount,
    bool includeRepairs)
{
    if (amount <= 0.0f)
        return 0.0f;
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < (int)m_structures.size(); ++i)
    {
        const Structure& s = m_structures[i];
        if (s.team != team || s.type == EStructureType::Base)
            continue;
        const bool wantsMaterials = s.blueprint
            || (includeRepairs && s.health < m_structureHealthMax - 1e-3f);
        if (!wantsMaterials)
            continue;
        const float dist = glm::distance(glm::vec2(s.pos.x, s.pos.z), glm::vec2(pos.x, pos.z));
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best >= 0 ? investMaterials(m_structures[best], amount) : 0.0f;
}

void StructureSystem::applyCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team)
{
    // Validated at APPLY time (the MP validation seam): endpoints may have died, or a same-frame
    // duplicate may already have toggled the pair. PvP: both endpoints must be the requester's
    // team — grids never bridge teams, and nobody rewires someone else's base.
    {
        const int a = structureIndexById(idA);
        const int b = structureIndexById(idB);
        if (a < 0 || b < 0 || m_structures[a].team != team || m_structures[b].team != team)
            return;
    }
    for (size_t i = 0; i < m_cables.size(); ++i)
    {
        Cable& c = m_cables[i];
        if (!((c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA)))
            continue;
        if (c.type == type)
        {
            m_cables.erase(m_cables.begin() + i);
            Log::info("Cable removed");
            if (onCableChanged)
                onCableChanged(idA, idB, type, true);
        }
        else
        {
            // retype in place — the medium may switch, so the Connector single-medium rule
            // re-validates with THIS cable excluded from the check
            const int a = structureIndexById(idA);
            const int b = structureIndexById(idB);
            if (!cableAllowed(a, b, type, (int)i))
                return;
            c.type = type;
            Log::info(type == ECableType::Pipe ? "Retyped to pipeline"
                : type == ECableType::Conveyor ? "Retyped to conveyor"
                : type == ECableType::Heavy ? "Retyped to heavy cable" : "Retyped to basic cable");
            if (onCableChanged)
                onCableChanged(idA, idB, type, false);
        }
        return;
    }
    const int a = structureIndexById(idA);
    const int b = structureIndexById(idB);
    if (!cableAllowed(a, b, type))
        return;
    m_cables.push_back(Cable{ idA, idB, type });
    Log::info(type == ECableType::Pipe ? "Pipeline connected"
        : type == ECableType::Conveyor ? "Conveyor connected"
        : type == ECableType::Heavy ? "Heavy cable connected" : "Cable connected");
    if (onCableChanged)
        onCableChanged(idA, idB, type, false);
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

void StructureSystem::spawnBase(const glm::vec3& groundPos, uint8 team)
{
    Structure s;
    s.id = m_nextStructureId++;
    s.type = EStructureType::Base;
    s.team = team;
    s.pos = groundPos + glm::vec3(0.0f, structureSpawnHeights[(int)EStructureType::Base], 0.0f);
    s.health = m_structureHealthMax; // never drained — the Base is skipped in tickDamage
    s.charge = m_batteryCapacity;    // its battery-worth of storage starts full
    s.fuel = glm::min(m_startFuel, m_generatorFuelTank); // "Start fuel" seeds the Base tank
    s.mineralStore = glm::min(m_startMinerals, m_mineralBaseCapacity); // the starting war chest
    s.entity = Globals::world.spawnAssetFile(structurePrefabs[(int)EStructureType::Base], Transform(s.pos), true);
    if (!s.entity)
        return;
    s.entity->setName("Base");
    Globals::world.addRootEntity(s.entity);
    if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        fc->emitter.setTeam(team); // PvP: the second team's Base carries ITS bubble
    m_structures.push_back(std::move(s)); // no query: invulnerable, no damage check needed
}

void StructureSystem::placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing, uint8 team)
{
    if ((int)type >= NumPlaceableStructures || (int)team >= GameMaxTeams)
        return; // the Base only enters through spawnBase
    // GRID: every placement snaps (extractors snap the node's position too) and occupied cells
    // refuse — validated HERE, the MP seam, not just at aim time.
    const glm::vec3 snappedGround = snapToGrid(type, groundPos);
    if (!cellsFree(type, snappedGround))
        return; // overlap raced the ghost — silently refused (it already showed red)
    // CONSTRUCTION: placing is FREE — the structure spawns as an inert BLUEPRINT and becomes real
    // once its cost in materials is invested (players in proximity, or a Constructor in range).
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
    s.pos = snappedGround + glm::vec3(0.0f, structureSpawnHeights[(int)type], 0.0f);
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
        {
            rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
            s.facingXZ = dir; // kept for the co-op join replay
        }
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
    s.team = team;
    s.blueprint = true;
    s.health = 1.0f; // health IS the build progress — a fresh ghost is nearly dead until funded
    if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 0 — the builder's team owns the field
    // Enemy-team projectiles chip structures on contact (same-team and co-op shots are free:
    // both sides are team 0 there). Server-only — clients mirror health through GSt/GRm.
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity.get()))
        pc->onContact = [this, id = s.id, team](Entity& other, bool begin)
        {
            if (!begin || std::string_view(other.getName()) != "Projectile")
                return;
            const ForceComponent* fc = getComponent<ForceComponent>(&other);
            if (fc && fc->emitter.getTeam() != team)
                damageStructure(id, m_projectileStructDamage);
        };
    s.query = Globals::forceSystem.createQuery(s.pos);
    applyStructureTint(s); // blueprint gray until built
    m_structures.push_back(std::move(s));
    if (onStructurePlaced)
        onStructurePlaced((int)m_structures.size() - 1);
}

// CLIENT mirror of placeStructure: forced id, no cost, no ForceQuery (no damage sim here), pos is
// the server's final (already height-raised) position.
void StructureSystem::mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos,
    const glm::vec2& facingXZ, int nodeIndex, uint8 team, bool built)
{
    if (structureIndexById(id) >= 0 || (int)type >= (int)EStructureType::Count
        || (int)team >= GameMaxTeams)
        return; // duplicate replay / garbage
    Structure s;
    s.id = id;
    s.type = type;
    s.pos = pos;
    s.facingXZ = facingXZ;
    s.nodeIndex = nodeIndex;
    s.team = team;
    if (!built)
        s.health = 1.0f; // health mirrors the build progress until GSt refreshes it
    s.health = m_structureHealthMax;
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::dot(facingXZ, facingXZ) > 1e-4f)
    {
        const glm::vec2 dir = glm::normalize(facingXZ);
        rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    s.entity = Globals::world.spawnAssetFile(structurePrefabs[(int)type], Transform(s.pos, 1.0f, rot), true);
    if (!s.entity)
        return;
    s.entity->setName(structureNames[(int)type]);
    Globals::world.addRootEntity(s.entity);
    s.blueprint = !built;
    if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        fc->emitter.setTeam(team); // the local field carries the owner's team (PvP readbacks)
    applyStructureTint(s);
    if (nodeIndex >= 0 && nodeIndex < (int)m_nodes.size())
        m_nodes[nodeIndex].extracted = true;
    m_nextStructureId = glm::max(m_nextStructureId, id + 1);
    m_structures.push_back(std::move(s));
}

void StructureSystem::mirrorRemove(uint32 id)
{
    const int index = structureIndexById(id);
    if (index >= 0)
        destroyStructureAt((size_t)index);
}

void StructureSystem::mirrorCable(uint32 idA, uint32 idB, ECableType type, bool removed)
{
    std::erase_if(m_cables, [&](const Cable& c) {
        return (c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA); });
    if (!removed)
        m_cables.push_back(Cable{ idA, idB, type });
}

void StructureSystem::mirrorStructureState(uint32 id, float healthFrac, float chargeFrac,
    float fuelFrac, float mineralFrac, float outputFrac, float utilFrac, bool powered, bool blueprint)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return;
    Structure& s = m_structures[index];
    if (s.blueprint != blueprint)
    {
        s.blueprint = blueprint; // completion (or a fresh ghost) — swap the tint to match
        applyStructureTint(s);
    }
    s.health = healthFrac * m_structureHealthMax;
    s.charge = chargeFrac * energyCapacityOf(s.type);
    s.fuel = fuelFrac * fuelCapacityOf(s.type);
    s.mineralStore = mineralFrac * mineralCapacityOf(s.type);
    s.flowUtil = utilFrac; // already server-smoothed; the 5 Hz mirror cadence is smooth enough
    s.outputFracTarget = outputFrac; // tickMirror eases the live fraction + field toward this
    s.powered = powered;
}

void StructureSystem::tickMirror(float deltaSec)
{
    rebuildLinks();
    // Ease each emitter's fraction toward the synced target at ramp-like speed (covers the grow
    // ramp's 2/s), then drive the LOCAL field from it — the bubble animates as smoothly as the
    // server's own instead of stepping at the GSt cadence.
    for (Structure& s : m_structures)
    {
        if (!isEmitterType(s.type))
            continue;
        const float step = glm::clamp(s.outputFracTarget - s.outputFrac, -deltaSec * 3.0f, deltaSec * 3.0f);
        s.outputFrac = glm::clamp(s.outputFrac + step, 0.0f, 1.0f);
        if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
        {
            fc->emitter.setOutput(emitterOutputOf(s.type) * s.outputFrac); // no sentinel — see tickPower
            fc->emitter.setReach(emitterReachOf(s.type));
        }
    }
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
             : type == EStructureType::Constructor ? m_extractorEnergyPerSec // powered while building
             : type == EStructureType::Fabricator ? m_fabricatorEnergyPerSec : 0.0f;
    };
    const float dt = glm::max(deltaSec, 1e-6f);

    // ---- fuel income: powered fuel extractors fill their OWN tank, the Base its own — a full
    // ---- tank stalls production (export-limited), the same rule generators obey for energy ----
    for (Structure& s : m_structures)
    {
        if (s.blueprint)
            continue; // blueprints are inert until constructed
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
        if (s.blueprint)
            continue;
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
    // ---- The link's MEDIUM picks the store: Basic/Heavy = energy, Pipe = fuel, Conveyor = minerals. ----
    for (const Link& l : m_links)
    {
        Cable& c = m_cables[l.cableIndex];
        Structure& a = m_structures[l.a];
        Structure& b = m_structures[l.b];
        if (a.blueprint || b.blueprint)
        {
            c.lastFlowRate = 0.0f; // links to a blueprint pre-wire it but carry nothing yet
            continue;
        }
        const int medium = cableMedium(c.type);
        const auto capOf = [&](EStructureType t) {
            return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
        const float capA = capOf(a.type);
        const float capB = capOf(b.type);
        c.lastFlowRate = 0.0f;
        if (capA <= 0.0f || capB <= 0.0f)
            continue;
        float& storeA = medium == 1 ? a.fuel : medium == 2 ? a.mineralStore : a.charge;
        float& storeB = medium == 1 ? b.fuel : medium == 2 ? b.mineralStore : b.charge;
        const float tEq = (storeA / capA - storeB / capB) * (capA * capB / (capA + capB));
        const float maxT = m_cableThroughput[(int)c.type] * dt;
        const float T = glm::clamp(tEq, -maxT, maxT);
        storeA -= T;
        storeB += T;
        c.lastFlowRate = T / dt;
    }

    // ---- connector throughput gauges: peak utilization of the touching links, EMA-smoothed
    // ---- (~0.25 s) — the equalizing flow jitters tick to tick and would strobe the bar ----
    for (Structure& s : m_structures)
    {
        if (s.type != EStructureType::Connector)
            continue;
        float peak = 0.0f;
        for (const Cable& c : m_cables)
            if (c.idA == s.id || c.idB == s.id)
                peak = glm::max(peak, glm::abs(c.lastFlowRate) / glm::max(m_cableThroughput[(int)c.type], 1e-3f));
        s.flowUtil += (glm::min(peak, 1.0f) - s.flowUtil) * glm::min(dt * 4.0f, 1.0f);
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
        if (s.blueprint)
        {
            s.powered = false;
            if (isEmitterType(s.type))
            {
                s.outputFrac = 0.0f;
                if (ForceComponent* fc = getComponent<ForceComponent>(s.entity.get()))
                    fc->emitter.setOutput(0.0f);
            }
            continue;
        }
        if (isEmitterType(s.type))
        {
            ForceComponent* fc = getComponent<ForceComponent>(s.entity.get());
            // Surface tension mirrored on the DEFENDING side: the surcharge grows superlinearly
            // with pressure — being leaned on hard burns the emitter's supply line too.
            const float pressure = fc ? fc->emitter.getPressure() : 0.0f;
            const float draw = emitterDrawOf(s.type)
                + pressure * (1.0f + m_pressureDrawTension * pressure) * m_emitterPressureDraw
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
                // NO sentinel: a dark emitter's field is fully gone (output 0 = no draw, no
                // deformation, no ghost cone from shaped emitters). Its pressure readback reads 0
                // while down, so a contested emitter restarts at the rest-rate price and re-learns
                // the contested draw one tick later — accepted, the down-latch still stops flicker.
                fc->emitter.setOutput(emitterOutputOf(s.type) * s.outputFrac);
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
    float fuelSum = 0.0f;
    for (float& f : m_fuelTotal)
        f = 0.0f;
    for (const Structure& s : m_structures)
    {
        // The HUD's "energy stored" counts DEDICATED storage only (Battery + Base) — generator
        // production buffers and consumer trickle buffers are working charge, not reserves.
        if (s.type == EStructureType::Battery || s.type == EStructureType::Base)
        {
            m_gridEnergyTotal += s.charge;
            m_gridCapacityTotal += energyCapacityOf(s.type);
        }
        m_fuelTotal[s.team] += s.fuel;
        fuelSum += s.fuel;
    }
    // Fuel-collapse tripwire: the usual death spiral is fuel dry -> generators stopped -> emitters
    // dark -> waves eat everything. Make the FIRST domino loud.
    const bool fuelDry = fuelSum <= 0.01f && totalDemand > 0.0f;
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
        // PvP: HOSTILE = any OTHER team's bubble owns the structure's point (push your field over
        // their base to siege it) — the ambient gate is meaningless with the ambient off. PvE keeps
        // the ambient requirement so unit bubbles never field-drain structures by proximity.
        const bool hostileField = m_pvp
            ? territory.valid && territory.inside && territory.owningTeam != (int)s.team
            : territory.valid && territory.inside && territory.owningTeam != 0
                && ambientAt(s.pos) > fp.isoThreshold;
        if (hostileField)
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
        placeStructure(req.type, req.pos, req.nodeIndex, req.facing, req.team);
    m_requests.clear();
    for (const Cable& req : m_cableRequests)
        applyCableRequest(req.idA, req.idB, req.type, req.team);
    m_cableRequests.clear();
    for (const auto& [id, team] : m_demolishRequests)
        applyDemolishRequest(id, team);
    m_demolishRequests.clear();

    // MINERAL income fills each producer's OWN buffer (full = production stalls, the generator
    // rule); Conveyor links haul it to Silos/the Base in tickPower's flow pass. The Base trickle
    // lands straight in its bank. FUEL income is per-tank and lives in tickPower.
    for (Structure& s : m_structures)
    {
        if (s.blueprint)
            continue;
        if (s.type == EStructureType::Extractor && s.powered && s.nodeIndex >= 0
            && m_nodes[s.nodeIndex].type == ENodeType::Mineral)
            s.mineralStore = glm::min(s.mineralStore + m_mineralRate * deltaSec, mineralCapacityOf(s.type));
        else if (s.type == EStructureType::Fabricator && s.powered)
            s.mineralStore = glm::min(s.mineralStore + m_fabricatorMineralsPerSec * deltaSec,
                mineralCapacityOf(s.type)); // energy+fuel -> minerals converter
        else if (s.type == EStructureType::Base)
            s.mineralStore = glm::min(s.mineralStore + m_mineralRate * m_baseIncomeMult * deltaSec,
                mineralCapacityOf(s.type)); // trickle, not an extractor
    }

    // CONSTRUCTORS: a powered Constructor invests its conveyor-fed mineral stock into the nearest
    // own-team blueprint OR damaged structure in range — automated build + repair lines. With
    // nothing to build, it instead SPEEDS UP the nearest own-team barracks (materials + energy).
    m_barracksBoost.clear();
    for (Structure& s : m_structures)
    {
        if (s.blueprint || s.type != EStructureType::Constructor || !s.powered || s.mineralStore <= 0.0f)
            continue;
        const float budget = glm::min(m_constructorBuildRate * deltaSec, s.mineralStore);
        const float spent = fundNearbyBlueprint(s.pos, m_constructorRange, s.team, budget, true);
        s.mineralStore -= spent;
        if (spent > 0.0f)
            continue; // build/repair takes priority over boosting
        int barracks = -1;
        float bestDist = m_constructorRange;
        for (int i = 0; i < (int)m_structures.size(); ++i)
        {
            const Structure& b = m_structures[i];
            if (!isBarracksType(b.type) || b.blueprint || b.team != s.team)
                continue;
            const float dist = glm::distance(glm::vec2(b.pos.x, b.pos.z), glm::vec2(s.pos.x, s.pos.z));
            if (dist < bestDist)
            {
                bestDist = dist;
                barracks = i;
            }
        }
        if (barracks < 0)
            continue;
        const float mats = m_constructorBoostMaterials * deltaSec;
        const float energy = m_constructorBoostEnergy * deltaSec;
        if (s.mineralStore < mats || s.charge < energy)
            continue; // the boost is all-or-nothing per tick — starved constructors idle
        s.mineralStore -= mats;
        s.charge -= energy;
        m_barracksBoost[m_structures[barracks].id] += m_constructorBoostRate;
    }
    // SPENDABLE minerals = the team's Silo + Base stores (extractor buffers must be conveyed home).
    for (float& m : m_minerals)
        m = 0.0f;
    for (const Structure& s : m_structures)
        if (s.type == EStructureType::MineralSilo || s.type == EStructureType::Base)
            m_minerals[s.team] += s.mineralStore;

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
        const glm::vec3 hue = c.type == ECableType::Pipe ? glm::vec3(1.0f, 0.55f, 0.15f)     // fuel orange
            : c.type == ECableType::Conveyor ? glm::vec3(0.35f, 0.5f, 1.0f)                  // mineral blue
            : c.type == ECableType::Heavy ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        const uint32 bodyColor = packColor(hue * (0.25f + 0.75f * util));

        // Wireframe TUBE (hexagonal prism of debug lines) instead of a single line — reads as a
        // cylinder while keeping the per-cable color coding (type hue, utilization brightness,
        // flow pulse) that a mesh instance could not carry. Radius by type.
        const float radius = c.type == ECableType::Heavy ? 0.14f
                           : c.type == ECableType::Conveyor ? 0.12f
                           : c.type == ECableType::Pipe ? 0.11f : 0.07f;
        const glm::vec3 axis = posB - posA;
        const float len = glm::length(axis);
        if (len < 1e-3f)
            continue;
        const glm::vec3 dir = axis / len;
        const glm::vec3 side = glm::normalize(glm::abs(dir.y) < 0.99f
            ? glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)) : glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 up = glm::cross(dir, side);
        constexpr int TubeSides = 6;
        glm::vec3 prevOffset = side * radius; // ring offset at angle 0
        for (int k = 1; k <= TubeSides; ++k)
        {
            const float a = float(k) / TubeSides * glm::two_pi<float>();
            const glm::vec3 offset = (side * std::cos(a) + up * std::sin(a)) * radius;
            Globals::rendererVK.addDebugLine(posA + offset, posB + offset, bodyColor);     // longitudinal
            Globals::rendererVK.addDebugLine(posA + prevOffset, posA + offset, bodyColor); // end ring A
            Globals::rendererVK.addDebugLine(posB + prevOffset, posB + offset, bodyColor); // end ring B
            prevOffset = offset;
        }
        if (util > 0.02f)
        {
            float t = glm::fract(m_time * (0.4f + 1.2f * util));
            if (c.lastFlowRate < 0.0f)
                t = 1.0f - t; // pulse travels WITH the energy (A->B positive)
            const glm::vec3 p = glm::mix(posA, posB, t);
            // the pulse is a small ring sliding along the tube
            glm::vec3 prevRing = side * (radius * 1.8f);
            for (int k = 1; k <= TubeSides; ++k)
            {
                const float a = float(k) / TubeSides * glm::two_pi<float>();
                const glm::vec3 offset = (side * std::cos(a) + up * std::sin(a)) * (radius * 1.8f);
                Globals::rendererVK.addDebugLine(p + prevRing, p + offset, packColor(hue));
                prevRing = offset;
            }
        }
    }

    // Node rings: blue = mineral, orange = fuel — always at full resource color (the extractor
    // standing on it already tells extracted from free).
    for (const Node& node : m_nodes)
    {
        const glm::vec3 base = node.type == ENodeType::Mineral
            ? glm::vec3(0.2f, 0.4f, 1.0f) : glm::vec3(1.0f, 0.6f, 0.15f);
        drawCircle(glm::vec3(node.pos.x, 0.3f, node.pos.z), m_extractorSnapRadius, packColor(base));
    }

    // Constructor build/repair reach — the placement-planning ring (amber, like its tint).
    const uint32 constructorRing = packColor(glm::vec3(1.0f, 0.75f, 0.45f) * 0.6f);
    for (const Structure& s : m_structures)
        if (s.type == EStructureType::Constructor && !s.blueprint)
            drawCircle(glm::vec3(s.pos.x, 0.4f, s.pos.z), m_constructorRange, constructorRing, 40);

    // Unpowered consumers get a red marker ring so "why is there no bubble / no income" is
    // answerable at a glance (unconnected extractors show it too — cable them to a powered grid).
    const uint32 unpoweredColor = packColor(glm::vec3(1.0f, 0.25f, 0.2f));
    for (const Structure& s : m_structures)
        if ((isEmitterType(s.type) || s.type == EStructureType::Extractor
            || s.type == EStructureType::Constructor
            || s.type == EStructureType::Fabricator) && !s.powered && !s.blueprint)
            drawCircle(glm::vec3(s.pos.x, 0.3f, s.pos.z), 1.0f, unpoweredColor, 16);
}
