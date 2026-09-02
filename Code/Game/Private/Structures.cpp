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
    "Entities/Game/base.pre", "Entities/Game/cablePower.pre", "Entities/Game/cablePipe.pre",
    "Entities/Game/cableConveyor.pre", "Entities/Game/crossing.pre", "Entities/Game/house.pre" };
static constexpr const char* structureNames[] = { "Emitter", "Generator", "Connector", "Extractor",
    "Battery", "Fuel tank", "Solar", "Fabricator", "Bastion", "Lance", "Barracks", "Brute barracks",
    "Runner barracks", "Spitter barracks", "Wall", "Turret", "Mineral silo", "Constructor", "Base",
    "Power cable", "Pipeline", "Conveyor", "Crossing", "House" };
// Spawn height = each prefab's box HALF height, so every shape sits flush (see the prefabs).
static constexpr float structureSpawnHeights[] = { 1.0f, 1.0f, 3.0f, 2.0f, 0.5f, 2.0f, 0.5f, 2.0f,
    2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 2.0f, 2.0f, 4.0f, 1.0f, 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 1.5f };
static constexpr glm::vec3 c_blueprintColor(0.45f, 0.55f, 0.7f); // ghost tint until built
static_assert(oc::size(structurePrefabs) == (size_t)EStructureType::Count);
static_assert(oc::size(structureNames) == (size_t)EStructureType::Count);
static_assert(oc::size(structureSpawnHeights) == (size_t)EStructureType::Count);

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
    // Lance + Crossing carry an authored facing (join replay + save); the Crossing's decides which
    // cells its 1x3 footprint covers, so it MUST survive the wire or client derivation diverges.
    if (m_frame[index].type != EStructureType::Lance && m_frame[index].type != EStructureType::Crossing)
        return glm::vec2(0.0f);
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
    Tweak::floatVar("Game/Economy", "Extractor cost", &m_costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Battery cost", &m_costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &m_costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Solar cost", &m_costs[6], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fabricator cost", &m_costs[7], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion cost", &m_costs[8], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lance cost", &m_costs[9], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Barracks cost", &m_costs[10], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "House cost", &m_costs[(int)EStructureType::House], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo cost", &m_costs[16], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Constructor cost", &m_costs[17], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Power cable cost", &m_costs[(int)EStructureType::CablePower], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Pipeline cost", &m_costs[(int)EStructureType::CablePipe], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Conveyor cost", &m_costs[(int)EStructureType::CableConveyor], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Crossing cost", &m_costs[(int)EStructureType::Crossing], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Cable health max", &m_cableHealthMax, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Constructor range", &m_constructorRange, 2.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Constructor build rate", &m_constructorBuildRate, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Structures", "Waypoint radius", &m_waypointRadius, 0.5f, 15.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Mineral base capacity", &m_mineralBaseCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo capacity", &m_mineralSiloCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Barracks energy capacity", &m_barracksEnergyCapacity, 1.0f, 500.0f, 1.0f);
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
    Tweak::floatVar("Game/Economy", "Base energy capacity", &m_baseEnergyCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Base energy gen/s", &m_baseEnergyGenPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base shield energy/s", &m_baseShieldEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield output", &m_baseShieldOutput, 0.1f, 8.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield reach", &m_baseShieldReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Extractor energy/s", &m_extractorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Battery capacity", &m_batteryCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Internal buffer", &m_internalBuffer, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Generator buffer", &m_generatorBuffer, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Cable throughput", &m_cableThroughput[0], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Pipeline throughput", &m_cableThroughput[1], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Conveyor throughput", &m_cableThroughput[2], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &m_generatorFuelTank, 5.0f, 500.0f, 1.0f);
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
    Tweak::intVar("Game/Friendlies", "Barracks population", &m_barracksPopulation, 0, 200, 1);
    Tweak::intVar("Game/Friendlies", "House population", &m_housePopulation, 0, 100, 1);
    Tweak::floatVar("Game/Friendlies", "House link radius", &m_houseLinkRadius, 2.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Barracks seconds per energy", &sp.barracksSecondsPerEnergy, 0.05f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Grunt spawn energy", &m_spawnEnergy[0], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Brute spawn energy", &m_spawnEnergy[1], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Runner spawn energy", &m_spawnEnergy[2], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Spitter spawn energy", &m_spawnEnergy[3], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Swarm spawn energy", &m_spawnEnergy[4], 0.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Friendlies", "Grunt population", &m_unitPopulation[0], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Brute population", &m_unitPopulation[1], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Runner population", &m_unitPopulation[2], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Spitter population", &m_unitPopulation[3], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Swarm population", &m_unitPopulation[4], 0, 50, 1);
    Tweak::floatVar("Game/Friendlies", "Turret range", &sp.turretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret fire interval", &sp.turretFireInterval, 0.1f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Turret shot energy", &sp.turretShotEnergy, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Turret damage", &sp.turretDamage, 0.0f, 500.0f, 1.0f);
}

// ---------------------------------------------------------------- frame view

void StructureSystem::refresh()
{
    ProfileScope scope("Structures refresh", EProfileCategory::Game);
    // The roster is maintained at the spawn/remove seams (no world query — type, node and id index
    // were recorded at spawnStructure); only the live tuning re-stamps per frame so tweaks apply.
    linkHouses(); // house counts feed the barracks' population cap stamped below
    for (const Ref& s : m_frame)
        stampTuning(s);
}

void StructureSystem::linkHouses()
{
    for (const Ref& s : m_frame)
        if (isBarracksType(s.type))
            s.state->barracks.houses = 0;
    const float radiusSq = m_houseLinkRadius * m_houseLinkRadius;
    for (Ref& h : m_frame)
    {
        if (h.type != EStructureType::House)
            continue;
        h.linkedId = 0;
        if (h.state->blueprint)
            continue; // a ghost house feeds nothing
        const glm::vec2 hp(h.entity->pos.x, h.entity->pos.z);
        float bestSq = radiusSq;
        const Ref* best = nullptr;
        for (const Ref& b : m_frame)
        {
            if (!isBarracksType(b.type) || b.state->blueprint || b.state->team != h.state->team)
                continue;
            const glm::vec2 d = glm::vec2(b.entity->pos.x, b.entity->pos.z) - hp;
            const float distSq = glm::dot(d, d);
            if (distSq <= bestSq)
            {
                bestSq = distSq;
                best = &b;
            }
        }
        if (!best)
            continue;
        h.linkedId = best->state->structureId;
        best->state->barracks.houses = (uint8)glm::min((int)best->state->barracks.houses + 1, 255);
    }
}

void StructureSystem::stampTuning(const Ref& s)
{
    GameStructureComponent& c = *s.state;
    c.healthMax = isCableOrCrossing(s.type) ? m_cableHealthMax : m_structureHealthMax;
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
    // what its receivers cannot take. Cables hold NO stores (capacity 0 everywhere): they are
    // never link endpoints — links derive between the BUILDINGS their runs touch.
    c.band[0] = s.type == EStructureType::Generator || s.type == EStructureType::Solar ? 2
              : s.type == EStructureType::Battery ? 1 : 0;
    c.band[1] = s.type == EStructureType::Extractor ? 2
              : s.type == EStructureType::FuelTank ? 1 : 0;
    c.band[2] = s.type == EStructureType::Extractor || s.type == EStructureType::Fabricator ? 3
              : s.type == EStructureType::Base ? 2
              : s.type == EStructureType::MineralSilo ? 1 : 0;
    // Link throughputs follow the live tweaks (medium-indexed — tiers are gone).
    for (GameStructureLink& l : c.links)
        l.throughput = m_cableThroughput[glm::min((int)l.medium, 2)];
    // The union's machine variant (barracks spawn / turret fire logic runs per-entity in the
    // component update; the selected unit type's prices + the population cap are stamped here so
    // the tweaks and the house links stay live).
    if (isBarracksType(s.type))
    {
        c.machineKind = GameStructureComponent::EMachineKind::Barracks;
        if (!isBarracksUnitType(c.barracks.unitType))
            c.barracks.unitType = 0;
        c.barracks.spawnCost = m_spawnEnergy[c.barracks.unitType];
        c.barracks.spawnPop = (uint8)glm::clamp(m_unitPopulation[c.barracks.unitType], 0, 255);
        c.barracks.popCap = m_barracksPopulation + (int)c.barracks.houses * m_housePopulation;
    }
    else
        c.machineKind = s.type == EStructureType::Turret
            ? GameStructureComponent::EMachineKind::Turret
            : GameStructureComponent::EMachineKind::None;
}

void StructureSystem::clear()
{
    // Teardown: silent (no GRm hooks). Deregister the whole roster FIRST — removeRootEntity's
    // onWorldRootRemoved callback then no-ops instead of mutating m_frame under the loop.
    oc::vector<Ref> roster = oc::move(m_frame);
    m_frame.clear();
    m_byId.clear();
    for (const Ref& s : roster)
    {
        s.state->unlinkAll(*s.entity);
        Globals::world.removeRootEntity(s.entity);
    }
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_nodes.clear();
    m_requests.clear();
    m_demolishRequests.clear();
    m_routeRequests.clear();
    m_cells.clear();
    m_runs.clear();
    m_terrainBlocked.clear();
    m_linksDirty = true;
}

// ---------------------------------------------------------------- nodes + world

void StructureSystem::spawnNode(float x, float z, ENodeType type)
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
    m_nodes.push_back(oc::move(node));
}

void StructureSystem::clearNodes()
{
    // Co-op map regeneration only (a new seed re-places the whole set). Structures referencing a
    // node by index are cleared by the caller around this — every nodeIndex access elsewhere is
    // bound-checked, so a brief count mismatch cannot read out of range.
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_nodes.clear();
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

glm::ivec2 StructureSystem::footprintExtent(EStructureType t, const glm::quat& rot)
{
    if (t != EStructureType::Crossing)
        return glm::ivec2(footprintCellsOf(t));
    // 1x3 along the facing, quantized to an axis at placement (entity -Z = forward).
    const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::abs(forward.x) >= glm::abs(forward.z) ? glm::ivec2(3, 1) : glm::ivec2(1, 3);
}

bool StructureSystem::cellsFree(EStructureType type, const glm::vec3& p, const glm::quat& rot,
    bool ignoreCables) const
{
    const glm::ivec2 ext = footprintExtent(type, rot);
    const glm::vec2 half(ext.x * GridCellSize * 0.5f, ext.y * GridCellSize * 0.5f);
    if (m_hasBounds && (p.x - half.x < m_boundsMin.x - 1e-3f || p.x + half.x > m_boundsMax.x + 1e-3f
        || p.z - half.y < m_boundsMin.y - 1e-3f || p.z + half.y > m_boundsMax.y + 1e-3f))
        return false; // footprints stay fully inside the arena
    for (const glm::vec4& r : m_terrainBlocked) // co-op rock rects (minX, minZ, maxX, maxZ)
        if (p.x + half.x > r.x + 1e-3f && p.x - half.x < r.z - 1e-3f
            && p.z + half.y > r.y + 1e-3f && p.z - half.y < r.w - 1e-3f)
            return false; // impassable terrain: nothing builds in a rock (unit spawns skip it too)
    // FREE nodes reserve their future extractor's footprint (buildings can't block extraction);
    // extracted nodes rely on the standing extractor's own cells.
    if (type != EStructureType::Extractor)
    {
        const auto overlaps = [&](const glm::vec3& q, float halfB) {
            return glm::abs(p.x - q.x) < half.x + halfB - 1e-3f
                && glm::abs(p.z - q.z) < half.y + halfB - 1e-3f; };
        const float exHalf = footprintCellsOf(EStructureType::Extractor) * GridCellSize * 0.5f;
        for (const Node& n : m_nodes)
            if (!n.extracted
                && overlaps(snapToGrid(EStructureType::Extractor, glm::vec3(n.pos.x, 0.0f, n.pos.z)), exHalf))
                return false;
    }
    // PER-CELL occupancy (the hash the derived links run on): buildings refuse ANY occupied cell,
    // a cable may slot UNDER a crossing's free middle cell, a crossing may bridge OVER exactly one
    // cable in its middle cell. ignoreCables treats cable cells as open (unit-spawn probe).
    bool free = true;
    int cellIdx = -1;
    forEachFootprintCell(type, p, rot, [&](int cx, int cz)
    {
        ++cellIdx;
        if (!free)
            return;
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return; // empty cell
        const CellEntry& entry = it->second;
        const int occIdx = structureIndexById(entry.id);
        const EStructureType occ = occIdx >= 0 ? m_frame[occIdx].type : EStructureType::Emitter;
        if (ignoreCables && isCableOrCrossing(occ))
            return; // walk-through pieces do not block a unit spawn point
        if (isCableType(type))
        {
            // A cable may enter an existing crossing's MIDDLE cell (its center) while it is free.
            if (occIdx >= 0 && occ == EStructureType::Crossing && entry.underId == 0)
            {
                const glm::vec3 cpos = m_frame[occIdx].entity->pos;
                if ((int)std::lround(cpos.x / GridCellSize - 0.5f) == cx
                    && (int)std::lround(cpos.z / GridCellSize - 0.5f) == cz)
                    return;
            }
            free = false;
            return;
        }
        if (type == EStructureType::Crossing)
        {
            // The MIDDLE cell (index 1 of the 3) may hold exactly one plain cable; ends must be free.
            const bool middle = cellIdx == 1;
            if (middle && occIdx >= 0 && isCableType(occ) && entry.underId == 0)
                return;
            free = false;
            return;
        }
        free = false; // buildings refuse any occupied cell (cables included — no building on a cable)
    });
    return free;
}

float StructureSystem::spawnHeightOf(EStructureType type)
{
    return structureSpawnHeights[(int)type]; // = the prefab box's HALF height (it sits flush)
}

bool StructureSystem::actorInFootprint(EStructureType type, const glm::vec3& p)
{
    // A player capsule or a unit standing on the cells blocks the placement: the structure would
    // spawn inside them and the solver would fling whatever it engulfs. Checked at aim (red ghost)
    // AND in placeStructure (the MP seam) — actors move between the two.
    // Cables/crossings are WALK-THROUGH (their collider ignores bodies), so standing on the cells
    // never blocks them.
    if (isCableOrCrossing(type))
        return false;
    constexpr float actorRadius = 0.7f; // capsule/unit body, generous by design
    const float half = footprintCellsOf(type) * GridCellSize * 0.5f + actorRadius;
    thread_local oc::vector<uint64> results;
    Globals::spatialIndex.querySphere(glm::dvec3(p), half * 1.5f, SpatialLayer_Render, results);
    for (const uint64 user : results)
    {
        const Entity* entity = reinterpret_cast<const Entity*>(user);
        if (!hasComponent<GameUnitComponent>(entity)) // units AND player capsules (puppets)
            continue;
        if (glm::abs(entity->pos.x - p.x) < half && glm::abs(entity->pos.z - p.z) < half)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- requests

void StructureSystem::queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing, uint8 team)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, facing, nodeIndex, team });
}

void StructureSystem::queueDemolishRequest(uint32 id, uint8 team)
{
    m_demolishRequests.push_back({ id, team });
}

void StructureSystem::queueRouteRequest(uint32 id, oc::span<const glm::vec3> points, uint8 team)
{
    RouteRequest request;
    request.id = id;
    request.team = team;
    const size_t count = glm::min(points.size(), (size_t)MaxRouteWaypoints);
    request.points.assign(points.begin(), points.begin() + count);
    m_routeRequests.push_back(oc::move(request));
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
        Log::warning(oc::string(structurePrefabs[(int)type])
            + " has no GameStructure component — placement refused");
        return -1;
    }
    entity->setName(structureNames[(int)type]);
    Globals::world.addRootEntity(entity);
    state->structureId = id;
    state->team = team;
    const float healthMax = isCableOrCrossing(type) ? m_cableHealthMax : m_structureHealthMax;
    state->healthMax = healthMax;
    state->blueprint = !built;
    state->health = built ? healthMax : 1.0f; // health IS the build progress
    if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 0 — the builder's team owns the field
    Ref ref;
    ref.owner = entity; // owning: the roster's raw pointers can never dangle
    ref.entity = entity.get();
    ref.state = state;
    ref.type = type;
    ref.nodeIndex = type == EStructureType::Extractor ? nodeIndex : -1;
    if (ref.nodeIndex >= 0 && ref.nodeIndex < (int)m_nodes.size())
        m_nodes[ref.nodeIndex].extracted = true;
    if (isCableType(type)) // cache the 4 render-only arm children (see updateArms)
    {
        static constexpr const char* armNames[4] = { "ArmPX", "ArmNX", "ArmPZ", "ArmNZ" };
        if (const SceneComponent* sc = getComponent<SceneComponent>(entity.get()))
            for (const EntityPtr& child : sc->children)
                for (int a = 0; a < 4; ++a)
                    if (oc::string_view(child->getName()) == armNames[a])
                        ref.arms[a] = child.get();
    }
    stampTuning(ref);
    applyStructureTint(ref); // blueprint gray until built
    m_nextStructureId = glm::max(m_nextStructureId, id + 1);
    m_byId[id] = (int)m_frame.size();
    m_frame.push_back(ref);
    insertCells(m_frame.back()); // the occupancy hash the derived links + placement run on
    m_linksDirty = true;
    return (int)m_frame.size() - 1;
}

