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
import Threading; // the ambient wander rides the post-update batch
import File; // AssetNode + loadAssetFile/writeAssetText (game save/load)
import :Match;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// GRID HOTKEYS (RTS style): the 12 hotbar slots map onto QWER / ASDF / ZXCV, row-major. The ROOT
// page holds the categories (Q Combat, W Production), the three cables on A/S/D and Delete on X;
// a category page holds its items in order with Cancel on C (straight back to Select). The same
// slot index is reached by its key or by clicking the drawn slot.
static constexpr int c_gridSlots = 12;
static constexpr SDL_Scancode c_gridKeys[c_gridSlots] = {
    SDL_Scancode::SDL_SCANCODE_Q, SDL_Scancode::SDL_SCANCODE_W, SDL_Scancode::SDL_SCANCODE_E, SDL_Scancode::SDL_SCANCODE_R,
    SDL_Scancode::SDL_SCANCODE_A, SDL_Scancode::SDL_SCANCODE_S, SDL_Scancode::SDL_SCANCODE_D, SDL_Scancode::SDL_SCANCODE_F,
    SDL_Scancode::SDL_SCANCODE_Z, SDL_Scancode::SDL_SCANCODE_X, SDL_Scancode::SDL_SCANCODE_C, SDL_Scancode::SDL_SCANCODE_V,
};
static constexpr oc::string_view c_gridKeyLabels[c_gridSlots] = { "Q", "W", "E", "R", "A", "S", "D", "F", "Z", "X", "C", "V" };
// Root page slots: the three category pages (Q/W/E) and Delete.
static constexpr int c_rootDeleteSlot = 9;     // X
static constexpr int c_cancelSlot = 10;        // C: straight back to Select (Esc/Tab step back one level instead)
static constexpr int c_numCategories = 2;
static constexpr const char* c_buildCategories[c_numCategories] = { "CMBT", "PROD" };      // slot captions
static constexpr const char* c_buildCategoryNames[c_numCategories] = { "Combat", "Production" }; // log prose
static constexpr const char* c_buildCategoryCards[c_numCategories] = { // the slots' hover cards
    "Combat\nShields, walls, turrets and the barracks.",
    "Production\nPower, extraction, refining and storage." };
// The three cables live on the ROOT page (A/S/D) — no page of their own. Arming one enters Build
// with this HIDDEN category, which draws and behaves as the root page (see isRootPage).
static constexpr int c_cableCategory = 2;
static constexpr int c_rootCableSlot = 4;      // A: c_cableItems[0], S, D follow
// 3-5 char shorthands, indexed by EStructureType — the SAME vocabulary the world tag over a
// building uses, so a hotbar slot and the thing it builds read identically.
static constexpr const char* c_structureShortNames[] = { "EMIT", "GEN", "CON", "EXTR", "BATT",
    "FUEL", "SOL", "FAB", "BSTN", "LNC", "BRK", "BRK-B", "BRK-R", "BRK-S", "WALL", "TRT", "SILO",
    "CNST", "BASE", "CBL-P", "CBL-F", "CBL-M", "CRS-P", "CRS-F", "CRS-M", "HOUS", "MEDC" };
static_assert(oc::size(c_structureShortNames) == (size_t)EStructureType::Count);
// The barracks' unit-type popup captions, in ENpcType order (the same order the price tables use).
static constexpr const char* c_unitTypeNames[] = { "Grunt", "Brute", "Runner", "Spitter", "Swarm",
    "Elite", "Giant", "Titan", "Lobber", "Spawner", "Warrior" };
static_assert(oc::size(c_unitTypeNames) == (size_t)ENpcType::Count);
// The popup's buttons, in order: the producible types (isBarracksUnitType — no Spitter).
static constexpr uint8 c_barracksMenu[] = { (uint8)ENpcType::Grunt, (uint8)ENpcType::Warrior,
    (uint8)ENpcType::Brute, (uint8)ENpcType::Runner, (uint8)ENpcType::Swarm };
