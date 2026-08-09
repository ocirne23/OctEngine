module UI;

import Core;
import Core.imgui;
import Entity;
import File;
import :AssetBrowser;

static bool isImageFile(const std::string& ext)
{
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".hdr";
}

static bool isMeshFile(const std::string& ext)
{
	return ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".dae";
}

static bool isShaderFile(const std::string& ext)
{
	return ext == ".glsl" || ext == ".hlsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".spv";
}

static bool isPrefabFile(const std::string& ext)
{
	return ext == ".pre";
}

static bool isObjectContainer(const std::string& ext)
{
	return ext == ".oc";
}

static bool isScriptFile(const std::string& ext)
{
	return ext == ".dsl"; // the one authorable script format -- .scr (the removed node editor's) reads as plain text now
}

static bool isTextFile(const std::string& ext)
{
	// .scr: the removed node editor's format -- kept viewable as the generated C++ it is, no longer a script.
	return ext == ".txt" || ext == ".scr";
}

static bool isSpawnableFile(const std::filesystem::path& p)
{
	return p.extension() == ".pre";
}

static const char* fileIcon(const std::filesystem::path& p)
{
	if (std::filesystem::is_directory(p)) return "[Dir]";
	auto ext = p.extension().string();
	if (isImageFile(ext))                  return "[Img]";
	if (isMeshFile(ext))                   return "[Msh]";
	if (isShaderFile(ext))                 return "[Shd]";
	if (isPrefabFile(ext))                 return "[Pre]";
	if (isObjectContainer(ext))            return "[OC]";
	if (isScriptFile(ext))                 return "[Scr]";
	if (isTextFile(ext))                   return "[Txt]";
	return "[Fil]";
}

static ImVec4 fileColor(const std::filesystem::path& p)
{
	if (std::filesystem::is_directory(p)) return ImVec4(1.0f, 0.85f, 0.4f, 1.0f);   // yellow
	auto ext = p.extension().string();
	if (isImageFile(ext))                   return ImVec4(0.4f, 0.8f,  1.0f, 1.0f);   // cyan
	if (isMeshFile(ext))                    return ImVec4(0.6f, 1.0f,  0.6f, 1.0f);   // green
	if (isShaderFile(ext))                  return ImVec4(1.0f, 0.6f,  0.3f, 1.0f);   // orange
	if (isPrefabFile(ext))                  return ImVec4(0.9f, 0.5f,  1.0f, 1.0f);   // purple
	if (isObjectContainer(ext))             return ImVec4(0.5f, 1.0f,  0.9f, 1.0f);   // light teal
	if (isScriptFile(ext))                  return ImVec4(0.5f, 1.0f,  0.6f, 1.0f);   // script green
	if (isTextFile(ext))                    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f); // light grey
	return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);                                         // grey
}

static void assetDragSource(const std::filesystem::path& p)
{
	if (std::filesystem::is_directory(p))
		return; // dragging a folder isn't meaningful to any current drop target

	const bool spawnable = isSpawnableFile(p);
	const bool script = isScriptFile(p.extension().string());
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
	{
		const std::string path = p.string();
		// Every file is draggable: spawnable/script (.scr or .dsl) keep their dedicated payload (viewport
		// spawn / script assign), anything else falls back to TEXT_FILE so it can be dropped onto the Script
		// Editor and viewed/edited as plain text.
		const char* payloadTag = spawnable ? "ASSET_FILE" : script ? "SCRIPT_FILE" : "TEXT_FILE";
		ImGui::SetDragDropPayload(payloadTag, path.c_str(), path.size() + 1);
		ImGui::Text(spawnable ? "Spawn %s" : script ? "Assign %s" : "View %s as text", p.filename().string().c_str());
		ImGui::EndDragDropSource();
	}
}

static std::string truncateLabel(const std::string& name, float maxWidth)
{
	if (ImGui::CalcTextSize(name.c_str()).x <= maxWidth)
		return name;
	size_t lo = 0, hi = name.size();
	while (lo + 1 < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (ImGui::CalcTextSize((name.substr(0, mid) + "...").c_str()).x <= maxWidth)
			lo = mid;
		else
			hi = mid;
	}
	return name.substr(0, lo) + "...";
}

void AssetBrowser::initialize()
{
	std::error_code ec;
	m_rootPath = std::filesystem::canonical(std::filesystem::current_path(), ec);
	if (ec)
		m_rootPath = std::filesystem::current_path();
	m_currentPath = m_rootPath;
}

