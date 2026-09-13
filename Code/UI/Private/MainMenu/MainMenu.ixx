export module UI:MainMenu;

import Core;
import Core.glm;
import Core.Rect;
import Core.Tweaks;
import :ChatPanel;

// The game-facing start screen, shown when App.exe launches without a mode (no --game/--server/
// --connect and not an automated run). Rendered by the widget pass INSTEAD of the editor panels
// while active (see UI::updateJob); the world renders full-window behind it. The menu itself only
// records the player's choice - main() polls takeAction() after the post-update join and performs
// the actual mode start (network host/join, world content, GameMatch), so all side effects stay on
// the main thread in the pre-kick window.
export struct MainMenuAction
{
	enum class EType : uint8
	{
		None,
		StartCoop,    // --game --coop equivalent
		StartPvp,     // --game
		StartSandbox, // the engine testbed (no game)
		Quit,
	};
	EType type = EType::None;
	bool host = false;         // start as a listen server (windowed --server)
	oc::string connectAddress; // !host: "ip[:port]" to join; empty = offline single player
};

// ---- Lobby (pre-game screen) ----
// The local lobby's WORLD block. The preview image is the Procedural terrain preview's output, copied
// ONCE per generation by main into this UI-side type (UI cannot import Procedural) and shared by
// pointer - the page re-uploads it into its ImGui texture when the generation changes.
export struct LobbyPreviewImage
{
	uint32 width = 0;
	uint32 height = 0;
	uint32 generation = 0;
	oc::vector<uint32> rgba; // RGBA8, row-major, row 0 = most negative Z
	float extentKm = 0.0f;   // the world span the map covers (both axes), for the caption
	float tileWorldSizeM = 0.0f; // one full-detail tile's side in world metres (the seeding unit)
};

export struct LobbyWorldView
{
	enum class EPreview : uint8 { Idle, LoadingModels, Generating, Ready, Failed };
	uint32 terrainSeed = 0;
	EPreview preview = EPreview::Idle;
	float previewProgress = 0.0f;       // Generating: rows done / rows
	oc::string previewStatus;           // LoadingModels / Failed: the generator's status line
	oc::shared_ptr<const LobbyPreviewImage> image; // null until a preview is ready
	// The seeded world: Seed world pressed, the playable area's diffusion TILES generate around
	// the pick (the terrain mesh itself streams live) until settled.
	bool seeded = false;
	bool seededSettled = false;
	float seededProgress = 0.0f;        // tiles done / tiles total
	oc::string seededStatus;            // "Loading terrain models...", "Tiles 12 / 64 (20 on disk)"
	glm::vec2 seededPick = glm::vec2(0.5f); // the pick the world was seeded at (the page's marker)
	// The generated tiles' footprint on the map (normalized 0..1, valid when seededAreaValid): the
	// square the page draws.
	bool seededAreaValid = false;
	glm::vec2 seededAreaMin = glm::vec2(0.0f);
	glm::vec2 seededAreaMax = glm::vec2(0.0f);
};

// The UI-facing SNAPSHOT of the lobby: Game's LobbySystem (the model - roster, ready flags,
// countdown, all networked server-authoritatively) rebuilds it every frame on the main thread
// (setLobbyView, between the widget-pass join and kick); the widget pass only draws it and
// records a LobbyAction for main to poll - the same sequencing as MainMenuAction.
export struct LobbyView
{
	struct Player
	{
		uint32 clientId = 0;
		uint8 team = 0; // PvP pick (0-based; the page shows it 1-based)
		bool ready = false;
		bool isSelf = false;
	};
	oc::vector<Player> players;
	bool valid = false;   // client: false until the first lobby-state event arrived ("Connecting...")
	bool hosting = false; // show the host-endpoint block (what other clients dial)
	bool coop = false;
	bool localReady = false;
	uint8 localTeam = 0;
	int numTeams = 2;     // PvP: the host's "Number of teams" (2..8); 1 in co-op (no team picking)
	int pvpMap = 0;       // PvP: the host's arena pick, an index into pvpMapNames
	oc::vector<const char*> pvpMapNames; // the Game layer's arena names (string literals)
	bool allReady = false;
	bool countdownActive = false;
	float countdownRemaining = 0.0f;
	// Co-op MAP settings (host-chosen, mirrored to every client through the lobby state so the
	// page shows the upcoming map; the values that actually generate ride the GMp event at start).
	uint32 mapSeed = 0;        // 0 = the host rolls a random one at start
	float terrainFill = 0.3f;
	int terrainLanes = 6;
	// LOCAL (offline) lobby: no roster, no ready check - Start launches at once - and the page
	// carries the WORLD block: a terrain seed, its preview map and the "Seed world" pick (below).
	bool local = false;
	LobbyWorldView world;
};

