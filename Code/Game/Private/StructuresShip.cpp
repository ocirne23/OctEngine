module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Transform;
import Entity;
import Physics;
import :Ship;
import :Structures;

// THE SHIP'S STRUCTURES (see "The ship's structures" in Structures.ixx and Docs/Context/Game/Ship.md): a structure on
// the deck keeps its place in ship space; aloft it rides the ship out of the world, landed it is part of the base.

void StructureSystem::setShipPose(bool exists, const glm::vec3& pos, const glm::quat& rot, bool landed)
{
    m_shipExists = exists;
    m_shipPos = pos;
    m_shipRot = rot;
    m_shipLanded = exists && landed;
}

bool StructureSystem::worldCellOnDeck(int cx, int cz) const
{
    if (!m_shipJoined || !nearDeck(cx, cz))
        return false;
    // The cell's centre in ship space: landed, the pose is a pure quarter turn about a tile corner, so a cell centre
    // lands on a tile centre (an odd metre), and the floor is exact.
    const glm::vec3 centre(((float)cx + 0.5f) * GridCellSize, m_shipPos.y, ((float)cz + 0.5f) * GridCellSize);
    const glm::vec3 local = glm::inverse(m_shipRot) * (centre - m_shipPos);
    return GameShip::deckCell((int)std::floor(local.x / GridCellSize), (int)std::floor(local.z / GridCellSize));
}

bool StructureSystem::deckCellsFree(EStructureType type, const glm::vec3& p) const
{
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    bool free = true;
    forEachFootprintCell(type, p, identity, [&](int cx, int cz)
    {
        free = free && GameShip::deckCell(cx, cz) && !GameShip::helmCell(cx, cz);
    });
    if (!free)
        return false;
    const float half = footprintCellsOf(type) * GridCellSize * 0.5f;
    for (const Ref& r : m_frame)
    {
        // A GUN may stand on a RAIL tile (the rail stays; landed, the rail is the cell's occupant, the gun rides underId).
        if (!r.onShip || (isGunType(type) && r.type == EStructureType::ShipRail))
            continue;
        const float other = footprintCellsOf(r.type) * GridCellSize * 0.5f;
        if (glm::abs(r.shipLocal.x - p.x) < half + other - 1e-3f && glm::abs(r.shipLocal.z - p.z) < half + other - 1e-3f)
            return false;
    }
    return true;
}

void StructureSystem::queueShipPlaceRequest(EStructureType type, const glm::vec3& snappedLocal, float yield, uint8 team, int turn)
{
    PlaceRequest request{ type, snappedLocal, glm::vec3(0.0f), yield, team };
    request.ship = true;
    request.turn = turn;
    m_requests.push_back(request);
}

void StructureSystem::placeShipStructure(EStructureType type, const glm::vec3& local, float yield, uint8 team, int turn)
{
    if (!m_shipExists || !isShipPlaceableType(type) || (int)team >= GameMaxTeams)
        return;
    const glm::vec3 p = snapToDeck(type, glm::vec3(local.x, 0.0f, local.z));
    if (!deckCellsFree(type, p) || actorInFootprint(type, shipToWorld(p)))
        return; // raced the ghost: refused like a land placement
    // (No GPl: clients see no ship. CHEAT "Free instant build" skips the blueprint like on land.)
    spawnShipStructure(m_nextStructureId++, type, p, team, /*built*/ m_settings.cheatInstantBuild, yield,
        isRotatedPlacement(type) ? turn : 0);
}

