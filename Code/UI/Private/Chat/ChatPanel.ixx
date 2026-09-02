export module UI:ChatPanel;

import Core;
import Core.imgui;
import Core.glm;
import Core.Rect;

// The multiplayer TEXT CHAT widget: a scrolling message log + an input line. App's ChatSystem is
// the model (messages arrive over the "ChM" network event); main pushes a ChatView snapshot when
// the log changed (setView, between the widget-pass join and kick) and polls takeOutgoing() for
// the line the player sent — the same sequencing every menu/lobby action uses.
//
// Drawn in two places: EMBEDDED in the lobby page (renderEmbedded) and as an OVERLAY window in the
// bottom-right corner of the game viewport (renderOverlay). Enter opens the input line when no
// widget is active (in-game: the viewport window is focused, so nothing else is), Enter sends,
// Esc cancels; both hand ImGui focus back to the named window so the game's viewport-focus gate
// re-enables the grid hotkeys the moment typing ends.
export struct ChatView
{
	struct Message
	{
		uint32 clientId = 0; // 0 = the host; the panel names it "Host" / "Player N"
		bool isSelf = false;
		oc::string text;
	};
	oc::vector<Message> messages; // oldest first
};

export class ChatPanel final
{
public:

	static constexpr size_t c_maxTextLength = 200;

	void setView(ChatView view) { m_view = oc::move(view); } // main thread, on change only
	// Main thread: the line the player sent (empty = none). One line per frame at most.
	oc::string takeOutgoing()
	{
		oc::string line = oc::move(m_outgoing);
		m_outgoing.clear();
		return line;
	}
	void clear() { m_view.messages.clear(); m_input[0] = '\0'; m_outgoing.clear(); }

	// Widget pass. Inside a window the caller owns (the lobby page): a log of logHeight px plus
	// the input line, full content width.
	void renderEmbedded(float logHeight)
	{
		renderLog("##chatLog", ImVec2(0.0f, logHeight));
		renderInput(nullptr);
	}

	// Widget pass, game layout: a translucent window in the viewport's bottom-right corner. Enter
	// (with the viewport focused) opens the line; sending/cancelling refocuses refocusWindow.
	void renderOverlay(const Rect& viewport, const char* refocusWindow)
	{
		const glm::ivec2 size = viewport.getSize();
		if (size.x <= 0 || size.y <= 0)
			return;
		const float width = glm::min(280.0f, (float)size.x - 16.0f);
		const float height = glm::min(150.0f, (float)size.y - 16.0f);
		ImGui::SetNextWindowPos(ImVec2((float)viewport.max.x - 8.0f, (float)viewport.max.y - 8.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
			| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav;
		// Nearly invisible while idle (the text carries its own shadow-free contrast over the
		// world); a light tint only while typing so the input line reads as a field.
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, m_typing ? 0.2f : 0.05f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, m_typing ? 0.15f : 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, m_typing ? 0.35f : 0.05f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
		if (ImGui::Begin("##GameChat", nullptr, flags))
		{
			const float inputHeight = ImGui::GetFrameHeightWithSpacing();
			renderLog("##chatLog", ImVec2(0.0f, -inputHeight));
			renderInput(refocusWindow);
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
	}

private:

	static void nameOf(uint32 clientId, char* out, size_t size)
	{
		if (clientId == 0)
			snprintf(out, size, "Host");
		else
			snprintf(out, size, "Player %u", clientId);
	}

	void renderLog(const char* id, ImVec2 size)
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.05f));
		if (ImGui::BeginChild(id, size, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav))
		{
			ImGui::PushTextWrapPos(0.0f);
			if (m_view.messages.empty())
				ImGui::TextDisabled("No messages yet - press Enter to chat");
			for (const ChatView::Message& message : m_view.messages)
			{
				char name[32];
				nameOf(message.clientId, name, sizeof(name));
				const ImVec4 color = message.isSelf ? ImVec4(0.6f, 0.85f, 1.0f, 1.0f)
					: message.clientId == 0 ? ImVec4(1.0f, 0.85f, 0.5f, 1.0f) : ImVec4(0.75f, 1.0f, 0.75f, 1.0f);
				ImGui::TextColored(color, "%s:", name);
				ImGui::SameLine();
				ImGui::TextUnformatted(message.text.c_str());
			}
			ImGui::PopTextWrapPos();
			// stick to the bottom while new lines arrive (a manual scroll up holds)
			if (m_view.messages.size() != m_scrolledCount)
			{
				m_scrolledCount = m_view.messages.size();
				ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	void renderInput(const char* refocusWindow)
	{
		// Enter with nothing active = start typing. The key is read through ImGui (every SDL event
		// reaches ImGui before the engine's input gate), so this works while the game viewport owns
		// the keyboard.
		if (!m_typing && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
			m_focusPending = true;
		if (m_focusPending)
		{
			m_focusPending = false;
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		const bool submitted = ImGui::InputTextWithHint("##chatInput", "Press Enter to chat", m_input, sizeof(m_input),
			ImGuiInputTextFlags_EnterReturnsTrue);
		const bool active = ImGui::IsItemActive();
		bool done = false;
		if (submitted)
		{
			oc::string_view line(m_input);
			while (!line.empty() && line.front() == ' ')
				line.remove_prefix(1);
			while (!line.empty() && line.back() == ' ')
				line.remove_suffix(1);
			if (!line.empty())
				m_outgoing = oc::string(line);
			m_input[0] = '\0';
			done = true;
		}
		else if (m_typing && !active)
			done = true; // Esc (ImGui cancels the edit) or a click elsewhere
		m_typing = active && !done;
		if (done && refocusWindow)
			ImGui::SetWindowFocus(refocusWindow); // back to the game: the viewport-focus gate re-arms the hotkeys
	}

	ChatView m_view;
	oc::string m_outgoing;
	char m_input[c_maxTextLength + 1] = "";
	size_t m_scrolledCount = 0;
	bool m_typing = false;
	bool m_focusPending = false;
};