// Deregister + full bookkeeping, WITHOUT touching the world's root list — shared by the game's own
// removal (destroyStructureAt) and by onWorldRootRemoved for out-of-band deletions.
void StructureSystem::removeStructureBookkeeping(size_t index)
{
    const Ref s = m_frame[index]; // owning copy: the entity stays alive through the bookkeeping
    const uint32 id = s.state->structureId;
    if (s.nodeIndex >= 0 && s.nodeIndex < (int)m_nodes.size())
        m_nodes[s.nodeIndex].extracted = false; // a removed extractor frees its node
    eraseCells(s); // promotes an under-cable back to primary occupant
    m_linksDirty = true;
    s.state->unlinkAll(*s.entity); // neighbors' link entries drop BEFORE the entity dies
    m_frame.erase(m_frame.begin() + index);
    m_byId.clear();
    for (int i = 0; i < (int)m_frame.size(); ++i)
        m_byId[m_frame[i].state->structureId] = i;
    if (onStructureRemoved)
        onStructureRemoved(id);
}

void StructureSystem::destroyStructureAt(size_t index)
{
    Entity* entity = m_frame[index].entity;
    const EntityPtr keepAlive = m_frame[index].owner; // outlive the roster erase below
    removeStructureBookkeeping(index); // deregister FIRST: removeRootEntity's callback then no-ops
    Globals::world.removeRootEntity(entity);
}

