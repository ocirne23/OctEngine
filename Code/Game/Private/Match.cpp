module Game;

import Core;
import Core.glm;
import Core.SDL;
import Core.Log;
import Core.Tweaks;
import Core.Time; // real clock for the barrier pulse
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
import Nav;
import Spatial;
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
// Root page slots: the three category pages (Q/W/E) and Delete.
static constexpr int c_rootDeleteSlot = 9;     // X
static constexpr int c_cancelSlot = 10;        // C: one level back (armed item -> disarm; else -> Select), like Esc
static constexpr int c_pageBackSlot = 11;      // V: straight back to Select
static constexpr int c_numCategories = 3;
static constexpr const char* c_buildCategories[c_numCategories] = { "CMBT", "PROD", "CBLE" };      // slot captions
static constexpr const char* c_buildCategoryNames[c_numCategories] = { "Combat", "Production", "Cables" }; // log prose
// 3-5 char shorthands, indexed by EStructureType — the SAME vocabulary the world tag over a
// building uses, so a hotbar slot and the thing it builds read identically.
static constexpr const char* c_structureShortNames[] = { "EMIT", "GEN", "CON", "EXTR", "BATT",
    "FUEL", "SOL", "FAB", "BSTN", "LNC", "BRK", "BRK-B", "BRK-R", "BRK-S", "WALL", "TRT", "SILO",
    "CNST", "BASE", "CBL-P", "CBL-F", "CBL-M", "CRSS" };
static_assert(oc::size(c_structureShortNames) == (size_t)EStructureType::Count);
// A category page is just its list of placeable types — the shorthand table above IS each slot's
// caption.
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
static constexpr int c_cableMaxSegments = 32; // one L-line / paint-fill placement burst cap
// Everything that is not a weapon: generation, extraction and the distribution buildings.
static constexpr EStructureType c_productionItems[] = {
    EStructureType::Generator,
    EStructureType::Solar,
    EStructureType::Extractor,
    EStructureType::Fabricator,
    EStructureType::Constructor,
    EStructureType::Battery,
    EStructureType::FuelTank,
    EStructureType::MineralSilo,
};
// PHYSICAL cables: one segment type per medium + the 1x3 crossing bridge. Placement paints or
// draws L-lines (see updateCablePlacement); connections derive from cell adjacency.
static constexpr EStructureType c_cableItems[] = {
    EStructureType::CablePower,
    EStructureType::CablePipe,
    EStructureType::CableConveyor,
    EStructureType::Crossing,
};
static oc::span<const EStructureType> buildCategoryItems(int category)
{
    switch (category)
    {
    case 0: return c_combatItems;
    case 1: return c_productionItems;
    case 2: return c_cableItems;
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
static void drawStructureGhostExtent(EStructureType type, const glm::vec3& groundPos, uint32 color,
    const glm::ivec2& ext)
{
    const glm::vec2 half(ext.x * StructureSystem::GridCellSize * 0.5f,
                         ext.y * StructureSystem::GridCellSize * 0.5f);
    const float height = StructureSystem::spawnHeightOf(type) * 2.0f;
    const glm::vec3 c(groundPos.x, 0.0f, groundPos.z);
    const glm::vec3 corner[4] = {
        c + glm::vec3(-half.x, 0.0f, -half.y), c + glm::vec3(half.x, 0.0f, -half.y),
        c + glm::vec3(half.x, 0.0f, half.y),   c + glm::vec3(-half.x, 0.0f, half.y) };
    const glm::vec3 up(0.0f, height, 0.0f);
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec3& a = corner[i];
        const glm::vec3& b = corner[(i + 1) % 4];
        Globals::rendererVK.addDebugLine(a, b, color);                 // base
        Globals::rendererVK.addDebugLine(a + up, b + up, color);       // top
        Globals::rendererVK.addDebugLine(a, a + up, color);            // riser
    }
    for (int i = 1; i < ext.x; ++i) // interior grid: which cells are taken
    {
        const float o = -half.x + i * StructureSystem::GridCellSize;
        Globals::rendererVK.addDebugLine(c + glm::vec3(o, 0.0f, -half.y), c + glm::vec3(o, 0.0f, half.y), color);
    }
    for (int i = 1; i < ext.y; ++i)
    {
        const float o = -half.y + i * StructureSystem::GridCellSize;
        Globals::rendererVK.addDebugLine(c + glm::vec3(-half.x, 0.0f, o), c + glm::vec3(half.x, 0.0f, o), color);
    }
}

static void drawStructureGhost(EStructureType type, const glm::vec3& groundPos, uint32 color)
{
    drawStructureGhostExtent(type, groundPos, color,
        glm::ivec2(StructureSystem::footprintCellsOf(type)));
}

// The PVP arenas (EPvpMap): rock-terrain layouts on a 5 m cell grid, each ringed by a 10 m rock
// border (the old borderwall.pre fence is gone). The Lane is the original corridor along X.
static constexpr float c_corridorHalfLength = 65.0f; // Lane/Wide/Chokepoints x extent of the play area
static constexpr float c_corridorHalfWidth = 20.0f;  // Lane z extent
static constexpr float c_pvpWideHalfWidth = 40.0f;   // Wide lane / Chokepoints z extent
static constexpr float c_pvpChokeWallHalf = 5.0f;    // Chokepoints: the middle wall's x half-width
static constexpr float c_pvpCircleRadius = 65.0f;    // Circle: the open disc
static constexpr float c_pvpCircleInner = 28.0f;     // Circle: the central rock column
static constexpr float c_pvpCircleBaseRadius = 47.0f; // Circle: the Bases sit mid-ring
static constexpr float c_pvpCellSize = 5.0f;         // rock.pre (a 10 m cube) spawned at half scale
static constexpr int c_pvpBorderCells = 2;           // 10 m of rock around every arena
static constexpr float c_pvpBaseClear = 8.0f;        // rock-free radius around every Base cell
// The CO-OP map: a big square centred on the shared Base, RANDOMLY GENERATED — impassable rock
// terrain over a coarse cell grid plus a player-blocking barrier ring at ±c_coopHalfSize (see
// GameMatch::generateCoopGrid). The ground plane (ground.pre) is 400 m, so ±200 is the hard edge;
// waves spawn in the open ring BETWEEN the barrier and the ground edge and walk in through it
// (the barrier's collider only matches the Player layer).
static constexpr float c_coopHalfSize = 180.0f;
static constexpr float c_coopCellSize = 10.0f; // terrain cell = one rock block (5x the 2 m grid)
static constexpr int c_coopCells = 36;         // cells per side
static_assert((float)c_coopCells * c_coopCellSize == c_coopHalfSize * 2.0f);
static constexpr float c_coopGroundEdge = 196.0f;  // spawn clamp just inside the 400 m ground
static constexpr float c_coopBaseClearRadius = 26.0f; // rock-free zone around the Base + starters
static constexpr float c_barrierStep = 20.0f;      // one barrier.pre segment (posts at centers)

// Deterministic map math: every instance must derive the IDENTICAL layout from the seed alone
// (the corridor-table contract), so the generator uses its own splitmix-style hash/RNG — never
// glm::linearRand, whose global engine state differs per instance.
static uint32 mapHash(uint32 x)
{
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    return x ^ (x >> 16);
}
struct MapRng
{
    uint32 state;
    uint32 next() { return state = mapHash(state); }
    float next01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};
// Value noise on the cell lattice: hashed corners, bilinear blend. Period in cells.
static float mapNoise(uint32 seed, float x, float z, float period)
{
    const float fx = x / period, fz = z / period;
    const int ix = (int)std::floor(fx), iz = (int)std::floor(fz);
    const float tx = fx - (float)ix, tz = fz - (float)iz;
    const auto corner = [&](int cx, int cz) {
        return (float)(mapHash(seed ^ mapHash((uint32)(cx * 73856093) ^ (uint32)(cz * 19349663))) >> 8)
            * (1.0f / 16777216.0f); };
    const float a = glm::mix(corner(ix, iz), corner(ix + 1, iz), tx);
    const float b = glm::mix(corner(ix, iz + 1), corner(ix + 1, iz + 1), tx);
    return glm::mix(a, b, tz);
}

GameMatch::GameMatch(bool enabled, bool coop) : m_coop(coop), m_enabled(enabled)
{
    if (!m_enabled)
        return;

    {
        // Gameplay tweaks persist between runs and the server's values overrule the clients'.
        const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
        Tweak::floatVar("Game/Coop", "First wave delay (s)", &m_waveFirstDelay, 5.0f, 600.0f, 5.0f);
        Tweak::floatVar("Game/Coop", "Wave interval (s)", &m_waveInterval, 10.0f, 600.0f, 5.0f);
        Tweak::intVar("Game/Coop", "Wave budget", &m_waveBudget, 1, 5000, 10);
        Tweak::floatVar("Game/Coop", "Wave budget growth", &m_waveBudgetGrowth, 0.0f, 1000.0f, 5.0f);
        Tweak::floatVar("Game/Coop", "Cost grunt", &m_waveCost[(int)ENpcType::Grunt], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost brute", &m_waveCost[(int)ENpcType::Brute], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost runner", &m_waveCost[(int)ENpcType::Runner], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost spitter", &m_waveCost[(int)ENpcType::Spitter], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost swarm", &m_waveCost[(int)ENpcType::Swarm], 0.1f, 100.0f, 0.5f);
        Tweak::intVar("Game/Coop", "Max enemy units", &m_waveMaxAlive, 1, 20000, 50);
        Tweak::intVar("Game/Coop", "Ambient budget", &m_ambientBudget, 0, 20000, 10);
        Tweak::floatVar("Game/Coop", "Ambient safe radius", &m_ambientSafeRadius, 10.0f, 200.0f, 1.0f);
        Tweak::intVar("Game/Coop", "Spawns per frame", &m_spawnsPerFrame, 1, 200, 1);
        // Map generation inputs, read once at generation on the AUTHORITY. Clients never read
        // them: the values actually used ride the GMp event (and the save) with the seed — a
        // joiner's tweak sync lands after the world replay, too late to drive generation.
        Tweak::intVar("Game/Coop", "Map seed", &m_mapSeedTweak, 0, 0x7fffffff, 1);
        Tweak::floatVar("Game/Coop", "Terrain fill", &m_terrainFill, 0.0f, 0.6f, 0.02f);
        Tweak::intVar("Game/Coop", "Terrain lanes", &m_terrainLanes, 2, 12, 1);
        Tweak::floatVar("Game/Construction", "Refill radius", &m_refillRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Refill rate", &m_refillRate, 0.5f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Construction", "Player build radius", &m_buildRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Player build rate", &m_playerBuildRate, 0.5f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Base heal radius", &m_baseHealRadius, 0.0f, 60.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Base heal/s", &m_baseHealRate, 0.0f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Melee damage/s", &m_meleeDps, 0.0f, 200.0f, 0.5f);
        Tweak::floatVar("Game/Player", "Melee radius", &m_meleeRadius, 0.0f, 12.0f, 0.25f);
        Tweak::floatVar("Game/Enemies/Steer", "Group cluster radius", &m_selectionClusterRadius, 2.0f, 60.0f, 0.5f);
    }
    m_camera.registerTweaks();
    m_player.registerTweaks();
    m_structures.registerTweaks();
    m_npcs.registerTweaks();
    Globals::navSystem.initialize(); // "Nav" tweaks + density staging (job system is up by now)
    // Co-op: players share team 0, the AI is team 1 — the force shaders/bakes shrink to fit. PvP
    // sets the default cap EXPLICITLY: exit-to-menu can chain a co-op session into a PvP one in
    // the same process, so the previous mode's count must never linger (a no-op when unchanged).
    Globals::forceSystem.setNumTeams(m_coop ? 2 : 8);

    // The game rosters (structures + units/projectiles) replace every world-wide spatial query:
    // they deregister through this ONE notification, which every removal path funnels into
    // (destroy requests, editor deletes, network despawns). Cleared in ~GameMatch — the world
    // outlives this object.
    Globals::world.setOnRootEntityRemoved([this](const Entity* entity)
    {
        m_structures.onWorldRootRemoved(entity);
        m_npcs.onWorldRootRemoved(entity);
    });
    // Route change -> the barracks' live units (the march index is KEPT and clamped: an appended
    // route continues where the unit was, a finished unit marches to the new tail).
    m_structures.onRouteLiveUnits = [this](uint32 sourceId, oc::span<const glm::vec3> route)
    {
        for (const EntityPtr& e : m_npcs.units())
            if (GameUnitComponent* u = getComponent<GameUnitComponent>(e.get()); u && u->sourceId == sourceId)
            {
                u->routeCount = (uint8)glm::min((int)route.size(), (int)GameUnitComponent::MaxRoutePoints);
                for (int i = 0; i < u->routeCount; ++i)
                    u->route[i] = route[i];
                u->routeIndex = (uint8)glm::min((int)u->routeIndex, glm::max((int)u->routeCount - 1, 0));
            }
    };

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
        {
            m_placeClicked = true;
            m_lmbDown = true;
            m_lmbDownPos = glm::vec2(float(evt.x), float(evt.y));
        }
    };
    m_mouse->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 2)
            m_mmbDown = false;
        if (evt.button == 3)
            m_rmbDown = false;
        if (evt.button == 1 && m_lmbDown)
        {
            m_lmbDown = false;
            m_lmbReleased = true;
        }
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
    // Exit-to-menu can destroy a GameMatch MID-RUN: every tweak registered on a member (the ctor's
    // Game/* block + camera/player/structures/npcs) must leave the registry with it, or the
    // per-frame poll reads freed memory. Statics (component params) stay and re-register in place.
    TweakRegistry::get().unregisterInRange(this, sizeof(GameMatch));
    Globals::navSystem.clear(); // waits on in-flight builds before the world goes
    m_npcs.clear();
    m_structures.clear();
    m_player.despawn();
    if (m_terrainRoot)
        Globals::world.removeRootEntity(m_terrainRoot.get()); // co-op rocks + barrier segments
    if (m_ground)
        Globals::world.removeRootEntity(m_ground.get()); // corridor walls are its children — they go with it
    Globals::world.setOnRootEntityRemoved(nullptr); // last: the callback captures this object
}