bool AssetBrowser::isWithinRoot(const std::filesystem::path& path) const
{
	std::error_code ec;
	const auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (ec)
		return false;
	const auto rel = std::filesystem::relative(canonical, m_rootPath, ec);
	if (ec || rel.empty())
		return false;
	return *rel.begin() != "..";   // anything starting with ".." escapes the root upward
}

void AssetBrowser::render()
{
	renderToolbar();
	ImGui::Separator();

	ImGui::BeginChild("##ab_left", ImVec2(m_leftPaneWidth, 0.0f), ImGuiChildFlags_Borders);
	renderDirectoryTree(m_rootPath);
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.28f, 0.28f, 0.28f, 0.60f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.50f, 0.50f, 0.80f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.50f, 0.50f, 1.00f));
	ImGui::Button("##ab_splitter", ImVec2(4.0f, -1.0f));
	if (ImGui::IsItemActive())
	{
		m_leftPaneWidth += ImGui::GetIO().MouseDelta.x;
		m_leftPaneWidth  = std::clamp(m_leftPaneWidth, 80.0f, 600.0f);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	ImGui::PopStyleColor(3);

	ImGui::SameLine();

	ImGui::BeginChild("##ab_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	if (m_listView)
		renderContentList();
	else
		renderContentGrid();
	acceptPrefabDrop();
	renderNewAssetContextMenu();
	ImGui::EndChild();

	renderOverwritePopup();
	renderCyclePopup();
	renderRenamePopup();
}

void AssetBrowser::queueSavePrefab(Entity* root, const std::filesystem::path& path)
{
	std::error_code ec;
	const std::filesystem::path rel = std::filesystem::relative(path, ec);
	const std::string savePath = (ec || rel.empty()) ? path.string() : rel.string();
	m_changes.push_back({ EntityChange::SavePrefab{ EntityPtr(root), savePath } });
	m_selectedPath = path; // absolute, to match the directory listing for highlight
}

void AssetBrowser::acceptPrefabDrop()
{
	const ImVec2 mn = ImGui::GetWindowPos();
	const ImVec2 mx = ImVec2(mn.x + ImGui::GetWindowSize().x, mn.y + ImGui::GetWindowSize().y);
	if (!ImGui::BeginDragDropTargetCustom(ImRect(mn, mx), ImGui::GetID("##ab_prefab_drop")))
		return;
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SV_ENTITY"))
	{
		Entity* entity = *static_cast<Entity**>(payload->Data);
		const std::string name = entity->hasName() ? std::string(entity->getName()) : std::string("Prefab");
		const std::filesystem::path out = m_currentPath / (name + ".pre");
		std::error_code ec;
		if (prefabWouldCycle(entity, name))
		{
			m_pendingSavePath = out; // for the message filename
			m_openCyclePopup = true;
		}
		else if (std::filesystem::exists(out, ec))
		{
			m_pendingSaveRoot = EntityPtr(entity);
			m_pendingSavePath = out;
			m_openOverwritePopup = true;
		}
		else
		{
			queueSavePrefab(entity, out);
		}
	}
	ImGui::EndDragDropTarget();
}

void AssetBrowser::renderOverwritePopup()
{
	static const char* popupId = "Overwrite prefab?##ab_overwrite";
	if (m_openOverwritePopup)
	{
		ImGui::OpenPopup(popupId);
		m_openOverwritePopup = false;
	}

	const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::Text("\"%s\" already exists.\nOverwrite it?", m_pendingSavePath.filename().string().c_str());
	ImGui::Separator();
	if (ImGui::Button("Overwrite", ImVec2(120.0f, 0.0f)))
	{
		queueSavePrefab(m_pendingSaveRoot.get(), m_pendingSavePath);
		m_pendingSaveRoot.release();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
	{
		m_pendingSaveRoot.release();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void AssetBrowser::renderCyclePopup()
{
	static const char* popupId = "Cannot save prefab##ab_cycle";
	if (m_openCyclePopup)
	{
		ImGui::OpenPopup(popupId);
		m_openCyclePopup = false;
	}

	const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::Text("\"%s\" contains an instance of itself.\nSaving it would create a prefab cycle.",
		m_pendingSavePath.filename().string().c_str());
	ImGui::Separator();
	if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void AssetBrowser::renderToolbar()
{
	const bool canGoUp = (m_currentPath != m_rootPath) && m_currentPath.has_parent_path();
	if (!canGoUp)
	{
		ImGui::BeginDisabled();
	}
	if (ImGui::SmallButton(" ^ ##ab_up"))
		navigateUp();
	if (!canGoUp)
	{
		ImGui::EndDisabled();
	}

	ImGui::SameLine();

	{
		const std::string rootLabel = m_rootPath.filename().empty()
			? m_rootPath.string()
			: m_rootPath.filename().string();
		ImGui::PushID("##ab_crumb_root");
		if (ImGui::SmallButton(rootLabel.c_str()))
			navigateTo(m_rootPath);
		ImGui::PopID();

		std::error_code ec;
		const std::filesystem::path rel = std::filesystem::relative(m_currentPath, m_rootPath, ec);
		std::filesystem::path accumulated = m_rootPath;
		if (!ec && rel != ".")
		{
			int i = 0;
			for (const auto& part : rel)
			{
				accumulated /= part;
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::TextDisabled("/");
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::PushID(i++);
				if (ImGui::SmallButton(part.string().c_str()))
					navigateTo(accumulated);
				ImGui::PopID();
			}
		}
	}

	const float searchWidth  = 180.0f;
	const float sliderWidth  = 80.0f;
	const float toggleWidth  = 40.0f;
	const float rightWidth   = searchWidth + toggleWidth + (m_listView ? 0.0f : sliderWidth + 4.0f) + 12.0f;
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - rightWidth);
	ImGui::SetNextItemWidth(searchWidth);
	ImGui::InputTextWithHint("##ab_search", "Search...", m_searchBuf, sizeof(m_searchBuf));

	ImGui::SameLine();
	const bool listViewBeforeClick = m_listView;
	if (listViewBeforeClick)
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	if (ImGui::Button("List", ImVec2(toggleWidth, 0.0f)))
		m_listView = !m_listView;
	if (listViewBeforeClick)
		ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(m_listView ? "Switch to icon view" : "Switch to list view");

	if (!m_listView)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(sliderWidth);
		ImGui::SliderFloat("##ab_iconsize", &m_iconSize, 40.0f, 128.0f, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Icon size");
	}
}

void AssetBrowser::renderDirectoryTree(const std::filesystem::path& dir)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec))
		return;

	const bool isCurrent = (dir == m_currentPath);

	bool hasSubDirs = false;
	for (const auto& e : std::filesystem::directory_iterator(dir, ec))
		if (e.is_directory()) { hasSubDirs = true; break; }

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
		| ImGuiTreeNodeFlags_SpanAvailWidth
		| ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (isCurrent)  flags |= ImGuiTreeNodeFlags_Selected;
	if (!hasSubDirs) flags |= ImGuiTreeNodeFlags_Leaf;
	if (dir == m_rootPath) flags |= ImGuiTreeNodeFlags_DefaultOpen;

	const std::string label = dir.filename().string().empty()
		? dir.string()
		: dir.filename().string();

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	ImGui::PopStyleColor();

	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		navigateTo(dir);

	if (open)
	{
		std::vector<std::filesystem::path> subDirs;
		for (const auto& e : std::filesystem::directory_iterator(dir, ec))
			if (e.is_directory()) subDirs.push_back(e.path());
		std::sort(subDirs.begin(), subDirs.end());
		for (const auto& sub : subDirs)
			renderDirectoryTree(sub);
		ImGui::TreePop();
	}
}

void AssetBrowser::renderContentGrid()
{
	const float cellSize    = m_iconSize + 20.0f;
	const float panelWidth  = ImGui::GetContentRegionAvail().x;
	const int   columnCount = std::max(1, static_cast<int>(panelWidth / cellSize));
	const std::string filter(m_searchBuf);

	std::error_code ec;
	std::vector<std::filesystem::directory_entry> entries;
	for (const auto& e : std::filesystem::directory_iterator(m_currentPath, ec))
	{
		if (!filter.empty())
		{
			const std::string fname = e.path().filename().string();
			auto it = std::search(fname.begin(), fname.end(), filter.begin(), filter.end(),
				[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
			if (it == fname.end()) continue;
		}
		entries.push_back(e);
	}

	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		const bool ad = a.is_directory(), bd = b.is_directory();
		if (ad != bd) return ad > bd;
		return a.path().filename() < b.path().filename();
	});

	if (ImGui::BeginTable("##ab_grid", columnCount, ImGuiTableFlags_None))
	{
		for (const auto& entry : entries)
		{
			ImGui::TableNextColumn();
			const std::filesystem::path& p = entry.path();
			const std::string name = p.filename().string();
			const bool isSelected  = (p == m_selectedPath);

			ImGui::PushID(name.c_str());

			if (isSelected)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			else
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

			ImGui::PushStyleColor(ImGuiCol_Text, fileColor(p));
			const bool clicked = ImGui::Button(fileIcon(p), ImVec2(m_iconSize, m_iconSize));
			ImGui::PopStyleColor(2); // Text + Button

			assetDragSource(p);

			if (clicked)
				m_selectedPath = p;

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (entry.is_directory())
					navigateTo(p);
				else if (isScriptFile(p.extension().string()))
					m_scriptOpenRequest = p.string();
				else if (isTextFile(p.extension().string()))
					m_textOpenRequest = p.string();
			}

			renderContextMenu(p);

			const std::string display = truncateLabel(name, cellSize - 4.0f);
			ImGui::TextUnformatted(display.c_str());
			if (display != name && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", name.c_str());

			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
		&& !ImGui::IsAnyItemHovered())
	{
		m_selectedPath.clear();
	}
}

void AssetBrowser::renderContentList()
{
	const std::string filter(m_searchBuf);

	std::error_code ec;
	std::vector<std::filesystem::directory_entry> entries;
	for (const auto& e : std::filesystem::directory_iterator(m_currentPath, ec))
	{
		if (!filter.empty())
		{
			const std::string fname = e.path().filename().string();
			auto it = std::search(fname.begin(), fname.end(), filter.begin(), filter.end(),
				[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
			if (it == fname.end()) continue;
		}
		entries.push_back(e);
	}

	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		const bool ad = a.is_directory(), bd = b.is_directory();
		if (ad != bd) return ad > bd;
		return a.path().filename() < b.path().filename();
	});

	const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
		| ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##ab_list", 3, tableFlags))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();

		for (const auto& entry : entries)
		{
			const std::filesystem::path& p = entry.path();
			const std::string name         = p.filename().string();
			const bool isSelected          = (p == m_selectedPath);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);

			ImGui::PushID(name.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, fileColor(p));
			if (ImGui::Selectable(name.c_str(), isSelected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
				ImVec2(0.0f, 0.0f)))
			{
				m_selectedPath = p;
			}
			ImGui::PopStyleColor();

			assetDragSource(p);

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (entry.is_directory())
					navigateTo(p);
				else if (isScriptFile(p.extension().string()))
					m_scriptOpenRequest = p.string();
				else if (isTextFile(p.extension().string()))
					m_textOpenRequest = p.string();
			}

			renderContextMenu(p);

			ImGui::TableSetColumnIndex(1);
			if (entry.is_directory())
				ImGui::TextDisabled("Folder");
			else
				ImGui::TextDisabled("%s", p.extension().string().c_str());

			ImGui::TableSetColumnIndex(2);
			if (!entry.is_directory())
			{
				std::error_code sec;
				const auto bytes = entry.file_size(sec);
				if (!sec)
				{
					if (bytes < 1024)
						ImGui::TextDisabled("%llu B", bytes);
					else if (bytes < 1024 * 1024)
						ImGui::TextDisabled("%.1f KB", bytes / 1024.0);
					else
						ImGui::TextDisabled("%.1f MB", bytes / (1024.0 * 1024.0));
				}
			}

			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
		&& !ImGui::IsAnyItemHovered())
	{
		m_selectedPath.clear();
	}
}

