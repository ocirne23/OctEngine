module Animation;

import Core;
import Core.glm;

namespace
{
    template <typename KeyT>
    uint32 findKey(const oc::vector<KeyT>& keys, float t, float& outFactor)
    {
        for (uint32 i = 0; i + 1 < (uint32)keys.size(); ++i)
        {
            if (t < keys[i + 1].time)
            {
                const float span = keys[i + 1].time - keys[i].time;
                outFactor = span > 1e-8f ? (t - keys[i].time) / span : 0.0f;
                return i;
            }
        }
        outFactor = 0.0f;
        return (uint32)keys.size() - 1;
    }

    glm::vec3 sampleVec(const oc::vector<PositionKey>& keys, float t, const glm::vec3& fallback)
    {
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return keys[0].value;
        float f;
        const uint32 i = findKey(keys, t, f);
        if (i + 1 >= (uint32)keys.size()) return keys.back().value;
        return glm::mix(keys[i].value, keys[i + 1].value, f);
    }

    glm::vec3 sampleScale(const oc::vector<ScaleKey>& keys, float t, const glm::vec3& fallback)
    {
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return keys[0].value;
        float f;
        const uint32 i = findKey(keys, t, f);
        if (i + 1 >= (uint32)keys.size()) return keys.back().value;
        return glm::mix(keys[i].value, keys[i + 1].value, f);
    }

    glm::quat sampleQuat(const oc::vector<RotationKey>& keys, float t, const glm::quat& fallback)
    {
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return keys[0].value;
        float f;
        const uint32 i = findKey(keys, t, f);
        if (i + 1 >= (uint32)keys.size()) return keys.back().value;
        return glm::normalize(glm::slerp(keys[i].value, keys[i + 1].value, f));
    }

    // Decomposes a TRS-composed local matrix (translate * rotate * scale) into its components. Bind-pose
    // node transforms are well-formed TRS, so this avoids glm's experimental matrix_decompose.
    void decomposeTRS(const glm::mat4& m, glm::vec3& pos, glm::quat& rot, glm::vec3& scale)
    {
        pos = glm::vec3(m[3]);
        const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
        scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
        const glm::mat3 r(c0 / (scale.x > 1e-8f ? scale.x : 1.0f),
                          c1 / (scale.y > 1e-8f ? scale.y : 1.0f),
                          c2 / (scale.z > 1e-8f ? scale.z : 1.0f));
        rot = glm::normalize(glm::quat_cast(r));
    }

    float wrap(float t, float duration)
    {
        return duration > 0.0f ? t - duration * std::floor(t / duration) : 0.0f;
    }
}

void AnimationPlayer::initialize(const Skeleton* pSkeleton, const AnimationClip* pClip)
{
    m_pSkeleton = pSkeleton;
    const uint32 numBones = pSkeleton ? pSkeleton->numBones() : 0;

    m_bind.resize(numBones);
    for (uint32 i = 0; i < numBones; ++i)
        decomposeTRS(pSkeleton->localBind[i], m_bind.pos[i], m_bind.rot[i], m_bind.scale[i]);

    m_poseA.resize(numBones);
    m_poseB.resize(numBones);
    m_snapshot.resize(numBones);
    m_globalTransforms.assign(numBones, glm::mat4(1.0f));
    m_palette.assign(numBones, glm::mat4(1.0f));
    m_boneModifiers.assign(numBones, BoneModifier{});
    m_anyBoneModifier = false;

    m_pClip = nullptr;
    m_pBlendSpace = nullptr;
    m_blendSpaceActive = false;
    m_fade = 1.0f;
    play(pClip, 0.0f);
}

void AnimationPlayer::beginCrossfade(float fadeSeconds)
{
    if (fadeSeconds > 0.0f)
    {
        m_snapshot = m_poseA; // freeze the outgoing pose; the new source blends in over the fade
        m_fade = 0.0f;
        m_fadeDuration = fadeSeconds;
    }
    else
    {
        m_fade = 1.0f;
        m_fadeDuration = 0.0f;
    }
}

void AnimationPlayer::play(const AnimationClip* pClip, float fadeSeconds)
{
    if (!m_blendSpaceActive && pClip == m_pClip && m_fade >= 1.0f)
        return;
    beginCrossfade(fadeSeconds);
    m_pClip = pClip;
    m_blendSpaceActive = false;
    m_time = 0.0f;
    evaluate();
}

bool AnimationPlayer::play(const oc::string& name, float fadeSeconds)
{
    if (!m_pLibrary)
        return false;
    const AnimationClip* pClip = m_pLibrary->find(name);
    if (!pClip)
        return false;
    play(pClip, fadeSeconds);
    return true;
}

