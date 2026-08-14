module Game;

import Core;
import Core.glm;
import Core.SDL;
import Core.Log;
import Core.Tweaks;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Core.GameHud;
import Input;
import UI;
import Entity;
import Physics;
import Force;
import RendererVK;
import Network;
import File; // AssetNode + loadAssetFile/writeAssetText (game save/load)
import :Match;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// The grid hotbar's category tables. Key 1..3 picks a category at the top level; inside one,
// keys 1..N arm an item and key 0 backs out.
static constexpr const char* c_buildCategories[3] = { "Combat", "Production", "Distribution" };
static constexpr BuildItem c_combatItems[] = {
    { "Emitter", EBuildTool::Structure, EStructureType::Emitter },
    { "Bastion", EBuildTool::Structure, EStructureType::Bastion },
    { "Lance", EBuildTool::Structure, EStructureType::Lance },
    { "Wall", EBuildTool::Structure, EStructureType::Wall },
    { "Turret", EBuildTool::Structure, EStructureType::Turret },
    { "Barracks", EBuildTool::Structure, EStructureType::Barracks },
    { "B-Brute", EBuildTool::Structure, EStructureType::BarracksBrute },
    { "B-Runner", EBuildTool::Structure, EStructureType::BarracksRunner },
    { "B-Spitter", EBuildTool::Structure, EStructureType::BarracksSpitter },
};
static constexpr float c_wallSegmentSpacing = 2.0f; // one segment per box width along the line
static constexpr int c_wallMaxSegments = 16;
static constexpr BuildItem c_productionItems[] = {
    { "Generator", EBuildTool::Structure, EStructureType::Generator },
    { "Solar", EBuildTool::Structure, EStructureType::Solar },
    { "Extractor", EBuildTool::Structure, EStructureType::Extractor },
    { "Fabricator", EBuildTool::Structure, EStructureType::Fabricator },
    { "Constructor", EBuildTool::Structure, EStructureType::Constructor },
};
// Link CREATION lives in Select mode (right-click smart connect); the hotbar keeps only the
// two-click management tools.
static constexpr BuildItem c_distributionItems[] = {
    { "Connector", EBuildTool::Structure, EStructureType::Connector },
    { "Disconnect", EBuildTool::Disconnect, EStructureType::Emitter },
    { "Upgrade", EBuildTool::Upgrade, EStructureType::Emitter },
    { "Battery", EBuildTool::Structure, EStructureType::Battery },
    { "Fuel tank", EBuildTool::Structure, EStructureType::FuelTank },
    { "M-Silo", EBuildTool::Structure, EStructureType::MineralSilo },
};
static std::span<const BuildItem> buildCategoryItems(int category)
{
    switch (category)
    {
    case 0: return c_combatItems;
    case 1: return c_productionItems;
    case 2: return c_distributionItems;
    default: return {};
    }
}

static uint32 packColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

static void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments = 32)
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

// The arena: a walled corridor along X — combat funnels through the middle.
static constexpr float c_corridorHalfLength = 65.0f; // x extent of the play area
static constexpr float c_corridorHalfWidth = 20.0f;  // z extent
static constexpr float c_wallStep = 10.0f;           // border block size (borderwall.pre)

GameMatch::GameMatch(bool enabled) : m_enabled(enabled)
{
    if (!m_enabled)
        return;

    {
        // Gameplay tweaks persist between runs and the server's values overrule the clients'.
        const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
        Tweak::floatVar("Game/Construction", "Refill radius", &m_refillRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Refill rate", &m_refillRate, 0.5f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Construction", "Player build radius", &m_buildRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Player build rate", &m_playerBuildRate, 0.5f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Base heal radius", &m_baseHealRadius, 0.0f, 60.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Base heal/s", &m_baseHealRate, 0.0f, 100.0f, 0.5f);
    }
    m_camera.registerTweaks();
    m_player.registerTweaks();
    m_structures.registerTweaks();
    m_npcs.registerTweaks();

    m_mouse = Globals::input.addMouseListener();
    m_mouse->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
    {
        const glm::vec2 pos(float(evt.x), float(evt.y));
        if (m_rmbDown)
            m_dragDeltaX += pos.x - m_mousePos.x;
        m_mousePos = pos;
    };
    m_mouse->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 3)
        {
            m_rmbDown = true;
            if (Globals::input.isWindowHasFocus() && Globals::ui.isViewportFocused()
                && !Globals::input.isMouseCaptured())
                m_rmbClicked = true;
        }
        if (evt.button == 1 && Globals::input.isWindowHasFocus() && Globals::ui.isViewportFocused()
            && !Globals::input.isMouseCaptured())
            m_placeClicked = true;
    };
    m_mouse->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 3)
            m_rmbDown = false;
    };
    m_mouse->onMouseWheelMoved = [this](const SDL_MouseWheelEvent& evt)
    {
        m_wheelAccum += float(evt.y);
    };
}

GameMatch::~GameMatch()
{
    if (!m_enabled)
        return;
    m_npcs.clear();
    m_structures.clear();
    m_player.despawn();
    if (m_ground)
        Globals::world.removeRootEntity(m_ground.get()); // corridor walls are its children — they go with it
}

void GameMatch::spawnWorld()
{
    if (!m_enabled)
        return;
    m_isServer = Globals::networkManager.role() == ENetRole::Server;
    m_isClient = Globals::networkManager.role() == ENetRole::Client;

    m_ground = Globals::world.spawnAssetFile("Entities/Game/ground.pre",
        Transform(glm::vec3(0.0f, -0.5f, 0.0f)), true); // box top = walkable y 0
    if (m_ground)
    {
        m_ground->setName("Ground");
        Globals::world.addRootEntity(m_ground);
    }

    Globals::networkManager.setOnGameEvent([this](std::string_view name) { handleNetEvent(name); });
    if (m_isServer)
    {
        // Structure changes broadcast to the client mirrors; only Gq* requests may come FROM clients.
        m_structures.onStructurePlaced = [this](int index) { sendStructurePlaced(index); };
        m_structures.onStructureRemoved = [this](uint32 id)
        {
            uint8 buffer[8];
            NetWriter writer(buffer);
            writer.write<uint32>(id);
            Globals::networkManager.fireNetworkEvent("GRm", writer.data());
        };
        m_structures.onCableChanged = [this](uint32 a, uint32 b, ECableType t, bool removed)
        {
            sendCableChanged(a, b, t, removed);
        };
        m_structures.onRouteChanged = [this](uint32 id)
        {
            if (const int index = m_structures.structureIndexById(id); index >= 0)
                sendRoute(index);
        };
        Globals::networkManager.setEventFilter([](uint32, std::string_view name, std::span<const uint8> data, Entity*)
        {
            return name.size() >= 2 && name[0] == 'G' && name[1] == 'q' && data.size() <= 64;
        });
    }

    m_structures.spawnNodes(); // deterministic on every instance (no sync needed)
    spawnCorridorWalls();      // deterministic local scenery too — every role builds its own copy
    // Placement stays INSIDE the arena (footprints may not clip the border wall ring).
    m_structures.setPlacementBounds(
        glm::vec2(-c_corridorHalfLength, -c_corridorHalfWidth),
        glm::vec2(c_corridorHalfLength, c_corridorHalfWidth));
    if (!m_isClient)
    {
        m_structures.spawnBase(m_basePos); // clients get it through the GPl mirror stream
        m_structures.spawnBase(m_enemyBasePos, 1); // the opposing team's anchor + income
        m_player.spawn(m_playerStart);     // clients ADOPT the capsule the server spawns for them
        // The server's own capsule is a PRIMARY: never handed to a client by the proximity
        // transfer, and it re-claims transferred objects it walks up to (the client symmetric).
        if (m_isServer && m_player.entity())
            Globals::networkManager.setServerPrimary(*m_player.entity(), true);
    }

    // Spawn view faces the MAP CENTER from wherever this instance's player starts (each end of
    // the corridor looks inward at the other team).
    const glm::vec3 cameraAnchor = m_isClient
        ? m_enemyBasePos + glm::vec3(0.0f, 1.0f, -6.0f) : m_playerStart;
    m_camera.setYawToward(glm::vec3(0.0f) - cameraAnchor);

    Globals::gameHud.setHotbarVisible(false); // hidden until Build mode (B); setMode populates the
    Globals::gameHud.selectSlot(0);           // grid hotbar (categories -> items) when entered

    Log::info("Game mode: SELECT by default (click inspects, RMB smart-connects, ground-click sets "
              "barracks routes). B/V/C = build categories, X = delete. Build powered Emitters to "
              "hold ground");
}

