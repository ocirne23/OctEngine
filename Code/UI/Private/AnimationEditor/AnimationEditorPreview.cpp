module UI;

import Core;
import Core.imgui;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import Animation;
import Entity;
import RendererVK;
import :AnimationEditor;

static constexpr float c_minDistance = 0.02f;
static constexpr float c_maxPitch = 1.55f;
static constexpr float c_cameraFovY = 0.7f;

// IM_COL32 is a macro: it does not cross the module import.
static constexpr ImU32 apCol(int r, int g, int b, int a = 255)
{
	return (ImU32(a) << 24) | (ImU32(b) << 16) | (ImU32(g) << 8) | ImU32(r);
}

static constexpr ImU32 c_axisColors[3] = { apCol(235, 80, 70), apCol(110, 220, 80), apCol(80, 140, 255) };

namespace
{
struct PreviewView
{
	glm::mat4 viewProj;
	glm::vec2 imageMin, imageSize;

	bool project(const glm::vec3& world, ImVec2& out) const
	{
		const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
		if (clip.w <= 1e-4f)
			return false;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		const glm::vec2 screen = imageMin + (ndc * 0.5f + 0.5f) * imageSize;
		out = ImVec2(screen.x, screen.y);
		return true;
	}

	glm::vec3 rayDirection(const ImVec2& mouse, const glm::vec3& cameraPos) const
	{
		const glm::vec2 ndc = (glm::vec2(mouse.x, mouse.y) - imageMin) / imageSize * 2.0f - 1.0f;
		const glm::vec4 farPoint = glm::inverse(viewProj) * glm::vec4(ndc, 0.0f, 1.0f); // reversed-Z: z 0 is the far plane
		return glm::normalize(glm::vec3(farPoint) / farPoint.w - cameraPos);
	}
};

float segmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
	const glm::vec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
	const float len2 = glm::dot(ab, ab);
	const float t = len2 > 1e-6f ? glm::clamp(glm::dot(ap, ab) / len2, 0.0f, 1.0f) : 0.0f;
	return glm::length(ap - ab * t);
}

bool rayPlane(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
{
	const float denom = glm::dot(dir, normal);
	if (glm::abs(denom) < 0.02f)
		return false;
	const float t = glm::dot(point - origin, normal) / denom;
	if (t <= 0.0f)
		return false;
	hit = origin + dir * t;
	return true;
}

// The parameter s of the point on the line point + s * axis that is closest to the ray (both directions unit length).
bool closestOnAxis(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& point, const glm::vec3& axis, float& s)
{
	const glm::vec3 w0 = point - rayOrigin;
	const float b = glm::dot(axis, rayDir);
	const float denom = 1.0f - b * b;
	if (denom < 1e-4f)
		return false;
	s = (b * glm::dot(rayDir, w0) - glm::dot(axis, w0)) / denom;
	return true;
}

glm::quat rotationOf(const glm::mat4& m)
{
	glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
	const float l0 = glm::length(c0), l1 = glm::length(c1), l2 = glm::length(c2);
	if (l0 < 1e-8f || l1 < 1e-8f || l2 < 1e-8f)
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	return glm::normalize(glm::quat_cast(glm::mat3(c0 / l0, c1 / l1, c2 / l2)));
}
}

void AnimationEditor::frameCamera()
{
	m_cameraTarget = m_boundsCenter;
	m_cameraDistance = glm::max(glm::max(m_boundsRadius, 0.1f) / glm::sin(c_cameraFovY * 0.5f) * 1.1f, c_minDistance);
}

// ---- The skeleton overlay ----

