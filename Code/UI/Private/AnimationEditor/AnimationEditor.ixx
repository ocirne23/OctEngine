export module UI:AnimationEditor;

import Core;
import Core.glm;
import Core.imgui;
import Animation;
import Entity;
import RendererVK;

// THE ANIMATION EDITOR: authors a .clip (keyframes by bone name) on a preview rig, in the style of Unity's Animation window - a timeline with a dopesheet and
// a curve view, record mode (a pose edit while recording keys it at the playhead), a bone gizmo in its own preview, events, and an FBX / .anm importer.
// Like the Entity Editor, the document is DATA: nothing of it is in the world until a save, which goes to World as EntityChange::SaveAnimationClip and
// hot-swaps the clip into every live animator that plays it.
export class AnimationEditor
{
public:

	void render(double deltaSec);

	// A .clip opens (through the unsaved-changes guard). A model file or an .anm opens the import dialog on it.
	void requestOpen(const oc::string& path);
	void requestImport(const oc::string& path);

	// Appends into the frame's merged drain (UI::takeEntityChanges); clear() keeps this panel's capacity.
	void takeChanges(oc::vector<EntityChange>& out)
	{
		if (m_changes.empty())
			return;
		out.insert(out.end(), oc::make_move_iterator(m_changes.begin()), oc::make_move_iterator(m_changes.end()));
		m_changes.clear();
	}
	oc::string takeRevealRequest() { return oc::move(m_revealRequest); }

	// MAIN THREAD (UI::flushMainThreadWork): the rig import (an ObjectContainer creates GPU resources) and the FBX imports (Assimp).
	void flushMainThreadWork();

private:

	// ---- The document ----

	bool hasDocument() const { return m_hasDocument; }
	oc::string documentName() const;
	void setDocument(ClipAsset&& clip, const oc::string& path);
	void closeDocument();
	void markEdited() { m_editPending = true; ++m_docVersion; }
	void commitPendingEdit();
	void undo();
	void redo();
	bool isDirty() const { return m_dirty || m_editPending; }

	enum class PendingSwitch : uint8 { None, New, Open, Close, Import };
	void guardedSwitch(PendingSwitch what);
	void runSwitch(PendingSwitch what);
	void doOpen(const oc::string& path);
	void doNew();
	void trySave(const oc::string& path);
	void queueSave(const oc::string& path);

	// ---- Rig ----

	void setRig(const oc::string& containerName);
	void resolveRig();          // the container is loaded (lookup only, this is the worker): the skeleton and the player
	void rebuildRuntimeClip();  // the document -> the runtime clip on the rig, when either changed
	void needContainer(const oc::string& name);
	const Skeleton* skeleton() const { return m_skeleton; }

	// ---- Keys ----

	struct KeyRef
	{
		uint32 track;
		ClipChannel channel;
		float time;
	};
	// A key's identity: track, channel and the exact time bits, so a selection survives every edit that does not move the key (and is decodable back).
	static uint64 keyId(uint32 track, ClipChannel channel, float time);
	static KeyRef keyFromId(uint64 id);
	bool isKeySelected(uint32 track, ClipChannel channel, float time) const { return m_keySelection.count(keyId(track, channel, time)) != 0; }
	void selectKey(uint32 track, ClipChannel channel, float time, bool select);
	void clearKeySelection() { m_keySelection.clear(); }
	void collectKeysAt(int32 rowIdx, float time, oc::vector<KeyRef>& out) const; // every key a row's diamond at `time` stands for
	void collectSelectedKeys(oc::vector<KeyRef>& out) const;
	int32 ensureTrack(const oc::string& bone);
	void setKeyVec3(uint32 track, ClipChannel channel, float time, const glm::vec3& value);
	void setKeyQuat(uint32 track, float time, const glm::quat& value);
	void deleteSelectedKeys();
	void deleteEvent(int32 index);
	void deleteTrack(int32 track);
	void moveSelectedKeys(float deltaSeconds);
	void setSelectedKeysInterp(KeyInterp interp);
	void copySelectedKeys();
	void pasteKeys(float atTime);
	void keyBone(int32 bone, bool position, bool rotation, bool scale); // keys the bone's current pose at the playhead
	void keySelectedBones();
	float snapTime(float t) const;
	float gridStep() const { return m_clip.step(); }             // seconds per key step
	int32 stepIndex(float t) const { return (int32)glm::round(t / gridStep()); }
	// A time past the end grows the clip by whole steps (the step stays, the step count grows); the events keep their seconds.
	void extendTo(float t);
	float latestContent() const; // the time of the last key or event
	// After a drag or a delete: if the end held a key or an event before (latestBefore) and holds none now, the clip shrinks by whole steps to
	// its new last key or event (one step when nothing is left). A clip whose end was empty keeps its length.
	void shrinkToContent(float latestBefore);