// A category page is just its list of placeable types — the shorthand table above IS each slot's
// caption.
static constexpr EStructureType c_combatItems[] = {
    EStructureType::Emitter,
    EStructureType::Bastion,
    EStructureType::Lance,
    EStructureType::Wall,
    EStructureType::Turret,
    EStructureType::Barracks,
    EStructureType::House,
};
static constexpr float c_wallSegmentSpacing = 2.0f; // one segment per box width along the line
static constexpr int c_wallMaxSegments = 16;
static constexpr int c_cableMaxSegments = 32; // one paint-fill placement burst cap
// Everything that is not a weapon: generation, extraction and the distribution buildings.
static constexpr EStructureType c_productionItems[] = {
    EStructureType::Generator,
    EStructureType::Solar,
    EStructureType::Extractor,
    EStructureType::Fabricator,
    EStructureType::Constructor,
    EStructureType::Battery,
    EStructureType::MineralSilo,
    EStructureType::FuelTank,
    EStructureType::MedicStation,
};
// PHYSICAL cables: one segment type per medium, on the root page's A/S/D. Placement PAINTS
// (press + drag, see updateCablePlacement); connections derive from cell adjacency. The per-medium
// crossings are NOT on the hotbar: the paint stroke places them itself (placeCableLine).
static constexpr EStructureType c_cableItems[] = {
    EStructureType::CablePower,
    EStructureType::CablePipe,
    EStructureType::CableConveyor,
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
// GameMatch::generateCoopGrid). The ground plane (ground.pre) is 600 m, so ±300 is the hard edge;
// waves spawn in the open ring BETWEEN the barrier and the ground edge and walk in through it
// (the barrier's collider only matches the Player layer).
static constexpr float c_coopHalfSize = 270.0f;
static constexpr float c_coopCellSize = 10.0f; // terrain cell = one rock block (5x the 2 m grid)
static constexpr int c_coopCells = 54;         // cells per side
static_assert((float)c_coopCells * c_coopCellSize == c_coopHalfSize * 2.0f);
static constexpr float c_coopGroundEdge = 296.0f;  // spawn clamp just inside the 600 m ground
static constexpr float c_coopBaseClearRadius = 26.0f; // rock-free zone around the Base + starters
static constexpr float c_barrierStep = 20.0f;      // one barrier.pre segment (posts at centers)
// The INVISIBLE edge-wall ring just inside the ground rim: waves spawn in the band outside the
// barrier, where crowd pressure used to shove bodies off the world. Blocks everyone (edgewall.pre
// stays on the Default layer, like rock.pre). 100 m segments — nothing sees it, so long boxes keep
// the static body count at 6 a side instead of 30.
static constexpr float c_coopEdgeWall = 299.0f;
static constexpr float c_edgeWallStep = 100.0f;
static constexpr float c_edgeWallHalfThick = 0.5f; // MUST match edgewall.pre's HalfExtents z
static constexpr float c_edgeWallSpan = 300.0f;    // tangential reach of a side: the whole rim, so
                                                   // the four sides seal the corners between them
// A wave spawn point is clamped to +-c_coopGroundEdge, so this is the gap between the furthest a
// body can be placed and the wall's INNER FACE. It has to clear the largest unit's body radius —
// enemyTitan.pre, 2.0 m — or that unit spawns overlapping a static and gets kicked when it unparks.
static constexpr float c_edgeWallClearance = c_coopEdgeWall - c_edgeWallHalfThick - c_coopGroundEdge;
static_assert(c_edgeWallClearance >= 2.0f, "spawn clamp is too close to the edge wall: the biggest "
    "unit body would spawn inside it — move c_coopGroundEdge in or c_coopEdgeWall out");

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
    // Only the co-op AI carves seed lanes toward HUNTED targets; player-team units seed for routes
    // and move orders alone (see GameUnitParams::huntSeedTeam). PvP has no AI team: none.
    GameUnitComponent::params.huntSeedTeam = coop ? (int)CoopAiTeam : -1;
    GameUnitComponent::params.localTeam = m_team; // own-team units tint green (re-stamped when the team changes)

    {
        // Gameplay tweaks persist between runs and the server's values overrule the clients'.
        const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
        Tweak::floatVar("Game/Coop", "First wave delay (s)", &m_waveFirstDelay, 5.0f, 600.0f, 5.0f);
        Tweak::floatVar("Game/Coop", "Wave interval (s)", &m_waveInterval, 10.0f, 600.0f, 5.0f);
        Tweak::intVar("Game/Coop", "Wave budget", &m_waveBudget, 1, 5000, 10);
        Tweak::floatVar("Game/Coop", "Wave budget growth", &m_waveBudgetGrowth, 0.0f, 1000.0f, 5.0f);
        Tweak::floatVar("Game/Coop", "Wave growth growth", &m_waveGrowthGrowth, 0.0f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost grunt", &m_waveCost[(int)ENpcType::Grunt], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost brute", &m_waveCost[(int)ENpcType::Brute], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost runner", &m_waveCost[(int)ENpcType::Runner], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost spitter", &m_waveCost[(int)ENpcType::Spitter], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost swarm", &m_waveCost[(int)ENpcType::Swarm], 0.1f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost elite", &m_waveCost[(int)ENpcType::Elite], 0.1f, 500.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost giant", &m_waveCost[(int)ENpcType::Giant], 0.1f, 500.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost titan", &m_waveCost[(int)ENpcType::Titan], 0.1f, 500.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost lobber", &m_waveCost[(int)ENpcType::Lobber], 0.1f, 500.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost spawner", &m_waveCost[(int)ENpcType::Spawner], 0.1f, 500.0f, 0.5f);
        Tweak::floatVar("Game/Coop", "Cost warrior", &m_waveCost[(int)ENpcType::Warrior], 0.1f, 500.0f, 0.5f);
        Tweak::intVar("Game/Coop", "Max enemy units", &m_waveMaxAlive, 1, 100000, 50);
        Tweak::floatVar("Game/Coop", "Wave spawn area per unit", &m_waveSpawnAreaPerUnit, 1.0f, 60.0f, 0.5f);
        Tweak::intVar("Game/Coop", "Ambient budget", &m_ambientBudget, 0, 1000000, 10);
        Tweak::floatVar("Game/Coop", "Ambient safe radius", &m_ambientSafeRadius, 10.0f, 200.0f, 1.0f);
        Tweak::intVar("Game/Coop", "Ambient recipe window", &m_ambientRecipeWindow, 0, 20, 1);
        Tweak::floatVar("Game/Coop", "Ambient depth scale", &m_ambientDepthScale, 0.5f, 1.0f, 0.01f);
        Tweak::floatVar("Game/Coop", "Ambient wander interval (s)", &m_ambientWanderInterval, 0.0f, 600.0f, 5.0f);
        Tweak::floatVar("Game/Coop", "Ambient wander distance", &m_ambientWanderDistance, 2.0f, 60.0f, 1.0f);
        Tweak::floatVar("Game/Coop", "Ambient wander base bias", &m_ambientWanderBaseBias, 0.0f, 2.0f, 0.05f);
        Tweak::floatVar("Game/Coop", "Ambient wander timeout (s)", &m_ambientWanderTimeout, 1.0f, 60.0f, 1.0f);
        Tweak::floatVar("Game/HUD", "Label max distance", &m_labelMaxDistance, 10.0f, 2000.0f, 10.0f, {}, ETweakFlags::None);
        Tweak::floatVar("Game/Nav", "Nav unit source reach", &m_navUnitSourceReach, 8.0f, 400.0f, 4.0f);
        Tweak::floatVar("Game/Sim LOD", "Unit cluster focus radius", &m_focusClusterRadius, 5.0f, 200.0f, 1.0f, {}, ETweakFlags::None);
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
    Globals::jobSystem.wait(m_labelsCounter); // the labels job reads this object
    // Exit-to-menu can destroy a GameMatch MID-RUN: every tweak registered on a member (the ctor's
    // Game/* block + camera/player/structures/npcs) must leave the registry with it, or the
    // per-frame poll reads freed memory. Statics (component params) stay and re-register in place.
    TweakRegistry::get().unregisterInRange(this, sizeof(GameMatch));
    applyPause(false); // a shared pause must not outlive the match (exit-to-menu mid-pause)
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
        m_structures.onUnitTypeChanged = [this](uint32 id)
        {
            if (const int index = m_structures.structureIndexById(id); index >= 0)
                sendUnitType(index);
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
            GameUnitComponent::params.localTeam = m_team;
            m_player.setTeam(m_team);
            m_playerStart = teamStartPos((uint8)m_team);
        }
        else
        {
            m_structures.spawnBase(m_basePos);
            m_ambientPendingBudget = (float)m_ambientBudget; // the scatter's points, trickled in (tickCoopSpawns)
        }
        // Respawns land on a FREE cell near the anchor: buildings placed on the spawn spot after
        // the fact (a wall ring around the Base, a house) must not swallow the capsule. Rings of
        // 1x1 probes outward from the anchor, cables walk-through; a fully built-over area falls
        // back to the anchor itself.
        m_player.setRespawnResolver([this](const glm::vec3& anchor)
        {
            constexpr float c_step = StructureSystem::GridCellSize;
            for (int ring = 0; ring <= 8; ++ring)
            {
                const int probes = ring == 0 ? 1 : 8 * ring;
                for (int k = 0; k < probes; ++k)
                {
                    const float a = (float)k / (float)probes * glm::two_pi<float>();
                    const glm::vec3 p = StructureSystem::snapToGrid(EStructureType::Emitter,
                        anchor + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (c_step * (float)ring));
                    if (m_structures.cellsFree(EStructureType::Emitter, p,
                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), /*ignoreCables*/ true))
                        return glm::vec3(p.x, anchor.y, p.z);
                }
            }
            return anchor;
        });
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
              "(Combat/Production/Cables), X = delete, C = cancel. Cables are physical: paint "
              "(press + drag) a run between buildings to connect them");
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
        struct { ENpcType type; float weight; } mix[5]; // weight 0 = unused slot
    };
    // The ELITE tier (Elite/Giant/Titan/Lobber) enters at wave 7 and dominates from ~10 on: the
    // Lobber's splash shells punish packed defences, the Giant/Titan soak turret fire, and the
    // budget growth is what lets a late wave afford a Titan next to its escort.
    constexpr WaveArchetype c_waveArchetypes[] = {
        { "swarm",           1, { { ENpcType::Swarm, 1.0f } } },
        { "swarm + runners", 2, { { ENpcType::Swarm, 0.75f }, { ENpcType::Runner, 0.25f } } },
        { "grunt push",      2, { { ENpcType::Grunt, 0.65f }, { ENpcType::Swarm, 0.35f } } },
        { "runner rush",     3, { { ENpcType::Runner, 1.0f } } },
        { "warrior line",    4, { { ENpcType::Warrior, 0.4f }, { ENpcType::Grunt, 0.3f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "shield wall",     6, { { ENpcType::Warrior, 0.6f }, { ENpcType::Spitter, 0.15f },
                                  { ENpcType::Swarm, 0.25f } } },
        { "spitter siege",   4, { { ENpcType::Spitter, 0.3f }, { ENpcType::Swarm, 0.7f } } },
        { "brute hammer",    5, { { ENpcType::Brute, 0.25f }, { ENpcType::Swarm, 0.75f } } },
        { "combined arms",   6, { { ENpcType::Grunt, 0.3f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.2f }, { ENpcType::Swarm, 0.25f } } },
        { "brute wall",      7, { { ENpcType::Brute, 0.85f }, { ENpcType::Spitter, 0.15f } } },
        { "elite guard",     7, { { ENpcType::Elite, 0.45f }, { ENpcType::Grunt, 0.25f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "the works",       8, { { ENpcType::Swarm, 0.4f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.15f }, { ENpcType::Brute, 0.2f } } },
        { "lobber barrage",  8, { { ENpcType::Lobber, 0.3f }, { ENpcType::Elite, 0.2f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "giant push",      9, { { ENpcType::Giant, 0.15f }, { ENpcType::Elite, 0.35f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "siege column",   10, { { ENpcType::Giant, 0.2f }, { ENpcType::Lobber, 0.3f },
                                  { ENpcType::Brute, 0.2f }, { ENpcType::Swarm, 0.3f } } },
        { "titan",          11, { { ENpcType::Titan, 0.05f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.6f } } },
        { "endgame",        13, { { ENpcType::Titan, 0.1f }, { ENpcType::Giant, 0.2f },
                                  { ENpcType::Elite, 0.3f }, { ENpcType::Lobber, 0.2f },
                                  { ENpcType::Swarm, 0.2f } } },
        { "hive",            9, { { ENpcType::Spawner, 0.1f }, { ENpcType::Elite, 0.3f },
                                  { ENpcType::Swarm, 0.6f } } },
        { "hive siege",     12, { { ENpcType::Spawner, 0.15f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.5f } } },
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

// Live AI bodies — ambient and previous waves alike. Roster walk, main thread.
int GameMatch::aiAliveCount() const
{
    return m_npcs.getNumUnits();
}

// The wave blob's radius for `budget` points of the CURRENT mix. Area — not radius — scales with
// the expected body count, so the areal density is the same in wave 1 and wave 20: the old formula
// grew the radius LINEARLY off the REMAINING budget and capped at 30 m, which meant a big wave
// stacked hundreds of bodies on one disc (parked bodies do not push apart, so the pile only
// resolved as physics shoved them out once a player came near) AND the disc SHRANK as the wave
// trickled, packing the tail tightest.
//
// The blob is clamped to the ground plane and any point inside the barrier square is pushed back
// out (see tickCoopSpawns), so an oversized disc becomes a wide BAND hugging the barrier — the
// only direction with room, since the spawn band is just c_coopGroundEdge - c_coopHalfSize deep.
float GameMatch::waveSpawnRadius(float budget) const
{
    float weight = 0.0f, cost = 0.0f;
    for (const WaveMixEntry& e : m_waveMix)
    {
        weight += e.weight;
        cost += e.weight * waveCostOf(e.type);
    }
    const float meanCost = weight > 0.0f ? glm::max(cost / weight, 0.1f) : 1.0f;
    const float bodies = glm::max(budget, 0.0f) / meanCost;
    const float area = bodies * glm::max(m_waveSpawnAreaPerUnit, 1.0f);
    // 8 m floor keeps a handful of bodies from spawning on one spot; the ceiling is the world.
    return glm::clamp(std::sqrt(area / glm::pi<float>()), 8.0f, c_coopGroundEdge);
}

void GameMatch::queueWave()
{
    const int aiAlive = aiAliveCount(); // ambient + previous waves both count against the cap
    // BUDGET points, not a unit count: the alive cap converts conservatively at the CHEAPEST cost
    // (the worst-case body count a budget could buy).
    float cheapest = FLT_MAX;
    for (int t = 0; t < (int)ENpcType::Count; ++t)
        cheapest = glm::min(cheapest, waveCostOf((ENpcType)t));
    // Wave i (0-based) = base + growth per wave so far, where the growth itself climbs by "Wave
    // growth growth" every wave: base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)).
    const float budget = glm::min(nextWaveBudget(),
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
    // Size the blob for EVERYTHING still queued (a previous wave's tail included) — once, with the
    // mix that will spend it, so the density holds from the first body to the last.
    m_waveRadius = waveSpawnRadius(m_wavePendingBudget);
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
        // Cluster around the ring point in the blob queueWave sized for THIS wave's body count
        // (m_waveRadius — constant for the whole trickle, so the tail is as loose as the head).
        // The blob stays in the band OUTSIDE the barrier but inside the ground plane: a point that
        // drifted through the barrier line pushes back out along the wave's dominant axis, which
        // is what turns a big wave's oversized disc into a wide band along the barrier face.
        // A few rolls against the recent spawn points: a spot inside a body placed a moment ago
        // is rejected (parked bodies never push each other apart until a player is near).
        constexpr float c_spawnSpacing = 2.0f; // > two grunt radii
        glm::vec3 pos;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const float a = glm::linearRand(0.0f, glm::two_pi<float>());
            const float r = m_waveRadius * std::sqrt(glm::linearRand(0.0f, 1.0f)); // uniform by area
            pos = m_waveOrigin + glm::vec3(std::cos(a) * r, 1.0f, std::sin(a) * r);
            pos.x = glm::clamp(pos.x, -c_coopGroundEdge, c_coopGroundEdge);
            pos.z = glm::clamp(pos.z, -c_coopGroundEdge, c_coopGroundEdge);
            if (glm::abs(pos.x) < c_coopHalfSize + 1.5f && glm::abs(pos.z) < c_coopHalfSize + 1.5f)
            {
                if (glm::abs(m_waveOrigin.x) >= glm::abs(m_waveOrigin.z))
                    pos.x = glm::sign(m_waveOrigin.x) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
                else
                    pos.z = glm::sign(m_waveOrigin.z) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
            }
            bool clear = true;
            for (int i = 0; i < m_waveRecentCount && clear; ++i)
                clear = glm::distance(m_waveRecent[i], glm::vec2(pos.x, pos.z)) >= c_spawnSpacing;
            if (clear)
                break;
        }
        m_waveRecent[m_waveRecentNext] = glm::vec2(pos.x, pos.z);
        m_waveRecentNext = (m_waveRecentNext + 1) % c_waveRecentSpawns;
        m_waveRecentCount = glm::min(m_waveRecentCount + 1, c_waveRecentSpawns);
        spawns.push_back({ .pos = pos,
            .orderDest = m_waveDest + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f)),
            .type = type, .team = (uint8)CoopAiTeam, .hasOrder = true });
    }
    while (budget > 0 && m_ambientPendingBudget > 0.0f)
    {
        --budget;
        // AMBIENT units come in small GROUPS: a cluster anchored on a random REACHABLE open cell
        // of the generated map (uniform by area, outside the safe ring, reachable from the Base
        // by the flood fill's guarantee), its bodies scattered in a disc around the anchor and
        // slid off any rock edge — a loose blob per group instead of one body per cell, which
        // lined up on the 10 m lattice. The Nav team fields never pull them (they cover the whole
        // map, which marched every scattered unit to the base) — only the local search aggroes
        // them, so an ambient group holds its patch until players expand near it. Wave units
        // above stay field-driven after their order releases.
        if (m_coopMap.reachable.empty())
            break;
        if (m_ambientSpawn.remaining <= 0)
        {
            const int cell = m_coopMap.reachable[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)m_coopMap.reachable.size()),
                0, (int)m_coopMap.reachable.size() - 1)];
            const glm::vec3 center = coopCellCenter(cell);
            if (glm::length(glm::vec2(center.x, center.z)) < m_ambientSafeRadius)
                continue; // safe-ring reject: costs one budget tick, never the points
            // DISTANCE = DIFFICULTY: the group's archetype is gated by GEODESIC depth (BFS
            // distance from the Base over the generated map) exactly like waves gate by index —
            // the BAND is a window of recipes: a group rolls only recipes whose wave gate sits
            // within "Ambient recipe window" BELOW its depth band, so the near ring only rolls the
            // early recipes (swarm-grade) and the deep map rolls ONLY the elite-tier ones (giants,
            // titans, lobbers never appear near the Base, and the outer map never wastes a group
            // on a plain swarm). Costs then make far groups FEWER, TOUGHER bodies for the same
            // points. One recipe per group, so it reads as a unit type holding ground rather than
            // a random assortment.
            // "Ambient depth scale" < 1 reaches the top band before the map's deepest cell, so the
            // final tier (titans) is not confined to the corners: at 0.9 the mid-edges qualify.
            const float depth = glm::min((float)m_coopMap.depth[cell]
                / ((float)m_coopMap.maxDepth * m_ambientDepthScale), 1.0f);
            const int band = 1 + (int)(depth * (float)(c_maxArchetypeMinWave - 1) + 0.5f);
            int eligible[c_numWaveArchetypes];
            int numEligible = 0;
            for (int i = 0; i < c_numWaveArchetypes; ++i)
                if (c_waveArchetypes[i].minWave <= band
                    && c_waveArchetypes[i].minWave >= band - m_ambientRecipeWindow)
                    eligible[numEligible++] = i;
            if (numEligible == 0) // a gap in the gate table under the window: fall back to all unlocked
                for (int i = 0; i < c_numWaveArchetypes; ++i)
                    if (c_waveArchetypes[i].minWave <= band)
                        eligible[numEligible++] = i;
            m_ambientSpawn.archetype = eligible[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible), 0, numEligible - 1)];
            m_ambientSpawn.center = center + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f));
            m_ambientSpawn.radius = glm::linearRand(4.0f, 9.0f);
            m_ambientSpawn.remaining = (int)glm::linearRand(3.0f, 9.99f);
        }
        --m_ambientSpawn.remaining;
        ENpcType type = sampleArchetype(c_waveArchetypes[m_ambientSpawn.archetype]);
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
        spawns.push_back({ .pos = ambientPointNear(m_ambientSpawn.center, m_ambientSpawn.radius),
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
// everything placed on open ground (nodes, ambient spawns, move orders) is reachable too. Then the
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
    // The ground-edge ring, OUTSIDE the barrier: solid for everyone, so a wave shoved around in
    // its spawn band cannot be pushed off the world. Segments overlap at the corners — they are
    // static boxes, so that costs nothing.
    const size_t edgeStart = requests.size();
    for (float o = -c_edgeWallSpan + c_edgeWallStep * 0.5f; !m_coopMap.pvp && o < c_edgeWallSpan; o += c_edgeWallStep)
    {
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(o, 10.0f, -c_coopEdgeWall)) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(o, 10.0f, c_coopEdgeWall)) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(-c_coopEdgeWall, 10.0f, o), 1.0f, yaw90) });
        requests.push_back({ "Entities/Game/edgewall.pre", Transform(glm::vec3(c_coopEdgeWall, 10.0f, o), 1.0f, yaw90) });
    }
    oc::vector<EntityPtr> spawned = Globals::world.spawnBatch(requests, /*addRoots*/ false);
    for (size_t i = 0; i < spawned.size(); ++i)
    {
        if (!spawned[i])
            continue;
        spawned[i]->setName(i >= edgeStart ? "EdgeWall"
            : i >= rockCount ? "Barrier" : (i & 1) ? "RockMark" : "Rock");
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
    m_ambientSpawn.remaining = 0; // an ambient group anchored on the old map is void
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

// An ambient body position: a random point in the disc around the group anchor that lands on OPEN
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
    {
        if (!m_structures.structureRoute(i).empty())
            sendRoute(i); // barracks waypoint routes replay too
        if (isBarracksType(m_structures.structureType(i)))
            sendUnitType(i); // and the produced unit type
    }
    if (m_paused) // the joiner lands in a paused game: show it the box
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(1);
        Globals::networkManager.fireNetworkEvent("GPz", writer.data());
    }
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

