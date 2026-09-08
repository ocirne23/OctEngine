export module Core.Frustum;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Camera;

export enum class EFrustumTest : uint8
{
    Outside,
    Intersect,
    Inside,
};

export struct Frustum
{
    glm::vec4 planes[6];
    enum ESide { LEFT = 0, RIGHT = 1, TOP = 2, BOTTOM = 3, BACK = 4, FRONT = 5 };

    Frustum() = default;
    Frustum(const glm::mat4& matrix) { fromMatrix(matrix); }

    void fromMatrix(const glm::mat4& matrix)
    {
        planes[LEFT].x = matrix[0].w + matrix[0].x;
        planes[LEFT].y = matrix[1].w + matrix[1].x;
        planes[LEFT].z = matrix[2].w + matrix[2].x;
        planes[LEFT].w = matrix[3].w + matrix[3].x;

        planes[RIGHT].x = matrix[0].w - matrix[0].x;
        planes[RIGHT].y = matrix[1].w - matrix[1].x;
        planes[RIGHT].z = matrix[2].w - matrix[2].x;
        planes[RIGHT].w = matrix[3].w - matrix[3].x;

        planes[TOP].x = matrix[0].w - matrix[0].y;
        planes[TOP].y = matrix[1].w - matrix[1].y;
        planes[TOP].z = matrix[2].w - matrix[2].y;
        planes[TOP].w = matrix[3].w - matrix[3].y;

        planes[BOTTOM].x = matrix[0].w + matrix[0].y;
        planes[BOTTOM].y = matrix[1].w + matrix[1].y;
        planes[BOTTOM].z = matrix[2].w + matrix[2].y;
        planes[BOTTOM].w = matrix[3].w + matrix[3].y;

        planes[BACK].x = matrix[0].w + matrix[0].z;
        planes[BACK].y = matrix[1].w + matrix[1].z;
        planes[BACK].z = matrix[2].w + matrix[2].z;
        planes[BACK].w = matrix[3].w + matrix[3].z;

        planes[FRONT].x = matrix[0].w - matrix[0].z;
        planes[FRONT].y = matrix[1].w - matrix[1].z;
        planes[FRONT].z = matrix[2].w - matrix[2].z;
        planes[FRONT].w = matrix[3].w - matrix[3].z;

        for (uint32 i = 0; i < 6; ++i)
        {
            float length = sqrtf(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
            planes[i] /= length;
        }
    }

    // Vulkan zero-to-one depth variant: the near plane is row z alone (0 <= z), not w + z.
    // Matches the plane extraction the GPU shadow cull uses on the sun cascade matrices.
    void fromMatrixZO(const glm::mat4& matrix)
    {
        fromMatrix(matrix);
        planes[BACK].x = matrix[0].z;
        planes[BACK].y = matrix[1].z;
        planes[BACK].z = matrix[2].z;
        planes[BACK].w = matrix[3].z;
        const float length = sqrtf(planes[BACK].x * planes[BACK].x + planes[BACK].y * planes[BACK].y + planes[BACK].z * planes[BACK].z);
        planes[BACK] /= length;
    }

    inline bool sphereInFrustum(const glm::vec3& pos, float radius) const
    {
        for (uint32 i = 0; i < 6; ++i)
            if (glm::dot(glm::vec3(planes[i]), pos) + planes[i].w < -radius)
                return false;
        return true;
    }

    inline bool sphereInFrustum(const Sphere& sphere) const
    {
        return sphereInFrustum(sphere.pos, sphere.radius);
    }

    inline bool pointInFrustum(const glm::vec3& pos) const
    {
        for (uint32 i = 0; i < 6; ++i)
            if (glm::dot(glm::vec3(planes[i]), pos) + planes[i].w < 0.0f)
                return false;
        return true;
    }

    inline EFrustumTest testAABB(const glm::vec3& center, const glm::vec3& halfExtent) const
    {
        EFrustumTest result = EFrustumTest::Inside;
        for (uint32 i = 0; i < 6; ++i)
        {
            const glm::vec3 normal = glm::vec3(planes[i]);
            const float r = halfExtent.x * glm::abs(normal.x) + halfExtent.y * glm::abs(normal.y) + halfExtent.z * glm::abs(normal.z);
            const float d = glm::dot(normal, center) + planes[i].w;
            if (d < -r)
                return EFrustumTest::Outside;
            if (d < r)
                result = EFrustumTest::Intersect;
        }
        return result;
    }
};

// A culling-view snapshot handed from the renderer to the spatial index (the two libraries do not
// link each other, so Core carries the type — the Core.VrSession pattern): the culling camera, its
// frustum, and the camera-relative reversed-z view-projection for the CPU occlusion rasterizer.
// valid=false = no view exists yet (the first VR frame — the head pose arrives only inside
// Renderer::beginFrame, so VR culls one frame latent on the previous head view).
export struct CullView
{
    Camera camera;
    Frustum frustum;
    glm::mat4 viewProjRelCamera = glm::mat4(1.0f);
    glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f); // toward the sun; the spatial Shadow pass sweeps the frustum up-sun
    bool valid = false;
};