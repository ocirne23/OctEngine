module Game;

import Core;
import Core.glm;
import Core.Log;
import Settings;
import Settings.Tweaks;
import Core.Transform;
import Entity;
import Force;
import RendererVK;
import :Structures;

// See Structures.ixx: structures are entities; identity and the float stores live on their
// GameStructureComponent. Resources move between them over the CABLE TRANSPORT networks this
// system owns (Transport.cpp: integer cells per segment, a fixed-rate job). THIS file is the core:
// the per-type tables, the hover card, the tweaks, the per-frame tuning re-stamp, the nodes, the
// request queues and the two tick entry points (authority / mirror) that sequence the topic units
// (placement, network, economy, sync - see the list at the end of Structures.ixx).

// The PLAYER-FACING names are the setting's (Docs/GDD.md): light, wood and stone for energy, fuel and minerals. The
// code, the tweaks and the save keys keep the old words.
static constexpr const char* structureNames[] = { "Lantern", "Hearth", "Lumberyard",
    "Crystal store", "Woodshed", "Mirror", "Stoneworks", "Great lantern", "Lance", "Barracks",
    "Wall", "Light tower", "Storehouse", "Builder's hut", "Beacon",
    "Light conduit", "Wood track", "Road", "Conduit crossing", "Wood crossing", "Road crossing",
    "House", "Healer's hut", "Quarry", "Bridge", "Gate", "Lens", "Searchlight" };
// Spawn height = each prefab's box HALF height, so every shape sits flush (see the prefabs).
static constexpr float structureSpawnHeights[] = { 1.0f, 2.0f, 2.0f, 2.0f, 2.0f, 0.5f, 2.0f,
    2.0f, 1.0f, 3.0f, 1.5f, 2.0f, 4.0f, 1.0f, 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f,
    0.25f, 1.5f, 1.0f, 2.0f, 0.25f, 1.5f, 1.5f, 2.0f }; // (the Wall and the Gate: 3 m tall)
static_assert(oc::size(structureNames) == (size_t)EStructureType::Count);
static_assert(oc::size(structureSpawnHeights) == (size_t)EStructureType::Count);

const char* structureTypeName(EStructureType type)
{
    return structureNames[(int)type];
}

float StructureSystem::spawnHeightOf(EStructureType type)
{
    return structureSpawnHeights[(int)type]; // = the prefab box's HALF height (it sits flush)
}

