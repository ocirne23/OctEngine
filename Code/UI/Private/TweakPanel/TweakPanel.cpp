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

}

// exported from :TweakPanel — the settings menu draws the same rows
void drawTweakVar(const TweakVar& var, int index, oc::vector<const TweakVar*>& deferredCallbacks)
{
		ImGui::PushID(index);

		bool changed = false;

		switch (var.type)
		{
		case ETweakType::Float:
		{
			drawLabel(var);
			float* v = static_cast<float*>(var.data);
			int numDecimals = var.speed >= 1.0f ? 1 : oc::max(static_cast<int>(-std::log10(var.speed)), 2);
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

		// Deferred to TweakPanel::flushDeferredCallbacks on the main thread - the widget pass runs
		// on a worker and the callbacks are main-thread work (renderer, physics world, ...).
		if (changed && var.onChange)
			deferredCallbacks.push_back(&var);

		ImGui::PopID();
}

namespace
{
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

	// Header/TreeNode background from the owning group's colour: full strength for the group fold
	// (depth 0), a dimmer tint for the category fold (depth 1); deeper nodes stay unframed.
	void pushFoldColors(const glm::vec4& color, float alpha)
	{
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, alpha));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, glm::min(1.0f, alpha + 0.15f)));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(color.x, color.y, color.z, glm::min(1.0f, alpha + 0.3f)));
	}

	void renderCategory(CategoryNode& node, int depth, oc::string_view parentPath, const glm::vec4& groupColor, oc::vector<const TweakVar*>& deferredCallbacks)
	{
		// Visible text is node.name; everything after "##" is the (hidden) ImGui ID.
		// Use the full category path as the ID so categories that share a display
		// name in different branches (e.g. Physics/World vs Ocean/World) don't collide.
		oc::string path(parentPath);
		path += '/';
		path.append(node.name.data(), node.name.size()); // eastl::string has no string_view +=

		oc::string label(node.name);
		label += "##";
		label += path;

		bool open;
		if (depth == 0)
		{
			pushFoldColors(groupColor, 0.55f);
			open = ImGui::CollapsingHeader(label.c_str());
			ImGui::PopStyleColor(3);
		}
		else if (depth == 1)
		{
			pushFoldColors(groupColor, 0.25f);
			open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed);
			ImGui::PopStyleColor(3);
		}
		else
			open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);

		if (open)
		{
			for (int i : node.varIndices)
				drawTweakVar(TweakRegistry::get().vars()[i], i, deferredCallbacks);

			for (CategoryNode& child : node.children)
				renderCategory(child, depth + 1, path, groupColor, deferredCallbacks);

			if (depth != 0)
				ImGui::TreePop();
		}
	}

	// Root categories in the group table's order; roots the table does not list come after, in
	// registration order (they all sit in the fallback group, so only that group ever has them).
	void sortRootCategories(CategoryNode& group, oc::span<const oc::string_view> order)
	{
		size_t next = 0;
		for (const oc::string_view name : order)
			for (size_t i = next; i < group.children.size(); ++i)
				if (group.children[i].name == name)
				{
					if (i != next)
						oc::swap(group.children[i], group.children[next]);
					++next;
					break;
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

	// One node per group (Tweak::groups() order), the root categories beneath it.
	const oc::span<const TweakGroup> groups = Tweak::groups();
	oc::vector<CategoryNode> groupNodes(groups.size());
	for (size_t g = 0; g < groups.size(); ++g)
		groupNodes[g].name = groups[g].name;

	for (int i = 0; i < static_cast<int>(vars.size()); ++i)
	{
		oc::string_view cat = vars[i].category.empty() ? oc::string_view("General") : vars[i].category;

		CategoryNode* node = &groupNodes[Tweak::groupIndexOf(cat)];
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

	for (size_t g = 0; g < groups.size(); ++g)
	{
		CategoryNode& group = groupNodes[g];
		if (group.children.empty())
			continue; // nothing registered under it (Game/* before a match, the fallback group)
		sortRootCategories(group, groups[g].categories);
		renderCategory(group, 0, {}, groups[g].color, m_deferredCallbacks);
	}
}

void TweakPanel::flushDeferredCallbacks()
{
	for (const TweakVar* var : m_deferredCallbacks)
		var->onChange();
	m_deferredCallbacks.clear();
}
