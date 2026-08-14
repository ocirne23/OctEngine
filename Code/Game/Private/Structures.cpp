module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Tweaks;
import Core.Transform;
import Entity;
import File;
import Force;
import RendererVK;
import Spatial;
import :Structures;

// See Structures.ixx: there is NO structure roster. Structures are entities; identity, stores and
// LINKS live on their GameStructureComponent (flows run per-entity in the engine's pass). This
// file is the seam: placement/requests (MP validation), production, sweeps, mirrors, save/load —
// all iterating m_frame, a per-frame spatial-query view.

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
// Spawn height = each prefab's box HALF height, so every shape sits flush (see the prefabs).
static constexpr float structureSpawnHeights[] = { 1.0f, 1.0f, 3.0f, 2.0f, 0.5f, 2.0f, 0.5f, 2.0f,
    2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 2.0f, 2.0f, 4.0f, 1.0f, 3.0f };
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
    return m_frame[index].entity->pos
        + glm::vec3(0.0f, structureSpawnHeights[(int)m_frame[index].type] + 0.7f, 0.0f);
}

glm::vec2 StructureSystem::structureFacing(int index) const
{
    if (m_frame[index].type != EStructureType::Lance)
        return glm::vec2(0.0f); // only the Lance carries an authored facing (join replay)
    const glm::vec3 forward = m_frame[index].entity->rot * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::vec2(forward.x, forward.z);
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

// tier <-> throughput lookup for the engine links (tweak-live: refresh() re-stamps per frame).
static ECableType tierOf(const GameStructureLink& l) { return (ECableType)l.cableTier; }

void StructureSystem::registerTweaks()
{
    // Gameplay tweaks persist between runs and the server's values overrule the clients'.
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::boolean("Game/Construction", "Free instant build", &m_cheatInstantBuild);
    Tweak::floatVar("Game/Structures", "Pressure draw tension", &m_pressureDrawTension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Start minerals", &m_startMinerals, 0.0f, 1000.0f, 1.0f);
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
    Tweak::floatVar("Game/Structures", "Waypoint radius", &m_waypointRadius, 0.5f, 15.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Mineral base capacity", &m_mineralBaseCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo capacity", &m_mineralSiloCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Wall cost (per segment)", &m_costs[14], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Turret cost", &m_costs[15], 0.0f, 500.0f, 1.0f);
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
    // The territory drain runs in GameStructureComponent::update — tune its shared param.
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field",
        &GameStructureComponent::params.fieldDamageRate, 0.0f, 100.0f, 0.5f);
    // Production tuning consumed by the component's machine logic (names unchanged — the saved
    // cfg keys keep applying).
    GameStructureParams& sp = GameStructureComponent::params;
    Tweak::intVar("Game/Friendlies", "Barracks unit limit", &sp.barracksUnitLimit, 0, 16, 1);
    Tweak::floatVar("Game/Friendlies", "Barracks spawn interval", &sp.barracksSpawnInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Grunt spawn materials", &m_spawnMaterials[0], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Brute spawn materials", &m_spawnMaterials[1], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Runner spawn materials", &m_spawnMaterials[2], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Spitter spawn materials", &m_spawnMaterials[3], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret range", &sp.turretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret fire interval", &sp.turretFireInterval, 0.1f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Turret shot energy", &sp.turretShotEnergy, 0.0f, 20.0f, 0.1f);
}

// ---------------------------------------------------------------- frame view

void StructureSystem::refresh()
{
    m_frame.clear();
    m_byId.clear();
    thread_local std::vector<uint64> results;
    Globals::spatialIndex.querySphere(glm::dvec3(0.0), 1000.0f, SpatialLayer_Render, results);
    for (const uint64 user : results)
    {
        Entity* entity = reinterpret_cast<Entity*>(user);
        GameStructureComponent* state = getComponent<GameStructureComponent>(entity);
        if (!state || state->structureId == 0)
            continue; // not a placed structure (id 0 = never registered)
        Ref ref;
        ref.entity = entity;
        ref.state = state;
        // Type from the entity name (set at spawn; unique per type).
        const std::string_view name = entity->getName();
        ref.type = EStructureType::Emitter;
        for (int t = 0; t < (int)EStructureType::Count; ++t)
            if (name == structureNames[t])
            {
                ref.type = (EStructureType)t;
                break;
            }
        // Extractors: derive the node association by NEAREST node. The placement grid-snaps the
        // node's ground position (odd-coordinate nodes shift up to ~1.4 m), so an exact-position
        // match fails — take the closest node within the snap displacement bound.
        if (ref.type == EStructureType::Extractor)
        {
            float bestDistSq = 2.0f * 2.0f;
            for (int n = 0; n < (int)m_nodes.size(); ++n)
            {
                const glm::vec2 d = glm::vec2(m_nodes[n].pos.x, m_nodes[n].pos.z)
                    - glm::vec2(entity->pos.x, entity->pos.z);
                if (glm::dot(d, d) < bestDistSq)
                {
                    bestDistSq = glm::dot(d, d);
                    ref.nodeIndex = n;
                }
            }
        }
        m_byId[state->structureId] = (int)m_frame.size();
        m_frame.push_back(ref);
        stampTuning(ref); // capacities/bands per tick — tweaks stay live for the engine's flow
    }
}

void StructureSystem::stampTuning(const Ref& s)
{
    GameStructureComponent& c = *s.state;
    c.healthMax = m_structureHealthMax;
    c.capacity[0] = energyCapacityOf(s.type);
    c.capacity[1] = fuelCapacityOf(s.type);
    c.capacity[2] = mineralCapacityOf(s.type);
    for (int m = 0; m < 3; ++m)
        c.store[m] = glm::min(c.store[m], c.capacity[m]);
    // Gravity bands per medium (see GameComponents.ixx): higher exports to lower at full
    // throughput, equal bands balance by fill fraction. Producer 2 / storage-relay 1 / consumer 0,
    // except MINERALS, where the Base sits on its own band 2 BETWEEN the producers (3) and the
    // silos (1): extractors still dump into it at full rate, but its own trickle PREFERS flowing
    // out (to silos and to whatever spends minerals) over sitting in the bank — it only keeps
    // what its receivers cannot take.
    if (s.type == EStructureType::Connector)
    {
        c.band[0] = c.band[1] = c.band[2] = 1;
    }
    else
    {
        c.band[0] = s.type == EStructureType::Generator || s.type == EStructureType::Solar ? 2
                  : s.type == EStructureType::Battery ? 1 : 0;
        c.band[1] = s.type == EStructureType::Extractor ? 2
                  : s.type == EStructureType::FuelTank ? 1 : 0;
        c.band[2] = s.type == EStructureType::Extractor || s.type == EStructureType::Fabricator ? 3
                  : s.type == EStructureType::Base ? 2
                  : s.type == EStructureType::MineralSilo ? 1 : 0;
    }
    // Link throughputs follow the live tweaks.
    for (GameStructureLink& l : c.links)
        l.throughput = m_cableThroughput[l.cableTier];
    // The union's machine variant (barracks spawn / turret fire logic runs per-entity in the
    // component update; the per-TYPE spawn cost is stamped here so the tweak stays live).
    if (isBarracksType(s.type))
    {
        c.machineKind = GameStructureComponent::EMachineKind::Barracks;
        c.barracks.spawnCost = m_spawnMaterials[
            s.type == EStructureType::BarracksBrute ? 1
            : s.type == EStructureType::BarracksRunner ? 2
            : s.type == EStructureType::BarracksSpitter ? 3 : 0];
    }
    else
        c.machineKind = s.type == EStructureType::Turret
            ? GameStructureComponent::EMachineKind::Turret
            : GameStructureComponent::EMachineKind::None;
}

void StructureSystem::clear()
{
    for (const Ref& s : m_frame)
    {
        s.state->unlinkAll(*s.entity);
        Globals::world.removeRootEntity(s.entity);
    }
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_frame.clear();
    m_byId.clear();
    m_nodes.clear();
    m_requests.clear();
    m_cableRequests.clear();
    m_demolishRequests.clear();
    m_routeRequests.clear();
}

// ---------------------------------------------------------------- nodes + world

void StructureSystem::spawnNodes()
{
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
    // The CORRIDOR arena (GameMatch::spawnCorridorWalls: x -65..65, z -20..20, bases at
    // x = -55 / +55): every node lives on a SIDE, exactly mirrored (180° symmetry) — the center
    // stays EMPTY; each side's FORWARD fuel node (±14) is the exposed prize near the middle.
    static constexpr struct { float x, z; ENodeType type; } corridor[] = {
        { -45.0f,   8.0f, ENodeType::Mineral }, {  45.0f,  -8.0f, ENodeType::Mineral },
        { -37.0f, -10.0f, ENodeType::Fuel },    {  37.0f,  10.0f, ENodeType::Fuel },
        { -25.0f,  12.0f, ENodeType::Mineral }, {  25.0f, -12.0f, ENodeType::Mineral },
        { -14.0f, -10.0f, ENodeType::Fuel },    {  14.0f,  10.0f, ENodeType::Fuel },
    };
    for (const auto& n : corridor)
        spawnNode(n.x, n.z, n.type);
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
        if (glm::dot(d, d) <= bestDistSq)
        {
            bestDistSq = glm::dot(d, d);
            best = i;
        }
    }
    return best;
}

int StructureSystem::findConnectableNear(const glm::vec3& pos, float maxDist) const
{
    int best = -1;
    float bestDistSq = maxDist * maxDist;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        const glm::vec3 p = m_frame[i].entity->pos;
        const glm::vec2 d = glm::vec2(p.x, p.z) - glm::vec2(pos.x, pos.z);
        if (glm::dot(d, d) <= bestDistSq)
        {
            bestDistSq = glm::dot(d, d);
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------- grid placement

glm::vec3 StructureSystem::snapToGrid(EStructureType type, const glm::vec3& groundPos)
{
    // Odd footprints center on a CELL, even ones on a corner — either way the footprint covers
    // whole cells exactly.
    const float offset = (footprintCellsOf(type) & 1) ? GridCellSize * 0.5f : 0.0f;
    const auto snap = [&](float v) { return std::round((v - offset) / GridCellSize) * GridCellSize + offset; };
    return glm::vec3(snap(groundPos.x), 0.0f, snap(groundPos.z));
}

bool StructureSystem::cellsFree(EStructureType type, const glm::vec3& p) const
{
    const float half = footprintCellsOf(type) * GridCellSize * 0.5f;
    if (m_hasBounds && (p.x - half < m_boundsMin.x - 1e-3f || p.x + half > m_boundsMax.x + 1e-3f
        || p.z - half < m_boundsMin.y - 1e-3f || p.z + half > m_boundsMax.y + 1e-3f))
        return false; // footprints stay fully inside the arena
    const auto overlaps = [&](const glm::vec3& q, float halfB) {
        return glm::abs(p.x - q.x) < half + halfB - 1e-3f && glm::abs(p.z - q.z) < half + halfB - 1e-3f; };
    for (const Ref& s : m_frame)
        if (overlaps(s.entity->pos, footprintCellsOf(s.type) * GridCellSize * 0.5f))
            return false;
    // FREE nodes reserve their future extractor's footprint (buildings can't block extraction);
    // extracted nodes rely on the standing extractor's own cells.
    if (type != EStructureType::Extractor)
    {
        const float exHalf = footprintCellsOf(EStructureType::Extractor) * GridCellSize * 0.5f;
        for (const Node& n : m_nodes)
            if (!n.extracted
                && overlaps(snapToGrid(EStructureType::Extractor, glm::vec3(n.pos.x, 0.0f, n.pos.z)), exHalf))
                return false;
    }
    return true;
}

// ---------------------------------------------------------------- requests

void StructureSystem::queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing, uint8 team)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, facing, nodeIndex, team });
}