void AssetBrowser::renderContextMenu(const std::filesystem::path& p)
{
	if (!ImGui::BeginPopupContextItem("##ab_ctx"))
		return;

	m_selectedPath = p;

	if (ImGui::MenuItem("Copy path"))
		ImGui::SetClipboardText(p.string().c_str());

	if (ImGui::MenuItem("Copy filename"))
		ImGui::SetClipboardText(p.filename().string().c_str());

	if (ImGui::MenuItem("Rename"))
	{
		m_renameTarget = p;
		const std::string fname = p.filename().string();
		const size_t n = std::min(fname.size(), sizeof(m_renameBuf) - 1);
		for (size_t i = 0; i < sizeof(m_renameBuf); ++i) m_renameBuf[i] = i < n ? fname[i] : '\0';
		m_openRenamePopup = true;
	}

	ImGui::Separator();

#if defined(_WIN32)
	if (ImGui::MenuItem("Show in Explorer"))
	{
		const std::string cmd = "explorer /select,\"" + p.string() + "\"";
		std::system(cmd.c_str());
	}
#endif

	if (isScriptFile(p.extension().string()))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open Script"))
			m_scriptOpenRequest = p.string();
	}

	if (isTextFile(p.extension().string()))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open Text File"))
			m_textOpenRequest = p.string();
	}

	if (isSpawnableFile(p))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Edit Entity"))
			m_entityEditRequest = p.string();
	}

	if (std::filesystem::is_directory(p))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open folder"))
			navigateTo(p);
	}

	ImGui::EndPopup();
}

