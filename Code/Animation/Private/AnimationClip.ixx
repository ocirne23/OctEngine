export module Animation:Clip;

import Core;
import Core.glm;
import :Skeleton;

export struct PositionKey { float time; glm::vec3 value; };
export struct RotationKey { float time; glm::quat value; };
export struct ScaleKey    { float time; glm::vec3 value; };

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

    // ---- Programmatic bone posing -------------------------------------------------------------------
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
    void detectEvents(const AnimationClip* pClip, float prevNorm, float curNorm);

    enum class BoneModifierMode : uint8 { None, Override, Additive };
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
    oc::vector<glm::mat4> m_globalTransforms; // scratch: per-bone composed global transform
    oc::vector<glm::mat4> m_palette;
    oc::vector<BoneModifier> m_boneModifiers; // per-bone, sized to the skeleton; None unless posed
    bool m_anyBoneModifier = false;            // fast skip in evaluate() when nothing is posed
};
