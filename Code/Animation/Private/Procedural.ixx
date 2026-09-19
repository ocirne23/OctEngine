export module Animation:Procedural;

import Core;
import Core.glm;

// Procedural PART animation: rigid parts (a box limb, a turret barrel) moved by closed-form sines.
// No keys, no sampled pose, no blend of poses: a layer is ONE phase + ONE weight, a track is
//   value = weight * amplitude * sin(2 pi * (harmonic * phase + trackPhase))
// turned into a rotation about a fixed part-local axis, or a translation along a fixed offset. A
// blend is the weight. A tick costs one sin/cos pair per active layer plus a short polynomial per
// track, and advance() says when the pose did not change, so the owner then writes NOTHING.
// Knows nothing about entities: the owner resolves part names and writes the transforms.

export struct PartLayer
{
    oc::string name;
    // Stride layer (cyclesPerMetre > 0): the phase follows the DISTANCE moved - feet do not slide - and
    // the weight follows the speed, 1 at fullSpeed. Else a timed layer: cyclesPerSecond at `weight`.
    float cyclesPerMetre = 0.0f;
    float fullSpeed = 1.0f;
    float cyclesPerSecond = 1.0f;
    float weight = 1.0f;
    float fadeRate = 8.0f; // weight change per second
};

export struct PartTrack
{
    glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f); // rotation: unit, part-local. translation: direction * metres
    float amplitude = 0.0f;                       // rotation: radians. translation: 1
    float sinPhase = 0.0f;                        // of 2 pi * trackPhase, so a track needs no trig of its own
    float cosPhase = 1.0f;
    uint16 layer = 0;
    uint8 harmonic = 1; // 1 or 2 cycles per layer cycle
    bool translate = false;
};

export struct Part
{
    oc::string name;
    glm::vec3 bindPos = glm::vec3(0.0f);
    glm::quat bindRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    uint16 firstTrack = 0; // tracks are grouped by part
    uint16 numTracks = 0;
    bool rotates = false;
    bool translates = false;
    bool bindRotIdentity = true;

    void setBind(const glm::vec3& pos, const glm::quat& rot)
    {
        bindPos = pos;
        bindRot = rot;
        bindRotIdentity = rot == glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
};

// Shared and immutable once built: one per prefab, not per instance.
export struct SceneAnimation
{
    oc::vector<PartLayer> layers;
    oc::vector<Part> parts; // only parts a track moves
    oc::vector<PartTrack> tracks;

    int32 findLayer(oc::string_view name) const
    {
        for (uint32 i = 0; i < (uint32)layers.size(); ++i)
            if (layers[i].name == name)
                return (int32)i;
        return -1;
    }
};

// The whole per-instance state: one entry per layer of the animation.
export struct PartLayerState
{
    float phase = 0.0f;         // 0..1
    float weight = 0.0f;
    float manualWeight = -1.0f; // >= 0 replaces the layer's own weight rule
    float s[2] = {};            // sin / cos of harmonic 1 / 2 at `phase`: advanceParts writes, evaluatePart reads
    float c[2] = {};
};
export struct SceneAnimatorState
{
    oc::vector<PartLayerState> layers;

    void initialize(const SceneAnimation& anim) { layers.assign(anim.layers.size(), PartLayerState{}); }
};

// Advances phases and weights by one tick (distance = metres moved this tick). FALSE = the pose is the
// same as after the last tick: skip the evaluation and the writes.
export bool advanceParts(const SceneAnimation& anim, SceneAnimatorState& state, float deltaSeconds, float distance);

// The part's local transform. outPos is valid when part.translates, outRot when part.rotates.
export void evaluatePart(const SceneAnimation& anim, const Part& part, const SceneAnimatorState& state, glm::vec3& outPos, glm::quat& outRot);

// A limb walk: legs in opposite phase, each arm opposite to the leg on its side. An empty part name
// leaves that limb out. bobPart (optional) moves up twice per cycle.
export struct WalkCycleParams
{
    glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f);
    float legAngle = 0.61f; // radians
    float armAngle = 0.52f;
    oc::string leftLeg = "LeftLeg";
    oc::string rightLeg = "RightLeg";
    oc::string leftArm = "LeftArm";
    oc::string rightArm = "RightArm";
    oc::string bobPart;
    float bobHeight = 0.0f;
};

export class SceneAnimationBuilder
{
public:
    uint32 addLayer(const PartLayer& layer);

    // trackPhase in 0..1 cycles; harmonic 1 or 2.
    void swing(uint32 layer, const oc::string& part, const glm::vec3& axis, float angleRadians, float trackPhase = 0.0f, uint32 harmonic = 1);
    void bob(uint32 layer, const oc::string& part, const glm::vec3& offset, float trackPhase = 0.0f, uint32 harmonic = 2);
    void walkCycle(uint32 layer, const WalkCycleParams& params);

    // Parts come out with an identity bind; the owner sets the real one (Part::setBind).
    SceneAnimation build() const;

private:
    struct Pending { oc::string part; PartTrack track; };
    void add(uint32 layer, const oc::string& part, PartTrack track, float trackPhase, uint32 harmonic);

    oc::vector<PartLayer> m_layers;
    oc::vector<Pending> m_pending;
};
