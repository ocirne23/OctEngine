export module App.Lobby;

import Core;
import Core.Log;
import Core.glm;
import Entity;  // NetworkManager
import Network; // NetWriter/NetReader
import UI;      // LobbyView/LobbyAction (the MainMenu's lobby page)

// The multiplayer PRE-GAME LOBBY: after the menu hosts or joins, players gather here, toggle
// Ready, and any player can press Start once everyone is ready — a 3 second countdown runs, any
// un-ready (or a new player joining) cancels it, and at zero the match launches.
//
// SERVER-AUTHORITATIVE over NetworkManager events (all reliable, so state never silently drops):
//   "LbR" client->server [u8 ready]   ready toggle request
//   "LbG" client->server              start request (server re-validates all-ready)
//   "LbS" server->clients [u8 flags: 1 coop | 2 countdown | 4 started][f32 remaining]
//         [u8 count]{[u32 clientId][u8 ready]}   — the full state, broadcast on every change
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
		if (m_host)
		{
			m_players.push_back({ 0, false }); // the server itself is clientId 0
			// While the lobby runs, clients may only send lobby traffic; GameMatch installs the
			// game's own Gq*-only filter when it spawns.
			Globals::networkManager.setEventFilter([](uint32, oc::string_view name, oc::span<const uint8> data, Entity*)
			{
				return handlesEvent(name) && data.size() <= 8;
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
			m_players.push_back({ clientId, false });
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
			const uint8 count = reader.read<uint8>();
			oc::vector<Player> players;
			for (uint8 i = 0; i < count && !reader.overflowed(); ++i)
			{
				Player player;
				player.clientId = reader.read<uint32>();
				player.ready = reader.read<uint8>() != 0;
				players.push_back(player);
			}
			if (reader.overflowed())
				return;
			m_players = oc::move(players);
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
		const uint32 self = Globals::networkManager.localClientId();
		bool allReady = !m_players.empty();
		for (const Player& player : m_players)
		{
			allReady &= player.ready;
			const bool isSelf = player.clientId == self;
			if (isSelf)
				v.localReady = player.ready;
			v.players.push_back({ player.clientId, player.ready, isSelf });
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
	}

private:

	enum class EState : uint8 { Inactive, Lobby, Countdown, Started };

	struct Player
	{
		uint32 clientId = 0;
		bool ready = false;
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
		const uint8 count = (uint8)oc::min(m_players.size(), (size_t)32);
		writer.write<uint8>(count);
		for (uint8 i = 0; i < count; ++i)
		{
			writer.write<uint32>(m_players[i].clientId);
			writer.write<uint8>(m_players[i].ready ? 1 : 0);
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
};