int StructureSystem::spawnShipStructure(uint32 id, EStructureType type, const glm::vec3& local, uint8 team, bool built, float yield, int turn)
{
    const glm::vec3 pos = shipToWorld(local + glm::vec3(0.0f, spawnHeightOf(type), 0.0f));
    const glm::quat rot = m_shipRot * glm::angleAxis((float)(turn & 7) * glm::quarter_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    const int index = spawnStructure(id, type, pos, rot, team, built, yield, /*insert*/ m_shipJoined);
    if (index < 0)
        return -1;
    Ref& ref = m_frame[index];
    ref.onShip = true;
    ref.shipLocal = local;
    ref.shipTurn = (int8)(turn & 7);
    ref.authoredInvulnerable = ref.state->invulnerable != 0;
    if (type == EStructureType::ShipRail)
        applyRailSides(ref);
    if (!m_shipJoined)
        shipLeave(ref); // aloft: out of the world (it holds no cells yet - shipLeave only drops its own)
    else
        buildRailBody(ref);
    ++m_shipRevision;
    return index;
}

void StructureSystem::shipJoin(const Ref& s)
{
    insertCells(s);
    if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity))
    {
        Globals::physics.teleportBody(pc->body, s.entity->pos, s.entity->rot);
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(s.state->rubble ? 0.0f : 1.0f));
    }
    s.state->invulnerable = s.authoredInvulnerable ? 1 : 0;
    buildRailBody(s);
}

void StructureSystem::shipLeave(const Ref& s)
{
    eraseCells(s);
    if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity))
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(0.0f));
    s.state->invulnerable = 1; // no unit bites it, no shot or field hurts it, no Nav obstacle (gatherNavFeed)
    s.railBody.reset();
}

