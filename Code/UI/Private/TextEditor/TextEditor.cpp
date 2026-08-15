module UI;

import Core;
import Core.imgui;
import File;
import :TextEditor;

// ImGui's InputTextMultiline needs a fixed buffer; grow it into m_text via the resize callback (the
// imgui_stdlib.cpp pattern — that helper isn't vendored here, so it's inlined).
static int textResizeCallback(ImGuiInputTextCallbackData* data)
{
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		std::string* str = static_cast<std::string*>(data->UserData);
		assert(data->Buf == str->c_str());
		str->resize(static_cast<size_t>(data->BufTextLen));
		data->Buf = str->data();
	}
	return 0;
}

void TextEditor::requestOpen(const std::string& path)
{
	if (path.empty() || (m_hasDoc && path == m_path))
		return; // already the active document

	if (isDirty())
	{
		m_pendingSwitch     = PendingSwitch::OpenPath;
		m_pendingSwitchPath = path;
		m_openUnsavedPopup  = true;
	}
	else
	{
		doOpen(path);
	}
}

void TextEditor::requestNew()
{
	if (isDirty())
	{
		m_pendingSwitch    = PendingSwitch::New;
		m_openUnsavedPopup = true;
	}
	else
	{
		doNew();
	}
}

void TextEditor::doOpen(const std::string& path)
{
	m_text         = FileSystem::readFileStr(path);
	m_baselineText = m_text;
	m_path         = path;
	m_hasDoc       = true;
}

void TextEditor::doNew()
{
	m_text.clear();
	m_baselineText.clear();
	m_path.clear();
	m_hasDoc = true;
}

void TextEditor::save()
{
	if (m_path.empty())
	{
		m_openSaveAsPopup = true;
		return;
	}
	saveAs(m_path);
}

void TextEditor::saveAs(const std::string& path)
{
	if (!FileSystem::writeFileStr(path, m_text))
		return;
	m_path         = path;
	m_baselineText = m_text;
}

void TextEditor::renderToolbar()
{
	if (ImGui::Button("New"))
		requestNew();
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		save();
	ImGui::SameLine();
	if (ImGui::Button("Save As..."))
		ImGui::OpenPopup("Save Text As");

	ImGui::SameLine();
	const std::string label = m_hasDoc
		? (m_path.empty() ? std::string("(unsaved)") : FileSystem::filename(m_path))
		: std::string("(no file open — double-click a .txt in Content)");
	ImGui::TextDisabled("%s%s", label.c_str(), isDirty() ? " *" : "");

	renderSaveAsPopup();
}

void TextEditor::renderSaveAsPopup()
{
	if (m_openSaveAsPopup)
	{
		ImGui::OpenPopup("Save Text As");
		m_openSaveAsPopup = false;
	}

	if (!ImGui::BeginPopup("Save Text As"))
		return;

	ImGui::TextUnformatted("File name (saved under Assets/ as .txt):");
	ImGui::SetNextItemWidth(220.0f);
	const bool entered = ImGui::InputText("##ted_saveas", m_saveAsBuf, sizeof(m_saveAsBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if ((ImGui::Button("Save##ted_as") || entered) && m_saveAsBuf[0] != '\0')
	{
		std::string name = m_saveAsBuf;
		if (!name.ends_with(".txt"))
			name += ".txt";
		saveAs(name);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void TextEditor::renderUnsavedPopup()
{
	if (m_openUnsavedPopup)
	{
		ImGui::OpenPopup("Unsaved Text Changes");
		m_openUnsavedPopup = false;
	}
	if (!ImGui::BeginPopupModal("Unsaved Text Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::Text("'%s' has unsaved changes.",
		(m_path.empty() ? std::string("(unsaved)") : FileSystem::filename(m_path)).c_str());
	if (m_pendingSwitch == PendingSwitch::OpenPath)
		ImGui::Text("Switch to '%s'?", FileSystem::filename(m_pendingSwitchPath).c_str());
	else
		ImGui::TextUnformatted("Start a new file?");

	ImGui::Separator();
	const bool doSave    = ImGui::Button("Save");
	ImGui::SameLine();
	const bool discard   = ImGui::Button("Discard");
	ImGui::SameLine();
	const bool cancel    = ImGui::Button("Cancel");

	if (doSave)
		save();
	if (doSave || discard)
	{
		if (m_pendingSwitch == PendingSwitch::OpenPath)
			doOpen(m_pendingSwitchPath);
		else if (m_pendingSwitch == PendingSwitch::New)
			doNew();
	}
	if (doSave || discard || cancel)
	{
		m_pendingSwitch = PendingSwitch::None;
		m_pendingSwitchPath.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// Any file dragged out of the asset browser carries one of these path payloads depending on its type
// (ASSET_FILE = spawnable .pre, SCRIPT_FILE = .scr, TEXT_FILE = everything else) — accept all three here
// so dropping ANY asset onto the panel opens it for viewing/editing as plain text.
static void acceptFileDropTarget(TextEditor& editor, const ImVec2& min, const ImVec2& max)
{
	if (!ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID("##ted_drop")))
		return;
	for (const char* tag : { "TEXT_FILE", "ASSET_FILE", "SCRIPT_FILE" })
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(tag))
			editor.requestOpen(static_cast<const char*>(payload->Data));
	ImGui::EndDragDropTarget();
}

void TextEditor::render()
{
	renderToolbar();
	renderUnsavedPopup();

	ImGui::Separator();

	if (!m_hasDoc)
	{
		ImGui::TextDisabled("No file open. Drag any file from Content here, double-click a .txt, or click New.");
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImGui::GetContentRegionAvail());
		acceptFileDropTarget(*this, min, ImGui::GetItemRectMax());
		return;
	}

	// Ctrl+scroll zoom: while the text area is hovered, steal the wheel to change font scale instead of
	// letting the input widget below scroll with it.
	ImGuiIO&     io          = ImGui::GetIO();
	const ImVec2 textAreaMin = ImGui::GetCursorScreenPos();
	const ImVec2 avail       = ImGui::GetContentRegionAvail();
	const ImVec2 textAreaMax = ImVec2(textAreaMin.x + avail.x, textAreaMin.y + avail.y);
	if (io.KeyCtrl && io.MouseWheel != 0.0f && ImGui::IsMouseHoveringRect(textAreaMin, textAreaMax))
	{
		m_fontScale   = std::clamp(m_fontScale + io.MouseWheel * 0.1f, 0.3f, 4.0f);
		io.MouseWheel = 0.0f;
	}

	// buf_size = capacity()+1 (not size()+1): matches ImGui's own imgui_stdlib.cpp reference so the
	// resize callback only fires on real capacity growth, not on every keystroke.
	//
	// PushFont(NULL, size), not SetWindowFontScale: InputTextMultiline draws into its own internal child
	// window, and this ImGui version's docs call SetWindowFontScale out as the legacy/unreliable path —
	// PushFont's size is a stack value that applies through nested child windows unconditionally.
	const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * m_fontScale);
	ImGui::InputTextMultiline("##ted_text", m_text.data(), m_text.capacity() + 1, avail,
		flags, textResizeCallback, &m_text);
	ImGui::PopFont();

	acceptFileDropTarget(*this, textAreaMin, textAreaMax);
}
