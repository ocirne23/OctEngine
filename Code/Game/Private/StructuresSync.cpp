module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import File;
import :Structures;

// STATE IN AND OUT (see Structures.ixx): the client-side MIRROR appliers (idempotent, driven by
// GameMatch's game events — GPl / GRm / GSt / GCb / GRt / GBu), and the F9/F10 SAVE/LOAD of the
// whole structure set (ids preserved, the networks re-derive from the cells on load).

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
    s.store[0] = chargeFrac * s.capacity[0]; // per-instance (a barracks' capacity = its unit's cost)
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
    // ONLY NON-DEFAULT VALUES are written (a base is hundreds of cable segments): every optional
    // key below has a load fallback that restores the same state when it is missing — full
    // health, empty stores, built, no facing, no node, team 0, a dark emitter, the Grunt barracks,
    // no fill. Id, Type and Position are the keys every structure carries. A BLUEPRINT's health is
    // its build progress, always below max, so it is always written (the fallback is "full").
    for (const Ref& s : m_frame)
    {
        AssetNode& n = root.addChild("Structure");
        n.set("Id", oc::to_string(s.state->structureId));
        n.set("Type", oc::to_string((int)s.type));
        n.set("Position", s.entity->pos);
        const glm::vec3 forward = s.type == EStructureType::Lance || isCrossingType(s.type)
            ? s.entity->rot * glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f);
        if (glm::dot(glm::vec2(forward.x, forward.z), glm::vec2(forward.x, forward.z)) > 1e-4f)
            n.set("Facing", glm::vec3(forward.x, 0.0f, forward.z));
        if (s.nodeIndex >= 0)
            n.set("NodeIndex", oc::to_string(s.nodeIndex));
        if (s.state->team != 0)
            n.set("Team", oc::to_string((int)s.state->team));
        if (s.state->blueprint)
            n.set("Blueprint", true); // bitfield -> the bool overload
        if (s.state->health < s.state->healthMax - 1e-3f)
            n.set("Health", s.state->health);
        if (s.state->store[0] > 1e-3f)
            n.set("Charge", s.state->store[0]);
        if (s.state->store[1] > 1e-3f)
            n.set("Fuel", s.state->store[1]);
        if (s.state->store[2] > 1e-3f)
            n.set("Minerals", s.state->store[2]);
        if (isCableOrCrossing(s.type)) // the cells it holds (transport node fill)
            if (const auto it = m_net.nodeById.find(s.state->structureId);
                it != m_net.nodeById.end() && m_net.nodes[it->second].fill != 0)
                n.set("Fill", oc::to_string((int)m_net.nodes[it->second].fill));
        if (hasShieldEmitter(s.type) && s.state->emitter.outputFrac > 1e-3f) // union variants: only the active one is meaningful
            n.set("OutputFrac", s.state->emitter.outputFrac);
        if (isBarracksType(s.type) && s.state->barracks.unitType != 0)
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
    joinTransport();
    m_net = TransportNet{};
    m_savedFills.clear();
    m_linksDirty = true;
}

void StructureSystem::loadFrom(const AssetNode& root)
{
    refresh();
    clearAllStructures();
    // THE FALLBACKS BELOW ARE THE SAVE'S DEFAULTS: saveTo omits every key whose value the
    // fallback restores (full health, empty stores, built, no facing, no node, team 0, a dark
    // emitter, unit type 0, no fill), so a missing key is the common case, not an old save. Keep
    // the two in step.
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
        if (const AssetNode* f = n->find("Fill"); f && isCableOrCrossing((EStructureType)typeInt))
            m_savedFills[id] = (uint16)glm::clamp(f->asInt(), 0, 65535); // the rebuild below picks it up
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
    // (Old saves' Cable nodes are ignored: the networks derive from the cable segments' cells.)
    rebuildNetworks(); // every spawnStructure above set the dirty flag
    Log::info("Game state loaded: " + oc::to_string(m_frame.size()) + " structures, "
        + oc::to_string(m_net.runs.size()) + " cable runs");
}
