export module Settings.Input;

import Core;

// "Editor": the free-fly editor camera (FreeFlyCameraController).
export struct FreeFlyCameraSettings
{
    bool lockToWorldUp = true;
    float speed = 50.0f;          // also stepped by the mouse wheel
    float sensitivity = 0.005f;
    float cameraNear = 0.05f;
    float cameraFar = 42000.0f;
};

// "Editor/Gizmo": the transform gizmo (GizmoController).
export struct GizmoSettings
{
    float screenSize = 0.06f;     // gizmo apparent size: world scale = distance * screenSize
    float axisPickFrac = 0.07f;   // axis pick tube radius as a fraction of the arm length
    float planePickScale = 1.4f;  // plane pick radius as a multiple of the plane handle's bounds radius
    float ringPickScale = 0.6f;   // rotate ring radial tolerance as a multiple of the arc's bounds radius
};

export namespace Settings
{
    void registerFreeFlyCamera(FreeFlyCameraSettings& s);
    void registerGizmo(GizmoSettings& s);
}