void StructureSystem::queueCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team)
{
    m_cableRequests.push_back(CableRequest{ idA, idB, type, team });
}

void StructureSystem::queueDemolishRequest(uint32 id, uint8 team)
{
    m_demolishRequests.push_back({ id, team });
}

void StructureSystem::queueRouteRequest(uint32 id, std::span<const glm::vec3> points, uint8 team)
{
    RouteRequest request;
    request.id = id;
    request.team = team;
    const size_t count = glm::min(points.size(), (size_t)MaxRouteWaypoints);
    request.points.assign(points.begin(), points.begin() + count);
    m_routeRequests.push_back(std::move(request));
}

// ---------------------------------------------------------------- spawn/destroy

int StructureSystem::spawnStructure(uint32 id, EStructureType type, const glm::vec3& pos,
    const glm::quat& rot, uint8 team, bool built, int nodeIndex)
{
    EntityPtr entity = Globals::world.spawnAssetFile(structurePrefabs[(int)type],
        Transform(pos, 1.0f, rot), true);
    if (!entity)
        return -1;
    GameStructureComponent* state = getComponent<GameStructureComponent>(entity.get());
    if (!state)
    {
        Log::warning(std::string(structurePrefabs[(int)type])
            + " has no GameStructure component — placement refused");
        return -1;
    }
    entity->setName(structureNames[(int)type]);
    Globals::world.addRootEntity(entity);
    state->structureId = id;
    state->team = team;
    state->healthMax = m_structureHealthMax;
    state->blueprint = !built;
    state->health = built ? m_structureHealthMax : 1.0f; // health IS the build progress
    if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 0 — the builder's team owns the field
    Ref ref;
    ref.entity = entity.get();
    ref.state = state;
    ref.type = type;
    ref.nodeIndex = type == EStructureType::Extractor ? nodeIndex : -1;
    if (ref.nodeIndex >= 0 && ref.nodeIndex < (int)m_nodes.size())
        m_nodes[ref.nodeIndex].extracted = true;
    stampTuning(ref);
    applyStructureTint(ref); // blueprint gray until built
    m_nextStructureId = glm::max(m_nextStructureId, id + 1);
    m_byId[id] = (int)m_frame.size();
    m_frame.push_back(ref);
    return (int)m_frame.size() - 1;
}

