export module UI;

import Core;
import Core.Rect;
import Core.Camera;
import Core.glm;
import Core.SDL;
import Entity;
import Threading; // JobCounter for the panel prepare jobs
import UI.Gizmo;

import UI.fwd;
export import :MainMenu; // MainMenuAction crosses to main(), which performs the mode start
import :AssetBrowser;
import :SceneView;
import :PropertiesPanel;
import :EntityEditor;
import :OutputLog;
import :TweakPanel;
import :ProfilerPanel;
import :MemoryPanel;
import :TextEditor;
import :GameHudOverlay;
import Script;
import :ScriptEditor;

import RendererVK;

export class UI final
{
public:

    UI() {}
    UI(const UI&) = delete;

    void initialize();
    // TWO PHASES per frame. ImGui itself is single-context/single-thread, so the widget pass
    // (update) stays serial — but the panels' DATA work (profiler ring snapshot + per-track sort,
    // memory treemap snapshot, log snapshot + filter) touches no ImGui and runs on JOBS. Call
    // prepare() as early in the frame as possible (main.cpp: right after input.update, before the
    // camera/game/controls work) — the jobs overlap with everything up to update(), which waits on
    // them first thing. Only panels that were OPEN last frame prepare, and every panel prepares
    // inline in its render if nothing ran ahead, so prepare() is an optimization, never required.
    void prepare();
    void renderImGuiToSnapshot(); // end of the widget-pass job: ImGui::Render + double-buffered snapshot
    // MAIN THREAD ONLY, once per frame, right before JobSystem::kickPostUpdateJobs(): does the SDL
    // backend half of the ImGui frame here (ImGui_ImplSDL3_NewFrame talks to SDL - window size,
    // cursor, mouse capture - and those calls keep their affinity to the thread that owns the window
    // and its message pump), then QUEUES the widget pass as a post-update job. The arguments are
    // captured for it: rootEntities and camera BY REFERENCE, so they must outlive the join.
    void update(const oc::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec);
    // The widget pass itself - the body of the job update() queues, not called directly. Kicked
    // just before present and joined at the TOP of the next frame, so it runs during present,
    // profiler.endFrame and the fence/vsync wait, when main mutates nothing the panels read and the
    // workers are otherwise idle. Everything it produces is consumed after the join: the draw-data
    // snapshot by the next frame's present (double-buffered, so present may overlap the job writing
    // the other slot), the EntityChange/reload queues by the drains, the deferred work below by
    // flushMainThreadWork.
    void updateJob(const oc::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec);
    // Main thread, after the widget pass joined: work panels collected but must not run on a
    // worker - tweak onChange callbacks and the Entity Editor's container imports.
    void flushMainThreadWork();
	void setRenderStats(const Stats& stats) { m_renderStats = stats; }

    Entity* getSelectedEntity() const { return m_sceneView.getSelected(); }

    // ---- Transform gizmo ----
    // Driven from here; the app owns the storage (the implementation lives in Input, below UI, so it
    // arrives as an IGizmo). NON-OWNING - the GizmoController detaches itself here in its destructor.
    // update() drives it from the panel's own viewport rect + selection; the gizmo ENTITY has to tick
    // with the rest of the scene, hence the separate call the app makes at that point in the frame
    // (it needs the renderer, which UI::update has no business holding).
    void setGizmo(IGizmo* gizmo) { m_gizmo = gizmo; }
    IGizmo* getGizmo() const { return m_gizmo; }
    void drawGizmoEntity(Renderer& renderer, float deltaSec);

    bool isViewportGrabbed() const { return m_isViewportGrabbed; }
    bool isViewportFocused() const { return m_isViewportFocused; }
    // True while the Script Editor panel (or any child of it) holds ImGui focus -- see ScriptEditor::hasFocus.
    // Input.cpp's global-shortcut gate checks this the same way it already checks WantCaptureKeyboard/WantTextInput.
    bool isScriptEditorFocused() const { return m_scriptEditorOpen && m_scriptEditor.hasFocus(); }
    bool hasViewportGainedFocused() const { return m_hasViewportGainedFocus; }
    const Rect& getViewportRect() const { return m_viewportRect; }

    // Merged drain of every panel's queued changes into ONE vector: each source appends and keeps its
    // own capacity (see the panels' takeChanges). The steady-state all-empty frame builds nothing —
    // empty-range inserts touch no storage, and the returned vector never allocates.
    oc::vector<EntityChange> takeEntityChanges()
    {
        oc::vector<EntityChange> changes;
        m_sceneView.takeChanges(changes);
        if (!m_viewportChanges.empty())
        {
            changes.insert(changes.end(), oc::make_move_iterator(m_viewportChanges.begin()), oc::make_move_iterator(m_viewportChanges.end()));
            m_viewportChanges.clear();
        }
        m_assetBrowser.takeChanges(changes);
        m_entityEditor.takeChanges(changes);
        m_propertiesPanel.takeChanges(changes);
        return changes;
    }