// The build hotbar's HOVER CARD: one sentence, then this type's exact per-second flows (and what it
// banks or does), one per line. Every number comes straight off the live tweaks, so a retuned
// economy retunes the card - nothing here is a hand-written constant.
oc::string StructureSystem::describeType(EStructureType type) const
{
    // Every metric line is "<sign> <value> <unit>[ <note>]", so the card scans as one column:
    //   -  an input (a per-second draw, or the one-off build cost)
    //   +  an output (a per-second yield)
    //   =  a capacity it banks
    // Anything else (reach, damage, a rule of thumb) is a plain grey line.
    oc::string desc, flows;
    const auto line = [&](oc::string text) { if (!flows.empty()) flows += '\n'; flows += oc::move(text); };
    const auto in = [&](float rate, const char* what, const char* note = "") {
        if (rate > 0.0f) line(oc::format("- {:g} {}/s{}", rate, what, note)); };
    const auto out_ = [&](float rate, const char* what, const char* note = "") {
        if (rate > 0.0f) line(oc::format("+ {:g} {}/s{}", rate, what, note)); };
    const auto banks = [&](float amount, const char* what) {
        line(oc::format("= {:g} {}", amount, what)); };
    // ONE word for every kind of reach - a bubble's, a beam's, a heal radius, a build range -
    // so the cards never make a player wonder whether "Reach" and "Radius" mean different things.
    const auto range = [&](float metres) { line(oc::format("Range {:g} m", metres)); };
    const GameStructureParams& p = Globals::settings.game.structureParams;

    switch (type)
    {
    case EStructureType::Emitter:
    case EStructureType::Bastion:
    case EStructureType::Lance:
        desc = type == EStructureType::Lance
            ? "A CONE of hardened light along its aimed facing. Demons inside it are solid and burn."
            : "A lantern bubble of hardened light. Demons inside it are solid and burn, and they drain it.";
        line("Sleeps by day: faint, and it uses no light until dark or until a demon is on it");
        in(emitterDrawOf(type), "light");
        in(m_settings.emitterPressureDraw, "light", " more at full pressure");
        range(emitterReachOf(type));
        break;
    case EStructureType::Generator:
        desc = "Burns wood into light, day and night, for the nearest Crystal store or the Beacon in reach.";
        line(oc::format("Needs {} workers", m_settings.workersGenerator));
        in(m_settings.fuelBurnRate, "wood");
        out_(m_settings.genEnergyPerSec, "light");
        break;
    case EStructureType::Solar:
        desc = "Catches sunlight and sends it on a beam to the nearest Crystal store or the Beacon. The beam needs a clear line. Gives nothing at night.";
        out_(m_settings.solarEnergyPerSec, "light", " in full daylight");
        range(m_settings.mirrorBeamRange);
        break;
    case EStructureType::Lumberyard:
        desc = "Fells the trees around it. It stands anywhere: the more trees in its range, the more wood it makes. Needs no light.";
        line(oc::format("Needs {} workers", m_settings.workersHarvest));
        out_(m_settings.fuelRate, "wood", oc::format(" with {:g} trees in range", m_settings.harvestTreesFull).c_str());
        range(m_settings.harvestRadius);
        break;
    case EStructureType::Quarry:
        desc = "Breaks the rocks and boulders around it. It stands anywhere: the more rock in its range, the more stone it makes. Needs no light.";
        line(oc::format("Needs {} workers", m_settings.workersHarvest));
        out_(m_settings.mineralRate, "stone", " with a lot of rock in range");
        range(m_settings.harvestRadius);
        break;
    case EStructureType::Fabricator:
        desc = "Makes cut stone with wood, where no rock is near. Needs no light.";
        line(oc::format("Needs {} workers", m_settings.workersFabricator));
        in(m_settings.fabricatorFuelPerSec, "wood");
        out_(m_settings.fabricatorMineralsPerSec, "stone");
        break;
    case EStructureType::Constructor:
        desc = "Builds and repairs nearby structures from its stone stock. Needs no light.";
        line(oc::format("Needs {} workers", m_settings.workersConstructor));
        in(m_settings.constructorBuildRate, "stone", " while building");
        range(m_settings.constructorRange);
        break;
    case EStructureType::Battery:
        desc = "Holds the day's light for the night, and feeds every light building in its reach that it has a clear line to. Two stores in range share their light.";
        banks(m_settings.batteryCapacity, "light");
        range(m_settings.lightHubReach);
        break;
    case EStructureType::Lens:
        desc = "Passes light on. It shares light with the crystal store, the Beacon or the next lens near it, and the mirrors, hearths and stores around it link to it: a line of lenses carries light over a distance, both ways. Each beam needs a clear line.";
        banks(m_settings.lensBuffer, "light on its way");
        range(m_settings.lensRange);
        break;
    case EStructureType::FuelTank:
        desc = "Holds the wood the lumberyards send in.";
        banks(m_settings.fuelTankCapacity, "wood");
        break;
    case EStructureType::MineralSilo:
        desc = "Holds every good the carriers bring: stone and wood. Only storehouses and the Beacon hold stone you can BUILD with.";
        banks(m_settings.mineralSiloCapacity, "stone");
        banks(m_settings.fuelTankCapacity, "wood");
        break;
    case EStructureType::Barracks:
        desc = "Trains units. Its light draw IS the build bar.";
        in(m_settings.barracksEnergyIntake, "light");
        line(oc::format("Build time = the unit's light cost / {:g}", m_settings.barracksEnergyIntake));
        line(oc::format("Population {} (+{} per linked house)", m_settings.barracksPopulation, m_settings.housePopulation));
        break;
    case EStructureType::House:
        desc = "Homes for workers: production runs on them, and the free ones carry goods on the road (the house must touch a road). It also raises the population cap of the nearest barracks.";
        line(oc::format("+ {} workers", m_settings.houseWorkers));
        line(oc::format("+ {} barracks population", m_settings.housePopulation));
        range(m_settings.houseLinkRadius); // how far it reaches for that barracks
        break;
    case EStructureType::Turret:
        desc = "Strikes the nearest SOLID demon with light: only a demon inside a lantern bubble. Never misses.";
        in(p.turretShotEnergy / glm::max(p.turretFireInterval, 1e-3f), "light");
        banks(p.turretShotEnergy, "light per shot");
        line(oc::format("{:g} damage every {:g} s", p.turretDamage, p.turretFireInterval));
        range(p.turretRange);
        break;
    case EStructureType::Searchlight:
        desc = "Turns a cone of light on the nearest demon. Every demon in the cone is solid, as in a lantern field, and burns. It is dark while no demon is in range.";
        in(p.searchlightEnergyPerSec, "light", " while it shines");
        line(oc::format("{:g} damage/s to each demon in the cone", p.searchlightDps));
        line(oc::format("Cone {:g} degrees wide", p.searchlightHalfAngleDeg * 2.0f));
        range(p.searchlightRange);
        break;
    case EStructureType::MedicStation:
        desc = "Heals own units and players standing in its radius.";
        in(m_settings.medicEnergyPerSec, "light");
        out_(medicHealRate(), "health", " and staff light, per body");
        range(medicHealRadius());
        break;
    case EStructureType::Wall:
        desc = "Cut stone. It stops solid demons: they chew through it instead of walking around. Needs no light.";
        break;
    case EStructureType::Gate:
        desc = "A wall that you walk through. It stops solid demons like a wall, and it stands in water too: across a river it reaches from the bed to a wall's height over the surface. Needs no light.";
        line("Place it: hold and drag a line.");
        break;
    case EStructureType::Bridge:
        desc = "A road on a deck, over a river, a lake or the sea: it lies just over the water surface, on piers down to the bed. It joins the road, and the workers walk it. A road painted over water becomes a bridge by itself.";
        line("Paint it: hold and drag.");
        break;
    case EStructureType::CablePower:
    case EStructureType::CablePipe:
    case EStructureType::CableConveyor:
    {
        static constexpr const char* c_what[3] = { "light", "wood", "stone" };
        const int medium = cableMediumOf(type);
        if (medium != 0 && m_settings.carriers)
        {
            desc = "Free workers walk it with wood and stone, from the building that makes them to the nearest one that takes them. They come from houses on the road.";
            line(oc::format("A worker takes {:g} goods at {:g} m/s", m_settings.carrierLoad, m_settings.carrierSpeed));
        }
        else
        {
            desc = oc::format("Carries {} between the buildings its run touches.", c_what[medium]);
            out_(m_settings.cableThroughput[medium], c_what[medium], " per link");
        }
        line("Paint it: hold and drag. Crossings place themselves.");
        break;
    }
    case EStructureType::CrossingPower:
    case EStructureType::CrossingPipe:
    case EStructureType::CrossingConveyor:
    {
        static constexpr const char* c_what[3] = { "light", "wood", "stone" };
        desc = oc::format("Bridges {} over a line of another kind.", c_what[crossingMediumOf(type)]);
        line("Placed automatically by a paint stroke.");
        break;
    }
    default:
        break;
    }
    // Assembled last: the sentence, then the COST right under it (an input like any other), then
    // the flows.
    oc::string out = oc::move(desc);
    if (const float cost = mineralCost(type); cost > 0.0f && isPlaceableType(type))
    {
        if (!out.empty())
            out += '\n';
        out += oc::format("- {:g} stone to build", cost);
    }
    if (!flows.empty())
    {
        if (!out.empty())
            out += '\n';
        out += flows;
    }
    return out;
}

