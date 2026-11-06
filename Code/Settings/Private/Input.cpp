module Settings.Input;

import Core;
import Settings.Tweaks;

void Settings::registerFreeFlyCamera(FreeFlyCameraSettings& s)
{
    Tweak::boolean("Editor", "Lock To World Up", &s.lockToWorldUp);
    Tweak::floatVar("Editor", "Speed", &s.speed, 0.1f, 200.0f, 0.1f);
    Tweak::floatVar("Editor", "Sensitivity", &s.sensitivity, 0.001f, 0.015f, 0.001f);
    Tweak::floatVar("Editor", "Camera Near", &s.cameraNear, 0.001f, 10.0f, 0.001f);
    Tweak::floatVar("Editor", "Camera Far", &s.cameraFar, 500.0f, 262114.0f, 500.0f);
}

void Settings::registerGizmo(GizmoSettings& s)
{
    Tweak::floatVar("Editor/Gizmo", "Size (px)", &s.sizePx, 40.0f, 400.0f, 1.0f);
    Tweak::floatVar("Editor/Gizmo", "Pick Radius (px)", &s.pickRadiusPx, 2.0f, 24.0f, 0.5f);
    Tweak::floatVar("Editor/Gizmo", "Line Thickness", &s.lineThickness, 1.0f, 8.0f, 0.5f);
}