	// The bone's pose at the playhead as the preview shows it: an unkeyed edit (the override), else the sampled pose.
	void currentLocalPose(int32 bone, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) const;
	// A pose edit from the gizmo or the inspector. Recording: keys it at the playhead. Not recording: an override, shown until the time changes (K keys it).
	void editBonePose(int32 bone, ClipChannel channel, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale);
	void clearOverrides() { m_overrides.clear(); }

	// ---- Clip operations ----

	// scaleKeys: the keys and the step scale with the length (the step count stays). Otherwise the step stays: the clip is trimmed or extended by
	// whole steps.
	void setClipDuration(float seconds, bool scaleKeys);
	void setSteps(uint32 steps); // the length stays, the step changes; the keys keep their times
	void reduceKeys();
	void matchLoopPose();
	void removeUnboundTracks();

	// ---- Playback ----

	void setTime(float t);
	void stepFrames(int32 frames);
	void jumpKey(int32 direction); // the next / previous key of the selected bone (all keys when none)
	void tickPlayback(float dt);

	// ---- The panel ----

	void handleShortcuts();
	void renderToolbar();
	void renderEmptyState();
	void renderInspector();
	void renderBoneInspector(int32 bone);
	void renderKeyInspector();
	void renderEventInspector();
	void renderClipSettings();
	void renderPopups();
	void renderImportPopup();
	void renderSaveAsPopup();
	void renderUnsavedPopup();
	void renderOpenPopup();
	void renderReducePopup();
	bool rotationField(const char* label, glm::quat& quat);

	// ---- The track list + timeline (AnimationEditorTimeline.cpp) ----

	enum class RowKind : uint8 { Bone, Channel };
	struct Row
	{
		RowKind kind;
		uint8 depth;
		ClipChannel channel;
		bool expandable;
		int32 bone;   // skeleton index, -1 = no rig / not in the rig
		int32 track;  // the clip track, -1 = the bone has none
	};
	void buildRows();
	void renderTimelineArea();
	void renderTrackList(float width, float height);
	void renderTimelineHeader(const ImVec2& origin, float width);
	void renderDopesheet(const ImVec2& origin, float width, float height);
	void renderCurves(const ImVec2& origin, float width, float height);
	// The press / drag / release logic of a lane of diamonds (rowIdx = c_summaryRow for the summary lane), shared by the dopesheet and the summary lane.
	void handleKeyPress(int32 rowIdx, float keyTime, bool hitKey);
	void handleKeyDrag(bool released);
	void zoomTime(float wheel, float mouseX);
	static constexpr int32 c_summaryRow = -2;
	void renderTimelineContextMenu();
	float timeToX(float t) const { return m_timelineX0 + (t - m_viewStart) * m_pixelsPerSecond; }
	float xToTime(float x) const { return m_viewStart + (x - m_timelineX0) / m_pixelsPerSecond; }
	void frameAll(float width);
	void rowTimes(const Row& row, oc::vector<float>& out) const; // the sorted, unique key times a row shows
	// One diamond of a lane: the keys at one time, how many of them are selected, and (one channel) their blend mode. Sorted by time, one pass.
	struct Diamond
	{
		float time;
		uint32 count;
		uint32 selected;
		KeyInterp interp;
	};
	void gatherDiamonds(int32 rowIdx, oc::vector<Diamond>& out) const; // rowIdx = c_summaryRow: every key of the clip

	// ---- The preview (AnimationEditorPreview.cpp) ----