void AnimationPlayer::playBlendSpace(const BlendSpace1D* pBlendSpace, float fadeSeconds)
{
    beginCrossfade(fadeSeconds);
    m_pBlendSpace = pBlendSpace;
    m_blendSpaceActive = true;
    m_phase = 0.0f;
    evaluate();
}

AnimationPlayer::BlendResult AnimationPlayer::resolveBlend() const
{
    BlendResult r;
    if (!m_blendSpaceActive || !m_pBlendSpace || m_pBlendSpace->samples.empty())
    {
        r.a = m_pClip;
        return r;
    }
    const oc::vector<BlendSample1D>& s = m_pBlendSpace->samples;
    if (s.size() == 1 || m_blendParam <= s.front().position) { r.a = s.front().clip; return r; }
    if (m_blendParam >= s.back().position)                   { r.a = s.back().clip;  return r; }
    uint32 i = 0;
    while (i + 1 < (uint32)s.size() && m_blendParam >= s[i + 1].position) ++i;
    r.a = s[i].clip;
    r.b = s[i + 1].clip;
    const float span = s[i + 1].position - s[i].position;
    r.w = span > 1e-8f ? (m_blendParam - s[i].position) / span : 0.0f;
    return r;
}

void AnimationPlayer::sampleClip(const AnimationClip* pClip, float time, Pose& outPose) const
{
    outPose = m_bind; // bones a clip doesn't animate fall back to the bind pose
    if (!pClip)
        return;
    const uint32 numBones = (uint32)m_bind.pos.size();
    for (const AnimationChannel& ch : pClip->channels)
    {
        if (ch.boneIndex < 0 || (uint32)ch.boneIndex >= numBones)
            continue;
        outPose.pos[ch.boneIndex]   = sampleVec(ch.positionKeys, time, m_bind.pos[ch.boneIndex]);
        outPose.rot[ch.boneIndex]   = sampleQuat(ch.rotationKeys, time, m_bind.rot[ch.boneIndex]);
        outPose.scale[ch.boneIndex] = sampleScale(ch.scaleKeys, time, m_bind.scale[ch.boneIndex]);
    }
}

void AnimationPlayer::blendPose(Pose& inOutA, const Pose& b, float w) const
{
    const uint32 numBones = (uint32)inOutA.pos.size();
    for (uint32 i = 0; i < numBones; ++i)
    {
        inOutA.pos[i]   = glm::mix(inOutA.pos[i], b.pos[i], w);
        inOutA.rot[i]   = glm::normalize(glm::slerp(inOutA.rot[i], b.rot[i], w));
        inOutA.scale[i] = glm::mix(inOutA.scale[i], b.scale[i], w);
    }
}

void AnimationPlayer::sampleForeground(Pose& outPose)
{
    const BlendResult br = resolveBlend();
    const float dA = br.a ? br.a->duration : 0.0f;
    if (m_blendSpaceActive && br.b)
    {
        sampleClip(br.a, m_phase * dA, outPose);
        sampleClip(br.b, m_phase * br.b->duration, m_poseB);
        blendPose(outPose, m_poseB, br.w);
    }
    else if (m_blendSpaceActive)
    {
        sampleClip(br.a, m_phase * dA, outPose);
    }
    else
    {
        sampleClip(m_pClip, m_time, outPose);
    }
}

void AnimationPlayer::detectEvents(const AnimationClip* pClip, float prevNorm, float curNorm)
{
    if (!pClip || pClip->events.empty())
        return;
    const bool wrapped = curNorm < prevNorm; // playback looped past 1.0 this tick
    for (const AnimationEventKey& e : pClip->events)
    {
        const float t = e.normalizedTime;
        const bool fired = wrapped ? (t > prevNorm || t <= curNorm)  // (prev,1] ∪ [0,cur]
                                   : (t > prevNorm && t <= curNorm);
        if (fired)
            m_firedEvents.push_back(e.name);
    }
}

void AnimationPlayer::tick(float deltaSeconds)
{
    m_firedEvents.clear();
    if (!m_paused)
    {
        const float dt = deltaSeconds * m_speed;
        if (m_blendSpaceActive)
        {
            const BlendResult br = resolveBlend();
            const float duration = (br.b ? glm::mix(br.a->duration, br.b->duration, br.w) : (br.a ? br.a->duration : 0.0f));
            const float prevNorm = m_phase;
            if (duration > 1e-6f)
                m_phase = wrap(m_phase + dt / duration, 1.0f); // phase-normalized so blended clips stay synced
            // Events come from the dominant bracketing clip (the blend clips share the 0..1 phase).
            detectEvents((br.b && br.w >= 0.5f) ? br.b : br.a, prevNorm, m_phase);
        }
        else if (m_pClip && m_pClip->duration > 0.0f)
        {
            const float dur = m_pClip->duration;
            const float prevNorm = m_time / dur;
            if (m_pClip->loop)
                m_time = wrap(m_time + dt, dur);
            else
                m_time = glm::clamp(m_time + dt, 0.0f, dur); // one-shot: hold the final frame
            detectEvents(m_pClip, prevNorm, m_time / dur);
        }

        if (m_fade < 1.0f)
        {
            // The crossfade advances in real time, independent of playback speed.
            m_fade += m_fadeDuration > 0.0f ? deltaSeconds / m_fadeDuration : 1.0f;
            if (m_fade >= 1.0f)
                m_fade = 1.0f;
        }
    }
    evaluate();
}

