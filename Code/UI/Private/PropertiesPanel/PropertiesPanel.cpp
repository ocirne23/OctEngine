module;
// OcScriptField: ScriptModule carries the exposed-field table as void* (it doesn't depend on the script ABI),
// so the panel needs the real struct to read it. In the GLOBAL module fragment -- a plain #include after
// `module UI;` lands in the module purview and collides with the std header units Core re-exports.
#include "ScriptAPI.h"
module UI;

import Core;
import Core.imgui;
import Core.glm;
import Entity;
import RendererVK;
import Script; // ScriptModule's exposed-field table -- the only description of a ScriptData block's layout
import :PropertiesPanel;

void PropertiesPanel::render(Entity* selected)
{
	if (!selected)
	{
		ImGui::TextDisabled("No entity selected");
		return;
	}

	if (ImGui::CollapsingHeader("Entity", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Name");
		ImGui::SameLine(80.0f);
		ImGui::SetNextItemWidth(-1.0f);
		{
			char nameBuf[256];
			strncpy_s(nameBuf, sizeof(nameBuf), selected->getName(), sizeof(nameBuf) - 1);
			nameBuf[sizeof(nameBuf) - 1] = '\0';
			if (ImGui::InputText("##pp_name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
				selected->setName(nameBuf);
		}

		{
			const oc::string& sourceFile = selected->getSourceFile();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Asset");
			ImGui::SameLine(80.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::TextWrapped("%s", sourceFile.empty() ? "<inline>" : sourceFile.c_str());
		}

		{
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Enabled");
			ImGui::SameLine(80.0f);
			bool enabled = selected->isEnabled();
			if (ImGui::Checkbox("##pp_enabled", &enabled))
				selected->setEnabled(enabled);
		}
	}

	if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Position");
		ImGui::SameLine(80.0f);
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::DragFloat3("##pp_pos", &selected->pos.x, 0.05f);

		ImGui::AlignTextToFramePadding();
		ImGui::Text("Scale");
		ImGui::SameLine(80.0f);
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::DragFloat("##pp_scale", &selected->scale, 0.01f, 0.0001f, 10000.0f);

		glm::vec3 euler = glm::degrees(glm::eulerAngles(selected->rot));
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Rotation");
		ImGui::SameLine(80.0f);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3("##pp_rot", &euler.x, 0.5f))
			selected->rot = glm::quat(glm::radians(euler));
	}

	if (RenderComponent* rc = getComponent<RenderComponent>(selected))
	{
		if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(selected);

			ImGui::AlignTextToFramePadding();
			ImGui::Text("Container");
			ImGui::SameLine(100.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::TextWrapped("%s", (info && info->container) ? info->container->getFilePath().c_str() : "<none>");

			ImGui::AlignTextToFramePadding();
			ImGui::Text("Node");
			ImGui::SameLine(100.0f);
			ImGui::TextWrapped("%s", info ? info->nodePath.c_str() : "<none>");

			if (info)
			{
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Position");
				ImGui::SameLine(100.0f);
				ImGui::Text("%.3f, %.3f, %.3f", info->localTransform.pos.x, info->localTransform.pos.y, info->localTransform.pos.z);

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Scale");
				ImGui::SameLine(100.0f);
				ImGui::Text("%.3f", info->localTransform.scale);

				glm::vec3 euler = glm::degrees(glm::eulerAngles(info->localTransform.quat));
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Rotation");
				ImGui::SameLine(100.0f);
				ImGui::Text("%.2f, %.2f, %.2f", euler.x, euler.y, euler.z);
			}

			ImGui::AlignTextToFramePadding();
			ImGui::Text("Mesh insts");
			ImGui::SameLine(100.0f);
			ImGui::Text("%zu", rc->node.getNumMeshInstances());

			ImGui::AlignTextToFramePadding();
			ImGui::Text("Show bounds");
			ImGui::SameLine(100.0f);
			ImGui::Checkbox("##pp_show_bounds", &rc->showBounds);
		}
	}

	if (ScriptComponent* script = getComponent<ScriptComponent>(selected))
	{
		if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Script");
			/*
			ImGui::SameLine(80.0f);
			ImGui::SetNextItemWidth(-1.0f);
			char scriptBuf[256];
			strncpy_s(scriptBuf, sizeof(scriptBuf), script->scriptPath.c_str(), sizeof(scriptBuf) - 1);
			scriptBuf[sizeof(scriptBuf) - 1] = '\0';
			if (ImGui::InputText("##pp_script", scriptBuf, sizeof(scriptBuf), ImGuiInputTextFlags_EnterReturnsTrue))
				script->scriptPath = scriptBuf;
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCRIPT_FILE"))
					script->scriptPath = static_cast<const char*>(payload->Data);
				ImGui::EndDragDropTarget();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Type a path or drag a .scr here");
			ImGui::AlignTextToFramePadding();*/
			ImGui::Text("Enabled");
			ImGui::SameLine(80.0f);
			ImGui::Checkbox("##pp_script_enabled", &script->enabled);
			drawScriptDataFields(*script);
		}
	}
}

// The script's EXPOSED ScriptData fields (the DSL's "//@@data private int num"), edited LIVE: these write
// straight into the running instance's data block and are deliberately NOT serialized -- the .pre stores the
// INITIAL value, authored in the Entity Editor, and this is for poking at a live entity.
//
// The field table comes from the loaded module (ScriptModule::dataFields), which is the only thing that knows
// this block's layout -- so nothing shows until the script has compiled, and a script that exposes nothing
// shows nothing.
void PropertiesPanel::drawScriptDataFields(ScriptComponent& script)
{
	const ScriptModule* module = script.scriptModule;
	if (module == nullptr || module->dataFields == nullptr || module->numDataFields <= 0)
		return;
	// A block allocated against a DIFFERENT layout must not be read with this table: a hot-reload that changed
	// the fields reallocates on the next entry point, and until then these offsets don't describe it.
	if (script.scriptData == nullptr || script.scriptDataLayoutId != module->dataLayoutId)
		return;

	// ScriptModule carries the table as void* (it doesn't depend on the script ABI); this is where it regains
	// its type, once, rather than at every use below.
	const OcScriptField* fields = static_cast<const OcScriptField*>(module->dataFields);

	ImGui::SeparatorText("Script Data");
	for (int i = 0; i < module->numDataFields; ++i)
	{
		const OcScriptField& field = fields[i];
		if (field.offset < 0 || static_cast<uint32>(field.offset) >= script.scriptDataSize)
			continue; // defensive: a table that doesn't match the block it came with
		void* value = script.scriptData.get() + field.offset;

		ImGui::PushID(i);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s", field.name);
		ImGui::SameLine(120.0f);
		ImGui::SetNextItemWidth(-1.0f);
		switch (field.type)
		{
		case OC_FIELD_INT:   ImGui::DragInt("##v", static_cast<int*>(value)); break;
		case OC_FIELD_FLOAT: ImGui::DragFloat("##v", static_cast<float*>(value), 0.01f); break;
		case OC_FIELD_BOOL:  ImGui::Checkbox("##v", static_cast<bool*>(value)); break;
		case OC_FIELD_VEC2:  ImGui::DragFloat2("##v", static_cast<float*>(value), 0.01f); break;
		case OC_FIELD_VEC3:  ImGui::DragFloat3("##v", static_cast<float*>(value), 0.01f); break;
		case OC_FIELD_VEC4:  ImGui::DragFloat4("##v", static_cast<float*>(value), 0.01f); break;
		case OC_FIELD_QUAT:  ImGui::DragFloat4("##v", &static_cast<glm::quat*>(value)->x, 0.01f); break;
		case OC_FIELD_STRING:
		{
			// The field holds a const char* into ENGINE-interned storage, so a new value has to be interned
			// rather than copied into the block. Committed on Enter: interning every keystroke would leave a
			// permanent entry per intermediate spelling (the intern set never shrinks).
			const char* current = *static_cast<const char* const*>(value);
			char buffer[256];
			strncpy_s(buffer, sizeof(buffer), current != nullptr ? current : "", sizeof(buffer) - 1);
			if (ImGui::InputText("##v", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue))
				*static_cast<const char**>(value) = Globals::scriptContext.internString(buffer);
			break;
		}
		default: ImGui::TextDisabled("(unsupported)"); break;
		}
		ImGui::PopID();
	}
}
