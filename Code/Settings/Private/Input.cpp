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
    Tweak::floatVar("Editor/Gizmo", "Screen Size", &s.screenSize, 0.01f, 0.5f, 0.001f);
    Tweak::floatVar("Editor/Gizmo", "Axis Pick Frac", &s.axisPickFrac, 0.02f, 0.5f, 0.01f);
    Tweak::floatVar("Editor/Gizmo", "Plane Pick Scale", &s.planePickScale, 0.1f, 4.0f, 0.1f);
    Tweak::floatVar("Editor/Gizmo", "Ring Pick Scale", &s.ringPickScale, 0.1f, 4.0f, 0.1f);
}