// Any root leaving the world (editor delete, script destroy request — paths that never reach
// destroyStructureAt). Ours are deregistered by then, so this only fires for out-of-band removals.
void StructureSystem::onWorldRootRemoved(const Entity* entity)
{
    const int index = structureIndexByEntity(entity);
    if (index >= 0)
        removeStructureBookkeeping((size_t)index);
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
    Log::info(oc::string(structureNames[(int)m_frame[index].type]) + " demolished");
    destroyStructureAt((size_t)index);
}

void StructureSystem::spawnBase(const glm::vec3& groundPos, uint8 team)
{
    // Same grid snap every placement gets (3x3 = odd footprint -> centered on a CELL): an
    // unsnapped Base sat half a cell off, so nothing placed next to it could line up flush.
    const glm::vec3 pos = snapToGrid(EStructureType::Base, groundPos)
        + glm::vec3(0.0f, structureSpawnHeights[(int)EStructureType::Base], 0.0f);
    const int index = spawnStructure(m_nextStructureId++, EStructureType::Base, pos,
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), team, /*built*/ true, -1);
    if (index >= 0) // the starting war chest + a FULL shield battery (it drains like any emitter)
    {
        m_frame[index].state->store[2] = glm::min(m_startMinerals, m_mineralBaseCapacity);
        m_frame[index].state->store[0] = m_frame[index].state->capacity[0];
    }
}