void GameMatch::sendUnitType(int index)
{
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>(m_structures.structureUnitType(index));
    Globals::networkManager.fireNetworkEvent("GBu", writer.data());
}

static constexpr const char* c_gameSavePath = "Local/gamesave.txt"; // cwd = Assets/

// The PENDING TRICKLE: a wave is sized in points at queueWave and the bodies materialize over the
// following frames (tickCoopSpawns, "Spawns per frame"). Saving mid-wave used to drop everything
// not yet spawned, so an F9 during a big wave quietly shrank it. Everything the trickle reads is
// written here — the remaining points, WHERE they enter (m_waveOrigin) and march (m_waveDest), and
// the wave's ROLLED, JITTERED mix, which cannot be re-derived from the archetype table. The
// spacing ring (m_waveRecent) is deliberately left out: it only rejects spots for a few spawns.
void GameMatch::saveTrickle(AssetNode& root) const
{
    AssetNode& wave = root.addChild("WaveTrickle");
    wave.set("Pending", m_wavePendingBudget);
    wave.set("Origin", m_waveOrigin);
    wave.set("Dest", m_waveDest);
    wave.set("Radius", m_waveRadius); // the blob queueWave sized for this wave's body count
    wave.set("LastArchetype", oc::to_string(m_lastArchetype)); // keeps "never twice in a row"
    for (const WaveMixEntry& e : m_waveMix)
    {
        AssetNode& entry = wave.addChild("Mix"); // Type is the raw ENpcType int — append-only
        entry.values = { oc::to_string((int)e.type), oc::to_string(e.weight) };
    }
    // The world-start SCATTER trickles through the same budget, plus the group it is mid-way
    // through placing (anchored on this map's cells, which the load regenerates identically).
    AssetNode& ambient = root.addChild("AmbientTrickle");
    ambient.set("Pending", m_ambientPendingBudget);
    ambient.set("Center", m_ambientSpawn.center);
    ambient.set("Radius", m_ambientSpawn.radius);
    ambient.set("Remaining", oc::to_string(m_ambientSpawn.remaining));
    ambient.set("Archetype", oc::to_string(m_ambientSpawn.archetype));
}

// Counterpart of saveTrickle. MUST run after rebuildCoopMap, which voids the in-progress ambient
// group (its anchor belongs to the old map). A save with NO trickle nodes — every save before this
// existed — clears both budgets: that file IS the complete state, and letting the running session's
// own scatter continue on top of it would double-spawn.
void GameMatch::loadTrickle(const AssetNode& root)
{
    m_wavePendingBudget = 0.0f;
    m_ambientPendingBudget = 0.0f;
    m_ambientSpawn.remaining = 0;
    if (const AssetNode* wave = root.find("WaveTrickle"))
    {
        m_wavePendingBudget = glm::max(wave->find("Pending") ? wave->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_waveOrigin = wave->find("Origin") ? wave->find("Origin")->asVec3() : m_waveOrigin;
        m_waveDest = wave->find("Dest") ? wave->find("Dest")->asVec3() : m_waveDest;
        m_lastArchetype = wave->find("LastArchetype") ? wave->find("LastArchetype")->asInt() : -1;
        m_waveMix.clear();
        for (const AssetNode* entry : wave->findAll("Mix"))
        {
            if (m_waveMix.size() >= m_waveMix.capacity())
                break; // fixed_vector: a hand-edited save cannot overrun it
            const int type = glm::clamp(entry->asInt(0), 0, (int)ENpcType::Count - 1);
            m_waveMix.push_back({ (ENpcType)type, glm::max(entry->asFloat(1), 0.0f) });
        }
        // Older trickle saves have no Radius: re-derive it from what is LEFT, which is the best
        // this end can do — the original wave's total is not in the file.
        m_waveRadius = wave->find("Radius") ? glm::clamp(wave->find("Radius")->asFloat(), 8.0f, c_coopGroundEdge)
                                            : waveSpawnRadius(m_wavePendingBudget);
        if (m_waveMix.empty())
            m_wavePendingBudget = 0.0f; // nothing to sample: dropping the points beats a wrong mix
        else if (m_wavePendingBudget > 0.0f) // re-seed the lane the trickled units will follow
            Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    }
    if (const AssetNode* ambient = root.find("AmbientTrickle"))
    {
        m_ambientPendingBudget = glm::max(ambient->find("Pending") ? ambient->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_ambientSpawn.center = ambient->find("Center") ? ambient->find("Center")->asVec3() : glm::vec3(0.0f);
        m_ambientSpawn.radius = glm::max(ambient->find("Radius") ? ambient->find("Radius")->asFloat() : 6.0f, 0.0f);
        m_ambientSpawn.remaining = glm::max(ambient->find("Remaining") ? ambient->find("Remaining")->asInt() : 0, 0);
        m_ambientSpawn.archetype = glm::clamp(ambient->find("Archetype") ? ambient->find("Archetype")->asInt() : 0,
            0, c_numWaveArchetypes - 1);
    }
}

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
        // The wave clock: how many waves launched (sizes the next one) and the seconds to it, so a
        // load resumes the escalation instead of restarting at wave 1.
        root.set("WaveIndex", oc::to_string(m_waveIndex));
        root.set("WaveTimer", glm::max(m_waveTimer, 0.0f));
        root.set("MatchTime", m_matchTime); // the HUD clock (sim seconds since the world spawned)
        saveTrickle(root); // the wave/ambient spawns still queued (a save mid-wave loses nothing)
    }
    else if (m_coopMap.built && m_coopMap.pvp)
        root.set("PvpMap", oc::to_string((int)m_coopMap.pvpMap)); // the arena (EPvpMap index)
    // The LOCAL player's body position (its own capsule only — remote players are not saved).
    // Headless (no capsule) writes nothing, and a load without the key leaves the player put.
    if (m_player.entity())
        root.set("PlayerPos", m_player.bodyPos());
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
    // The wave clock (co-op saves; older saves without it keep the current clock).
    if (const AssetNode* n = root.find("WaveIndex"))
        m_waveIndex = glm::max(n->asInt(), 0);
    if (const AssetNode* n = root.find("WaveTimer"))
        m_waveTimer = glm::max(n->asFloat(), 0.0f);
    if (const AssetNode* n = root.find("MatchTime"))
        m_matchTime = glm::max(n->asFloat(), 0.0f);
    // The local player back to where it stood. Both call sites (F10 in updateWindowed, the
    // --scenario timer) are main thread and pre-physics, so the direct body setters are sanctioned.
    // Older saves carry no key and leave the player put; remote players keep their own positions.
    if (const AssetNode* n = root.find("PlayerPos"))
        m_player.teleport(n->asVec3(m_player.bodyPos()));
    loadTrickle(root); // resume the queued wave/ambient spawns (after rebuildCoopMap — see there)
    if (m_isServer)
    {
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            sendStructurePlaced(i);
            if (!m_structures.structureRoute(i).empty())
                sendRoute(i);
            if (isBarracksType(m_structures.structureType(i)))
                sendUnitType(i);
        }
        // (No cable replay: clients re-derive links from the mirrored segments.)
    }
}

void GameMatch::requestSetUnitType(uint32 id, uint8 unitType)
{
    if (!m_isClient)
    {
        m_structures.queueUnitTypeRequest(id, unitType, (uint8)m_team);
        return;
    }
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    writer.write<uint8>(unitType);
    Globals::networkManager.fireNetworkEvent("GqU", writer.data());
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
        // The output byte doubles as the barracks' POPULATION tally (no emitter there).
        writer.write<uint8>(isBarracksType(m_structures.structureType(i))
            ? (uint8)glm::clamp(m_structures.structurePopulation(i), 0, 255)
            : frac8(m_structures.structureOutputFrac(i), 1.0f));
        writer.write<uint8>(frac8(m_structures.structureFlowUtil(i), 1.0f));
        writer.write<uint8>((uint8)((m_structures.structurePowered(i) ? 1u : 0u)
            | (m_structures.structureBlueprint(i) ? 2u : 0u))); // status bits (health IS progress)
    }
    Globals::networkManager.fireNetworkEvent("GSt", writer.data());

    // GCb: build progress of cable/crossing BLUEPRINTS only (the segments under construction — a
    // small, changing set; built ones are silent). 5 B per record; a long unbuilt run past the cap
    // rotates through m_cableSyncCursor over consecutive sends.
    constexpr int c_maxCableRecords = 190; // 2 + 190 * 5 = 952 B, under the 1024 B event cap
    uint8 cableBuffer[1000];
    NetWriter cableWriter(cableBuffer);
    cableWriter.write<uint16>(0);
    uint16 cableCount = 0;
    const int total = m_structures.structureCount();
    int i = total > 0 ? m_cableSyncCursor % total : 0;
    for (int visited = 0; visited < total && cableCount < c_maxCableRecords; ++visited, i = (i + 1) % total)
    {
        if (!isCableOrCrossing(m_structures.structureType(i)) || !m_structures.structureBlueprint(i))
            continue;
        cableWriter.write<uint32>(m_structures.structureId(i));
        cableWriter.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMaxOf(i)));
        ++cableCount;
    }
    m_cableSyncCursor = i;
    if (cableCount == 0)
        return;
    cableWriter.writeAt(0, cableCount);
    Globals::networkManager.fireNetworkEvent("GCb", cableWriter.data());

    // GCf: the cable FILLS + throughput (transport cells per segment, the ~2 s average as a
    // fraction of the rate), rotating through the cable nodes from m_cableFillCursor — the
    // clients' cable visuals and the selected-cable label. 6 B each.
    {
        constexpr int c_maxFillRecords = 160; // 2 + 160 * 6 = 962 B, under the 1024 B event cap
        oc::vector<StructureSystem::CableMirror> fills;
        m_structures.collectCableFills(fills, m_cableFillCursor, c_maxFillRecords);
        if (!fills.empty())
        {
            uint8 fillBuffer[1000];
            NetWriter fillWriter(fillBuffer);
            fillWriter.write<uint16>((uint16)fills.size());
            for (const StructureSystem::CableMirror& f : fills)
            {
                fillWriter.write<uint32>(f.id);
                fillWriter.write<uint8>(f.fill);
                fillWriter.write<uint8>(f.util);
            }
            Globals::networkManager.fireNetworkEvent("GCf", fillWriter.data());
        }
    }
}