void StructureSystem::collectShieldBubbles(oc::vector<glm::vec4>& out, uint32 maxCount) const
{
    oc::small_vector<uint32, 32> seenGroups; // a merge group's sphere is the same for every member
    for (const Ref& ref : m_frame)
    {
        if (out.size() >= maxCount)
            break;
        if (!hasShieldEmitter(ref.type))
            continue;
        const ForceComponent* fc = getComponent<ForceComponent>(ref.entity);
        if (!fc)
            continue;
        glm::vec3 center;
        float radius;
        uint32 group = 0;
        if (!fc->emitter.getBubbleBounds(center, radius, &group))
            continue;
        if (group != 0)
        {
            if (oc::find(seenGroups.begin(), seenGroups.end(), group) != seenGroups.end())
                continue;
            seenGroups.push_back(group);
        }
        out.push_back(glm::vec4(center, radius));
    }
}

glm::vec3 StructureSystem::structureLabelAnchor(int index) const
{
    return m_frame[index].entity->pos
        + glm::vec3(0.0f, structureSpawnHeights[(int)m_frame[index].type] + 0.7f, 0.0f);
}

glm::vec2 StructureSystem::structureFacing(int index) const
{
    // Lance + crossings carry an authored facing (join replay + save); a crossing's decides which
    // cells its 1x3 footprint covers, so it MUST survive the wire or client derivation diverges.
    if (m_frame[index].type != EStructureType::Lance && !isCrossingType(m_frame[index].type))
        return glm::vec2(0.0f);
    const glm::vec3 forward = m_frame[index].entity->rot * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::vec2(forward.x, forward.z);
}

