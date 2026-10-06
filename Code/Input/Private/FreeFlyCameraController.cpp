module Input;

import Core;
import Core.glm;
import Core.Camera;
import Core.SDL;
import Settings;
import UI;

import :Input;
import :FreeFlyCameraController;

namespace
{
    // Camera right for a world-up-locked basis. Degenerates when looking straight along the pole,
    // where any right is valid: keep the one the current basis already implies.
    glm::vec3 worldLockedRight(glm::vec3 direction, glm::vec3 worldUp, glm::vec3 currentUp)
    {
        const glm::vec3 right = glm::cross(direction, worldUp);
        return glm::normalize(glm::dot(right, right) > 1e-6f ? right : glm::cross(direction, currentUp));
    }
}

void FreeFlyCameraController::initialize(glm::vec3 position, glm::vec3 lookAt, glm::vec3 up)
{
    m_position = position;
    m_direction = glm::normalize(lookAt - position);
    m_worldUp = glm::normalize(up);
    m_up = glm::normalize(glm::cross(worldLockedRight(m_direction, m_worldUp, m_worldUp), m_direction));
    m_viewMatrix = glm::lookAt(m_position, m_position + m_direction, m_up);

    m_mouseListener = Globals::input.addMouseListener();
    m_mouseListener->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button == 1)
            {
                m_isMouseDown = true;
                m_mousePosUpdated = false;
            }
        };
    m_mouseListener->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
        {
            if (evt.button == 1)
            {
                m_isMouseDown = false;
                m_mousePosUpdated = false;
            }
        };
    m_mouseListener->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
        {
            if (!Globals::input.isWindowHasFocus() || !m_isMouseDown || !Globals::ui.isViewportFocused() || Globals::input.isMouseCaptured())
            {
                m_mousePosUpdated = false;
                return;
            }
            if (!m_mousePosUpdated || Globals::ui.hasViewportGainedFocused())
            {
                m_lastMousePos = glm::vec2(evt.x, evt.y);
                m_mousePosUpdated = true;
            }
            glm::vec2 currentPos = glm::vec2(evt.x, evt.y);
            glm::vec2 delta = currentPos - m_lastMousePos;
            m_lastMousePos = currentPos;

            // Windows coalesces WM_MOUSEMOVE, so a frame hitch collapses the whole move into one
            // event; missed events around focus changes do the same. A real mouse never travels
            // this far between two events, so drop it and resume from the new position.
            if (glm::dot(delta, delta) > m_maxLookDelta * m_maxLookDelta)
                return;

            float yaw = -delta.x * m_settings.sensitivity;
            float pitch = -delta.y * m_settings.sensitivity;

            if (m_settings.lockToWorldUp)
            {
                // Clamped so the yaw axis stays well conditioned at the pole. update() derives m_up.
                constexpr float c_maxPitch = 1.55334303f; // 89 degrees
                const float currentPitch = std::asin(glm::clamp(glm::dot(m_direction, m_worldUp), -1.0f, 1.0f));
                pitch = glm::clamp(currentPitch + pitch, -c_maxPitch, c_maxPitch) - currentPitch;

                glm::mat4 rot = glm::rotate(glm::mat4(1.0f), yaw, m_worldUp);
                rot = glm::rotate(rot, pitch, worldLockedRight(m_direction, m_worldUp, m_up));
                m_direction = glm::normalize(glm::vec3(rot * glm::vec4(m_direction, 0.0f)));
            }
            else
            {
                const glm::vec3 right = glm::normalize(glm::cross(m_direction, m_up));
                glm::mat4 rot = glm::rotate(glm::mat4(1.0f), yaw, m_up);
                rot = glm::rotate(rot, pitch, right);
                m_direction = glm::normalize(glm::vec3(rot * glm::vec4(m_direction, 0.0f)));
                m_up = glm::normalize(glm::cross(right, m_direction));
            }
        };
    m_mouseListener->onMouseWheelMoved = [this](const SDL_MouseWheelEvent& evt)
        {
            m_settings.speed = oc::clamp(m_settings.speed * (evt.y > 0.0f ? 1.1f : 0.9f), 0.1f, 400.0f);
        };
}

void FreeFlyCameraController::setPose(const glm::vec3& position, const glm::vec3& lookAt)
{
    m_position = position;
    const glm::vec3 toTarget = lookAt - position;
    if (glm::dot(toTarget, toTarget) > 1e-8f)
        m_direction = glm::normalize(toTarget);
    m_up = glm::normalize(glm::cross(worldLockedRight(m_direction, m_worldUp, m_up), m_direction));
    m_viewMatrix = glm::lookAt(m_position, m_position + m_direction, m_up);
}

void FreeFlyCameraController::update(double deltaTime)
{
    const float deltaSec = static_cast<float>(deltaTime);
    Input& input = Globals::input;
    const bool viewportActive = input.isWindowHasFocus() && Globals::ui.isViewportFocused();

    // Input::update drops mouse events while the viewport is unfocused, so the button release that
    // ends a drag can go missing entirely. End it from the frame state, which always runs.
    if (!viewportActive)
    {
        m_isMouseDown = false;
        m_mousePosUpdated = false;
    }

    if (viewportActive && m_movementEnabled)
    {
        const float boost = input.isKeyDown(SDL_SCANCODE_LSHIFT) ? m_boostMultiplier : 1.0f;
        const float step = m_settings.speed * deltaSec * boost;

        if (input.isKeyDown(SDL_SCANCODE_W))
            m_position += m_direction * step;
        if (input.isKeyDown(SDL_SCANCODE_S))
            m_position -= m_direction * step;
        if (input.isKeyDown(SDL_SCANCODE_A))
            m_position -= glm::normalize(glm::cross(m_direction, m_up)) * step;
        if (input.isKeyDown(SDL_SCANCODE_D))
            m_position += glm::normalize(glm::cross(m_direction, m_up)) * step;
        if (input.isKeyDown(SDL_SCANCODE_SPACE))
            m_position += m_up * step;
        if (input.isKeyDown(SDL_SCANCODE_LCTRL))
            m_position -= m_up * step;
        if (!m_settings.lockToWorldUp)
        {
            if (input.isKeyDown(SDL_SCANCODE_Q))
                m_up = glm::normalize(glm::vec3(glm::rotate(glm::mat4(1.0f), -1.0f * deltaSec, m_direction) * glm::vec4(m_up, 0.0f)));
            if (input.isKeyDown(SDL_SCANCODE_E))
                m_up = glm::normalize(glm::vec3(glm::rotate(glm::mat4(1.0f), 1.0f * deltaSec, m_direction) * glm::vec4(m_up, 0.0f)));
        }
    }

    // Rebuilding from the world up every frame is what keeps roll out; it also levels out whatever
    // roll free mode left behind when the lock is toggled back on.
    if (m_settings.lockToWorldUp)
        m_up = glm::normalize(glm::cross(worldLockedRight(m_direction, m_worldUp, m_up), m_direction));

    m_viewMatrix = glm::lookAt(m_position, m_position + m_direction, m_up);
}
