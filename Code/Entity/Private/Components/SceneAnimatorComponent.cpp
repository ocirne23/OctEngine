module;

#include <immintrin.h>

module Entity;

import Core;
import Core.Log;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import :Entity;
import File;
import Animation;
import RendererVK;
import Spatial;

namespace
{
    constexpr float TwoPi = 6.28318530718f;
}

// ---- SceneAnimRig: authoring -----------------------------------------------------------------------

uint32 SceneAnimRig::addLayer(const Layer& layer)
{
    layers.push_back(layer);
    return (uint32)layers.size() - 1u;
}

void SceneAnimRig::add(uint32 layer, const oc::string& bone, Track track, float trackPhase, uint32 harmonic)
{
    if (bone.empty() || layer >= layers.size())
        return;
    track.layer = uint16(layer);
    track.harmonic = harmonic >= 2 ? uint8(2) : uint8(1);
    track.sinPhase = std::sin(TwoPi * trackPhase);
    track.cosPhase = std::cos(TwoPi * trackPhase);
    m_pending.push_back({ bone, track });
}

void SceneAnimRig::swing(uint32 layer, const oc::string& bone, const glm::vec3& axis, float angleRadians, float trackPhase, uint32 harmonic)
{
    if (glm::dot(axis, axis) < 1e-12f)
        return;
    Track track;
    track.axis = glm::normalize(axis);
    track.amplitude = glm::clamp(angleRadians, -3.14159265f, 3.14159265f);
    add(layer, bone, track, trackPhase, harmonic);
}

void SceneAnimRig::bob(uint32 layer, const oc::string& bone, const glm::vec3& offset, float trackPhase, uint32 harmonic)
{
    Track track;
    track.axis = offset;
    track.amplitude = 1.0f;
    track.translate = true;
    add(layer, bone, track, trackPhase, harmonic);
}

void SceneAnimRig::walkCycle(uint32 layer, const WalkCycle& walk)
{
    swing(layer, walk.leftLeg,  walk.axis, walk.legAngle, 0.0f);
    swing(layer, walk.rightLeg, walk.axis, walk.legAngle, 0.5f);
    swing(layer, walk.leftArm,  walk.axis, walk.armAngle, 0.5f);
    swing(layer, walk.rightArm, walk.axis, walk.armAngle, 0.0f);
    if (walk.bobHeight != 0.0f)
        bob(layer, walk.bobBone, glm::vec3(0.0f, walk.bobHeight, 0.0f));
}

void SceneAnimRig::finalize(const oc::string& ownerName)
{
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    for (size_t b = 0; b < bones.size(); ++b)
    {
        Bone& bone = bones[b];
        if (bone.render)
        {
            const Transform& rl = bone.render->localTransform;
            bone.renderOffsetOnly = rl.quat == identity && rl.scale == 1.0f;
        }

        Part part;
        part.bone = uint16(b);
        part.firstTrack = uint16(tracks.size());
        part.bindRotIdentity = bone.bind.quat == identity;
        for (const bool translations : { false, true }) // two runs, so evaluate() has no branch per track
            for (const Pending& p : m_pending)
                if (p.bone == bone.name && p.track.translate == translations)
                {
                    tracks.push_back(p.track);
                    ++(translations ? part.numTranslations : part.numRotations);
                }
        if (part.numRotations == 1 && part.numTranslations == 0 && part.bindRotIdentity)
        {
            // The usual limb: a lane of a swing batch instead of a Part.
            const Track track = tracks.back();
            tracks.pop_back();
            if (swingBatches.empty() || swingBatches.back().count == 4)
                swingBatches.emplace_back();
            SwingBatch& batch = swingBatches.back();
            const uint32 lane = batch.count++;
            batch.halfAmplitude[lane] = 0.5f * track.amplitude;
            batch.cosPhase[lane] = track.cosPhase;
            batch.sinPhase[lane] = track.sinPhase;
            batch.axisX[lane] = track.axis.x;
            batch.axisY[lane] = track.axis.y;
            batch.axisZ[lane] = track.axis.z;
            batch.bone[lane] = uint16(b);
            batch.layer[lane] = track.layer;
            batch.harmonic[lane] = track.harmonic;
            batch.uniformLayer &= track.layer == batch.layer[0] && track.harmonic == batch.harmonic[0];
        }
        else if (part.numRotations || part.numTranslations)
            parts.push_back(part); // only the bones a track moves
    }
    for (const Pending& p : m_pending)
    {
        bool known = false;
        for (const Bone& bone : bones)
            known |= bone.name == p.bone;
        if (!known)
            Log::warning("Scene: entity '" + ownerName + "' SceneAnimator moves unknown bone '" + p.bone + "'");
    }
    m_pending.clear();
    m_pending.shrink_to_fit();
}

int32 SceneAnimRig::findLayer(oc::string_view name) const
{
    for (uint32 i = 0; i < (uint32)layers.size(); ++i)
        if (layers[i].name == name)
            return (int32)i;
    return -1;
}