uint8 StructureSystem::railWorldSides(const Ref& s) const
{
    const uint8 local = GameShip::railSides((int)std::floor(s.shipLocal.x / GridCellSize), (int)std::floor(s.shipLocal.z / GridCellSize));
    static constexpr glm::vec3 c_dirs[4] = { { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
    uint8 world = 0;
    for (int a = 0; a < 4; ++a)
        if (local & (1 << a))
        {
            const glm::vec3 d = m_shipRot * c_dirs[a]; // a quarter turn: an axis again
            world |= glm::abs(d.x) > glm::abs(d.z) ? (d.x > 0.0f ? 1 : 2) : (d.z > 0.0f ? 4 : 8);
        }
    return world;
}

int StructureSystem::railPieces(const glm::vec3& tileLocal, glm::vec3 (&centres)[4], glm::vec3 (&halves)[4])
{
    // A piece along each open side of the tile, 0.4 m thick on the tile's edge, 1 m over the deck (shipRail.obj's).
    const uint8 sides = GameShip::railSides((int)std::floor(tileLocal.x / GridCellSize), (int)std::floor(tileLocal.z / GridCellSize));
    static constexpr glm::vec3 c_offsets[4] = { { 0.8f, 0.0f, 0.0f }, { -0.8f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.8f }, { 0.0f, 0.0f, -0.8f } };
    static constexpr glm::vec3 c_halves[4] = { { 0.2f, 0.5f, 1.0f }, { 0.2f, 0.5f, 1.0f }, { 1.0f, 0.5f, 0.2f }, { 1.0f, 0.5f, 0.2f } };
    int count = 0;
    for (int a = 0; a < 4; ++a)
        if (sides & (1 << a))
        {
            centres[count] = tileLocal + glm::vec3(0.0f, 0.5f, 0.0f) + c_offsets[a];
            halves[count] = c_halves[a];
            ++count;
        }
    return count;
}

void StructureSystem::buildRailBody(const Ref& s)
{
    s.railBody.reset();
    if (!s.onShip || s.type != EStructureType::ShipRail || !m_shipJoined || s.state->rubble)
        return;
    glm::vec3 centres[4], halves[4];
    const int count = railPieces(s.shipLocal, centres, halves);
    if (count == 0)
        return;
    // The body sits at the entity (0.5 over the deck, the ship's turn): the pieces relative to it.
    const glm::vec3 origin = s.shipLocal + glm::vec3(0.0f, spawnHeightOf(s.type), 0.0f);
    PhysicsShape shapes[4];
    for (int i = 0; i < count; ++i)
    {
        shapes[i].type = EPhysicsShapeType::Box;
        shapes[i].halfExtents = halves[i];
        shapes[i].offset = centres[i] - origin;
        shapes[i].friction = 0.9f;
        shapes[i].contactEvents = true; // the shots that hit it (the entity is the body's user data)
    }
    PhysicsBodyDesc desc;
    desc.type = EPhysicsBodyType::Static;
    desc.transform = Transform(s.entity->pos, 1.0f, s.entity->rot);
    desc.userData = s.entity;
    s.railBody = oc::make_shared<PhysicsBody>(Globals::physics.createBody(desc, oc::span<const PhysicsShape>(shapes, count)));
}

void StructureSystem::tickShipStructures()
{
    if (!m_shipExists)
        return;
    for (const Ref& r : m_frame)
    {
        if (!r.onShip)
            continue;
        r.entity->pos = shipToWorld(r.shipLocal + glm::vec3(0.0f, spawnHeightOf(r.type), 0.0f));
        r.entity->rot = shipStructureRot(r) * glm::angleAxis(r.cannonAim, glm::vec3(0.0f, 1.0f, 0.0f)); // (a Cannon: its gun's turn)
        if (r.rubbleVisual)
        {
            r.rubbleVisual->pos = shipToWorld(r.shipLocal);
            r.rubbleVisual->rot = m_shipRot;
        }
    }
    if (m_shipJoined && m_gangwayRetry > 0 && --m_gangwayRetry == 0)
    {
        buildGangways(); // a flight had no ground collider under it: try again (it sets the retry again while one has none)
        m_linksDirty = true; // the stairs' road cells
    }
    if (m_shipLanded == m_shipJoined)
        return;
    // THE EDGE: the ship set down on the grid (its tiles are world cells now) or lifted off.
    m_shipJoined = m_shipLanded;
    m_deckCellMin = glm::ivec2(INT32_MAX);
    m_deckCellMax = glm::ivec2(INT32_MIN);
    if (m_shipJoined) // the deck's world cells, for the cheap "not near the deck" answers (nearDeck)
    {
        for (int cz = -GameShip::DeckRows / 2; cz < GameShip::DeckRows / 2; ++cz)
            for (int cx = -GameShip::MaxHalfTiles; cx <= GameShip::MaxHalfTiles; ++cx)
            {
                if (!GameShip::deckCell(cx, cz))
                    continue;
                const glm::vec3 w = shipToWorld(glm::vec3(((float)cx + 0.5f) * GridCellSize, 0.0f, ((float)cz + 0.5f) * GridCellSize));
                const glm::ivec2 cell((int)std::floor(w.x / GridCellSize), (int)std::floor(w.z / GridCellSize));
                m_deckCellMin = glm::min(m_deckCellMin, cell - 1);
                m_deckCellMax = glm::max(m_deckCellMax, cell + 1);
            }
    }
    for (Ref& r : m_frame)
        if (r.onShip)
        {
            if (m_shipJoined)
                shipJoin(r);
            else
                shipLeave(r);
            r.warningTimer = 0.0f; // the badges ("No road") again at once, not on their ~1 s timer
        }
    m_carrierPassTimer = 0.0f; // the houses (the Beacon) send their workers onto the new road at once
    if (m_shipJoined)
        buildGangways();
    else
        clearGangways();
    m_linksDirty = true; // the road: the deck's tiles came or went
    ++m_shipRevision;
}

void StructureSystem::clearGangways()
{
    for (Gangway& g : m_gangways)
    {
        for (const EntityPtr& step : g.steps)
            Globals::world.removeRootEntity(step.get());
        g.steps.clear();
        g.body = PhysicsBody();
    }
    m_gangwayRoad.clear();
    m_gangwayRetry = 0;
}

void StructureSystem::buildGangways()
{
    // THE STAIRS: out of every ENTRANCE (GameShip::entrances: one to two tiles wide, on a side or the stern), 0.5 m out
    // and 0.375 m down a step (37 degrees: a body walks it, where 45 degrees was a climb), until a step's top meets the
    // ground under it - so a deck over low ground gets a flight down, and a deck only a little over the ground gets ONE
    // step (no step only where the ground is at the deck or over it). An entrance whose cells out there hold a land
    // structure gets none.
    clearGangways();
    bool noGround = false; // a probe found no ground collider: try again later (m_gangwayRetry)
    constexpr float c_tread = 0.5f;   // out per step (gangwayStep.pre is 0.5 m deep)
    constexpr float c_rise = 0.375f;  // down per step (the step is 1 m tall: the steps overlap, no gap shows)
    constexpr float c_slope = c_rise / c_tread;
    constexpr int c_maxSteps = 28; // 8.4 m: more than the landing allows (KeelDepth - 1)
    const float deckY = m_shipPos.y;
    const oc::span<const GameShip::Entrance> entrances = GameShip::entrances();
    static_assert(sizeof(m_gangways) / sizeof(m_gangways[0]) >= 5, "one gangway per entrance");
    for (size_t e = 0; e < entrances.size() && e < oc::size(m_gangways); ++e)
    {
        const GameShip::Entrance& ent = entrances[e];
        // Ship space: OUT from the edge, ACROSS it (along the entrance's tiles), the edge's middle.
        const glm::vec3 out((float)ent.out.x, 0.0f, (float)ent.out.y);
        const glm::ivec2 alongCells = ent.out.x != 0 ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
        const glm::vec3 across((float)alongCells.x, 0.0f, (float)alongCells.y);
        const auto tileCentre = [&](glm::ivec2 c) { return glm::vec3(((float)c.x + 0.5f) * GridCellSize, 0.0f, ((float)c.y + 0.5f) * GridCellSize); };
        const glm::vec3 edge = (tileCentre(ent.first) + tileCentre(ent.first + alongCells * (ent.count - 1))) * 0.5f + out * (GridCellSize * 0.5f);
        const float halfWidth = (float)ent.count * GridCellSize * 0.5f;
        const auto groundUnder = [&](float o) // the highest ground across the flight's width, o out from the edge
        {
            float ground = -FLT_MAX;
            for (const float a : { -halfWidth + 0.3f, 0.0f, halfWidth - 0.3f })
            {
                const glm::vec3 w = shipToWorld(edge + out * o + across * a);
                const float h = groundHeightAt(w.x, w.z, -FLT_MAX);
                if (h == -FLT_MAX)
                {
                    noGround = true;
                    return 0.0f; // no collider there: as if at the deck (the flight ends)
                }
                ground = glm::max(ground, h - deckY);
            }
            return ground;
        };
        int steps = 0;
        while (steps < c_maxSteps && -c_rise * (float)(steps + 1) > groundUnder(c_tread * ((float)steps + 0.5f)) + 0.15f)
            ++steps;
        if (steps == 0 && groundUnder(c_tread * 0.5f) < -0.05f)
            steps = 1; // a small drop: still one step (its top no lower than the ground, below), and the ramp
        if (steps == 0)
            continue;
        const float run = c_tread * (float)steps;
        // Its cells (the world cells out of the entrance tiles): none may hold a land structure.
        const int cellsOut = (int)std::ceil(run / GridCellSize - 1e-3f);
        bool clear = true;
        oc::small_vector<oc::pair<uint64, float>, 16> road;
        for (int j = 0; j < cellsOut; ++j)
            for (int i = 0; i < ent.count; ++i)
            {
                const glm::vec3 w = shipToWorld(tileCentre(ent.first + alongCells * i + ent.out * (j + 1)));
                const uint64 key = cellKey((int)std::floor(w.x / GridCellSize), (int)std::floor(w.z / GridCellSize));
                clear = clear && m_cells.find(key) == m_cells.end();
                // The road height: the ramp's surface at the cell's centre (the collider below), never under the ground there.
                const float o = GridCellSize * ((float)j + 0.5f);
                road.push_back({ key, deckY + glm::max(-c_slope * o, groundUnder(o) + 0.25f) });
            }
        if (!clear)
            continue;
        for (const auto& [key, y] : road)
            m_gangwayRoad[key] = y;
        Gangway& g = m_gangways[e];
        // The steps: one 2 m piece per tile across (gangwayStep.pre runs along its X: turned to OUT).
        const float yawOut = std::atan2(-out.z, out.x);
        const glm::quat outRot = glm::angleAxis(yawOut, glm::vec3(0.0f, 1.0f, 0.0f));
        for (int k = 0; k < steps; ++k)
        {
            // The step's top: one rise under the one before, never under the ground there (the one step of a small drop).
            const float top = glm::max(-c_rise * (float)(k + 1), groundUnder(c_tread * ((float)k + 0.5f)) + 0.05f);
            for (int i = 0; i < ent.count; ++i)
            {
                const float a = ((float)i + 0.5f) * GridCellSize - halfWidth;
                const glm::vec3 local = edge + out * (c_tread * ((float)k + 0.5f)) + across * a + glm::vec3(0.0f, top, 0.0f);
                EntityPtr step = Globals::world.spawnAssetFile("Entities/Game/gangwayStep.pre", Transform(shipToWorld(local), 1.0f, m_shipRot * outRot), true);
                if (!step)
                    continue;
                Globals::world.addRootEntity(step);
                g.steps.push_back(step);
            }
        }
        // ONE SLOPED BOX for the bodies: a smooth RAMP, y = -slope x out, its long axis turned to OUT, then tilted down
        // about ACROSS. FLUSH WITH THE DECK at the top (it starts at the deck's edge at the deck's height: no lip, and
        // nothing over the deck), and at the bottom it runs on INTO THE GROUND (to 0.3 m under the ground at the flight's
        // end), so its end is no lip either. It lies over the steps' tops by at most one rise.
        const float bottomGround = groundUnder(run);
        const float length = glm::clamp((0.3f - bottomGround) / c_slope, run, run + 3.0f); // along OUT
        const float tilt = std::atan(c_slope);
        const glm::quat rot = outRot * glm::angleAxis(-tilt, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 normal = rot * glm::vec3(0.0f, 1.0f, 0.0f);
        constexpr float c_thick = 0.1f; // half
        const glm::vec3 centre = edge + out * (length * 0.5f) + glm::vec3(0.0f, -c_slope * length * 0.5f, 0.0f) - normal * c_thick;
        PhysicsShape shape;
        shape.type = EPhysicsShapeType::Box;
        shape.halfExtents = glm::vec3(0.5f * length / std::cos(tilt), c_thick, halfWidth);
        shape.friction = 0.9f;
        PhysicsBodyDesc desc;
        desc.type = EPhysicsBodyType::Static;
        desc.transform = Transform(shipToWorld(centre), 1.0f, m_shipRot * rot);
        g.body = Globals::physics.createBody(desc, oc::span<const PhysicsShape>(&shape, 1));
    }
    m_gangwayRetry = noGround ? 30 : 0; // ~0.5 s at 60 ticks
}

void StructureSystem::shipColliderBoxes(oc::vector<GameShip::Box>& out) const
{
    out.clear();
    if (m_shipJoined)
        return; // landed: every structure has its own collider again
    for (const Ref& r : m_frame)
    {
        if (!r.onShip || r.state->rubble || isWalkThrough(r.type)) // (the Cannon: walked through)
            continue;
        if (r.type == EStructureType::ShipRail) // the rail pieces only: the tile is walked on
        {
            glm::vec3 centres[4], halves[4];
            const int count = railPieces(r.shipLocal, centres, halves);
            for (int i = 0; i < count; ++i)
            {
                GameShip::Box box;
                box.center = centres[i];
                box.half = halves[i];
                out.push_back(box);
            }
            continue;
        }
        const float height = spawnHeightOf(r.type);
        const float half = footprintCellsOf(r.type) * GridCellSize * 0.5f;
        GameShip::Box box;
        box.center = r.shipLocal + glm::vec3(0.0f, height, 0.0f);
        box.half = glm::vec3(half, height, half);
        out.push_back(box);
    }
}

void StructureSystem::applyRailSides(const Ref& s)
{
    const uint8 sides = GameShip::railSides((int)std::floor(s.shipLocal.x / GridCellSize), (int)std::floor(s.shipLocal.z / GridCellSize));
    static constexpr const char* c_names[4] = { "ArmPX", "ArmNX", "ArmPZ", "ArmNZ" };
    if (const SceneComponent* sc = getComponent<SceneComponent>(s.entity))
        for (const EntityPtr& child : sc->children)
            for (int a = 0; a < 4; ++a)
                if (oc::string_view(child->getName()) == c_names[a])
                    child->setEnabled((sides & (1 << a)) != 0);
}

void StructureSystem::spawnShipBase(uint8 team)
{
    if (!m_shipExists)
        return;
    const glm::ivec2 cell = GameShip::baseCell();
    const glm::vec3 local(((float)cell.x + 0.5f) * GridCellSize, 0.0f, ((float)cell.y + 0.5f) * GridCellSize);
    const int index = spawnShipStructure(m_nextStructureId++, EStructureType::Base, local, team, /*built*/ true);
    if (index >= 0) // the starting war chest + a FULL shield battery, as spawnBase
    {
        m_frame[index].state->store[2] = glm::min(m_settings.startMinerals, m_settings.mineralBaseCapacity);
        m_frame[index].state->store[0] = m_frame[index].state->capacity[0];
    }
}

void StructureSystem::ensureShipRails(uint8 team)
{
    if (!m_shipExists)
        return;
    // A rail where none belongs (an entrance now, from an older save): gone.
    for (size_t i = m_frame.size(); i-- > 0;)
        if (m_frame[i].onShip && m_frame[i].type == EStructureType::ShipRail
            && GameShip::railSides((int)std::floor(m_frame[i].shipLocal.x / GridCellSize), (int)std::floor(m_frame[i].shipLocal.z / GridCellSize)) == 0)
            destroyStructureAt(i);
    for (int cz = -GameShip::DeckRows / 2; cz < GameShip::DeckRows / 2; ++cz)
        for (int cx = -GameShip::MaxHalfTiles; cx <= GameShip::MaxHalfTiles; ++cx)
        {
            if (GameShip::railSides(cx, cz) == 0)
                continue;
            const glm::vec3 local(((float)cx + 0.5f) * GridCellSize, 0.0f, ((float)cz + 0.5f) * GridCellSize);
            bool taken = false;
            for (const Ref& r : m_frame)
                taken = taken || (r.onShip && glm::abs(r.shipLocal.x - local.x) < 0.5f && glm::abs(r.shipLocal.z - local.z) < 0.5f);
            if (!taken)
                spawnShipStructure(m_nextStructureId++, EStructureType::ShipRail, local, team, /*built*/ true);
        }
}

bool StructureSystem::shipLandingClear(const glm::vec3& centre, int turns) const
{
    const glm::quat rot = glm::angleAxis(GameShip::yawOfQuarterTurns(turns), glm::vec3(0.0f, 1.0f, 0.0f));
    for (int cz = -GameShip::DeckRows / 2; cz < GameShip::DeckRows / 2; ++cz)
        for (int cx = -GameShip::MaxHalfTiles; cx <= GameShip::MaxHalfTiles; ++cx)
        {
            if (!GameShip::deckCell(cx, cz))
                continue;
            const glm::vec3 world = centre + rot * glm::vec3(((float)cx + 0.5f) * GridCellSize, 0.0f, ((float)cz + 0.5f) * GridCellSize);
            const auto it = m_cells.find(cellKey((int)std::floor(world.x / GridCellSize), (int)std::floor(world.z / GridCellSize)));
            if (it == m_cells.end())
                continue;
            const int index = structureIndexById(it->second.id);
            if (index < 0 || !m_frame[index].onShip)
                return false; // a structure of the land stands there
        }
    return true;
}
