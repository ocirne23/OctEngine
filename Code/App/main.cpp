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

static oc::atomic<bool> g_running = true; // cleared by the window's onQuit (windowed) or the console ctrl handler (headless)

static BOOL __stdcall consoleCtrlHandler(DWORD) { g_running = false; return TRUE; } // any console ctrl event = clean shutdown

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
        // both ends must agree, or the handshake denies with a clear reason
        else if (arg == "--no-encrypt")                   NetworkManager::setEncryption(false);
        else Log::warning("Unknown command line argument: " + oc::string(arg));
    }
    if (headless && launchMode != ELaunchMode::Server)
        Log::warning("--headless only applies to --server, ignoring");
    if (gameMode && headless && launchMode == ELaunchMode::Server)
    {
        Log::warning("--game needs a window (GPU field readbacks drive the authority sim), ignoring --game");
        gameMode = false; // co-op = a WINDOWED listen server (--game --server) + clients (--game --connect)
    }
    const bool headlessServer = headless && launchMode == ELaunchMode::Server;
    const bool unattendedRun = profileAfterSec > 0.0 || quitAfterSec > 0.0;

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
                           [] { Globals::jobSystem.externalHelperWait(); },
                           [] { Globals::jobSystem.wakeExternalHelper(); });
    }
    if (!headlessServer)
    {
        cameraController.initialize(glm::vec3(-1.5f, 14.0f, -7.1f), glm::vec3(0.0f, 4.0f, 0.0f));
        Globals::rendererVK.initialize(window, EValidation::DISABLED, EVr::DISABLED); // ENABLED DISABLED
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
    controls.setProfileDump(profileOutPath, profileOptions); // F7 writes where --profile-out points
    GameMatch game(gameMode); // stack local: holds EntityPtrs/Force handles, destructs before the globals
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

    JobCounter uiCounter;      // the in-flight UI widget pass, joined at the TOP of the next frame
    bool uiJobKicked = false;  // no ImGui::Render / draw data until the first pass ran

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

        if (!headlessServer)
        {
            {
                // LAST frame's widget pass (kicked right after present): it ran during endFrame and
                // the fence/vsync wait above, so this is near-zero unless the widget pass outlasted
                // the whole stall. Joined before anything that mutates state the panels read and
                // before the input pump touches the ImGui context.
                ProfileScope uiJoinScope("UI join", EProfileCategory::Wait);
                Globals::jobSystem.wait(uiCounter);
            }
            if (uiJobKicked)
                Globals::ui.flushMainThreadWork(); // deferred tweak onChange callbacks + container imports (the job produced its own draw-data snapshot)
            Globals::forceSystem.joinMerge(); // last frame's merge job (kicked after the force upload, ran during present + the stall); before input/drains can touch emitters
        }

        const double deltaSec = Globals::time.getDeltaSec(); // clock advanced by limitFrameRate / update above
        TweakRegistry::get().update((float)deltaSec); // Saved/Synced change detection



        if (!headlessServer)
        {
            Globals::input.update(deltaSec);
            Globals::ui.prepare(); // panel data jobs (profiler snapshot etc.) — overlap everything below until ui.update
            controls.update((float)deltaSec);

            if (game.enabled())
            {
                game.updateWindowed(camera, (float)deltaSec); // game mode: follow-camera overwrite + aim/HUD/debug draw
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
        Globals::world.handleEntityChanges(Globals::scriptEvents.takeEntityChanges(), camera, Globals::ui.getViewportRect());

        Globals::networkManager.receive(deltaSec); // snapshot targets + events land before the sim/entity updates read them
        game.update((float)deltaSec); // authority tick, pre-physics (direct body setters sanctioned); becomes the server tick in MP
        Globals::scriptContext.update(camera, (float)deltaSec, (float)Globals::time.getElapsedSec());

        Globals::physics.update(deltaSec, [](const PhysicsWorld::ContactEvent& evt) { Globals::world.handleContactEvent(evt); });

        if (!headlessServer)
        {
            Globals::audio.update(camera);
            const Frustum& frustum = Globals::rendererVK.beginFrame(camera, Globals::ui.getViewportRect()); // stable: the widget pass joined at the top of the frame
            Globals::spatialIndex.update(camera, frustum, Globals::rendererVK.getCenterViewProj() * glm::translate(glm::mat4(1.0f), camera.position)); // translate corrects the reverse-z renderer proj matrix
        }
        Globals::navSystem.finishSteps(); // the flow/pressure step job feedNav kicked; units sample the fields in the entity pass
        Globals::navSystem.finishSteps(); // the flow/pressure step job feedNav kicked; units sample the fields in the entity pass
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
            Globals::rendererVK.present(); // ImGui pass records from the PREVIOUS widget pass's snapshot

            Globals::ui.beginImGuiFrame(); // kick next frame's ui update job
            Globals::jobSystem.submit([&] { Globals::ui.update(Globals::world.rootEntities(), camera, deltaSec); },
                { "UI widget pass", EProfileCategory::UI }, EJobPriority::Normal, &uiCounter);
            uiJobKicked = true;
        }

        mainLoopScope.stop(); // before the frame mark, so the record stays inside this frame's window
        Globals::profiler.endFrame();
        frameCount++;

        // Unattended profiling: the report covers completed frames only, so it is written right
        // after the frame mark; --quit-after alone (no dump) is a plain timed exit. Each trigger
        // fires once, on the first frame past its time.
        const double elapsedSec = Globals::time.getElapsedSec();
        if (scenario && elapsedSec >= scenarioAtSec)
        {
            scenario = false;
            if (!game.enabled())
                Log::warning("--scenario needs --game");
            else
            {
                FileSystem::AllowMainThreadIO scenarioIo; // one-shot load, like F10
                game.runScenario(scenarioSave == "default" ? oc::string_view() : oc::string_view(scenarioSave));
            }
        }
        if (profileAfterSec > 0.0 && elapsedSec >= profileAfterSec)
        {
            profileAfterSec = 0.0;
            writeProfileReport(profileOutPath, profileOptions);
        }
        if (quitAfterSec > 0.0 && elapsedSec >= quitAfterSec)
            g_running = false;

        if (headlessServer)
            while (g_running && Clock::now() < Globals::time.getCurrentTime() + Clock::duration(std::chrono::seconds(1)) / tickHz)
                Sleep(1);
    }

    Globals::jobSystem.wait(uiCounter); // the final frame's widget pass may still be in flight
    Globals::forceSystem.joinMerge();   // and the final frame's merge job

    return 0;
}
