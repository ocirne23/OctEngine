module Game;

import Core;
import Core.glm;
import Core.Transform;
import Entity;
import Physics;
import Spatial;
import :Structures;

// THE CARRIERS (see Structures.ixx): wood (store 1) and stone (store 2) move by WALKERS on the road, not as cells on
// the cables. Authority only: the stores they change mirror to clients through GSt like every store; the walker
// entities are local visuals of the authority.

namespace
{
    constexpr int c_maxRoadCells = 4096;      // the search stops there: a road network larger than this is not walked
    constexpr float c_walkHeight = 0.1f;      // the walker's feet over the road segment's origin
    constexpr float c_retrySeconds = 1.0f;    // a waiting walker looks for a load (and for other work) this often
    constexpr float c_passSeconds = 1.0f;     // the house pass
    constexpr float c_idleLeave = 8.0f;       // the one walker of a job with nothing to carry may take a waiting load elsewhere
    constexpr float c_idleHome = 20.0f;       // a walker with nothing to do for this long goes home (not a producer's own one)
    constexpr const char* c_carrierPrefab = "Entities/Game/carrier.pre";
    constexpr glm::ivec2 c_neighbours[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

    enum EWalkerState : uint8
    {
        Walker_ToJob,      // walking to its job's pickup, empty
        Walker_Waiting,    // at the pickup
        Walker_Delivering, // loaded, walking to the target
        Walker_Returning,  // walking back to the pickup (with what the target did not take)
        Walker_Home,       // walking home; it leaves the road there
        Walker_Working,    // at a blueprint or a damaged structure: its stone goes in over time ("Build rate (stone/s)")
        // THE GUARD states (its job is an alarmed Armory), all from here on:
        Walker_GuardToArmory, // walking to the Armory for its bow
        Walker_GuardToPost,   // armed, walking to its post
        Walker_Guarding,      // at its post: it shoots the nearest enemy unit in "Guard range (m)"
    };
    constexpr const char* c_guardPrefab = "Entities/Game/guard.pre";
    constexpr float c_guardRepost = 4.0f; // s with nothing to shoot before a guard looks for a better post

    // The buildings that hand out bows: the Armory, and the Beacon (as many guards as one Armory).
    constexpr bool armsGuards(EStructureType t)
    {
        return t == EStructureType::Armory || t == EStructureType::Base;
    }

    glm::ivec2 cellOf(const glm::vec3& pos)
    {
        return glm::ivec2((int)std::floor(pos.x / StructureSystem::GridCellSize), (int)std::floor(pos.z / StructureSystem::GridCellSize));
    }
}

// A BUILT road piece in the cell: the primary occupant or the cable under a crossing. outY = its entity height.
bool StructureSystem::roadAt(int cx, int cz, float& outY) const
{
    const auto it = m_cells.find(cellKey(cx, cz));
    if (it == m_cells.end())
        return false;
    for (const uint32 id : { it->second.id, it->second.underId })
    {
        if (id == 0)
            continue;
        const int index = structureIndexById(id);
        if (index < 0 || m_frame[index].state->blueprint)
            continue;
        const EStructureType type = m_frame[index].type;
        if (cableMediumOf(type) == 2 || type == EStructureType::CrossingConveyor) // the Road, the Bridge, the road crossing
        {
            outY = m_frame[index].entity->pos.y;
            if (m_frame[index].walkDir >= 0) // a Walkway's stairs: the middle of the slope
                outY -= WalkwayDeckHeight * 0.5f;
            return true;
        }
    }
    return false;
}

bool StructureSystem::touchesRoad(int index) const
{
    bool road = false;
    const Ref& ref = m_frame[index];
    forEachFootprintCell(ref.type, ref.entity->pos, ref.entity->rot, [&](int cx, int cz)
    {
        float y;
        for (const glm::ivec2& n : c_neighbours)
            road = road || roadAt(cx + n.x, cz + n.y, y);
    });
    return road;
}

// Does `targetIdx` take `good` from `sourceIdx`? Its role in that medium decides (transportRoleOf): a consumer while
// it has room (and, for a storage's carrier, only while it is under half full: a storage does not top up a consumer
// that a producer already feeds), a storage while it has room. Never a producer, a blueprint or another team.
float StructureSystem::blueprintNeed(const Ref& ref) const
{
    const GameStructureComponent& s = *ref.state;
    if (!s.blueprint || !isPlaceableType(ref.type) || rubbleBlocked(ref)) // (rubble with an enemy near: no trip to it)
        return 0.0f;
    const float healthMax = glm::max(s.healthMax, 1e-3f); // investMaterials' price: the type's cost over its health
    return glm::max((healthMax - s.health) * glm::max(m_settings.costs[(int)ref.type], 0.01f) / healthMax - ref.buildIncoming, 0.0f);
}

// The stone a DAMAGED built structure needs to be whole again, less what the walkers already carry to it. A blueprint
// (rubble too) is build work, not a repair.
float StructureSystem::repairNeed(const Ref& ref) const
{
    const GameStructureComponent& s = *ref.state;
    if (s.blueprint || (!isPlaceableType(ref.type) && ref.type != EStructureType::Base))
        return 0.0f;
    const float healthMax = glm::max(s.healthMax, 1e-3f);
    return glm::max((healthMax - s.health) * glm::max(m_settings.costs[(int)ref.type], 0.01f) / healthMax - ref.buildIncoming, 0.0f);
}

float StructureSystem::transportAmount(int sourceIdx, int targetIdx, int good) const
{
    constexpr float c_minFillGap = 0.15f; // of a full store: closer fills are even enough
    constexpr float c_minCart = 0.2f;    // of a cart: a smaller load is no trip
    const Ref& source = m_frame[sourceIdx];
    const Ref& target = m_frame[targetIdx];
    const GameStructureComponent& s = *source.state;
    const GameStructureComponent& t = *target.state;
    if (sourceIdx == targetIdx || s.team != t.team || s.blueprint || t.blueprint || s.capacity[good] <= 0.0f || t.capacity[good] <= 0.0f
        || transportRoleOf(source.type, good) != ETransportRole::Storage || transportRoleOf(target.type, good) != ETransportRole::Storage)
        return 0.0f;
    const float held = t.store[good] + target.goodsIncoming[good]; // with what is on its way there
    const float gap = s.store[good] / s.capacity[good] - held / t.capacity[good];
    if (gap < c_minFillGap)
        return 0.0f;
    const float cart = glm::max(m_settings.carrierCartLoad, 1.0f);
    const float even = gap * s.capacity[good] * t.capacity[good] / (s.capacity[good] + t.capacity[good]); // the amount that makes the fills equal
    const float amount = glm::min(glm::min(cart, even), glm::min(s.store[good], t.capacity[good] - held));
    return amount >= cart * c_minCart ? amount : 0.0f;
}

bool StructureSystem::carrierAccepts(int sourceIdx, int targetIdx, int good, bool consumersOnly, int work, bool& isConsumer) const
{
    if (targetIdx == sourceIdx)
        return false;
    if (work == CarrierWork_Transport)
    {
        isConsumer = false;
        return transportAmount(sourceIdx, targetIdx, good) > 0.0f;
    }
    const Ref& target = m_frame[targetIdx];
    const GameStructureComponent& t = *target.state;
    if (t.team != m_frame[sourceIdx].state->team)
        return false;
    // WORK (ECarrierWork): a BLUEPRINT at the road takes STONE until it is built - a building, and a piece of the
    // road itself (the search looks at what every road cell touches, so the blueprint piece at the end of a built
    // road is found); a DAMAGED structure at the road takes stone until it is whole.
    if (work != CarrierWork_Goods || t.blueprint)
    {
        isConsumer = true;
        if (good != 2)
            return false;
        if (work == CarrierWork_Build)
            return t.blueprint && blueprintNeed(target) > 0.01f;
        return work == CarrierWork_Repair && repairNeed(target) > 0.25f; // (never a blueprint: repairNeed is 0 there)
    }
    if (isCableOrCrossing(target.type) || t.capacity[good] <= 0.0f)
        return false;
    const ETransportRole role = transportRoleOf(target.type, good);
    if (role == ETransportRole::Producer)
        return false;
    isConsumer = role == ETransportRole::Consumer;
    // ONLY WHERE THE LOAD FITS IN FULL: the load the source would send (a full one, or all of a smaller store) against
    // the target's room, less what other walkers already carry to it. A hearth that misses one wood is no target for
    // five: the walker would bring four back. (A target smaller than a load takes what its whole store holds.)
    const float load = glm::min(glm::max(m_settings.carrierLoad, 1.0f), m_frame[sourceIdx].state->store[good]);
    const float fit = glm::min(load, t.capacity[good]);
    const bool fits = fit > 0.0f && t.capacity[good] - t.store[good] - target.goodsIncoming[good] >= fit - 1e-3f;
    if (consumersOnly)
        return isConsumer && fits && t.store[good] < t.capacity[good] * 0.5f;
    return fits;
}

// ---- The Armory's guards ----

// The nearest living enemy unit of `team` within `radius` of pos (flat, on the map); null = none.
// sightFrom: the unit must also be IN SIGHT of that point - no static body (a building, a wall, a gate, the terrain, a
// tree or a rock) on the line to it. Only the c_sightTries nearest ones are tried: one ray each, not one per unit of a horde.
GameUnitComponent* StructureSystem::nearestEnemyUnit(const glm::vec3& pos, float radius, uint32 team, glm::vec3& outPos,
    const glm::vec3* sightFrom) const
{
    constexpr int c_sightTries = 6;
    struct Candidate { GameUnitComponent* unit; glm::vec3 pos; float distSq; };
    Candidate nearest[c_sightTries];
    int count = 0;
    const int keep = sightFrom ? c_sightTries : 1;
    Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), radius, SpatialLayer_Render, [&](uint64 user)
    {
        Entity* other = reinterpret_cast<Entity*>(user);
        GameUnitComponent* unit = getComponent<GameUnitComponent>(other);
        if (!unit || unit->puppet || unit->team == team || !unit->alive())
            return;
        const glm::vec2 d(other->pos.x - pos.x, other->pos.z - pos.z);
        const float distSq = glm::dot(d, d);
        if (distSq > radius * radius || (count == keep && distSq >= nearest[count - 1].distSq))
            return;
        int at = count < keep ? count++ : count - 1; // sorted by distance: the new one goes in at its place
        for (; at > 0 && nearest[at - 1].distSq > distSq; --at)
            nearest[at] = nearest[at - 1];
        nearest[at] = { unit, other->pos, distSq };
    });
    const uint64 sightMask = !sightFrom ? 0 :PhysicsLayers::bit("Default") | PhysicsLayers::bit("Gate") | PhysicsLayers::bit("Terrain")
        | PhysicsLayers::bit("Vegetation");
    for (int i = 0; i < count; ++i)
    {
        const glm::vec3 to = nearest[i].pos + glm::vec3(0.0f, 0.5f, 0.0f);
        if (sightFrom && Globals::physics.castRayClosest(*sightFrom, to - *sightFrom, sightMask, nullptr, true).hit)
            continue;
        outPos = nearest[i].pos;
        return nearest[i].unit;
    }
    return nullptr;
}

