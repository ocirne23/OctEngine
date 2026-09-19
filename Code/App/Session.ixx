export module App.Session;

import Core;
import Core.Log;
import Core.glm;
import Core.Camera;
import Core.Time;
import Core.Tweaks;

import App.InputControls;

import Game;
import File;
import Input;
import UI;
import RendererVK;
import Entity;
import Physics;
import Network;
import Procedural;

export oc::atomic<bool> g_running = true;

export enum class ELaunchMode { Single, Server, Client };

export struct LaunchOptions
{
    ELaunchMode launchMode = ELaunchMode::Single;
    uint16 netPort = 27888;
    int tickHz = 60;
    oc::string connectAddress;
    bool headless = false;
    bool gameMode = false;
    bool coopMode = false;

    double profileAfterSec = 0.0;
    double quitAfterSec = 0.0;
    oc::string profileOutPath = "Local/profile.txt";
    ProfileReportOptions profileOptions;
    bool scenario = false;
    oc::string scenarioSave;
    double scenarioAtSec = 1.0;

    bool headlessServer() const { return headless && launchMode == ELaunchMode::Server; }
    bool unattendedRun() const { return profileAfterSec > 0.0 || quitAfterSec > 0.0; }
    bool mainMenu() const { return !headlessServer() && launchMode == ELaunchMode::Single && !gameMode && !unattendedRun() && !scenario; }
};

export LaunchOptions parseCommandLine(int argc, char* argv[])
{
    LaunchOptions o;
    for (int i = 1; i < argc; ++i)
    {
        const oc::string_view arg = argv[i];
        if (arg == "--profile-after" && i + 1 < argc)     o.profileAfterSec = oc::max(std::atof(argv[++i]), 0.01);
        else if (arg == "--profile-frames" && i + 1 < argc) o.profileOptions.frames = (uint32)glm::clamp(std::atoi(argv[++i]), 1, (int)Profiler::FRAME_HISTORY - 1);
        else if (arg == "--profile-out" && i + 1 < argc)  o.profileOutPath = argv[++i];
        else if (arg == "--profile-workers")              o.profileOptions.perWorkerTrees = true;
        else if (arg == "--quit-after" && i + 1 < argc)   o.quitAfterSec = oc::max(std::atof(argv[++i]), 0.01);
        else if (arg == "--no-vsync")                     TweakRegistry::get().setOverride("Time/VSync=0");
        else if (arg == "--scenario" && i + 1 < argc)     { o.scenarioSave = argv[++i]; o.scenario = true; }
        else if (arg == "--scenario-at" && i + 1 < argc)  o.scenarioAtSec = oc::max(std::atof(argv[++i]), 0.0);
        else if (arg == "--tweak" && i + 1 < argc)        { if (!TweakRegistry::get().setOverride(argv[++i])) Log::warning("--tweak: cannot parse " + oc::string(argv[i])); }
        else if (arg == "--tweaks" && i + 1 < argc)
        {
            const oc::string path = argv[++i];
            if (!FileSystem::exists(path, true))
                Log::warning("--tweaks: no such file " + path);
            else
                Log::info("--tweaks: " + oc::to_string(TweakRegistry::get().loadOverrides(FileSystem::readFileStr(path, true), path)) + " overrides from " + path);
        }
        else if (arg == "--server")                       o.launchMode = ELaunchMode::Server;
        else if (arg == "--connect" && i + 1 < argc)      { o.launchMode = ELaunchMode::Client; o.connectAddress = argv[++i]; }
        else if (arg == "--port" && i + 1 < argc)         o.netPort = uint16(std::atoi(argv[++i]));
        else if (arg == "--tickrate" && i + 1 < argc)     o.tickHz = glm::clamp(std::atoi(argv[++i]), 10, 240);
        else if (arg == "--headless")                     o.headless = true;
        else if (arg == "--game")                         o.gameMode = true;
        else if (arg == "--coop")                         o.coopMode = true;
        else if (arg == "--no-encrypt")                   NetworkManager::setEncryption(false);
        else Log::warning("Unknown command line argument: " + oc::string(arg));
    }
    if (o.headless && o.launchMode != ELaunchMode::Server)
        Log::warning("--headless only applies to --server, ignoring");
    if (o.gameMode && o.headless && o.launchMode == ELaunchMode::Server)
    {
        Log::warning("--game needs a window (GPU field readbacks drive the authority sim), ignoring --game");
        o.gameMode = false;
    }
    if (o.coopMode && !o.gameMode)
    {
        Log::warning("--coop needs --game, ignoring --coop");
        o.coopMode = false;
    }
    return o;
}

