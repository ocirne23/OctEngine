module Input;

import Core;
import Core.glm;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Core.Sphere;
import Core.Tweaks;
import Core.SDL;
import Entity;
import RendererVK;
import UI;

import :Input;
import :GizmoController;

namespace
{
    const glm::vec3 kAxis[3] = { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) };

    int dominantAxis(const glm::vec3& v)
    {
        const glm::vec3 a = glm::abs(v);
        if (a.x >= a.y && a.x >= a.z) return 0;
        return a.y >= a.z ? 1 : 2;
    }

    int minorAxis(const glm::vec3& v)
    {
        const glm::vec3 a = glm::abs(v);
        if (a.x <= a.y && a.x <= a.z) return 0;
        return a.y <= a.z ? 1 : 2;
    }

    Entity* findChild(Entity* parent, const char* name)
    {
        if (SceneComponent* sc = getComponent<SceneComponent>(parent))
            for (const EntityPtr& c : sc->children)
                if (oc::string_view(c->getName()) == name)
                    return c.get();
        return nullptr;
    }

    bool handleBounds(Entity* entity, Sphere& out)
    {
        if (RenderComponent* rc = getComponent<RenderComponent>(entity))
        {
            out = rc->node.getWorldBounds();
            return true;
        }
        return false;
    }

    Transform worldTransform(Entity* entity)
    {
        Transform t(entity->pos, entity->scale, entity->rot);
        for (Entity* p = entity->parent; p; p = p->parent)
            t = composeTransform(Transform(p->pos, p->scale, p->rot), t);
        return t;
    }

    glm::vec3 inverseTransformPoint(const Transform& t, const glm::vec3& p)
    {
        glm::vec3 v = (p - t.pos) * (1.0f / t.scale);
        return glm::conjugate(t.quat) * v;
    }

    // Parameter along the (unit) line direction `ld` of the point on the line closest to `ray`.
    float closestParamOnLine(const Ray& ray, const glm::vec3& lo, const glm::vec3& ld)
    {
        const glm::vec3 w = ray.origin - lo;
        const float b = glm::dot(ray.dir, ld);
        const float d = glm::dot(ray.dir, w);
        const float e = glm::dot(ld, w);
        const float denom = 1.0f - b * b;            // ray.dir and ld are unit length
        if (denom < 1e-6f)
            return e;                                // near-parallel: project onto the line
        return (e - b * d) / denom;
    }

    float distancePointToRay(const glm::vec3& p, const Ray& ray)
    {
        const float t = glm::max(glm::dot(p - ray.origin, ray.dir), 0.0f);
        return glm::length(p - (ray.origin + ray.dir * t));
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
}

GizmoController::~GizmoController()
{
    if (Globals::ui.getGizmo() == this) // the UI's reference is non-owning; detach before it dangles
        Globals::ui.setGizmo(nullptr);
}

void GizmoController::initialize(World& world)
{
    m_gizmo = world.spawn("Gizmo", Transform());

    m_mouseListener = Globals::input.addMouseListener();
    m_mouseListener->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
        {
            m_mousePos = glm::vec2(float(evt.x), float(evt.y));
            if (m_activeHandle != Handle::None)
                continueDrag(m_camera.screenToRay(m_viewport, m_mousePos));
        };
    m_mouseListener->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button != 1 || !m_visible || !m_selected)
                return;
            if (!Globals::input.isWindowHasFocus() || !Globals::ui.isViewportFocused())
                return;
            m_mousePos = glm::vec2(float(evt.x), float(evt.y));
            if (!viewportContains(m_viewport, m_mousePos))
                return;
            // if gizmo too close to camera, return
            const glm::vec3 targetPos = m_selected->pos;
            const float dist = glm::length(m_camera.position - targetPos);
            if (dist < 0.1f)
                return;

            const Ray ray = m_camera.screenToRay(m_viewport, m_mousePos);
            const Handle handle = pickHandle(ray);
            if (handle != Handle::None)
            {
                beginDrag(handle, ray);
                Globals::input.setMouseCaptured(true); // freefly camera yields the left-drag
            }
        };
    m_mouseListener->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button == 1 && m_activeHandle != Handle::None)
            {
                m_activeHandle = Handle::None;
                Globals::input.setMouseCaptured(false);
            }
        };

    Tweak::floatVar("Editor/Gizmo", "Screen Size", &m_screenSize, 0.01f, 0.5f, 0.001f);
    Tweak::floatVar("Editor/Gizmo", "Axis Pick Frac", &m_axisPickFrac, 0.02f, 0.5f, 0.01f);
    Tweak::floatVar("Editor/Gizmo", "Plane Pick Scale", &m_planePickScale, 0.1f, 4.0f, 0.1f);
    Tweak::floatVar("Editor/Gizmo", "Ring Pick Scale", &m_ringPickScale, 0.1f, 4.0f, 0.1f);

    applyMode();
}