void GameMatch::spawnWorld()
{
    if (!m_enabled)
        return;
    m_isServer = Globals::networkManager.role() == ENetRole::Server;
    m_isClient = Globals::networkManager.role() == ENetRole::Client;
    if (m_coop)
    {
        // CO-OP: its own OPEN world — one shared Base at the center of a big wall-less map (the
        // corridor and its border ring are PvP-only). Waves come from any compass direction.
        m_basePos = glm::vec3(0.0f);
        m_playerStart = glm::vec3(0.0f, 1.0f, -6.0f);
        m_waveTimer = m_waveFirstDelay;
    }

    m_ground = Globals::world.spawnAssetFile("Entities/Game/ground.pre",
        Transform(glm::vec3(0.0f, -0.5f, 0.0f)), true); // box top = walkable y 0
    if (m_ground)
    {
        m_ground->setName("Ground");
        Globals::world.addRootEntity(m_ground);
    }

    // NOTE: no setOnGameEvent here — main.cpp owns the single dispatcher (lobby + game routing)
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
        // A completed blueprint re-fires GPl: cables sit outside the GSt stat sync, so this is the
        // only way a client learns a segment finished building (mirrorPlace applies it idempotently).
        m_structures.onStructureBuilt = [this](uint32 id)
        {
            if (const int index = m_structures.structureIndexById(id); index >= 0)
                sendStructurePlaced(index);
        };
        m_structures.onRouteChanged = [this](uint32 id)
        {
            seedRouteLane(id);
            if (const int index = m_structures.structureIndexById(id); index >= 0)
                sendRoute(index);
        };
        // (+ the App layer's text chat "ChM" — a string up to 256B; App.Chat's c_maxEventBytes)
        Globals::networkManager.setEventFilter([](uint32, oc::string_view name, oc::span<const uint8> data, Entity*)
        {
            return (name.size() >= 2 && name[0] == 'G' && name[1] == 'q' && data.size() <= 64)
                || (name == "ChM" && data.size() <= 256);
        });
    }

    if (!m_isServer && !m_isClient) // single player: the server binding above did not run
        m_structures.onRouteChanged = [this](uint32 id) { seedRouteLane(id); };
    if (m_coop)
    {
        // The generated co-op map (terrain + barrier + nodes): the AUTHORITY rolls the seed and
        // builds now; a CLIENT builds the identical set locally when the server's GMp event
        // delivers the seed (first thing in its join replay). Placement bounds = inside the
        // barrier either way — cellsFree needs them before the map lands.
        m_structures.setPlacementBounds(glm::vec2(-c_coopHalfSize), glm::vec2(c_coopHalfSize));
        if (!m_isClient)
        {
            uint32 seed = (uint32)m_mapSeedTweak;
            if (seed == 0) // tweak 0 = roll a fresh map every run
                seed = (std::random_device{}() & 0x7fffffffu) | 1u;
            rebuildCoopMap(seed, m_terrainFill, m_terrainLanes);
        }
    }
    else
    {
        // PvP ARENA: rock terrain + nodes from the lobby's map pick (setPvpMap). The AUTHORITY
        // builds now; a CLIENT builds the identical layout when the server's GMp event names the
        // arena (first in its join replay) — nodes included, so extractor node indices agree.
        // Until then the Lane's bounds stand in (cellsFree needs some before the map lands).
        if (!m_isClient)
            rebuildPvpMap(m_pvpMap);
        else
            m_structures.setPlacementBounds(
                glm::vec2(-c_corridorHalfLength, -c_corridorHalfWidth),
                glm::vec2(c_corridorHalfLength, c_corridorHalfWidth));
    }
    if (!m_isClient)
    {
        if (!m_coop)
        {
            // One Base per playable team (clients get them through the GPl mirror stream); the
            // server's own capsule sits beside ITS lobby-picked team's Base.
            for (uint8 t = 0; t < m_numTeams; ++t)
                m_structures.spawnBase(baseGroundPos(t), t);
            m_team = allocateClientTeam(0);
            m_player.setTeam(m_team);
            m_playerStart = teamStartPos((uint8)m_team);
        }
        else
        {
            m_structures.spawnBase(m_basePos);
            m_ambientPendingBudget = (float)m_ambientBudget; // the scatter's points, trickled in (tickCoopSpawns)
        }
        m_player.spawn(m_playerStart);     // clients ADOPT the capsule the server spawns for them
        // The server's own capsule is a PRIMARY: never handed to a client by the proximity
        // transfer, and it re-claims transferred objects it walks up to (the client symmetric).
        if (m_isServer && m_player.entity())
            Globals::networkManager.setServerPrimary(*m_player.entity(), true);
    }

    // Spawn view faces the MAP CENTER from wherever this instance's player starts (each end of
    // the corridor looks inward at the other team; co-op starts everyone at the central Base).
    const glm::vec3 cameraAnchor = m_isClient && !m_coop
        ? m_enemyBasePos + glm::vec3(0.0f, 1.0f, -6.0f) : m_playerStart;
    m_camera.setYawToward(glm::vec3(0.0f) - cameraAnchor);

    // The grid hotbar is always up in game mode: the ROOT page in Select/Delete, a category page in
    // Build. refreshBuildHotbar rebuilds it every windowed frame.
    Globals::gameHud.setHotbarLayout(4, c_gridKeyLabels);
    Globals::gameHud.setHotbarVisible(true);
    m_buildCategory = -1;
    refreshBuildHotbar();

    Log::info("Game mode: SELECT by default (click inspects, RMB sets barracks routes / moves the "
              "player). Grid hotkeys QWER/ASDF/ZXCV or click the slots: Q/W/E = build categories "
              "(Combat/Production/Cables), X = delete, C = cancel. Cables are physical: paint or "
              "two-click a run between buildings to connect them");
    if (m_coop)
        Log::info("CO-OP: defend the central Base — swarm waves attack periodically, and the map "
                  "is crawling with scattered enemies to clear as you expand");
}

// ---- CO-OP director -------------------------------------------------------------------------

// Wave ARCHETYPES: every wave rolls ONE recipe — a named mix of up to 4 unit types — gated by the
// wave index so early waves stay simple and the heavy stuff unlocks over time. Single-type rushes
// and combined-arms mixes both happen, but never every type in every wave; queueWave jitters the
// picked recipe's weights so two waves of the same archetype still differ.
namespace
{
    struct WaveArchetype
    {
        const char* name;
        int minWave; // first wave index this recipe can roll (m_waveIndex is 1-based at roll time)
        struct { ENpcType type; float weight; } mix[4]; // weight 0 = unused slot
    };
    constexpr WaveArchetype c_waveArchetypes[] = {
        { "swarm",           1, { { ENpcType::Swarm, 1.0f } } },
        { "swarm + runners", 2, { { ENpcType::Swarm, 0.75f }, { ENpcType::Runner, 0.25f } } },
        { "grunt push",      2, { { ENpcType::Grunt, 0.65f }, { ENpcType::Swarm, 0.35f } } },
        { "runner rush",     3, { { ENpcType::Runner, 1.0f } } },
        { "spitter siege",   4, { { ENpcType::Spitter, 0.3f }, { ENpcType::Swarm, 0.7f } } },
        { "brute hammer",    5, { { ENpcType::Brute, 0.25f }, { ENpcType::Swarm, 0.75f } } },
        { "combined arms",   6, { { ENpcType::Grunt, 0.3f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.2f }, { ENpcType::Swarm, 0.25f } } },
        { "brute wall",      7, { { ENpcType::Brute, 0.85f }, { ENpcType::Spitter, 0.15f } } },
        { "the works",       8, { { ENpcType::Swarm, 0.4f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.15f }, { ENpcType::Brute, 0.2f } } },
    };
    constexpr int c_numWaveArchetypes = (int)(sizeof(c_waveArchetypes) / sizeof(c_waveArchetypes[0]));
    constexpr int c_maxArchetypeMinWave = [] {
        int m = 1;
        for (const WaveArchetype& a : c_waveArchetypes)
            m = a.minWave > m ? a.minWave : m;
        return m;
    }();

    // One unit from an archetype's authored mix (the ambient scatter's per-spawn roll — the wave
    // path samples its stored, jittered copy instead).
    ENpcType sampleArchetype(const WaveArchetype& arch)
    {
        float total = 0.0f;
        for (const auto& e : arch.mix)
            total += e.weight;
        float r = glm::linearRand(0.0f, glm::max(total, 1e-3f));
        for (const auto& e : arch.mix)
        {
            r -= e.weight;
            if (e.weight > 0.0f && r <= 0.0f)
                return e.type;
        }
        return ENpcType::Swarm;
    }
}

ENpcType GameMatch::sampleMix(const oc::fixed_vector<WaveMixEntry, 4>& mix) const
{
    float total = 0.0f;
    for (const WaveMixEntry& e : mix)
        total += e.weight;
    if (total <= 0.0f)
        return ENpcType::Swarm; // no rolled mix (shouldn't happen): the classic mass
    float r = glm::linearRand(0.0f, total);
    for (const WaveMixEntry& e : mix)
    {
        r -= e.weight;
        if (r <= 0.0f)
            return e.type;
    }
    return mix.back().type;
}

// The wave clock (authority, co-op only). The actual entity spawns are TRICKLED by tickCoopSpawns
// — a They-are-Billions-sized wave materialized in one frame would be a multi-hundred-spawn hitch.
void GameMatch::tickWaves(float deltaSec)
{
    m_waveTimer -= deltaSec;
    if (m_waveTimer > 0.0f)
        return;
    m_waveTimer = m_waveInterval;
    queueWave();
}

void GameMatch::queueWave()
{
    int aiAlive = 0; // ambient + previous waves both count against the cap
    for (const EntityPtr& e : m_npcs.units())
        if (const GameUnitComponent* u = getComponent<GameUnitComponent>(e.get());
            u && u->team == (uint32)CoopAiTeam && u->alive())
            ++aiAlive;
    // BUDGET points, not a unit count: the alive cap converts conservatively at the CHEAPEST cost
    // (the worst-case body count a budget could buy).
    float cheapest = FLT_MAX;
    for (int t = 0; t < (int)ENpcType::Count; ++t)
        cheapest = glm::min(cheapest, waveCostOf((ENpcType)t));
    const float budget = glm::min((float)m_waveBudget + m_waveBudgetGrowth * (float)m_waveIndex,
        (float)(m_waveMaxAlive - aiAlive) * cheapest - m_ambientPendingBudget - m_wavePendingBudget);
    ++m_waveIndex;
    if (budget <= 0.0f)
        return; // at the cap: the clock (and the scaling) still advanced
    // A random compass direction, projected onto the barrier square and stepped OUTSIDE it: the
    // swarm clusters in the open ring beyond the barrier (its collider only matches players) and
    // pushes at the Base's near face (a point INSIDE the footprint would fail the A* and the move
    // order alike — the pointOutsideFootprint rule). The locked order releases on arrival; the AI
    // takes over there. The carved lanes guarantee a route in from any side.
    const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
    const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
    const float k = (c_coopHalfSize + 8.0f) / glm::max(glm::abs(dir.x), glm::abs(dir.z));
    m_waveOrigin = dir * k;
    m_waveDest = dir * 6.0f;
    m_wavePendingBudget += budget;
    // Roll this wave's COMPOSITION among the archetypes the index has unlocked — never the same
    // recipe twice in a row when a choice exists — then jitter its weights so repeats still vary.
    int eligible[c_numWaveArchetypes];
    int numEligible = 0;
    for (int i = 0; i < c_numWaveArchetypes; ++i)
        if (m_waveIndex >= c_waveArchetypes[i].minWave && (i != m_lastArchetype || numEligible == 0))
            eligible[numEligible++] = i;
    if (numEligible > 1 && eligible[0] == m_lastArchetype) // slot 0 was only a can't-be-empty seed
    {
        eligible[0] = eligible[numEligible - 1];
        --numEligible;
    }
    const int pick = eligible[glm::clamp((int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible),
        0, numEligible - 1)];
    m_lastArchetype = pick;
    m_waveMix.clear();
    for (const auto& entry : c_waveArchetypes[pick].mix)
        if (entry.weight > 0.0f)
            m_waveMix.push_back({ entry.type, entry.weight * glm::linearRand(0.6f, 1.4f) });
    // One planned lane from the spawn ring to the Base — the swarm commits to it, and the units'
    // own periodic seed requests keep it fresh (Nav's proximity dedup makes the wave one plan).
    Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    Log::info(oc::format("Co-op: wave {} incoming — budget {:.0f} ({}) from ({:.0f}, {:.0f})", m_waveIndex,
        budget, c_waveArchetypes[pick].name, m_waveOrigin.x, m_waveOrigin.z));
    if (m_isServer)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint16>((uint16)m_waveIndex);
        Globals::networkManager.fireNetworkEvent("GWv", writer.data());
    }
}