void AnimationEditor::drawSkeleton(const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageSize)
{
	if (!m_skeleton || !m_showBones)
		return;
	const PreviewView view{ viewProj, imageMin, imageSize };
	ImDrawList* list = ImGui::GetWindowDrawList();
	const oc::span<const glm::mat4> globals = m_player.getGlobalTransforms();
	const Skeleton& skel = *m_skeleton;
	const uint32 n = glm::min(skel.numBones(), (uint32)globals.size());
	for (uint32 i = 0; i < n; ++i)
	{
		const int32 parent = skel.parentIndices[i];
		if (parent < 0 || skel.parentIndices[parent] < 0)
			continue; // the link up to the scene root is no bone
		ImVec2 a, b;
		if (!view.project(glm::vec3(globals[parent][3]), a) || !view.project(glm::vec3(globals[i][3]), b))
			continue;
		const bool selected = (int32)i == m_selectedBone || parent == m_selectedBone;
		const bool animated = i < m_boneTrack.size() && m_boneTrack[i] >= 0;
		const ImU32 color = selected ? apCol(255, 180, 70, 230) : animated ? apCol(120, 210, 255, 190) : apCol(200, 200, 205, 120);
		list->AddLine(a, b, apCol(0, 0, 0, 110), 3.5f);
		list->AddLine(a, b, color, 1.5f);
	}
	for (uint32 i = 0; i < n; ++i)
	{
		if (i < m_boneIsHelper.size() && m_boneIsHelper[i])
			continue;
		ImVec2 p;
		if (!view.project(glm::vec3(globals[i][3]), p))
			continue;
		const bool selected = (int32)i == m_selectedBone;
		const bool overridden = m_overrides.count((int32)i) != 0;
		const bool animated = i < m_boneTrack.size() && m_boneTrack[i] >= 0;
		const ImU32 color = selected ? apCol(255, 190, 80) : overridden ? apCol(255, 150, 60) : animated ? apCol(120, 210, 255) : apCol(210, 210, 215, 170);
		list->AddCircleFilled(p, selected ? 5.0f : 3.0f, apCol(0, 0, 0, 140));
		list->AddCircleFilled(p, selected ? 4.0f : 2.2f, color);
	}
	if (m_selectedBone >= 0 && (uint32)m_selectedBone < n)
	{
		ImVec2 p;
		if (view.project(glm::vec3(globals[m_selectedBone][3]), p))
			list->AddText(ImVec2(p.x + 8.0f, p.y - 18.0f), apCol(255, 220, 150), skel.boneNames[m_selectedBone].c_str());
	}
}

// ---- The gizmo: rotate rings / move arrows on the selected bone ----

