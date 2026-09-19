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

bool HumanoidRig::configure(const Desc& desc, const oc::string& ownerName)
{
    const auto refuse = [&](const oc::string& why)
    {
        Log::warning("Scene: entity '" + ownerName + "' HumanoidAnimator: " + why);
        return false;
    };
    if (!flat)
        return refuse("the bones must not nest");

    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    size_t limbBone[NumLimbs] = {};
    for (uint32 limb = 0; limb < NumLimbs; ++limb)
    {
        int32 found = -1;
        for (size_t b = 0; b < bones.size() && found < 0; ++b)
            if (bones[b].name == desc.limbs[limb])
                found = int32(b);
        if (found < 0)
            return refuse("no bone '" + desc.limbs[limb] + "'");
        for (uint32 other = 0; other < limb; ++other)
            if (limbBone[other] == size_t(found))
                return refuse("bone '" + desc.limbs[limb] + "' is named for two limbs");
        limbBone[limb] = size_t(found);

        const Bone& bone = bones[found];
        if (!bone.render)
            return refuse("bone '" + bone.name + "' has no Component Render");
        const Transform& renderLocal = bone.render->localTransform;
        if (bone.bind.quat != identity)
            return refuse("bone '" + bone.name + "' needs an identity bind rotation");
        // Rotation only: with the mesh origin at the pivot the node position never changes. A mesh that
        // hangs off its bone by a render offset (Pillar3) would have to swing on an arc.
        if (renderLocal.pos != glm::vec3(0.0f) || renderLocal.quat != identity || renderLocal.scale != 1.0f)
            return refuse("bone '" + bone.name + "' Component Render must have no Position / Rotation / Scale - use a mesh whose origin is the pivot (Limb3)");
    }

    // The four limbs become bones 0..3, in Limb order, so the tick stores lane i into bone i with no index
    // table. A flat rig has no parent indices to fix, and the rest keep their authored order behind the limbs.
    oc::vector<Bone> ordered;
    ordered.reserve(bones.size());
    for (const size_t b : limbBone)
        ordered.push_back(oc::move(bones[b]));
    for (size_t b = 0; b < bones.size(); ++b)
    {
        bool isLimb = false;
        for (const size_t l : limbBone)
            isLimb |= l == b;
        if (!isLimb)
            ordered.push_back(oc::move(bones[b]));
    }
    bones = oc::move(ordered);

    constexpr float c_maxHalfAngle = 0.785398f; // 45 degrees: the un-normalized polynomial is within 3e-7 there
    const float halfLeg = glm::clamp(0.5f * desc.legAngle, -c_maxHalfAngle, c_maxHalfAngle);
    const float halfArm = glm::clamp(0.5f * desc.armAngle, -c_maxHalfAngle, c_maxHalfAngle);
    // Legs in opposite phase, each arm opposite to the leg on its side.
    halfAngle[LeftLeg] = halfLeg;
    halfAngle[RightLeg] = -halfLeg;
    halfAngle[LeftArm] = -halfArm;
    halfAngle[RightArm] = halfArm;
    cyclesPerUnit = 1.0f / glm::max(desc.stride, 1e-3f);
    invFullSpeed = 1.0f / glm::max(desc.fullSpeed, 1e-3f);
    fadeRate = 1.0f / glm::max(desc.fadeSeconds, 1e-3f);
    return true;
}

void HumanoidAnimatorComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    rig = info.rig.get();
    enabled = info.enabled;
    if (rig)
        spawnBoneNodes(*rig, base, bones);
}

void HumanoidAnimatorComponent::destroy(Entity& entity, const SpawnInfo& info)
{
}

bool HumanoidAnimatorComponent::beginTick(const Entity& entity, float& deltaSeconds, float& outDistance)
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

