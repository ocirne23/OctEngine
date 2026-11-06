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

// The transform gizmo: follows the scene-panel selection and is drawn in SCREEN SPACE with ImGui (no mesh, no entity).
// update() projects every handle of the mode into viewport pixels ONCE (m_shapes); the draw and the click pick both read
// those same 2D shapes, so a click grabs exactly what is on screen. Lives here (not in UI) because it needs mouse
// listeners and viewport focus; the UI owns the instance through IGizmo, see UI.Gizmo.
export class GizmoController final : public IGizmo
{
public:

    GizmoController() = default;
    ~GizmoController() override;
    GizmoController(const GizmoController&) = delete;

    void initialize();
    void update(const Camera& camera, const Rect& viewport, Entity* selected, double deltaSec) override;

    void setMode(EGizmoMode mode) override { if (m_activeHandle == Handle::None) m_mode = mode; }
    EGizmoMode getMode() const override { return m_mode; }

    bool isVisible() const override { return m_visible; }
    bool isDragging() const override { return m_activeHandle != Handle::None; }

private:

    enum class Handle : uint8
    {
        None,
        MoveX, MoveY, MoveZ,
        MoveYZ, MoveZX, MoveXY, // by the plane's normal axis: X, Y, Z
        MoveView,               // the centre dot: moves in the camera plane
        RotateX, RotateY, RotateZ,
        RotateView,             // the outer circle: turns about the view direction
        ScaleX, ScaleY, ScaleZ, // Entity scale is one float, so every scale handle scales uniformly
        ScaleUniform,
    };

    static constexpr int c_ringSegments = 64;

    // Point states of a projected ring point.
    static constexpr uint8 c_pointHidden = 0; // behind the near plane
    static constexpr uint8 c_pointBack   = 1; // on the far side of the gizmo sphere
    static constexpr uint8 c_pointFront  = 2;

    // Every handle of the current mode, in viewport pixels. Built by buildShapes(), read by draw() and pick().
    struct Shapes
    {
        bool      valid = false;
        glm::vec2 origin = glm::vec2(0.0f);
        glm::vec2 armStart[3], armHeadBase[3], armTip[3];
        float     armAlpha[3] = {};  // 0 = the arm points at the camera: hidden and not pickable
        glm::vec2 plane[3][4];       // by normal axis, clockwise on screen
        float     planeAlpha[3] = {}; // 0 = edge-on
        glm::vec2 ring[3][c_ringSegments + 1];
        uint8     ringState[3][c_ringSegments + 1] = {};
        float     viewRingRadius = 0.0f;
    };

    void   buildShapes();
    void   draw() const;
    Handle pick(glm::vec2 mouse, float* outRingAngle) const;
    void   beginDrag(Handle handle, glm::vec2 mouse, float ringAngle);
    void   continueDrag(glm::vec2 mouse);
    void   endDrag(bool cancel);

    glm::vec3 frameAxis(int axis) const;

    MouseListenerHandle m_mouseListener; // unregisters itself on destruction
    Entity*             m_selected = nullptr;

    EGizmoMode m_mode = EGizmoMode::Translate;
    bool       m_visible = false;
    Handle     m_activeHandle = Handle::None;
    Handle     m_hovered = Handle::None;

    // Latest frame state, captured in update() so the event-driven pick and drag use the camera the shapes were built with.
    Camera    m_camera;
    Rect      m_viewport;
    glm::vec3 m_origin = glm::vec3(0.0f);          // gizmo world position
    glm::quat m_frame = glm::quat(1, 0, 0, 0);     // handle frame: the object's rotation in Rotate mode, else world-aligned
    float     m_worldLength = 1.0f;                // arm length in world units at the gizmo's depth
    Shapes    m_shapes;

    // Drag anchor, snapshotted in beginDrag() so dragging is absolute (no per-frame drift).
    glm::vec2 m_dragStartMouse = glm::vec2(0.0f);
    glm::vec3 m_dragOrigin = glm::vec3(0.0f);      // gizmo world origin at grab
    Transform m_dragParentWorld;                   // selected entity's parent world transform (world -> local)
    glm::vec3 m_dragAxis = glm::vec3(0.0f);        // move axis, move plane normal or rotation axis (world)
    glm::vec3 m_dragStartHit = glm::vec3(0.0f);    // move plane / rotate: the ray hit at grab
    float     m_dragStartParam = 0.0f;             // move axis: param along the axis at grab
    glm::vec3 m_dragStartDir = glm::vec3(1, 0, 0); // rotate: unit vector from the origin to the grab point, in the ring plane
    bool      m_dragByPlane = true;                // rotate: follow the ray / ring-plane hit (face-on) or the screen tangent (edge-on)
    glm::vec2 m_dragTangentPx = glm::vec2(0.0f);   // rotate by tangent: screen pixels per radian at the grab point
    float     m_dragLastRaw = 0.0f;                // rotate by plane: last wrapped angle, to unwrap past 180 degrees
    glm::vec2 m_dragScreenDir = glm::vec2(1, 0);   // scale: the screen direction that grows the object
    float     m_dragAngle = 0.0f;                  // rotate feedback
    float     m_dragFactor = 1.0f;                 // scale feedback
    glm::quat m_dragStartWorldRot = glm::quat(1, 0, 0, 0);

    // The selected entity's local transform at grab, restored by a right click.
    glm::vec3 m_startPos = glm::vec3(0.0f);
    glm::quat m_startRot = glm::quat(1, 0, 0, 0);
    float     m_startScale = 1.0f;
};