void GameMatch::spawnCorridorWalls()
{
    // A ring of border segments (borderwall.pre): the VISIBLE wall is a low 10x5x10 half-cube the
    // camera sees over, but the COLLIDER is a tall 10x20x10 box — an invisible fence keeps bodies
    // inside (the prefab offsets the render node down to the ground). Two long sides whose inner
    // faces sit ON the half-width, and two end caps behind the bases (~52 segments). Parented
    // under the Ground entity — the scene root stays lean and the whole ring dies with it. The
    // static body bakes at the WORLD pose at spawn; the pos rewrite below re-expresses it in the
    // ground's local space for the render.
    const auto spawnSegment = [this](float x, float z)
    {
        const glm::vec3 worldPos(x, c_wallStep, z); // collider center: half of the 20 m tall box
        EntityPtr wall = Globals::world.spawnAssetFile("Entities/Game/borderwall.pre",
            Transform(worldPos), true);
        if (!wall)
            return;
        wall->setName("Border");
        if (m_ground)
        {
            wall->reparentEntity(m_ground.get());
            wall->pos = worldPos - m_ground->pos; // ground has identity rot/scale
        }
        else
            Globals::world.addRootEntity(wall);
    };
    const float half = c_wallStep * 0.5f;
    for (float x = -c_corridorHalfLength + half; x < c_corridorHalfLength; x += c_wallStep)
    {
        spawnSegment(x, -c_corridorHalfWidth - half);
        spawnSegment(x, c_corridorHalfWidth + half);
    }
    for (float z = -c_corridorHalfWidth - c_wallStep + half;
         z < c_corridorHalfWidth + c_wallStep; z += c_wallStep)
    {
        spawnSegment(-c_corridorHalfLength - half, z);
        spawnSegment(c_corridorHalfLength + half, z);
    }
}

uint8 GameMatch::allocateClientTeam() const
{
    // Lowest free playable slot. The server holds m_team (0); each connected client's team lives
    // on its capsule's puppet component, so the live set needs no separate bookkeeping. With every
    // slot taken the extras double up on the last one — sharing a team beats having no Base.
    bool used[PlayableTeams] = {};
    if (m_team < PlayableTeams)
        used[m_team] = true;
    for (const auto& [id, p] : m_clientPlayers)
        if (p)
            if (const GameUnitComponent* u = getComponent<GameUnitComponent>(p.get()); u && u->team < PlayableTeams)
                used[u->team] = true;
    for (uint8 t = 0; t < PlayableTeams; ++t)
        if (!used[t])
            return t;
    return PlayableTeams - 1;
}

int GameMatch::clientTeam(uint32 clientId) const
{
    if (clientId == 0)
        return (int)m_team; // the server itself
    const auto it = m_clientPlayers.find(clientId);
    if (it == m_clientPlayers.end() || !it->second)
        return -1; // unknown: no capsule spawned for that id (never grant it a team's authority)
    const GameUnitComponent* u = getComponent<GameUnitComponent>(it->second.get());
    return u ? (int)u->team : -1;
}

glm::vec3 GameMatch::teamStartPos(uint8 team) const
{
    // One Base per playable team; the spawn sits just beside it (same offset the host uses).
    return team == 0 ? m_playerStart : m_enemyBasePos + glm::vec3(0.0f, 1.0f, -6.0f);
}

void GameMatch::onClientJoined(uint32 clientId)
{
    // Their player: spawned server-side (player.pre carries Component Network), handed over with
    // setOwner in the SAME frame; the client adopts + drives it through the claim stream.
    // Clients spawn beside THEIR team's Base — the team is a freshly allocated slot, and the
    // capsule's puppet component carries it to the owner through the snapshot game blob.
    const uint8 team = allocateClientTeam();
    const glm::vec3 clientStart = teamStartPos(team);
    EntityPtr player = Globals::world.spawnAssetFile("Entities/Game/player.pre",
        Transform(clientStart + glm::vec3(2.0f * (float)(clientId % 5), 0.0f, 1.5f * (float)(clientId % 3))), true);
    if (player)
    {
        player->setName(("Player " + std::to_string(clientId)).c_str());
        Globals::networkManager.setOwner(*player, clientId);
        if (GameUnitComponent* unit = getComponent<GameUnitComponent>(player.get()))
            unit->team = team; // the authoritative assignment: everything else reads it from here
        // the server-side twin's field carries the client's team (readbacks, visuals)
        if (ForceComponent* fc = getComponent<ForceComponent>(player.get()))
            fc->emitter.setTeam(team);
        Globals::world.addRootEntity(player);
        m_clientPlayers[clientId] = std::move(player);
        Log::info("Game: client " + std::to_string(clientId) + " assigned team " + std::to_string(team));
    }
    // World-state replay for the late joiner: every structure + cable, broadcast (mirrorPlace/
    // mirrorCable are idempotent, so already-connected clients shrug the duplicates off).
    for (int i = 0; i < m_structures.structureCount(); ++i)
        sendStructurePlaced(i);
    uint32 idA, idB;
    ECableType type;
    for (int i = 0; i < m_structures.cableTotal(); ++i)
    {
        m_structures.cableAt(i, idA, idB, type);
        sendCableChanged(idA, idB, type, false);
    }
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (!m_structures.structureRoute(i).empty())
            sendRoute(i); // barracks waypoint routes replay too
    Log::info("Game: client " + std::to_string(clientId) + " joined, world replayed");
}

void GameMatch::onClientLeft(uint32 clientId)
{
    if (const auto it = m_clientPlayers.find(clientId); it != m_clientPlayers.end())
    {
        if (it->second)
            Globals::world.removeRootEntity(it->second.get());
        m_clientPlayers.erase(it);
    }
}

void GameMatch::sendStructurePlaced(int index)
{
    uint8 buffer[32];
    NetWriter writer(buffer);
    const glm::vec3 pos = m_structures.structurePos(index);
    const glm::vec2 facing = m_structures.structureFacing(index);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>((uint8)m_structures.structureType(index));
    writer.write<float>(pos.x);
    writer.write<float>(pos.y);
    writer.write<float>(pos.z);
    writer.write<float>(facing.x);
    writer.write<float>(facing.y);
    writer.write<int16>((int16)m_structures.structureNodeIndex(index));
    writer.write<uint8>(m_structures.structureTeam(index));
    writer.write<uint8>(m_structures.structureBlueprint(index) ? 0u : 1u); // built flag (Base replay)
    Globals::networkManager.fireNetworkEvent("GPl", writer.data());
}

void GameMatch::sendCableChanged(uint32 idA, uint32 idB, ECableType type, bool removed)
{
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint32>(idA);
    writer.write<uint32>(idB);
    writer.write<uint8>((uint8)type);
    writer.write<uint8>(removed ? 1u : 0u);
    Globals::networkManager.fireNetworkEvent("GCb", writer.data());
}

void GameMatch::sendRoute(int index)
{
    uint8 buffer[80];
    NetWriter writer(buffer);
    const std::span<const glm::vec3> route = m_structures.structureRoute(index);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>((uint8)route.size());
    for (const glm::vec3& p : route)
    {
        writer.write<float>(p.x);
        writer.write<float>(p.z);
    }
    Globals::networkManager.fireNetworkEvent("GRt", writer.data());
}

static constexpr const char* c_gameSavePath = "Local/gamesave.txt"; // cwd = Assets/

void GameMatch::saveGame()
{
    if (m_isClient)
    {
        Log::warning("Save game: server only");
        return;
    }
    AssetNode root; // unnamed — writeAssetText writes only the children
    m_structures.saveTo(root);
    m_npcs.saveUnits(root);
    std::ofstream file(c_gameSavePath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file)
    {
        Log::warning(std::string("Save game: cannot write ") + c_gameSavePath);
        return;
    }
    file << writeAssetText(root);
    Log::info(std::string("Game saved to ") + c_gameSavePath);
}