// The trickle spawner (authority, per frame): a fixed budget of entity spawns serves the wave
// first, then the world-start ambient scatter — thousands of units enter the world over a few
// seconds instead of one giant frame hitch.
void GameMatch::tickCoopSpawns()
{
    // The frame's spawns are ROLLED first (budget math + RNG stay serial on main — glm's linearRand
    // is not thread-safe), then materialized in ONE NpcSystem::spawnLooseUnits batch: the entity
    // creations fan out over the job system instead of running one by one.
    oc::small_vector<NpcSystem::LooseSpawn, 64> spawns;
    int budget = glm::max(m_spawnsPerFrame, 1);
    while (budget > 0 && m_wavePendingBudget > 0.0f)
    {
        --budget;
        // Roll the type from the wave's mix, then SPEND its cost. A roll the remaining budget
        // can't afford downgrades to the cheapest type in the mix; if even that doesn't fit, the
        // wave is done (the tail rounds down instead of overspending into the next wave's points).
        ENpcType type = sampleMix(m_waveMix);
        if (waveCostOf(type) > m_wavePendingBudget)
        {
            for (const WaveMixEntry& e : m_waveMix)
                if (waveCostOf(e.type) < waveCostOf(type))
                    type = e.type;
            if (waveCostOf(type) > m_wavePendingBudget)
            {
                m_wavePendingBudget = 0.0f;
                break;
            }
        }
        m_wavePendingBudget -= waveCostOf(type);
        // Cluster around the ring point — bigger remaining waves spread over a wider blob. The
        // blob stays in the band OUTSIDE the barrier but inside the ground plane: a point that
        // drifted through the barrier line pushes back out along the wave's dominant axis.
        const float a = glm::linearRand(0.0f, glm::two_pi<float>());
        const float r = glm::min(8.0f + m_wavePendingBudget * 0.05f, 30.0f)
            * std::sqrt(glm::linearRand(0.0f, 1.0f));
        glm::vec3 pos = m_waveOrigin + glm::vec3(std::cos(a) * r, 1.0f, std::sin(a) * r);
        pos.x = glm::clamp(pos.x, -c_coopGroundEdge, c_coopGroundEdge);
        pos.z = glm::clamp(pos.z, -c_coopGroundEdge, c_coopGroundEdge);
        if (glm::abs(pos.x) < c_coopHalfSize + 1.5f && glm::abs(pos.z) < c_coopHalfSize + 1.5f)
        {
            if (glm::abs(m_waveOrigin.x) >= glm::abs(m_waveOrigin.z))
                pos.x = glm::sign(m_waveOrigin.x) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
            else
                pos.z = glm::sign(m_waveOrigin.z) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
        }
        spawns.push_back({ .pos = pos,
            .orderDest = m_waveDest + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f)),
            .type = type, .team = (uint8)CoopAiTeam, .hasOrder = true });
    }
    while (budget > 0 && m_ambientPendingBudget > 0.0f)
    {
        --budget;
        // AMBIENT units come in CAMPS: a cluster anchored on a random REACHABLE open cell of the
        // generated map (uniform by area, outside the safe ring, reachable from the Base by the
        // flood fill's guarantee), its bodies scattered in a disc around the anchor and slid off
        // any rock edge — a loose blob per camp instead of one body per cell, which lined up on
        // the 10 m lattice. The Nav team fields never pull them (they cover the whole map, which
        // marched every scattered unit to the base) — only the local search aggroes them, so a
        // camp holds its patch until players expand near it. Wave units above stay field-driven
        // after their order releases.
        if (m_coopMap.reachable.empty())
            break;
        if (m_ambientCamp.remaining <= 0)
        {
            const int cell = m_coopMap.reachable[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)m_coopMap.reachable.size()),
                0, (int)m_coopMap.reachable.size() - 1)];
            const glm::vec3 center = coopCellCenter(cell);
            if (glm::length(glm::vec2(center.x, center.z)) < m_ambientSafeRadius)
                continue; // safe-ring reject: costs one budget tick, never the points
            // DISTANCE = DIFFICULTY: the camp's archetype is gated by GEODESIC depth (BFS distance
            // from the Base over the generated map) exactly like waves gate by index — the near
            // ring only rolls the early recipes (swarm-grade), the deep map unlocks the whole
            // table (brute walls, combined arms). Costs then make far camps FEWER, TOUGHER
            // bodies for the same points. One recipe per camp, so a camp reads as a unit type
            // holding ground rather than a random assortment.
            const float depth = (float)m_coopMap.depth[cell] / (float)m_coopMap.maxDepth;
            const int band = 1 + (int)(depth * (float)(c_maxArchetypeMinWave - 1) + 0.5f);
            int eligible[c_numWaveArchetypes];
            int numEligible = 0;
            for (int i = 0; i < c_numWaveArchetypes; ++i)
                if (c_waveArchetypes[i].minWave <= band)
                    eligible[numEligible++] = i;
            m_ambientCamp.archetype = eligible[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible), 0, numEligible - 1)];
            m_ambientCamp.center = center + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f));
            m_ambientCamp.radius = glm::linearRand(4.0f, 9.0f);
            m_ambientCamp.remaining = (int)glm::linearRand(3.0f, 9.99f);
        }
        --m_ambientCamp.remaining;
        ENpcType type = sampleArchetype(c_waveArchetypes[m_ambientCamp.archetype]);
        if (waveCostOf(type) > m_ambientPendingBudget)
        {
            type = ENpcType::Swarm; // the tail rounds down to the cheapest body
            if (waveCostOf(type) > m_ambientPendingBudget)
            {
                m_ambientPendingBudget = 0.0f;
                break;
            }
        }
        m_ambientPendingBudget -= waveCostOf(type);
        spawns.push_back({ .pos = ambientPointNear(m_ambientCamp.center, m_ambientCamp.radius),
            .type = type, .team = (uint8)CoopAiTeam });
    }
    m_npcs.spawnLooseUnits(oc::span<const NpcSystem::LooseSpawn>(spawns.data(), spawns.size()));
}

// ---- Generated terrain (co-op map + PvP arenas) ---------------------------------------------

glm::vec3 GameMatch::coopCellCenter(int index) const
{
    const int x = index % m_coopMap.cellsX, z = index / m_coopMap.cellsX;
    return glm::vec3(m_coopMap.origin.x + ((float)x + 0.5f) * m_coopMap.cellSize, 0.0f,
                     m_coopMap.origin.y + ((float)z + 0.5f) * m_coopMap.cellSize);
}

int GameMatch::coopCellAt(const glm::vec3& pos) const
{
    const int x = (int)std::floor((pos.x - m_coopMap.origin.x) / m_coopMap.cellSize);
    const int z = (int)std::floor((pos.z - m_coopMap.origin.y) / m_coopMap.cellSize);
    if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
        return -1;
    return z * m_coopMap.cellsX + x;
}

// The PvP arena layouts: pure functions of (arena, team count) — identical on every instance.
// A rock BORDER ring surrounds each open area; the flood fill from team 0's Base seals anything
// the layout cut off, and every Base cell gets a rock-free disc (a middle team's Base on the
// Chokepoints wall line carves its own gap).
void GameMatch::generatePvpGrid()
{
    CoopMap& map = m_coopMap;
    map.pvp = true;
    map.cellSize = c_pvpCellSize;
    glm::vec2 half(c_corridorHalfLength, c_corridorHalfWidth);
    switch (map.pvpMap)
    {
    case EPvpMap::WideLane:
    case EPvpMap::Chokepoints: half.y = c_pvpWideHalfWidth; break;
    case EPvpMap::Circle:      half = glm::vec2(c_pvpCircleRadius); break;
    default: break;
    }
    m_pvpInterior = half;
    map.cellsX = (int)std::lround(half.x * 2.0f / c_pvpCellSize) + 2 * c_pvpBorderCells;
    map.cellsZ = (int)std::lround(half.y * 2.0f / c_pvpCellSize) + 2 * c_pvpBorderCells;
    map.origin = -half - glm::vec2((float)c_pvpBorderCells * c_pvpCellSize);
    map.blocked.assign((size_t)map.cellsX * map.cellsZ, 0);
    for (int i = 0; i < map.cellsX * map.cellsZ; ++i)
    {
        const glm::vec3 c = coopCellCenter(i);
        bool open = glm::abs(c.x) < half.x && glm::abs(c.z) < half.y;
        if (map.pvpMap == EPvpMap::Circle)
        {
            const float r = glm::length(glm::vec2(c.x, c.z));
            open = r < c_pvpCircleRadius && r > c_pvpCircleInner;
        }
        else if (map.pvpMap == EPvpMap::Chokepoints && glm::abs(c.x) < c_pvpChokeWallHalf)
        {
            // the middle wall, pierced at the center and near each side: three 10 m openings
            const float az = glm::abs(c.z);
            open = az < 5.0f || (az > 20.0f && az < 30.0f);
        }
        map.blocked[i] = open ? 0 : 1;
    }
    oc::vector<int> seeds;
    for (uint8 t = 0; t < m_numTeams; ++t)
    {
        const glm::vec3 base = baseGroundPos(t);
        for (int i = 0; i < map.cellsX * map.cellsZ; ++i)
        {
            const glm::vec3 c = coopCellCenter(i);
            if (glm::length(glm::vec2(c.x - base.x, c.z - base.z)) < c_pvpBaseClear)
                map.blocked[i] = 0;
        }
        if (t == 0)
            seeds.push_back(coopCellAt(base));
    }
    finishGrid(seeds);
}

// The pure-math half: identical on every instance from (seed, fill, lanes) alone — MapRng only,
// never glm::linearRand (global engine state). Produces blocked cells, the BFS depth/reachable
// sets and the merged obstacle rects.
void GameMatch::generateCoopGrid()
{
    CoopMap& map = m_coopMap;
    const int n = c_coopCells;
    map.cellsX = map.cellsZ = n;
    map.cellSize = c_coopCellSize;
    map.origin = glm::vec2(-c_coopHalfSize);
    map.pvp = false;
    map.blocked.assign((size_t)n * n, 0);
    MapRng rng{ map.seed ? map.seed : 1u };
    const uint32 noiseA = rng.next(), noiseB = rng.next();

    // Two-octave value noise per cell, thresholded to EXACTLY the requested fill fraction (the
    // threshold is read off the sorted copy, so "Terrain fill" means what it says at any seed).
    oc::vector<float> value((size_t)n * n);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
            value[(size_t)z * n + x] = 0.65f * mapNoise(noiseA, (float)x + 0.5f, (float)z + 0.5f, 5.5f)
                                     + 0.35f * mapNoise(noiseB, (float)x + 0.5f, (float)z + 0.5f, 2.5f);
    const float fill = glm::clamp(map.fill, 0.0f, 0.6f);
    oc::vector<float> sorted = value;
    oc::sort(sorted.begin(), sorted.end());
    const float threshold = sorted[glm::clamp((int)((1.0f - fill) * (float)sorted.size()),
        0, (int)sorted.size() - 1)];
    for (size_t i = 0; i < value.size(); ++i)
        map.blocked[i] = fill > 0.0f && value[i] >= threshold ? 1 : 0;

    // The Base zone stays rock-free (Base footprint + starter node pair + breathing room).
    for (int i = 0; i < n * n; ++i)
    {
        const glm::vec3 c = coopCellCenter(i);
        if (glm::length(glm::vec2(c.x, c.z)) < c_coopBaseClearRadius)
            map.blocked[i] = 0;
    }

    // Carved ATTACK LANES: evenly spread directions (jittered) wobbling from the base ring out
    // past the barrier — they guarantee the edge ring connects to the Base (waves walk in through
    // the openings) and give the noise blobs natural chokepoints to defend.
    const int lanes = glm::clamp(map.lanes, 2, 12);
    const auto clearAt = [&](const glm::vec2& p) // ~1.5 cells wide: the cell + its 4 neighbours
    {
        const int cell = coopCellAt(glm::vec3(p.x, 0.0f, p.y));
        if (cell < 0)
            return;
        map.blocked[cell] = 0;
        const int x = cell % n, z = cell / n;
        if (x > 0)     map.blocked[cell - 1] = 0;
        if (x < n - 1) map.blocked[cell + 1] = 0;
        if (z > 0)     map.blocked[cell - n] = 0;
        if (z < n - 1) map.blocked[cell + n] = 0;
    };
    for (int i = 0; i < lanes; ++i)
    {
        const float a = ((float)i + rng.next01() * 0.7f) / (float)lanes * glm::two_pi<float>();
        const float phase = rng.next01() * glm::two_pi<float>();
        const glm::vec2 dir(std::cos(a), std::sin(a));
        const glm::vec2 perp(-dir.y, dir.x);
        for (float r = c_coopBaseClearRadius - 6.0f; r < c_coopHalfSize * 1.5f; r += c_coopCellSize * 0.35f)
        {
            const glm::vec2 p = dir * r + perp * (std::sin(r * 0.045f + phase) * 10.0f);
            if (glm::abs(p.x) > c_coopHalfSize || glm::abs(p.y) > c_coopHalfSize)
                break;
            clearAt(p);
        }
    }

    const int c0 = n / 2 - 1, c1 = n / 2;
    const int seeds[] = { c0 * n + c0, c0 * n + c1, c1 * n + c0, c1 * n + c1 };
    finishGrid(seeds);
}

