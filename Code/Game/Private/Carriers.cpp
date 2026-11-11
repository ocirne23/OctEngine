module Game;

import Core;
import Core.glm;
import Core.Transform;
import Entity;
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
    };

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
        if (type == EStructureType::CableConveyor || type == EStructureType::CrossingConveyor)
        {
            outY = m_frame[index].entity->pos.y;
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
bool StructureSystem::carrierAccepts(int sourceIdx, int targetIdx, int good, bool consumersOnly, bool& isConsumer) const
{
    if (targetIdx == sourceIdx)
        return false;
    const Ref& target = m_frame[targetIdx];
    const GameStructureComponent& t = *target.state;
    if (t.blueprint || t.team != m_frame[sourceIdx].state->team || isCableOrCrossing(target.type) || t.capacity[good] <= 0.0f)
        return false;
    const ETransportRole role = transportRoleOf(target.type, good);
    if (role == ETransportRole::Producer)
        return false;
    isConsumer = role == ETransportRole::Consumer;
    if (consumersOnly)
        return isConsumer && t.store[good] < t.capacity[good] * 0.5f;
    return t.capacity[good] - t.store[good] >= 1.0f;
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

int StructureSystem::findRoadTarget(int sourceIdx, int fromStructure, glm::ivec2 fromCell, int good, bool consumersOnly,
    oc::vector<glm::vec3>& outPath) const
{
    return roadSearch(fromStructure, fromCell, [&](int index)
    {
        bool isConsumer = false;
        if (!carrierAccepts(sourceIdx, index, good, consumersOnly, isConsumer))
            return 0;
        return isConsumer ? 2 : 1;
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

int StructureSystem::carrierJobTier(int index, uint8 team) const
{
    const Ref& ref = m_frame[index];
    const GameStructureComponent& s = *ref.state;
    if (s.blueprint || s.team != team || isCableOrCrossing(ref.type) || (int)ref.walkers >= glm::max(m_settings.carriersPerBuilding, 1))
        return 0;
    if (ref.sendReady && ref.walkersIdle == 0)
        return 2;
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
    oc::vector<glm::vec3> scratch;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        ref.sendReady = false;
        if (isCableOrCrossing(ref.type) || ref.state->blueprint)
            continue;
        for (int good = 1; good <= 2 && !ref.sendReady; ++good)
        {
            bool consumersOnly = false;
            if (carrierSendGood(i, good, consumersOnly))
                ref.sendReady = !consumersOnly || findRoadTarget(i, i, glm::ivec2(0), good, true, scratch) >= 0;
        }
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
        const int job = roadSearch(i, glm::ivec2(0), [&](int index) { return carrierJobTier(index, team); }, w.path);
        if (job < 0 || w.path.empty())
            continue;
        w.entity = Globals::world.spawnAssetFile(c_carrierPrefab, Transform(w.path.front()), true);
        if (!w.entity)
            continue;
        Globals::world.addRootEntity(w.entity);
        w.homeId = homeId;
        w.jobId = m_frame[job].state->structureId;
        w.state = Walker_ToJob;
        ++m_frame[job].walkers;
        ++m_frame[job].walkersIdle;
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
        ref.walkers = ref.walkersIdle = ref.waitRank = ref.homeWalkers = 0;
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
            ++m_frame[job].walkers;
            if (w.state == Walker_ToJob || w.state == Walker_Waiting)
                ++m_frame[job].walkersIdle;
        }
    }
    if (!m_settings.carriers)
        return;

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
        const glm::ivec2 hereCell = w.path.empty() ? cellOf(here) : cellOf(w.path.back()); // waiting: the leg's last cell
        int job = w.state != Walker_Home ? structureIndexById(w.jobId) : -1;
        if (job >= 0 && m_frame[job].state->blueprint)
            job = -1;

        const auto leaveJob = [&]
        {
            if (job >= 0)
            {
                Ref& old = m_frame[job];
                if (old.walkers > 0)
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
                if (const SceneComponent* scene = getComponent<SceneComponent>(w.entity.get()); scene && scene->children.size() > 1)
                    scene->children[1]->setEnabled(false);
            }
            w.idle += deltaSec;
            w.wait -= deltaSec;
            if (w.wait > 0.0f)
                continue;
            w.wait = c_retrySeconds;
            if (rank == 0) // the first in line loads
            {
                GameStructureComponent& s = *jobRef.state;
                for (int good = 1; good <= 2 && w.state == Walker_Waiting; ++good)
                {
                    bool consumersOnly = false;
                    if (!carrierSendGood(job, good, consumersOnly))
                        continue;
                    oc::vector<glm::vec3> path;
                    const int target = findRoadTarget(job, -1, hereCell, good, consumersOnly, path);
                    if (target < 0 || path.empty())
                        continue;
                    w.load = glm::min(loadSize, s.store[good]);
                    s.store[good] -= w.load;
                    w.good = (uint8)good;
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
            // The LOAD (carrier.pre's second child) shows only while the walker carries one.
            if (const SceneComponent* scene = getComponent<SceneComponent>(w.entity.get()); scene && scene->children.size() > 1)
                scene->children[1]->setEnabled(w.load > 0.0f);
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
            if (const int target = structureIndexById(w.targetId); target >= 0 && !m_frame[target].state->blueprint)
            {
                GameStructureComponent& t = *m_frame[target].state;
                const float give = glm::clamp(t.capacity[w.good] - t.store[w.good], 0.0f, w.load);
                t.store[w.good] += give;
                w.load -= give;
            }
            oc::reverse(w.path.begin(), w.path.end());
            w.along = 0.0f;
            w.state = Walker_Returning;
            break;
        case Walker_Returning: // at the pickup again: what the target did not take goes back into the store
            if (job >= 0)
            {
                GameStructureComponent& s = *m_frame[job].state;
                s.store[w.good] = glm::min(s.store[w.good] + w.load, glm::max(s.capacity[w.good], s.store[w.good]));
                ++m_frame[job].walkersIdle;
            }
            w.load = 0.0f;
            w.state = Walker_Waiting;
            w.wait = 0.0f;
            w.idle = 0.0f;
            break;
        default: // Walker_Home: back in its house
            removeWalker(k--);
            break;
        }
    }
}