	void renderPreview(float width, float height);
	void renderPreviewToolbar(const glm::vec2& imageMin);
	void drawSkeleton(const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageSize);
	bool updateGizmo(const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec2& imageMin, const glm::vec2& imageSize, bool pressed, bool held);
	void frameCamera();

	// ---- State ----

	bool m_hasDocument = false;
	ClipAsset m_clip;
	oc::string m_path;          // empty until first save / open
	oc::string m_baselineText;  // the text at the last load / save, for the dirty flag
	bool m_dirty = false;
	bool m_editPending = false;
	uint32 m_docVersion = 1;
	ClipAsset m_committed;
	oc::vector<ClipAsset> m_undoStack;
	oc::vector<ClipAsset> m_redoStack;
	oc::vector<EntityChange> m_changes;
	oc::string m_revealRequest;

	PendingSwitch m_pendingSwitch = PendingSwitch::None;
	oc::string m_pendingPath;
	bool m_openUnsavedPopup = false;
	bool m_openSaveAsPopup = false;
	bool m_openOverwritePopup = false;
	bool m_openOpenPopup = false;
	bool m_openImportPopup = false;
	bool m_openReducePopup = false;
	oc::string m_pendingSavePath;
	char m_saveAsName[128] = {};
	char m_saveAsFolder[192] = "Animations";
	char m_openSearch[128] = {};
	oc::string m_message;       // shown in the status line (red) for a while
	float m_messageTimer = 0.0f;
	bool m_messageIsError = false;
	void showMessage(oc::string text, bool error) { m_message = oc::move(text); m_messageTimer = 6.0f; m_messageIsError = error; }

	// Rig
	oc::string m_rigName;
	ObjectContainer* m_rigContainer = nullptr;
	const Skeleton* m_skeleton = nullptr;
	oc::vector<oc::string> m_containerLoadRequests;
	char m_rigSearch[96] = {};
	AnimationPlayer m_player;
	AnimationClip m_runtimeClip;
	uint32 m_builtVersion = 0;
	const Skeleton* m_builtSkeleton = nullptr;
	oc::vector<int32> m_boneTrack;          // per skeleton bone: its track, -1 = none
	oc::vector<uint8> m_boneSubtreeAnimated; // per skeleton bone: it or a descendant has a track
	oc::vector<uint8> m_boneIsHelper;        // per skeleton bone: an FBX pivot helper / the scene root (drawn dim, no joint)

	// Playback
	float m_time = 0.0f;
	bool m_playing = false;
	float m_playSpeed = 1.0f;
	bool m_recording = false;
	bool m_snap = true;
	bool m_showSeconds = false;
	bool m_lengthScalesKeys = true; // the Length field scales the keys with it (else it trims / extends by whole steps)
	KeyInterp m_newKeyInterp = KeyInterp::Smooth; // what a key made here blends with (imported keys stay Linear)

	struct PoseOverride
	{
		glm::vec3 pos{ 0.0f };
		glm::quat rot{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 scale{ 1.0f };
		uint8 channels = 0; // bit per ClipChannel that was edited
	};
	oc::unordered_map<int32, PoseOverride> m_overrides;

	// Selection
	int32 m_selectedBone = -1;          // skeleton index (with a rig)
	int32 m_selectedTrack = -1;         // the track row picked without a rig / outside it
	oc::unordered_set<uint64> m_keySelection; // keyId
	int32 m_selectedEvent = -1;
	oc::unordered_set<oc::string> m_expandedBones; // bones whose channel rows show
	oc::unordered_set<uint64> m_curveRows;         // track << 2 | channel: the channels the curve view draws

	struct ClipboardKey
	{
		oc::string bone;
		ClipChannel channel;
		float offset;      // seconds after the earliest copied key
		glm::vec4 value;   // xyz, or a quaternion xyzw
		KeyInterp interp;
	};
	oc::vector<ClipboardKey> m_clipboard;

