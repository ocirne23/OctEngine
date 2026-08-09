module UI;

import Core;
import Core.imgui;
import Core.Log;
import Core.glm;
import Core.SDL;
import Entity;

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

void UI::initialize()
{
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

void UI::update(const std::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec)
{
    ProfileScope profileScope("UI update", EProfileCategory::UI);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    {
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
                        glm::ivec2(int(mouse.x), int(mouse.y)), std::string(droppedPath) } });
                }
                ImGui::EndDragDropTarget();
            }

            // In-game HUD, painted into this window's draw list so it stays clipped to the viewport
            // and never takes focus (pure drawing, no widgets).
            m_gameHudOverlay.render(m_viewportRect);
        }

        ImGui::End();
        ImGui::PopStyleVar(1);
    }

    {
        if (ImGui::Begin("Text Editor"))
            m_textEditor.render();
        ImGui::End();
    }

    {
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
                    const std::string& path = script->scriptModule->scriptPath;
                    if (std::filesystem::path(path).extension() == ".dsl")
                        m_scriptEditor.requestOpen(path);
                }
    }

    {
        if (ImGui::Begin("Log"))
            m_outputLog.render();
        ImGui::End();
    }

    {
        if (ImGui::Begin("Profiler"))
            m_profilerPanel.render();
        ImGui::End();
    }

    {
        if (ImGui::Begin("Memory"))
            m_memoryPanel.render();
        ImGui::End();
    }

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
        ImGui::Text("meshLOD groups: %u, picks L0-L4: %u/%u/%u/%u/%u", m_renderStats.numMeshLodGroups,
            m_renderStats.lodInstanceCounts[0], m_renderStats.lodInstanceCounts[1], m_renderStats.lodInstanceCounts[2],
            m_renderStats.lodInstanceCounts[3], m_renderStats.lodInstanceCounts[4]);
    }
    ImGui::End();

    {
        if (ImGui::Begin("Content"))
            m_assetBrowser.render();
        ImGui::End();

        // Route script file actions from the asset browser (double-click / "New Script") into the Script
        // Editor. Only .dsl raises this request now -- a .scr reads as a plain text file (the node editor and
        // its whole integration are gone; the NodeEditor/ sources remain for reference only).
        if (std::string openPath = m_assetBrowser.takeScriptOpenRequest(); !openPath.empty())
        {
            m_scriptEditor.requestOpen(openPath);
            // ...and surface the tab: an explicit "open this script" gesture should end with the editor in
            // front, even when the file was already the open document. By name, so it works whichever dock
            // tab currently covers it (applies on the next frame's Begin). The Scene-selection follow above
            // deliberately does NOT do this -- clicking entities must never yank the focused tab around.
            ImGui::SetWindowFocus("Script Editor");
        }

        // Route the asset browser's "Edit Entity" action into the Entity Editor.
        if (std::string editPath = m_assetBrowser.takeEntityEditRequest(); !editPath.empty())
            m_entityEditor.requestOpen(editPath);

        // Route the asset browser's "Open Text File" action into the Text Editor.
        if (std::string textPath = m_assetBrowser.takeTextOpenRequest(); !textPath.empty())
            m_textEditor.requestOpen(textPath);
    }

    {
        if (ImGui::Begin("Tweaks"))
            m_tweakPanel.render();
        ImGui::End();
    }

    {
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
        if (std::string revealPath = m_entityEditor.takeRevealRequest(); !revealPath.empty())
            m_assetBrowser.selectFile(revealPath);
    }

    {
        if (ImGui::Begin("Properties"))
            m_propertiesPanel.render(m_sceneView.getSelected());
        ImGui::End();
    }

    // Last: the panels above settle this frame's viewport rect and selection, which is exactly what the
    // gizmo follows.
    if (m_gizmo)
        m_gizmo->update(camera, m_viewportRect, m_sceneView.getSelected(), deltaSec);
}

void UI::handleKeyEvent(SDL_Event evt)
{
    if (m_scriptEditorOpen)
        m_scriptEditor.handleKeyEvent(evt);
}

void UI::render()
{
    ProfileScope profileScope("UI render", EProfileCategory::UI);
    ImGui::Render();
}