// Flood fill from the seed cells (4-connected BFS = geodesic cell distance). Any open cell it
// never reaches becomes rock — so EVERY open cell is reachable from the Base by construction, and
// everything placed on open ground (nodes, ambient camps, move orders) is reachable too. Then the
// merged obstacle rects.
void GameMatch::finishGrid(oc::span<const int> seedCells)
{
    CoopMap& map = m_coopMap;
    const int n = map.cellsX, nz = map.cellsZ;
    map.depth.assign((size_t)n * nz, 0xFFFF);
    map.reachable.clear();
    map.maxDepth = 1;
    oc::vector<int> queue;
    queue.reserve((size_t)n * nz);
    for (const int seedCell : seedCells)
        if (seedCell >= 0 && !map.blocked[seedCell] && map.depth[seedCell] == 0xFFFF)
        {
            map.depth[seedCell] = 0;
            queue.push_back(seedCell);
        }
    for (size_t head = 0; head < queue.size(); ++head)
    {
        const int cell = queue[head];
        const int x = cell % n, z = cell / n;
        const uint16 d = (uint16)(map.depth[cell] + 1);
        const auto visit = [&](int next)
        {
            if (!map.blocked[next] && map.depth[next] == 0xFFFF)
            {
                map.depth[next] = d;
                queue.push_back(next);
            }
        };
        if (x > 0)     visit(cell - 1);
        if (x < n - 1) visit(cell + 1);
        if (z > 0)     visit(cell - n);
        if (z < nz - 1) visit(cell + n);
    }
    for (int i = 0; i < n * nz; ++i)
    {
        if (map.blocked[i])
            continue;
        if (map.depth[i] == 0xFFFF)
        {
            map.blocked[i] = 1; // sealed pocket: fill it in rather than leave unreachable ground
            continue;
        }
        map.reachable.push_back(i);
        map.maxDepth = map.depth[i] > map.maxDepth ? map.depth[i] : map.maxDepth;
    }

    // Merged obstacle rects (horizontal runs): one rect serves Nav, placement (cellsFree) and the
    // spawn probes — a few hundred instead of ~1300 per-cell entries.
    m_terrainRects.clear();
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < n; )
        {
            if (!map.blocked[z * n + x])
            {
                ++x;
                continue;
            }
            int end = x;
            while (end < n && map.blocked[z * n + end])
                ++end;
            m_terrainRects.push_back(glm::vec4(
                map.origin.x + (float)x * map.cellSize, map.origin.y + (float)z * map.cellSize,
                map.origin.x + (float)end * map.cellSize, map.origin.y + (float)(z + 1) * map.cellSize));
            x = end;
        }
}

// Rocks (+ the co-op barrier ring) under ONE root entity (removed whole on regeneration), then
// the resource nodes on reachable open cells. Deterministic: MapRng seeded off the map seed (a
// PvP arena has none — its rock heights roll from the arena index).
void GameMatch::spawnTerrain()
{
    MapRng rng{ mapHash((m_coopMap.pvp ? (uint32)m_coopMap.pvpMap + 17u : m_coopMap.seed) ^ 0xC0FFEEu) };
    const float rockScale = m_coopMap.cellSize / c_coopCellSize; // rock.pre/terrainmark.pre are 10 m cells
    // A Scene-only prefab, NOT createEmptyEntity: that one has no components at all, and an
    // entity without a SceneComponent refuses children (the pieces would end up unparented).
    m_terrainRoot = Globals::world.spawnAssetFile("Entities/Game/terrainroot.pre", Transform(), true);
    if (m_terrainRoot)
    {
        m_terrainRoot->setName(m_coopMap.pvp ? "ArenaTerrain" : "CoopTerrain");
        Globals::world.addRootEntity(m_terrainRoot);
    }

    oc::vector<World::SpawnRequest> requests;
    requests.reserve(m_coopMap.reachable.size() / 2 + 128);
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < m_coopMap.cellsX * m_coopMap.cellsZ; ++i)
    {
        if (!m_coopMap.blocked[i])
            continue;
        // The rock is a cube (10 m × rockScale) sunk a random depth: exposed height 3.5..6 m for
        // the 10 m co-op block, 3..4.5 m for a 5 m arena block (always above the player's jump,
        // never floating), tops varying so a field of them reads as terrain, not tiling.
        const float exposed = m_coopMap.pvp ? 3.0f + rng.next01() * 1.5f : 3.5f + rng.next01() * 2.5f;
        requests.push_back({ "Entities/Game/rock.pre",
            Transform(coopCellCenter(i) + glm::vec3(0.0f, exposed - 5.0f * rockScale, 0.0f), rockScale, identity) });
        // + the ground marker: a tinted plane wider than the block outlines the blocked cell
        requests.push_back({ "Entities/Game/terrainmark.pre", Transform(coopCellCenter(i), rockScale, identity) });
    }
    const size_t rockCount = requests.size();
    // The co-op barrier ring: 20 m segments on all four sides (E/W rotated 90° — the prefab's
    // long axis is X). Collider layer Barrier vs Player only: units and shots pass, capsules do
    // not. A PvP arena needs none — its rock border is solid for everyone.
    const glm::quat yaw90 = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    for (float o = -c_coopHalfSize + c_barrierStep * 0.5f; !m_coopMap.pvp && o < c_coopHalfSize; o += c_barrierStep)
    {
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(o, 10.0f, -c_coopHalfSize)) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(o, 10.0f, c_coopHalfSize)) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(-c_coopHalfSize, 10.0f, o), 1.0f, yaw90) });
        requests.push_back({ "Entities/Game/barrier.pre", Transform(glm::vec3(c_coopHalfSize, 10.0f, o), 1.0f, yaw90) });
    }
    oc::vector<EntityPtr> spawned = Globals::world.spawnBatch(requests, /*addRoots*/ false);
    for (size_t i = 0; i < spawned.size(); ++i)
    {
        if (!spawned[i])
            continue;
        spawned[i]->setName(i >= rockCount ? "Barrier" : (i & 1) ? "RockMark" : "Rock");
        if (m_terrainRoot)
            spawned[i]->reparentEntity(m_terrainRoot.get()); // root at origin: world pos == local
        else
            Globals::world.addRootEntity(oc::move(spawned[i]));
    }

    if (m_coopMap.pvp)
    {
        spawnPvpNodes();
        return;
    }
    // RESOURCE NODES: the starter pair in the cleared Base zone, then a golden-angle spiral of
    // candidates SNAPPED to reachable open cells (skip when none nearby or too close to another
    // node) — even coverage that respects whatever the noise carved, and reachable by the flood
    // fill's guarantee.
    m_structures.spawnNode(10.0f, 4.0f, ENodeType::Mineral);
    m_structures.spawnNode(-4.0f, 10.0f, ENodeType::Fuel);
    oc::vector<glm::vec2> placed = { glm::vec2(10.0f, 4.0f), glm::vec2(-4.0f, 10.0f) };
    constexpr int c_nodeTarget = 48;
    int placedCount = 0;
    for (int i = 0; i < 120 && placedCount < c_nodeTarget; ++i)
    {
        const float t = ((float)i + 0.5f) / 120.0f;
        const float r = glm::mix(34.0f, c_coopHalfSize - 16.0f, std::sqrt(t)); // sqrt = uniform area
        const float a = (float)i * 2.3999632f; // the golden angle
        const glm::vec3 want(std::cos(a) * r, 0.0f, std::sin(a) * r);
        // Snap to the nearest reachable open cell around the candidate (2 rings).
        int bestCell = -1;
        float bestDistSq = FLT_MAX;
        const int wantCell = coopCellAt(want);
        if (wantCell < 0)
            continue;
        const int wx = wantCell % m_coopMap.cellsX, wz = wantCell / m_coopMap.cellsX;
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int x = wx + dx, z = wz + dz;
                if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
                    continue;
                const int cell = z * m_coopMap.cellsX + x;
                if (m_coopMap.blocked[cell]) // post-flood: open == reachable
                    continue;
                const glm::vec3 c = coopCellCenter(cell);
                const float distSq = glm::dot(glm::vec2(c.x - want.x, c.z - want.z),
                                              glm::vec2(c.x - want.x, c.z - want.z));
                if (distSq < bestDistSq)
                {
                    bestDistSq = distSq;
                    bestCell = cell;
                }
            }
        if (bestCell < 0)
            continue;
        const glm::vec3 center = coopCellCenter(bestCell);
        const glm::vec2 pos(center.x + (rng.next01() * 4.0f - 2.0f),
                            center.z + (rng.next01() * 4.0f - 2.0f));
        bool spaced = true;
        for (const glm::vec2& q : placed)
            if (glm::dot(pos - q, pos - q) < 13.0f * 13.0f)
            {
                spaced = false;
                break;
            }
        if (!spaced)
            continue;
        m_structures.spawnNode(pos.x, pos.y, (placedCount % 3) == 1 ? ENodeType::Fuel : ENodeType::Mineral);
        placed.push_back(pos);
        ++placedCount;
    }
}

// The arena node tables: the lane's 180°-symmetric set (every node on a SIDE, the center empty,
// each side's forward fuel node the exposed prize), widened with an outer band on the wide
// arenas; the Circle rings its nodes between the Bases. A node whose cell is rock is skipped.
void GameMatch::spawnPvpNodes()
{
    const auto nodeIfOpen = [this](float x, float z, ENodeType type)
    {
        const int cell = coopCellAt(glm::vec3(x, 0.0f, z));
        if (cell >= 0 && !m_coopMap.blocked[cell])
            m_structures.spawnNode(x, z, type);
    };
    if (m_coopMap.pvpMap == EPvpMap::Circle)
    {
        for (int k = 0; k < 6; ++k)
        {
            const float a = ((float)k + 0.5f) * glm::pi<float>() / 3.0f; // between the two-team Bases
            nodeIfOpen(std::cos(a) * 38.0f, std::sin(a) * 38.0f, (k & 1) ? ENodeType::Fuel : ENodeType::Mineral);
            nodeIfOpen(std::cos(a) * 56.0f, std::sin(a) * 56.0f, (k & 1) ? ENodeType::Mineral : ENodeType::Fuel);
        }
        return;
    }
    static constexpr struct { float x, z; ENodeType type; } c_laneNodes[] = {
        { -45.0f,   8.0f, ENodeType::Mineral }, {  45.0f,  -8.0f, ENodeType::Mineral },
        { -37.0f, -10.0f, ENodeType::Fuel },    {  37.0f,  10.0f, ENodeType::Fuel },
        { -25.0f,  12.0f, ENodeType::Mineral }, {  25.0f, -12.0f, ENodeType::Mineral },
        { -14.0f, -10.0f, ENodeType::Fuel },    {  14.0f,  10.0f, ENodeType::Fuel },
    };
    static constexpr struct { float x, z; ENodeType type; } c_wideNodes[] = {
        { -40.0f,  30.0f, ENodeType::Fuel },    {  40.0f, -30.0f, ENodeType::Fuel },
        { -18.0f,  32.0f, ENodeType::Mineral }, {  18.0f, -32.0f, ENodeType::Mineral },
        { -50.0f, -28.0f, ENodeType::Mineral }, {  50.0f,  28.0f, ENodeType::Mineral },
    };
    for (const auto& n : c_laneNodes)
        nodeIfOpen(n.x, n.z, n.type);
    if (m_coopMap.pvpMap != EPvpMap::Lane)
        for (const auto& n : c_wideNodes)
            nodeIfOpen(n.x, n.z, n.type);
}

void GameMatch::rebuildPvpMap(EPvpMap map)
{
    if (m_coopMap.built && m_coopMap.pvp && m_coopMap.pvpMap == map)
        return; // the join-replay re-broadcast / a duplicate GMp
    if (m_terrainRoot)
    {
        Globals::world.removeRootEntity(m_terrainRoot.get());
        m_terrainRoot = {};
    }
    m_structures.clearNodes();
    m_pvpMap = map;
    m_coopMap.pvpMap = map;
    generatePvpGrid();
    m_coopMap.built = true;
    spawnTerrain();
    m_wallObstacles.clear();
    for (const glm::vec4& r : m_terrainRects)
        m_wallObstacles.push_back(Nav::NavObstacle{ glm::vec2(r.x, r.y), glm::vec2(r.z, r.w) });
    m_structures.setTerrainBlocked(m_terrainRects);
    m_structures.setPlacementBounds(-m_pvpInterior, m_pvpInterior); // inside the rock border
    Log::info(oc::format("PvP arena: {}, {} rock rects, {} open cells",
        pvpMapName(map), m_terrainRects.size(), m_coopMap.reachable.size()));
}

