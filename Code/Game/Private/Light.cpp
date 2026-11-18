module Game;

import Core;
import Core.glm;
import Entity;
import Particle; // the mirrors' light glitter (ParticleSystem::emitStream)
import Physics;
import RendererVK;
import :Structures;

// LIGHT ON BEAMS (see Structures.ixx). Everything here is DERIVED from the roster: no link is authored, saved or sent.
// A structure's role is its role in the energy medium (transportRoleOf): a PRODUCER beams into a hub, a CONSUMER is
// fed by a hub, a STORAGE is a hub (the Crystal store, the Beacon).

namespace
{
    constexpr float c_linkPeriod = 1.0f;   // seconds between two derivations (they cast the line-of-sight rays)
    constexpr float c_beamLift = 1.0f;     // a beam ends this far over a building's top
    constexpr float c_baseReserve = 0.25f; // the share of its store the Beacon keeps for its own field
    constexpr float c_searchlightLamp = 3.4f; // the Searchlight's lamp over its origin (the `Head` child of searchlight.pre)
    // m along the cone's axis from the head's centre to the light: the lens face (searchlight.pre's `Lens`, front at 0.375).
    // The lens and the lamp crystal cast no shadow; the housing behind the lens does, so the light starts in front of it.
    constexpr float c_searchlightLampClear = 0.4f;

    bool isLightHub(EStructureType t) { return t == EStructureType::Battery || t == EStructureType::Base || t == EStructureType::Lens; }

    // Where a building's beam starts and ends: the CENTRE OF ITS CRYSTAL, as its prefab places it over the entity's
    // origin (keep these in step with the `Crystal` child of the .pre files). A building with no crystal: a little
    // over its top.
    float beamHeightOf(EStructureType t)
    {
        switch (t)
        {
        case EStructureType::Base:     return 4.8f;
        case EStructureType::Battery:  return 0.9f;
        case EStructureType::Emitter:  return 1.85f;
        case EStructureType::Bastion:  return 3.5f;
        case EStructureType::Turret:   return 3.6f;
        case EStructureType::Solar:    return 1.16f;
        case EStructureType::Barracks: return 3.15f;
        case EStructureType::Lens:     return 2.2f;
        case EStructureType::Searchlight: return c_searchlightLamp;
        case EStructureType::WardResist:
        case EStructureType::WardAbsorb:
        case EStructureType::WardRetaliate: return 2.65f; // the `Crystal` child of the ward prefabs
        default:                       return -1.0f;
        }
    }
}

glm::vec3 StructureSystem::beamPointOf(const Ref& ref) const
{
    const float crystal = beamHeightOf(ref.type);
    return ref.entity->pos + glm::vec3(0.0f, crystal >= 0.0f ? crystal : spawnHeightOf(ref.type) + c_beamLift, 0.0f);
}

// A clear line between the two buildings' beam points: the terrain, the trees and the rocks block it. Buildings and
// units do not (their colliders are on other layers). With no collider in the way (the flat maps) every line is clear.
bool StructureSystem::lightLineClear(const Ref& from, const Ref& to) const
{
    const glm::vec3 a = beamPointOf(from);
    const glm::vec3 b = beamPointOf(to);
    return !Globals::physics.castRayClosest(a, b - a, PhysicsLayers::bit("Terrain") | PhysicsLayers::bit("Vegetation")).hit;
}

