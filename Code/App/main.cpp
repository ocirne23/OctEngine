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
import Particle;
import Force;

static std::atomic<bool> g_running = true; // cleared by the window's onQuit (windowed) or the console ctrl handler (headless)
static BOOL __stdcall consoleCtrlHandler(DWORD) { g_running = false; return TRUE; } // any console ctrl event = clean shutdown

int main(int argc, char* argv[])
{
    Globals::profiler.endStaticInit(); // closes the "Static init" scope opened at the profiler's static-init construction; must precede any main() scope
    ProfileScope initScope("main() initialize", EProfileCategory::App);
    FileSystem::initialize();
    TweakRegistry::get().loadSaved(); // Saved-flagged tweaks apply from Local/tweaks.cfg from here on

    // --server [--port N] [--headless] [--tickrate N] hosts an authoritative session; --connect
    // <ip[:port]> joins one; no flags = single player (networking fully inert). Same executable for
    // all of them; --headless runs the server without window/renderer/UI (server only).
    enum class ELaunchMode { Single, Server, Client };
    ELaunchMode launchMode = ELaunchMode::Single;
    uint16 netPort = 27888;
    int tickHz = 60;
    std::string connectAddress;
    bool headless = false;
    bool gameMode = false;
    bool pvpMode = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--server")                            launchMode = ELaunchMode::Server;
        else if (arg == "--connect" && i + 1 < argc)      { launchMode = ELaunchMode::Client; connectAddress = argv[++i]; }
        else if (arg == "--port" && i + 1 < argc)         netPort = uint16(std::atoi(argv[++i]));
        else if (arg == "--tickrate" && i + 1 < argc)     tickHz = glm::clamp(std::atoi(argv[++i]), 10, 240);
        else if (arg == "--headless")                     headless = true;
        else if (arg == "--game")                         gameMode = true; // whitebox game instead of the testbed scene
        else if (arg == "--pvp")                          pvpMode = true;  // with --game: no ambient/enemies, per-client teams
        // both ends must agree, or the handshake denies with a clear reason
        else if (arg == "--no-encrypt")                   NetworkManager::setEncryption(false);
        else Log::warning("Unknown command line argument: " + std::string(arg));
    }
    if (headless && launchMode != ELaunchMode::Server)
        Log::warning("--headless only applies to --server, ignoring");
    if (gameMode && headless && launchMode == ELaunchMode::Server)
    {
        Log::warning("--game needs a window (GPU field readbacks drive the authority sim), ignoring --game");
        gameMode = false; // co-op = a WINDOWED listen server (--game --server) + clients (--game --connect)
    }
    const bool headlessServer = headless && launchMode == ELaunchMode::Server;

    Window window;
    FreeFlyCameraController cameraController;
    VRFreeFlyCameraController vrCameraController;

    if (!headlessServer)
    {
        window.initialize("Vulkan", glm::ivec2(5, 35), glm::ivec2(1920, 1080));
        Globals::input.initialize();
    }
    Globals::jobSystem.initialize();
    if (!headlessServer)
    {
        cameraController.initialize(glm::vec3(-1.5f, 14.0f, -7.1f), glm::vec3(0.0f, 4.0f, 0.0f));
        Globals::rendererVK.initialize(window, EValidation::DISABLED, EVSync::ENABLED, EVr::DISABLED); // ENABLED DISABLED
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

    if (launchMode != ELaunchMode::Single)
    {
        const bool started = launchMode == ELaunchMode::Server
            ? Globals::networkManager.startServer(netPort)
            : Globals::networkManager.startClient(connectAddress, netPort);
        if (!started)
            return 1;
        if (launchMode == ELaunchMode::Server)
        {
            Globals::networkManager.setOnClientJoined([](uint32 clientId)
            {
                EntityPtr player = Globals::world.spawnAssetFile("Entities/Debug/netPlayerCapsule.pre",
                    Transform(glm::vec3(0, 10.0f, 0)), true);
                if (!player)
                    return;
                player->setName("Player " + std::to_string(clientId));
                Globals::networkManager.setOwner(*player, clientId);
                // ownership STEALING: whatever this player's body collides with becomes theirs (last
                // collider wins, other players' primaries excluded) — needs ContactEvents on the shapes
                if (PhysicsComponent* pc = getComponent<PhysicsComponent>(player.get()))
                    pc->onContact = [clientId](Entity& other, bool begin)
                    {
                        if (begin)
                            Globals::networkManager.stealOwnershipOnContact(other, clientId);
                    };
                Globals::world.addRootEntity(std::move(player));
            });
            Globals::networkManager.setOnClientLeft([](uint32 clientId)
            {
                std::vector<Entity*> owned; // collected first: removeRootEntity mutates the list being walked
                for (const EntityPtr& root : Globals::world.rootEntities())
                    if (const NetworkComponent* comp = getComponent<NetworkComponent>(root.get()); comp && comp->ownerClientId == clientId)
                        owned.push_back(root.get());
                for (Entity* entity : owned)
                    Globals::world.removeRootEntity(entity);
            });
        }
    }

    EntityPtr terrainEntity;
    if (!headlessServer)
    {
        Globals::terrain.initialize();
        terrainEntity = Globals::world.createEmptyEntity("Terrain");
        Globals::terrainCollider.initialize((void*)terrainEntity);
        Globals::scatter.initialize();
        Globals::ocean.initialize();
        Globals::terrain.setFlowWindAngle(Globals::ocean.swellTravelAngle());
        Globals::physics.setWaterSurface([](float x, float z) { return Globals::ocean.sampleWaterHeight(x, z); });
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

    if (!gameMode)
        Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/sponza.pre", Transform(), true));
    //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/skysphere.pre", Transform(spawnOffset), true));
    //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/character.pre", Transform(spawnOffset), true));
    //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/particle.pre", Transform(spawnOffset), true));
    //Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/SphereField.pre", Transform(spawnOffset), true));

    if (!gameMode && launchMode == ELaunchMode::Server)
    {
        Globals::world.addRootEntity(Globals::world.spawnAssetFile("Entities/Debug/networkTest.pre", Transform(glm::vec3(0, 0, 0)), true));
    }

    GizmoController gizmo;
    InputControls controls(gizmo, cameraController, Globals::world); // headless-inert: update/key handling never run
    GameMatch game(gameMode, pvpMode && gameMode); // stack local: holds EntityPtrs/Force handles, destructs before the globals
    if (game.enabled())
    {
        game.spawnWorld();
        controls.setGameMode(true);                  // mutes the testbed spawn/possess keys
        cameraController.setMovementEnabled(false);  // WASD belongs to the game player
        if (Globals::networkManager.role() == ENetRole::Server)
        {
            // Co-op: the game owns per-client lifecycles (player spawn + world-state replay),
            // replacing the testbed's netPlayerCapsule hooks registered above.
            Globals::networkManager.setOnClientJoined([&game](uint32 clientId) { game.onClientJoined(clientId); });
            Globals::networkManager.setOnClientLeft([&game](uint32 clientId) { game.onClientLeft(clientId); });
        }
    }

    Camera camera;
    camera.viewMatrix = glm::mat4(1.0f);
    camera.position = glm::vec3(0.0f);
    if (!headlessServer)
    {
        gizmo.initialize(Globals::world);
        Globals::ui.setGizmo(&gizmo);
        Globals::physics.setDebugDrawCallback([](const glm::vec3& a, const glm::vec3& b, uint32 color) { Globals::rendererVK.addDebugLine(a, b, color); }, [&camera]() { return camera.position; });
        Globals::world.setOnPrefabOpened([](const EntityPtr& entity, const std::string& path) { Globals::ui.onOpened(entity, path); });
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
            const std::string netStatus = Globals::networkManager.getStatusText(); // empty in single player
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
                Log::info("Headless: " + std::to_string(fps) + " ticks/s | " + Globals::networkManager.getStatusText());
            return Timer::REPEAT;
        });

    initScope.stop();

    while (g_running)
    {
        ProfileScope mainLoopScope("main loop", EProfileCategory::App);

        Globals::time.update();
        const double deltaSec = Globals::time.getDeltaSec();
        TweakRegistry::get().update((float)deltaSec); // Saved/Synced change detection



        if (!headlessServer)
        {
            Globals::input.update(deltaSec);
            controls.update((float)deltaSec);
            if (Globals::rendererVK.isVrEnabled())
            {
                vrCameraController.update(deltaSec); // thumbstick locomotion; pulls Globals::vrInput
                camera = vrCameraController.getCamera();
            }
            else
            {
                cameraController.update(deltaSec);
                camera = cameraController.getCamera();
                controls.applyPlayerCamera(camera); // possessed capsule view (first/third person), see InputControls
                game.updateWindowed(camera, (float)deltaSec); // game mode: follow-camera overwrite + aim/HUD/debug draw
            }
            Globals::ui.update(Globals::world.rootEntities(), camera, deltaSec); // also drives the gizmo it owns

            for (const std::string& reloadPath : Globals::ui.takeScriptReloadRequests()) Globals::scriptHost.getOrLoad(reloadPath, true);
            for (EntityChange& change : Globals::ui.takeEntityChanges()) Globals::world.handleEntityChange(change, camera, Globals::ui.getViewportRect());
        }
        for (EntityChange& change : Globals::scriptEvents.takeEntityChanges()) Globals::world.handleEntityChange(change, camera, Globals::ui.getViewportRect());

        Globals::networkManager.receive(deltaSec); // snapshot targets + events land before the sim/entity updates read them
        game.update((float)deltaSec); // authority tick, pre-physics (direct body setters sanctioned); becomes the server tick in MP
        Globals::scriptContext.update(camera, (float)deltaSec, (float)Globals::time.getElapsedSec());
        Globals::physics.update(deltaSec, [](const PhysicsWorld::ContactEvent& evt) { Globals::world.handleContactEvent(evt); });

        if (!headlessServer)
        {
            Globals::audio.update(camera);
            const Frustum& frustum = Globals::rendererVK.beginFrame(camera, Globals::ui.getViewportRect());
            Globals::spatialIndex.update(camera, frustum, Globals::rendererVK.getCenterViewProj() * glm::translate(glm::mat4(1.0f), camera.position)); // translate corrects the reverse-z renderer proj matrix
        }
        Globals::world.update(Globals::rendererVK, (float)deltaSec); // serial script prepass + parallel component/tree pass + sink flush; headless: renderer passed through but never dereferenced (headless archetypes)
        Globals::networkManager.send(deltaSec); // server: snapshot entities at their post-update poses; both roles: flush queued packets

        if (!headlessServer)
        {
            Globals::terrain.update(Globals::rendererVK, camera);
            Globals::terrainCollider.update(camera.position, Globals::terrain.activeClimateMaps());
            Globals::ocean.update(Globals::rendererVK, camera, Globals::terrain.activeTerrainData(), Globals::terrain.seaLevel());
            Globals::scatter.update(Globals::rendererVK, camera, Globals::terrain.activeClimateMaps());
            Globals::particleSystem.update(Globals::rendererVK, (float)deltaSec);
            Globals::forceSystem.update(Globals::rendererVK, (float)deltaSec);

            Globals::ui.drawGizmoEntity(Globals::rendererVK, (float)deltaSec);
            Globals::ui.render();
            Globals::rendererVK.present();
        }

        mainLoopScope.stop(); // before the frame mark, so the record stays inside this frame's window
        Globals::profiler.endFrame();
        frameCount++;

        if (headlessServer)
            while (g_running && Clock::now() < Globals::time.getCurrentTime() + Clock::duration(std::chrono::seconds(1)) / tickHz)
                Sleep(1);
    }

    return 0;
}