void GameMatch::loadGame()
{
    if (m_isClient)
    {
        Log::warning("Load game: server only");
        return;
    }
    AssetNode root;
    std::string error;
    if (!loadAssetFile(c_gameSavePath, root, error))
    {
        Log::warning("Load game: " + error);
        return;
    }
    // Replace the sim. Structure removal hooks fire during the clear (GRm to clients), then the
    // loaded state re-broadcasts below; unit entities resync through the normal despawn/spawn
    // replication (Component Network in their prefabs).
    m_structures.loadFrom(root);
    m_npcs.loadUnits(root, m_structures);
    m_selectedId = 0;
    m_cablePendingId = 0;
    if (m_isServer)
    {
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            sendStructurePlaced(i);
            if (!m_structures.structureRoute(i).empty())
                sendRoute(i);
        }
        for (int c = 0; c < m_structures.cableTotal(); ++c)
        {
            uint32 idA, idB;
            ECableType type;
            m_structures.cableAt(c, idA, idB, type);
            sendCableChanged(idA, idB, type, false);
        }
    }
}

void GameMatch::requestSetRoute(uint32 id, std::span<const glm::vec3> points)
{
    const size_t count = glm::min(points.size(), (size_t)StructureSystem::MaxRouteWaypoints);
    if (!m_isClient)
    {
        m_structures.queueRouteRequest(id, points.subspan(0, count), (uint8)m_team);
        return;
    }
    uint8 buffer[64]; // 6 waypoints = 53B, inside the Gq* request cap
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    writer.write<uint8>((uint8)count);
    for (size_t k = 0; k < count; ++k)
    {
        writer.write<float>(points[k].x);
        writer.write<float>(points[k].z);
    }
    Globals::networkManager.fireNetworkEvent("GqW", writer.data());
}

void GameMatch::sendStats()
{
    // Volatile mirror state at ~5 Hz: resource totals + per-structure fractions (u8-quantized).
    uint8 buffer[1000];
    NetWriter writer(buffer);
    for (int t = 0; t < GameMaxTeams; ++t)
        writer.write<float>(m_structures.minerals((uint8)t));
    for (int t = 0; t < GameMaxTeams; ++t)
        writer.write<float>(m_structures.fuel((uint8)t));
    writer.write<float>(m_structures.gridEnergy());
    writer.write<float>(m_structures.gridEnergyCapacity());
    writer.write<float>(m_structures.energyGenPerSec());
    writer.write<float>(m_structures.energyUsePerSec());
    const int count = glm::min(m_structures.structureCount(), 79); // 11B each + the 146B header
                                                                   // stays under the 1024B event cap
    writer.write<uint16>((uint16)count);
    const auto frac8 = [](float v, float max) {
        return (uint8)glm::clamp(max > 0.0f ? v / max * 255.0f : 0.0f, 0.0f, 255.0f); };
    for (int i = 0; i < count; ++i)
    {
        writer.write<uint32>(m_structures.structureId(i));
        writer.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMax()));
        writer.write<uint8>(frac8(m_structures.structureCharge(i), m_structures.structureCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureFuel(i), m_structures.structureFuelCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureMinerals(i), m_structures.structureMineralCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureOutputFrac(i), 1.0f));
        writer.write<uint8>(frac8(m_structures.connectorUtilization(i), 1.0f));
        writer.write<uint8>((uint8)((m_structures.structurePowered(i) ? 1u : 0u)
            | (m_structures.structureBlueprint(i) ? 2u : 0u))); // status bits (health IS progress)
    }
    Globals::networkManager.fireNetworkEvent("GSt", writer.data());
}

void GameMatch::handleNetEvent(std::string_view name)
{
    // Both roles share the hook; each side reacts only to the names meant for it (our own
    // broadcasts self-dispatch locally and fall through harmlessly).
    if (name.size() < 3 || name[0] != 'G')
        return;
    NetReader reader(Globals::networkManager.currentEventData());
    if (m_isClient)
    {
        if (name == "GPl")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 type = reader.read<uint8>();
            const float x = reader.read<float>(), y = reader.read<float>(), z = reader.read<float>();
            const float fx = reader.read<float>(), fz = reader.read<float>();
            const int16 nodeIndex = reader.read<int16>();
            const uint8 team = reader.read<uint8>();
            const uint8 built = reader.read<uint8>();
            if (!reader.overflowed())
                m_structures.mirrorPlace(id, (EStructureType)type, glm::vec3(x, y, z), glm::vec2(fx, fz),
                    nodeIndex, team, built != 0);
        }
        else if (name == "GRm")
        {
            const uint32 id = reader.read<uint32>();
            if (!reader.overflowed())
                m_structures.mirrorRemove(id);
        }
        else if (name == "GCb")
        {
            const uint32 idA = reader.read<uint32>();
            const uint32 idB = reader.read<uint32>();
            const uint8 type = reader.read<uint8>();
            const uint8 removed = reader.read<uint8>();
            if (!reader.overflowed() && type < (uint8)ECableType::Count)
                m_structures.mirrorCable(idA, idB, (ECableType)type, removed != 0);
        }
        else if (name == "GSt")
        {
            float minerals[GameMaxTeams], fuel[GameMaxTeams];
            for (float& m : minerals)
                m = reader.read<float>();
            for (float& f : fuel)
                f = reader.read<float>();
            const float energy = reader.read<float>(), cap = reader.read<float>();
            const float gen = reader.read<float>(), use = reader.read<float>();
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 health = reader.read<uint8>(), charge = reader.read<uint8>();
                const uint8 fuelFrac = reader.read<uint8>(), mineralFrac = reader.read<uint8>();
                const uint8 output = reader.read<uint8>(), util = reader.read<uint8>();
                const uint8 status = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorStructureState(id, health / 255.0f, charge / 255.0f,
                        fuelFrac / 255.0f, mineralFrac / 255.0f, output / 255.0f, util / 255.0f,
                        (status & 1u) != 0, (status & 2u) != 0);
            }
            if (!reader.overflowed())
                m_structures.mirrorTotals(minerals, fuel, energy, cap, gen, use);
            // (mirrorTotals takes the spans by value into its own arrays — safe past this scope)
        }
        else if (name == "GDm")
        {
            // Unit melee damage the server's sim dealt to OUR player (we own our health).
            const uint8 count = reader.read<uint8>();
            for (uint8 k = 0; k < count && !reader.overflowed(); ++k)
            {
                const uint32 clientId = reader.read<uint32>();
                const float damage = reader.read<float>();
                if (!reader.overflowed() && clientId == Globals::networkManager.localClientId()
                    && damage > 0.0f && damage < 1000.0f)
                    m_player.applyDamage(damage);
            }
        }
        else if (name == "GRt")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 count = reader.read<uint8>();
            glm::vec3 points[StructureSystem::MaxRouteWaypoints];
            const uint8 used = glm::min<uint8>(count, StructureSystem::MaxRouteWaypoints);
            for (uint8 k = 0; k < used; ++k)
            {
                const float x = reader.read<float>(), z = reader.read<float>();
                points[k] = glm::vec3(x, 0.0f, z);
            }
            if (!reader.overflowed())
                m_structures.mirrorRoute(id, std::span<const glm::vec3>(points, used));
        }
        // (shield/materials state rides the entity snapshot's game blob now — no GSh event)
        return;
    }
    if (!m_isServer || name[1] != 'q')
        return;
    // Client requests, validated here + through the same authority seams local input uses. A
    // sender with no capsule has no assigned team, so it gets no authority to act as one.
    const uint32 sender = Globals::networkManager.currentEventSender();
    if (clientTeam(sender) < 0)
        return;
    if (name == "GqP")
    {
        const uint8 type = reader.read<uint8>();
        const float x = reader.read<float>(), z = reader.read<float>();
        const int16 nodeIndex = reader.read<int16>();
        const float fx = reader.read<float>(), fz = reader.read<float>();
        if (!reader.overflowed() && type < NumPlaceableStructures)
            m_structures.queuePlaceRequest((EStructureType)type, glm::vec3(x, 0.0f, z), nodeIndex,
                glm::vec3(fx, 0.0f, fz), requestTeam(sender));
    }
    else if (name == "GqC")
    {
        const uint32 idA = reader.read<uint32>();
        const uint32 idB = reader.read<uint32>();
        const uint8 type = reader.read<uint8>();
        if (!reader.overflowed() && type < (uint8)ECableType::Count)
            m_structures.queueCableRequest(idA, idB, (ECableType)type, requestTeam(sender));
    }
    else if (name == "GqD")
    {
        const uint32 id = reader.read<uint32>();
        if (!reader.overflowed())
            m_structures.queueDemolishRequest(id, requestTeam(sender));
    }
    else if (name == "GqW")
    {
        const uint32 id = reader.read<uint32>();
        const uint8 count = reader.read<uint8>();
        glm::vec3 points[StructureSystem::MaxRouteWaypoints];
        const uint8 used = glm::min<uint8>(count, StructureSystem::MaxRouteWaypoints);
        for (uint8 k = 0; k < used; ++k)
        {
            const float x = reader.read<float>(), z = reader.read<float>();
            points[k] = glm::vec3(x, 0.0f, z);
        }
        if (!reader.overflowed()) // team/barracks ownership validated at apply
            m_structures.queueRouteRequest(id, std::span<const glm::vec3>(points, used), requestTeam(sender));
    }
    // (GqE — the owner's shield self-report — now rides the claim stream's game blob, applied by
    // NetworkManager to the twin's GameUnitComponent + emitter. GqS/GqM went with player combat.
    // Unknown Gq* names from stale builds simply fall through here.)
}

