module UI;

import Core;
import Core.imgui;
import Core.glm;
import Core.Tweaks;
import :TweakPanel;

namespace
{
	constexpr float kLabelWidth = 120.0f;

	void drawLabel(const TweakVar& var)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(var.name.data(), var.name.data() + var.name.size());
		ImGui::SameLine(kLabelWidth);
		ImGui::SetNextItemWidth(-1.0f);
	}

	// Right-clicking a slider/drag enters text-input mode (same as ctrl+click)
	void typeValueOnRightClick()
	{
		if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::SetKeyboardFocusHere(-1);
	}

	void drawVar(const TweakVar& var, int index)
	{
		ImGui::PushID(index);

		bool changed = false;

		switch (var.type)
		{
		case ETweakType::Float:
		{
			drawLabel(var);
			float* v = static_cast<float*>(var.data);
			int numDecimals = var.speed >= 1.0f ? 1 : static_cast<int>(-std::log10(var.speed));
			oc::string formatStr = ("%." + oc::to_string(numDecimals) + "f");
			if (var.isUnbounded())
				changed = ImGui::DragFloat("##v", v, var.speed, var.min, FLT_MAX, formatStr.c_str());
			else
				changed = ImGui::SliderFloat("##v", v, var.min, var.max, formatStr.c_str());
			typeValueOnRightClick();
			break;
		}
		case ETweakType::Float2:
			drawLabel(var);
			changed = ImGui::DragFloat2("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Float3:
			drawLabel(var);
			changed = ImGui::DragFloat3("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Float4:
			drawLabel(var);
			changed = ImGui::DragFloat4("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Color3:
		{
			drawLabel(var);
			float* c = static_cast<float*>(var.data);
			if (var.intensity != nullptr)
			{
				changed = ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::DragFloat("##intensity", var.intensity, var.speed, var.min, var.max, "x %.3f");
				typeValueOnRightClick();
			}
			else
			{
				changed = ImGui::ColorEdit3("##v", c);
			}
			break;
		}
		case ETweakType::Color4:
			drawLabel(var);
			changed = ImGui::ColorEdit4("##v", static_cast<float*>(var.data), ImGuiColorEditFlags_AlphaBar);
			break;
		case ETweakType::Bool:
			drawLabel(var);
			changed = ImGui::Checkbox("##v", static_cast<bool*>(var.data));
			break;
		case ETweakType::Int:
		{
			drawLabel(var);
			int* v = static_cast<int*>(var.data);
			if (var.isUnbounded())
				changed = ImGui::DragInt("##v", v, var.speed, static_cast<int>(var.min), INT_MAX);
			else
				changed = ImGui::SliderInt("##v", v, static_cast<int>(var.min), static_cast<int>(var.max));
			typeValueOnRightClick();
			break;
		}
		case ETweakType::Enum:
		{
			drawLabel(var);
			int* sel = static_cast<int*>(var.data);
			const int count = static_cast<int>(var.enumNames.size());
			const oc::string_view current = (*sel >= 0 && *sel < count) ? var.enumNames[*sel] : oc::string_view{};
			oc::string currentStr(current);
			if (ImGui::BeginCombo("##v", currentStr.c_str()))
			{
				for (int i = 0; i < count; ++i)
				{
					oc::string item(var.enumNames[i]);
					const bool selected = (i == *sel);
					if (ImGui::Selectable(item.c_str(), selected) && i != *sel)
					{
						*sel = i;
						changed = true;
					}
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			break;
		}
		}

		if (changed && var.onChange)
			var.onChange();

		ImGui::PopID();
	}

	struct CategoryNode
	{
		oc::string_view          name;
		oc::vector<CategoryNode> children;
		oc::vector<int>          varIndices;

		CategoryNode* child(oc::string_view segment)
		{
			for (CategoryNode& c : children)
				if (c.name == segment)
					return &c;
			children.push_back(CategoryNode{ segment });
			return &children.back();
		}
	};

	void renderCategory(CategoryNode& node, int depth, oc::string_view parentPath)
	{
		// Visible text is node.name; everything after "##" is the (hidden) ImGui ID.
		// Use the full category path as the ID so categories that share a display
		// name in different branches (e.g. Physics/World vs Ocean/World) don't collide.
		oc::string path(parentPath);
		path += '/';
		path += node.name;

		oc::string label(node.name);
		label += "##";
		label += path;

		bool open;
		if (depth == 0)
			open = ImGui::CollapsingHeader(label.c_str());
		else
			open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);

		if (open)
		{
			for (int i : node.varIndices)
				drawVar(TweakRegistry::get().vars()[i], i);

			for (CategoryNode& child : node.children)
				renderCategory(child, depth + 1, path);

			if (depth != 0)
				ImGui::TreePop();
		}
	}
}

void TweakPanel::render()
{
	const oc::vector<TweakVar>& vars = TweakRegistry::get().vars();
	if (vars.empty())
	{
		ImGui::TextDisabled("No tweak variables registered");
		return;
	}

	CategoryNode root;
	for (int i = 0; i < static_cast<int>(vars.size()); ++i)
	{
		oc::string_view cat = vars[i].category.empty() ? oc::string_view("General") : vars[i].category;

		CategoryNode* node = &root;
		size_t start = 0;
		while (start <= cat.size())
		{
			const size_t slash = cat.find('/', start);
			const size_t end = (slash == oc::string_view::npos) ? cat.size() : slash;
			const oc::string_view segment = cat.substr(start, end - start);
			if (!segment.empty())
				node = node->child(segment);
			if (slash == oc::string_view::npos)
				break;
			start = slash + 1;
		}
		node->varIndices.push_back(i);
	}

	for (CategoryNode& child : root.children)
		renderCategory(child, 0, {});
}