void GameMatch::rebuildCoopMap(uint32 seed, float fill, int lanes)
{
    if (m_coopMap.built && !m_coopMap.pvp && m_coopMap.seed == seed && m_coopMap.fill == fill && m_coopMap.lanes == lanes)
        return; // the join-replay re-broadcast / a duplicate GMp
    if (m_terrainRoot)
    {
        Globals::world.removeRootEntity(m_terrainRoot.get()); // rocks + barrier die with it
        m_terrainRoot = {};
    }
    m_structures.clearNodes();
    m_coopMap.seed = seed;
    m_coopMap.fill = fill;
    m_coopMap.lanes = lanes;
    m_ambientCamp.remaining = 0; // a camp anchored on the old map is void
    generateCoopGrid();
    m_coopMap.built = true;
    spawnTerrain();
    // The rock rects are the co-op "walls": Nav obstacles (feedNav re-reads m_wallObstacles every
    // frame) and placement refusals (cellsFree — ghosts turn red on rock, unit spawns skip it).
    m_wallObstacles.clear();
    for (const glm::vec4& r : m_terrainRects)
        m_wallObstacles.push_back(Nav::NavObstacle{ glm::vec2(r.x, r.y), glm::vec2(r.z, r.w) });
    m_structures.setTerrainBlocked(m_terrainRects);
    m_structures.setPlacementBounds(glm::vec2(-c_coopHalfSize), glm::vec2(c_coopHalfSize));
    Log::info(oc::format("Co-op map: seed {}, {} rock rects, {} reachable cells",
        seed, m_terrainRects.size(), m_coopMap.reachable.size()));
}

void GameMatch::sendMapSeed()
{
    // [u8 mode: 0 co-op | 1 PvP] then the co-op inputs (seed/fill/lanes) or the PvP arena index.
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint8>(m_coopMap.pvp ? 1 : 0);
    if (m_coopMap.pvp)
        writer.write<uint8>((uint8)m_coopMap.pvpMap);
    else
    {
        writer.write<uint32>(m_coopMap.seed);
        writer.write<float>(m_coopMap.fill);
        writer.write<uint8>((uint8)m_coopMap.lanes);
    }
    Globals::networkManager.fireNetworkEvent("GMp", writer.data());
}

// A camp body position: a random point in the disc around the camp anchor that lands on OPEN
// ground (rejection-sampled), then slid at least a body's width off any neighbouring rock edge —
// continuous coverage across cell borders, so nothing lines up on the lattice.
glm::vec3 GameMatch::ambientPointNear(const glm::vec3& center, float radius) const
{
    glm::vec3 p = center;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float a = glm::linearRand(0.0f, glm::two_pi<float>());
        const float r = radius * std::sqrt(glm::linearRand(0.0f, 1.0f));
        const glm::vec3 q = center + glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
        const int cell = coopCellAt(q);
        if (cell >= 0 && !m_coopMap.blocked[cell])
        {
            p = q;
            break;
        }
    }
    const int cell = coopCellAt(p);
    if (cell >= 0)
    {
        constexpr float c_keep = 1.2f; // body radius + a gap from the rock face
        const int x = cell % m_coopMap.cellsX, z = cell / m_coopMap.cellsX;
        const glm::vec3 c = coopCellCenter(cell);
        const float half = m_coopMap.cellSize * 0.5f;
        if (x == 0 || m_coopMap.blocked[cell - 1])
            p.x = glm::max(p.x, c.x - half + c_keep);
        if (x == m_coopMap.cellsX - 1 || m_coopMap.blocked[cell + 1])
            p.x = glm::min(p.x, c.x + half - c_keep);
        if (z == 0 || m_coopMap.blocked[cell - m_coopMap.cellsX])
            p.z = glm::max(p.z, c.z - half + c_keep);
        if (z == m_coopMap.cellsZ - 1 || m_coopMap.blocked[cell + m_coopMap.cellsX])
            p.z = glm::min(p.z, c.z + half - c_keep);
    }
    return glm::vec3(p.x, 1.0f, p.z);
}

glm::vec3 GameMatch::clampToOpenGround(const glm::vec3& pos) const
{
    if (!m_coopMap.built)
        return pos;
    const int cell = coopCellAt(pos);
    if (cell < 0 || !m_coopMap.blocked[cell])
        return pos;
    // Inside a rock: the nearest open cell center within 3 rings (post-flood, open == reachable).
    const int cx = cell % m_coopMap.cellsX, cz = cell / m_coopMap.cellsX;
    int best = -1;
    float bestDistSq = FLT_MAX;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx)
        {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= m_coopMap.cellsX || z >= m_coopMap.cellsZ)
                continue;
            const int c = z * m_coopMap.cellsX + x;
            if (m_coopMap.blocked[c])
                continue;
            const glm::vec3 center = coopCellCenter(c);
            const glm::vec2 d(center.x - pos.x, center.z - pos.z);
            if (glm::dot(d, d) < bestDistSq)
            {
                bestDistSq = glm::dot(d, d);
                best = c;
            }
        }
    return best >= 0 ? coopCellCenter(best) : pos;
}

// The barrier's "energy fence": pulsing lines strung between the posts at three heights, a slow
// wave running along each side. Cheap (a few hundred debug lines) and unmistakably "do not pass".
void GameMatch::drawCoopBarrier()
{
    const float t = (float)Globals::time.getElapsedSec(); // real clock: the fence hums while paused
    constexpr float c_heights[3] = { 1.4f, 3.0f, 4.6f };
    const glm::vec3 base(1.0f, 0.5f, 0.15f);
    const int segs = (int)(c_coopHalfSize * 2.0f / c_barrierStep);
    for (int side = 0; side < 4; ++side)
    {
        for (int i = 0; i < segs; ++i)
        {
            const float a0 = -c_coopHalfSize + (float)i * c_barrierStep;
            const float a1 = a0 + c_barrierStep;
            const uint32 color = packColor(base * (0.55f + 0.45f
                * std::sin(t * 2.2f + (float)(i + side * segs) * 0.7f)));
            for (const float h : c_heights)
            {
                const glm::vec3 p0 = side < 2 ? glm::vec3(a0, h, side == 0 ? -c_coopHalfSize : c_coopHalfSize)
                                              : glm::vec3(side == 2 ? -c_coopHalfSize : c_coopHalfSize, h, a0);
                const glm::vec3 p1 = side < 2 ? glm::vec3(a1, h, side == 0 ? -c_coopHalfSize : c_coopHalfSize)
                                              : glm::vec3(side == 2 ? -c_coopHalfSize : c_coopHalfSize, h, a1);
                Globals::rendererVK.addDebugLine(p0, p1, color);
            }
        }
    }
}

void GameMatch::setLobbyTeams(uint8 numTeams, oc::span<const oc::pair<uint32, uint8>> picks)
{
    m_numTeams = (uint8)glm::clamp((int)numTeams, 2, GameMaxTeams);
    m_lobbyTeams.assign(picks.begin(), picks.end());
}

glm::vec3 GameMatch::baseGroundPos(uint8 team) const
{
    // Circle: the Bases sit evenly around the ring, team 0 at the west. The lane arenas: two
    // teams = the corridor's ends (the original layout); more spread EVENLY along the x axis
    // between them on the corridor's center line, so every team keeps the same node symmetry
    // and the middle teams sit between two neighbours (generatePvpGrid clears rock around each).
    if (m_pvpMap == EPvpMap::Circle)
    {
        const float a = glm::pi<float>() + glm::two_pi<float>() * (float)team / (float)glm::max((int)m_numTeams, 1);
        return glm::vec3(std::cos(a), 0.0f, std::sin(a)) * c_pvpCircleBaseRadius;
    }
    if (m_numTeams <= 2)
        return team == 0 ? m_basePos : m_enemyBasePos;
    const float t = (float)glm::min((int)team, (int)m_numTeams - 1) / (float)(m_numTeams - 1);
    return glm::mix(m_basePos, m_enemyBasePos, t);
}

uint8 GameMatch::allocateClientTeam(uint32 clientId) const
{
    if (m_coop)
        return 0; // co-op: everyone plays on the server's team
    // The lobby pick wins. Otherwise (command-line start, a late joiner) the least-populated
    // playable team, lowest index on a tie. The server holds m_team; each connected client's team
    // lives on its capsule's puppet component, so the live set needs no separate bookkeeping.
    for (const auto& [id, team] : m_lobbyTeams)
        if (id == clientId && team < m_numTeams)
            return team;
    int counts[GameMaxTeams] = {};
    if (clientId != 0 && m_team < m_numTeams)
        ++counts[m_team];
    for (const auto& [id, p] : m_clientPlayers)
        if (p && id != clientId)
            if (const GameUnitComponent* u = getComponent<GameUnitComponent>(p.get()); u && u->team < m_numTeams)
                ++counts[u->team];
    uint8 best = 0;
    for (uint8 t = 1; t < m_numTeams; ++t)
        if (counts[t] < counts[best])
            best = t;
    return best;
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
    if (m_coop)
        return m_playerStart; // one shared Base — everyone spawns/respawns beside it
    // One Base per playable team; the spawn sits just beside it. Prefer the LIVE Base of that
    // team (on a client the mirrored one — it never learned the lobby's team count), else the
    // layout formula.
    const glm::vec3 offset(0.0f, 1.0f, -6.0f);
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) == team)
            return glm::vec3(m_structures.structurePos(i).x, 0.0f, m_structures.structurePos(i).z) + offset;
    return baseGroundPos(team) + offset;
}