void GameMatch::update(float deltaSec)
{
    if (!m_enabled)
        return;

    if (m_isClient)
    {
        // CLIENT: adopt + drive our own capsule (the owner simulates; claims stream the state);
        // shield/health run on LOCAL readbacks against the mirrored fields. World sim is remote.
        m_player.clientAdopt(teamStartPos((uint8)m_team));
        // Our team is the SERVER's assignment, carried on our capsule's puppet component by the
        // snapshot game blob (never derived from the clientId — see allocateClientTeam). It lands
        // a snapshot or two after adoption; follow it whenever it changes.
        if (m_player.team() != m_team)
        {
            m_team = m_player.team();
            m_player.setRespawnPos(teamStartPos((uint8)m_team));
            Log::info("We are team " + std::to_string(m_team));
        }
        m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
        m_player.tickShieldAndHealth(deltaSec);
        tickBaseHealing(deltaSec);
        m_structures.tickMirror(deltaSec);
        return;
    }

    const glm::vec3 playerPos = m_player.bodyPos();
    m_structures.tickAuthority(playerPos, deltaSec);
    m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
    m_player.tickShieldAndHealth(deltaSec);
    tickBaseHealing(deltaSec);

    // (No player-target publish step: units find enemy players — puppet GameUnitComponents —
    // through the same spatial queries as structures, and damage them through the same damage().)

    // MATERIALS loop (server-authoritative for every player): refill the carried inventory from
    // nearby own-team Silos/Base, invest it into nearby blueprints.
    {
        const auto tickPlayerMaterials = [&](const glm::vec3& pos, uint8 team, float& materials)
        {
            materials += m_structures.takeStoredMinerals(pos, m_refillRadius, team,
                glm::min(m_refillRate * deltaSec, m_player.materialsMax() - materials));
            materials -= m_structures.fundNearbyBlueprint(pos, m_buildRadius, team,
                glm::min(m_playerBuildRate * deltaSec, materials));
        };
        float serverMaterials = m_player.materials();
        tickPlayerMaterials(playerPos, (uint8)m_team, serverMaterials);
        m_player.setMaterials(serverMaterials);
        // Client twins: the puppet component IS the store — materialsFrac holds the
        // server-authoritative inventory (the snapshot game blob carries it back to the owner),
        // so it dies with the capsule and no clientId-keyed map exists. Team is stamped here too:
        // claims never apply it (client-forgeable), and units read it straight off the component.
        const float materialsMax = glm::max(m_player.materialsMax(), 1e-3f);
        for (const auto& [id, p] : m_clientPlayers)
            if (p)
                if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(p.get()); pc && pc->body.isValid())
                    if (GameUnitComponent* unit = getComponent<GameUnitComponent>(p.get()))
                    {
                        float materials = glm::clamp(unit->materialsFrac, 0.0f, 1.0f) * materialsMax;
                        tickPlayerMaterials(pc->body.getPosition(), requestTeam(id), materials);
                        unit->materialsFrac = materials / materialsMax;
                        unit->team = requestTeam(id);
                    }
    }

    // The unit SIM runs inside the entity pass (GameUnitComponent); this drains what it queued
    // (shots to spawn, deaths) and runs production.
    m_npcs.service(m_structures);

    if (m_isServer)
    {
        m_statTimer -= deltaSec;
        if (m_statTimer <= 0.0f)
        {
            sendStats();
            m_statTimer = 0.2f; // ~5 Hz volatile-state mirror
        }
        // GDm: damage banked on the client twins' puppet inboxes (unit melee, projectile hits —
        // the same damage() call every victim gets) is owed to each owner, whose GamePlayer runs
        // the shield-absorb rules (health is owner-computed). The component inbox accumulates
        // between flushes, so no clientId-keyed map is needed. Our own capsule's inbox drains in
        // GamePlayer::tickShieldAndHealth.
        m_damageTimer -= deltaSec;
        if (m_damageTimer <= 0.0f)
        {
            constexpr int c_maxDamageRecords = 32; // = the engine's client cap; 1 + 32*8 B fits the buffer
            uint32 owedIds[c_maxDamageRecords];
            float owed[c_maxDamageRecords];
            int count = 0;
            for (const auto& [id, p] : m_clientPlayers)
                if (p && count < c_maxDamageRecords)
                    if (GameUnitComponent* unit = getComponent<GameUnitComponent>(p.get()))
                        if (const float damage = unit->takePendingDamage(); damage > 0.0f)
                        {
                            owedIds[count] = id;
                            owed[count] = damage;
                            ++count;
                        }
            if (count > 0)
            {
                uint8 buffer[300];
                NetWriter writer(buffer);
                writer.write<uint8>((uint8)count);
                for (int i = 0; i < count; ++i)
                {
                    writer.write<uint32>(owedIds[i]);
                    writer.write<float>(owed[i]);
                }
                Globals::networkManager.fireNetworkEvent("GDm", writer.data());
            }
            m_damageTimer = 0.1f;
        }
    }
}

// Build/delete/cable intents: local queue on the authority, Gq* request events from a client —
// the SAME validation runs server-side either way (the seams the plan called for).
void GameMatch::requestPlace(EStructureType type, const glm::vec3& pos, int nodeIndex, const glm::vec3& facing)
{
    if (!m_isClient)
    {
        m_structures.queuePlaceRequest(type, pos, nodeIndex, facing, (uint8)m_team);
        return;
    }
    uint8 buffer[32];
    NetWriter writer(buffer);
    writer.write<uint8>((uint8)type);
    writer.write<float>(pos.x);
    writer.write<float>(pos.z);
    writer.write<int16>((int16)nodeIndex);
    writer.write<float>(facing.x);
    writer.write<float>(facing.z);
    Globals::networkManager.fireNetworkEvent("GqP", writer.data());
}

void GameMatch::requestCable(uint32 idA, uint32 idB, ECableType type)
{
    if (!m_isClient)
    {
        m_structures.queueCableRequest(idA, idB, type, (uint8)m_team);
        return;
    }
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint32>(idA);
    writer.write<uint32>(idB);
    writer.write<uint8>((uint8)type);
    Globals::networkManager.fireNetworkEvent("GqC", writer.data());
}

void GameMatch::requestDemolish(uint32 id)
{
    if (!m_isClient)
    {
        m_structures.queueDemolishRequest(id, (uint8)m_team);
        return;
    }
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    Globals::networkManager.fireNetworkEvent("GqD", writer.data());
}