// (Declared in Structures.ixx - shared by every Game implementation unit.)
float emitterShellAlpha(float charge, uint32 id, float warnCharge)
{
    if (warnCharge <= 0.0f || charge >= warnCharge)
        return 1.0f;
    const float low = 1.0f - glm::clamp(charge / warnCharge, 0.0f, 1.0f); // 0 at the warning level, 1 at empty
    const float t = (float)Globals::time.getElapsedSec() * glm::mix(4.0f, 14.0f, low) + (float)(id % 64u);
    // Two sines of unrelated rates: an uneven lantern flicker, not a regular blink.
    const float flicker = 0.5f + 0.25f * std::sin(t) + 0.25f * std::sin(t * 2.7f + 1.3f);
    return 1.0f - low * 0.75f * flicker;
}

uint32 packColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments)
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

StructureSystem::StructureSystem()
{
    // The transport tick: rates/capacities re-stamp at the next network rebuild.
    Tweak::onChange(m_settings.transportTickHz, this, [this] { m_linksDirty = true; });
    Tweak::onChange(m_settings.transportSubsteps, this, [this] { m_linksDirty = true; });
    Tweak::onChange(m_settings.transportSpread, this, [this] { m_linksDirty = true; });
}

StructureSystem::~StructureSystem()
{
    Tweak::removeListeners(this);
}

// ---------------------------------------------------------------- frame view