void GizmoController::update(const Camera& camera, const Rect& viewport, Entity* selected, double deltaSec)
{
    m_camera = camera;
    m_viewport = viewport;
    m_selected = selected;
    m_visible = selected != nullptr;

    if (!m_gizmo)
        return;

    m_gizmo->setEnabled(m_visible);

    if (!m_visible)
    {
        if (m_activeHandle != Handle::None)
        {
            m_activeHandle = Handle::None;
            Globals::input.setMouseCaptured(false);
        }
        return;
    }

    const glm::vec3 targetPos = worldTransform(selected).pos;
    const float dist = glm::length(camera.position - targetPos);
	if (dist < 0.1f)
		m_gizmo->setEnabled(false);

    // Rotate is object-local: rings tilt with the object so the ring you grab is the axis you turn about.
    // Translate/Scale stay world-aligned (global).
    m_gizmoRot = (m_mode == EGizmoMode::Rotate) ? glm::normalize(worldTransform(selected).quat)
                                                : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    m_gizmoScale = glm::max(dist * m_screenSize, 1e-4f);
    m_gizmo->pos = targetPos;
    m_gizmo->scale = m_gizmoScale;
    m_gizmo->rot = m_gizmoRot;

    applyMode();
}

GizmoController::Handle GizmoController::pickAxisInGroup(const char* group, const glm::vec3& origin, const Ray& ray) const
{
    const Handle axisH[3] = { Handle::AxisX, Handle::AxisY, Handle::AxisZ };
    Handle best = Handle::None;
    float bestDist = FLT_MAX;
    if (Entity* arrows = findChild(m_gizmo.get(), group))
        if (SceneComponent* sc = getComponent<SceneComponent>(arrows))
            for (const EntityPtr& child : sc->children)
            {
                Sphere b;
                if (!handleBounds(child.get(), b))
                    continue;
                const glm::vec3 d = b.pos - origin;
                const int axis = dominantAxis(d);
                const float armLen = glm::abs(d[axis]) + b.radius;
                float t = glm::clamp(closestParamOnLine(ray, origin, kAxis[axis]), 0.0f, armLen);
                const float dist = distancePointToRay(origin + kAxis[axis] * t, ray);
                if (dist <= armLen * m_axisPickFrac && dist < bestDist)
                {
                    bestDist = dist;
                    best = axisH[axis];
                }
            }
    return best;
}

GizmoController::Handle GizmoController::pickRing(const glm::vec3& origin, const Ray& ray) const
{
    const Handle ringH[3] = { Handle::RingX, Handle::RingY, Handle::RingZ };
    Handle best = Handle::None;
    float bestDist = FLT_MAX;
    if (Entity* arches = findChild(m_gizmo.get(), "Arches"))
        if (SceneComponent* sc = getComponent<SceneComponent>(arches))
            for (const EntityPtr& child : sc->children)
            {
                Sphere b;
                if (!handleBounds(child.get(), b))
                    continue;
                // Work in the gizmo's (possibly tilted) frame: un-rotate the arc offset to a cardinal axis,
                // then take that axis back out to world for the ring's plane normal.
                const int axis = minorAxis(glm::conjugate(m_gizmoRot) * (b.pos - origin));
                const glm::vec3 normal = m_gizmoRot * kAxis[axis];
                glm::vec3 hit;
                if (!rayPlane(ray, origin, normal, hit))  // ray near-parallel to an edge-on ring is rejected
                    continue;
                const float ringRadius = glm::length(b.pos - origin); // in-plane (arc center's normal component ~0)
                const glm::vec3 inPlane = hit - origin;
                if (glm::length(inPlane) < 1e-5f)
                    continue;
                // Nearest point on the ring CIRCLE to the ray, then its true 3D distance to the ray: this
                // selects the arc the cursor is actually over, instead of any plane that crosses radius R.
                const glm::vec3 circlePoint = origin + glm::normalize(inPlane) * ringRadius;
                const float dist = distancePointToRay(circlePoint, ray);
                if (dist <= b.radius * m_ringPickScale && dist < bestDist)
                {
                    bestDist = dist;
                    best = ringH[axis];
                }
            }
    return best;
}

GizmoController::Handle GizmoController::pickHandle(const Ray& ray) const
{
    const glm::vec3 origin = worldTransform(m_selected).pos;

    if (m_mode == EGizmoMode::Rotate)
        return pickRing(origin, ray);

    if (m_mode == EGizmoMode::Scale)
        return pickAxisInGroup("Scale", origin, ray);

    // Translate: plane quads (small corner handles) take priority over the axis lines. Each handle's
    // world bounds tell us where it is and how big it is, so picking self-calibrates to the actual mesh.
    const Handle planeForNormal[3] = { Handle::PlaneYZ, Handle::PlaneZX, Handle::PlaneXY };
    if (Entity* planes = findChild(m_gizmo.get(), "Planes"))
        if (SceneComponent* sc = getComponent<SceneComponent>(planes))
            for (const EntityPtr& child : sc->children)
            {
                Sphere b;
                if (!handleBounds(child.get(), b))
                    continue;
                const int n = minorAxis(b.pos - origin); // axis the quad is perpendicular to
                glm::vec3 hit;
                if (!rayPlane(ray, b.pos, kAxis[n], hit))
                    continue;
                if (glm::length(hit - b.pos) <= b.radius * m_planePickScale)
                    return planeForNormal[n];
            }

    return pickAxisInGroup("Arrows", origin, ray);
}

