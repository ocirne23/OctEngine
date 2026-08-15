module UI;

import Core;
import Core.imgui;
import Core.Time;
import Entity;
import File;
import :AssetBrowser;

// Splits a relative path into its components ("a/b/c" -> {a, b, c}) — the breadcrumb walks these.
static std::vector<std::string> splitPathComponents(const std::string& path)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= path.size())
	{
		const size_t sep = path.find_first_of("/\\", start);
		const std::string part = path.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
		if (!part.empty())
			parts.push_back(part);
		if (sep == std::string::npos)
			break;
		start = sep + 1;
	}
	return parts;
}

// Case-insensitive substring match against the search box (empty filter passes everything).
static bool passesFilter(const std::string& name, const char* filter)
{
	if (filter == nullptr || filter[0] == '\0')
		return true;
	const std::string_view needle(filter);
	const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(),
		[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
	return it != name.end();
}

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

// NOTE: isDirectory + ext come from the cached listing — these used to call
// std::filesystem::is_directory() PER ITEM PER FRAME (a syscall each).
static const char* fileIcon(bool isDirectory, const std::string& ext)
{
	if (isDirectory) return "[Dir]";
	if (isImageFile(ext))                  return "[Img]";
	if (isMeshFile(ext))                   return "[Msh]";
	if (isShaderFile(ext))                 return "[Shd]";
	if (isPrefabFile(ext))                 return "[Pre]";
	if (isObjectContainer(ext))            return "[OC]";
	if (isScriptFile(ext))                 return "[Scr]";
	if (isTextFile(ext))                   return "[Txt]";
	return "[Fil]";
}

static ImVec4 fileColor(bool isDirectory, const std::string& ext)
{
	if (isDirectory) return ImVec4(1.0f, 0.85f, 0.4f, 1.0f);   // yellow
	if (isImageFile(ext))                   return ImVec4(0.4f, 0.8f,  1.0f, 1.0f);   // cyan
	if (isMeshFile(ext))                    return ImVec4(0.6f, 1.0f,  0.6f, 1.0f);   // green
	if (isShaderFile(ext))                  return ImVec4(1.0f, 0.6f,  0.3f, 1.0f);   // orange
	if (isPrefabFile(ext))                  return ImVec4(0.9f, 0.5f,  1.0f, 1.0f);   // purple
	if (isObjectContainer(ext))             return ImVec4(0.5f, 1.0f,  0.9f, 1.0f);   // light teal
	if (isScriptFile(ext))                  return ImVec4(0.5f, 1.0f,  0.6f, 1.0f);   // script green
	if (isTextFile(ext))                    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f); // light grey
	return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);                                         // grey
}