// ---- SceneAnimatorComponent ------------------------------------------------------------------------

void SceneAnimatorComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    rig = info.rig.get();
    enabled = info.enabled;
    if (!rig)
        return;
    layers.resize(rig->layers.size());
    spawnBoneNodes(*rig, base, bones);
    for (size_t i = 0; i < bones.size(); ++i)
        bones[i].local = rig->bones[i].bind;
}

void SceneAnimatorComponent::destroy(Entity& entity, const SpawnInfo& info)
{
}

bool SceneAnimatorComponent::advance(float deltaSeconds, float distance)
{
    bool changed = false;
    const uint32 numLayers = (uint32)layers.size();
    for (uint32 i = 0; i < numLayers; ++i)
    {
        const SceneAnimRig::Layer& layer = rig->layers[i];
        LayerState& s = layers[i];
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

        const float advanceBy = stride ? distance * layer.cyclesPerMetre : deltaSeconds * layer.cyclesPerSecond;
        changed |= advanceBy != 0.0f;
        const float phase = s.phase + advanceBy;
        s.phase = phase - std::floor(phase);

        float s1, c1;
        BoneMath::phaseTrig(s.phase, s1, c1);
        s.s[0] = s1;
        s.c[0] = c1;
        s.s[1] = 2.0f * s1 * c1; // harmonic 2 from the double-angle identities
        s.c[1] = c1 * c1 - s1 * s1;
    }
    return changed;
}

void SceneAnimatorComponent::evaluate(const SceneAnimRig::Part& part, Transform& local) const
{
    // weight * sin(a + b): the layer's sin / cos of this tick and the track's constant phase
    const auto trackValue = [this](const SceneAnimRig::Track& track)
    {
        const LayerState& t = layers[track.layer];
        const uint32 h = track.harmonic - 1u;
        return t.weight * (t.s[h] * track.cosPhase + t.c[h] * track.sinPhase); // weight 0 = a silent layer = 0
    };

    const Transform& bind = rig->bones[part.bone].bind;
    const SceneAnimRig::Track* track = rig->tracks.data() + part.firstTrack;
    if (part.numRotations)
    {
        glm::quat result = bind.quat;
        bool first = part.bindRotIdentity;
        for (const SceneAnimRig::Track* end = track + part.numRotations; track != end; ++track)
        {
            const float value = trackValue(*track);
            if (value == 0.0f)
                continue; // a silent layer: the delta is the identity
            // A swing is at most 180 degrees, so the half angle is within the polynomial's range; the
            // normalize turns its residual (< 2e-4 at the limit) into an angle error, never a scale.
            float sh, ch;
            BoneMath::polyTrig(0.5f * track->amplitude * value, sh, ch);
            const float inv = glm::inversesqrt(sh * sh + ch * ch);
            const glm::quat delta(ch * inv, track->axis * (sh * inv));
            result = first ? delta : result * delta;
            first = false;
        }
        local.quat = result;
    }
    if (part.numTranslations)
    {
        glm::vec3 result = bind.pos;
        for (const SceneAnimRig::Track* end = track + part.numTranslations; track != end; ++track)
            result += track->axis * trackValue(*track);
        local.pos = result;
    }
}

// Four limbs at once: value -> half angle -> polynomial sin / cos -> normalize -> (axis * sin, cos).
// The bind rotation is the identity for every lane (finalize's rule), so the delta IS the rotation.
void SceneAnimatorComponent::evaluateSwings(const SceneAnimRig::SwingBatch& batch)
{
    __m128 layerSin, layerCos; // weight * sin / cos of the lane's layer + harmonic
    if (batch.uniformLayer)
    {
        const LayerState& t = layers[batch.layer[0]];
        const uint32 h = batch.harmonic[0] - 1u;
        layerSin = _mm_set1_ps(t.weight * t.s[h]);
        layerCos = _mm_set1_ps(t.weight * t.c[h]);
    }
    else
    {
        float ws[4], wc[4];
        for (uint32 i = 0; i < 4; ++i)
        {
            const LayerState& t = layers[batch.layer[i]];
            const uint32 h = batch.harmonic[i] - 1u;
            ws[i] = t.weight * t.s[h];
            wc[i] = t.weight * t.c[h];
        }
        layerSin = _mm_loadu_ps(ws);
        layerCos = _mm_loadu_ps(wc);
    }
    // weight * sin(a + b), then the half angle
    const __m128 value = _mm_fmadd_ps(layerSin, _mm_loadu_ps(batch.cosPhase), _mm_mul_ps(layerCos, _mm_loadu_ps(batch.sinPhase)));
    __m128 sh, ch;
    BoneMath::polyTrig4(_mm_mul_ps(value, _mm_loadu_ps(batch.halfAmplitude)), sh, ch);

    // 1 / sqrt(sh^2 + ch^2): the estimate + one Newton step (the sum is within 2e-4 of 1)
    const __m128 n = _mm_fmadd_ps(sh, sh, _mm_mul_ps(ch, ch));
    __m128 inv = _mm_rsqrt_ps(n);
    inv = _mm_mul_ps(inv, _mm_fnmadd_ps(_mm_mul_ps(_mm_set1_ps(0.5f), n), _mm_mul_ps(inv, inv), _mm_set1_ps(1.5f)));
    sh = _mm_mul_ps(sh, inv);

    __m128 qx = _mm_mul_ps(_mm_loadu_ps(batch.axisX), sh);
    __m128 qy = _mm_mul_ps(_mm_loadu_ps(batch.axisY), sh);
    __m128 qz = _mm_mul_ps(_mm_loadu_ps(batch.axisZ), sh);
    __m128 qw = _mm_mul_ps(ch, inv);
    _MM_TRANSPOSE4_PS(qx, qy, qz, qw); // SoA -> four (x, y, z, w) quaternions

    const __m128 quats[4] = { qx, qy, qz, qw };
    for (uint32 i = 0; i < batch.count; ++i)
    {
        _mm_storeu_ps(&bones[batch.bone[i]].local.quat.x, quats[i]);
        refreshNodeLocal(batch.bone[i]);
    }
}

