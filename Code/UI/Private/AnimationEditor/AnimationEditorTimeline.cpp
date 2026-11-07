module UI;

import Core;
import Core.imgui;
import Core.glm;
import Animation;
import :AnimationEditor;

// IM_COL32 is a macro: it does not cross the module import.
static constexpr ImU32 aeCol(int r, int g, int b, int a = 255)
{
	return (ImU32(a) << 24) | (ImU32(b) << 16) | (ImU32(g) << 8) | ImU32(r);
}

static constexpr float c_rulerHeight = 22.0f;
static constexpr float c_timeEps = 1e-5f;
static constexpr ImU32 c_componentColors[3] = { aeCol(235, 95, 85), aeCol(120, 215, 95), aeCol(95, 150, 255) };
static const char* const c_channelNames[3] = { "Position", "Rotation", "Scale" };

template <typename F>
static void aeKeysOf(const ClipTrack& track, ClipChannel channel, F&& f)
{
	switch (channel)
	{
	case ClipChannel::Position: f(track.positionKeys); break;
	case ClipChannel::Rotation: f(track.rotationKeys); break;
	default:                    f(track.scaleKeys); break;
	}
}

template <typename F>
static void aeKeysOfMut(ClipTrack& track, ClipChannel channel, F&& f)
{
	switch (channel)
	{
	case ClipChannel::Position: f(track.positionKeys); break;
	case ClipChannel::Rotation: f(track.rotationKeys); break;
	default:                    f(track.scaleKeys); break;
	}
}

// The frames between labelled ticks: the smallest "round" step that keeps labels about 56 px apart.
static int aeMajorStep(float pixelsPerFrame)
{
	static constexpr int c_steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3000, 6000 };
	for (int s : c_steps)
		if (float(s) * pixelsPerFrame >= 56.0f)
			return s;
	return 6000;
}

static void aeSortUnique(oc::vector<float>& times)
{
	oc::sort(times.begin(), times.end());
	size_t w = 0;
	for (size_t r = 0; r < times.size(); ++r)
		if (w == 0 || times[r] - times[w - 1] > c_timeEps)
			times[w++] = times[r];
	times.resize(w);
}

static oc::string aeShortName(const oc::string& name)
{
	// "mixamorig:LeftArm" -> "LeftArm": the namespace repeats on every bone.
	const size_t colon = name.rfind(':');
	return colon == oc::string::npos || colon + 1 >= name.size() ? name : name.substr(colon + 1);
}

static void aeDiamond(ImDrawList* list, const ImVec2& c, float r, ImU32 fill, ImU32 outline)
{
	const ImVec2 pts[4] = { ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y) };
	list->AddConvexPolyFilled(pts, 4, fill);
	list->AddPolyline(pts, 4, outline, ImDrawFlags_Closed, 1.0f);
}

// Euler degrees of a quaternion, each component moved by whole turns to lie closest to `near` (a continuous curve).
static glm::vec3 aeEulerNear(const glm::quat& q, const glm::vec3& near)
{
	glm::vec3 e = glm::degrees(glm::eulerAngles(q));
	for (int c = 0; c < 3; ++c)
		e[c] += 360.0f * glm::round((near[c] - e[c]) / 360.0f);
	return e;
}

// ---- Rows ----

void AnimationEditor::buildRows()
{
	m_rows.clear();
	const bool filtering = m_filter[0] != '\0';
	auto matches = [&](const oc::string& name)
	{
		if (!filtering)
			return true;
		oc::string h = name, n = m_filter;
		for (char& c : h) c = (char)std::tolower((unsigned char)c);
		for (char& c : n) c = (char)std::tolower((unsigned char)c);
		return h.find(n) != oc::string::npos;
	};
	auto addBone = [&](int32 bone, int32 track, uint8 depth, const oc::string& name)
	{
		const bool hasKeys = track >= 0 && !m_clip.tracks[track].empty();
		m_rows.push_back(Row{ RowKind::Bone, depth, ClipChannel::Position, hasKeys, bone, track });
		if (!hasKeys || m_expandedBones.count(name) == 0)
			return;
		for (uint32 c = 0; c < c_numClipChannels; ++c)
			if (m_clip.tracks[track].numKeys(ClipChannel(c)) > 0)
				m_rows.push_back(Row{ RowKind::Channel, uint8(depth + 1), ClipChannel(c), false, bone, track });
	};

	if (const Skeleton* skel = m_skeleton; skel && m_boneTrack.size() == skel->numBones())
	{
		const uint32 n = skel->numBones();
		oc::vector<uint8> depth(n, 0);
		for (uint32 i = 0; i < n; ++i)
		{
			const int32 parent = skel->parentIndices[i];
			depth[i] = parent < 0 ? 0 : uint8(glm::min(int(depth[parent]) + 1, 200));
			if (m_animatedOnly && !m_boneSubtreeAnimated[i] && (int32)i != m_selectedBone)
				continue;
			if (!matches(skel->boneNames[i]))
				continue;
			addBone((int32)i, m_boneTrack[i], filtering ? 0 : depth[i], skel->boneNames[i]);
		}
		// Tracks the rig has no bone for still show, flagged.
		for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
			if (skel->findBone(m_clip.tracks[t].bone) < 0 && matches(m_clip.tracks[t].bone))
				addBone(-1, (int32)t, 0, m_clip.tracks[t].bone);
	}
	else
	{
		for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
			if (matches(m_clip.tracks[t].bone))
				addBone(-1, (int32)t, 0, m_clip.tracks[t].bone);
	}
}

void AnimationEditor::rowTimes(const Row& row, oc::vector<float>& out) const
{
	out.clear();
	if (row.track < 0 || row.track >= (int32)m_clip.tracks.size())
		return;
	const ClipTrack& track = m_clip.tracks[row.track];
	auto add = [&](ClipChannel channel) { aeKeysOf(track, channel, [&](const auto& keys) { for (const auto& k : keys) out.push_back(k.time); }); };
	if (row.kind == RowKind::Channel)
		add(row.channel);
	else
		for (uint32 c = 0; c < c_numClipChannels; ++c)
			add(ClipChannel(c));
	aeSortUnique(out);
}

void AnimationEditor::gatherDiamonds(int32 rowIdx, oc::vector<Diamond>& out) const
{
	out.clear();
	auto addTrack = [&](uint32 t, int32 onlyChannel)
	{
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			if (onlyChannel >= 0 && (int32)c != onlyChannel)
				continue;
			aeKeysOf(m_clip.tracks[t], ClipChannel(c), [&](const auto& keys)
			{
				for (const auto& k : keys)
					out.push_back(Diamond{ k.time, 1u, isKeySelected(t, ClipChannel(c), k.time) ? 1u : 0u, k.interp });
			});
		}
	};
	if (rowIdx == c_summaryRow)
	{
		for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
			addTrack(t, -1);
	}
	else if (rowIdx >= 0 && rowIdx < (int32)m_rows.size())
	{
		const Row& row = m_rows[rowIdx];
		if (row.track >= 0 && row.track < (int32)m_clip.tracks.size())
			addTrack((uint32)row.track, row.kind == RowKind::Channel ? (int32)row.channel : -1);
	}
	if (out.empty())
		return;
	oc::sort(out.begin(), out.end(), [](const Diamond& a, const Diamond& b) { return a.time < b.time; });
	size_t w = 0;
	for (size_t r = 0; r < out.size(); ++r)
	{
		if (w > 0 && out[r].time - out[w - 1].time <= c_timeEps)
		{
			out[w - 1].count += out[r].count;
			out[w - 1].selected += out[r].selected;
			if (out[w - 1].interp != out[r].interp)
				out[w - 1].interp = KeyInterp::Linear; // mixed: the plain diamond
		}
		else
			out[w++] = out[r];
	}
	out.resize(w);
}

