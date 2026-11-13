module;
// OcScriptField: ScriptModule carries the exposed-field table as void* (it doesn't depend on the script ABI), so
// the Script Data section needs the real struct to read it. In the GLOBAL module fragment -- a plain #include
// after `module UI;` lands in the module purview and collides with the std header units Core re-exports.
#include "ScriptAPI.h"
module UI;

import Core;
import Core.imgui;
import Core.glm;
import Entity;
import Physics;
import File;
import Script; // ScriptHost::getOrLoad + ScriptModule's field table
import :EntityEditor;

// ---- The form widgets: every row is a label in one column and its widget filling the other ----

static constexpr float c_labelWidth = 118.0f;

static void fieldLabel(const char* label, const char* tooltip = nullptr)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (tooltip && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", tooltip);
	ImGui::SameLine(c_labelWidth);
	ImGui::SetNextItemWidth(-FLT_MIN);
}

static bool fieldFloat(const char* label, float& value, float speed, float minValue = 0.0f, float maxValue = 0.0f, const char* tooltip = nullptr, const char* format = "%.3f")
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::DragFloat("##v", &value, speed, minValue, maxValue, format);
	ImGui::PopID();
	return changed;
}

static bool fieldVec3(const char* label, glm::vec3& value, float speed, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::DragFloat3("##v", &value.x, speed);
	ImGui::PopID();
	return changed;
}

static bool fieldColor(const char* label, glm::vec3& value, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::ColorEdit3("##v", &value.x);
	ImGui::PopID();
	return changed;
}

static bool fieldSlider(const char* label, float& value, float minValue, float maxValue, const char* tooltip = nullptr, const char* format = "%.3f")
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::SliderFloat("##v", &value, minValue, maxValue, format);
	ImGui::PopID();
	return changed;
}

static bool fieldInt(const char* label, int& value, float speed, int minValue = 0, int maxValue = 0, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::DragInt("##v", &value, speed, minValue, maxValue);
	ImGui::PopID();
	return changed;
}

static bool fieldCheck(const char* label, bool& value, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::Checkbox("##v", &value);
	ImGui::PopID();
	return changed;
}

static bool fieldCombo(const char* label, int& value, const char* const* items, int count, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = ImGui::Combo("##v", &value, items, count);
	ImGui::PopID();
	return changed;
}

static bool inputString(const char* id, oc::string& value, const char* hint = nullptr)
{
	char buf[256];
	strncpy_s(buf, sizeof(buf), value.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	const bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, sizeof(buf)) : ImGui::InputText(id, buf, sizeof(buf));
	if (changed)
		value = buf;
	return changed;
}

static bool fieldString(const char* label, oc::string& value, const char* tooltip = nullptr, const char* hint = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const bool changed = inputString("##v", value, hint);
	ImGui::PopID();
	return changed;
}

static bool containsCI(const oc::string& haystack, const char* needle)
{
	if (needle[0] == '\0')
		return true;
	oc::string h = haystack, n = needle;
	for (char& c : h) c = (char)std::tolower((unsigned char)c);
	for (char& c : n) c = (char)std::tolower((unsigned char)c);
	return h.find(n) != oc::string::npos;
}

