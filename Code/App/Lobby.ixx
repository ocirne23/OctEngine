export module App.Lobby;

import Core;
import Core.Log;
import Core.glm;
import Entity;  // NetworkManager
import Network; // NetWriter/NetReader
import UI;      // LobbyView/LobbyAction (the MainMenu's lobby page)
import App.Chat; // the chat event the lobby-phase filter lets through

// The multiplayer PRE-GAME LOBBY: after the menu hosts or joins, players gather here, toggle
// Ready, and any player can press Start once everyone is ready — a 3 second countdown runs, any
// un-ready (or a new player joining) cancels it, and at zero the match launches.
//
// SERVER-AUTHORITATIVE over NetworkManager events (all reliable, so state never silently drops):
//   "LbR" client->server [u8 ready]   ready toggle request
//   "LbT" client->server [u8 team]    team pick request (PvP; server validates < numTeams)
//   "LbG" client->server              start request (server re-validates all-ready)
//   "LbS" server->clients [u8 flags: 1 coop | 2 countdown | 4 started][f32 remaining]
//         [u32 mapSeed][f32 terrainFill][u8 terrainLanes]   — the host's co-op MAP settings
//         [u8 numTeams]                                     — the host's PvP team count
//         [u8 count]{[u32 clientId][u8 ready][u8 team]}   — the full state, broadcast on every change
// PVP TEAMS: the host sets "Number of teams" (2..GameMaxTeams, SetNumTeams action); every player
// picks their own team on the lobby page (SetTeam action -> LbT on a client). A joiner lands on
// the least-populated team; lowering the count re-seats anyone above it the same way. At launch
// main hands the count + the roster's picks to the host's GameMatch (setLobbyTeams, before
// spawnWorld) — it spawns one Base per team and seats each client's capsule on its pick.
// CO-OP MAP SETTINGS: the host edits them on the lobby page (SetMapSettings action), every LbS
// mirrors them so clients see the upcoming map. At launch main hands them to the host's GameMatch
// (setMapSettings, before spawnWorld); the values that GENERATE reach clients through the game's
// own "GMp" event, which precedes the world replay — a client never generates from the lobby
// copy, so the two can never disagree.
//   "LbX" server->clients [u8 coop]   the countdown finished: launch now
// Clients ignore relayed LbR/LbG (a client's event also relays to the other clients) and act only
// on the server's LbS/LbX (sender 0). A client joining an already-RUNNING game gets LbS(started)
// from the join hook, which precedes the world replay in the reliable stream — see main.cpp's
// dispatcher for why the client must construct its GameMatch inside that dispatch.
//
// Main-thread only (NetworkManager contract); holds no entity handles — a plain stack object in
// main(). The countdown ticks on the REAL clock (the sim clock may be paused).
export class LobbySystem final
{
public:

	static constexpr float c_countdownSeconds = 3.0f;

	static bool handlesEvent(oc::string_view name) { return name.size() >= 2 && name[0] == 'L' && name[1] == 'b'; }

	// Enter the lobby right after the network started (host) / the connect began (client). The
	// client's coop flag is provisional — the first LbS overrides it with the host's mode.
	void enter(bool host, bool coop)
	{
		m_host = host;
		m_coop = coop;
		m_state = EState::Lobby;
		m_players.clear();
		m_haveState = false;
		m_numTeams = 2;
		if (m_host)
		{
			m_players.push_back({ 0, false, 0 }); // the server itself is clientId 0
			// While the lobby runs, clients may only send lobby traffic (+ chat lines); GameMatch
			// installs the game's own Gq*-only filter when it spawns.
			Globals::networkManager.setEventFilter([](uint32, oc::string_view name, oc::span<const uint8> data, Entity*)
			{
				return (handlesEvent(name) && data.size() <= 8) || ChatSystem::allowsEvent(name, data);
			});
		}
	}

	// Command-line game start (no lobby ran): record the running state so a menu client joining
	// this server still gets the "game is running" signal (onClientJoined below).
	void markStarted(bool coop)
	{
		m_state = EState::Started;
		m_coop = coop;
		m_host = Globals::networkManager.role() == ENetRole::Server;
	}

	// Exit-to-menu: back to a blank, inactive lobby (the network session is torn down with it).
	void reset()
	{
		m_state = EState::Inactive;
		m_players.clear();
		m_haveState = false;
		m_serverStart = false;
		m_clientConstruct = false;
		m_clientStart = false;
		m_countdown = 0.0f;
	}