void GameMatch::onClientJoined(uint32 clientId)
{
    // Their player: spawned server-side (player.pre carries Component Network), handed over with
    // setOwner in the SAME frame; the client adopts + drives it through the claim stream.
    // Clients spawn beside THEIR team's Base — the team is a freshly allocated slot, and the
    // capsule's puppet component carries it to the owner through the snapshot game blob.
    const uint8 team = allocateClientTeam(clientId);
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
    // World-state replay for the late joiner: the co-op map seed FIRST (the joiner builds its
    // terrain + nodes from it before any structure mirror below arrives — reliable ch1 is
    // ordered), then every structure, broadcast (mirrorPlace is idempotent, so already-connected
    // clients shrug the duplicates off; rebuildCoopMap no-ops on the repeated seed). Links need
    // no replay — each client derives them locally from the mirrored cable segments.
    if (m_isServer && m_coopMap.built)
        sendMapSeed(); // co-op inputs or the PvP arena — the joiner's terrain + nodes
    for (int i = 0; i < m_structures.structureCount(); ++i)
        sendStructurePlaced(i);
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
    if (m_coop && m_coopMap.built)
    {
        // The map's generation inputs: structure/unit positions and node indices only make sense
        // on the exact map the save was played on — loadGame regenerates it from these.
        root.set("MapSeed", oc::to_string((int)m_coopMap.seed));
        root.set("MapFill", m_coopMap.fill);
        root.set("MapLanes", oc::to_string(m_coopMap.lanes));
    }
    else if (m_coopMap.built && m_coopMap.pvp)
        root.set("PvpMap", oc::to_string((int)m_coopMap.pvpMap)); // the arena (EPvpMap index)
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

void GameMatch::loadGame(oc::string_view path)
{
    if (m_isClient)
    {
        Log::warning("Load game: server only");
        return;
    }
    AssetNode root;
    oc::string error;
    if (!loadAssetFile(path.empty() ? oc::string(c_gameSavePath) : oc::string(path), root, error))
    {
        Log::warning("Load game: " + error);
        return;
    }
    // A co-op save carries its map's generation inputs: regenerate the exact map first (clearing
    // the current structures BEFORE the node set swaps, so no stale extractor outlives its node).
    // Connected clients rebuild from the GMp broadcast the same way.
    if (m_coop)
    {
        if (const AssetNode* seedNode = root.find("MapSeed"); seedNode && seedNode->asInt() != 0)
        {
            m_structures.clearAllStructures(); // fires GRm hooks — clients prune ahead of the swap
            rebuildCoopMap((uint32)seedNode->asInt(),
                root.find("MapFill") ? root.find("MapFill")->asFloat() : m_coopMap.fill,
                root.find("MapLanes") ? root.find("MapLanes")->asInt() : m_coopMap.lanes);
            if (m_isServer)
                sendMapSeed();
        }
    }
    else if (const AssetNode* mapNode = root.find("PvpMap")) // a PvP save names its arena
    {
        const EPvpMap map = (EPvpMap)glm::clamp(mapNode->asInt(), 0, (int)EPvpMap::Count - 1);
        if (!m_coopMap.built || !m_coopMap.pvp || m_coopMap.pvpMap != map)
        {
            m_structures.clearAllStructures();
            rebuildPvpMap(map);
            if (m_isServer)
                sendMapSeed();
        }
    }
    // Replace the sim. Structure removal hooks fire during the clear (GRm to clients), then the
    // loaded state re-broadcasts below; unit entities resync through the normal despawn/spawn
    // replication (Component Network in their prefabs).
    m_structures.loadFrom(root);
    m_npcs.loadUnits(root, m_structures);
    m_selectedId = 0;
    if (m_isServer)
    {
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            sendStructurePlaced(i);
            if (!m_structures.structureRoute(i).empty())
                sendRoute(i);
        }
        // (No cable replay: clients re-derive links from the mirrored segments.)
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
    // CABLE SEGMENTS are excluded: they carry no stores, hundreds of them would blow the 79-record
    // cap, and their built flip mirrors through the GPl re-send instead (onStructureBuilt).
    constexpr int c_maxRecords = 79; // 11B each + the 146B header stays under the 1024B event cap
    int indices[c_maxRecords];
    int count = 0;
    for (int i = 0; i < m_structures.structureCount() && count < c_maxRecords; ++i)
        if (!isCableOrCrossing(m_structures.structureType(i)))
            indices[count++] = i;
    writer.write<uint16>((uint16)count);
    const auto frac8 = [](float v, float max) {
        return (uint8)glm::clamp(max > 0.0f ? v / max * 255.0f : 0.0f, 0.0f, 255.0f); };
    for (int k = 0; k < count; ++k)
    {
        const int i = indices[k];
        writer.write<uint32>(m_structures.structureId(i));
        writer.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMax()));
        writer.write<uint8>(frac8(m_structures.structureCharge(i), m_structures.structureCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureFuel(i), m_structures.structureFuelCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureMinerals(i), m_structures.structureMineralCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureOutputFrac(i), 1.0f));
        writer.write<uint8>(frac8(m_structures.structureFlowUtil(i), 1.0f));
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
        else if (name == "GWv")
        {
            // Co-op wave announcement (the wave itself arrives as replicated unit entities).
            const uint16 wave = reader.read<uint16>();
            if (!reader.overflowed())
                Log::info(oc::format("Co-op: wave {} incoming", (int)wave));
        }
        else if (name == "GMp")
        {
            // The server's terrain: the co-op map's inputs or the PvP arena — build the identical
            // terrain (+ barrier) + nodes locally (the rebuilds are no-ops on the re-broadcasts
            // later joiners trigger). Sent FIRST in the join replay, so the map exists before any
            // structure mirror lands.
            const uint8 mode = reader.read<uint8>();
            if (mode == 1)
            {
                const uint8 map = reader.read<uint8>();
                if (!reader.overflowed() && !m_coop && map < (uint8)EPvpMap::Count)
                    rebuildPvpMap((EPvpMap)map);
            }
            else
            {
                const uint32 seed = reader.read<uint32>();
                const float fill = reader.read<float>();
                const uint8 lanes = reader.read<uint8>();
                if (!reader.overflowed() && m_coop && seed != 0 && fill >= 0.0f && fill <= 1.0f)
                    rebuildCoopMap(seed, fill, (int)lanes);
            }
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
        if (!reader.overflowed() && isPlaceableType((EStructureType)type))
            m_structures.queuePlaceRequest((EStructureType)type, glm::vec3(x, 0.0f, z), nodeIndex,
                glm::vec3(fx, 0.0f, fz), requestTeam(sender));
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

// Pre-physics, before the spatial/begin-frame kicks — ONLY the player body writes (see Match.ixx).
void GameMatch::updatePlayer(float deltaSec)
{
    if (!m_enabled)
        return;
    ProfileScope scope("Game player", EProfileCategory::Game);
    if (m_isClient)
    {
        // CLIENT: adopt + drive our own capsule (the owner simulates; claims stream the state);
        // shield/health run on LOCAL readbacks against the mirrored fields. World sim is remote.
        m_player.clientAdopt(teamStartPos((uint8)m_team));
    }
    m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
    m_player.tickShieldAndHealth(deltaSec); // body writes too: the shield push impulse, the death stop
}

// Post-join (see Match.ixx for the placement + its consequences).
void GameMatch::update(float deltaSec)
{
    if (!m_enabled)
        return;
    ProfileScope scope("Game update", EProfileCategory::Game);
    if (m_scenarioOrderPending && !m_isClient)
        issueScenarioOrder(); // after runScenario's load: units come from the roster, the Base/raster from the ticks below

    if (m_isClient)
    {
        // Our team is the SERVER's assignment, carried on our capsule's puppet component by the
        // snapshot game blob (never derived from the clientId — see allocateClientTeam). It lands
        // a snapshot or two after adoption; follow it whenever it changes.
        if (m_player.team() != m_team)
        {
            m_team = m_player.team();
            m_player.setRespawnPos(teamStartPos((uint8)m_team));
            Log::info("We are team " + oc::to_string(m_team));
        }
        tickBaseHealing(deltaSec);
        m_structures.tickMirror(deltaSec);
        feedNav(); // obstacles only: the local player's move-order goal field
        return;
    }

    const glm::vec3 playerPos = m_player.bodyPos();
    m_structures.tickAuthority(playerPos, deltaSec);
    tickBaseHealing(deltaSec);
    tickPlayerMelee(deltaSec);

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
    if (m_coop)
    {
        tickWaves(deltaSec);
        tickCoopSpawns();
    }

    // Flow fields: obstacles + per-team sources staged for the NEXT frame's NavSystem::update
    // (which runs in main.cpp's kick/join window, BEFORE this bulk tick — one frame of source
    // latency, well inside nav's own async tolerances).
    feedNav();

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

void GameMatch::feedNav()
{
    ProfileScope scope("Game nav feed", EProfileCategory::Game);
    // Obstacles: every structure footprint (the same half-extent math as cellsFree) over the static
    // border ring. Change-detected inside Nav, so rebuilding the list per frame costs a hash.
    m_navObstacles.assign(m_wallObstacles.begin(), m_wallObstacles.end());
    for (oc::vector<Nav::NavSource>& v : m_navSources)
        v.clear();
    for (const StructureSystem::Ref& s : m_structures.structures())
    {
        // Cables/crossings are WALK-THROUGH: no nav obstacle (units path straight over them) and
        // never a NavSource (enemies do not march at power lines).
        if (isCableOrCrossing(s.type))
            continue;
        const float half = StructureSystem::footprintCellsOf(s.type) * StructureSystem::GridCellSize * 0.5f;
        const glm::vec2 c(s.entity->pos.x, s.entity->pos.z);
        m_navObstacles.push_back(Nav::NavObstacle{ c - half, c + half });
        // Sources: what units of OTHER teams walk toward — the same filter the local search used
        // (alive, not the invulnerable Base). Clients run no unit sim: obstacles only, for the
        // local player's goal field.
        if (m_isClient || s.state->invulnerable || !s.state->alive() || s.state->team >= Nav::MaxTeams)
            continue;
        m_navSources[s.state->team].push_back(Nav::NavSource{
            s.entity->pos, glm::max(s.state->meleeRadius, half), s.state->structureId, 0 });
    }
    // Enemy UNITS are targets too (unit-vs-unit combat): the NpcSystem roster IS the world-wide
    // unit list — no spatial sweep (rosters are maintained at the spawn/despawn seams).
    if (!m_isClient)
        for (const EntityPtr& e : m_npcs.units())
        {
            const GameUnitComponent* u = getComponent<GameUnitComponent>(e.get());
            if (!u || u->puppet || !u->alive() || u->team >= Nav::MaxTeams)
                continue;
            m_navSources[u->team].push_back(Nav::NavSource{ e->pos, glm::max(u->bodyRadius, 0.25f), 0, 3 });
        }
    // Player bodies (puppets) are targets too: our capsule + every client twin.
    const auto addPlayer = [&](Entity* e, uint8 team)
    {
        if (!e || team >= Nav::MaxTeams)
            return;
        const PhysicsComponent* pc = getComponent<PhysicsComponent>(e);
        if (!pc || !pc->body.isValid())
            return;
        m_navSources[team].push_back(Nav::NavSource{ pc->body.getPosition(), 0.5f, 0, 1 });
    };
    if (!m_isClient)
    {
        addPlayer(m_player.entity(), (uint8)m_team);
        for (const auto& [id, p] : m_clientPlayers)
            addPlayer(p.get(), requestTeam(id));
    }

    Globals::navSystem.setObstacles(m_navObstacles);
    for (uint32 t = 0; t < Nav::MaxTeams; ++t)
        Globals::navSystem.setTeamSources(t, m_navSources[t]);
    // NavSystem::update (publish + build kicks) runs from main.cpp's kick/join window after
    // physics.update — it touches neither the spatial index nor the renderer, so it fills the
    // stretch where main otherwise only waits on the "Spatial cull"/"Begin frame" jobs. The
    // gathering above stays HERE: the unit sweep is a spatial query, illegal in that window.
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
        hud.setSlot(c_rootDeleteSlot, "DEL", 0);
        hud.selectSlot(m_mode == EPlayerMode::Delete ? c_rootDeleteSlot : -1);
        return;
    }
    const oc::span<const EStructureType> items = buildCategoryItems(m_buildCategory);
    for (int i = 0; i < (int)items.size() && i < c_rootDeleteSlot; ++i)
        hud.setSlot(i, c_structureShortNames[(int)items[i]],
            m_structures.affordableCount(items[i], (uint8)m_team));
    hud.setSlot(c_rootDeleteSlot, "DEL", 0); // Delete stays on X on EVERY page
    hud.setSlot(c_cancelSlot, "CNCL", 0);
    hud.setSlot(c_pageBackSlot, "BACK", 0);
    hud.selectSlot(m_buildSelection);
}

void GameMatch::setMode(EPlayerMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_selectedId = 0;
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cableLinePending = false;
    m_buildSelection = -1;
    if (mode != EPlayerMode::Build)
        m_buildCategory = -1; // back to the root page
    refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category entry logs its own line (activateSlot)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish — X returns to Select"); break;
    case EPlayerMode::Select: Log::info("Select mode: click inspects, RMB routes / moves — Q/W/E build, X delete"); break;
    }
}

// One level back: a half-finished two-click step drops first, then the armed item disarms, then
// the category page (or Delete mode) returns to Select. Esc/Tab and the C "Cancel" slot.
void GameMatch::cancelOneLevel()
{
    if (m_mode == EPlayerMode::Build && m_buildSelection >= 0)
    {
        if (m_lanceAiming || m_wallPlacing || m_cableLinePending || m_cablePainting)
        {
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
            m_cableLinePending = false;
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
        else if (slot == c_rootDeleteSlot)
            setMode(m_mode == EPlayerMode::Delete ? EPlayerMode::Select : EPlayerMode::Delete);
        else if (slot == c_cancelSlot)
            setMode(EPlayerMode::Select); // cancels Delete mode; a no-op in Select
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
    if (slot == c_rootDeleteSlot)
    {
        setMode(EPlayerMode::Delete); // X works on every page, not just the root
        return;
    }
    if (slot >= (int)buildCategoryItems(m_buildCategory).size() || slot >= c_rootDeleteSlot)
        return; // empty slot
    if (slot == m_buildSelection
        && buildCategoryItems(m_buildCategory)[slot] == EStructureType::Crossing)
    {
        m_crossingRotated = !m_crossingRotated; // re-press of the armed CRSS slot rotates 90°
        return;
    }
    m_lanceAiming = false; // switching items drops half-done aims/flows
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cableLinePending = false;
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

void GameMatch::disarmBuild()
{
    m_buildSelection = -1;
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cableLinePending = false;
    refreshBuildHotbar(); // the slot highlight follows in the same frame
}

// Fill the auto-bent L between two snapped 1-cell positions — the dominant leg first, then the
// perpendicular one — requesting a placement per FREE cell (occupied cells are skipped, so a line
// across an existing run just fills the gaps). preview = draw ghosts instead of placing.
void GameMatch::placeCableLine(EStructureType armed, const glm::vec3& from, const glm::vec3& to, bool preview)
{
    constexpr float step = StructureSystem::GridCellSize;
    glm::vec3 points[c_cableMaxSegments];
    int count = 0;
    glm::vec3 p = from;
    const auto push = [&] { if (count < c_cableMaxSegments) points[count++] = p; };
    push();
    const glm::vec2 d(to.x - from.x, to.z - from.z);
    const bool xFirst = glm::abs(d.x) >= glm::abs(d.y);
    for (int leg = 0; leg < 2; ++leg)
    {
        const bool alongX = xFirst == (leg == 0);
        const float target = alongX ? to.x : to.z;
        float& axis = alongX ? p.x : p.z;
        while (glm::abs(target - axis) > step * 0.5f && count < c_cableMaxSegments)
        {
            axis += target > axis ? step : -step;
            push();
        }
    }
    for (int i = 0; i < count; ++i)
    {
        const bool free = m_structures.cellsFree(armed, points[i]);
        if (preview)
            drawStructureGhost(armed, points[i],
                packColor(free ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)));
        else if (free)
            requestPlace(armed, points[i], -1, glm::vec3(0.0f));
    }
}

// Cable segments place with BOTH inputs (see Match.ixx): a press paints its cell and keeps
// painting cells the cursor crosses (L-filled between samples so the run never breaks); a plain
// click (no drag) anchors the two-click L-line, whose second click places it and CHAINS.
void GameMatch::updateCablePlacement(const Camera& camera, EStructureType armed, bool confirmEdge)
{
    const Aim aim = computeAim(camera, armed);
    if (m_cablePainting)
    {
        if (!m_lmbDown)
        {
            // Release: a plain click (never left its cell) arms the L-line from that cell.
            m_cablePainting = false;
            m_cableLinePending = !m_cablePaintMoved;
            m_cableLineStart = m_cablePaintLast;
        }
        else if (aim.valid && glm::distance(glm::vec2(aim.pos.x, aim.pos.z),
            glm::vec2(m_cablePaintLast.x, m_cablePaintLast.z)) > 0.1f)
        {
            placeCableLine(armed, m_cablePaintLast, aim.pos, /*preview*/ false);
            m_cablePaintLast = aim.pos;
            m_cablePaintMoved = true;
        }
        if (aim.valid)
            drawStructureGhost(armed, aim.pos, packColor(glm::vec3(0.3f, 1.0f, 0.4f)));
        return;
    }
    if (!aim.valid)
    {
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    if (m_cableLinePending)
    {
        placeCableLine(armed, m_cableLineStart, aim.pos, /*preview*/ true);
        if (confirmEdge)
        {
            if (glm::distance(glm::vec2(aim.pos.x, aim.pos.z),
                glm::vec2(m_cableLineStart.x, m_cableLineStart.z)) < 0.1f)
                m_cableLinePending = false; // clicking the anchor again drops it
            else
            {
                placeCableLine(armed, m_cableLineStart, aim.pos, /*preview*/ false);
                m_cableLineStart = aim.pos; // CHAIN: the endpoint anchors the next line
            }
        }
        return;
    }
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawStructureGhost(armed, aim.pos, color);
    if (confirmEdge && aim.affordable)
    {
        requestPlace(armed, aim.pos, -1, glm::vec3(0.0f));
        m_cablePainting = true; // hold + drag paints from here; a plain click arms the L-line
        m_cablePaintMoved = false;
        m_cablePaintLast = aim.pos;
    }
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ !aim.affordable);
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
    // RMB CANCELS, one step at a time: a half-finished two-click flow (Lance aim, Wall line, cable
    // line, Crossing aim) drops first, and the next RMB disarms the item itself. Only once nothing
    // is armed does RMB go back to its Select-mode meaning (barracks route / move order) above.
    const EStructureType armed = buildCategoryItems(m_buildCategory)[m_buildSelection];

    // Cable segments have their own paint/L-line input (drag + two-click both).
    if (isCableType(armed))
    {
        if (cancelEdge)
        {
            if (m_cableLinePending || m_cablePainting)
            {
                m_cableLinePending = false;
                m_cablePainting = false;
            }
            else
                disarmBuild();
            return; // NOT consumed: the same press also walks the player
        }
        updateCablePlacement(camera, armed, confirmEdge);
        return;
    }

    // Crossing: single-click placement — the long axis follows the CAMERA facing (quantized to
    // ±X/±Z), and re-pressing the armed CRSS slot rotates it 90° (activateSlot).
    if (armed == EStructureType::Crossing)
    {
        if (cancelEdge)
        {
            disarmBuild();
            return; // NOT consumed: the same press also walks the player
        }
        const Aim aim = computeAim(camera, armed);
        if (!aim.valid)
        {
            updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
            return;
        }
        const glm::vec3 fwd = m_camera.forwardPlanar();
        glm::vec2 dir = glm::abs(fwd.x) >= glm::abs(fwd.z)
            ? glm::vec2(fwd.x >= 0.0f ? 1.0f : -1.0f, 0.0f)
            : glm::vec2(0.0f, fwd.z >= 0.0f ? 1.0f : -1.0f);
        if (m_crossingRotated)
            dir = glm::vec2(-dir.y, dir.x);
        const glm::quat rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
        // computeAim validated with the identity axis — redo the footprint checks with the real one.
        const bool free = m_structures.cellsFree(EStructureType::Crossing, aim.pos, rot)
            && !StructureSystem::actorInFootprint(EStructureType::Crossing, aim.pos);
        drawStructureGhostExtent(EStructureType::Crossing, aim.pos,
            packColor(free ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)),
            StructureSystem::footprintExtent(EStructureType::Crossing, rot));
        if (confirmEdge && free)
            requestPlace(EStructureType::Crossing, aim.pos, -1, glm::vec3(dir.x, 0.0f, dir.y));
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ !free);
        return;
    }

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
                                                  // (the Crossing has its own branch above)
    if (isEmitterType(aim.type)) // show the field footprint the powered variant would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterReachOf(aim.type) * 0.5f, color, 32);
    if (aim.type == EStructureType::Constructor) // show the build/repair reach it would cover
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.constructorRange(), color, 40);
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
    updateUnitSelection(camera);
}

