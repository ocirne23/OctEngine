module UI;

import Core;
import Core.imgui;
import Core.Log;
import :OutputLog;

static ImVec4 levelColor(Log::Level level)
{
	switch (level)
	{
	case Log::Level::Verbose: return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
	case Log::Level::Info:    return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
	case Log::Level::Warning: return ImVec4(1.00f, 0.85f, 0.20f, 1.0f);
	case Log::Level::Error:   return ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
	default:                  return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
	}
}

static const char* levelLabel(Log::Level level)
{
	switch (level)
	{
	case Log::Level::Verbose: return "VERBOSE";
	case Log::Level::Info:    return "INFO";
	case Log::Level::Warning: return "WARNING";
	case Log::Level::Error:   return "ERROR";
	default:                  return "";
	}
}

static bool levelToggle(const char* label, bool* active, ImVec4 colour)
{
	if (!*active)
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	else
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colour.x * 0.4f, colour.y * 0.4f, colour.z * 0.4f, 1.0f));

	ImGui::PushStyleColor(ImGuiCol_Text, colour);
	bool clicked = ImGui::Button(label);
	ImGui::PopStyleColor(2);

	if (clicked)
		*active = !*active;

	return clicked;
}

void OutputLog::prepare()
{
	ProfileScope scope("Log panel prepare", EProfileCategory::UI);
	m_prepared = true;
	const uint32 rev = Log::getRevision();
	if (rev != m_cachedRevision)
	{
		m_snapshot       = Log::getMessages();
		m_cachedRevision = rev;
	}
	const bool hasFilter = m_filterBuf[0] != '\0';
	m_visible.clear();
	for (uint32 i = 0; i < static_cast<uint32>(m_snapshot.size()); ++i)
	{
		const Log::Message& msg = m_snapshot[i];
		switch (msg.level)
		{
		case Log::Level::Verbose: if (!m_showVerbose) continue; break;
		case Log::Level::Info:    if (!m_showInfo)    continue; break;
		case Log::Level::Warning: if (!m_showWarning) continue; break;
		case Log::Level::Error:   if (!m_showError)   continue; break;
		default: break;
		}
		if (hasFilter && msg.text.find(m_filterBuf) == std::string::npos)
			continue;
		m_visible.push_back(i);
	}
}

void OutputLog::render()
{
	if (!m_prepared)
		prepare(); // nothing ran ahead of us — inline
	m_prepared = false;
	if (ImGui::Button("Clear"))
		Log::clear();

	ImGui::SameLine();
	ImGui::SetNextItemWidth(180.0f);
	ImGui::InputTextWithHint("##ol_filter", "Filter...", m_filterBuf, sizeof(m_filterBuf));

	ImGui::SameLine();
	levelToggle("Verbose", &m_showVerbose, levelColor(Log::Level::Verbose));
	ImGui::SameLine();
	levelToggle("Info",    &m_showInfo,    levelColor(Log::Level::Info));
	ImGui::SameLine();
	levelToggle("Warning", &m_showWarning, levelColor(Log::Level::Warning));
	ImGui::SameLine();
	levelToggle("Error",   &m_showError,   levelColor(Log::Level::Error));

	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &m_autoScroll);

	ImGui::Separator();

	// (snapshot + filter ran in prepare(): m_visible indexes m_snapshot — both replaced together,
	// so the pair is always consistent; a filter edit shows up next frame)
	ImGui::BeginChild("##ol_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
		ImGuiWindowFlags_HorizontalScrollbar);

	ImGuiListClipper clipper;
	const std::vector<uint32>& s_visible = m_visible;
	clipper.Begin(static_cast<int>(s_visible.size()));
	while (clipper.Step())
	{
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
		{
			const Log::Message& msg = m_snapshot[s_visible[row]];
			const ImVec4        col = levelColor(msg.level);

			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::Text("[%-7s]", levelLabel(msg.level));
			ImGui::PopStyleColor();

			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, col);
			ImGui::TextUnformatted(msg.text.c_str());
			ImGui::PopStyleColor();
		}
	}
	clipper.End();

	if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
}
