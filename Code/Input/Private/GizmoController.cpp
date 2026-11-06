module Input;

import Core;
import Core.glm;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Core.SDL;
import Core.imgui;
import Settings;
import Entity;
import UI;

import :Input;
import :GizmoController;

namespace
{
    const glm::vec3 kAxis[3] = { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) };

    const glm::vec3 kAxisColor[3] = { glm::vec3(0.93f, 0.27f, 0.30f), glm::vec3(0.45f, 0.80f, 0.22f), glm::vec3(0.25f, 0.52f, 0.96f) };
    const glm::vec3 kHighlightColor = glm::vec3(1.0f, 0.82f, 0.20f);
    const glm::vec3 kViewColor = glm::vec3(0.92f, 0.92f, 0.92f);

    // Handle layout in gizmo arm lengths.
    constexpr float kArmStart = 0.18f;   // the arms start outside the centre handle
    constexpr float kArmHeadBase = 0.8f; // translate arrow head from here to the tip
    constexpr float kPlaneMin = 0.3f;    // translate plane squares span [min, max] on both of their axes
    constexpr float kPlaneMax = 0.55f;
    constexpr float kViewRing = 1.2f;    // rotate view circle radius
    constexpr float kTwoPi = 6.28318531f;

    float centreSize(float thickness) { return thickness * 2.5f; } // translate dot radius, scale box half size

    ImU32 toColor(const glm::vec3& c, float alpha)
    {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, glm::clamp(alpha, 0.0f, 1.0f)));
    }

    ImVec2 im(const glm::vec2& v) { return ImVec2(v.x, v.y); }

    float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

    // Distance from p to the segment ab; outT is the closest point's parameter along it.
    float distanceToSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float* outT = nullptr)
    {
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t = len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        if (outT)
            *outT = t;
        return glm::length(p - (a + ab * t));
    }

    // Positive area in y-down screen space = clockwise on screen: the winding ImGui's anti-aliased fill expects.
    void orderClockwise(glm::vec2* points, int count)
    {
        float area = 0.0f;
        for (int i = 0; i < count; ++i)
            area += cross2(points[i], points[(i + 1) % count]);
        if (area < 0.0f)
            for (int i = 0; i < count / 2; ++i)
                oc::swap(points[i], points[count - 1 - i]);
    }

    bool insideClockwise(const glm::vec2& p, const glm::vec2* points, int count)
    {
        for (int i = 0; i < count; ++i)
            if (cross2(points[(i + 1) % count] - points[i], p - points[i]) < 0.0f)
                return false;
        return true;
    }

    void fillConvex(ImDrawList* list, const glm::vec2* points, int count, ImU32 color)
    {
        glm::vec2 ordered[8];
        ImVec2 out[8];
        for (int i = 0; i < count; ++i)
            ordered[i] = points[i];
        orderClockwise(ordered, count);
        for (int i = 0; i < count; ++i)
            out[i] = im(ordered[i]);
        list->AddConvexPolyFilled(out, count, color);
    }

    void fillTriangle(ImDrawList* list, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, ImU32 color)
    {
        const glm::vec2 points[3] = { a, b, c };
        fillConvex(list, points, 3, color);
    }

    Transform worldTransform(Entity* entity)
    {
        Transform t(entity->pos, entity->scale, entity->rot);
        for (Entity* p = entity->getParent(); p; p = p->getParent())
            t = composeTransform(Transform(p->pos, p->scale, p->rot), t);
        return t;
    }

    glm::vec3 inverseTransformPoint(const Transform& t, const glm::vec3& p)
    {
        glm::vec3 v = (p - t.pos) * (1.0f / t.scale);
        return glm::conjugate(t.quat) * v;
    }

    // Closest points between the ray and the line lo + t * ld (both directions unit length): t along the line, s along
    // the ray. False when they are near-parallel.
    bool closestParams(const Ray& ray, const glm::vec3& lo, const glm::vec3& ld, float& outT, float& outS)
    {
        const glm::vec3 w = ray.origin - lo;
        const float b = glm::dot(ray.dir, ld);
        const float d = glm::dot(ray.dir, w);
        const float e = glm::dot(ld, w);
        const float denom = 1.0f - b * b;
        if (denom < 1e-6f)
            return false;
        outT = (e - b * d) / denom;
        outS = (b * e - d) / denom;
        return true;
    }

    bool rayPlane(const Ray& ray, const glm::vec3& p0, const glm::vec3& n, glm::vec3& outHit)
    {
        const float denom = glm::dot(ray.dir, n);
        if (glm::abs(denom) < 1e-6f)
            return false;
        const float t = glm::dot(p0 - ray.origin, n) / denom;
        if (t < 0.0f)
            return false;
        outHit = ray.origin + ray.dir * t;
        return true;
    }

    bool viewportContains(const Rect& r, glm::vec2 p)
    {
        return p.x >= float(r.min.x) && p.x <= float(r.max.x) && p.y >= float(r.min.y) && p.y <= float(r.max.y);
    }

    // World -> viewport pixels with the same conventions as Camera::screenToRay, plus near-plane clipping for segments.
    struct Projector
    {
        const Camera& camera;
        glm::vec2 vpMin;
        glm::vec2 vpSize;
        float tanHalfFov;
        float aspect;
        float nearZ;

        Projector(const Camera& c, const Rect& r)
            : camera(c)
            , vpMin(float(r.min.x), float(r.min.y))
            , vpSize(glm::vec2(r.getSize()))
            , tanHalfFov(glm::tan(glm::radians(c.fovDeg) * 0.5f))
            , aspect(vpSize.y > 0.0f ? vpSize.x / vpSize.y : 1.0f)
            , nearZ(glm::max(c.near, 1e-3f))
        {
        }

        glm::vec3 toView(const glm::vec3& world) const { return glm::vec3(camera.viewMatrix * glm::vec4(world, 1.0f)); }

        glm::vec2 viewToScreen(const glm::vec3& v) const
        {
            const float ndcX = v.x / (-v.z * tanHalfFov * aspect);
            const float ndcY = v.y / (-v.z * tanHalfFov);
            return vpMin + glm::vec2(ndcX * 0.5f + 0.5f, 0.5f - ndcY * 0.5f) * vpSize;
        }

        bool point(const glm::vec3& world, glm::vec2& out) const
        {
            const glm::vec3 v = toView(world);
            if (v.z > -nearZ)
                return false;
            out = viewToScreen(v);
            return true;
        }

        bool segment(const glm::vec3& worldA, const glm::vec3& worldB, glm::vec2& outA, glm::vec2& outB) const
        {
            glm::vec3 a = toView(worldA);
            glm::vec3 b = toView(worldB);
            const bool aBehind = a.z > -nearZ;
            const bool bBehind = b.z > -nearZ;
            if (aBehind && bBehind)
                return false;
            if (aBehind || bBehind)
            {
                const glm::vec3 clipped = glm::mix(a, b, (-nearZ - a.z) / (b.z - a.z));
                (aBehind ? a : b) = clipped;
            }
            outA = viewToScreen(a);
            outB = viewToScreen(b);
            return true;
        }
    };

    // Handles come in X / Y / Z triples: true when h is one of the triple that starts at `first`.
    bool inTriple(int h, int first) { return h >= first && h < first + 3; }
}