GameMatch::Aim GameMatch::computeAim(const Camera& camera, EStructureType type) const
{
    Aim aim;
    aim.type = type;

    glm::vec3 pos;
    if (!aimGroundPoint(camera, pos))
        return aim;

    // Clamp to build range around the player, flatten to the whitebox ground plane.
    const glm::vec3 playerPos = m_player.bodyPos();
    glm::vec2 offset = glm::vec2(pos.x, pos.z) - glm::vec2(playerPos.x, playerPos.z);
    const float dist = glm::length(offset);
    if (dist > m_structures.placeRange())
        offset *= m_structures.placeRange() / dist;
    aim.pos = glm::vec3(playerPos.x + offset.x, 0.0f, playerPos.z + offset.y);

    if (aim.type == EStructureType::Extractor)
    {
        // Extractors only build ON a free resource node: snap the ghost to the nearest one.
        aim.nodeIndex = m_structures.findFreeNodeNear(aim.pos, m_structures.extractorSnapRadius());
        if (aim.nodeIndex < 0)
            return aim; // no free node under the cursor — invalid, no ghost
        aim.pos = m_structures.nodeGroundPos(aim.nodeIndex);
    }
    aim.pos = StructureSystem::snapToGrid(aim.type, aim.pos); // grid-aligned (extractors too)

    aim.valid = true;
    // Placing a blueprint is free — "affordable" now means the footprint's cells are unoccupied
    // (drives the red ghost AND gates the confirm click).
    aim.affordable = m_structures.cellsFree(aim.type, aim.pos);
    return aim;
}

bool GameMatch::aimGroundPoint(const Camera& camera, glm::vec3& outPos) const
{
    // Analytic ray vs the y=0 ground plane — the world floor IS flat, and a physics raycast kept
    // hitting the tall border-wall colliders (and other bodies) instead of the ground behind them,
    // which made near-wall placement jumpy.
    const Ray ray = camera.screenToRay(Globals::ui.getViewportRect(), m_mousePos);
    if (ray.dir.y > -1e-4f)
        return false; // looking at/above the horizon — no ground under the cursor
    outPos = ray.origin + ray.dir * (-ray.origin.y / ray.dir.y);
    return true;
}

int GameMatch::hoveredStructure(const Camera& camera) const
{
    glm::vec3 aimPos;
    if (!aimGroundPoint(camera, aimPos))
        return -1;
    return m_structures.findConnectableNear(aimPos, 4.0f);
}

// Slot labels/counts for the current grid level: categories at the top, the picked category's
// items (+ "Back" on key 0) inside one. Called every Build-mode frame — counts stay live.
void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    const std::span<const BuildItem> items = buildCategoryItems(m_buildCategory);
    for (int i = 0; i < (int)items.size(); ++i)
        hud.setSlot(i, items[i].label, items[i].tool == EBuildTool::Structure
            ? m_structures.affordableCount(items[i].structure, (uint8)m_team) : 0);
    for (int i = (int)items.size(); i < GameHud::NumSlots; ++i)
        hud.clearSlot(i);
    if (m_buildSelection >= 0)
        hud.selectSlot(m_buildSelection);
}

void GameMatch::setMode(EPlayerMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_cablePendingId = 0;
    m_selectedId = 0;
    m_lanceAiming = false;
    m_buildCategory = glm::max(m_buildCategory, 0); // default to Emitters; the category keys set it
    m_buildSelection = -1;
    Globals::gameHud.setHotbarVisible(mode == EPlayerMode::Build); // the hotbar IS the Build indicator
    if (mode == EPlayerMode::Build)
        refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category switch logs its own line (updateModeSwitching)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish — X returns to Select"); break;
    case EPlayerMode::Select: Log::info("Select mode: click inspects, RMB smart-connects, ground-click sets barracks routes — B/V/C build, X delete"); break;
    case EPlayerMode::None:   break; // unused — Select is the neutral mode
    }
}

void GameMatch::updateModeSwitching()
{
    Input& input = Globals::input;
    const bool focused = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    // B/V/C jump straight into Build with that category (fast chains: b->1->LMB, v->3->LMB);
    // the ACTIVE category's key exits to neutral, another one switches category in place.
    const SDL_Scancode catKeys[3] = { SDL_Scancode::SDL_SCANCODE_B, SDL_Scancode::SDL_SCANCODE_V, SDL_Scancode::SDL_SCANCODE_C };
    for (int i = 0; i < 3; ++i)
    {
        const bool down = focused && input.isKeyDown(catKeys[i]);
        if (down && !m_modeKeyWasDown[i])
        {
            if (m_mode == EPlayerMode::Build && m_buildCategory == i)
                setMode(EPlayerMode::Select);
            else
            {
                if (m_mode != EPlayerMode::Build)
                    setMode(EPlayerMode::Build);
                m_buildCategory = i;
                m_buildSelection = -1;
                m_cablePendingId = 0;
                m_lanceAiming = false;
                refreshBuildHotbar();
                Log::info(std::string("Build: ") + c_buildCategories[i]
                    + " — 1-9 arms, 0 disarms, LMB/F places, same key exits");
            }
        }
        m_modeKeyWasDown[i] = down;
    }
    // X toggles Delete <-> Select; Tab always returns to Select (the neutral mode).
    const bool xDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_X);
    if (xDown && !m_modeKeyWasDown[3])
        setMode(m_mode == EPlayerMode::Delete ? EPlayerMode::Select : EPlayerMode::Delete);
    m_modeKeyWasDown[3] = xDown;
    const bool tabDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_TAB);
    if (tabDown && !m_modeKeyWasDown[4])
        setMode(EPlayerMode::Select);
    m_modeKeyWasDown[4] = tabDown;
    // F9 save / F10 load (authority only — a client has no sim to save).
    const bool saveDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F9);
    if (saveDown && !m_saveKeyWasDown)
        saveGame();
    m_saveKeyWasDown = saveDown;
    const bool loadDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F10);
    if (loadDown && !m_loadKeyWasDown)
        loadGame();
    m_loadKeyWasDown = loadDown;
}

// The Disconnect/Upgrade tools: first click selects an endpoint, second click the other — acts on
// the EXISTING link between them (Disconnect removes it; Upgrade retypes a Basic cable to Heavy,
// the only tiered medium). Link CREATION lives in Select mode's right-click smart connect.
// Clicking empty ground, the selected structure, or RIGHT-clicking clears the selection; it
// persists by stable id.
void GameMatch::disarmBuild()
{
    m_buildSelection = -1;
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_cablePendingId = 0;
    refreshBuildHotbar(); // the slot highlight follows in the same frame
}

void GameMatch::updateLinkTool(const Camera& camera, bool confirmEdge, bool cancelEdge, EBuildTool tool)
{
    if (cancelEdge)
        m_cablePendingId = 0; // RIGHT-click drops the selected endpoint

    const int hover = hoveredStructure(camera);

    int pendingIdx = -1;
    if (m_cablePendingId != 0)
    {
        pendingIdx = m_structures.structureIndexById(m_cablePendingId);
        if (pendingIdx < 0)
            m_cablePendingId = 0; // the selected endpoint died
    }

    const glm::vec3 up(0.0f, 0.3f, 0.0f);
    if (hover >= 0)
        drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + up, 1.6f,
            packColor(glm::vec3(0.9f, 0.9f, 0.9f)), 20);
    if (pendingIdx >= 0)
        drawCircle(m_structures.structurePos(pendingIdx) * glm::vec3(1, 0, 1) + up, 1.9f,
            packColor(glm::vec3(0.3f, 1.0f, 0.4f)), 20);
    if (pendingIdx >= 0 && hover >= 0 && hover != pendingIdx)
    {
        // Preview: white = Disconnect will remove, cyan = Upgrade applies (Basic -> Heavy),
        // red = no link between the pair (or nothing to upgrade).
        const ECableType existing = m_structures.cableTypeBetween(m_cablePendingId, m_structures.structureId(hover));
        const bool actionable = existing != ECableType::Count
            && (tool == EBuildTool::Disconnect || existing == ECableType::Basic);
        const glm::vec3 previewColor = !actionable ? glm::vec3(1.0f, 0.3f, 0.2f)
            : tool == EBuildTool::Disconnect ? glm::vec3(0.9f, 0.9f, 0.9f) : glm::vec3(0.3f, 0.9f, 1.0f);
        Globals::rendererVK.addDebugLine(m_structures.structurePos(pendingIdx),
            m_structures.structurePos(hover), packColor(previewColor));
    }

    if (!confirmEdge)
        return;
    if (hover < 0)
    {
        m_cablePendingId = 0; // clicked empty ground
        return;
    }
    const uint32 hoverId = m_structures.structureId(hover);
    if (m_cablePendingId == 0)
        m_cablePendingId = hoverId;
    else if (m_cablePendingId == hoverId)
        m_cablePendingId = 0;
    else
    {
        const ECableType existing = m_structures.cableTypeBetween(m_cablePendingId, hoverId);
        if (existing == ECableType::Count)
            Log::info("No link between those structures");
        else if (tool == EBuildTool::Disconnect)
            requestCable(m_cablePendingId, hoverId, existing); // same pair + same type = remove
        else if (existing == ECableType::Basic)
            requestCable(m_cablePendingId, hoverId, ECableType::Heavy); // retype in place
        else
            Log::info("Only Basic cables upgrade (to Heavy)");
        m_cablePendingId = 0;
    }
}