export void installFileHooks()
{
    ProfileScope scope("installFileHooks", EProfileCategory::App);
    TweakRegistry::get().setFileIo(
        [](const oc::string& path) { return FileSystem::readFileStr(path, true); },
        [](const oc::string& path, const oc::string& content) { return FileSystem::writeFileStr(path, content, true); });
    TweakRegistry::get().loadSaved();

    Globals::profiler.setReportWriter([](const oc::string& path, const oc::string& report)
    {
        const oc::string dir = FileSystem::parentPath(path);
        if (!dir.empty())
            FileSystem::createDirectories(dir, true);
        if (!FileSystem::writeFileStr(path, report, true))
        {
            Log::error("Profile report: could not write " + path);
            return false;
        }
        Log::info("Profile report written to " + path + " (" + oc::to_string(report.size() / 1024) + " KB)");
        return true;
    });
}

export class Session
{
public:
    Session(const LaunchOptions& options)
        : m_options(options)
    {
        if (options.headlessServer())
            return;
        m_cameraController.initialize(glm::vec3(-1.5f, 14.0f, -7.1f), glm::vec3(0.0f, 4.0f, 0.0f));
        if (Globals::rendererVK.isVrEnabled()) // the renderer is up before the session exists
            m_vrCameraController.initialize(glm::vec3(-1.0f, Globals::rendererVK.isVrStageSpace() ? 0.0f : 1.0f, 0.0f));
    }
    // The controls take the camera by reference, so they are constructed AFTER the session and
    // linked back here.
    void attachControls(InputControls& controls) { m_controls = &controls; }
    FreeFlyCameraController& cameraController() { return m_cameraController; }

    GameMatch* game() { return m_game ? &*m_game : nullptr; }
    bool gameRunningOnScreen() const { return m_game && m_game->enabled() && !Globals::ui.isMainMenuActive(); }

    void installNetworkCallbacks()
    {
        Globals::networkManager.setOnGameEvent([this](oc::string_view name)
        {
            if (ChatSystem::handlesEvent(name))
            {
                m_chat.handleNetEvent();
                return;
            }
            if (!LobbySystem::handlesEvent(name))
            {
                if (m_game)
                    m_game->handleNetEvent(name);
                return;
            }
            m_lobby.handleNetEvent(name);
            if (m_lobby.takeClientConstruct() && !m_game)
                startWorldAndGame(true, m_lobby.coop());
            if (m_lobby.takeClientStart())
                Globals::ui.setMainMenuActive(false);
        });
        Globals::networkManager.setOnServerLost([this]()
        {
            if (m_options.mainMenu())
                m_serverLost = true;
        });
    }

    bool startNetworkFor(ELaunchMode mode, bool startGame, const oc::string& address)
    {
        if (mode == ELaunchMode::Single)
            return true;
        const bool started = mode == ELaunchMode::Server
            ? Globals::networkManager.startServer(m_options.netPort)
            : Globals::networkManager.startClient(address, m_options.netPort);
        if (!started)
            return false;
        if (mode != ELaunchMode::Server)
            return true;
        if (startGame)
        {
            Globals::networkManager.setOnClientJoined([this](uint32 clientId)
            {
                m_lobby.onClientJoined(clientId);
                if (m_game)
                    m_game->onClientJoined(clientId);
            });
            Globals::networkManager.setOnClientLeft([this](uint32 clientId)
            {
                m_lobby.onClientLeft(clientId);
                if (m_game)
                    m_game->onClientLeft(clientId);
            });
        }
        else
        {
            Globals::networkManager.setOnClientJoined([](uint32 clientId) { spawnTestbedNetPlayer(clientId); });
            Globals::networkManager.setOnClientLeft([](uint32 clientId) { removeEntitiesOwnedBy(clientId); });
        }
        return true;
    }

