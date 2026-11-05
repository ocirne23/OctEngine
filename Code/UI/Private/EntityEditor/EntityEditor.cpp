module UI;

import Core;
import Core.Log;
import Core.Rect;
import Core.imgui;
import Core.glm;
import Entity;
import File;
import :EntityEditor;

static constexpr size_t c_maxUndoSteps = 128;
static constexpr const char* c_docNodePayload = "ENTITY_DOC_NODE";

static bool containsCaseInsensitive(const oc::string& haystack, const char* needle)
{
	if (needle[0] == '\0')
		return true;
	oc::string h = haystack, n = needle;
	for (char& c : h) c = (char)std::tolower((unsigned char)c);
	for (char& c : n) c = (char)std::tolower((unsigned char)c);
	return h.find(n) != oc::string::npos;
}

static bool isPrefabFile(const oc::string& path)
{
	return path.size() > 4 && path.compare(path.size() - 4, 4, ".pre") == 0;
}

// Relative to Assets/, the form the registry and the .pre files use. A drag from the browser can carry an absolute one.
static oc::string assetRelativePath(const oc::string& path)
{
	if (!FileSystem::isAbsolute(path))
		return FileSystem::normalize(path);
	const oc::string relative = FileSystem::relativePath(path, oc::string(), /*allowMainThread*/ true);
	return relative.empty() ? path : relative;
}

// ---- The document ----

oc::string EntityEditor::documentId() const
{
	if (!m_path.empty())
		return FileSystem::stem(m_path);
	return (m_root && !m_root->name.empty()) ? m_root->name : oc::string("NewEntity");
}

void EntityEditor::setDocument(EntityDocNode&& root, const oc::string& path)
{
	m_root = oc::make_unique<EntityDocNode>(oc::move(root));
	m_selectedId = m_root->id;
	m_path = path;
	m_baselineText = serializeEntityDoc(*m_root, documentId());
	m_dirty = path.empty(); // a document with no file has something to save
	m_editPending = false;
	m_committed = *m_root;
	m_undoStack.clear();
	m_redoStack.clear();
	m_renameNodeId = 0;
	m_deferredTreeAction = nullptr;
}

void EntityEditor::closeDocument()
{
	m_root.reset();
	m_selectedId = 0;
	m_path.clear();
	m_baselineText.clear();
	m_dirty = false;
	m_editPending = false;
	m_committed = EntityDocNode{};
	m_undoStack.clear();
	m_redoStack.clear();
	m_renameNodeId = 0;
	m_deferredTreeAction = nullptr;
}

void EntityEditor::commitPendingEdit()
{
	if (!m_editPending || !m_root || ImGui::IsAnyItemActive())
		return;
	m_editPending = false;
	if (m_undoStack.size() >= c_maxUndoSteps)
		m_undoStack.erase(m_undoStack.begin());
	m_undoStack.push_back(oc::move(m_committed));
	m_redoStack.clear();
	m_committed = *m_root;
	m_dirty = m_path.empty() || serializeEntityDoc(*m_root, documentId()) != m_baselineText;
}

void EntityEditor::undo()
{
	if (m_editPending)
		commitPendingEdit();
	if (m_undoStack.empty() || m_editPending)
		return;
	m_redoStack.push_back(oc::move(m_committed));
	m_committed = oc::move(m_undoStack.back());
	m_undoStack.pop_back();
	*m_root = m_committed;
	if (!findDocNode(*m_root, m_selectedId))
		m_selectedId = m_root->id;
	m_dirty = m_path.empty() || serializeEntityDoc(*m_root, documentId()) != m_baselineText;
}

void EntityEditor::redo()
{
	if (m_redoStack.empty() || m_editPending)
		return;
	m_undoStack.push_back(oc::move(m_committed));
	m_committed = oc::move(m_redoStack.back());
	m_redoStack.pop_back();
	*m_root = m_committed;
	if (!findDocNode(*m_root, m_selectedId))
		m_selectedId = m_root->id;
	m_dirty = m_path.empty() || serializeEntityDoc(*m_root, documentId()) != m_baselineText;
}

