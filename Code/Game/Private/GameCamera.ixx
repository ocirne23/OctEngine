export module Game:GameCamera;

import Core;
import Core.glm;
import Core.Camera;
import Core.Tweaks;

// Angled top-down follow camera: fixed downward pitch, player-rotatable yaw (Q/E held + RMB drag),
// scroll-wheel zoom. Owns no entity state — apply() overwrites the frame camera's position and view
// matrix after the fly camera produced it (the applyPlayerCamera pattern), so everything downstream
// (UI picking, audio listener, renderer, culling) sees the game view.
export class GameCamera final
{
public:
    void registerTweaks()
    {
        // Camera feel is a PERSONAL preference: saved locally, never synced from the server.
        const Tweak::ScopedFlags scoped(ETweakFlags::Saved);
        Tweak::floatVar("Game/Camera", "Pitch", &m_pitchDeg, 30.0f, 75.0f, 0.5f);
        Tweak::floatVar("Game/Camera", "Distance", &m_distance, 4.0f, 80.0f, 0.5f);
        Tweak::floatVar("Game/Camera", "Min distance", &m_minDistance, 2.0f, 40.0f, 0.5f);
        Tweak::floatVar("Game/Camera", "Max distance", &m_maxDistance, 10.0f, 120.0f, 0.5f);
        Tweak::floatVar("Game/Camera", "Zoom speed", &m_zoomSpeed, 0.5f, 10.0f, 0.1f);
        Tweak::floatVar("Game/Camera", "Yaw speed (deg/s)", &m_yawSpeed, 30.0f, 360.0f, 1.0f);
        Tweak::floatVar("Game/Camera", "Drag sensitivity", &m_dragSensitivity, 0.05f, 1.0f, 0.01f);
        Tweak::floatVar("Game/Camera", "Aim height", &m_aimHeight, 0.0f, 4.0f, 0.05f);
    }

    // qeAxis: -1..1 from held Q/E keys. mouseDragDeltaX: pixels of RMB drag this frame.
    // wheelDelta: accumulated scroll (positive = zoom in).
    void apply(Camera& camera, const glm::vec3& targetPos, float deltaSec,
               float qeAxis, float mouseDragDeltaX, float wheelDelta)
    {
        m_yawDeg += qeAxis * m_yawSpeed * deltaSec + mouseDragDeltaX * m_dragSensitivity;
        m_distance = glm::clamp(m_distance - wheelDelta * m_zoomSpeed, m_minDistance, m_maxDistance);

        const glm::vec3 look = lookDir();
        const glm::vec3 target = targetPos + glm::vec3(0.0f, m_aimHeight, 0.0f);
        const glm::vec3 eye = target - look * m_distance;
        camera.position = eye;
        camera.viewMatrix = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // Yaw-derived planar forward for camera-relative player movement (valid on any thread/tick —
    // pure function of the stored yaw).
    glm::vec3 forwardPlanar() const
    {
        const float yawR = glm::radians(m_yawDeg);
        return glm::vec3(std::sin(yawR), 0.0f, -std::cos(yawR));
    }

private:
    glm::vec3 lookDir() const // from camera toward the target, tilted down by pitch
    {
        const float pitchR = glm::radians(m_pitchDeg);
        return glm::normalize(forwardPlanar() * std::cos(pitchR) - glm::vec3(0.0f, std::sin(pitchR), 0.0f));
    }

    float m_yawDeg = 0.0f;
    float m_pitchDeg = 55.0f;
    float m_distance = 18.0f;
    float m_minDistance = 6.0f;
    float m_maxDistance = 60.0f;
    float m_zoomSpeed = 2.0f;
    float m_yawSpeed = 120.0f;
    float m_dragSensitivity = 0.25f;
    float m_aimHeight = 1.0f;
};