void GameMatch::applyPause(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    Globals::time.setPaused(paused);
    Log::info(paused ? "Game paused" : "Game resumed");
}

void GameMatch::requestPause(bool paused)
{
    if (m_isClient)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(paused ? 1 : 0);
        Globals::networkManager.fireNetworkEvent("GqZ", writer.data());
        return;
    }
    applyPause(paused);
    if (m_isServer)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(paused ? 1 : 0);
        Globals::networkManager.fireNetworkEvent("GPz", writer.data());
    }
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
        else if (name == "GCb")
        {
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 health = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorCableProgress(id, health / 255.0f);
            }
        }
        else if (name == "GCf")
        {
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 fill = reader.read<uint8>();
                const uint8 util = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorCableFill(id, fill, util);
            }
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
        else if (name == "GBu")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 unitType = reader.read<uint8>();
            if (!reader.overflowed())
                m_structures.mirrorUnitType(id, unitType);
        }
        else if (name == "GLt")
        {
            const float fx = reader.read<float>(), fy = reader.read<float>(), fz = reader.read<float>();
            const float tx = reader.read<float>(), ty = reader.read<float>(), tz = reader.read<float>();
            if (!reader.overflowed())
                m_npcs.addBeam(glm::vec3(fx, fy, fz), glm::vec3(tx, ty, tz));
        }
        else if (name == "GPz")
        {
            const uint8 paused = reader.read<uint8>();
            if (!reader.overflowed())
                applyPause(paused != 0);
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
    if (name == "GqZ") // any seated player may pause/resume for everyone
    {
        const uint8 paused = reader.read<uint8>();
        if (!reader.overflowed())
            requestPause(paused != 0);
        return;
    }
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
    else if (name == "GqU")
    {
        const uint32 id = reader.read<uint32>();
        const uint8 unitType = reader.read<uint8>();
        if (!reader.overflowed()) // team/barracks ownership + type range validated at apply
            m_structures.queueUnitTypeRequest(id, unitType, requestTeam(sender));
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

    // SIM LOD focus = every player: our capsule plus (server) each client's twin, so a unit is
    // never throttled near ANY player. Published before world.update reads it (main.cpp order).
    // PLUS FRIENDLY UNIT CLUSTERS: combat only runs inside the selection, so an army fighting far
    // from every player — and the enemies around it — would otherwise be dormant. Greedy clusters
    // over the non-AI units, refreshed every 0.25 s (the selection tolerates a frame of staleness
    // anyway): a unit farther than "Unit cluster focus radius" from every focus point seeds a new
    // one, up to the focus cap; the remaining slots go to the first clusters found in roster order.
    {
        glm::vec3 focus[World::MaxSimLodFocus];
        uint32 focusCount = 0;
        focus[focusCount++] = m_player.bodyPos();
        for (const auto& [id, p] : m_clientPlayers)
            if (p && focusCount < World::MaxSimLodFocus)
                focus[focusCount++] = p->pos;
        m_focusClusterTimer -= deltaSec;
        if (m_focusClusterTimer <= 0.0f)
        {
            m_focusClusterTimer = 0.25f;
            m_focusClusters.clear();
            const float r2 = m_focusClusterRadius * m_focusClusterRadius;
            const auto nearPoint = [&](const glm::vec3& a, const glm::vec3& b) {
                const glm::vec2 d(a.x - b.x, a.z - b.z);
                return glm::dot(d, d) < r2; };
            const auto nearAnyFocus = [&](const glm::vec3& p) {
                for (uint32 i = 0; i < focusCount; ++i)
                    if (nearPoint(focus[i], p))
                        return true;
                for (const glm::vec3& c : m_focusClusters)
                    if (nearPoint(c, p))
                        return true;
                return false; };
            for (const EntityPtr& e : m_npcs.units())
            {
                if (focusCount + (uint32)m_focusClusters.size() >= World::MaxSimLodFocus)
                    break;
                const GameUnitComponent* u = getComponent<GameUnitComponent>(e.get());
                if (!u || u->puppet || !u->alive() || (m_coop && u->team == CoopAiTeam))
                    continue;
                if (!nearAnyFocus(e->pos))
                    m_focusClusters.push_back(e->pos);
            }
            // The base fields as ZONES (tier 1 + band): the structures are Global (always ticking,
            // fields always projected), but a unit in their field beyond every focus point was
            // unselected — teleported by the far tick straight through the barrier.
            m_fieldZones.clear();
            m_structures.collectShieldBubbles(m_fieldZones, World::MaxSimLodZones);
            Globals::world.setSimLodZones(m_fieldZones.data(), (uint32)m_fieldZones.size());
        }
        for (const glm::vec3& c : m_focusClusters)
            if (focusCount < World::MaxSimLodFocus)
                focus[focusCount++] = c;
        Globals::world.setSimLodFocus(focus, focusCount);
    }

    if (m_isClient)
    {
        // Our team is the SERVER's assignment, carried on our capsule's puppet component by the
        // snapshot game blob (never derived from the clientId — see allocateClientTeam). It lands
        // a snapshot or two after adoption; follow it whenever it changes.
        if (m_player.team() != m_team)
        {
            m_team = m_player.team();
            GameUnitComponent::params.localTeam = m_team; // replicated units re-tint as their next snapshot lands
            m_player.setRespawnPos(teamStartPos((uint8)m_team));
            Log::info("We are team " + oc::to_string(m_team));
        }
        tickBaseHealing(deltaSec);
        tickMedicHealing(deltaSec); // own player only on a client (units heal on the server)
        m_structures.tickMirror(deltaSec);
        submitNavFeed(deltaSec); // obstacles only: the local player's move-order goal field
        submitWorldLabels(deltaSec);
        return;
    }

    const glm::vec3 playerPos = m_player.bodyPos();
    m_matchTime += deltaSec; // the HUD clock: authority sim time (a pause stops the sim delta)
    m_structures.tickAuthority(playerPos, deltaSec);
    tickBaseHealing(deltaSec);
    tickMedicHealing(deltaSec);
    // The ambient wander is a POST-UPDATE job: it only writes idle units' order fields and reads
    // the roster + the immutable co-op map, all stable once the entity pass is done, so it runs
    // during present instead of in front of the batch submit. Its orders land in the next pass.
    if (m_coop)
    {
        ProfileScope queueScope("Ambient wander queue", EProfileCategory::Game);
        Globals::jobSystem.submitPostUpdate([this, deltaSec] { tickAmbientWander(deltaSec); },
            { "Ambient wander", EProfileCategory::Game }, EJobPriority::Normal, 0, JobSystem::EPostUpdateBatch::Sim);
    }
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
    if (m_isServer) // turret lightning is a pure visual on clients: broadcast this frame's strikes
        for (const NpcSystem::Beam& beam : m_npcs.newBeams())
        {
            uint8 buffer[32];
            NetWriter writer(buffer);
            writer.write<float>(beam.from.x); writer.write<float>(beam.from.y); writer.write<float>(beam.from.z);
            writer.write<float>(beam.to.x);   writer.write<float>(beam.to.y);   writer.write<float>(beam.to.z);
            Globals::networkManager.fireNetworkEvent("GLt", writer.data());
        }
    if (m_coop)
    {
        tickWaves(deltaSec);
        tickCoopSpawns();
    }

    // Flow fields: obstacles + per-team sources staged for the NEXT frame's NavSystem::update
    // (which runs in main.cpp's kick/join window, BEFORE this bulk tick — one frame of source
    // latency, well inside nav's own async tolerances). The gather is a post-update job.
    submitNavFeed(deltaSec);

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
    submitWorldLabels(deltaSec); // the tick's mutations are done: the labels job may read the structures now
}

// The nav feed rides the post-update batch: everything it reads is stable once the entity pass is
// done (rosters change only on main during the tick, positions are the pass's output), and the
// Nav setters it ends with are legal there too — the batch joins at the top of the next frame,
// before that frame's NavSystem::update, and nothing on main touches Nav during present (the Nav
// field-step job in the same batch has only the fields). So the change-detect compare and the
// source copy run on the job as well. Same one-frame latency as the old inline feed, none of it
// in front of the entity batch submit.
void GameMatch::submitNavFeed(float deltaSec)
{
    // Sim batch: joined before the next frame's entity-change drains (the first point main mutates
    // rosters), so the sweep also runs through the input/camera stretch.
    Globals::jobSystem.submitPostUpdate([this, deltaSec] { gatherNavFeed(deltaSec); }, { "Game nav feed", EProfileCategory::Game },
        EJobPriority::Normal, 0, JobSystem::EPostUpdateBatch::Sim);
}

void GameMatch::gatherNavFeed(float deltaSec)
{
    ProfileScope scope("Game nav feed", EProfileCategory::Game);
    // THE UNIT SWEEP IS SLICED: Nav consumes sources once per "Rebuild interval", so the roster is
    // walked over that many frames — ceil(n * dt / interval) units a frame, a constant slice — and
    // the lists publish when the cursor wraps. Culling (see below) tests each unit against the team
    // cell hash the PREVIOUS cycle built: one interval stale, a metre or two of motion against
    // 64 m cells. Clients (no unit sim) have an empty sweep and publish every frame.
    const float cell = glm::max(m_navUnitSourceReach, 8.0f);
    const auto cellKey = [&](const glm::vec3& p) {
        return (uint64)(uint32)(int)glm::floor(p.x / cell) << 32 | (uint32)(int)glm::floor(p.z / cell); };
    // A unit's field is read only by OTHER teams' units within navFollowRadius of it, so a unit
    // with no other team's unit or player anywhere near contributes nothing but flood area — and
    // thousands of ambient enemies as sources tiled the whole map with the AI team's field. A
    // unit stays a source when the 3x3 cells around it hold another team (reach .. 2x reach).
    const auto otherTeamNear = [&](const glm::vec3& p, uint8 team) {
        const int cx = (int)glm::floor(p.x / cell), cz = (int)glm::floor(p.z / cell);
        const uint8 own = uint8(1u << team);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (const auto it = m_navCellTeams.find((uint64)(uint32)(cx + dx) << 32 | (uint32)(cz + dz));
                    it != m_navCellTeams.end() && (it->second & ~own))
                    return true;
        return false; };
    const oc::span<const EntityPtr> units = m_npcs.units();
    const uint32 n = m_isClient ? 0u : (uint32)units.size();
    if (n > 0)
    {
        const float interval = glm::max(Globals::navSystem.rebuildInterval(), 1e-3f);
        const uint32 slice = glm::max((uint32)glm::ceil((float)n * deltaSec / interval), 1u);
        const uint32 end = glm::min(n, m_navFeedCursor + slice);
        for (uint32 i = m_navFeedCursor; i < end; ++i)
        {
            Entity* e = units[i].get();
            const GameUnitComponent* u = getComponent<GameUnitComponent>(e);
            if (!u || u->puppet || !u->alive() || u->team >= Nav::MaxTeams)
                continue;
            m_navCellTeamsNext[cellKey(e->pos)] |= uint8(1u << u->team);
            if (otherTeamNear(e->pos, (uint8)u->team))
                m_navUnitSources[u->team].push_back(Nav::NavSource{ e->pos, glm::max(u->bodyRadius, 0.25f), 0, 3 });
            // Pre-emption point every 256 units (see JobSystem::preemptionPoint): a Normal job
            // in the present window, so High work (physics tasks, spatial chunks) gets through.
            // Every unit is fully recorded before the point - the sweep resumes at i + 1.
            if ((i & 255) == 255)
                Globals::jobSystem.preemptionPoint();
        }
        m_navFeedCursor = end;
        if (m_navFeedCursor < n)
            return; // mid-cycle: nothing publishes this frame
    }

    // CYCLE END: the cheap parts (players into the hash, obstacles + structure sources, player
    // sources), then the publish.
    Globals::jobSystem.preemptionPoint(); // sweep complete, cycle end not started
    m_navFeedCursor = 0;
    const auto markPlayer = [&](Entity* e, uint8 team) {
        if (e && team < Nav::MaxTeams)
            m_navCellTeamsNext[cellKey(e->pos)] |= uint8(1u << team); };
    markPlayer(m_player.entity(), (uint8)m_team);
    for (const auto& [id, p] : m_clientPlayers)
        markPlayer(p.get(), (uint8)requestTeam(id));
    m_navCellTeams.swap(m_navCellTeamsNext);
    m_navCellTeamsNext.clear();
    // Obstacles: every structure footprint (the same half-extent math as cellsFree) over the static
    // border ring. Change-detected inside Nav, so rebuilding the list per cycle costs a hash.
    m_navObstacles.assign(m_wallObstacles.begin(), m_wallObstacles.end());
    for (oc::vector<Nav::NavSource>& v : m_navSources)
        v.clear();
    for (const StructureSystem::Ref& s : m_structures.structures())
    {
        // Cables/crossings/solars are WALK-THROUGH: no nav obstacle (units path straight over
        // them) and never a NavSource (enemies do not march at power lines).
        if (isWalkThrough(s.type))
            continue;
        const float half = StructureSystem::footprintCellsOf(s.type) * StructureSystem::GridCellSize * 0.5f;
        const glm::vec2 c(s.entity->pos.x, s.entity->pos.z);
        // WALLS are BREACHABLE obstacles ("Wall breach cost"): the field routes through one where
        // the detour is longer, the units walk into it and chew it down instead of skirting it.
        const uint8 breach = s.type == EStructureType::Wall && !s.state->blueprint
            ? (uint8)glm::clamp(m_structures.wallBreachCost(), 1, 254) : (uint8)0;
        m_navObstacles.push_back(Nav::NavObstacle{ c - half, c + half, breach });
        // Sources: what units of OTHER teams walk toward — the same filter the local search used
        // (alive, not the invulnerable Base). Clients run no unit sim: obstacles only, for the
        // local player's goal field.
        if (m_isClient || s.state->invulnerable || !s.state->alive() || s.state->team >= Nav::MaxTeams)
            continue;
        m_navSources[s.state->team].push_back(Nav::NavSource{
            s.entity->pos, glm::max(s.state->meleeRadius, half), s.state->structureId, 0 });
    }
    // Enemy UNITS are targets too (unit-vs-unit combat): the cycle's sliced sweep above collected
    // them (the NpcSystem roster IS the world-wide unit list — no spatial sweep).
    for (uint32 t = 0; t < Nav::MaxTeams; ++t)
    {
        m_navSources[t].insert(m_navSources[t].end(), m_navUnitSources[t].begin(), m_navUnitSources[t].end());
        m_navUnitSources[t].clear();
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
    // The Nav setters (change-detected: a compare per source, a copy on change) — on the job, see
    // submitNavFeed for why that is legal.
    Globals::jobSystem.preemptionPoint(); // lists complete, publish not started
    {
        ProfileScope publishScope("Game nav publish", EProfileCategory::Game);
        Globals::navSystem.setObstacles(m_navObstacles);
        for (uint32 t = 0; t < Nav::MaxTeams; ++t)
            Globals::navSystem.setTeamSources(t, m_navSources[t]);
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
bool GameMatch::isRootPage() const
{
    return m_mode != EPlayerMode::Build || m_buildCategory < 0 || m_buildCategory == c_cableCategory;
}

void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    // HOVER CARDS: the full type name + StructureSystem's description (one sentence, then the
    // exact per-second flows). Formatted from the LIVE tweaks, so they are rebuilt on a slow
    // cadence instead of per frame — this runs every frame for the counts, and 26 formatted
    // strings a frame is pure waste for text that only moves when someone drags a tweak. The
    // cadence is REAL seconds, not a frame count: the rate must not follow the frame rate, and a
    // paused game still updates its cards. The setters below then early-out on an unchanged
    // string, so a steady frame allocates nothing.
    const double now = Globals::time.getElapsedSec(); // real clock: the cards refresh while paused too
    if (now - m_typeCardTime >= 1.0)
    {
        m_typeCardTime = now;
        for (int t = 0; t < (int)EStructureType::Count; ++t)
            m_typeCards[t] = oc::string(structureTypeName((EStructureType)t)) + "\n"
                + m_structures.describeType((EStructureType)t);
    }
    uint32 used = 0; // slots this page filled; everything else is cleared at the end
    const auto itemSlot = [&](int slot, EStructureType type)
    {
        hud.setSlot(slot, c_structureShortNames[(int)type],
            m_structures.affordableCount(type, (uint8)m_team));
        hud.setSlotTooltip(slot, m_typeCards[(int)type]);
        used |= 1u << slot;
    };
    const auto plainSlot = [&](int slot, const char* label, const char* card)
    {
        hud.setSlot(slot, label, 0);
        hud.setSlotTooltip(slot, card);
        used |= 1u << slot;
    };
    if (isRootPage())
    {
        for (int i = 0; i < c_numCategories; ++i)
            plainSlot(i, c_buildCategories[i], c_buildCategoryCards[i]);
        for (int i = 0; i < (int)oc::size(c_cableItems); ++i)
            itemSlot(c_rootCableSlot + i, c_cableItems[i]);
        const bool cableArmed = m_mode == EPlayerMode::Build && m_buildSelection >= 0;
        hud.selectSlot(m_mode == EPlayerMode::Delete ? c_rootDeleteSlot
                     : cableArmed ? c_rootCableSlot + m_buildSelection : -1);
    }
    else
    {
        const oc::span<const EStructureType> items = buildCategoryItems(m_buildCategory);
        for (int i = 0; i < (int)items.size() && i < c_rootDeleteSlot; ++i)
            itemSlot(i, items[i]);
        hud.selectSlot(m_buildSelection);
    }
    plainSlot(c_rootDeleteSlot, "DEL", "Delete\nClick a structure to demolish it."); // X on EVERY page
    plainSlot(c_cancelSlot, "CNCL", "Cancel\nBack to Select mode.");                 // C on EVERY page
    for (int i = 0; i < GameHud::NumSlots; ++i)
        if (!(used & (1u << i)))
            hud.clearSlot(i);
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
    m_buildSelection = -1;
    if (mode != EPlayerMode::Build)
        m_buildCategory = -1; // back to the root page
    refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category entry logs its own line (activateSlot)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish (one, then back to Select) — X cancels"); break;
    case EPlayerMode::Select: Log::info("Select mode: click inspects, RMB routes / moves — Q/W build, A/S/D cables, X delete"); break;
    }
}

// One level back: a half-finished two-click step drops first, then the armed item disarms, then
// the category page (or Delete mode) returns to Select. Esc/Tab (the C "Cancel" slot goes straight
// back to Select).
void GameMatch::cancelOneLevel()
{
    if (m_mode == EPlayerMode::Build && m_buildSelection >= 0)
    {
        if (m_lanceAiming || m_wallPlacing || m_cablePainting)
        {
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
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
    if (isRootPage())
    {
        // ROOT page
        if (slot < c_numCategories)
        {
            setMode(EPlayerMode::Build);
            m_buildCategory = slot;
            m_buildSelection = -1;
            refreshBuildHotbar();
            Log::info(oc::string("Build: ") + c_buildCategoryNames[slot]
                + " — grid keys arm an item, LMB places, RMB cancels, C/Esc back");
        }
        else if (slot >= c_rootCableSlot && slot < c_rootCableSlot + (int)oc::size(c_cableItems))
        {
            // A cable arms straight from the root page (the hidden cable category).
            setMode(EPlayerMode::Build);
            m_buildCategory = c_cableCategory;
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
            m_cablePendingValid = false;
            m_buildSelection = slot - c_rootCableSlot;
            refreshBuildHotbar();
        }
        else if (slot == c_rootDeleteSlot)
            setMode(m_mode == EPlayerMode::Delete ? EPlayerMode::Select : EPlayerMode::Delete);
        else if (slot == c_cancelSlot)
            setMode(EPlayerMode::Select); // cancels Delete mode / an armed cable; a no-op in Select
        return;
    }
    // CATEGORY page
    if (slot == c_cancelSlot)
    {
        setMode(EPlayerMode::Select); // straight back, whatever was armed
        return;
    }
    if (slot == c_rootDeleteSlot)
    {
        setMode(EPlayerMode::Delete); // X works on every page, not just the root
        return;
    }
    if (slot >= (int)buildCategoryItems(m_buildCategory).size() || slot >= c_rootDeleteSlot)
        return; // empty slot
    m_lanceAiming = false; // switching items drops half-done aims/flows
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cablePendingValid = false;
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
    // Escape/Tab: one level back.
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
    m_cablePendingValid = false;
    refreshBuildHotbar(); // the slot highlight follows in the same frame
}

// The Crossing's axis rotation for a ±X/±Z facing — the SAME formula placeStructure applies to
// the request's facing, so a client-side cellsFree probes exactly the cells it will cover.
static glm::quat crossingRotation(const glm::vec2& dir)
{
    return glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
}

// Fill the auto-bent L between two snapped 1-cell positions — the dominant leg first, then the
// perpendicular one — requesting a placement per FREE cell (occupied cells are skipped, so a line
// across an existing run just fills the gaps). The stroke HOLDS its newest cell back until the
// cell after it is known (m_cablePending): when that next cell holds a plain cable of ANOTHER
// medium, the stroke's OWN medium's Crossing goes over it with its long axis along the stroke —
// the held cell and the cell beyond are its two END cells and are never painted — so a stroke
// across a foreign run bridges it instead of leaving a gap. A crossing the cells refuse falls
// back to the plain skip.
void GameMatch::placeCableLine(EStructureType armed, const glm::vec3& from, const glm::vec3& to)
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
    const auto sameCell = [](const glm::vec3& a, const glm::vec3& b) {
        return glm::abs(a.x - b.x) < step * 0.5f && glm::abs(a.z - b.z) < step * 0.5f; };
    // points[0] is `from`: the held-back cell when one is pending, else already dealt with.
    for (int i = m_cablePendingValid ? 0 : 1; i < count; ++i)
    {
        if (i == count - 1)
        {
            m_cablePending = points[i]; // held until the next sample or the release
            m_cablePendingValid = true;
            m_cablePaintLast = points[i];
            return;
        }
        const glm::vec3& next = points[i + 1];
        const int foreign = m_structures.bridgeableAt(next);
        if (foreign >= 0 && conduitMediumOf(m_structures.structureType(foreign)) != cableMediumOf(armed))
        {
            const glm::vec2 dir(glm::sign(next.x - points[i].x), glm::sign(next.z - points[i].z));
            const EStructureType crossing = crossingForMedium(cableMediumOf(armed));
            // planCrossing, not cellsFree: an END cell holding our OWN medium is replaced, so a
            // stroke crosses a foreign line even where its own run already stands.
            if (m_structures.planCrossing(crossing, next, crossingRotation(dir), (uint8)m_team).valid
                && !StructureSystem::actorInFootprint(crossing, next))
            {
                requestPlace(crossing, next, -1, glm::vec3(dir.x, 0.0f, dir.y));
                // points[i] and the cell beyond `next` are the crossing's END cells: never cables.
                const glm::vec3 farEnd = next + glm::vec3(dir.x, 0.0f, dir.y) * step;
                i += i + 2 < count && sameCell(points[i + 2], farEnd) ? 2 : 1;
                continue;
            }
        }
        if (m_structures.cellsFree(armed, points[i]))
            requestPlace(armed, points[i], -1, glm::vec3(0.0f));
    }
    m_cablePendingValid = false;
    m_cablePaintLast = points[count - 1];
}

void GameMatch::finishCableStroke(EStructureType armed)
{
    if (m_cablePendingValid && m_structures.cellsFree(armed, m_cablePending))
        requestPlace(armed, m_cablePending, -1, glm::vec3(0.0f));
    m_cablePendingValid = false;
    m_cablePainting = false;
}

// Cable segments place by PAINTING (see Match.ixx): a press starts the stroke at its cell and,
// while held, the stroke places the cells the cursor crosses (L-filled between samples so the run
// never breaks). Release ends the stroke; a plain click is a one-cell stroke.
void GameMatch::updateCablePlacement(const Camera& camera, EStructureType armed, bool confirmEdge)
{
    const Aim aim = computeAim(camera, armed);
    if (m_cablePainting)
    {
        if (!m_lmbDown)
            finishCableStroke(armed); // release ends the stroke (and lands the held cell)
        else if (aim.valid && glm::distance(glm::vec2(aim.pos.x, aim.pos.z),
            glm::vec2(m_cablePaintLast.x, m_cablePaintLast.z)) > 0.1f)
            placeCableLine(armed, m_cablePaintLast, aim.pos);
        if (aim.valid)
            drawStructureGhost(armed, aim.pos, packColor(glm::vec3(0.3f, 1.0f, 0.4f)));
        return;
    }
    if (!aim.valid)
    {
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawStructureGhost(armed, aim.pos, color);
    if (confirmEdge && aim.affordable)
    {
        m_cablePainting = true; // hold + drag paints from here
        m_cablePending = aim.pos; // the press's cell is held back one step (see placeCableLine)
        m_cablePendingValid = true;
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
        // Nothing armed — browsing the category: clicks inspect, LMB drag box-selects units and
        // RMB sets barracks routes / orders, exactly as Select mode. Only an ARMED item takes the
        // clicks away.
        updateRightClickActions(camera, cancelEdge);
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        updateUnitSelection(camera);
        return;
    }
    // RMB CANCELS, one step at a time: a half-finished flow (Lance aim, Wall line, cable paint
    // stroke, Crossing aim) drops first, and the next RMB disarms the item itself. Only once nothing
    // is armed does RMB go back to its Select-mode meaning (barracks route / move order) above.
    const EStructureType armed = buildCategoryItems(m_buildCategory)[m_buildSelection];

    // Cable segments have their own paint input (press + drag).
    if (isCableType(armed))
    {
        if (cancelEdge)
        {
            if (m_cablePainting)
                finishCableStroke(armed); // the held cell was already shown painted — it lands
            else
                disarmBuild();
            return; // NOT consumed: the same press also walks the player
        }
        updateCablePlacement(camera, armed, confirmEdge);
        return;
    }

    // (Crossings are never armed: the paint stroke places them — placeCableLine.)

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

    // Wall DRAG: the press anchored the line start; while held, segments preview along the line
    // to the cursor, and the RELEASE queues one placement per segment. RIGHT-click cancels the
    // stroke before the release.
    if (m_wallPlacing)
    {
        if (cancelEdge)
        {
            m_wallPlacing = false;
            return; // NOT consumed: the same press also walks the player
        }
        const bool release = !m_lmbDown;
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
            if (release)
                for (int s = 0; s < count; ++s)
                    requestPlace(EStructureType::Wall, points[s], -1, glm::vec3(0.0f));
        }
        if (release)
            m_wallPlacing = false; // a release off the ground (no valid end) just drops the stroke
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
                                                  // (crossings are auto-placed by the paint stroke)
    if (isEmitterType(aim.type)) // show the field footprint the powered variant would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterReachOf(aim.type) * 0.5f, color, 32);
    if (aim.type == EStructureType::Constructor) // show the build/repair reach it would cover
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.constructorRange(), color, 40);
    if (aim.type == EStructureType::House) // show how far it links to a barracks
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.houseLinkRadius(), color, 48);
    if (aim.type == EStructureType::MedicStation) // show the heal reach
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.medicHealRadius(), color, 48);
    if (confirmEdge && aim.affordable)
    {
        if (aim.type == EStructureType::Lance)
        {
            m_lanceAiming = true; // first click anchors; the next click aims the cone
            m_lancePendingPos = aim.pos;
        }
        else if (aim.type == EStructureType::Wall)
        {
            m_wallPlacing = true; // the press anchors the line start; the release ends it
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
    {
        requestDemolish(m_structures.structureId(hover)); // validated in the authority tick (server)
        setMode(EPlayerMode::Select); // one demolish per arm: the Delete button releases itself
    }
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
            NpcSystem::queryVisibleUnits(camera, FLT_MAX, units); // a box select reaches every unit on screen, however far
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
    // PvP: the loaded units' spatial entries link at the next commitFrame — the select-all query
    // runs from the next update() (issueScenarioOrder), marching everything on the other Base.
    // CO-OP: no order — there is no enemy Base to march on, and the session is measured as saved
    // (the player holds position, the AI waves and ambient groups carry on).
    m_scenarioOrderPending = !m_coop;
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
        m_player.moveTarget().x, m_player.moveTarget().z, m_scenarioOrderTries, laneSeeded ? "plan queued" : "NOT queued (no raster yet)"));
}

// THE right-click move order, shared by the RMB handler and the profiling scenario: the player and
// the selected units walk to a world position (a fresh order: the lane is seeded from the group).
bool GameMatch::moveOrderAt(const glm::vec3& worldPos, bool includePlayer)
{
    // A click that lands inside co-op rock clamps to the nearest open cell — a target on blocked
    // cells fails the lane A* and every unit's own plan request (the pointOutsideFootprint rule,
    // applied to terrain).
    const glm::vec3 dest = clampToOpenGround(glm::vec3(worldPos.x, 0.0f, worldPos.z));
    if (includePlayer)
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
    // A FRESH order seeds a planned LANE from the group to the destination: one A* (a job; Nav
    // writes the lane on a later update), into the team flow, and the units follow it as crowd flow — the group routes around
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
        hover >= 0 && !isWalkThrough(m_structures.structureType(hover)))
        return; // on a building: the caller turns it into a MOVE order (walk-through pieces are ground)
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

// World-anchored UI: a health bar above every damageable structure, plus name/HP/power info on the
// selected one. Projected with THIS frame's final camera (worldToScreen — captured in
// updateWindowed), replaced wholesale each frame; the overlay just paints at the given viewport
// pixels. A JOB: submitted at the END of the game tick (structures settled for the frame; only the
// entity pass runs alongside, and it changes field values, never the rosters or the structure
// list — torn float reads are fine for a bar) and joined by main right before the widget pass is
// queued (joinWorldLabels), which is what consumes the labels. GameHud's writes are mutexed.
void GameMatch::submitWorldLabels(float deltaSec)
{
    if (!m_labelsCameraValid)
        return; // headless / no windowed tick this frame
    m_labelsDelta = deltaSec; // the warning timers age by this inside the job
    Globals::jobSystem.submit([this] { buildWorldLabels(); }, { "Game world labels", EProfileCategory::Game },
        EJobPriority::Normal, &m_labelsCounter);
}

void GameMatch::joinWorldLabels()
{
    Globals::jobSystem.wait(m_labelsCounter);
}

// The most pressing PROBLEM over an own-team structure, as a short badge (nullptr = nothing wrong).
// These are the states the bars alone do not explain: a consumer with no power, an input that never
// arrives, an output with nowhere to go, a full store, a capped barracks. Ordered by severity —
// one badge per structure, the first hit wins. Blueprints are inert BY DESIGN and never warn.
const char* GameMatch::structureWarning(int index, glm::vec3& color) const
{
    const EStructureType type = m_structures.structureType(index);
    if (isCableOrCrossing(type) || m_structures.structureBlueprint(index)
        || m_structures.structureTeam(index) != (uint8)m_team)
        return nullptr;
    const GameStructureComponent& s = *m_structures.structures()[index].state;
    const auto linked = [&](uint8 medium) { return (s.attachedMask & (1u << medium)) != 0; }; // a run touches it
    constexpr glm::vec3 c_red(1.0f, 0.35f, 0.3f), c_orange(1.0f, 0.62f, 0.25f), c_amber(1.0f, 0.85f, 0.35f);

    // 1) A MEDIUM WITH NO CABLE OF ITS OWN. Whichever media a structure MOVES — one it eats, one it
    //    makes, one it banks — a missing line of that medium is dead weight: the input can never
    //    arrive, or the output has nowhere to go. Structural, so it does NOT gate on the current
    //    stock: a cabled-but-starved or cabled-but-backed-up machine is not a badge (its bars say
    //    that, and it resolves itself), while an uncabled one never resolves.
    //    The BASE is exempt: it is the hub, self-generates, and starts every match bare.
    const int node = m_structures.structureNodeIndex(index);
    const bool fuelNode = node >= 0 && node < m_structures.nodeCount()
        && m_structures.nodeType(node) == ENodeType::Fuel; // an extractor makes ONE of the two
    // ENERGY: burnt by the emitters/machines/turrets, banked by batteries, made by gen + solar.
    const bool energy = hasShieldEmitter(type) || isBarracksType(type)
        || type == EStructureType::Extractor || type == EStructureType::Fabricator
        || type == EStructureType::Constructor || type == EStructureType::MedicStation
        || type == EStructureType::Turret || type == EStructureType::Generator
        || type == EStructureType::Solar || type == EStructureType::Battery;
    // FUEL: burnt by generators and fabricators, banked by tanks, made by a fuel-node extractor.
    const bool fuel = type == EStructureType::Generator || type == EStructureType::Fabricator
        || type == EStructureType::FuelTank || (type == EStructureType::Extractor && fuelNode);
    // MINERALS: spent by constructors, banked by silos, made by fabricators + mineral extractors.
    const bool minerals = type == EStructureType::Constructor || type == EStructureType::MineralSilo
        || type == EStructureType::Fabricator || (type == EStructureType::Extractor && !fuelNode);
    if (type != EStructureType::Base)
    {
        if (energy && !linked(0))
        {
            color = c_red;
            return "No power cable";
        }
        if (fuel && !linked(1))
        {
            color = c_orange;
            return "No pipeline";
        }
        if (minerals && !linked(2))
        {
            color = c_amber;
            return "No conveyor";
        }
    }
    // 2) POPULATION CAPPED: the build bar fills but the unit can never be born (build houses).
    if (isBarracksType(type)
        && s.barracks.population + (int)s.barracks.spawnPop > s.barracks.popCap)
    {
        color = c_amber;
        return "Pop full";
    }
    return nullptr;
}

void GameMatch::buildWorldLabels()
{
    ProfileScope scope("Game world labels", EProfileCategory::Game);
    const Camera& camera = m_labelsCamera;
    // Both are the job's kept scratch, SWAPPED into GameHud at the end (last frame's list comes
    // back with its capacity): no list allocation per frame, only the label strings that outgrow
    // the SSO buffer (the selected structure's info block).
    oc::vector<HudWorldLabel>& labels = m_labelsScratch;
    labels.clear();
    labels.reserve(m_structures.structureCount());
    HudPopup& popup = m_labelsPopup; // the selected own barracks' unit-type picker (inactive = none)
    popup.active = false;
    popup.title.clear();
    popup.buttons.clear();
    const Rect& viewport = m_labelsViewport;
    // CULLING. worldToScreen only rejects what is BEHIND the camera, so without these every
    // structure on the map and every unit in the frustum built a label, was copied into GameHud
    // and walked by the widget pass, which then clipped most of them. A label past "Label max
    // distance" is unreadable and one outside the viewport (plus the overlay's own margin) is
    // never drawn; neither is worth building. The selected structure keeps its label regardless.
    // The distance is from the PLAYER, not the camera: the top-down camera sits well above and
    // behind the capsule, and what matters is what is near the player (the camera is the
    // fallback when there is no player entity - the editor, a spectating client).
    const Entity* cullEntity = m_player.entity();
    const glm::vec3 cullCenter = cullEntity ? cullEntity->pos : camera.position;
    const float maxDist2 = m_labelMaxDistance * m_labelMaxDistance;
    const auto inRange = [&](const glm::vec3& p)
    {
        const glm::vec3 d = p - cullCenter;
        return glm::dot(d, d) <= maxDist2;
    };
    const glm::vec2 vpMin = glm::vec2(viewport.min) - 100.0f;
    const glm::vec2 vpMax = glm::vec2(viewport.max) + 100.0f;
    const auto onScreen = [&](const glm::vec2& p)
    {
        return p.x >= vpMin.x && p.x <= vpMax.x && p.y >= vpMin.y && p.y <= vpMax.y;
    };
    // PROBLEM BADGES, on a per-structure JITTERED ~1 s timer: the check scans a structure's links,
    // and every state it reports changes on the timescale of a player's actions, so re-running it
    // per structure per frame is waste. The jitter is a STABLE per-id phase (a hash of the
    // structure id), so a batch placed or loaded together spreads over the interval instead of
    // re-checking in lockstep forever. The result rides the roster entry (Ref::warning).
    {
        constexpr float c_warningInterval = 1.0f;
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            float& timer = m_structures.structureWarningTimer(i);
            timer -= m_labelsDelta;
            if (timer > 0.0f)
                continue;
            const uint32 hash = m_structures.structureId(i) * 2654435761u;
            const float phase = 0.75f + 0.5f * (float)(hash >> 8) / (float)(1u << 24); // 0.75 .. 1.25
            timer = c_warningInterval * phase;
            glm::vec3 color(1.0f);
            const char* warning = structureWarning(i, color);
            m_structures.setStructureWarning(i, warning, color);
        }
    }
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
        const glm::vec3 anchor = m_structures.structureLabelAnchor(i);
        if (i != selected && !inRange(anchor))
            continue;
        if (!camera.worldToScreen(viewport, anchor, label.screenPos) || !onScreen(label.screenPos))
            continue;
        label.title = c_structureShortNames[(int)type]; // the selected one overrides w/ full name
        if (const char* warning = m_structures.structureWarning(i)) // the cached problem bubble
        {
            label.warning = warning;
            label.warningColor = m_structures.structureWarningColor(i);
        }
        const bool consumer = hasShieldEmitter(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator || type == EStructureType::MedicStation;
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
        // on anything that runs on minerals alone, energy (yellow) elsewhere (barracks included).
        const bool fuelBar = type == EStructureType::Generator || type == EStructureType::FuelTank;
        const bool mineralBar = type == EStructureType::MineralSilo
            || (mineralCap > 0.0f && energyCap <= 0.0f);
        // The STORE bars are opt-in when unselected: only the prefabs that author
        // `AlwaysShowResources true` (storage, emitters, barracks — the stores a player watches at
        // a glance) carry them around, everything else shows them while SELECTED. The health bar
        // is unaffected: damage always shows one.
        const bool showResources = i == selected
            || m_structures.structures()[i].state->alwaysShowResources;
        if (m_structures.structureBlueprint(i) || !showResources)
        {
        } // blueprint: no second bar — the (blue) health bar IS the build progress
        else if (isCableOrCrossing(type))
        {
            // A conduit's second bar is its THROUGHPUT: the ~2 s average of cells leaving the
            // segment against its out-rate, hued by medium.
            StructureSystem::CableInfo ci;
            if (m_structures.cableInfo(i, ci))
            {
                label.bar2Value = ci.movedPerSec;
                label.bar2Max = ci.ratePerSec;
                label.bar2Color = ci.medium == 1 ? glm::vec3(1.0f, 0.6f, 0.2f)
                    : ci.medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(1.0f, 0.9f, 0.3f);
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
            // A barracks' store IS its build bar (capacity = the unit's cost) and a turret's IS its
            // reload (capacity = one shot): green progress, not energy-yellow.
            label.bar2Color = isBarracksType(type) || type == EStructureType::Turret
                ? glm::vec3(0.4f, 0.95f, 0.5f) : glm::vec3(1.0f, 0.9f, 0.3f);
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
                len += snprintf(info + len, sizeof(info) - len,
                    isBarracksType(type) ? "\nBuild %.0f / %.0f" : "\nEnergy %.0f / %.0f",
                    m_structures.structureCharge(i), energyCap);
            if (fuelCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nFuel %.0f / %.0f",
                    m_structures.structureFuel(i), fuelCap);
            if (mineralCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nMinerals %.0f / %.0f",
                    m_structures.structureMinerals(i), mineralCap);
            if (isBarracksType(type) && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nPopulation %d / %d (%d houses)",
                    m_structures.structurePopulation(i), m_structures.structurePopCap(i),
                    m_structures.structureHouses(i));
            if (type == EStructureType::House && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structureLinkedId(i) != 0 ? "Linked to a barracks" : "No barracks in range");
            if (isCableOrCrossing(type) && len > 0 && len < (int)sizeof(info))
            {
                // The segment's transport readout: cells held, cells that left it last tick
                // against its out-rate, and its run's total (see StructureSystem::cableInfo).
                static constexpr const char* c_medium[3] = { "Energy", "Fuel", "Minerals" };
                StructureSystem::CableInfo ci;
                if (m_structures.cableInfo(i, ci))
                    len += snprintf(info + len, sizeof(info) - len, "\n%s %d / %d cells, %.1f / %.1f per s\nRun %d / %d cells over %d segments",
                        c_medium[glm::clamp(ci.medium, 0, 2)], ci.fill, ci.capacity, ci.movedPerSec, ci.ratePerSec,
                        ci.runFill, ci.runCapacity, ci.runSegments);
                else
                    len += snprintf(info + len, sizeof(info) - len, "\nNot conducting");
            }
            if (consumer && len > 0 && len < (int)sizeof(info))
                snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structurePowered(i) ? "Powered" : "No power");
            label.info = info;
            // The unit-type picker floats above an OWN barracks' label: one button per type, the
            // produced one highlighted. Clicks resolve in updateWindowed (popupButtonAtScreenPos).
            if (isBarracksType(type) && m_structures.structureTeam(i) == (uint8)m_team)
            {
                popup.active = true;
                popup.screenPos = label.screenPos;
                popup.title = "Produce";
                popup.buttons.reserve(oc::size(c_barracksMenu));
                const uint8 produced = m_structures.structureUnitType(i);
                for (const uint8 t : c_barracksMenu)
                {
                    HudPopupButton& b = popup.buttons.emplace_back();
                    b.label = c_unitTypeNames[t];
                    char sub[32];
                    snprintf(sub, sizeof(sub), "%d pop  %.0f E", m_structures.unitPopulation(t),
                        m_structures.unitSpawnEnergy(t));
                    b.sub = sub;
                    b.selected = t == (int)produced;
                }
            }
        }
        labels.push_back(oc::move(label));
    }
    // Units + players: own team green, enemy teams red. Only VISIBLE ones are fetched — an
    // off-screen one would just fail worldToScreen below. Works identically on server AND client:
    // remote instances' GameUnitComponents are populated by the snapshot game blob. Puppets are
    // player capsules; the own player is skipped (its HUD bars cover it).
    Entity* ownPlayer = m_player.entity();
    oc::vector<Entity*>& units = m_labelUnits; // the labels job's own scratch (one job at a time)
    // The query measures from the CAMERA; a unit within maxDist of the player is within
    // maxDist + |camera - player| of the camera, so that bound keeps the traversal tight and the
    // exact player-distance test below does the rest.
    NpcSystem::queryVisibleUnits(camera, m_labelMaxDistance + glm::distance(cullCenter, camera.position), units);
    for (Entity* unitEntity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(unitEntity);
        if (!u || unitEntity == ownPlayer || !inRange(unitEntity->pos))
            continue;
        // (Swarm bodies included: full bars are hidden, so only the DAMAGED slice of a thousand-
        // body horde pushes a label — the drown-the-HUD concern the old shieldOutput skip covered.)
        HudWorldLabel label;
        const float height = u->puppet ? 2.0f : 1.6f;
        if (!camera.worldToScreen(viewport, unitEntity->pos + glm::vec3(0.0f, height, 0.0f), label.screenPos)
            || !onScreen(label.screenPos)) // the frustum query is conservative (entry bounds)
            continue;
        label.title = u->getShortName(); // the prefab's `ShortName` tag (same on every instance — no wire type needed)
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
    Globals::gameHud.swapWorldLabels(labels);
    Globals::gameHud.swapPopup(popup);
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
    const auto meleeAround = [&](const glm::vec3& pos, uint8 team)
    {
        Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), m_meleeRadius, SpatialLayer_Render, [&](uint64 user)
        {
            Entity* other = reinterpret_cast<Entity*>(user);
            GameUnitComponent* u = getComponent<GameUnitComponent>(other);
            if (!u || u->puppet || u->team == team || !u->alive())
                return;
            // The query matches bounding spheres — the melee rule is the CENTER distance (XZ,
            // the same measure the units' own melee probes use).
            const glm::vec2 d = glm::vec2(other->pos.x, other->pos.z) - glm::vec2(pos.x, pos.z);
            if (glm::dot(d, d) <= m_meleeRadius * m_meleeRadius)
                u->damage(m_meleeDps * deltaSec);
        });
    };
    meleeAround(m_player.bodyPos(), (uint8)m_team);
    for (const auto& [id, p] : m_clientPlayers)
        if (p)
            if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(p.get()); pc && pc->body.isValid())
                meleeAround(pc->body.getPosition(), requestTeam(id));
}

