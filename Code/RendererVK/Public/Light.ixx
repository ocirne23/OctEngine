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

// A BEAM: a straight line of light of any length (a lamp tube, or a column kilometres tall). `pos` is its CENTRE.
// It lights surfaces within `lightRange` of the line, with ONE shadow ray at a fixed point of the line (no jitter, so
// no noise), and the fog apply draws its glow in the air in closed form (the first RendererState::MAX_BEAM_COLUMNS
// beams of a frame, no shadow rays). The fog froxels, GI and particles see it as a point at the centre.
export struct BeamLight : RendererVKLayout::LightInfo
{
    BeamLight() = default;
    BeamLight(const glm::vec3& pos, float lightRange, const glm::vec3& color, float intensity, const glm::vec3& direction, float beamRadius, float length)
    {
        this->pos = pos;
        this->radius = -lightRange; // negative marks this as a beam light
        this->color = color * intensity;
        this->width = beamRadius;
        this->direction = glm::normalize(direction) * length; // full length encoded in the direction's magnitude; doBeamLight halves it for the half-extent
        this->rotation = 0.0f;
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