void StructureSystem::placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing, uint8 team)
{
    if (!isPlaceableType(type) || (int)team >= GameMaxTeams)
        return; // the Base only enters through spawnBase; the Connector is retired
    // Orientation FIRST (the Crossing's footprint depends on it), then the grid validation.
    // Lance: the AIMED facing from the two-click placement when given, else auto — away from the
    // own Base. Crossing: the facing quantized to an axis (default +X).
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (type == EStructureType::Crossing)
    {
        glm::vec2 dir(1.0f, 0.0f);
        if (glm::abs(facing.x) >= glm::abs(facing.z) && glm::abs(facing.x) > 1e-4f)
            dir = glm::vec2(facing.x > 0.0f ? 1.0f : -1.0f, 0.0f);
        else if (glm::abs(facing.z) > 1e-4f)
            dir = glm::vec2(0.0f, facing.z > 0.0f ? 1.0f : -1.0f);
        rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // GRID: every placement snaps (extractors snap the node's position too) and occupied cells
    // refuse — validated HERE, the MP seam, not just at aim time.
    const glm::vec3 snappedGround = snapToGrid(type, groundPos);
    if (!cellsFree(type, snappedGround, rot) || actorInFootprint(type, snappedGround))
        return; // overlap raced the ghost — silently refused (it already showed red)
    if (type == EStructureType::Extractor)
    {
        if (nodeIndex < 0 || nodeIndex >= (int)m_nodes.size() || m_nodes[nodeIndex].extracted)
            return; // two same-frame requests for one node race — validated at apply
    }
    const glm::vec3 pos = snappedGround + glm::vec3(0.0f, structureSpawnHeights[(int)type], 0.0f);
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

// ---------------------------------------------------------------- cells + derived links

void StructureSystem::insertCells(const Ref& s)
{
    const uint32 id = s.state->structureId;
    forEachFootprintCell(s.type, s.entity->pos, s.entity->rot, [&](int cx, int cz)
    {
        CellEntry& entry = m_cells[cellKey(cx, cz)];
        if (entry.id == 0)
        {
            entry.id = id;
            return;
        }
        // Sharing is only ever crossing-over-cable (cellsFree enforced it). Whichever arrives
        // second, the CROSSING is the primary occupant and the cable rides underId.
        if (s.type == EStructureType::Crossing)
        {
            entry.underId = entry.id;
            entry.id = id;
        }
        else
            entry.underId = id;
    });
}

void StructureSystem::eraseCells(const Ref& s)
{
    const uint32 id = s.state->structureId;
    forEachFootprintCell(s.type, s.entity->pos, s.entity->rot, [&](int cx, int cz)
    {
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return;
        CellEntry& entry = it->second;
        if (entry.id == id)
        {
            if (entry.underId != 0) // the under-cable becomes the primary occupant again
            {
                entry.id = entry.underId;
                entry.underId = 0;
            }
            else
                m_cells.erase(it);
        }
        else if (entry.underId == id)
            entry.underId = 0;
    });
}

int StructureSystem::cableSegmentAt(int cx, int cz, bool builtOnly) const
{
    const auto it = m_cells.find(cellKey(cx, cz));
    if (it == m_cells.end())
        return -1;
    // The under-cable stands in when a crossing bridges the cell — which is also what keeps a
    // crossing from unioning with the cable passing under it (the crossing itself is not a cable).
    for (const uint32 id : { it->second.id, it->second.underId })
    {
        if (id == 0)
            continue;
        const int index = structureIndexById(id);
        if (index >= 0 && isCableType(m_frame[index].type)
            && (!builtOnly || !m_frame[index].state->blueprint))
            return index;
    }
    return -1;
}

void StructureSystem::rebuildDerivedLinks()
{
    if (!m_linksDirty)
        return;
    m_linksDirty = false;
    ProfileScope scope("Structures link rebuild", EProfileCategory::Game);
    const auto capacityIn = [&](EStructureType t, int medium) {
        return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
    const auto cellOf = [](const glm::vec3& p) {
        return glm::ivec2((int)std::lround(p.x / GridCellSize - 0.5f),
                          (int)std::lround(p.z / GridCellSize - 0.5f)); };

    // 1) Collect the BUILT cable segments and union-find them over 4-neighbour adjacency of equal
    //    medium (blueprint segments are non-conductive: the path is broken until they finish).
    struct Seg { int frameIdx; glm::ivec2 cell; uint8 medium; };
    oc::vector<Seg> segs;
    oc::unordered_map<uint64, int> segAtCell; // the segment's OWN cell (under-cables included)
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        if (!isCableType(m_frame[i].type) || m_frame[i].state->blueprint)
            continue;
        const glm::ivec2 cell = cellOf(m_frame[i].entity->pos);
        segAtCell[cellKey(cell.x, cell.y)] = (int)segs.size();
        segs.push_back(Seg{ i, cell, (uint8)cableMediumOf(m_frame[i].type) });
    }
    oc::vector<int> parent(segs.size());
    for (int i = 0; i < (int)parent.size(); ++i)
        parent[i] = i;
    const auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    const auto unite = [&](int a, int b) { parent[find(a)] = find(b); };
    const auto segAt = [&](int cx, int cz) -> int {
        const auto it = segAtCell.find(cellKey(cx, cz));
        return it != segAtCell.end() ? it->second : -1; };
    for (int i = 0; i < (int)segs.size(); ++i)
        for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(0, 1) })
            if (const int n = segAt(segs[i].cell.x + d.x, segs[i].cell.y + d.y);
                n >= 0 && segs[n].medium == segs[i].medium)
                unite(i, n);

    // 2) CROSSINGS conduct: each BUILT crossing resolves what sits just beyond its two END cells
    //    along its axis — a cable run, an already-conducting crossing, or a building. Two runs of
    //    the SAME medium union through it; a run on one end and a capacity-holding building on the
    //    other attaches the building. A fixpoint loop serves crossing chains.
    // Each END accepts connections from THREE sides: straight out along the axis plus the two
    // laterals (only the raised MIDDLE cell is pass-through-only). Candidate scan order is fixed
    // (outward, +lateral, -lateral) so every instance resolves identically.
    struct Cross { int frameIdx; glm::ivec2 end[2]; glm::ivec2 axis; int seg = -1; }; // seg = a run member it joined
    oc::vector<Cross> crossings;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        if (m_frame[i].type != EStructureType::Crossing || m_frame[i].state->blueprint)
            continue;
        const glm::ivec2 center = cellOf(m_frame[i].entity->pos);
        const glm::ivec2 ext = footprintExtent(EStructureType::Crossing, m_frame[i].entity->rot);
        const glm::ivec2 axis = ext.x == 3 ? glm::ivec2(1, 0) : glm::ivec2(0, 1);
        crossings.push_back(Cross{ i, { center - axis, center + axis }, axis });
    }
    // The three cells an end connects through: outward continues the axis, the laterals let a
    // perpendicular cable enter at the side.
    const auto endCandidates = [](const Cross& c, int e, glm::ivec2 out[3])
    {
        const glm::ivec2 outward = e == 0 ? -c.axis : c.axis;
        const glm::ivec2 lateral(c.axis.y, c.axis.x);
        out[0] = c.end[e] + outward;
        out[1] = c.end[e] + lateral;
        out[2] = c.end[e] - lateral;
    };
    const auto crossAtOut = [&](const glm::ivec2& cell) -> int { // a conducting crossing whose END
        for (int c = 0; c < (int)crossings.size(); ++c)          // touches this candidate cell
        {
            if (crossings[c].seg < 0)
                continue;
            if (cell == crossings[c].end[0] || cell == crossings[c].end[1])
                return c;
        }
        return -1; };
    oc::vector<oc::pair<int, int>> crossAttach; // (seg slot, building frameIdx) via a crossing end
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Cross& c : crossings)
        {
            if (c.seg >= 0)
                continue;
            oc::fixed_vector<int, 3> endRuns[2]; // cable run members reachable at each end
            int building[2] = { -1, -1 };        // first building reachable at each end
            for (int e = 0; e < 2; ++e)
            {
                glm::ivec2 cand[3];
                endCandidates(c, e, cand);
                for (const glm::ivec2& cell : cand)
                {
                    if (const int seg = segAt(cell.x, cell.y); seg >= 0)
                        endRuns[e].push_back(seg);
                    else if (const int cc = crossAtOut(cell); cc >= 0)
                        endRuns[e].push_back(crossings[cc].seg);
                    else if (const auto it = m_cells.find(cellKey(cell.x, cell.y));
                        it != m_cells.end() && building[e] < 0)
                    {
                        const int idx = structureIndexById(it->second.id);
                        if (idx >= 0 && !isCableOrCrossing(m_frame[idx].type))
                            building[e] = idx;
                    }
                }
            }
            // A same-medium run pair across the two ends conducts (first in scan order); every
            // OTHER run of that medium touching either end joins the same union.
            int chosen = -1;
            for (const int a : endRuns[0])
            {
                for (const int b : endRuns[1])
                    if (segs[a].medium == segs[b].medium)
                    {
                        unite(a, b);
                        chosen = a;
                        break;
                    }
                if (chosen >= 0)
                    break;
            }
            // Fallback: run(s) on one side only + a building holding that medium on the other.
            if (chosen < 0)
            {
                const int e = endRuns[0].empty() ? 1 : 0;
                const int far = building[1 - e];
                if (!endRuns[e].empty() && far >= 0
                    && capacityIn(m_frame[far].type, segs[endRuns[e].front()].medium) > 0.0f)
                {
                    chosen = endRuns[e].front();
                    crossAttach.push_back({ chosen, far });
                }
            }
            if (chosen >= 0)
            {
                for (int e = 0; e < 2; ++e)
                    for (const int r : endRuns[e])
                        if (segs[r].medium == segs[chosen].medium)
                            unite(r, chosen);
                c.seg = chosen;
                changed = true;
            }
        }
    }

    // Crossing TINT: stamp the conducted medium onto the roster entry — the mesh takes the
    // medium's hue (yellow/orange/blue), authored gray while inert; re-tint only on change.
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        if (m_frame[i].type != EStructureType::Crossing)
            continue;
        int8 medium = -1;
        for (const Cross& c : crossings)
            if (c.frameIdx == i && c.seg >= 0)
                medium = (int8)segs[c.seg].medium;
        if (m_frame[i].conductMedium != medium)
        {
            m_frame[i].conductMedium = medium;
            applyStructureTint(m_frame[i]);
        }
    }

    // 3) Building attachment: every run cell's 4-neighbours that hold a building with capacity in
    //    the run's medium (blueprint buildings pre-wire — the flow already gates on their flag).
    //    Collected as raw (seg slot, building) pairs first: the bridge step below still unions.
    oc::vector<oc::pair<int, int>> attachPairs; // (seg slot, building frame index)
    for (int i = 0; i < (int)segs.size(); ++i)
        for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1) })
        {
            const auto it = m_cells.find(cellKey(segs[i].cell.x + d.x, segs[i].cell.y + d.y));
            if (it == m_cells.end())
                continue;
            const int idx = structureIndexById(it->second.id);
            if (idx >= 0 && !isCableOrCrossing(m_frame[idx].type)
                && capacityIn(m_frame[idx].type, segs[i].medium) > 0.0f)
                attachPairs.push_back({ i, idx });
        }
    for (const auto& [segSlot, buildingIdx] : crossAttach)
        attachPairs.push_back({ segSlot, buildingIdx });

    // 3b) BUILDINGS BRIDGE: a BUILT building conducts every medium it holds — two same-medium runs
    //     touching it merge into one (a power line with an emitter cut into the middle carries
    //     through), exactly like a cable cell would. Buildings still never connect DIRECTLY to
    //     each other (adjacency alone derives nothing — a link always needs cable in between), and
    //     a BLUEPRINT building does not bridge (consistent with blueprint cables breaking the
    //     path), though it still attaches to each run for the pre-wire.
    {
        oc::unordered_map<uint64, int> firstSlot; // (building << 2 | medium) -> first seg slot seen
        for (const auto& [segSlot, buildingIdx] : attachPairs)
        {
            if (m_frame[buildingIdx].state->blueprint)
                continue;
            const uint64 key = (uint64)buildingIdx << 2 | segs[segSlot].medium;
            const auto [it, inserted] = firstSlot.insert({ key, segSlot });
            if (!inserted)
                unite(segSlot, it->second);
        }
    }

    // 3c) Bucket the attachments by the FINAL union roots (dedup — several pairs can name the same
    //     building through different segments).
    oc::unordered_map<int, oc::vector<int>> runBuildings; // union root -> building frame indices
    for (const auto& [segSlot, buildingIdx] : attachPairs)
    {
        oc::vector<int>& list = runBuildings[find(segSlot)];
        bool known = false;
        for (const int existing : list)
            known |= existing == buildingIdx;
        if (!known)
            list.push_back(buildingIdx);
    }

    // 4) The DESIRED link set: per run all attached pairs (the band model equalizes multi-way) up
    //    to a clique cap; past it a STAR from a ROLE-PICKED hub — storage band first (a band-1 hub
    //    relays both directions: producers pour in downhill, it balances with other storage and
    //    exports to consumers — the old Connector's role), else a producer, NEVER a consumer when
    //    a better role exists (a band-0 hub can never send energy UP to a battery, which starved
    //    batteries on big runs), lowest id as the deterministic tie-break (bands come from
    //    stampTuning — same types/ids on every instance, so clients derive the same star).
    //    Key = (minId << 32 | maxId), value = medium mask.
    constexpr size_t c_runCliqueCap = 32; // 32 buildings = 496 links; past that the star bounds it
    oc::unordered_map<uint64, uint8> desired;
    const auto pairKey = [](uint32 a, uint32 b) {
        return (uint64)glm::min(a, b) << 32 | glm::max(a, b); };
    for (auto& [root, buildings] : runBuildings)
    {
        if (buildings.size() < 2)
            continue;
        oc::sort(buildings.begin(), buildings.end(), [&](int a, int b) {
            return m_frame[a].state->structureId < m_frame[b].state->structureId; });
        const int m = (int)segs[root].medium;
        const uint8 mediumBit = (uint8)(1u << m);
        const size_t count = buildings.size();
        if (count <= c_runCliqueCap)
        {
            for (size_t a = 0; a < count; ++a)
                for (size_t b = a + 1; b < count; ++b)
                    desired[pairKey(m_frame[buildings[a]].state->structureId,
                        m_frame[buildings[b]].state->structureId)] |= mediumBit;
        }
        else
        {
            const auto hubScore = [&](size_t i) {
                const int band = m_frame[buildings[i]].state->band[m];
                return band == 1 ? 2 : band > 1 ? 1 : 0; }; // storage > producer > consumer
            size_t hub = 0;
            for (size_t i = 1; i < count; ++i)
                if (hubScore(i) > hubScore(hub)) // ties keep the earlier = lower id
                    hub = i;
            for (size_t b = 0; b < count; ++b)
                if (b != hub)
                    desired[pairKey(m_frame[buildings[hub]].state->structureId,
                        m_frame[buildings[b]].state->structureId)] |= mediumBit;
        }
    }

    // 5) Diff against the live links: unlink stale, link missing (owner = the lower-id side, so a
    //    rebuild never flips ownership and flowAvg survives on untouched links). The create loop
    //    walks the desired map itself, so the topology above is spelled exactly once.
    struct Stale { Entity* a; Entity* b; int medium; };
    oc::vector<Stale> stale;
    for (const Ref& s : m_frame)
        for (const GameStructureLink& l : s.state->links)
        {
            if (!l.owner || !l.other)
                continue;
            const GameStructureComponent* far = getComponent<GameStructureComponent>(l.other.get());
            const auto it = far ? desired.find(pairKey(s.state->structureId, far->structureId))
                                : desired.end();
            if (it == desired.end() || (it->second & (1u << l.medium)) == 0)
                stale.push_back({ s.entity, l.other.get(), (int)l.medium });
        }
    for (const Stale& st : stale)
        GameStructureComponent::unlink(*st.a, *st.b, st.medium);
    for (const auto& [key, mask] : desired)
    {
        const int a = structureIndexById((uint32)(key >> 32)); // the LOWER id = the owner side
        const int b = structureIndexById((uint32)key);
        if (a < 0 || b < 0)
            continue;
        for (int m = 0; m < 3; ++m)
            if ((mask & (1u << m)) && !m_frame[a].state->findLink(m_frame[b].entity, m))
                GameStructureComponent::link(*m_frame[a].entity, *m_frame[b].entity, (uint8)m,
                    m_cableThroughput[m]);
    }

    // 6) The run table (draw) + arm visuals.
    m_runs.clear();
    oc::unordered_map<int, int> runIndex; // union root -> m_runs index
    for (int i = 0; i < (int)segs.size(); ++i)
    {
        const int root = find(i);
        auto [it, inserted] = runIndex.insert({ root, (int)m_runs.size() });
        if (inserted)
        {
            CableRun run;
            run.medium = segs[root].medium;
            if (const auto rb = runBuildings.find(root); rb != runBuildings.end())
                for (const int building : rb->second)
                    run.buildingIds.push_back(m_frame[building].state->structureId);
            m_runs.push_back(oc::move(run));
        }
        m_runs[it->second].segmentIds.push_back(m_frame[segs[i].frameIdx].state->structureId);
    }
    for (const Cross& c : crossings)
        if (c.seg >= 0)
            if (const auto it = runIndex.find(find(c.seg)); it != runIndex.end())
                m_runs[it->second].segmentIds.push_back(m_frame[c.frameIdx].state->structureId);
    for (const Ref& s : m_frame)
        updateArms(s);
}