// ---- Main-thread work ----

void EntityEditor::flushMainThreadWork()
{
	for (const PendingDocLoad& load : m_docLoads)
	{
		AssetNode doc;
		oc::string error;
		const AssetNode* declaration = parseAssetText(load.text, doc, error) ? doc.find("Prefab") : nullptr;
		oc::shared_ptr<const EntitySpawnTemplate> tmpl = declaration ? Globals::world.buildDetachedTemplate(*declaration) : nullptr;
		if (!tmpl)
		{
			m_message = "Could not load '" + load.path + "'. " + error;
			m_openMessagePopup = true;
			continue;
		}
		EntityDocNode root = makeDocNode(*tmpl, m_nextId);
		root.prefabRef.clear();
		setDocument(oc::move(root), load.path);
		if (!load.keepView)
			m_framePending = true;
	}
	m_docLoads.clear();

	for (const PendingChildLoad& load : m_childLoads)
	{
		EntityDocNode* parent = m_root ? findDocNode(*m_root, load.parentId) : nullptr;
		oc::shared_ptr<const EntitySpawnTemplate> tmpl = parent ? Globals::world.resolveAssetTemplate(load.path) : nullptr;
		if (!tmpl)
			continue;
		EntityDocNode child = makeDocNode(*tmpl, m_nextId);
		child.transform = Transform(); // the reference site's placement: at the parent
		const uint64 childId = child.id;
		parent->hasScene = true;
		parent->children.push_back(oc::move(child));
		m_selectedId = childId;
		markEdited();
	}
	m_childLoads.clear();

	for (const oc::string& name : m_containerLoadRequests)
		Globals::world.getOrLoadContainer(name);
	m_containerLoadRequests.clear();
}

// ---- Open, save, close ----

void EntityEditor::requestOpen(const oc::string& rawPath)
{
	if (!isPrefabFile(rawPath))
		return;
	const oc::string path = assetRelativePath(rawPath);
	if (m_root && (m_dirty || m_editPending))
	{
		m_pendingSwitch = PendingSwitch::OpenPath;
		m_pendingSwitchPath = path;
		m_openUnsavedPopup = true;
		return;
	}
	doOpen(path);
}

void EntityEditor::requestNew(const oc::string& name)
{
	if (m_root && (m_dirty || m_editPending))
	{
		m_pendingSwitch = PendingSwitch::New;
		m_pendingSwitchName = name;
		m_openUnsavedPopup = true;
		return;
	}
	doNew(name);
}

void EntityEditor::requestOpenSelected(Entity* entity)
{
	if (!entity)
		return;
	m_pendingSwitchEntity = EntityPtr(entity);
	if (m_root && (m_dirty || m_editPending))
	{
		m_pendingSwitch = PendingSwitch::OpenSelected;
		m_openUnsavedPopup = true;
		return;
	}
	doOpenSelected();
}

void EntityEditor::requestClose()
{
	if (!m_root)
		return;
	if (m_dirty || m_editPending)
	{
		m_pendingSwitch = PendingSwitch::Close;
		m_openUnsavedPopup = true;
		return;
	}
	closeDocument();
}

void EntityEditor::runSwitch(PendingSwitch what)
{
	switch (what)
	{
	case PendingSwitch::New:          doNew(m_pendingSwitchName); break;
	case PendingSwitch::OpenPath:     doOpen(m_pendingSwitchPath); break;
	case PendingSwitch::OpenSelected: doOpenSelected(); break;
	case PendingSwitch::Close:        closeDocument(); break;
	default: break;
	}
}

void EntityEditor::doOpen(const oc::string& path)
{
	oc::string text = FileSystem::readFileStr(path);
	if (text.empty())
	{
		m_message = "Could not read '" + path + "'.";
		m_openMessagePopup = true;
		return;
	}
	m_docLoads.push_back({ path, oc::move(text) });
}

