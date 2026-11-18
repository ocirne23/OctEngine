module Game;

import Core;
import Core.glm;
import Core.Transform;
import Entity;
import :Structures;

// THE WARDS (see Structures.ixx and "The wards" in Docs/Context/Game/Economy.md). A ward is a 1x1 pylon with a light
// store. It covers the wall and gate segments connected to it, and each covered segment's GameStructureComponent does
// the work in damage() (the union's `wall` variant): Resist takes a share off every hit, the Absorb shield takes what is
// left first, Retaliate sends a share of every bite back to the biter. This file derives the links, pays the wards and
// shows the pieces on the walls.

namespace
{
    constexpr float c_linkPeriod = 0.5f;      // s between two link derivations
    constexpr float c_lookPeriod = 0.25f;     // s between two looks at the pieces
    constexpr float c_activeCharge = 0.01f;   // a ward with less light than this does nothing
    constexpr float c_drainedCharge = 1.0f;   // light: a ward under this is EMPTY and latches off (a client sees the
                                              // mirrored store, which a refill lifts past 0.01 within a tick)
    constexpr float c_absorbShown = 1.0f;     // health: an Absorb shield under this shows no pane

    // The four sides of a cell, in updateArms' order (+X, -X, +Z, -Z), and the turn about +Y that takes a piece
    // modelled on the +X side (Models/wards.obj) to each. A turn by a maps (x, z) to (x cos a + z sin a, z cos a - x sin a).
    constexpr glm::ivec2 c_sides[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    constexpr float c_sideTurn[4] = { 0.0f, glm::pi<float>(), -glm::half_pi<float>(), glm::half_pi<float>() };
    // The four outer corners as pairs of sides, and the turn that takes the corner post (modelled at +X +Z) to each.
    constexpr int c_cornerSides[4][2] = { { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 } };
    constexpr float c_cornerTurn[4] = { 0.0f, glm::half_pi<float>(), -glm::half_pi<float>(), glm::pi<float>() };

    constexpr const char* c_resistPane = "Entities/Game/wardResistPane.pre";
    constexpr const char* c_resistCorner = "Entities/Game/wardResistCorner.pre";
    constexpr const char* c_absorbPane = "Entities/Game/wardAbsorbPane.pre";
    constexpr const char* c_spikes = "Entities/Game/wardSpikes.pre";

    glm::ivec2 cellOf(const glm::vec3& pos) // a 1x1 structure's cell
    {
        return glm::ivec2((int)std::lround(pos.x / StructureSystem::GridCellSize - 0.5f),
                          (int)std::lround(pos.z / StructureSystem::GridCellSize - 0.5f));
    }
}

void StructureSystem::tickWardLinks(float deltaSec, bool authority)
{
    updateWardLatches();
    m_wardLinkTimer -= deltaSec;
    if (m_wardLinkTimer <= 0.0f || m_wardLinkCount != m_frame.size())
    {
        m_wardLinkTimer = c_linkPeriod;
        rebuildWardLinks();
        m_wardLookTimer = 0.0f; // the pieces follow the new links at once
    }
    m_wardLookTimer -= deltaSec;
    if (m_wardLookTimer <= 0.0f)
    {
        m_wardLookTimer = c_lookPeriod;
        updateWardLooks(authority);
    }
}

bool StructureSystem::wardActive(uint32 wardId) const
{
    if (wardId == 0)
        return false;
    const int index = structureIndexById(wardId);
    if (index < 0)
        return false;
    const Ref& ward = m_frame[index];
    return isWardType(ward.type) && !ward.state->blueprint && !ward.wardDrained && ward.state->store[0] > c_activeCharge;
}

// THE LATCH (every instance, every tick): a ward whose store runs empty switches off, pieces and all, and stays off
// until its store is back at "Restart at" of its capacity, so a ward that is drained faster than it refills does not
// flicker on and off at the refill limit. A new ward starts empty, so it charges up first.
void StructureSystem::updateWardLatches()
{
    const float restart = glm::clamp(m_settings.wardRestart, 0.0f, 1.0f) * m_settings.wardCapacity;
    for (Ref& r : m_frame)
    {
        if (!isWardType(r.type))
            continue;
        const float stored = r.state->store[0];
        if (stored < c_drainedCharge)
            r.wardDrained = true;
        else if (r.wardDrained && stored >= glm::max(restart, c_drainedCharge))
            r.wardDrained = false;
    }
}

void StructureSystem::rebuildWardLinks()
{
    m_wardLinkCount = m_frame.size();
    for (Ref& r : m_frame)
    {
        for (uint32& id : r.wardOf)
            id = 0;
        r.wardWalls = 0;
    }
    // A built wall or gate segment of `team` in a cell (rubble is a blueprint too, and a ship aloft has no cells).
    const auto segmentAt = [&](int cx, int cz, uint32 team) -> int
    {
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return -1;
        const int index = structureIndexById(it->second.id);
        if (index < 0)
            return -1;
        const Ref& r = m_frame[index];
        return isLineType(r.type) && r.state->team == team && !r.state->blueprint ? index : -1;
    };
    // ONE breadth-first search per ward, in roster order (the older ward first). It visits "Walls per ward" segments at
    // most and covers the ones no earlier ward of its kind covers; a covered segment is still walked through, so a ward
    // behind another one reaches the walls past it.
    oc::vector<uint32> visited(m_frame.size(), 0u);
    oc::vector<int> queue;
    uint32 stamp = 0;
    const int limit = glm::max(m_settings.wardWalls, 1);
    for (Ref& ward : m_frame)
    {
        const int kind = wardKindOf(ward.type);
        if (kind < 0 || ward.state->blueprint || (ward.onShip && !m_shipJoined))
            continue;
        const uint32 team = ward.state->team;
        const glm::ivec2 home = cellOf(ward.entity->pos);
        ++stamp;
        queue.clear();
        for (const glm::ivec2& side : c_sides) // the segments that TOUCH the ward
            if (const int index = segmentAt(home.x + side.x, home.y + side.y, team); index >= 0 && visited[index] != stamp)
            {
                visited[index] = stamp;
                queue.push_back(index);
            }
        for (size_t head = 0; head < queue.size() && (int)head < limit; ++head)
        {
            Ref& segment = m_frame[queue[head]];
            if (segment.wardOf[kind] == 0)
            {
                segment.wardOf[kind] = ward.state->structureId;
                ++ward.wardWalls;
            }
            const glm::ivec2 cell = cellOf(segment.entity->pos);
            for (int dz = -1; dz <= 1; ++dz) // the 8 cells around it: a line dragged at a slant joins at the corners
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx != 0 || dz != 0))
                        if (const int index = segmentAt(cell.x + dx, cell.y + dz, team); index >= 0 && visited[index] != stamp)
                        {
                            visited[index] = stamp;
                            queue.push_back(index);
                        }
        }
    }
    // The flag damage() reads. A segment that comes under a ward or leaves it starts clean: no shield, nothing owed.
    for (Ref& r : m_frame)
    {
        if (!isLineType(r.type))
            continue;
        const bool warded = r.wardOf[0] != 0 || r.wardOf[1] != 0 || r.wardOf[2] != 0;
        if (warded != (r.state->warded != 0))
        {
            r.state->wall = GameStructureComponent::WallData{};
            r.state->warded = warded ? 1 : 0;
        }
    }
}

