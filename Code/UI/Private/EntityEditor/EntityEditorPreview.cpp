module UI;

import Core;
import Core.imgui;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import Entity;
import RendererVK;
import Physics;
import :EntityEditor;

static constexpr float c_minDistance = 0.02f;
static constexpr float c_maxPitch = 1.55f;

// IM_COL32 is a macro: it does not cross the module import.
static constexpr ImU32 col32(int r, int g, int b, int a)
{
	return (ImU32(a) << 24) | (ImU32(b) << 16) | (ImU32(g) << 8) | ImU32(r);
}

// ---- Building what the renderer draws, from the document ----

void EntityEditor::needContainer(const oc::string& name)
{
	if (name.empty() || !Globals::assetRegistry.findObjectContainer(name))
		return;
	if (oc::find(m_containerLoadRequests.begin(), m_containerLoadRequests.end(), name) == m_containerLoadRequests.end())
		m_containerLoadRequests.push_back(name);
}

void EntityEditor::appendPreviewRender(const RenderComponent::SpawnInfo& render, const Transform& world, const glm::vec3& tint, uint64 pickId, bool highlighted)
{
	// Lookup only, this is the worker: a miss is imported on main and shows a frame later.
	ObjectContainer* container = Globals::world.findLoadedContainer(render.containerName);
	if (!container)
	{
		needContainer(render.containerName);
		return;
	}
	NodeSpawnIdx nodeIdx = NodeSpawnIdx_ROOT;
	if (!render.nodePath.empty() && render.nodePath != "ROOT")
		if (const NodeSpawnIdx found = container->getSpawnIdxForPath(render.nodePath); found != NodeSpawnIdx_INVALID)
			nodeIdx = found;

	const glm::vec3 meshTint = render.color.x >= 0.0f ? render.color : tint;
	const Sphere bounds = container->appendPreviewMeshes(nodeIdx, render.skinned, composeTransform(world, render.localTransform), meshTint, highlighted, m_previewFrame.meshes);
	m_previewPicks.push_back({ pickId, bounds.pos, bounds.radius });
}

void EntityEditor::appendPreviewNode(const EntityDocNode& node, const Transform& parentWorld, bool isRoot, const glm::vec3& parentTint, uint64 pickId, bool highlighted)
{
	if (!node.enabled)
		return;
	const Transform world = isRoot ? node.transform : composeTransform(parentWorld, node.transform);
	m_previewWorlds.push_back({ node.id, world });

	// Everything inside a prefab reference picks as the reference: its parts are not editable here.
	const uint64 nodePickId = pickId != 0 ? pickId : (node.isReference() ? node.id : 0);
	const uint64 ownPickId = nodePickId != 0 ? nodePickId : node.id;
	const bool nodeHighlighted = highlighted || (m_highlightSelected && !isRoot && node.id == m_selectedId);
	const glm::vec3 tint = (node.hasGameUnit && node.gameUnit.color.x >= 0.0f) ? node.gameUnit.color : parentTint;

	if (node.hasRender)
		appendPreviewRender(node.render, world, tint, ownPickId, nodeHighlighted);
	if (node.humanoid && node.humanoid->rig)
	{
		// The rest pose of the bone model, parent before child like the rig's own placement.
		const auto& bones = node.humanoid->rig->bones;
		oc::small_vector<Transform, 16> boneWorlds;
		boneWorlds.resize(bones.size());
		for (size_t i = 0; i < bones.size(); ++i)
		{
			const BoneModelRig::Bone& bone = bones[i];
			boneWorlds[i] = composeTransform(bone.parent >= 0 ? boneWorlds[size_t(bone.parent)] : world, bone.bind);
			if (bone.render)
				appendPreviewRender(*bone.render, boneWorlds[i], tint, ownPickId, nodeHighlighted);
		}
	}
	for (const EntityDocNode& child : node.children)
		appendPreviewNode(child, world, false, tint, nodePickId, nodeHighlighted);
}