void EntityEditor::doNew(const oc::string& name)
{
	EntityDocNode root;
	root.id = m_nextId++;
	root.name = name;
	root.hasScene = true; // a new entity can hold children from the start
	setDocument(oc::move(root), oc::string());
	m_framePending = true;
}

// A scene entity with a registered file opens that file. Anything else opens as a copy of what it is now, with no file. The live entity stays untouched either way.
void EntityEditor::doOpenSelected()
{
	Entity* entity = m_pendingSwitchEntity.get();
	if (!entity)
		return;
	if (entity->isPrefabInstance())
		if (const oc::string* path = Globals::assetRegistry.findPrefab(entity->getPrefabName()))
		{
			doOpen(*path);
			m_pendingSwitchEntity = EntityPtr();
			return;
		}
	const oc::string name = entity->hasName() ? oc::string(entity->getName()) : oc::string("Entity");
	m_docLoads.push_back({ oc::string(), serializePrefabText(entity, name) });
	m_pendingSwitchEntity = EntityPtr();
}

void EntityEditor::trySave(const oc::string& path)
{
	if (!m_root || path.empty())
		return;
	const oc::string normalized = assetRelativePath(path);
	const oc::string text = serializeEntityDoc(*m_root, FileSystem::stem(normalized));
	if (prefabTextWouldCycle(text, FileSystem::stem(normalized)))
	{
		m_message = "'" + FileSystem::stem(normalized) + "' contains a prefab reference that leads back to itself. Remove that reference to save under this name.";
		m_openMessagePopup = true;
		return;
	}
	if (normalized != m_path && FileSystem::exists(normalized, /*allowMainThread*/ true))
	{
		m_pendingSavePath = normalized;
		m_openOverwritePopup = true;
		return;
	}
	queueSave(normalized);
}

void EntityEditor::queueSave(const oc::string& path)
{
	m_path = path;
	m_baselineText = serializeEntityDoc(*m_root, documentId());
	m_changes.push_back({ EntityChange::SavePrefabText{ path, m_baselineText, m_updateInstancesOnSave } });
	m_dirty = false;
	m_prefabNamesDirty = true;
}

void EntityEditor::revert()
{
	if (m_path.empty())
		return;
	oc::string text = FileSystem::readFileStr(m_path);
	if (text.empty())
	{
		m_message = "Could not read '" + m_path + "'.";
		m_openMessagePopup = true;
		return;
	}
	m_docLoads.push_back({ m_path, oc::move(text), /*keepView*/ true });
}

// ---- Tree edits ----

void EntityEditor::addChild(uint64 parentId)
{
	EntityDocNode* parent = findDocNode(*m_root, parentId);
	if (!parent || parent->isReference())
		return;
	EntityDocNode child;
	child.id = m_nextId++;
	child.name = "Entity";
	parent->hasScene = true; // adding a child makes the entity able to hold it
	m_selectedId = child.id;
	m_renameNodeId = child.id;
	strncpy_s(m_renameBuf, sizeof(m_renameBuf), child.name.c_str(), sizeof(m_renameBuf) - 1);
	m_renameFocusPending = true;
	parent->children.push_back(oc::move(child));
	markEdited();
}

void EntityEditor::removeNode(uint64 id)
{
	EntityDocNode* parent = findDocParent(*m_root, id);
	if (!parent)
		return; // the root
	oc::erase_if(parent->children, [id](const EntityDocNode& child) { return child.id == id; });
	if (!findDocNode(*m_root, m_selectedId))
		m_selectedId = parent->id;
	markEdited();
}

void EntityEditor::duplicateNode(uint64 id)
{
	EntityDocNode* parent = findDocParent(*m_root, id);
	EntityDocNode* node = findDocNode(*m_root, id);
	if (!parent || !node)
		return;
	EntityDocNode copy = *node;
	assignDocIds(copy, m_nextId);
	copy.name += " Copy";
	m_selectedId = copy.id;
	const size_t index = size_t(node - parent->children.data());
	parent->children.insert(parent->children.begin() + index + 1, oc::move(copy));
	markEdited();
}

