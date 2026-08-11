export module Core.Camera;

import Core.glm;
import Core.Rect;

export struct Ray
{
    glm::vec3 origin;
    glm::vec3 dir; // normalized
};

// Rewrites a glm perspective/frustum projection's z row to REVERSED-Z [0,1] clip (near -> 1, far -> 0).
// glm builds OpenGL [-1,1] matrices here (GLM_FORCE_DEPTH_ZERO_TO_ONE is not set), which Vulkan clips to
// [0,w] — that both near-clipped at ~2x near and wasted half the depth curve. Reversed-Z stores distant
// depths near 0.0, where float32 spacing shrinks exponentially — this cancels the projection's 1/d^2
// compression and gives near-constant relative depth precision at every distance (planet-scale terrain
// stops z-fighting). The whole engine renders this convention: depth compare eGreater, clears/sky at 0.0.
// Idempotent (only the z row is written), x/y rows and the -1 w row pass through untouched.
export inline glm::mat4 reverseZProjection(glm::mat4 proj, float nearZ, float farZ)
{
    proj[2][2] = nearZ / (farZ - nearZ);
    proj[3][2] = nearZ * farZ / (farZ - nearZ);
    return proj;
}

export struct Camera
{
public:

    // Unprojects a viewport-space screen point into a world-space ray through the camera.
    Ray screenToRay(const Rect& viewport, glm::vec2 screenPos) const
    {
        const glm::vec2 size = glm::vec2(viewport.getSize());
        if (size.x <= 0.0f || size.y <= 0.0f)
            return Ray{ position, glm::vec3(0.0f, 0.0f, -1.0f) };

        const float ndcX = (screenPos.x - viewport.min.x) / size.x * 2.0f - 1.0f;
        const float ndcY = 1.0f - (screenPos.y - viewport.min.y) / size.y * 2.0f;
        const float tanHalfFov = glm::tan(glm::radians(fovDeg) * 0.5f);
        const float aspect = size.x / size.y;

        const glm::vec3 viewDir = glm::normalize(glm::vec3(ndcX * tanHalfFov * aspect, ndcY * tanHalfFov, -1.0f));
        const glm::mat4 invView = glm::inverse(viewMatrix);
        const glm::vec3 worldDir = glm::normalize(glm::vec3(invView * glm::vec4(viewDir, 0.0f)));
        return Ray{ position, worldDir };
    }

    // Unprojects a viewport-space screen point into the world: a ray through the camera is intersected
    // with the ground plane (y = 0); if it doesn't hit, the point lands a fixed distance ahead.
    glm::vec3 screenToWorld(const Rect& viewport, glm::vec2 screenPos) const
    {
        const glm::vec2 size = glm::vec2(viewport.getSize());
        if (size.x <= 0.0f || size.y <= 0.0f)
            return position;

        const Ray ray = screenToRay(viewport, screenPos);
        const glm::vec3& worldDir = ray.dir;

        if (glm::abs(worldDir.y) > 1e-4f)
        {
            const float t = -position.y / worldDir.y;
            if (t > 0.0f && t < 1000.0f)
                return position + worldDir * t;
        }
        return position + worldDir * 15.0f;
    }

    // Projects a world point into viewport-space pixels — the inverse of screenToRay (same fov/
    // aspect conventions). Returns false when the point is behind the camera or the viewport is
    // degenerate. For world-anchored UI (labels/bars over entities).
    bool worldToScreen(const Rect& viewport, const glm::vec3& world, glm::vec2& outScreen) const
    {
        const glm::vec2 size = glm::vec2(viewport.getSize());
        if (size.x <= 0.0f || size.y <= 0.0f)
            return false;
        const glm::vec3 view = glm::vec3(viewMatrix * glm::vec4(world, 1.0f));
        if (view.z >= -1e-4f) // view space looks down -Z
            return false;
        const float tanHalfFov = glm::tan(glm::radians(fovDeg) * 0.5f);
        const float aspect = size.x / size.y;
        const float ndcX = view.x / (-view.z * tanHalfFov * aspect);
        const float ndcY = view.y / (-view.z * tanHalfFov);
        outScreen.x = viewport.min.x + (ndcX * 0.5f + 0.5f) * size.x;
        outScreen.y = viewport.min.y + (0.5f - ndcY * 0.5f) * size.y;
        return true;
    }

    glm::mat4 viewMatrix;
    glm::vec3 position;
    // VR only: the play-space yaw. Locomotion turn rotates this; the renderer composes the headset pose
    // on top of it (T(position) * R(playSpaceOrientation) * headPose). Identity on desktop (unused).
    glm::quat playSpaceOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float fovDeg = 45.0f;
    float near = 0.01f;
    float far = 5000.0f;
};