    void startWorldAndGame(bool startGame, bool startCoop)
    {
        ProfileScope scope("Session::startWorldAndGame", EProfileCategory::App);
        FileSystem::AllowMainThreadIO modeIo;
        if (!startGame)
        {
            TweakRegistry::get().setOverride("Terrain/Enabled=1");
            TweakRegistry::get().setOverride("Ocean/Enabled=1");
            Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/sponza.pre", Transform(), true));
            Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/skysphere.pre", Transform(), true));
            if (Globals::networkManager.role() == ENetRole::Server)
                Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/Debug/networkTest.pre", Transform(glm::vec3(0, 0, 0)), true));
        }
        m_controls->setGameMode(startGame);
        m_cameraController.setMovementEnabled(!startGame);
        Globals::ui.setGameLayout(startGame);
        if (!startGame)
            return;
        m_game.emplace(true, startCoop);
        uint32 mapSeed;
        float mapFill;
        int mapLanes;
        if (startCoop && m_lobby.hostMapSettings(mapSeed, mapFill, mapLanes))
            m_game->setMapSettings(mapSeed, mapFill, mapLanes);
        uint8 numTeams;
        oc::vector<oc::pair<uint32, uint8>> teamPicks;
        if (!startCoop && m_lobby.teamSettings(numTeams, teamPicks))
        {
            m_game->setLobbyTeams(numTeams, teamPicks);
            m_game->setPvpMap(m_lobby.pvpMap());
        }
        m_game->spawnWorld();
    }

    void exitToMenu()
    {
        m_controls->resetForMenu();
        m_game.reset();
        Globals::world.clearRootEntities();
        unseedWorld(); // Terrain/Ocean off, the origin back at (0, 0)
        m_terrainPreview.reset();
        m_previewUiImage = nullptr;
        m_previewUiGeneration = 0;
        Globals::networkManager.shutdown();
        Globals::networkManager.setEventFilter({});
        m_lobby.reset();
        m_chat.reset();
        Globals::ui.clearChat();
        m_controls->setGameMode(true);
        m_cameraController.setMovementEnabled(false);
        m_gameCameraDetached = false;
        Globals::ui.setGameLayout(false);
        Globals::ui.setMainMenuActive(true);
    }

    bool startFromCommandLine()
    {
        if (!startNetworkFor(m_options.launchMode, m_options.gameMode, m_options.connectAddress))
            return false;
        if (m_options.gameMode && m_options.launchMode == ELaunchMode::Server)
            m_lobby.markStarted(m_options.coopMode);
        startWorldAndGame(m_options.gameMode, m_options.coopMode);
        return true;
    }

    void enterMainMenu()
    {
        ProfileScope scope("Session::enterMainMenu", EProfileCategory::App);
        m_controls->setGameMode(true);
        m_cameraController.setMovementEnabled(false);
        NetAddress hostEndpoint = netGetLocalAddress();
        hostEndpoint.port = m_options.netPort;
        m_menuLanEndpoint = hostEndpoint.toString();
        Globals::ui.setMainMenuHostEndpoint(m_menuLanEndpoint);
        Globals::ui.setMainMenuHostNote("Resolving external IP...");
        Globals::ui.setMainMenuActive(true);
        m_externalIp = oc::make_shared<ExternalIpResult>();
        std::thread([result = m_externalIp]
        {
            result->address = netGetExternalAddress();
            result->done.store(true, oc::memory_order_release);
        }).detach();
    }

