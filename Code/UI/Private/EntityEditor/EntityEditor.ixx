export module UI:EntityEditor;

import Core;
import Core.Rect;
import Core.Transform;
import Core.glm;
import Entity;
import RendererVK;
import :EntityDocument;

// Authors a .pre as a DOCUMENT OF PLAIN DATA (EntityDocNode), beside a preview that draws it into its own image. Nothing of the document is in the live world:
// opening a prefab spawns nothing, runs no script and creates no physics body. The world only sees a save (and, when asked, a respawn of its live instances).
export class EntityEditor
{
public:

	// sceneSelection: the main Scene panel's selection, for "Open Selected". viewportRect: where "Spawn in Scene" aims.
	void render(Entity* sceneSelection, const Rect& viewportRect);

	// Queues an open / new request, through the unsaved-changes guard. From the toolbar, a drag-drop of a .pre onto the panel, or the asset browser's "Edit Entity".
	void requestOpen(const oc::string& path);
	void requestNew(const oc::string& name);

	// Appends into the frame's merged drain (UI::takeEntityChanges); clear() keeps this panel's capacity.
	void takeChanges(oc::vector<EntityChange>& out)
	{
		if (m_changes.empty())
			return;
		out.insert(out.end(), oc::make_move_iterator(m_changes.begin()), oc::make_move_iterator(m_changes.end()));
		m_changes.clear();
	}

	oc::string takeRevealRequest() { return oc::move(m_revealRequest); }
	oc::string takeScriptOpenRequest() { return oc::move(m_scriptOpenRequest); }

	// MAIN THREAD (UI::flushMainThreadWork): what the off-main widget pass cannot do, because a template build or a container import creates renderer resources -
	// the document loads, the dropped prefab children and the containers the document names. Each lands a frame later.
	void flushMainThreadWork();

private:

	// ---- The document ----

	bool hasDocument() const { return m_root != nullptr; }
	EntityDocNode* selectedNode() { return m_root ? findDocNode(*m_root, m_selectedId) : nullptr; }
	void setDocument(EntityDocNode&& root, const oc::string& path);
	void closeDocument();
	oc::string documentId() const; // the file stem once saved / opened, else the root's name
	bool isDirty() const { return m_dirty; }

	// Every edit calls this. The undo step is taken when the edit ENDS (no widget active any more), so one drag or one typed name is one step.
	void markEdited() { m_editPending = true; }
	void commitPendingEdit();
	void undo();
	void redo();

	// ---- Open, save, close (guarded) ----

	enum class PendingSwitch { None, New, OpenPath, OpenSelected, Close };
	void requestOpenSelected(Entity* entity);
	void requestClose();
	void runSwitch(PendingSwitch what);
	void doOpen(const oc::string& path);
	void doNew(const oc::string& name);
	void doOpenSelected();
	void trySave(const oc::string& path); // overwrite-confirms when the target exists, refuses a prefab cycle
	void queueSave(const oc::string& path);
	void revert();

	// ---- Tree edits ----

	void addChild(uint64 parentId);
	void removeNode(uint64 id);
	void duplicateNode(uint64 id);
	void copyNode(uint64 id);
	void pasteNode(uint64 parentId);
	void moveNode(uint64 id, int delta); // among its siblings
	void reparentNode(uint64 id, uint64 newParentId);
	void unpackNode(uint64 id);
	void requestPrefabChild(uint64 parentId, const oc::string& path);

	// ---- The panel ----

	void handleShortcuts();
	void renderToolbar(const Rect& viewportRect);
	void renderOpenPopup();
	void renderEmptyState();
	void renderPrefabList(float height);
	void renderOverwritePopup();
	void renderUnsavedPopup();
	void renderSaveAsPopup();
	void renderMessagePopup();
	void renderTree();
	void renderTreeNode(EntityDocNode& node);
	void renderProperties();
	void renderProblems(const EntityDocNode& node);
	void renderIdentity(EntityDocNode& node);
	void renderReference(EntityDocNode& node);
	void renderAddComponent(EntityDocNode& node);
	void renderComponents(EntityDocNode& node);

	void renderSceneSection(EntityDocNode& node);
	void renderRenderSection(EntityDocNode& node);
	void renderAnimatorSection(EntityDocNode& node);
	void renderPhysicsSection(EntityDocNode& node);
	void renderAudioSection(EntityDocNode& node);
	void renderParticleSection(EntityDocNode& node);
	void renderForceSection(EntityDocNode& node);
	void renderLightSection(EntityDocNode& node);
	void renderNetworkSection(EntityDocNode& node);
	void renderGameUnitSection(EntityDocNode& node);
	void renderGameStructureSection(EntityDocNode& node);
	void renderGameProjectileSection(EntityDocNode& node);
	void renderHumanoidSection(EntityDocNode& node);
	void renderScriptSection(EntityDocNode& node);
	// The INITIAL values of the picked script's exposed ScriptData fields -- serialized into the .pre and applied at every spawn, unlike the Properties panel's
	// live edits of the same fields.
	void renderScriptDataSection(EntityDocNode& node);

	// ---- The preview (EntityEditorPreview.cpp) ----

