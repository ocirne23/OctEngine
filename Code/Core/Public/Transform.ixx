export module Core.Transform;

import Core.glm;

export struct Transform
{
    Transform() : pos(0, 0, 0), scale(1.0f), quat(1, 0, 0, 0) {}
    Transform(glm::vec3 pos) : pos(pos), scale(1.0f), quat(1, 0, 0, 0) {}
    Transform(const glm::vec3& pos, float scale, const glm::quat& quat) : pos(pos), scale(scale), quat(quat) {}
    glm::vec3 pos;
    float scale;
    glm::quat quat;

    inline glm::vec3 transformPoint(const glm::vec3& p) const { return pos + quat * (p * scale); }

    Transform inverse() const
    {
        const float invScale = 1.0f / scale;
        const glm::quat invQuat = glm::conjugate(quat);
        return Transform(invQuat * (-pos) * invScale, invScale, invQuat);
    }

    inline Transform operator*(const Transform& child) const;
};

export inline Transform composeTransform(const Transform& parent, const Transform& local)
{
    return Transform(
        parent.pos + parent.quat * (local.pos * parent.scale),
        parent.scale * local.scale,
        parent.quat * local.quat);
}

inline Transform Transform::operator*(const Transform& child) const
{
    return composeTransform(*this, child);
}