GizmoController::~GizmoController()
{
    if (Globals::ui.getGizmo() == this) // the UI's reference is non-owning; detach before it dangles
        Globals::ui.setGizmo(nullptr);
}

void GizmoController::initialize()
{
    m_mouseListener = Globals::input.addMouseListener();
    m_mouseListener->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
        {
            if (m_activeHandle != Handle::None)
                continueDrag(glm::vec2(float(evt.x), float(evt.y)));
        };
    m_mouseListener->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button == 3 && m_activeHandle != Handle::None) // right click while dragging: put it back
            {
                endDrag(true);
                return;
            }
            if (evt.button != 1 || !m_visible || !m_selected || m_activeHandle != Handle::None)
                return;
            if (!Globals::input.isWindowHasFocus() || !Globals::ui.isViewportFocused())
                return;
            const glm::vec2 mouse(float(evt.x), float(evt.y));
            if (!viewportContains(m_viewport, mouse))
                return;
            float ringAngle = 0.0f;
            const Handle handle = pick(mouse, &ringAngle);
            if (handle != Handle::None)
            {
                beginDrag(handle, mouse, ringAngle);
                Globals::input.setMouseCaptured(true); // freefly camera yields the left-drag
            }
        };
    m_mouseListener->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button == 1 && m_activeHandle != Handle::None)
                endDrag(false);
        };
}