void SceneAnimatorComponent::refreshNodeLocal(uint32 boneIdx)
{
    Bone& bone = bones[boneIdx];
    if (!bone.node.isValid())
        return;
    const SceneAnimRig::Bone& src = rig->bones[boneIdx];
    const Transform& renderLocal = src.render->localTransform;
    if (src.renderOffsetOnly) // the usual mesh offset: no quaternion product, no scale product
        bone.nodeLocal = Transform(bone.local.pos + bone.local.quat * (renderLocal.pos * bone.local.scale), bone.local.scale, bone.local.quat);
    else
        bone.nodeLocal = composeTransform(bone.local, renderLocal);
}

bool SceneAnimatorComponent::beginTick(const Entity& entity, float& deltaSeconds, float& outDistance)
{
    float distance;
    if (drivenDistance >= 0.0f)
    {
        if (drivenDelta <= 0.0f)
            return false; // driven, and the parent did not tick this frame (SIM LOD)
        distance = drivenDistance;
        deltaSeconds = drivenDelta; // the parent's tick, catch-up included
        drivenDistance = 0.0f;
        drivenDelta = 0.0f;
    }
    else
    {
        // Planar distance in the parent's space: the world, for a root entity.
        const glm::vec3 pos = entity.pos;
        distance = hasLastPos ? glm::length(glm::vec2(pos.x - lastPos.x, pos.z - lastPos.z)) : 0.0f;
        lastPos = pos;
        hasLastPos = true;
    }
    outDistance = distance / glm::max(entity.scale, 1e-4f);
    return true;
}

void SceneAnimatorComponent::update(Entity& entity, float deltaSeconds)
{
    if (!enabled || !rig)
        return;
    float distance;
    if (!beginTick(entity, deltaSeconds, distance))
        return;
    if (!advance(deltaSeconds, distance))
        return;

    for (const SceneAnimRig::SwingBatch& batch : rig->swingBatches)
        evaluateSwings(batch);
    for (const SceneAnimRig::Part& part : rig->parts)
    {
        evaluate(part, bones[part.bone].local);
        refreshNodeLocal(part.bone);
    }
    placeDirty = true;
}

void SceneAnimatorComponent::place(Renderer& renderer, const Transform& world, uint32 passMask)
{
    // Compose again only when a pose changed or the entity's world transform did.
    if (placeDirty || world.pos != placedWorld.pos || world.quat != placedWorld.quat || world.scale != placedWorld.scale)
    {
        placedWorld = world;
        placeDirty = false;
        if (rig->flat)
            placeFlatBones(bones, world);
        else
        {
            const uint32 numBones = (uint32)bones.size();
            oc::small_vector<Transform, 16> worlds; // nested bones: the chain, parent before child
            worlds.resize(numBones);
            for (uint32 i = 0; i < numBones; ++i)
            {
                const int32 parent = rig->bones[i].parent;
                worlds[i] = composeTransform(parent < 0 ? world : worlds[parent], bones[i].local);
                if (bones[i].node.isValid())
                    bones[i].node.setTransform(composeTransform(worlds[i], rig->bones[i].render->localTransform));
            }
        }
    }
    if (passMask != 0)
        submitBones(renderer, bones, passMask);
}

void SceneAnimatorComponent::addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const
{
    if (rig)
        addBoneRestBounds(*rig, bones, toEntity, bounds, any);
}

void writeSceneAnimatorSpawnInfo(const SceneAnimatorComponent::SpawnInfo& info, AssetNode& out)
{
    for (const AssetNode& child : info.source.children)
        out.children.push_back(child);
}
