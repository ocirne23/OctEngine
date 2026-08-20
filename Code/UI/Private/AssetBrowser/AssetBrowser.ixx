export module UI:AssetBrowser;

import Core;
import Entity;

export class AssetBrowser
{
public:

	AssetBrowser() {}
	void initialize();
	// Refresh stale directory LISTINGS (the panel's only real cost — filesystem enumeration).
	// No ImGui: UI::prepare runs it on a job, so the I/O overlaps the frame's other work.
	void prepare();
	void render();

	void selectFile(const oc::string& path); // navigate to the file's folder and select it

	const oc::string& getSelectedPath() const { return m_selectedPath; }
	bool hasSelection() const { return !m_selectedPath.empty(); }

	// Appends into the frame's merged drain (UI::takeEntityChanges); clear() keeps this panel's capacity.
	void takeChanges(oc::vector<EntityChange>& out)
	{
		out.insert(out.end(), oc::make_move_iterator(m_changes.begin()), oc::make_move_iterator(m_changes.end()));
		m_changes.clear();
	}

	// Script files: the UI drains this and routes by extension -- .dsl to the Script Editor, .scr to the node
	// panel. Covers open AND create ("New Script" writes a minimal .dsl and raises this same request).
	oc::string takeScriptOpenRequest()   { return oc::move(m_scriptOpenRequest); }

	// Text (.txt) files: the UI drains this and drives the Text Editor panel.
	oc::string takeTextOpenRequest() { return oc::move(m_textOpenRequest); }

	// .pre files: the UI drains this and drives the Entity Editor panel.
	oc::string takeEntityEditRequest() { return oc::move(m_entityEditRequest); }

private:

	// CACHED DIRECTORY LISTING. The browser used to enumerate the filesystem every frame — the
	// current folder once (plus a file_size syscall per row in list view) and, worse, the tree ran
	// TWO directory_iterator passes per expanded folder, recursively. That was ~1 ms/frame of pure
	// syscalls. Now each folder is scanned once and re-scanned at most every "RescanIntervalSec"
	// (files also appear from outside the browser: prefab saves, compiled scripts, cooked assets),
	// with the rescans running on the prepare job instead of the main thread — and ONLY while the
	// Content panel is focused or hovered (m_active): a browser sitting in a background tab has no
	// reason to touch the disk at all, and the first interaction re-scans what it draws.
	struct DirEntryInfo
	{
		oc::string path;
		oc::string name;      // filename
		oc::string extension;
		bool isDirectory = false;
		uint64 size = 0;       // files only, cached (list view drew this with a syscall per row)
		// Grid view's fitted caption: truncateLabel binary-searches ImGui::CalcTextSize with a
		// substring allocation per probe — far too much to redo per item per frame. Cached against
		// the width it was fitted to (the icon-size slider changes it).
		oc::string display;
		float displayWidth = -1.0f;
	};
	struct DirListing
	{
		oc::vector<DirEntryInfo> entries; // directories first, then by name — the draw order
		bool hasSubDirs = false;
		double lastScanSec = -1.0;
		uint64 touchedFrame = 0; // last frame a render actually read this listing
	};
	static constexpr double RescanIntervalSec = 1.0;

	DirListing& listing(const oc::string& dir); // cached; scans inline if unseen
	static void scanDirectory(const oc::string& dir, DirListing& out);
	void invalidateListings(); // after the browser itself creates/renames something

	void renderToolbar();
	void renderDirectoryTree(const oc::string& dir);
	void renderContentGrid();
	void renderContentList();
	void acceptPrefabDrop();
	void renderOverwritePopup();
	void renderCyclePopup();
	void renderRenamePopup();
	void renderNewAssetContextMenu();
	void queueSavePrefab(Entity* root, const oc::string& path);
	void renderContextMenu(const oc::string& p);
	oc::string makeUniqueAssetPath(const char* stem, const char* ext) const;
	void navigateTo(const oc::string& path);
	void navigateUp();

	bool isWithinRoot(const oc::string& path) const;

	oc::string m_rootPath;
	oc::string m_currentPath;
	oc::string m_selectedPath;

	oc::vector<EntityChange> m_changes;          // prefab saves queued for the app to drain
	oc::string           m_scriptOpenRequest;     // script the user asked to open/create (drained by UI)
	oc::string           m_textOpenRequest;       // .txt the user asked to open (drained by UI)
	oc::string           m_entityEditRequest;       // .pre the user asked to edit (drained by UI)
	EntityPtr             m_pendingSaveRoot;       // entity awaiting overwrite confirmation (kept alive)
	oc::string m_pendingSavePath;       // target .pre for the pending save
	bool                  m_openOverwritePopup = false;
	bool                  m_openCyclePopup     = false;

	oc::string m_renameTarget;          // file/folder awaiting rename
	bool                  m_openRenamePopup = false;
	char                  m_renameBuf[256] = {};

	oc::unordered_map<oc::string, DirListing> m_dirCache; // keyed by path string; node-stable refs
	uint64 m_frame = 0;
	bool m_active = false; // the Content window was focused or hovered last frame (gates rescans)

	char  m_searchBuf[256] = {};
	float m_leftPaneWidth  = 220.0f;
	float m_iconSize       = 72.0f;
	bool  m_listView       = false;
};