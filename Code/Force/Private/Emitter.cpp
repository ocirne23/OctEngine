module Force;

import Core;
import Core.glm;
import :Emitter;
import :ForceSystem;

// The ForceEmitter handle: every call resolves the instance through Globals::forceSystem (a
// generation-checked index - a stale handle reads as invalid) and touches only that instance.

ForceEmitter& ForceEmitter::operator=(ForceEmitter&& move) noexcept
{
    if (this != &move)
    {
        destroy();
        m_handle = move.m_handle;
        move.m_handle = 0;
    }
    return *this;
}

void ForceEmitter::destroy()
{
    if (m_handle != 0)
    {
        Globals::forceSystem.destroyEmitter(m_handle);
        m_handle = 0;
    }
}

void ForceEmitter::setTransform(const glm::vec3& pos, const glm::vec3& direction)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
    {
        inst->pos = pos;
        inst->dir = direction;
    }
}

void ForceEmitter::setPosition(const glm::vec3& pos)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->pos = pos;
}

void ForceEmitter::setOutput(float output)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->output = output;
}

void ForceEmitter::setActive(bool active)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->active = active;
}

void ForceEmitter::setReach(float reach)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->reach = reach;
}

void ForceEmitter::setFocus(float focus)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->focus = focus;
}

void ForceEmitter::setDistribution(float distribution)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->distribution = distribution;
}

void ForceEmitter::setWidth(float width)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->width = width;
}

void ForceEmitter::setTeam(uint32 team)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->team = glm::min(team, Globals::forceSystem.numTeams() - 1);
}

void ForceEmitter::setShellAlpha(float alpha)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->shellAlpha = glm::clamp(alpha, 0.0f, 1.0f);
}

void ForceEmitter::setAnalyticReadback(bool analytic)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->analyticReadback = analytic;
}

void ForceEmitter::setMergeable(bool mergeable)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->mergeable = mergeable; // a cleared flag makes the next merge pass drop it from its group
}

bool ForceEmitter::getMergeable() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->mergeable;
    return false;
}

bool ForceEmitter::isMerged() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->group != 0;
    return false;
}

bool ForceEmitter::getBubbleBounds(glm::vec3& center, float& radius, uint32* groupId) const
{
    const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle);
    if (!inst || !inst->active)
        return false;
    if (groupId)
        *groupId = inst->group;
    if (inst->group != 0)
    {
        const ForceSystem::MergeGroup& group = Globals::forceSystem.m_groups[inst->group - 1];
        center = group.center;
        radius = group.coverRadius;
    }
    else
    {
        center = inst->bubbleCenter;
        radius = inst->bubbleRadius;
    }
    return radius > 0.0f;
}

glm::vec3 ForceEmitter::getAppliedForce() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->appliedForce;
    return glm::vec3(0.0f);
}

float ForceEmitter::getPressure() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->pressure;
    return 0.0f;
}

float ForceEmitter::getEquilibriumRadius() const
{
    const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle);
    if (!inst)
        return 0.0f;
    // Same fold + gain the upload and the debug rings use; the distNorm cache is refreshed by
    // every update(), so a same-frame focus/distribution setter is at most one frame stale.
    const float W = glm::clamp(inst->width, 0.05f, 4.0f);
    const float folded = inst->output * forceReferenceBudget() / (glm::max(inst->distNormE, 1e-6f) * W * W);
    const float centerDensity = folded * forceDistributionGain(0.5f, glm::clamp(inst->distribution, 0.0f, 1.0f));
    const float threshold = glm::max(Globals::forceSystem.getParams().isoThreshold, inst->pressure);
    if (threshold <= 0.0f || centerDensity <= threshold)
        return 0.0f;
    const float u2 = 1.0f - std::sqrt(threshold / centerDensity);
    return 0.5f * glm::max(inst->reach, 1e-3f) * W * std::sqrt(u2);
}

float ForceEmitter::getCenterDensityFactor() const
{
    const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle);
    if (!inst)
        return 1.0f;
    const float W = glm::clamp(inst->width, 0.05f, 4.0f);
    const float fold = forceReferenceBudget() / (glm::max(inst->distNormE, 1e-6f) * W * W);
    return fold * forceDistributionGain(0.5f, glm::clamp(inst->distribution, 0.0f, 1.0f));
}

float ForceEmitter::getOutput() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->output;
    return 0.0f;
}

float ForceEmitter::getReach() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->reach;
    return 0.0f;
}

float ForceEmitter::getFocus() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->focus;
    return 0.5f;
}

float ForceEmitter::getDistribution() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->distribution;
    return 0.5f;
}

float ForceEmitter::getWidth() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->width;
    return 1.0f;
}

uint32 ForceEmitter::getTeam() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->team;
    return 0;
}

float ForceEmitter::getShellAlpha() const
{
    if (const ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        return inst->shellAlpha;
    return 1.0f;
}