export struct LobbyAction
{
	// Leave: back to the main menu (main tears the session down - the host's leave ends it for
	// every client, a client's leave is just its disconnect)
	enum class EType : uint8
	{
		None, ToggleReady, Start, SetMapSettings, SetTeam, SetNumTeams, SetPvpMap, Leave,
		GeneratePreview, // local: preview the terrain seed (terrainSeed)
		SeedWorld,       // local: build the full-detail world at the picked spot (pickU/pickV)
		UnseedWorld,     // local: drop the seeded world (back to the flat default)
	};
	EType type = EType::None;
	uint32 mapSeed = 0; // SetMapSettings (host only): the edited values
	float terrainFill = 0.3f;
	int terrainLanes = 6;
	uint8 team = 0;     // SetTeam: the local player's pick (0-based)
	int numTeams = 2;   // SetNumTeams (host only)
	int pvpMap = 0;     // SetPvpMap (host only): index into LobbyView::pvpMapNames
	uint32 terrainSeed = 0; // GeneratePreview
	float pickU = 0.5f;     // SeedWorld: the picked spot on the preview (0..1, u right, v down)
	float pickV = 0.5f;
	float areaSizeM = 1024.0f; // SeedWorld: the playable area's side, world metres
};

// ---- Escape menu (Esc overlay in every RUNNING mode + the lobby; never over the main menu) ----
// Pause = the escape menu's "Pause game" button (a SHARED game pause - every player sees the paused
// box); Unpause = the paused box's "Resume" button (any player may press it).
export enum class EscapeMenuAction : uint8 { None, Resume, ExitToMenu, Quit, Pause, Unpause };

export class MainMenu final
{
public:

	void setActive(bool active)
	{
		m_active = active;
		if (active) // (re)activation always lands on the FRONT page - a previous session's stale
		{           // lobby/settings page must not greet the next one (exit-to-menu reactivates)
			m_settingsOpen = false;
			m_lobbyOpen = false;
		}
	}
	bool isActive() const { return m_active; }
	bool isLobbyOpen() const { return m_active && m_lobbyOpen; }

	// Shown in place of the address field while "Host a server" is checked. main seeds it with the
	// LAN address (netGetLocalAddress() + the launch port) and upgrades it to the EXTERNAL IP once
	// the background lookup (netGetExternalAddress on its own thread) lands.
	void setHostEndpoint(oc::string endpoint) { m_hostEndpoint = oc::move(endpoint); }
	// Dim status line under the endpoint: "Resolving external IP...", then the LAN address (or the
	// lookup failure). Empty = no line.
	void setHostNote(oc::string note) { m_hostNote = oc::move(note); }
	// Status line on the FRONT page (e.g. "Disconnected from the server"); cleared by the next
	// mode pick. Empty = no line.
	void setStatus(oc::string status) { m_status = oc::move(status); }

	// Main thread, after the widget-pass join (the action was written by the PREVIOUS frame's
	// widget pass - same sequencing as every panel queue). Returns None when nothing was clicked.
	MainMenuAction takeAction()
	{
		MainMenuAction action = oc::move(m_action);
		m_action = MainMenuAction{};
		return action;
	}

	// ---- Lobby page ----
	void openLobby()
	{
		m_lobbyOpen = true;
		m_settingsOpen = false;
		m_terrainSeedEditInit = false; // the world block's widgets start from the session's state
		m_previewPicked = false;
		m_mapZoom = 1.0f;
		m_mapCenter = glm::vec2(0.5f);
		m_mapPressed = false;
	}
	// UI's chat widget, drawn embedded in the lobby page (non-owning; set once by UI's ctor).
	void setChatPanel(ChatPanel* chat) { m_chat = chat; }
	void setLobbyView(const LobbyView& view) { m_lobbyView = view; } // main thread, every frame while open
	LobbyAction takeLobbyAction()
	{
		const LobbyAction action = m_lobbyAction;
		m_lobbyAction = LobbyAction{};
		return action;
	}