void StructureSystem::destroyStructureAt(size_t index)
{
    const Ref s = m_frame[index];
    const uint32 id = s.state->structureId;
    if (s.nodeIndex >= 0 && s.nodeIndex < (int)m_nodes.size())
        m_nodes[s.nodeIndex].extracted = false; // a removed extractor frees its node
    s.state->unlinkAll(*s.entity); // neighbors' link entries drop BEFORE the entity dies
    Globals::world.removeRootEntity(s.entity);
    m_frame.erase(m_frame.begin() + index);
    m_byId.clear();
    for (int i = 0; i < (int)m_frame.size(); ++i)
        m_byId[m_frame[i].state->structureId] = i;
    if (onStructureRemoved)
        onStructureRemoved(id);
}

void StructureSystem::applyDemolishRequest(uint32 id, uint8 team)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return; // already gone
    if (m_frame[index].type == EStructureType::Base)
        return; // the respawn anchor is not deletable
    if (m_frame[index].state->team != team)
        return; // only the owning team demolishes its structures
    Log::info(std::string(structureNames[(int)m_frame[index].type]) + " demolished");
    destroyStructureAt((size_t)index);
}

void StructureSystem::spawnBase(const glm::vec3& groundPos, uint8 team)
{
    const glm::vec3 pos = groundPos + glm::vec3(0.0f, structureSpawnHeights[(int)EStructureType::Base], 0.0f);
    const int index = spawnStructure(m_nextStructureId++, EStructureType::Base, pos,
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), team, /*built*/ true, -1);
    if (index >= 0) // the starting war chest (the Base stores ONLY minerals — no energy, no fuel)
        m_frame[index].state->store[2] = glm::min(m_startMinerals, m_mineralBaseCapacity);
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
    if (type == EStructureType::Extractor)
    {
        if (nodeIndex < 0 || nodeIndex >= (int)m_nodes.size() || m_nodes[nodeIndex].extracted)
            return; // two same-frame requests for one node race — validated at apply
    }
    const glm::vec3 pos = snappedGround + glm::vec3(0.0f, structureSpawnHeights[(int)type], 0.0f);
    // Lance orientation (its prefab's Force axis is entity -Z): the AIMED facing from the
    // two-click placement when given, else auto — away from the own Base.
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (type == EStructureType::Lance)
    {
        glm::vec2 dir(0.0f);
        if (glm::dot(glm::vec2(facing.x, facing.z), glm::vec2(facing.x, facing.z)) > 1e-4f)
            dir = glm::normalize(glm::vec2(facing.x, facing.z));
        else
            for (const Ref& other : m_frame)
                if (other.type == EStructureType::Base && other.state->team == team)
                {
                    const glm::vec2 d = glm::vec2(pos.x, pos.z)
                        - glm::vec2(other.entity->pos.x, other.entity->pos.z);
                    if (glm::dot(d, d) > 1e-4f)
                        dir = glm::normalize(d);
                    break;
                }
        if (glm::dot(dir, dir) > 0.5f)
            rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // CHEAT ("Free instant build", Synced — the server's value rules): skip the blueprint phase.
    const int index = spawnStructure(m_nextStructureId++, type, pos, rot, team,
        /*built*/ m_cheatInstantBuild, type == EStructureType::Extractor ? nodeIndex : -1);
    if (index >= 0 && onStructurePlaced)
        onStructurePlaced(index);
}

// ---------------------------------------------------------------- links (cables)

bool StructureSystem::cableExists(uint32 idA, uint32 idB, int medium) const
{
    const int a = structureIndexById(idA), b = structureIndexById(idB);
    if (a < 0 || b < 0)
        return false;
    for (const GameStructureLink& l : m_frame[a].state->links)
        if (l.other.get() == m_frame[b].entity && (medium < 0 || (int)l.medium == medium))
            return true;
    return false;
}

ECableType StructureSystem::cableTypeBetween(uint32 idA, uint32 idB) const
{
    const int a = structureIndexById(idA), b = structureIndexById(idB);
    if (a < 0 || b < 0)
        return ECableType::Count;
    for (const GameStructureLink& l : m_frame[a].state->links)
        if (l.other.get() == m_frame[b].entity)
            return tierOf(l);
    return ECableType::Count;
}

int StructureSystem::cableTotal() const
{
    int n = 0;
    for (const Ref& s : m_frame)
        for (const GameStructureLink& l : s.state->links)
            if (l.owner)
                ++n;
    return n;
}

void StructureSystem::cableAt(int i, uint32& idA, uint32& idB, ECableType& type) const
{
    int n = 0;
    for (const Ref& s : m_frame)
        for (const GameStructureLink& l : s.state->links)
        {
            if (!l.owner)
                continue;
            if (n++ != i)
                continue;
            idA = s.state->structureId;
            const GameStructureComponent* far = getComponent<GameStructureComponent>(l.other.get());
            idB = far ? far->structureId : 0;
            type = tierOf(l);
            return;
        }
    idA = idB = 0;
    type = ECableType::Count;
}

