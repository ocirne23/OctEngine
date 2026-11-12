module Game;

import Core;
import Core.glm;
import Entity;
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
    constexpr float c_searchlightLamp = 3.4f; // the Searchlight's lamp over its origin (the `Lamp` child of searchlight.pre)
    constexpr float c_searchlightLampClear = 1.0f; // m along the cone's axis from the lamp's centre to the light: clear of the gem

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
    // Two passes: everything but the lenses, then THE LENSES. A lens is a RELAY: a hub with a small store that links
    // to the nearest hub within "Lens range" - but not to one that links to IT already (that pair is joined). Each
    // structure has one link, so without this rule a lens between a store and the next lens would link back to the
    // store that links to it, and the chain would end there. The lenses come last, so they see the others' links;
    // a later lens also sees the earlier lenses' links.
    for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        Ref& ref = m_frame[i];
        const bool lens = ref.type == EStructureType::Lens;
        if (lens != (pass == 1))
            continue;
        if (ref.state->blueprint || isCableOrCrossing(ref.type) || ref.state->capacity[0] <= 0.0f)
            continue;
        const ETransportRole role = transportRoleOf(ref.type, 0);
        const float range = lens ? m_settings.lensRange : ref.type == EStructureType::Solar ? m_settings.mirrorBeamRange
            : role == ETransportRole::Storage ? m_settings.lightHubLinkRange : m_settings.lightHubReach;
        const uint32 ownId = ref.state->structureId;
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
            if (h == i || hub.state->team != ref.state->team || (lens && hub.lightId == ownId))
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

// The flow along the links (authority). Three moves, each bounded by a rate: a producer empties its buffer into its
// hub; a consumer fills from its hub (the barracks and the turret at their METERED intake: their store is their build
// / reload bar); two linked hubs level their fill fractions.
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
                : m_settings.lightFeedRate;
            // The Beacon keeps a reserve for its own field: its consumers cannot drain it dark.
            const float reserve = m_frame[hubIndex].type == EStructureType::Base ? hub.capacity[0] * c_baseReserve : 0.0f;
            const float take = glm::min(glm::min(glm::max(s.capacity[0] - s.store[0], 0.0f), rate * deltaSec), glm::max(hub.store[0] - reserve, 0.0f));
            hub.store[0] -= take;
            s.store[0] += take;
            moved = take;
        }
        else if (s.capacity[0] > 0.0f && hub.capacity[0] > 0.0f) // two hubs: toward equal fill fractions
        {
            const float diff = hub.store[0] / hub.capacity[0] - s.store[0] / s.capacity[0]; // > 0: the other hub is fuller
            // A link with a Lens at an end is a BEAM: it moves light at the beam rate (a relay must keep up with what the
            // producers send into it).
            const float rate = (ref.type == EStructureType::Lens || m_frame[hubIndex].type == EStructureType::Lens
                ? m_settings.lightBeamRate : m_settings.lightShareRate) * deltaSec;
            const float move = glm::clamp(diff * glm::min(s.capacity[0], hub.capacity[0]) * 0.5f, -rate, rate);
            hub.store[0] -= move;
            s.store[0] += move;
            moved = glm::abs(move);
        }
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
        }
        // The lamp stands ~5.4 m over the ground: the axis leans down so the cone's far end is on the ground.
        const float lean = (spawnHeightOf(ref.type) + c_searchlightLamp) / glm::max(params.searchlightRange, 1.0f);
        const glm::vec3 axis = glm::normalize(glm::vec3(dir.x, -lean, dir.y));
        const glm::vec3 color(1.0f, 0.93f, 0.75f);
        // The light starts IN FRONT of the lamp gem, not at its centre: from inside the mesh every shadow ray hits the
        // gem itself and the cone is dark.
        const glm::vec3 source = lamp + axis * c_searchlightLampClear;
        Globals::rendererVK.addLightInfo(SpotLight(source, params.searchlightRange * 1.15f, color, params.searchlightIntensity, axis, halfAngle, 0.15f));
        // (No beam column for a shaft in the air: a column is a cylinder, and it did not read as this cone.)
    }
    unit.lightConeCount = count;
    unit.lightConeDps = params.searchlightDps;
}

// The beams: a link shows ONLY WHILE IT MOVES LIGHT (a producer with light in its buffer, a consumer that is not
// full, two hubs that are not level), as a BeamLight between the two crystals - the Beacon's beam in small: a glowing
// column in the air that lights what stands under it. No line is drawn. The renderer draws a column for
// RendererState::MAX_BEAM_COLUMNS BeamLights a frame (the Beacon's own takes one at night), so the c_linkBeams
// moving links NEAREST the camera get one. The fog apply draws the columns: with the fog off no beam shows.
void StructureSystem::drawLightLinks() const
{
    if (!m_settings.lightBeams)
        return;
    constexpr int c_linkBeams = 14;
    struct Beam { glm::vec3 a, b; float distSq, power; };
    Beam nearest[c_linkBeams];
    int count = 0;
    const glm::vec3 eye = Globals::rendererVK.cameraPos();
    const glm::vec3 gold(1.0f, 0.85f, 0.4f);
    for (const Ref& ref : m_frame)
    {
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
        // Keep the c_linkBeams nearest (by the link's middle): replace the farthest kept one.
        const glm::vec3 mid = (a + b) * 0.5f - eye;
        // The beam follows the AMOUNT of light: its glow is the flow over "Beam full flow (light/s)", up to 1.
        const Beam beam{ a, b, glm::dot(mid, mid), glm::clamp(ref.lightFlow / glm::max(m_settings.lightBeamFullFlow, 0.1f), 0.0f, 1.0f) };
        if (count < c_linkBeams)
            nearest[count++] = beam;
        else
        {
            int farthest = 0;
            for (int k = 1; k < c_linkBeams; ++k)
                if (nearest[k].distSq > nearest[farthest].distSq)
                    farthest = k;
            if (beam.distSq < nearest[farthest].distSq)
                nearest[farthest] = beam;
        }
    }
    for (int k = 0; k < count; ++k)
    {
        const glm::vec3 axis = nearest[k].b - nearest[k].a;
        const float length = glm::length(axis);
        if (length < 0.5f)
            continue;
        Globals::rendererVK.addLightInfo(BeamLight((nearest[k].a + nearest[k].b) * 0.5f, glm::max(m_settings.lightBeamGlowRange, 0.5f), gold,
            m_settings.lightBeamGlow * nearest[k].power, axis,
            glm::max(m_settings.lightBeamRadius * (0.5f + 0.5f * nearest[k].power), 0.02f), length)); // a weak flow is thinner too
    }
}