void AssetBrowser::navigateTo(const std::filesystem::path& path)
{
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec) && isWithinRoot(path))
	{
		m_currentPath  = std::filesystem::canonical(path, ec);
		m_selectedPath.clear();
		m_searchBuf[0] = '\0';
	}
}

void AssetBrowser::selectFile(const std::filesystem::path& path)
{
	std::error_code ec;
	const std::filesystem::path abs = std::filesystem::canonical(path, ec);
	if (ec || !abs.has_parent_path())
		return;
	navigateTo(abs.parent_path());
	m_selectedPath = abs;
}

void AssetBrowser::navigateUp()
{
	if (m_currentPath != m_rootPath && m_currentPath.has_parent_path())
		navigateTo(m_currentPath.parent_path());
}

void AssetBrowser::renderNewAssetContextMenu()
{
	if (!ImGui::BeginPopupContextWindow("##ab_winctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		return;
	if (ImGui::MenuItem("New Script"))
	{
		// A .dsl, not a .scr -- the node system is deprecated for new scripts. The file written is the SMALLEST
		// loadable document (an empty version-1 block); it opens in the Script Editor immediately through the
		// same request a double-click raises, and that editor's own Save writes the full form from then on.
		const std::filesystem::path path = makeUniqueAssetPath("NewScript", ".dsl");
		if (FileSystem::writeFileStr(path.string(), "//@@dsl 1\n//@@end\n"))
			m_scriptOpenRequest = path.string();
	}
	if (ImGui::MenuItem("New Text File"))
	{
		const std::filesystem::path path = makeUniqueAssetPath("NewText", ".txt");
		if (FileSystem::writeFileStr(path.string(), ""))
			m_textOpenRequest = path.string();
	}
	if (ImGui::MenuItem("New Prefab"))
	{
		const std::filesystem::path path = makeUniqueAssetPath("NewPrefab", ".pre");
		const std::string id = path.stem().string();
		if (FileSystem::writeFileStr(path.string(), "Prefab " + id + "\n"))
		{
			Globals::assetRegistry.addPrefab(id, path.string());
			m_entityEditRequest = path.string();
		}
	}
	ImGui::EndPopup();
}

std::filesystem::path AssetBrowser::makeUniqueAssetPath(const char* stem, const char* ext) const
{
	std::error_code ec;
	std::filesystem::path candidate = m_currentPath / (std::string(stem) + ext);
	if (!std::filesystem::exists(candidate, ec))
		return candidate;
	for (int i = 1; i < 1000; ++i)
	{
		candidate = m_currentPath / (stem + std::to_string(i) + ext);
		if (!std::filesystem::exists(candidate, ec))
			return candidate;
	}
	return candidate;
}

void AssetBrowser::renderRenamePopup()
{
	static const char* popupId = "Rename##ab_rename";
	if (m_openRenamePopup)
	{
		ImGui::OpenPopup(popupId);
		m_openRenamePopup = false;
	}

	const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextUnformatted("New name:");
	ImGui::SetNextItemWidth(280.0f);
	const bool entered = ImGui::InputText("##ab_rename_input", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

	const bool hasName = m_renameBuf[0] != '\0';
	std::error_code ec;
	std::filesystem::path dest;
	bool conflict = false;
	if (hasName)
	{
		dest = m_renameTarget.parent_path() / m_renameBuf;
		conflict = dest != m_renameTarget && std::filesystem::exists(dest, ec);
	}
	if (conflict)
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "A file with that name already exists.");

	ImGui::Separator();
	const bool confirm = ImGui::Button("Rename", ImVec2(120.0f, 0.0f)) || entered;
	ImGui::SameLine();
	const bool cancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f));

	if (confirm && hasName && !conflict)
	{
		if (dest != m_renameTarget)
		{
			std::filesystem::rename(m_renameTarget, dest, ec);
			if (!ec)
				m_selectedPath = dest;
		}
		ImGui::CloseCurrentPopup();
	}
	if (cancel)
		ImGui::CloseCurrentPopup();

	ImGui::EndPopup();
}