void StructureSystem::refresh()
{
    ProfileScope scope("Structures refresh", EProfileCategory::Game);
    // The roster is maintained at the spawn/remove seams (no world query - type, yield and id index
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
    const float radiusSq = m_settings.houseLinkRadius * m_settings.houseLinkRadius;
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
    c.healthMax = isCableOrCrossing(s.type) ? m_settings.cableHealthMax : m_settings.structureHealthMax;
    c.capacity[0] = energyCapacityOf(s.type);
    c.capacity[1] = fuelCapacityOf(s.type);
    c.capacity[2] = mineralCapacityOf(s.type);
    // A BARRACKS' energy capacity is its selected unit's cost (the build bar) - resolved HERE,
    // before the store clamp below: energyCapacityOf's 1.0 attach placeholder would otherwise
    // clamp the store to 1 every tick (the bar sat at 1 forever).
    if (isBarracksType(s.type))
    {
        if (!isBarracksUnitType(c.barracks.unitType))
            c.barracks.unitType = 0;
        c.barracks.spawnCost = m_settings.spawnEnergy[c.barracks.unitType];
        c.capacity[0] = glm::max(c.barracks.spawnCost, 1.0f);
    }
    // A TURRET's energy capacity is ONE SHOT - the same rule, so its store reads as a RELOAD bar
    // and a full one fires. Resolved here for the same reason: before the store clamp.
    else if (s.type == EStructureType::Turret)
        c.capacity[0] = glm::max(Globals::settings.game.structureParams.turretShotEnergy, 0.01f);
    for (int m = 0; m < 3; ++m)
        c.store[m] = glm::min(c.store[m], c.capacity[m]);
    // (Cables hold NO stores - capacity 0 everywhere: they are transport nodes, never endpoints.
    // Roles - producer / consumer / storage per medium - and the METERED intakes of the barracks
    // and turret are resolved by the transport's inject, see transportInject.)
    // The union's machine variant (barracks spawn / turret fire logic runs per-entity in the
    // component update; the selected unit type's prices + the population cap are stamped here so
    // the tweaks and the house links stay live). The barracks' energy CAPACITY is the selected
    // unit's cost: the store fills at the capped intake and reads as the BUILD BAR, full = spawn
    // (build time = cost / intake). Switching the type re-clamps the store to the new cost. A
    // TURRET works the same way, its capacity being one shot (see the intake block above).
    if (isBarracksType(s.type))
    {
        c.machineKind = GameStructureComponent::EMachineKind::Barracks;
        // (unitType/spawnCost/capacity[0] were resolved above, ahead of the store clamp)
        c.barracks.spawnPop = (uint8)glm::clamp(m_settings.unitPopulation[c.barracks.unitType], 0, 255);
        c.barracks.popCap = m_settings.barracksPopulation + (int)c.barracks.houses * m_settings.housePopulation;
    }
    else
        c.machineKind = s.type == EStructureType::Turret ? GameStructureComponent::EMachineKind::Turret
                      : s.type == EStructureType::MedicStation ? GameStructureComponent::EMachineKind::Medic
                      : s.type == EStructureType::Searchlight ? GameStructureComponent::EMachineKind::Searchlight
                      : GameStructureComponent::EMachineKind::None;
}

void StructureSystem::clear()
{
    // Teardown: silent (no GRm hooks). Deregister the whole roster FIRST - removeRootEntity's
    // onWorldRootRemoved callback then no-ops instead of mutating m_frame under the loop.
    oc::vector<Ref> roster = oc::move(m_frame);
    m_frame.clear();
    m_byId.clear();
    joinTransport(); // the job holds component pointers: joined before any entity dies
    for (const Ref& s : roster)
        Globals::world.removeRootEntity(s.entity);
    for (const Walker& walker : m_walkers)
        if (walker.entity)
            Globals::world.removeRootEntity(walker.entity.get());
    m_walkers.clear();
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_nodes.clear();
    m_requests.clear();
    m_demolishRequests.clear();
    m_routeRequests.clear();
    m_cells.clear();
    joinTransport();
    m_net = TransportNet{};
    m_savedFills.clear();
    m_terrainBlocked.clear();
    m_linksDirty = true;
}

// ---------------------------------------------------------------- nodes + world

