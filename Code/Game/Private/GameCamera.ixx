export module Game:GameCamera;

import Core;
import Core.glm;
import Core.Camera;
import Settings;

// Angled top-down follow camera: fixed downward pitch, player-rotatable yaw (Q/E held + RMB drag),
// scroll-wheel zoom. Owns no entity state - apply() overwrites the frame camera's position and view
// matrix after the fly camera produced it (the applyPlayerCamera pattern), so everything downstream
// (UI picking, audio listener, renderer, culling) sees the game view. The feel is the "Game/Camera"
// settings (Globals::settings.game.camera); the zoom writes its live distance back into them.
export class GameCamera final
{
public:
    // qeAxis: -1..1 from held Q/E keys. mouseDragDeltaX: pixels of RMB drag this frame.
    // wheelDelta: accumulated scroll (positive = zoom in).
    void apply(Camera& camera, const glm::vec3& targetPos, float deltaSec,
               float qeAxis, float mouseDragDeltaX, float wheelDelta)
    {
        GameCameraSettings& s = Globals::settings.game.camera;
        m_yawDeg += qeAxis * s.yawSpeed * deltaSec + mouseDragDeltaX * s.dragSensitivity;
        s.distance = glm::clamp(s.distance - wheelDelta * s.zoomSpeed, s.minDistance, s.maxDistance);

        const glm::vec3 look = lookDir();
        const glm::vec3 target = targetPos + glm::vec3(0.0f, s.aimHeight, 0.0f);
        const glm::vec3 eye = target - look * s.distance;
        camera.position = eye;
        camera.viewMatrix = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // Point the planar forward along dir (spawn: face the map center). No-op on a ~zero direction.
    void setYawToward(const glm::vec3& dir)
    {
        if (dir.x * dir.x + dir.z * dir.z > 1e-6f)
            m_yawDeg = glm::degrees(std::atan2(dir.x, -dir.z));
    }

    // Yaw-derived planar forward for camera-relative player movement (valid on any thread/tick -
    // pure function of the stored yaw).
    glm::vec3 forwardPlanar() const
    {
        const float yawR = glm::radians(m_yawDeg);
        return glm::vec3(std::sin(yawR), 0.0f, -std::cos(yawR));
    }

private:
    glm::vec3 lookDir() const // from camera toward the target, tilted down by pitch
    {
        const float pitchR = glm::radians(Globals::settings.game.camera.pitchDeg);
        return glm::normalize(forwardPlanar() * std::cos(pitchR) - glm::vec3(0.0f, std::sin(pitchR), 0.0f));
    }

    float m_yawDeg = 0.0f;
};
