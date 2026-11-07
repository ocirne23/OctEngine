export module App.PlayerLocomotion;

import Core;
import Core.glm;
import Core.Log;
import Settings;

import Animation;
import Entity;

// Camera-locked strafe for the local key C capsule (playerCapsule.pre, ThirdPersonAnimator) with only FORWARD clips:
//
// * the entity yaw follows the camera yaw, so the upper body always faces where the camera looks;
// * the HIPS turn (a model-space rotation, so the legs follow) toward the move direction, and Spine / Chest / Neck turn
//   back by the same total, so the legs run along the velocity while the chest stays on the camera;
// * moving more than ~110 deg away from the facing flips to BACKWARD: the hips aim at the opposite direction and the
//   locomotion plays in reverse (a hysteresis band keeps it from flipping at a pure strafe);
// * the playback rate is the ground speed over the blended clip's stride (the "Player/Locomotion" clip speeds), so the
//   feet do not slide;
// * standing still, the feet stay planted while the camera turns, until the twist passes "Idle turn angle";
// * the upper body leans into the horizontal acceleration.
//
// Main thread, before the entity pass (the bone modifiers and the parameters apply at this frame's animator tick).
export class PlayerLocomotion
{
public:
    // A new body: drop the cached bones and start facing `yaw` (radians, 0 = +Z).
    void reset(float yaw)
    {
        m_animator = nullptr;
        m_bodyYaw = yaw;
        m_legYaw = 0.0f;
        m_backward = false;
        m_idleTurning = false;
        m_lean = glm::vec3(0.0f);
        m_hasVelocity = false;
    }

