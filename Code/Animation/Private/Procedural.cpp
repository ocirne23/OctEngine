module Animation;

import Core;
import Core.glm;

namespace
{
    constexpr float TwoPi = 6.28318530718f;

    // sin / cos of a HALF angle, |x| <= pi/2 (a track swings at most 180 degrees). The quaternion is
    // normalized after, so the residual (< 2e-4 at the limit) is an angle error, never a scale.
    inline void halfAngleTrig(float x, float& outSin, float& outCos)
    {
        const float x2 = x * x;
        outSin = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f))));
        outCos = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f))));
    }
}

bool advanceParts(const SceneAnimation& anim, SceneAnimatorState& state, float deltaSeconds, float distance)
{
    bool changed = false;
    const uint32 numLayers = (uint32)glm::min(anim.layers.size(), state.layers.size());
    for (uint32 i = 0; i < numLayers; ++i)
    {
        const PartLayer& layer = anim.layers[i];
        PartLayerState& s = state.layers[i];
        const bool stride = layer.cyclesPerMetre > 0.0f;

        const float target = s.manualWeight >= 0.0f ? s.manualWeight
            : stride ? glm::min(distance / (deltaSeconds * layer.fullSpeed), 1.0f)
            : layer.weight;
        const float step = layer.fadeRate * deltaSeconds;
        const float weight = s.weight + glm::clamp(target - s.weight, -step, step);
        changed |= weight != s.weight;
        s.weight = weight;
        if (weight <= 0.0f)
            continue; // the phase of a silent layer has no effect

        const float advance = stride ? distance * layer.cyclesPerMetre : deltaSeconds * layer.cyclesPerSecond;
        changed |= advance != 0.0f;
        const float phase = s.phase + advance;
        s.phase = phase - std::floor(phase);

        const float angle = TwoPi * s.phase;
        const float s1 = std::sin(angle), c1 = std::cos(angle);
        s.s[0] = s1;
        s.c[0] = c1;
        s.s[1] = 2.0f * s1 * c1; // harmonic 2 from the double-angle identities
        s.c[1] = c1 * c1 - s1 * s1;
    }
    return changed;
}

void evaluatePart(const SceneAnimation& anim, const Part& part, const SceneAnimatorState& state, glm::vec3& outPos, glm::quat& outRot)
{
    glm::vec3 pos = part.bindPos;
    glm::quat rot = part.bindRot;
    bool first = part.bindRotIdentity;

    const PartTrack* track = anim.tracks.data() + part.firstTrack;
    for (const PartTrack* end = track + part.numTracks; track != end; ++track)
    {
        const PartLayerState& t = state.layers[track->layer];
        if (t.weight <= 0.0f)
            continue;
        const uint32 h = track->harmonic - 1u;
        const float value = t.weight * (t.s[h] * track->cosPhase + t.c[h] * track->sinPhase); // sin(a + b)
        if (track->translate)
        {
            pos += track->axis * value;
            continue;
        }
        float sh, ch;
        halfAngleTrig(0.5f * track->amplitude * value, sh, ch);
        const float inv = glm::inversesqrt(sh * sh + ch * ch);
        const glm::quat delta(ch * inv, track->axis * (sh * inv));
        rot = first ? delta : rot * delta;
        first = false;
    }
    outPos = pos;
    outRot = rot;
}

uint32 SceneAnimationBuilder::addLayer(const PartLayer& layer)
{
    m_layers.push_back(layer);
    return (uint32)m_layers.size() - 1u;
}

void SceneAnimationBuilder::add(uint32 layer, const oc::string& part, PartTrack track, float trackPhase, uint32 harmonic)
{
    if (part.empty() || layer >= m_layers.size())
        return;
    track.layer = uint16(layer);
    track.harmonic = harmonic >= 2 ? uint8(2) : uint8(1);
    track.sinPhase = std::sin(TwoPi * trackPhase);
    track.cosPhase = std::cos(TwoPi * trackPhase);
    m_pending.push_back({ part, track });
}

void SceneAnimationBuilder::swing(uint32 layer, const oc::string& part, const glm::vec3& axis, float angleRadians, float trackPhase, uint32 harmonic)
{
    if (glm::dot(axis, axis) < 1e-12f)
        return;
    PartTrack track;
    track.axis = glm::normalize(axis);
    track.amplitude = glm::clamp(angleRadians, -3.14159265f, 3.14159265f);
    add(layer, part, track, trackPhase, harmonic);
}

void SceneAnimationBuilder::bob(uint32 layer, const oc::string& part, const glm::vec3& offset, float trackPhase, uint32 harmonic)
{
    PartTrack track;
    track.axis = offset;
    track.amplitude = 1.0f;
    track.translate = true;
    add(layer, part, track, trackPhase, harmonic);
}

void SceneAnimationBuilder::walkCycle(uint32 layer, const WalkCycleParams& params)
{
    swing(layer, params.leftLeg,  params.axis, params.legAngle, 0.0f);
    swing(layer, params.rightLeg, params.axis, params.legAngle, 0.5f);
    swing(layer, params.leftArm,  params.axis, params.armAngle, 0.5f);
    swing(layer, params.rightArm, params.axis, params.armAngle, 0.0f);
    if (params.bobHeight != 0.0f)
        bob(layer, params.bobPart, glm::vec3(0.0f, params.bobHeight, 0.0f));
}

SceneAnimation SceneAnimationBuilder::build() const
{
    SceneAnimation anim;
    anim.layers = m_layers;
    for (const Pending& p : m_pending) // parts in first-use order
    {
        bool known = false;
        for (const Part& part : anim.parts)
            known |= part.name == p.part;
        if (!known)
            anim.parts.emplace_back().name = p.part;
    }
    for (Part& part : anim.parts)
    {
        part.firstTrack = uint16(anim.tracks.size());
        for (const Pending& p : m_pending)
        {
            if (p.part != part.name)
                continue;
            anim.tracks.push_back(p.track);
            (p.track.translate ? part.translates : part.rotates) = true;
        }
        part.numTracks = uint16(anim.tracks.size() - part.firstTrack);
    }
    return anim;
}