void GizmoController::beginDrag(Handle handle, const Ray& ray)
{
    m_activeHandle = handle;
    m_dragOrigin = worldTransform(m_selected).pos;
    m_dragParentWorld = m_selected->parent ? worldTransform(m_selected->parent) : Transform();

    switch (handle)
    {
    case Handle::AxisX: case Handle::AxisY: case Handle::AxisZ:
        m_dragAxis = kAxis[int(handle) - int(Handle::AxisX)];
        m_dragStartParam = closestParamOnLine(ray, m_dragOrigin, m_dragAxis);
        if (m_mode == EGizmoMode::Scale)
        {
            m_dragStartScale = m_selected->scale;
            // The arm length (origin -> hit param) maps a full doubling, so the drag feels proportional.
            m_dragScaleRef = glm::max(glm::abs(m_dragStartParam), 1e-3f);
        }
        break;
    case Handle::PlaneXY: m_dragNormal = kAxis[2]; rayPlane(ray, m_dragOrigin, m_dragNormal, m_dragStartHit); break;
    case Handle::PlaneYZ: m_dragNormal = kAxis[0]; rayPlane(ray, m_dragOrigin, m_dragNormal, m_dragStartHit); break;
    case Handle::PlaneZX: m_dragNormal = kAxis[1]; rayPlane(ray, m_dragOrigin, m_dragNormal, m_dragStartHit); break;
    case Handle::RingX: case Handle::RingY: case Handle::RingZ:
        // Tilted ring axis (object-local direction in world space) -> rotating about it is a local rotation.
        m_dragNormal = m_gizmoRot * kAxis[int(handle) - int(Handle::RingX)];
        rayPlane(ray, m_dragOrigin, m_dragNormal, m_dragStartHit);
        m_dragStartRot = m_dragParentWorld.quat * m_selected->rot; // world rotation at grab
        break;
    default: break;
    }
}

void GizmoController::continueDrag(const Ray& ray)
{
    if (!m_selected)
        return;

    switch (m_activeHandle)
    {
    case Handle::AxisX: case Handle::AxisY: case Handle::AxisZ:
    {
        const float t = closestParamOnLine(ray, m_dragOrigin, m_dragAxis);
        if (m_mode == EGizmoMode::Scale)
        {
            const float factor = 1.0f + (t - m_dragStartParam) / m_dragScaleRef;
            m_selected->scale = glm::max(m_dragStartScale * factor, 1e-3f); // uniform: Entity scale is a single float
        }
        else
        {
            const glm::vec3 newOrigin = m_dragOrigin + m_dragAxis * (t - m_dragStartParam);
            m_selected->pos = inverseTransformPoint(m_dragParentWorld, newOrigin);
        }
        break;
    }
    case Handle::PlaneXY: case Handle::PlaneYZ: case Handle::PlaneZX:
    {
        glm::vec3 hit;
        if (!rayPlane(ray, m_dragOrigin, m_dragNormal, hit))
            return;
        m_selected->pos = inverseTransformPoint(m_dragParentWorld, m_dragOrigin + (hit - m_dragStartHit));
        break;
    }
    case Handle::RingX: case Handle::RingY: case Handle::RingZ:
    {
        glm::vec3 hit;
        if (!rayPlane(ray, m_dragOrigin, m_dragNormal, hit))
            return;
        const glm::vec3 startVec = m_dragStartHit - m_dragOrigin;
        const glm::vec3 nowVec = hit - m_dragOrigin;
        if (glm::length(startVec) < 1e-5f || glm::length(nowVec) < 1e-5f)
            return;
        const glm::vec3 a = glm::normalize(startVec);
        const glm::vec3 b = glm::normalize(nowVec);
        const float angle = glm::atan(glm::dot(glm::cross(a, b), m_dragNormal), glm::dot(a, b));
        const glm::quat worldRot = glm::angleAxis(angle, m_dragNormal) * m_dragStartRot;
        m_selected->rot = glm::conjugate(m_dragParentWorld.quat) * worldRot; // back to entity-local space
        break;
    }
    default:
        return;
    }
}

void GizmoController::applyMode()
{
    setChildEnabled("Arrows", m_mode == EGizmoMode::Translate);
    setChildEnabled("Planes", m_mode == EGizmoMode::Translate);
    setChildEnabled("Arches", m_mode == EGizmoMode::Rotate);
    setChildEnabled("Scale",  m_mode == EGizmoMode::Scale);
}

void GizmoController::setChildEnabled(const char* name, bool enabled)
{
    SceneComponent* root = getComponent<SceneComponent>(m_gizmo.get());
    if (!root)
        return;
    for (const EntityPtr& child : root->children)
        if (oc::string_view(child->getName()) == name)
        {
            child->setEnabled(enabled);
            return;
        }
}
