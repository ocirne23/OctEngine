export module Settings.Input;

import Core;

// "Editor": the free-fly editor camera (FreeFlyCameraController).
export struct FreeFlyCameraSettings
{
    bool lockToWorldUp = true;
    float speed = 50.0f;          // also stepped by the mouse wheel
    float sensitivity = 0.005f;
    float cameraNear = 0.1f;
    float cameraFar = 60000.0f;
};

// "Editor/Gizmo": the transform gizmo (GizmoController).
export struct GizmoSettings
{
    float sizePx = 110.0f;        // arm length / ring radius on screen, in pixels
    float pickRadiusPx = 8.0f;    // how far from a drawn line or point a click still grabs it, in pixels
    float lineThickness = 3.0f;   // handle line width, in pixels
};

export namespace Settings
{
    void registerFreeFlyCamera(FreeFlyCameraSettings& s);
    void registerGizmo(GizmoSettings& s);
}