	// Track list + timeline view
	oc::vector<Row> m_rows;
	char m_filter[96] = {};
	bool m_animatedOnly = true;
	bool m_curveView = false;
	float m_viewStart = -0.05f;
	float m_pixelsPerSecond = 200.0f;
	bool m_frameAllPending = true;
	float m_listWidth = 280.0f;
	float m_topFraction = 0.58f;
	float m_inspectorWidth = 300.0f;
	float m_rowScroll = 0.0f;          // the track list's scroll; the dopesheet draws its rows at the same offset
	float m_pendingListScroll = -1.0f; // a scroll the dopesheet asked of the track list (its wheel), applied next frame
	float m_timelineX0 = 0.0f;     // screen x of time 0 - m_viewStart, refreshed every frame
	float m_dopesheetTop = 0.0f;   // screen y of row 0 at scroll 0 (the box select maps rows through it)
	float m_curveMin = -1.0f, m_curveMax = 1.0f;
	bool m_curveFitPending = true;

	enum class DragMode : uint8 { None, Scrub, MoveKeys, BoxSelect, Pan, MoveEvent, CurveKey };
	DragMode m_drag = DragMode::None;
	ImVec2 m_dragStart{ 0.0f, 0.0f };
	float m_dragAnchorTime = 0.0f;
	float m_dragDelta = 0.0f;        // snapped seconds the selection moves by on release
	float m_dragViewStart = 0.0f;
	bool m_dragMoved = false;
	KeyRef m_curveDragKey{ 0, ClipChannel::Position, 0.0f };
	int32 m_curveDragComponent = 0;
	float m_contextTime = 0.0f;
	int32 m_contextRow = -1;

	// Preview camera: orbits m_cameraTarget, Y up like the world
	glm::vec3 m_cameraTarget{ 0.0f, 1.0f, 0.0f };
	float m_cameraYaw = 0.6f;
	float m_cameraPitch = 0.25f;
	float m_cameraDistance = 4.0f;
	bool m_framePending = true;
	bool m_previewDragged = false;
	bool m_showGrid = true;
	bool m_showBones = true;
	bool m_showMesh = true;
	bool m_showWireframe = false;
	glm::vec3 m_boundsCenter{ 0.0f, 1.0f, 0.0f };
	float m_boundsRadius = 1.0f;
	EntityPreviewFrame m_previewFrame;

	// Gizmo
	enum class GizmoMode : uint8 { Rotate, Move };
	GizmoMode m_gizmoMode = GizmoMode::Rotate;
	bool m_gizmoLocal = true;
	int32 m_gizmoHover = -1;   // axis 0..2
	int32 m_gizmoActive = -1;
	glm::vec3 m_gizmoAxisWorld{ 0.0f };  // the dragged axis as it was at the press (the bone turns under it)
	glm::vec3 m_gizmoStartVector{ 0.0f };
	float m_gizmoStartAxisParam = 0.0f;
	glm::vec3 m_gizmoStartPos{ 0.0f };
	glm::quat m_gizmoStartRot{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 m_gizmoStartScale{ 1.0f };
	glm::vec3 m_gizmoStartOrigin{ 0.0f };
	ImVec2 m_gizmoStartMouse{ 0.0f, 0.0f };

	// Inspector: Euler angles as typed (re-deriving them per frame makes a drag flip at the gimbal)
	glm::quat m_eulerQuat{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 m_eulerDegrees{ 0.0f };
	char m_eventNameBuf[96] = {};
	int32 m_eventNameFor = -1;

	// Import dialog
	struct ImportJob
	{
		oc::string source;      // the model file
		oc::string name;        // the clip name (the file stem); several kept tracks get _<n>
		oc::string skip;
		oc::string track;
		oc::string folder;      // relative to Assets/
		oc::string rig;
		bool reduce = true;
		ClipReduceTolerance tolerance;
		bool fromAnm = false;   // loop + events from the .anm entry of `name`
		bool loop = true;
		oc::vector<AnimationEventKey> events;
		bool openAfter = false;
	};
	oc::vector<ImportJob> m_importJobs;
	bool m_importFromAnm = false;
	char m_importSearch[128] = {};
	oc::string m_importSource;
	oc::unordered_set<oc::string> m_importAnmPicked;
	char m_importSkip[64] = {};
	char m_importFolder[192] = "Animations";
	bool m_importReduce = true;
	ClipReduceTolerance m_tolerance;
	oc::vector<oc::string> m_modelFiles;   // refreshed when the dialog opens
	oc::vector<oc::string> m_clipFiles;    // refreshed when the Open list opens
};