bool AnimationEditor::updateGizmo(const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec2& imageMin, const glm::vec2& imageSize, bool pressed, bool held)
{
	if (!m_skeleton || m_selectedBone < 0 || (uint32)m_selectedBone >= m_player.getGlobalTransforms().size())
	{
		m_gizmoActive = -1;
		m_gizmoHover = -1;
		return false;
	}
	const PreviewView view{ viewProj, imageMin, imageSize };
	ImDrawList* list = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const int32 bone = m_selectedBone;
	const oc::span<const glm::mat4> globals = m_player.getGlobalTransforms();
	const glm::mat4& global = globals[bone];
	const int32 parent = m_skeleton->parentIndices[bone];
	const glm::mat4 parentGlobal = parent >= 0 ? globals[parent] : glm::mat4(1.0f);
	const glm::vec3 origin(global[3]);
	const glm::quat axesRot = m_gizmoLocal ? rotationOf(global) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	const glm::vec3 axes[3] = { axesRot * glm::vec3(1, 0, 0), axesRot * glm::vec3(0, 1, 0), axesRot * glm::vec3(0, 0, 1) };
	const float size = glm::max(glm::length(origin - cameraPos) * 0.13f, 1e-3f);
	const glm::vec3 rayDir = view.rayDirection(io.MousePos, cameraPos);

	// ---- Hover ----
	if (m_gizmoActive < 0)
	{
		m_gizmoHover = -1;
		float best = 7.0f;
		for (int32 a = 0; a < 3; ++a)
		{
			if (m_gizmoMode == GizmoMode::Rotate)
			{
				const glm::vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
				ImVec2 prev;
				bool prevOk = false;
				for (int k = 0; k <= 48; ++k)
				{
					const float angle = 6.2831853f * float(k) / 48.0f;
					ImVec2 p;
					const bool ok = view.project(origin + (u * glm::cos(angle) + v * glm::sin(angle)) * size, p);
					if (ok && prevOk)
					{
						const float d = segmentDistance(io.MousePos, prev, p);
						if (d < best) { best = d; m_gizmoHover = a; }
					}
					prev = p;
					prevOk = ok;
				}
			}
			else
			{
				ImVec2 p0, p1;
				if (view.project(origin, p0) && view.project(origin + axes[a] * size * 1.2f, p1))
				{
					const float d = segmentDistance(io.MousePos, p0, p1);
					if (d < best) { best = d; m_gizmoHover = a; }
				}
			}
		}
	}

	// ---- Press: the start state ----
	if (pressed && m_gizmoHover >= 0)
	{
		m_gizmoActive = m_gizmoHover;
		m_gizmoAxisWorld = axes[m_gizmoActive];
		m_gizmoStartOrigin = origin;
		m_gizmoStartMouse = io.MousePos;
		currentLocalPose(bone, m_gizmoStartPos, m_gizmoStartRot, m_gizmoStartScale);
		glm::vec3 hit;
		m_gizmoStartVector = rayPlane(cameraPos, rayDir, origin, m_gizmoAxisWorld, hit) && glm::length(hit - origin) > 1e-6f
			? glm::normalize(hit - origin) : glm::vec3(0.0f);
		float s = 0.0f;
		m_gizmoStartAxisParam = closestOnAxis(cameraPos, rayDir, origin, m_gizmoAxisWorld, s) ? s : 0.0f;
		m_playing = false;
	}

	// ---- Drag ----
	if (m_gizmoActive >= 0 && held)
	{
		const glm::vec3 axis = m_gizmoAxisWorld;
		if (m_gizmoMode == GizmoMode::Rotate)
		{
			float angle = 0.0f;
			glm::vec3 hit;
			if (glm::length(m_gizmoStartVector) > 0.5f && rayPlane(cameraPos, rayDir, m_gizmoStartOrigin, axis, hit) && glm::length(hit - m_gizmoStartOrigin) > 1e-6f)
			{
				const glm::vec3 v1 = glm::normalize(hit - m_gizmoStartOrigin);
				angle = glm::atan(glm::dot(glm::cross(m_gizmoStartVector, v1), axis), glm::dot(m_gizmoStartVector, v1));
			}
			else
				angle = (io.MousePos.x - m_gizmoStartMouse.x) * 0.01f; // edge-on ring: the mouse's sideways travel
			if (io.KeyCtrl)
				angle = glm::radians(glm::round(glm::degrees(angle) / 15.0f) * 15.0f);
			// A turn about a WORLD axis through the bone, in the parent's frame: local' = parent^-1 * q * parent * local.
			const glm::quat parentRot = rotationOf(parentGlobal);
			const glm::quat worldTurn = glm::angleAxis(angle, axis);
			const glm::quat local = glm::normalize(glm::inverse(parentRot) * worldTurn * parentRot * m_gizmoStartRot);
			editBonePose(bone, ClipChannel::Rotation, m_gizmoStartPos, local, m_gizmoStartScale);
		}
		else
		{
			float s = 0.0f;
			if (closestOnAxis(cameraPos, rayDir, m_gizmoStartOrigin, axis, s))
			{
				float delta = s - m_gizmoStartAxisParam;
				if (io.KeyCtrl)
					delta = glm::round(delta / 0.05f) * 0.05f;
				const glm::vec3 worldOrigin = m_gizmoStartOrigin + axis * delta;
				const glm::vec3 local = glm::vec3(glm::inverse(parentGlobal) * glm::vec4(worldOrigin, 1.0f));
				editBonePose(bone, ClipChannel::Position, local, m_gizmoStartRot, m_gizmoStartScale);
			}
		}
	}
	else if (!held)
		m_gizmoActive = -1;

	// ---- Draw ----
	const int32 lit = m_gizmoActive >= 0 ? m_gizmoActive : m_gizmoHover;
	for (int32 a = 0; a < 3; ++a)
	{
		const ImU32 color = a == lit ? apCol(255, 230, 90) : c_axisColors[a];
		const float thickness = a == lit ? 3.0f : 2.0f;
		if (m_gizmoMode == GizmoMode::Rotate)
		{
			const glm::vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
			ImVec2 prev;
			bool prevOk = false;
			for (int k = 0; k <= 48; ++k)
			{
				const float angle = 6.2831853f * float(k) / 48.0f;
				const glm::vec3 p3 = origin + (u * glm::cos(angle) + v * glm::sin(angle)) * size;
				ImVec2 p;
				const bool ok = view.project(p3, p);
				// The far half of a ring dims: it reads as a sphere.
				const bool front = glm::dot(p3 - origin, cameraPos - origin) >= 0.0f;
				if (ok && prevOk)
					list->AddLine(prev, p, front || a == lit ? color : (color & 0x00FFFFFFu) | 0x50000000u, thickness);
				prev = p;
				prevOk = ok;
			}
		}
		else
		{
			ImVec2 p0, p1;
			if (view.project(origin, p0) && view.project(origin + axes[a] * size * 1.2f, p1))
			{
				list->AddLine(p0, p1, color, thickness + 0.5f);
				const glm::vec2 d = glm::normalize(glm::vec2(p1.x - p0.x, p1.y - p0.y) + glm::vec2(1e-6f));
				const glm::vec2 n(-d.y, d.x);
				const ImVec2 tri[3] = { ImVec2(p1.x + d.x * 10.0f, p1.y + d.y * 10.0f), ImVec2(p1.x + n.x * 5.0f, p1.y + n.y * 5.0f), ImVec2(p1.x - n.x * 5.0f, p1.y - n.y * 5.0f) };
				list->AddConvexPolyFilled(tri, 3, color);
			}
		}
	}
	if (m_gizmoActive >= 0 && m_gizmoMode == GizmoMode::Rotate)
	{
		glm::vec3 p, s;
		glm::quat r;
		currentLocalPose(bone, p, r, s);
		const float degrees = glm::degrees(2.0f * glm::acos(glm::min(glm::abs(glm::dot(r, m_gizmoStartRot)), 1.0f)));
		ImVec2 screen;
		if (view.project(origin, screen))
		{
			char text[32];
			snprintf(text, sizeof(text), "%.1f deg", degrees);
			list->AddText(ImVec2(screen.x + 12.0f, screen.y + 8.0f), apCol(255, 230, 150), text);
		}
	}
	return m_gizmoActive >= 0 || (pressed && m_gizmoHover >= 0);
}