void StructureSystem::updateArms(const Ref& s)
{
    if (!isCableType(s.type))
        return;
    const int medium = cableMediumOf(s.type);
    const int cx = (int)std::lround(s.entity->pos.x / GridCellSize - 0.5f);
    const int cz = (int)std::lround(s.entity->pos.z / GridCellSize - 0.5f);
    static constexpr glm::ivec2 dirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }; // PX NX PZ NZ
    for (int a = 0; a < 4; ++a)
    {
        if (!s.arms[a])
            continue;
        // An arm shows toward anything the segment VISUALLY joins: a same-medium cable (blueprint
        // included — adjacency, not conduction), a crossing, or a building holding the medium.
        bool on = false;
        const int nx = cx + dirs[a].x, nz = cz + dirs[a].y;
        if (const int seg = cableSegmentAt(nx, nz, /*builtOnly*/ false);
            seg >= 0 && cableMediumOf(m_frame[seg].type) == medium)
            on = true;
        else if (const auto it = m_cells.find(cellKey(nx, nz)); it != m_cells.end())
        {
            const int idx = structureIndexById(it->second.id);
            if (idx >= 0)
            {
                const EStructureType t = m_frame[idx].type;
                const float cap = medium == 1 ? fuelCapacityOf(t)
                                : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t);
                on = t == EStructureType::Crossing || (!isCableOrCrossing(t) && cap > 0.0f);
            }
        }
        s.arms[a]->setEnabled(on);
    }
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

