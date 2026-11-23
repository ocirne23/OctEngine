export module Core.Sphere;

import Core;
import Core.glm;

export OC_FWD_ATTACH struct Sphere
{
    glm::vec3 pos = glm::vec3(FLT_NAN);
    float radius = 0.0f;

    void combineSphere(Sphere& other)
    {
        glm::vec3 centerOffset = other.pos - pos;
        float distance = glm::length(centerOffset);
        if (distance + other.radius <= radius)
            return; // sphere is already inside this sphere
        if (distance + radius <= other.radius)
        {
            pos = other.pos;
            radius = other.radius;
            return; // this sphere is inside the other sphere
        }
        float newRadius = (radius + distance + other.radius) * 0.5f;
        float k = (newRadius - radius) / distance;
        radius = newRadius;
        pos += centerOffset * k;
    }
};