// ---- The view ----

void AnimationEditor::renderPreviewToolbar(const glm::vec2& imageMin)
{
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 6.0f, imageMin.y + 6.0f));
	if (ImGui::SmallButton("Frame"))
		frameCamera();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("F");
	ImGui::SameLine();
	if (ImGui::SmallButton("View"))
		ImGui::OpenPopup("##ae_preview_view");
	if (ImGui::BeginPopup("##ae_preview_view"))
	{
		ImGui::Checkbox("Mesh", &m_showMesh);
		ImGui::Checkbox("Bones", &m_showBones);
		ImGui::Checkbox("Grid", &m_showGrid);
		ImGui::Checkbox("Wireframe", &m_showWireframe);
		ImGui::EndPopup();
	}
	ImGui::SameLine(0.0f, 12.0f);
	if (ImGui::RadioButton("Rotate (E)", m_gizmoMode == GizmoMode::Rotate))
		m_gizmoMode = GizmoMode::Rotate;
	ImGui::SameLine();
	if (ImGui::RadioButton("Move (W)", m_gizmoMode == GizmoMode::Move))
		m_gizmoMode = GizmoMode::Move;
	ImGui::SameLine(0.0f, 12.0f);
	if (ImGui::SmallButton(m_gizmoLocal ? "Local" : "World"))
		m_gizmoLocal = !m_gizmoLocal;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The gizmo's axes: the bone's own, or the world's");
}