glm::vec3 GizmoController::frameAxis(int axis) const
{
    return m_frame * kAxis[axis];
}

void GizmoController::update(const Camera& camera, const Rect& viewport, Entity* selected, double deltaSec)
{
    (void)deltaSec;
    m_camera = camera;
    m_viewport = viewport;
    m_selected = selected;
    m_visible = selected != nullptr;
    m_shapes.valid = false;
    m_hovered = Handle::None;

    if (!m_visible)
    {
        if (m_activeHandle != Handle::None)
            endDrag(false);
        return;
    }

    // Rotate is object-local: the rings tilt with the object, so the ring you grab is the axis you turn about.
    // Translate / Scale stay world-aligned.
    const Transform world = worldTransform(selected);
    m_origin = world.pos;
    m_frame = (m_mode == EGizmoMode::Rotate) ? glm::normalize(world.quat) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    buildShapes();
    if (!m_shapes.valid)
        return;

    // Hover shows what a click would grab, so it follows the same gate as the click.
    if (m_activeHandle == Handle::None && Globals::ui.isViewportFocused())
    {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const glm::vec2 mouse(mousePos.x, mousePos.y);
        if (viewportContains(viewport, mouse))
            m_hovered = pick(mouse, nullptr);
    }

    draw();
}

void GizmoController::buildShapes()
{
    Shapes& s = m_shapes;
    s.valid = false;

    const Projector proj(m_camera, m_viewport);
    if (proj.vpSize.x <= 0.0f || proj.vpSize.y <= 0.0f)
        return;
    const glm::vec3 viewOrigin = proj.toView(m_origin);
    const float depth = -viewOrigin.z;
    if (depth < proj.nearZ * 4.0f)
        return;

    // Constant screen size: one arm length is sizePx pixels at the origin's depth.
    const float lengthPx = Globals::settings.gizmo.sizePx;
    m_worldLength = lengthPx * 2.0f * depth * proj.tanHalfFov / proj.vpSize.y;
    const float len = m_worldLength;
    const glm::vec3 toCamera = glm::normalize(m_camera.position - m_origin);
    s.origin = proj.viewToScreen(viewOrigin);

    if (m_mode == EGizmoMode::Translate || m_mode == EGizmoMode::Scale)
    {
        // While scaling, the arms stretch with the factor.
        const bool scaling = m_mode == EGizmoMode::Scale && m_activeHandle != Handle::None;
        const float armLength = scaling ? len * glm::clamp(m_dragFactor, 0.25f, 4.0f) : len;
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 a = frameAxis(i);
            const bool ok = proj.point(m_origin + a * (len * kArmStart), s.armStart[i])
                && proj.point(m_origin + a * (armLength * kArmHeadBase), s.armHeadBase[i])
                && proj.point(m_origin + a * armLength, s.armTip[i]);
            // An arm that points at the camera shrinks to a dot: fade it out and stop picking it.
            const float screenLength = glm::length(s.armTip[i] - s.origin) / (lengthPx * armLength / len);
            s.armAlpha[i] = ok ? glm::smoothstep(0.12f, 0.3f, screenLength) : 0.0f;
        }
    }

    if (m_mode == EGizmoMode::Translate)
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 u = frameAxis((i + 1) % 3) * len;
            const glm::vec3 v = frameAxis((i + 2) % 3) * len;
            const bool ok = proj.point(m_origin + u * kPlaneMin + v * kPlaneMin, s.plane[i][0])
                && proj.point(m_origin + u * kPlaneMax + v * kPlaneMin, s.plane[i][1])
                && proj.point(m_origin + u * kPlaneMax + v * kPlaneMax, s.plane[i][2])
                && proj.point(m_origin + u * kPlaneMin + v * kPlaneMax, s.plane[i][3]);
            orderClockwise(s.plane[i], 4);
            // An edge-on plane is a sliver and its ray hit is unstable: fade it out and stop picking it.
            s.planeAlpha[i] = ok ? glm::smoothstep(0.1f, 0.25f, glm::abs(glm::dot(frameAxis(i), toCamera))) : 0.0f;
        }

    if (m_mode == EGizmoMode::Rotate)
    {
        // A ring point is in front when it lies on the camera's half of the gizmo sphere (a little slack keeps a face-on
        // ring, which lies exactly on that boundary, in front).
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 u = frameAxis((i + 1) % 3) * len;
            const glm::vec3 v = frameAxis((i + 2) % 3) * len;
            for (int k = 0; k <= c_ringSegments; ++k)
            {
                const float angle = kTwoPi * float(k) / float(c_ringSegments);
                const glm::vec3 offset = u * glm::cos(angle) + v * glm::sin(angle);
                if (!proj.point(m_origin + offset, s.ring[i][k]))
                    s.ringState[i][k] = c_pointHidden;
                else
                    s.ringState[i][k] = glm::dot(offset, toCamera) > -0.05f * len ? c_pointFront : c_pointBack;
            }
        }
        s.viewRingRadius = lengthPx * kViewRing;
    }

    s.valid = true;
}