void StructureSystem::rebuildLightLinks()
{
    oc::small_vector<int, 32> hubs;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        m_frame[i].lightId = 0;
        m_frame[i].lightBlocked = false;
        if (isLightHub(m_frame[i].type) && !m_frame[i].state->blueprint)
            hubs.push_back(i);
    }
    // ---- THE HUB NETWORKS: a spanning forest (Kruskal) ----
    // Every pair of hubs of one team in reach of each other ("Hub link range"; a pair with a Lens end: "Lens range"),
    // the shortest first; a pair joins two networks that are not joined yet if its line is clear. So every hub that can
    // reach another by a chain of links is in ONE network, with one link less than it has hubs. The nearest-hub links
    // this replaces made islands: two pairs of stores close together each linked inside the pair, and their light never
    // met. A ray is cast only for a pair that would join two networks.
    m_lightNet.clear();
    {
        struct Pair { float dist; int a, b; }; // indices into hubs
        oc::vector<Pair> pairs;
        for (int a = 0; a < (int)hubs.size(); ++a)
            for (int b = a + 1; b < (int)hubs.size(); ++b)
            {
                const Ref& ra = m_frame[hubs[a]];
                const Ref& rb = m_frame[hubs[b]];
                if (ra.state->team != rb.state->team)
                    continue;
                const float range = ra.type == EStructureType::Lens || rb.type == EStructureType::Lens ? m_settings.lensRange
                    : m_settings.lightHubLinkRange;
                const float dist = glm::distance(glm::vec2(ra.entity->pos.x, ra.entity->pos.z), glm::vec2(rb.entity->pos.x, rb.entity->pos.z));
                if (dist <= range)
                    pairs.push_back({ dist, a, b });
            }
        oc::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.dist < y.dist; });
        oc::vector<int> root(hubs.size());
        for (int a = 0; a < (int)hubs.size(); ++a)
            root[a] = a;
        const auto find = [&](int a) { while (root[a] != a) a = root[a] = root[root[a]]; return a; };
        oc::vector<oc::small_vector<int, 4>> adjacent(hubs.size());
        for (const Pair& p : pairs)
        {
            const int ra = find(p.a), rb = find(p.b);
            if (ra == rb)
                continue;
            if (!lightLineClear(m_frame[hubs[p.a]], m_frame[hubs[p.b]]))
            {
                m_frame[hubs[p.a]].lightBlocked = m_frame[hubs[p.b]].lightBlocked = true;
                continue;
            }
            root[ra] = rb;
            adjacent[p.a].push_back(p.b);
            adjacent[p.b].push_back(p.a);
        }
        // Each network as a tree in breadth-first order from its root (the Beacon when it is in it, else its first hub):
        // m_lightNet, and each hub's lightId = its parent.
        oc::vector<int> netIndex(hubs.size(), -1);
        const auto addNetwork = [&](int start)
        {
            const size_t begin = m_lightNet.size();
            netIndex[start] = (int)begin;
            m_lightNet.push_back({ m_frame[hubs[start]].state->structureId, -1 });
            oc::small_vector<int, 32> order{ start };
            for (size_t head = 0; head < order.size(); ++head)
                for (const int next : adjacent[order[head]])
                    if (netIndex[next] < 0)
                    {
                        netIndex[next] = (int)m_lightNet.size();
                        m_lightNet.push_back({ m_frame[hubs[next]].state->structureId, netIndex[order[head]] });
                        Ref& child = m_frame[hubs[next]];
                        child.lightId = m_frame[hubs[order[head]]].state->structureId;
                        child.lightBlocked = false;
                        order.push_back(next);
                    }
            if (m_lightNet.size() > begin + 1)
                m_frame[hubs[start]].lightBlocked = false;
        };
        for (int a = 0; a < (int)hubs.size(); ++a)
            if (m_frame[hubs[a]].type == EStructureType::Base && netIndex[a] < 0)
                addNetwork(a);
        for (int a = 0; a < (int)hubs.size(); ++a)
            if (netIndex[a] < 0)
                addNetwork(a);
    }

    // ---- The producers and the consumers: each to one hub ----
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        if (isLightHub(ref.type)) // (the networks above)
            continue;
        if (ref.state->blueprint || isCableOrCrossing(ref.type) || ref.state->capacity[0] <= 0.0f)
            continue;
        const ETransportRole role = transportRoleOf(ref.type, 0);
        const float range = ref.type == EStructureType::Solar ? m_settings.mirrorBeamRange
            : role == ETransportRole::Storage ? m_settings.lightHubLinkRange : m_settings.lightHubReach;
        // The nearest USEFUL hub in range with a clear line, else the nearest one. Useful: for a CONSUMER a hub that
        // has light to give (over the Beacon's reserve), for a PRODUCER one with room. The nearest hub alone left a
        // turret or a barracks dark next to a full Crystal store whenever an empty hub stood nearer (the Beacon down
        // at its reserve, a lens with nothing behind it), and a mirror beaming into a full store. The links are
        // derived again about once a second, so a link follows the light. Two hubs: the nearest. The ray is cast only
        // for a hub that can be better than the best so far.
        const auto useful = [&](const Ref& hub)
        {
            const GameStructureComponent& hs = *hub.state;
            if (role == ETransportRole::Consumer)
                return hs.store[0] - (hub.type == EStructureType::Base ? hs.capacity[0] * c_baseReserve : 0.0f) > 1.0f;
            if (role == ETransportRole::Producer)
                return hs.capacity[0] - hs.store[0] > 1.0f;
            return true;
        };
        int best = -1, bestUseful = -1;
        float bestDist = range, bestUsefulDist = range;
        for (const int h : hubs)
        {
            const Ref& hub = m_frame[h];
            if (h == i || hub.state->team != ref.state->team)
                continue;
            const float dist = glm::distance(glm::vec2(hub.entity->pos.x, hub.entity->pos.z), glm::vec2(ref.entity->pos.x, ref.entity->pos.z));
            // A link with a lens at an end is no longer than the lens reaches, whatever the other end's own range is.
            if (hub.type == EStructureType::Lens && dist > m_settings.lensRange)
                continue;
            const bool good = useful(hub);
            if (dist > bestDist && !(good && dist <= bestUsefulDist))
                continue;
            if (!lightLineClear(ref, hub))
            {
                ref.lightBlocked = true;
                continue;
            }
            if (dist <= bestDist)
            {
                best = h;
                bestDist = dist;
            }
            if (good && dist <= bestUsefulDist)
            {
                bestUseful = h;
                bestUsefulDist = dist;
            }
        }
        if (bestUseful >= 0)
            best = bestUseful;
        if (best >= 0)
        {
            ref.lightId = m_frame[best].state->structureId;
            ref.lightBlocked = false;
        }
    }
    m_lightLinkCount = m_frame.size();
}

