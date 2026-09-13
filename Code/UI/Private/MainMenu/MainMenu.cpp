module UI;

import Core;
import Core.imgui;
import Core.glm;
import Core.Rect;
import Core.Tweaks;

import :MainMenu;
import :TweakPanel; // drawTweakVar - the settings page draws the same widget rows

namespace
{
	// ---- The lobby's world-preview texture ---------------------------------------------------
	// One ImGui-managed texture (ImTextureData: the backend creates the image and uploads the pixels
	// through Renderer::updateImGuiTextures, the same path the font atlas takes - no renderer API).
	// A NAMESPACE-SCOPE plain global, deliberately NOT a MainMenu member: ImGui_ImplVulkan_Shutdown
	// (the renderer's dtor) walks ImGui::GetPlatformIO().Textures and destroys every entry with
	// RefCount 1, so the object must still exist then - and UI (XCU9) is the first engine global to
	// go, while a plain global destructs after every numbered section, the renderer's included.
	// The wrapper frees only the CPU pixels, after the backend has released the GPU side.
	struct PreviewTexture
	{
		ImTextureData data;
		~PreviewTexture() { data.DestroyPixels(); }
	};
	PreviewTexture g_previewTexture;

	// ---- Settings curation -----------------------------------------------------------------
	// The settings page is BUILT FROM THE TWEAK REGISTRY: each section lists top-level category
	// prefixes (a prefix pulls the whole subtree - "Ocean" includes "Ocean/Waves"), and dev-only
	// branches are filtered below. A tweak that is not registered yet (e.g. Game/* before a game
	// mode started) simply does not appear.
	struct SettingsSection
	{
		const char* name;
		const oc::string_view* prefixes;
		size_t count;
	};

	constexpr oc::string_view c_displayCats[]  = { "Time", "HUD", "Post" };
	constexpr oc::string_view c_graphicsCats[] = { "TAA", "Shadows", "RT", "RTAO", "GI", "LOD",
		"Fog", "Decals", "Particles", "Sky", "Force/Shell", "Texture Streaming", "Mesh Streaming" };
	constexpr oc::string_view c_worldCats[]    = { "Terrain", "Ocean", "Scatter" };
	constexpr oc::string_view c_audioCats[]    = { "Audio" };
	constexpr oc::string_view c_gameplayCats[] = { "Game" };

	constexpr SettingsSection c_sections[] = {
		{ "Display",  c_displayCats,  sizeof(c_displayCats) / sizeof(c_displayCats[0]) },
		{ "Graphics", c_graphicsCats, sizeof(c_graphicsCats) / sizeof(c_graphicsCats[0]) },
		{ "World",    c_worldCats,    sizeof(c_worldCats) / sizeof(c_worldCats[0]) },
		{ "Audio",    c_audioCats,    sizeof(c_audioCats) / sizeof(c_audioCats[0]) },
		{ "Gameplay", c_gameplayCats, sizeof(c_gameplayCats) / sizeof(c_gameplayCats[0]) },
	};

	bool matchesPrefix(oc::string_view category, oc::string_view prefix)
	{
		// "Fog" matches "Fog" and "Fog/Quality"; "RT" never matches "RTAO"
		if (!oc::startsWith(category, prefix))
			return false;
		return category.size() == prefix.size() || category[prefix.size()] == '/';
	}

	bool varExcluded(const TweakVar& var)
	{
		// Dev-only branches that never belong in a game settings menu; matched anywhere in the
		// category path ("Force/Debug", "Terrain/V3", the testbed's "Force/Emitter", ...).
		constexpr oc::string_view c_excluded[] = { "Debug", "Stress", "Stats", "Cheats", "Emitter", "V3" };
		for (const oc::string_view bad : c_excluded)
			if (var.category.find(bad) != oc::string_view::npos)
				return true;
		// The global pause is a game action (Pause/Break key), not a setting.
		if (var.category == "Time" && var.name == "Paused")
			return true;
		return false;
	}

	bool sectionContains(const SettingsSection& section, const TweakVar& var)
	{
		for (size_t i = 0; i < section.count; ++i)
			if (matchesPrefix(var.category, section.prefixes[i]))
				return !varExcluded(var);
		return false;
	}

	void centeredText(const char* label)
	{
		const float width = ImGui::GetContentRegionAvail().x;
		const float textWidth = ImGui::CalcTextSize(label).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + glm::max(0.0f, (width - textWidth) * 0.5f));
		ImGui::TextUnformatted(label);
	}
}