static void assetDragSource(const std::string& p, bool isDirectory, const std::string& ext)
{
	if (isDirectory)
		return; // dragging a folder isn't meaningful to any current drop target

	const bool spawnable = ext == ".pre";
	const bool script = isScriptFile(ext);
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
	{
		const std::string path = p;
		// Every file is draggable: spawnable/script (.scr or .dsl) keep their dedicated payload (viewport
		// spawn / script assign), anything else falls back to TEXT_FILE so it can be dropped onto the Script
		// Editor and viewed/edited as plain text.
		const char* payloadTag = spawnable ? "ASSET_FILE" : script ? "SCRIPT_FILE" : "TEXT_FILE";
		ImGui::SetDragDropPayload(payloadTag, path.c_str(), path.size() + 1);
		ImGui::Text(spawnable ? "Spawn %s" : script ? "Assign %s" : "View %s as text", FileSystem::filename(p).c_str());
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
	m_rootPath = FileSystem::canonicalPath(FileSystem::currentPath(/*allowMainThread*/ true), true);
	if (m_rootPath.empty())
		m_rootPath = FileSystem::currentPath(/*allowMainThread*/ true);
	m_currentPath = m_rootPath;
}

bool AssetBrowser::isWithinRoot(const std::string& path) const
{
	const std::string canonical = FileSystem::weaklyCanonicalPath(path, /*allowMainThread*/ true);
	if (canonical.empty())
		return false;
	const std::string rel = FileSystem::relativePath(canonical, m_rootPath, /*allowMainThread*/ true);
	// anything starting with ".." escapes the root upward
	return !rel.empty() && !rel.starts_with("..");
}

void AssetBrowser::scanDirectory(const std::string& dir, DirListing& out)
{
	ProfileScope scope("Asset dir scan", EProfileCategory::File);
	out.entries.clear();
	out.hasSubDirs = false;
	// The rescan poll runs on the prepare JOB; the first scan of a folder happens inline on the
	// main thread (there is nothing to draw otherwise), so the listing itself is allowed either way.
	std::vector<FileSystem::DirEntry> listed;
	FileSystem::listDirectory(dir, listed, /*allowMainThread*/ true);
	out.entries.reserve(listed.size());
	for (const FileSystem::DirEntry& e : listed)
	{
		DirEntryInfo info;
		info.path = e.path;
		info.name = e.name;
		info.extension = e.extension;
		info.isDirectory = e.isDirectory;
		info.size = e.size;
		if (info.isDirectory)
			out.hasSubDirs = true;
		out.entries.push_back(std::move(info));
	}
	std::sort(out.entries.begin(), out.entries.end(), [](const DirEntryInfo& a, const DirEntryInfo& b)
		{ return a.isDirectory != b.isDirectory ? a.isDirectory : a.name < b.name; });
	out.lastScanSec = Globals::time.getElapsedSec();
}

AssetBrowser::DirListing& AssetBrowser::listing(const std::string& dir)
{
	DirListing& cached = m_dirCache[dir];
	if (cached.lastScanSec < 0.0)
		scanDirectory(dir, cached); // never seen: scan inline, there is nothing to draw otherwise
	cached.touchedFrame = m_frame;  // prepare() only refreshes what a render actually reads
	return cached;
}

void AssetBrowser::invalidateListings()
{
	for (auto& [path, cached] : m_dirCache)
		cached.lastScanSec = -1.0; // next listing() call re-scans (the user just changed something)
}

void AssetBrowser::prepare()
{
	// Periodic refresh of the listings the panel actually drew, on a job. Files also appear from
	// OUTSIDE the browser (prefab saves, compiled scripts, cooked assets), so a poll is the only
	// way to notice — but only while the panel is being used.
	if (!m_active)
		return;
	ProfileScope scope("Asset browser prepare", EProfileCategory::UI);
	const double now = Globals::time.getElapsedSec();
	for (auto& [path, cached] : m_dirCache)
		if (cached.touchedFrame + 2 >= m_frame && now - cached.lastScanSec > RescanIntervalSec)
			scanDirectory(path, cached);
}

void AssetBrowser::render()
{
	++m_frame;
	// Focused OR hovered counts as "in use" — the rescan poll (prepare) runs only then.
	m_active = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		|| ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
	{
		ProfileScope scope("AB toolbar", EProfileCategory::UI);
		renderToolbar();
	}
	ImGui::Separator();

	ImGui::BeginChild("##ab_left", ImVec2(m_leftPaneWidth, 0.0f), ImGuiChildFlags_Borders);
	{
		ProfileScope scope("AB dir tree", EProfileCategory::UI);
		renderDirectoryTree(m_rootPath);
	}
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
	{
		ProfileScope scope("AB drop + menus", EProfileCategory::UI);
		acceptPrefabDrop();
		renderNewAssetContextMenu();
	}
	ImGui::EndChild();

	{
		ProfileScope scope("AB popups", EProfileCategory::UI);
		renderOverwritePopup();
		renderCyclePopup();
		renderRenamePopup();
	}
}

void AssetBrowser::queueSavePrefab(Entity* root, const std::string& path)
{
	invalidateListings(); // the app writes the .pre this frame — show it on the next draw
	std::error_code ec;
	const std::string rel = FileSystem::relativePath(path, std::string(), /*allowMainThread*/ true);
	const std::string savePath = (ec || rel.empty()) ? path : rel;
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
		const std::string out = FileSystem::join(m_currentPath, name + ".pre");
		std::error_code ec;
		if (prefabWouldCycle(entity, name))
		{
			m_pendingSavePath = out; // for the message filename
			m_openCyclePopup = true;
		}
		else if (FileSystem::exists(out, /*allowMainThread*/ true))
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

	ImGui::Text("\"%s\" already exists.\nOverwrite it?", FileSystem::filename(m_pendingSavePath).c_str());
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
		FileSystem::filename(m_pendingSavePath).c_str());
	ImGui::Separator();
	if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void AssetBrowser::renderToolbar()
{
	const bool canGoUp = (m_currentPath != m_rootPath) && !FileSystem::parentPath(m_currentPath).empty();
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
		const std::string rootLabel = FileSystem::filename(m_rootPath).empty()
			? m_rootPath
			: FileSystem::filename(m_rootPath);
		ImGui::PushID("##ab_crumb_root");
		if (ImGui::SmallButton(rootLabel.c_str()))
			navigateTo(m_rootPath);
		ImGui::PopID();

		// LEXICAL, not relativePath(): this runs every frame and std::filesystem::relative resolves
		// both sides through weakly_canonical (real syscalls). Both paths are already canonical.
		const std::string rel = FileSystem::lexicallyRelative(m_currentPath, m_rootPath);
		std::string accumulated = m_rootPath;
		if (!rel.empty() && rel != ".")
		{
			int i = 0;
			for (const std::string& part : splitPathComponents(rel))
			{
				accumulated = FileSystem::join(accumulated, part);
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::TextDisabled("/");
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::PushID(i++);
				if (ImGui::SmallButton(part.c_str()))
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

void AssetBrowser::renderDirectoryTree(const std::string& dir)
{
	// NO is_directory() probe here: it ran per node PER FRAME (a stat syscall each). The caller only
	// ever recurses into entries the cached listing already marked as directories, and a folder that
	// disappeared simply lists empty (drawn as a leaf) until the next rescan notices.
	const bool isCurrent = (dir == m_currentPath);
	DirListing& dirListing = listing(dir); // cached — no per-frame enumeration
	const bool hasSubDirs = dirListing.hasSubDirs;

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
		| ImGuiTreeNodeFlags_SpanAvailWidth
		| ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (isCurrent)  flags |= ImGuiTreeNodeFlags_Selected;
	if (!hasSubDirs) flags |= ImGuiTreeNodeFlags_Leaf;
	if (dir == m_rootPath) flags |= ImGuiTreeNodeFlags_DefaultOpen;

	const std::string label = FileSystem::filename(dir).empty()
		? dir
		: FileSystem::filename(dir);

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	ImGui::PopStyleColor();

	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		navigateTo(dir);

	if (open)
	{
		// The listing is sorted directories-first by name, so this is the same order as before.
		// Copy the paths: the recursion inserts into m_dirCache, and while unordered_map keeps
		// references stable, the vector inside THIS listing must not be read across those inserts.
		std::vector<std::string> subDirs;
		for (const DirEntryInfo& entry : dirListing.entries)
			if (entry.isDirectory)
				subDirs.push_back(entry.path);
		for (const std::string& sub : subDirs)
			renderDirectoryTree(sub);
		ImGui::TreePop();
	}
}

void AssetBrowser::renderContentGrid()
{
	ProfileScope scope("AB grid", EProfileCategory::UI);
	const float cellSize    = m_iconSize + 20.0f;
	const float panelWidth  = ImGui::GetContentRegionAvail().x;
	const int   columnCount = std::max(1, static_cast<int>(panelWidth / cellSize));
	DirListing& dirListing = listing(m_currentPath); // cached + pre-sorted

	if (ImGui::BeginTable("##ab_grid", columnCount, ImGuiTableFlags_None))
	{
		for (DirEntryInfo& entry : dirListing.entries)
		{
			if (!passesFilter(entry.name, m_searchBuf))
				continue;
			ImGui::TableNextColumn();
			const std::string& p = entry.path;
			const std::string& name = entry.name;
			const bool isSelected  = (p == m_selectedPath);

			ImGui::PushID(name.c_str());

			if (isSelected)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			else
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

			ImGui::PushStyleColor(ImGuiCol_Text, fileColor(entry.isDirectory, entry.extension));
			const bool clicked = ImGui::Button(fileIcon(entry.isDirectory, entry.extension), ImVec2(m_iconSize, m_iconSize));
			ImGui::PopStyleColor(2); // Text + Button

			assetDragSource(p, entry.isDirectory, entry.extension);

			if (clicked)
				m_selectedPath = p;

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (entry.isDirectory)
					navigateTo(p);
				else if (isScriptFile(entry.extension))
					m_scriptOpenRequest = p;
				else if (isTextFile(entry.extension))
					m_textOpenRequest = p;
			}

			renderContextMenu(p);

			const float labelWidth = cellSize - 4.0f;
			if (entry.displayWidth != labelWidth) // re-fit only when the icon size changed
			{
				entry.display = truncateLabel(name, labelWidth);
				entry.displayWidth = labelWidth;
			}
			const std::string& display = entry.display;
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
	ProfileScope scope("AB list", EProfileCategory::UI);
	DirListing& dirListing = listing(m_currentPath); // cached + pre-sorted, sizes included

	const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
		| ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##ab_list", 3, tableFlags))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();

		for (const DirEntryInfo& entry : dirListing.entries)
		{
			if (!passesFilter(entry.name, m_searchBuf))
				continue;
			const std::string& p = entry.path;
			const std::string& name        = entry.name;
			const bool isSelected          = (p == m_selectedPath);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);

			ImGui::PushID(name.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, fileColor(entry.isDirectory, entry.extension));
			if (ImGui::Selectable(name.c_str(), isSelected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
				ImVec2(0.0f, 0.0f)))
			{
				m_selectedPath = p;
			}
			ImGui::PopStyleColor();

			assetDragSource(p, entry.isDirectory, entry.extension);

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (entry.isDirectory)
					navigateTo(p);
				else if (isScriptFile(entry.extension))
					m_scriptOpenRequest = p;
				else if (isTextFile(entry.extension))
					m_textOpenRequest = p;
			}

			renderContextMenu(p);

			ImGui::TableSetColumnIndex(1);
			if (entry.isDirectory)
				ImGui::TextDisabled("Folder");
			else
				ImGui::TextDisabled("%s", entry.extension.c_str());

			ImGui::TableSetColumnIndex(2);
			if (!entry.isDirectory) // size cached at scan time (was a syscall per row, per frame)
			{
				const uint64 bytes = entry.size;
				if (bytes < 1024)
					ImGui::TextDisabled("%llu B", bytes);
				else if (bytes < 1024 * 1024)
					ImGui::TextDisabled("%.1f KB", bytes / 1024.0);
				else
					ImGui::TextDisabled("%.1f MB", bytes / (1024.0 * 1024.0));
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

void AssetBrowser::renderContextMenu(const std::string& p)
{
	// (no ProfileScope: this runs PER ITEM — hundreds of records per frame would flood the ring.
	// The early-out below means an unopened popup costs one ImGui call.)
	if (!ImGui::BeginPopupContextItem("##ab_ctx"))
		return;

	m_selectedPath = p;

	if (ImGui::MenuItem("Copy path"))
		ImGui::SetClipboardText(p.c_str());

	if (ImGui::MenuItem("Copy filename"))
		ImGui::SetClipboardText(FileSystem::filename(p).c_str());

	if (ImGui::MenuItem("Rename"))
	{
		m_renameTarget = p;
		const std::string fname = FileSystem::filename(p);
		const size_t n = std::min(fname.size(), sizeof(m_renameBuf) - 1);
		for (size_t i = 0; i < sizeof(m_renameBuf); ++i) m_renameBuf[i] = i < n ? fname[i] : '\0';
		m_openRenamePopup = true;
	}

	ImGui::Separator();

#if defined(_WIN32)
	if (ImGui::MenuItem("Show in Explorer"))
	{
		const std::string cmd = "explorer /select,\"" + p + "\"";
		std::system(cmd.c_str());
	}
#endif

	if (isScriptFile(FileSystem::extension(p)))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open Script"))
			m_scriptOpenRequest = p;
	}

	if (isTextFile(FileSystem::extension(p)))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open Text File"))
			m_textOpenRequest = p;
	}

	if (isPrefabFile(FileSystem::extension(p)))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Edit Entity"))
			m_entityEditRequest = p;
	}

	if (FileSystem::isDirectory(p, /*allowMainThread*/ true))
	{
		ImGui::Separator();
		if (ImGui::MenuItem("Open folder"))
			navigateTo(p);
	}

	ImGui::EndPopup();
}

void AssetBrowser::navigateTo(const std::string& path)
{
	std::error_code ec;
	if (FileSystem::isDirectory(path, /*allowMainThread*/ true) && isWithinRoot(path))
	{
		m_currentPath  = FileSystem::canonicalPath(path, /*allowMainThread*/ true);
		m_selectedPath.clear();
		m_searchBuf[0] = '\0';
		// Entering a folder is the one moment a stale listing would be most visible: re-scan it
		// on the spot (one directory, on a click — not per frame).
		scanDirectory(m_currentPath, m_dirCache[m_currentPath]);
	}
}

void AssetBrowser::selectFile(const std::string& path)
{
	const std::string abs = FileSystem::canonicalPath(path, /*allowMainThread*/ true);
	if (abs.empty() || FileSystem::parentPath(abs).empty())
		return;
	navigateTo(FileSystem::parentPath(abs));
	m_selectedPath = abs;
}

void AssetBrowser::navigateUp()
{
	if (m_currentPath != m_rootPath && !FileSystem::parentPath(m_currentPath).empty())
		navigateTo(FileSystem::parentPath(m_currentPath));
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
		const std::string path = makeUniqueAssetPath("NewScript", ".dsl");
		if (FileSystem::writeFileStr(path, "//@@dsl 1\n//@@end\n"))
		{
			m_scriptOpenRequest = path;
			invalidateListings(); // the new file must show up now, not at the next poll
		}
	}
	if (ImGui::MenuItem("New Text File"))
	{
		const std::string path = makeUniqueAssetPath("NewText", ".txt");
		if (FileSystem::writeFileStr(path, ""))
		{
			m_textOpenRequest = path;
			invalidateListings();
		}
	}
	if (ImGui::MenuItem("New Prefab"))
	{
		const std::string path = makeUniqueAssetPath("NewPrefab", ".pre");
		const std::string id = FileSystem::stem(path);
		if (FileSystem::writeFileStr(path, "Prefab " + id + "\n"))
		{
			Globals::assetRegistry.addPrefab(id, path);
			m_entityEditRequest = path;
			invalidateListings();
		}
	}
	ImGui::EndPopup();
}

std::string AssetBrowser::makeUniqueAssetPath(const char* stem, const char* ext) const
{
	std::string candidate = FileSystem::join(m_currentPath, std::string(stem) + ext);
	if (!FileSystem::exists(candidate, /*allowMainThread*/ true))
		return candidate;
	for (int i = 1; i < 1000; ++i)
	{
		candidate = FileSystem::join(m_currentPath, stem + std::to_string(i) + ext);
		if (!FileSystem::exists(candidate, /*allowMainThread*/ true))
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
	std::string dest;
	bool conflict = false;
	if (hasName)
	{
		dest = FileSystem::join(FileSystem::parentPath(m_renameTarget), m_renameBuf);
		conflict = dest != m_renameTarget && FileSystem::exists(dest, /*allowMainThread*/ true);
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
			FileSystem::rename(m_renameTarget, dest, /*allowMainThread*/ true);
			if (!ec)
			{
				m_selectedPath = dest;
				invalidateListings();
			}
		}
		ImGui::CloseCurrentPopup();
	}
	if (cancel)
		ImGui::CloseCurrentPopup();

	ImGui::EndPopup();
}
