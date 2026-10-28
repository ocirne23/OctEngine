export module RendererVK:Light;

import Core;
import Core.glm;

import :Layout;

export struct PointLight : RendererVKLayout::LightInfo
{
    PointLight() = default;
    PointLight(const glm::vec3& pos, float range, const glm::vec3& color, float intensity)
    {
        this->pos = pos;
        this->radius = range;
        this->color = color * intensity;
        this->width = 0.0f; // marks this as a point light
        this->direction = glm::vec3(0.0f);
        this->rotation = 0.0f;
    }
};

export struct AreaLight : RendererVKLayout::LightInfo
{
    AreaLight() = default;
    AreaLight(const glm::vec3& pos, float range, const glm::vec3& color, float intensity, const glm::vec3& direction, float width, float height, float rotation = 0.0f)
    {
        this->pos = pos;
        this->radius = range;
        this->color = color * intensity;
        this->width = width;
        this->direction = glm::normalize(direction) * height; // height encoded in the direction's magnitude
        this->rotation = rotation;
    }
};

export struct TubeLight : RendererVKLayout::LightInfo
{
    TubeLight() = default;
    TubeLight(const glm::vec3& pos, float lightRange, const glm::vec3& color, float intensity, const glm::vec3& direction, float tubeRadius, float length, float rotation = 0.0f)
    {
        this->pos = pos;
        this->radius = -lightRange; // negative marks this as a tube light
        this->color = color * intensity;
        this->width = tubeRadius;
        this->direction = glm::normalize(direction) * length; // full length encoded in the direction's magnitude; doTubeLight halves it for the half-extent
        this->rotation = rotation;
    }
};

export struct SpotLight : RendererVKLayout::LightInfo
{
    SpotLight() = default;
    SpotLight(const glm::vec3& pos, float range, const glm::vec3& color, float intensity, const glm::vec3& direction, float coneHalfAngleRad, float edgeSoftness = 0.1f)
    {
        this->pos = pos;
        this->radius = range;
        this->color = color * intensity;
        this->width = -1.0f; // negative marks this as a spot light
        this->direction = glm::normalize(direction) * glm::max(edgeSoftness, 0.001f); // cone axis; length encodes edge softness
        this->rotation = coneHalfAngleRad;
    }
};