void AnimationEditor::collectKeysAt(int32 rowIdx, float time, oc::vector<KeyRef>& out) const
{
	out.clear();
	auto addTrack = [&](uint32 t, int32 onlyChannel)
	{
		for (uint32 c = 0; c < c_numClipChannels; ++c)
		{
			if (onlyChannel >= 0 && (int32)c != onlyChannel)
				continue;
			aeKeysOf(m_clip.tracks[t], ClipChannel(c), [&](const auto& keys)
			{
				for (const auto& k : keys)
					if (glm::abs(k.time - time) <= c_timeEps)
						out.push_back(KeyRef{ t, ClipChannel(c), k.time });
			});
		}
	};
	if (rowIdx == c_summaryRow)
	{
		for (uint32 t = 0; t < (uint32)m_clip.tracks.size(); ++t)
			addTrack(t, -1);
		return;
	}
	if (rowIdx < 0 || rowIdx >= (int32)m_rows.size())
		return;
	const Row& row = m_rows[rowIdx];
	if (row.track < 0 || row.track >= (int32)m_clip.tracks.size())
		return;
	addTrack((uint32)row.track, row.kind == RowKind::Channel ? (int32)row.channel : -1);
}

// ---- View ----

void AnimationEditor::frameAll(float width)
{
	const float duration = glm::max(m_clip.duration, 0.1f);
	m_pixelsPerSecond = glm::clamp((width - 24.0f) / (duration * 1.04f), 10.0f, 20000.0f);
	m_viewStart = -duration * 0.02f;
}

void AnimationEditor::zoomTime(float wheel, float mouseX)
{
	const float pivot = xToTime(mouseX);
	m_pixelsPerSecond = glm::clamp(m_pixelsPerSecond * glm::pow(1.18f, wheel), 10.0f, 20000.0f);
	m_viewStart = pivot - (mouseX - m_timelineX0) / m_pixelsPerSecond;
}

// ---- The area: list | timeline ----