void AnimationEditor::renderPreview(float width, float height)
{
	(void)width;
	(void)height;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (avail.x < 8.0f || avail.y < 8.0f)
		return;
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const glm::vec2 imageMin(glm::floor(cursor.x), glm::floor(cursor.y));
	const glm::vec2 imageSize(glm::floor(avail.x), glm::floor(avail.y));
	const glm::vec2 imageMax = imageMin + imageSize;
	ImDrawList* list = ImGui::GetWindowDrawList();

	// ---- The pose at the playhead, unkeyed edits on top ----
	const bool posed = m_skeleton && m_builtSkeleton == m_skeleton;
	if (posed)
	{
		m_player.clearBoneModifiers();
		for (const auto& [bone, o] : m_overrides)
		{
			glm::vec3 p, s;
			glm::quat r;
			m_player.getSampledLocalPose((uint32)bone, p, r, s); // last frame's sample: close enough for the channels not edited
			if (o.channels & (1u << (uint32)ClipChannel::Position)) p = o.pos;
			if (o.channels & (1u << (uint32)ClipChannel::Rotation)) r = o.rot;
			if (o.channels & (1u << (uint32)ClipChannel::Scale))    s = o.scale;
			m_player.setBoneTransform((uint32)bone, glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s));
		}
		m_player.setTime(m_time);
	}

	// ---- Bounds for the framing: the posed joints ----
	if (posed)
	{
		const oc::span<const glm::mat4> globals = m_player.getGlobalTransforms();
		glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
		for (uint32 i = 0; i < (uint32)globals.size(); ++i)
			if (i >= m_boneIsHelper.size() || !m_boneIsHelper[i])
			{
				lo = glm::min(lo, glm::vec3(globals[i][3]));
				hi = glm::max(hi, glm::vec3(globals[i][3]));
			}
		if (lo.x <= hi.x)
		{
			m_boundsCenter = (lo + hi) * 0.5f;
			m_boundsRadius = glm::max(glm::length(hi - lo) * 0.6f, 0.05f);
		}
		if (m_framePending)
		{
			frameCamera();
			m_framePending = false;
		}
	}

	// ---- The camera input. The gizmo has the left button first ----
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x, imageMin.y));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##ae_view", ImVec2(imageSize.x, imageSize.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const bool activated = ImGui::IsItemActivated();
	const bool deactivated = ImGui::IsItemDeactivated();
	const ImGuiIO& io = ImGui::GetIO();

	const glm::vec3 toCamera(glm::cos(m_cameraPitch) * glm::sin(m_cameraYaw), glm::sin(m_cameraPitch), glm::cos(m_cameraPitch) * glm::cos(m_cameraYaw));
	const glm::vec3 cameraPos = m_cameraTarget + toCamera * m_cameraDistance;
	const float nearPlane = glm::max(m_cameraDistance * 0.01f, 0.005f);
	const float farPlane = glm::max(m_cameraDistance + m_boundsRadius * 4.0f, m_cameraDistance * 200.0f);
	const glm::mat4 view = glm::lookAtRH(cameraPos, m_cameraTarget, glm::vec3(0, 1, 0));
	glm::mat4 proj = glm::perspectiveRH_ZO(c_cameraFovY, imageSize.x / imageSize.y, farPlane, nearPlane);
	proj[1][1] *= -1.0f;
	const glm::mat4 viewProj = proj * view;

	// ---- Submit last frame's scene, show last frame's image ----
	m_previewFrame.target = EPreviewTarget::AnimationEditor;
	m_previewFrame.meshes.clear();
	m_previewFrame.palette.clear();
	if (posed && m_showMesh && m_rigContainer)
	{
		m_rigContainer->appendPreviewMeshes(NodeSpawnIdx_ROOT, true, Transform(), glm::vec3(-1.0f), false, m_previewFrame.meshes);
		const oc::span<const glm::mat4> palette = m_player.getPalette();
		m_previewFrame.palette.assign(palette.begin(), palette.end());
		for (EntityPreviewMesh& mesh : m_previewFrame.meshes)
			if (mesh.skinVertexOffset != UINT32_MAX)
				mesh.paletteOffset = 0;
	}
	const float pixelScale = glm::min(2.0f, float(Renderer::ENTITY_PREVIEW_TARGET_SIZE) / glm::max(imageSize.x, imageSize.y));
	const glm::uvec2 pixelSize(glm::max(imageSize * pixelScale, glm::vec2(1.0f)));
	m_previewFrame.viewProj = viewProj;
	m_previewFrame.cameraPos = cameraPos;
	m_previewFrame.size = pixelSize;
	m_previewFrame.gridY = 0.0f;
	m_previewFrame.grid = m_showGrid;
	m_previewFrame.wireframe = m_showWireframe;
	Globals::rendererVK.submitEntityPreview(m_previewFrame);

	const uint64 texture = Globals::rendererVK.getEntityPreviewTexture(EPreviewTarget::AnimationEditor);
	if (texture != 0)
	{
		const glm::vec2 uvMax = glm::vec2(pixelSize) / float(Renderer::ENTITY_PREVIEW_TARGET_SIZE);
		list->AddImage(ImTextureRef((ImTextureID)texture), ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), ImVec2(0.0f, 0.0f), ImVec2(uvMax.x, uvMax.y));
	}
	else
		list->AddRectFilled(ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), apCol(22, 23, 26));

	list->PushClipRect(ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), true);
	if (posed)
		drawSkeleton(viewProj, imageMin, imageSize);
	const bool gizmoBusy = posed && updateGizmo(viewProj, cameraPos, imageMin, imageSize,
		activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left), active && ImGui::IsMouseDown(ImGuiMouseButton_Left));
	if (m_recording)
		list->AddRect(ImVec2(imageMin.x + 1.0f, imageMin.y + 1.0f), ImVec2(imageMax.x - 1.0f, imageMax.y - 1.0f), apCol(230, 50, 50, 220), 0.0f, 0, 3.0f);
	list->PopClipRect();

	// ---- Orbit / pan / zoom / pick ----
	if (activated)
		m_previewDragged = gizmoBusy;
	if (active && !gizmoBusy && m_gizmoActive < 0
		&& (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 3.0f)))
	{
		m_previewDragged = true;
		const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyShift && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)));
		if (pan)
		{
			const glm::vec3 forward = toCamera;
			const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), forward));
			const glm::vec3 up = glm::cross(forward, right);
			const float unitsPerPixel = 2.0f * m_cameraDistance * glm::tan(c_cameraFovY * 0.5f) / imageSize.y;
			m_cameraTarget += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * unitsPerPixel;
		}
		else
		{
			m_cameraYaw -= io.MouseDelta.x * 0.008f;
			m_cameraPitch = glm::clamp(m_cameraPitch + io.MouseDelta.y * 0.008f, -c_maxPitch, c_maxPitch);
		}
	}
	if (hovered && io.MouseWheel != 0.0f)
		m_cameraDistance = glm::max(m_cameraDistance * glm::pow(0.88f, io.MouseWheel), c_minDistance);
	if (hovered && ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.WantTextInput)
		frameCamera();

	// A click (no drag, no gizmo): the nearest joint under the cursor.
	if (deactivated && !m_previewDragged && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && posed)
	{
		const PreviewView pv{ viewProj, imageMin, imageSize };
		const oc::span<const glm::mat4> globals = m_player.getGlobalTransforms();
		float best = 12.0f;
		int32 picked = -1;
		for (uint32 i = 0; i < (uint32)globals.size(); ++i)
		{
			if (i < m_boneIsHelper.size() && m_boneIsHelper[i])
				continue;
			ImVec2 p;
			if (!pv.project(glm::vec3(globals[i][3]), p))
				continue;
			const float d = glm::length(glm::vec2(p.x - io.MousePos.x, p.y - io.MousePos.y));
			if (d < best)
			{
				best = d;
				picked = (int32)i;
			}
		}
		m_selectedBone = picked;
		m_selectedTrack = -1;
		if (picked >= 0)
		{
			// Show it in the list: its parents' rows are already there, its own channels open.
			m_expandedBones.insert(m_skeleton->boneNames[picked]);
			m_curveRows.clear();
			m_curveFitPending = true;
		}
	}

	renderPreviewToolbar(imageMin);

	if (!posed)
	{
		const char* text = m_rigName.empty() ? "Pick a rig (toolbar) to preview and pose the clip"
			: !m_rigContainer ? "Loading the rig..." : "This rig has no skeleton: pick a skinned one";
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		list->AddText(ImVec2((imageMin.x + imageMax.x - textSize.x) * 0.5f, (imageMin.y + imageMax.y - textSize.y) * 0.5f), apCol(170, 170, 170), text);
	}
	else if (hovered && !active)
	{
		const char* hint = "Click a joint: select   Drag a ring / arrow: pose (Ctrl: snap)   Drag: orbit   Shift / middle: pan   Wheel: zoom   F: frame";
		list->AddText(ImVec2(imageMin.x + 8.0f, imageMax.y - ImGui::GetTextLineHeight() - 6.0f), apCol(200, 200, 200, 160), hint);
	}

	ImGui::SetCursorScreenPos(ImVec2(imageMin.x, imageMin.y));
	ImGui::Dummy(ImVec2(imageSize.x, imageSize.y));
}
