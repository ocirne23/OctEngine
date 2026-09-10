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
import Nav;
import Spatial;
import Threading; // the ambient wander + nav feed ride the post-update batch
import :Match;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// The orchestrator's CORE: construction, the world spawn, the three frame entry points, the nav
// feed, the player ticks and the HUD. The rest of GameMatch is split by topic — see the file list
// at the end of Match.ixx.

GameMatch::GameMatch(bool enabled, bool coop) : m_coop(coop), m_enabled(enabled)
{
    if (!m_enabled)
        return;
    // Only the co-op AI carves seed lanes toward HUNTED targets; player-team units seed for routes
    // and move orders alone (see GameUnitParams::huntSeedTeam). PvP has no AI team: none.
    GameUnitComponent::params.huntSeedTeam = coop ? (int)CoopAiTeam : -1;
    GameUnitComponent::params.localTeam = m_team; // own-team units tint green (re-stamped when the team changes)

    // Top-down shadow preset. The cascades are nested spheres around the PLAYER (setSceneFocus, fed
    // by updateWindowed), so "Max distance" is metres from the player: 250 m reaches the zoomed-out
    // view with an even near/log split, the caster pad only needs the tallest structure, and the biases
    // go to zero because the ground plane is flat and the casters sit on it. Written into the live
    // "Shadows" tweaks (still editable in the panel); the sandbox values come back in ~GameMatch.
    m_sandboxShadowParams = Globals::rendererVK.shadowParams();
    {
        ShadowParams preset = m_sandboxShadowParams;
        preset.maxDistance = 250.0f;
        preset.splitLambda = 0.5f;
        preset.casterPad = 500.0f;
        preset.depthBias = 0.0f;
        preset.normalBias = 0.0f;
        Globals::rendererVK.setShadowParams(preset);
    }

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
        Tweak::floatVar("HUD", "Label max distance", &m_labelMaxDistance, 10.0f, 2000.0f, 10.0f, {}, ETweakFlags::None);
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
        Tweak::floatVar("Game/Nav", "Group cluster radius", &m_selectionClusterRadius, 2.0f, 60.0f, 0.5f);
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

    // The structure roster replaces every world-wide spatial query for structures: it deregisters
    // through this ONE notification, which every removal path funnels into (destroy requests,
    // editor deletes, network despawns). Units and projectiles have no roster — the World's root
    // list is walked instead (NpcSystem::queryAllUnits). Cleared in ~GameMatch — the world
    // outlives this object.
    Globals::world.setOnRootEntityRemoved([this](const Entity* entity)
    {
        m_structures.onWorldRootRemoved(entity);
    });
    // Route change -> the barracks' live units (the march index is KEPT and clamped: an appended
    // route continues where the unit was, a finished unit marches to the new tail).
    m_structures.onRouteLiveUnits = [](uint32 sourceId, oc::span<const glm::vec3> route)
    {
        for (const EntityPtr& e : Globals::world.rootEntities()) // every unit is a World root
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
    Globals::rendererVK.clearSceneFocus(); // back to camera-based cascades and RTAO falloff for the sandbox/menu
    // The ctor's top-down preset off again. The debug mode stays as it is NOW: it is a baked shader
    // define, and only its tweak callback reloads the pipeline — a silent write would desync the two.
    ShadowParams restored = m_sandboxShadowParams;
    restored.debugMode = Globals::rendererVK.shadowParams().debugMode;
    Globals::rendererVK.setShadowParams(restored);
    // Exit-to-menu can destroy a GameMatch MID-RUN: every tweak registered on a member (the ctor's
    // Game/* block + camera/player/structures/npcs) must leave the registry with it, or the
    // per-frame poll reads freed memory. Statics (component params) stay and re-register in place.
    TweakRegistry::get().unregisterInRange(this, sizeof(GameMatch));
    applyPause(false); // a shared pause must not outlive the match (exit-to-menu mid-pause)
    Globals::navSystem.clear(); // waits on in-flight builds before the world goes
    // The WHOLE World is wiped below (NpcSystem::clear): every holder drops its EntityPtrs FIRST,
    // so the World's batch release is the last reference to everything and the teardown fans out
    // over the job system. StructureSystem::clear also resets its tables (and joins the transport
    // job, which holds component pointers) — its per-entity removals just find the roots still
    // there.
    m_structures.clear();
    m_player.despawn();
    m_clientPlayers.clear();
    m_selectedUnits.clear();
    m_terrainRoot = {}; // co-op rocks + barrier segments (children)
    m_ground = {};      // corridor walls are its children
    Globals::world.setOnRootEntityRemoved(nullptr); // before the wipe: the callback captures this object
    m_npcs.clear();
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

// ---- the frame ---------------------------------------------------------------------------------

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
        issueScenarioOrder(); // after runScenario's load: units come from the World's root list, the Base/raster from the ticks below

    // The live unit count, ONE walk of the World's root list a frame: the HUD's "AI alive", the
    // wave cap (queueWave, below) and the wander budget (a post-update job) all read the cached
    // value instead of walking again.
    m_aliveUnits = NpcSystem::countUnits();

    // SIM LOD focus = every player: our capsule plus (server) each client's twin, so a unit is
    // never throttled near ANY player. Published before world.update reads it (main.cpp order).
    // PLUS FRIENDLY UNIT CLUSTERS: combat only runs inside the selection, so an army fighting far
    // from every player — and the enemies around it — would otherwise be dormant. Greedy clusters
    // over the non-AI units, refreshed every 0.25 s (the selection tolerates a frame of staleness
    // anyway): a unit farther than "Unit cluster focus radius" from every focus point seeds a new
    // one, up to the focus cap; the remaining slots go to the first clusters found in World root
    // order.
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
            for (const EntityPtr& e : Globals::world.rootEntities()) // every unit is a World root
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
    if (m_isServer) // strikes (turret lightning, melee hits) are pure visuals on clients: broadcast this frame's
        for (const NpcSystem::Beam& beam : m_npcs.newBeams())
        {
            uint8 buffer[32];
            NetWriter writer(buffer);
            writer.write<uint8>((uint8)beam.kind);
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

// ---- the nav feed ------------------------------------------------------------------------------

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
    // THE UNIT SWEEP IS SLICED: Nav consumes sources once per "Rebuild interval", so the World's
    // root list (every unit is a root; other roots are skipped) is walked over that many frames —
    // ceil(n * dt / interval) roots a frame, a constant slice — and the lists publish when the
    // cursor wraps. Culling (see below) tests each unit against the team cell hash the PREVIOUS
    // cycle built: one interval stale, a metre or two of motion against 64 m cells. Clients (no
    // unit sim) have an empty sweep and publish every frame. The root list only mutates on main,
    // after this job joins.
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
                if (m_navCellTeams.find((uint64)(uint32)(cx + dx) << 32 | (uint32)(cz + dz)) & ~own)
                    return true;
        return false; };
    const oc::vector<EntityPtr>& units = Globals::world.rootEntities();
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
            m_navCellTeamsNext.orTeams(cellKey(e->pos), uint8(1u << u->team));
            if (otherTeamNear(e->pos, (uint8)u->team))
                m_navUnitSources[u->team].push_back(Nav::NavSource{ e->pos, glm::max(u->bodyRadius, 0.25f), 0, 3 });
            // Pre-emption point every 256 roots (see JobSystem::preemptionPoint): a Normal job
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
            m_navCellTeamsNext.orTeams(cellKey(e->pos), uint8(1u << team)); };
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

// ---- the player ticks --------------------------------------------------------------------------

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
                u->damage(m_meleeDps * deltaSec, team);
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

// ---- the HUD and the windowed frame ------------------------------------------------------------

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

void GameMatch::updateGameInput(const Camera& camera)
{
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
}

void GameMatch::updateWindowed(Camera& camera, float deltaSec)
{
    if (!m_enabled)
        return;
    ProfileScope scope("Game windowed", EProfileCategory::Game);

    // DETACHED ("Game/Player/Detach camera"): main already ran the fly camera into `camera`; the
    // follow camera and every game input stand down (LMB/WASD are the fly controls now). The
    // listener-fed edges are still drained so no click lands the moment the tweak flips back.
    const bool detached = m_player.cameraDetached();
    if (!detached)
    {
        Input& input = Globals::input;
        // Camera yaw on the ARROW keys (Q/E belong to the grid hotkeys) + middle-drag.
        const float yawAxis = (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_RIGHT) ? 1.0f : 0.0f)
                            - (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LEFT) ? 1.0f : 0.0f);
        m_camera.apply(camera, m_player.interpolatedPos(), deltaSec, yawAxis, m_dragDeltaX, m_wheelAccum);
    }
    m_dragDeltaX = 0.0f;
    m_wheelAccum = 0.0f;
    // SCENE FOCUS = the player: sun cascades are nested spheres around it, picked by distance to it,
    // the RTAO fade measures from it and the GI clipmap centres on it — not on the follow camera hanging
    // in empty sky. With the camera detached the focus STAYS on the player (inspect its lighting from
    // anywhere) unless "Detach focus point" is on too, which hands it to the fly camera.
    if (detached && m_player.focusDetached())
        Globals::rendererVK.clearSceneFocus();
    else
    {
        // Player X/Z only, Y pinned at 1 m (the walkable plane is y = 0): a jump must not slide the
        // shadow cascades, the RTAO falloff and the GI clipmap window up and down with the capsule.
        const glm::vec3 playerPos = m_player.interpolatedPos();
        Globals::rendererVK.setSceneFocus(glm::vec3(playerPos.x, 1.0f, playerPos.z));
    }
    if (detached)
    {
        m_placeClicked = false;
        m_rmbClicked = false;
        m_lmbReleased = false;
        m_rmbMoveDrag = false;
        refreshBuildHotbar(); // the HUD stays live
    }
    else
        updateGameInput(camera);

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
