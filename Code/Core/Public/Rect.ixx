export module Core.Rect;

import Core.glm;

export OC_FWD_ATTACH struct Rect
{
    Rect() {}
    Rect(glm::ivec2 min, glm::ivec2 max) : min(min), max(max) {}

    bool operator==(const Rect& other) const
    {
        return min == other.min && max == other.max;
    }

    glm::ivec2 getSize() const
    {
        return max - min;
    }

    glm::ivec2 min = glm::ivec2(0);
    glm::ivec2 max = glm::ivec2(0);
};