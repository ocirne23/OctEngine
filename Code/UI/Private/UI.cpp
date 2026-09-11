module UI;

import Core;
import File; // disk access goes through FileSystem (Core no longer exports <filesystem>)
import Core.imgui;
import Core.Log;
import Core.glm;
import Core.SDL;
import Entity;
import RendererVK; // hands the snapshotted ImGui draw data to the renderer (see UI::render)
import Threading;

import :AssetBrowser;
import :SceneView;
import :PropertiesPanel;
import :OutputLog;
import :TweakPanel;
import :ProfilerPanel;
import :MemoryPanel;
import :TextEditor;
import :ScriptEditor;
import :GameHudOverlay;
import :MainMenu;

void UI::initialize()
{
    ProfileScope scope("UI::initialize", EProfileCategory::UI);
    ImGuiContext* context = ImGui::GetCurrentContext();
    assert(context != nullptr && "Imgui must be initialized by renderer first");
    (void)context;

    ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.0, 0.0, 0.0, 0.0);

    m_assetBrowser.initialize();
    m_gameHudOverlay.registerTweaks();
}

void UI::drawGizmoEntity(Renderer& renderer, float deltaSec)
{
    ProfileScope profileScope("Gizmo entity", EProfileCategory::UI);
    if (m_gizmo && m_gizmo->isVisible())
        m_gizmo->getGizmoEntity()->update(renderer, deltaSec);
}

void UI::prepare()
{
    ProfileScope profileScope("UI prepare", EProfileCategory::UI);
    // One job per data-heavy panel that was open last frame; each panel's prepare() is worker-safe
    // (no ImGui, reads only its own state + reader-safe engine sources: profiler rings, the memory
    // tracker's atomic tree, the log under its mutex). updateJob() waits on the counter before
    // ImGui::NewFrame, so nothing here ever overlaps the widget pass that reads the results.
    if (m_profilerOpen)
        Globals::jobSystem.submit([this] { m_profilerPanel.prepare(); }, { "uiProfilerPrepare", EProfileCategory::UI }, EJobPriority::High, &m_prepareCounter);
    if (m_memoryOpen)
        Globals::jobSystem.submit([this] { m_memoryPanel.prepare(); }, { "uiMemoryPrepare", EProfileCategory::UI }, EJobPriority::High, &m_prepareCounter);
    if (m_logOpen)
        Globals::jobSystem.submit([this] { m_outputLog.prepare(); }, { "uiLogPrepare", EProfileCategory::UI }, EJobPriority::High, &m_prepareCounter);
    if (m_contentOpen) // the asset browser's filesystem rescans (it gates itself on focus/hover)
        Globals::jobSystem.submit([this] { m_assetBrowser.prepare(); }, { "uiContentPrepare", EProfileCategory::UI }, EJobPriority::High, &m_prepareCounter);
}

void UI::update(const oc::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec)
{
    {
        // Main thread, before the widget pass: the SDL3 backend queries the window and sets the OS
        // cursor / mouse capture - thread-affine to the window's owner. ImGui::NewFrame itself only
        // consumes the io state this writes, so it can run on the job (sequenced by the submit).
        ProfileScope scope("ImGui backend new frame", EProfileCategory::UI);
        ImGui_ImplSDL3_NewFrame();
    }
    // The widget pass rides the frame's POST-UPDATE batch: main kicks it right before present and
    // joins it at the top of the next frame, so it fills the present + fence-wait window. The two
    // references outlive that join (the world's root list, main's camera); deltaSec is copied.
    Globals::jobSystem.submitPostUpdate([this, &rootEntities, &camera, deltaSec] { updateJob(rootEntities, camera, deltaSec); },
        { "UI update", EProfileCategory::UI }, EJobPriority::Normal,
        EJobFlag_ForeignWait); // updateJob's first act waits on m_prepareCounter — see the flag's comment
}

