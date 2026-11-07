export module Animation:Clip;

import Core;
import Core.glm;
import :Skeleton;

// How a key blends to the NEXT key of its track. Linear = lerp (slerp for rotations); Step = hold this value until the next key; Smooth = a cubic through the
// neighbouring keys (Catmull-Rom tangents over the key times, flat at the first and last key).
export enum class KeyInterp : uint8 { Linear, Step, Smooth };

export struct PositionKey { float time; glm::vec3 value; KeyInterp interp = KeyInterp::Linear; };
export struct RotationKey { float time; glm::quat value; KeyInterp interp = KeyInterp::Linear; };
export struct ScaleKey    { float time; glm::vec3 value; KeyInterp interp = KeyInterp::Linear; };

// ---- Key sampling (the runtime player and the Animation Editor's curves use the same code) ----

namespace AnimKeys
{
// The segment [i, i+1] that holds t: the last key with time <= t (0 before the first key). Binary search.
export template <typename KeyT>
inline uint32 findSegment(oc::span<const KeyT> keys, float t)
{
    uint32 lo = 0, hi = (uint32)keys.size();
    while (lo < hi)
    {
        const uint32 mid = (lo + hi) >> 1;
        if (keys[mid].time <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : lo - 1;
}

export inline float hermite(float p0, float m0, float p1, float m1, float h, float f)
{
    const float f2 = f * f, f3 = f2 * f;
    return (2.0f * f3 - 3.0f * f2 + 1.0f) * p0 + (f3 - 2.0f * f2 + f) * h * m0 + (-2.0f * f3 + 3.0f * f2) * p1 + (f3 - f2) * h * m1;
}

// The Smooth tangent (value per second) at key k of a channel of n keys: Catmull-Rom over the neighbour times, 0 at either end.
export template <typename V>
inline V smoothTangent(const V& prev, const V& next, float prevTime, float nextTime)
{
    const float span = nextTime - prevTime;
    return span > 1e-8f ? (next - prev) / span : V(0.0f);
}

export template <typename KeyT>
inline glm::vec3 sampleVec3(oc::span<const KeyT> keys, float t, const glm::vec3& fallback)
{
    const uint32 n = (uint32)keys.size();
    if (n == 0) return fallback;
    if (n == 1 || t <= keys[0].time) return keys[0].value;
    const uint32 i = findSegment(keys, t);
    if (i + 1 >= n) return keys[n - 1].value;
    const KeyT& a = keys[i];
    const KeyT& b = keys[i + 1];
    const float span = b.time - a.time;
    const float f = span > 1e-8f ? (t - a.time) / span : 0.0f;
    switch (a.interp)
    {
    case KeyInterp::Step: return a.value;
    case KeyInterp::Smooth:
    {
        const glm::vec3 m0 = i > 0 ? smoothTangent(keys[i - 1].value, b.value, keys[i - 1].time, b.time) : glm::vec3(0.0f);
        const glm::vec3 m1 = i + 2 < n ? smoothTangent(a.value, keys[i + 2].value, a.time, keys[i + 2].time) : glm::vec3(0.0f);
        return glm::vec3(hermite(a.value.x, m0.x, b.value.x, m1.x, span, f), hermite(a.value.y, m0.y, b.value.y, m1.y, span, f),
            hermite(a.value.z, m0.z, b.value.z, m1.z, span, f));
    }
    default: return glm::mix(a.value, b.value, f);
    }
}

// Flips q onto the hemisphere of ref, so a component-wise blend takes the short way.
export inline glm::vec4 alignQuat(const glm::quat& q, const glm::vec4& ref)
{
    const glm::vec4 v(q.x, q.y, q.z, q.w);
    return glm::dot(v, ref) < 0.0f ? -v : v;
}

export inline glm::quat sampleQuat(oc::span<const RotationKey> keys, float t, const glm::quat& fallback)
{
    const uint32 n = (uint32)keys.size();
    if (n == 0) return fallback;
    if (n == 1 || t <= keys[0].time) return keys[0].value;
    const uint32 i = findSegment(keys, t);
    if (i + 1 >= n) return keys[n - 1].value;
    const RotationKey& a = keys[i];
    const RotationKey& b = keys[i + 1];
    const float span = b.time - a.time;
    const float f = span > 1e-8f ? (t - a.time) / span : 0.0f;
    switch (a.interp)
    {
    case KeyInterp::Step: return a.value;
    case KeyInterp::Smooth:
    {
        // A cubic on the 4D components, every neighbour on a's hemisphere, then normalized: close to squad for the key spacing authored by hand.
        const glm::vec4 qa(a.value.x, a.value.y, a.value.z, a.value.w);
        const glm::vec4 qb = alignQuat(b.value, qa);
        const glm::vec4 m0 = i > 0 ? smoothTangent(alignQuat(keys[i - 1].value, qa), qb, keys[i - 1].time, b.time) : glm::vec4(0.0f);
        const glm::vec4 m1 = i + 2 < n ? smoothTangent(qa, alignQuat(keys[i + 2].value, qb), a.time, keys[i + 2].time) : glm::vec4(0.0f);
        const glm::vec4 q(hermite(qa.x, m0.x, qb.x, m1.x, span, f), hermite(qa.y, m0.y, qb.y, m1.y, span, f),
            hermite(qa.z, m0.z, qb.z, m1.z, span, f), hermite(qa.w, m0.w, qb.w, m1.w, span, f));
        const float len = glm::length(q);
        return len > 1e-8f ? glm::quat(q.w / len, q.x / len, q.y / len, q.z / len) : a.value;
    }
    default: return glm::normalize(glm::slerp(a.value, b.value, f));
    }
}
}

// One animated bone track. Keys are sorted by time (in seconds). boneIndex is resolved against the
// Skeleton the clip was built for (-1 = the channel targets a node not present in the skeleton).
export struct AnimationChannel
{
    int32 boneIndex = -1;
    oc::vector<PositionKey> positionKeys;
    oc::vector<RotationKey> rotationKeys;
    oc::vector<ScaleKey>    scaleKeys;
};

// A notify tagged at a normalized time (0..1) in a clip; fired when playback crosses it (see
// AnimationPlayer::getFiredEvents). Authored in .anm as `Event <name> <normalizedTime>`.
export struct AnimationEventKey
{
    oc::string name;
    float normalizedTime = 0.0f;
};

export struct AnimationClip
{
    oc::string name;
    float duration = 0.0f; // seconds
    oc::vector<AnimationChannel> channels;
    bool loop = true;                          // false = one-shot: playback clamps + holds the last frame
    oc::vector<AnimationEventKey> events;     // notifies fired as playback crosses them
};

// A named collection of clips that share one skeleton (e.g. all of a character's animations). Lets an
// AnimationPlayer resolve clips by name. Owns the clips.
export struct AnimationSet
{
    oc::vector<AnimationClip> clips;
    oc::unordered_map<oc::string, uint32> nameToIndex;

    uint32 numClips() const { return (uint32)clips.size(); }
    const AnimationClip* get(uint32 idx) const { return idx < clips.size() ? &clips[idx] : nullptr; }
    const AnimationClip* find(const oc::string& name) const
    {
        const auto it = nameToIndex.find(name);
        return it == nameToIndex.end() ? nullptr : &clips[it->second];
    }
};

// A 1D blend space: clips placed along a single parameter axis (e.g. speed -> idle/walk/run). The player
// blends the two clips bracketing the current parameter. Playback is phase-normalized (all clips share a
// 0..1 phase advanced at the blended duration) so footfalls stay roughly aligned as the blend shifts.
export struct BlendSample1D { const AnimationClip* clip = nullptr; float position = 0.0f; };
export struct BlendSpace1D
{
    oc::vector<BlendSample1D> samples; // kept sorted ascending by position

    void addSample(const AnimationClip* clip, float position)
    {
        auto it = samples.begin();
        while (it != samples.end() && it->position < position) ++it;
        samples.insert(it, BlendSample1D{ clip, position });
    }
};

// Samples animation against a Skeleton each tick into a bone palette (one mat4 per bone,
// = globalBoneTransform * inverseBind), consumed by GPU skinning. CPU-side. The active "source" is either
// a single clip or a 1D blend space; switching sources crossfades from a snapshot of the outgoing pose.
// Sampling is per-bone TRS so rotations blend correctly (slerp), not as matrix lerp. Also supports
// programmatic per-bone posing layered on top.
export class AnimationPlayer
{
public:
    void initialize(const Skeleton* pSkeleton, const AnimationClip* pClip = nullptr);

    // Optional clip library for play(name, ...).
    void setClipLibrary(const AnimationSet* pLibrary) { m_pLibrary = pLibrary; }

    // Crossfade to a source over fadeSeconds (0 = instant). play(name) resolves against the clip library.
    void play(const AnimationClip* pClip, float fadeSeconds = 0.0f);
    bool play(const oc::string& name, float fadeSeconds = 0.0f);
    void playBlendSpace(const BlendSpace1D* pBlendSpace, float fadeSeconds = 0.0f);
    void setClip(const AnimationClip* pClip) { play(pClip, 0.0f); } // instant switch (kept for convenience)

    // Blend-space axis parameter (e.g. movement speed). Clamped to the sample range. Takes effect next tick.
    void setBlendParameter(float x) { m_blendParam = x; }
    float getBlendParameter() const { return m_blendParam; }

    const AnimationClip* getCurrentClip() const { return m_blendSpaceActive ? nullptr : m_pClip; }
    bool isBlendSpaceActive() const { return m_blendSpaceActive; }
    bool isCrossfading() const { return m_fade < 1.0f; }

    void tick(float deltaSeconds);

    // Events whose normalized time was crossed during the most recent tick(). Valid until the next tick().
    oc::span<const oc::string> getFiredEvents() const { return m_firedEvents; }

    void setSpeed(float speed) { m_speed = speed; }
    void setPaused(bool paused) { m_paused = paused; }

    oc::span<const glm::mat4> getPalette() const { return m_palette; }
    uint32 getNumBones() const { return (uint32)m_palette.size(); }

    // 0..1 progress of the active source (single-clip time/duration, or blend-space phase). Used by the
    // state machine for exit-time transitions.
    float getNormalizedTime() const;

    // Scrub: puts the single-clip source at `seconds` (clamped into the clip) and evaluates at once. No events fire and no crossfade runs. The Animation
    // Editor drives its preview through this.
    void setTime(float seconds);
    float getTime() const { return m_time; }

    // The last evaluate()'s per-bone transforms: the composed model-space ones (modifiers applied), and the sampled LOCAL pose before the modifiers.
    oc::span<const glm::mat4> getGlobalTransforms() const { return m_globalTransforms; }
    void getSampledLocalPose(uint32 boneIndex, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) const
    {
        pos = m_poseA.pos[boneIndex];
        rot = m_poseA.rot[boneIndex];
        scale = m_poseA.scale[boneIndex];
    }

    // ---- Programmatic bone posing ----
    // Bones are addressed by name (resolved against the Skeleton) or index. Modifiers are sticky: they
    // apply every tick until cleared, and take effect on the next tick() (which the app calls per frame,
    // even while paused). All transforms are the bone's LOCAL (parent-relative) space.
    const Skeleton* getSkeleton() const { return m_pSkeleton; }
    int32 findBone(const oc::string& name) const;

    // Override: replaces the bone's local transform entirely (ignores the clip/bind for that bone).
    void setBoneTransform(const oc::string& name, const glm::mat4& localTransform);
    void setBoneTransform(uint32 boneIndex, const glm::mat4& localTransform);
    void setBoneTransform(const oc::string& name, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale = glm::vec3(1.0f));

    // Additive: layered on top of the clip/bind pose for that bone (post-multiplied, i.e. applied in the
    // bone's local frame). Use the quat overload to simply rotate a bone relative to its animated pose.
    void setBoneOffset(const oc::string& name, const glm::mat4& localOffset);
    void setBoneOffset(uint32 boneIndex, const glm::mat4& localOffset);
    void setBoneOffset(const oc::string& name, const glm::quat& deltaRotation);

    // Model rotation: turns the bone and its subtree about the bone's own origin by a rotation in MODEL space (the
    // skeleton root's frame, the mesh's own axes), after the clip pose. Lets a caller say "yaw the hips about up"
    // without knowing the bone's local axes.
    void setBoneModelRotation(uint32 boneIndex, const glm::quat& modelRotation);

    void clearBoneModifier(const oc::string& name);
    void clearBoneModifier(uint32 boneIndex);
    void clearBoneModifiers();

private:
    // Per-bone local transform, decomposed so two poses blend correctly (lerp pos/scale, slerp rot).
    struct Pose
    {
        oc::vector<glm::vec3> pos;
        oc::vector<glm::quat> rot;
        oc::vector<glm::vec3> scale;
        void resize(uint32 n) { pos.resize(n); rot.resize(n); scale.resize(n); }
    };
    // The two clips bracketing the current blend parameter (b null when the source is a single clip), and
    // the weight toward b.
    struct BlendResult { const AnimationClip* a = nullptr; const AnimationClip* b = nullptr; float w = 0.0f; };

    void beginCrossfade(float fadeSeconds);
    BlendResult resolveBlend() const;
    void sampleClip(const AnimationClip* pClip, float time, Pose& outPose) const;
    void sampleForeground(Pose& outPose);
    void blendPose(Pose& inOutA, const Pose& b, float w) const;
    void evaluate();
    // Appends to m_firedEvents any event of pClip whose normalized time lies in (prevNorm, curNorm],
    // splitting the interval across 1.0/0.0 when playback wrapped this tick (curNorm < prevNorm).
    // Backward playback (negative speed) tests [curNorm, prevNorm) instead, wrapping when curNorm > prevNorm.
    void detectEvents(const AnimationClip* pClip, float prevNorm, float curNorm, bool backward);

    enum class BoneModifierMode : uint8 { None, Override, Additive, ModelRotate };
    struct BoneModifier { glm::mat4 transform = glm::mat4(1.0f); BoneModifierMode mode = BoneModifierMode::None; };

    const Skeleton* m_pSkeleton = nullptr;
    const AnimationSet* m_pLibrary = nullptr;

    // Active source: a single clip (m_time, seconds) or a 1D blend space (m_phase, normalized 0..1).
    const AnimationClip* m_pClip = nullptr;
    const BlendSpace1D* m_pBlendSpace = nullptr;
    bool m_blendSpaceActive = false;
    float m_time = 0.0f;
    float m_phase = 0.0f;
    float m_blendParam = 0.0f;
    float m_speed = 1.0f;
    bool m_paused = false;

    oc::vector<oc::string> m_firedEvents; // event notifies fired during the last tick()

    // Snapshot crossfade: when a new source starts, the outgoing pose is frozen into m_snapshot and the
    // new source is blended in as m_fade ramps 0 -> 1 over m_fadeDuration seconds.
    float m_fade = 1.0f;
    float m_fadeDuration = 0.0f;
    Pose m_snapshot;

    Pose m_bind;     // bind-pose local TRS (fallback for bones a clip doesn't animate)
    Pose m_poseA;    // scratch: foreground pose
    Pose m_poseB;    // scratch: second blend-space clip
    oc::vector<glm::mat4> m_localTransforms;  // scratch: per-bone local TRS matrix (kept: evaluate() runs every tick)
    oc::vector<glm::mat4> m_globalTransforms; // scratch: per-bone composed global transform
    oc::vector<glm::mat4> m_palette;
    oc::vector<BoneModifier> m_boneModifiers; // per-bone, sized to the skeleton; None unless posed
    bool m_anyBoneModifier = false;            // fast skip in evaluate() when nothing is posed
};