void AnimationPlayer::evaluate()
{
    if (!m_pSkeleton)
        return;

    const Skeleton& skel = *m_pSkeleton;
    const uint32 numBones = skel.numBones();

    sampleForeground(m_poseA);
    if (m_fade < 1.0f)
        blendPose(m_poseA, m_snapshot, 1.0f - m_fade); // = lerp(snapshot, foreground, m_fade)

    // Build local matrices from the (blended) TRS, applying any programmatic per-bone modifiers.
    oc::vector<glm::mat4> local(numBones);
    for (uint32 i = 0; i < numBones; ++i)
    {
        local[i] = glm::translate(glm::mat4(1.0f), m_poseA.pos[i]) * glm::mat4_cast(m_poseA.rot[i]) * glm::scale(glm::mat4(1.0f), m_poseA.scale[i]);
        if (m_anyBoneModifier)
        {
            const BoneModifier& mod = m_boneModifiers[i];
            if (mod.mode == BoneModifierMode::Override)
                local[i] = mod.transform;
            else if (mod.mode == BoneModifierMode::Additive)
                local[i] = local[i] * mod.transform;
        }
    }

    // Compose global transforms (parent-before-child order guarantees the parent is already done), then
    // build the skinning palette.
    for (uint32 i = 0; i < numBones; ++i)
    {
        const int32 parent = skel.parentIndices[i];
        m_globalTransforms[i] = parent < 0 ? local[i] : m_globalTransforms[parent] * local[i];
        m_palette[i] = m_globalTransforms[i] * skel.inverseBind[i];
    }
}

float AnimationPlayer::getNormalizedTime() const
{
    if (m_blendSpaceActive)
        return m_phase;
    if (m_pClip && m_pClip->duration > 0.0f)
        return m_time / m_pClip->duration;
    return 0.0f;
}

int32 AnimationPlayer::findBone(const oc::string& name) const
{
    return m_pSkeleton ? m_pSkeleton->findBone(name) : -1;
}

void AnimationPlayer::setBoneTransform(uint32 boneIndex, const glm::mat4& localTransform)
{
    if (boneIndex >= m_boneModifiers.size())
        return;
    m_boneModifiers[boneIndex] = BoneModifier{ localTransform, BoneModifierMode::Override };
    m_anyBoneModifier = true;
}

void AnimationPlayer::setBoneTransform(const oc::string& name, const glm::mat4& localTransform)
{
    const int32 idx = findBone(name);
    if (idx >= 0)
        setBoneTransform((uint32)idx, localTransform);
}

void AnimationPlayer::setBoneTransform(const oc::string& name, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale)
{
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), scale);
    setBoneTransform(name, m);
}

void AnimationPlayer::setBoneOffset(uint32 boneIndex, const glm::mat4& localOffset)
{
    if (boneIndex >= m_boneModifiers.size())
        return;
    m_boneModifiers[boneIndex] = BoneModifier{ localOffset, BoneModifierMode::Additive };
    m_anyBoneModifier = true;
}

void AnimationPlayer::setBoneOffset(const oc::string& name, const glm::mat4& localOffset)
{
    const int32 idx = findBone(name);
    if (idx >= 0)
        setBoneOffset((uint32)idx, localOffset);
}

void AnimationPlayer::setBoneOffset(const oc::string& name, const glm::quat& deltaRotation)
{
    setBoneOffset(name, glm::mat4_cast(deltaRotation));
}

void AnimationPlayer::clearBoneModifier(uint32 boneIndex)
{
    if (boneIndex >= m_boneModifiers.size())
        return;
    m_boneModifiers[boneIndex] = BoneModifier{};
    m_anyBoneModifier = false;
    for (const BoneModifier& mod : m_boneModifiers)
        if (mod.mode != BoneModifierMode::None) { m_anyBoneModifier = true; break; }
}

void AnimationPlayer::clearBoneModifier(const oc::string& name)
{
    const int32 idx = findBone(name);
    if (idx >= 0)
        clearBoneModifier((uint32)idx);
}

void AnimationPlayer::clearBoneModifiers()
{
    m_boneModifiers.assign(m_boneModifiers.size(), BoneModifier{});
    m_anyBoneModifier = false;
}