    // Forwards the entity main.cpp just spawned/respawned for an EntityEditor request into the panel.
    void onOpened(EntityPtr root, const oc::string& path) { m_entityEditor.onOpened(root, path); }
    void onEntityRespawned(EntityPtr oldEntity, EntityPtr newEntity) { m_entityEditor.onRespawned(oldEntity, newEntity); }

    // Script paths the DSL Script Editor asked the host to (re)compile + hot-reload this frame -- every path it
    // just saved (a save always writes fresh generated C++, so it's ready to (re)compile the moment it lands).
    oc::vector<oc::string> takeScriptReloadRequests()
    {
        return m_scriptEditor.takeReloadRequests();
    }

    void handleKeyEvent(SDL_Event evt);

    // ---- Main menu (game-facing start screen; main performs the mode start) ----
    // While active, the widget pass draws ONLY the menu (fullscreen viewport, no editor panels).
    // main() polls takeMainMenuAction() after the post-update join and deactivates on success.
    void setMainMenuActive(bool active) { m_mainMenu.setActive(active); }
    bool isMainMenuActive() const { return m_mainMenu.isActive(); }
    void setMainMenuHostEndpoint(oc::string endpoint) { m_mainMenu.setHostEndpoint(oc::move(endpoint)); }
    void setMainMenuHostNote(oc::string note) { m_mainMenu.setHostNote(oc::move(note)); }
    MainMenuAction takeMainMenuAction() { return m_mainMenu.takeAction(); }
    // Lobby page (App's LobbySystem is the model — main pushes a view snapshot each frame and
    // polls the button actions, same sequencing as the menu action above)
    void openMainMenuLobby() { m_mainMenu.openLobby(); }
    bool isMainMenuLobbyOpen() const { return m_mainMenu.isLobbyOpen(); }
    void setMainMenuLobbyView(const LobbyView& view) { m_mainMenu.setLobbyView(view); }
    LobbyAction takeMainMenuLobbyAction() { return m_mainMenu.takeLobbyAction(); }
    // Escape menu (Esc overlay in every running mode + the lobby; main owns the open state and
    // applies the polled action — Resume/ExitToMenu/Quit)
    void setEscapeMenuOpen(bool open) { m_mainMenu.setEscapeOpen(open); }
    bool isEscapeMenuOpen() const { return m_mainMenu.isEscapeOpen(); }
    EscapeMenuAction takeEscapeMenuAction() { return m_mainMenu.takeEscapeAction(); }

private:

    bool m_isViewportGrabbed = false;
    bool m_isViewportFocused = false;
    bool m_hasViewportGainedFocus = false;
    bool m_scriptEditorOpen = false;
    // prepare-phase gating: which data-heavy panels were open last frame (ImGui::Begin result),
    // and the counter update() waits on before ImGui::NewFrame
    bool m_profilerOpen = false;
    bool m_memoryOpen = false;
    bool m_logOpen = false;
    bool m_contentOpen = false;
    JobCounter m_prepareCounter;
    Rect m_viewportRect = Rect();
    oc::vector<EntityChange> m_viewportChanges;   // assets dropped onto the viewport, drained via takeEntityChanges

    // Follow the hierarchy selection into the Script Editor: when a selected entity carries a .dsl script, open
    // it. Only ever acts on selection CHANGES, hence the tracking pointer.
    Entity*     m_scriptSelectionTracked = nullptr;

	IGizmo* m_gizmo = nullptr; // non-owning, see setGizmo
	Stats m_renderStats;
	AssetBrowser    m_assetBrowser;
	SceneView       m_sceneView;
	PropertiesPanel m_propertiesPanel;
	EntityEditor    m_entityEditor;
	OutputLog       m_outputLog;
	TweakPanel      m_tweakPanel;
	ProfilerPanel   m_profilerPanel;
	MemoryPanel     m_memoryPanel;
	TextEditor      m_textEditor;
	ScriptEditor    m_scriptEditor;
	GameHudOverlay  m_gameHudOverlay; // in-game HUD painted over the viewport (Core.GameHud is the model)
	MainMenu        m_mainMenu;
};

export namespace Globals
{
// The FIRST engine global to destruct (see InitSeg.h) — the panels hold EntityPtrs (Scene selection,
// Entity Editor document, pending prefab save) and EntityChange queues, released while everything an
// entity destructor touches is still alive.
OC_INIT_SEG(OC_SEG_UI)
    UI ui;
}