bool StructureSystem::cableAllowed(int indexA, int indexB, ECableType type, bool ignoreExisting) const
{
    if (indexA < 0 || indexB < 0 || indexA == indexB)
        return false;
    const int medium = cableMedium(type);
    // BOTH endpoints must actually USE the medium (capacity > 0) — a link that could never flow
    // is refused instead of dangling dead.
    const auto capOf = [&](EStructureType t) {
        return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
    if (capOf(m_frame[indexA].type) <= 0.0f || capOf(m_frame[indexB].type) <= 0.0f)
        return false;
    // Range: links touching a Connector reach further — they are the long-haul relays.
    const bool viaConnector = m_frame[indexA].type == EStructureType::Connector
        || m_frame[indexB].type == EStructureType::Connector;
    const float range = viaConnector ? m_connectorRange : m_cableRange;
    const glm::vec3 d = m_frame[indexA].entity->pos - m_frame[indexB].entity->pos;
    if (glm::dot(d, d) > range * range)
        return false;
    // A Connector carries exactly ONE medium: every other link touching it must match the new
    // link's medium. ignoreExisting skips the A<->B pair itself (the retype-in-place path).
    for (const int endpoint : { indexA, indexB })
    {
        if (m_frame[endpoint].type != EStructureType::Connector)
            continue;
        const int otherEnd = endpoint == indexA ? indexB : indexA;
        for (const GameStructureLink& l : m_frame[endpoint].state->links)
        {
            // The retype path replaces the pair's SAME-medium link — only that one is exempt.
            if (ignoreExisting && l.other.get() == m_frame[otherEnd].entity && (int)l.medium == medium)
                continue;
            if ((int)l.medium != medium)
                return false;
        }
    }
    return true;
}

ECableType StructureSystem::smartLinkTypeFor(int indexA, int indexB) const
{
    if (indexA < 0 || indexB < 0 || indexA == indexB)
        return ECableType::Count;
    const auto capacityIn = [&](EStructureType t, int medium) {
        return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
    // A candidate medium must be held by BOTH ends and NOT already linked between this pair —
    // a second right-click adds the pair's NEXT medium (e.g. extractor->generator pipe first,
    // then generator->extractor power cable).
    const auto bothHold = [&](int medium) {
        return capacityIn(m_frame[indexA].type, medium) > 0.0f
            && capacityIn(m_frame[indexB].type, medium) > 0.0f
            && !cableExists(m_frame[indexA].state->structureId,
                m_frame[indexB].state->structureId, medium); };
    // 1) A Connector endpoint that already carries a medium FORCES it. Two connectors with
    //    different media = no link.
    int medium = -1;
    for (const int endpoint : { indexA, indexB })
        if (m_frame[endpoint].type == EStructureType::Connector)
            if (const int carried = connectorMedium(endpoint); carried >= 0)
                medium = (medium < 0 || medium == carried) ? carried : -2;
    if (medium == -2)
        return ECableType::Count;
    // 2) The selected building's resource OUTPUT (A first, then B).
    const auto outputMedium = [&](const Ref& s) -> int {
        switch (s.type)
        {
        case EStructureType::Generator:
        case EStructureType::Solar:
        case EStructureType::Battery:     return 0;
        case EStructureType::FuelTank:    return 1;
        case EStructureType::MineralSilo:
        case EStructureType::Fabricator:  return 2;
        case EStructureType::Extractor:
            return s.nodeIndex >= 0 && s.nodeIndex < (int)m_nodes.size()
                && m_nodes[s.nodeIndex].type == ENodeType::Fuel ? 1 : 2;
        default:                          return -1;
        }
    };
    if (medium < 0)
        if (const int out = outputMedium(m_frame[indexA]); out >= 0 && bothHold(out))
            medium = out;
    if (medium < 0)
        if (const int out = outputMedium(m_frame[indexB]); out >= 0 && bothHold(out))
            medium = out;
    // 3) Fall back to the first medium both endpoints hold.
    for (int m = 0; medium < 0 && m < 3; ++m)
        if (bothHold(m))
            medium = m;
    if (medium < 0 || !bothHold(medium))
        return ECableType::Count;
    const ECableType type = medium == 1 ? ECableType::Pipe
                          : medium == 2 ? ECableType::Conveyor : ECableType::Basic;
    return cableAllowed(indexA, indexB, type) ? type : ECableType::Count;
}

void StructureSystem::applyCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team)
{
    // Validated at APPLY time (the MP seam): endpoints may have died, or a same-frame duplicate
    // may already have toggled the pair. Both endpoints must be the requester's team.
    const int a = structureIndexById(idA);
    const int b = structureIndexById(idB);
    if (a < 0 || b < 0 || m_frame[a].state->team != team || m_frame[b].state->team != team)
        return;
    // Per-MEDIUM pair rules: same medium + same tier toggles OFF, same medium + other tier retypes
    // in place, an unlinked medium ADDS alongside the pair's other links.
    const int medium = cableMedium(type);
    const GameStructureLink* existing = m_frame[a].state->findLink(m_frame[b].entity, medium);
    if (existing && (ECableType)existing->cableTier == type)
    {
        GameStructureComponent::unlink(*m_frame[a].entity, *m_frame[b].entity, medium);
        Log::info("Cable removed");
        if (onCableChanged)
            onCableChanged(idA, idB, type, true);
        return;
    }
    if (!cableAllowed(a, b, type, /*ignoreExisting*/ existing != nullptr))
        return;
    GameStructureComponent::link(*m_frame[a].entity, *m_frame[b].entity,
        (uint8)medium, (uint8)type, m_cableThroughput[(int)type]);
    Log::info(existing != nullptr
        ? (type == ECableType::Pipe ? "Retyped to pipeline"
            : type == ECableType::Conveyor ? "Retyped to conveyor"
            : type == ECableType::Heavy ? "Retyped to heavy cable" : "Retyped to basic cable")
        : (type == ECableType::Pipe ? "Pipeline connected"
            : type == ECableType::Conveyor ? "Conveyor connected"
            : type == ECableType::Heavy ? "Heavy cable connected" : "Cable connected"));
    if (onCableChanged)
        onCableChanged(idA, idB, type, false);
}

// ---------------------------------------------------------------- combat/economy helpers

uint32 StructureSystem::randomTargetStructureId(const glm::vec3& nearPos, uint8 attackerTeam) const
{
    // (The units target through their own spatial queries now; this remains for tooling.)
    constexpr int MaxCandidates = 4;
    struct Candidate { float distSq; uint32 id; };
    Candidate best[MaxCandidates];
    int count = 0;
    for (const Ref& s : m_frame)
    {
        if (s.type == EStructureType::Base || s.state->team == attackerTeam)
            continue;
        const glm::vec2 d = glm::vec2(s.entity->pos.x, s.entity->pos.z) - glm::vec2(nearPos.x, nearPos.z);
        const Candidate c{ glm::dot(d, d), s.state->structureId };
        if (count < MaxCandidates)
            best[count++] = c;
        else
        {
            int worst = 0;
            for (int i = 1; i < MaxCandidates; ++i)
                if (best[i].distSq > best[worst].distSq)
                    worst = i;
            if (c.distSq < best[worst].distSq)
                best[worst] = c;
        }
    }
    if (count == 0)
        return 0;
    return best[glm::clamp((int)glm::linearRand(0.0f, (float)count), 0, count - 1)].id;
}

void StructureSystem::damageStructure(uint32 id, float amount)
{
    const int index = structureIndexById(id);
    if (index >= 0)
        m_frame[index].state->damage(amount); // atomic; invulnerable (Base) refuses inside
}

void StructureSystem::spendMinerals(uint8 team, float amount)
{
    // Silos drain first; the Base last — its trickle refills, and keeping silo stock rotating
    // makes conveyor supply lines visibly matter.
    for (const EStructureType pass : { EStructureType::MineralSilo, EStructureType::Base })
        for (const Ref& s : m_frame)
        {
            if (amount <= 0.0f)
                return;
            if (s.type != pass || s.state->team != team)
                continue;
            const float take = glm::min(amount, s.state->store[2]);
            s.state->store[2] -= take;
            amount -= take;
        }
}

void StructureSystem::applyStructureTint(const Ref& s)
{
    RenderComponent* rc = getComponent<RenderComponent>(s.entity);
    if (!rc || !rc->node.isValid())
        return;
    glm::vec3 color = c_blueprintColor;
    if (!s.state->blueprint)
    {
        color = glm::vec3(1.0f); // fallback: authored tint missing
        if (const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(s.entity); info && info->color.x >= 0.0f)
            color = info->color;
    }
    rc->node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(color));
}