GizmoController::Handle GizmoController::pick(glm::vec2 mouse, float* outRingAngle) const
{
    const Shapes& s = m_shapes;
    if (!s.valid)
        return Handle::None;

    const float thickness = Globals::settings.gizmo.lineThickness;
    const float radius = Globals::settings.gizmo.pickRadiusPx + thickness * 0.5f;
    Handle best = Handle::None;
    float bestDistance = radius;
    auto consider = [&](Handle h, float distance)
        {
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = h;
            }
        };

    switch (m_mode)
    {
    case EGizmoMode::Translate:
        // Filled shapes win over lines: the centre dot, then a plane square the cursor is inside, then the nearest arm.
        if (glm::length(mouse - s.origin) <= centreSize(thickness) + radius * 0.5f)
            return Handle::MoveView;
        for (int i = 0; i < 3; ++i)
            if (s.planeAlpha[i] > 0.5f && insideClockwise(mouse, s.plane[i], 4))
                return Handle(int(Handle::MoveYZ) + i);
        for (int i = 0; i < 3; ++i)
            if (s.armAlpha[i] > 0.5f)
                consider(Handle(int(Handle::MoveX) + i), distanceToSegment(mouse, s.armStart[i], s.armTip[i]));
        break;

    case EGizmoMode::Scale:
    {
        const glm::vec2 d = glm::abs(mouse - s.origin);
        if (glm::max(d.x, d.y) <= centreSize(thickness) + radius * 0.5f)
            return Handle::ScaleUniform;
        for (int i = 0; i < 3; ++i)
            if (s.armAlpha[i] > 0.5f)
                consider(Handle(int(Handle::ScaleX) + i), distanceToSegment(mouse, s.armStart[i], s.armTip[i]));
        break;
    }

    case EGizmoMode::Rotate:
        consider(Handle::RotateView, glm::abs(glm::length(mouse - s.origin) - s.viewRingRadius));
        // Only the front half of a ring is grabbable: the back half sits behind the front of the others.
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < c_ringSegments; ++k)
            {
                if (s.ringState[i][k] != c_pointFront || s.ringState[i][k + 1] != c_pointFront)
                    continue;
                float t = 0.0f;
                const float distance = distanceToSegment(mouse, s.ring[i][k], s.ring[i][k + 1], &t);
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    best = Handle(int(Handle::RotateX) + i);
                    if (outRingAngle)
                        *outRingAngle = kTwoPi * (float(k) + t) / float(c_ringSegments);
                }
            }
        break;
    }
    return best;
}

