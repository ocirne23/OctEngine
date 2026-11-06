export module UI.Gizmo;

import Core;
import Core.Camera;
import Core.Rect;
import Entity;

export enum class EGizmoMode : uint8
{
    Translate = 0,
    Rotate,
    Scale,
};

// The transform gizmo as the UI consumes it. The implementation (Input's GizmoController) needs mouse
// listeners and viewport focus, and Input sits BELOW UI in the dependency order, so UI owns the gizmo
// through this interface rather than naming the concrete type -- the same split Core.VrSession uses
// between RendererVK's OpenXRSession and Input's VrInput. The app constructs one and hands it over
// with UI::setGizmo; everything after that is driven from the UI.
export class IGizmo
{
public:

    virtual ~IGizmo() = default;

    // Follows `selected`, keeping a constant apparent screen size within `viewport`. Called inside the widget pass: it
    // draws itself with ImGui.
    virtual void update(const Camera& camera, const Rect& viewport, Entity* selected, double deltaSec) = 0;

    virtual void setMode(EGizmoMode mode) = 0;
    virtual EGizmoMode getMode() const = 0;

    virtual bool isVisible() const = 0;
    virtual bool isDragging() const = 0;
};
