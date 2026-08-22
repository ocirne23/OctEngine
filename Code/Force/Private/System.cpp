module Force;

import Core;
import Core.glm;
import Core.Tweaks;
import RendererVK;
import :System;

using namespace RendererVKLayout;

// CPU mirror of the shader's axial density bump (force_field.inc.glsl forceDistributionGain).
static float forceDistributionGain(float t, float D)
{
    const float b = (t - D) * (1.0f / 0.45f);
    return 0.15f + std::exp(-b * b);
}

// The emitter's total field budget integral (gain-weighted, in shared quadrature units): the
// lateral integral of the falloff collapses in the warped-sphere coordinates (substituting the
// warp turns each axial station into q^-m * (1-X^2)^3 up to a constant), so this is a cheap 1D
// quadrature. Cached per emitter (depends only on focus + distribution; width scales the total by
// width^2 analytically and reach by reach^3 — reach is deliberately NOT normalized away, so a
// bigger bubble is more total power at the same density, not a fainter one).
static float forceShapeBudget(float focus, float D)
{
    const float m = 1.0f - 2.0f * glm::clamp(focus, 0.0f, 1.0f);
    double sum = 0.0;
    constexpr int NUM_SAMPLES = 64;
    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        const float X = -1.0f + (i + 0.5f) * (2.0f / NUM_SAMPLES);
        const float q = glm::clamp((1.0f - X) / (1.0f + X), 1e-4f, 1e4f);
        sum += std::pow((double)q, (double)-m) * std::pow(1.0 - (double)X * X, 3.0)
            * forceDistributionGain((X + 1.0f) * 0.5f, D);
    }
    return (float)sum;
}

// Reference: the plain gain-free sphere (focus 0.5, width 1) — an emitter with any focus/
// distribution/width carries exactly this shape's total, so Output is a balance-able budget and
// narrowing/pinching visibly DENSIFIES the field instead of shedding power.
static float forceReferenceBudget()
{
    static const float ref = [] {
        double sum = 0.0;
        constexpr int NUM_SAMPLES = 64;
        for (int i = 0; i < NUM_SAMPLES; ++i)
        {
            const float X = -1.0f + (i + 0.5f) * (2.0f / NUM_SAMPLES);
            sum += std::pow(1.0 - (double)X * X, 3.0);
        }
        return (float)sum;
    }();
    return ref;
}

float ForceSystem::refreshDistributionScale(EmitterInstance& inst) const
{
    const float f = glm::clamp(inst.focus, 0.0f, 1.0f);
    const float D = glm::clamp(inst.distribution, 0.0f, 1.0f);
    if (inst.distNormFocus != f || inst.distNormD != D)
    {
        inst.distNormE = forceShapeBudget(f, D);
        inst.distNormFocus = f;
        inst.distNormD = D;
    }
    const float W = glm::clamp(inst.width, 0.05f, 4.0f);
    return forceReferenceBudget() / (glm::max(inst.distNormE, 1e-6f) * W * W);
}

static uint32 packDebugColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

// ---- ForceEmitter handle ----

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
        inst->team = glm::min(team, MAX_FORCE_TEAMS - 1);
}