float StructureSystem::investMaterials(const Ref& s, float amount)
{
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp — the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT.
    if (amount <= 0.0f || (int)s.type >= NumPlaceableStructures)
        return 0.0f;
    const float materialsPerHp = glm::max(m_costs[(int)s.type], 0.01f) / glm::max(m_structureHealthMax, 1e-3f);
    const float heal = glm::min(amount / materialsPerHp, m_structureHealthMax - s.state->health);
    if (heal <= 0.0f)
        return 0.0f;
    s.state->health += heal;
    if (s.state->blueprint && s.state->health >= m_structureHealthMax - 1e-3f)
    {
        s.state->blueprint = false;
        applyStructureTint(s); // back to the authored color
        Log::info(std::string(structureNames[(int)s.type]) + " constructed");
    }
    return heal * materialsPerHp;
}

float StructureSystem::takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount)
{
    float taken = 0.0f;
    for (const Ref& s : m_frame)
    {
        if (amount - taken <= 0.0f)
            break;
        if (s.state->blueprint || s.state->team != team
            || (s.type != EStructureType::MineralSilo && s.type != EStructureType::Base))
            continue;
        if (glm::distance(glm::vec2(s.entity->pos.x, s.entity->pos.z), glm::vec2(pos.x, pos.z)) > radius)
            continue;
        const float take = glm::min(amount - taken, s.state->store[2]);
        s.state->store[2] -= take;
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
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        const Ref& s = m_frame[i];
        if (s.state->team != team || s.type == EStructureType::Base)
            continue;
        const bool wantsMaterials = s.state->blueprint
            || (includeRepairs && s.state->health < m_structureHealthMax - 1e-3f);
        if (!wantsMaterials)
            continue;
        const float dist = glm::distance(glm::vec2(s.entity->pos.x, s.entity->pos.z), glm::vec2(pos.x, pos.z));
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best >= 0 ? investMaterials(m_frame[best], amount) : 0.0f;
}

// ---------------------------------------------------------------- production + sweeps

void StructureSystem::tickProduction(float deltaSec)
{
    const float dt = glm::max(deltaSec, 1e-6f);
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_emitterEnergyPerSec
             : type == EStructureType::Extractor ? m_extractorEnergyPerSec
             : type == EStructureType::Constructor ? m_extractorEnergyPerSec // powered while building
             : type == EStructureType::Fabricator ? m_fabricatorEnergyPerSec : 0.0f;
    };

    m_genRateTotal = 0.0f;
    m_useRateTotal = 0.0f;
    float totalDemand = 0.0f;
    for (const Ref& ref : m_frame)
    {
        GameStructureComponent& s = *ref.state;
        if (s.blueprint)
        {
            s.powered = false;
            if (isEmitterType(ref.type))
            {
                s.emitter.outputFrac = 0.0f;
                if (ForceComponent* fc = getComponent<ForceComponent>(ref.entity))
                    fc->emitter.setOutput(0.0f);
            }
            continue;
        }

        // ---- income: powered extractors fill their OWN buffer/tank (full = production stalls),
        // the Base trickles minerals into its bank, fabricators convert (energy+fuel -> minerals).
        if (ref.type == EStructureType::Extractor && s.powered && ref.nodeIndex >= 0)
        {
            if (m_nodes[ref.nodeIndex].type == ENodeType::Fuel)
                s.store[1] = glm::min(s.store[1] + m_fuelRate * dt, s.capacity[1]);
            else
                s.store[2] = glm::min(s.store[2] + m_mineralRate * dt, s.capacity[2]);
        }
        else if (ref.type == EStructureType::Fabricator && s.powered)
            s.store[2] = glm::min(s.store[2] + m_fabricatorMineralsPerSec * dt, s.capacity[2]);
        else if (ref.type == EStructureType::Base)
            s.store[2] = glm::min(s.store[2] + m_mineralRate * m_baseIncomeMult * dt, s.capacity[2]);

        // ---- producers: generators burn their OWN tank into their OWN buffer (full buffer =
        // export-limited = no fuel burn), solar trickles for free.
        if (ref.type == EStructureType::Solar)
        {
            const float add = glm::min(m_solarEnergyPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            m_genRateTotal += add / dt;
        }
        else if (ref.type == EStructureType::Generator && m_genEnergyPerSec > 0.0f)
        {
            float want = glm::min(m_genEnergyPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            if (want > 0.0f)
            {
                const float fuelNeeded = m_fuelBurnRate * want / m_genEnergyPerSec;
                if (fuelNeeded > 1e-9f)
                {
                    const float fuelTaken = glm::min(fuelNeeded, s.store[1]);
                    want *= fuelTaken / fuelNeeded;
                    s.store[1] -= fuelTaken;
                }
                s.store[0] += want;
                m_genRateTotal += want / dt;
            }
        }

        // ---- consumers drain their internal battery. Emitters pay EXTRA per unit of pressure,
        // LATCH OFF at empty until "Emitter restart charge" and ramp their bubble smoothly.
        if (isEmitterType(ref.type))
        {
            ForceComponent* fc = getComponent<ForceComponent>(ref.entity);
            const float pressure = fc ? fc->emitter.getPressure() : 0.0f;
            const float draw = emitterDrawOf(ref.type)
                + pressure * (1.0f + m_pressureDrawTension * pressure) * m_emitterPressureDraw
                + s.emitter.unitLoad; // enemy units/shots leaning on the bubble
            s.emitter.unitLoad = 0.0f;
            totalDemand += draw;
            if (s.emitter.down && s.store[0] >= glm::min(m_emitterRestartCharge, m_internalBuffer))
                s.emitter.down = false;
            bool paid = false;
            if (!s.emitter.down)
            {
                paid = s.store[0] >= draw * dt - 1e-4f;
                if (paid)
                {
                    s.store[0] -= draw * dt;
                    m_useRateTotal += draw;
                }
                else
                    s.emitter.down = true;
            }
            s.powered = paid;
            const float target = paid ? 1.0f : 0.0f;
            const float rampTime = glm::max(target > s.emitter.outputFrac ? m_emitterGrowTime : m_emitterShrinkTime, 0.01f);
            s.emitter.outputFrac = glm::clamp(s.emitter.outputFrac
                + (target > s.emitter.outputFrac ? dt : -dt) / rampTime, 0.0f, 1.0f);
            if (fc)
            {
                // NO sentinel: a dark emitter's field is fully gone.
                fc->emitter.setOutput(emitterOutputOf(ref.type) * s.emitter.outputFrac);
                fc->emitter.setReach(emitterReachOf(ref.type));
            }
            continue;
        }
        const float draw = consumerDraw(ref.type);
        if (draw <= 0.0f)
            continue;
        totalDemand += draw;
        // Fabricators burn fuel alongside energy (piped into their own tank) — both must be there.
        const float fuelDraw = ref.type == EStructureType::Fabricator ? m_fabricatorFuelPerSec : 0.0f;
        s.powered = s.store[0] >= draw * dt - 1e-4f && s.store[1] >= fuelDraw * dt - 1e-4f;
        if (s.powered)
        {
            s.store[0] -= draw * dt;
            s.store[1] -= fuelDraw * dt;
            m_useRateTotal += draw;
        }
    }

    // ---- totals ----
    m_gridEnergyTotal = 0.0f;
    m_gridCapacityTotal = 0.0f;
    float fuelSum = 0.0f;
    for (float& f : m_fuelTotal)
        f = 0.0f;
    for (float& m : m_minerals)
        m = 0.0f;
    for (const Ref& s : m_frame)
    {
        // The HUD's "energy stored" counts DEDICATED storage only (Battery) — production buffers
        // and consumer trickle buffers are working charge, not reserves.
        if (s.type == EStructureType::Battery)
        {
            m_gridEnergyTotal += s.state->store[0];
            m_gridCapacityTotal += s.state->capacity[0];
        }
        // SPENDABLE minerals = the team's Silo + Base stores (buffers must be conveyed home).
        if (s.type == EStructureType::MineralSilo || s.type == EStructureType::Base)
            m_minerals[s.state->team] += s.state->store[2];
        m_fuelTotal[s.state->team] += s.state->store[1];
        fuelSum += s.state->store[1];
    }
    // Fuel-collapse tripwire: make the FIRST domino of the death spiral loud.
    const bool fuelDry = fuelSum <= 0.01f && totalDemand > 0.0f;
    if (fuelDry && !m_wasFuelDry)
        Log::warning("FUEL DRY — generators stopped, buffers draining; emitters go dark next");
    m_wasFuelDry = fuelDry;
}

void StructureSystem::tickDamage(float)
{
    // Damage happens ON the entities (territory drain + atomic intake in the component) — this is
    // the DEATH SWEEP plus the strainable mark units aim their siege drain at.
    for (size_t i = 0; i < m_frame.size();)
    {
        const Ref& s = m_frame[i];
        s.state->strainable = isEmitterType(s.type) && !s.state->blueprint
            && s.state->emitter.outputFrac > 0.05f;
        if (s.state->invulnerable || s.state->alive())
        {
            ++i;
            continue;
        }
        Log::info(std::string(structureNames[(int)s.type]) + " destroyed");
        destroyStructureAt(i);
    }
}

void StructureSystem::tickConstructors(float deltaSec)
{
    // A powered Constructor invests its conveyor-fed mineral stock into the nearest own-team
    // blueprint OR damaged structure in range; with nothing to build it SPEEDS UP the nearest
    // own-team barracks (materials + energy, all-or-nothing per tick). The boost lives ON the
    // barracks component (rebuilt here every tick — no id-keyed map).
    for (const Ref& ref : m_frame)
        if (isBarracksType(ref.type))
            ref.state->barracks.boost = 0.0f;
    for (const Ref& ref : m_frame)
    {
        GameStructureComponent& s = *ref.state;
        if (s.blueprint || ref.type != EStructureType::Constructor || !s.powered || s.store[2] <= 0.0f)
            continue;
        const glm::vec3 pos = ref.entity->pos;
        const float budget = glm::min(m_constructorBuildRate * deltaSec, s.store[2]);
        const float spent = fundNearbyBlueprint(pos, m_constructorRange, (uint8)s.team, budget, true);
        s.store[2] -= spent;
        if (spent > 0.0f)
            continue; // build/repair takes priority over boosting
        int barracks = -1;
        float bestDist = m_constructorRange;
        for (int i = 0; i < (int)m_frame.size(); ++i)
        {
            const Ref& b = m_frame[i];
            if (!isBarracksType(b.type) || b.state->blueprint || b.state->team != s.team)
                continue;
            const float dist = glm::distance(glm::vec2(b.entity->pos.x, b.entity->pos.z),
                glm::vec2(pos.x, pos.z));
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
        if (s.store[2] < mats || s.store[0] < energy)
            continue; // starved constructors idle
        s.store[2] -= mats;
        s.store[0] -= energy;
        m_frame[barracks].state->barracks.boost += m_constructorBoostRate;
    }
}

void StructureSystem::tickAuthority(const glm::vec3&, float deltaSec)
{
    m_time += deltaSec;
    refresh(); // the frame view every request/tick below works on
    for (const PlaceRequest& req : m_requests)
        placeStructure(req.type, req.pos, req.nodeIndex, req.facing, req.team);
    m_requests.clear();
    for (const CableRequest& req : m_cableRequests)
        applyCableRequest(req.idA, req.idB, req.type, req.team);
    m_cableRequests.clear();
    for (const auto& [id, team] : m_demolishRequests)
        applyDemolishRequest(id, team);
    m_demolishRequests.clear();
    for (RouteRequest& request : m_routeRequests)
    {
        const int index = structureIndexById(request.id);
        if (index < 0 || !isBarracksType(m_frame[index].type)
            || m_frame[index].state->team != request.team)
            continue; // died, not a barracks, or someone else's — refused (the MP seam)
        GameStructureComponent& s = *m_frame[index].state;
        request.points.resize(glm::min(request.points.size(), (size_t)MaxRouteWaypoints));
        s.route = std::move(request.points);
        if (onRouteChanged)
            onRouteChanged(request.id);
        // LIVE ORDERS: units already spawned from this barracks pick the new route up too (a
        // route change is a rare user action — one arena query then is fine). The march index is
        // KEPT and clamped: an appended route continues where the unit was, a unit that had
        // finished marches to the new tail, and a fresh single-waypoint route restarts at 0.
        thread_local std::vector<uint64> results;
        Globals::spatialIndex.querySphere(glm::dvec3(0.0), 1000.0f, SpatialLayer_Render, results);
        for (const uint64 user : results)
        {
            Entity* unitEntity = reinterpret_cast<Entity*>(user);
            GameUnitComponent* u = getComponent<GameUnitComponent>(unitEntity);
            if (!u || u->sourceId != request.id)
                continue;
            u->routeCount = (uint8)glm::min((int)s.route.size(), (int)GameUnitComponent::MaxRoutePoints);
            for (int i = 0; i < u->routeCount; ++i)
                u->route[i] = s.route[i];
            u->routeIndex = (uint8)glm::min((int)u->routeIndex, glm::max((int)u->routeCount - 1, 0));
        }
    }
    m_routeRequests.clear();

    tickProduction(deltaSec); // flows themselves run per-entity in the engine's pass
    tickConstructors(deltaSec);
    tickDamage(deltaSec);
}

void StructureSystem::tickMirror(float deltaSec)
{
    refresh();
    // Ease each emitter's fraction toward the synced target at ramp-like speed, then drive the
    // LOCAL field from it — the bubble animates as smoothly as the server's own.
    for (const Ref& ref : m_frame)
    {
        if (!isEmitterType(ref.type))
            continue;
        GameStructureComponent& s = *ref.state;
        const float step = glm::clamp(s.emitter.outputFracTarget - s.emitter.outputFrac,
            -deltaSec * 3.0f, deltaSec * 3.0f);
        s.emitter.outputFrac = glm::clamp(s.emitter.outputFrac + step, 0.0f, 1.0f);
        if (ForceComponent* fc = getComponent<ForceComponent>(ref.entity))
        {
            fc->emitter.setOutput(emitterOutputOf(ref.type) * s.emitter.outputFrac);
            fc->emitter.setReach(emitterReachOf(ref.type));
        }
    }
}

// ---------------------------------------------------------------- mirrors

void StructureSystem::mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos,
    const glm::vec2& facingXZ, int nodeIndex, uint8 team, bool built)
{
    if (structureIndexById(id) >= 0 || (int)type >= (int)EStructureType::Count
        || (int)team >= GameMaxTeams)
        return; // duplicate replay / garbage
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::dot(facingXZ, facingXZ) > 1e-4f)
    {
        const glm::vec2 dir = glm::normalize(facingXZ);
        rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    spawnStructure(id, type, pos, rot, team, built, nodeIndex); // client components never damage-sim
}

void StructureSystem::mirrorRemove(uint32 id)
{
    const int index = structureIndexById(id);
    if (index >= 0)
        destroyStructureAt((size_t)index);
}

void StructureSystem::mirrorCable(uint32 idA, uint32 idB, ECableType type, bool removed)
{
    const int a = structureIndexById(idA), b = structureIndexById(idB);
    if (a < 0 || b < 0)
        return;
    if (removed)
        GameStructureComponent::unlink(*m_frame[a].entity, *m_frame[b].entity, cableMedium(type));
    else
        GameStructureComponent::link(*m_frame[a].entity, *m_frame[b].entity,
            (uint8)cableMedium(type), (uint8)type, m_cableThroughput[(int)type]);
}

void StructureSystem::mirrorStructureState(uint32 id, float healthFrac, float chargeFrac,
    float fuelFrac, float mineralFrac, float outputFrac, float utilFrac, bool powered, bool blueprint)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return;
    const Ref& ref = m_frame[index];
    GameStructureComponent& s = *ref.state;
    if (s.blueprint != blueprint)
    {
        s.blueprint = blueprint; // completion (or a fresh ghost) — swap the tint to match
        applyStructureTint(ref);
    }
    s.health = healthFrac * m_structureHealthMax;
    s.store[0] = chargeFrac * energyCapacityOf(ref.type);
    s.store[1] = fuelFrac * fuelCapacityOf(ref.type);
    s.store[2] = mineralFrac * mineralCapacityOf(ref.type);
    s.flowUtil = utilFrac; // already server-smoothed
    if (isEmitterType(ref.type)) // union: emitter variant only
        s.emitter.outputFracTarget = outputFrac; // tickMirror eases the live field toward this
    s.powered = powered;
}

void StructureSystem::mirrorRoute(uint32 id, std::span<const glm::vec3> points)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return;
    if (!isBarracksType(m_frame[index].type))
        return; // routes are a barracks concept
    const size_t count = glm::min(points.size(), (size_t)MaxRouteWaypoints);
    m_frame[index].state->route.assign(points.begin(), points.begin() + count);
}

