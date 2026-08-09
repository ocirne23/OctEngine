export module UI;

import Core;
import Core.Rect;
import Core.Camera;
import Core.glm;
import Core.SDL;
import Entity;
import UI.Gizmo;

import UI.fwd;
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
    void update(const std::vector<EntityPtr>& rootEntities, const Camera& camera, double deltaSec);
    void render();
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

    std::vector<EntityChange> takeEntityChanges()
    {
        std::vector<EntityChange> changes = m_sceneView.takeChanges();
        changes.insert(changes.end(), std::make_move_iterator(m_viewportChanges.begin()),
                                      std::make_move_iterator(m_viewportChanges.end()));
        m_viewportChanges.clear();

        std::vector<EntityChange> assetChanges = m_assetBrowser.takeChanges();
        changes.insert(changes.end(), std::make_move_iterator(assetChanges.begin()),
                                      std::make_move_iterator(assetChanges.end()));

        std::vector<EntityChange> entityEditorChanges = m_entityEditor.takeChanges();
        changes.insert(changes.end(), std::make_move_iterator(entityEditorChanges.begin()),
                                      std::make_move_iterator(entityEditorChanges.end()));
        return changes;
    }

    // Forwards the entity main.cpp just spawned/respawned for an EntityEditor request into the panel.
    void onOpened(EntityPtr root, const std::string& path) { m_entityEditor.onOpened(root, path); }
    void onEntityRespawned(EntityPtr oldEntity, EntityPtr newEntity) { m_entityEditor.onRespawned(oldEntity, newEntity); }

    // Script paths the DSL Script Editor asked the host to (re)compile + hot-reload this frame -- every path it
    // just saved (a save always writes fresh generated C++, so it's ready to (re)compile the moment it lands).
    std::vector<std::string> takeScriptReloadRequests()
    {
        return m_scriptEditor.takeReloadRequests();
    }

    void handleKeyEvent(SDL_Event evt);

private:

    bool m_isViewportGrabbed = false;
    bool m_isViewportFocused = false;
    bool m_hasViewportGainedFocus = false;
    bool m_scriptEditorOpen = false;
    Rect m_viewportRect = Rect();
    std::vector<EntityChange> m_viewportChanges;   // assets dropped onto the viewport, drained via takeEntityChanges

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
};

export namespace Globals
{
// The FIRST engine global to destruct (see InitSeg.h) — the panels hold EntityPtrs (Scene selection,
// Entity Editor document, pending prefab save) and EntityChange queues, released while everything an
// entity destructor touches is still alive.
OC_INIT_SEG(OC_SEG_UI)
    UI ui;
}