void GameMatch::tickBaseHealing(float deltaSec)
{
    ProfileScope scope("Base healing", EProfileCategory::Game);
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

// AMBIENT WANDER (co-op authority): an IDLE AI unit now and then takes a short stroll, its heading
// biased toward the Base. Performance: the EXPECTED number of strolls this frame is roster /
// "Ambient wander interval" × dt (5000 units at 90 s and 60 fps = ~0.93 a frame), carried as a
// fractional budget so every frame issues that many on average — a steady trickle instead of a
// burst — and each stroll costs at most c_wanderScanCap random roster probes to find an idle
// unit. No per-unit timer. The order is a wander (orderWander): it never seeds a lane and
// self-expires after "Ambient wander timeout", so a target behind rock cannot pin the unit. Far
// (LOD-skipped) units walk it through the far tick's teleport like any order. Hunting, locked or
// routing units are skipped as candidates. Runs on a POST-UPDATE job (see update): its own RNG,
// since the shared C rand is not a thing to share with main.
void GameMatch::tickAmbientWander(float deltaSec)
{
    if (!m_coop || m_ambientWanderInterval <= 0.0f)
        return;
    const oc::span<const EntityPtr> units = m_npcs.units();
    const int n = (int)units.size();
    if (n == 0)
        return;
    const auto rand01 = [&] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_wanderRng); };
    // The budget is sized from the SELECTED roster, not the whole one: only selected units are
    // candidates, so a whole-roster budget would land every far unit's strolls on the few near a
    // player. The selected fraction is estimated from the random probes below (each is a fair
    // sample of the roster), smoothed — no roster walk.
    m_wanderBudget += (float)n * m_wanderSelectedFrac * deltaSec / m_ambientWanderInterval;
    int issue = (int)m_wanderBudget;
    if (issue <= 0)
        return;
    m_wanderBudget -= (float)issue;
    constexpr int c_wanderScanCap = 32; // random probes per stroll before giving up this frame
    glm::vec3 basePos(0.0f);
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) == 0)
        {
            basePos = m_structures.structurePos(i);
            break;
        }
    for (; issue > 0; --issue)
    {
        // Pre-emption point every 16 strolls (a stroll is up to 32 probes plus a ground clamp):
        // a Normal post-update job, so High work gets through. A stroll is issued whole.
        if ((issue & 15) == 0)
            Globals::jobSystem.preemptionPoint();
        GameUnitComponent* u = nullptr;
        Entity* e = nullptr;
        for (int scan = 0; scan < c_wanderScanCap && !u; ++scan)
        {
            e = units[glm::clamp((int)(rand01() * (float)n), 0, n - 1)].get();
            u = e ? getComponent<GameUnitComponent>(e) : nullptr;
            // Only SELECTED units (inside the SIM LOD's outer tier of some player) stroll: a far
            // unit is invisible and would walk its order by the far tick's teleport, then arrive
            // in view mid-stroll — a whole patch "starting to wander" the moment a player came
            // near. Tier 2 and closer all behave alike.
            const bool selected = e && Globals::world.simLodSelected(*e);
            m_wanderSelectedFrac += ((selected ? 1.0f : 0.0f) - m_wanderSelectedFrac) * 0.02f;
            if (u && (u->team != CoopAiTeam || u->puppet || !u->alive() || u->targetLocked || u->hasTarget
                || u->routeIndex < u->routeCount || !selected))
                u = nullptr;
        }
        if (!u)
            continue; // no idle unit found in the probe budget: this stroll is simply dropped
        const float angle = rand01() * 6.2831853f;
        glm::vec2 dir(glm::cos(angle), glm::sin(angle));
        const glm::vec2 toBase(basePos.x - e->pos.x, basePos.z - e->pos.z);
        if (const float len = glm::length(toBase); len > 1e-3f)
            dir += toBase / len * m_ambientWanderBaseBias; // the bias tilts the stroll toward the Base
        if (glm::dot(dir, dir) < 1e-4f)
            continue;
        dir = glm::normalize(dir);
        const float dist = (0.4f + 0.6f * rand01()) * m_ambientWanderDistance;
        const glm::vec3 target = clampToOpenGround(e->pos + glm::vec3(dir.x, 0.0f, dir.y) * dist);
        u->orderWander(target, m_ambientWanderTimeout);
    }
}