// ---------------------------------------------------------------- save/load

void StructureSystem::saveTo(AssetNode& root) const
{
    for (const Ref& s : m_frame)
    {
        AssetNode& n = root.addChild("Structure");
        n.set("Id", std::to_string(s.state->structureId));
        n.set("Type", std::to_string((int)s.type));
        n.set("Position", s.entity->pos);
        const glm::vec3 forward = s.type == EStructureType::Lance
            ? s.entity->rot * glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f);
        n.set("Facing", glm::vec3(forward.x, 0.0f, forward.z));
        n.set("NodeIndex", std::to_string(s.nodeIndex));
        n.set("Team", std::to_string((int)s.state->team));
        n.set("Blueprint", s.state->blueprint != 0); // bitfield -> the bool overload
        n.set("Health", s.state->health);
        n.set("Charge", s.state->store[0]);
        n.set("Fuel", s.state->store[1]);
        n.set("Minerals", s.state->store[2]);
        if (isEmitterType(s.type)) // union variants: only the active one is meaningful
            n.set("OutputFrac", s.state->emitter.outputFrac);
        if (isBarracksType(s.type) && !s.state->route.empty())
        {
            AssetNode& r = n.addChild("Route");
            for (const glm::vec3& wp : s.state->route)
            {
                AssetNode& p = r.addChild("Point");
                p.values = { std::to_string(wp.x), std::to_string(wp.z) };
            }
        }
    }
    const int cables = cableTotal();
    for (int i = 0; i < cables; ++i)
    {
        uint32 idA, idB;
        ECableType type;
        cableAt(i, idA, idB, type);
        AssetNode& n = root.addChild("Cable");
        n.set("A", std::to_string(idA));
        n.set("B", std::to_string(idB));
        n.set("Type", std::to_string((int)type));
    }
}