void MainMenu::render(const Rect& fullRect, oc::vector<const TweakVar*>& deferredCallbacks)
{
	const glm::ivec2 vpSize = fullRect.getSize();
	if (vpSize.x <= 0 || vpSize.y <= 0)
		return;

	// dim the world behind the menu
	ImGui::GetBackgroundDrawList()->AddRectFilled(
		ImVec2((float)fullRect.min.x, (float)fullRect.min.y),
		ImVec2((float)fullRect.max.x, (float)fullRect.max.y),
		ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.43f)));

	const ImVec2 center((fullRect.min.x + fullRect.max.x) * 0.5f, (fullRect.min.y + fullRect.max.y) * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
	if (m_lobbyOpen)
	{
		// The local lobby adds the world column (the preview map) on the right; height fits content.
		ImGui::SetNextWindowSize(ImVec2(m_lobbyView.local ? 1080.0f : 520.0f, 0.0f), ImGuiCond_Always);
		if (ImGui::Begin("##MainMenuLobby", nullptr, flags))
			renderLobby();
		ImGui::End();
	}
	else if (m_settingsOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(glm::min((float)vpSize.x - 80.0f, 960.0f),
			glm::min((float)vpSize.y - 80.0f, 660.0f)), ImGuiCond_Always);
		if (ImGui::Begin("##MainMenuSettings", nullptr, flags))
			renderSettings(deferredCallbacks);
		ImGui::End();
	}
	else
	{
		ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always); // height fits content
		if (ImGui::Begin("##MainMenu", nullptr, flags))
			renderMain();
		ImGui::End();
	}
	ImGui::PopStyleVar(2);
}