void ForceEmitter::setShellAlpha(float alpha)
{
    if (ForceSystem::EmitterInstance* inst = Globals::forceSystem.resolveEmitter(m_handle))
        inst->shellAlpha = glm::clamp(alpha, 0.0f, 1.0f);
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

// ---- ForceQuery handle ----

ForceQuery& ForceQuery::operator=(ForceQuery&& move) noexcept
{
    if (this != &move)
    {
        destroy();
        m_handle = move.m_handle;
        move.m_handle = 0;
    }
    return *this;
}

void ForceQuery::destroy()
{
    if (m_handle != 0)
    {
        Globals::forceSystem.destroyQuery(m_handle);
        m_handle = 0;
    }
}

void ForceQuery::setPosition(const glm::vec3& pos)
{
    if (ForceSystem::QueryInstance* inst = Globals::forceSystem.resolveQuery(m_handle))
        inst->pos = pos;
}

ForceQuery::Result ForceQuery::getResult() const
{
    if (ForceSystem::QueryInstance* inst = Globals::forceSystem.resolveQuery(m_handle))
        return inst->result;
    return Result{};
}

// ---- ForceSystem ----

void ForceSystem::initialize()
{
    Tweak::boolean("Force", "Enabled", &m_params.enabled);
    Tweak::floatVar("Force", "Iso threshold", &m_params.isoThreshold, 0.01f, 2.0f);
    Tweak::intVar("Force", "March steps", &m_params.marchSteps, 8, 128);
    Tweak::floatVar("Force", "Big reach threshold (m)", &m_params.bigReachThreshold, 8.0f, 512.0f, 1.0f);
    Tweak::boolean("Force", "Use grid", &m_params.useGrid); // off = brute force (A-B correctness check)
    Tweak::floatVar("Force", "Force gain", &m_params.forceGain, 0.0f, 10.0f);
    Tweak::floatVar("Force/Shell", "Alpha", &m_params.shellAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Interior alpha", &m_params.interiorAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Backface alpha", &m_params.backfaceAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Rim power", &m_params.rimPower, 0.5f, 8.0f);
    Tweak::floatVar("Force/Shell", "Rim intensity", &m_params.rimIntensity, 0.0f, 8.0f);
    Tweak::floatVar("Force/Glow", "Contact intensity", &m_params.contactGlowIntensity, 0.0f, 16.0f);
    Tweak::floatVar("Force/Glow", "Contact width", &m_params.contactGlowWidth, 0.01f, 1.0f);
    Tweak::floatVar("Force/Glow", "Contact wall alpha", &m_params.contactWallAlpha, 0.0f, 1.0f);
    Tweak::floatVar("Force/Shell", "Junction smoothing", &m_params.junctionSmoothing, 0.0f, 2.0f);
    Tweak::floatVar("Force/Glow", "Geometry distance (m)", &m_params.geoGlowDistance, 0.0f, 4.0f);
    Tweak::floatVar("Force/Pattern", "Scale (1/m)", &m_params.patternScale, 0.01f, 8.0f);
    Tweak::floatVar("Force/Pattern", "Scroll speed", &m_params.patternSpeed, 0.0f, 4.0f);
    Tweak::floatVar("Force/Pattern", "Intensity", &m_params.patternIntensity, 0.0f, 4.0f);
    static const char* teamNames[MAX_FORCE_TEAMS] = { "Team 0", "Team 1", "Team 2", "Team 3", "Team 4", "Team 5", "Team 6", "Team 7" };
    for (uint32 i = 0; i < MAX_FORCE_TEAMS; ++i)
        Tweak::color3("Force/Teams", teamNames[i], &m_params.teamColors[i]);
    m_pairStaging.initialize();
    m_candidateStaging.initialize();
    Tweak::boolean("Force/Merge", "Enabled", &m_merge.enabled);
    Tweak::floatVar("Force/Merge", "Join distance (x radii)", &m_merge.joinDistance, 0.0f, 1.5f, 0.01f);
    Tweak::floatVar("Force/Merge", "Leave distance (x radii)", &m_merge.leaveDistance, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover spread scale", &m_merge.spreadScale, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover radius scale", &m_merge.radiusScale, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover scale", &m_merge.coverScale, 0.1f, 2.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Cover margin (m)", &m_merge.coverMargin, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Force/Merge", "Max group radius (m)", &m_merge.maxRadius, 1.0f, 256.0f, 0.5f);
    Tweak::intVar("Force/Merge", "Max members", &m_merge.maxMembers, 2, 1024);
    Tweak::intVar("Force/Merge", "Min members", &m_merge.minMembers, 2, 64);
    Tweak::floatVar("Force/Merge", "Summed output fraction", &m_merge.sumFraction, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Force/Merge", "Member readback", &m_merge.memberReadback);
    Tweak::floatVar("Force/Merge", "Smooth time (s)", &m_merge.smoothTime, 0.0f, 3.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Blend time (s)", &m_merge.blendTime, 0.01f, 3.0f, 0.01f);
    Tweak::floatVar("Force/Merge", "Leave from group sphere", &m_merge.leaveFromGroup, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Force/Merge", "Groups (stat)", &m_statGroups, 0, 100000);
    Tweak::intVar("Force/Merge", "Merged emitters (stat)", &m_statMerged, 0, 100000);
    Tweak::boolean("Force/Debug", "Draw emitters", &m_debugDraw);
    Tweak::boolean("Force/Debug", "Draw merge groups", &m_debugDrawGroups);
    Tweak::boolean("Force/Debug", "Draw queries", &m_debugDrawQueries);
    Tweak::boolean("Force/Debug", "Density view", &m_params.densityView);
    Tweak::floatVar("Force/Debug", "Density range", &m_params.densityRange, 0.1f, 10.0f, 0.05f);
}

// outputScale = the distribution budget fold (refreshDistributionScale): the GPU sees Output
// pre-divided by the gain's field-weighted mean, so total emitted field stays exactly conserved.
// flags: FORCE_FLAG_ACTIVE for a live field, | FORCE_FLAG_PASSIVE for a merged member that only
// wants its own readback, 0 for a merged member the compaction should skip entirely.
static ForceEmitterGpu buildEmitterGpu(const glm::vec3& pos, const glm::vec3& dir, float output,
    float reach, float focus, uint32 team, float distribution, float width, float outputScale,
    float shellAlpha, uint32 flags = FORCE_FLAG_ACTIVE)
{
    ForceEmitterGpu gpu;
    gpu.posReach = glm::vec4(pos, glm::max(reach, 1e-3f));
    const glm::vec3 d = glm::dot(dir, dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0.0f, 1.0f, 0.0f);
    gpu.dirFocus = glm::vec4(d, glm::clamp(focus, 0.0f, 1.0f));
    gpu.outputParams = glm::vec4(glm::max(output, 0.0f) * outputScale, glm::clamp(shellAlpha, 0.0f, 1.0f),
        glm::clamp(distribution, 0.0f, 1.0f), glm::clamp(width, 0.05f, 4.0f));
    gpu.teamFlags = glm::uvec4(glm::min(team, MAX_FORCE_TEAMS - 1), flags, 0u, 0u);
    return gpu;
}

// Closed-form iso lateral half-width at axial station t of the warped-sphere shape (mirrors
// forceContribution): O' g(t) (1 - u^2)^2 = iso -> u^2 = 1 - sqrt(iso / (O' g)), lateral =
// (R/2) W sqrt((u^2 - X^2) q^-m) where positive; X = 2t - 1, q = (1-X)/(1+X), m = 1 - 2 focus.
static float forceIsoLateral(float t, float R, float m, float W, float D, float foldedOutput, float iso)
{
    const float peak = foldedOutput * forceDistributionGain(t, D);
    if (peak <= iso)
        return 0.0f; // density trough below iso: no surface at this station
    const float u2 = 1.0f - std::sqrt(iso / peak);
    const float X = t * 2.0f - 1.0f;
    const float y2 = u2 - X * X;
    if (y2 <= 0.0f)
        return 0.0f;
    const float q = glm::clamp((1.0f - X) / (1.0f + X), 1e-4f, 1e4f);
    return 0.5f * R * W * std::sqrt(y2 * std::pow(q, -m));
}

ForceEmitter ForceSystem::createEmitter(uint32 team, const glm::vec3& pos, const glm::vec3& direction,
    float output, float reach, float focus, float distribution, float width)
{
    EmitterInstance staged;
    staged.focus = focus;
    staged.distribution = distribution;
    const float outputScale = refreshDistributionScale(staged);
    const uint32 slot = Globals::rendererVK.createForceEmitter(
        buildEmitterGpu(pos, direction, output, reach, focus, team, distribution, width, outputScale, 1.0f));
    if (slot == UINT32_MAX)
    {
        printf("ForceSystem: out of force emitter slots (%u live)\n", m_numLiveEmitters);
        return ForceEmitter();
    }
    uint32 idx;
    if (!m_freeEmitters.empty())
    {
        idx = m_freeEmitters.back();
        m_freeEmitters.pop_back();
    }
    else
    {
        m_emitters.emplace_back();
        idx = (uint32)m_emitters.size() - 1;
    }
    EmitterInstance& inst = m_emitters[idx];
    inst = EmitterInstance{};
    inst.generation = m_generationCounter++;
    if (m_generationCounter == 0)
        m_generationCounter = 1;
    inst.rendererSlot = slot;
    inst.team = glm::min(team, MAX_FORCE_TEAMS - 1);
    inst.output = output;
    inst.reach = reach;
    inst.focus = focus;
    inst.pos = pos;
    inst.dir = direction;
    inst.distribution = distribution;
    inst.width = width;
    inst.distNormE = staged.distNormE;
    inst.distNormFocus = staged.distNormFocus;
    inst.distNormD = staged.distNormD;
    ++m_numLiveEmitters;
    return ForceEmitter(((uint64)inst.generation << 32) | idx);
}

ForceQuery ForceSystem::createQuery(const glm::vec3& pos)
{
    const uint32 slot = Globals::rendererVK.createForceQuerySlot();
    if (slot == UINT32_MAX)
    {
        printf("ForceSystem: out of force query slots\n");
        return ForceQuery();
    }
    uint32 idx;
    if (!m_freeQueries.empty())
    {
        idx = m_freeQueries.back();
        m_freeQueries.pop_back();
    }
    else
    {
        m_queries.emplace_back();
        idx = (uint32)m_queries.size() - 1;
    }
    QueryInstance& inst = m_queries[idx];
    inst = QueryInstance{};
    inst.generation = m_generationCounter++;
    if (m_generationCounter == 0)
        m_generationCounter = 1;
    inst.rendererSlot = slot;
    inst.pos = pos;
    return ForceQuery(((uint64)inst.generation << 32) | idx);
}

ForceSystem::EmitterInstance* ForceSystem::resolveEmitter(uint64 handle)
{
    const uint32 idx = (uint32)handle;
    const uint32 gen = (uint32)(handle >> 32);
    if (gen != 0 && idx < m_emitters.size() && m_emitters[idx].generation == gen)
        return &m_emitters[idx];
    return nullptr;
}

const ForceSystem::EmitterInstance* ForceSystem::resolveEmitter(uint64 handle) const
{
    return const_cast<ForceSystem*>(this)->resolveEmitter(handle);
}

ForceSystem::QueryInstance* ForceSystem::resolveQuery(uint64 handle)
{
    const uint32 idx = (uint32)handle;
    const uint32 gen = (uint32)(handle >> 32);
    if (gen != 0 && idx < m_queries.size() && m_queries[idx].generation == gen)
        return &m_queries[idx];
    return nullptr;
}

void ForceSystem::destroyEmitter(uint64 handle)
{
    if (EmitterInstance* inst = resolveEmitter(handle))
    {
        Globals::rendererVK.destroyForceEmitter(inst->rendererSlot);
        inst->generation = 0;
        inst->group = 0; // its group prunes the stale index on the next merge pass
        m_freeEmitters.push_back((uint32)handle);
        --m_numLiveEmitters;
    }
}

void ForceSystem::destroyQuery(uint64 handle)
{
    if (QueryInstance* inst = resolveQuery(handle))
    {
        Globals::rendererVK.destroyForceQuerySlot(inst->rendererSlot);
        inst->generation = 0;
        m_freeQueries.push_back((uint32)handle);
    }
}

// The plain sphere's budget fold (focus 0.5 / distribution 0.5 / width 1) — group and transition
// spheres all use it; constants only, so computed once.
static float forceSphereFold()
{
    static const float fold = forceReferenceBudget() / forceShapeBudget(0.5f, 0.5f);
    return fold;
}

float ForceSystem::sphereReach(float radius, float output) const
{
    const float centerDensity = output * forceSphereFold() * forceDistributionGain(0.5f, 0.5f);
    const float iso = m_params.isoThreshold;
    if (radius <= 0.0f || centerDensity <= iso * 1.001f)
        return 0.0f;
    const float u2 = 1.0f - std::sqrt(iso / centerDensity); // visible radius = (R/2) sqrt(u2)
    return 2.0f * radius / std::sqrt(u2);
}

void ForceSystem::joinMerge()
{
    if (!m_mergeKicked)
        return;
    ProfileScope joinScope("Force merge join", EProfileCategory::Wait);
    Globals::jobSystem.wait(m_mergeCounter);
    m_mergeKicked = false;
}

void ForceSystem::update(Renderer& renderer, float deltaSec)
{
    ProfileScope profileScope("Force", EProfileCategory::Force);
    joinMerge(); // normally already joined at the loop top ("Force merge join", Wait); a guarantee, not the expected path
    {
        ProfileScope prepareScope("Force prepare", EProfileCategory::Force); // params push + the job's retired group slots
        renderer.setForceFieldParams(m_params);
        for (const uint32 slot : m_retiredGroupSlots)
            renderer.destroyForceEmitter(slot);
        m_retiredGroupSlots.clear();
    }
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float blendStep = deltaSec / glm::max(m_merge.blendTime, 1e-3f);
    // Per-emitter upload on jobs: each iteration touches only its own instance, its own renderer
    // slot (distinct vector elements, no growth — create/destroy are main-thread outside this),
    // the read-only readback span and the PerWorker-staged debug lines.
    Globals::jobSystem.parallelFor(0u, (uint32)m_emitters.size(), 64u, JobProfile{ "Force upload", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
    for (uint32 emitterIdx = begin; emitterIdx < end; ++emitterIdx)
    {
        EmitterInstance& inst = m_emitters[emitterIdx];
        if (inst.generation == 0)
            continue;
        // Transition spheres: advance the blend, pick the live target (the group's displayed
        // sphere when Joining, the own bubble when Leaving), resolve the end states.
        if (inst.mergeState == EmitterInstance::EMergeState::Joining && inst.group == 0)
            inst.mergeState = EmitterInstance::EMergeState::Leaving; // group vanished under it
        glm::vec3 sphereCenter(0.0f);
        float sphereRadius = 0.0f;
        if (inst.mergeState == EmitterInstance::EMergeState::Joining || inst.mergeState == EmitterInstance::EMergeState::Leaving)
        {
            inst.blend = glm::min(inst.blend + blendStep, 1.0f);
            const bool joining = inst.mergeState == EmitterInstance::EMergeState::Joining;
            const glm::vec3 toCenter = joining ? m_groups[inst.group - 1].center : inst.bubbleCenter;
            const float toRadius = joining ? m_groups[inst.group - 1].coverRadius : inst.bubbleRadius;
            const float s = inst.blend * inst.blend * (3.0f - 2.0f * inst.blend);
            sphereCenter = glm::mix(inst.blendFromCenter, toCenter, s);
            sphereRadius = glm::mix(inst.blendFromRadius, toRadius, s);
            inst.blendCenter = sphereCenter;
            inst.blendRadius = sphereRadius;
            if (inst.blend >= 1.0f)
            {
                if (!joining)
                    inst.mergeState = EmitterInstance::EMergeState::Own;
                else if (memberCover(inst, toCenter) <= toRadius)
                    inst.mergeState = EmitterInstance::EMergeState::Merged; // the group sphere covers it now
            }
        }
        // A Merged member projects no field: it uploads PASSIVE (the force compute still evaluates
        // its own slot against the group's field) or not at all (the group's readback is split
        // among the members below).
        const bool merged = inst.mergeState == EmitterInstance::EMergeState::Merged;
        const bool transition = inst.mergeState == EmitterInstance::EMergeState::Joining
            || inst.mergeState == EmitterInstance::EMergeState::Leaving;
        const uint32 flags = !merged ? FORCE_FLAG_ACTIVE
            : (m_merge.memberReadback ? (FORCE_FLAG_ACTIVE | FORCE_FLAG_PASSIVE) : 0u);
        const float transitionReach = transition ? sphereReach(sphereRadius, inst.output) : 0.0f;
        EmitterInstance sphere; // the transition sphere as an instance (upload + debug rings)
        if (transitionReach > 0.0f)
        {
            sphere.team = inst.team;
            sphere.output = inst.output;
            sphere.reach = transitionReach;
            sphere.focus = 0.5f;
            sphere.distribution = 0.5f;
            sphere.width = 1.0f;
            sphere.shellAlpha = inst.shellAlpha;
            sphere.dir = up;
            sphere.pos = sphereCenter - up * (transitionReach * 0.5f);
        }
        const EmitterInstance& src = transitionReach > 0.0f ? sphere : inst;
        renderer.updateForceEmitter(inst.rendererSlot,
            buildEmitterGpu(src.pos, src.dir, src.output, src.reach, src.focus, src.team,
                src.distribution, src.width, refreshDistributionScale(transitionReach > 0.0f ? sphere : inst),
                src.shellAlpha, flags));
        // Latch the GPU force readback (slot-indexed, ~2 frames old; zero until the first lands).
        // "Force gain" applies HERE, not on the GPU: the compute writes the raw integral, so the
        // tweak takes effect instantly on the CPU read instead of riding the readback latency.
        if (flags != 0u)
        {
            const glm::vec4 readback = renderer.getForceEmitterReadback(inst.rendererSlot);
            inst.appliedForce = glm::vec3(readback) * m_params.forceGain;
            inst.pressure = readback.w;
        }
        if (m_debugDraw && !merged)
            debugDrawEmitter(renderer, src);
    }
    });
    ProfileScope groupsScope("Force groups upload", EProfileCategory::Force);
    for (MergeGroup& group : m_groups)
    {
        if (group.generation == 0)
            continue;
        if (group.rendererSlot == UINT32_MAX) // founded on the job: the slot is minted here, on main
        {
            group.rendererSlot = renderer.createForceEmitter(buildEmitterGpu(group.center, up, 0.0f, 1.0f, 0.5f,
                group.team, 0.5f, 1.0f, 1.0f, 1.0f, 0u));
            if (group.rendererSlot == UINT32_MAX)
                continue; // out of slots this frame: members still carry their transition spheres
        }
        // The group sphere: focus 0.5 / distribution 0.5 / width 1, axis up, centred on `center`.
        static const float sphereScale = [this] {
            EmitterInstance sphere;
            sphere.focus = 0.5f;
            sphere.distribution = 0.5f;
            return refreshDistributionScale(sphere);
        }();
        renderer.updateForceEmitter(group.rendererSlot,
            buildEmitterGpu(group.center - up * (group.reach * 0.5f), up, group.output, group.reach, 0.5f,
                group.team, 0.5f, 1.0f, sphereScale, group.shellAlpha));
        const glm::vec4 readback = renderer.getForceEmitterReadback(group.rendererSlot);
        group.appliedForce = glm::vec3(readback) * m_params.forceGain;
        group.pressure = readback.w;
        if (!m_merge.memberReadback)
        {
            // Shared readback: the group's integral split by output share (getAppliedForce scales
            // with the emitter's own output, which consumers normalize by), pressure as measured.
            const float invSum = 1.0f / glm::max(group.sumOutput, 1e-6f);
            for (const uint32 idx : group.members)
            {
                EmitterInstance& member = m_emitters[idx];
                if (member.mergeState != EmitterInstance::EMergeState::Merged)
                    continue; // a Joining member still has its own active transition sphere
                member.appliedForce = group.appliedForce * (glm::max(member.output, 0.0f) * invSum);
                member.pressure = group.pressure;
            }
        }
        if (m_debugDrawGroups)
            debugDrawGroup(renderer, group);
    }
    groupsScope.stop();
    ProfileScope queriesScope("Force queries", EProfileCategory::Force);
    for (QueryInstance& query : m_queries)
    {
        if (query.generation == 0)
            continue;
        renderer.setForceQuery(query.rendererSlot, query.pos);
        const RendererVKLayout::ForceQueryResult result = renderer.getForceQueryReadback(query.rendererSlot);
        query.result.valid = result.frameStamp != 0u;
        query.result.inside = result.owningTeam < MAX_FORCE_TEAMS;
        query.result.owningTeam = query.result.inside ? result.owningTeam : 0u;
        query.result.ownField = result.ownField;
        query.result.opposingField = result.bestOpposingField;
        if (m_debugDrawQueries)
        {
            const uint32 color = query.result.inside
                ? packDebugColor(m_params.teamColors[query.result.owningTeam]) : 0xFF404040u;
            const float s = 0.25f;
            renderer.addDebugLine(query.pos - glm::vec3(s, 0, 0), query.pos + glm::vec3(s, 0, 0), color);
            renderer.addDebugLine(query.pos - glm::vec3(0, s, 0), query.pos + glm::vec3(0, s, 0), color);
            renderer.addDebugLine(query.pos - glm::vec3(0, 0, s), query.pos + glm::vec3(0, 0, s), color);
        }
    }
    queriesScope.stop();
    // Kick next frame's merge over this frame's state: it runs during present + the fence wait
    // and is joined at the loop top (joinMerge) before anything can create/destroy emitters.
    ProfileScope kickScope("Force merge kick", EProfileCategory::Force);
    m_mergeDeltaSec = deltaSec;
    Globals::jobSystem.submit([this] { updateMerging(m_mergeDeltaSec); },
        { "Force merge", EProfileCategory::Force }, EJobPriority::Normal, &m_mergeCounter);
    m_mergeKicked = true;
}

// Draws the emitter's UNCONTESTED iso surface (what its bubble looks like alone): the closed-form
// profile of the warped-sphere shape solved for the iso threshold — four half-profiles in the two
// axial planes + a circle at the widest station + the output line pos -> target. Deformation
// against other bubbles only exists in the field evaluation — this is the authoring view of
// reach/focus/distribution, not the equilibrium surface.
void ForceSystem::debugDrawEmitter(Renderer& renderer, const EmitterInstance& inst) const
{
    const glm::vec3 dir = glm::dot(inst.dir, inst.dir) > 1e-6f ? glm::normalize(inst.dir) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 ref = std::abs(dir.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(dir, ref));
    const glm::vec3 up = glm::cross(right, dir);
    const uint32 color = packDebugColor(m_params.teamColors[glm::min(inst.team, MAX_FORCE_TEAMS - 1)]);

    const float R = glm::max(inst.reach, 1e-3f);
    const float m = 1.0f - 2.0f * glm::clamp(inst.focus, 0.0f, 1.0f);
    const float W = glm::clamp(inst.width, 0.05f, 4.0f);
    // Same fold the upload applies (cache is fresh: update() refreshed it before drawing).
    const float foldedOutput = inst.output * forceReferenceBudget() / (glm::max(inst.distNormE, 1e-6f) * W * W);
    const float D = glm::clamp(inst.distribution, 0.0f, 1.0f);
    const auto isoLateral = [&](float t) { return forceIsoLateral(t, R, m, W, D, foldedOutput, m_params.isoThreshold); };

    constexpr int STATIONS = 32;
    float maxLat = 0.0f;
    float maxLatT = 0.5f;
    for (const glm::vec3& planeAxis : { right, up })
    {
        for (const float side : { 1.0f, -1.0f })
        {
            glm::vec3 prev = inst.pos;
            for (int i = 1; i <= STATIONS; ++i)
            {
                const float t = (float)i / STATIONS;
                const float lat = isoLateral(t);
                const glm::vec3 p = inst.pos + dir * (t * R) + planeAxis * (side * lat);
                renderer.addDebugLine(prev, p, color);
                prev = p;
                if (lat > maxLat)
                {
                    maxLat = lat;
                    maxLatT = t;
                }
            }
        }
    }
    if (maxLat > 0.0f)
    {
        constexpr int SEG = 32;
        const glm::vec3 ringCenter = inst.pos + dir * (maxLatT * R);
        glm::vec3 prev = ringCenter + right * maxLat;
        for (int i = 1; i <= SEG; ++i)
        {
            const float phi = (6.2831853f / SEG) * i;
            const glm::vec3 p = ringCenter + (right * std::cos(phi) + up * std::sin(phi)) * maxLat;
            renderer.addDebugLine(prev, p, color);
            prev = p;
        }
    }
    renderer.addDebugLine(inst.pos, inst.pos + dir * R, color); // the output line (emitter -> target)
}

// ---- merging ----

static float forceDist2(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = a - b;
    return glm::dot(d, d);
}

// Bounding sphere of the emitter's UNCONTESTED iso bubble: centre at the output line's midpoint,
// radius = the farthest iso-profile point over sampled axial stations (+2% slack for the sampling).
// Works for any shape; radius 0 = no bubble above iso (e.g. a collapsed shield's 0.01 output),
// which keeps the emitter out of every group.
// Runs on a job (one emitter per call, writes only its own instance + the worker's staging list).
void ForceSystem::refreshBubbleBounds(EmitterInstance& inst)
{
    refreshDistributionScale(inst); // distNorm cache fresh before the fold below
    const float R = glm::max(inst.reach, 1e-3f);
    const glm::vec3 dir = glm::dot(inst.dir, inst.dir) > 1e-6f ? glm::normalize(inst.dir) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 center = inst.pos + dir * (R * 0.5f);
    inst.prevBubbleCenter = inst.bubbleValid ? inst.bubbleCenter : center;
    inst.bubbleValid = true;
    inst.bubbleCenter = center;
    const float iso = m_params.isoThreshold;
    if (inst.boundsOutput != inst.output || inst.boundsReach != inst.reach || inst.boundsFocus != inst.focus
        || inst.boundsDist != inst.distribution || inst.boundsWidth != inst.width || inst.boundsIso != iso)
    {
        inst.boundsOutput = inst.output;
        inst.boundsReach = inst.reach;
        inst.boundsFocus = inst.focus;
        inst.boundsDist = inst.distribution;
        inst.boundsWidth = inst.width;
        inst.boundsIso = iso;
        const float m = 1.0f - 2.0f * glm::clamp(inst.focus, 0.0f, 1.0f);
        const float W = glm::clamp(inst.width, 0.05f, 4.0f);
        const float D = glm::clamp(inst.distribution, 0.0f, 1.0f);
        const float foldedOutput = inst.output * forceReferenceBudget() / (glm::max(inst.distNormE, 1e-6f) * W * W);
        float r2 = 0.0f;
        constexpr int STATIONS = 16;
        for (int i = 0; i <= STATIONS; ++i)
        {
            const float t = (float)i / STATIONS;
            const float lat = forceIsoLateral(t, R, m, W, D, foldedOutput, iso);
            if (lat <= 0.0f)
                continue;
            const float axial = (t - 0.5f) * R;
            r2 = glm::max(r2, lat * lat + axial * axial);
        }
        inst.bubbleRadius = r2 > 0.0f ? std::sqrt(r2) * 1.02f : 0.0f;
    }
    // A candidate is mergeable, has a bubble, and could fit SOME group at all (its own cover term
    // under "Max group radius") — a map-scale emitter would otherwise stretch the candidate cells.
    inst.candidate = inst.mergeable && inst.bubbleRadius > 0.0f
        && m_merge.radiusScale * inst.bubbleRadius * m_merge.coverScale + m_merge.coverMargin <= m_merge.maxRadius;
    if (inst.candidate)
    {
        m_candidateStaging.local().push_back((uint32)(&inst - m_emitters.data()));
        const float joinRadius = glm::max(m_merge.joinDistance, 0.0f) * inst.bubbleRadius;
        const uint32 bits = std::bit_cast<uint32>(joinRadius);
        uint32 seen = m_maxJoinRadiusBits.load(oc::memory_order_relaxed);
        while (bits > seen && !m_maxJoinRadiusBits.compare_exchange_weak(seen, bits, oc::memory_order_relaxed)) {}
    }
}

// Candidate cell key: 21-bit signed cell coords packed into 64 bits.
static uint64 forceCellKey(const glm::vec3& p, float invCell)
{
    const glm::ivec3 c = glm::ivec3(glm::floor(p * invCell)) + (1 << 20);
    return ((uint64)(c.x & 0x1FFFFF) << 42) | ((uint64)(c.y & 0x1FFFFF) << 21) | (uint64)(c.z & 0x1FFFFF);
}

// Runs on the merge job: no renderer access — update() allocates the renderer slot on main.
uint32 ForceSystem::createGroup(uint32 team)
{
    uint32 idx;
    if (!m_freeGroups.empty())
    {
        idx = m_freeGroups.back();
        m_freeGroups.pop_back();
    }
    else
    {
        m_groups.emplace_back();
        idx = (uint32)m_groups.size() - 1;
    }
    MergeGroup& group = m_groups[idx];
    group.members.clear();
    group.generation = m_generationCounter++;
    if (m_generationCounter == 0)
        m_generationCounter = 1;
    group.rendererSlot = UINT32_MAX;
    group.team = team;
    group.center = group.targetCenter = glm::vec3(0.0f);
    group.coverRadius = group.reach = group.output = group.sumOutput = 0.0f;
    group.targetRadius = group.targetOutput = 0.0f;
    group.shellAlpha = 1.0f;
    group.appliedForce = glm::vec3(0.0f);
    group.pressure = 0.0f;
    return idx;
}

// Every live member gets a field back where the group sphere stands (a caller moving members to
// another group clears the list first).
void ForceSystem::dissolveGroup(uint32 groupIdx)
{
    MergeGroup& group = m_groups[groupIdx];
    for (const uint32 idx : group.members)
        if (idx < m_emitters.size() && m_emitters[idx].group == groupIdx + 1)
            beginLeave(m_emitters[idx], group.center, group.coverRadius);
    group.members.clear();
    if (group.rendererSlot != UINT32_MAX)
        m_retiredGroupSlots.push_back(group.rendererSlot); // destroyed on main in update()
    group.rendererSlot = UINT32_MAX;
    group.generation = 0;
    m_freeGroups.push_back(groupIdx);
}

// Transitions restart from wherever the emitter's field currently IS: the own bubble (Own), the
// sphere uploaded last frame (mid-transition), or the group sphere the caller passes (Merged).
void ForceSystem::beginJoin(EmitterInstance& inst, uint32 groupIdx, const glm::vec3& fromCenter, float fromRadius)
{
    const bool midTransition = inst.mergeState == EmitterInstance::EMergeState::Joining
        || inst.mergeState == EmitterInstance::EMergeState::Leaving;
    inst.blendFromCenter = midTransition ? inst.blendCenter : fromCenter;
    inst.blendFromRadius = midTransition ? inst.blendRadius : fromRadius;
    inst.blendCenter = inst.blendFromCenter;
    inst.blendRadius = inst.blendFromRadius;
    inst.blend = 0.0f;
    inst.mergeState = EmitterInstance::EMergeState::Joining;
    inst.group = groupIdx + 1;
}

void ForceSystem::beginLeave(EmitterInstance& inst, const glm::vec3& groupCenter, float groupRadius)
{
    const bool midTransition = inst.mergeState == EmitterInstance::EMergeState::Joining
        || inst.mergeState == EmitterInstance::EMergeState::Leaving;
    // "Leave from group sphere" picks the start between the own bubble (0) and the group sphere (1).
    const float f = glm::clamp(m_merge.leaveFromGroup, 0.0f, 1.0f);
    inst.blendFromCenter = midTransition ? inst.blendCenter : glm::mix(inst.bubbleCenter, groupCenter, f);
    inst.blendFromRadius = midTransition ? inst.blendRadius : glm::mix(inst.bubbleRadius, groupRadius, f);
    inst.blendCenter = inst.blendFromCenter;
    inst.blendRadius = inst.blendFromRadius;
    inst.blend = 0.0f;
    inst.mergeState = EmitterInstance::EMergeState::Leaving;
    inst.group = 0;
}

// Exponential ease of the displayed sphere toward the target; the radius is FLOORED by the cover
// of the Merged members at the displayed centre (they project no field of their own — Joining
// members still carry their transition sphere, so they may wait for the growth).
void ForceSystem::smoothGroup(MergeGroup& group, float deltaSec)
{
    // Follow the members' own MOTION 1:1 (output-weighted mean displacement of the members that
    // were already in the group last frame — a just-joined member still has blend 0), so a moving
    // crowd carries its sphere along instead of towing it on a time constant; the ease below then
    // only absorbs the membership-induced jumps of the centroid.
    glm::vec3 motion(0.0f);
    float motionWeight = 0.0f;
    for (const uint32 idx : group.members)
    {
        const EmitterInstance& m = m_emitters[idx];
        if (m.mergeState == EmitterInstance::EMergeState::Joining && m.blend <= 0.0f)
            continue;
        const float w = glm::max(m.output, 1e-4f);
        motion += (m.bubbleCenter - m.prevBubbleCenter) * w;
        motionWeight += w;
    }
    if (motionWeight > 0.0f)
        group.center += motion / motionWeight;
    const float k = m_merge.smoothTime > 1e-4f ? 1.0f - std::exp(-deltaSec / m_merge.smoothTime) : 1.0f;
    group.center = glm::mix(group.center, group.targetCenter, k);
    group.coverRadius = glm::mix(group.coverRadius, group.targetRadius, k);
    group.output = glm::mix(group.output, group.targetOutput, k);
    float floorRadius = 0.0f;
    for (const uint32 idx : group.members)
    {
        const EmitterInstance& m = m_emitters[idx];
        if (m.mergeState == EmitterInstance::EMergeState::Merged)
            floorRadius = glm::max(floorRadius, memberCover(m, group.center));
    }
    group.coverRadius = glm::max(group.coverRadius, floorRadius * m_merge.coverScale + m_merge.coverMargin);
    group.reach = sphereReach(group.coverRadius, group.output);
}

// Group TARGET sphere from the members: centre = output-weighted centroid of the bubble centres,
// iso radius = the cover of every member bubble + margin, output = max(sum * fraction, the densest
// member's centre density) so the merged bubble is never fainter than any member. smoothGroup
// eases the displayed sphere there and solves its reach so the visible radius equals the cover.
bool ForceSystem::recomputeCover(MergeGroup& group)
{
    float sumOut = 0.0f;
    float maxOut = 0.0f;
    float maxCenterDensity = 0.0f;
    float maxAlpha = 0.0f;
    glm::vec3 centroid(0.0f);
    for (const uint32 idx : group.members)
    {
        const EmitterInstance& m = m_emitters[idx];
        const float w = glm::max(m.output, 1e-4f);
        sumOut += w;
        maxOut = glm::max(maxOut, w);
        const float W = glm::clamp(m.width, 0.05f, 4.0f);
        maxCenterDensity = glm::max(maxCenterDensity, w * forceReferenceBudget() / (glm::max(m.distNormE, 1e-6f) * W * W)
            * forceDistributionGain(0.5f, glm::clamp(m.distribution, 0.0f, 1.0f)));
        maxAlpha = glm::max(maxAlpha, m.shellAlpha);
        centroid += m.bubbleCenter * w;
    }
    if ((int)group.members.size() < glm::max(m_merge.minMembers, 1))
        return false;
    centroid /= sumOut;
    float cover = 0.0f;
    for (const uint32 idx : group.members)
    {
        const EmitterInstance& m = m_emitters[idx];
        cover = glm::max(cover, memberCover(m, centroid));
    }
    cover = cover * m_merge.coverScale + m_merge.coverMargin;

    const float sphereGain = forceDistributionGain(0.5f, 0.5f);
    float output = glm::max(sumOut * m_merge.sumFraction, maxOut);
    output = glm::max(output, maxCenterDensity / (forceSphereFold() * sphereGain));
    if (sphereReach(cover, output) <= 0.0f)
        return false;
    group.targetCenter = centroid;
    group.targetRadius = cover;
    group.targetOutput = output;
    group.sumOutput = sumOut;
    group.shellAlpha = maxAlpha;
    return true;
}

// The body of the "Force merge" job (see joinMerge). Every pass that is per-emitter or per-group
// is a runPass (inline when small, parallelFor when not): an emitter's bounds refresh writes only
// its own instance + its worker's staging list, a group's leave/cover pass touches only its own
// members (an emitter belongs to at most one group), and the neighbour search reads the sorted
// candidate cells and stages pairs per worker. Only the candidate sort, the pair UNION (group
// creation / membership moves across groups) and the dissolve sweep are serial — their cost is
// the number of candidates and join-distance PAIRS, not the emitter count. No renderer access here.
void ForceSystem::updateMerging(float deltaSec)
{
    const uint32 numEmitters = (uint32)m_emitters.size();
    const uint32 numGroups = (uint32)m_groups.size();

    // 1. Bubble bounds for every live emitter (cached profile; the transition targets need fresh
    // centres even while merging is disabled) + the candidate staging.
    m_candidateStaging.forEach([](oc::vector<uint32>& list) { list.clear(); });
    m_maxJoinRadiusBits.store(0u, oc::memory_order_relaxed);
    runPass(numEmitters, 64u, 256u, JobProfile{ "Force merge bounds", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
        for (uint32 i = begin; i < end; ++i)
            if (m_emitters[i].generation != 0)
                refreshBubbleBounds(m_emitters[i]);
    });

    if (!m_merge.enabled)
    {
        for (uint32 g = 0; g < numGroups; ++g)
            if (m_groups[g].generation != 0)
                dissolveGroup(g);
        m_statGroups = m_statMerged = 0;
        return;
    }
    const float joinK = glm::max(m_merge.joinDistance, 0.0f);
    const float leaveK = glm::max(m_merge.leaveDistance, joinK); // leave never tighter than join
    const uint32 maxMembers = (uint32)glm::max(m_merge.maxMembers, 2);
    const float maxRadius = glm::max(m_merge.maxRadius, 0.0f);
    // fits: adding a bubble (c, r) keeps the group under its size cap and its cover under the max
    // radius (tested against the current centre; the cover is recomputed after the join pass).
    const auto fits = [&](const MergeGroup& group, const EmitterInstance& m) {
        return group.members.size() < maxMembers
            && memberCover(m, group.targetCenter) * m_merge.coverScale + m_merge.coverMargin <= maxRadius;
    };

    // 2. Leave pass (one job per group): prune dead/re-created slots, then drop members that no
    // longer qualify — not mergeable, no bubble, team changed, too far from the centre, or
    // (hysteresis) no other member within leaveK * (ri + rj). leaveK < 1 means the member's own
    // bubble still overlaps a neighbour's — i.e. is still inside the group's cover — on the frame
    // it gets its field back.
    runPass(numGroups, 1u, 8u, JobProfile{ "Force merge leave", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
    oc::small_vector<uint8, 256> hasNeighbour; // stack-local: a parallelFor body must not hold thread_locals
    for (uint32 g = begin; g < end; ++g)
    {
        MergeGroup& group = m_groups[g];
        if (group.generation == 0)
            continue;
        for (size_t k = 0; k < group.members.size();)
        {
            const uint32 idx = group.members[k];
            const EmitterInstance& m = m_emitters[idx];
            const bool stale = m.generation == 0 || m.group != g + 1;
            const bool unfit = !stale && (!m.mergeable || m.bubbleRadius <= 0.0f || m.team != group.team
                || memberCover(m, group.center) * m_merge.coverScale + m_merge.coverMargin > maxRadius * 1.1f);
            if (stale || unfit)
            {
                if (!stale)
                    beginLeave(m_emitters[idx], group.center, group.coverRadius);
                group.members[k] = group.members.back();
                group.members.pop_back();
            }
            else
                ++k;
        }
        const size_t n = group.members.size();
        hasNeighbour.clear();
        hasNeighbour.resize(n); // value-initialized: no neighbour yet
        for (size_t a = 0; a < n; ++a)
        {
            const EmitterInstance& ma = m_emitters[group.members[a]];
            for (size_t b = a + 1; b < n; ++b)
            {
                if (hasNeighbour[a] && hasNeighbour[b])
                    continue;
                const EmitterInstance& mb = m_emitters[group.members[b]];
                const float limit = leaveK * (ma.bubbleRadius + mb.bubbleRadius);
                if (forceDist2(ma.bubbleCenter, mb.bubbleCenter) < limit * limit)
                    hasNeighbour[a] = hasNeighbour[b] = 1;
            }
        }
        for (size_t k = n; k-- > 0;)
            if (!hasNeighbour[k])
            {
                beginLeave(m_emitters[group.members[k]], group.center, group.coverRadius);
                group.members[k] = group.members.back();
                group.members.pop_back();
            }
    }
    });

    // 3. Candidate cells (serial: concatenate the staged candidates, sort by cell key — a few
    // hundred entries) then the neighbour search (one job per candidate chunk): each candidate
    // binary-searches the 27 cells around its own and stages the pairs (i < j, once) that pass
    // the exact test joinK * (ri + rj). Cell = 2 x the largest join radius, so no partner can sit
    // outside the neighbourhood.
    {
        ProfileScope cellsScope("Force merge cells", EProfileCategory::Force);
        m_cells.clear();
        const float cell = glm::max(2.0f * std::bit_cast<float>(m_maxJoinRadiusBits.load(oc::memory_order_relaxed)), 0.5f);
        const float invCell = 1.0f / cell;
        m_candidateStaging.forEach([&](const oc::vector<uint32>& list) {
            for (const uint32 idx : list)
                m_cells.emplace_back(forceCellKey(m_emitters[idx].bubbleCenter, invCell), idx);
        });
        oc::sort(m_cells.begin(), m_cells.end(), [](const oc::pair<uint64, uint32>& a, const oc::pair<uint64, uint32>& b) {
            return a.first < b.first;
        });
    }
    m_pairStaging.forEach([](oc::vector<uint64>& pairs) { pairs.clear(); });
    const uint32 numCandidates = (uint32)m_cells.size();
    runPass(numCandidates, 32u, 128u, JobProfile{ "Force merge neighbours", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
        oc::vector<uint64>& pairs = m_pairStaging.local(); // no waits inside: the slot stays ours
        const oc::pair<uint64, uint32>* cells = m_cells.data();
        for (uint32 c = begin; c < end; ++c)
        {
            const uint32 i = cells[c].second;
            const EmitterInstance& ea = m_emitters[i];
            const uint64 key = cells[c].first;
            for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
            {
                // sign-extended 64-bit offsets (modular add; the +2^20 field bias keeps a -1 from borrowing)
                const uint64 nkey = key + (uint64)((int64)dx << 42) + (uint64)((int64)dy << 21) + (uint64)(int64)dz;
                // lower_bound over the sorted keys
                uint32 lo = 0, hi = numCandidates;
                while (lo < hi)
                {
                    const uint32 mid = (lo + hi) >> 1;
                    if (cells[mid].first < nkey) lo = mid + 1; else hi = mid;
                }
                for (uint32 k = lo; k < numCandidates && cells[k].first == nkey; ++k)
                {
                    const uint32 j = cells[k].second;
                    if (j <= i)
                        continue;
                    const EmitterInstance& eb = m_emitters[j];
                    if (eb.team != ea.team)
                        continue;
                    const float limit = joinK * (ea.bubbleRadius + eb.bubbleRadius);
                    if (forceDist2(ea.bubbleCenter, eb.bubbleCenter) < limit * limit)
                        pairs.push_back(((uint64)i << 32) | j);
                }
            }
        }
    });

    // 4. Union pass (serial — creates groups, moves membership across groups): an ungrouped pair
    // founds a group, an ungrouped emitter joins its neighbour's group, two groups merge (smaller
    // into larger) when the result fits.
    const auto processPair = [&](uint32 ia, uint32 ib)
    {
            EmitterInstance& ea = m_emitters[ia];
            EmitterInstance& eb = m_emitters[ib];
            if (ea.team != eb.team || (ea.group != 0 && ea.group == eb.group))
                return;
            const float limit = joinK * (ea.bubbleRadius + eb.bubbleRadius);
            if (forceDist2(ea.bubbleCenter, eb.bubbleCenter) >= limit * limit)
                return;
            if (ea.group == 0 && eb.group == 0)
            {
                const uint32 g = createGroup(ea.team);
                MergeGroup& group = m_groups[g];
                group.targetCenter = (ea.bubbleCenter + eb.bubbleCenter) * 0.5f;
                if (!fits(group, ea) || !fits(group, eb))
                {
                    dissolveGroup(g);
                    return;
                }
                // The group sphere GROWS out of the larger founder's bubble (smoothGroup eases it to
                // the cover); both founders keep their own field as Joining transition spheres.
                const EmitterInstance& seed = ea.bubbleRadius >= eb.bubbleRadius ? ea : eb;
                group.center = seed.bubbleCenter;
                group.coverRadius = seed.bubbleRadius;
                group.output = seed.output;
                group.members.push_back(ia);
                group.members.push_back(ib);
                beginJoin(ea, g, ea.bubbleCenter, ea.bubbleRadius);
                beginJoin(eb, g, eb.bubbleCenter, eb.bubbleRadius);
            }
            else if (ea.group == 0 || eb.group == 0)
            {
                EmitterInstance& lone = ea.group == 0 ? ea : eb;
                const uint32 loneIdx = ea.group == 0 ? ia : ib;
                const uint32 g = (ea.group == 0 ? eb.group : ea.group) - 1;
                MergeGroup& group = m_groups[g];
                if (fits(group, lone))
                {
                    group.members.push_back(loneIdx);
                    beginJoin(lone, g, lone.bubbleCenter, lone.bubbleRadius);
                }
            }
            else
            {
                uint32 big = ea.group - 1, small = eb.group - 1;
                if (m_groups[big].members.size() < m_groups[small].members.size())
                    oc::swap(big, small);
                MergeGroup& dst = m_groups[big];
                const MergeGroup& src = m_groups[small];
                if (dst.members.size() + src.members.size() > maxMembers
                    || glm::distance(src.targetCenter, dst.targetCenter) + src.targetRadius + m_merge.coverMargin > maxRadius)
                    return;
                // The small group's members slide into the big one as Joining spheres starting
                // from the small group's sphere (their summed own outputs stand in for it).
                for (const uint32 idx : src.members)
                {
                    dst.members.push_back(idx);
                    beginJoin(m_emitters[idx], big, src.center, src.coverRadius);
                }
                m_groups[small].members.clear();
                dissolveGroup(small);
            }
    };
    {
        ProfileScope unionScope("Force merge union", EProfileCategory::Force);
        m_pairStaging.forEach([&](const oc::vector<uint64>& pairs) {
            for (const uint64 pair : pairs)
                processPair((uint32)(pair >> 32), (uint32)pair);
        });
    }

    // 5. Covers (one job per group): target + displayed sphere; undersized groups flag a dissolve
    // that the serial sweep performs (renderer slot + the members' Leaving transitions).
    const uint32 numGroupsNow = (uint32)m_groups.size(); // the union pass may have created some
    runPass(numGroupsNow, 1u, 8u, JobProfile{ "Force merge cover", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
        for (uint32 g = begin; g < end; ++g)
        {
            MergeGroup& group = m_groups[g];
            if (group.generation == 0)
                continue;
            group.dissolve = !recomputeCover(group);
            if (!group.dissolve)
                smoothGroup(group, deltaSec);
        }
    });
    ProfileScope dissolveScope("Force merge dissolve", EProfileCategory::Force);
    m_statGroups = m_statMerged = 0;
    for (uint32 g = 0; g < numGroupsNow; ++g)
    {
        MergeGroup& group = m_groups[g];
        if (group.generation == 0)
            continue;
        if (group.dissolve)
        {
            dissolveGroup(g);
            continue;
        }
        ++m_statGroups;
        m_statMerged += (int)group.members.size();
    }
}

// The group sphere as the debug rings draw any emitter, plus a spoke from its centre to every
// member's bubble centre (so the coverage is readable at a glance).
void ForceSystem::debugDrawGroup(Renderer& renderer, const MergeGroup& group) const
{
    EmitterInstance sphere;
    sphere.team = group.team;
    sphere.output = group.output;
    sphere.reach = group.reach;
    sphere.focus = 0.5f;
    sphere.distribution = 0.5f;
    sphere.width = 1.0f;
    sphere.dir = glm::vec3(0.0f, 1.0f, 0.0f);
    sphere.pos = group.center - sphere.dir * (group.reach * 0.5f);
    refreshDistributionScale(sphere);
    debugDrawEmitter(renderer, sphere);
    const uint32 color = packDebugColor(m_params.teamColors[glm::min(group.team, MAX_FORCE_TEAMS - 1)] * 0.6f + 0.4f);
    for (const uint32 idx : group.members)
        renderer.addDebugLine(group.center, m_emitters[idx].bubbleCenter, color);
}