void StructureSystem::clearAllStructures()
{
    while (!m_frame.empty())
        destroyStructureAt(m_frame.size() - 1); // fires onStructureRemoved — clients prune
    m_requests.clear();
    m_cableRequests.clear();
    m_demolishRequests.clear();
    m_routeRequests.clear();
}

void StructureSystem::loadFrom(const AssetNode& root)
{
    refresh();
    clearAllStructures();
    for (const AssetNode* n : root.findAll("Structure"))
    {
        const int typeInt = n->find("Type") ? n->find("Type")->asInt() : -1;
        const uint32 id = n->find("Id") ? (uint32)n->find("Id")->asInt() : 0;
        if (typeInt < 0 || typeInt >= (int)EStructureType::Count || id == 0 || structureIndexById(id) >= 0)
            continue; // garbage / duplicate entry
        const glm::vec3 pos = n->find("Position") ? n->find("Position")->asVec3() : glm::vec3(0.0f);
        const glm::vec3 facing = n->find("Facing") ? n->find("Facing")->asVec3() : glm::vec3(0.0f);
        glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        if (glm::dot(glm::vec2(facing.x, facing.z), glm::vec2(facing.x, facing.z)) > 1e-4f)
        {
            const glm::vec2 dir = glm::normalize(glm::vec2(facing.x, facing.z));
            rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
        }
        const int nodeIndex = n->find("NodeIndex") ? n->find("NodeIndex")->asInt() : -1;
        const uint8 team = (uint8)glm::clamp(n->find("Team") ? n->find("Team")->asInt() : 0, 0, GameMaxTeams - 1);
        const bool blueprint = n->find("Blueprint") ? n->find("Blueprint")->asBool() : false;
        const int index = spawnStructure(id, (EStructureType)typeInt, pos, rot, team, !blueprint, nodeIndex);
        if (index < 0)
            continue;
        GameStructureComponent& s = *m_frame[index].state;
        s.health = glm::clamp(n->find("Health") ? n->find("Health")->asFloat() : m_structureHealthMax,
            1.0f, m_structureHealthMax);
        s.store[0] = glm::clamp(n->find("Charge") ? n->find("Charge")->asFloat() : 0.0f, 0.0f, s.capacity[0]);
        s.store[1] = glm::clamp(n->find("Fuel") ? n->find("Fuel")->asFloat() : 0.0f, 0.0f, s.capacity[1]);
        s.store[2] = glm::clamp(n->find("Minerals") ? n->find("Minerals")->asFloat() : 0.0f, 0.0f, s.capacity[2]);
        if (isEmitterType((EStructureType)typeInt)) // union variants: write only the active one
            s.emitter.outputFrac = glm::clamp(n->find("OutputFrac") ? n->find("OutputFrac")->asFloat() : 0.0f,
                0.0f, 1.0f);
        if (const AssetNode* r = n->find("Route"); r && isBarracksType((EStructureType)typeInt))
            for (const AssetNode* p : r->findAll("Point"))
                if ((int)s.route.size() < MaxRouteWaypoints)
                    s.route.push_back(glm::vec3(p->asFloat(0), 0.0f, p->asFloat(1)));
    }
    for (const AssetNode* n : root.findAll("Cable"))
    {
        const uint32 idA = n->find("A") ? (uint32)n->find("A")->asInt() : 0;
        const uint32 idB = n->find("B") ? (uint32)n->find("B")->asInt() : 0;
        const int type = n->find("Type") ? n->find("Type")->asInt() : -1;
        const int a = structureIndexById(idA), b = structureIndexById(idB);
        if (type >= 0 && type < (int)ECableType::Count && a >= 0 && b >= 0
            && !cableExists(idA, idB, cableMedium((ECableType)type)))
            GameStructureComponent::link(*m_frame[a].entity, *m_frame[b].entity,
                (uint8)cableMedium((ECableType)type), (uint8)type, m_cableThroughput[type]);
    }
    Log::info("Game state loaded: " + std::to_string(m_frame.size()) + " structures, "
        + std::to_string(cableTotal()) + " cables");
}