// The Esc overlay: a fullscreen click-blocking DIM window (a background-draw-list rect would sit
// UNDER the docked panels, a foreground one OVER the buttons - a window layers correctly), then
// the button window on top of it. Both are FOCUSED ONLY ON THE OPENING FRAME (dim first, buttons
// last, so the pair lands above the editor panels with the buttons in front): a per-frame
// SetNextWindowFocus re-runs ImGui's focus change every Begin, and that CLEARS THE ACTIVE WIDGET
// each time - a button press never survived to its release, so clicks did nothing. Afterwards the
// display order is stable on its own: the dim blocks clicks from reaching the panels, and its
// NoBringToFrontOnFocus keeps a click on the dim itself from raising it over the buttons.
void MainMenu::renderEscape(bool offerDebugToggle)
{
	const bool focusThisFrame = m_escapeFocusPending;
	m_escapeFocusPending = false;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImGuiWindowFlags dimFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	if (focusThisFrame)
		ImGui::SetNextWindowFocus();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.43f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("##EscapeDim", nullptr, dimFlags);
	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
	if (focusThisFrame)
		ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoDocking;
	if (ImGui::Begin("##EscapeMenu", nullptr, flags))
	{
		ImGui::SetWindowFontScale(1.5f);
		centeredText("Menu");
		ImGui::SetWindowFontScale(1.0f);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		const ImVec2 buttonSize(ImGui::GetContentRegionAvail().x, 38.0f);
		if (ImGui::Button("Resume", buttonSize))
			m_escapeAction = EscapeMenuAction::Resume;
		if (m_escapeOffersPause && !m_gamePaused && ImGui::Button("Pause game", buttonSize))
			m_escapeAction = EscapeMenuAction::Pause; // shared: every player sees the paused box
		if (offerDebugToggle)
		{
			ImGui::Spacing();
			ImGui::Checkbox("Debug panels", &m_debugPanels);
			// The Profiler panel's Pause button without the panel: freezes the profiler's rings +
			// frame marks (Profiler::setPaused); the panel adopts the state when it next shows.
			bool profilerPaused = Globals::profiler.isPaused();
			if (ImGui::Checkbox("Pause profiler", &profilerPaused))
				Globals::profiler.setPaused(profilerPaused);
			ImGui::Spacing();
		}
		if (ImGui::Button("Exit to menu", buttonSize))
			m_escapeAction = EscapeMenuAction::ExitToMenu;
		if (ImGui::Button("Quit game", buttonSize))
			m_escapeAction = EscapeMenuAction::Quit;
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

void MainMenu::renderPausedBox()
{
	if (!m_gamePaused)
		return;
	// The SHARED pause: a small centered box over everything (under the escape menu when both
	// are up), with the one button any player may press. It never dims the screen - the game
	// view stays readable while everyone waits.
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.35f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoDocking;
	if (ImGui::Begin("##PausedBox", nullptr, flags))
	{
		ImGui::SetWindowFontScale(1.6f);
		centeredText("PAUSED");
		ImGui::SetWindowFontScale(1.0f);
		ImGui::Spacing();
		centeredText("Any player can resume");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		if (ImGui::Button("Resume", ImVec2(ImGui::GetContentRegionAvail().x, 38.0f)))
			m_escapeAction = EscapeMenuAction::Unpause;
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

void MainMenu::renderMain()
{
	ImGui::SetWindowFontScale(2.0f);
	centeredText("OctEngine");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const auto start = [this](MainMenuAction::EType type)
	{
		m_status.clear();
		m_action.type = type;
		m_action.host = m_host;
		oc::string_view address(m_address);
		while (!address.empty() && address.front() == ' ')
			address.remove_prefix(1);
		while (!address.empty() && address.back() == ' ')
			address.remove_suffix(1);
		m_action.connectAddress = m_host ? oc::string() : oc::string(address);
	};

	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 buttonSize(width, 42.0f);
	if (ImGui::Button("Play co-op (PvE)", buttonSize))
		start(MainMenuAction::EType::StartCoop);
	if (ImGui::Button("Play PvP", buttonSize))
		start(MainMenuAction::EType::StartPvp);
	if (ImGui::Button("Sandbox (engine testbed)", buttonSize))
		start(MainMenuAction::EType::StartSandbox);

	ImGui::Spacing();
	ImGui::SeparatorText("Multiplayer");
	ImGui::Checkbox("Host a server", &m_host);
	if (m_host)
	{
		ImGui::TextUnformatted("Other players connect to:");
		// read-only InputText so the address is selectable
		char endpoint[64];
		snprintf(endpoint, sizeof(endpoint), "%s", m_hostEndpoint.c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##hostEndpoint", endpoint, sizeof(endpoint), ImGuiInputTextFlags_ReadOnly);
		if (!m_hostNote.empty())
			ImGui::TextDisabled("%s", m_hostNote.c_str());
	}
	else
	{
		ImGui::TextUnformatted("Server address:");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##address", "ip[:port] - empty = play offline", m_address, sizeof(m_address));
	}
	if (!m_status.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", m_status.c_str());

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	if (ImGui::Button("Settings", ImVec2(width, 0.0f)))
		m_settingsOpen = true;
	if (ImGui::Button("Quit", ImVec2(width, 0.0f)))
		m_action.type = MainMenuAction::EType::Quit;
}

void MainMenu::renderLobby()
{
	ImGui::SetWindowFontScale(1.6f);
	centeredText(m_lobbyView.coop ? "Co-op lobby" : "PvP lobby");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (!m_lobbyView.valid)
	{
		centeredText("Connecting to server...");
		ImGui::Spacing();
		if (ImGui::Button("Leave", ImVec2(ImGui::GetContentRegionAvail().x, 38.0f)))
			m_lobbyAction.type = LobbyAction::EType::Leave; // the way out of a dead address
		return;
	}

	// Local: two columns - the match settings and buttons left, the world block (seed + preview map)
	// right. Everything below reads its width from the cell, so the column is transparent to it.
	const bool twoColumns = m_lobbyView.local
		&& ImGui::BeginTable("##lobbyColumns", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV);
	if (twoColumns)
	{
		ImGui::TableSetupColumn("##lobbyLeft", ImGuiTableColumnFlags_WidthFixed, 470.0f);
		ImGui::TableSetupColumn("##lobbyWorld", ImGuiTableColumnFlags_WidthFixed, 540.0f);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
	}

	if (m_lobbyView.hosting && !m_lobbyView.local)
	{
		ImGui::TextUnformatted("Other players connect to:");
		char endpoint[64];
		snprintf(endpoint, sizeof(endpoint), "%s", m_hostEndpoint.c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##lobbyHostEndpoint", endpoint, sizeof(endpoint), ImGuiInputTextFlags_ReadOnly);
		if (!m_hostNote.empty())
			ImGui::TextDisabled("%s", m_hostNote.c_str());
		ImGui::Spacing();
	}

	const float width = ImGui::GetContentRegionAvail().x;
	if (m_lobbyView.coop)
	{
		ImGui::SeparatorText("Map");
		if (m_lobbyView.hosting)
		{
			// The host edits; every widget commits on RELEASE (one lobby-state broadcast per edit,
			// not one per drag frame). Between edits the fields track the model's snapshot.
			if (!m_mapEditActive)
			{
				m_mapEditSeed = (int)m_lobbyView.mapSeed;
				m_mapEditFill = m_lobbyView.terrainFill;
				m_mapEditLanes = m_lobbyView.terrainLanes;
			}
			bool active = false, commit = false;
			ImGui::SetNextItemWidth(width - 90.0f);
			ImGui::InputInt("##lobbyMapSeed", &m_mapEditSeed, 0, 0);
			active |= ImGui::IsItemActive();
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SameLine();
			if (ImGui::Button("Random", ImVec2(82.0f, 0.0f)))
			{
				m_mapEditSeed = 0;
				commit = true;
			}
			ImGui::TextDisabled("Map seed (0 = random at start)");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##lobbyMapFill", &m_mapEditFill, 0.0f, 0.6f, "Terrain fill %.2f");
			active |= ImGui::IsItemActive();
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderInt("##lobbyMapLanes", &m_mapEditLanes, 2, 12, "Attack lanes %d");
			active |= ImGui::IsItemActive();
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			m_mapEditActive = active;
			if (commit)
			{
				m_lobbyAction.type = LobbyAction::EType::SetMapSettings;
				m_lobbyAction.mapSeed = (uint32)glm::max(m_mapEditSeed, 0);
				m_lobbyAction.terrainFill = glm::clamp(m_mapEditFill, 0.0f, 0.6f);
				m_lobbyAction.terrainLanes = glm::clamp(m_mapEditLanes, 2, 12);
			}
		}
		else
		{
			char seed[32];
			if (m_lobbyView.mapSeed == 0)
				snprintf(seed, sizeof(seed), "random");
			else
				snprintf(seed, sizeof(seed), "%u", m_lobbyView.mapSeed);
			ImGui::Text("Seed %s   |   terrain fill %.0f%%   |   %d attack lanes", seed,
				m_lobbyView.terrainFill * 100.0f, m_lobbyView.terrainLanes);
			ImGui::TextDisabled("Chosen by the host");
		}
		ImGui::Spacing();
	}
	else
	{
		ImGui::SeparatorText("Match");
		const int mapCount = (int)m_lobbyView.pvpMapNames.size();
		const int mapIdx = glm::clamp(m_lobbyView.pvpMap, 0, glm::max(mapCount - 1, 0));
		const char* mapName = mapCount > 0 ? m_lobbyView.pvpMapNames[mapIdx] : "?";
		if (m_lobbyView.hosting)
		{
			// The arena pick: a combo, applied at once (one broadcast per pick).
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##lobbyPvpMap", mapName))
			{
				for (int i = 0; i < mapCount; ++i)
					if (ImGui::Selectable(m_lobbyView.pvpMapNames[i], i == mapIdx) && i != mapIdx)
					{
						m_lobbyAction.type = LobbyAction::EType::SetPvpMap;
						m_lobbyAction.pvpMap = i;
					}
				ImGui::EndCombo();
			}
			ImGui::TextDisabled("Map");
			// Same commit-on-release pattern as the map widgets: one broadcast per edit.
			if (!m_teamsEditActive)
				m_teamsEdit = m_lobbyView.numTeams;
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderInt("##lobbyNumTeams", &m_teamsEdit, 2, 8, "Number of teams %d");
			m_teamsEditActive = ImGui::IsItemActive();
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				m_lobbyAction.type = LobbyAction::EType::SetNumTeams;
				m_lobbyAction.numTeams = glm::clamp(m_teamsEdit, 2, 8);
			}
		}
		else
		{
			ImGui::Text("Map: %s   |   %d teams", mapName, m_lobbyView.numTeams);
			ImGui::TextDisabled("Chosen by the host");
		}
		ImGui::TextDisabled("Pick your team in the player list");
		ImGui::Spacing();
	}

	const float teamColumn = width - 200.0f;
	if (!m_lobbyView.local)
		ImGui::SeparatorText("Players");
	for (const LobbyView::Player& player : m_lobbyView.players)
	{
		if (m_lobbyView.local)
			break; // one player, no ready check: nothing to list
		char name[48];
		snprintf(name, sizeof(name), "%s %u%s", player.clientId == 0 ? "Host" : "Player",
			player.clientId, player.isSelf ? "  (you)" : "");
		ImGui::TextUnformatted(name);
		if (m_lobbyView.numTeams > 1)
		{
			ImGui::SameLine(teamColumn);
			char team[16];
			snprintf(team, sizeof(team), "Team %d", (int)player.team + 1);
			if (player.isSelf)
			{
				// Only OUR row is a picker; the host's count bounds it. The pick reaches the
				// server as an LbT request (or applies at once on the host) and the LbS echo
				// moves the row - the same round trip the Ready toggle takes.
				ImGui::SetNextItemWidth(110.0f);
				char comboId[32];
				snprintf(comboId, sizeof(comboId), "##lobbyTeam%u", player.clientId);
				if (ImGui::BeginCombo(comboId, team))
				{
					for (int t = 0; t < m_lobbyView.numTeams; ++t)
					{
						char option[16];
						snprintf(option, sizeof(option), "Team %d", t + 1);
						if (ImGui::Selectable(option, t == (int)player.team) && t != (int)player.team)
						{
							m_lobbyAction.type = LobbyAction::EType::SetTeam;
							m_lobbyAction.team = (uint8)t;
						}
					}
					ImGui::EndCombo();
				}
			}
			else
				ImGui::TextUnformatted(team);
		}
		ImGui::SameLine(width - 60.0f);
		if (player.ready)
			ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.4f, 1.0f), "READY");
		else
			ImGui::TextDisabled("---");
	}

	if (m_chat && !m_lobbyView.local)
	{
		ImGui::SeparatorText("Chat");
		m_chat->renderEmbedded(150.0f);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (m_lobbyView.countdownActive)
	{
		char buf[48];
		snprintf(buf, sizeof(buf), "Starting in %d...", (int)glm::ceil(m_lobbyView.countdownRemaining));
		ImGui::SetWindowFontScale(1.5f);
		centeredText(buf);
		ImGui::SetWindowFontScale(1.0f);
		ImGui::Spacing();
	}

	const ImVec2 buttonSize(width, 38.0f);
	if (m_lobbyView.local)
	{
		// No ready check offline. A seeded world holds Start until its chunks are in (the world
		// column shows the bar); an unseeded lobby starts on the flat default at once.
		const bool worldBusy = m_lobbyView.world.seeded && !m_lobbyView.world.seededSettled;
		ImGui::BeginDisabled(worldBusy);
		if (ImGui::Button("Start", buttonSize))
			m_lobbyAction.type = LobbyAction::EType::Start;
		ImGui::EndDisabled();
		if (worldBusy)
			ImGui::TextDisabled("Start unlocks once the world has streamed in");
	}
	else
	{
		if (ImGui::Button(m_lobbyView.localReady
			? (m_lobbyView.countdownActive ? "Un-ready (cancels the countdown)" : "Un-ready") : "Ready",
			buttonSize))
			m_lobbyAction.type = LobbyAction::EType::ToggleReady;

		ImGui::BeginDisabled(!m_lobbyView.allReady || m_lobbyView.countdownActive);
		if (ImGui::Button("Start", buttonSize))
			m_lobbyAction.type = LobbyAction::EType::Start;
		ImGui::EndDisabled();
		if (!m_lobbyView.allReady)
			ImGui::TextDisabled("Start unlocks when every player is ready");
	}
	ImGui::Spacing();
	if (ImGui::Button(m_lobbyView.hosting && !m_lobbyView.local ? "Leave (closes the lobby for everyone)" : "Leave", buttonSize))
		m_lobbyAction.type = LobbyAction::EType::Leave;

	if (twoColumns)
	{
		ImGui::TableNextColumn();
		renderLobbyWorld();
		ImGui::EndTable();
	}
}

void MainMenu::uploadPreviewImage(const LobbyPreviewImage& image)
{
	ImTextureData& tex = g_previewTexture.data;
	if (tex.Pixels == nullptr)
	{
		// First image: create the texture and REGISTER it as a user texture. ImGui rebuilds
		// PlatformIO.Textures at every Render (atlas textures + the registered user list), so a plain
		// push into that vector is gone before main services it; a registered texture is appended
		// every frame, which is what gets it created, updated and destroyed at shutdown.
		// RefCount 1 = "one context uses it", which is what the backend's shutdown destroys.
		tex.Create(ImTextureFormat_RGBA32, (int)image.width, (int)image.height);
		tex.RefCount = 1;
		tex.UseColors = true;
		tex.SetStatus(ImTextureStatus_WantCreate);
		ImGui::RegisterUserTexture(&tex);
	}
	else if (tex.Width != (int)image.width || tex.Height != (int)image.height)
		return; // fixed-size by construction (TerrainPreview::c_resolution); never resized live
	memcpy(tex.GetPixels(), image.rgba.data(), (size_t)image.width * image.height * 4);
	if (tex.Status == ImTextureStatus_OK || tex.Status == ImTextureStatus_WantUpdates)
	{
		// A create uploads the whole buffer anyway; an existing texture gets one full-rect update.
		ImTextureRect rect;
		rect.x = 0;
		rect.y = 0;
		rect.w = (unsigned short)image.width;
		rect.h = (unsigned short)image.height;
		tex.Updates.push_back(rect);
		tex.UpdateRect = rect;
		tex.SetStatus(ImTextureStatus_WantUpdates);
	}
	m_previewUploadedGeneration = image.generation;
}

void MainMenu::renderLobbyWorld()
{
	const LobbyWorldView& world = m_lobbyView.world;
	using EPreview = LobbyWorldView::EPreview;
	const float width = ImGui::GetContentRegionAvail().x;

	ImGui::SeparatorText("World");
	if (!m_terrainSeedEditInit)
	{
		m_terrainSeedEdit = (int)world.terrainSeed;
		m_terrainSeedEditInit = true;
	}
	const bool previewBusy = world.preview == EPreview::LoadingModels || world.preview == EPreview::Generating;
	ImGui::SetNextItemWidth(width - 270.0f);
	ImGui::InputInt("##terrainSeed", &m_terrainSeedEdit, 0, 0);
	ImGui::SameLine();
	if (ImGui::Button("Random##terrainSeed", ImVec2(76.0f, 0.0f))) // the co-op map block has its own "Random"
		m_terrainSeedEdit = (int)((uint64)(ImGui::GetTime() * 1e6) % 16000000u);
	ImGui::SameLine();
	ImGui::BeginDisabled(previewBusy);
	if (ImGui::Button("Generate preview", ImVec2(-FLT_MIN, 0.0f)))
	{
		m_lobbyAction.type = LobbyAction::EType::GeneratePreview;
		m_lobbyAction.terrainSeed = (uint32)glm::clamp(m_terrainSeedEdit, 0, 16000000);
		m_previewPicked = false;
	}
	ImGui::EndDisabled();
	ImGui::TextDisabled("Terrain seed");
	ImGui::Spacing();

	// ---- The map ----
	if (world.image && world.image->generation != m_previewUploadedGeneration)
		uploadPreviewImage(*world.image);
	const float mapSize = glm::min(width, 512.0f);
	const ImVec2 mapMin = ImGui::GetCursorScreenPos();
	const ImVec2 mapMax(mapMin.x + mapSize, mapMin.y + mapSize);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	// Draw the image only once the backend has CREATED the texture (Status OK, a real id): a draw
	// command recorded against a WantCreate texture binds a null descriptor set. The create lands
	// on main at the top of the next frame, so the map shows one pass after its first upload.
	const bool haveMap = world.image && m_previewUploadedGeneration == world.image->generation
		&& g_previewTexture.data.Status == ImTextureStatus_OK && g_previewTexture.data.TexID != 0;
	if (haveMap)
	{
		// The view: a zoom-in window of the map (uv0..uv1), centred on m_mapCenter. Everything below
		// maps between MAP space (0..1 over the whole image - the picks, the seeded square) and
		// SCREEN space through that window, so the overlays follow the zoom.
		m_mapZoom = glm::clamp(m_mapZoom, 1.0f, 32.0f);
		const auto clampCenter = [&]()
		{
			const float half = 0.5f / m_mapZoom;
			m_mapCenter = glm::clamp(m_mapCenter, glm::vec2(half), glm::vec2(1.0f - half));
		};
		clampCenter();
		const float span = 1.0f / m_mapZoom; // map units visible across the image
		const ImVec2 uv0(m_mapCenter.x - span * 0.5f, m_mapCenter.y - span * 0.5f);
		const ImVec2 uv1(m_mapCenter.x + span * 0.5f, m_mapCenter.y + span * 0.5f);
		const auto toMap = [&](ImVec2 s) { return glm::vec2(uv0.x + (s.x - mapMin.x) / mapSize * span, uv0.y + (s.y - mapMin.y) / mapSize * span); };
		const auto toScreen = [&](glm::vec2 p) { return ImVec2(mapMin.x + (p.x - uv0.x) / span * mapSize, mapMin.y + (p.y - uv0.y) / span * mapSize); };

		ImGui::Image(g_previewTexture.data.GetTexRef(), ImVec2(mapSize, mapSize), uv0, uv1);
		const bool hovered = ImGui::IsItemHovered();
		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mouse = ImGui::GetMousePos();

		// Wheel: zoom about the cursor (the map point under it stays put).
		if (hovered && io.MouseWheel != 0.0f)
		{
			const glm::vec2 anchor = toMap(mouse);
			const float newZoom = glm::clamp(m_mapZoom * std::pow(1.25f, io.MouseWheel), 1.0f, 32.0f);
			const glm::vec2 cursor01((mouse.x - mapMin.x) / mapSize, (mouse.y - mapMin.y) / mapSize);
			m_mapCenter = anchor - (cursor01 - 0.5f) * (1.0f / newZoom);
			m_mapZoom = newZoom;
			clampCenter();
		}
		// Left button: a drag pans, a release without a drag picks. Double-click resets the view.
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_mapPressed = true;
			m_mapDragged = false;
		}
		if (m_mapPressed && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
			{
				m_mapDragged = true;
				m_mapCenter -= glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / mapSize * span;
				clampCenter();
			}
		}
		if (m_mapPressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			if (!m_mapDragged && hovered)
			{
				m_previewPick = glm::clamp(toMap(mouse), glm::vec2(0.0f), glm::vec2(1.0f));
				m_previewPicked = true;
			}
			m_mapPressed = false;
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			m_mapZoom = 1.0f;
			m_mapCenter = glm::vec2(0.5f);
		}

		draw->PushClipRect(mapMin, mapMax, true); // overlays clip to the visible window of the map
		const auto marker = [&](glm::vec2 pick, ImU32 colour, float radius)
		{
			const ImVec2 c = toScreen(pick);
			draw->AddCircle(c, radius, colour, 0, 2.0f);
			draw->AddLine(ImVec2(c.x - radius * 1.8f, c.y), ImVec2(c.x + radius * 1.8f, c.y), colour, 1.5f);
			draw->AddLine(ImVec2(c.x, c.y - radius * 1.8f), ImVec2(c.x, c.y + radius * 1.8f), colour, 1.5f);
		};
		if (world.seeded && world.seededAreaValid)
		{
			// The generated tiles' footprint (tile-aligned, so a little larger than the picked size).
			const ImU32 area = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.63f, 0.16f, 0.9f));
			const ImVec2 a = toScreen(world.seededAreaMin);
			const ImVec2 b = toScreen(world.seededAreaMax);
			draw->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.63f, 0.16f, 0.18f)));
			draw->AddRect(a, b, area, 0.0f, 0, 2.0f);
		}
		if (world.seeded)
			marker(world.seededPick, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.63f, 0.16f, 1.0f)), 9.0f); // where the world is built
		if (m_previewPicked)
			marker(m_previewPick, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.9f)), 7.0f);
		draw->PopClipRect();
		ImGui::TextDisabled("%.0f km across, %.1fx  |  click = pick, drag = pan, wheel = zoom, double-click = reset",
			world.image->extentKm, m_mapZoom);
	}
	else
	{
		draw->AddRectFilled(mapMin, mapMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.08f, 0.09f, 0.11f, 1.0f)));
		draw->AddRect(mapMin, mapMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.31f, 0.33f, 0.38f, 1.0f)));
		const char* caption = world.preview == EPreview::LoadingModels ? "Loading the terrain models..."
			: world.preview == EPreview::Generating ? "Generating the preview..."
			: world.preview == EPreview::Failed ? "The terrain generator is unavailable"
			: world.image ? "Uploading the preview..."
			: "No preview yet";
		const ImVec2 textSize = ImGui::CalcTextSize(caption);
		draw->AddText(ImVec2(mapMin.x + (mapSize - textSize.x) * 0.5f, mapMin.y + (mapSize - textSize.y) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(ImVec4(0.67f, 0.68f, 0.72f, 1.0f)), caption);
		ImGui::Dummy(ImVec2(mapSize, mapSize));
		ImGui::TextDisabled("Enter a seed and press Generate preview");
	}

	// ---- Preview status ----
	if (world.preview == EPreview::Generating)
		ImGui::ProgressBar(world.previewProgress, ImVec2(-FLT_MIN, 0.0f), "Generating preview");
	else if (world.preview == EPreview::LoadingModels)
	{
		ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0.0f), "Loading terrain models");
		if (!world.previewStatus.empty())
			ImGui::TextDisabled("%s", world.previewStatus.c_str());
	}
	else if (world.preview == EPreview::Failed)
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", world.previewStatus.c_str());
	ImGui::Spacing();

	// ---- Seed world ----
	ImGui::SeparatorText("Seed world");
	// The playable area: how much terrain the seeding generates tiles for (and the streamer's ring
	// is sized to). The caption estimates the tile count from the preview's tile size, so the cost
	// of a pick is visible before pressing the button.
	static constexpr float c_areaSizes[] = { 512.0f, 1024.0f, 2048.0f, 4096.0f, 8192.0f };
	static constexpr const char* c_areaLabels[] = { "512 m", "1 km", "2 km", "4 km", "8 km" };
	m_areaSizeIdx = glm::clamp(m_areaSizeIdx, 0, 4);
	ImGui::SetNextItemWidth(120.0f);
	ImGui::Combo("##playableArea", &m_areaSizeIdx, c_areaLabels, 5);
	ImGui::SameLine();
	if (haveMap && world.image->tileWorldSizeM > 0.0f)
	{
		const int perSide = (int)glm::ceil(c_areaSizes[m_areaSizeIdx] / world.image->tileWorldSizeM) + 1;
		const int tiles = perSide * perSide;
		ImGui::TextDisabled("Playable area  ~%d tiles, up to ~%.0f min if none are cached", tiles, (float)tiles * 1.5f / 60.0f);
	}
	else
		ImGui::TextDisabled("Playable area (side)");
	const auto seedAction = [this]()
	{
		m_lobbyAction.type = LobbyAction::EType::SeedWorld;
		m_lobbyAction.pickU = m_previewPick.x;
		m_lobbyAction.pickV = m_previewPick.y;
		m_lobbyAction.areaSizeM = c_areaSizes[m_areaSizeIdx];
	};
	if (!world.seeded)
	{
		ImGui::BeginDisabled(!haveMap || !m_previewPicked || world.preview != EPreview::Ready);
		if (ImGui::Button("Seed world at the picked spot", ImVec2(-FLT_MIN, 38.0f)))
			seedAction();
		ImGui::EndDisabled();
		if (!haveMap)
			ImGui::TextDisabled("Generate a preview first");
		else if (!m_previewPicked)
			ImGui::TextDisabled("Click the map to pick a spot");
		else
			ImGui::TextDisabled("Generates the area's terrain tiles (cached ones load); the mesh streams live");
	}
	else
	{
		if (world.seededSettled)
			ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.4f, 1.0f), "World ready - %s", world.seededStatus.c_str());
		else
		{
			ImGui::ProgressBar(world.seededProgress, ImVec2(-FLT_MIN, 0.0f), world.seededStatus.c_str());
			ImGui::TextDisabled("Generating the terrain tiles around the pick...");
		}
		const bool moved = m_previewPicked && glm::distance(m_previewPick, world.seededPick) > 0.002f;
		ImGui::BeginDisabled(world.preview != EPreview::Ready);
		if (ImGui::Button(moved ? "Re-seed at the picked spot" : "Re-seed with this area size", ImVec2(-FLT_MIN, 0.0f)))
			seedAction();
		ImGui::EndDisabled();
		if (ImGui::Button("Unseed (play on the flat default)", ImVec2(-FLT_MIN, 0.0f)))
			m_lobbyAction.type = LobbyAction::EType::UnseedWorld;
	}
}