	bool active() const { return m_state == EState::Lobby || m_state == EState::Countdown; }
	bool coop() const { return m_coop; }
	// The host's co-op map settings for the launching GameMatch (false = not a co-op host lobby:
	// the command-line path and clients keep GameMatch's tweak defaults / the GMp inputs).
	bool hostMapSettings(uint32& seed, float& fill, int& lanes) const
	{
		if (!m_host || !m_coop || m_state == EState::Inactive)
			return false;
		seed = m_mapSeed;
		fill = m_mapFill;
		lanes = m_mapLanes;
		return true;
	}
	// The PvP team setup for the launching GameMatch: the host's team count + every roster
	// member's pick (clientId, team). False = not a PvP lobby (co-op seats everyone on team 0;
	// the command-line path keeps GameMatch's defaults).
	bool teamSettings(uint8& numTeams, oc::vector<oc::pair<uint32, uint8>>& picks) const
	{
		if (m_coop || m_state == EState::Inactive)
			return false;
		numTeams = (uint8)m_numTeams;
		picks.clear();
		for (const Player& player : m_players)
			picks.push_back({ player.clientId, player.team });
		return true;
	}

	// TRUE once, server: the countdown hit zero — main spawns the game world.
	bool takeServerStart() { return take(m_serverStart); }
	// TRUE once, client: the first lobby state arrived (the mode is now known) — main constructs
	// the local GameMatch INSIDE the event dispatch (see main.cpp's dispatcher comment).
	bool takeClientConstruct() { return take(m_clientConstruct); }
	// TRUE once, client: the server declared the game running — main closes the menu.
	bool takeClientStart() { return take(m_clientStart); }

	// Server roster minus itself: who needs a capsule + world replay when the match launches.
	oc::vector<uint32> connectedClientIds() const
	{
		oc::vector<uint32> ids;
		for (const Player& player : m_players)
			if (player.clientId != 0)
				ids.push_back(player.clientId);
		return ids;
	}

	// Server join/left hooks (main wraps them together with GameMatch's — lobby FIRST, so a late
	// joiner's start signal precedes the world replay in the same reliable stream).
	void onClientJoined(uint32 clientId)
	{
		if (m_state == EState::Inactive)
			return;
		if (m_state == EState::Started)
		{
			broadcastState(); // "game is running" for the newcomer; in-game clients shrug it off
			return;
		}
		bool known = false;
		for (const Player& player : m_players)
			known |= player.clientId == clientId;
		if (!known)
			m_players.push_back({ clientId, false, leastPopulatedTeam() });
		if (m_state == EState::Countdown)
			cancelCountdown(); // the newcomer is not ready
		broadcastState();
	}

	void onClientLeft(uint32 clientId)
	{
		if (m_state != EState::Lobby && m_state != EState::Countdown)
			return;
		for (size_t i = 0; i < m_players.size(); ++i)
			if (m_players[i].clientId == clientId)
			{
				m_players.erase(m_players.begin() + i);
				break;
			}
		broadcastState();
	}

	// main's dispatcher routes every "Lb*" event here (main thread: lobby events are only fired
	// from main and received in receive()).
	void handleNetEvent(oc::string_view name)
	{
		const uint32 sender = Globals::networkManager.currentEventSender();
		NetReader reader(Globals::networkManager.currentEventData());
		if (m_host)
		{
			// requests from clients; our own broadcasts also self-dispatch here — sender 0 skips
			if (sender == 0)
				return;
			if (name == "LbR" && (m_state == EState::Lobby || m_state == EState::Countdown))
			{
				const bool ready = reader.read<uint8>() != 0;
				if (!reader.overflowed())
					applyReady(sender, ready);
			}
			else if (name == "LbT" && (m_state == EState::Lobby || m_state == EState::Countdown))
			{
				const uint8 team = reader.read<uint8>();
				if (!reader.overflowed())
					applyTeam(sender, team);
			}
			else if (name == "LbG")
				tryStartCountdown();
			return;
		}
		// client: only the server's state broadcasts matter (relayed LbR/LbG have sender != 0)
		if (sender != 0 || m_state == EState::Inactive)
			return;
		if (name == "LbS")
		{
			const uint8 flags = reader.read<uint8>();
			const float remaining = reader.read<float>();
			const uint32 mapSeed = reader.read<uint32>();
			const float mapFill = reader.read<float>();
			const uint8 mapLanes = reader.read<uint8>();
			const uint8 numTeams = reader.read<uint8>();
			const uint8 count = reader.read<uint8>();
			oc::vector<Player> players;
			for (uint8 i = 0; i < count && !reader.overflowed(); ++i)
			{
				Player player;
				player.clientId = reader.read<uint32>();
				player.ready = reader.read<uint8>() != 0;
				player.team = reader.read<uint8>();
				players.push_back(player);
			}
			if (reader.overflowed())
				return;
			m_players = oc::move(players);
			m_mapSeed = mapSeed; // display only on a client (see the header: GMp generates)
			m_mapFill = mapFill;
			m_mapLanes = mapLanes;
			m_numTeams = glm::clamp((int)numTeams, 2, c_maxTeams);
			if ((flags & 4) != 0)
				clientMarkStarted((flags & 1) != 0);
			else
			{
				m_coop = (flags & 1) != 0;
				if (!m_haveState)
				{
					m_haveState = true;
					m_clientConstruct = true; // the mode is known: build the local GameMatch
				}
				m_state = (flags & 2) != 0 ? EState::Countdown : EState::Lobby;
				m_countdown = remaining;
			}
		}
		else if (name == "LbX")
		{
			const uint8 coop = reader.read<uint8>();
			if (!reader.overflowed())
				clientMarkStarted(coop != 0);
		}
	}