// ---------------------------------------------------------------- debug draw

void StructureSystem::drawDebug() const
{
    // Links: base hue by TIER, brightness by UTILIZATION, plus a pulse mark travelling in the
    // flow direction. Iterates each structure's OWNED links — no cable list anywhere.
    for (const Ref& s : m_frame)
        for (const GameStructureLink& l : s.state->links)
        {
            if (!l.owner || !l.other)
                continue;
            const ECableType type = tierOf(l);
            // Fixed height per TYPE so parallel links of one pair never overlap: power on top,
            // pipes in the middle, conveyors at the bottom.
            const float lift = l.medium == 0 ? 0.5f : l.medium == 1 ? 0.0f : -0.5f;
            const glm::vec3 posA = s.entity->pos + glm::vec3(0.0f, lift, 0.0f);
            const glm::vec3 posB = l.other->pos + glm::vec3(0.0f, lift, 0.0f);
            // SMOOTHED rate (flowAvg, ~0.25 s EMA on the component): raw per-tick transfers are
            // bursty and made the brightness/pulse strobe. Clients never run the flow sim, so
            // they fall back to the endpoints' GSt-synced utilization gauge.
            float util = glm::abs(l.flowAvg) / glm::max(l.throughput, 1e-3f);
            if (Globals::networkManager.role() == ENetRole::Client)
            {
                const GameStructureComponent* far = getComponent<GameStructureComponent>(l.other.get());
                util = glm::max(s.state->flowUtil, far ? far->flowUtil : 0.0f);
            }
            util = glm::clamp(util, 0.0f, 1.0f);
            const glm::vec3 hue = type == ECableType::Pipe ? glm::vec3(1.0f, 0.55f, 0.15f)
                : type == ECableType::Conveyor ? glm::vec3(0.35f, 0.5f, 1.0f)
                : type == ECableType::Heavy ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
            const uint32 bodyColor = packColor(hue * (0.25f + 0.75f * util));
            const float radius = type == ECableType::Heavy ? 0.14f
                               : type == ECableType::Conveyor ? 0.12f
                               : type == ECableType::Pipe ? 0.11f : 0.07f;
            const glm::vec3 axis = posB - posA;
            const float len = glm::length(axis);
            if (len < 1e-3f)
                continue;
            const glm::vec3 dir = axis / len;
            const glm::vec3 side = glm::normalize(glm::abs(dir.y) < 0.99f
                ? glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)) : glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::vec3 up = glm::cross(dir, side);
            constexpr int TubeSides = 6;
            glm::vec3 prevOffset = side * radius;
            for (int k = 1; k <= TubeSides; ++k)
            {
                const float a = float(k) / TubeSides * glm::two_pi<float>();
                const glm::vec3 offset = (side * std::cos(a) + up * std::sin(a)) * radius;
                Globals::rendererVK.addDebugLine(posA + offset, posB + offset, bodyColor);
                Globals::rendererVK.addDebugLine(posA + prevOffset, posA + offset, bodyColor);
                Globals::rendererVK.addDebugLine(posB + prevOffset, posB + offset, bodyColor);
                prevOffset = offset;
            }
            // Speed is in link lengths per second: ~4 s end to end idling, ~2 s at full load.
            if (util > 0.02f)
            {
                float t = glm::fract(m_time * (0.25f + 0.25f * util));
                if (l.flowAvg < 0.0f)
                    t = 1.0f - t; // pulse travels WITH the flow (self -> other positive)
                const glm::vec3 p = glm::mix(posA, posB, t);
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

    // Node rings: blue = mineral, orange = fuel.
    for (const Node& node : m_nodes)
    {
        const glm::vec3 base = node.type == ENodeType::Mineral
            ? glm::vec3(0.2f, 0.4f, 1.0f) : glm::vec3(1.0f, 0.6f, 0.15f);
        drawCircle(glm::vec3(node.pos.x, 0.3f, node.pos.z), m_extractorSnapRadius, packColor(base));
    }

    // Constructor build/repair reach (amber) + red rings on unpowered consumers.
    const uint32 constructorRing = packColor(glm::vec3(1.0f, 0.75f, 0.45f) * 0.6f);
    const uint32 unpoweredColor = packColor(glm::vec3(1.0f, 0.25f, 0.2f));
    for (const Ref& s : m_frame)
    {
        if (s.type == EStructureType::Constructor && !s.state->blueprint)
            drawCircle(glm::vec3(s.entity->pos.x, 0.4f, s.entity->pos.z), m_constructorRange, constructorRing, 40);
        if ((isEmitterType(s.type) || s.type == EStructureType::Extractor
            || s.type == EStructureType::Constructor
            || s.type == EStructureType::Fabricator) && !s.state->powered && !s.state->blueprint)
            drawCircle(glm::vec3(s.entity->pos.x, 0.3f, s.entity->pos.z), 1.0f, unpoweredColor, 16);
    }
}