// MEDIC STATIONS, the PLAYER half: every BUILT + POWERED own-team medic within "Medic heal radius"
// heals the own capsule at "Medic heal/s" — HEALTH and the SHIELD BATTERY alike — on EVERY
// instance against the local mirror (health/energy are owner-computed; `powered` reaches clients
// through GSt). One heal per tick however many stations overlap. Units are the component's job.
void GameMatch::tickMedicHealing(float deltaSec)
{
    ProfileScope scope("Medic healing", EProfileCategory::Game);
    const float rate = m_structures.medicHealRate();
    const float radius = m_structures.medicHealRadius();
    if (rate <= 0.0f || radius <= 0.0f)
        return;
    const auto isActiveMedic = [&](int i) {
        return m_structures.structureType(i) == EStructureType::MedicStation
            && !m_structures.structureBlueprint(i) && m_structures.structurePowered(i); };
    const auto inReach = [&](int i, const glm::vec3& p) {
        const glm::vec3 m = m_structures.structurePos(i);
        return glm::distance(glm::vec2(p.x, p.z), glm::vec2(m.x, m.z)) <= radius; };
    if (m_player.entity())
    {
        const glm::vec3 pos = m_player.bodyPos();
        for (int i = 0; i < m_structures.structureCount(); ++i)
            if (isActiveMedic(i) && m_structures.structureTeam(i) == (uint8)m_team && inReach(i, pos))
            {
                m_player.heal(rate * deltaSec);
                m_player.charge(rate * deltaSec);
                break;
            }
    }
    // (UNITS are healed by the station's own component update in the parallel pass — one spatial
    // query per powered station, banked into the unit's heal inbox. Nothing to do here.)
}

