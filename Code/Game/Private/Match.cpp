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

// GRID HOTKEYS (RTS style): the 12 hotbar slots map onto QWER / ASDF / ZXCV, row-major. The ROOT
// page holds the categories (Q Combat, W Production, E Distribution) and Delete on X; a category
// page holds its items in order with Back on V (the last slot). The same slot index is reached by
// its key or by clicking the drawn slot.
static constexpr int c_gridSlots = 12;
static constexpr SDL_Scancode c_gridKeys[c_gridSlots] = {
    SDL_Scancode::SDL_SCANCODE_Q, SDL_Scancode::SDL_SCANCODE_W, SDL_Scancode::SDL_SCANCODE_E, SDL_Scancode::SDL_SCANCODE_R,
    SDL_Scancode::SDL_SCANCODE_A, SDL_Scancode::SDL_SCANCODE_S, SDL_Scancode::SDL_SCANCODE_D, SDL_Scancode::SDL_SCANCODE_F,
    SDL_Scancode::SDL_SCANCODE_Z, SDL_Scancode::SDL_SCANCODE_X, SDL_Scancode::SDL_SCANCODE_C, SDL_Scancode::SDL_SCANCODE_V,
};
static constexpr oc::string_view c_gridKeyLabels[c_gridSlots] = { "Q", "W", "E", "R", "A", "S", "D", "F", "Z", "X", "C", "V" };
// Root page slots: the two category pages, the two LINK TOOLS (armed directly — they need no
// page of their own), Delete.
static constexpr int c_rootConnectSlot = 2;    // E
static constexpr int c_rootDisconnectSlot = 6; // D
static constexpr int c_rootUpgradeSlot = 7;    // F
static constexpr int c_rootDeleteSlot = 9;     // X
static constexpr int c_cancelSlot = 10;        // C: one level back (armed item -> disarm; else -> Select), like Esc
static constexpr int c_pageBackSlot = 11;      // V: straight back to Select
static constexpr int c_numCategories = 2;
static constexpr const char* c_buildCategories[c_numCategories] = { "CMBT", "PROD" };      // slot captions
static constexpr const char* c_buildCategoryNames[c_numCategories] = { "Combat", "Production" }; // log prose
// 3-5 char shorthands, indexed by EStructureType — the SAME vocabulary the world tag over a
// building uses, so a hotbar slot and the thing it builds read identically.
static constexpr const char* c_structureShortNames[] = { "EMIT", "GEN", "CON", "EXTR", "BATT",
    "FUEL", "SOL", "FAB", "BSTN", "LNC", "BRK", "BRK-B", "BRK-R", "BRK-S", "WALL", "TRT", "SILO",
    "CNST", "BASE" };