void GameMatch::updateBuildMode(const Camera& camera, bool confirmEdge, bool cancelEdge)
{
    // Grid navigation: polled 1..9,0 edges (SDL scancodes are contiguous; InputControls routes the
    // same keys to the HUD's selectSlot for the highlight — harmless duplication).
    Input& input = Globals::input;
    const bool focus = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    int pressed = -1;
    for (int k = 0; k < 10; ++k)
    {
        const bool down = focus && input.isKeyDown((SDL_Scancode)((int)SDL_Scancode::SDL_SCANCODE_1 + k));
        if (down && !m_numKeyWasDown[k])
            pressed = k;
        m_numKeyWasDown[k] = down;
    }
    if (pressed >= 0)
    {
        m_cablePendingId = 0; // switching tools drops a half-made connection (and half-done aims)
        m_lanceAiming = false;
        m_wallPlacing = false;
        if (pressed == 9) // key 0 disarms the ghost (RMB does the same, see below)
            m_buildSelection = -1;
        else if (pressed < (int)buildCategoryItems(m_buildCategory).size())
            m_buildSelection = pressed;
    }
    refreshBuildHotbar();

    if (m_buildSelection < 0)
    {
        // Nothing armed — browsing the category: clicks inspect and RMB smart-connects / sets
        // barracks routes, exactly as Select mode.
        updateRightClickActions(camera, cancelEdge);
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    // RMB CANCELS, one step at a time: a half-finished two-click flow (link endpoint, Lance aim,
    // Wall line) drops first, and the next RMB disarms the item itself. Only once nothing is armed
    // does RMB go back to its Select-mode meaning (smart connect / barracks route) above.
    const BuildItem& item = buildCategoryItems(m_buildCategory)[m_buildSelection];
    if (item.tool != EBuildTool::Structure)
    {
        if (cancelEdge && m_cablePendingId == 0)
        {
            disarmBuild();
            return;
        }
        updateLinkTool(camera, confirmEdge, cancelEdge, item.tool);
        return;
    }
    m_cablePendingId = 0;

    // Lance second click: the position is anchored — the cursor now aims the cone's facing
    // (relative to the anchor); confirm places, too-close clicks just keep waiting. RIGHT-click
    // cancels the anchor before the confirm.
    if (m_lanceAiming)
    {
        if (cancelEdge)
        {
            m_lanceAiming = false;
            return;
        }
        const glm::vec3 up(0.0f, 0.3f, 0.0f);
        const uint32 color = packColor(glm::vec3(0.3f, 1.0f, 0.4f));
        drawCircle(m_lancePendingPos + up, 1.0f, color, 20);
        glm::vec3 target;
        glm::vec3 facing(0.0f);
        if (aimGroundPoint(camera, target))
        {
            const glm::vec2 d(target.x - m_lancePendingPos.x, target.z - m_lancePendingPos.z);
            if (glm::dot(d, d) > 0.25f)
            {
                const glm::vec2 dir = glm::normalize(d);
                facing = glm::vec3(dir.x, 0.0f, dir.y);
                // Preview: the aim line plus the lobe extent along the chosen facing.
                Globals::rendererVK.addDebugLine(m_lancePendingPos + up, target + up, color);
                Globals::rendererVK.addDebugLine(m_lancePendingPos + up,
                    m_lancePendingPos + facing * m_structures.emitterReachOf(EStructureType::Lance) + up,
                    packColor(glm::vec3(0.3f, 0.8f, 1.0f)));
            }
        }
        if (confirmEdge && glm::dot(facing, facing) > 0.5f)
        {
            requestPlace(EStructureType::Lance, m_lancePendingPos, -1, facing);
            m_lanceAiming = false;
        }
        return;
    }

    // Wall second click: segments preview along the anchored line; confirm queues one placement
    // per segment. RIGHT-click cancels the anchored line before the confirm.
    if (m_wallPlacing)
    {
        if (cancelEdge)
        {
            m_wallPlacing = false;
            return;
        }
        const Aim end = computeAim(camera, EStructureType::Wall);
        if (end.valid)
        {
            const glm::vec2 span(end.pos.x - m_wallStart.x, end.pos.z - m_wallStart.z);
            const float len = glm::length(span);
            const int segments = glm::clamp((int)(len / c_wallSegmentSpacing) + 1, 1, c_wallMaxSegments);
            const glm::vec2 dir = len > 1e-3f ? span / len : glm::vec2(0.0f);
            // Snap each sample to the SAME grid placeStructure uses — the preview circles must sit
            // exactly where the segments will land. Diagonal lines can snap two samples into the
            // same cell; dedup so it draws (and places) once.
            glm::vec3 points[c_wallMaxSegments];
            int count = 0;
            for (int s = 0; s < segments; ++s)
            {
                const glm::vec3 snapped = StructureSystem::snapToGrid(EStructureType::Wall,
                    m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s));
                if (count > 0 && glm::distance(glm::vec2(points[count - 1].x, points[count - 1].z),
                    glm::vec2(snapped.x, snapped.z)) < 0.1f)
                    continue;
                points[count++] = snapped;
            }
            for (int s = 0; s < count; ++s)
            {
                const bool free = m_structures.cellsFree(EStructureType::Wall, points[s]);
                drawCircle(points[s] + glm::vec3(0.0f, 0.3f, 0.0f), 0.9f,
                    packColor(free ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)), 12);
            }
            if (confirmEdge)
            {
                for (int s = 0; s < count; ++s)
                    requestPlace(EStructureType::Wall, points[s], -1, glm::vec3(0.0f));
                m_wallPlacing = false;
            }
        }
        return;
    }

    if (cancelEdge) // nothing half-placed (the two-click flows returned above): drop the ghost
    {
        disarmBuild();
        return;
    }

    const Aim aim = computeAim(camera, item.structure);
    if (!aim.valid)
    {
        // No ghost here (off-map, or an armed EXTRACTOR with no free node under the cursor — its
        // aim is invalid over ordinary ground). Clicks still inspect.
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f, color, 20);
    if (isEmitterType(aim.type)) // show the field footprint the powered variant would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterReachOf(aim.type) * 0.5f, color, 32);
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.cableRange(),
        packColor(glm::vec3(0.4f, 0.4f, 0.45f)), 40); // cable reach from this spot
    if (confirmEdge && aim.affordable)
    {
        if (aim.type == EStructureType::Lance)
        {
            m_lanceAiming = true; // first click anchors; the next click aims the cone
            m_lancePendingPos = aim.pos;
        }
        else if (aim.type == EStructureType::Wall)
        {
            m_wallPlacing = true; // first click anchors the line start; the next click ends it
            m_wallStart = aim.pos;
        }
        else
            requestPlace(aim.type, aim.pos, aim.nodeIndex, glm::vec3(0.0f));
    }
    // A click the placement REFUSES (occupied cells — i.e. on a building) inspects it instead of
    // doing nothing; a click that can place always places.
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ !aim.affordable);
}

void GameMatch::updateDeleteMode(const Camera& camera, bool confirmEdge)
{
    const int hover = hoveredStructure(camera);
    if (hover < 0)
        return;
    const bool deletable = m_structures.structureType(hover) != EStructureType::Base;
    drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
        packColor(deletable ? glm::vec3(1.0f, 0.25f, 0.2f) : glm::vec3(0.5f, 0.5f, 0.5f)), 20);
    if (confirmEdge && deletable)
        requestDemolish(m_structures.structureId(hover)); // validated in the authority tick (server)
}