// BOX SELECTION of own-team units (Select mode): LMB drag draws the box on the ground; on
// release every visible own unit whose screen position falls inside it is selected (SHIFT adds).
// A plain click (no drag) clears the unit selection. Selected units carry a ring; RMB orders send
// them along with the player (see the move-order block in updateWindowed).
void GameMatch::updateUnitSelection(const Camera& camera)
{
    static constexpr float c_dragPx = 8.0f;
    const Rect viewport = Globals::ui.getViewportRect();
    const auto groundOf = [&](const glm::vec2& screen, glm::vec3& out)
    {
        const Ray ray = camera.screenToRay(viewport, screen);
        if (ray.dir.y > -1e-4f)
            return false;
        out = ray.origin + ray.dir * (-ray.origin.y / ray.dir.y);
        return true;
    };
    if (m_lmbDown && glm::distance(m_lmbDownPos, m_mousePos) > c_dragPx)
    {
        // Live box: the four screen corners projected onto the ground.
        const glm::vec2 a = m_lmbDownPos, b = m_mousePos;
        const glm::vec2 corners[4] = { a, glm::vec2(b.x, a.y), b, glm::vec2(a.x, b.y) };
        glm::vec3 g[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i)
            ok = groundOf(corners[i], g[i]);
        if (ok)
        {
            const uint32 col = packColor(glm::vec3(0.4f, 1.0f, 0.5f));
            for (int i = 0; i < 4; ++i)
                Globals::rendererVK.addDebugLine(g[i] + glm::vec3(0.0f, 0.2f, 0.0f), g[(i + 1) % 4] + glm::vec3(0.0f, 0.2f, 0.0f), col);
        }
    }
    if (m_lmbReleased)
    {
        m_lmbReleased = false;
        const bool drag = glm::distance(m_lmbDownPos, m_mousePos) > c_dragPx;
        if (!Globals::input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT))
            m_selectedUnits.clear();
        if (drag && !m_isClient)
        {
            const glm::vec2 lo = glm::min(m_lmbDownPos, m_mousePos), hi = glm::max(m_lmbDownPos, m_mousePos);
            oc::vector<Entity*> units;
            NpcSystem::queryVisibleUnits(camera, units);
            for (Entity* e : units)
            {
                const GameUnitComponent* u = getComponent<GameUnitComponent>(e);
                if (!u || u->puppet || !u->alive() || u->team != (uint32)m_team)
                    continue;
                glm::vec2 sp;
                if (!camera.worldToScreen(viewport, e->pos, sp))
                    continue;
                if (sp.x < lo.x || sp.x > hi.x || sp.y < lo.y || sp.y > hi.y)
                    continue;
                bool already = false;
                for (const EntityPtr& p : m_selectedUnits)
                    already |= p.get() == e;
                if (!already)
                    m_selectedUnits.push_back(EntityPtr(e));
            }
        }
    }
    pruneSelectedUnits();
    for (const EntityPtr& p : m_selectedUnits)
        drawCircle(p->pos * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.15f, 0.0f), 0.9f,
            packColor(glm::vec3(0.4f, 1.0f, 0.5f)), 16);
}

void GameMatch::pruneSelectedUnits()
{
    for (size_t i = 0; i < m_selectedUnits.size();)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(m_selectedUnits[i].get());
        if (!u || !u->alive())
            m_selectedUnits.erase(m_selectedUnits.begin() + i);
        else
            ++i;
    }
}

// A barracks route becomes a LANE in the team's flow field: one A* per leg (barracks -> wp1 ->
// ...), written as flow the marching units simply follow — no unit plans anything. The lane decays
// like any other ("Nav/Flow decay"), and the units walking it keep it alive.
void GameMatch::seedRouteLane(uint32 structureId)
{
    if (m_isClient)
        return;
    const int index = m_structures.structureIndexById(structureId);
    if (index < 0 || !isBarracksType(m_structures.structureType(index)))
        return;
    const oc::span<const glm::vec3> route = m_structures.structureRoute(index);
    const uint8 team = m_structures.structureTeam(index);
    glm::vec3 from = m_structures.structurePos(index);
    for (const glm::vec3& wp : route)
    {
        Globals::navSystem.seedPath(team, from, wp, laneSeedSpeed(), laneSeedWidth());
        from = wp;
    }
}

// Centroid of the LARGEST CLUSTER of the selected units (single-linkage flood fill with
// `linkRadius`): the lane a move order seeds must start where the BULK of the group is, and a plain
// average would be dragged off by one straggler across the map. Selections are small, so O(n^2) is
// the right amount of machinery here.
static bool largestClusterCentroid(oc::span<const EntityPtr> units, float linkRadius, glm::vec3& out)
{
    const size_t n = units.size();
    if (n == 0)
        return false;
    oc::small_vector<int, 64> cluster;   // -1 = unassigned
    oc::small_vector<int, 64> stack;
    for (size_t i = 0; i < n; ++i)
        cluster.push_back(-1);
    const float r2 = linkRadius * linkRadius;
    int best = -1, bestCount = 0;
    glm::vec3 bestSum(0.0f);
    int clusters = 0;
    for (size_t seed = 0; seed < n; ++seed)
    {
        if (cluster[seed] >= 0)
            continue;
        const int id = clusters++;
        int count = 0;
        glm::vec3 sum(0.0f);
        cluster[seed] = id;
        stack.push_back(int(seed));
        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            sum += units[i]->pos;
            ++count;
            for (size_t j = 0; j < n; ++j)
            {
                if (cluster[j] >= 0)
                    continue;
                const glm::vec2 d(units[j]->pos.x - units[i]->pos.x, units[j]->pos.z - units[i]->pos.z);
                if (glm::dot(d, d) <= r2)
                {
                    cluster[j] = id;
                    stack.push_back(int(j));
                }
            }
        }
        if (count > bestCount)
        {
            bestCount = count;
            bestSum = sum;
            best = id;
        }
    }
    if (best < 0)
        return false;
    out = bestSum / float(bestCount);
    return true;
}

bool GameMatch::runScenario(oc::string_view savePath)
{
    if (!m_enabled || m_isClient)
    {
        Log::warning("Scenario: needs --game on the authority (single player or server)");
        return false;
    }
    loadGame(savePath);
    Log::info(oc::format("Scenario: loaded '{}'", savePath.empty() ? oc::string_view(c_gameSavePath) : savePath));
    // The loaded units' spatial entries link at the next commitFrame — the select-all query runs
    // from the next update() (issueScenarioOrder).
    m_scenarioOrderPending = true;
    m_scenarioOrderTries = 0;
    return true;
}

void GameMatch::issueScenarioOrder()
{
    // Select ALL live own-team units (not just the visible ones — the box select's query is a
    // frustum), then the same order the RMB press gives: locked move target + one seeded lane.
    // The unit roster serves loaded units immediately (no spatial-link latency any more), but the
    // enemy Base view and the published nav raster still arrive frames later — the retry loop
    // stays (bounded, in case the save held none).
    m_selectedUnits.clear();
    oc::vector<Entity*> units;
    m_npcs.queryAllUnits(units);
    for (Entity* e : units)
        if (const GameUnitComponent* u = getComponent<GameUnitComponent>(e); u && !u->puppet && u->alive() && u->team == (uint32)m_team)
            m_selectedUnits.push_back(EntityPtr(e));
    // The same order a right-click on the other team's Base gives: clicked at its centre, so the
    // destination lands on the face toward the player (pushed out of the footprint — the centre
    // itself is blocked cells and the A* would fail). The structure view (m_frame) is a per-frame
    // spatial query refreshed AFTER this point in update, so the loaded Base shows up a frame
    // after the units do: wait for it as well.
    int enemyBase = -1;
    for (int i = 0; i < m_structures.structureCount() && enemyBase < 0; ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) != (uint8)m_team)
            enemyBase = i;
    // Also wait for the Nav obstacle raster: the order's lane is an A* over it, and a unit's own
    // plan requests need it too — an order before it is published walks straight into the walls.
    const bool ready = !m_selectedUnits.empty() && enemyBase >= 0 && Globals::navSystem.raster() != nullptr;
    if (!ready && ++m_scenarioOrderTries < 600)
        return; // try again next update (bounded: ~10 s, in case the save holds no units / Nav is off)
    if (!ready)
        Log::warning(oc::format("Scenario: giving up after {} frames ({} units, enemy Base {}, raster {})", m_scenarioOrderTries,
            (uint32)m_selectedUnits.size(), enemyBase >= 0 ? "found" : "missing", Globals::navSystem.raster() != nullptr ? "published" : "missing"));
    m_scenarioOrderPending = false;
    const glm::vec3 target = enemyBase >= 0 ? pointOutsideFootprint(m_structures.structurePos(enemyBase), enemyBase)
                                            : (m_team == 0 ? m_enemyBasePos : m_basePos);
    const bool laneSeeded = moveOrderAt(target);
    Log::info(oc::format("Scenario: {} units ordered to ({:.0f}, {:.0f}) after {} frames, lane {}", (uint32)m_selectedUnits.size(),
        m_player.moveTarget().x, m_player.moveTarget().z, m_scenarioOrderTries, laneSeeded ? "seeded" : "NOT seeded (A* failed)"));
}