    void serviceServerLost()
    {
        if (!m_serverLost)
            return;
        m_serverLost = false;
        if (Globals::networkManager.role() == ENetRole::Client)
        {
            exitToMenu();
            Globals::ui.setMainMenuStatus("Disconnected from the server");
        }
    }

    void serviceMainMenu()
    {
        if (!Globals::ui.isMainMenuActive())
            return;
        serviceExternalIp();
        serviceMenuAction();
        serviceLobby();
    }

    void serviceChat()
    {
        if (const oc::string line = Globals::ui.takeChatOutgoing(); !line.empty())
            m_chat.send(line);
        if (m_chat.generation() != m_chatViewGeneration)
        {
            m_chatViewGeneration = m_chat.generation();
            Globals::ui.setChatView(m_chat.view());
        }
    }

    void serviceEscapeMenu()
    {
        const bool escapeAllowed = !Globals::ui.isMainMenuActive() || Globals::ui.isMainMenuLobbyOpen();
        if (m_controls->takeEscapePressed() && escapeAllowed)
        {
            if (Globals::ui.isEscapeMenuOpen())
                Globals::ui.setEscapeMenuOpen(false);
            else if (!m_game || !m_game->enabled() || Globals::ui.isMainMenuActive() || !m_game->escWouldCancel())
                Globals::ui.setEscapeMenuOpen(true);
        }
        const bool gameRunning = gameRunningOnScreen();
        Globals::ui.setEscapeOffersPause(gameRunning);
        Globals::ui.setGamePaused(gameRunning && m_game->isPaused());
        switch (Globals::ui.takeEscapeMenuAction())
        {
        case EscapeMenuAction::Resume:
            Globals::ui.setEscapeMenuOpen(false);
            break;
        case EscapeMenuAction::Pause:
            Globals::ui.setEscapeMenuOpen(false);
            if (gameRunning)
                m_game->requestPause(true);
            break;
        case EscapeMenuAction::Unpause:
            if (gameRunning)
                m_game->requestPause(false);
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

    void updateCamera(Camera& camera, double deltaSec)
    {
        if (gameRunningOnScreen() && !Globals::ui.isEscapeMenuOpen())
        {
            const bool detached = m_game->cameraDetached();
            if (detached != m_gameCameraDetached)
            {
                if (detached)
                {
                    const glm::vec3 forward = -glm::vec3(camera.viewMatrix[0][2], camera.viewMatrix[1][2], camera.viewMatrix[2][2]);
                    m_cameraController.setPose(camera.position, camera.position + forward);
                }
                m_cameraController.setMovementEnabled(detached);
                m_gameCameraDetached = detached;
            }
            if (detached)
            {
                m_cameraController.update(deltaSec);
                camera = m_cameraController.getCamera();
            }
            m_game->updateWindowed(camera, (float)deltaSec);
        }
        else if (gameRunningOnScreen())
        {
        }
        else if (Globals::rendererVK.isVrEnabled())
        {
            m_vrCameraController.update(deltaSec);
            camera = m_vrCameraController.getCamera();
        }
        else
        {
            m_cameraController.update(deltaSec);
            camera = m_cameraController.getCamera();
            m_controls->applyPlayerCamera(camera);
        }
    }

private:
    struct ExternalIpResult
    {
        NetAddress address;
        oc::atomic<bool> done{ false };
    };

    static void spawnTestbedNetPlayer(uint32 clientId)
    {
        EntityPtr player = Globals::world.spawnAssetFile("Entities/Debug/netPlayerCapsule.pre", Transform(glm::vec3(0, 10.0f, 0)), true);
        if (!player)
            return;
        player->setName("Player " + oc::to_string(clientId));
        Globals::networkManager.setOwner(*player, clientId);
        if (PhysicsComponent* pc = getComponent<PhysicsComponent>(player.get()))
            pc->onContact = [](Entity& self, Entity& other, bool begin)
            {
                // a player is never stealable, so its owner stays the client it spawned for
                if (const NetworkComponent* net = begin ? getComponent<NetworkComponent>(&self) : nullptr)
                    Globals::networkManager.stealOwnershipOnContact(other, net->ownerClientId);
            };
        Globals::world.addRootEntity(oc::move(player));
    }

    static void removeEntitiesOwnedBy(uint32 clientId)
    {
        oc::vector<Entity*> owned;
        for (const EntityPtr& root : Globals::world.rootEntities())
            if (const NetworkComponent* comp = getComponent<NetworkComponent>(root.get()); comp && comp->ownerClientId == clientId)
                owned.push_back(root.get());
        for (Entity* entity : owned)
            Globals::world.removeRootEntity(entity);
    }

    void serviceExternalIp()
    {
        if (!m_externalIp || !m_externalIp->done.load(oc::memory_order_acquire))
            return;
        NetAddress external = m_externalIp->address;
        m_externalIp.reset();
        if (external.ip != 0)
        {
            external.port = m_options.netPort;
            Globals::ui.setMainMenuHostEndpoint(external.toString());
            Globals::ui.setMainMenuHostNote("LAN: " + m_menuLanEndpoint);
            Log::info("External IP: " + external.toString());
        }
        else
            Globals::ui.setMainMenuHostNote("External IP lookup failed - this is the LAN address");
    }

    void serviceMenuAction()
    {
        const MainMenuAction action = Globals::ui.takeMainMenuAction();
        if (action.type == MainMenuAction::EType::Quit)
        {
            g_running = false;
            return;
        }
        if (action.type == MainMenuAction::EType::None)
            return;
        const ELaunchMode mode = action.host ? ELaunchMode::Server
            : !action.connectAddress.empty() ? ELaunchMode::Client : ELaunchMode::Single;
        const bool startGame = action.type != MainMenuAction::EType::StartSandbox;
        const bool startCoop = action.type == MainMenuAction::EType::StartCoop;
        if (!startNetworkFor(mode, startGame, action.connectAddress))
            return;
        if (startGame)
        {
            // Every game pick goes through the lobby: multiplayer to gather players, offline
            // (local) for the world block - terrain seed, preview map, "Seed world".
            m_lobby.enter(mode != ELaunchMode::Client, startCoop, mode == ELaunchMode::Single);
            Globals::ui.openMainMenuLobby();
        }
        else
        {
            startWorldAndGame(startGame, startCoop);
            Globals::ui.setMainMenuActive(false);
        }
    }

    void serviceLobby()
    {
        if (m_lobby.active())
        {
            const LobbyAction lobbyAction = Globals::ui.takeMainMenuLobbyAction();
            if (lobbyAction.type == LobbyAction::EType::Leave)
                exitToMenu();
            else
            {
                serviceWorldAction(lobbyAction);
                m_lobby.handleAction(lobbyAction);
                m_lobby.update((float)Globals::time.getDeltaSec());
                LobbyView view = m_lobby.view();
                if (view.local)
                    fillWorldView(view.world);
                Globals::ui.setMainMenuLobbyView(view);
            }
        }
        if (m_lobby.takeServerStart())
        {
            // A seeded world has every tile it may need on disk (Start waited for the seeder), so the
            // 2.28 GB of diffusion weights go before the match: the streamer keeps serving the caches
            // and nothing infers in game. The next preview or the sandbox reloads them.
            if (m_worldSeeded)
                Procedural::TerrainGenV3::unloadModels();
            startWorldAndGame(true, m_lobby.coop());
            if (m_game)
                for (const uint32 clientId : m_lobby.connectedClientIds())
                    m_game->onClientJoined(clientId);
            Globals::ui.setMainMenuActive(false);
        }
    }

    // ---- The local lobby's world block (the terrain seed preview + "Seed world") ----
    // The preview is Procedural's TerrainPreview (a session member: it owns a job and a generator,
    // nothing an entity reaches). Seeding the world is four tweak OVERRIDES - seed, origin X/Z,
    // Terrain + Ocean enabled - the same switch the sandbox throws, so the streamer builds the
    // full-detail ring around the pick while the lobby is still up (the world shows behind the
    // menu) and the page's bar follows TerrainStreamer::streamStatus until it settles.
    void serviceWorldAction(const LobbyAction& action)
    {
        if (action.type == LobbyAction::EType::GeneratePreview)
        {
            // The diffusion runtime holds ONE seed process-wide (TerrainPreview's header): a live
            // world of another seed would lose its tiles under the preview, so it goes first.
            if (m_worldSeeded)
                unseedWorld();
            m_terrainPreview.request(action.terrainSeed, Globals::terrain.v3MetersPerPixel());
        }
        else if (action.type == LobbyAction::EType::SeedWorld)
        {
            if (m_terrainPreview.state() != Procedural::TerrainPreview::EState::Ready)
                return;
            const glm::vec2 origin = m_terrainPreview.worldOffsetAt(action.pickU, action.pickV);
            const float halfSize = glm::clamp(action.areaSizeM, 256.0f, 65536.0f) * 0.5f;
            if (!m_worldSeeded)
                m_ringRadiusBeforeSeed = Globals::terrain.ringRadius(); // restored by unseedWorld
            // The ring covers the playable area and no more: the streamer builds meshes live over
            // the seeded tiles, and chunks past the area would pull cold tiles on the fly.
            const int ringRadius = glm::clamp((int)glm::ceil(halfSize / (float)glm::max(Globals::terrain.chunkSize(), 1)), 1, 64);
            TweakRegistry& tweaks = TweakRegistry::get();
            tweaks.setOverride(oc::format("Terrain/Seed={}", m_terrainPreview.seed()));
            tweaks.setOverride(oc::format("Terrain/Origin X (m)={}", origin.x));
            tweaks.setOverride(oc::format("Terrain/Origin Z (m)={}", origin.y));
            tweaks.setOverride(oc::format("Terrain/Range (chunks)={}", ringRadius));
            tweaks.setOverride("Terrain/Enabled=1");
            tweaks.setOverride("Ocean/Enabled=1");
            m_terrainSeeder.start(m_terrainPreview.seed(), Globals::terrain.v3MetersPerPixel(), origin, halfSize);
            // The streamer must not touch anything past the seeded tiles: the bounds gate its ring and
            // turn every full-detail sample outside them into a coarse one.
            glm::vec2 boundsMin, boundsMax;
            if (m_terrainSeeder.engineCoverage(boundsMin, boundsMax))
                Globals::terrain.setGeneratedBounds(true, boundsMin, boundsMax);
            m_worldSeeded = true;
            m_worldSeededPick = glm::vec2(action.pickU, action.pickV);
            Log::info(oc::format("Lobby: seeding the world with seed {} at ({:.0f}, {:.0f}) m, {:.0f} m playable area, ring {}",
                m_terrainPreview.seed(), origin.x, origin.y, halfSize * 2.0f, ringRadius));
        }
        else if (action.type == LobbyAction::EType::UnseedWorld)
            unseedWorld();
    }

    void unseedWorld()
    {
        TweakRegistry& tweaks = TweakRegistry::get();
        tweaks.setOverride("Terrain/Enabled=0");
        tweaks.setOverride("Ocean/Enabled=0");
        tweaks.setOverride("Terrain/Origin X (m)=0");
        tweaks.setOverride("Terrain/Origin Z (m)=0");
        if (m_worldSeeded)
        {
            tweaks.setOverride(oc::format("Terrain/Range (chunks)={}", m_ringRadiusBeforeSeed));
            Globals::terrain.setGeneratedBounds(false, glm::vec2(0.0f), glm::vec2(0.0f));
        }
        m_terrainSeeder.reset();
        m_worldSeeded = false;
    }

    void fillWorldView(LobbyWorldView& world)
    {
        using EState = Procedural::TerrainPreview::EState;
        m_terrainPreview.update();
        world.terrainSeed = m_terrainPreview.seed();
        switch (m_terrainPreview.state())
        {
        case EState::LoadingModels: world.preview = LobbyWorldView::EPreview::LoadingModels; break;
        case EState::Generating:    world.preview = LobbyWorldView::EPreview::Generating; break;
        case EState::Ready:         world.preview = LobbyWorldView::EPreview::Ready; break;
        case EState::Failed:        world.preview = LobbyWorldView::EPreview::Failed; break;
        default:                    world.preview = LobbyWorldView::EPreview::Idle; break;
        }
        world.previewProgress = m_terrainPreview.progress();
        world.previewStatus = m_terrainPreview.statusText();
        // The image crosses to the UI's own type ONCE per generation (UI cannot import Procedural).
        if (const oc::shared_ptr<const Procedural::TerrainPreview::Image> image = m_terrainPreview.image())
        {
            if (image->generation != m_previewUiGeneration)
            {
                auto ui = oc::make_shared<LobbyPreviewImage>();
                ui->width = image->width;
                ui->height = image->height;
                ui->generation = image->generation;
                ui->rgba = image->rgba;
                ui->extentKm = (float)(image->texelWorldSize * (double)image->width / 1000.0);
                ui->tileWorldSizeM = (float)image->tileWorldSize;
                m_previewUiImage = oc::move(ui);
                m_previewUiGeneration = image->generation;
            }
        }
        else
            m_previewUiImage = nullptr;
        world.image = m_previewUiImage;

        world.seeded = m_worldSeeded;
        world.seededPick = m_worldSeededPick;
        if (!m_worldSeeded)
            return;
        // The bar is the TILE seeding (the diffusion work); the terrain mesh streams live around it.
        using ESeed = Procedural::TerrainSeeder::EState;
        m_terrainSeeder.update();
        world.seededSettled = m_terrainSeeder.state() == ESeed::Done;
        world.seededProgress = m_terrainSeeder.progress();
        switch (m_terrainSeeder.state())
        {
        case ESeed::Failed:        world.seededStatus = "Terrain generator unavailable"; break;
        case ESeed::LoadingModels: world.seededStatus = "Loading terrain models..."; break;
        case ESeed::Generating:
        case ESeed::Done:
            world.seededStatus = oc::format("Tiles {} / {} ({} on disk)", m_terrainSeeder.tilesDone(),
                m_terrainSeeder.tilesTotal(), m_terrainSeeder.tilesCached());
            break;
        default:                   world.seededStatus = "Starting..."; break;
        }
        // The generated tiles' footprint, model space -> the map's normalized frame.
        glm::vec2 coveredMin, coveredMax;
        if (m_previewUiImage && m_terrainSeeder.coverage(coveredMin, coveredMax))
        {
            const float extent = m_previewUiImage->extentKm * 1000.0f;
            world.seededAreaValid = extent > 0.0f;
            world.seededAreaMin = coveredMin / extent + 0.5f;
            world.seededAreaMax = coveredMax / extent + 0.5f;
        }
    }

    const LaunchOptions& m_options;
    InputControls* m_controls = nullptr; // attachControls
    FreeFlyCameraController m_cameraController;
    VRFreeFlyCameraController m_vrCameraController;

    oc::optional<GameMatch> m_game;
    LobbySystem m_lobby;
    ChatSystem m_chat;
    uint32 m_chatViewGeneration = 0;
    bool m_serverLost = false;
    bool m_gameCameraDetached = false;
    oc::shared_ptr<ExternalIpResult> m_externalIp;
    oc::string m_menuLanEndpoint;
    Procedural::TerrainPreview m_terrainPreview;
    Procedural::TerrainSeeder m_terrainSeeder;
    oc::shared_ptr<const LobbyPreviewImage> m_previewUiImage;
    uint32 m_previewUiGeneration = 0;
    bool m_worldSeeded = false;
    glm::vec2 m_worldSeededPick = glm::vec2(0.5f);
    int m_ringRadiusBeforeSeed = 32; // "Terrain/Range (chunks)" before seeding sized the ring
};