// Everything a general animator decides per tick was decided once in HumanoidRig::configure, so this is
// straight-line: one weight, one phase, one sine, then the four limbs as ONE SSE pass over the rig's SoA
// lane. A rotation about Z is the quaternion (0, 0, sin h, cos h) and needs no product; the limbs differ
// only in the SIGN of their half angle, which the odd / even polynomial carries through. ROTATION ONLY:
// the limb mesh's origin is its pivot, so no position moves - the tick writes the four nodeLocal
// quaternions DIRECTLY (there is no separate pose; the placement reads nothing else) and that is all.
void HumanoidAnimatorComponent::update(Entity& entity, float deltaSeconds)
{
    if (!enabled) // `rig` is never null: World makes no component for a setup that does not fit
        return;
    float distance;
    if (!beginTick(entity, deltaSeconds, distance))
        return;
    // AT REST and not moving (a self-driven one; the parent of a driven one does not even feed it): out
    // before the division and the fade.
    if (distance <= 0.0f && walkWeight <= 0.0f)
        return;
    const HumanoidRig& h = *rig;

    const float target = glm::min(distance * h.invFullSpeed / deltaSeconds, 1.0f);
    const float step = h.fadeRate * deltaSeconds;
    const float weight = walkWeight + glm::clamp(target - walkWeight, -step, step);
    walkWeight = weight;
    atRest = weight <= 0.0f; // reached only while moving or fading, so the pose always changes below

    // The phase stays in 0..1: the integer part by truncation (it is never negative).
    const float phase = walkPhase + distance * h.cyclesPerUnit;
    walkPhase = phase - float(int(phase));

    // ONE pass, one lane per limb, on the signed half angles. The sine has no branch (phaseSin).
    __m128 hs, hc;
    BoneMath::polyTrig4(_mm_mul_ps(_mm_loadu_ps(h.halfAngle), _mm_set1_ps(weight * BoneMath::phaseSin(walkPhase))), hs, hc);

    // The quaternions (0, 0, hs, hc): interleave, then put two zeros in front. configure() put the limbs at
    // bones 0..3 in lane order, and their position + scale never change (spawn set them from the bind).
    const __m128 zero = _mm_setzero_ps();
    const __m128 lo = _mm_unpacklo_ps(hs, hc); // hs0 hc0 hs1 hc1
    const __m128 hi = _mm_unpackhi_ps(hs, hc); // hs2 hc2 hs3 hc3
    _mm_storeu_ps(&bones[0].nodeLocal.quat.x, _mm_shuffle_ps(zero, lo, _MM_SHUFFLE(1, 0, 0, 0)));
    _mm_storeu_ps(&bones[1].nodeLocal.quat.x, _mm_shuffle_ps(zero, lo, _MM_SHUFFLE(3, 2, 0, 0)));
    _mm_storeu_ps(&bones[2].nodeLocal.quat.x, _mm_shuffle_ps(zero, hi, _MM_SHUFFLE(1, 0, 0, 0)));
    _mm_storeu_ps(&bones[3].nodeLocal.quat.x, _mm_shuffle_ps(zero, hi, _MM_SHUFFLE(3, 2, 0, 0)));
    placeDirty = true;
}

void HumanoidAnimatorComponent::place(Renderer& renderer, const Transform& world, uint32 passMask)
{
    // Compose again only when a pose changed or the entity's world transform did.
    if (placeDirty || world.pos != placedWorld.pos || world.quat != placedWorld.quat || world.scale != placedWorld.scale)
    {
        placedWorld = world;
        placeDirty = false;
        placeFlatBones(bones, world); // configure() guarantees a flat rig
    }
    if (passMask != 0)
        submitBones(renderer, bones, passMask);
}

void HumanoidAnimatorComponent::addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const
{
    if (rig)
        addBoneRestBounds(*rig, bones, toEntity, bounds, any);
}

const HumanoidAnimatorComponent::SpawnInfo* getHumanoidAnimatorSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<HumanoidAnimatorComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_HumanoidAnimator); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const HumanoidAnimatorComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeHumanoidAnimatorSpawnInfo(const HumanoidAnimatorComponent::SpawnInfo& info, AssetNode& out)
{
    for (const AssetNode& child : info.source.children)
        out.children.push_back(child);
}
