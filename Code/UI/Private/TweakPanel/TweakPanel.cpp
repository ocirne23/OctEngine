module UI;

import Core;
import Core.imgui;
import Core.glm;
import Settings.Tweaks;
import :TweakPanel;

namespace
{
	constexpr float kLabelWidth = 120.0f;

	char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

	// Case-insensitive find of a lowercase needle.
	size_t findNoCase(oc::string_view haystack, oc::string_view needle, size_t from = 0)
	{
		if (needle.size() > haystack.size())
			return oc::string_view::npos;
		for (size_t i = from; i + needle.size() <= haystack.size(); ++i)
		{
			size_t j = 0;
			while (j < needle.size() && lowerAscii(haystack[i + j]) == needle[j])
				++j;
			if (j == needle.size())
				return i;
		}
		return oc::string_view::npos;
	}

	// Marks every occurrence of every term behind the label text about to be drawn at the cursor.
	void highlightMatches(oc::string_view text, oc::span<const oc::string_view> terms)
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float height = ImGui::GetFrameHeight();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImU32 color = ImGui::GetColorU32(ImVec4(1.0f, 0.70f, 0.20f, 0.35f));
		for (const oc::string_view term : terms)
			for (size_t at = findNoCase(text, term); at != oc::string_view::npos; at = findNoCase(text, term, at + term.size()))
			{
				const float x0 = pos.x + ImGui::CalcTextSize(text.data(), text.data() + at).x;
				const float x1 = pos.x + ImGui::CalcTextSize(text.data(), text.data() + at + term.size()).x;
				draw->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + height), color, 2.0f);
			}
	}

	void drawLabel(const TweakVar& var, oc::span<const oc::string_view> highlight)
	{
		ImGui::AlignTextToFramePadding();
		if (!highlight.empty())
			highlightMatches(var.name, highlight);
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

// exported from :TweakPanel - the settings menu draws the same rows
void drawTweakVar(const TweakVar& var, int index, oc::vector<const TweakVar*>& deferredCallbacks, oc::vector<TweakLockToggle>* deferredLocks,
	oc::span<const oc::string_view> highlight)
{
		ImGui::PushID(index);
		// A locked row is read-only: the values it feeds are baked into the shaders (TweakLock).
		const TweakRegistry& registry = TweakRegistry::get();
		const bool locked = registry.isVarLocked(var);
		const uint32 lockId = deferredLocks ? registry.lockOf(var) : TweakRegistry::c_noLock;
		if (lockId != TweakRegistry::c_noLock)
		{
			if (locked)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.10f, 0.85f));
			if (ImGui::SmallButton(locked ? "L##lock" : "U##lock"))
				deferredLocks->push_back(TweakLockToggle{ lockId, &var, !locked });
			if (locked)
				ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(locked
					? "LOCKED: what this tweak feeds is constant in the shaders.\nClick to unlock this one tweak (recompiles the shaders)."
					: "Unlocked: the shaders read what this tweak feeds live.\nClick to lock it (recompiles the shaders).");
			ImGui::SameLine();
		}
		if (locked)
			ImGui::BeginDisabled();

		bool changed = false;

		switch (var.type)
		{
		case ETweakType::Float:
		{
			drawLabel(var, highlight);
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
			drawLabel(var, highlight);
			changed = ImGui::DragFloat2("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Float3:
			drawLabel(var, highlight);
			changed = ImGui::DragFloat3("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Float4:
			drawLabel(var, highlight);
			changed = ImGui::DragFloat4("##v", static_cast<float*>(var.data), var.speed);
			typeValueOnRightClick();
			break;
		case ETweakType::Color3:
		{
			drawLabel(var, highlight);
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
			drawLabel(var, highlight);
			changed = ImGui::ColorEdit4("##v", static_cast<float*>(var.data), ImGuiColorEditFlags_AlphaBar);
			break;
		case ETweakType::Bool:
			drawLabel(var, highlight);
			changed = ImGui::Checkbox("##v", static_cast<bool*>(var.data));
			break;
		case ETweakType::Int:
		{
			drawLabel(var, highlight);
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
			drawLabel(var, highlight);
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
		// on a worker and the callbacks are main-thread work (renderer, physics world, ...). Every change goes:
		// the registry's change listeners (the UBO bake) see vars without an onChange too.
		if (changed)
			deferredCallbacks.push_back(&var);

		if (locked)
			ImGui::EndDisabled();
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

	struct RenderTargets
	{
		oc::vector<const TweakVar*>& deferredCallbacks;
		oc::vector<TweakLockToggle>& deferredLocks;
	};

	// A lock's section toggle, right-aligned on its category fold's row (the fold allows the overlap): every row
	// under it. The click is only COLLECTED: setLocked fires the owner's main-thread work
	// (TweakPanel::flushDeferredCallbacks).
	void drawLockToggle(uint32 lockId, float rightX, oc::vector<TweakLockToggle>& deferredLocks)
	{
		const TweakRegistry& registry = TweakRegistry::get();
		const TweakLock& lock = registry.locks()[lockId];
		const ETweakLockState state = registry.lockState(lockId);
		const char* label = state == ETweakLockState::All ? "Locked" : state == ETweakLockState::Some ? "Partly locked" : "Lock";
		ImGui::SameLine(rightX - ImGui::CalcTextSize(label).x - ImGui::GetStyle().FramePadding.x * 2.0f);
		ImGui::PushID((int)lockId);
		if (state != ETweakLockState::None)
			ImGui::PushStyleColor(ImGuiCol_Button, state == ETweakLockState::All ? ImVec4(0.85f, 0.55f, 0.10f, 0.85f) : ImVec4(0.55f, 0.38f, 0.12f, 0.85f));
		if (ImGui::SmallButton(label))
			deferredLocks.push_back(TweakLockToggle{ lockId, nullptr, state != ETweakLockState::All });
		if (state != ETweakLockState::None)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			oc::string sections;
			for (const oc::string& category : lock.categories)
				sections += (sections.empty() ? "" : ", ") + category;
			ImGui::SetTooltip(state == ETweakLockState::All
				? "%s (%s) is LOCKED: its values are constants in the shaders and its rows are read-only.\nClick to unlock every row (recompiles the shaders). Each row's own button unlocks just that tweak."
				: "Lock every row of %s (%s): bake their values into the shaders as constants and make the rows read-only.\nRecompiles the shaders.",
				lock.name.c_str(), sections.c_str());
		}
		ImGui::PopID();
	}

	// categoryPath = the node's "Category/Sub" path (its group fold excluded) - what a lock names.
	void renderCategory(CategoryNode& node, int depth, oc::string_view parentPath, oc::string_view categoryPath, const glm::vec4& groupColor, RenderTargets& out)
	{
		// Visible text is node.name; everything after "##" is the (hidden) ImGui ID.
		// Use the full category path as the ID so categories that share a display
		// name in different branches (e.g. Physics/World vs Ocean/World) don't collide.
		oc::string path(parentPath);
		path += '/';
		path.append(node.name.data(), node.name.size()); // eastl::string has no string_view +=

		oc::string category(categoryPath);
		if (depth > 0)
		{
			if (!category.empty())
				category += '/';
			category.append(node.name.data(), node.name.size());
		}

		oc::string label(node.name);
		label += "##";
		label += path;

		const uint32 lockId = depth > 0 ? TweakRegistry::get().lockAt(oc::string_view(category.data(), category.size())) : TweakRegistry::c_noLock;
		const ImGuiTreeNodeFlags lockFlags = lockId != TweakRegistry::c_noLock ? ImGuiTreeNodeFlags_AllowOverlap : ImGuiTreeNodeFlags_None;
		const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

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
			open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed | lockFlags);
			ImGui::PopStyleColor(3);
		}
		else
			open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | lockFlags);

		if (lockId != TweakRegistry::c_noLock)
			drawLockToggle(lockId, rightX, out.deferredLocks);

		if (open)
		{
			for (int i : node.varIndices)
				drawTweakVar(TweakRegistry::get().vars()[i], i, out.deferredCallbacks, &out.deferredLocks);

			for (CategoryNode& child : node.children)
				renderCategory(child, depth + 1, path, oc::string_view(category.data(), category.size()), groupColor, out);

			if (depth != 0)
				ImGui::TreePop();
		}
	}

	// Search results: no folds, so the tree's own open state stays untouched. Every category that holds a match
	// is one separator line in its group's colour ("Graphics / Ocean / Foam"), its matching rows beneath.
	void renderResults(const CategoryNode& node, oc::string& path, const glm::vec4& groupColor, oc::span<const oc::string_view> terms, RenderTargets& out)
	{
		const size_t mark = path.size();
		if (!path.empty())
			path += " / ";
		path.append(node.name.data(), node.name.size());

		if (!node.varIndices.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f + 0.5f * groupColor.x, 0.5f + 0.5f * groupColor.y, 0.5f + 0.5f * groupColor.z, 1.0f));
			ImGui::SeparatorText(path.c_str());
			ImGui::PopStyleColor();
			for (int i : node.varIndices)
				drawTweakVar(TweakRegistry::get().vars()[i], i, out.deferredCallbacks, &out.deferredLocks, terms);
		}

		for (const CategoryNode& child : node.children)
			renderResults(child, path, groupColor, terms, out);
		path.resize(mark);
	}

	// Every term must be in the name, the category path or the group name.
	bool matchesSearch(const TweakVar& var, oc::string_view category, oc::string_view group, oc::span<const oc::string_view> terms)
	{
		for (const oc::string_view term : terms)
			if (findNoCase(var.name, term) == oc::string_view::npos
				&& findNoCase(category, term) == oc::string_view::npos
				&& findNoCase(group, term) == oc::string_view::npos)
				return false;
		return true;
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

	// Search bar: Ctrl+F focuses it, Escape clears it. Space-separated terms, all must match (case-insensitive).
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F))
		ImGui::SetKeyboardFocusHere();
	const bool hasSearch = m_search[0] != '\0';
	const ImGuiStyle& style = ImGui::GetStyle();
	ImGui::SetNextItemWidth(hasSearch ? -(ImGui::GetFrameHeight() + style.ItemSpacing.x) : -1.0f);
	const bool searchEdited = ImGui::InputTextWithHint("##tweak_search", "Search tweaks (Ctrl+F)", m_search, sizeof(m_search),
		ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_AutoSelectAll);
	const ImVec2 searchMin = ImGui::GetItemRectMin();
	const ImVec2 searchMax = ImGui::GetItemRectMax();
	if (hasSearch)
	{
		ImGui::SameLine();
		if (ImGui::Button("X##tweak_search_clear", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
			m_search[0] = '\0';
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Clear the search");
	}

	char lowered[sizeof(m_search)];
	constexpr size_t kMaxTerms = 16;
	oc::string_view terms[kMaxTerms];
	size_t numTerms = 0;
	for (size_t i = 0, start = 0; ; ++i)
	{
		const char c = m_search[i];
		lowered[i] = lowerAscii(c);
		if (c == ' ' || c == '\0')
		{
			if (i > start && numTerms < kMaxTerms)
				terms[numTerms++] = oc::string_view(lowered + start, i - start);
			start = i + 1;
		}
		if (c == '\0')
			break;
	}
	const oc::span<const oc::string_view> searchTerms(terms, numTerms);
	const bool searching = numTerms > 0;

	// One node per group (Tweak::groups() order), the root categories beneath it.
	const oc::span<const TweakGroup> groups = Tweak::groups();
	oc::vector<CategoryNode> groupNodes(groups.size());
	for (size_t g = 0; g < groups.size(); ++g)
		groupNodes[g].name = groups[g].name;

	int numMatches = 0;
	for (int i = 0; i < static_cast<int>(vars.size()); ++i)
	{
		oc::string_view cat = vars[i].category.empty() ? oc::string_view("General") : vars[i].category;
		const size_t groupIndex = Tweak::groupIndexOf(cat);
		if (searching && !matchesSearch(vars[i], cat, groups[groupIndex].name, searchTerms))
			continue;
		++numMatches;

		CategoryNode* node = &groupNodes[groupIndex];
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

	// Match count, right-aligned inside the search box.
	if (searching)
	{
		const oc::string count = oc::format("{} / {}", numMatches, vars.size());
		const ImVec2 size = ImGui::CalcTextSize(count.c_str());
		ImGui::GetWindowDrawList()->AddText(ImVec2(searchMax.x - size.x - style.FramePadding.x, searchMin.y + style.FramePadding.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), count.c_str());
	}

	ImGui::BeginChild("##tweak_list");
	if (searchEdited)
		ImGui::SetScrollY(0.0f);

	RenderTargets out{ m_deferredCallbacks, m_deferredLocks };
	if (searching && numMatches == 0)
		ImGui::TextDisabled("No tweak matches \"%s\"", m_search);
	oc::string path;
	for (size_t g = 0; g < groups.size(); ++g)
	{
		CategoryNode& group = groupNodes[g];
		if (group.children.empty())
			continue; // nothing registered under it (Game/* before a match, the fallback group), or no match
		sortRootCategories(group, groups[g].categories);
		if (searching)
			renderResults(group, path, groups[g].color, searchTerms, out);
		else
			renderCategory(group, 0, {}, {}, groups[g].color, out);
	}
	ImGui::EndChild();
}

void TweakPanel::flushDeferredCallbacks()
{
	for (const TweakVar* var : m_deferredCallbacks)
		TweakRegistry::get().notifyChanged(*var);
	m_deferredCallbacks.clear();
	for (const TweakLockToggle& toggle : m_deferredLocks)
	{
		if (toggle.var)
			TweakRegistry::get().setVarLocked(*toggle.var, toggle.locked);
		else
			TweakRegistry::get().setLocked(toggle.lock, toggle.locked);
	}
	m_deferredLocks.clear();
}
