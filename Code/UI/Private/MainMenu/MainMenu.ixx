export module UI:MainMenu;

import Core;
import Core.Rect;
import Core.Tweaks;

// The game-facing start screen, shown when App.exe launches without a mode (no --game/--server/
// --connect and not an automated run). Rendered by the widget pass INSTEAD of the editor panels
// while active (see UI::updateJob); the world renders full-window behind it. The menu itself only
// records the player's choice — main() polls takeAction() after the post-update join and performs
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

// ---- Lobby (multiplayer pre-game screen) ----
// The UI-facing SNAPSHOT of the lobby: App's LobbySystem (the model — roster, ready flags,
// countdown, all networked server-authoritatively) rebuilds it every frame on the main thread
// (setLobbyView, between the widget-pass join and kick); the widget pass only draws it and
// records a LobbyAction for main to poll — the same sequencing as MainMenuAction.
export struct LobbyView
{
	struct Player
	{
		uint32 clientId = 0;
		bool ready = false;
		bool isSelf = false;
	};
	oc::vector<Player> players;
	bool valid = false;   // client: false until the first lobby-state event arrived ("Connecting...")
	bool hosting = false; // show the host-endpoint block (what other clients dial)
	bool coop = false;
	bool localReady = false;
	bool allReady = false;
	bool countdownActive = false;
	float countdownRemaining = 0.0f;
};

export struct LobbyAction
{
	enum class EType : uint8 { None, ToggleReady, Start };
	EType type = EType::None;
};

// ---- Escape menu (Esc overlay in every RUNNING mode + the lobby; never over the main menu) ----
export enum class EscapeMenuAction : uint8 { None, Resume, ExitToMenu, Quit };

export class MainMenu final
{
public:

	void setActive(bool active)
	{
		m_active = active;
		if (active) // (re)activation always lands on the FRONT page — a previous session's stale
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

	// Main thread, after the widget-pass join (the action was written by the PREVIOUS frame's
	// widget pass — same sequencing as every panel queue). Returns None when nothing was clicked.
	MainMenuAction takeAction()
	{
		MainMenuAction action = oc::move(m_action);
		m_action = MainMenuAction{};
		return action;
	}

	// ---- Lobby page ----
	void openLobby() { m_lobbyOpen = true; m_settingsOpen = false; }
	void setLobbyView(const LobbyView& view) { m_lobbyView = view; } // main thread, every frame while open
	LobbyAction takeLobbyAction()
	{
		const LobbyAction action = m_lobbyAction;
		m_lobbyAction = LobbyAction{};
		return action;
	}

	// ---- Escape menu ----
	// State written by MAIN only (Esc toggle, action application); the widget pass renders the
	// overlay while open and records the button press for main to poll — the usual sequencing.
	void setEscapeOpen(bool open) { m_escapeOpen = open; }
	bool isEscapeOpen() const { return m_escapeOpen; }
	EscapeMenuAction takeEscapeAction()
	{
		const EscapeMenuAction action = m_escapeAction;
		m_escapeAction = EscapeMenuAction::None;
		return action;
	}
	void renderEscape(); // widget pass, over the docked panels (or over the lobby page)

	// Widget pass only. fullRect = the whole window (the menu centers itself in it); changed
	// settings vars with onChange callbacks are collected into deferredCallbacks (the TweakPanel's
	// list, flushed on main by UI::flushMainThreadWork).
	void render(const Rect& fullRect, oc::vector<const TweakVar*>& deferredCallbacks);

private:

	void renderMain();
	void renderSettings(oc::vector<const TweakVar*>& deferredCallbacks);
	void renderLobby();

	bool m_active = false;
	bool m_settingsOpen = false;
	bool m_lobbyOpen = false;
	bool m_host = false;
	int m_settingsSection = 0;
	char m_address[64] = "";   // client address input, "ip[:port]"
	oc::string m_hostEndpoint; // what the host displays ("203.0.113.7:27888" once external resolved)
	oc::string m_hostNote;     // dim line under it (LAN address / lookup status)
	MainMenuAction m_action;
	LobbyView m_lobbyView;
	LobbyAction m_lobbyAction;
	bool m_escapeOpen = false;
	EscapeMenuAction m_escapeAction = EscapeMenuAction::None;
};