// The flow along the links (authority). Three moves, each bounded by a rate: each hub network levels as one pool
// (levelLightNet); a producer empties its buffer into its hub; a consumer fills from its hub (the barracks and the turret
// at their METERED intake: their store is their build / reload bar).
void StructureSystem::tickLight(float deltaSec)
{
    if (!m_settings.lightBeams)
        return;
    m_lightLinkTimer -= deltaSec;
    if (m_lightLinkTimer <= 0.0f || m_lightLinkCount != m_frame.size())
    {
        m_lightLinkTimer = c_linkPeriod;
        rebuildLightLinks();
    }
    // THE HUB NETWORKS first: each levels as one pool (each network starts at its root, parent -1).
    m_lightPoolMoved.assign(m_frame.size(), 0.0f);
    for (size_t begin = 0; begin < m_lightNet.size();)
    {
        size_t end = begin + 1;
        while (end < m_lightNet.size() && m_lightNet[end].parent >= 0)
            ++end;
        if (end - begin > 1)
            levelLightNet(begin, end, deltaSec);
        begin = end;
    }
    const GameStructureParams& params = Globals::settings.game.structureParams;
    const float smooth = glm::min(deltaSec * 3.0f, 1.0f); // the flow gauge: ~1/3 s, so a beam does not blink per tick
    for (Ref& ref : m_frame)
    {
        float moved = 0.0f;
        // The gauge is written at the end of this iteration, also for a link that moved nothing (it decays to 0).
        struct Gauge
        {
            Ref& ref; float& moved; float deltaSec, smooth;
            ~Gauge() { ref.lightFlow += ((deltaSec > 1e-6f ? moved / deltaSec : 0.0f) - ref.lightFlow) * smooth; }
        } gauge{ ref, moved, deltaSec, smooth };
        if (ref.lightId == 0 || ref.state->blueprint)
            continue;
        const int hubIndex = structureIndexById(ref.lightId);
        if (hubIndex < 0 || m_frame[hubIndex].state->blueprint)
            continue; // the hub died since the last derivation
        GameStructureComponent& s = *ref.state;
        GameStructureComponent& hub = *m_frame[hubIndex].state;
        const ETransportRole role = transportRoleOf(ref.type, 0);
        if (role == ETransportRole::Producer)
        {
            const float give = glm::min(glm::min(s.store[0], m_settings.lightBeamRate * deltaSec), glm::max(hub.capacity[0] - hub.store[0], 0.0f));
            s.store[0] -= give;
            hub.store[0] += give;
            moved = give;
        }
        else if (role == ETransportRole::Consumer)
        {
            const float rate = isBarracksType(ref.type) ? m_settings.barracksEnergyIntake
                : ref.type == EStructureType::Turret ? params.turretShotEnergy / glm::max(params.turretFireInterval, 1e-3f)
                : isWardType(ref.type) ? glm::min(m_settings.wardIntake, m_settings.lightFeedRate) // the ward's refill limit
                : m_settings.lightFeedRate;
            // The Beacon keeps a reserve for its own field: its consumers cannot drain it dark.
            const float reserve = m_frame[hubIndex].type == EStructureType::Base ? hub.capacity[0] * c_baseReserve : 0.0f;
            const float take = glm::min(glm::min(glm::max(s.capacity[0] - s.store[0], 0.0f), rate * deltaSec), glm::max(hub.store[0] - reserve, 0.0f));
            hub.store[0] -= take;
            s.store[0] += take;
            moved = take;
        }
        else if (isLightHub(ref.type)) // a tree link of a hub network: what the pool moved over it above
            moved = m_lightPoolMoved[(size_t)(&ref - m_frame.data())];
    }
}