void EntityEditor::buildPreviewScene()
{
	m_previewFrame.meshes.clear();
	m_previewPicks.clear();
	m_previewWorlds.clear();
	appendPreviewNode(*m_root, Transform(), true, glm::vec3(-1.0f), 0, false);

	glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
	for (const PreviewPick& pick : m_previewPicks)
	{
		boundsMin = glm::min(boundsMin, pick.center - glm::vec3(pick.radius));
		boundsMax = glm::max(boundsMax, pick.center + glm::vec3(pick.radius));
	}
	if (!m_previewPicks.empty())
	{
		m_previewBoundsCenter = (boundsMin + boundsMax) * 0.5f;
		m_previewBoundsRadius = glm::length(boundsMax - boundsMin) * 0.5f / glm::sqrt(3.0f); // a sphere inside the box corners reads better than one around them
	}
	else
	{
		m_previewBoundsCenter = m_root->transform.pos;
		m_previewBoundsRadius = 0.0f;
	}
}

void EntityEditor::frameCamera()
{
	const float radius = m_previewBoundsRadius > 0.0f ? m_previewBoundsRadius : 1.0f;
	m_cameraTarget = m_previewBoundsCenter;
	m_cameraDistance = glm::max(radius / glm::sin(c_cameraFovY * 0.5f) * 1.15f, c_minDistance);
}

// ---- Screen-space gizmos, painted over the image ----

namespace
{
struct GizmoPainter
{
	ImDrawList* list;
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

	void line(const glm::vec3& a, const glm::vec3& b, ImU32 color, float thickness = 1.5f) const
	{
		ImVec2 pa, pb;
		if (project(a, pa) && project(b, pb))
			list->AddLine(pa, pb, color, thickness);
	}

	void circle(const glm::vec3& center, const glm::vec3& axisA, const glm::vec3& axisB, float radius, ImU32 color, float arcFrom = 0.0f, float arcTo = 6.2831853f) const
	{
		constexpr int segments = 32;
		glm::vec3 prev = center + (axisA * glm::cos(arcFrom) + axisB * glm::sin(arcFrom)) * radius;
		for (int i = 1; i <= segments; ++i)
		{
			const float angle = arcFrom + (arcTo - arcFrom) * float(i) / float(segments);
			const glm::vec3 next = center + (axisA * glm::cos(angle) + axisB * glm::sin(angle)) * radius;
			line(prev, next, color);
			prev = next;
		}
	}

	void sphere(const glm::vec3& center, float radius, const glm::quat& rot, ImU32 color) const
	{
		const glm::vec3 x = rot * glm::vec3(1, 0, 0), y = rot * glm::vec3(0, 1, 0), z = rot * glm::vec3(0, 0, 1);
		circle(center, x, y, radius, color);
		circle(center, y, z, radius, color);
		circle(center, z, x, radius, color);
	}

	void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rot, ImU32 color) const
	{
		const glm::vec3 x = rot * glm::vec3(halfExtents.x, 0, 0), y = rot * glm::vec3(0, halfExtents.y, 0), z = rot * glm::vec3(0, 0, halfExtents.z);
		const glm::vec3 c[8] = {
			center - x - y - z, center + x - y - z, center + x + y - z, center - x + y - z,
			center - x - y + z, center + x - y + z, center + x + y + z, center - x + y + z };
		for (int i = 0; i < 4; ++i)
		{
			line(c[i], c[(i + 1) % 4], color);
			line(c[i + 4], c[(i + 1) % 4 + 4], color);
			line(c[i], c[i + 4], color);
		}
	}

	void capsule(const glm::vec3& center, float radius, float halfHeight, const glm::quat& rot, ImU32 color) const
	{
		const glm::vec3 x = rot * glm::vec3(1, 0, 0), y = rot * glm::vec3(0, 1, 0), z = rot * glm::vec3(0, 0, 1);
		const glm::vec3 top = center + y * halfHeight, bottom = center - y * halfHeight;
		circle(top, x, z, radius, color);
		circle(bottom, x, z, radius, color);
		circle(top, x, y, radius, color, 0.0f, 3.14159265f);
		circle(top, z, y, radius, color, 0.0f, 3.14159265f);
		circle(bottom, x, y, radius, color, 3.14159265f, 6.2831853f);
		circle(bottom, z, y, radius, color, 3.14159265f, 6.2831853f);
		line(top + x * radius, bottom + x * radius, color);
		line(top - x * radius, bottom - x * radius, color);
		line(top + z * radius, bottom + z * radius, color);
		line(top - z * radius, bottom - z * radius, color);
	}
};
}