void GameMatch::updateSelectMode(const Camera& camera, bool confirmEdge, bool smartLinkEdge)
{
    updateRightClickActions(camera, smartLinkEdge);
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
}

// RIGHT-CLICK actions with a selection, shared by Select AND Build mode: on a structure = SMART
// CONNECT (below); on GROUND with an own-team BARRACKS selected = set its unit ROUTE waypoint
// (SHIFT appends, a plain click restarts the route).
void GameMatch::updateRightClickActions(const Camera& camera, bool rmbEdge)
{
    if (!rmbEdge || m_selectedId == 0)
        return;
    const int hover = hoveredStructure(camera);
    if (hover >= 0)
    {
        trySmartConnect(hover);
        return;
    }
    const int sel = m_structures.structureIndexById(m_selectedId);
    glm::vec3 ground;
    if (sel < 0 || !isBarracksType(m_structures.structureType(sel))
        || m_structures.structureTeam(sel) != (uint8)m_team || !aimGroundPoint(camera, ground))
        return;
    ground.y = 0.0f;
    std::vector<glm::vec3> route;
    if (Globals::input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT))
    {
        const std::span<const glm::vec3> current = m_structures.structureRoute(sel);
        route.assign(current.begin(), current.end()); // hold shift: extend the route
    }
    if ((int)route.size() < StructureSystem::MaxRouteWaypoints)
        route.push_back(ground);
    requestSetRoute(m_selectedId, route);
}

// RIGHT-click SMART CONNECT, shared by Select AND Build mode: links the selection to the hovered
// structure with the inferred medium (connector's carried medium > the selection's output > the
// first medium both hold), then CHAINS the selection to the target so runs are one click each.
// No cableExists gate: a pair holds one link PER MEDIUM, and smartLinkTypeFor skips media the
// pair already has — a second right-click adds the NEXT medium (pipe from the extractor first,
// then the power cable from the generator side).
void GameMatch::trySmartConnect(int hoverIndex)
{
    const int sel = m_structures.structureIndexById(m_selectedId);
    if (sel < 0 || hoverIndex < 0 || sel == hoverIndex)
        return;
    const uint32 hoverId = m_structures.structureId(hoverIndex);
    if (hoverId == m_selectedId
        || m_structures.structureTeam(sel) != (uint8)m_team
        || m_structures.structureTeam(hoverIndex) != (uint8)m_team)
        return;
    const ECableType type = m_structures.smartLinkTypeFor(sel, hoverIndex);
    if (type == ECableType::Count)
        return;
    requestCable(m_selectedId, hoverId, type);
    m_selectedId = hoverId; // chain: the next right-click continues from here
}

// Click-to-select, shared by Select AND Build mode so inspecting a building never needs a mode
// switch: hover ring, LMB picks (empty ground deselects), and the selection keeps its highlight.
// allowPick false = this frame's click belongs to something else (a valid placement), so only the
// rings draw — the selection still shows while building.
void GameMatch::updateSelectionClick(const Camera& camera, bool confirmEdge, bool allowPick)
{
    const int hover = hoveredStructure(camera);
    if (hover >= 0 && allowPick)
        drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
            packColor(glm::vec3(0.9f, 0.9f, 0.9f)), 20);
    if (confirmEdge && allowPick)
        m_selectedId = hover >= 0 ? m_structures.structureId(hover) : 0;

    int selected = -1;
    if (m_selectedId != 0)
    {
        selected = m_structures.structureIndexById(m_selectedId);
        if (selected < 0)
            m_selectedId = 0; // it died
    }
    if (selected >= 0) // highlight ring; the info block rides the world label (buildWorldLabels)
        drawCircle(m_structures.structurePos(selected) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 2.0f,
            packColor(glm::vec3(0.3f, 1.0f, 0.4f)), 24);
}

// Short 3-5 char tags drawn above every structure/unit bar — the baseshape boxes all look alike,
// so the tag says what a thing is at a glance. The selected structure shows its full name instead.
static constexpr const char* c_structureShortNames[] = { "EMIT", "GEN", "CON", "EXTR", "BATT",
    "FUEL", "SOL", "FAB", "BSTN", "LNC", "BRK", "BRK-B", "BRK-R", "BRK-S", "WALL", "TRT", "SILO",
    "CNST", "BASE" };
static_assert(std::size(c_structureShortNames) == (size_t)EStructureType::Count);

// Remote actors (client side) carry no type on the wire — derive the tag from the replicated
// entity's prefab-derived name ("gameEnemyBrute", ...).
static const char* remoteShortName(std::string_view entityName, uint8 kind)
{
    if (kind == 2)
        return "PLR";
    if (entityName.find("Brute") != std::string_view::npos)   return "BRUT";
    if (entityName.find("Runner") != std::string_view::npos)  return "RUN";
    if (entityName.find("Spitter") != std::string_view::npos) return "SPIT";
    return "GRNT";
}

// World-anchored UI: a health bar above every damageable structure, plus name/HP/power info on the
// selected one. Projected here with THIS frame's final camera (worldToScreen), replaced wholesale
// each frame; the overlay just paints at the given viewport pixels.
void GameMatch::buildWorldLabels(const Camera& camera)
{
    std::vector<HudWorldLabel> labels;
    labels.reserve(m_structures.structureCount());
    const Rect& viewport = Globals::ui.getViewportRect();
    const int selected = m_selectedId != 0 ? m_structures.structureIndexById(m_selectedId) : -1;
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_structures.structureLabelAnchor(i), label.screenPos))
            continue;
        const EStructureType type = m_structures.structureType(i);
        label.title = c_structureShortNames[(int)type]; // the selected one overrides w/ full name
        const bool consumer = isEmitterType(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator;
        if (type != EStructureType::Base) // the Base is invulnerable — no health bar
        {
            label.barValue = m_structures.structureHealth(i);
            label.barMax = m_structures.structureHealthMax();
            const float frac = label.barValue / label.barMax;
            // Blueprint: health IS the construction progress — the bar reads blue while building.
            label.barColor = m_structures.structureBlueprint(i) ? glm::vec3(0.5f, 0.7f, 1.0f)
                : glm::mix(glm::vec3(1.0f, 0.25f, 0.2f), glm::vec3(0.3f, 1.0f, 0.4f), frac);
        }
        const float energyCap = m_structures.structureCapacity(i);
        const float fuelCap = m_structures.structureFuelCapacity(i);
        const float mineralCap = m_structures.structureMineralCapacity(i);
        // Second bar: fuel (orange) on the fuel holders, MINERALS (blue) on the mineral stores and
        // on anything that runs on minerals alone (barracks), energy (yellow) elsewhere.
        const bool fuelBar = type == EStructureType::Generator || type == EStructureType::FuelTank;
        const bool mineralBar = type == EStructureType::MineralSilo
            || (mineralCap > 0.0f && energyCap <= 0.0f);
        if (m_structures.structureBlueprint(i))
        {
        } // blueprint: no second bar — the (blue) health bar IS the build progress
        else if (type == EStructureType::Connector)
        {
            // THROUGHPUT gauge, not the relay buffer: how hard the busiest attached link runs,
            // colored by the medium the connector carries. No links = no bar.
            if (const int medium = m_structures.connectorMedium(i); medium >= 0)
            {
                label.bar2Value = m_structures.connectorUtilization(i);
                label.bar2Max = 1.0f;
                label.bar2Color = medium == 1 ? glm::vec3(1.0f, 0.6f, 0.2f)
                    : medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        else if (fuelBar)
        {
            label.bar2Value = m_structures.structureFuel(i);
            label.bar2Max = fuelCap;
            label.bar2Color = glm::vec3(1.0f, 0.6f, 0.2f);
        }
        else if (mineralBar)
        {
            label.bar2Value = m_structures.structureMinerals(i);
            label.bar2Max = mineralCap;
            label.bar2Color = glm::vec3(0.35f, 0.5f, 1.0f);
        }
        else if (energyCap > 0.0f)
        {
            label.bar2Value = m_structures.structureCharge(i);
            label.bar2Max = energyCap;
            label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        }
        if (i == selected)
        {
            label.emphasized = true;
            label.title = structureTypeName(type);
            char info[192];
            int len = type == EStructureType::Base
                ? snprintf(info, sizeof(info), "Invulnerable")
                : snprintf(info, sizeof(info), "HP %.0f / %.0f", label.barValue, label.barMax);
            if (type == EStructureType::Connector)
            {
                // A Connector holds capacity in every medium but carries exactly ONE — report
                // that one (and how hard its busiest link runs), not three idle relay buffers.
                const int medium = m_structures.connectorMedium(i);
                if (len > 0 && len < (int)sizeof(info))
                    len += snprintf(info + len, sizeof(info) - len, "\n%s",
                        medium < 0 ? "No links" : medium == 1 ? "Fuel line"
                        : medium == 2 ? "Conveyor" : "Power line");
                if (medium >= 0 && len > 0 && len < (int)sizeof(info))
                    len += snprintf(info + len, sizeof(info) - len, "\nThroughput %.0f%%",
                        m_structures.connectorUtilization(i) * 100.0f);
            }
            else
            {
                if (energyCap > 0.0f && len > 0 && len < (int)sizeof(info))
                    len += snprintf(info + len, sizeof(info) - len, "\nEnergy %.0f / %.0f",
                        m_structures.structureCharge(i), energyCap);
                if (fuelCap > 0.0f && len > 0 && len < (int)sizeof(info))
                    len += snprintf(info + len, sizeof(info) - len, "\nFuel %.0f / %.0f",
                        m_structures.structureFuel(i), fuelCap);
                if (mineralCap > 0.0f && len > 0 && len < (int)sizeof(info))
                    len += snprintf(info + len, sizeof(info) - len, "\nMinerals %.0f / %.0f",
                        m_structures.structureMinerals(i), mineralCap);
            }
            if (consumer && len > 0 && len < (int)sizeof(info))
                snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structurePowered(i) ? "Powered" : "No power");
            label.info = info;
        }
        labels.push_back(std::move(label));
    }
    // Units + players: own team green, enemy teams red. Only VISIBLE ones are fetched — an
    // off-screen one would just fail worldToScreen below. Works identically on server AND client:
    // remote instances' GameUnitComponents are populated by the snapshot game blob. Puppets are
    // player capsules; the own player is skipped (its HUD bars cover it).
    Entity* ownPlayer = m_player.entity();
    thread_local std::vector<Entity*> units;
    NpcSystem::queryVisibleUnits(camera, units);
    for (Entity* unitEntity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(unitEntity);
        if (!u || unitEntity == ownPlayer)
            continue;
        HudWorldLabel label;
        const float height = u->puppet ? 2.0f : 1.6f;
        if (!camera.worldToScreen(viewport, unitEntity->pos + glm::vec3(0.0f, height, 0.0f), label.screenPos))
            continue;
        label.title = remoteShortName(unitEntity->getName(), u->puppet ? 2 : 0);
        label.barValue = u->health;
        label.barMax = glm::max(u->healthMax, 1e-3f); // per-type: Brutes triple, Runners half
        label.barColor = u->team == (uint32)m_team
            ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.25f, 0.2f);
        label.bar2Value = u->energy; // shield battery under the health bar
        label.bar2Max = glm::max(u->energyMax, 1e-3f);
        label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        labels.push_back(std::move(label));
    }
    Globals::gameHud.setWorldLabels(std::move(labels));
}