void GizmoController::beginDrag(Handle handle, glm::vec2 mouse, float ringAngle)
{
    m_activeHandle = handle;
    m_dragStartMouse = mouse;
    m_dragOrigin = m_origin;
    m_dragParentWorld = m_selected->getParent() ? worldTransform(m_selected->getParent()) : Transform();
    m_startPos = m_selected->pos;
    m_startRot = m_selected->rot;
    m_startScale = m_selected->scale;
    m_dragAngle = 0.0f;
    m_dragFactor = 1.0f;

    const Ray ray = m_camera.screenToRay(m_viewport, mouse);
    const glm::vec3 toCamera = glm::normalize(m_camera.position - m_origin);
    const int h = int(handle);

    if (inTriple(h, int(Handle::MoveX)))
    {
        m_dragAxis = frameAxis(h - int(Handle::MoveX));
        float s = 0.0f;
        if (!closestParams(ray, m_dragOrigin, m_dragAxis, m_dragStartParam, s))
            m_dragStartParam = 0.0f;
    }
    else if (inTriple(h, int(Handle::MoveYZ)) || handle == Handle::MoveView)
    {
        m_dragAxis = handle == Handle::MoveView ? toCamera : frameAxis(h - int(Handle::MoveYZ));
        if (!rayPlane(ray, m_dragOrigin, m_dragAxis, m_dragStartHit))
            m_dragStartHit = m_dragOrigin;
    }
    else if (inTriple(h, int(Handle::RotateX)) || handle == Handle::RotateView)
    {
        m_dragStartWorldRot = glm::normalize(m_dragParentWorld.quat * m_selected->rot);
        glm::vec3 grabDir;
        if (handle == Handle::RotateView)
        {
            m_dragAxis = toCamera;
            grabDir = glm::normalize(glm::cross(m_dragAxis, glm::abs(m_dragAxis.y) < 0.9f ? kAxis[1] : kAxis[0]));
        }
        else
        {
            const int i = h - int(Handle::RotateX);
            m_dragAxis = frameAxis(i);
            grabDir = frameAxis((i + 1) % 3) * glm::cos(ringAngle) + frameAxis((i + 2) % 3) * glm::sin(ringAngle);
        }

        // A face-on ring follows the cursor around it (the ray / ring-plane hit). An edge-on ring has no usable plane hit,
        // so the drag runs along the ring's tangent at the grab point as it shows on screen.
        m_dragByPlane = handle == Handle::RotateView || glm::abs(glm::dot(m_dragAxis, toCamera)) > 0.3f;
        m_dragStartDir = grabDir;
        m_dragLastRaw = 0.0f;
        if (m_dragByPlane)
        {
            glm::vec3 hit;
            if (rayPlane(ray, m_dragOrigin, m_dragAxis, hit) && glm::length(hit - m_dragOrigin) > m_worldLength * 1e-3f)
                m_dragStartDir = glm::normalize(hit - m_dragOrigin);
        }
        else
        {
            const Projector proj(m_camera, m_viewport);
            const float lengthPx = Globals::settings.gizmo.sizePx;
            const glm::vec3 grabPoint = m_dragOrigin + grabDir * m_worldLength;
            const glm::vec3 tangent = glm::cross(m_dragAxis, grabDir); // d(grab point)/d(angle), per arm length
            constexpr float eps = 0.01f;
            glm::vec2 a(0.0f), b(0.0f), grabScreen(0.0f);
            m_dragTangentPx = glm::vec2(0.0f);
            if (proj.point(grabPoint, a) && proj.point(grabPoint + tangent * (m_worldLength * eps), b))
                m_dragTangentPx = (b - a) / eps;
            grabScreen = a;
            // A tangent that points at the camera has no screen length: keep a minimum speed along its direction (or across
            // the ring's radius when it has none at all).
            const float minPx = lengthPx * 0.35f;
            if (glm::length(m_dragTangentPx) < minPx)
            {
                glm::vec2 dir = m_dragTangentPx;
                if (glm::length(dir) < 1e-3f)
                {
                    const glm::vec2 radial = grabScreen - m_shapes.origin;
                    dir = glm::length(radial) > 1e-3f ? glm::vec2(-radial.y, radial.x) : glm::vec2(1.0f, 0.0f);
                }
                m_dragTangentPx = glm::normalize(dir) * minPx;
            }
        }
    }
    else // scale
    {
        glm::vec2 dir = glm::vec2(1.0f, -1.0f); // the centre box grows toward the top right
        if (handle != Handle::ScaleUniform)
        {
            const int i = h - int(Handle::ScaleX);
            dir = m_shapes.armTip[i] - m_shapes.origin;
        }
        m_dragScreenDir = glm::length(dir) > 1e-3f ? glm::normalize(dir) : glm::vec2(1.0f, 0.0f);
    }
}

