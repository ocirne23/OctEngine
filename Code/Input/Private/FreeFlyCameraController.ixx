export module Input:FreeFlyCameraController;

import Core;
import Core.glm;
import Core.Camera;

import Input.fwd;
import :Input; // MouseListenerHandle

export class FreeFlyCameraController final
{
public:

    FreeFlyCameraController() {}
    FreeFlyCameraController(const FreeFlyCameraController&) = delete;

    void initialize(glm::vec3 position, glm::vec3 lookAt, glm::vec3 up = glm::vec3(0, 1, 0));
    void update(double deltaTime);
    void setSpeed(float speed) { m_speed = speed; }
    void setSensitivity(float sensitivity) { m_sensitivity = sensitivity; }
    void setLockToWorldUp(bool lock) { m_lockToWorldUp = lock; }
    // Off = mouse-look only: the WASD/Space/Ctrl fly keys are released to whoever borrowed them
    // (player control routes them to the owned entity while active - see InputControls key C)
    void setMovementEnabled(bool enabled) { m_movementEnabled = enabled; }
    // Hard reposition (view matrix refreshes on the next update): player control hands the camera
    // back where the possessed capsule left it instead of popping to the pre-possession spot
    void setPosition(const glm::vec3& position) { m_position = position; }
    // Hard reposition PLUS re-aim (world-up-locked basis): the game's detached camera starts the
    // flight from the follow camera's exact view instead of the testbed pose. No re-registration
    // of tweaks or listeners - initialize() stays a once-per-run call.
    void setPose(const glm::vec3& position, const glm::vec3& lookAt);

    glm::vec3 getPosition() const { return m_position; }
    glm::vec3 getDirection() const { return m_direction; }
    glm::vec3 getUp() const { return m_up; }
    glm::mat4 getViewMatrix() const { return m_viewMatrix; }
	glm::quat getOrientation() const { return glm::quat_cast(glm::inverse(m_viewMatrix)); }

    Camera getCamera()
    {
        Camera camera;
        camera.position = m_position;
        camera.viewMatrix = m_viewMatrix;
        camera.near = m_near;
        camera.far = m_far;
        return camera;
    }

private:

    MouseListenerHandle m_mouseListener; // unregisters itself on destruction

    bool m_isMouseDown = false;
    bool m_mousePosUpdated = false;
    bool m_lockToWorldUp = true;
    bool m_movementEnabled = true;
    float m_boostMultiplier = 10.0f;
    float m_speed = 5.0f;
    float m_sensitivity = 0.01f;
    float m_maxLookDelta = 150.0f; // pixels between two mouse events; beyond this we missed some
    float m_near = 0.05f;
    float m_far = 42000.0f;
    glm::vec2 m_lastMousePos;
    glm::vec3 m_position;
    glm::vec3 m_direction;
    glm::vec3 m_up;
    glm::vec3 m_worldUp = glm::vec3(0, 1, 0);
    glm::mat4 m_viewMatrix;
};