void EntityEditor::copyNode(uint64 id)
{
	if (EntityDocNode* node = findDocNode(*m_root, id))
		m_nodeClipboard = oc::make_unique<EntityDocNode>(*node);
}

void EntityEditor::pasteNode(uint64 parentId)
{
	EntityDocNode* parent = findDocNode(*m_root, parentId);
	if (!parent || parent->isReference() || !m_nodeClipboard)
		return;
	EntityDocNode copy = *m_nodeClipboard;
	assignDocIds(copy, m_nextId);
	m_selectedId = copy.id;
	parent->hasScene = true;
	parent->children.push_back(oc::move(copy));
	markEdited();
}

void EntityEditor::moveNode(uint64 id, int delta)
{
	EntityDocNode* parent = findDocParent(*m_root, id);
	EntityDocNode* node = findDocNode(*m_root, id);
	if (!parent || !node)
		return;
	const int index = int(node - parent->children.data());
	const int target = index + delta;
	if (target < 0 || target >= int(parent->children.size()))
		return;
	oc::swap(parent->children[index], parent->children[target]);
	markEdited();
}

void EntityEditor::reparentNode(uint64 id, uint64 newParentId)
{
	EntityDocNode* node = findDocNode(*m_root, id);
	EntityDocNode* oldParent = findDocParent(*m_root, id);
	if (!node || !oldParent || oldParent->id == newParentId || docSubtreeContains(*node, newParentId))
		return;
	EntityDocNode moved = oc::move(*node);
	oc::erase_if(oldParent->children, [id](const EntityDocNode& child) { return child.id == id; });
	EntityDocNode* newParent = findDocNode(*m_root, newParentId); // after the erase: it may have moved
	if (!newParent || newParent->isReference())
		return;
	newParent->hasScene = true;
	newParent->children.push_back(oc::move(moved));
	markEdited();
}

void EntityEditor::unpackNode(uint64 id)
{
	if (EntityDocNode* node = findDocNode(*m_root, id))
	{
		node->prefabRef.clear();
		markEdited();
	}
}

void EntityEditor::requestPrefabChild(uint64 parentId, const oc::string& rawPath)
{
	if (!isPrefabFile(rawPath))
		return;
	const oc::string path = assetRelativePath(rawPath);
	if (FileSystem::stem(path) == documentId())
	{
		m_message = "An entity cannot contain a reference to itself.";
		m_openMessagePopup = true;
		return;
	}
	m_childLoads.push_back({ parentId, path });
}

// ---- Popups ----