// The medium hues the cable prefabs/flow rings use: power yellow, fuel orange, minerals blue.
static glm::vec3 mediumHue(int medium)
{
    return medium == 1 ? glm::vec3(1.0f, 0.55f, 0.15f)
         : medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
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
        // A conducting Crossing shows the medium flowing through it (authored gray while inert).
        if (s.type == EStructureType::Crossing && s.conductMedium >= 0)
            color = mediumHue(s.conductMedium);
    }
    const auto material = Globals::rendererVK.createSolidColorMaterial(color);
    rc->node.setMaterialOverride(material);
    // Cables/crossings are COMPOSITES (arm/end/ramp child entities): tint the child pieces along —
    // this is also what turns them blueprint-gray and back.
    if (isCableOrCrossing(s.type))
        if (const SceneComponent* sc = getComponent<SceneComponent>(s.entity))
            for (const EntityPtr& child : sc->children)
                if (RenderComponent* crc = getComponent<RenderComponent>(child.get());
                    crc && crc->node.isValid())
                    crc->node.setMaterialOverride(material);
}

float StructureSystem::investMaterials(const Ref& s, float amount)
{
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp — the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT. Heals
    // against the COMPONENT's healthMax (cables are softer than buildings).
    if (amount <= 0.0f || (!isPlaceableType(s.type) && s.type != EStructureType::Base))
        return 0.0f; // the Base is never placed, but it repairs at its m_costs entry like the rest
    const float healthMax = glm::max(s.state->healthMax, 1e-3f);
    const float materialsPerHp = glm::max(m_costs[(int)s.type], 0.01f) / healthMax;
    const float heal = glm::min(amount / materialsPerHp, healthMax - s.state->health);
    if (heal <= 0.0f)
        return 0.0f;
    s.state->health += heal;
    if (s.state->blueprint && s.state->health >= healthMax - 1e-3f)
    {
        s.state->blueprint = false;
        applyStructureTint(s); // back to the authored color
        m_linksDirty = true;   // a finished cable segment starts conducting
        Log::info(oc::string(structureNames[(int)s.type]) + " constructed");
        if (onStructureBuilt) // server: re-fire GPl — cables are outside GSt, so this IS the
            onStructureBuilt(s.state->structureId); // client's only built notification
    }
    return heal * materialsPerHp;
}

