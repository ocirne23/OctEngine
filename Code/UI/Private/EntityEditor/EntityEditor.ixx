export module UI:EntityEditor;

import Core;
import Core.Transform;
import Entity;

// Panel for defining an entity's components (Render/Animator/Physics/Audio/Script) and saving/loading it
// as a .pre file, instead of hand-editing the text. The entity being edited is spawned as a real root
// entity in the running world (like everything else in this engine) and unpacked so it's freely editable.
//
// Components are inline-allocated in one buffer sized by typeBits at entity-creation time — there is no
// live add/remove/resize API. So adding/removing a component, or committing a field edit, assembles a
// fresh EntitySpawnTemplate from the entity's current component set and respawns it in place (see
// commitRespawn()). This reuses the exact machinery Save/Load already rely on (World's buildXSpawnInfo
// resolvers), just driven directly instead of through a file.
export class EntityEditor
{
public:

	// sceneSelection is the main Scene panel's current selection (not this editor's own tree selection),
	// used only to gate/drive the "Open Selected" toolbar button.
	void render(Entity* sceneSelection);

	// Called (via UI) once main.cpp has fulfilled an OpenPrefabForEdit/NewPrefab EntityChange and the
	// entity actually exists in the world's root list.
	void onOpened(EntityPtr root, const oc::string& path);

	// Called once main.cpp has fulfilled a RespawnEntity change (a component was added/removed/edited).
	void onRespawned(EntityPtr oldEntity, EntityPtr newEntity);

	// Queues an open/new request, going through the unsaved-changes guard first. Safe to call from the
	// toolbar, a drag-drop of ASSET_FILE onto the panel, or the asset browser's "Edit Entity" action.
	void requestOpen(const oc::string& path);
	void requestNew(const oc::string& name);

	oc::vector<EntityChange> takeChanges() { return oc::move(m_changes); }

	// "Select Prefab" toolbar button: the UI drains this and selects the .pre in the asset browser.
	oc::string takeRevealRequest() { return oc::move(m_revealRequest); }
	// Containers the render section needed but that were not loaded yet: the widget pass runs off
	// the main thread and an import creates renderer resources (main-thread work), so render()
	// only queues the name and UI::flushMainThreadWork calls this on main - the node list fills in
	// a frame later.
	void flushContainerLoads();

private:

	void renderToolbar();
	void renderOverwritePopup();
	void renderUnsavedPopup();
	void renderTree();
	void renderTreeNode(Entity* node);
	void renderNameAndTransform();
	void renderComponents();
	void renderSceneSection();
	void renderRenderSection();
	void renderAnimatorSection();
	void renderPhysicsSection();
	void renderAudioSection();
	void renderParticleSection();
	void renderForceSection();
	void renderLightSection();
	void renderNetworkSection();
	void renderScriptSection();
	// The INITIAL values of the picked script's exposed ScriptData fields -- serialized into the .pre and
	// applied at every spawn, unlike the Properties panel's live edits of the same fields.
	void renderScriptDataSection();

	// Editing an already-live scene entity in place (via "Open Selected") vs. a dedicated one this editor
	// spawned itself (New / Open by path) — only the latter gets deleted when switching away/closing.
	void requestOpenSelected(Entity* entity);
	void requestClose();
	void doSwitchOpenSelected(EntityPtr entity);
	void doClose();

	void doSwitchOpen(const oc::string& path);
	void doSwitchNew(const oc::string& name);
	void closeCurrent(); // detaches m_editRoot bookkeeping; deletes it only if this editor owns it

	void trySave(const oc::string& path);   // overwrite-confirms if the target file already exists
	void queueSave(const oc::string& path);

	bool isDirty() const;
	void rebaseline();
	oc::string serializeDocText() const; // document text; Sync off swaps in the transform draft for m_selected
	oc::string currentId() const; // filename stem once saved/opened, else the root's display name

	void selectEntity(EntityPtr entity); // switches which entity Name/Transform/Components edits
	void addChild();                     // creates a blank inline child under m_selected (name box)
	void removeChild(Entity* child);

	void refreshDraftsFromEntity(); // re-reads every component's current SpawnInfo from m_selected

	// Assembles a fresh EntitySpawnTemplate from the current draft state and queues a RespawnEntity for
	// m_selected (not necessarily the document root — any node in the tree can be edited/respawned).
	void commitRespawn();