	// Main thread, every frame while the menu runs: countdown tick (server = the authority,
	// clients only tick the displayed number between LbS refreshes).
	void update(float deltaSec)
	{
		if (m_state != EState::Countdown)
			return;
		m_countdown -= deltaSec;
		if (m_host && m_countdown <= 0.0f)
		{
			m_state = EState::Started;
			m_serverStart = true;
			Log::info("Lobby: countdown finished, launching");
			uint8 buffer[4];
			NetWriter writer(buffer);
			writer.write<uint8>(m_coop ? 1 : 0);
			Globals::networkManager.fireNetworkEvent("LbX", writer.data());
			broadcastState(); // the started flag, also the record replayed at late joins
		}
	}

	LobbyView view() const
	{
		LobbyView v;
		v.valid = m_host || m_haveState;
		v.hosting = m_host;
		v.coop = m_coop;
		v.countdownActive = m_state == EState::Countdown;
		v.countdownRemaining = oc::max(m_countdown, 0.0f);
		v.mapSeed = m_mapSeed;
		v.terrainFill = m_mapFill;
		v.terrainLanes = m_mapLanes;
		v.numTeams = m_coop ? 1 : m_numTeams;
		const uint32 self = Globals::networkManager.localClientId();
		bool allReady = !m_players.empty();
		for (const Player& player : m_players)
		{
			allReady &= player.ready;
			const bool isSelf = player.clientId == self;
			if (isSelf)
			{
				v.localReady = player.ready;
				v.localTeam = player.team;
			}
			v.players.push_back({ player.clientId, player.team, player.ready, isSelf });
		}
		v.allReady = allReady;
		return v;
	}

	// The lobby page's button presses (main thread, polled from the UI).
	void handleAction(const LobbyAction& action)
	{
		if (!active() || action.type == LobbyAction::EType::None)
			return;
		if (action.type == LobbyAction::EType::ToggleReady)
		{
			if (m_host)
				applyReady(0, !readyOf(0));
			else
			{
				// toggle against the mirrored state; the server's LbS echo is the confirmation
				uint8 buffer[2];
				NetWriter writer(buffer);
				writer.write<uint8>(readyOf(Globals::networkManager.localClientId()) ? 0 : 1);
				Globals::networkManager.fireNetworkEvent("LbR", writer.data());
			}
		}
		else if (action.type == LobbyAction::EType::Start)
		{
			if (m_host)
				tryStartCountdown();
			else
				Globals::networkManager.fireNetworkEvent("LbG");
		}
		else if (action.type == LobbyAction::EType::SetTeam && !m_coop)
		{
			if (m_host)
				applyTeam(0, action.team);
			else
			{
				uint8 buffer[2];
				NetWriter writer(buffer);
				writer.write<uint8>(action.team);
				Globals::networkManager.fireNetworkEvent("LbT", writer.data());
			}
		}
		else if (action.type == LobbyAction::EType::SetNumTeams && m_host && !m_coop)
		{
			// Host only. Anyone seated above the new count is re-seated on the least-populated
			// team; a team-layout change mid-countdown cancels it (everyone should see it).
			const int numTeams = glm::clamp(action.numTeams, 2, c_maxTeams);
			if (numTeams == m_numTeams)
				return;
			m_numTeams = numTeams;
			for (Player& player : m_players)
				if (player.team >= m_numTeams)
					player.team = leastPopulatedTeam();
			if (m_state == EState::Countdown)
				cancelCountdown();
			broadcastState();
		}
		else if (action.type == LobbyAction::EType::SetMapSettings && m_host && m_coop)
		{
			// Host only (the page shows clients a read-only line, and no client event carries
			// these). Clamped to what the generator accepts; every change mirrors at once.
			m_mapSeed = action.mapSeed & 0x7fffffffu;
			m_mapFill = glm::clamp(action.terrainFill, 0.0f, 0.6f);
			m_mapLanes = glm::clamp(action.terrainLanes, 2, 12);
			broadcastState();
		}
	}

private:

	enum class EState : uint8 { Inactive, Lobby, Countdown, Started };

	// The PvP team-count ceiling (= the Game layer's GameMaxTeams / the Force system's cap).
	static constexpr int c_maxTeams = 8;

	struct Player
	{
		uint32 clientId = 0;
		bool ready = false;
		uint8 team = 0; // PvP pick (always 0 in co-op)
	};

	static bool take(bool& flag)
	{
		const bool value = flag;
		flag = false;
		return value;
	}

	bool readyOf(uint32 clientId) const
	{
		for (const Player& player : m_players)
			if (player.clientId == clientId)
				return player.ready;
		return false;
	}

	void clientMarkStarted(bool coop)
	{
		m_coop = coop;
		if (!m_haveState)
		{
			m_haveState = true;
			m_clientConstruct = true;
		}
		if (m_state != EState::Started)
		{
			m_state = EState::Started;
			m_clientStart = true;
		}
	}

	// PvP: the team with the fewest seated players (lowest index on a tie) — where a joiner and a
	// re-seated player land. Co-op is always team 0.
	uint8 leastPopulatedTeam() const
	{
		if (m_coop)
			return 0;
		int counts[c_maxTeams] = {};
		for (const Player& player : m_players)
			if (player.team < m_numTeams)
				++counts[player.team];
		uint8 best = 0;
		for (int t = 1; t < m_numTeams; ++t)
			if (counts[t] < counts[best])
				best = (uint8)t;
		return best;
	}

	void applyTeam(uint32 clientId, uint8 team) // server
	{
		if (m_coop || team >= m_numTeams)
			return;
		for (Player& player : m_players)
			if (player.clientId == clientId)
			{
				if (player.team == team)
					return;
				player.team = team;
			}
		if (m_state == EState::Countdown)
			cancelCountdown(); // the team layout changed under the countdown
		broadcastState();
	}

	void applyReady(uint32 clientId, bool ready) // server
	{
		for (Player& player : m_players)
			if (player.clientId == clientId)
			{
				if (player.ready == ready)
					return;
				player.ready = ready;
			}
		if (!ready && m_state == EState::Countdown)
			cancelCountdown();
		broadcastState();
	}

	void tryStartCountdown() // server; validates all-ready whoever asked
	{
		if (m_state != EState::Lobby || m_players.empty())
			return;
		for (const Player& player : m_players)
			if (!player.ready)
				return;
		m_state = EState::Countdown;
		m_countdown = c_countdownSeconds;
		Log::info("Lobby: countdown started");
		broadcastState();
	}

	void cancelCountdown() // server; the caller broadcasts
	{
		m_state = EState::Lobby;
		m_countdown = 0.0f;
		Log::info("Lobby: countdown cancelled");
	}

	void broadcastState() // server
	{
		if (!m_host)
			return;
		uint8 buffer[256];
		NetWriter writer(buffer);
		writer.write<uint8>(uint8((m_coop ? 1 : 0) | (m_state == EState::Countdown ? 2 : 0)
			| (m_state == EState::Started ? 4 : 0)));
		writer.write<float>(oc::max(m_countdown, 0.0f));
		writer.write<uint32>(m_mapSeed);
		writer.write<float>(m_mapFill);
		writer.write<uint8>((uint8)m_mapLanes);
		writer.write<uint8>((uint8)m_numTeams);
		const uint8 count = (uint8)oc::min(m_players.size(), (size_t)32);
		writer.write<uint8>(count);
		for (uint8 i = 0; i < count; ++i)
		{
			writer.write<uint32>(m_players[i].clientId);
			writer.write<uint8>(m_players[i].ready ? 1 : 0);
			writer.write<uint8>(m_players[i].team);
		}
		Globals::networkManager.fireNetworkEvent("LbS", writer.data());
	}

	oc::vector<Player> m_players;
	EState m_state = EState::Inactive;
	bool m_host = false;
	bool m_coop = false;
	bool m_haveState = false;       // client: the first LbS arrived
	bool m_serverStart = false;     // see takeServerStart
	bool m_clientConstruct = false; // see takeClientConstruct
	bool m_clientStart = false;     // see takeClientStart
	float m_countdown = 0.0f;
	int m_numTeams = 2; // PvP: the host's team count (2..c_maxTeams). Mirrored in LbS.
	// Co-op map settings (host-authored; defaults = GameMatch's tweak defaults). Mirrored in LbS.
	uint32 m_mapSeed = 0;
	float m_mapFill = 0.3f;
	int m_mapLanes = 6;
};