float StructureSystem::takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount)
{
    // Silos, the Base, AND mineral extractors: a player standing at a fresh dig site can courier
    // its buffer by hand before any conveyor exists (a fuel extractor's mineral store stays 0, so
    // including the type unconditionally is safe).
    const auto holdsMinerals = [](EStructureType t) {
        return t == EStructureType::MineralSilo || t == EStructureType::Base
            || t == EStructureType::Extractor; };
    float taken = 0.0f;
    for (const Ref& s : m_frame)
    {
        if (amount - taken <= 0.0f)
            break;
        if (s.state->blueprint || s.state->team != team || !holdsMinerals(s.type))
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
        if (s.state->team != team)
            continue;
        // The Base is never a blueprint, but it IS repairable now that it takes damage.
        const bool wantsMaterials = s.state->blueprint
            || (includeRepairs && s.state->health < s.state->healthMax - 1e-3f);
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
    ProfileScope scope("Structures production", EProfileCategory::Game);
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
            if (hasShieldEmitter(ref.type))
            {
                s.emitter.outputFrac = 0.0f;
                if (ForceComponent* fc = getComponent<ForceComponent>(ref.entity))
                    fc->emitter.setOutput(0.0f);
            }
            continue;
        }

        // ---- income: powered extractors fill their OWN buffer/tank (full = production stalls),
        // the Base trickles minerals into its bank, fabricators convert (energy+fuel -> minerals).
        if (ref.type == EStructureType::Extractor && s.powered && ref.nodeIndex >= 0
            && ref.nodeIndex < (int)m_nodes.size())
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
        // export-limited = no fuel burn), solar trickles for free. The Base self-generates the
        // same way into its own store — a baseline its shield draw eats from; sieges outpace it.
        if (ref.type == EStructureType::Base && m_baseEnergyGenPerSec > 0.0f)
        {
            const float add = glm::min(m_baseEnergyGenPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            if (dt > 1e-9f) // paused (sim dt 0): 0/0 would put a NaN into the HUD's gen rate
                m_genRateTotal += add / dt;
        }
        else if (ref.type == EStructureType::Solar)
        {
            const float add = glm::min(m_solarEnergyPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            if (dt > 1e-9f)
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
                if (dt > 1e-9f)
                    m_genRateTotal += want / dt;
            }
        }

        // ---- consumers drain their internal battery. Emitters (the Base's shield included) pay
        // EXTRA per unit of pressure, LATCH OFF at empty until "Emitter restart charge" and ramp
        // their bubble smoothly.
        if (hasShieldEmitter(ref.type))
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
}

void StructureSystem::tickDamage(float)
{
    ProfileScope scope("Structures death sweep", EProfileCategory::Game);
    // Damage happens ON the entities (territory drain + atomic intake in the component) — this is
    // the DEATH SWEEP plus the strainable mark units aim their siege drain at.
    for (size_t i = 0; i < m_frame.size();)
    {
        const Ref& s = m_frame[i];
        s.state->strainable = hasShieldEmitter(s.type) && !s.state->blueprint
            && s.state->emitter.outputFrac > 0.05f;
        // The CPU bubble-radius stand-in shield-less units test against (see GameComponents.ixx):
        // the visible sphere radius is ~half the reach, scaled by the live output ramp.
        s.state->bubbleRadius = s.state->strainable
            ? emitterReachOf(s.type) * 0.5f * s.state->emitter.outputFrac : 0.0f;
        // The Base takes damage but is NEVER destroyed (no lose condition yet): it survives at
        // 0 hp — dead but standing, until players repair it back up.
        if (s.state->invulnerable || s.type == EStructureType::Base || s.state->alive())
        {
            ++i;
            continue;
        }
        Log::info(oc::string(structureNames[(int)s.type]) + " destroyed");
        destroyStructureAt(i);
    }
}

void StructureSystem::tickConstructors(float deltaSec)
{
    ProfileScope scope("Structures constructors", EProfileCategory::Game);
    // A powered Constructor invests its conveyor-fed mineral stock into the nearest own-team
    // blueprint OR damaged structure in range; with nothing to build or repair it idles.
    for (const Ref& ref : m_frame)
    {
        GameStructureComponent& s = *ref.state;
        if (s.blueprint || ref.type != EStructureType::Constructor || !s.powered || s.store[2] <= 0.0f)
            continue;
        const float budget = glm::min(m_constructorBuildRate * deltaSec, s.store[2]);
        s.store[2] -= fundNearbyBlueprint(ref.entity->pos, m_constructorRange, (uint8)s.team, budget, true);
    }
}

void StructureSystem::tickAuthority(const glm::vec3&, float deltaSec)
{
    ProfileScope scope("Structures authority", EProfileCategory::Game);
    m_time += deltaSec;
    refresh(); // the frame view every request/tick below works on
    ProfileScope requestScope("Structure requests", EProfileCategory::Game);
    for (const PlaceRequest& req : m_requests)
        placeStructure(req.type, req.pos, req.nodeIndex, req.facing, req.team);
    m_requests.clear();
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
        s.route = oc::move(request.points);
        if (onRouteChanged)
            onRouteChanged(request.id);
        // LIVE ORDERS: units already spawned from this barracks pick the new route up too — a
        // roster walk via the GameMatch-wired hook (no world query; the unit roster is NpcSystem's).
        if (onRouteLiveUnits)
            onRouteLiveUnits(request.id, oc::span<const glm::vec3>(s.route.data(), s.route.size()));
    }
    m_routeRequests.clear();
    for (const UnitTypeRequest& request : m_unitTypeRequests)
    {
        const int index = structureIndexById(request.id);
        if (index < 0 || !isBarracksType(m_frame[index].type)
            || m_frame[index].state->team != request.team || !isBarracksUnitType(request.unitType))
            continue; // died, not a barracks, someone else's, or garbage — refused (the MP seam)
        GameStructureComponent& s = *m_frame[index].state;
        if (s.barracks.unitType == request.unitType)
            continue;
        s.barracks.unitType = request.unitType;
        stampTuning(m_frame[index]); // prices follow the type this same tick
        if (onUnitTypeChanged)
            onUnitTypeChanged(request.id);
    }
    m_unitTypeRequests.clear();
    requestScope.stop();

    // Same-tick links for fresh placements (each placeStructure above set the dirty flag); a death
    // in tickDamage below re-dirties and rebuilds on the NEXT tick.
    rebuildDerivedLinks();
    tickProduction(deltaSec); // flows themselves run per-entity in the engine's pass
    // Death sweep BEFORE constructors: last frame's damage lands after this tick (contacts +
    // entity pass), so the sweep must judge it before a repair trickle can resurrect a 0-hp
    // structure — repairs-first made buildings unkillable inside any constructor's range (the
    // heal always ran between the killing blow and the sweep). A structure that ends a frame at
    // EXACTLY 0 now dies; anything above 0 can still be out-healed legitimately.
    tickDamage(deltaSec);
    tickConstructors(deltaSec);
}

void StructureSystem::tickMirror(float deltaSec)
{
    ProfileScope scope("Structures mirror", EProfileCategory::Game);
    refresh();
    rebuildDerivedLinks(); // clients derive locally from the mirrored structures — same code,
                           // same deterministic inputs, so both sides agree without a cable wire
    // Ease each emitter's fraction toward the synced target at ramp-like speed, then drive the
    // LOCAL field from it — the bubble animates as smoothly as the server's own.
    for (const Ref& ref : m_frame)
    {
        if (!hasShieldEmitter(ref.type))
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
    if ((int)type >= (int)EStructureType::Count || (int)team >= GameMaxTeams)
        return; // garbage
    if (const int existing = structureIndexById(id); existing >= 0)
    {
        // Duplicate replay — or the server's blueprint-completed re-send (see onStructureBuilt):
        // apply the built flag idempotently so the client's derivation starts conducting too.
        if (built && m_frame[existing].state->blueprint)
        {
            m_frame[existing].state->blueprint = false;
            m_frame[existing].state->health = m_frame[existing].state->healthMax;
            applyStructureTint(m_frame[existing]);
            m_linksDirty = true;
        }
        return;
    }
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
        m_linksDirty = true;     // built buildings attach to runs; ghosts detach
    }
    s.health = healthFrac * s.healthMax;
    s.store[0] = chargeFrac * energyCapacityOf(ref.type);
    s.store[1] = fuelFrac * fuelCapacityOf(ref.type);
    s.store[2] = mineralFrac * mineralCapacityOf(ref.type);
    s.flowUtil = utilFrac; // already server-smoothed
    if (hasShieldEmitter(ref.type)) // union: emitter variant (Base included)
        s.emitter.outputFracTarget = outputFrac; // tickMirror eases the live field toward this
    else if (isBarracksType(ref.type)) // the output byte carries the barracks' POPULATION instead
        s.barracks.population = (int)glm::round(outputFrac * 255.0f);
    s.powered = powered;
}

void StructureSystem::mirrorUnitType(uint32 id, uint8 unitType)
{
    const int index = structureIndexById(id);
    if (index < 0 || !isBarracksType(m_frame[index].type) || !isBarracksUnitType(unitType))
        return;
    m_frame[index].state->barracks.unitType = unitType;
    stampTuning(m_frame[index]);
}

void StructureSystem::queueUnitTypeRequest(uint32 id, uint8 unitType, uint8 team)
{
    m_unitTypeRequests.push_back({ id, unitType, team });
}