	// ---- Escape menu ----
	// State written by MAIN only (Esc toggle, action application); the widget pass renders the
	// overlay while open and records the button press for main to poll - the usual sequencing.
	void setEscapeOpen(bool open)
	{
		if (open && !m_escapeOpen)
			m_escapeFocusPending = true; // bring the overlay pair to the front ONCE (see renderEscape)
		m_escapeOpen = open;
	}
	bool isEscapeOpen() const { return m_escapeOpen; }
	EscapeMenuAction takeEscapeAction()
	{
		const EscapeMenuAction action = m_escapeAction;
		m_escapeAction = EscapeMenuAction::None;
		return action;
	}
	// Widget pass, over the docked panels / the game layout / the lobby page. offerDebugToggle
	// adds the "Debug panels" and "Pause profiler" checkboxes (game layout only - the editor
	// already has every panel).
	void renderEscape(bool offerDebugToggle);
	// The SHARED GAME PAUSE (main writes both, every frame): offersPause adds the escape menu's
	// "Pause game" button (a running game only); paused shows the centered PAUSED box with its
	// "Resume" button over everything (any player may resume - the game syncs the state).
	void setEscapeOffersPause(bool offers) { m_escapeOffersPause = offers; }
	void setGamePaused(bool paused) { m_gamePaused = paused; }
	bool isGamePaused() const { return m_gamePaused; }
	void renderPausedBox(); // widget pass; no-op unless paused
	// The escape menu's "Debug panels" checkbox: while set, the GAME layout shows the left-side
	// Tweaks/Profiler/Memory section (see UI::updateJob). Written by the widget pass, read by it.
	bool debugPanelsEnabled() const { return m_debugPanels; }

	// Widget pass only. fullRect = the whole window (the menu centers itself in it); changed
	// settings vars with onChange callbacks are collected into deferredCallbacks (the TweakPanel's
	// list, flushed on main by UI::flushMainThreadWork).
	void render(const Rect& fullRect, oc::vector<const TweakVar*>& deferredCallbacks);

private:

	void renderMain();
	void renderSettings(oc::vector<const TweakVar*>& deferredCallbacks);
	void renderLobby();
	void renderLobbyWorld(); // the local lobby's right-hand column: seed, preview map, Seed world
	void uploadPreviewImage(const LobbyPreviewImage& image); // (re)fills the ImGui texture

	bool m_active = false;
	bool m_settingsOpen = false;
	bool m_lobbyOpen = false;
	bool m_host = false;
	int m_settingsSection = 0;
	char m_address[64] = "";   // client address input, "ip[:port]"
	oc::string m_hostEndpoint; // what the host displays ("203.0.113.7:27888" once external resolved)
	oc::string m_hostNote;     // dim line under it (LAN address / lookup status)
	oc::string m_status;       // front-page status line, see setStatus
	MainMenuAction m_action;
	ChatPanel* m_chat = nullptr; // see setChatPanel
	LobbyView m_lobbyView;
	LobbyAction m_lobbyAction;
	// Host's map-setting widgets: local edit copies that track the view snapshot while no widget
	// is active, committed as ONE SetMapSettings on release (not per drag frame).
	int m_mapEditSeed = 0;
	float m_mapEditFill = 0.3f;
	int m_mapEditLanes = 6;
	bool m_mapEditActive = false;
	// The world block's widgets: the seed field (an edit copy; the view's seed is what was last
	// previewed), the pick on the map (page-local until Seed world sends it), and which image
	// generation the ImGui texture currently holds (0 = no texture yet).
	int m_terrainSeedEdit = 1337;
	bool m_terrainSeedEditInit = false;
	bool m_previewPicked = false;
	glm::vec2 m_previewPick = glm::vec2(0.5f);
	uint32 m_previewUploadedGeneration = 0;
	int m_areaSizeIdx = 1; // index into the page's playable-area size table (512 m, 1 km, 2 km, 4 km, 8 km)
	// The map's view: zoom (1 = the whole image) and the map-space centre; wheel zooms about the
	// cursor, a left drag pans, a click picks, double-click resets.
	float m_mapZoom = 1.0f;
	glm::vec2 m_mapCenter = glm::vec2(0.5f);
	bool m_mapPressed = false; // left button went down over the map
	bool m_mapDragged = false; // ...and moved past the drag threshold (so the release is not a pick)
	int m_teamsEdit = 2; // host's "Number of teams" slider, committed on release (SetNumTeams)
	bool m_teamsEditActive = false;
	bool m_escapeOpen = false;
	bool m_escapeFocusPending = false; // focus the overlay windows on the OPENING frame only
	bool m_debugPanels = false;        // "Debug panels" checkbox (game layout's left-side section)
	bool m_escapeOffersPause = false;  // a game runs: the escape menu shows "Pause game"
	bool m_gamePaused = false;         // the shared pause is on: the PAUSED box shows
	EscapeMenuAction m_escapeAction = EscapeMenuAction::None;
};