void GizmoController::continueDrag(glm::vec2 mouse)
{
    if (!m_selected)
        return;

    const Ray ray = m_camera.screenToRay(m_viewport, mouse);
    const int h = int(m_activeHandle);

    if (inTriple(h, int(Handle::MoveX)))
    {
        float t = 0.0f, s = 0.0f;
        // No closest point in front of the camera within its range: the cursor is past the axis' vanishing point.
        if (!closestParams(ray, m_dragOrigin, m_dragAxis, t, s) || s < 0.0f || s > m_camera.far)
            return;
        m_selected->pos = inverseTransformPoint(m_dragParentWorld, m_dragOrigin + m_dragAxis * (t - m_dragStartParam));
    }
    else if (inTriple(h, int(Handle::MoveYZ)) || m_activeHandle == Handle::MoveView)
    {
        glm::vec3 hit;
        if (!rayPlane(ray, m_dragOrigin, m_dragAxis, hit) || glm::length(hit - ray.origin) > m_camera.far)
            return;
        m_selected->pos = inverseTransformPoint(m_dragParentWorld, m_dragOrigin + (hit - m_dragStartHit));
    }
    else if (inTriple(h, int(Handle::RotateX)) || m_activeHandle == Handle::RotateView)
    {
        if (m_dragByPlane)
        {
            glm::vec3 hit;
            if (!rayPlane(ray, m_dragOrigin, m_dragAxis, hit))
                return;
            const glm::vec3 offset = hit - m_dragOrigin;
            if (glm::length(offset) < m_worldLength * 1e-3f)
                return;
            const glm::vec3 now = glm::normalize(offset);
            const float raw = glm::atan(glm::dot(glm::cross(m_dragStartDir, now), m_dragAxis), glm::dot(m_dragStartDir, now));
            float delta = raw - m_dragLastRaw; // unwrapped, so a drag past half a turn keeps counting
            if (delta > kTwoPi * 0.5f) delta -= kTwoPi;
            if (delta < -kTwoPi * 0.5f) delta += kTwoPi;
            m_dragAngle += delta;
            m_dragLastRaw = raw;
        }
        else
            m_dragAngle = glm::dot(mouse - m_dragStartMouse, m_dragTangentPx) / glm::dot(m_dragTangentPx, m_dragTangentPx);

        const glm::quat worldRot = glm::angleAxis(m_dragAngle, m_dragAxis) * m_dragStartWorldRot;
        m_selected->rot = glm::normalize(glm::conjugate(m_dragParentWorld.quat) * worldRot); // back to entity-local space
    }
    else if (m_activeHandle != Handle::None) // scale: one arm length of cursor travel adds the start scale once more
    {
        m_dragFactor = glm::max(1.0f + glm::dot(mouse - m_dragStartMouse, m_dragScreenDir) / Globals::settings.gizmo.sizePx, 0.01f);
        m_selected->scale = glm::max(m_startScale * m_dragFactor, 1e-4f);
    }
}

void GizmoController::endDrag(bool cancel)
{
    if (cancel && m_selected)
    {
        m_selected->pos = m_startPos;
        m_selected->rot = m_startRot;
        m_selected->scale = m_startScale;
    }
    m_activeHandle = Handle::None;
    m_dragFactor = 1.0f;
    Globals::input.setMouseCaptured(false);
}