	struct PreviewPick
	{
		uint64 nodeId;
		glm::vec3 center;
		float radius;
	};
	struct PreviewWorld
	{
		uint64 nodeId;
		Transform world;
	};
	void renderPreview();
	void renderPreviewToolbar(const glm::vec2& imageMin, uint32 meshCount);
	void buildPreviewScene();
	void appendPreviewNode(const EntityDocNode& node, const Transform& parentWorld, bool isRoot, const glm::vec3& parentTint, uint64 pickId, bool highlighted);
	void appendPreviewRender(const RenderComponent::SpawnInfo& render, const Transform& world, const glm::vec3& tint, uint64 pickId, bool highlighted);
	void drawPreviewOverlays(const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageMax);
	void drawNodeGizmos(const EntityDocNode& node, const Transform& world, const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageMax);
	void frameCamera();
	void needContainer(const oc::string& name);

	oc::vector<oc::string> m_containerLoadRequests;

	// ---- State ----

	oc::unique_ptr<EntityDocNode> m_root;
	uint64 m_selectedId = 0;
	uint64 m_nextId = 1;
	oc::string m_path;          // empty until first save / open
	oc::string m_baselineText;  // the text at the last load / save, for the dirty flag
	bool m_dirty = false;
	bool m_editPending = false;
	bool m_updateInstancesOnSave = true;

	EntityDocNode m_committed;                // the document as of the last finished edit
	oc::vector<EntityDocNode> m_undoStack;
	oc::vector<EntityDocNode> m_redoStack;
	oc::unique_ptr<EntityDocNode> m_nodeClipboard;

	Entity* m_sceneSelection = nullptr;
	oc::vector<EntityChange> m_changes;
	oc::string m_revealRequest;
	oc::string m_scriptOpenRequest;

	// Main-thread work, see flushMainThreadWork. A document load carries the file's TEXT, read here on the worker.
	struct PendingDocLoad
	{
		oc::string path;
		oc::string text;
		bool keepView = false; // a revert: same camera
	};
	struct PendingChildLoad
	{
		uint64 parentId;
		oc::string path;
	};
	oc::vector<PendingDocLoad> m_docLoads;
	oc::vector<PendingChildLoad> m_childLoads;

	PendingSwitch m_pendingSwitch = PendingSwitch::None;
	oc::string m_pendingSwitchPath;
	oc::string m_pendingSwitchName;
	EntityPtr m_pendingSwitchEntity;
	bool m_openUnsavedPopup = false;
	bool m_openOverwritePopup = false;
	bool m_openSaveAsPopup = false;
	bool m_openOpenPopup = false;
	oc::string m_pendingSavePath;
	char m_saveAsBuf[256] = {};
	oc::string m_message; // a refusal the user must see: a prefab cycle, a load failure
	bool m_openMessagePopup = false;

	char m_prefabSearch[128] = {};
	oc::vector<oc::string> m_prefabNames; // sorted, refreshed when the list opens
	bool m_prefabNamesDirty = true;

	char m_renderPickerSearch[128] = {};
	char m_renderNodePickerSearch[128] = {};
	char m_animatorPickerSearch[128] = {};
	char m_filePickerSearch[128] = {};
	oc::vector<oc::string> m_filePickerFiles; // refreshed when a file picker opens
	char m_physCollidesWithBuf[256] = {};
	uint64 m_physCollidesWithNode = 0;

	// Euler angles are kept as typed: re-deriving them from the quaternion each frame makes a drag flip at the gimbal.
	struct EulerEdit
	{
		glm::quat quat{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 degrees{ 0.0f };
	};
	EulerEdit m_eulerNode;
	EulerEdit m_eulerRender;
	bool rotationField(const char* label, glm::quat& quat, EulerEdit& edit);

	uint64 m_renameNodeId = 0;
	bool m_renameFocusPending = false;
	char m_renameBuf[128] = {};
	oc::function<void()> m_deferredTreeAction; // a structural edit, run after the tree walk that asked for it
	oc::vector<oc::string> m_problems;         // scratch: the selected node's, then each tree row's
	bool m_previewDragged = false;             // the press on the image moved: a release is no pick

	// The preview camera orbits m_cameraTarget. Y up, like the world.
	glm::vec3 m_cameraTarget{ 0.0f };
	float m_cameraYaw = 0.65f;
	float m_cameraPitch = 0.35f;
	float m_cameraDistance = 5.0f;
	static constexpr float c_cameraFovY = 0.7f;
	bool m_framePending = true;       // frame the document as soon as it has bounds (its containers may still be loading)
	bool m_showGrid = true;
	bool m_showWireframe = false;
	bool m_showAxes = true;
	bool m_showBounds = false;
	bool m_showPhysics = true;
	bool m_showLights = true;
	bool m_showForce = true;
	bool m_overlaysAllNodes = false;  // gizmos of every node, not only the selected one
	bool m_highlightSelected = true;
	float m_gridY = 0.0f;

	EntityPreviewFrame m_previewFrame;
	oc::vector<PreviewPick> m_previewPicks;
	oc::vector<PreviewWorld> m_previewWorlds;
	glm::vec3 m_previewBoundsCenter{ 0.0f };
	float m_previewBoundsRadius = 0.0f;
};
