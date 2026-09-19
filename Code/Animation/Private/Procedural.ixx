export module Animation:Procedural;

import Core;
import Core.glm;
import :Skeleton;
import :Clip;

// Builds an AnimationClip from code instead of an imported file. Bones are addressed by name against the
// skeleton the clip is for; a name the skeleton does not have is ignored (the same rule as the import
// retarget). Raw keys are ABSOLUTE local transforms; swing / bob are relative to the bone's bind pose.
export class ClipBuilder
{
public:
    ClipBuilder(const Skeleton& skeleton, const oc::string& name, float durationSeconds, bool loop = true);

    ClipBuilder& position(const oc::string& bone, float time, const glm::vec3& value);
    ClipBuilder& rotation(const oc::string& bone, float time, const glm::quat& value);
    ClipBuilder& scale(const oc::string& bone, float time, const glm::vec3& value);

    // Sine rotation about `axis` (the bone's local frame) around the bind rotation: +-angleRadians,
    // `cycles` periods over the clip, phase in 0..1 periods. Replaces the bone's rotation keys. A looping
    // clip needs a whole number of cycles to close.
    ClipBuilder& swing(const oc::string& bone, const glm::vec3& axis, float angleRadians, float phase = 0.0f, float cycles = 1.0f, uint32 keysPerCycle = 16);
    // Sine translation along `offset` (parent space) around the bind position. Replaces the position keys.
    ClipBuilder& bob(const oc::string& bone, const glm::vec3& offset, float phase = 0.0f, float cycles = 1.0f, uint32 keysPerCycle = 16);

    ClipBuilder& event(const oc::string& name, float normalizedTime);

    AnimationClip build() { return oc::move(m_clip); }

private:
    AnimationChannel* channelFor(const oc::string& bone);

    const Skeleton& m_skeleton;
    AnimationClip m_clip;
};

// Data form of a swing / bob clip, so an asset file can describe one (see .anm `Procedural`).
export struct ProceduralSwing
{
    oc::string bone;
    glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f);
    float angle = 0.5f; // radians
    float phase = 0.0f; // 0..1 periods
    float cycles = 1.0f;
};
export struct ProceduralBob
{
    oc::string bone;
    glm::vec3 offset = glm::vec3(0.0f, 0.1f, 0.0f);
    float phase = 0.0f;
    float cycles = 2.0f;
};
export struct ProceduralClipDesc
{
    float duration = 1.0f;
    oc::vector<ProceduralSwing> swings;
    oc::vector<ProceduralBob> bobs;
};

// No swings and no bobs = a clip that holds the bind pose (an idle to blend a walk against).
export AnimationClip buildProceduralClip(const Skeleton& skeleton, const oc::string& name, const ProceduralClipDesc& desc, bool loop = true);

// A limb walk: legs in opposite phase, each arm opposite to the leg on its side. An empty bone name
// leaves that limb out. bobBone (optional) moves up twice per cycle.
export struct WalkCycleParams
{
    float duration = 1.0f;
    glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f);
    float legAngle = 0.61f; // radians
    float armAngle = 0.52f;
    oc::string leftLeg = "LeftLeg";
    oc::string rightLeg = "RightLeg";
    oc::string leftArm = "LeftArm";
    oc::string rightArm = "RightArm";
    oc::string bobBone;
    float bobHeight = 0.0f;
};
export ProceduralClipDesc makeWalkCycleDesc(const WalkCycleParams& params);