	// Pushes the drafts straight into m_selected's LIVE components, no respawn. Covers only what can be
	// re-applied in place (render local transform, force emitter, lights); anything baked at spawn
	// (physics bodies, animator graphs, script data) is untouched and still needs the respawn.
	void applyDraftsLive();
	// Standard binding for a draft drag widget: `commitDrag(ImGui::DragFloat(...))`. Live-applies on
	// every change frame so the viewport tracks the drag, and respawns once on release.
	void commitDrag(bool changed);

	EntityPtr   m_editRoot; // the document's root entity
	EntityPtr   m_selected; // entity currently shown in Name/Transform/Components (root or a descendant)
	oc::string m_path; // empty until first save/open

	Entity* m_sceneSelection = nullptr; // this frame's main Scene panel selection, set at the top of render()
	bool    m_ownsEntity     = true;    // false when editing an existing scene entity in place ("Open Selected")
	bool    m_wasPacked      = false;   // was m_editRoot a locked prefab instance before we unpacked it to edit?

	oc::string m_baselineText; // serialized text at last load/save, for dirty-tracking

	bool      m_syncTransform = false; // Transform floats mirror/edit the live entity; off = detached draft that saves as-is
	Transform m_transformDraft;        // m_selected's transform as shown in the panel, refreshed on selection change

	oc::vector<EntityChange> m_changes;
	oc::string m_revealRequest; // .pre to select in the asset browser (drained by UI)
	oc::vector<oc::string> m_containerLoadRequests; // see takeContainerLoadRequests

	char m_pathBuf[256] = "Entities/NewEntity.pre";
	char m_nameBuf[256] = {};

	enum class PendingSwitch { None, New, OpenPath, OpenSelected, Close };
	PendingSwitch m_pendingSwitch = PendingSwitch::None;
	oc::string   m_pendingSwitchPath;
	oc::string   m_pendingSwitchName;
	EntityPtr     m_pendingSwitchEntity;
	bool          m_openUnsavedPopup = false;

	bool        m_openOverwritePopup = false;
	oc::string m_pendingSavePath;

	// --- Component presence + draft state, for m_selected. Drafts hold the real SpawnInfo types directly
	// (they're fully visible here since Entity.ixx exports :Component) so widgets bind straight to their
	// fields; commit (Add/Remove, or a field's IsItemDeactivatedAfterEdit) calls commitRespawn(). ---
	bool m_hasScene    = false;
	bool m_hasRender   = false;
	bool m_hasAnimator = false;
	bool m_hasPhysics  = false;
	bool m_hasAudio    = false;
	bool m_hasParticle = false;
	bool m_hasForce    = false;
	bool m_hasLight    = false;
	bool m_hasNetwork  = false;
	bool m_hasScript   = false;

	RenderComponent::SpawnInfo   m_renderDraft;
	AnimatorComponent::SpawnInfo m_animatorDraft;
	PhysicsComponent::SpawnInfo  m_physicsDraft;
	AudioComponent::SpawnInfo    m_audioDraft;
	ParticleComponent::SpawnInfo m_particleDraft;
	ForceComponent::SpawnInfo    m_forceDraft;
	LightComponent::SpawnInfo    m_lightDraft;
	NetworkComponent::SpawnInfo  m_networkDraft;
	ScriptComponent::SpawnInfo   m_scriptDraft;

	char m_particleEffectBuf[256] = {}; // edit buffer for m_particleDraft.effectPath

	char m_physCollidesWithBuf[256] = {}; // comma-joined display/edit buffer for physicsDraft.collidesWith

	char m_renderPickerSearch[128]     = {}; // container (.oc) name picker
	char m_renderNodePickerSearch[128] = {}; // node picker, scoped to the currently chosen container
	char m_animatorPickerSearch[128]  = {};
	char m_scriptPickerSearch[128]    = {};
	oc::vector<oc::string> m_scriptPickerFiles; // .scr paths relative to Assets/, refreshed when the picker opens

	char m_newChildNameBuf[128] = "Entity";
	Entity* m_pendingRemoveChild = nullptr; // set while walking the tree, applied after (avoids mutating mid-walk)

	float m_treeHeight = 60.0f; // user-resizable via the drag handle below the hierarchy tree
};
