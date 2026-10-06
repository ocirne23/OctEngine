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

static constexpr const char* structureNames[] = { "Emitter", "Generator", "Connector", "Extractor",
    "Battery", "Fuel tank", "Solar", "Fabricator", "Bastion", "Lance", "Barracks", "Brute barracks",
    "Runner barracks", "Spitter barracks", "Wall", "Turret", "Mineral silo", "Constructor", "Base",
    "Power cable", "Pipeline", "Conveyor", "Power crossing", "Pipe crossing", "Conveyor crossing",
    "House", "Medic station" };
// Spawn height = each prefab's box HALF height, so every shape sits flush (see the prefabs).
static constexpr float structureSpawnHeights[] = { 1.0f, 1.0f, 3.0f, 2.0f, 0.5f, 2.0f, 0.5f, 2.0f,
    2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 2.0f, 2.0f, 4.0f, 1.0f, 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f,
    0.25f, 1.5f, 1.0f };
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
            ? "Shield CONE along its aimed facing: pushes enemies out and drains them."
            : "Shield bubble: pushes enemies out and drains their batteries.";
        in(emitterDrawOf(type), "energy");
        in(m_settings.emitterPressureDraw, "energy", " more at full pressure");
        range(emitterReachOf(type));
        break;
    case EStructureType::Generator:
        desc = "Burns fuel into grid energy.";
        in(m_settings.fuelBurnRate, "fuel");
        out_(m_settings.genEnergyPerSec, "energy");
        break;
    case EStructureType::Solar:
        desc = "Free energy trickle. Needs no fuel.";
        out_(m_settings.solarEnergyPerSec, "energy");
        break;
    case EStructureType::Extractor:
        desc = "Mines the resource node under it.";
        in(m_settings.extractorEnergyPerSec, "energy");
        out_(m_settings.mineralRate, "minerals", " on a mineral node");
        out_(m_settings.fuelRate, "fuel", " on a fuel node");
        break;
    case EStructureType::Fabricator:
        desc = "Converts fuel and power into minerals.";
        in(m_settings.fabricatorEnergyPerSec, "energy");
        in(m_settings.fabricatorFuelPerSec, "fuel");
        out_(m_settings.fabricatorMineralsPerSec, "minerals");
        break;
    case EStructureType::Constructor:
        desc = "Builds and repairs nearby structures from its mineral stock.";
        in(m_settings.extractorEnergyPerSec, "energy");
        in(m_settings.constructorBuildRate, "minerals", " while building");
        range(m_settings.constructorRange);
        break;
    case EStructureType::Battery:
        desc = "Banks grid energy for the peaks.";
        banks(m_settings.batteryCapacity, "energy");
        break;
    case EStructureType::FuelTank:
        desc = "Banks the fuel the extractors pipe in.";
        banks(m_settings.fuelTankCapacity, "fuel");
        break;
    case EStructureType::MineralSilo:
        desc = "Banks minerals: only silos and the Base hold SPENDABLE stock.";
        banks(m_settings.mineralSiloCapacity, "minerals");
        break;
    case EStructureType::Barracks:
        desc = "Trains units. Its power draw IS the build bar.";
        in(m_settings.barracksEnergyIntake, "energy");
        line(oc::format("Build time = the unit's energy cost / {:g}", m_settings.barracksEnergyIntake));
        line(oc::format("Population {} (+{} per linked house)", m_settings.barracksPopulation, m_settings.housePopulation));
        break;
    case EStructureType::House:
        desc = "Raises the population cap of the nearest barracks. Needs no power.";
        line(oc::format("+ {} population", m_settings.housePopulation));
        range(m_settings.houseLinkRadius); // how far it reaches for that barracks
        break;
    case EStructureType::Turret:
        desc = "Hitscan lightning at the nearest enemy unit. Never misses.";
        in(p.turretShotEnergy / glm::max(p.turretFireInterval, 1e-3f), "energy");
        banks(p.turretShotEnergy, "energy per shot");
        line(oc::format("{:g} damage every {:g} s", p.turretDamage, p.turretFireInterval));
        range(p.turretRange);
        break;
    case EStructureType::MedicStation:
        desc = "Heals own units and players standing in its radius.";
        in(m_settings.medicEnergyPerSec, "energy");
        out_(medicHealRate(), "health", " and shield, per body");
        range(medicHealRadius());
        break;
    case EStructureType::Wall:
        desc = "Breachable barrier: enemies chew through it instead of walking around. Needs no power.";
        break;
    case EStructureType::CablePower:
    case EStructureType::CablePipe:
    case EStructureType::CableConveyor:
    {
        static constexpr const char* c_what[3] = { "energy", "fuel", "minerals" };
        const int medium = cableMediumOf(type);
        desc = oc::format("Carries {} between the buildings its run touches.", c_what[medium]);
        out_(m_settings.cableThroughput[medium], c_what[medium], " per link");
        line("Paint it: hold and drag. Crossings place themselves.");
        break;
    }
    case EStructureType::CrossingPower:
    case EStructureType::CrossingPipe:
    case EStructureType::CrossingConveyor:
    {
        static constexpr const char* c_what[3] = { "energy", "fuel", "minerals" };
        desc = oc::format("Bridges {} over a line of another medium.", c_what[crossingMediumOf(type)]);
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
        out += oc::format("- {:g} minerals to build", cost);
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
    // The roster is maintained at the spawn/remove seams (no world query - type, node and id index
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
    // node by index are cleared by the caller around this - every nodeIndex access elsewhere is
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
        }
    }
}