void GameMatch::tickBaseHealing(float deltaSec)
{
    // Own player only: health is OWNER-computed, so every instance heals its own capsule against
    // its LOCAL structure mirror (clients hold the Bases through the GPl replay) — no sync needed.
    if (m_baseHealRate <= 0.0f || m_baseHealRadius <= 0.0f || !m_player.entity())
        return;
    const glm::vec3 pos = m_player.bodyPos();
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        if (m_structures.structureType(i) != EStructureType::Base
            || m_structures.structureTeam(i) != (uint8)m_team)
            continue;
        const glm::vec3 basePos = m_structures.structurePos(i);
        if (glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(basePos.x, basePos.z)) <= m_baseHealRadius)
        {
            m_player.heal(m_baseHealRate * deltaSec);
            return; // one Base is enough — never stack multiple
        }
    }
}

void GameMatch::updateHud()
{
    GameHud& hud = Globals::gameHud;
    hud.setBar("Health", m_player.health(), m_player.healthMax(), glm::vec3(0.9f, 0.25f, 0.2f));
    hud.setBar("Energy", m_player.energy(), m_player.energyMax(), glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setBar("Materials", m_player.materials(), m_player.materialsMax(), glm::vec3(1.0f, 0.8f, 0.4f)); // carried inventory
    hud.setCounter("Pressure", m_player.pressure(), 2, glm::vec3(0.8f, 0.4f, 1.0f));
    hud.setCounter("Density", m_player.density(), 2, glm::vec3(0.6f, 0.9f, 0.6f));
    hud.setCounter("Shield radius", m_player.shieldRadius(), 2, glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setCounter("Minerals", m_structures.minerals((uint8)m_team), 0, glm::vec3(0.6f, 0.8f, 1.0f));
    hud.setCounter("Fuel", m_structures.fuel((uint8)m_team), 0, glm::vec3(1.0f, 0.6f, 0.2f));
    hud.setBar("Grid energy", m_structures.gridEnergy(), glm::max(m_structures.gridEnergyCapacity(), 1.0f),
        glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy gen/s", m_structures.energyGenPerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy use/s", m_structures.energyUsePerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
}

void GameMatch::updateWindowed(Camera& camera, float deltaSec)
{
    if (!m_enabled)
        return;

    Input& input = Globals::input;
    const float qeAxis = (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_E) ? 1.0f : 0.0f)
                       - (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_Q) ? 1.0f : 0.0f);
    m_camera.apply(camera, m_player.interpolatedPos(), deltaSec, qeAxis, m_dragDeltaX, m_wheelAccum);
    m_dragDeltaX = 0.0f;
    m_wheelAccum = 0.0f;

    // Mode keys first, then the active mode consumes the inputs. In the tool modes LMB and F both
    // confirm (F is the fallback the UI can never eat); in NEUTRAL mode they are the combat keys —
    // LMB shoots a projectile at the cursor, F swings melee. Requests queue here and are
    // validated/applied in the authority tick.
    updateModeSwitching();
    const bool fDown = input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F)
        && input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    const bool lmbEdge = m_placeClicked;
    const bool rmbEdge = m_rmbClicked;
    const bool fEdge = fDown && !m_placeKeyWasDown;
    m_placeKeyWasDown = fDown;
    m_placeClicked = false;
    m_rmbClicked = false;
    const bool confirmEdge = lmbEdge || fEdge;

    switch (m_mode)
    {
    case EPlayerMode::Build:  updateBuildMode(camera, confirmEdge, rmbEdge); break;
    case EPlayerMode::Delete: updateDeleteMode(camera, confirmEdge); break;
    case EPlayerMode::Select: updateSelectMode(camera, confirmEdge, rmbEdge); break;
    case EPlayerMode::None:   break; // unused — Select IS the neutral mode (no player combat)
    }

    // Faint ring at the estimated equilibrium shield radius — compare it against the drawn bubble.
    const float shieldR = m_player.shieldRadius();
    if (shieldR > 0.05f)
        drawCircle(m_player.interpolatedPos(), shieldR, packColor(glm::vec3(0.3f, 0.8f, 1.0f)), 32);

    m_structures.drawDebug();

    // Barracks unit routes (own team): line chain from the barracks through its waypoints, each
    // with its destination circle — bright for the selected barracks, dim otherwise.
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        const std::span<const glm::vec3> route = m_structures.structureRoute(i);
        if (route.empty() || m_structures.structureTeam(i) != (uint8)m_team)
            continue;
        const bool bright = m_structures.structureId(i) == m_selectedId;
        const uint32 routeColor = packColor(glm::vec3(0.3f, 0.95f, 0.6f) * (bright ? 1.0f : 0.4f));
        glm::vec3 prev = m_structures.structurePos(i) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.4f, 0.0f);
        for (const glm::vec3& wp : route)
        {
            const glm::vec3 p(wp.x, 0.4f, wp.z);
            Globals::rendererVK.addDebugLine(prev, p, routeColor);
            drawCircle(p, m_structures.waypointRadius(), routeColor, 24);
            prev = p;
        }
    }

    buildWorldLabels(camera);
    updateHud();
}