void StructureSystem::mirrorCableProgress(uint32 id, float healthFrac)
{
    const int index = structureIndexById(id);
    if (index < 0 || !m_frame[index].state->blueprint)
        return; // a finished segment's health is authoritative from the built flip (GPl)
    GameStructureComponent& s = *m_frame[index].state;
    s.health = glm::clamp(healthFrac * s.healthMax, 1.0f, s.healthMax);
}

void StructureSystem::mirrorRoute(uint32 id, oc::span<const glm::vec3> points)
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
        n.set("Id", oc::to_string(s.state->structureId));
        n.set("Type", oc::to_string((int)s.type));
        n.set("Position", s.entity->pos);
        const glm::vec3 forward = s.type == EStructureType::Lance || s.type == EStructureType::Crossing
            ? s.entity->rot * glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f);
        n.set("Facing", glm::vec3(forward.x, 0.0f, forward.z));
        n.set("NodeIndex", oc::to_string(s.nodeIndex));
        n.set("Team", oc::to_string((int)s.state->team));
        n.set("Blueprint", s.state->blueprint != 0); // bitfield -> the bool overload
        n.set("Health", s.state->health);
        n.set("Charge", s.state->store[0]);
        n.set("Fuel", s.state->store[1]);
        n.set("Minerals", s.state->store[2]);
        if (hasShieldEmitter(s.type)) // union variants: only the active one is meaningful
            n.set("OutputFrac", s.state->emitter.outputFrac);
        if (isBarracksType(s.type))
            n.set("UnitType", oc::to_string((int)s.state->barracks.unitType));
        if (isBarracksType(s.type) && !s.state->route.empty())
        {
            AssetNode& r = n.addChild("Route");
            for (const glm::vec3& wp : s.state->route)
            {
                AssetNode& p = r.addChild("Point");
                p.values = { oc::to_string(wp.x), oc::to_string(wp.z) };
            }
        }
    }
    // (No Cable nodes any more: cable segments save as ordinary Structure entries and the links
    // re-derive on load. Old saves' Cable nodes are simply ignored by loadFrom.)
}

void StructureSystem::clearAllStructures()
{
    while (!m_frame.empty())
        destroyStructureAt(m_frame.size() - 1); // fires onStructureRemoved — clients prune
    m_requests.clear();
    m_demolishRequests.clear();
    m_routeRequests.clear();
    m_cells.clear();
    m_runs.clear();
    m_linksDirty = true;
}

void StructureSystem::loadFrom(const AssetNode& root)
{
    refresh();
    clearAllStructures();
    for (const AssetNode* n : root.findAll("Structure"))
    {
        int typeInt = n->find("Type") ? n->find("Type")->asInt() : -1;
        const uint32 id = n->find("Id") ? (uint32)n->find("Id")->asInt() : 0;
        if (typeInt < 0 || typeInt >= (int)EStructureType::Count || id == 0 || structureIndexById(id) >= 0)
            continue; // garbage / duplicate entry
        if (typeInt == (int)EStructureType::Connector)
        {
            Log::warning("Load game: Connector is retired — entry skipped (old save)");
            continue;
        }
        // Old saves' per-type barracks become a Barracks producing that type (Brute 1 / Runner 2 /
        // Spitter 3 in ENpcType order); a UnitType key from a newer save wins below.
        int unitType = 0;
        if (isRetiredBarracksType((EStructureType)typeInt))
        {
            unitType = typeInt == (int)EStructureType::BarracksBrute ? 1
                     : typeInt == (int)EStructureType::BarracksRunner ? 2 : 3;
            typeInt = (int)EStructureType::Barracks;
        }
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
        s.health = glm::clamp(n->find("Health") ? n->find("Health")->asFloat() : s.healthMax,
            1.0f, s.healthMax);
        s.store[0] = glm::clamp(n->find("Charge") ? n->find("Charge")->asFloat() : 0.0f, 0.0f, s.capacity[0]);
        s.store[1] = glm::clamp(n->find("Fuel") ? n->find("Fuel")->asFloat() : 0.0f, 0.0f, s.capacity[1]);
        s.store[2] = glm::clamp(n->find("Minerals") ? n->find("Minerals")->asFloat() : 0.0f, 0.0f, s.capacity[2]);
        if (hasShieldEmitter((EStructureType)typeInt)) // union variants: write only the active one
            s.emitter.outputFrac = glm::clamp(n->find("OutputFrac") ? n->find("OutputFrac")->asFloat() : 0.0f,
                0.0f, 1.0f);
        if (isBarracksType((EStructureType)typeInt))
        {
            if (const AssetNode* u = n->find("UnitType"))
                unitType = u->asInt();
            s.barracks.unitType = (uint8)(isBarracksUnitType(unitType) ? unitType : 0);
            stampTuning(m_frame[index]);
        }
        if (const AssetNode* r = n->find("Route"); r && isBarracksType((EStructureType)typeInt))
            for (const AssetNode* p : r->findAll("Point"))
                if ((int)s.route.size() < MaxRouteWaypoints)
                    s.route.push_back(glm::vec3(p->asFloat(0), 0.0f, p->asFloat(1)));
    }
    // (Old saves' Cable nodes are ignored: links derive from the cable segments' cell adjacency.)
    rebuildDerivedLinks(); // every spawnStructure above set the dirty flag
    Log::info("Game state loaded: " + oc::to_string(m_frame.size()) + " structures, "
        + oc::to_string(m_runs.size()) + " cable runs");
}

// ---------------------------------------------------------------- debug draw

void StructureSystem::drawDebug() const
{
    ProfileScope scope("Structures debug draw", EProfileCategory::Game);
    // Cable RUNS: the segments themselves are real meshes now — this pass only adds the FLOW
    // feedback: a pulsing ring over every segment of a working run, brightness/size by the run's
    // utilization. Util = the busiest attached building's flowUtil gauge, which the server computes
    // and clients receive through GSt — the same reading on every instance.
    for (const CableRun& run : m_runs)
    {
        float util = 0.0f;
        for (const uint32 id : run.buildingIds)
            if (const int index = structureIndexById(id); index >= 0)
                util = glm::max(util, m_frame[index].state->flowUtil);
        util = glm::clamp(util, 0.0f, 1.0f);
        if (util <= 0.02f || run.buildingIds.size() < 2)
            continue;
        const glm::vec3 hue = run.medium == 1 ? glm::vec3(1.0f, 0.55f, 0.15f)
            : run.medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        const float pulse = 0.5f + 0.5f * std::sin(m_time * (2.0f + 6.0f * util));
        const uint32 color = packColor(hue * (0.3f + 0.7f * util) * (0.5f + 0.5f * pulse));
        const float radius = 0.25f + 0.15f * pulse;
        for (const uint32 id : run.segmentIds)
            if (const int index = structureIndexById(id); index >= 0)
                drawCircle(m_frame[index].entity->pos + glm::vec3(0.0f, 0.45f, 0.0f), radius, color, 10);
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
        if ((hasShieldEmitter(s.type) || s.type == EStructureType::Extractor
            || s.type == EStructureType::Constructor
            || s.type == EStructureType::Fabricator) && !s.state->powered && !s.state->blueprint)
            drawCircle(glm::vec3(s.entity->pos.x, 0.3f, s.entity->pos.z), 1.0f, unpoweredColor, 16);
    }
}
