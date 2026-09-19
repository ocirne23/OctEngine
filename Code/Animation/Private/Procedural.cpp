module Animation;

import Core;
import Core.glm;

namespace
{
    constexpr float TwoPi = 6.28318530718f;

    template <typename KeyT, typename ValueT>
    void insertKey(oc::vector<KeyT>& keys, float time, const ValueT& value)
    {
        auto it = keys.begin();
        while (it != keys.end() && it->time <= time) ++it;
        keys.insert(it, KeyT{ time, value });
    }

    glm::quat bindRotation(const glm::mat4& m)
    {
        const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
        const float sx = glm::length(c0), sy = glm::length(c1), sz = glm::length(c2);
        const glm::mat3 r(c0 / (sx > 1e-8f ? sx : 1.0f), c1 / (sy > 1e-8f ? sy : 1.0f), c2 / (sz > 1e-8f ? sz : 1.0f));
        return glm::normalize(glm::quat_cast(r));
    }

    uint32 sineKeyCount(float cycles, uint32 keysPerCycle)
    {
        return glm::max(uint32(std::ceil(glm::max(cycles, 0.0f) * float(glm::max(keysPerCycle, 4u)))), 2u);
    }
}

ClipBuilder::ClipBuilder(const Skeleton& skeleton, const oc::string& name, float durationSeconds, bool loop)
    : m_skeleton(skeleton)
{
    m_clip.name = name;
    m_clip.duration = glm::max(durationSeconds, 1e-3f);
    m_clip.loop = loop;
}

AnimationChannel* ClipBuilder::channelFor(const oc::string& bone)
{
    const int32 idx = m_skeleton.findBone(bone);
    if (idx < 0)
        return nullptr;
    for (AnimationChannel& ch : m_clip.channels)
        if (ch.boneIndex == idx)
            return &ch;
    AnimationChannel& ch = m_clip.channels.emplace_back();
    ch.boneIndex = idx;
    return &ch;
}

ClipBuilder& ClipBuilder::position(const oc::string& bone, float time, const glm::vec3& value)
{
    if (AnimationChannel* ch = channelFor(bone))
        insertKey(ch->positionKeys, time, value);
    return *this;
}

ClipBuilder& ClipBuilder::rotation(const oc::string& bone, float time, const glm::quat& value)
{
    if (AnimationChannel* ch = channelFor(bone))
        insertKey(ch->rotationKeys, time, value);
    return *this;
}

ClipBuilder& ClipBuilder::scale(const oc::string& bone, float time, const glm::vec3& value)
{
    if (AnimationChannel* ch = channelFor(bone))
        insertKey(ch->scaleKeys, time, value);
    return *this;
}

ClipBuilder& ClipBuilder::swing(const oc::string& bone, const glm::vec3& axis, float angleRadians, float phase, float cycles, uint32 keysPerCycle)
{
    AnimationChannel* ch = channelFor(bone);
    if (!ch || glm::dot(axis, axis) < 1e-12f)
        return *this;

    const glm::vec3 n = glm::normalize(axis);
    const glm::quat bind = bindRotation(m_skeleton.localBind[ch->boneIndex]);
    const uint32 numKeys = sineKeyCount(cycles, keysPerCycle);
    ch->rotationKeys.clear();
    ch->rotationKeys.reserve(numKeys + 1);
    for (uint32 i = 0; i <= numKeys; ++i)
    {
        const float u = float(i) / float(numKeys);
        const float angle = angleRadians * std::sin(TwoPi * (cycles * u + phase));
        ch->rotationKeys.push_back({ u * m_clip.duration, bind * glm::angleAxis(angle, n) });
    }
    return *this;
}

ClipBuilder& ClipBuilder::bob(const oc::string& bone, const glm::vec3& offset, float phase, float cycles, uint32 keysPerCycle)
{
    AnimationChannel* ch = channelFor(bone);
    if (!ch)
        return *this;

    const glm::vec3 bind(m_skeleton.localBind[ch->boneIndex][3]);
    const uint32 numKeys = sineKeyCount(cycles, keysPerCycle);
    ch->positionKeys.clear();
    ch->positionKeys.reserve(numKeys + 1);
    for (uint32 i = 0; i <= numKeys; ++i)
    {
        const float u = float(i) / float(numKeys);
        ch->positionKeys.push_back({ u * m_clip.duration, bind + offset * std::sin(TwoPi * (cycles * u + phase)) });
    }
    return *this;
}

ClipBuilder& ClipBuilder::event(const oc::string& name, float normalizedTime)
{
    m_clip.events.push_back({ name, glm::clamp(normalizedTime, 0.0f, 1.0f) });
    return *this;
}

AnimationClip buildProceduralClip(const Skeleton& skeleton, const oc::string& name, const ProceduralClipDesc& desc, bool loop)
{
    ClipBuilder builder(skeleton, name, desc.duration, loop);
    for (const ProceduralSwing& s : desc.swings)
        builder.swing(s.bone, s.axis, s.angle, s.phase, s.cycles);
    for (const ProceduralBob& b : desc.bobs)
        builder.bob(b.bone, b.offset, b.phase, b.cycles);
    return builder.build();
}

ProceduralClipDesc makeWalkCycleDesc(const WalkCycleParams& params)
{
    ProceduralClipDesc desc;
    desc.duration = params.duration;
    auto limb = [&](const oc::string& bone, float angle, float phase)
    {
        if (!bone.empty())
            desc.swings.push_back({ bone, params.axis, angle, phase, 1.0f });
    };
    limb(params.leftLeg,  params.legAngle, 0.0f);
    limb(params.rightLeg, params.legAngle, 0.5f);
    limb(params.leftArm,  params.armAngle, 0.5f);
    limb(params.rightArm, params.armAngle, 0.0f);
    if (!params.bobBone.empty() && params.bobHeight != 0.0f)
        desc.bobs.push_back({ params.bobBone, glm::vec3(0.0f, params.bobHeight, 0.0f), 0.0f, 2.0f });
    return desc;
}