void EntityEditor::drawNodeGizmos(const EntityDocNode& node, const Transform& world, const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageMax)
{
	const GizmoPainter paint{ ImGui::GetWindowDrawList(), viewProj, imageMin, imageMax - imageMin };

	if (m_showPhysics && node.hasPhysics)
	{
		const PhysicsShape& shape = node.physics.shape;
		const ImU32 color = shape.isSensor ? col32(120, 220, 255, 220) : col32(110, 240, 130, 220);
		const glm::vec3 center = world.transformPoint(shape.offset);
		switch (shape.type)
		{
		case EPhysicsShapeType::Box:     paint.box(center, shape.halfExtents * world.scale, world.quat, color); break;
		case EPhysicsShapeType::Sphere:  paint.sphere(center, shape.radius * world.scale, world.quat, color); break;
		case EPhysicsShapeType::Capsule: paint.capsule(center, shape.radius * world.scale, shape.halfHeight * world.scale, world.quat, color); break;
		default: break; // Hull and Mesh are the render mesh itself
		}
	}
	if (m_showLights && node.hasLight)
	{
		for (const LightComponent::LightDesc& light : node.light.lights)
		{
			if (!light.enabled)
				continue;
			const glm::vec3 color = glm::clamp(light.color, 0.0f, 1.0f);
			const ImU32 col = col32(int(color.x * 255.0f), int(color.y * 255.0f), int(color.z * 255.0f), 230);
			const glm::vec3 center = world.transformPoint(light.offset);
			const float range = light.range * world.scale;
			if (light.type == ELightType::Point)
			{
				paint.sphere(center, range, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), col);
				continue;
			}
			const glm::vec3 direction = glm::normalize(world.quat * light.direction);
			const glm::vec3 helper = glm::abs(direction.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
			const glm::vec3 a = glm::normalize(glm::cross(direction, helper));
			const glm::vec3 b = glm::cross(direction, a);
			if (light.type == ELightType::Spot)
			{
				const float rimRadius = range * glm::tan(glm::radians(light.coneAngle));
				const glm::vec3 rim = center + direction * range;
				paint.circle(rim, a, b, rimRadius, col);
				paint.line(center, rim + a * rimRadius, col);
				paint.line(center, rim - a * rimRadius, col);
				paint.line(center, rim + b * rimRadius, col);
				paint.line(center, rim - b * rimRadius, col);
			}
			else
			{
				paint.line(center, center + direction * range, col);
				paint.circle(center, a, b, light.width * world.scale, col);
			}
		}
	}
	if (m_showForce && node.hasForce)
	{
		const ImU32 col = col32(255, 170, 70, 220);
		const float reach = node.force.reach * world.scale;
		const glm::vec3 direction = glm::length(node.force.direction) > 1e-5f ? glm::normalize(world.quat * node.force.direction) : glm::vec3(0, 0, -1);
		const glm::vec3 anchor = world.transformPoint(node.force.offset);
		// Centered: the anchor is the bubble's centre. Otherwise the span starts at the anchor.
		const glm::vec3 center = node.force.centered ? anchor : anchor + direction * reach * 0.5f;
		paint.sphere(center, reach * 0.5f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), col);
		paint.line(center - direction * reach * 0.5f, center + direction * reach * 0.5f, col);
	}
}

