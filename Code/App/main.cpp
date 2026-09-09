import Core;
import Core.Allocator;
import Core.Log;
import Core.Window;
import Core.SDL;
import Core.Frustum;
import Core.Time;
import Core.glm;
import Core.Camera;
import Core.Tweaks;
import Core.Windows;

import App.InputControls;
import App.Lobby;
import App.Chat;
import App.ProfileDump;

import Game;

import Animation;
import File;
import Input;
import UI;
import RendererVK;
import Entity;
import Script;
import Physics;
import Audio;
import Spatial;
import Threading;
import Procedural;
import Nav;
import Nav;
import Particle;
import Force;
import Network; // netGetLocalAddress — the main menu shows the host endpoint other clients dial

static oc::atomic<bool> g_running = true; // cleared by the window's onQuit (windowed) or the console ctrl handler (headless)

static BOOL __stdcall consoleCtrlHandler(DWORD) { g_running = false; return TRUE; } // any console ctrl event = clean shutdown

// Command-line seconds -> Clock::duration for the one-shot Timers below (Timer asserts under 1 ms)
static Clock::duration timerDelay(double sec) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(oc::max(sec, 0.001))); }

int main(int argc, char* argv[])
{
    Globals::profiler.endStaticInit(); // closes the "Static init" scope opened at the profiler's static-init construction; must precede any main() scope
    ProfileScope initScope("main() initialize", EProfileCategory::App);
    FileSystem::initialize();
    // STARTUP loads the world off the disk (asset registry scan, scenes, shaders, cooked caches):
    // main-thread IO is expected here and only here. The scope ends with initScope, so any disk
    // access from the FRAME LOOP trips FileSystem's main-thread assert instead — which is the point.
    oc::optional<FileSystem::AllowMainThreadIO> startupIo;
    startupIo.emplace();
    // Core sits below File, so the tweak registry gets its file IO injected from here (startup +
    // the debounced save are explicit main-thread work).
    TweakRegistry::get().setFileIo(
        [](const oc::string& path) { return FileSystem::readFileStr(path, /*allowMainThread*/ true); },
        [](const oc::string& path, const oc::string& content)
        { return FileSystem::writeFileStr(path, content, /*allowMainThread*/ true); });
    TweakRegistry::get().loadSaved(); // Saved-flagged tweaks apply from Local/tweaks.cfg from here on

    // --server [--port N] [--headless] [--tickrate N] hosts an authoritative session; --connect
    // <ip[:port]> joins one; no flags = single player (networking fully inert). Same executable for
    // all of them; --headless runs the server without window/renderer/UI (server only).
    enum class ELaunchMode { Single, Server, Client };
    ELaunchMode launchMode = ELaunchMode::Single;
    uint16 netPort = 27888;
    int tickHz = 60;
    oc::string connectAddress;
    bool headless = false;
    bool gameMode = false;
    bool coopMode = false; // --coop (with --game): PvE — central Base, AI camps + attack waves
    // AUTOMATED PROFILING (Tools/profile.ps1 drives it): --profile-after S writes a text report of
    // the last --profile-frames frames S seconds into the run (to --profile-out, default
    // Local/profile.txt; F7 writes the same on demand), --quit-after S exits cleanly at S seconds,
    // --no-vsync removes the present throttle so CPU scopes run back to back (it is the "Time/VSync"
    // tweak override: pinned for the run, never written back). Times are seconds of
    // engine time since the loop started (frame counts would shift with the frame rate). Either
    // timed flag marks the run UNATTENDED: the window counts as focused (else the "Inactive max
    // FPS" cap would be what gets measured).
    double profileAfterSec = 0.0;
    double quitAfterSec = 0.0;
    oc::string profileOutPath = "Local/profile.txt";
    ProfileReportOptions profileOptions;
    bool scenario = false;
    oc::string scenarioSave; // "" = the F10 path
    double scenarioAtSec = 1.0;
    for (int i = 1; i < argc; ++i)
    {
        const oc::string_view arg = argv[i];
        if (arg == "--profile-after" && i + 1 < argc)     profileAfterSec = oc::max(std::atof(argv[++i]), 0.01);
        else if (arg == "--profile-frames" && i + 1 < argc) profileOptions.frames = (uint32)glm::clamp(std::atoi(argv[++i]), 1, (int)Profiler::FRAME_HISTORY - 1);
        else if (arg == "--profile-out" && i + 1 < argc)  profileOutPath = argv[++i];
        else if (arg == "--profile-workers")              profileOptions.perWorkerTrees = true; // one tree per worker instead of the merged one
        else if (arg == "--quit-after" && i + 1 < argc)   quitAfterSec = oc::max(std::atof(argv[++i]), 0.01);
        else if (arg == "--no-vsync")                     TweakRegistry::get().setOverride("Time/VSync=0");
        // --game scenario: load a save ("default" = F10's Local/gamesave.txt), select every own unit
        // and order them to the other Base at --scenario-at seconds (default 1, after the world settled)
        else if (arg == "--scenario" && i + 1 < argc)     { scenarioSave = argv[++i]; scenario = true; }
        else if (arg == "--scenario-at" && i + 1 < argc)  scenarioAtSec = oc::max(std::atof(argv[++i]), 0.0);
        // "Category/Name=v" (up to 4 values): wins over Local/tweaks.cfg, never written back
        else if (arg == "--tweak" && i + 1 < argc)        { if (!TweakRegistry::get().setOverride(argv[++i])) Log::warning("--tweak: cannot parse " + oc::string(argv[i])); }
        // a whole file of them (tweaks.cfg format, Assets/-relative; e.g. Scenarios/cpu-profile.tweaks)
        else if (arg == "--tweaks" && i + 1 < argc)
        {
            const oc::string path = argv[++i];
            if (!FileSystem::exists(path, /*allowMainThread*/ true))
                Log::warning("--tweaks: no such file " + path);
            else
                Log::info("--tweaks: " + oc::to_string(TweakRegistry::get().loadOverrides(FileSystem::readFileStr(path, /*allowMainThread*/ true), path)) + " overrides from " + path);
        }
        else if (arg == "--server")                       launchMode = ELaunchMode::Server;
        else if (arg == "--connect" && i + 1 < argc)      { launchMode = ELaunchMode::Client; connectAddress = argv[++i]; }
        else if (arg == "--port" && i + 1 < argc)         netPort = uint16(std::atoi(argv[++i]));
        else if (arg == "--tickrate" && i + 1 < argc)     tickHz = glm::clamp(std::atoi(argv[++i]), 10, 240);
        else if (arg == "--headless")                     headless = true;
        else if (arg == "--game")                         gameMode = true; // whitebox game instead of the testbed scene
        else if (arg == "--coop")                         coopMode = true; // PvE mode (clients pass it too — the layout is local)
        // both ends must agree, or the handshake denies with a clear reason
        else if (arg == "--no-encrypt")                   NetworkManager::setEncryption(false);
        else Log::warning("Unknown command line argument: " + oc::string(arg));
    }
    if (headless && launchMode != ELaunchMode::Server)
        Log::warning("--headless only applies to --server, ignoring");
    if (gameMode && headless && launchMode == ELaunchMode::Server)
    {
        Log::warning("--game needs a window (GPU field readbacks drive the authority sim), ignoring --game");
        gameMode = false; // multiplayer = a WINDOWED listen server (--game --server) + clients (--game --connect)
    }
    if (coopMode && !gameMode)
    {
        Log::warning("--coop needs --game, ignoring --coop");
        coopMode = false;
    }
    const bool headlessServer = headless && launchMode == ELaunchMode::Server;
    const bool unattendedRun = profileAfterSec > 0.0 || quitAfterSec > 0.0;
    // No mode chosen on the command line and not an automated run: boot into the MAIN MENU. The
    // engine runs fully underneath (renderer/UI/world, terrain as background); nothing
    // mode-specific starts until a selection arrives — see startNetworkFor/startWorldAndGame below.
    const bool mainMenu = !headlessServer && launchMode == ELaunchMode::Single && !gameMode && !unattendedRun && !scenario;

    Window window;
    FreeFlyCameraController cameraController;
    VRFreeFlyCameraController vrCameraController;

    if (!headlessServer)
    {
        window.initialize("Vulkan", glm::ivec2(5, 35), glm::ivec2(1920, 1080)); // spawns the WINDOW THREAD (owns SDL + the pump)
        Globals::input.setEventSource(&window);
        Globals::input.initialize();
    }
    Globals::jobSystem.initialize();
    if (!headlessServer)
    {
        // Between event pumps the window thread helps the job system - HIGH priority only, so it
        // can never sit in a multi-second Low job (V3 tile, nav build) when the next pump is due.
        window.runOnWindowThread([] { Globals::jobSystem.registerExternalHelper(); }, /*wait*/ true);
        window.setIdleWork([] { return Globals::jobSystem.tryRunOneHighJob(); },
                           [](bool (*wakeNow)(const void*), const void* user) { Globals::jobSystem.externalHelperWait(wakeNow, user); },
                           [] { Globals::jobSystem.wakeExternalHelper(); });
    }
    if (!headlessServer)
    {
        cameraController.initialize(glm::vec3(-1.5f, 14.0f, -7.1f), glm::vec3(0.0f, 4.0f, 0.0f));
        Globals::rendererVK.initialize(window, EValidation::ENABLED, EVr::DISABLED); // ENABLED DISABLED
        Globals::ui.initialize();
    }
    Globals::world.initialize();
    Globals::world.setHeadless(headlessServer); // BEFORE any spawn: gates template building
    Globals::physics.initialize();
    if (!headlessServer)
        Globals::audio.initialize();
    Globals::spatialIndex.initialize();    // headless: stays empty, but script spatial queries must be safe to run
    Globals::occlusionBuffer.initialize(); // static mesh colliders register occluders at spawn
    if (!headlessServer)
    {
        Globals::particleSystem.initialize(); // before any world spawn: ParticleComponents register effects on spawn
        Globals::forceSystem.initialize();
    }
    Globals::networkManager.initialize();

    Globals::scriptEvents.initialize();
    registerScriptDslBindings(); // must run before anything touches Globals::scriptBindings (ScriptEditor's build() included)

    EntityPtr terrainEntity;
    if (!headlessServer)
    {
        Globals::terrain.initialize();
        terrainEntity = Globals::world.createEmptyEntity("Terrain");
        Globals::terrainCollider.initialize((void*)terrainEntity);
        Globals::scatter.initialize();
        Globals::ocean.initialize();
        Globals::terrain.setFlowWindAngle(Globals::ocean.swellTravelAngle());
        Globals::physics.setWaterSurface([](float x, float z) { return Globals::ocean.sampleWaterHeight(x, z); },
            [] { return Globals::ocean.hasWater(); }); // no ocean = the whole buoyancy pass skips (it sweeps the broadphase per step)
        if (Globals::rendererVK.isVrEnabled())
        {
            Globals::vrInput.initialize(Globals::rendererVK.getVrSession());
            vrCameraController.initialize(glm::vec3(-1.0f, Globals::rendererVK.isVrStageSpace() ? 0.0f : 1.0f, 0.0f));
        }
    }

    SystemEventListenerHandle systemEventListener;
    if (headlessServer)
    {
        SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE);
    }
    else
    {
        systemEventListener = Globals::input.addSystemEventListener();
        systemEventListener->onQuit = []() { g_running = false; };
        systemEventListener->onWindowEvent = [&window](const SDL_WindowEvent& evt)
        {
            if (evt.type == SDL_EVENT_WINDOW_RESIZED)   Globals::rendererVK.recreateWindowSurface(window);
            if (evt.type == SDL_EVENT_WINDOW_MINIMIZED) Globals::rendererVK.setWindowMinimized(true);
            if (evt.type == SDL_EVENT_WINDOW_MAXIMIZED) Globals::rendererVK.setWindowMinimized(false);
            if (evt.type == SDL_EVENT_WINDOW_RESTORED)  Globals::rendererVK.setWindowMinimized(false);
        };
    }

    GizmoController gizmo;
    InputControls controls(gizmo, cameraController, Globals::world); // headless-inert: update/key handling never run
    controls.setProfileDump(profileOutPath, profileOptions); // F7 writes where --profile-out points
    // Stack local like GameMatch always was (holds EntityPtrs/Force handles, destructs before the
    // globals) — optional because it now constructs at GAME START: immediately below on the
    // command-line path, at countdown end on a lobby host, or inside the event dispatch on a
    // lobby client.
    oc::optional<GameMatch> game;
    // The multiplayer pre-game LOBBY (ready checks + start countdown, menu flow only). Plain
    // state, no entity handles; command-line game servers mark it "started" so menu clients
    // joining them still get the go signal.
    LobbySystem lobby;
    // The text CHAT log (lobby page + in-game overlay; the "ChM" event). Plain state like the lobby.
    ChatSystem chat;
    uint32 chatViewGeneration = 0; // the log generation the UI last received
    // Client: the server connection dropped (set inside receive() by the NetworkManager hook,
    // acted on at the top of the next frame — a menu-launched session returns to the menu).
    bool serverLost = false;
    // Game "Detach camera" tweak edge: the fly camera picks up the follow camera's pose on the
    // frame the tweak flips on, and hands WASD back to the game when it flips off.
    bool gameCameraDetached = false;

    // The GAME/WORLD half of a mode start: testbed content, or GameMatch + its world. Runs before
    // the loop on the command-line path, at countdown end on the lobby host, and INSIDE the event
    // dispatch on a lobby client (see the dispatcher below) — all on the main thread in the
    // pre-kick window, where spawns are legal.
    const auto startWorldAndGame = [&](bool startGame, bool startCoop)
    {
        FileSystem::AllowMainThreadIO modeIo; // one-shot prefab/scene loads on selection (the F10 pattern)
        if (!startGame)
        {
            Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/sponza.pre", Transform(), true));
            //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/skysphere.pre", Transform(spawnOffset), true));
            //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/character.pre", Transform(spawnOffset), true));
            //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/particle.pre", Transform(spawnOffset), true));
            //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/SphereField.pre", Transform(spawnOffset), true));
            if (Globals::networkManager.role() == ENetRole::Server)
                Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/Debug/networkTest.pre", Transform(glm::vec3(0, 0, 0)), true));
        }
        controls.setGameMode(startGame);                 // game: mutes the testbed spawn/possess keys
        cameraController.setMovementEnabled(!startGame); // game: WASD belongs to the game player
        Globals::ui.setGameLayout(startGame);            // game: viewport-only widget pass (debug side section via the escape menu)
        if (startGame)
        {
            game.emplace(true, startCoop);
            // A co-op HOST launching from the lobby generates the map the lobby page showed
            // (seed/fill/lanes); clients ignore this (they generate from the server's GMp event)
            // and the command-line path keeps the Game/Coop tweak values.
            uint32 mapSeed;
            float mapFill;
            int mapLanes;
            if (startCoop && lobby.hostMapSettings(mapSeed, mapFill, mapLanes))
                game->setMapSettings(mapSeed, mapFill, mapLanes);
            // A PvP HOST seats every lobby player on their picked team and spawns one Base per
            // team (the lobby's count); clients learn their team from the capsule's puppet
            // component as before, the command-line path keeps the two-team default.
            uint8 numTeams;
            oc::vector<oc::pair<uint32, uint8>> teamPicks;
            if (!startCoop && lobby.teamSettings(numTeams, teamPicks))
            {
                game->setLobbyTeams(numTeams, teamPicks);
                game->setPvpMap(lobby.pvpMap()); // the arena; clients build whatever GMp names
            }
            game->spawnWorld();
        }
    };

    // The NETWORK half: host/join plus the server-side join hooks. The game flavor wires the
    // LOBBY roster and the GameMatch lifecycles together — lobby FIRST, so a late joiner's
    // "game is running" signal precedes the world replay in the reliable stream; GameMatch is
    // null-guarded because it only exists once the match launched. The sandbox flavor keeps the
    // testbed's netPlayerCapsule hooks. Returns false when hosting/joining failed.
    const auto startNetworkFor = [&](ELaunchMode mode, bool startGame, const oc::string& address) -> bool
    {
        if (mode == ELaunchMode::Single)
            return true;
        const bool started = mode == ELaunchMode::Server
            ? Globals::networkManager.startServer(netPort)
            : Globals::networkManager.startClient(address, netPort);
        if (!started)
            return false;
        if (mode == ELaunchMode::Server)
        {
            if (startGame)
            {
                // Per-client lifecycles: the lobby keeps the roster + go signal, the game (once
                // it exists) spawns capsules and replays world state.
                Globals::networkManager.setOnClientJoined([&lobby, &game](uint32 clientId)
                {
                    lobby.onClientJoined(clientId);
                    if (game)
                        game->onClientJoined(clientId);
                });
                Globals::networkManager.setOnClientLeft([&lobby, &game](uint32 clientId)
                {
                    lobby.onClientLeft(clientId);
                    if (game)
                        game->onClientLeft(clientId);
                });
            }
            else
            {
                Globals::networkManager.setOnClientJoined([](uint32 clientId)
                {
                    EntityPtr player = Globals::world.spawnAssetFile("Entities/Debug/netPlayerCapsule.pre",
                        Transform(glm::vec3(0, 10.0f, 0)), true);
                    if (!player)
                        return;
                    player->setName("Player " + oc::to_string(clientId));
                    Globals::networkManager.setOwner(*player, clientId);
                    // ownership STEALING: whatever this player's body collides with becomes theirs (last
                    // collider wins, other players' primaries excluded) — needs ContactEvents on the shapes
                    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(player.get()))
                        pc->onContact = [clientId](Entity& other, bool begin)
                        {
                            if (begin)
                                Globals::networkManager.stealOwnershipOnContact(other, clientId);
                        };
                    Globals::world.addRootEntity(oc::move(player));
                });
                Globals::networkManager.setOnClientLeft([](uint32 clientId)
                {
                    oc::vector<Entity*> owned; // collected first: removeRootEntity mutates the list being walked
                    for (const EntityPtr& root : Globals::world.rootEntities())
                        if (const NetworkComponent* comp = getComponent<NetworkComponent>(root.get()); comp && comp->ownerClientId == clientId)
                            owned.push_back(root.get());
                    for (Entity* entity : owned)
                        Globals::world.removeRootEntity(entity);
                });
            }
        }
        return true;
    };

    // THE game-event dispatcher, installed ONCE and never replaced: "Lb*" routes to the lobby,
    // everything else to the GameMatch once it exists (GameMatch no longer installs its own hook —
    // replacing the oc::function from INSIDE a dispatch would destroy the executing lambda). A
    // lobby CLIENT constructs its GameMatch right here, inside the dispatch of the first lobby
    // state: that state precedes every game event in the reliable stream, so events later in the
    // SAME receive batch (a late join's world replay) already reach the fresh GameMatch —
    // deferring the construction by even one frame would drop them. Game events fired by script
    // thunks dispatch on workers (as before, straight into handleNetEvent); lobby events are only
    // ever fired and received on the main thread.
    Globals::networkManager.setOnGameEvent([&](oc::string_view name)
    {
        if (ChatSystem::handlesEvent(name))
        {
            chat.handleNetEvent(); // main thread only: fired from main, received in receive()
            return;
        }
        if (!LobbySystem::handlesEvent(name))
        {
            if (game)
                game->handleNetEvent(name);
            return;
        }
        lobby.handleNetEvent(name);
        if (lobby.takeClientConstruct() && !game)
            startWorldAndGame(true, lobby.coop());
        if (lobby.takeClientStart())
            Globals::ui.setMainMenuActive(false); // the server declared the match running
    });

    // A lost server: command-line clients keep the manager's auto-reconnect (server restarts
    // heal); a MENU-launched client (lobby or match) goes back to the menu with a status line
    // instead of sitting on a dead session — deferred to the loop top, since the hook runs inside
    // receive() where the host may not be shut down.
    Globals::networkManager.setOnServerLost([&]()
    {
        if (mainMenu)
            serverLost = true;
    });

    // ESCAPE-MENU "Exit to menu": tear the running mode (or the lobby) down to the blank
    // menu-phase engine — entity holders first, then the world's roots, then the network, the same
    // order the engine's init_seg teardown uses. Main thread, pre-kick window only. The other side
    // of a live session just sees this end as a disconnect (no goodbye message).
    const auto exitToMenu = [&]()
    {
        controls.resetForMenu();            // possessed capsule / test emitters / force balls released
        game.reset();                       // ~GameMatch: nav clear, rosters, structures, player, ground (+ its tweak unregistration)
        Globals::world.clearRootEntities(); // sponza / capsules / leftovers; NetworkComponents unregister through the still-open host
        Globals::networkManager.shutdown(); // role back to None — hosting/joining again is supported
        Globals::networkManager.setEventFilter({}); // a stale Gq*/Lb* filter must not gate the next session
        lobby.reset();
        chat.reset();
        Globals::ui.clearChat();
        controls.setGameMode(true);         // the menu phase mutes testbed keys + pauses free flight again
        cameraController.setMovementEnabled(false);
        gameCameraDetached = false;         // the next game re-seeds the fly camera from its own follow view
        Globals::ui.setGameLayout(false);   // the editor layout returns if the next pick is the sandbox
        Globals::ui.setMainMenuActive(true); // reactivation resets to the front page
    };

    // The menu's host display: seeded with the LAN address, upgraded to the EXTERNAL IP when the
    // background lookup lands (netGetExternalAddress blocks on DNS + an HTTP round trip, so it runs
    // on its own detached thread; the shared_ptr keeps the result block alive whichever side
    // finishes last, and the loop's menu block polls the flag).
    struct ExternalIpResult
    {
        NetAddress address;
        oc::atomic<bool> done{ false };
    };
    oc::shared_ptr<ExternalIpResult> externalIp;
    oc::string menuLanEndpoint;

    if (!mainMenu)
    {
        if (!startNetworkFor(launchMode, gameMode, connectAddress))
            return 1;
        if (gameMode && launchMode == ELaunchMode::Server)
            lobby.markStarted(coopMode); // no lobby ran: menu clients joining late still get the go signal
        startWorldAndGame(gameMode, coopMode);
    }
    else
    {
        // Menu phase: testbed spawn keys muted (setGameMode), free flight paused; both are restored
        // by startWorldAndGame per the selection. The menu shows the endpoint other clients would dial.
        controls.setGameMode(true);
        cameraController.setMovementEnabled(false);
        NetAddress hostEndpoint = netGetLocalAddress();
        hostEndpoint.port = netPort;
        menuLanEndpoint = hostEndpoint.toString();
        Globals::ui.setMainMenuHostEndpoint(menuLanEndpoint);
        Globals::ui.setMainMenuHostNote("Resolving external IP...");
        Globals::ui.setMainMenuActive(true);
        externalIp = oc::make_shared<ExternalIpResult>();
        std::thread([result = externalIp]
        {
            result->address = netGetExternalAddress();
            result->done.store(true, oc::memory_order_release);
        }).detach(); // touches only its own shared block; killed harmlessly at process exit if late
    }

    Camera camera;
    camera.viewMatrix = glm::mat4(1.0f);
    camera.position = glm::vec3(0.0f);
    if (!headlessServer)
    {
        gizmo.initialize(Globals::world);
        Globals::ui.setGizmo(&gizmo);
        Globals::physics.setDebugDrawCallback([](const glm::vec3& a, const glm::vec3& b, uint32 color) { Globals::rendererVK.addDebugLine(a, b, color); }, [&camera]() { return camera.position; });
        Globals::world.setOnPrefabOpened([](const EntityPtr& entity, const oc::string& path) { Globals::ui.onOpened(entity, path); });
        Globals::world.setOnEntityRespawned([](const EntityPtr& oldEntity, const EntityPtr& newEntity) { Globals::ui.onEntityRespawned(oldEntity, newEntity); });
    }

    uint32 frameCount = 0;
    uint32 fps = 0;
    Timer fpsTimer(std::chrono::seconds(1), [&](Timer& timer) {
            fps = frameCount;
            frameCount = 0;
            return Timer::REPEAT;
        });

    Timer titleUpdateTimer(std::chrono::milliseconds(100), [&](Timer& timer) {
            if (headlessServer)
                return Timer::REPEAT; // no window; the status Timer below logs instead
            glm::vec3 pos = cameraController.getPosition();
            glm::vec3 dir = cameraController.getDirection();
            const oc::string netStatus = Globals::networkManager.getStatusText(); // empty in single player
            char windowTitleBuf[320];
            sprintf_s(windowTitleBuf, sizeof(windowTitleBuf), "%s%sFPS: %i mem: %.2fmb instances: %i meshtypes: %i materials: %i, pos: %.1f, %.1f, %.1f, dir: %.1f, %.1f, %.1f",
                netStatus.c_str(), netStatus.empty() ? "" : " | ",
                fps, (double)(Globals::allocator.getUsedSize() + getAlignedAllocatedSize()) / 1024.0 / 1024.0,
                Globals::rendererVK.getNumMeshInstances(), Globals::rendererVK.getNumMeshTypes(), Globals::rendererVK.getNumMaterials(), pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
            window.setTitle(windowTitleBuf);
            return Timer::REPEAT;
        });

    Timer renderStatsUpdateTimer(std::chrono::seconds(1), [&](Timer& timer) {
            if (!headlessServer)
                Globals::ui.setRenderStats(Globals::rendererVK.getStats());
            return Timer::REPEAT;
        });

    Timer headlessStatusTimer(std::chrono::seconds(5), [&](Timer& timer) {
            if (headlessServer)
                Log::info("Headless: " + oc::to_string(fps) + " ticks/s | " + Globals::networkManager.getStatusText());
            return Timer::REPEAT;
        });

    initScope.stop();
    startupIo.reset(); // from here on, main-thread IO asserts (see FileSystem)

    Globals::time.registerTweaks(); // frame pacing + event-pump lead (see Time::beginFrame)

    // The clock stood at static-init time all through init (nothing calls update() before the loop),
    // so re-base it here: the one-shot Timers below and the first frame's delta must measure the RUN,
    // not the init phase (a long init used to fire every timed trigger on frame 1).
    Globals::time.update();

    // Unattended-run triggers. They are Timers, so they fire from Time::update() at the frame
    // boundary - after the previous frame's mark and before the "main loop" scope opens, so a report
    // still covers completed frames only and its IO lands outside the frame window. Each is armed
    // only when its flag was given and returns DONE, i.e. fires exactly once.
    oc::optional<Timer> scenarioTimer;
    if (scenario)
        scenarioTimer.emplace(timerDelay(scenarioAtSec), [&](Timer&) {
                if (!game || !game->enabled())
                    Log::warning("--scenario needs --game");
                else
                {
                    FileSystem::AllowMainThreadIO scenarioIo; // one-shot load, like F10
                    game->runScenario(scenarioSave == "default" ? oc::string_view() : oc::string_view(scenarioSave));
                }
                return Timer::DONE;
            });

    oc::optional<Timer> profileDumpTimer;
    if (profileAfterSec > 0.0)
        profileDumpTimer.emplace(timerDelay(profileAfterSec), [&](Timer&) {
                writeProfileReport(profileOutPath, profileOptions);
                return Timer::DONE;
            });

    oc::optional<Timer> quitTimer;
    if (quitAfterSec > 0.0)
        quitTimer.emplace(timerDelay(quitAfterSec), [](Timer&) {
                g_running = false; // the loop condition is checked next, so this frame still completes
                return Timer::DONE;
            });

    bool uiJobKicked = false;  // no ImGui::Render / draw data until the first widget pass ran

    while (g_running)
    {
        // The vsync/GPU throttle: block on this frame slot's fence FIRST, before the loop scope opens
        // and before input is sampled — so the stall shows as its own top-level "Fence wait" (the
        // "main loop" scope measures only real work) and input -> sim -> present stays tight instead
        // of the sampled input going stale during the wait.
        if (!headlessServer)
        {
            // The whole frame boundary: fence wait, frame-rate limit, event-pump kick, next frame's
            // clock (Time::beginFrame - see its comment for the pacing and pump-timing rules).
            Globals::time.beginFrame(Globals::input.isWindowHasFocus() || unattendedRun, Globals::rendererVK.isVrEnabled(),
                Globals::rendererVK.isVSyncEnabled(), window.getDisplayRefreshHz(), &window,
                [](uint64 timeoutNs) { return Globals::rendererVK.waitFrameSlot(timeoutNs); });
        }
        else
            Globals::time.update();

        ProfileScope mainLoopScope("main loop", EProfileCategory::App);

        // The previous frame's post-update batch, the UI widget pass among it, kicked just before
        // its present: it ran during present, the frame mark and the fence/vsync wait above, so
        // this is near-zero unless the batch outlasted the whole stall. Joined before anything that
        // mutates state the panels read and before the input pump touches the ImGui context.
        // Windowed and headless alike.
        Globals::jobSystem.joinPostUpdateJobs();

        if (!headlessServer)
        {
            if (uiJobKicked)
            {
                Globals::ui.flushMainThreadWork();       // deferred tweak onChange callbacks + container imports (the job produced its own draw-data snapshot)
                Globals::rendererVK.updateImGuiTextures(); // glyphs the pass baked; the ImGui context is quiescent from the join until ui.update()
            }
            Globals::forceSystem.joinMerge(); // last frame's merge job (kicked after the force upload, ran during present + the stall); before input/drains can touch emitters

            if (serverLost) // client: the host left (or the link died) — back to the menu, pre-kick window
            {
                serverLost = false;
                if (Globals::networkManager.role() == ENetRole::Client)
                {
                    exitToMenu();
                    Globals::ui.setMainMenuStatus("Disconnected from the server");
                }
            }

            // Main menu selection (written by the PREVIOUS frame's widget pass, sequenced by the
            // join above): start the chosen mode here in the pre-kick window, where main-thread
            // spawns and the network start are legal. A failed host/join leaves the menu up.
            if (Globals::ui.isMainMenuActive())
            {
                // The external-IP lookup landed: upgrade the host display (the LAN address moves
                // into the note line), or report the failure and keep the LAN address.
                if (externalIp && externalIp->done.load(oc::memory_order_acquire))
                {
                    NetAddress external = externalIp->address;
                    externalIp.reset();
                    if (external.ip != 0)
                    {
                        external.port = netPort;
                        Globals::ui.setMainMenuHostEndpoint(external.toString());
                        Globals::ui.setMainMenuHostNote("LAN: " + menuLanEndpoint);
                        Log::info("External IP: " + external.toString());
                    }
                    else
                        Globals::ui.setMainMenuHostNote("External IP lookup failed - this is the LAN address");
                }

                const MainMenuAction action = Globals::ui.takeMainMenuAction();
                if (action.type == MainMenuAction::EType::Quit)
                    g_running = false;
                else if (action.type != MainMenuAction::EType::None)
                {
                    const ELaunchMode mode = action.host ? ELaunchMode::Server
                        : !action.connectAddress.empty() ? ELaunchMode::Client : ELaunchMode::Single;
                    const bool startGame = action.type != MainMenuAction::EType::StartSandbox;
                    const bool startCoop = action.type == MainMenuAction::EType::StartCoop;
                    if (startNetworkFor(mode, startGame, action.connectAddress))
                    {
                        if (startGame && mode != ELaunchMode::Single)
                        {
                            // Multiplayer game: gather in the LOBBY first (ready checks + start
                            // countdown). The world/GameMatch start when the countdown finishes
                            // (host) or when the server's go signal arrives (client).
                            lobby.enter(mode == ELaunchMode::Server, startCoop);
                            Globals::ui.openMainMenuLobby();
                        }
                        else
                        {
                            startWorldAndGame(startGame, startCoop);
                            Globals::ui.setMainMenuActive(false);
                        }
                    }
                    // network failure: the menu stays up, the log has the reason
                }

                // Lobby servicing: the page's button presses, the countdown tick (+ the host's
                // state broadcasts), then this frame's view snapshot for the widget pass.
                if (lobby.active())
                {
                    const LobbyAction lobbyAction = Globals::ui.takeMainMenuLobbyAction();
                    if (lobbyAction.type == LobbyAction::EType::Leave)
                        exitToMenu(); // host: the server closes and every client sees a disconnect; client: just leaves
                    else
                    {
                        lobby.handleAction(lobbyAction);
                        lobby.update((float)Globals::time.getDeltaSec());
                        Globals::ui.setMainMenuLobbyView(lobby.view());
                    }
                }
                if (lobby.takeServerStart())
                {
                    startWorldAndGame(true, lobby.coop());
                    if (game)
                        for (const uint32 clientId : lobby.connectedClientIds())
                            game->onClientJoined(clientId); // capsules + world replay for everyone already in the lobby
                    Globals::ui.setMainMenuActive(false);
                }
            }

            // Chat servicing (lobby page + game overlay draw the same widget): the line the
            // previous widget pass sent, then a fresh log snapshot only when the log changed.
            if (const oc::string line = Globals::ui.takeChatOutgoing(); !line.empty())
                chat.send(line);
            if (chat.generation() != chatViewGeneration)
            {
                chatViewGeneration = chat.generation();
                Globals::ui.setChatView(chat.view());
            }
        }

        const double deltaSec = Globals::time.getDeltaSec(); // clock advanced by limitFrameRate / update above
        TweakRegistry::get().update((float)deltaSec); // Saved/Synced change detection

        if (!headlessServer)
        {
            Globals::input.update(deltaSec);
            Globals::ui.prepare(); // panel data jobs (profiler snapshot etc.) — overlap everything below until ui.update
            controls.update((float)deltaSec);

            // ESCAPE MENU (Esc — every running mode plus the LOBBY page, never over the main
            // menu's front/settings pages). In game mode the game's own Esc cancel chain (build
            // steps, hotbar pages) keeps first claim: the overlay only opens once Esc has nothing
            // left to cancel. The teardown below runs in the pre-kick window, where main-thread
            // entity destruction and the network shutdown are legal.
            {
                const bool escapeAllowed = !Globals::ui.isMainMenuActive() || Globals::ui.isMainMenuLobbyOpen();
                if (controls.takeEscapePressed() && escapeAllowed)
                {
                    if (Globals::ui.isEscapeMenuOpen())
                        Globals::ui.setEscapeMenuOpen(false);
                    else if (!game || !game->enabled() || Globals::ui.isMainMenuActive() || !game->escWouldCancel())
                        Globals::ui.setEscapeMenuOpen(true);
                }
                // The SHARED GAME PAUSE: the escape menu offers "Pause game" over a running game;
                // the paused box (any player's Resume) mirrors the game's synced state.
                const bool gameRunning = game && game->enabled() && !Globals::ui.isMainMenuActive();
                Globals::ui.setEscapeOffersPause(gameRunning);
                Globals::ui.setGamePaused(gameRunning && game->isPaused());
                switch (Globals::ui.takeEscapeMenuAction())
                {
                case EscapeMenuAction::Resume:
                    Globals::ui.setEscapeMenuOpen(false);
                    break;
                case EscapeMenuAction::Pause:
                    Globals::ui.setEscapeMenuOpen(false);
                    if (gameRunning)
                        game->requestPause(true);
                    break;
                case EscapeMenuAction::Unpause:
                    if (gameRunning)
                        game->requestPause(false);
                    break;
                case EscapeMenuAction::Quit:
                    g_running = false;
                    break;
                case EscapeMenuAction::ExitToMenu:
                    Globals::ui.setEscapeMenuOpen(false);
                    exitToMenu();
                    break;
                default:
                    break;
                }
            }

            // The SIM post-update batch (Nav field steps, the game's nav feed + ambient wander) ran
            // through input, prepare and the camera; it must be done before the first main-thread
            // writes to units/rosters — the game's windowed tick (unit orders) and the entity-change
            // drains below. Normally already finished: a no-op.
            Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim);
            if (game && game->enabled() && !Globals::ui.isMainMenuActive() && !Globals::ui.isEscapeMenuOpen())
            {
                // Menu/lobby/escape-menu active = no game input, camera overwrite or HUD (a lobby
                // client's GameMatch already exists and simulates, but the overlay owns the screen).
                // "Game/Player/Detach camera": the testbed fly camera takes the frame instead of
                // the follow camera, seeded from the follow view on the flip so nothing pops.
                const bool detached = game->cameraDetached();
                if (detached != gameCameraDetached)
                {
                    if (detached)
                    {
                        const glm::vec3 forward = -glm::vec3(camera.viewMatrix[0][2], camera.viewMatrix[1][2], camera.viewMatrix[2][2]);
                        cameraController.setPose(camera.position, camera.position + forward);
                    }
                    cameraController.setMovementEnabled(detached);
                    gameCameraDetached = detached;
                }
                if (detached)
                {
                    cameraController.update(deltaSec);
                    camera = cameraController.getCamera();
                }
                game->updateWindowed(camera, (float)deltaSec); // game mode: follow-camera overwrite + aim/HUD/debug draw (neither while detached)
            }
            else if (game && game->enabled() && !Globals::ui.isMainMenuActive())
            {
                // Escape menu over a running game: the camera stays where the game left it — the
                // fly-camera branch below would overwrite it with the testbed camera's own pose
                // (the view dropped to that pose every time the overlay opened).
            }
            else if (Globals::rendererVK.isVrEnabled())
            {
                vrCameraController.update(deltaSec); // thumbstick locomotion; pulls Globals::vrInput
                camera = vrCameraController.getCamera();
            }
            else
            {
                cameraController.update(deltaSec);
                camera = cameraController.getCamera();
                controls.applyPlayerCamera(camera); // possessed capsule view (first/third person), see InputControls
            }
            Globals::scriptHost.handleScriptReloadRequests(Globals::ui.takeScriptReloadRequests());
            Globals::world.handleEntityChanges(Globals::ui.takeEntityChanges(), camera, Globals::ui.getViewportRect());
        }
        Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim); // headless has no windowed block: its first roster mutation is this drain (a no-op when already joined above)
        Globals::world.handleEntityChanges(Globals::scriptEvents.takeEntityChanges(), camera, Globals::ui.getViewportRect());

        // PAUSE ("Time/Paused" tweak or the Pause/Break key): every simulation consumer below takes
        // the SIM delta (0 while paused) — camera/input/UI/tweaks above and the network transport
        // (keepalives, RTT, snapshot cadence) stay on the real delta. The entity pass and script
        // events additionally gate on Time::isPaused() like Frozen (see Time::setPaused). Read here,
        // after the input dispatch and tweak poll, so a toggle applies to this very frame.
        const double simDeltaSec = Globals::time.getSimDeltaSec();
        Globals::jobSystem.setFrameHasPhysicsStep(Globals::physics.willStep(simDeltaSec)); // before any kick: optional jobs keep off the step's worker load

        Globals::networkManager.receive(deltaSec); // snapshot targets + events land before the sim/entity updates read them
        if (game)
            game->updatePlayer((float)simDeltaSec); // ONLY the player-body writes (camera hot path, pre-physics); the rest of the game tick runs after the joins below
        Globals::scriptContext.update(camera, (float)simDeltaSec, (float)Globals::time.getSimElapsedSec());

        // The spatial index + renderer frame state are QUIESCENT from here until world.update: the
        // drains / net receive / game.update above were the last registers and container loads, and
        // physics.update no longer fires contact scripts (deferred below) — so the "Spatial cull" +
        // "Begin frame" jobs kick BEFORE physics and overlap the step, audio and the nav publish.
        // Nothing between the kicks and the joins may touch the index or renderer frame state.
        Globals::world.joinSelection(); // last frame's SIM LOD selection query must be done before the commit inside the spatial kick (headless: commitFrame below)
        if (!headlessServer)
        {
            ProfileScope kickScope("Frame kicks", EProfileCategory::App); // attributes the submit + wake cost that used to read as a gap
            const Rect viewportRect = Globals::ui.getViewportRect(); // stable: the widget pass joined at the top of the frame
            CullView cullView;
            {
                ProfileScope scope("Cull view", EProfileCategory::Renderer);
                cullView = Globals::rendererVK.getCullView(camera, viewportRect);
            }
            {
                ProfileScope scope("Spatial kick", EProfileCategory::Spatial);
                Globals::spatialIndex.kickUpdateJob(cullView);
            }
            {
                ProfileScope scope("Begin frame kick", EProfileCategory::Renderer);
                Globals::rendererVK.kickBeginFrameJob(camera, viewportRect);
            }
        }
        Globals::physics.update(simDeltaSec); // ≤1 step; contact events stay buffered until the dispatch below
        if (!headlessServer)
        {
            Globals::audio.update(camera);
            Globals::navSystem.update((float)simDeltaSec); // consumes the feed last frame's post-update job set (joined at the frame top)
            Globals::rendererVK.joinBeginFrameJob(); // VR: beginFrame runs synchronously here
            Globals::spatialIndex.joinUpdateJob();
        }
        else
            Globals::spatialIndex.commitFrame(); // no cull job headless, but every entity registers: link the entries so queries (script radius, unit targeting) see them
        // The game tick's bulk (structures/production/materials/nav staging — everything but the
        // player-body writes in game.updatePlayer above): spawns, destroys and spatial queries are
        // legal again after the joins, and mid-frame container loads after beginFrame are a
        // supported path (present() re-checks texture/mesh generations). Becomes the server tick
        // in MP. See GameMatch::update's declaration for the one-frame latencies this placement buys.
        if (game)
            game->update((float)simDeltaSec); // also publishes the SIM LOD focus (the players)
        else if (!headlessServer)
            Globals::world.setSimLodFocus(&camera.position, 1); // testbed: the camera is the focus
        // Contact scripts (OnPhysicsEvent) query the spatial index and can touch renderer state
        // (light/sun thunks), so they fire AFTER the joins — still before the entity pass, as before.
        Globals::physics.dispatchContactEvents([](const PhysicsWorld::ContactEvent& evt) { Globals::world.handleContactEvent(evt); });

        Globals::world.update(Globals::rendererVK, (float)simDeltaSec); // serial script prepass + parallel component/tree pass + sink flush; headless: renderer passed through but never dereferenced (headless archetypes)
        Globals::networkManager.send(deltaSec); // server: snapshot entities at their post-update poses; both roles: flush queued packets

        if (!headlessServer)
        {
            Globals::terrain.update(Globals::rendererVK, camera);
            Globals::terrainCollider.update(camera.position, Globals::terrain.activeClimateMaps());
            Globals::ocean.update(Globals::rendererVK, camera, Globals::terrain.activeTerrainData(), Globals::terrain.seaLevel());
            Globals::scatter.update(Globals::rendererVK, camera, Globals::terrain.activeClimateMaps());
            Globals::particleSystem.update(Globals::rendererVK, (float)simDeltaSec);
            Globals::forceSystem.update(Globals::rendererVK, (float)simDeltaSec);

            Globals::ui.drawGizmoEntity(Globals::rendererVK, (float)deltaSec);
            if (game)
                game->joinWorldLabels(); // the labels job (kicked at the end of the game tick) feeds the widget pass's HUD overlay
            Globals::ui.update(Globals::world.rootEntities(), camera, deltaSec); // ImGui backend new frame (main thread) + queues the widget pass
            uiJobKicked = true;

            Globals::jobSystem.kickPostUpdateJobs(); // the widget pass + any other post-update work: runs THROUGH present, joined at the top of the next frame
            Globals::rendererVK.present();           // ImGui pass records from the PREVIOUS widget pass's snapshot
        }
        else
            Globals::jobSystem.kickPostUpdateJobs(); // no present() here, but the queue must still kick once a tick or it fills up

        mainLoopScope.stop(); // before the frame mark, so the record stays inside this frame's window
        Globals::profiler.endFrame();
        frameCount++;

        if (headlessServer)
            while (g_running && Clock::now() < Globals::time.getCurrentTime() + Clock::duration(std::chrono::seconds(1)) / tickHz)
                Sleep(1);
    }

    Globals::jobSystem.joinPostUpdateJobs(); // the final frame's post-update batches (the widget pass among them) may still be in flight
    Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim);
    Globals::world.joinSelection();          // and its fire-and-forget selection query (touches the World, which destructs before the JobSystem)
    Globals::forceSystem.joinMerge();        // and the final frame's merge job

    return 0;
}