void MainMenu::renderSettings(oc::vector<const TweakVar*>& deferredCallbacks)
{
	if (ImGui::Button("< Back"))
		m_settingsOpen = false;
	ImGui::SameLine();
	ImGui::SetWindowFontScale(1.4f);
	centeredText("Settings");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Separator();

	// left: section list
	ImGui::BeginChild("##sections", ImVec2(150.0f, 0.0f), ImGuiChildFlags_None);
	for (int i = 0; i < (int)(sizeof(c_sections) / sizeof(c_sections[0])); ++i)
		if (ImGui::Selectable(c_sections[i].name, i == m_settingsSection))
			m_settingsSection = i;
	ImGui::EndChild();
	ImGui::SameLine();

	// right: every registered tweak the section's category prefixes pull in, grouped by category
	ImGui::BeginChild("##vars", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	const SettingsSection& section = c_sections[m_settingsSection];
	const oc::vector<TweakVar>& vars = TweakRegistry::get().vars();
	oc::vector<int> indices;
	for (int i = 0; i < (int)vars.size(); ++i)
		if (sectionContains(section, vars[i]))
			indices.push_back(i);
	oc::sort(indices.begin(), indices.end(), [&vars](int a, int b)
	{
		if (vars[a].category != vars[b].category)
			return vars[a].category < vars[b].category;
		return a < b; // registration order within a category
	});

	if (indices.empty())
		ImGui::TextDisabled("Nothing registered here yet (mode-specific settings appear once that mode runs)");
	oc::string_view lastCategory;
	for (const int i : indices)
	{
		if (vars[i].category != lastCategory)
		{
			lastCategory = vars[i].category;
			ImGui::SeparatorText(oc::string(lastCategory).c_str());
		}
		drawTweakVar(vars[i], i, deferredCallbacks);
	}
	ImGui::EndChild();
}