void GameMatch::updateHud()
{
    GameHud& hud = Globals::gameHud;
    hud.setBar("Health", m_player.health(), m_player.healthMax(), glm::vec3(0.9f, 0.25f, 0.2f));
    hud.setBar("Energy", m_player.energy(), m_player.energyMax(), glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setBar("Materials", m_player.materials(), m_player.materialsMax(), glm::vec3(1.0f, 0.8f, 0.4f)); // carried inventory
    // (Pressure, density, shield radius, minerals and fuel are deliberately NOT counters: they
    // read on the structure labels and the tweaks, and the HUD column is kept to what a player
    // acts on each second.)
    hud.setBar("Grid energy", m_structures.gridEnergy(), glm::max(m_structures.gridEnergyCapacity(), 1.0f),
        glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy gen/s", m_structures.energyGenPerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy use/s", m_structures.energyUsePerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    if (m_coop && !m_isClient) // the wave clock is authority state (clients get the GWv log)
    {
        // The match clock as h:mm:ss (sim time since the world spawned; saved and restored).
        const int total = (int)m_matchTime;
        char clock[16];
        snprintf(clock, sizeof(clock), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        hud.setCounterText("Time", clock, glm::vec3(0.9f, 0.9f, 0.9f));
        hud.setCounter("Next wave (s)", glm::max(m_waveTimer, 0.0f), 0, glm::vec3(1.0f, 0.45f, 0.3f));
        hud.setCounter("Next wave power", nextWaveBudget(), 0, glm::vec3(1.0f, 0.45f, 0.3f)); // budget points before the alive cap
        // Live AI bodies against "Max enemy units" — an O(1) roster size, so no caching.
        hud.setCounter("AI alive", (float)aiAliveCount(), 0, glm::vec3(1.0f, 0.45f, 0.3f));
    }
}

// The NEXT wave's budget in points, before the "Max enemy units" cap: base + growth per wave so
// far, where the growth itself climbs by "Wave growth growth" every wave —
// base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)) for the 0-based wave index i.
float GameMatch::nextWaveBudget() const
{
    const float wave = (float)m_waveIndex;
    return (float)m_waveBudget + m_waveBudgetGrowth * wave + m_waveGrowthGrowth * wave * (wave - 1.0f) * 0.5f;
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
    if (const int button = Globals::gameHud.popupButtonAtScreenPos(m_mousePos); button >= 0)
    {
        // The barracks' unit-type picker (drawn last frame over the selected barracks) eats
        // clicks the same way the hotbar does.
        if (lmbEdge && m_selectedId != 0)
            requestSetUnitType(m_selectedId, c_barracksMenu[glm::min(button, (int)oc::size(c_barracksMenu) - 1)]);
        lmbEdge = false;
        rmbEdge = false;
        m_rmbMoveDrag = false;
    }
    else if (const int slot = Globals::gameHud.slotAtScreenPos(m_mousePos); slot >= 0)
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
    m_lmbReleased = false; // a release the active mode did not consume (box select runs in Select
                           // and in Build with nothing armed)

    // MOVE ORDER (RTS right-click): RMB ALWAYS moves the player. Cancelling rides along on the
    // same press — disarming a ghost, dropping a Lance/Wall anchor or a picked link endpoint, or
    // leaving a link tool all happen AND the capsule starts walking, because a cancel that also
    // ate the movement felt like a dropped input. Only the barracks ROUTE waypoint consumes the
    // press (m_rmbConsumed): it is a positive order, not a cancel, and pairing it with a move
    // would send the player off toward every rally point.
    // CTRL held on the press = UNITS ONLY: the selected units take the order and the player stays
    // put (latched for the whole hold, so a held re-aim keeps excluding the capsule).
    if (!m_rmbDown)
        m_rmbMoveDrag = false;
    if (rmbEdge)
        m_rmbUnitsOnly = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    if (rmbEdge && !m_rmbConsumed)
    {
        // Cables/crossings/solars are WALK-THROUGH — an RMB near one is a plain ground order,
        // never a walk-to-its-face building order.
        int hover = hoveredStructure(camera);
        if (hover >= 0 && isWalkThrough(m_structures.structureType(hover)))
            hover = -1;
        glm::vec3 clicked;
        if (hover >= 0 && aimGroundPoint(camera, clicked))
        {
            moveOrderAt(pointOutsideFootprint(clicked, hover), /*includePlayer*/ !m_rmbUnitsOnly);
            m_rmbMoveDrag = false; // a building order is one-shot: dragging off it must not re-aim
        }
        else if (hover < 0)
            m_rmbMoveDrag = true; // ground: holding keeps re-aiming at the cursor
    }
    glm::vec3 moveGround;
    if (m_rmbMoveDrag && aimGroundPoint(camera, moveGround))
    {
        const glm::vec3 dest = clampToOpenGround(glm::vec3(moveGround.x, 0.0f, moveGround.z));
        if (!m_rmbUnitsOnly)
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
    m_npcs.drawBeams(deltaSec); // turret lightning strikes
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
    // House links (own team): a line from each linked house to its barracks — bright when either
    // end is selected; a selected house also shows its link radius, a selected constructor its
    // build/repair reach.
    if (const int sel = m_structures.structureIndexById(m_selectedId);
        sel >= 0 && m_structures.structureType(sel) == EStructureType::Constructor)
        drawCircle(m_structures.structurePos(sel) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.4f, 0.0f),
            m_structures.constructorRange(), packColor(glm::vec3(0.9f, 0.7f, 0.3f)), 40);
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        if (m_structures.structureType(i) != EStructureType::House
            || m_structures.structureTeam(i) != (uint8)m_team)
            continue;
        const bool houseSelected = m_structures.structureId(i) == m_selectedId;
        const glm::vec3 housePos = m_structures.structurePos(i) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.4f, 0.0f);
        if (houseSelected)
            drawCircle(housePos, m_structures.houseLinkRadius(), packColor(glm::vec3(0.9f, 0.7f, 0.3f)), 48);
        const uint32 linked = m_structures.structureLinkedId(i);
        const int barracks = linked != 0 ? m_structures.structureIndexById(linked) : -1;
        if (barracks < 0)
            continue;
        const bool bright = houseSelected || linked == m_selectedId;
        Globals::rendererVK.addDebugLine(housePos,
            m_structures.structurePos(barracks) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.4f, 0.0f),
            packColor(glm::vec3(0.9f, 0.7f, 0.3f) * (bright ? 1.0f : 0.4f)));
    }

    routesScope.stop();

    // The world labels are built on a JOB kicked at the end of the game tick (submitWorldLabels):
    // this frame's final camera and viewport are captured here for it.
    m_labelsCamera = camera;
    m_labelsViewport = Globals::ui.getViewportRect();
    m_labelsCameraValid = true;
    {
        ProfileScope hudScope("Game HUD update", EProfileCategory::Game);
        updateHud();
    }
}
