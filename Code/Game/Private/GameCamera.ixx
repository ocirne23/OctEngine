export module Game:GameCamera;

import Core;
import Core.glm;
import Core.Camera;
import Settings;
import Physics; // the third-person wall pull-in

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

    // THIRD PERSON (co-op in the sandbox world): an orbit around the body's eye point - the mouse turns it (lookDelta =
    // relative mouse counts this frame), the wheel zooms, and a ray from the pivot pulls the eye in front of walls and
    // terrain (ignoring the player's own body). The crosshair is the view center, so the aim ray is the camera's
    // forward. Shares the yaw with the top-down mode, so forwardPlanar() steers the body either way.
    void applyThirdPerson(Camera& camera, const glm::vec3& targetPos, const glm::vec2& lookDelta, float wheelDelta,
        const PhysicsBody* ignoreBody)
    {
        GameCameraSettings& s = Globals::settings.game.camera;
        m_yawDeg += glm::degrees(lookDelta.x * s.thirdSensitivity);
        m_pitchDeg = glm::clamp(m_pitchDeg - glm::degrees(lookDelta.y * s.thirdSensitivity), -80.0f, 70.0f);
        if (wheelDelta != 0.0f)
            s.thirdDistance = glm::clamp(s.thirdDistance * (wheelDelta > 0.0f ? 0.9f : 1.1f),
                s.thirdMinDistance, glm::max(s.thirdMaxDistance, s.thirdMinDistance));
        const float pitchR = glm::radians(m_pitchDeg);
        const glm::vec3 dir = glm::normalize(forwardPlanar() * std::cos(pitchR) + glm::vec3(0.0f, std::sin(pitchR), 0.0f));
        const glm::vec3 pivot = targetPos + glm::vec3(0.0f, s.thirdEyeHeight, 0.0f);
        const glm::vec3 back = -dir * s.thirdDistance;
        const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(pivot, back, PhysicsLayers::All, ignoreBody);
        glm::vec3 eye = pivot + (hit.hit ? back * glm::max(hit.fraction - 0.1f, 0.0f) : back);
        // THE MIN HEIGHT over the ground ("Third person min height"): a ray down from just over the eye; the eye lifts to
        // that far over what it hits (the terrain, or a roof it stands on), so a low orbit does not skim the ground.
        if (s.thirdMinHeight > 0.0f)
        {
            constexpr float c_probeUp = 1.5f;
            const glm::vec3 down(0.0f, -(c_probeUp + s.thirdMinHeight), 0.0f);
            const PhysicsWorld::RayHit ground = Globals::physics.castRayClosest(eye + glm::vec3(0.0f, c_probeUp, 0.0f), down, PhysicsLayers::All, ignoreBody);
            if (ground.hit)
                eye.y = glm::max(eye.y, eye.y + c_probeUp + down.y * ground.fraction + s.thirdMinHeight);
        }
        camera.position = eye;
        camera.viewMatrix = glm::lookAt(eye, eye + dir, glm::vec3(0.0f, 1.0f, 0.0f));
        // The sandbox's clip planes: the view reaches the horizon like the fly camera's (the top-down camera keeps
        // whatever the frame camera held).
        camera.near = Globals::settings.freeFlyCamera.cameraNear;
        camera.far = Globals::settings.freeFlyCamera.cameraFar;
    }

    // Third person: start looking along dir (yaw AND pitch) - the sandbox camera's start direction.
    void setLookDir(const glm::vec3& dir)
    {
        setYawToward(dir);
        const float len = glm::length(dir);
        if (len > 1e-4f)
            m_pitchDeg = glm::clamp(glm::degrees(std::asin(glm::clamp(dir.y / len, -1.0f, 1.0f))), -80.0f, 70.0f);
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
    float m_pitchDeg = -15.0f; // third person only (+ = looking up)
};