void UI::flushMainThreadWork()
{
    // After the widget pass joined: callbacks and imports the panels deferred because they are
    // main-thread work (renderer/physics onChange effects, container imports creating GPU meshes).
    m_tweakPanel.flushDeferredCallbacks();
    m_entityEditor.flushContainerLoads();
}

void UI::updateJob(const oc::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec)
{
    {
        // The panel prepare jobs (see prepare()) must have landed before any panel renders — a
        // wait that shows up here means the prep did not fully overlap the pre-UI work.
        ProfileScope waitScope("UI prepare wait", EProfileCategory::Wait);
        Globals::jobSystem.wait(m_prepareCounter);
    }
    {
        ProfileScope scope("ImGui new frame", EProfileCategory::UI);
        ImGui::NewFrame();
    }

    if (m_mainMenu.isActive())
    {
        // Start screen instead of the editor: the 3D world renders into the FULL window (main
        // passes this rect to the renderer next frame) with the menu drawn over it. No panels run,
        // so their open flags stay false and no prepare jobs get submitted; the dockspace's
        // first-time layout runs on the first post-menu frame instead.
        ProfileScope menuScope("Main menu", EProfileCategory::UI);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        m_viewportRect = Rect(glm::ivec2((int)viewport->Pos.x, (int)viewport->Pos.y),
            glm::ivec2(int(viewport->Pos.x + viewport->Size.x), int(viewport->Pos.y + viewport->Size.y)));
        m_isViewportGrabbed = false;
        m_isViewportFocused = false;
        m_hasViewportGainedFocus = false;
        m_mainMenu.render(m_viewportRect, m_tweakPanel.deferredCallbacks());
        m_mainMenu.renderPausedBox(); // (no-op unless the shared pause is on)
        if (m_mainMenu.isEscapeOpen()) // reachable from the LOBBY page (its only leave mechanism)
            m_mainMenu.renderEscape(false);
        renderImGuiToSnapshot();
        return;
    }

    if (m_gameLayout)
    {
        // GAME LAYOUT: no editor panels. Optional LEFT debug section (escape menu checkbox), then
        // ONE fullscreen undecorated viewport window over the rest — a real ImGui window so the
        // HUD paints into its draw list and IsWindowFocused feeds the same viewport-focus gate the
        // editor's Viewport panel does (clicking into the debug section takes focus off the game,
        // clicking the world gives it back; the escape overlay takes it while open).
        ProfileScope layoutScope("Game layout", EProfileCategory::UI);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        m_logOpen = false;
        m_contentOpen = false;
        m_scriptEditorOpen = false;
        m_profilerOpen = false;
        m_memoryOpen = false;
        float viewportLeft = viewport->Pos.x;

        if (m_mainMenu.debugPanelsEnabled())
        {
            ProfileScope scope("Panel: Debug", EProfileCategory::UI);
            // Pinned to the left edge at full height; only the WIDTH is free (edge-drag resize).
            ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, viewport->Size.y), ImVec2(viewport->Size.x * 0.6f, viewport->Size.y));
            ImGui::SetNextWindowSize(ImVec2(460.0f, viewport->Size.y), ImGuiCond_FirstUseEver);
            const ImGuiWindowFlags debugFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar
                | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
            if (ImGui::Begin("##GameDebug", nullptr, debugFlags))
            {
                if (ImGui::BeginTabBar("##GameDebugTabs"))
                {
                    if (ImGui::BeginTabItem("Tweaks"))
                    {
                        ProfileScope tabScope("Panel: Tweaks", EProfileCategory::UI);
                        m_tweakPanel.render();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Profiler"))
                    {
                        ProfileScope tabScope("Panel: Profiler", EProfileCategory::UI);
                        m_profilerOpen = true; // prepare job next frame (see prepare())
                        m_profilerPanel.render();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Memory"))
                    {
                        ProfileScope tabScope("Panel: Memory", EProfileCategory::UI);
                        m_memoryOpen = true;
                        m_memoryPanel.render();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Log"))
                    {
                        ProfileScope tabScope("Panel: Log", EProfileCategory::UI);
                        m_logOpen = true;
                        m_outputLog.render();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            viewportLeft = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
            ImGui::End();
        }

        {
            ProfileScope scope("Panel: Viewport", EProfileCategory::UI);
            const ImVec2 vpPos(viewportLeft, viewport->Pos.y);
            const ImVec2 vpSize(viewport->Pos.x + viewport->Size.x - viewportLeft, viewport->Size.y);
            ImGui::SetNextWindowPos(vpPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(vpSize, ImGuiCond_Always);
            if (m_gameLayoutFocusPending)
            {
                m_gameLayoutFocusPending = false;
                ImGui::SetNextWindowFocus();
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            const ImGuiWindowFlags vpFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus
                | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
            const bool viewportOpen = ImGui::Begin("##GameViewport", nullptr, vpFlags);
            m_isViewportGrabbed = false;
            const bool isViewportFocused = viewportOpen && ImGui::IsWindowFocused();
            m_hasViewportGainedFocus = isViewportFocused && !m_isViewportFocused;
            m_isViewportFocused = isViewportFocused;
            m_viewportRect = Rect(glm::ivec2((int)vpPos.x, (int)vpPos.y), glm::ivec2(int(vpPos.x + vpSize.x), int(vpPos.y + vpSize.y)));
            if (viewportOpen)
            {
                ProfileScope hudScope("Game HUD overlay", EProfileCategory::UI);
                m_gameHudOverlay.render(m_viewportRect);
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        {
            // Text chat over the viewport's bottom-right corner (after the viewport window so it
            // layers above it; the escape overlay below still covers it). Ending a line refocuses
            // the viewport window, which re-arms the game's viewport-focus hotkey gate.
            ProfileScope scope("Game chat", EProfileCategory::UI);
            m_chat.renderOverlay(m_viewportRect, "##GameViewport");
        }

        if (m_gizmo) // no Scene panel = no selection; keeps the gizmo's own hide/follow logic ticking
        {
            ProfileScope scope("Gizmo update", EProfileCategory::UI);
            m_gizmo->update(camera, m_viewportRect, nullptr, deltaSec);
        }

        m_mainMenu.renderPausedBox(); // the shared pause's box, under the escape menu
        if (m_mainMenu.isEscapeOpen())
        {
            ProfileScope scope("Escape menu", EProfileCategory::UI);
            m_mainMenu.renderEscape(true);
        }
        renderImGuiToSnapshot();
        return;
    }

    {
        ProfileScope scope("Dockspace", EProfileCategory::UI);
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        const ImGuiWindowFlags rootWindowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
        ImGui::Begin("Root", nullptr, rootWindowFlags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("Root");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), 0);

        static bool first_time = true;
        if (first_time)
        {
            first_time = false;

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

            ImGuiID dock_id_right, dock_id_up, dock_id_left, dock_id_down;
            ImGuiID dock_id_left_top, dock_id_left_bottom;
            ImGuiID dock_id_scene, dock_id_properties;

            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.3f, &dock_id_left, &dock_id_right);
            ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.8f, &dock_id_up, &dock_id_down);
            ImGui::DockBuilderSplitNode(dock_id_left,  ImGuiDir_Up, 0.8f, &dock_id_left_top, &dock_id_left_bottom);
            ImGui::DockBuilderSplitNode(dock_id_left_top, ImGuiDir_Right, 0.5f, &dock_id_properties, &dock_id_scene);

            ImGui::DockBuilderDockWindow("Scene",      dock_id_scene);
            ImGui::DockBuilderDockWindow("Properties", dock_id_properties);
            ImGui::DockBuilderDockWindow("Stats",      dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Log",        dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Tweaks",     dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Profiler",   dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Memory",     dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Content",       dock_id_down);
            ImGui::DockBuilderDockWindow("Entity Editor",  dock_id_properties);
            ImGui::DockBuilderDockWindow("Text Editor",   dock_id_up);
            ImGui::DockBuilderDockWindow("Script Editor", dock_id_up);
            ImGui::DockBuilderDockWindow("Viewport",   dock_id_up);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Viewport", EProfileCategory::UI);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool viewportOpen = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoBackground);

        // Focus/grab tracking stays unconditional (not gated on viewportOpen below): if we skipped this
        // while the Viewport tab is in the background, m_isViewportFocused would stay stuck at whatever it
        // was on the last visible frame instead of dropping to false — e.g. camera-look input would keep
        // responding to mouse/keyboard after the user switched to another tab entirely.
        const ImGuiContext* ctx = ImGui::GetCurrentContext();
        const bool isViewportGrabbed = (ctx->MovingWindow == ctx->CurrentWindow);
        const bool wasViewportGrabbed = m_isViewportGrabbed && !isViewportGrabbed;
        m_isViewportGrabbed = isViewportGrabbed;
        const bool isViewportFocused = viewportOpen && ImGui::IsWindowFocused(ImGuiFocusedFlags_DockHierarchy) && !m_isViewportGrabbed && !wasViewportGrabbed;
        m_hasViewportGainedFocus = isViewportFocused && !m_isViewportFocused;
        m_isViewportFocused = isViewportFocused;

        if (viewportOpen)
        {
            const ImVec2 size = ImGui::GetContentRegionAvail();
            const ImVec2 viewportPos = ImGui::GetCursorScreenPos();
            m_viewportRect = Rect(glm::ivec2(viewportPos.x, viewportPos.y), glm::ivec2(viewportPos.x + size.x, viewportPos.y + size.y));

            const ImRect dropRect(viewportPos, ImVec2(viewportPos.x + size.x, viewportPos.y + size.y));
            if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##viewport_drop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
                {
                    const char* droppedPath = static_cast<const char*>(payload->Data);
                    const ImVec2 mouse = ImGui::GetMousePos();
                    m_viewportChanges.push_back({ EntityChange::CreateViewport{
                        glm::ivec2(int(mouse.x), int(mouse.y)), oc::string(droppedPath) } });
                }
                ImGui::EndDragDropTarget();
            }

            // In-game HUD, painted into this window's draw list so it stays clipped to the viewport
            // and never takes focus (pure drawing, no widgets).
            ProfileScope hudScope("Game HUD overlay", EProfileCategory::UI);
            m_gameHudOverlay.render(m_viewportRect);
        }

        ImGui::End();
        ImGui::PopStyleVar(1);
    }

    {
        ProfileScope scope("Panel: Text Editor", EProfileCategory::UI);
        if (ImGui::Begin("Text Editor"))
            m_textEditor.render();
        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Script Editor", EProfileCategory::UI);
        if (ImGui::Begin("Script Editor"))
        {
            m_scriptEditorOpen = true;
            m_scriptEditor.render();
        }
        else
            m_scriptEditorOpen = false;
        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Scene", EProfileCategory::UI);
        if (ImGui::Begin("Scene"))
            m_sceneView.render(rootEntities);
        ImGui::End();
    }

    // When the hierarchy selection changes to an entity carrying a .dsl script, make it active in the Script
    // Editor (the only script editor -- the node panel and its .scr routes are gone).
    if (Entity* selected = m_sceneView.getSelected(); selected != m_scriptSelectionTracked)
    {
        m_scriptSelectionTracked = selected;
        if (selected)
            if (ScriptComponent* script = getComponent<ScriptComponent>(selected))
                if (script->scriptModule)
                {
                    const oc::string& path = script->scriptModule->scriptPath;
                    if (FileSystem::extension(path) == ".dsl")
                        m_scriptEditor.requestOpen(path);
                }
    }

    // The three data-heavy panels remember whether they were open: only open ones get a prepare
    // job next frame (a hidden panel costs nothing, exactly as before).
    {
        ProfileScope scope("Panel: Log", EProfileCategory::UI);
        m_logOpen = ImGui::Begin("Log");
        if (m_logOpen)
            m_outputLog.render();
        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Profiler", EProfileCategory::UI);
        m_profilerOpen = ImGui::Begin("Profiler");
        if (m_profilerOpen)
            m_profilerPanel.render();
        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Memory", EProfileCategory::UI);
        m_memoryOpen = ImGui::Begin("Memory");
        if (m_memoryOpen)
            m_memoryPanel.render();
        ImGui::End();
    }

    ProfileScope statsScope("Panel: Stats", EProfileCategory::UI);
    if (ImGui::Begin("Stats"))
    {
        ImGui::Text("numMeshInstances: %i (%.1f%%)", m_renderStats.numMeshInstances, (float)m_renderStats.numMeshInstances / m_renderStats.maxMeshInstances * 100.0f);
        ImGui::Text("numInstanceOffsets: %i (%.1f%%)", m_renderStats.numInstanceOffsets, (float)m_renderStats.numInstanceOffsets / m_renderStats.maxInstanceOffsets * 100.0f);
        ImGui::Text("numMeshTypes: %i (%.1f%%)", m_renderStats.numMeshTypes, (float)m_renderStats.numMeshTypes / m_renderStats.maxMeshTypes * 100.0f);
        ImGui::Text("numMaterials: %i (%.1f%%)", m_renderStats.numMaterials, (float)m_renderStats.numMaterials / m_renderStats.maxMaterials * 100.0f);
        ImGui::Text("numRenderNodes: %i (%.1f%%)", m_renderStats.numRenderNodes, (float)m_renderStats.numRenderNodes / m_renderStats.maxRenderNodes * 100.0f);
		ImGui::Text("numTextures: %i (%.1f%%)", m_renderStats.numTextures, (float)m_renderStats.numTextures / m_renderStats.maxTextures * 100.0f);
        ImGui::Text("numObjectContainers: %i", m_renderStats.numObjectContainers);
        ImGui::Text("numLightGrids: %i (%.1f%%)", m_renderStats.numLightGrids, (float)m_renderStats.numLightGrids / m_renderStats.maxLightGrids * 100.0f);
        ImGui::Text("lightGridMemUsageBytes: %llu (%.1f%%)", m_renderStats.lightGridMemUsageBytes, (float)m_renderStats.lightGridMemUsageBytes / m_renderStats.maxLightGridMemUsageBytes * 100.0f);
		ImGui::Text("vertexDataUsedBytes: %llu (%.1f%%)", m_renderStats.vertexDataUsedBytes, (float)m_renderStats.vertexDataUsedBytes / m_renderStats.maxVertexDataBytes * 100.0f);
		ImGui::Text("indexDataUsedBytes: %llu (%.1f%%)", m_renderStats.indexDataUsedBytes, (float)m_renderStats.indexDataUsedBytes / m_renderStats.maxIndexDataBytes * 100.0f);
        ImGui::Separator();
        const float toMiB = 1.0f / (1024.0f * 1024.0f);
        if (m_renderStats.gpuMemoryBudgetBytes > 0)
            ImGui::Text("gpuMemoryUsed: %.1f MiB (%.1f%% of %.1f MiB budget)", m_renderStats.gpuMemoryUsedBytes * toMiB,
                (float)m_renderStats.gpuMemoryUsedBytes / m_renderStats.gpuMemoryBudgetBytes * 100.0f, m_renderStats.gpuMemoryBudgetBytes * toMiB);
        else
            ImGui::Text("gpuMemoryUsed: %.1f MiB", m_renderStats.gpuMemoryUsedBytes * toMiB);
        ImGui::Text("gpuMemoryReserved: %.1f MiB", m_renderStats.gpuMemoryReservedBytes * toMiB);
        ImGui::Separator();
        ImGui::Text("texStream resident: %.1f MiB (%.1f%% of %.1f MiB budget)", m_renderStats.textureResidentBytes * toMiB,
            m_renderStats.textureBudgetBytes > 0 ? (float)m_renderStats.textureResidentBytes / m_renderStats.textureBudgetBytes * 100.0f : 0.0f,
            m_renderStats.textureBudgetBytes * toMiB);
        ImGui::Text("texStream desired: %.1f MiB, tail: %.1f MiB, pinned: %.1f MiB", m_renderStats.textureDesiredBytes * toMiB,
            m_renderStats.textureTailBytes * toMiB, m_renderStats.texturePinnedBytes * toMiB);
        ImGui::Text("texStream streamable: %u, opsInFlight: %u", m_renderStats.numStreamableTextures, m_renderStats.numStreamOpsInFlight);
        ImGui::Text("meshStream resident: %.1f MiB (of %.1f MiB streamable, %.1f MiB budget)", m_renderStats.meshResidentBytes * toMiB,
            m_renderStats.meshStreamableBytes * toMiB, m_renderStats.meshBudgetBytes * toMiB);
        ImGui::Text("meshStream cold: %.1f MiB, sets: %u, evicted: %u", m_renderStats.meshColdBytes * toMiB,
            m_renderStats.numMeshSets, m_renderStats.numEvictedMeshSets);
        ImGui::Text("static BLAS: %.1f MiB (compaction saved %.1f MiB)", m_renderStats.blasBytes * toMiB,
            m_renderStats.blasCompactionSavedBytes * toMiB);
        ImGui::Text("meshLOD groups: %u, picks L0-L4 (debug builds): %u/%u/%u/%u/%u", m_renderStats.numMeshLodGroups,
            m_renderStats.lodInstanceCounts[0], m_renderStats.lodInstanceCounts[1], m_renderStats.lodInstanceCounts[2],
            m_renderStats.lodInstanceCounts[3], m_renderStats.lodInstanceCounts[4]);
    }
    ImGui::End();
    statsScope.stop();

    {
        ProfileScope scope("Panel: Content", EProfileCategory::UI);
        m_contentOpen = ImGui::Begin("Content");
        if (m_contentOpen)
            m_assetBrowser.render();
        ImGui::End();

        // Route script file actions from the asset browser (double-click / "New Script") into the Script
        // Editor. Only .dsl raises this request now -- a .scr reads as a plain text file (the node editor and
        // its whole integration are gone; the NodeEditor/ sources remain for reference only).
        if (oc::string openPath = m_assetBrowser.takeScriptOpenRequest(); !openPath.empty())
        {
            m_scriptEditor.requestOpen(openPath);
            // ...and surface the tab: an explicit "open this script" gesture should end with the editor in
            // front, even when the file was already the open document. By name, so it works whichever dock
            // tab currently covers it (applies on the next frame's Begin). The Scene-selection follow above
            // deliberately does NOT do this -- clicking entities must never yank the focused tab around.
            ImGui::SetWindowFocus("Script Editor");
        }

        // Route the asset browser's "Edit Entity" action into the Entity Editor.
        if (oc::string editPath = m_assetBrowser.takeEntityEditRequest(); !editPath.empty())
            m_entityEditor.requestOpen(editPath);

        // Route the asset browser's "Open Text File" action into the Text Editor.
        if (oc::string textPath = m_assetBrowser.takeTextOpenRequest(); !textPath.empty())
            m_textEditor.requestOpen(textPath);
    }

    {
        ProfileScope scope("Panel: Tweaks", EProfileCategory::UI);
        if (ImGui::Begin("Tweaks"))
            m_tweakPanel.render();
        ImGui::End();
    }

    {
        ProfileScope scope("Panel: Entity Editor", EProfileCategory::UI);
        if (ImGui::Begin("Entity Editor"))
        {
            m_entityEditor.render(m_sceneView.getSelected());

            if (ImGui::BeginDragDropTargetCustom(
                ImRect(ImGui::GetWindowPos(),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                           ImGui::GetWindowPos().y + ImGui::GetWindowSize().y)),
                ImGui::GetID("##ee_drop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
                    m_entityEditor.requestOpen(static_cast<const char*>(payload->Data));
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::End();

        // Route the entity editor's "Select Prefab" action into the asset browser.
        if (oc::string revealPath = m_entityEditor.takeRevealRequest(); !revealPath.empty())
            m_assetBrowser.selectFile(revealPath);
    }

    {
        ProfileScope scope("Panel: Properties", EProfileCategory::UI);
        if (ImGui::Begin("Properties"))
            m_propertiesPanel.render(m_sceneView.getSelected());
        ImGui::End();
    }

    // Last: the panels above settle this frame's viewport rect and selection, which is exactly what the
    // gizmo follows.
    if (m_gizmo)
    {
        ProfileScope scope("Gizmo update", EProfileCategory::UI);
        m_gizmo->update(camera, m_viewportRect, m_sceneView.getSelected(), deltaSec);
    }

    // Escape menu overlay (Esc in any running mode): drawn last, over the docked panels.
    m_mainMenu.renderPausedBox(); // the shared pause's box, under the escape menu
    if (m_mainMenu.isEscapeOpen())
    {
        ProfileScope scope("Escape menu", EProfileCategory::UI);
        m_mainMenu.renderEscape(false);
    }

    // The widget pass ends by producing its own draw data: ImGui::Render + the renderer-facing
    // snapshot run right here on the job (the context is ours until the join), so main's frame top
    // is just the join + flushMainThreadWork.
    renderImGuiToSnapshot();
}

void UI::handleKeyEvent(SDL_Event evt)
{
    if (m_scriptEditorOpen)
        m_scriptEditor.handleKeyEvent(evt);
}

// ImGui::GetDrawData() is only valid until the NEXT ImGui::NewFrame, so the widget-pass job ends
// by deep-copying the lists (CloneOutput) into a snapshot the context can't touch and pointing the
// renderer at it (read by the NEXT frame's present, sequenced by the post-update join). The atlas
// is NOT part of the copy - see the Textures line below. DOUBLE-BUFFERED:
// the job for frame N runs concurrently with present N, which is still recording from snapshot
// N-1 - so the job writes slot N&1 and frees only what that slot held from N-2, which present N-1
// finished with before this job was even kicked.
namespace
{
struct ImGuiSnapshot
{
    ImDrawData data;
    oc::vector<ImDrawList*> lists;
};
ImGuiSnapshot g_imguiSnapshots[2];
uint32 g_imguiSnapshotFlip = 0;
} // namespace

void UI::renderImGuiToSnapshot()
{
    ProfileScope profileScope("UI render", EProfileCategory::UI);
    ImGui::Render();

    ImGuiSnapshot& snapshot = g_imguiSnapshots[g_imguiSnapshotFlip & 1];
    ++g_imguiSnapshotFlip;
    for (ImDrawList* list : snapshot.lists)
        IM_DELETE(list);
    snapshot.lists.clear();

    const ImDrawData* src = ImGui::GetDrawData();
    snapshot.data = *src; // scalar fields + the CmdLists vector (entries replaced with clones below)
    // The one field a copy must NOT carry over: Textures points into the LIVE context
    // (ImGui::GetPlatformIO().Textures), which this pass keeps mutating while present records from
    // the snapshot. Null tells the backend to skip texture work entirely - main does the uploads via
    // Renderer::updateImGuiTextures() at the top of the next frame (see its comment).
    snapshot.data.Textures = nullptr;
    for (int i = 0; i < snapshot.data.CmdLists.Size; ++i)
    {
        ImDrawList* clone = src->CmdLists[i]->CloneOutput();
        snapshot.data.CmdLists[i] = clone;
        snapshot.lists.push_back(clone);
    }
    Globals::rendererVK.setImGuiDrawData(&snapshot.data);
}