void EntityEditor::renderOverwritePopup()
{
	if (m_openOverwritePopup)
	{
		ImGui::OpenPopup("Overwrite Entity File?");
		m_openOverwritePopup = false;
	}
	if (ImGui::BeginPopupModal("Overwrite Entity File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("'%s' already exists. Overwrite it?", m_pendingSavePath.c_str());
		ImGui::Separator();
		if (ImGui::Button("Overwrite"))
		{
			queueSave(m_pendingSavePath);
			m_pendingSavePath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_pendingSavePath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void EntityEditor::renderUnsavedPopup()
{
	if (m_openUnsavedPopup)
	{
		ImGui::OpenPopup("Unsaved Entity Changes");
		m_openUnsavedPopup = false;
	}
	if (ImGui::BeginPopupModal("Unsaved Entity Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("'%s' has unsaved changes.", documentId().c_str());
		switch (m_pendingSwitch)
		{
		case PendingSwitch::New:          ImGui::TextUnformatted("Start a new entity?"); break;
		case PendingSwitch::OpenSelected: ImGui::TextUnformatted("Open the selected entity?"); break;
		case PendingSwitch::Close:        ImGui::TextUnformatted("Close it?"); break;
		default:                          ImGui::Text("Open '%s'?", m_pendingSwitchPath.c_str()); break;
		}
		ImGui::Separator();
		const bool save = ImGui::Button("Save");
		ImGui::SameLine();
		const bool discard = ImGui::Button("Discard");
		ImGui::SameLine();
		const bool cancel = ImGui::Button("Cancel");

		if (save)
			trySave(m_path.empty() ? "Entities/" + documentId() + ".pre" : m_path);
		if (save || discard)
			runSwitch(m_pendingSwitch);
		if (save || discard || cancel)
		{
			m_pendingSwitch = PendingSwitch::None;
			m_pendingSwitchPath.clear();
			m_pendingSwitchName.clear();
			m_pendingSwitchEntity = EntityPtr();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void EntityEditor::renderSaveAsPopup()
{
	if (m_openSaveAsPopup)
	{
		const oc::string suggested = m_path.empty() ? "Entities/" + documentId() + ".pre" : m_path;
		strncpy_s(m_saveAsBuf, sizeof(m_saveAsBuf), suggested.c_str(), sizeof(m_saveAsBuf) - 1);
		ImGui::OpenPopup("Save Entity As");
		m_openSaveAsPopup = false;
	}
	if (ImGui::BeginPopupModal("Save Entity As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("Path, relative to Assets/. The file name is the prefab's name.");
		ImGui::SetNextItemWidth(420.0f);
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		const bool entered = ImGui::InputText("##path", m_saveAsBuf, sizeof(m_saveAsBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		oc::string path = m_saveAsBuf;
		if (!path.empty() && !isPrefabFile(path))
			path += ".pre";
		ImGui::Separator();
		ImGui::BeginDisabled(path.empty());
		if (ImGui::Button("Save") || (entered && !path.empty()))
		{
			trySave(path);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void EntityEditor::renderMessagePopup()
{
	if (m_openMessagePopup)
	{
		ImGui::OpenPopup("Entity Editor##message");
		m_openMessagePopup = false;
	}
	if (ImGui::BeginPopupModal("Entity Editor##message", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
		ImGui::TextUnformatted(m_message.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Separator();
		if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// ---- The toolbar ----

// A row that wraps instead of clipping when the panel is narrow.
static void toolbarNext(const char* nextLabel)
{
	const ImGuiStyle& style = ImGui::GetStyle();
	const float nextWidth = ImGui::CalcTextSize(nextLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
	if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
		ImGui::SameLine();
}

void EntityEditor::renderToolbar(const Rect& viewportRect)
{
	if (ImGui::Button("New"))
		requestNew("NewEntity");
	toolbarNext("Open...");
	if (ImGui::Button("Open..."))
	{
		m_openOpenPopup = true;
		m_prefabSearch[0] = '\0';
	}
	renderOpenPopup();

	toolbarNext("Open Selected");
	ImGui::BeginDisabled(m_sceneSelection == nullptr);
	if (ImGui::Button("Open Selected"))
		requestOpenSelected(m_sceneSelection);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(m_sceneSelection ? "Open the Scene panel's selection: its prefab file, or a copy when it has none.\nThe live entity is not changed."
			: "Select an entity in the Scene panel first");

	if (!m_root)
		return;

	toolbarNext("Save");
	if (ImGui::Button("Save"))
	{
		if (m_path.empty())
			m_openSaveAsPopup = true;
		else
			trySave(m_path);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Ctrl+S");
	toolbarNext("Save As...");
	if (ImGui::Button("Save As..."))
		m_openSaveAsPopup = true;

	toolbarNext("Revert");
	ImGui::BeginDisabled(m_path.empty() || !m_dirty);
	if (ImGui::Button("Revert"))
		revert();
	ImGui::EndDisabled();

	toolbarNext("Undo");
	ImGui::BeginDisabled(m_undoStack.empty() && !m_editPending);
	if (ImGui::Button("Undo"))
		undo();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Ctrl+Z");
	toolbarNext("Redo");
	ImGui::BeginDisabled(m_redoStack.empty());
	if (ImGui::Button("Redo"))
		redo();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Ctrl+Y");

	toolbarNext("Spawn in Scene");
	ImGui::BeginDisabled(m_path.empty() || m_dirty);
	if (ImGui::Button("Spawn in Scene"))
	{
		const glm::ivec2 center = (viewportRect.min + viewportRect.max) / 2;
		m_changes.push_back({ EntityChange::CreateViewport{ center, m_path } });
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Spawn the saved prefab at the viewport's centre.\nSave first: the world spawns what is on disk.");

	toolbarNext("Reveal");
	ImGui::BeginDisabled(m_path.empty());
	if (ImGui::Button("Reveal"))
		m_revealRequest = m_path;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Select the file in Content");

	toolbarNext("Close");
	if (ImGui::Button("Close"))
		requestClose();

	toolbarNext("Update scene on save");
	ImGui::Checkbox("Update scene on save", &m_updateInstancesOnSave);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("A save also respawns this prefab's live instances in the world, in place.\nOff: only the next spawn gets the new version.");

	// The file and its state, on a line of their own.
	const bool dirty = m_dirty || m_editPending;
	ImGui::TextColored(dirty ? ImVec4(1.0f, 0.72f, 0.3f, 1.0f) : ImVec4(0.55f, 0.8f, 0.55f, 1.0f), dirty ? "Unsaved" : "Saved");
	ImGui::SameLine();
	if (m_path.empty())
		ImGui::TextDisabled("%s  (no file yet)", documentId().c_str());
	else
		ImGui::TextDisabled("%s", m_path.c_str());
}

// Every registered prefab, searchable: the popup's and the empty panel's list.
void EntityEditor::renderPrefabList(float height)
{
	if (m_prefabNamesDirty)
	{
		m_prefabNames.clear();
		for (const auto& kv : Globals::assetRegistry.getPrefabs())
			m_prefabNames.push_back(kv.first);
		oc::sort(m_prefabNames.begin(), m_prefabNames.end());
		m_prefabNamesDirty = false;
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##prefabsearch", "Search prefabs...", m_prefabSearch, sizeof(m_prefabSearch));
	ImGui::BeginChild("##prefablist", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	for (const oc::string& name : m_prefabNames)
	{
		if (!containsCaseInsensitive(name, m_prefabSearch))
			continue;
		const oc::string* path = Globals::assetRegistry.findPrefab(name);
		if (ImGui::Selectable(name.c_str()) && path)
		{
			requestOpen(*path);
			ImGui::CloseCurrentPopup(); // nothing when the list is not in a popup
		}
		if (path && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", path->c_str());
	}
	ImGui::EndChild();
}

void EntityEditor::renderOpenPopup()
{
	if (m_openOpenPopup)
	{
		ImGui::OpenPopup("##ee_open");
		m_openOpenPopup = false;
		m_prefabNamesDirty = true;
	}
	ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f));
	if (ImGui::BeginPopup("##ee_open"))
	{
		renderPrefabList(300.0f);
		ImGui::EndPopup();
	}
}

void EntityEditor::renderEmptyState()
{
	ImGui::Dummy(ImVec2(0.0f, 6.0f));
	ImGui::TextUnformatted("No entity open.");
	ImGui::TextDisabled("Pick a prefab, press New, or drag a .pre from Content onto this panel.");
	ImGui::TextDisabled("It opens here only. Nothing is added to the scene until you ask for it.");
	ImGui::Dummy(ImVec2(0.0f, 4.0f));
	renderPrefabList(oc::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4.0f, 120.0f));
}

// ---- The hierarchy ----

void EntityEditor::renderTreeNode(EntityDocNode& node)
{
	ImGui::PushID((void*)(uintptr_t)node.id);
	const uint64 nodeId = node.id;
	const bool isRoot = nodeId == m_root->id;
	const bool isLocked = node.isReference();
	const bool hasChildren = !isLocked && !node.children.empty();

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding;
	if (!hasChildren)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (m_selectedId == nodeId)
		flags |= ImGuiTreeNodeFlags_Selected;

	m_problems.clear();
	collectDocProblems(node, m_problems);
	const char* label = node.name.empty() ? "Entity" : node.name.c_str();

	bool open = false;
	if (m_renameNodeId == nodeId)
	{
		// In place of the row's text, after its arrow.
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (m_renameFocusPending)
		{
			ImGui::SetKeyboardFocusHere();
			m_renameFocusPending = false;
		}
		const bool entered = ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			m_renameNodeId = 0;
		else if (entered || ImGui::IsItemDeactivated())
		{
			if (m_renameBuf[0] != '\0' && node.name != m_renameBuf)
			{
				node.name = m_renameBuf;
				markEdited();
			}
			m_renameNodeId = 0;
		}
		if (hasChildren)
		{
			// The rename field replaced the tree node, so its children draw indented by hand.
			ImGui::Indent();
			for (EntityDocNode& child : node.children)
				renderTreeNode(child);
			ImGui::Unindent();
		}
		ImGui::PopID();
		return;
	}

	ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	if (isLocked)
		textColor = ImVec4(0.45f, 0.62f, 0.95f, 1.0f);
	if (!node.enabled)
		textColor.w *= 0.45f;
	if (!m_problems.empty())
		textColor = ImVec4(1.0f, 0.72f, 0.3f, node.enabled ? 1.0f : 0.45f);
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	open = ImGui::TreeNodeEx("##node", flags, "%s%s", label, isLocked ? "  (prefab)" : "");
	ImGui::PopStyleColor();

	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		m_selectedId = nodeId;
	if (!m_problems.empty() && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", m_problems[0].c_str());

	if (!isRoot && ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload(c_docNodePayload, &nodeId, sizeof(nodeId));
		ImGui::TextUnformatted(label);
		ImGui::EndDragDropSource();
	}
	if (!isLocked && ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(c_docNodePayload))
		{
			const uint64 movedId = *static_cast<const uint64*>(payload->Data);
			m_deferredTreeAction = [this, movedId, nodeId]() { reparentNode(movedId, nodeId); };
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
			requestPrefabChild(nodeId, static_cast<const char*>(payload->Data));
		ImGui::EndDragDropTarget();
	}

	if (ImGui::BeginPopupContextItem("##tree_ctx"))
	{
		m_selectedId = nodeId;
		if (!isLocked && ImGui::MenuItem("Add Child"))
			m_deferredTreeAction = [this, nodeId]() { addChild(nodeId); };
		if (!isLocked && ImGui::MenuItem("Paste as Child", "Ctrl+V", false, m_nodeClipboard != nullptr))
			m_deferredTreeAction = [this, nodeId]() { pasteNode(nodeId); };
		ImGui::Separator();
		if (ImGui::MenuItem("Rename", "F2"))
		{
			m_renameNodeId = nodeId;
			strncpy_s(m_renameBuf, sizeof(m_renameBuf), node.name.c_str(), sizeof(m_renameBuf) - 1);
			m_renameFocusPending = true;
		}
		if (ImGui::MenuItem("Copy", "Ctrl+C"))
			copyNode(nodeId);
		if (!isRoot)
		{
			if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
				m_deferredTreeAction = [this, nodeId]() { duplicateNode(nodeId); };
			if (ImGui::MenuItem("Move Up", "Alt+Up"))
				m_deferredTreeAction = [this, nodeId]() { moveNode(nodeId, -1); };
			if (ImGui::MenuItem("Move Down", "Alt+Down"))
				m_deferredTreeAction = [this, nodeId]() { moveNode(nodeId, 1); };
		}
		if (isLocked)
		{
			ImGui::Separator();
			if (ImGui::MenuItem("Unpack Prefab"))
				m_deferredTreeAction = [this, nodeId]() { unpackNode(nodeId); };
			if (ImGui::MenuItem("Open Prefab"))
				if (const oc::string* path = Globals::assetRegistry.findPrefab(node.prefabRef))
					requestOpen(*path);
		}
		if (!isRoot)
		{
			ImGui::Separator();
			if (ImGui::MenuItem("Delete", "Del"))
				m_deferredTreeAction = [this, nodeId]() { removeNode(nodeId); };
		}
		ImGui::EndPopup();
	}

	if (open && hasChildren)
	{
		for (EntityDocNode& child : node.children)
			renderTreeNode(child);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void EntityEditor::renderTree()
{
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f));
	renderTreeNode(*m_root);
	ImGui::PopStyleVar();

	// A click on the empty area under the rows selects the root. Also the target for dropping onto the document as a whole.
	const ImVec2 rest = ImGui::GetContentRegionAvail();
	if (rest.y > 4.0f)
	{
		ImGui::InvisibleButton("##tree_rest", rest);
		if (ImGui::IsItemClicked())
			m_selectedId = m_root->id;
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(c_docNodePayload))
			{
				const uint64 movedId = *static_cast<const uint64*>(payload->Data);
				const uint64 rootId = m_root->id;
				m_deferredTreeAction = [this, movedId, rootId]() { reparentNode(movedId, rootId); };
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
				requestPrefabChild(m_root->id, static_cast<const char*>(payload->Data));
			ImGui::EndDragDropTarget();
		}
	}

	if (m_deferredTreeAction)
	{
		oc::function<void()> action = oc::move(m_deferredTreeAction);
		m_deferredTreeAction = nullptr;
		action();
	}
}

// ---- Shortcuts ----

void EntityEditor::handleShortcuts()
{
	if (!m_root || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
		return;
	const ImGuiIO& io = ImGui::GetIO();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
	{
		if (m_path.empty())
			m_openSaveAsPopup = true;
		else
			trySave(m_path);
	}
	if (io.WantTextInput)
		return; // a focused text field keeps its own undo and clipboard keys
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
	{
		if (io.KeyShift)
			redo();
		else
			undo();
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
		redo();

	const uint64 id = m_selectedId;
	const bool isRoot = id == m_root->id;
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
		copyNode(id);
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
		pasteNode(id);
	if (!isRoot && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		duplicateNode(id);
	if (!isRoot && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		removeNode(id);
	if (!isRoot && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
		moveNode(id, -1);
	if (!isRoot && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
		moveNode(id, 1);
	if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
		if (EntityDocNode* node = selectedNode())
		{
			m_renameNodeId = id;
			strncpy_s(m_renameBuf, sizeof(m_renameBuf), node->name.c_str(), sizeof(m_renameBuf) - 1);
			m_renameFocusPending = true;
		}
}

// ---- The panel ----

void EntityEditor::render(Entity* sceneSelection, const Rect& viewportRect)
{
	m_sceneSelection = sceneSelection;

	handleShortcuts();
	renderToolbar(viewportRect);
	ImGui::Separator();

	if (!m_root)
		renderEmptyState();
	else
	{
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		// Side by side when there is room for both, the preview on top when the panel is narrow. Both splitters are the child windows' own resize edges.
		const bool sideBySide = avail.x >= 520.0f;
		if (sideBySide)
		{
			ImGui::BeginChild("##ee_edit", ImVec2(avail.x * 0.5f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
			ImGui::BeginChild("##ee_tree", ImVec2(0.0f, 150.0f), ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);
			renderTree();
			ImGui::EndChild();
			ImGui::BeginChild("##ee_props");
			renderProperties();
			ImGui::EndChild();
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##ee_preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			renderPreview();
			ImGui::EndChild();
		}
		else
		{
			ImGui::BeginChild("##ee_preview_top", ImVec2(0.0f, oc::max(avail.y * 0.4f, 120.0f)), ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			renderPreview();
			ImGui::EndChild();
			ImGui::BeginChild("##ee_tree", ImVec2(0.0f, 120.0f), ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);
			renderTree();
			ImGui::EndChild();
			ImGui::BeginChild("##ee_props");
			renderProperties();
			ImGui::EndChild();
		}
	}

	renderOverwritePopup();
	renderUnsavedPopup();
	renderSaveAsPopup();
	renderMessagePopup();

	commitPendingEdit();
}