void StructureSystem::levelLightNet(size_t begin, size_t end, float deltaSec)
{
    struct Node { int index; float free, locked, want, move; int rank; };
    oc::small_vector<Node, 32> nodes;
    float capacity[3] = {}, pool = 0.0f;
    for (size_t k = begin; k < end; ++k)
    {
        const int index = structureIndexById(m_lightNet[k].id);
        if (index < 0 || m_frame[index].state->blueprint)
            return; // a hub died since the last derivation: the next one (within a second) leaves it out
        const Ref& ref = m_frame[index];
        const GameStructureComponent& s = *ref.state;
        // The Beacon's RESERVE is its own: locked out of the pool (as much of it as it holds), and its capacity in the
        // pool is what lies over it.
        const float reserve = ref.type == EStructureType::Base ? s.capacity[0] * c_baseReserve : 0.0f;
        Node node{ index, glm::max(s.capacity[0] - reserve, 0.0f), glm::min(s.store[0], reserve), 0.0f, 0.0f, storeRank(ref) };
        capacity[node.rank] += node.free;
        pool += s.store[0] - node.locked;
        nodes.push_back(node);
    }
    // THE SHARES: the pool fills the Fill hubs first, then the Balance hubs, then the Drain hubs - within a rank all to
    // the same fill fraction.
    float fraction[3] = {};
    for (int rank = 2; rank >= 0; --rank)
    {
        fraction[rank] = capacity[rank] > 0.0f ? glm::clamp(pool / capacity[rank], 0.0f, 1.0f) : 0.0f;
        pool = glm::max(pool - fraction[rank] * capacity[rank], 0.0f);
    }
    // THE MOVE: each hub toward its share at its own rate (a Lens at "Lens rate": a relay keeps up with the producers
    // and the consumers on it - its store is all they get, so its rate is their total). What the takers can take and
    // what the givers can give is scaled to the same total, so no light is made or lost.
    float take = 0.0f, give = 0.0f;
    for (Node& node : nodes)
    {
        const Ref& ref = m_frame[node.index];
        const float rate = (ref.type == EStructureType::Lens ? m_settings.lensRate : m_settings.lightShareRate) * deltaSec;
        node.want = glm::clamp(node.locked + fraction[node.rank] * node.free - ref.state->store[0], -rate, rate);
        (node.want > 0.0f ? take : give) += glm::abs(node.want);
    }
    const float total = glm::min(take, give);
    if (total <= 1e-6f)
        return;
    for (Node& node : nodes)
    {
        node.move = node.want * total / (node.want > 0.0f ? take : give);
        m_frame[node.index].state->store[0] += node.move;
    }
    // THE BEAMS: the light on a tree link is what the child's whole subtree took or gave (children come after their
    // parent, so one pass from the back sums the subtrees).
    for (size_t k = end - 1; k > begin; --k)
    {
        const Node& node = nodes[k - begin];
        m_lightPoolMoved[(size_t)node.index] = glm::abs(node.move);
        nodes[(size_t)m_lightNet[k].parent - begin].move += node.move;
    }
}

