export module Core.AABB;

import Core;
import Core.glm;

export OC_FWD_ATTACH struct AABB final
{
    glm::vec3 min;
    glm::vec3 max;

    AABB() {}
    AABB(glm::vec3 min, glm::vec3 max) : min(min), max(max) {}

    float getRadius() const
    {
        return glm::length(max - min) * 0.5f;
    }

    glm::vec3 getCenter() const     
    {
        return (min + max) * 0.5f;
    }
};