void EntityEditor::drawPreviewOverlays(const glm::mat4& viewProj, const glm::vec2& imageMin, const glm::vec2& imageMax)
{
	ImDrawList* list = ImGui::GetWindowDrawList();
	list->PushClipRect(ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), true);
	const GizmoPainter paint{ list, viewProj, imageMin, imageMax - imageMin };

	for (const PreviewWorld& entry : m_previewWorlds)
	{
		if (!m_overlaysAllNodes && entry.nodeId != m_selectedId)
			continue;
		if (const EntityDocNode* node = findDocNode(*m_root, entry.nodeId))
			drawNodeGizmos(*node, entry.world, viewProj, imageMin, imageMax);
		if (m_showAxes && entry.nodeId == m_selectedId)
		{
			const float size = glm::max(m_cameraDistance * 0.12f, 1e-3f);
			paint.line(entry.world.pos, entry.world.pos + entry.world.quat * glm::vec3(size, 0, 0), col32(235, 70, 60, 255), 2.0f);
			paint.line(entry.world.pos, entry.world.pos + entry.world.quat * glm::vec3(0, size, 0), col32(90, 220, 80, 255), 2.0f);
			paint.line(entry.world.pos, entry.world.pos + entry.world.quat * glm::vec3(0, 0, size), col32(80, 130, 255, 255), 2.0f);
		}
	}
	if (m_showBounds)
		for (const PreviewPick& pick : m_previewPicks)
			paint.sphere(pick.center, pick.radius, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
				pick.nodeId == m_selectedId ? col32(255, 210, 90, 220) : col32(200, 200, 200, 110));

	list->PopClipRect();
}

// ---- The view ----

void EntityEditor::renderPreviewToolbar(const glm::vec2& imageMin, uint32 meshCount)
{
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 6.0f, imageMin.y + 6.0f));
	if (ImGui::SmallButton("Frame"))
		frameCamera();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("F");
	ImGui::SameLine();
	if (ImGui::SmallButton("View"))
		ImGui::OpenPopup("##ee_preview_view");
	if (ImGui::BeginPopup("##ee_preview_view"))
	{
		ImGui::Checkbox("Grid", &m_showGrid);
		ImGui::SetNextItemWidth(90.0f);
		ImGui::DragFloat("Grid height", &m_gridY, 0.01f);
		ImGui::Checkbox("Wireframe", &m_showWireframe);
		ImGui::Checkbox("Highlight selected", &m_highlightSelected);
		ImGui::SeparatorText("Overlays");
		ImGui::Checkbox("Axes", &m_showAxes);
		ImGui::Checkbox("Bounds", &m_showBounds);
		ImGui::Checkbox("Physics shapes", &m_showPhysics);
		ImGui::Checkbox("Lights", &m_showLights);
		ImGui::Checkbox("Force bubbles", &m_showForce);
		ImGui::Checkbox("Of every node, not only the selected", &m_overlaysAllNodes);
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%u meshes", meshCount);
}