// The shining searchlights, each frame on every instance: their cones for the units (GameUnitParams::lightCones - a unit
// in one is solid and burns, GameUnitComponent::setSolid) and a SpotLight each. The unit test is FLAT (a wedge on the
// map), so the ground's height under the cone does not count; the drawn light leans down a little to reach the ground.
void StructureSystem::publishSearchlights() const
{
    const GameStructureParams& params = Globals::settings.game.structureParams;
    GameUnitParams& unit = Globals::settings.game.unitParams;
    const float halfAngle = glm::radians(glm::clamp(params.searchlightHalfAngleDeg, 1.0f, 85.0f));
    const float cosHalf = std::cos(halfAngle);
    int count = 0;
    for (const Ref& ref : m_frame)
    {
        if (ref.type != EStructureType::Searchlight || ref.state->blueprint
            || ref.state->machineKind != GameStructureComponent::EMachineKind::Searchlight || !ref.state->searchlight.lit)
            continue;
        const float yaw = ref.state->searchlight.yaw;
        const glm::vec2 dir(std::cos(yaw), std::sin(yaw));
        const glm::vec3 lamp = ref.entity->pos + glm::vec3(0.0f, c_searchlightLamp, 0.0f);
        if (count < GameUnitParams::MaxLightCones)
        {
            float* cone = unit.lightCones[count++];
            cone[0] = lamp.x;
            cone[1] = lamp.z;
            cone[2] = params.searchlightRange;
            cone[3] = dir.x;
            cone[4] = dir.y;
            cone[5] = cosHalf;
            cone[6] = (float)ref.state->team;
            cone[7] = params.searchlightDps;
            cone[10] = 0.0f; // a flat wedge
        }
        // The lamp stands ~5.4 m over the ground: the axis leans down so the cone's far end is on the ground.
        const float lean = (spawnHeightOf(ref.type) + c_searchlightLamp) / glm::max(params.searchlightRange, 1.0f);
        const glm::vec3 axis = glm::normalize(glm::vec3(dir.x, -lean, dir.y));
        const glm::vec3 color(1.0f, 0.93f, 0.75f);
        // The light starts AT THE LENS, not at the head's centre: from inside the housing every shadow ray hits it and the
        // cone is dark.
        const glm::vec3 source = lamp + axis * c_searchlightLampClear;
        // "Searchlight brightness" is for a 26 m range; the light falls off with the square of the distance, so a longer
        // reach gets that much more (its far end stays as bright).
        const float reach = glm::max(params.searchlightRange, 1.0f) / 26.0f;
        Globals::rendererVK.addLightInfo(SpotLight(source, params.searchlightRange * 1.15f, color, params.searchlightIntensity * reach * reach,
            axis, halfAngle, 0.15f));
        // (No beam column for a shaft in the air: a column is a cylinder, and it did not read as this cone.)
    }
    // THE MAGES (walkers of a Mage tower, the authority only): a small Searchlight cone each, from the staff, while it has
    // a target (Walker::lit, aimed in tickCarriers). The same flat wedge for the units, a weaker light.
    const float mageHalf = glm::radians(glm::clamp(m_settings.mageHalfAngleDeg, 1.0f, 85.0f));
    const float mageCos = std::cos(mageHalf);
    const glm::vec3 mageColor(0.7f, 0.8f, 1.0f);
    for (const Walker& w : m_walkers)
    {
        if (!w.lit || !w.entity)
            continue;
        const glm::vec2 dir(std::cos(w.yaw), std::sin(w.yaw));
        const glm::vec3 staff = w.entity->pos + glm::vec3(0.0f, 1.4f, 0.0f);
        if (count < GameUnitParams::MaxLightCones)
        {
            float* cone = unit.lightCones[count++];
            cone[0] = staff.x;
            cone[1] = staff.z;
            cone[2] = m_settings.mageRange;
            cone[3] = dir.x;
            cone[4] = dir.y;
            cone[5] = mageCos;
            cone[6] = (float)w.team;
            cone[7] = m_settings.mageDps;
            cone[10] = 0.0f; // a flat wedge
        }
        const float lean = 1.4f / glm::max(m_settings.mageRange, 1.0f);
        const glm::vec3 axis = glm::normalize(glm::vec3(dir.x, -lean, dir.y));
        Globals::rendererVK.addLightInfo(SpotLight(staff + axis * 0.3f, m_settings.mageRange * 1.15f, mageColor,
            params.searchlightIntensity * m_settings.mageLightIntensity, axis, mageHalf, 0.15f));
    }
    unit.lightConeCount = count;
}