void StructureSystem::spawnNode(float x, float z, ENodeType type, float groundY)
{
    Node node;
    node.type = type;
    node.pos = glm::vec3(x, groundY + c_nodeHeight, z);
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
    // Map regeneration only (a new seed re-places the whole set).
    for (Node& n : m_nodes)
        if (n.entity)
            Globals::world.removeRootEntity(n.entity.get());
    m_nodes.clear();
}

bool StructureSystem::harvestYield(EStructureType type, const glm::vec3& groundPos, float& outYield) const
{
    outYield = 1.0f;
    if (!isHarvestType(type))
        return true;
    const bool lumber = type == EStructureType::Lumberyard;
    const float radius = m_settings.harvestRadius;
    float yield = 0.0f;
    // The flat maps' deposits: one node of the yard's kind in the radius is a full yield.
    for (const Node& n : m_nodes)
    {
        const glm::vec2 d = glm::vec2(n.pos.x, n.pos.z) - glm::vec2(groundPos.x, groundPos.z);
        if ((n.type == ENodeType::Fuel) == lumber && glm::dot(d, d) <= radius * radius)
            yield = 1.0f;
    }
    // The sandbox world: the trees and the rocks that stand there.
    if (m_harvestSource)
    {
        float wood = 0.0f, stone = 0.0f;
        if (!m_harvestSource(groundPos.x, groundPos.z, radius, wood, stone))
            return false;
        yield = glm::max(yield, lumber ? wood / glm::max(m_settings.harvestTreesFull, 1.0f)
                                       : stone / glm::max(m_settings.harvestStoneFull, 1.0f));
    }
    outYield = glm::clamp(yield, glm::clamp(m_settings.harvestMinYield, 0.0f, 1.0f), 1.0f);
    return true;
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

// ---------------------------------------------------------------- requests

void StructureSystem::queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, float yield,
    const glm::vec3& facing, uint8 team)
{
    m_requests.push_back(PlaceRequest{ type, groundPos, facing, yield, team });
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

void StructureSystem::queueUnitTypeRequest(uint32 id, uint8 unitType, uint8 team)
{
    m_unitTypeRequests.push_back({ id, unitType, team });
}

// ---------------------------------------------------------------- the ticks

void StructureSystem::tickAuthority(const glm::vec3&, float deltaSec)
{
    ProfileScope scope("Structures authority", EProfileCategory::Game);
    m_time += deltaSec;
    refresh(); // the frame view every request/tick below works on
    ProfileScope requestScope("Structure requests", EProfileCategory::Game);
    for (const PlaceRequest& req : m_requests)
        placeStructure(req.type, req.pos, req.yield, req.facing, req.team);
    m_requests.clear();
    for (const auto& [id, team] : m_demolishRequests)
        applyDemolishRequest(id, team);
    m_demolishRequests.clear();
    for (RouteRequest& request : m_routeRequests)
    {
        const int index = structureIndexById(request.id);
        if (index < 0 || !isBarracksType(m_frame[index].type)
            || m_frame[index].state->team != request.team)
            continue; // died, not a barracks, or someone else's - refused (the MP seam)
        GameStructureComponent& s = *m_frame[index].state;
        request.points.resize(glm::min(request.points.size(), (size_t)MaxRouteWaypoints));
        s.route = oc::move(request.points);
        if (onRouteChanged)
            onRouteChanged(request.id);
        // LIVE ORDERS: units already spawned from this barracks pick the new route up too - a
        // World root walk via the GameMatch-wired hook (units are World roots, no roster here).
        if (onRouteLiveUnits)
            onRouteLiveUnits(request.id, oc::span<const glm::vec3>(s.route.data(), s.route.size()));
    }
    m_routeRequests.clear();
    for (const UnitTypeRequest& request : m_unitTypeRequests)
    {
        const int index = structureIndexById(request.id);
        if (index < 0 || !isBarracksType(m_frame[index].type)
            || m_frame[index].state->team != request.team || !isBarracksUnitType(request.unitType))
            continue; // died, not a barracks, someone else's, or garbage - refused (the MP seam)
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

    // Same-tick networks for fresh placements (each placeStructure above set the dirty flag); a
    // death in tickDamage below re-dirties and rebuilds on the NEXT tick. The transport job from
    // last frame joins first (inside the rebuild, or here) and hands its cells to the stores.
    joinTransport();
    rebuildNetworks();
    tickProduction(deltaSec);
    // Death sweep BEFORE constructors: last frame's damage lands after this tick (contacts +
    // entity pass), so the sweep must judge it before a repair trickle can resurrect a 0-hp
    // structure - repairs-first made buildings unkillable inside any constructor's range (the
    // heal always ran between the killing blow and the sweep). A structure that ends a frame at
    // EXACTLY 0 now dies; anything above 0 can still be out-healed legitimately.
    tickDamage(deltaSec);
    tickConstructors(deltaSec);
    tickCarriers(deltaSec); // wood and stone: the walkers on the road
    tickLight(deltaSec);    // light: the beams between the buildings and their hubs
    drawLightLinks();       // HERE, not in drawDebug: a light record added before the begin-frame job is reset by it
    publishSearchlights();  // the same rule: its spot lights are light records
    // The transport: inject the due runs' supply/demand and kick the job - it overlaps present
    // and the next frame's front, joined at the top of the next tick.
    m_transportTime += deltaSec;
    kickTransport();
}

void StructureSystem::tickMirror(float deltaSec)
{
    ProfileScope scope("Structures mirror", EProfileCategory::Game);
    refresh();
    rebuildNetworks(); // clients derive the graph locally from the mirrored structures - same
                       // code, same inputs; the fills arrive by id (GCf), nothing is simulated
    updateStaffing();  // the HUD's workers and the "Short of workers" badge, from the mirrored roster
    if (m_settings.lightBeams) // the light links, for the beams and the badges (the flow is the authority's)
    {
        m_lightLinkTimer -= deltaSec;
        if (m_lightLinkTimer <= 0.0f || m_lightLinkCount != m_frame.size())
        {
            m_lightLinkTimer = 1.0f;
            rebuildLightLinks();
        }
        // The mirror moves no light: each link's flow is an ESTIMATE from what the building makes or draws.
        for (Ref& ref : m_frame)
        {
            float flow = 0.0f;
            if (ref.lightId != 0 && !ref.state->blueprint)
            {
                if (ref.type == EStructureType::Solar)
                    flow = m_settings.solarEnergyPerSec * m_settings.solarDaylight;
                else if (ref.type == EStructureType::Generator)
                    flow = ref.state->store[1] > 0.0f ? m_settings.genEnergyPerSec : 0.0f;
                else if (hasShieldEmitter(ref.type))
                    flow = emitterDrawOf(ref.type) * glm::clamp(m_settings.fieldNight, 0.0f, 1.0f);
                else if (ref.state->capacity[0] > 0.0f && ref.state->store[0] < ref.state->capacity[0] - 0.05f)
                    flow = m_settings.lightFeedRate;
            }
            ref.lightFlow = flow;
        }
        drawLightLinks();
    }
    publishSearchlights(); // the cones: aimed by the mirrored components themselves (see GameStructureComponent::update)
    // Ease each emitter's fraction toward the synced target at ramp-like speed, then drive the
    // LOCAL field from it - the bubble animates as smoothly as the server's own.
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
            // The mirror knows no unit drain: a field is awake at night, or under the pressure its own emitter reads.
            const float awake = fc->emitter.getPressure() > 0.01f ? 1.0f : glm::clamp(m_settings.fieldNight, 0.0f, 1.0f);
            fc->emitter.setShellAlpha(emitterShellAlpha(s.capacity[0] > 0.0f ? s.store[0] / s.capacity[0] : 0.0f,
                s.structureId, m_settings.emitterWarnCharge) * glm::mix(m_settings.fieldDayAlpha, 1.0f, awake));
        }
    }
}