void EntityEditor::renderPreview()
{
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (avail.x < 8.0f || avail.y < 8.0f)
		return;
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const glm::vec2 imageMin(glm::floor(cursor.x), glm::floor(cursor.y));
	const glm::vec2 imageSize(glm::floor(avail.x), glm::floor(avail.y));
	const glm::vec2 imageMax = imageMin + imageSize;

	buildPreviewScene();
	if (m_framePending && (m_previewBoundsRadius > 0.0f || m_containerLoadRequests.empty()))
	{
		frameCamera();
		m_framePending = false;
	}

	// ---- The camera input ----
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x, imageMin.y));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##ee_view", ImVec2(imageSize.x, imageSize.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsItemActivated())
		m_previewDragged = false;
	if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 3.0f)))
		m_previewDragged = true;
	if (active && m_previewDragged)
	{
		const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left));
		if (pan)
		{
			const glm::vec3 forward(glm::cos(m_cameraPitch) * glm::sin(m_cameraYaw), glm::sin(m_cameraPitch), glm::cos(m_cameraPitch) * glm::cos(m_cameraYaw));
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

	// ---- The view-projection: reversed-Z, y down in clip space, for a positive-height viewport ----
	const glm::vec3 toCamera(glm::cos(m_cameraPitch) * glm::sin(m_cameraYaw), glm::sin(m_cameraPitch), glm::cos(m_cameraPitch) * glm::cos(m_cameraYaw));
	const glm::vec3 cameraPos = m_cameraTarget + toCamera * m_cameraDistance;
	const float nearPlane = glm::max(m_cameraDistance * 0.01f, 0.005f);
	const float farPlane = glm::max(m_cameraDistance + m_previewBoundsRadius * 4.0f, m_cameraDistance * 200.0f);
	const glm::mat4 view = glm::lookAtRH(cameraPos, m_cameraTarget, glm::vec3(0, 1, 0));
	glm::mat4 proj = glm::perspectiveRH_ZO(c_cameraFovY, imageSize.x / imageSize.y, farPlane, nearPlane);
	proj[1][1] *= -1.0f;
	const glm::mat4 viewProj = proj * view;

	// A click (a press that did not drag): the nearest bounds under the cursor select that node.
	if (ImGui::IsItemDeactivated() && !m_previewDragged && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
		const glm::mat4 invViewProj = glm::inverse(viewProj);
		const glm::vec2 ndc = (mouse - imageMin) / imageSize * 2.0f - 1.0f;
		const glm::vec4 farPoint = invViewProj * glm::vec4(ndc, 0.0f, 1.0f); // reversed-Z: z 0 is the far plane
		const glm::vec3 rayDir = glm::normalize(glm::vec3(farPoint) / farPoint.w - cameraPos);
		float bestRadius = FLT_MAX;
		uint64 bestId = m_root->id;
		for (const PreviewPick& pick : m_previewPicks)
		{
			const glm::vec3 toCenter = pick.center - cameraPos;
			const float along = glm::dot(toCenter, rayDir);
			const float distance2 = glm::dot(toCenter, toCenter) - along * along;
			// The SMALLEST sphere under the cursor: a part in front of its enclosing whole, whichever is closer.
			if (along > 0.0f && distance2 <= pick.radius * pick.radius && pick.radius < bestRadius)
			{
				bestRadius = pick.radius;
				bestId = pick.nodeId;
			}
		}
		m_selectedId = bestId;
	}

	// ---- Submit, and show last frame's image ----
	const float pixelScale = glm::min(2.0f, float(Renderer::ENTITY_PREVIEW_TARGET_SIZE) / glm::max(imageSize.x, imageSize.y)); // 2:1 + bilinear = the antialiasing
	const glm::uvec2 pixelSize(glm::max(imageSize * pixelScale, glm::vec2(1.0f)));
	m_previewFrame.viewProj = viewProj;
	m_previewFrame.cameraPos = cameraPos;
	m_previewFrame.size = pixelSize;
	m_previewFrame.gridY = m_gridY;
	m_previewFrame.grid = m_showGrid;
	m_previewFrame.wireframe = m_showWireframe;
	const uint32 meshCount = uint32(m_previewFrame.meshes.size());
	Globals::rendererVK.submitEntityPreview(m_previewFrame);

	ImDrawList* list = ImGui::GetWindowDrawList();
	const uint64 texture = Globals::rendererVK.getEntityPreviewTexture();
	if (texture != 0)
	{
		const glm::vec2 uvMax = glm::vec2(pixelSize) / float(Renderer::ENTITY_PREVIEW_TARGET_SIZE);
		list->AddImage(ImTextureRef((ImTextureID)texture), ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), ImVec2(0.0f, 0.0f), ImVec2(uvMax.x, uvMax.y));
	}
	else
		list->AddRectFilled(ImVec2(imageMin.x, imageMin.y), ImVec2(imageMax.x, imageMax.y), col32(22, 23, 26, 255));

	drawPreviewOverlays(viewProj, imageMin, imageMax);

	renderPreviewToolbar(imageMin, meshCount);

	if (meshCount == 0 && m_containerLoadRequests.empty())
	{
		const char* text = "Nothing to draw: add a Render component";
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		list->AddText(ImVec2((imageMin.x + imageMax.x - textSize.x) * 0.5f, (imageMin.y + imageMax.y - textSize.y) * 0.5f), col32(170, 170, 170, 255), text);
	}
	if (hovered && !active && !ImGui::IsAnyItemActive())
	{
		const char* hint = "Drag: orbit   Shift+drag or middle: pan   Wheel: zoom   Click: select   F: frame";
		list->AddText(ImVec2(imageMin.x + 8.0f, imageMax.y - ImGui::GetTextLineHeight() - 6.0f), col32(200, 200, 200, 160), hint);
	}

	ImGui::SetCursorScreenPos(ImVec2(imageMin.x, imageMin.y));
	ImGui::Dummy(ImVec2(imageSize.x, imageSize.y));
}