// A built Walkway DECK in the cell (not its stairs): a guard on it shoots over the walls.
bool StructureSystem::walkwayDeckAt(int cx, int cz) const
{
    const auto it = m_cells.find(cellKey(cx, cz));
    const int index = it != m_cells.end() ? structureIndexById(it->second.id) : -1;
    return index >= 0 && m_frame[index].type == EStructureType::Walkway && !m_frame[index].state->blueprint && m_frame[index].walkDir == -1;
}

// An alarmed, built Armory of the team that has fewer guards than "Guards".
bool StructureSystem::guardWanted(int index, uint8 team) const
{
    const Ref& ref = m_frame[index];
    return armsGuards(ref.type) && !ref.state->blueprint && ref.state->team == team && ref.alarm > 0.0f
        && (int)ref.guards < m_settings.armoryGuards;
}

// A guard's POST, by road from fromCell: a road cell within the Armory's range that no other guard has. A Walkway DECK
// cell first (the place behind a wall), then any road cell; among those, the one nearest to the Armory's threat. One
// breadth-first search over the road (like roadSearch); outPath = the walk there (one point: it stands on the post).
bool StructureSystem::guardPost(glm::ivec2 fromCell, int armoryIdx, size_t self, oc::vector<glm::vec3>& outPath) const
{
    const Ref& armory = m_frame[armoryIdx];
    const glm::vec2 centre(armory.entity->pos.x, armory.entity->pos.z), threat(armory.threat.x, armory.threat.z);
    const float rangeSq = m_settings.armoryRange * m_settings.armoryRange;
    oc::small_vector<uint64, 32> taken;
    for (size_t k = 0; k < m_walkers.size(); ++k)
        if (const Walker& w = m_walkers[k]; k != self && (w.state == Walker_GuardToPost || w.state == Walker_Guarding) && !w.path.empty())
            taken.push_back(cellKey(cellOf(w.path.back()).x, cellOf(w.path.back()).y));

    struct Visit { int cx, cz, parent; float y; };
    oc::vector<Visit> open;
    oc::unordered_map<uint64, int> seen;
    const auto visit = [&](int cx, int cz, int parent)
    {
        float y;
        if ((int)open.size() >= c_maxRoadCells || seen.count(cellKey(cx, cz)) || !roadAt(cx, cz, y))
            return;
        seen[cellKey(cx, cz)] = (int)open.size();
        open.push_back({ cx, cz, parent, y });
    };
    visit(fromCell.x, fromCell.y, -1);
    int best = -1;
    float bestScore = FLT_MAX;
    for (int i = 0; i < (int)open.size(); ++i)
    {
        const Visit cell = open[i]; // a copy: visit() grows the vector
        const glm::vec2 pos(((float)cell.cx + 0.5f) * GridCellSize, ((float)cell.cz + 0.5f) * GridCellSize);
        if (glm::dot(pos - centre, pos - centre) <= rangeSq && oc::find(taken.begin(), taken.end(), cellKey(cell.cx, cell.cz)) == taken.end())
            if (const float score = glm::distance(pos, threat) - (walkwayDeckAt(cell.cx, cell.cz) ? 10000.0f : 0.0f); score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        for (const glm::ivec2& n : c_neighbours)
            visit(cell.cx + n.x, cell.cz + n.y, i);
    }
    if (best < 0)
        return false;
    outPath.clear();
    for (int i = best; i >= 0; i = open[i].parent)
        outPath.push_back(glm::vec3(((float)open[i].cx + 0.5f) * GridCellSize, open[i].y + c_walkHeight, ((float)open[i].cz + 0.5f) * GridCellSize));
    oc::reverse(outPath.begin(), outPath.end());
    return true;
}

// The walker's look: carrier.pre, or guard.pre while it has a bow. The entity is replaced where it stands.
void StructureSystem::setGuardLook(Walker& w, bool guard)
{
    if (w.guard == guard || !w.entity)
        return;
    const Transform at(w.entity->pos, 1.0f, w.entity->rot);
    Globals::world.removeRootEntity(w.entity.get());
    w.entity = Globals::world.spawnAssetFile(guard ? c_guardPrefab : c_carrierPrefab, at, true);
    if (w.entity)
        Globals::world.addRootEntity(w.entity);
    w.guard = guard;
}

int StructureSystem::roadSearch(int fromStructure, glm::ivec2 fromCell, const oc::function<int(int)>& goal, oc::vector<glm::vec3>& outPath) const
{
    struct Visit { int cx, cz, parent; float y; };
    oc::vector<Visit> open;
    oc::unordered_map<uint64, int> seen;
    const auto visit = [&](int cx, int cz, int parent)
    {
        float y;
        if ((int)open.size() >= c_maxRoadCells || seen.count(cellKey(cx, cz)) || !roadAt(cx, cz, y))
            return;
        seen[cellKey(cx, cz)] = (int)open.size();
        open.push_back({ cx, cz, parent, y });
    };
    if (fromStructure >= 0)
    {
        const Ref& source = m_frame[fromStructure];
        forEachFootprintCell(source.type, source.entity->pos, source.entity->rot, [&](int cx, int cz)
        {
            for (const glm::ivec2& n : c_neighbours)
                visit(cx + n.x, cz + n.y, -1);
        });
    }
    else
        visit(fromCell.x, fromCell.y, -1);

    int found = -1, foundCell = -1;       // the first goal 2: the search ends there
    int fallback = -1, fallbackCell = -1; // the first goal 1: used when there is no goal 2 on the road
    for (int i = 0; i < (int)open.size() && found < 0; ++i)
    {
        const Visit cell = open[i]; // a copy: visit() grows the vector
        for (const glm::ivec2& n : c_neighbours)
        {
            const int cx = cell.cx + n.x, cz = cell.cz + n.y;
            if (const auto it = m_cells.find(cellKey(cx, cz)); it != m_cells.end() && it->second.id != 0)
                if (const int index = structureIndexById(it->second.id); index >= 0)
                {
                    const int priority = goal(index);
                    if (priority >= 2)
                    {
                        found = index;
                        foundCell = i;
                        break;
                    }
                    if (priority == 1 && fallback < 0)
                    {
                        fallback = index;
                        fallbackCell = i;
                    }
                }
            visit(cx, cz, i);
        }
    }
    if (found < 0)
    {
        found = fallback;
        foundCell = fallbackCell;
    }
    if (found < 0)
        return -1;
    outPath.clear();
    for (int i = foundCell; i >= 0; i = open[i].parent)
        outPath.push_back(glm::vec3(((float)open[i].cx + 0.5f) * GridCellSize, open[i].y + c_walkHeight,
            ((float)open[i].cz + 0.5f) * GridCellSize));
    oc::reverse(outPath.begin(), outPath.end());
    return found;
}

int StructureSystem::findRoadTarget(int sourceIdx, int fromStructure, glm::ivec2 fromCell, int good, bool consumersOnly, int work,
    oc::vector<glm::vec3>& outPath) const
{
    // THE NEAREST TAKER wins, a storage as much as a consumer (the search ends at the first one, and it goes out by
    // road distance): a storage near the producer takes the load, and the storage's own carriers feed the consumers
    // farther on. (A consumer first, whatever the distance, sent every load past a storehouse with room to a far one.)
    return roadSearch(fromStructure, fromCell, [&](int index)
    {
        bool isConsumer = false;
        return carrierAccepts(sourceIdx, index, good, consumersOnly, work, isConsumer) ? 2 : 0;
    }, outPath);
}

// A producer sends out a full load (or its whole small store); a storage feeds hungry consumers only.
bool StructureSystem::carrierSendGood(int index, int good, bool& consumersOnly) const
{
    const Ref& ref = m_frame[index];
    const GameStructureComponent& s = *ref.state;
    if (s.capacity[good] <= 0.0f)
        return false;
    const float loadSize = glm::max(m_settings.carrierLoad, 1.0f);
    const ETransportRole role = transportRoleOf(ref.type, good);
    consumersOnly = role == ETransportRole::Storage;
    if (role == ETransportRole::Producer)
        return s.store[good] >= glm::min(loadSize, s.capacity[good] * 0.5f);
    return role == ETransportRole::Storage && s.store[good] >= loadSize;
}

// Stone a walker can take to a blueprint: any store of it but a consumer's own buffer.
bool StructureSystem::carrierHoldsStone(int index) const
{
    const Ref& ref = m_frame[index];
    return ref.state->capacity[2] > 0.0f && ref.state->store[2] > 0.01f && transportRoleOf(ref.type, 2) != ETransportRole::Consumer;
}

int StructureSystem::carrierJobTier(int index, uint8 team) const
{
    const Ref& ref = m_frame[index];
    const GameStructureComponent& s = *ref.state;
    if (s.blueprint || s.team != team || isCableOrCrossing(ref.type) || (int)ref.walkers >= glm::max(m_settings.carriersPerBuilding, 1))
        return 0;
    if (ref.sendReady && ref.walkersIdle == 0)
        return 2;
    if ((ref.buildReady || ref.repairReady || ref.transportReady) && ref.walkersIdle == 0) // stone for a blueprint or a repair, a cart trip: only when no load waits anywhere
        return 1;
    if (ref.walkers == 0)
        for (int good = 1; good <= 2; ++good)
            if (s.capacity[good] > 0.0f && transportRoleOf(ref.type, good) == ETransportRole::Producer)
                return 1;
    return 0;
}

// The ~1 s pass: which buildings have a load to send, and every House / Beacon on a road sends out its team's FREE
// workers (workers - production jobs) while a job wants one, or calls them back when production took the worker.
void StructureSystem::carrierHousePass()
{
    // THE ALARM: an enemy unit within "Range (m)" of a built Armory calls its guards out, and holds them for "Stand down
    // after (s)" (tickCarriers counts it down). The nearest one is the THREAT the posts are picked by.
    bool guardsWanted[GameMaxTeams] = {};
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        if (!armsGuards(ref.type) || ref.state->blueprint || ref.state->team >= GameMaxTeams)
            continue;
        glm::vec3 threat;
        if (nearestEnemyUnit(ref.entity->pos, m_settings.armoryRange, ref.state->team, threat))
        {
            ref.alarm = glm::max(m_settings.armoryStandDown, c_passSeconds * 1.5f);
            ref.threat = threat;
        }
        guardsWanted[ref.state->team] |= guardWanted(i, (uint8)ref.state->team);
    }

    oc::vector<glm::vec3> scratch;
    bool blueprintWaits[GameMaxTeams] = {}; // a team with no blueprint to build needs no search for one
    bool repairWaits[GameMaxTeams] = {};    // the same for a damaged structure
    for (const Ref& ref : m_frame)
        if (ref.state->team < GameMaxTeams)
        {
            blueprintWaits[ref.state->team] |= blueprintNeed(ref) > 0.01f;
            repairWaits[ref.state->team] |= repairNeed(ref) > 0.25f;
        }
    bool buildFound[GameMaxTeams] = {};
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        ref.sendReady = false;
        ref.buildReady = false;
        ref.repairReady = false;
        ref.transportReady = false;
        if (isCableOrCrossing(ref.type) || ref.state->blueprint)
            continue;
        for (int good = 1; good <= 2 && !ref.sendReady; ++good)
        {
            bool consumersOnly = false;
            if (carrierSendGood(i, good, consumersOnly))
                ref.sendReady = !consumersOnly || findRoadTarget(i, i, glm::ivec2(0), good, true, CarrierWork_Goods, scratch) >= 0;
        }
        // BUILD WORK: a building that holds stone (a store, the Beacon, a quarry) with a blueprint on its road.
        if (!ref.sendReady && ref.state->team < GameMaxTeams && blueprintWaits[ref.state->team] && carrierHoldsStone(i))
        {
            ref.buildReady = findRoadTarget(i, i, glm::ivec2(0), 2, false, CarrierWork_Build, scratch) >= 0;
            buildFound[ref.state->team] |= ref.buildReady;
        }
    }
    // REPAIR WORK, the LAST of all: only while the team has no build work on any road (it shares the build work's job
    // tier, so this gate is what puts it behind).
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        if (isCableOrCrossing(ref.type) || ref.state->blueprint || ref.sendReady || ref.state->team >= GameMaxTeams
            || !repairWaits[ref.state->team] || buildFound[ref.state->team] || !carrierHoldsStone(i))
            continue;
        ref.repairReady = findRoadTarget(i, i, glm::ivec2(0), 2, false, CarrierWork_Repair, scratch) >= 0;
    }
    // TRANSPORT (the cart), behind every other work of the building: a storage with a much emptier storage of the same
    // good on its road. Only a team with two storages of a good has any (no search for the others).
    int storages[GameMaxTeams][3] = {};
    for (const Ref& ref : m_frame)
        if (!ref.state->blueprint && ref.state->team < GameMaxTeams)
            for (int good = 1; good <= 2; ++good)
                storages[ref.state->team][good] += ref.state->capacity[good] > 0.0f && transportRoleOf(ref.type, good) == ETransportRole::Storage ? 1 : 0;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        if (ref.state->blueprint || ref.sendReady || ref.buildReady || ref.repairReady || ref.state->team >= GameMaxTeams)
            continue;
        for (int good = 1; good <= 2 && !ref.transportReady; ++good)
            if (storages[ref.state->team][good] >= 2 && ref.state->capacity[good] > 0.0f
                && transportRoleOf(ref.type, good) == ETransportRole::Storage
                && ref.state->store[good] >= glm::max(m_settings.carrierCartLoad, 1.0f) * 0.2f)
                ref.transportReady = findRoadTarget(i, i, glm::ivec2(0), good, false, CarrierWork_Transport, scratch) >= 0;
    }

    int freeLeft[GameMaxTeams];
    for (int t = 0; t < GameMaxTeams; ++t)
        freeLeft[t] = glm::max(m_workers[t] - m_jobs[t], 0);
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        if ((ref.type != EStructureType::House && ref.type != EStructureType::Base) || ref.state->blueprint || ref.state->team >= GameMaxTeams)
            continue;
        const uint8 team = (uint8)ref.state->team;
        const uint32 homeId = ref.state->structureId;
        const int housed = glm::max(ref.type == EStructureType::Base ? m_settings.baseWorkers : m_settings.houseWorkers, 0);
        const int allowed = touchesRoad(i) ? glm::min(housed, freeLeft[team]) : 0;
        int active = 0;
        for (const Walker& w : m_walkers)
            active += w.homeId == homeId && !w.dismissed ? 1 : 0;
        for (int pass = 0; pass < 2 && active > allowed; ++pass) // the waiting ones first
            for (Walker& w : m_walkers)
                if (active > allowed && w.homeId == homeId && !w.dismissed && (pass == 1 || w.state == Walker_Waiting))
                {
                    w.dismissed = true;
                    --active;
                }
        freeLeft[team] = glm::max(freeLeft[team] - (int)ref.homeWalkers, 0); // a called-back one counts until it is home
        if (active >= allowed || freeLeft[team] <= 0)
            continue;
        Walker w;
        // A GUARD first: an alarmed Armory on this road that is short of guards. Then the carrier jobs.
        int job = guardsWanted[team] ? roadSearch(i, glm::ivec2(0), [&](int index) { return guardWanted(index, team) ? 2 : 0; }, w.path) : -1;
        const bool guard = job >= 0 && !w.path.empty();
        if (!guard)
            job = roadSearch(i, glm::ivec2(0), [&](int index) { return carrierJobTier(index, team); }, w.path);
        if (job < 0 || w.path.empty())
            continue;
        w.entity = Globals::world.spawnAssetFile(c_carrierPrefab, Transform(w.path.front()), true);
        if (!w.entity)
            continue;
        Globals::world.addRootEntity(w.entity);
        w.homeId = homeId;
        w.jobId = m_frame[job].state->structureId;
        w.state = guard ? Walker_GuardToArmory : Walker_ToJob;
        if (guard)
            ++m_frame[job].guards;
        else
        {
            ++m_frame[job].walkers;
            ++m_frame[job].walkersIdle;
        }
        ++ref.homeWalkers;
        ++m_carrierCount[team];
        --freeLeft[team];
        m_walkers.push_back(oc::move(w));
    }
}

