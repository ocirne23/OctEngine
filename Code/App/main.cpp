import Core;
import Core.Allocator;
import Core.Log;
import Core.Window;
import Core.SDL;
import Core.Time;
import Core.glm;
import Core.Camera;
import Settings;
import Core.Windows;

import App.Session;
import App.Tools;
import App.InputControls;
import App.UnattendedRun;

import Game;
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
import Particle;
import Force;
import Network;

int main(int argc, char* argv[])
{
    Globals::profiler.endStaticInit();
    ProfileScope initScope("main() initialize", EProfileCategory::App);
    FileSystem::initialize();
    oc::optional<FileSystem::AllowMainThreadIO> startupIo;
    startupIo.emplace();
    installFileHooks();
    // Every tweak, before any system initializes (their Tweak::onChange listeners need the rows); the saved values
    // apply as each row registers.
    Settings::registerAll();
    Globals::time.bindSettings(Globals::settings.time);

    runCommandLineTool(argc, argv);

    const LaunchOptions options = parseCommandLine(argc, argv);
    const bool headlessServer = options.headlessServer();
    const bool unattendedRun = options.unattendedRun();
    if (unattendedRun)
        installUnattendedFailureHandling();

    // --fullscreen: a borderless desktop-size window with the editor panels hidden (full-resolution profiling).
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i)
        fullscreen |= oc::string_view(argv[i]) == "--fullscreen";

    Window window;
    if (!headlessServer)
    {
        window.initialize("Vulkan", glm::ivec2(5, 35), glm::ivec2(1920, 1080), fullscreen);
        Globals::input.setEventSource(&window);
        Globals::input.initialize();
    }
    Globals::jobSystem.initialize();
    if (!headlessServer)
    {
        window.runOnWindowThread([] { Globals::jobSystem.registerExternalHelper(); }, true);
        window.setIdleWork([] { return Globals::jobSystem.tryRunOneHighJob(); },
                           [](bool (*wakeNow)(const void*), const void* user) { Globals::jobSystem.externalHelperWait(wakeNow, user); },
                           [] { Globals::jobSystem.wakeExternalHelper(); });
        Globals::rendererVK.initialize(window, EValidation::DISABLED, EVr::DISABLED); // DISABLED ENABLED
        Globals::ui.initialize();
        if (fullscreen)
            Globals::ui.setEditorPanels(false);
    }
    Globals::world.initialize();
    Globals::world.setHeadless(headlessServer);
    Globals::physics.initialize();
    if (!headlessServer)
        Globals::audio.initialize();
    Globals::spatialIndex.initialize();
    Globals::occlusionBuffer.initialize();
    if (!headlessServer)
    {
        Globals::particleSystem.initialize();
        Globals::forceSystem.initialize();
    }
    Globals::networkManager.initialize();
    Globals::scriptEvents.initialize();
    registerScriptDslBindings();

    EntityPtr terrainEntity;
    if (!headlessServer)
    {
        Globals::terrain.initialize();
        terrainEntity = Globals::world.createEmptyEntity("Terrain");
        Globals::terrainCollider.initialize((void*)terrainEntity);
        Globals::trees.initialize();
        Globals::rocks.initialize();
        Globals::clutter.initialize();
        Globals::ocean.initialize();
        Globals::terrain.setFlowWindAngle(Globals::ocean.swellTravelAngle());
        Globals::physics.setWaterSurface([](float x, float z) { return Globals::ocean.sampleWaterHeight(x, z); },
            [] { return Globals::ocean.hasWater(); });
        if (Globals::rendererVK.isVrEnabled())
            Globals::vrInput.initialize(Globals::rendererVK.getVrSession());
    }

    // RESIZE HOLD: while the left mouse button is down - dragging the window border or an ImGui panel splitter - the
    // renderer keeps its sizes (the last viewport rect, no swapchain / window-surface rebuild) and takes the final size
    // when the button is released. A rebuild per frame of the drag recreated the DLSS feature every frame and broke it.
    bool windowResizePending = false;
    Rect heldViewportRect;
    SystemEventListenerHandle systemEventListener;
    if (headlessServer)
        SetConsoleCtrlHandler([](DWORD) -> BOOL { g_running = false; return TRUE; }, TRUE);
    else
    {
        systemEventListener = Globals::input.addSystemEventListener();
        systemEventListener->onQuit = []() { g_running = false; };
        systemEventListener->onWindowEvent = [&windowResizePending](const SDL_WindowEvent& evt)
        {
            if (evt.type == SDL_EVENT_WINDOW_RESIZED)   windowResizePending = true; // applied once the resize hold ends (main loop)
            if (evt.type == SDL_EVENT_WINDOW_MINIMIZED) Globals::rendererVK.setWindowMinimized(true);
            if (evt.type == SDL_EVENT_WINDOW_MAXIMIZED) Globals::rendererVK.setWindowMinimized(false);
            if (evt.type == SDL_EVENT_WINDOW_RESTORED)  Globals::rendererVK.setWindowMinimized(false);
        };
    }

    GizmoController gizmo;
    Session session(options);
    InputControls controls(gizmo, session.cameraController(), Globals::world);
    controls.setProfileDump(options.profileOutPath, options.profileOptions);
    session.attachControls(controls);
    session.installNetworkCallbacks();
    if (options.mainMenu())
        session.enterMainMenu();
    else if (!session.startFromCommandLine())
        return 1;

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
    Timer fpsTimer(std::chrono::seconds(1), [&](Timer&) {
            fps = frameCount;
            frameCount = 0;
            return Timer::REPEAT;
        });
    Timer titleUpdateTimer(std::chrono::milliseconds(100), [&](Timer&) {
            if (headlessServer)
                return Timer::REPEAT;
            const glm::vec3 pos = session.cameraController().getPosition();
            const glm::vec3 dir = session.cameraController().getDirection();
            const oc::string netStatus = Globals::networkManager.getStatusText();
            char windowTitleBuf[320];
            sprintf_s(windowTitleBuf, sizeof(windowTitleBuf), "%s%sFPS: %i mem: %.2fmb instances: %i meshtypes: %i materials: %i, pos: %.1f, %.1f, %.1f, dir: %.2f, %.2f, %.2f",
                netStatus.c_str(), netStatus.empty() ? "" : " | ",
                fps, (double)Globals::allocator.getUsedSize() / 1024.0 / 1024.0,
                Globals::rendererVK.getNumMeshInstances(), Globals::rendererVK.getNumMeshTypes(), Globals::rendererVK.getNumMaterials(), pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
            window.setTitle(windowTitleBuf);
            return Timer::REPEAT;
        });
    Timer renderStatsUpdateTimer(std::chrono::seconds(1), [&](Timer&) {
            if (!headlessServer)
                Globals::ui.setRenderStats(Globals::rendererVK.getStats());
            return Timer::REPEAT;
        });
    Timer headlessStatusTimer(std::chrono::seconds(5), [&](Timer&) {
            if (headlessServer)
                Log::info("Headless: " + oc::to_string(fps) + " ticks/s | " + Globals::networkManager.getStatusText());
            return Timer::REPEAT;
        });

    initScope.stop();
    startupIo.reset();

    Globals::time.update();
    const auto timerDelay = [](double sec) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(oc::max(sec, 0.001))); };
    oc::optional<Timer> scenarioTimer;
    if (options.scenario)
        scenarioTimer.emplace(timerDelay(options.scenarioAtSec), [&](Timer&) {
                if (GameMatch* game = session.game(); game && game->enabled())
                {
                    FileSystem::AllowMainThreadIO scenarioIo;
                    game->runScenario(options.scenarioSave == "default" ? oc::string_view() : oc::string_view(options.scenarioSave));
                }
                else
                    Log::warning("--scenario needs --game");
                return Timer::DONE;
            });
    oc::optional<Timer> profileDumpTimer;
    if (options.profileAfterSec > 0.0)
        profileDumpTimer.emplace(timerDelay(options.profileAfterSec), [&](Timer&) {
                Globals::profiler.writeReport(options.profileOutPath, options.profileOptions);
                return Timer::DONE;
            });
    oc::optional<Timer> screenshotTimer;
    if (options.screenshotAfterSec > 0.0 && !headlessServer)
        screenshotTimer.emplace(timerDelay(options.screenshotAfterSec), [&](Timer&) {
                Globals::rendererVK.requestScreenshot(options.screenshotOutPath);
                return Timer::DONE;
            });
    oc::optional<Timer> quitTimer;
    if (options.quitAfterSec > 0.0)
        quitTimer.emplace(timerDelay(options.quitAfterSec), [](Timer&) {
                g_running = false;
                return Timer::DONE;
            });

    bool uiJobKicked = false;
    while (g_running)
    {
        if (!headlessServer)
        {
            Globals::time.setFpsCeiling(Globals::ui.isMainMenuActive() ? 60 : 0);
            Globals::time.beginFrame(Globals::input.isWindowHasFocus() || unattendedRun, Globals::rendererVK.isVrEnabled(),
                Globals::rendererVK.isVSyncEnabled(), window.getDisplayRefreshHz(), &window,
                [](uint64 timeoutNs) { return Globals::rendererVK.waitFrameSlot(timeoutNs); });
        }
        else
            Globals::time.update();

        ProfileScope mainLoopScope("main loop", EProfileCategory::App);
        Globals::jobSystem.joinPostUpdateJobs();

        if (!headlessServer)
        {
            if (uiJobKicked)
            {
                Globals::ui.flushMainThreadWork();
                Globals::rendererVK.updateImGuiTextures();
            }
            Globals::forceSystem.joinMerge();
            session.serviceServerLost();
            session.serviceMainMenu();
            session.serviceChat();
        }

        const double deltaSec = Globals::time.getDeltaSec();
        TweakRegistry::get().update((float)deltaSec);

        if (!headlessServer)
        {
            Globals::input.update(deltaSec);
            Globals::ui.prepare();
            controls.update((float)deltaSec);
            session.serviceEscapeMenu();
            Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim);
            session.updateCamera(camera, deltaSec);
            Globals::scriptHost.handleScriptReloadRequests(Globals::ui.takeScriptReloadRequests());
            Globals::world.handleEntityChanges(Globals::ui.takeEntityChanges(), camera, Globals::ui.getViewportRect());
        }
        Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim);
        Globals::world.handleEntityChanges(Globals::scriptEvents.takeEntityChanges(), camera, Globals::ui.getViewportRect());

        const double simDeltaSec = Globals::time.getSimDeltaSec();
        Globals::jobSystem.setFrameHasPhysicsStep(Globals::physics.willStep(simDeltaSec));

        Globals::networkManager.receive(deltaSec);
        GameMatch* game = session.game();
        if (game)
            game->updatePlayer((float)simDeltaSec);
        Globals::scriptContext.update(camera, (float)simDeltaSec, (float)Globals::time.getSimElapsedSec());

        Globals::world.joinSelection();
        if (!headlessServer)
        {
            Globals::terrain.joinUploads(); // the begin-frame job and the entity pass read the mesh tables it grows
            ProfileScope kickScope("Frame kicks", EProfileCategory::App);
            // The resize hold (see windowResizePending). The global button state is an OS query: it also sees a drag on
            // the window border, which runs in Windows' modal sizing loop on the window thread.
            constexpr uint32 LEFT_BUTTON_MASK = 1u << 0; // SDL_BUTTON_LMASK (a macro: not through the module import)
            const bool resizeHold = ((uint32)SDL_GetGlobalMouseState(nullptr, nullptr) & LEFT_BUTTON_MASK) != 0;
            Globals::rendererVK.setResizeHold(resizeHold);
            if (!resizeHold && windowResizePending)
            {
                windowResizePending = false;
                Globals::rendererVK.recreateWindowSurface(window);
            }
            const Rect viewportRect = Globals::ui.getViewportRect();
            if (!resizeHold || heldViewportRect.getSize().x <= 0 || heldViewportRect.getSize().y <= 0)
                heldViewportRect = viewportRect;
            Globals::spatialIndex.kickUpdateJob(Globals::rendererVK.setFrameView(camera, heldViewportRect));
            Globals::rendererVK.kickBeginFrameJob();
        }
        Globals::physics.update(simDeltaSec);
        if (!headlessServer)
        {
            Globals::audio.update(camera);
            Globals::navSystem.update((float)simDeltaSec);
            Globals::rendererVK.joinBeginFrameJob();
            Globals::spatialIndex.joinUpdateJob();
        }
        else
            Globals::spatialIndex.commitFrame();
        if (game)
            game->update((float)simDeltaSec);
        else if (!headlessServer)
            Globals::world.setSimLodFocus(&camera.position, 1);
        Globals::physics.dispatchContactEvents([](const PhysicsWorld::ContactEvent& evt) { Globals::world.handleContactEvent(evt); });

        Globals::world.update(Globals::rendererVK, (float)simDeltaSec);
        Globals::networkManager.send(deltaSec);

        if (!headlessServer)
        {
            Globals::terrain.setFlowWindAngle(Globals::ocean.swellTravelAngle()); // follows "Sky/Wind" (the next bake takes it)
            Globals::terrain.update(Globals::rendererVK, camera);
            Globals::terrainCollider.update(camera.position, Globals::terrain.activeClimateMaps());
            Globals::ocean.update(Globals::rendererVK, camera, Globals::terrain.activeTerrainData(), Globals::terrain.seaLevel());
            Globals::terrain.render(Globals::rendererVK, Globals::ocean); // one walk of the visible hand-over for chunks + sectors; kicks both pushes
            Globals::trees.update(Globals::rendererVK, camera, Globals::terrain.activeClimateMaps());
            Globals::rocks.update(Globals::rendererVK, camera, Globals::terrain.activeClimateMaps());
            Globals::clutter.update(Globals::rendererVK, camera, Globals::terrain.activeClimateMaps()); // after trees: it reads their records
            Globals::particleSystem.update(Globals::rendererVK, (float)simDeltaSec);
            Globals::forceSystem.update(Globals::rendererVK, (float)simDeltaSec);
            Globals::rendererVK.kickGridBuilds(); // the force update is the frame's last light source and the emitters' last writer

            Globals::ui.drawGizmoEntity(Globals::rendererVK, (float)deltaSec);
            if (game)
                game->joinWorldLabels();
            Globals::ui.update(Globals::world.rootEntities(), camera, deltaSec);
            uiJobKicked = true;

            Globals::terrainCollider.joinUpdate(); // its box3d body writes end before the Sim batch reads bodies
            Globals::jobSystem.kickPostUpdateJobs();
            Globals::terrain.joinRender();
            Globals::ocean.joinRender();
            Globals::rendererVK.present();
            Globals::terrain.kickUploads(Globals::rendererVK); // overlaps the frame-pacing wait; joined before the next kicks
        }
        else
            Globals::jobSystem.kickPostUpdateJobs();

        mainLoopScope.stop();
        Globals::profiler.endFrame();
        frameCount++;

        if (headlessServer)
            while (g_running && Clock::now() < Globals::time.getCurrentTime() + Clock::duration(std::chrono::seconds(1)) / options.tickHz)
                Sleep(1);
    }

    Globals::jobSystem.joinPostUpdateJobs();
    Globals::jobSystem.joinPostUpdateJobs(JobSystem::EPostUpdateBatch::Sim);
    Globals::world.joinSelection();
    Globals::forceSystem.joinMerge();
    return 0;
}