// THE right-click move order, shared by the RMB handler and the profiling scenario: the player and
// the selected units walk to a world position (a fresh order: the lane is seeded from the group).
bool GameMatch::moveOrderAt(const glm::vec3& worldPos)
{
    // A click that lands inside co-op rock clamps to the nearest open cell — a target on blocked
    // cells fails the lane A* and every unit's own plan request (the pointOutsideFootprint rule,
    // applied to terrain).
    const glm::vec3 dest = clampToOpenGround(glm::vec3(worldPos.x, 0.0f, worldPos.z));
    m_player.setMoveTarget(dest);
    return orderSelectedUnits(dest, true);
}

// A clicked ground point on a structure, pushed just OUTSIDE its footprint along the side it fell
// on: the capsule ends up at that face instead of grinding into the wall, and the Nav A* has a
// reachable goal (a point inside the footprint is blocked cells — no lane, and every unit's own
// plan request to it fails too).
glm::vec3 GameMatch::pointOutsideFootprint(const glm::vec3& clicked, int structure) const
{
    const glm::vec3 center = m_structures.structurePos(structure);
    const float half = StructureSystem::footprintCellsOf(m_structures.structureType(structure))
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
    return glm::vec3(center.x + d.x, 0.0f, center.z + d.y);
}

bool GameMatch::orderSelectedUnits(const glm::vec3& target, bool freshOrder)
{
    for (const EntityPtr& p : m_selectedUnits)
        if (GameUnitComponent* u = getComponent<GameUnitComponent>(p.get()))
            u->orderMove(glm::vec3(target.x, 0.0f, target.z), freshOrder);
    bool laneSeeded = false;
    // A FRESH order seeds a planned LANE from the group to the destination: one A* (main thread),
    // written into the team flow, and the units follow it as crowd flow — the group routes around
    // buildings without any of them planning. The start is the largest cluster's centre, so a lone
    // straggler cannot pull the lane's origin away from the bulk of the group.
    glm::vec3 groupPos;
    if (freshOrder && largestClusterCentroid(m_selectedUnits, m_selectionClusterRadius, groupPos))
        laneSeeded = Globals::navSystem.seedPath(uint32(m_team), groupPos, target, laneSeedSpeed(), laneSeedWidth());
    // (No group re-seed timer: while they walk, the units themselves ask for a lane on their own
    // timers and Nav's proximity dedup turns the whole group's requests into one plan — see
    // GameUnitComponent's plan request and NavSystem::requestSeedPath.)
    return laneSeeded;
}

// RIGHT-CLICK actions with a selection, shared by Select AND Build mode: on a structure = SMART
// CONNECT (below); on GROUND with an own-team BARRACKS selected = set its unit ROUTE waypoint
// (SHIFT appends, a plain click restarts the route).
void GameMatch::updateRightClickActions(const Camera& camera, bool rmbEdge)
{
    if (!rmbEdge || m_selectedId == 0)
        return;
    if (const int hover = hoveredStructure(camera);
        hover >= 0 && !isCableOrCrossing(m_structures.structureType(hover)))
        return; // on a building: the caller turns it into a MOVE order (cables are ground)
    const int sel = m_structures.structureIndexById(m_selectedId);
    glm::vec3 ground;
    if (sel < 0 || !isBarracksType(m_structures.structureType(sel))
        || m_structures.structureTeam(sel) != (uint8)m_team || !aimGroundPoint(camera, ground))
        return; // not a route click either: the caller turns it into a MOVE order
    ground.y = 0.0f;
    ground = clampToOpenGround(ground); // a waypoint inside co-op rock would stall every marcher
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
        const EStructureType type = m_structures.structureType(i);
        // Cable segments: no label unless there is something to show — a blueprint's build
        // progress or damage, AUTHORITY only (cable health is not mirrored, so a client's copy
        // would read full). Hundreds of full-health segments would drown the HUD.
        const float healthMax = m_structures.structures()[i].state->healthMax;
        if (isCableOrCrossing(type) && i != selected
            && (m_isClient || (!m_structures.structureBlueprint(i)
                && m_structures.structureHealth(i) >= healthMax - 1e-3f)))
            continue;
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_structures.structureLabelAnchor(i), label.screenPos))
            continue;
        label.title = c_structureShortNames[(int)type]; // the selected one overrides w/ full name
        const bool consumer = hasShieldEmitter(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator;
        label.barValue = m_structures.structureHealth(i);
        label.barMax = healthMax; // per-type: cables are softer than buildings
        {
            const float frac = label.barValue / label.barMax;
            // Blueprint: health IS the construction progress — the bar reads blue while building.
            label.barColor = m_structures.structureBlueprint(i) ? glm::vec3(0.5f, 0.7f, 1.0f)
                : glm::mix(glm::vec3(1.0f, 0.25f, 0.2f), glm::vec3(0.3f, 1.0f, 0.4f), frac);
        }
        // A FULL health bar stays hidden — only damage (or blueprint progress, or selection, or
        // "AlwaysDisplayHealth true" in the .pre) draws one.
        if (!m_structures.structureBlueprint(i) && i != selected
            && !m_structures.structures()[i].state->alwaysDisplayHealth
            && label.barValue >= label.barMax - 1e-3f)
            label.barMax = 0.0f; // <= 0 = no bar (the selected-info HP string is selection-only)
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
            if (mineralCap > 0.0f) // the Base: energy AND its spendable mineral bank
            {
                label.bar3Value = m_structures.structureMinerals(i);
                label.bar3Max = mineralCap;
                label.bar3Color = glm::vec3(0.35f, 0.5f, 1.0f);
            }
        }
        if (i == selected)
        {
            label.emphasized = true;
            label.title = structureTypeName(type);
            char info[192];
            int len = snprintf(info, sizeof(info), "HP %.0f / %.0f", label.barValue, label.barMax);
            if (energyCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nEnergy %.0f / %.0f",
                    m_structures.structureCharge(i), energyCap);
            if (fuelCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nFuel %.0f / %.0f",
                    m_structures.structureFuel(i), fuelCap);
            if (mineralCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nMinerals %.0f / %.0f",
                    m_structures.structureMinerals(i), mineralCap);
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
        // (Swarm bodies included: full bars are hidden, so only the DAMAGED slice of a thousand-
        // body horde pushes a label — the drown-the-HUD concern the old shieldOutput skip covered.)
        HudWorldLabel label;
        const float height = u->puppet ? 2.0f : 1.6f;
        if (!camera.worldToScreen(viewport, unitEntity->pos + glm::vec3(0.0f, height, 0.0f), label.screenPos))
            continue;
        label.title = remoteShortName(unitEntity->getName(), u->puppet ? 2 : 0);
        // FULL bars stay hidden ("AlwaysDisplayHealth true" in the .pre opts a prefab back in):
        // only damage draws attention. An undamaged non-player unit skips its label entirely —
        // no floating name over a healthy crowd; players always keep their name tag.
        const bool always = u->alwaysDisplayHealth;
        if (!u->puppet && !u->collapsed && u->energy > 0.0f)
        { // shield-less bodies (swarm) spawn with a ZERO battery, so they land in the health branch
            // UNIT with a live shield: ONE bar — the shield IS the unit's front line, so the bar
            // shows it (shield color) until it collapses; only then does the health bar take over.
            if (always || u->energy < u->energyMax - 1e-3f)
            {
                label.barValue = u->energy;
                label.barMax = glm::max(u->energyMax, 1e-3f);
                label.barColor = glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        else
        {
            if (always || u->health < u->healthMax - 1e-3f)
            {
                label.barValue = u->health;
                label.barMax = glm::max(u->healthMax, 1e-3f); // per-type: Brutes triple, Runners half
                label.barColor = u->team == (uint32)m_team
                    ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.25f, 0.2f);
            }
            if (u->puppet && (always || u->energy < u->energyMax - 1e-3f))
            {
                label.bar2Value = u->energy; // players keep both bars: shield under health
                label.bar2Max = glm::max(u->energyMax, 1e-3f);
                label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        if (!u->puppet && label.barMax <= 0.0f)
            continue;
        labels.push_back(oc::move(label));
    }
    Globals::gameHud.setWorldLabels(oc::move(labels));
}

void GameMatch::tickPlayerMelee(float deltaSec)
{
    // AUTHORITY ONLY (called from the authority branch): units simulate here, so a client-side
    // hit would be stomped by the next snapshot blob. Every player capsule — the server's own AND
    // each client twin — grinds adjacent enemy UNITS (never puppets: players fighting players is
    // not a melee aura's job) through the unified GameUnitComponent::damage().
    if (m_meleeDps <= 0.0f || m_meleeRadius <= 0.0f)
        return;
    ProfileScope scope("Player melee", EProfileCategory::Game);
    thread_local oc::vector<uint64> nearby;
    const auto meleeAround = [&](const glm::vec3& pos, uint8 team)
    {
        Globals::spatialIndex.querySphere(glm::dvec3(pos), m_meleeRadius, SpatialLayer_Render, nearby);
        for (const uint64 user : nearby)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameUnitComponent* u = getComponent<GameUnitComponent>(other);
            if (!u || u->puppet || u->team == team || !u->alive())
                continue;
            // The query matches bounding spheres — the melee rule is the CENTER distance (XZ,
            // the same measure the units' own melee probes use).
            const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - glm::vec2(pos.x, pos.z);
            if (glm::dot(d, d) <= m_meleeRadius * m_meleeRadius)
                u->damage(m_meleeDps * deltaSec);
        }
    };
    meleeAround(m_player.bodyPos(), (uint8)m_team);
    for (const auto& [id, p] : m_clientPlayers)
        if (p)
            if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(p.get()); pc && pc->body.isValid())
                meleeAround(pc->body.getPosition(), requestTeam(id));
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
    if (m_coop && !m_isClient) // the wave clock is authority state (clients get the GWv log)
        hud.setCounter("Next wave (s)", glm::max(m_waveTimer, 0.0f), 0, glm::vec3(1.0f, 0.45f, 0.3f));
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
    ProfileScope modeScope("Game mode input", EProfileCategory::Game);
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
    case EPlayerMode::Select: updateSelectMode(camera, confirmEdge, rmbEdge); break;
    }
    m_lmbReleased = false; // a release the active mode did not consume (box select is Select-only)

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
        // Cables/crossings are WALK-THROUGH — an RMB near one is a plain ground order, never a
        // walk-to-its-face building order.
        int hover = hoveredStructure(camera);
        if (hover >= 0 && isCableOrCrossing(m_structures.structureType(hover)))
            hover = -1;
        glm::vec3 clicked;
        if (hover >= 0 && aimGroundPoint(camera, clicked))
        {
            moveOrderAt(pointOutsideFootprint(clicked, hover));
            m_rmbMoveDrag = false; // a building order is one-shot: dragging off it must not re-aim
        }
        else if (hover < 0)
            m_rmbMoveDrag = true; // ground: holding keeps re-aiming at the cursor
    }
    glm::vec3 moveGround;
    if (m_rmbMoveDrag && aimGroundPoint(camera, moveGround))
    {
        const glm::vec3 dest = clampToOpenGround(glm::vec3(moveGround.x, 0.0f, moveGround.z));
        m_player.setMoveTarget(dest);
        orderSelectedUnits(dest, rmbEdge); // the selected units follow the same order (re-aimed while held; the lane wipe only on the press)
    }
    if (m_player.hasMoveTarget()) // destination marker until the capsule arrives
        drawCircle(m_player.moveTarget() + glm::vec3(0.0f, 0.15f, 0.0f), 0.6f,
            packColor(glm::vec3(0.3f, 0.95f, 0.6f)), 16);

    // Faint ring at the estimated equilibrium shield radius — compare it against the drawn bubble.
    const float shieldR = m_player.shieldRadius();
    if (shieldR > 0.05f)
        drawCircle(m_player.interpolatedPos(), shieldR, packColor(glm::vec3(0.3f, 0.8f, 1.0f)), 32);
    modeScope.stop();

    m_structures.drawDebug();
    if (m_coop)
        drawCoopBarrier(); // the edge fence's pulsing energy lines
    if (Globals::navSystem.debugMode() > 0)
    {
        ProfileScope navDrawScope("Nav debug draw", EProfileCategory::Game);
        Globals::navSystem.drawDebug(m_player.interpolatedPos(),
            [](const glm::vec3& a, const glm::vec3& b, uint32 color) { Globals::rendererVK.addDebugLine(a, b, color); });
    }

    // Barracks unit routes (own team): line chain from the barracks through its waypoints, each
    // with its destination circle — bright for the selected barracks, dim otherwise.
    ProfileScope routesScope("Barracks routes", EProfileCategory::Game);
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

    routesScope.stop();

    buildWorldLabels(camera);
    {
        ProfileScope hudScope("Game HUD update", EProfileCategory::Game);
        updateHud();
    }
}