void StructureSystem::tickCarriers(float deltaSec)
{
    if (!m_settings.carriers && m_walkers.empty())
        return;
    ProfileScope scope("Carriers", EProfileCategory::Game);
    const auto removeWalker = [&](size_t k)
    {
        if (m_walkers[k].entity)
            Globals::world.removeRootEntity(m_walkers[k].entity.get());
        if (k + 1 != m_walkers.size())
            m_walkers[k] = oc::move(m_walkers.back());
        m_walkers.pop_back();
    };

    // The roster's view of the walkers, counted fresh: a removed building takes its walkers along.
    for (Ref& ref : m_frame)
    {
        ref.walkers = ref.walkersIdle = ref.waitRank = ref.homeWalkers = ref.guards = 0;
        ref.alarm = glm::max(ref.alarm - deltaSec, 0.0f);
        ref.buildIncoming = 0.0f;
        ref.goodsIncoming[1] = ref.goodsIncoming[2] = 0.0f;
    }
    for (int& count : m_carrierCount)
        count = 0;
    for (size_t k = 0; k < m_walkers.size(); ++k)
    {
        Walker& w = m_walkers[k];
        const int home = structureIndexById(w.homeId);
        if (home < 0 || !m_settings.carriers)
        {
            removeWalker(k--);
            continue;
        }
        ++m_frame[home].homeWalkers;
        if (m_frame[home].state->team < GameMaxTeams)
            ++m_carrierCount[m_frame[home].state->team];
        if (const int job = w.state != Walker_Home ? structureIndexById(w.jobId) : -1; job >= 0)
        {
            if (w.state >= Walker_GuardToArmory)
                ++m_frame[job].guards;
            else
            {
                ++m_frame[job].walkers;
                if (w.state == Walker_ToJob || w.state == Walker_Waiting)
                    ++m_frame[job].walkersIdle;
            }
        }
        if (w.state == Walker_Delivering || w.state == Walker_Working) // a load on its way (or going in): a blueprint needs that much less stone, a building has that much less room
            if (const int target = structureIndexById(w.targetId); target >= 0)
            {
                if (w.work == CarrierWork_Build || w.work == CarrierWork_Repair)
                    m_frame[target].buildIncoming += w.load;
                else
                    m_frame[target].goodsIncoming[w.good] += w.load;
            }
    }
    if (!m_settings.carriers)
        return;
    bool guardsWanted[GameMaxTeams] = {}; // a waiting walker looks for an Armory only while one of its team wants a guard
    for (int i = 0; i < (int)m_frame.size(); ++i)
        if (armsGuards(m_frame[i].type) && m_frame[i].state->team < GameMaxTeams)
            guardsWanted[m_frame[i].state->team] |= guardWanted(i, (uint8)m_frame[i].state->team);

    m_carrierPassTimer -= deltaSec;
    if (m_carrierPassTimer <= 0.0f)
    {
        m_carrierPassTimer = c_passSeconds;
        carrierHousePass();
    }

    const float loadSize = glm::max(m_settings.carrierLoad, 1.0f);
    const float speed = glm::max(m_settings.carrierSpeed, 0.1f);
    for (size_t k = 0; k < m_walkers.size(); ++k)
    {
        Walker& w = m_walkers[k];
        const int home = structureIndexById(w.homeId);
        const uint8 team = (uint8)m_frame[home].state->team;
        const glm::vec3 here = w.entity ? w.entity->pos : (w.path.empty() ? glm::vec3(0.0f) : w.path.back());
        glm::ivec2 hereCell = w.path.empty() ? cellOf(here) : cellOf(w.path.back()); // waiting: the leg's last cell
        int job = w.state != Walker_Home ? structureIndexById(w.jobId) : -1;
        if (job >= 0 && m_frame[job].state->blueprint)
            job = -1;

        const auto leaveJob = [&]
        {
            if (job >= 0)
            {
                Ref& old = m_frame[job];
                if (w.state >= Walker_GuardToArmory)
                    old.guards -= old.guards > 0 ? 1 : 0;
                else if (old.walkers > 0)
                    --old.walkers;
                if ((w.state == Walker_ToJob || w.state == Walker_Waiting) && old.walkersIdle > 0)
                    --old.walkersIdle;
            }
            job = -1;
            w.jobId = 0;
        };
        // Other work on this road network, from where the walker stands: a load that waits for a walker (tier 2),
        // and with minTier 1 also a producer that has no walker.
        const auto seekJob = [&](int minTier)
        {
            oc::vector<glm::vec3> path;
            const int found = roadSearch(-1, hereCell, [&](int index)
            {
                const int tier = index == job ? 0 : carrierJobTier(index, team);
                return tier >= minTier ? tier : 0;
            }, path);
            if (found < 0 || path.empty())
                return false;
            leaveJob();
            job = found;
            w.jobId = m_frame[found].state->structureId;
            ++m_frame[found].walkers;
            ++m_frame[found].walkersIdle;
            w.path = oc::move(path);
            w.along = 0.0f;
            w.state = Walker_ToJob;
            return true;
        };
        const auto goHome = [&] // false: no road home - the walker is gone
        {
            leaveJob();
            oc::vector<glm::vec3> path;
            if (roadSearch(-1, hereCell, [&](int index) { return index == home ? 2 : 0; }, path) < 0 || path.empty())
                return false;
            w.path = oc::move(path);
            w.along = 0.0f;
            w.state = Walker_Home;
            return true;
        };
        // An alarmed Armory on this road that is short of guards: the walker goes there for a bow.
        const auto seekArmory = [&]
        {
            oc::vector<glm::vec3> path;
            const int found = roadSearch(-1, hereCell, [&](int index) { return guardWanted(index, team) ? 2 : 0; }, path);
            if (found < 0 || path.empty())
                return false;
            leaveJob();
            job = found;
            w.jobId = m_frame[found].state->structureId;
            ++m_frame[found].guards;
            w.path = oc::move(path);
            w.along = 0.0f;
            w.state = Walker_GuardToArmory;
            return true;
        };

        // THE ROAD UNDER IT IS GONE (destroyed to rubble, or removed): the walker moves to a road cell next to it and
        // goes on from there - a standing one stands there, a walking one finds a new way to where it was going (or
        // home). No road cell next to it: the walker is gone, and its house sends a worker again.
        {
            const bool standing = w.state == Walker_Waiting || w.state == Walker_Working || w.state == Walker_Guarding;
            const glm::ivec2 at = standing ? hereCell : cellOf(here);
            float roadY = 0.0f;
            if (!roadAt(at.x, at.y, roadY))
            {
                bool found = false;
                for (const glm::ivec2& n : c_neighbours)
                    if (!found && roadAt(at.x + n.x, at.y + n.y, roadY))
                    {
                        hereCell = at + n;
                        found = true;
                    }
                if (!found)
                {
                    removeWalker(k--);
                    continue;
                }
                const glm::vec3 nextTo(((float)hereCell.x + 0.5f) * GridCellSize, roadY + c_walkHeight, ((float)hereCell.y + 0.5f) * GridCellSize);
                if (standing)
                {
                    w.path.assign(1, nextTo);
                    if (w.entity)
                        w.entity->pos = nextTo;
                }
                else
                {
                    const int dest = w.state == Walker_Delivering ? structureIndexById(w.targetId) : w.state == Walker_Home ? home : job;
                    oc::vector<glm::vec3> path;
                    const bool way = w.state == Walker_GuardToPost ? job >= 0 && guardPost(hereCell, job, k, path)
                        : dest >= 0 && roadSearch(-1, hereCell, [&](int index) { return index == dest ? 2 : 0; }, path) >= 0;
                    if (w.entity) // onto the road cell at once: the next tick must find it on a road
                        w.entity->pos = nextTo;
                    if (way && !path.empty())
                    {
                        w.path = oc::move(path); // (it starts at that cell)
                        w.along = 0.0f;
                    }
                    else
                    {
                        if (w.state == Walker_Home || !goHome())
                            removeWalker(k--);
                        continue;
                    }
                }
            }
        }

        // GUARDING: at its post, it shoots the nearest enemy unit in range every "Guard shot interval (s)" - a hit that
        // never misses, through anything. With nothing to shoot for a while it looks for a better post (the threat
        // moved). The alarm is over, the Armory is gone, or production took its worker: it puts the bow down and
        // takes other work, or goes home.
        if (w.state == Walker_Guarding)
        {
            if (job < 0 || m_frame[job].alarm <= 0.0f || w.dismissed)
            {
                setGuardLook(w, false);
                if (!((!w.dismissed && seekJob(1)) || goHome()))
                    removeWalker(k--);
                continue;
            }
            w.wait -= deltaSec;
            if (w.wait > 0.0f)
                continue;
            w.wait = glm::max(m_settings.guardInterval, 0.1f);
            // ON A WALKWAY DECK it shoots over the walls: anything in range. On any other road cell it needs a CLEAR LINE
            // from its eyes to the unit (nearestEnemyUnit's sightFrom).
            glm::vec3 victimPos;
            const glm::vec3 eyes = here + glm::vec3(0.0f, 1.3f, 0.0f);
            if (GameUnitComponent* victim = nearestEnemyUnit(here, m_settings.guardRange, team, victimPos,
                walkwayDeckAt(hereCell.x, hereCell.y) ? nullptr : &eyes))
            {
                victim->damage(m_settings.guardDamage, team);
                m_guardShots.push_back({ eyes, victimPos });
                if (const glm::vec3 d = victimPos - here; w.entity && d.x * d.x + d.z * d.z > 1e-6f)
                    w.entity->rot = glm::angleAxis(std::atan2(-d.x, -d.z), glm::vec3(0.0f, 1.0f, 0.0f));
                w.idle = 0.0f;
                continue;
            }
            w.idle += w.wait;
            if (w.idle >= c_guardRepost)
            {
                w.idle = 0.0f;
                oc::vector<glm::vec3> path;
                if (guardPost(hereCell, job, k, path) && path.size() > 1)
                {
                    w.path = oc::move(path);
                    w.along = 0.0f;
                    w.state = Walker_GuardToPost;
                }
            }
            continue;
        }

        if (w.state == Walker_Waiting)
        {
            if (w.dismissed || job < 0)
            {
                if (!((!w.dismissed && seekJob(1)) || goHome()))
                    removeWalker(k--);
                continue;
            }
            Ref& jobRef = m_frame[job];
            const int rank = jobRef.waitRank++;
            if (w.entity) // side by side, not inside each other
            {
                w.entity->pos = w.path.back() + glm::vec3((float)(rank % 3) * 0.45f, 0.0f, (float)(rank / 3 % 3) * 0.45f);
                if (const SceneComponent* scene = getComponent<SceneComponent>(w.entity.get()))
                    for (size_t c = 1; c < scene->children.size(); ++c) // no load, no cart
                        scene->children[c]->setEnabled(false);
            }
            w.idle += deltaSec;
            w.wait -= deltaSec;
            if (w.wait > 0.0f)
                continue;
            w.wait = c_retrySeconds;
            if (guardsWanted[team] && seekArmory()) // the alarm: a waiting worker goes for a bow before any load
                continue;
            if (rank == 0) // the first in line loads
            {
                GameStructureComponent& s = *jobRef.state;
                for (int good = 1; good <= 2 && w.state == Walker_Waiting; ++good)
                {
                    bool consumersOnly = false;
                    if (!carrierSendGood(job, good, consumersOnly))
                        continue;
                    oc::vector<glm::vec3> path;
                    const int target = findRoadTarget(job, -1, hereCell, good, consumersOnly, CarrierWork_Goods, path);
                    if (target < 0 || path.empty())
                        continue;
                    w.load = glm::min(glm::min(loadSize, s.store[good]), m_frame[target].state->capacity[good]); // it fits in full (carrierAccepts)
                    s.store[good] -= w.load;
                    m_frame[target].goodsIncoming[good] += w.load;
                    w.good = (uint8)good;
                    w.work = CarrierWork_Goods;
                    w.targetId = m_frame[target].state->structureId;
                    w.path = oc::move(path);
                    w.along = 0.0f;
                    w.idle = 0.0f;
                    w.state = Walker_Delivering;
                    if (jobRef.walkersIdle > 0)
                        --jobRef.walkersIdle;
                }
                // NO OTHER LOAD: BUILD. A load of stone from this building to the nearest blueprint on its road (what
                // that one does not need goes on to the next one, and the rest comes back - see the delivery).
                // THEN, with nothing to build on this road: REPAIR - the same trip to the nearest damaged structure.
                for (const uint8 work : { (uint8)CarrierWork_Build, (uint8)CarrierWork_Repair })
                {
                    if (w.state != Walker_Waiting || !carrierHoldsStone(job))
                        break;
                    oc::vector<glm::vec3> path;
                    const int target = findRoadTarget(job, -1, hereCell, 2, false, work, path);
                    if (target >= 0 && !path.empty())
                    {
                        w.load = glm::min(loadSize, s.store[2]);
                        s.store[2] -= w.load;
                        m_frame[target].buildIncoming += w.load;
                        w.good = 2;
                        w.work = work;
                        w.targetId = m_frame[target].state->structureId;
                        w.path = oc::move(path);
                        w.along = 0.0f;
                        w.idle = 0.0f;
                        w.state = Walker_Delivering;
                        if (jobRef.walkersIdle > 0)
                            --jobRef.walkersIdle;
                    }
                }
                // LAST: TRANSPORT. A cart load to a much emptier storage of the same good on this road (transportAmount).
                for (int good = 1; good <= 2 && w.state == Walker_Waiting; ++good)
                {
                    oc::vector<glm::vec3> path;
                    const int target = findRoadTarget(job, -1, hereCell, good, false, CarrierWork_Transport, path);
                    if (target < 0 || path.empty())
                        continue;
                    w.load = transportAmount(job, target, good);
                    s.store[good] -= w.load;
                    m_frame[target].goodsIncoming[good] += w.load;
                    w.good = (uint8)good;
                    w.work = CarrierWork_Transport;
                    w.targetId = m_frame[target].state->structureId;
                    w.path = oc::move(path);
                    w.along = 0.0f;
                    w.idle = 0.0f;
                    w.state = Walker_Delivering;
                    if (jobRef.walkersIdle > 0)
                        --jobRef.walkersIdle;
                }
                if (w.state != Walker_Waiting)
                    continue;
            }
            // Nothing to carry here. A walker behind another one takes any other job; the first in line leaves only
            // for a load that waits elsewhere, and only after a while. Long idle (and not a producer's own walker): home.
            if (rank > 0 ? seekJob(1) : (w.idle > c_idleLeave && seekJob(2)))
                continue;
            bool producer = false;
            for (int good = 1; good <= 2; ++good)
                producer = producer || (jobRef.state->capacity[good] > 0.0f && transportRoleOf(jobRef.type, good) == ETransportRole::Producer);
            if (w.idle > c_idleHome && (rank > 0 || !producer) && !goHome())
                removeWalker(k--);
            continue;
        }

        // After build / repair work: what is left of the load goes to the NEXT one of the same work from here - a piece
        // of road that was just built is road for that search, so one walker lays a painted road piece by piece - or
        // back to the pickup BY ROAD FROM HERE (after a chain the last leg does not lead there).
        const auto afterWork = [&]
        {
            oc::vector<glm::vec3> path;
            if (w.load > 0.01f)
                if (const int next = findRoadTarget(home, -1, hereCell, 2, false, w.work, path); next >= 0 && !path.empty())
                {
                    m_frame[next].buildIncoming += w.load; // it keeps its whole load: the next one takes what it needs
                    w.targetId = m_frame[next].state->structureId;
                    w.path = oc::move(path);
                    w.along = 0.0f;
                    w.state = Walker_Delivering;
                    return;
                }
            path.clear();
            if (job >= 0 && roadSearch(-1, hereCell, [&](int index) { return index == job ? 2 : 0; }, path) >= 0 && !path.empty())
                w.path = oc::move(path);
            else
                oc::reverse(w.path.begin(), w.path.end());
            w.along = 0.0f;
            w.state = Walker_Returning;
        };

        // WORKING: the walker stands at the structure and its stone goes in over time (health is a blueprint's progress;
        // a repair also has the health rate of investMaterials' cap). It leaves when its load is used up, or when the
        // structure takes no more: built, whole, gone, or rubble with an enemy near.
        if (w.state == Walker_Working)
        {
            const float rate = glm::max(m_settings.carrierBuildRate, 0.01f);
            const float repairCap = m_settings.constructorRepairRate * rate / glm::max(m_settings.constructorBuildRate, 0.01f);
            float spent = 0.0f;
            if (const int target = structureIndexById(w.targetId); target >= 0 && w.load > 0.01f)
                spent = investMaterials(m_frame[target], glm::min(w.load, rate * deltaSec), repairCap * deltaSec);
            w.load -= spent;
            if (spent <= 0.0f || w.load <= 0.01f)
                afterWork();
            continue;
        }

        // Walking: along the leg's polyline at the walk speed.
        w.along += speed * deltaSec;
        float left = w.along;
        glm::vec3 pos = w.path.empty() ? here : w.path.back(), dir(0.0f);
        bool arrived = true;
        for (size_t p = 1; p < w.path.size(); ++p)
        {
            const glm::vec3 seg = w.path[p] - w.path[p - 1];
            const float len = glm::length(seg);
            if (left < len)
            {
                pos = w.path[p - 1] + seg * (left / glm::max(len, 1e-4f));
                dir = seg;
                arrived = false;
                break;
            }
            left -= len;
        }
        if (w.entity)
        {
            w.entity->pos = pos;
            if (dir.x * dir.x + dir.z * dir.z > 1e-6f) // an entity's forward is -Z
                w.entity->rot = glm::angleAxis(std::atan2(-dir.x, -dir.z), glm::vec3(0.0f, 1.0f, 0.0f));
            // carrier.pre's children: [1] the LOAD in its hands, only while it carries one on foot; [2] the cart's load and
            // [3..] the CART, on a transport trip (the cart comes back with it, empty).
            if (const SceneComponent* scene = w.guard ? nullptr : getComponent<SceneComponent>(w.entity.get())) // (guard.pre: all shown)
            {
                const bool cart = w.work == CarrierWork_Transport;
                for (size_t c = 1; c < scene->children.size(); ++c)
                    scene->children[c]->setEnabled(c == 1 ? !cart && w.load > 0.0f : cart && (c > 2 || w.load > 0.0f));
            }
        }
        if (!arrived)
            continue;

        switch (w.state)
        {
        case Walker_ToJob: // at the pickup: it stays (the waiting branch takes over, counted idle already)
            w.state = Walker_Waiting;
            w.wait = 0.0f;
            w.idle = 0.0f;
            break;
        case Walker_Delivering: // at the target: unload what fits, then walk the same road back
        {
            if (w.work == CarrierWork_Build || w.work == CarrierWork_Repair) // stone for a blueprint or a repair: it stays and works (Walker_Working)
            {
                w.state = Walker_Working;
                break;
            }
            if (const int target = structureIndexById(w.targetId); target >= 0)
            {
                if (m_frame[target].state->blueprint)
                {
                    // goods for a building that is rubble now: nothing is unloaded, the load goes back
                }
                else
                {
                    GameStructureComponent& t = *m_frame[target].state;
                    const float give = glm::clamp(t.capacity[w.good] - t.store[w.good], 0.0f, w.load);
                    t.store[w.good] += give;
                    w.load -= give;
                }
            }
            oc::reverse(w.path.begin(), w.path.end());
            w.along = 0.0f;
            w.state = Walker_Returning;
            break;
        }
        case Walker_Returning: // at the pickup again: what the target did not take goes back into the store
            if (job >= 0)
            {
                GameStructureComponent& s = *m_frame[job].state;
                s.store[w.good] = glm::min(s.store[w.good] + w.load, glm::max(s.capacity[w.good], s.store[w.good]));
                ++m_frame[job].walkersIdle;
            }
            w.load = 0.0f;
            w.work = CarrierWork_Goods;
            w.state = Walker_Waiting;
            w.wait = 0.0f;
            w.idle = 0.0f;
            break;
        case Walker_GuardToArmory: // at the Armory: the bow, then the walk to a post (none free: it guards here)
        {
            setGuardLook(w, true);
            oc::vector<glm::vec3> path;
            if (job >= 0 && guardPost(hereCell, job, k, path) && path.size() > 1)
            {
                w.path = oc::move(path);
                w.along = 0.0f;
                w.state = Walker_GuardToPost;
            }
            else
                w.state = Walker_Guarding;
            w.wait = 0.0f;
            w.idle = 0.0f;
            break;
        }
        case Walker_GuardToPost:
            w.state = Walker_Guarding;
            w.wait = 0.0f;
            w.idle = 0.0f;
            break;
        default: // Walker_Home: back in its house
            removeWalker(k--);
            break;
        }
    }
}