    // `velocity` = the body's world velocity, `cameraYaw` in radians (0 = +Z, the model's forward).
    void update(Entity& entity, const glm::vec3& velocity, float cameraYaw, float deltaSec)
    {
        if (deltaSec <= 0.0f)
            return;
        const AppControlsSettings& s = Globals::settings.appControls;
        const glm::vec3 planar(velocity.x, 0.0f, velocity.z);
        if (!m_hasVelocity)
        {
            m_prevVelocity = planar;
            m_hasVelocity = true;
        }

        // ---- Body yaw: follows the camera ----
        const float prevBodyYaw = m_bodyYaw;
        m_bodyYaw = wrapPi(m_bodyYaw + wrapPi(cameraYaw - m_bodyYaw) * (1.0f - glm::exp(-s.bodyTurnRate * deltaSec)));
        const glm::quat bodyRot = glm::angleAxis(m_bodyYaw, glm::vec3(0.0f, 1.0f, 0.0f));
        entity.rot = bodyRot;

        // ---- Leg yaw: relative to the body, toward the move direction ----
        const float speed = glm::length(planar);
        const float maxTwist = glm::radians(s.maxHipTwist);
        const float legStep = glm::radians(s.legTurnSpeed) * deltaSec;
        constexpr float c_moveSpeed = 0.3f; // m/s: below this the body counts as standing
        if (speed > c_moveSpeed)
        {
            m_idleTurning = false;
            const float moveYaw = glm::atan(planar.x, planar.z);
            const float rel = wrapPi(moveYaw - m_bodyYaw);
            const float absRel = glm::abs(rel);
            if (!m_backward && absRel > glm::radians(110.0f))
                m_backward = true;
            else if (m_backward && absRel < glm::radians(70.0f))
                m_backward = false;
            const float target = glm::clamp(m_backward ? wrapPi(rel - glm::pi<float>()) : rel, -maxTwist, maxTwist);
            m_legYaw += glm::clamp(target - m_legYaw, -legStep, legStep); // linear, so a flip swings through 0
        }
        else
        {
            // Planted: the legs keep their world yaw while the body turns, then step back under it.
            m_legYaw = wrapPi(m_legYaw - wrapPi(m_bodyYaw - prevBodyYaw));
            if (glm::abs(m_legYaw) > glm::radians(s.idleTurnAngle))
                m_idleTurning = true;
            if (m_idleTurning)
            {
                m_legYaw += glm::clamp(-m_legYaw, -legStep, legStep);
                if (glm::abs(m_legYaw) < glm::radians(1.0f))
                    m_idleTurning = false;
            }
        }
        m_legYaw = glm::clamp(m_legYaw, -maxTwist, maxTwist);

        // ---- Lean: into the horizontal acceleration, in the body frame ----
        const glm::vec3 accel = glm::inverse(bodyRot) * ((planar - m_prevVelocity) / deltaSec);
        m_prevVelocity = planar;
        m_lean += (accel - m_lean) * (1.0f - glm::exp(-8.0f * deltaSec));

        AnimatorComponent* animator = getComponent<AnimatorComponent>(&entity);
        if (!animator || !animator->built)
            return;
        if (animator != m_animator)
            bind(*animator);

        // ---- Gait (0 idle, 1 walk, 2 run) and the stride-matched playback rate ----
        const float walkSpeed = glm::max(s.walkClipSpeed, 0.05f);
        const float runSpeed = glm::max(s.runClipSpeed, walkSpeed + 0.05f);
        float gait;
        float rate = 1.0f;
        if (speed <= walkSpeed)
            gait = speed / walkSpeed; // idle..walk: the blend shortens the stride, the cadence stays
        else
        {
            const float w = glm::min((speed - walkSpeed) / (runSpeed - walkSpeed), 1.0f);
            gait = 1.0f + w;
            const float cycle = glm::mix(m_walkDuration, m_runDuration, w);                         // seconds per blended cycle
            const float stride = glm::mix(walkSpeed * m_walkDuration, runSpeed * m_runDuration, w); // metres per blended cycle
            rate = stride > 1e-4f ? speed * cycle / stride : 1.0f;
        }
        animator->stateMachine.setFloat(m_gaitName, gait);
        animator->stateMachine.setFloat(m_rateName, m_backward ? -rate : rate);

        // ---- Bone yaw: hips toward the legs, the spine chain back to the body ----
        AnimationPlayer& player = animator->player;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const float leanDeg = glm::min(glm::length(glm::vec2(m_lean.x, m_lean.z)) * s.leanPerAccel, s.maxLean);
        const glm::vec3 leanAxis(m_lean.z, 0.0f, -m_lean.x); // cross(up, lean): tips the top toward the lean
        const glm::quat lean = leanDeg > 0.01f
            ? glm::angleAxis(glm::radians(leanDeg), glm::normalize(leanAxis))
            : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (m_hips >= 0)
            player.setBoneModelRotation(uint32(m_hips), glm::angleAxis(m_legYaw, up));
        constexpr float c_spineShare[3] = { 0.35f, 0.4f, 0.25f }; // Spine, Chest, Neck: sums to 1
        const int32 chain[3] = { m_spine, m_chest, m_neck };
        for (int i = 0; i < 3; ++i)
        {
            if (chain[i] < 0)
                continue;
            const glm::quat yaw = glm::angleAxis(-m_legYaw * c_spineShare[i], up);
            player.setBoneModelRotation(uint32(chain[i]), i == 0 ? lean * yaw : yaw);
        }
    }

private:
    void bind(AnimatorComponent& animator)
    {
        m_animator = &animator;
        const AnimationPlayer& player = animator.player;
        m_hips = player.findBone("Hips");
        m_spine = player.findBone("Spine");
        m_chest = player.findBone("Chest");
        m_neck = player.findBone("Neck");
        if (m_hips < 0 || m_spine < 0)
            Log::warning("PlayerLocomotion: the rig has no Hips / Spine bone - the strafe twist is off");
        if (const Skeleton* skel = player.getSkeleton(); skel && m_hips >= 0)
        {
            const int32 leg = player.findBone("LeftUpLeg");
            int32 p = leg;
            while (p >= 0 && p != m_hips)
                p = skel->parentIndices[p];
            if (leg >= 0 && p != m_hips)
                Log::warning("PlayerLocomotion: LeftUpLeg is not under Hips - turning the hips will not turn the legs");
        }
        const AnimationClip* walk = animator.clipSet ? animator.clipSet->find("walk") : nullptr;
        const AnimationClip* run = animator.clipSet ? animator.clipSet->find("run") : nullptr;
        m_walkDuration = walk && walk->duration > 0.0f ? walk->duration : 1.0f;
        m_runDuration = run && run->duration > 0.0f ? run->duration : 1.0f;
    }

    static float wrapPi(float a)
    {
        constexpr float twoPi = 2.0f * glm::pi<float>();
        return a - twoPi * glm::floor((a + glm::pi<float>()) / twoPi);
    }

    const oc::string m_gaitName = "gait";
    const oc::string m_rateName = "rate";
    const AnimatorComponent* m_animator = nullptr; // the bones below belong to this one
    int32 m_hips = -1, m_spine = -1, m_chest = -1, m_neck = -1;
    float m_walkDuration = 1.0f;
    float m_runDuration = 1.0f;

    float m_bodyYaw = 0.0f;   // world, radians
    float m_legYaw = 0.0f;    // relative to the body, radians
    bool m_backward = false;
    bool m_idleTurning = false;
    glm::vec3 m_lean = glm::vec3(0.0f); // smoothed body-frame acceleration, m/s^2
    glm::vec3 m_prevVelocity = glm::vec3(0.0f);
    bool m_hasVelocity = false;
};