// The authority, every tick: each covered segment hands over what it resisted and sent back since the last tick, and
// its wards pay for it (an overdraft of one tick stops at an empty store); the Absorb shield grows back as far as its
// ward can pay, but only "Absorb regen delay" after the segment's last hit; then the shares for the next hits - 0 for a
// kind whose ward is missing, empty or latched off.
void StructureSystem::tickWards(float deltaSec)
{
    ProfileScope scope("Structures wards", EProfileCategory::Game);
    const auto wardState = [&](uint32 id) -> GameStructureComponent*
    {
        return wardActive(id) ? m_frame[structureIndexById(id)].state : nullptr;
    };
    const auto pay = [](GameStructureComponent* ward, float light)
    {
        if (ward && light > 0.0f)
            ward->store[0] = glm::max(ward->store[0] - light, 0.0f);
    };
    for (const Ref& r : m_frame)
    {
        GameStructureComponent& s = *r.state;
        if (!s.warded)
            continue;
        GameStructureComponent::WallData& w = s.wall;
        const float resisted = oc::atomic_ref<float>(w.resisted).exchange(0.0f);
        const float retaliated = oc::atomic_ref<float>(w.retaliated).exchange(0.0f);
        if (oc::atomic_ref<uint32>(w.struck).exchange(0u) != 0u)
            w.regenWait = glm::max(m_settings.wardAbsorbDelay, 0.0f);
        else
            w.regenWait = glm::max(w.regenWait - deltaSec, 0.0f);
        GameStructureComponent* resist = wardState(r.wardOf[0]);
        GameStructureComponent* absorb = wardState(r.wardOf[1]);
        GameStructureComponent* retaliate = wardState(r.wardOf[2]);
        pay(resist, resisted * m_settings.wardResistCost);
        pay(retaliate, retaliated * m_settings.wardRetaliateCost);

        oc::atomic_ref<float> shield(w.shield);
        const float shieldMax = glm::max(m_settings.wardAbsorbShield, 0.0f);
        if (r.wardOf[1] == 0 || s.blueprint)
            shield.store(0.0f);
        else if (absorb && w.regenWait <= 0.0f)
        {
            const float now = shield.load();
            float grow = glm::clamp(shieldMax - now, 0.0f, glm::max(m_settings.wardAbsorbRegen, 0.0f) * deltaSec);
            if (m_settings.wardAbsorbCost > 0.0f)
                grow = glm::min(grow, absorb->store[0] / m_settings.wardAbsorbCost);
            if (grow > 0.0f)
            {
                pay(absorb, grow * m_settings.wardAbsorbCost);
                shield.fetch_add(grow);
            }
            else if (now > shieldMax)
                shield.store(shieldMax); // "Absorb shield (health)" was turned down
        }

        // (Recomputed again after the payments above: a store paid empty this tick protects no more.)
        w.resist = r.wardOf[0] != 0 && wardActive(r.wardOf[0]) ? glm::clamp(m_settings.wardResist, 0.0f, 1.0f) : 0.0f;
        w.retaliate = r.wardOf[2] != 0 && wardActive(r.wardOf[2]) ? glm::max(m_settings.wardRetaliate, 0.0f) : 0.0f;
    }
}