static_assert(oc::size(c_structureShortNames) == (size_t)EStructureType::Count);
// A category page is just its list of placeable types — the shorthand table above IS each slot's
// caption, and the two link TOOLS live on the root page (Link mode), not inside a page.
static constexpr EStructureType c_combatItems[] = {
    EStructureType::Emitter,
    EStructureType::Bastion,
    EStructureType::Lance,
    EStructureType::Wall,
    EStructureType::Turret,
    EStructureType::Barracks,
    EStructureType::BarracksBrute,
    EStructureType::BarracksRunner,
    EStructureType::BarracksSpitter,
};
static constexpr float c_wallSegmentSpacing = 2.0f; // one segment per box width along the line
static constexpr int c_wallMaxSegments = 16;
// Everything that is not a weapon: generation, extraction and the distribution buildings (the old
// separate Distribution page is gone — the three link TOOLS, creation included, sit on the root
// page as two-click tools).
static constexpr EStructureType c_productionItems[] = {
    EStructureType::Generator,
    EStructureType::Solar,
    EStructureType::Extractor,
    EStructureType::Fabricator,
    EStructureType::Constructor,
    EStructureType::Connector,
    EStructureType::Battery,
    EStructureType::FuelTank,
    EStructureType::MineralSilo,
};
static oc::span<const EStructureType> buildCategoryItems(int category)
{
    switch (category)
    {
    case 0: return c_combatItems;
    case 1: return c_productionItems;
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

// GHOST: the exact box the structure will occupy — footprint square × the prefab's height, drawn
// as a wireframe at the snapped position (every whitebox building IS a box, so this is the real
// shape, not an approximation), plus the interior cell lines so the grid it takes is unambiguous.
static void drawStructureGhost(EStructureType type, const glm::vec3& groundPos, uint32 color)
{
    const int cells = StructureSystem::footprintCellsOf(type);
    const float half = cells * StructureSystem::GridCellSize * 0.5f;
    const float height = StructureSystem::spawnHeightOf(type) * 2.0f;
    const glm::vec3 c(groundPos.x, 0.0f, groundPos.z);
    const glm::vec3 corner[4] = {
        c + glm::vec3(-half, 0.0f, -half), c + glm::vec3(half, 0.0f, -half),
        c + glm::vec3(half, 0.0f, half),   c + glm::vec3(-half, 0.0f, half) };
    const glm::vec3 up(0.0f, height, 0.0f);
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec3& a = corner[i];
        const glm::vec3& b = corner[(i + 1) % 4];
        Globals::rendererVK.addDebugLine(a, b, color);                 // base
        Globals::rendererVK.addDebugLine(a + up, b + up, color);       // top
        Globals::rendererVK.addDebugLine(a, a + up, color);            // riser
    }
    for (int i = 1; i < cells; ++i) // interior grid: which cells are taken
    {
        const float o = -half + i * StructureSystem::GridCellSize;
        Globals::rendererVK.addDebugLine(c + glm::vec3(o, 0.0f, -half), c + glm::vec3(o, 0.0f, half), color);
        Globals::rendererVK.addDebugLine(c + glm::vec3(-half, 0.0f, o), c + glm::vec3(half, 0.0f, o), color);
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
    // MIDDLE-drag yaws the camera. RMB cannot: holding it steers the player (RTS move order), and
    // a held button cannot mean two things at once — Q/E remain the keyboard yaw.
    m_mouse->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
    {
        const glm::vec2 pos(float(evt.x), float(evt.y));
        if (m_mmbDown)
            m_dragDeltaX += pos.x - m_mousePos.x;
        m_mousePos = pos;
    };
    m_mouse->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
    {
        const bool inViewport = Globals::input.isWindowHasFocus() && Globals::ui.isViewportFocused()
            && !Globals::input.isMouseCaptured();
        if (evt.button == 2)
            m_mmbDown = true;
        if (evt.button == 3)
        {
            m_rmbDown = true;
            if (inViewport)
                m_rmbClicked = true;
        }
        if (evt.button == 1 && inViewport)
            m_placeClicked = true;
    };
    m_mouse->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 2)
            m_mmbDown = false;
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

    Globals::networkManager.setOnGameEvent([this](oc::string_view name) { handleNetEvent(name); });
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
        Globals::networkManager.setEventFilter([](uint32, oc::string_view name, oc::span<const uint8> data, Entity*)
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

    // The grid hotbar is always up in game mode: the ROOT page in Select/Delete, a category page in
    // Build. refreshBuildHotbar rebuilds it every windowed frame.
    Globals::gameHud.setHotbarLayout(4, c_gridKeyLabels);
    Globals::gameHud.setHotbarVisible(true);
    m_buildCategory = -1;
    refreshBuildHotbar();

    Log::info("Game mode: SELECT by default (click inspects, RMB smart-connects / sets barracks routes "
              "/ moves the player). Grid hotkeys QWER/ASDF/ZXCV or click the slots: Q/W = build "
              "categories, D/F = disconnect/upgrade, X = delete, C = cancel. Build powered Emitters "
              "to hold ground");
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
        player->setName(("Player " + oc::to_string(clientId)).c_str());
        Globals::networkManager.setOwner(*player, clientId);
        if (GameUnitComponent* unit = getComponent<GameUnitComponent>(player.get()))
            unit->team = team; // the authoritative assignment: everything else reads it from here
        // the server-side twin's field carries the client's team (readbacks, visuals)
        if (ForceComponent* fc = getComponent<ForceComponent>(player.get()))
            fc->emitter.setTeam(team);
        Globals::world.addRootEntity(player);
        m_clientPlayers[clientId] = oc::move(player);
        Log::info("Game: client " + oc::to_string(clientId) + " assigned team " + oc::to_string(team));
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
    Log::info("Game: client " + oc::to_string(clientId) + " joined, world replayed");
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
    const oc::span<const glm::vec3> route = m_structures.structureRoute(index);
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
    // F9: an explicit user action, main thread.
    if (!FileSystem::writeFileStr(c_gameSavePath, writeAssetText(root), /*allowMainThread*/ true))
    {
        Log::warning(oc::string("Save game: cannot write ") + c_gameSavePath);
        return;
    }
    Log::info(oc::string("Game saved to ") + c_gameSavePath);
}

void GameMatch::loadGame()
{
    if (m_isClient)
    {
        Log::warning("Load game: server only");
        return;
    }
    AssetNode root;
    oc::string error;
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

void GameMatch::requestSetRoute(uint32 id, oc::span<const glm::vec3> points)
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
    ProfileScope scope("Game send stats", EProfileCategory::Game);
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

void GameMatch::handleNetEvent(oc::string_view name)
{
    ProfileScope scope("Game net event", EProfileCategory::Game);
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
                m_structures.mirrorRoute(id, oc::span<const glm::vec3>(points, used));
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
            m_structures.queueRouteRequest(id, oc::span<const glm::vec3>(points, used), requestTeam(sender));
    }
    // (GqE — the owner's shield self-report — now rides the claim stream's game blob, applied by
    // NetworkManager to the twin's GameUnitComponent + emitter. GqS/GqM went with player combat.
    // Unknown Gq* names from stale builds simply fall through here.)
}

void GameMatch::update(float deltaSec)
{
    if (!m_enabled)
        return;
    ProfileScope scope("Game update", EProfileCategory::Game);

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
            Log::info("We are team " + oc::to_string(m_team));
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
        ProfileScope materialsScope("Player materials", EProfileCategory::Game);
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
    // Placing a blueprint is free — "affordable" now means the footprint is CLEAR: no structure
    // (or reserved node) on those cells, and nobody standing in them (drives the red ghost AND
    // gates the confirm click; placeStructure re-checks both, the MP seam).
    aim.affordable = m_structures.cellsFree(aim.type, aim.pos)
        && !StructureSystem::actorInFootprint(aim.type, aim.pos);
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

// The hotbar page for the current state: ROOT (Select/Delete: categories + Delete on X) or the
// picked category's items (+ Back on V). Called every windowed frame — counts stay live and the
// highlight always mirrors the real state (the engine's number-key routing may poke selectSlot).
void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    for (int i = 0; i < GameHud::NumSlots; ++i)
        hud.clearSlot(i);
    if (m_mode != EPlayerMode::Build || m_buildCategory < 0)
    {
        for (int i = 0; i < c_numCategories; ++i)
            hud.setSlot(i, c_buildCategories[i], 0);
        hud.setSlot(c_rootConnectSlot, "CONN", 0);
        hud.setSlot(c_rootDisconnectSlot, "DISC", 0);
        hud.setSlot(c_rootUpgradeSlot, "UPGR", 0);
        hud.setSlot(c_rootDeleteSlot, "DEL", 0);
        hud.selectSlot(m_mode == EPlayerMode::Delete ? c_rootDeleteSlot
            : m_mode == EPlayerMode::Link
                ? (m_linkTool == EBuildTool::Connect ? c_rootConnectSlot
                    : m_linkTool == EBuildTool::Disconnect ? c_rootDisconnectSlot : c_rootUpgradeSlot)
                : -1);
        return;
    }
    const oc::span<const EStructureType> items = buildCategoryItems(m_buildCategory);
    for (int i = 0; i < (int)items.size() && i < c_cancelSlot; ++i)
        hud.setSlot(i, c_structureShortNames[(int)items[i]],
            m_structures.affordableCount(items[i], (uint8)m_team));
    hud.setSlot(c_cancelSlot, "CNCL", 0);
    hud.setSlot(c_pageBackSlot, "BACK", 0);
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
    m_wallPlacing = false;
    m_buildSelection = -1;
    if (mode != EPlayerMode::Build)
        m_buildCategory = -1; // back to the root page
    refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category entry logs its own line (activateSlot)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish — X returns to Select"); break;
    case EPlayerMode::Link:   Log::info(m_linkTool == EBuildTool::Connect
        ? "Connect (E): click one structure, then the other — the medium is inferred; same key/Esc exits"
        : m_linkTool == EBuildTool::Disconnect
        ? "Disconnect (D): click the two ends of a link to remove it — same key/Esc exits"
        : "Upgrade (F): click the two ends of a Basic cable to make it Heavy — same key/Esc exits"); break;
    case EPlayerMode::Select: Log::info("Select mode: click inspects, RMB routes / moves — Q/W build, E/D/F link tools, X delete"); break;
    }
}

// One level back: a half-finished two-click step drops first, then the armed item disarms, then
// the category page (or Delete mode) returns to Select. Esc/Tab and the C "Cancel" slot.
void GameMatch::cancelOneLevel()
{
    if (m_mode == EPlayerMode::Link && m_cablePendingId != 0)
        m_cablePendingId = 0; // drop the picked endpoint, keep the tool
    else if (m_mode == EPlayerMode::Build && m_buildSelection >= 0)
    {
        if (m_lanceAiming || m_wallPlacing)
        {
            m_lanceAiming = false;
            m_wallPlacing = false;
        }
        else
            disarmBuild();
    }
    else
        setMode(EPlayerMode::Select);
}

// ONE entry point for a hotbar slot, whether its key was pressed or the drawn slot was clicked.
void GameMatch::activateSlot(int slot)
{
    if (slot < 0 || slot >= c_gridSlots)
        return;
    if (m_mode != EPlayerMode::Build || m_buildCategory < 0)
    {
        // ROOT page
        if (slot < c_numCategories)
        {
            setMode(EPlayerMode::Build);
            m_buildCategory = slot;
            m_buildSelection = -1;
            refreshBuildHotbar();
            Log::info(oc::string("Build: ") + c_buildCategoryNames[slot]
                + " — grid keys arm an item, LMB places, RMB cancels, V/Esc back");
        }
        else if (slot == c_rootConnectSlot || slot == c_rootDisconnectSlot || slot == c_rootUpgradeSlot)
        {
            // Link TOOLS arm straight off the root page (no category page of their own): the same
            // key toggles back to Select, another switches tool in place.
            const EBuildTool tool = slot == c_rootConnectSlot ? EBuildTool::Connect
                : slot == c_rootDisconnectSlot ? EBuildTool::Disconnect : EBuildTool::Upgrade;
            if (m_mode == EPlayerMode::Link && m_linkTool == tool)
                setMode(EPlayerMode::Select);
            else
            {
                m_linkTool = tool;
                setMode(EPlayerMode::Link);
                m_cablePendingId = 0;
                refreshBuildHotbar();
            }
        }
        else if (slot == c_rootDeleteSlot)
            setMode(m_mode == EPlayerMode::Delete ? EPlayerMode::Select : EPlayerMode::Delete);
        else if (slot == c_cancelSlot)
            setMode(EPlayerMode::Select); // cancels Delete/Link mode; a no-op in Select
        return;
    }
    // CATEGORY page
    if (slot == c_pageBackSlot)
    {
        setMode(EPlayerMode::Select);
        return;
    }
    if (slot == c_cancelSlot)
    {
        cancelOneLevel();
        return;
    }
    if (slot >= (int)buildCategoryItems(m_buildCategory).size() || slot >= c_cancelSlot)
        return; // empty slot
    m_cablePendingId = 0; // switching tools drops a half-made connection (and half-done aims)
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_buildSelection = slot;
    refreshBuildHotbar();
}

void GameMatch::updateModeSwitching()
{
    Input& input = Globals::input;
    const bool focused = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    // Grid hotkeys: polled edges on the 12 keys, each mapping straight onto its slot.
    for (int k = 0; k < c_gridSlots; ++k)
    {
        const bool down = focused && input.isKeyDown(c_gridKeys[k]) && (SDL_GetModState() & SDL_KMOD_CTRL) == 0;
        if (down && !m_gridKeyWasDown[k])
            activateSlot(k);
        m_gridKeyWasDown[k] = down;
    }
    // Escape/Tab: one level back (same as the C "Cancel" slot).
    const bool backDown = focused && (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_ESCAPE)
        || input.isKeyDown(SDL_Scancode::SDL_SCANCODE_TAB));
    if (backDown && !m_modeKeyWasDown[0])
        cancelOneLevel();
    m_modeKeyWasDown[0] = backDown;
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
// the only tiered medium); Connect CREATES one, inferring the medium.
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
    if (cancelEdge && m_cablePendingId != 0)
        m_cablePendingId = 0; // RIGHT-click drops the selected endpoint (and still moves — see below)

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
        // Preview: green = Connect will create (the inferred medium), white = Disconnect will
        // remove, cyan = Upgrade applies (Basic -> Heavy), red = the pair refuses this action.
        const ECableType existing = m_structures.cableTypeBetween(m_cablePendingId, m_structures.structureId(hover));
        bool actionable = false;
        if (tool == EBuildTool::Connect)
            actionable = m_structures.smartLinkTypeFor(pendingIdx, hover) != ECableType::Count;
        else
            actionable = existing != ECableType::Count
                && (tool == EBuildTool::Disconnect || existing == ECableType::Basic);
        const glm::vec3 previewColor = !actionable ? glm::vec3(1.0f, 0.3f, 0.2f)
            : tool == EBuildTool::Connect ? glm::vec3(0.3f, 1.0f, 0.4f)
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
        if (tool == EBuildTool::Connect)
        {
            // Medium inference (smartLinkTypeFor): the connector's
            // carried medium > the source's output > the first medium both endpoints hold. A pair
            // that already has every medium it can carry (or cannot reach) simply refuses.
            const ECableType type = m_structures.smartLinkTypeFor(pendingIdx, hover);
            if (type == ECableType::Count)
                Log::info("Those two cannot be linked (range, capacity, or already connected)");
            else
                requestCable(m_cablePendingId, hoverId, type);
            m_cablePendingId = hoverId; // CHAIN: the new endpoint stays picked for the next click
            return;
        }
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
    // (Grid keys / slot clicks arm items through activateSlot — see updateModeSwitching and the
    // hotbar click in updateWindowed.)
    if (m_buildCategory < 0 || m_buildSelection < 0)
    {
        // Nothing armed — browsing the category: clicks inspect and RMB smart-connects / sets
        // barracks routes, exactly as Select mode.
        updateRightClickActions(camera, cancelEdge);
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    // RMB CANCELS, one step at a time: a half-finished two-click flow (Lance aim, Wall line) drops
    // first, and the next RMB disarms the item itself. Only once nothing is armed does RMB go back
    // to its Select-mode meaning (barracks route / move order) above.
    const EStructureType armed = buildCategoryItems(m_buildCategory)[m_buildSelection];
    m_cablePendingId = 0;

    // Lance second click: the position is anchored — the cursor now aims the cone's facing
    // (relative to the anchor); confirm places, too-close clicks just keep waiting. RIGHT-click
    // cancels the anchor before the confirm.
    if (m_lanceAiming)
    {
        if (cancelEdge)
        {
            m_lanceAiming = false;
            return; // NOT consumed: the same press also walks the player (see the RMB chain)
        }
        const glm::vec3 up(0.0f, 0.3f, 0.0f);
        const uint32 color = packColor(glm::vec3(0.3f, 1.0f, 0.4f));
        drawStructureGhost(EStructureType::Lance, m_lancePendingPos, color);
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
            return; // NOT consumed: the same press also walks the player
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
                const bool free = m_structures.cellsFree(EStructureType::Wall, points[s])
                    && !StructureSystem::actorInFootprint(EStructureType::Wall, points[s]);
                drawStructureGhost(EStructureType::Wall, points[s],
                    packColor(free ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)));
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
        return; // NOT consumed: cancelling and moving are one press (see the RMB chain)
    }

    const Aim aim = computeAim(camera, armed);
    if (!aim.valid)
    {
        // No ghost here (off-map, or an armed EXTRACTOR with no free node under the cursor — its
        // aim is invalid over ordinary ground). Clicks still inspect.
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawStructureGhost(aim.type, aim.pos, color); // the exact box that will be built
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

void GameMatch::updateSelectMode(const Camera& camera, bool confirmEdge, bool rmbEdge)
{
    updateRightClickActions(camera, rmbEdge);
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
}

// RIGHT-CLICK actions with a selection, shared by Select AND Build mode: on a structure = SMART
// CONNECT (below); on GROUND with an own-team BARRACKS selected = set its unit ROUTE waypoint
// (SHIFT appends, a plain click restarts the route).
void GameMatch::updateRightClickActions(const Camera& camera, bool rmbEdge)
{
    if (!rmbEdge || m_selectedId == 0)
        return;
    // (Linking moved to the two-click CONN tool on the root page — RMB no longer creates cables.)
    if (hoveredStructure(camera) >= 0)
        return; // on a building: the caller turns it into a MOVE order
    const int sel = m_structures.structureIndexById(m_selectedId);
    glm::vec3 ground;
    if (sel < 0 || !isBarracksType(m_structures.structureType(sel))
        || m_structures.structureTeam(sel) != (uint8)m_team || !aimGroundPoint(camera, ground))
        return; // not a route click either: the caller turns it into a MOVE order
    ground.y = 0.0f;
    m_rmbConsumed = true;
    oc::vector<glm::vec3> route;
    if (Globals::input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT))
    {
        const oc::span<const glm::vec3> current = m_structures.structureRoute(sel);
        route.assign(current.begin(), current.end()); // hold shift: extend the route
    }
    if ((int)route.size() < StructureSystem::MaxRouteWaypoints)
        route.push_back(ground);
    requestSetRoute(m_selectedId, route);
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
// Remote actors (client side) carry no type on the wire — derive the tag from the replicated
// entity's prefab-derived name ("gameEnemyBrute", ...).
static const char* remoteShortName(oc::string_view entityName, uint8 kind)
{
    if (kind == 2)
        return "PLR";
    if (entityName.find("Brute") != oc::string_view::npos)   return "BRUT";
    if (entityName.find("Runner") != oc::string_view::npos)  return "RUN";
    if (entityName.find("Spitter") != oc::string_view::npos) return "SPIT";
    return "GRNT";
}

// World-anchored UI: a health bar above every damageable structure, plus name/HP/power info on the
// selected one. Projected here with THIS frame's final camera (worldToScreen), replaced wholesale
// each frame; the overlay just paints at the given viewport pixels.
void GameMatch::buildWorldLabels(const Camera& camera)
{
    ProfileScope scope("Game world labels", EProfileCategory::Game);
    oc::vector<HudWorldLabel> labels;
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
        labels.push_back(oc::move(label));
    }
    // Units + players: own team green, enemy teams red. Only VISIBLE ones are fetched — an
    // off-screen one would just fail worldToScreen below. Works identically on server AND client:
    // remote instances' GameUnitComponents are populated by the snapshot game blob. Puppets are
    // player capsules; the own player is skipped (its HUD bars cover it).
    Entity* ownPlayer = m_player.entity();
    thread_local oc::vector<Entity*> units;
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
        labels.push_back(oc::move(label));
    }
    Globals::gameHud.setWorldLabels(oc::move(labels));
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
    ProfileScope scope("Game windowed", EProfileCategory::Game);

    Input& input = Globals::input;
    // Camera yaw on the ARROW keys (Q/E belong to the grid hotkeys) + middle-drag.
    const float yawAxis = (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f)
                        - (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LEFT) ? 1.0f : 0.0f);
    m_camera.apply(camera, m_player.interpolatedPos(), deltaSec, yawAxis, m_dragDeltaX, m_wheelAccum);
    m_dragDeltaX = 0.0f;
    m_wheelAccum = 0.0f;

    // Grid keys first, then the active mode consumes the clicks. Requests queue here and are
    // validated/applied in the authority tick. Whatever RMB the mode does NOT consume becomes a
    // player MOVE ORDER below. The HOTBAR eats clicks over it: LMB on a slot activates it (same
    // path as its key), and neither button reaches the world through the hotbar.
    updateModeSwitching();
    m_rmbConsumed = false;
    bool lmbEdge = m_placeClicked;
    bool rmbEdge = m_rmbClicked;
    m_placeClicked = false;
    m_rmbClicked = false;
    if (const int slot = Globals::gameHud.slotAtScreenPos(m_mousePos); slot >= 0)
    {
        if (lmbEdge)
            activateSlot(slot);
        lmbEdge = false;
        rmbEdge = false;
        m_rmbMoveDrag = false; // dragging a move order onto the hotbar ends it
    }
    const bool confirmEdge = lmbEdge;
    refreshBuildHotbar(); // page + counts + highlight, every frame

    switch (m_mode)
    {
    case EPlayerMode::Build:  updateBuildMode(camera, confirmEdge, rmbEdge); break;
    case EPlayerMode::Delete: updateDeleteMode(camera, confirmEdge); break;
    case EPlayerMode::Link:
        // RMB drops the picked endpoint, then exits the tool — the same one-step-at-a-time cancel
        // Build mode uses (and neither becomes a move order).
        if (rmbEdge && m_cablePendingId == 0)
            setMode(EPlayerMode::Select); // NOT consumed: the press also becomes a move order
        else
            updateLinkTool(camera, confirmEdge, rmbEdge, m_linkTool);
        break;
    case EPlayerMode::Select: updateSelectMode(camera, confirmEdge, rmbEdge); break;
    }

    // MOVE ORDER (RTS right-click): RMB ALWAYS moves the player. Cancelling rides along on the
    // same press — disarming a ghost, dropping a Lance/Wall anchor or a picked link endpoint, or
    // leaving a link tool all happen AND the capsule starts walking, because a cancel that also
    // ate the movement felt like a dropped input. Only the barracks ROUTE waypoint consumes the
    // press (m_rmbConsumed): it is a positive order, not a cancel, and pairing it with a move
    // would send the player off toward every rally point.
    if (!m_rmbDown)
        m_rmbMoveDrag = false;
    if (rmbEdge && !m_rmbConsumed)
    {
        const int hover = hoveredStructure(camera);
        glm::vec3 clicked;
        if (hover >= 0 && aimGroundPoint(camera, clicked))
        {
            // Walk to WHERE you clicked, not to the building's middle: the cursor's ground point
            // is the destination, pushed just outside the footprint along the side it fell on, so
            // the capsule ends up standing at that face instead of grinding into the wall.
            const glm::vec3 center = m_structures.structurePos(hover);
            const float half = StructureSystem::footprintCellsOf(m_structures.structureType(hover))
                * StructureSystem::GridCellSize * 0.5f + 1.2f; // + capsule and a gap
            glm::vec2 d(clicked.x - center.x, clicked.z - center.z);
            const float deepest = glm::max(glm::abs(d.x), glm::abs(d.y));
            if (deepest < half) // inside the inflated footprint: push out to the nearest face
            {
                if (deepest < 1e-3f) // dead center: come from the player's side
                    d = glm::vec2(m_player.bodyPos().x - center.x, m_player.bodyPos().z - center.z);
                const float scale = glm::max(glm::abs(d.x), glm::abs(d.y));
                d = scale > 1e-3f ? d * (half / scale) : glm::vec2(half, 0.0f);
            }
            m_player.setMoveTarget(glm::vec3(center.x + d.x, 0.0f, center.z + d.y));
            m_rmbMoveDrag = false; // a building order is one-shot: dragging off it must not re-aim
        }
        else if (hover < 0)
            m_rmbMoveDrag = true; // ground: holding keeps re-aiming at the cursor
    }
    glm::vec3 moveGround;
    if (m_rmbMoveDrag && aimGroundPoint(camera, moveGround))
        m_player.setMoveTarget(glm::vec3(moveGround.x, 0.0f, moveGround.z));
    if (m_player.hasMoveTarget()) // destination marker until the capsule arrives
        drawCircle(m_player.moveTarget() + glm::vec3(0.0f, 0.15f, 0.0f), 0.6f,
            packColor(glm::vec3(0.3f, 0.95f, 0.6f)), 16);

    // Faint ring at the estimated equilibrium shield radius — compare it against the drawn bubble.
    const float shieldR = m_player.shieldRadius();
    if (shieldR > 0.05f)
        drawCircle(m_player.interpolatedPos(), shieldR, packColor(glm::vec3(0.3f, 0.8f, 1.0f)), 32);

    m_structures.drawDebug();

    // Barracks unit routes (own team): line chain from the barracks through its waypoints, each
    // with its destination circle — bright for the selected barracks, dim otherwise.
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        const oc::span<const glm::vec3> route = m_structures.structureRoute(i);
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