// The beams: a link shows ONLY WHILE IT MOVES LIGHT (a producer with light in its buffer, a consumer that is not
// full, two hubs that are not level), as a beam between the two crystals - the Beacon's beam in small: a glowing
// column in the air that lights what stands under it. No line is drawn.
// EVERY moving link's glow is a BEAM CARD (Renderer::addBeamCard): the fog apply's column glow on a quad around the
// beam, which costs only the pixels it covers (up to c_maxBeamCards a frame). THE LIGHT ON SURFACES costs more, so
// only the "Lit beams" best links cast it (a BeamLight with no air column): by the flow, the length and the
// distance to the camera, with a bonus for the lit ones so a link at the edge of the list does not flicker. Each link
// fades its light in and out (Ref::beamLit, ~0.5 s); the glow in the air never changes, so no switch shows.
void StructureSystem::drawLightLinks(float deltaSec) const
{
    if (!m_settings.lightBeams)
        return;
    constexpr int c_maxBeamCards = 256; // RendererVK's BeamCardPipeline::MAX_CARDS
    constexpr float c_litFade = 2.0f;   // 1/s: the light on surfaces fades in / out over 0.5 s
    constexpr float c_litBonus = 1.3f;  // x the score of a link that is lit now
    m_beamScratch.clear();
    const glm::vec3 eye = Globals::rendererVK.cameraPos();
    const glm::vec3 gold(1.0f, 0.85f, 0.4f);
    for (const Ref& ref : m_frame)
    {
        const float wasLit = ref.beamLit;
        ref.beamLit = 0.0f; // set again below if the link still moves light
        if (ref.lightId == 0)
            continue;
        const int hubIndex = structureIndexById(ref.lightId);
        if (hubIndex < 0)
            continue;
        const Ref& hub = m_frame[hubIndex];
        const glm::vec3 a = beamPointOf(ref);
        const glm::vec3 b = beamPointOf(hub);
        // The link's measured flow (Ref::lightFlow): a store that is topped up every tick never LOOKS busy, so the
        // stores themselves cannot say whether light moves. Brighter with more light, from a floor.
        if (ref.lightFlow < 0.02f)
            continue;
        const glm::vec3 mid = (a + b) * 0.5f - eye;
        // The beam follows the AMOUNT of light: its glow is the flow over "Beam full flow (light/s)", up to 1.
        const float power = glm::clamp(ref.lightFlow / glm::max(m_settings.lightBeamFullFlow, 0.1f), 0.0f, 1.0f);
        // A MIRROR's beam is LIGHT GLITTER, not a beam: golden sparks that fly from the mirror to the crystal
        // (Effects/light_glitter.pfx through ParticleSystem::emitStream), more of them with more light. A field of
        // mirrors is dozens of small beams; a stream moves a few emitter slots from beam to beam.
        if (ref.type == EStructureType::Solar)
        {
            constexpr float c_mirrorGlitterRange = 400.0f; // m from the camera
            if (glm::dot(mid, mid) < c_mirrorGlitterRange * c_mirrorGlitterRange && m_settings.mirrorGlitterRate > 0.0f)
                Globals::particleSystem.emitStream("Effects/light_glitter.pfx", a, b,
                    m_settings.mirrorGlitterRate * glm::clamp(ref.lightFlow / glm::max(m_settings.solarEnergyPerSec, 0.01f), 0.0f, 1.0f));
            continue;
        }
        const glm::vec3 axis = b - a;
        const float length = glm::length(axis);
        if (length < 0.5f)
            continue;
        // THE SCORE: brighter, longer and nearer links first (the nearest point of the beam, not its middle).
        const float along = glm::clamp(glm::dot(eye - a, axis) / (length * length), 0.0f, 1.0f);
        const float dist = glm::max(glm::length(a + axis * along - eye), 5.0f);
        float score = (0.25f + power) * (1.0f + length / 20.0f) / dist;
        if (wasLit > 0.5f)
            score *= c_litBonus;
        m_beamScratch.push_back(BeamCandidate{ &ref, a, b, power, score, wasLit });
    }
    oc::sort(m_beamScratch.begin(), m_beamScratch.end(), [](const BeamCandidate& x, const BeamCandidate& y) { return x.score > y.score; });
    const int litCount = glm::max(m_settings.lightBeamLitCount, 0);
    const float step = c_litFade * glm::max(deltaSec, 0.0f);
    const int cards = glm::min((int)m_beamScratch.size(), c_maxBeamCards);
    for (int k = 0; k < (int)m_beamScratch.size(); ++k)
    {
        const BeamCandidate& c = m_beamScratch[k];
        const float lit = glm::clamp(c.wasLit + (k < litCount ? step : -step), 0.0f, 1.0f);
        c.ref->beamLit = lit;
        if (k >= cards)
            continue; // past the card budget: no glow, no light (the farthest, dimmest links)
        const glm::vec3 axis = c.b - c.a;
        const BeamLight beam((c.a + c.b) * 0.5f, glm::max(m_settings.lightBeamGlowRange, 0.5f), gold, m_settings.lightBeamGlow * c.power, axis,
            glm::max(m_settings.lightBeamRadius * (0.5f + 0.5f * c.power), 0.02f), glm::length(axis)); // a weak flow is thinner too
        Globals::rendererVK.addBeamCard(beam);
        if (lit > 0.01f)
        {
            BeamLight light = beam;
            light.color *= lit;
            Globals::rendererVK.addLightInfo(light, /*airColumn*/ false); // the card draws its glow
        }
    }
}