void GizmoController::draw() const
{
    const Shapes& s = m_shapes;
    if (!s.valid)
        return;

    // Under every ImGui window and over the 3D view (the Viewport window has no background), clipped to the viewport.
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    list->PushClipRect(ImVec2(float(m_viewport.min.x), float(m_viewport.min.y)), ImVec2(float(m_viewport.max.x), float(m_viewport.max.y)), true);

    const float thickness = Globals::settings.gizmo.lineThickness;
    const float lengthPx = Globals::settings.gizmo.sizePx;
    const bool dragging = m_activeHandle != Handle::None;
    const Handle lit = dragging ? m_activeHandle : m_hovered;
    const glm::vec3 toCamera = glm::normalize(m_camera.position - m_origin);
    const Projector proj(m_camera, m_viewport);

    // The held (or hovered) handle turns yellow; while one is held the others step back.
    auto color = [&](Handle h, const glm::vec3& base, float alpha)
        {
            if (h == lit)
                return toColor(kHighlightColor, alpha);
            return toColor(base, dragging ? alpha * 0.3f : alpha);
        };

    // Back to front: the arm that points away from the camera first.
    int order[3] = { 0, 1, 2 };
    oc::sort(order, order + 3, [&](int a, int b) { return glm::dot(frameAxis(a), toCamera) < glm::dot(frameAxis(b), toCamera); });

    if (m_mode == EGizmoMode::Translate)
    {
        if (inTriple(int(m_activeHandle), int(Handle::MoveX)))
        {
            // The line the object slides along.
            glm::vec2 a, b;
            const glm::vec3 extent = m_dragAxis * (m_worldLength * 200.0f);
            if (proj.segment(m_dragOrigin - extent, m_dragOrigin + extent, a, b))
                list->AddLine(im(a), im(b), toColor(kAxisColor[int(m_activeHandle) - int(Handle::MoveX)], 0.6f), 1.0f);
        }

        for (int i = 0; i < 3; ++i)
        {
            if (s.planeAlpha[i] <= 0.0f)
                continue;
            const Handle h = Handle(int(Handle::MoveYZ) + i);
            fillConvex(list, s.plane[i], 4, color(h, kAxisColor[i], s.planeAlpha[i] * (h == lit ? 0.55f : 0.3f)));
            ImVec2 outline[4];
            for (int k = 0; k < 4; ++k)
                outline[k] = im(s.plane[i][k]);
            list->AddPolyline(outline, 4, color(h, kAxisColor[i], s.planeAlpha[i]), ImDrawFlags_Closed, 1.5f);
        }

        for (const int i : order)
        {
            if (s.armAlpha[i] <= 0.0f)
                continue;
            const Handle h = Handle(int(Handle::MoveX) + i);
            const ImU32 c = color(h, kAxisColor[i], s.armAlpha[i]);
            list->AddLine(im(s.armStart[i]), im(s.armHeadBase[i]), c, thickness);
            const glm::vec2 along = s.armTip[i] - s.armHeadBase[i];
            const float alongLength = glm::length(along);
            const glm::vec2 side = alongLength > 1e-3f ? glm::vec2(-along.y, along.x) / alongLength * (thickness * 2.5f) : glm::vec2(0.0f);
            fillTriangle(list, s.armTip[i], s.armHeadBase[i] + side, s.armHeadBase[i] - side, c);
        }

        list->AddCircleFilled(im(s.origin), centreSize(thickness), color(Handle::MoveView, kViewColor, 0.9f), 24);
        list->AddCircle(im(s.origin), centreSize(thickness), toColor(glm::vec3(0.0f), 0.5f), 24, 1.0f);
    }
    else if (m_mode == EGizmoMode::Scale)
    {
        for (const int i : order)
        {
            if (s.armAlpha[i] <= 0.0f)
                continue;
            const Handle h = Handle(int(Handle::ScaleX) + i);
            const ImU32 c = color(h, kAxisColor[i], s.armAlpha[i]);
            const glm::vec2 half(thickness * 2.0f);
            list->AddLine(im(s.armStart[i]), im(s.armTip[i]), c, thickness);
            list->AddRectFilled(im(s.armTip[i] - half), im(s.armTip[i] + half), c);
        }
        const glm::vec2 half(centreSize(thickness));
        list->AddRectFilled(im(s.origin - half), im(s.origin + half), color(Handle::ScaleUniform, kViewColor, 0.9f));
        list->AddRect(im(s.origin - half), im(s.origin + half), toColor(glm::vec3(0.0f), 0.5f), 0.0f, 0, 1.0f);
    }
    else
    {
        // One polyline per run of points in the same state, so the joints stay closed.
        auto drawRuns = [&](int i, uint8 state, ImU32 c, float width)
            {
                ImVec2 run[c_ringSegments + 1];
                int count = 0;
                for (int k = 0; k <= c_ringSegments + 1; ++k)
                {
                    const bool inRun = k <= c_ringSegments && s.ringState[i][k] == state;
                    if (inRun)
                        run[count++] = im(s.ring[i][k]);
                    if ((!inRun || k == c_ringSegments) && count > 0)
                    {
                        if (count > 1)
                            list->AddPolyline(run, count, c, 0, width);
                        count = 0;
                    }
                }
            };

        list->AddCircle(im(s.origin), lengthPx, toColor(kViewColor, dragging ? 0.1f : 0.25f), 64, 1.0f);
        for (int i = 0; i < 3; ++i)
            drawRuns(i, c_pointBack, color(Handle(int(Handle::RotateX) + i), kAxisColor[i], 0.25f), thickness * 0.5f);
        list->AddCircle(im(s.origin), s.viewRingRadius, color(Handle::RotateView, kViewColor, 0.8f), 64, thickness);
        for (const int i : order)
            drawRuns(i, c_pointFront, color(Handle(int(Handle::RotateX) + i), kAxisColor[i], 1.0f), thickness);

        if (dragging)
        {
            // The swept angle as a fan in the ring's plane.
            const float radius = m_worldLength * (m_activeHandle == Handle::RotateView ? kViewRing : 1.0f);
            const glm::vec3 side = glm::cross(m_dragAxis, m_dragStartDir);
            const float swept = glm::clamp(m_dragAngle, -kTwoPi, kTwoPi);
            const int segments = glm::max(1, int(glm::abs(swept) / kTwoPi * float(c_ringSegments)) + 1);
            const ImU32 fill = toColor(kHighlightColor, 0.25f);
            glm::vec2 prev;
            bool prevOk = proj.point(m_dragOrigin + m_dragStartDir * radius, prev);
            if (prevOk)
                list->AddLine(im(s.origin), im(prev), toColor(kHighlightColor, 0.8f), 1.0f);
            for (int k = 1; k <= segments; ++k)
            {
                const float angle = swept * float(k) / float(segments);
                glm::vec2 next;
                const bool ok = proj.point(m_dragOrigin + (m_dragStartDir * glm::cos(angle) + side * glm::sin(angle)) * radius, next);
                if (ok && prevOk)
                    fillTriangle(list, s.origin, prev, next, fill);
                if (ok && k == segments)
                    list->AddLine(im(s.origin), im(next), toColor(kHighlightColor, 0.8f), 1.0f);
                prev = next;
                prevOk = ok;
            }
        }
    }

    if (dragging)
    {
        oc::string text;
        if (m_mode == EGizmoMode::Translate)
        {
            const glm::vec3 d = m_origin - m_dragOrigin;
            text = oc::format("{:.3f}  {:.3f}  {:.3f}", d.x, d.y, d.z);
        }
        else if (m_mode == EGizmoMode::Rotate)
            text = oc::format("{:.1f} deg", glm::degrees(m_dragAngle));
        else
            text = oc::format("x{:.3f}", m_dragFactor);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 at(mouse.x + 18.0f, mouse.y - 6.0f);
        list->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), toColor(glm::vec3(0.0f), 0.8f), text.c_str());
        list->AddText(at, toColor(glm::vec3(1.0f), 1.0f), text.c_str());
    }

    list->PopClipRect();
}