// A label, a text box and a "..." that opens a searchable list. `items` is any range of strings, built when the list is open.
template <typename Fill>
static bool fieldPicker(const char* label, oc::string& value, char* searchBuf, size_t searchBufSize, const Fill& fill, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const float buttonWidth = ImGui::GetFrameHeight() * 1.2f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
	bool changed = inputString("##v", value);
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if (ImGui::Button("...", ImVec2(buttonWidth, 0.0f)))
	{
		ImGui::OpenPopup("##pick");
		searchBuf[0] = '\0';
	}
	if (ImGui::BeginPopup("##pick"))
	{
		ImGui::SetNextItemWidth(300.0f);
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##search", "Search...", searchBuf, searchBufSize);
		ImGui::Separator();
		ImGui::BeginChild("##list", ImVec2(300.0f, 220.0f));
		fill([&](const oc::string& name)
			{
				if (!containsCI(name, searchBuf))
					return;
				if (ImGui::Selectable(name.c_str(), name == value))
				{
					value = name;
					changed = true;
					ImGui::CloseCurrentPopup();
				}
			});
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

// All files under Assets/ with one of the extensions, as forward-slash paths relative to it - the form a .pre stores. Assets/Local holds generated output.
static void gatherAssetFiles(oc::vector<oc::string>& out, std::initializer_list<const char*> extensions)
{
	out.clear();
	const oc::string root = FileSystem::rootPath();
	oc::vector<FileSystem::DirEntry> entries;
	FileSystem::listDirectoryRecursive(root, entries, /*allowMainThread*/ true);
	for (const FileSystem::DirEntry& entry : entries)
	{
		if (entry.isDirectory)
			continue;
		bool wanted = false;
		for (const char* extension : extensions)
			wanted |= entry.extension == extension;
		if (!wanted)
			continue;
		const oc::string relative = FileSystem::relativePath(entry.path, root, /*allowMainThread*/ true);
		if (oc::startsWith(relative, "Local/"))
			continue;
		out.push_back(relative.empty() ? entry.path : relative);
	}
	oc::sort(out.begin(), out.end());
}

// A path field fed from the disk: the list is scanned when the popup opens, and a file from Content can be dropped on the text box.
static bool fieldFile(const char* label, oc::string& value, oc::vector<oc::string>& files, char* searchBuf, size_t searchBufSize,
	std::initializer_list<const char*> extensions, const char* payloadType, const char* tooltip = nullptr)
{
	ImGui::PushID(label);
	fieldLabel(label, tooltip);
	const float buttonWidth = ImGui::GetFrameHeight() * 1.2f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
	bool changed = inputString("##v", value);
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType))
		{
			oc::string dropped = static_cast<const char*>(payload->Data);
			for (const char* extension : extensions)
				if (dropped.size() > strlen(extension) && dropped.compare(dropped.size() - strlen(extension), strlen(extension), extension) == 0)
				{
					value = FileSystem::isAbsolute(dropped) ? FileSystem::relativePath(dropped, oc::string(), /*allowMainThread*/ true) : dropped;
					changed = true;
				}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if (ImGui::Button("...", ImVec2(buttonWidth, 0.0f)))
	{
		ImGui::OpenPopup("##pick");
		searchBuf[0] = '\0';
		gatherAssetFiles(files, extensions);
	}
	if (ImGui::BeginPopup("##pick"))
	{
		ImGui::SetNextItemWidth(340.0f);
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##search", "Search...", searchBuf, searchBufSize);
		ImGui::Separator();
		ImGui::BeginChild("##list", ImVec2(340.0f, 220.0f));
		for (const oc::string& path : files)
		{
			if (!containsCI(path, searchBuf))
				continue;
			if (ImGui::Selectable(path.c_str(), path == value))
			{
				value = path;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

static oc::string joinComma(const oc::vector<oc::string>& v)
{
	oc::string out;
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (i) out += ", ";
		out += v[i];
	}
	return out;
}

static oc::vector<oc::string> splitComma(const oc::string& s)
{
	oc::vector<oc::string> out;
	size_t start = 0;
	while (start <= s.size())
	{
		size_t comma = s.find(',', start);
		size_t end = (comma == oc::string::npos) ? s.size() : comma;
		size_t a = start, b = end;
		while (a < b && std::isspace((unsigned char)s[a])) ++a;
		while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
		if (b > a)
			out.push_back(s.substr(a, b - a));
		if (comma == oc::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}

bool EntityEditor::rotationField(const char* label, glm::quat& quat, EulerEdit& edit)
{
	// The typed angles stay as long as they still describe this quaternion: an undo or another node re-derives them.
	if (edit.quat != quat)
	{
		edit.quat = quat;
		edit.degrees = glm::degrees(glm::eulerAngles(quat));
	}
	ImGui::PushID(label);
	fieldLabel(label, "Degrees, X Y Z");
	const bool changed = ImGui::DragFloat3("##v", &edit.degrees.x, 0.5f);
	ImGui::PopID();
	if (changed)
	{
		quat = glm::quat(glm::radians(edit.degrees));
		edit.quat = quat;
	}
	return changed;
}

// The header of one component section: its fold, a close button, and a right-click menu. Returns the fold's state. The caller applies `remove` / `reset`.
static bool componentHeader(const char* name, bool& remove, bool& reset, const char* removeBlockedReason = nullptr)
{
	bool visible = true;
	const bool open = ImGui::CollapsingHeader(name, removeBlockedReason ? nullptr : &visible, ImGuiTreeNodeFlags_DefaultOpen);
	if (!visible)
		remove = true;
	if (ImGui::BeginPopupContextItem())
	{
		if (ImGui::MenuItem("Reset to defaults"))
			reset = true;
		if (ImGui::MenuItem("Remove component", nullptr, false, removeBlockedReason == nullptr))
			remove = true;
		if (removeBlockedReason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", removeBlockedReason);
		ImGui::EndPopup();
	}
	return open;
}

// ---- Properties: the selected node ----

void EntityEditor::renderProblems(const EntityDocNode& node)
{
	m_problems.clear();
	collectDocProblems(node, m_problems);
	if (m_problems.empty())
		return;
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.3f, 1.0f));
	for (const oc::string& problem : m_problems)
	{
		ImGui::Bullet();
		ImGui::TextWrapped("%s", problem.c_str());
	}
	ImGui::PopStyleColor();
	ImGui::Separator();
}

void EntityEditor::renderIdentity(EntityDocNode& node)
{
	const bool isRoot = node.id == m_root->id;
	bool edited = fieldString("Name", node.name);
	edited |= fieldCheck("Enabled", node.enabled, "Off: the entity spawns disabled, its subtree does not update and its bodies are suspended");
	if (isRoot)
		edited |= fieldCheck("Global", node.global, "The World visits this root every frame instead of finding it through the spatial index.\nFor a root whose children spread across the world.");

	if (!node.isReference())
	{
		static const char* cullModes[] = { "PerEntity", "RootOnly", "None" };
		int cullMode = int(node.cullMode);
		if (fieldCombo("Cull Mode", cullMode, cullModes, 3, "PerEntity: every entity culls itself.\nRootOnly: this entity culls for its whole subtree.\nNone: never culled, always visited."))
		{
			node.cullMode = EEntityCullMode(cullMode);
			edited = true;
		}
	}

	ImGui::SeparatorText("Transform");
	edited |= fieldVec3("Position", node.transform.pos, 0.05f, isRoot ? "The authored position. A spawn at a point replaces it." : nullptr);
	edited |= rotationField("Rotation", node.transform.quat, m_eulerNode);
	edited |= fieldFloat("Scale", node.transform.scale, 0.01f, 0.0001f, 10000.0f);
	if (ImGui::SmallButton("Reset transform"))
	{
		node.transform = Transform();
		edited = true;
	}
	if (edited)
		markEdited();
}

void EntityEditor::renderReference(EntityDocNode& node)
{
	ImGui::SeparatorText("Prefab reference");
	ImGui::TextWrapped("'%s' is authored in its own file. Here you can change only its name, placement and enabled state.", node.prefabRef.c_str());
	if (ImGui::Button("Open Prefab"))
		if (const oc::string* path = Globals::assetRegistry.findPrefab(node.prefabRef))
			requestOpen(*path);
	ImGui::SameLine();
	if (ImGui::Button("Unpack"))
		unpackNode(node.id);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copy its contents into this file and break the link");
}

void EntityEditor::renderAddComponent(EntityDocNode& node)
{
	ImGui::Dummy(ImVec2(0.0f, 2.0f));
	if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
		ImGui::OpenPopup("##ee_add");
	if (ImGui::BeginPopup("##ee_add"))
	{
		auto entry = [&](const char* label, bool& has, const char* tip, bool enabled = true) -> bool
		{
			if (has)
				return false;
			const bool clicked = ImGui::MenuItem(label, nullptr, false, enabled);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", tip);
			if (clicked)
			{
				has = true;
				markEdited();
			}
			return clicked;
		};
		entry("Scene", node.hasScene, "Lets the entity hold children");
		if (entry("Render", node.hasRender, "A mesh from an ObjectContainer"))
			node.render = RenderComponent::SpawnInfo{};
		if (entry("Animator", node.hasAnimator, "Animates a skinned Render on this entity", node.hasRender))
			node.animator = AnimatorComponent::SpawnInfo{};
		if (entry("Physics", node.hasPhysics, "A rigid body"))
		{
			node.physics = PhysicsComponent::SpawnInfo{};
			node.physics.bodyType = EPhysicsBodyType::Static;
		}
		if (entry("Audio", node.hasAudio, "Named sounds a script can trigger"))
			node.audio = AudioComponent::SpawnInfo{};
		if (entry("Particle", node.hasParticle, "A .pfx effect"))
			node.particle = ParticleComponent::SpawnInfo{};
		if (entry("Force", node.hasForce, "A forcefield bubble"))
			node.force = ForceComponent::SpawnInfo{};
		if (entry("Light", node.hasLight, "One or more lights"))
		{
			node.light = LightComponent::SpawnInfo{};
			node.light.lights.emplace_back();
		}
		entry("Network", node.hasNetwork, "Syncs this entity in multiplayer");
		ImGui::Separator();
		if (entry("Game Unit", node.hasGameUnit, "A unit of the game"))
			node.gameUnit = GameUnitComponent::SpawnInfo{};
		if (entry("Game Structure", node.hasGameStructure, "A structure of the game"))
			node.gameStructure = GameStructureComponent::SpawnInfo{};
		if (entry("Game Projectile", node.hasGameProjectile, "A shot of the game"))
			node.gameProjectile = GameProjectileComponent::SpawnInfo{};
		ImGui::Separator();
		if (entry("Script", node.hasScript, "A .dsl script"))
			node.script = ScriptComponent::SpawnInfo{};
		ImGui::EndPopup();
	}
}

void EntityEditor::renderComponents(EntityDocNode& node)
{
	if (node.hasScene)         renderSceneSection(node);
	if (node.hasRender)        renderRenderSection(node);
	if (node.hasAnimator)      renderAnimatorSection(node);
	if (node.humanoid)         renderHumanoidSection(node);
	if (node.hasPhysics)       renderPhysicsSection(node);
	if (node.hasAudio)         renderAudioSection(node);
	if (node.hasParticle)      renderParticleSection(node);
	if (node.hasForce)         renderForceSection(node);
	if (node.hasLight)         renderLightSection(node);
	if (node.hasNetwork)       renderNetworkSection(node);
	if (node.hasGameUnit)      renderGameUnitSection(node);
	if (node.hasGameStructure) renderGameStructureSection(node);
	if (node.hasGameProjectile) renderGameProjectileSection(node);
	if (node.hasScript)        renderScriptSection(node);
	renderAddComponent(node);
}

void EntityEditor::renderProperties()
{
	EntityDocNode* node = selectedNode();
	if (!node)
		return;
	ImGui::PushID((void*)(uintptr_t)node->id);
	renderProblems(*node);
	renderIdentity(*node);
	if (node->isReference())
		renderReference(*node);
	else
	{
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
		renderComponents(*node);
	}
	ImGui::PopID();
}

// ---- The component sections ----

void EntityEditor::renderSceneSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const char* blocked = node.children.empty() ? nullptr : "Remove the children first";
	const bool open = componentHeader("Scene", remove, reset, blocked);
	if (remove || reset)
	{
		if (blocked == nullptr && remove)
			node.hasScene = false;
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("scene");
	ImGui::TextDisabled("%u children. Add them in the hierarchy, or drag a .pre from Content onto it.", uint32(node.children.size()));
	if (ImGui::Button("Add Child"))
		addChild(node.id);
	ImGui::PopID();
}

void EntityEditor::renderRenderSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Render", remove, reset);
	if (remove || reset)
	{
		if (remove)
		{
			node.hasRender = false;
			node.hasAnimator = false; // it would have nothing to drive
		}
		else
			node.render = RenderComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("render");
	RenderComponent::SpawnInfo& render = node.render;
	bool edited = false;

	const bool containerChanged = fieldPicker("Container", render.containerName, m_renderPickerSearch, sizeof(m_renderPickerSearch),
		[](auto&& item) { for (const auto& kv : Globals::assetRegistry.getObjectContainers()) item(kv.first); }, "The ObjectContainer (.oc) by name");
	if (containerChanged)
	{
		render.nodePath.clear(); // a node of the previous container no longer applies
		edited = true;
	}

	// Lookup only: this is the worker. A container not loaded yet is imported on main, and the node list fills in a frame later.
	ObjectContainer* container = Globals::world.findLoadedContainer(render.containerName);
	if (!container)
		needContainer(render.containerName);
	if (containerChanged && container)
		render.skinned = container->isSkinned(); // the type follows the container, like a .pre that names none
	oc::vector<oc::string> nodePaths;
	if (container)
	{
		nodePaths = container->getNodePaths();
		oc::sort(nodePaths.begin(), nodePaths.end());
	}
	edited |= fieldPicker("Node", render.nodePath, m_renderNodePickerSearch, sizeof(m_renderNodePickerSearch),
		[&](auto&& item) { for (const oc::string& path : nodePaths) item(path); }, "A node of the container. Empty or ROOT: all of it.");

	static const char* typeLabels[] = { "StaticMesh", "SkinnedMesh" };
	int typeIdx = render.skinned ? 1 : 0;
	if (fieldCombo("Type", typeIdx, typeLabels, 2, "The preview shows a skinned mesh in its bind pose"))
	{
		render.skinned = typeIdx == 1;
		edited = true;
	}
	if (render.skinned)
		edited |= fieldString("Rig", render.rigType, "Humanoid / Generic. Informational.");

	ImGui::SeparatorText("Mesh offset");
	edited |= fieldVec3("Position", render.localTransform.pos, 0.01f, "Within the entity");
	edited |= rotationField("Rotation", render.localTransform.quat, m_eulerRender);
	edited |= fieldFloat("Scale", render.localTransform.scale, 0.01f, 0.0001f, 10000.0f);

	bool tinted = render.color.x >= 0.0f;
	if (fieldCheck("Solid colour", tinted, "A flat colour instead of the mesh's materials"))
	{
		render.color = tinted ? glm::vec3(1.0f) : glm::vec3(-1.0f);
		edited = true;
	}
	if (tinted)
		edited |= fieldColor("Colour", render.color);

	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderAnimatorSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Animator", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasAnimator = false;
		else
			node.animator = AnimatorComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("animator");
	bool edited = fieldPicker("Graph", node.animator.animatorName, m_animatorPickerSearch, sizeof(m_animatorPickerSearch),
		[](auto&& item) { for (const auto& kv : Globals::assetRegistry.getAnimators()) item(kv.first); }, "The animator graph (.apl) by name");
	edited |= fieldCheck("Enabled", node.animator.enabled);
	if (edited)
		markEdited();
	ImGui::TextDisabled("The preview does not play it: it shows the bind pose.");
	ImGui::PopID();
}

void EntityEditor::renderHumanoidSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Humanoid Animator", remove, reset);
	if (remove)
	{
		node.humanoid.reset();
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("humanoid");
	ImGui::TextDisabled("Bones are authored in the .pre text. They save as they are.");
	if (node.humanoid->rig)
		for (const BoneModelRig::Bone& bone : node.humanoid->rig->bones)
			ImGui::BulletText("%s%s", bone.name.c_str(), bone.render ? "" : "  (joint)");
	ImGui::PopID();
}

void EntityEditor::renderPhysicsSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Physics", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasPhysics = false;
		else
		{
			node.physics = PhysicsComponent::SpawnInfo{};
			node.physics.bodyType = EPhysicsBodyType::Static;
		}
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("physics");
	PhysicsComponent::SpawnInfo& physics = node.physics;
	PhysicsShape& shape = physics.shape;
	bool edited = false;

	static const char* bodyLabels[] = { "Static", "Kinematic", "Dynamic" };
	int bodyIdx = (int)physics.bodyType;
	if (fieldCombo("Body", bodyIdx, bodyLabels, 3))
	{
		physics.bodyType = (EPhysicsBodyType)bodyIdx;
		edited = true;
	}
	static const char* shapeLabels[] = { "Box", "Sphere", "Capsule", "Hull", "Mesh" };
	int shapeIdx = (int)shape.type;
	if (fieldCombo("Shape", shapeIdx, shapeLabels, 5))
	{
		shape.type = (EPhysicsShapeType)shapeIdx;
		edited = true;
	}
	switch (shape.type)
	{
	case EPhysicsShapeType::Box:
		edited |= fieldVec3("Half Extents", shape.halfExtents, 0.01f);
		break;
	case EPhysicsShapeType::Sphere:
		edited |= fieldFloat("Radius", shape.radius, 0.01f, 0.001f, 10000.0f);
		break;
	case EPhysicsShapeType::Capsule:
		edited |= fieldFloat("Radius", shape.radius, 0.01f, 0.001f, 10000.0f);
		edited |= fieldFloat("Half Height", shape.halfHeight, 0.01f, 0.001f, 10000.0f);
		break;
	case EPhysicsShapeType::Hull:
		edited |= fieldInt("Max Vertices", shape.maxHullVertices, 1.0f, 4, 64);
		ImGui::TextDisabled("Geometry from the Render mesh");
		break;
	case EPhysicsShapeType::Mesh:
		ImGui::TextDisabled("Geometry from the Render mesh (Static bodies only)");
		break;
	}
	edited |= fieldVec3("Offset", shape.offset, 0.01f);
	edited |= fieldFloat("Density", shape.density, 1.0f, 0.0f, 100000.0f);
	edited |= fieldFloat("Friction", shape.friction, 0.01f, 0.0f, 10.0f);
	edited |= fieldFloat("Restitution", shape.restitution, 0.01f, 0.0f, 1.0f);
	edited |= fieldCheck("Sensor", shape.isSensor, "Reports overlaps, does not collide");
	edited |= fieldCheck("Contact Events", shape.contactEvents, "Fires contact events to a script");
	edited |= fieldCheck("Lock Rotation", physics.lockRotation);
	edited |= fieldString("Layer", physics.layer, "A named collision layer. A new name allocates a new one.", "Default");

	// The edit buffer follows the data unless it is being typed in.
	if (m_physCollidesWithNode != node.id || !ImGui::IsAnyItemActive())
	{
		strncpy_s(m_physCollidesWithBuf, sizeof(m_physCollidesWithBuf), joinComma(physics.collidesWith).c_str(), sizeof(m_physCollidesWithBuf) - 1);
		m_physCollidesWithNode = node.id;
	}
	ImGui::PushID("collides");
	fieldLabel("Collides With", "Layer names, separated by commas. All and None also work.");
	if (ImGui::InputTextWithHint("##v", "All", m_physCollidesWithBuf, sizeof(m_physCollidesWithBuf)))
	{
		physics.collidesWith = splitComma(m_physCollidesWithBuf);
		edited = true;
	}
	ImGui::PopID();
	edited |= fieldInt("Group", shape.groupIndex, 1.0f, 0, 0, "Non-zero overrides the layers for a matching group: negative = never collide, positive = always");

	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderAudioSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Audio", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasAudio = false;
		else
			node.audio = AudioComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("audio");
	bool edited = false;

	static const char* selectLabels[] = { "Single", "Random", "RandomNoRepeat", "Cycle", "CycleStartRandom" };
	int soundToRemove = -1;
	for (size_t si = 0; si < node.audio.sounds.size(); ++si)
	{
		AudioComponent::SoundDesc& sound = node.audio.sounds[si];
		ImGui::PushID((int)si);
		const oc::string header = (sound.alias.empty() ? oc::string("<no alias>") : sound.alias) + "###sound";
		if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			edited |= fieldString("Alias", sound.alias, "The name a script triggers it by");
			int selectIdx = (int)sound.select;
			if (fieldCombo("Select", selectIdx, selectLabels, 5, "Which clip plays when there are several"))
			{
				sound.select = (EAudioSelect)selectIdx;
				edited = true;
			}
			int clipToRemove = -1;
			for (size_t ci = 0; ci < sound.clips.size(); ++ci)
			{
				AudioComponent::Clip& clip = sound.clips[ci];
				ImGui::PushID((int)ci);
				ImGui::SeparatorText(("Clip " + oc::to_string(ci + 1)).c_str());
				edited |= fieldFile("Path", clip.path, m_filePickerFiles, m_filePickerSearch, sizeof(m_filePickerSearch), { ".wav", ".ogg", ".mp3", ".flac" }, "ASSET_FILE");
				edited |= fieldFloat("Volume", clip.volume, 0.01f, 0.0f, 10.0f);
				edited |= fieldFloat("Pitch", clip.pitch, 0.01f, 0.01f, 10.0f);
				edited |= fieldCheck("Loop", clip.loop);
				edited |= fieldCheck("Relative (2D)", clip.relative, "No position: it plays at the listener");
				edited |= fieldFloat("Reference Dist", clip.referenceDistance, 0.1f, 0.0f, 10000.0f);
				edited |= fieldFloat("Max Dist", clip.maxDistance, 1.0f, 0.0f, 1000000.0f, nullptr, clip.maxDistance >= FLT_MAX ? "inf" : "%.3f");
				edited |= fieldFloat("Rolloff", clip.rolloff, 0.01f, 0.0f, 10.0f);
				if (ImGui::SmallButton("Remove Clip"))
					clipToRemove = (int)ci;
				ImGui::PopID();
			}
			if (clipToRemove >= 0)
			{
				sound.clips.erase(sound.clips.begin() + clipToRemove);
				edited = true;
			}
			if (ImGui::SmallButton("Add Clip"))
			{
				sound.clips.push_back(AudioComponent::Clip{});
				edited = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove Sound"))
				soundToRemove = (int)si;
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	if (soundToRemove >= 0)
	{
		node.audio.sounds.erase(node.audio.sounds.begin() + soundToRemove);
		edited = true;
	}
	if (ImGui::Button("Add Sound"))
	{
		AudioComponent::SoundDesc sound;
		sound.alias = "Sound" + oc::to_string(node.audio.sounds.size() + 1);
		node.audio.sounds.push_back(oc::move(sound));
		edited = true;
	}
	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderParticleSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Particle", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasParticle = false;
		else
			node.particle = ParticleComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("particle");
	bool edited = fieldFile("Effect", node.particle.effectPath, m_filePickerFiles, m_filePickerSearch, sizeof(m_filePickerSearch), { ".pfx" }, "ASSET_FILE");
	edited |= fieldCheck("Emitting", node.particle.emitting, "Starts emitting at spawn");
	if (edited)
		markEdited();
	ImGui::TextDisabled("The preview does not simulate particles.");
	ImGui::PopID();
}

void EntityEditor::renderForceSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Force", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasForce = false;
		else
			node.force = ForceComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("force");
	ForceComponent::SpawnInfo& force = node.force;
	bool edited = false;
	int team = int(force.team);
	if (fieldInt("Team", team, 0.1f, 0, 7))
	{
		force.team = uint32(glm::clamp(team, 0, 7));
		edited = true;
	}
	edited |= fieldFloat("Output", force.output, 0.05f, 0.0f, 1000.0f);
	edited |= fieldFloat("Reach", force.reach, 0.05f, 0.01f, 10000.0f);
	edited |= fieldSlider("Focus", force.focus, 0.0f, 1.0f, "0.5 = sphere spanning the reach line, 0/1 = cones pointed at the emitter/target");
	edited |= fieldSlider("Distribution", force.distribution, 0.0f, 1.0f, "Where the output density sits along the line (0 = emitter end, 1 = target end)");
	edited |= fieldFloat("Width", force.width, 0.01f, 0.01f, 4.0f);
	edited |= fieldVec3("Direction", force.direction, 0.01f, "Bubble axis in entity space; only matters when Focus != 0.5");
	edited |= fieldVec3("Offset", force.offset, 0.01f, "Entity-space offset from the anchor - Centered: the bubble centre; otherwise the span start");
	edited |= fieldCheck("Centered", force.centered, "Pull the emitter back half a reach so a zero Offset centres the bubble on the entity");
	edited |= fieldCheck("Mergeable", force.mergeable, "Overlapping same-team mergeable bubbles share one GPU emitter (Force/Merge tweaks)");
	if (edited)
		markEdited();
	ImGui::TextDisabled("The preview shows the bubble as an overlay.");
	ImGui::PopID();
}

void EntityEditor::renderNetworkSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Network", remove, reset);
	if (remove)
	{
		node.hasNetwork = false;
		markEdited();
		return;
	}
	if (open)
		ImGui::TextDisabled("Nothing to author: net ids and ownership are minted at spawn.");
}

void EntityEditor::renderLightSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Light", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasLight = false;
		else
		{
			node.light = LightComponent::SpawnInfo{};
			node.light.lights.emplace_back();
		}
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("light");
	bool edited = fieldCheck("Debug geometry", node.light.debugDraw, "Wireframe overlay of each light's reach in the game (the Lights/Debug geometry tweak forces this on for every entity)");

	// One collapsible block per light - the whole point of the component is holding several.
	int removeIdx = -1;
	for (size_t i = 0; i < node.light.lights.size(); ++i)
	{
		LightComponent::LightDesc& desc = node.light.lights[i];
		ImGui::PushID(int(i));
		const oc::string label = oc::to_string(i) + ": " + lightTypeToken(desc.type) + "###light";
		if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			static const char* typeNames[] = { "Point", "Spot", "Area", "Beam" };
			int type = int(desc.type);
			if (fieldCombo("Type", type, typeNames, 4))
			{
				desc.type = ELightType(type);
				edited = true;
			}
			edited |= fieldColor("Color", desc.color);
			edited |= fieldFloat("Intensity", desc.intensity, 0.25f, 0.0f, 10000.0f);
			edited |= fieldFloat("Range", desc.range, 0.05f, 0.01f, 10000.0f);
			edited |= fieldVec3("Offset", desc.offset, 0.01f, "Entity-space position, scaled and rotated by the entity");
			if (desc.type != ELightType::Point)
				if (fieldVec3("Direction", desc.direction, 0.01f, "Aim axis in entity space (kept unit length)"))
				{
					// Re-normalized per drag frame like the Sun Direction tweak, so pulling one axis swings the aim instead of just lengthening the vector.
					const float len2 = glm::dot(desc.direction, desc.direction);
					desc.direction = len2 > 1e-12f ? desc.direction * glm::inversesqrt(len2) : glm::vec3(0.0f, 0.0f, -1.0f);
					edited = true;
				}
			if (desc.type == ELightType::Spot)
			{
				edited |= fieldSlider("Cone angle", desc.coneAngle, 1.0f, 89.0f, "Cone HALF-angle", "%.1f deg");
				edited |= fieldSlider("Edge softness", desc.edgeSoftness, 0.001f, 1.0f);
			}
			if (desc.type == ELightType::Area)
			{
				edited |= fieldFloat("Width", desc.width, 0.01f, 0.001f, 1000.0f);
				edited |= fieldFloat("Height", desc.height, 0.01f, 0.001f, 1000.0f);
				edited |= fieldFloat("Rotation", desc.rotation, 0.5f, -180.0f, 180.0f, "Extra roll about the aim axis, on top of the entity's own rotation", "%.1f deg");
			}
			if (desc.type == ELightType::Beam)
			{
				edited |= fieldFloat("Radius", desc.width, 0.01f, 0.001f, 1000.0f);
				edited |= fieldFloat("Length", desc.length, 0.01f, 0.001f, 1000.0f);
			}
			edited |= fieldCheck("Enabled", desc.enabled);
			if (ImGui::SmallButton("Remove Light"))
				removeIdx = int(i);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	if (removeIdx >= 0)
	{
		node.light.lights.erase(node.light.lights.begin() + removeIdx);
		edited = true;
	}
	if (ImGui::Button("Add Light"))
	{
		node.light.lights.emplace_back();
		edited = true;
	}
	if (edited)
		markEdited();
	ImGui::TextDisabled("The preview shows lights as overlays; its own light stays fixed.");
	ImGui::PopID();
}

void EntityEditor::renderGameUnitSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Game Unit", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasGameUnit = false;
		else
			node.gameUnit = GameUnitComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("gameunit");
	GameUnitComponent::SpawnInfo& unit = node.gameUnit;
	bool edited = false;
	int team = int(unit.team);
	if (fieldInt("Team", team, 0.1f, 0, 7))
	{
		unit.team = uint32(glm::max(team, 0));
		edited = true;
	}
	edited |= fieldCheck("Puppet", unit.puppet);
	edited |= fieldString("Short Name", unit.shortName, "The 3-5 character HUD tag", "UNIT");
	edited |= fieldFloat("Health Max", unit.healthMax, 1.0f, 0.0f, 1000000.0f);
	edited |= fieldFloat("Energy Max", unit.energyMax, 1.0f, 0.0f, 1000000.0f, "Shield battery (no regen)");
	edited |= fieldFloat("Shield Output", unit.shieldOutput, 0.01f, 0.0f, 1000.0f);
	edited |= fieldFloat("Move Speed", unit.moveSpeed, 0.05f, 0.0f, 1000.0f);
	edited |= fieldFloat("Accel", unit.accel, 0.1f, 0.0f, 1000.0f);
	edited |= fieldFloat("Attack Range", unit.attackRange, 0.05f, 0.0f, 1000.0f, "Melee reach measured to the victim's melee ring");
	edited |= fieldFloat("Attack Interval", unit.attackInterval, 0.01f, 0.05f, 100.0f);
	edited |= fieldFloat("Attack Damage", unit.attackDamage, 0.1f, 0.0f, 100000.0f);
	edited |= fieldFloat("Player Damage", unit.playerDamage, 0.1f, 0.0f, 100000.0f, "Per hit on a player capsule");
	edited |= fieldFloat("Structure Damage", unit.structureDamage, 0.1f, -1.0f, 100000.0f, "Per bite on a structure, before the global bite mult; below 0 = Attack Damage");
	edited |= fieldFloat("Emitter Drain", unit.emitterDrain, 0.01f, 0.0f, 1000.0f, "Energy/s this unit costs the nearest active enemy emitter");
	edited |= fieldCheck("Ranged", unit.ranged);
	if (unit.ranged)
	{
		edited |= fieldFloat("Standoff Range", unit.standoffRange, 0.1f, 0.0f, 10000.0f);
		edited |= fieldFloat("Fire Interval", unit.fireInterval, 0.01f, 0.01f, 100.0f);
		static const char* shotKinds[] = { "Direct", "Splash lob" };
		int shotKind = glm::clamp(int(unit.shotKind), 0, 1);
		if (fieldCombo("Shot Kind", shotKind, shotKinds, 2))
		{
			unit.shotKind = uint8(shotKind);
			edited = true;
		}
	}
	edited |= fieldCheck("Always Show Health", unit.alwaysDisplayHealth);
	edited |= fieldCheck("Field Blocked", unit.fieldBlocked);
	edited |= fieldFloat("Height Limit", unit.heightLimit, 0.1f, 0.0f, 0.0f, "0 = the game's limit, > 0 = its own ceiling, < 0 = none (flying)");
	bool colored = unit.color.x >= 0.0f;
	if (fieldCheck("Unit colour", colored, "A colour on every render node of the subtree"))
	{
		unit.color = colored ? glm::vec3(1.0f) : glm::vec3(-1.0f);
		edited = true;
	}
	if (colored)
		edited |= fieldColor("Colour", unit.color);
	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderGameStructureSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Game Structure", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasGameStructure = false;
		else
			node.gameStructure = GameStructureComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("gamestructure");
	GameStructureComponent::SpawnInfo& structure = node.gameStructure;
	bool edited = false;
	int team = int(structure.team);
	if (fieldInt("Team", team, 0.1f, 0, 7))
	{
		structure.team = uint32(glm::max(team, 0));
		edited = true;
	}
	edited |= fieldFloat("Health Max", structure.healthMax, 1.0f, 0.0f, 1000000.0f);
	edited |= fieldCheck("Invulnerable", structure.invulnerable, "Skipped by every damage path");
	edited |= fieldFloat("Melee Radius", structure.meleeRadius, 0.05f, 0.0f, 1000.0f, "Footprint half: units gnaw against this ring, not the center");
	edited |= fieldCheck("Always Show Health", structure.alwaysDisplayHealth);
	edited |= fieldCheck("Always Show Stores", structure.alwaysShowResources, "Overhead store bars even when not selected");
	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderGameProjectileSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Game Projectile", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasGameProjectile = false;
		else
			node.gameProjectile = GameProjectileComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("gameprojectile");
	GameProjectileComponent::SpawnInfo& projectile = node.gameProjectile;
	bool edited = false;
	int team = int(projectile.team);
	if (fieldInt("Team", team, 0.1f, 0, 7))
	{
		projectile.team = uint32(glm::max(team, 0));
		edited = true;
	}
	edited |= fieldFloat("Unit Damage", projectile.unitDamage, 0.1f, 0.0f, 100000.0f);
	edited |= fieldFloat("Structure Damage", projectile.structureDamage, 0.1f, 0.0f, 100000.0f);
	edited |= fieldFloat("Lifetime", projectile.lifetime, 0.05f, 0.0f, 1000.0f);
	edited |= fieldFloat("Emitter Drain", projectile.emitterDrain, 0.01f, 0.0f, 1000.0f, "Energy/s sapped from the nearest active enemy emitter");
	edited |= fieldFloat("Drain Radius", projectile.emitterDrainRadius, 0.05f, 0.0f, 1000.0f);
	edited |= fieldFloat("Splash Radius", projectile.splashRadius, 0.05f, 0.0f, 1000.0f, "> 0: the contact damages every enemy within it");
	if (edited)
		markEdited();
	ImGui::PopID();
}

void EntityEditor::renderScriptSection(EntityDocNode& node)
{
	bool remove = false, reset = false;
	const bool open = componentHeader("Script", remove, reset);
	if (remove || reset)
	{
		if (remove)
			node.hasScript = false;
		else
			node.script = ScriptComponent::SpawnInfo{};
		markEdited();
		return;
	}
	if (!open)
		return;
	ImGui::PushID("script");
	bool edited = fieldFile("Path", node.script.scriptPath, m_filePickerFiles, m_filePickerSearch, sizeof(m_filePickerSearch), { ".dsl" }, "SCRIPT_FILE", "A .dsl script. It does not run in the preview.");
	edited |= fieldCheck("Enabled", node.script.enabled);
	if (edited)
		markEdited();
	ImGui::BeginDisabled(node.script.scriptPath.empty());
	if (ImGui::SmallButton("Open in Script Editor"))
		m_scriptOpenRequest = node.script.scriptPath;
	ImGui::EndDisabled();

	renderScriptDataSection(node);
	ImGui::PopID();
}

// The INITIAL values of the picked script's exposed ScriptData fields -- what gets written into the .pre and
// applied on every spawn. Distinct from the Properties panel, which edits the same fields on a LIVE entity and
// saves nothing.
//
// The field list comes from the module, so the script has to have compiled at least once: getOrLoad returns the
// cached build when there is one, which is the normal case for a script already on the entity.
void EntityEditor::renderScriptDataSection(EntityDocNode& node)
{
	if (node.script.scriptPath.empty())
		return;
	const ScriptModule* module = Globals::scriptHost.getOrLoad(node.script.scriptPath);
	if (module == nullptr || module->dataFields == nullptr || module->numDataFields <= 0)
		return;
	const OcScriptField* fields = static_cast<const OcScriptField*>(module->dataFields);

	ImGui::SeparatorText("Script Data");
	for (int i = 0; i < module->numDataFields; ++i)
	{
		const OcScriptField& field = fields[i];

		// The authored entry for this field, or a fresh one -- a field the .pre says nothing about shows its
		// type's zero, and only starts being serialized once actually edited.
		oc::string* authored = nullptr;
		for (ScriptComponent::InitialFieldValue& value : node.script.initialValues)
			if (value.name == field.name)
				authored = &value.value;

		ImGui::PushID(i);
		fieldLabel(field.name);

		// Edited through the TEXT form the .pre stores, parsed into a widget-shaped value and printed back --
		// one representation, so what is shown is exactly what will be written and later applied.
		const oc::string current = (authored != nullptr) ? *authored : oc::string();
		oc::string edited;
		bool changed = false;
		switch (field.type)
		{
		case OC_FIELD_INT:
		{
			int value = std::atoi(current.c_str());
			if (ImGui::DragInt("##v", &value))
			{
				edited = oc::to_string(value);
				changed = true;
			}
			break;
		}
		case OC_FIELD_BOOL:
		{
			bool value = (current == "true" || current == "1");
			if (ImGui::Checkbox("##v", &value))
			{
				edited = value ? "true" : "false";
				changed = true;
			}
			break;
		}
		case OC_FIELD_STRING:
		{
			char buffer[256];
			strncpy_s(buffer, sizeof(buffer), current.c_str(), sizeof(buffer) - 1);
			if (ImGui::InputText("##v", buffer, sizeof(buffer)))
			{
				edited = buffer;
				changed = true;
			}
			break;
		}
		default:
		{
			// Every remaining kind is 1..4 floats, so one path covers Float/vec2/vec3/vec4/quat.
			const int count = (field.type == OC_FIELD_FLOAT) ? 1
				: (field.type == OC_FIELD_VEC2) ? 2
				: (field.type == OC_FIELD_VEC3) ? 3 : 4;
			float values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			{
				const char* p = current.c_str();
				for (int c = 0; c < count && *p != '\0'; ++c)
				{
					char* end = nullptr;
					values[c] = std::strtof(p, &end);
					if (end == p)
						break;
					p = end;
					while (*p == ',' || *p == ' ')
						++p;
				}
			}
			if (ImGui::DragScalarN("##v", ImGuiDataType_Float, values, count, 0.01f))
			{
				char buffer[128];
				int written = 0;
				for (int c = 0; c < count; ++c)
					written += snprintf(buffer + written, sizeof(buffer) - written, c > 0 ? ", %g" : "%g", values[c]);
				edited = buffer;
				changed = true;
			}
			break;
		}
		}
		ImGui::PopID();

		if (!changed)
			continue;
		if (authored != nullptr)
			*authored = oc::move(edited);
		else
			node.script.initialValues.push_back({ field.name, oc::move(edited) });
		markEdited();
	}
}