void StructureSystem::clearWardPieces(const Ref& s)
{
    for (const EntityPtr& piece : s.wardPieces)
        if (piece)
            Globals::world.removeRootEntity(piece.get());
    s.wardPieces.clear();
    s.wardLook = 0;
}

// THE PIECES ON THE WALLS (every instance). Each covered segment shows, on each OPEN side (a cell with nothing in it, or
// only a flat road or cable piece): Resist - a full-size pane of light, with a post at each outer corner where two of
// them meet, so a line of walls (corners too) has one barrier with no seams; Absorb - a smaller pane in front of that
// one; Retaliate - spikes out of the wall. A kind shows while its ward has light; the Absorb pane on the authority only
// while the segment's shield holds (a client does not know the shield). The pieces are spawned again only when what
// they show changes (Ref::wardLook).
void StructureSystem::updateWardLooks(bool authority)
{
    const auto open = [&](int cx, int cz)
    {
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return true;
        const int index = structureIndexById(it->second.id);
        return index < 0 || (isCableOrCrossing(m_frame[index].type) && m_frame[index].type != EStructureType::Walkway);
    };
    for (const Ref& r : m_frame)
    {
        if (!isLineType(r.type))
            continue;
        uint32 kinds = 0;
        if (r.state->warded && !r.state->blueprint)
            for (int k = 0; k < WardKindCount; ++k)
                if (wardActive(r.wardOf[k]) && (k != 1 || !authority || r.state->wall.shield >= c_absorbShown))
                    kinds |= 1u << k;
        uint32 look = 0;
        if (kinds != 0)
        {
            const glm::ivec2 cell = cellOf(r.entity->pos);
            uint32 sides = 0;
            for (int i = 0; i < 4; ++i)
                if (open(cell.x + c_sides[i].x, cell.y + c_sides[i].y))
                    sides |= 1u << i;
            look = kinds | sides << 3;
        }
        if (look == r.wardLook)
            continue;
        clearWardPieces(r);
        r.wardLook = look;
        if (look == 0)
            continue;
        const auto spawn = [&](const char* prefab, float turn)
        {
            EntityPtr piece = Globals::world.spawnAssetFile(prefab,
                Transform(r.entity->pos, 1.0f, glm::angleAxis(turn, glm::vec3(0.0f, 1.0f, 0.0f))), true);
            if (!piece)
                return;
            Globals::world.addRootEntity(piece);
            r.wardPieces.push_back(oc::move(piece));
        };
        const uint32 sides = look >> 3;
        for (int i = 0; i < 4; ++i)
        {
            if (!(sides & (1u << i)))
                continue;
            if (kinds & 1u)
                spawn(c_resistPane, c_sideTurn[i]);
            if (kinds & 2u)
                spawn(c_absorbPane, c_sideTurn[i]);
            if (kinds & 4u)
                spawn(c_spikes, c_sideTurn[i]);
        }
        if (kinds & 1u)
            for (int c = 0; c < 4; ++c)
                if ((sides & (1u << c_cornerSides[c][0])) && (sides & (1u << c_cornerSides[c][1])))
                    spawn(c_resistCorner, c_cornerTurn[c]);
    }
}
