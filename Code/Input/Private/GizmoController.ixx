export module Input:GizmoController;

import Core;
import Core.glm;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Entity;

import Input.fwd;
import :Input; // MouseListenerHandle
export import UI.Gizmo; // EGizmoMode + the IGizmo interface the UI owns this through

// Spawns the Gizmo.pre prefab and drives it: follows the scene-panel selection, keeps a constant
// apparent screen size, switches the visible handle set by mode. Drag interaction is added on top.
// Lives here (not in UI) because it needs mouse listeners and viewport focus; the UI owns the instance
// through IGizmo, see UI.Gizmo.
export class GizmoController final : public IGizmo
{
public:

    GizmoController() = default;
    ~GizmoController() override;
    GizmoController(const GizmoController&) = delete;

    void initialize(World& world);
    void update(const Camera& camera, const Rect& viewport, Entity* selected, double deltaSec) override;

    void setMode(EGizmoMode mode) override { m_mode = mode; }
    EGizmoMode getMode() const override { return m_mode; }

    Entity* getGizmoEntity() const override { return m_gizmo.get(); }
    bool isVisible() const override { return m_visible && m_selected != nullptr; }
    bool isDragging() const override { return m_activeHandle != Handle::None; }

private:

    enum class Handle : uint8
    {
        None,
        AxisX, AxisY, AxisZ,
        PlaneXY, PlaneYZ, PlaneZX,
        RingX, RingY, RingZ,
        ScaleUniform,
    };

    void applyMode();
    void setChildEnabled(const char* name, bool enabled);

    Handle pickHandle(const Ray& ray) const; // analytic ray vs axis-line / plane-quad / ring test in gizmo space
    Handle pickAxisInGroup(const char* group, const glm::vec3& origin, const Ray& ray) const;
    Handle pickRing(const glm::vec3& origin, const Ray& ray) const;
    void   beginDrag(Handle handle, const Ray& ray);
    void   continueDrag(const Ray& ray);

    MouseListenerHandle m_mouseListener; // unregisters itself on destruction
    EntityPtr           m_gizmo;
    Entity*        m_selected = nullptr;

    EGizmoMode m_mode = EGizmoMode::Translate;
    bool       m_visible = false;
    Handle     m_activeHandle = Handle::None;

    float m_gizmoScale = 1.0f;     // last applied world scale, used to size pick volumes
    glm::quat m_gizmoRot = glm::quat(1, 0, 0, 0); // last applied gizmo orientation (object-aligned in Rotate mode)

    glm::vec2 m_mousePos = glm::vec2(0.0f); // window-space, matches the viewport Rect

    // Latest frame state, captured in update() so the event-driven drag can unproject without it.
    Camera m_camera;
    Rect   m_viewport;

    // Drag anchor, snapshotted in beginDrag() so dragging is absolute (no per-frame drift).
    glm::vec3 m_dragOrigin = glm::vec3(0.0f);  // gizmo world origin at grab
    Transform m_dragParentWorld;               // selected entity's parent world transform (world->local)
    glm::vec3 m_dragAxis = glm::vec3(0.0f);    // axis drag: world-space axis direction
    glm::vec3 m_dragNormal = glm::vec3(0.0f);  // plane/ring drag: world-space plane normal (rotation axis)
    glm::vec3 m_dragStartHit = glm::vec3(0.0f);// plane/ring drag: ray/plane hit at grab
    float     m_dragStartParam = 0.0f;         // axis drag: param along axis at grab
    glm::quat m_dragStartRot = glm::quat(1, 0, 0, 0); // rotate drag: selected entity's world rotation at grab
    float     m_dragStartScale = 1.0f;         // scale drag: selected entity's scale at grab
    float     m_dragScaleRef = 1.0f;           // scale drag: axis param span that maps to a doubling
};