void AnimationEditor::renderTimelineArea()
{
	buildRows();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (avail.x < 60.0f || avail.y < 40.0f)
		return;
	const float rowHeight = ImGui::GetFrameHeight();
	const float headerHeight = c_rulerHeight + rowHeight * 2.0f; // the ruler, the summary lane, the events lane
	const float splitter = 5.0f;
	const float listWidth = glm::clamp(m_listWidth, 150.0f, glm::max(avail.x - 220.0f, 150.0f));
	const float timelineWidth = glm::max(avail.x - listWidth - splitter, 40.0f);

	// ---- The track list ----
	if (ImGui::BeginChild("##ae_list_pane", ImVec2(listWidth, avail.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::SetNextItemWidth(listWidth - 120.0f);
		ImGui::InputTextWithHint("##ae_filter", "Filter bones", m_filter, sizeof(m_filter));
		ImGui::SameLine();
		ImGui::Checkbox("Animated", &m_animatedOnly);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Only the bones with keys (and their parents)");
		// The two lane labels, level with the lanes on the right.
		const ImVec2 lanes(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + c_rulerHeight + ImGui::GetStyle().WindowPadding.y);
		ImGui::SetCursorScreenPos(ImVec2(lanes.x + 8.0f, lanes.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f + 0.0f));
		ImGui::TextDisabled("Summary");
		ImGui::SetCursorScreenPos(ImVec2(lanes.x + 8.0f, lanes.y + rowHeight + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
		ImGui::TextDisabled("Events");
		ImGui::SetCursorScreenPos(ImVec2(lanes.x, lanes.y + rowHeight * 2.0f));
		renderTrackList(listWidth, glm::max(avail.y - headerHeight - ImGui::GetStyle().WindowPadding.y * 2.0f, 10.0f));
	}
	ImGui::EndChild();

	ImGui::SameLine(0.0f, 0.0f);
	ImGui::InvisibleButton("##ae_split_list", ImVec2(splitter, avail.y));
	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	if (ImGui::IsItemActive())
		m_listWidth = glm::clamp(m_listWidth + ImGui::GetIO().MouseDelta.x, 150.0f, glm::max(avail.x - 220.0f, 150.0f));
	ImGui::SameLine(0.0f, 0.0f);

	// ---- The timeline ----
	if (ImGui::BeginChild("##ae_time_pane", ImVec2(timelineWidth, avail.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		const ImVec2 pos = ImGui::GetWindowPos();
		const ImVec2 size = ImGui::GetWindowSize();
		const ImVec2 origin(pos.x + 1.0f, pos.y + ImGui::GetStyle().WindowPadding.y);
		const float width = size.x - 2.0f;
		m_timelineX0 = origin.x + 6.0f;
		if (m_frameAllPending)
		{
			frameAll(width - 6.0f);
			m_frameAllPending = false;
		}
		renderTimelineHeader(origin, width);
		const float contentTop = origin.y + c_rulerHeight + rowHeight * 2.0f;
		const float contentHeight = glm::max(pos.y + size.y - ImGui::GetStyle().WindowPadding.y - contentTop, 10.0f);
		if (m_curveView)
			renderCurves(ImVec2(origin.x, contentTop), width, contentHeight);
		else
			renderDopesheet(ImVec2(origin.x, contentTop), width, contentHeight);

		// The playhead, over everything in the pane.
		ImDrawList* list = ImGui::GetWindowDrawList();
		const float x = timeToX(m_time);
		if (x >= origin.x && x <= origin.x + width)
		{
			const ImU32 color = m_recording ? aeCol(255, 70, 70) : aeCol(255, 200, 80);
			list->AddLine(ImVec2(x, origin.y), ImVec2(x, pos.y + size.y), color, 1.5f);
			const ImVec2 tri[3] = { ImVec2(x - 5.0f, origin.y), ImVec2(x + 5.0f, origin.y), ImVec2(x, origin.y + 7.0f) };
			list->AddConvexPolyFilled(tri, 3, color);
		}
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::GetIO().WantTextInput)
		{
			frameAll(width - 6.0f);
			m_curveFitPending = true;
		}
	}
	ImGui::EndChild();
	renderTimelineContextMenu();
}

void AnimationEditor::renderTrackList(float width, float height)
{
	const float rowHeight = ImGui::GetFrameHeight();
	if (m_pendingListScroll >= 0.0f)
	{
		ImGui::SetNextWindowScroll(ImVec2(-1.0f, m_pendingListScroll));
		m_pendingListScroll = -1.0f;
	}
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
	if (ImGui::BeginChild("##ae_rows", ImVec2(width - ImGui::GetStyle().WindowPadding.x, height), ImGuiChildFlags_None, ImGuiWindowFlags_None))
	{
		m_rowScroll = ImGui::GetScrollY();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImGuiIO& io = ImGui::GetIO();
		ImGuiListClipper clipper;
		clipper.Begin((int)m_rows.size(), rowHeight);
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				const Row& row = m_rows[i];
				ImGui::PushID(i);
				const bool boneSelected = row.kind == RowKind::Bone && ((row.bone >= 0 && row.bone == m_selectedBone) || (row.bone < 0 && row.track == m_selectedTrack));
				const uint64 curveId = row.track >= 0 ? (uint64(row.track) << 2) | uint64(row.channel) : 0;
				const bool channelSelected = row.kind == RowKind::Channel && m_curveRows.count(curveId) != 0;
				const ImVec2 rowMin = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable("##row", boneSelected || channelSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight)))
				{
					const float indent = 6.0f + float(glm::min<int>(row.depth, 14)) * 11.0f;
					const bool onArrow = row.expandable && io.MousePos.x < rowMin.x + indent + 12.0f && io.MousePos.x >= rowMin.x + indent - 2.0f;
					oc::string name = row.bone >= 0 && m_skeleton ? m_skeleton->boneNames[row.bone] : (row.track >= 0 ? m_clip.tracks[row.track].bone : oc::string());
					if (onArrow || (row.kind == RowKind::Bone && row.expandable && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
					{
						if (m_expandedBones.count(name)) m_expandedBones.erase(name);
						else m_expandedBones.insert(name);
					}
					if (!onArrow)
					{
						m_selectedBone = row.bone;
						m_selectedTrack = row.bone < 0 ? row.track : -1;
						if (!io.KeyCtrl)
							m_curveRows.clear();
						if (row.track >= 0)
						{
							if (row.kind == RowKind::Channel)
								m_curveRows.insert(curveId);
							else
							{
								for (uint32 c = 0; c < c_numClipChannels; ++c)
									if (m_clip.tracks[row.track].numKeys(ClipChannel(c)) > 0 && ClipChannel(c) != ClipChannel::Scale)
										m_curveRows.insert((uint64(row.track) << 2) | c);
							}
						}
						m_curveFitPending = true;
					}
				}
				if (ImGui::BeginPopupContextItem("##row_ctx"))
				{
					if (row.bone >= 0 && ImGui::MenuItem("Key this bone (K)"))
					{
						m_selectedBone = row.bone;
						keySelectedBones();
					}
					if (row.track >= 0 && ImGui::MenuItem("Select all keys of this row"))
					{
						oc::vector<float> times;
						rowTimes(row, times);
						oc::vector<KeyRef> keys;
						for (float t : times)
						{
							collectKeysAt(i, t, keys);
							for (const KeyRef& k : keys)
								selectKey(k.track, k.channel, k.time, true);
						}
					}
					if (row.track >= 0 && row.kind == RowKind::Bone && ImGui::MenuItem("Delete track"))
					{
						deleteTrack(row.track);
					}
					if (row.track >= 0 && row.kind == RowKind::Channel && ImGui::MenuItem("Delete this channel's keys"))
					{
						const float latestBefore = latestContent();
						aeKeysOfMut(m_clip.tracks[row.track], row.channel, [](auto& keys) { keys.clear(); });
						clearKeySelection();
						shrinkToContent(latestBefore);
						markEdited();
					}
					ImGui::EndPopup();
				}

				// The label, drawn over the selectable: indent, fold arrow, name, and a channel's value at the playhead.
				const float indent = 6.0f + float(glm::min<int>(row.depth, 14)) * 11.0f;
				const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
				ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
				if (row.kind == RowKind::Bone)
				{
					if (row.expandable)
					{
						const oc::string& name = row.bone >= 0 && m_skeleton ? m_skeleton->boneNames[row.bone] : m_clip.tracks[row.track].bone;
						ImGui::RenderArrow(list, ImVec2(rowMin.x + indent, textY), ImGui::GetColorU32(ImGuiCol_Text),
							m_expandedBones.count(name) ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
					}
					oc::string label;
					if (row.bone >= 0 && m_skeleton)
					{
						label = aeShortName(m_skeleton->boneNames[row.bone]);
						if ((uint32)row.bone < m_boneIsHelper.size() && m_boneIsHelper[row.bone])
							textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
						else if (row.track < 0 || m_clip.tracks[row.track].empty())
							textColor = aeCol(170, 170, 170);
					}
					else
					{
						label = row.track >= 0 ? aeShortName(m_clip.tracks[row.track].bone) : oc::string("?");
						if (m_skeleton)
							textColor = aeCol(255, 175, 90); // not in the rig
					}
					if (m_overrides.count(row.bone))
						textColor = aeCol(255, 190, 90);
					list->AddText(ImVec2(rowMin.x + indent + 13.0f, textY), textColor, label.c_str());
					if (ImGui::IsItemHovered() && ImGui::GetIO().KeyAlt == false)
					{
						const oc::string& full = row.bone >= 0 && m_skeleton ? m_skeleton->boneNames[row.bone] : m_clip.tracks[row.track].bone;
						if (full != label)
							ImGui::SetItemTooltip("%s", full.c_str());
					}
				}
				else
				{
					const uint32 c = (uint32)row.channel;
					list->AddText(ImVec2(rowMin.x + indent + 13.0f, textY), aeCol(200, 200, 210), c_channelNames[c]);
					// The value at the playhead.
					char buf[64] = {};
					const ClipTrack& track = m_clip.tracks[row.track];
					if (row.channel == ClipChannel::Rotation)
					{
						const glm::quat q = AnimKeys::sampleQuat(oc::span<const RotationKey>(track.rotationKeys), m_time, glm::quat(1, 0, 0, 0));
						const glm::vec3 e = glm::degrees(glm::eulerAngles(q));
						snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", e.x, e.y, e.z);
					}
					else
					{
						const glm::vec3 v = row.channel == ClipChannel::Position
							? AnimKeys::sampleVec3(oc::span<const PositionKey>(track.positionKeys), m_time, glm::vec3(0.0f))
							: AnimKeys::sampleVec3(oc::span<const ScaleKey>(track.scaleKeys), m_time, glm::vec3(1.0f));
						snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", v.x, v.y, v.z);
					}
					const float textWidth = ImGui::CalcTextSize(buf).x;
					list->AddText(ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x - textWidth - 4.0f, textY), aeCol(150, 150, 160), buf);
				}
				ImGui::PopID();
			}
		}
		clipper.End();
		if (m_rows.empty())
			ImGui::TextDisabled(m_skeleton ? "  No animated bones: pose one and key it (K)" : "  No tracks");
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
}

// ---- The ruler, the summary lane, the events lane ----

void AnimationEditor::renderTimelineHeader(const ImVec2& origin, float width)
{
	ImDrawList* list = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const float rowHeight = ImGui::GetFrameHeight();
	const float step = gridStep();
	const float x1 = origin.x + width;
	list->PushClipRect(origin, ImVec2(x1, origin.y + c_rulerHeight + rowHeight * 2.0f), true);

	// ---- The ruler: the key steps (labelled by index, or in seconds) ----
	list->AddRectFilled(origin, ImVec2(x1, origin.y + c_rulerHeight), aeCol(38, 40, 46));
	const float pixelsPerFrame = m_pixelsPerSecond * step;
	const int majorStep = aeMajorStep(pixelsPerFrame);
	const int firstFrame = (int)glm::floor(xToTime(origin.x) / step);
	const int lastFrame = (int)glm::ceil(xToTime(x1) / step);
	for (int f = firstFrame; f <= lastFrame; ++f)
	{
		if (f < 0)
			continue;
		const float x = timeToX(float(f) * step);
		if (f % majorStep == 0)
		{
			list->AddLine(ImVec2(x, origin.y + 6.0f), ImVec2(x, origin.y + c_rulerHeight), aeCol(150, 150, 160));
			char label[32];
			if (m_showSeconds)
				snprintf(label, sizeof(label), "%.2fs", float(f) * step);
			else
				snprintf(label, sizeof(label), "%d", f);
			list->AddText(ImVec2(x + 3.0f, origin.y + 2.0f), aeCol(190, 190, 200), label);
		}
		else if (pixelsPerFrame >= 5.0f)
			list->AddLine(ImVec2(x, origin.y + c_rulerHeight - 5.0f), ImVec2(x, origin.y + c_rulerHeight), aeCol(110, 110, 118));
	}
	const float endX = timeToX(m_clip.duration);
	list->AddLine(ImVec2(endX, origin.y), ImVec2(endX, origin.y + c_rulerHeight + rowHeight * 2.0f), aeCol(220, 220, 230, 140), 1.0f);

	ImGui::SetCursorScreenPos(origin);
	ImGui::InvisibleButton("##ae_ruler", ImVec2(width, c_rulerHeight));
	if (ImGui::IsItemActive())
	{
		m_playing = false;
		const float t = xToTime(io.MousePos.x);
		setTime(m_snap ? snapTime(t) : t);
	}
	if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
		zoomTime(io.MouseWheel, io.MousePos.x);
	if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
		ImGui::SetTooltip("Drag: scrub (past the end: the clip grows)   Wheel: zoom   F: frame all");

	// ---- The summary lane: every key time of the clip ----
	const float summaryY = origin.y + c_rulerHeight;
	list->AddRectFilled(ImVec2(origin.x, summaryY), ImVec2(x1, summaryY + rowHeight), aeCol(44, 46, 54));
	oc::vector<Diamond> diamonds;
	gatherDiamonds(c_summaryRow, diamonds);
	ImGui::SetCursorScreenPos(ImVec2(origin.x, summaryY));
	ImGui::InvisibleButton("##ae_summary", ImVec2(width, rowHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool summaryHovered = ImGui::IsItemHovered();
	float hoverTime = 0.0f;
	bool hoverHit = false;
	float bestDist = 7.0f;
	for (const Diamond& d : diamonds)
	{
		const float dist = glm::abs(timeToX(d.time) - io.MousePos.x);
		if (summaryHovered && dist < bestDist)
		{
			bestDist = dist;
			hoverTime = d.time;
			hoverHit = true;
		}
	}
	oc::vector<KeyRef> keys;
	for (const Diamond& d : diamonds)
	{
		const bool all = d.selected == d.count;
		float x = timeToX(d.time);
		if (m_drag == DragMode::MoveKeys && all)
			x = timeToX(glm::max(d.time + m_dragDelta, 0.0f));
		if (x < origin.x - 8.0f || x > x1 + 8.0f)
			continue;
		const bool hot = hoverHit && d.time == hoverTime;
		const ImU32 fill = all ? aeCol(255, 175, 60) : d.selected > 0 ? aeCol(200, 140, 70) : hot ? aeCol(230, 230, 235) : aeCol(170, 170, 178);
		aeDiamond(list, ImVec2(x, summaryY + rowHeight * 0.5f), 5.0f, fill, aeCol(20, 20, 24));
	}
	if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		handleKeyPress(c_summaryRow, hoverTime, hoverHit);
	if (ImGui::IsItemActive() && m_drag != DragMode::None)
		handleKeyDrag(false);
	if (ImGui::IsItemDeactivated() && m_drag != DragMode::None)
		handleKeyDrag(true);
	if (summaryHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_contextTime = hoverHit ? hoverTime : xToTime(io.MousePos.x);
		m_contextRow = c_summaryRow;
		if (hoverHit)
		{
			collectKeysAt(c_summaryRow, hoverTime, keys);
			bool all = !keys.empty();
			for (const KeyRef& k : keys)
				all &= isKeySelected(k.track, k.channel, k.time);
			if (!all)
			{
				clearKeySelection();
				for (const KeyRef& k : keys)
					selectKey(k.track, k.channel, k.time, true);
			}
		}
		ImGui::OpenPopup("##ae_tl_ctx");
	}

	// ---- The events lane ----
	const float eventsY = summaryY + rowHeight;
	list->AddRectFilled(ImVec2(origin.x, eventsY), ImVec2(x1, eventsY + rowHeight), aeCol(36, 38, 44));
	ImGui::SetCursorScreenPos(ImVec2(origin.x, eventsY));
	ImGui::InvisibleButton("##ae_events", ImVec2(width, rowHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool eventsHovered = ImGui::IsItemHovered();
	const float duration = glm::max(m_clip.duration, 1e-6f);
	int32 hoverEvent = -1;
	bestDist = 6.0f;
	for (int32 e = 0; e < (int32)m_clip.events.size(); ++e)
	{
		const float x = timeToX(m_clip.events[e].normalizedTime * duration);
		const float d = glm::abs(x - io.MousePos.x);
		if (eventsHovered && d < bestDist)
		{
			bestDist = d;
			hoverEvent = e;
		}
	}
	for (int32 e = 0; e < (int32)m_clip.events.size(); ++e)
	{
		const AnimationEventKey& ev = m_clip.events[e];
		const float x = timeToX(ev.normalizedTime * duration);
		const ImU32 color = e == m_selectedEvent ? aeCol(255, 175, 60) : e == hoverEvent ? aeCol(150, 230, 255) : aeCol(110, 190, 240);
		list->AddLine(ImVec2(x, eventsY + 2.0f), ImVec2(x, eventsY + rowHeight - 2.0f), color, 2.0f);
		const ImVec2 flag[3] = { ImVec2(x, eventsY + 2.0f), ImVec2(x + 8.0f, eventsY + 6.0f), ImVec2(x, eventsY + 10.0f) };
		list->AddConvexPolyFilled(flag, 3, color);
		if (pixelsPerFrame > 2.0f || e == m_selectedEvent)
			list->AddText(ImVec2(x + 10.0f, eventsY + 2.0f), aeCol(200, 210, 220), ev.name.c_str());
	}
	if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_selectedEvent = hoverEvent;
		if (hoverEvent >= 0)
			m_drag = DragMode::MoveEvent;
		else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const float t = snapTime(xToTime(io.MousePos.x));
			extendTo(t);
			m_clip.events.push_back({ "event", glm::clamp(t / glm::max(m_clip.duration, 1e-6f), 0.0f, 1.0f) });
			m_selectedEvent = (int32)m_clip.events.size() - 1;
			markEdited();
		}
	}
	if (ImGui::IsItemActive() && m_drag == DragMode::MoveEvent && m_selectedEvent >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
	{
		// Like a key: past the end the clip grows, and leaving the end empty shrinks it.
		const float t = snapTime(xToTime(io.MousePos.x));
		const float latestBefore = latestContent();
		if (glm::abs(m_clip.events[m_selectedEvent].normalizedTime * m_clip.duration - t) > 1e-6f)
		{
			extendTo(t);
			m_clip.events[m_selectedEvent].normalizedTime = glm::clamp(t / glm::max(m_clip.duration, 1e-6f), 0.0f, 1.0f);
			shrinkToContent(latestBefore);
			markEdited();
		}
	}
	if (ImGui::IsItemDeactivated() && m_drag == DragMode::MoveEvent)
		m_drag = DragMode::None;
	if (eventsHovered && !ImGui::IsItemActive())
		ImGui::SetTooltip(hoverEvent >= 0 ? "%s (drag to move, Delete removes)" : "Double-click: add an event   Right-click: menu",
			hoverEvent >= 0 ? m_clip.events[hoverEvent].name.c_str() : "");
	if (eventsHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_selectedEvent = hoverEvent;
		m_contextTime = xToTime(io.MousePos.x);
		m_contextRow = -3; // the events lane
		ImGui::OpenPopup("##ae_tl_ctx");
	}

	list->PopClipRect();
}

// ---- Key press / drag, shared by the summary lane and the dopesheet ----

void AnimationEditor::handleKeyPress(int32 rowIdx, float keyTime, bool hitKey)
{
	const ImGuiIO& io = ImGui::GetIO();
	m_dragStart = io.MousePos;
	m_dragMoved = false;
	m_dragDelta = 0.0f;
	if (!hitKey)
	{
		if (!io.KeyShift && !io.KeyCtrl)
			clearKeySelection();
		m_drag = DragMode::BoxSelect;
		m_selectedEvent = -1;
		return;
	}
	oc::vector<KeyRef> keys;
	collectKeysAt(rowIdx, keyTime, keys);
	bool all = !keys.empty();
	for (const KeyRef& k : keys)
		all &= isKeySelected(k.track, k.channel, k.time);
	if (io.KeyCtrl)
	{
		for (const KeyRef& k : keys)
			selectKey(k.track, k.channel, k.time, !all);
		m_drag = DragMode::None;
		return;
	}
	if (!all)
	{
		if (!io.KeyShift)
			clearKeySelection();
		for (const KeyRef& k : keys)
			selectKey(k.track, k.channel, k.time, true);
	}
	m_drag = DragMode::MoveKeys;
	m_dragAnchorTime = keyTime;
	m_selectedEvent = -1;
}

void AnimationEditor::handleKeyDrag(bool released)
{
	const ImGuiIO& io = ImGui::GetIO();
	if (glm::abs(io.MousePos.x - m_dragStart.x) > 2.0f || glm::abs(io.MousePos.y - m_dragStart.y) > 2.0f)
		m_dragMoved = true;
	if (m_drag == DragMode::MoveKeys)
	{
		const float raw = xToTime(io.MousePos.x) - xToTime(m_dragStart.x);
		float delta = m_snap ? snapTime(glm::max(m_dragAnchorTime + raw, 0.0f)) - m_dragAnchorTime : raw;
		if (!m_dragMoved)
			delta = 0.0f;
		m_dragDelta = delta;
		if (released)
		{
			if (delta != 0.0f)
				moveSelectedKeys(delta);
			else if (!m_dragMoved && m_keySelection.size() > 0)
			{
				// A plain click on a key also puts the playhead there, like the ruler.
				setTime(m_dragAnchorTime);
			}
			m_drag = DragMode::None;
			m_dragDelta = 0.0f;
		}
		return;
	}
	if (m_drag == DragMode::Pan)
	{
		m_viewStart = m_dragViewStart - (io.MousePos.x - m_dragStart.x) / m_pixelsPerSecond;
		if (released)
			m_drag = DragMode::None;
		return;
	}
	if (m_drag == DragMode::BoxSelect && released)
	{
		const float rowHeight = ImGui::GetFrameHeight();
		const float tA = xToTime(glm::min(m_dragStart.x, io.MousePos.x));
		const float tB = xToTime(glm::max(m_dragStart.x, io.MousePos.x));
		const float yA = glm::min(m_dragStart.y, io.MousePos.y);
		const float yB = glm::max(m_dragStart.y, io.MousePos.y);
		if (m_dragMoved)
		{
			// The lanes the box crosses: the dopesheet rows by their screen y (see renderDopesheet), the summary lane by its own.
			oc::vector<float> times;
			oc::vector<KeyRef> keys;
			for (int32 r = 0; r < (int32)m_rows.size(); ++r)
			{
				const float y0 = m_dopesheetTop + float(r) * rowHeight - m_rowScroll;
				if (y0 + rowHeight < yA || y0 > yB)
					continue;
				rowTimes(m_rows[r], times);
				for (float t : times)
					if (t >= tA && t <= tB)
					{
						collectKeysAt(r, t, keys);
						for (const KeyRef& k : keys)
							selectKey(k.track, k.channel, k.time, true);
					}
			}
		}
		else if (!io.KeyShift && !io.KeyCtrl)
		{
			// A click on empty space: the playhead goes there.
			const float t = xToTime(io.MousePos.x);
			setTime(m_snap ? snapTime(t) : t);
			m_playing = false;
		}
		m_drag = DragMode::None;
	}
	else if (released)
		m_drag = DragMode::None;
}

// ---- The dopesheet ----

void AnimationEditor::renderDopesheet(const ImVec2& origin, float width, float height)
{
	ImDrawList* list = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const float rowHeight = ImGui::GetFrameHeight();
	const float x1 = origin.x + width;
	const float y1 = origin.y + height;
	m_dopesheetTop = origin.y;

	ImGui::SetCursorScreenPos(origin);
	ImGui::InvisibleButton("##ae_dope", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool activated = ImGui::IsItemActivated();
	const bool active = ImGui::IsItemActive();
	const bool deactivated = ImGui::IsItemDeactivated();

	list->PushClipRect(origin, ImVec2(x1, y1), true);
	list->AddRectFilled(origin, ImVec2(x1, y1), aeCol(30, 31, 36));
	// Beyond the clip's end.
	const float endX = timeToX(m_clip.duration);
	if (endX < x1)
		list->AddRectFilled(ImVec2(glm::max(endX, origin.x), origin.y), ImVec2(x1, y1), aeCol(0, 0, 0, 70));
	if (timeToX(0.0f) > origin.x)
		list->AddRectFilled(origin, ImVec2(timeToX(0.0f), y1), aeCol(0, 0, 0, 70));

	// The key-step grid: every step faint when there is room, the ruler's labelled steps brighter.
	const float step = gridStep();
	const float pixelsPerFrame = m_pixelsPerSecond * step;
	const int majorStep = aeMajorStep(pixelsPerFrame);
	for (int f = glm::max((int)glm::floor(xToTime(origin.x) / step), 0); f <= (int)glm::ceil(xToTime(x1) / step); ++f)
	{
		const float x = timeToX(float(f) * step);
		if (f % majorStep == 0)
			list->AddLine(ImVec2(x, origin.y), ImVec2(x, y1), aeCol(255, 255, 255, 22));
		else if (pixelsPerFrame >= 8.0f)
			list->AddLine(ImVec2(x, origin.y), ImVec2(x, y1), aeCol(255, 255, 255, 8));
	}

	// ---- The rows ----
	const int32 firstRow = glm::max((int32)glm::floor(m_rowScroll / rowHeight), 0);
	const int32 lastRow = glm::min((int32)glm::ceil((m_rowScroll + height) / rowHeight), (int32)m_rows.size() - 1);
	const int32 mouseRow = hovered ? (int32)glm::floor((io.MousePos.y - origin.y + m_rowScroll) / rowHeight) : -1;
	int32 hoverRow = -1;
	float hoverTime = 0.0f;
	bool hoverHit = false;
	oc::vector<Diamond> diamonds;
	oc::vector<KeyRef> keys;
	for (int32 r = firstRow; r <= lastRow; ++r)
	{
		const Row& row = m_rows[r];
		const float y0 = origin.y + float(r) * rowHeight - m_rowScroll;
		const float cy = y0 + rowHeight * 0.5f;
		const bool rowSelected = row.kind == RowKind::Bone && ((row.bone >= 0 && row.bone == m_selectedBone) || (row.bone < 0 && row.track == m_selectedTrack));
		if (rowSelected)
			list->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x1, y0 + rowHeight), aeCol(70, 90, 130, 70));
		else if (r & 1)
			list->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x1, y0 + rowHeight), aeCol(255, 255, 255, 6));
		list->AddLine(ImVec2(origin.x, y0 + rowHeight), ImVec2(x1, y0 + rowHeight), aeCol(0, 0, 0, 60));

		gatherDiamonds(r, diamonds);
		if (r == mouseRow)
		{
			float bestDist = 7.0f;
			for (const Diamond& d : diamonds)
			{
				const float dist = glm::abs(timeToX(d.time) - io.MousePos.x);
				if (dist < bestDist)
				{
					bestDist = dist;
					hoverTime = d.time;
					hoverHit = true;
				}
			}
			hoverRow = r;
		}
		const float radius = row.kind == RowKind::Channel ? 4.5f : 5.5f;
		for (const Diamond& d : diamonds)
		{
			const float t = d.time;
			const KeyInterp interp = row.kind == RowKind::Channel ? d.interp : KeyInterp::Linear;
			const bool all = d.selected == d.count;
			float x = timeToX(t);
			if (m_drag == DragMode::MoveKeys && all)
				x = timeToX(glm::max(t + m_dragDelta, 0.0f));
			if (x < origin.x - 8.0f || x > x1 + 8.0f)
				continue;
			const bool hot = hoverHit && r == hoverRow && t == hoverTime;
			ImU32 fill = row.kind == RowKind::Bone ? aeCol(150, 150, 160) : aeCol(185, 185, 195);
			if (all) fill = aeCol(255, 175, 60);
			else if (d.selected > 0) fill = aeCol(200, 140, 70);
			else if (hot) fill = aeCol(235, 235, 240);
			if (interp == KeyInterp::Step)
				list->AddRectFilled(ImVec2(x - radius * 0.8f, cy - radius * 0.8f), ImVec2(x + radius * 0.8f, cy + radius * 0.8f), fill);
			else if (interp == KeyInterp::Smooth)
				list->AddCircleFilled(ImVec2(x, cy), radius * 0.85f, fill);
			else
				aeDiamond(list, ImVec2(x, cy), radius, fill, aeCol(20, 20, 24));
		}
	}

	// The box.
	if (m_drag == DragMode::BoxSelect && active && m_dragMoved)
	{
		const ImVec2 a(glm::min(m_dragStart.x, io.MousePos.x), glm::min(m_dragStart.y, io.MousePos.y));
		const ImVec2 b(glm::max(m_dragStart.x, io.MousePos.x), glm::max(m_dragStart.y, io.MousePos.y));
		list->AddRectFilled(a, b, aeCol(110, 160, 255, 40));
		list->AddRect(a, b, aeCol(110, 160, 255, 160));
	}
	if (m_rows.empty())
		list->AddText(ImVec2(origin.x + 12.0f, origin.y + 8.0f), aeCol(150, 150, 150),
			m_recording ? "Recording: pose a bone in the preview to key it." : "Turn on Rec and pose a bone, or select one and press K.");
	list->PopClipRect();

	// ---- Input ----
	if (activated)
	{
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			m_drag = DragMode::Pan;
			m_dragStart = io.MousePos;
			m_dragViewStart = m_viewStart;
		}
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !hoverHit && hoverRow >= 0)
			{
				// Double-click on empty space keys the row's bone (a channel row: that channel) at that frame.
				const Row& row = m_rows[hoverRow];
				if (row.bone >= 0)
				{
					m_playing = false;
					setTime(snapTime(xToTime(io.MousePos.x)));
					rebuildRuntimeClip();
					m_player.setTime(m_time);
					m_selectedBone = row.bone;
					if (row.kind == RowKind::Channel)
						keyBone(row.bone, row.channel == ClipChannel::Position, row.channel == ClipChannel::Rotation, row.channel == ClipChannel::Scale);
					else
						keySelectedBones();
				}
				m_drag = DragMode::None;
			}
			else
			{
				if (hoverRow >= 0 && hoverHit)
				{
					const Row& row = m_rows[hoverRow];
					m_selectedBone = row.bone;
					m_selectedTrack = row.bone < 0 ? row.track : -1;
				}
				handleKeyPress(hoverRow, hoverTime, hoverHit && hoverRow >= 0);
			}
		}
	}
	if (active && m_drag != DragMode::None)
		handleKeyDrag(false);
	if (deactivated && m_drag != DragMode::None)
		handleKeyDrag(true);

	if (hovered && io.MouseWheel != 0.0f)
	{
		if (io.KeyCtrl)
			zoomTime(io.MouseWheel, io.MousePos.x);
		else if (io.KeyShift)
			m_viewStart -= io.MouseWheel * 60.0f / m_pixelsPerSecond;
		else
		{
			const float maxScroll = glm::max(float(m_rows.size()) * rowHeight - height, 0.0f);
			m_pendingListScroll = glm::clamp(m_rowScroll - io.MouseWheel * rowHeight * 3.0f, 0.0f, maxScroll);
		}
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_contextTime = hoverHit ? hoverTime : xToTime(io.MousePos.x);
		m_contextRow = hoverRow;
		if (hoverHit && hoverRow >= 0)
		{
			collectKeysAt(hoverRow, hoverTime, keys);
			bool all = !keys.empty();
			for (const KeyRef& k : keys)
				all &= isKeySelected(k.track, k.channel, k.time);
			if (!all)
			{
				clearKeySelection();
				for (const KeyRef& k : keys)
					selectKey(k.track, k.channel, k.time, true);
			}
		}
		ImGui::OpenPopup("##ae_tl_ctx");
	}
	if (hovered && m_drag == DragMode::None && !ImGui::IsAnyItemActive())
		ImGui::SetItemTooltip("Click: select / move the playhead   Drag: move keys or box-select   Double-click: key the row's bone\n"
			"Ctrl+wheel: zoom   Shift+wheel / middle drag: pan   Right-click: menu");
}

void AnimationEditor::renderTimelineContextMenu()
{
	if (!ImGui::BeginPopup("##ae_tl_ctx"))
		return;
	const float t = snapTime(m_contextTime);
	if (m_contextRow == -3)
	{
		if (ImGui::MenuItem("Add event here"))
		{
			extendTo(t);
			m_clip.events.push_back({ "event", glm::clamp(t / glm::max(m_clip.duration, 1e-6f), 0.0f, 1.0f) });
			m_selectedEvent = (int32)m_clip.events.size() - 1;
			markEdited();
		}
		if (m_selectedEvent >= 0 && m_selectedEvent < (int32)m_clip.events.size() && ImGui::MenuItem("Delete event"))
		{
			deleteEvent(m_selectedEvent);
		}
		ImGui::EndPopup();
		return;
	}
	const bool hasSelection = !m_keySelection.empty();
	if (ImGui::BeginMenu("Blend", hasSelection))
	{
		if (ImGui::MenuItem("Linear")) setSelectedKeysInterp(KeyInterp::Linear);
		if (ImGui::MenuItem("Smooth")) setSelectedKeysInterp(KeyInterp::Smooth);
		if (ImGui::MenuItem("Step"))   setSelectedKeysInterp(KeyInterp::Step);
		ImGui::EndMenu();
	}
	if (ImGui::MenuItem("Delete keys", "Del", false, hasSelection))
		deleteSelectedKeys();
	if (ImGui::MenuItem("Copy keys", "Ctrl+C", false, hasSelection))
		copySelectedKeys();
	if (ImGui::MenuItem("Paste keys here", "Ctrl+V", false, !m_clipboard.empty()))
		pasteKeys(t);
	ImGui::Separator();
	if (m_contextRow >= 0 && m_contextRow < (int32)m_rows.size() && m_rows[m_contextRow].bone >= 0 && ImGui::MenuItem("Key the bone here"))
	{
		const Row& row = m_rows[m_contextRow];
		setTime(t);
		rebuildRuntimeClip();
		m_player.setTime(m_time);
		m_selectedBone = row.bone;
		keySelectedBones();
	}
	if (ImGui::MenuItem("Add event here"))
	{
		extendTo(t);
		m_clip.events.push_back({ "event", glm::clamp(t / glm::max(m_clip.duration, 1e-6f), 0.0f, 1.0f) });
		m_selectedEvent = (int32)m_clip.events.size() - 1;
		markEdited();
	}
	if (ImGui::MenuItem("Set the clip's end here", nullptr, false, t > 0.0f))
		setClipDuration(t, false);
	ImGui::EndPopup();
}

// ---- The curve view ----

void AnimationEditor::renderCurves(const ImVec2& origin, float width, float height)
{
	ImDrawList* list = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const float x1 = origin.x + width;
	const float y1 = origin.y + height;
	m_dopesheetTop = origin.y;

	// The channels drawn: the picked rows, else the selected bone's position and rotation.
	struct Curve
	{
		uint32 track;
		ClipChannel channel;
	};
	oc::small_vector<Curve, 8> curves;
	for (uint64 id : m_curveRows)
		if ((id >> 2) < m_clip.tracks.size())
			curves.push_back({ uint32(id >> 2), ClipChannel(id & 3u) });
	if (curves.empty())
	{
		const int32 track = m_skeleton && m_selectedBone >= 0 && (uint32)m_selectedBone < m_boneTrack.size() ? m_boneTrack[m_selectedBone] : m_selectedTrack;
		if (track >= 0 && track < (int32)m_clip.tracks.size())
			for (uint32 c = 0; c < 2; ++c)
				if (m_clip.tracks[track].numKeys(ClipChannel(c)) > 0)
					curves.push_back({ (uint32)track, ClipChannel(c) });
	}
	oc::sort(curves.begin(), curves.end(), [](const Curve& a, const Curve& b) { return a.track != b.track ? a.track < b.track : a.channel < b.channel; });

	// ---- Samples: one point per 2 px over the clip, the rotations as continuous Euler degrees ----
	const float duration = glm::max(m_clip.duration, 1e-4f);
	const float tStart = glm::max(xToTime(origin.x), 0.0f);
	const float tEnd = glm::min(xToTime(x1), duration);
	struct CurveSamples
	{
		oc::vector<glm::vec3> values; // per sample
		oc::vector<glm::vec3> keyValues;
		oc::vector<float> keyTimes;
	};
	oc::vector<CurveSamples> samples(curves.size());
	const float step = 2.0f / m_pixelsPerSecond;
	const int32 numSamples = tEnd > tStart ? (int32)glm::ceil((tEnd - tStart) / step) + 1 : 0;
	float vMin = FLT_MAX, vMax = -FLT_MAX;
	for (size_t ci = 0; ci < curves.size(); ++ci)
	{
		const ClipTrack& track = m_clip.tracks[curves[ci].track];
		CurveSamples& s = samples[ci];
		if (curves[ci].channel == ClipChannel::Rotation)
		{
			const oc::span<const RotationKey> keys(track.rotationKeys);
			// Unwrap from t = 0, so the visible part matches however far the view is scrolled.
			glm::vec3 prev = glm::degrees(glm::eulerAngles(AnimKeys::sampleQuat(keys, 0.0f, glm::quat(1, 0, 0, 0))));
			const float coarse = glm::max(step, duration / 2000.0f);
			for (float t = 0.0f; t < tStart; t += coarse)
				prev = aeEulerNear(AnimKeys::sampleQuat(keys, t, glm::quat(1, 0, 0, 0)), prev);
			for (int32 i = 0; i < numSamples; ++i)
			{
				const float t = glm::min(tStart + float(i) * step, tEnd);
				prev = aeEulerNear(AnimKeys::sampleQuat(keys, t, glm::quat(1, 0, 0, 0)), prev);
				s.values.push_back(prev);
			}
			glm::vec3 near = glm::degrees(glm::eulerAngles(keys.empty() ? glm::quat(1, 0, 0, 0) : keys[0].value));
			for (const RotationKey& k : keys)
			{
				// Each key onto the curve's branch at its time.
				const int32 idx = (int32)glm::round((k.time - tStart) / step);
				if (idx >= 0 && idx < (int32)s.values.size())
					near = s.values[idx];
				near = aeEulerNear(k.value, near);
				s.keyValues.push_back(near);
				s.keyTimes.push_back(k.time);
			}
		}
		else
		{
			aeKeysOf(track, curves[ci].channel, [&](const auto& keys)
			{
				using KeyT = typename std::remove_cvref_t<decltype(keys)>::value_type;
				if constexpr (!std::is_same_v<KeyT, RotationKey>)
				{
					const oc::span<const KeyT> span(keys);
					const glm::vec3 fallback = curves[ci].channel == ClipChannel::Scale ? glm::vec3(1.0f) : glm::vec3(0.0f);
					for (int32 i = 0; i < numSamples; ++i)
						s.values.push_back(AnimKeys::sampleVec3(span, glm::min(tStart + float(i) * step, tEnd), fallback));
					for (const auto& k : keys)
					{
						s.keyValues.push_back(k.value);
						s.keyTimes.push_back(k.time);
					}
				}
			});
		}
		if (m_curveFitPending)
			for (const glm::vec3& v : s.keyValues)
				for (int c = 0; c < 3; ++c)
				{
					vMin = glm::min(vMin, v[c]);
					vMax = glm::max(vMax, v[c]);
				}
	}
	if (m_curveFitPending && vMin <= vMax)
	{
		const float pad = glm::max((vMax - vMin) * 0.12f, 0.01f);
		m_curveMin = vMin - pad;
		m_curveMax = vMax + pad;
		m_curveFitPending = false;
	}
	if (m_curveMax - m_curveMin < 1e-5f)
		m_curveMax = m_curveMin + 1.0f;
	auto valueToY = [&](float v) { return origin.y + (m_curveMax - v) / (m_curveMax - m_curveMin) * height; };
	auto yToValue = [&](float y) { return m_curveMax - (y - origin.y) / height * (m_curveMax - m_curveMin); };

	ImGui::SetCursorScreenPos(origin);
	ImGui::InvisibleButton("##ae_curves", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool activated = ImGui::IsItemActivated();
	const bool active = ImGui::IsItemActive();
	const bool deactivated = ImGui::IsItemDeactivated();

	list->PushClipRect(origin, ImVec2(x1, y1), true);
	list->AddRectFilled(origin, ImVec2(x1, y1), aeCol(28, 29, 34));
	const float endX = timeToX(m_clip.duration);
	if (endX < x1)
		list->AddRectFilled(ImVec2(glm::max(endX, origin.x), origin.y), ImVec2(x1, y1), aeCol(0, 0, 0, 70));

	// Value grid: a 1-2-5 step that keeps lines about 40 px apart.
	const float range = m_curveMax - m_curveMin;
	const float rawStep = range * 40.0f / glm::max(height, 1.0f);
	const float magnitude = glm::pow(10.0f, glm::floor(glm::log(rawStep) / glm::log(10.0f)));
	float valueStep = magnitude;
	for (float m : { 1.0f, 2.0f, 5.0f, 10.0f })
		if (magnitude * m >= rawStep) { valueStep = magnitude * m; break; }
	for (float v = glm::ceil(m_curveMin / valueStep) * valueStep; v <= m_curveMax; v += valueStep)
	{
		const float y = valueToY(v);
		list->AddLine(ImVec2(origin.x, y), ImVec2(x1, y), glm::abs(v) < valueStep * 0.01f ? aeCol(255, 255, 255, 45) : aeCol(255, 255, 255, 16));
		char label[32];
		snprintf(label, sizeof(label), "%g", glm::abs(v) < valueStep * 0.01f ? 0.0f : v);
		list->AddText(ImVec2(origin.x + 4.0f, y - ImGui::GetTextLineHeight()), aeCol(140, 140, 150), label);
	}

	// ---- The curves and their keys ----
	struct Hit
	{
		size_t curve;
		int32 component;
		int32 key;
		float dist;
	};
	Hit hit{ 0, -1, -1, 7.0f };
	for (size_t ci = 0; ci < curves.size(); ++ci)
	{
		const CurveSamples& s = samples[ci];
		for (int c = 0; c < 3; ++c)
		{
			const ImU32 color = c_componentColors[c];
			ImVec2 prev;
			for (int32 i = 0; i < (int32)s.values.size(); ++i)
			{
				const ImVec2 p(timeToX(glm::min(tStart + float(i) * step, tEnd)), valueToY(s.values[i][c]));
				if (i > 0)
					list->AddLine(prev, p, curves[ci].channel == ClipChannel::Rotation ? color : (color & 0x00FFFFFFu) | 0xD0000000u, 1.5f);
				prev = p;
			}
			for (int32 k = 0; k < (int32)s.keyValues.size(); ++k)
			{
				const ImVec2 p(timeToX(s.keyTimes[k]), valueToY(s.keyValues[k][c]));
				const float d = glm::length(glm::vec2(p.x - io.MousePos.x, p.y - io.MousePos.y));
				if (hovered && d < hit.dist)
					hit = Hit{ ci, c, k, d };
				const bool selected = isKeySelected(curves[ci].track, curves[ci].channel, s.keyTimes[k]);
				list->AddCircleFilled(p, selected ? 4.5f : 3.5f, selected ? aeCol(255, 175, 60) : aeCol(235, 235, 240));
				list->AddCircle(p, selected ? 4.5f : 3.5f, color);
			}
		}
	}
	// The legend.
	float legendY = origin.y + 4.0f;
	for (const Curve& curve : curves)
	{
		const oc::string label = aeShortName(m_clip.tracks[curve.track].bone) + " " + c_channelNames[(uint32)curve.channel]
			+ (curve.channel == ClipChannel::Rotation ? " (deg)" : "");
		const float w = ImGui::CalcTextSize(label.c_str()).x;
		list->AddText(ImVec2(x1 - w - 8.0f, legendY), aeCol(200, 200, 210), label.c_str());
		legendY += ImGui::GetTextLineHeight();
	}
	if (curves.empty())
		list->AddText(ImVec2(origin.x + 12.0f, origin.y + 8.0f), aeCol(150, 150, 150), "Select a bone or channel rows (Ctrl+click) to see their curves.");
	list->PopClipRect();

	// ---- Input ----
	if (activated)
	{
		m_dragStart = io.MousePos;
		m_dragMoved = false;
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			m_drag = DragMode::Pan;
			m_dragViewStart = m_viewStart;
		}
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (hit.component >= 0)
			{
				const Curve& curve = curves[hit.curve];
				const float t = samples[hit.curve].keyTimes[hit.key];
				if (!io.KeyShift && !io.KeyCtrl)
					clearKeySelection();
				selectKey(curve.track, curve.channel, t, true);
				m_curveDragKey = KeyRef{ curve.track, curve.channel, t };
				m_curveDragComponent = hit.component;
				m_drag = DragMode::CurveKey;
			}
			else
			{
				if (!io.KeyShift && !io.KeyCtrl)
					clearKeySelection();
				const float t = xToTime(io.MousePos.x);
				setTime(m_snap ? snapTime(t) : t);
				m_playing = false;
				m_drag = DragMode::None;
			}
		}
	}
	if (active && m_drag == DragMode::Pan)
	{
		m_viewStart = m_dragViewStart - (io.MousePos.x - m_dragStart.x) / m_pixelsPerSecond;
		const float dv = io.MouseDelta.y / glm::max(height, 1.0f) * (m_curveMax - m_curveMin);
		m_curveMin += dv;
		m_curveMax += dv;
	}
	if (active && m_drag == DragMode::CurveKey && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && m_curveDragKey.track < m_clip.tracks.size())
	{
		ClipTrack& track = m_clip.tracks[m_curveDragKey.track];
		const float latestBefore = latestContent();
		const float newValue = yToValue(io.MousePos.y);
		const float newTime = m_snap ? snapTime(xToTime(io.MousePos.x)) : glm::max(xToTime(io.MousePos.x), 0.0f);
		const int c = m_curveDragComponent;
		const float eps = gridStep() * 0.25f;
		aeKeysOfMut(track, m_curveDragKey.channel, [&](auto& keys)
		{
			using KeyT = typename std::remove_reference_t<decltype(keys)>::value_type;
			int32 idx = -1;
			for (int32 k = 0; k < (int32)keys.size(); ++k)
				if (keys[k].time == m_curveDragKey.time)
					idx = k;
			if (idx < 0)
				return;
			if constexpr (std::is_same_v<KeyT, RotationKey>)
			{
				// Back from the drawn Euler branch: only the dragged component changes.
				const int32 sampleIdx = (int32)glm::round((keys[idx].time - tStart) / step);
				glm::vec3 near = glm::degrees(glm::eulerAngles(keys[idx].value));
				for (size_t ci = 0; ci < curves.size(); ++ci)
					if (curves[ci].track == m_curveDragKey.track && curves[ci].channel == ClipChannel::Rotation && sampleIdx >= 0 && sampleIdx < (int32)samples[ci].values.size())
						near = samples[ci].values[sampleIdx];
				glm::vec3 e = aeEulerNear(keys[idx].value, near);
				e[c] = newValue;
				keys[idx].value = glm::normalize(glm::quat(glm::radians(e)));
			}
			else
				keys[idx].value[c] = newValue;
			// The time moves too, unless another key already sits on the new frame.
			bool free = true;
			for (int32 k = 0; k < (int32)keys.size(); ++k)
				if (k != idx && glm::abs(keys[k].time - newTime) <= eps)
					free = false;
			if (free && newTime != keys[idx].time)
			{
				selectKey(m_curveDragKey.track, m_curveDragKey.channel, keys[idx].time, false);
				keys[idx].time = newTime;
				m_curveDragKey.time = newTime;
				selectKey(m_curveDragKey.track, m_curveDragKey.channel, newTime, true);
				oc::stable_sort(keys.begin(), keys.end(), [](const KeyT& a, const KeyT& b) { return a.time < b.time; });
				extendTo(newTime);
			}
		});
		shrinkToContent(latestBefore);
		markEdited();
	}
	if (deactivated)
		m_drag = DragMode::None;

	if (hovered && io.MouseWheel != 0.0f)
	{
		if (io.KeyCtrl)
			zoomTime(io.MouseWheel, io.MousePos.x);
		else
		{
			// The value axis zooms about the value under the cursor.
			const float pivot = yToValue(io.MousePos.y);
			const float factor = glm::pow(0.85f, io.MouseWheel);
			m_curveMin = pivot + (m_curveMin - pivot) * factor;
			m_curveMax = pivot + (m_curveMax - pivot) * factor;
		}
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_contextTime = xToTime(io.MousePos.x);
		m_contextRow = -1;
		ImGui::OpenPopup("##ae_tl_ctx");
	}
	if (hovered && m_drag == DragMode::None && !ImGui::IsAnyItemActive())
		ImGui::SetItemTooltip("Drag a key: value and frame   Wheel: value zoom   Ctrl+wheel: time zoom   Middle drag: pan   F: fit");
}
