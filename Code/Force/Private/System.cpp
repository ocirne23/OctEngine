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
// NAMESPACE scope, not a function-local static: the build is /Zc:threadSafeInit-, so a local static
// first reached from two threads at once is a race, and the merge job's parallelFors reach this one
// through refreshDistributionScale / sphereReach. A namespace-scope constant is built during static
// init and carries no per-access guard at all. Pure constant math, so nothing orders against it.
static const float g_forceReferenceBudget = [] {
    double sum = 0.0;
    constexpr int NUM_SAMPLES = 64;
    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        const float X = -1.0f + (i + 0.5f) * (2.0f / NUM_SAMPLES);
        sum += std::pow(1.0 - (double)X * X, 3.0);
    }
    return (float)sum;
}();

static float forceReferenceBudget() { return g_forceReferenceBudget; }

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

void ForceSystem::setNumTeams(uint32 numTeams)
{
    // Takes effect through the per-frame params push: the renderer detects the change, idles the
    // device once, recompiles the force shaders (NUM_FORCE_TEAMS) and remakes the team-sized
    // bake volume/buffers.
    m_params.numTeams = glm::clamp(numTeams, 2u, (uint32)MAX_FORCE_TEAMS);
}

void ForceSystem::initialize()
{
    // Reserved to the caps so createEmitter/createQuery growth NEVER reallocates: a concurrent
    // spawn job may be resolving its own fresh handle while another creates (see m_createMutex in
    // System.ixx). Emitter INSTANCES are capped far above the renderer's slots — only ACTIVE
    // emitters hold one (see MAX_FORCE_INSTANCES).
    m_emitters.reserve(MAX_FORCE_INSTANCES);
    m_queries.reserve(RendererVKLayout::MAX_FORCE_QUERIES);

    Tweak::boolean("Force", "Enabled", &m_params.enabled);
    Tweak::floatVar("Force", "Iso threshold", &m_params.isoThreshold, 0.01f, 2.0f);
    Tweak::intVar("Force", "March steps", &m_params.marchSteps, 8, 128);
    Tweak::boolean("Force", "Use grid", &m_params.useGrid); // off = brute force (A-B correctness check)
    Tweak::boolean("Force/Bake", "Enabled", &m_bakeEnabled);
    Tweak::floatVar("Force/Bake", "Sample height", &m_bakeSampleHeight, 0.0f, 10.0f, 0.1f);
    Tweak::intVar("Force/Bake", "Chunks (stat)", &m_statBakeChunks, 0, 100000);
    Tweak::floatVar("Force", "Force gain", &m_params.forceGain, 0.0f, 10.0f);
    Tweak::floatVar("Force/Shell", "Alpha", &m_params.shellAlpha, 0.0f, 1.0f);
    // Draw culling/LOD (the field/readbacks of a culled shell stay live; desktop only):
    Tweak::floatVar("Force/Shell", "Min screen radius (px)", &m_params.minShellPixels, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Force/Shell", "Full-detail radius (px)", &m_params.shellFullResPixels, 8.0f, 1024.0f, 4.0f);
    Tweak::floatVar("Force/Shell", "Sampled tier radius (m)", &m_params.sampledShellRadius, 0.0f, 100.0f, 1.0f);
    Tweak::boolean("Force/Shell", "Union march", &m_params.unionMarch);
    Tweak::boolean("Force/Shell", "Union half res", &m_params.unionHalfRes); // rebuild-class (device idle)
    Tweak::boolean("Force/Shell", "Union jitter", &m_params.unionJitter);    // rebuild-class (shader define)
    Tweak::floatVar("Force/Shell", "Union step (m)", &m_params.unionStepSize, 0.05f, 4.0f, 0.05f);
    Tweak::intVar("Force/Shell", "Union max steps", &m_params.unionMaxSteps, 8, 512, 8);
    // The sampled-tier volume's fit is clipped to the camera's view footprint + this (0 = the
    // unbounded union of every large bubble's support box).
    Tweak::floatVar("Force/Shell", "Volume view margin (m)", &m_params.shellVolumeViewMargin, 0.0f, 200.0f, 1.0f);
    // The draw-box shrink's iso reduction (see packVisibleBounds): 0 = full support boxes.
    Tweak::floatVar("Force/Shell", "Visible bounds iso frac", &m_visibleBoundsIsoFrac, 0.0f, 1.0f, 0.05f);
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
    Tweak::intVar("Force", "Emitters (stat)", &m_statEmitters, 0, 1000000);
    Tweak::intVar("Force", "GPU slots (stat)", &m_statSlots, 0, 1000000); // only ACTIVE emitters hold one
    m_pairStaging.initialize();
    m_candidateStaging.initialize();
    m_slotAcquire.initialize();
    m_slotRelease.initialize();
    m_bakeKeyStaging.initialize();
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
    Tweak::boolean("Force/Debug", "Log tier classification", &m_params.logTierDebug);
    Tweak::floatVar("Force/Debug", "Density range", &m_params.densityRange, 0.1f, 10.0f, 0.05f);
}

static float forceIsoLateral(float t, float R, float m, float W, float D, float foldedOutput, float iso);

// VISIBLE-BOUNDS pack (teamFlags.w, decoded by forceVisibleBounds in force_field.inc.glsl): the
// emitter's own iso-surface extent from the closed-form profile, evaluated at iso x "Visible
// bounds iso frac" (default 1.0 — tightest boxes; a surface can exist where two sub-iso fields
// SUM past iso, so lower the frac toward 0.5 for merge slack if a merged bulge ever clips at a
// box edge; same-team crowds are what the merge system replaces with one group sphere anyway). The proxy/interval draws shrink to this box so the
// marches skip the empty support space around a bubble far below its reach box (a drained shield
// in a full-size box); the FIELD keeps the full support everywhere (grid insert, bake fits, CPU
// mirrors). 0 = nothing clears the reduced iso (or the tweak is 0): the full support box stands.
static uint32 packVisibleBounds(float R, float m, float W, float D, float foldedOutput, float isoEff, float fullSide)
{
    if (isoEff <= 0.0f)
        return 0u;
    float lo = 1.0f, hi = 0.0f, lat = 0.0f;
    for (int i = 0; i <= 16; ++i)
    {
        const float t = (float)i * (1.0f / 16.0f);
        const float l = forceIsoLateral(t, R, m, W, D, foldedOutput, isoEff);
        if (l > 0.0f)
        {
            lo = glm::min(lo, t);
            hi = glm::max(hi, t);
            lat = glm::max(lat, l);
        }
    }
    if (hi < lo)
        return 0u;
    lo = glm::max(lo - 1.0f / 16.0f, 0.0f); // one-station slack for curvature between stations
    hi = glm::min(hi + 1.0f / 16.0f, 1.0f);
    const float latFrac = glm::clamp(lat * 1.1f / glm::max(fullSide, 1e-6f), 0.0f, 1.0f);
    const uint32 loQ = (uint32)(lo * 255.0f);           // floor/ceil: always conservative outward
    const uint32 hiQ = (uint32)glm::ceil(hi * 255.0f);
    const uint32 latQ = glm::max((uint32)glm::ceil(latFrac * 65535.0f), 1u);
    return loQ | (hiQ << 8) | (latQ << 16);
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
    // Clamp below the LIVE team count: the shaders' phi arrays are sized NUM_FORCE_TEAMS.
    gpu.teamFlags = glm::uvec4(glm::min(team, Globals::forceSystem.numTeams() - 1), flags, 0u, 0u);
    const float mFold = 1.0f - 2.0f * gpu.dirFocus.w;
    const float fullSide = 0.5f * gpu.posReach.w * (1.0f + glm::abs(mFold)) * gpu.outputParams.w * 1.03f;
    const float isoEff = glm::max(Globals::forceSystem.getParams().isoThreshold, 1e-3f)
        * Globals::forceSystem.visibleBoundsIsoFrac();
    gpu.teamFlags.w = packVisibleBounds(gpu.posReach.w, mFold, gpu.outputParams.w, gpu.outputParams.z,
        gpu.outputParams.x, isoEff, fullSide);
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
    refreshDistributionScale(staged); // the shape budget cache the instance starts with
    const std::lock_guard lock(m_createMutex); // parallel entity spawning
    uint32 idx;
    if (!m_freeEmitters.empty())
    {
        idx = m_freeEmitters.back();
        m_freeEmitters.pop_back();
    }
    else
    {
        if (m_emitters.size() >= MAX_FORCE_INSTANCES) // the reserve is the hard cap: no reallocation
        {
            if (!m_instanceCapWarned)
            {
                m_instanceCapWarned = true;
                printf("ForceSystem: out of force emitter instances (cap %u) — new bubbles are dead handles\n",
                    MAX_FORCE_INSTANCES);
            }
            return ForceEmitter();
        }
        m_emitters.emplace_back();
        idx = (uint32)m_emitters.size() - 1;
    }
    EmitterInstance& inst = m_emitters[idx];
    inst = EmitterInstance{};
    inst.generation = m_generationCounter++;
    if (m_generationCounter == 0)
        m_generationCounter = 1;
    // No renderer slot yet: update() mints one for an ACTIVE emitter and hands it back the frame
    // the SIM LOD gates the bubble off, so the scarce GPU slots follow the live bubbles only.
    inst.team = glm::min(team, m_params.numTeams - 1);
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
    const uint32 slot = Globals::rendererVK.createForceQuerySlot(); // own internal lock
    if (slot == UINT32_MAX)
    {
        printf("ForceSystem: out of force query slots\n");
        return ForceQuery();
    }
    const std::lock_guard lock(m_createMutex); // parallel entity spawning
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
    uint32 rendererSlot = UINT32_MAX;
    {
        const std::lock_guard lock(m_createMutex); // parallel entity spawning
        if (EmitterInstance* inst = resolveEmitter(handle))
        {
            rendererSlot = inst->rendererSlot;
            inst->generation = 0;
            inst->group = 0; // its group prunes the stale index on the next merge pass
            m_freeEmitters.push_back((uint32)handle);
            --m_numLiveEmitters;
            if (rendererSlot != UINT32_MAX) // a gated-off emitter holds none (see setActive)
                --m_numSlottedEmitters;
        }
    }
    // The renderer slot retire (its own lock) runs after: the instance is already invalidated, and
    // the slot cannot be re-handed out before this call retires it.
    if (rendererSlot != UINT32_MAX)
        Globals::rendererVK.destroyForceEmitter(rendererSlot);
}

void ForceSystem::destroyQuery(uint64 handle)
{
    uint32 rendererSlot = UINT32_MAX;
    {
        const std::lock_guard lock(m_createMutex); // parallel entity spawning
        if (QueryInstance* inst = resolveQuery(handle))
        {
            rendererSlot = inst->rendererSlot;
            inst->generation = 0;
            m_freeQueries.push_back((uint32)handle);
        }
    }
    if (rendererSlot != UINT32_MAX)
        Globals::rendererVK.destroyForceQuerySlot(rendererSlot); // see destroyEmitter
}

// The plain sphere's budget fold (focus 0.5 / distribution 0.5 / width 1) — group and transition
// spheres all use it; constants only, so computed once.
// Namespace scope for the same reason as g_forceReferenceBudget (sphereReach runs in the merge
// job's parallelFors). Declared BELOW that one, so within-TU static init order supplies it first.
static const float g_forceSphereFold = forceReferenceBudget() / forceShapeBudget(0.5f, 0.5f);

static float forceSphereFold() { return g_forceSphereFold; }

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

// One ACTIVE emitter's GPU upload. Called from the upload jobs (own instance, own renderer slot,
// read-only readback span, PerWorker debug lines) and serially from update() for an emitter that
// just took a slot back.
void ForceSystem::uploadEmitter(Renderer& renderer, EmitterInstance& inst, float blendStep)
{
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
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
    // among the members in update()).
    const bool merged = inst.mergeState == EmitterInstance::EMergeState::Merged;
    const bool transition = inst.mergeState == EmitterInstance::EMergeState::Joining
        || inst.mergeState == EmitterInstance::EMergeState::Leaving;
    // Readback source (setAnalyticReadback): the GPU integral only for flagged emitters, or for
    // everyone while no bake is published. A BAKE-READ merged member needs no upload at all —
    // its taps read its own position against the group's field, which is what PASSIVE bought.
    const bool analytic = inst.analyticReadback || !m_bakePublished;
    const uint32 readbackBit = analytic ? FORCE_FLAG_READBACK : 0u;
    const uint32 flags = !merged ? (FORCE_FLAG_ACTIVE | readbackBit)
        : (analytic && m_merge.memberReadback ? (FORCE_FLAG_ACTIVE | FORCE_FLAG_PASSIVE | FORCE_FLAG_READBACK) : 0u);
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
    const RendererVKLayout::ForceEmitterGpu gpu = buildEmitterGpu(src.pos, src.dir, src.output, src.reach,
        src.focus, src.team, src.distribution, src.width,
        refreshDistributionScale(transitionReach > 0.0f ? sphere : inst), src.shellAlpha, flags);
    renderer.updateForceEmitter(inst.rendererSlot, gpu);
    if (!analytic)
        bakedReadback(inst, gpu); // merged too: its own position's truth against the group's field
    else if (flags != 0u)
    {
        // Latch the GPU force readback (slot-indexed, ~2 frames old; zero until the first lands).
        // "Force gain" applies HERE, not on the GPU: the compute writes the raw integral, so the
        // tweak takes effect instantly on the CPU read instead of riding the readback latency.
        const glm::vec4 readback = renderer.getForceEmitterReadback(inst.rendererSlot);
        inst.appliedForce = glm::vec3(readback) * m_params.forceGain;
        inst.pressure = readback.w;
    }
    if (m_debugDraw && !merged)
        debugDrawEmitter(renderer, src);
}

void ForceSystem::bakedReadback(EmitterInstance& inst, const RendererVKLayout::ForceEmitterGpu& gpu) const
{
    // Mirrors force_emitter.cs: samples through the bubble weighted by the emitter's own
    // normalized field, force = Output x mean(wSelf x -grad), pressure = mean(opposing) over the
    // FULL tap count (a tap outside the own field adds zero, as the shader's `continue` does). The
    // ring is planar at the bake height — the bake is a ground-band field.
    const glm::vec3 dir(gpu.dirFocus);
    const float R = gpu.posReach.w;
    const glm::vec3 center = glm::vec3(gpu.posReach) + dir * (R * 0.5f);
    const float sampleRadius = 0.35f * R;
    const float emitterOutput = glm::max(gpu.outputParams.x, 1e-4f);
    const uint32 team = gpu.teamFlags.x;
    const int ringTaps = R > 20.0f ? 16 : 8; // a base-sized bubble resolves its contact arc
    glm::vec3 force(0.0f);
    float pressure = 0.0f;
    for (int s = 0; s <= ringTaps; ++s)
    {
        glm::vec3 x = center;
        if (s > 0)
        {
            const float a = (float)(s - 1) * (glm::two_pi<float>() / (float)ringTaps);
            x += glm::vec3(std::cos(a), 0.0f, std::sin(a)) * sampleRadius;
        }
        x.y = m_bakeSampleHeight;
        const float wSelf = RendererVKLayout::forceContributionCpu(x, gpu) / emitterOutput;
        if (wSelf <= 0.0f)
            continue;
        const FieldSample fs = sampleBakedField(x, team);
        force += wSelf * -fs.opposingGradient;
        pressure += fs.opposing;
    }
    const float invN = 1.0f / (float)(ringTaps + 1);
    inst.appliedForce = force * (gpu.outputParams.x * invN * m_params.forceGain);
    inst.pressure = pressure * invN;
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
    m_slotAcquire.forEach([](oc::vector<uint32>& list) { list.clear(); });
    m_slotRelease.forEach([](oc::vector<uint32>& list) { list.clear(); });
    // Per-emitter upload on jobs: each iteration touches only its own instance, its own renderer
    // slot (distinct vector elements, no growth — create/destroy are main-thread outside this),
    // the read-only readback span and the PerWorker-staged debug lines. An emitter whose ACTIVE
    // gate flipped only STAGES its index — the renderer slot itself is minted/retired serially
    // below, where growing the renderer's vectors cannot race these jobs.
    Globals::jobSystem.parallelFor(0u, (uint32)m_emitters.size(), 64u, JobProfile{ "Force upload", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
    for (uint32 emitterIdx = begin; emitterIdx < end; ++emitterIdx)
    {
        EmitterInstance& inst = m_emitters[emitterIdx];
        if (inst.generation == 0)
            continue;
        if (!inst.active)
        {
            // Gated off (SIM LOD): no field this frame, and the GPU SLOT GOES BACK — the far half
            // of a 25k-unit map must not sit on the renderer's MAX_FORCE_EMITTERS. Any merge
            // transition is dropped on the spot (the merge pass already evicted it — no bubble).
            inst.mergeState = EmitterInstance::EMergeState::Own;
            inst.group = 0;
            inst.blend = 0.0f;
            inst.appliedForce = glm::vec3(0.0f);
            inst.pressure = 0.0f;
            if (inst.rendererSlot != UINT32_MAX)
                m_slotRelease.local().push_back(emitterIdx);
            continue;
        }
        if (inst.rendererSlot == UINT32_MAX)
        {
            m_slotAcquire.local().push_back(emitterIdx); // minted AND uploaded serially below
            continue;
        }
        uploadEmitter(renderer, inst, blendStep);
    }
    });
    {
        // Slot churn, serial on main: releases first, so a slot freed this frame is at least in
        // the renderer's retirement queue before the acquires ask for one.
        ProfileScope slotScope("Force slots", EProfileCategory::Force);
        m_slotRelease.forEach([&](const oc::vector<uint32>& list)
        {
            for (const uint32 idx : list)
            {
                renderer.destroyForceEmitter(m_emitters[idx].rendererSlot);
                m_emitters[idx].rendererSlot = UINT32_MAX;
                --m_numSlottedEmitters;
            }
        });
        uint32 starved = 0;
        m_slotAcquire.forEach([&](const oc::vector<uint32>& list)
        {
            for (const uint32 idx : list)
            {
                EmitterInstance& inst = m_emitters[idx];
                // A placeholder desc (output 0): uploadEmitter overwrites it on the next line with
                // the real field, flags included.
                inst.rendererSlot = renderer.createForceEmitter(buildEmitterGpu(inst.pos, inst.dir, 0.0f,
                    inst.reach, inst.focus, inst.team, inst.distribution, inst.width,
                    refreshDistributionScale(inst), inst.shellAlpha, 0u));
                if (inst.rendererSlot == UINT32_MAX)
                {
                    ++starved; // no field this frame; the next update() tries again
                    continue;
                }
                ++m_numSlottedEmitters;
                uploadEmitter(renderer, inst, blendStep);
            }
        });
        if (starved > 0 && !m_slotCapWarned)
        {
            m_slotCapWarned = true; // once per stretch: more ACTIVE bubbles than the GPU has slots
            printf("ForceSystem: out of GPU force emitter slots (%u of %u held, %u live emitters) — "
                "%u active emitters have no field\n", m_numSlottedEmitters,
                RendererVKLayout::MAX_FORCE_EMITTERS, m_numLiveEmitters, starved);
        }
        else if (starved == 0)
            m_slotCapWarned = false;
        m_statEmitters = (int)m_numLiveEmitters;
        m_statSlots = (int)m_numSlottedEmitters;
        // A group founded on the merge job has no slot yet: minted here, on main, for the same
        // reason. UINT32_MAX = out of slots this frame; its members still carry their transition
        // spheres, and the next update() tries again.
        for (MergeGroup& group : m_groups)
            if (group.generation != 0 && group.rendererSlot == UINT32_MAX)
                group.rendererSlot = renderer.createForceEmitter(buildEmitterGpu(group.center, up, 0.0f, 1.0f, 0.5f,
                    group.team, 0.5f, 1.0f, 1.0f, 1.0f, 0u));
    }
    // Per-group upload on jobs: own group, own renderer slot, the read-only readback span, and
    // (shared readback mode) its OWN members — an emitter belongs to at most one group, so the
    // member writes are distinct too. Small group counts stay inline (runPass).
    runPass((uint32)m_groups.size(), 16u, 32u, JobProfile{ "Force groups upload", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
    for (uint32 g = begin; g < end; ++g)
    {
        MergeGroup& group = m_groups[g];
        if (group.generation == 0 || group.rendererSlot == UINT32_MAX)
            continue;
        // The group sphere: focus 0.5 / distribution 0.5 / width 1, axis up, centred on `center`
        // — its budget fold is the constant sphere fold (namespace scope: worker-reachable).
        // The group sphere integrates on the GPU only in shared-readback mode, where ANALYTIC
        // members take their split from it; bake-read members sample their own position.
        const uint32 groupFlags = FORCE_FLAG_ACTIVE | (m_merge.memberReadback ? 0u : FORCE_FLAG_READBACK);
        renderer.updateForceEmitter(group.rendererSlot,
            buildEmitterGpu(group.center - up * (group.reach * 0.5f), up, group.output, group.reach, 0.5f,
                group.team, 0.5f, 1.0f, forceSphereFold(), group.shellAlpha, groupFlags));
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
                if (member.mergeState != EmitterInstance::EMergeState::Merged
                    || !(member.analyticReadback || !m_bakePublished))
                    continue; // a Joining member still has its own active transition sphere; a
                              // bake-read member already sampled its own position in uploadEmitter
                member.appliedForce = group.appliedForce * (glm::max(member.output, 0.0f) * invSum);
                member.pressure = group.pressure;
            }
        }
        if (m_debugDrawGroups)
            debugDrawGroup(renderer, group);
    }
    });
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
    { // the baked pressure field: this frame's chunk set out, the paired readback republished
        ProfileScope bakeScope("Force bake", EProfileCategory::Force);
        buildBakeChunks(renderer);
        ProfileScope publishScope("Force bake publish", EProfileCategory::Force);
        publishBake(renderer);
    }
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
    if (!inst.active)
    {
        // No bubble while gated off: evicted from its group by the member sweep (radius 0 =
        // unfit), never a candidate. The bounds cache is dropped so reactivation recomputes.
        inst.bubbleRadius = 0.0f;
        inst.candidate = false;
        inst.boundsOutput = -1.0f;
        return;
    }
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
        const uint32 bits = oc::bitCast<uint32>(joinRadius);
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
// ---- the baked pressure field (see System.ixx) ----

static uint64 bakeChunkKey(int bx, int bz)
{
    return ((uint64)(uint32)bx << 32) | (uint32)bz;
}

// Chunk selection: every emitter's / group's support box is rasterized to 16 m chunk keys on jobs
// (a PerWorker key list each — no shared set, no cap check on the hot path), then ONE serial
// sort + unique over the staged keys, capped at MAX_FORCE_BAKE_CHUNKS. The cap therefore drops
// the highest keys (a corner of the covered area) instead of whichever emitter scanned last.
void ForceSystem::buildBakeChunks(Renderer& renderer)
{
    m_bakeChunkScratch.clear();
    constexpr float c_chunkSize = FORCE_BAKE_CHUNK_SAMPLES * FORCE_BAKE_SAMPLE_SPACING; // 16 m
    const auto addBox = [](BakeKeySet& keys, glm::vec2 lo, glm::vec2 hi)
    {
        const int bx0 = (int)std::floor(lo.x / c_chunkSize), bx1 = (int)std::floor(hi.x / c_chunkSize);
        const int bz0 = (int)std::floor(lo.y / c_chunkSize), bz1 = (int)std::floor(hi.y / c_chunkSize);
        for (int bz = bz0; bz <= bz1; ++bz)
            for (int bx = bx0; bx <= bx1; ++bx)
                keys.insert(bakeChunkKey(bx, bz));
    };
    bool capped = false;
    if (m_bakeEnabled)
    {
        m_bakeKeyStaging.forEach([](BakeKeySet& keys) { keys.begin(); });
        runPass((uint32)m_emitters.size(), 256u, 512u, JobProfile{ "Force bake boxes", EProfileCategory::Force },
            [&](uint32 begin, uint32 end)
        {
        BakeKeySet& keys = m_bakeKeyStaging.local(); // no waits inside: the slot stays ours
        for (uint32 i = begin; i < end; ++i)
        {
            const EmitterInstance& inst = m_emitters[i];
            // Gated-off first: it is the common reject on a big map and shares the instance's
            // FIRST cache line with the generation, so the scan stays one line per dead emitter.
            if (inst.generation == 0 || !inst.active || inst.rendererSlot == UINT32_MAX
                || inst.mergeState == EmitterInstance::EMergeState::Merged || inst.output <= 0.0f)
                continue; // merged members / gated-off / slotless emitters project no field of their own
            // Conservative XZ box of the support (the forceEmitterBounds rule): the output line
            // pos .. pos + dir * reach, expanded by the lateral half-width.
            const glm::vec3 target = inst.pos + inst.dir * inst.reach;
            const float side = 0.5f * inst.reach * (1.0f + glm::abs(1.0f - 2.0f * inst.focus))
                * glm::max(inst.width, 1.0f) * 1.05f;
            glm::vec2 lo = glm::min(glm::vec2(inst.pos.x, inst.pos.z), glm::vec2(target.x, target.z)) - side;
            glm::vec2 hi = glm::max(glm::vec2(inst.pos.x, inst.pos.z), glm::vec2(target.x, target.z)) + side;
            if (inst.mergeState == EmitterInstance::EMergeState::Joining
                || inst.mergeState == EmitterInstance::EMergeState::Leaving)
            {
                // The transition sphere lerps between the own bubble and the group sphere — cover
                // where it currently stands too (sub-iso fringe past 3x the visible radius is
                // negligible, so the multiplier is enough).
                const float r = glm::max(inst.blendRadius * 3.0f, 1.0f);
                lo = glm::min(lo, glm::vec2(inst.blendCenter.x, inst.blendCenter.z) - r);
                hi = glm::max(hi, glm::vec2(inst.blendCenter.x, inst.blendCenter.z) + r);
            }
            addBox(keys, lo, hi);
        }
        });
        BakeKeySet& mainKeys = m_bakeKeyStaging.local(); // main's own slot, after the join
        for (const MergeGroup& group : m_groups)
        {
            if (group.generation == 0 || group.rendererSlot == UINT32_MAX)
                continue;
            const float r = group.reach * 0.55f; // the uploaded sphere's lateral half-extent + slack
            addBox(mainKeys, glm::vec2(group.center.x, group.center.z) - r,
                   glm::vec2(group.center.x, group.center.z) + r);
        }
        // Serial merge: the workers' unique lists (a few hundred keys each at most) through one
        // more stamp table — no concatenate, no sort.
        m_bakeMerge.begin();
        m_bakeKeyStaging.forEach([&](const BakeKeySet& keys) {
            for (const uint64 key : keys.unique)
                m_bakeMerge.insert(key);
        });
        oc::vector<uint64>& uniqueKeys = m_bakeMerge.unique;
        if (m_bakeMerge.dirty)
        {
            oc::sort(uniqueKeys.begin(), uniqueKeys.end());
            uniqueKeys.erase(oc::unique(uniqueKeys.begin(), uniqueKeys.end()), uniqueKeys.end());
        }
        capped = uniqueKeys.size() > (size_t)MAX_FORCE_BAKE_CHUNKS;
        if (capped)
        {
            // Keep the chunks nearest the covered set's centroid: the OUTERMOST regions go
            // unbaked, never a coherent half-plane (the key packs bx as uint32, so negative X
            // sorts last — cutting the sorted tail would drop every chunk on one side).
            glm::dvec2 centroid(0.0);
            for (const uint64 key : uniqueKeys)
                centroid += glm::dvec2((int)(uint32)(key >> 32), (int)(uint32)key);
            centroid /= (double)uniqueKeys.size();
            const auto dist2 = [&](uint64 key) {
                const glm::dvec2 d = glm::dvec2((int)(uint32)(key >> 32), (int)(uint32)key) - centroid;
                return glm::dot(d, d);
            };
            oc::sort(uniqueKeys.begin(), uniqueKeys.end(), [&](uint64 a, uint64 b) { return dist2(a) < dist2(b); });
            uniqueKeys.resize(MAX_FORCE_BAKE_CHUNKS);
        }
        m_bakeChunkScratch.reserve(uniqueKeys.size());
        for (const uint64 key : uniqueKeys)
            m_bakeChunkScratch.push_back(glm::ivec4((int)(uint32)(key >> 32), (int)(uint32)key, 0, 0));
    }
    m_statBakeChunks = (int)m_bakeChunkScratch.size();
    if (capped && !m_bakeCapWarned)
    {
        m_bakeCapWarned = true; // once: dropped chunks read as zero field (no push/exposure there)
        printf("ForceSystem: baked-field chunk cap hit (%u) — outermost emitter regions unbaked\n",
            MAX_FORCE_BAKE_CHUNKS);
    }
    renderer.setForceBakeChunks(m_bakeChunkScratch, m_bakeSampleHeight);
}

void ForceSystem::publishBake(Renderer& renderer)
{
    // Copy THIS slot's readback (paired with the chunk list it was evaluated for) into stable
    // CPU storage: the mapped buffer is only safe until present re-submits the slot, while the
    // published copy is read by next frame's entity pass.
    const ForceBakeReadback bake = renderer.getForceBakeReadback();
    // TEAM-SIZED stride, mirroring force_bake.cs: one vec4 per sample with <= 4 live teams.
    const size_t vec4PerChunk = (size_t)FORCE_BAKE_SAMPLES_PER_CHUNK * ((m_params.numTeams + 3u) / 4u);
    const size_t numChunks = glm::min(bake.chunks.size(), bake.data.size() / vec4PerChunk);
    // Sized ONCE to the cap (a team-count change re-sizes): a per-frame resize would construct
    // up to the whole 2-4 MB again whenever the chunk count grows back.
    if (m_bakeData.size() != vec4PerChunk * MAX_FORCE_BAKE_CHUNKS)
        m_bakeData.resize(vec4PerChunk * MAX_FORCE_BAKE_CHUNKS);
    // The copy (up to 512 chunks x 4 KB per team quad) fans out per chunk: distinct destination
    // ranges, a read-only mapped source.
    runPass((uint32)numChunks, 16u, 64u, JobProfile{ "Force bake copy", EProfileCategory::Force },
        [&](uint32 begin, uint32 end)
    {
        memcpy(m_bakeData.data() + (size_t)begin * vec4PerChunk, bake.data.data() + (size_t)begin * vec4PerChunk,
            (size_t)(end - begin) * vec4PerChunk * sizeof(glm::vec4));
    });
    // The lookup: a dense grid over the chunks' bounding box (O(1) per corner) when it fits,
    // else the sorted key list. Both build in O(numChunks).
    glm::ivec2 lo(INT32_MAX), hi(INT32_MIN);
    for (size_t b = 0; b < numChunks; ++b)
    {
        lo = glm::min(lo, glm::ivec2(bake.chunks[b]));
        hi = glm::max(hi, glm::ivec2(bake.chunks[b]));
    }
    const glm::ivec2 size = numChunks > 0 ? hi - lo + 1 : glm::ivec2(0);
    if (numChunks > 0 && (uint64)size.x * (uint64)size.y <= MAX_BAKE_GRID_CELLS)
    {
        m_bakeGridLo = lo;
        m_bakeGridSize = size;
        m_bakeGrid.assign((size_t)size.x * size.y, UINT16_MAX);
        for (size_t b = 0; b < numChunks; ++b)
            m_bakeGrid[(size_t)(bake.chunks[b].y - lo.y) * size.x + (bake.chunks[b].x - lo.x)] = (uint16)b;
    }
    else
    {
        m_bakeGridSize = glm::ivec2(0);
        m_bakeSorted.clear();
        for (size_t b = 0; b < numChunks; ++b)
            m_bakeSorted.emplace_back(bakeChunkKey(bake.chunks[b].x, bake.chunks[b].y), (uint32)b);
        oc::sort(m_bakeSorted.begin(), m_bakeSorted.end());
    }
    m_bakePublished = m_bakeEnabled; // disabled: samplers report invalid, callers fall back
}

uint32 ForceSystem::findBakeChunk(int bx, int bz) const
{
    if (m_bakeGridSize.x > 0)
    {
        const int lx = bx - m_bakeGridLo.x, lz = bz - m_bakeGridLo.y;
        if (lx < 0 || lz < 0 || lx >= m_bakeGridSize.x || lz >= m_bakeGridSize.y)
            return UINT32_MAX;
        const uint16 idx = m_bakeGrid[(size_t)lz * m_bakeGridSize.x + lx];
        return idx == UINT16_MAX ? UINT32_MAX : idx;
    }
    const uint64 key = bakeChunkKey(bx, bz);
    const auto it = oc::lower_bound(m_bakeSorted.begin(), m_bakeSorted.end(), key,
        [](const oc::pair<uint64, uint32>& e, uint64 k) { return e.first < k; });
    return it != m_bakeSorted.end() && it->first == key ? it->second : UINT32_MAX;
}

void ForceSystem::BakeKeySet::begin()
{
    if (keys.empty())
    {
        keys.resize(SIZE);
        stamps.assign(SIZE, 0u);
    }
    if (++stamp == 0u) // wrapped: every slot would read as this frame's
    {
        stamps.assign(SIZE, 0u);
        stamp = 1u;
    }
    unique.clear();
    dirty = false;
}

void ForceSystem::BakeKeySet::insert(uint64 key)
{
    uint32 i = (uint32)((key * 0x9E3779B97F4A7C15ull) >> 52) & (SIZE - 1); // top bits of a Fibonacci hash
    for (uint32 probe = 0; probe < 32; ++probe, i = (i + 1) & (SIZE - 1))
    {
        if (stamps[i] != stamp)
        {
            stamps[i] = stamp;
            keys[i] = key;
            unique.push_back(key);
            return;
        }
        if (keys[i] == key)
            return;
    }
    unique.push_back(key); // crowded run: append unchecked, the merge dedups
    dirty = true;
}

ForceSystem::FieldSample ForceSystem::sampleBakedField(const glm::vec3& pos, uint32 team) const
{
    FieldSample s;
    if (!m_bakePublished)
        return s;
    s.valid = true;
    constexpr int N = (int)FORCE_BAKE_CHUNK_SAMPLES;
    constexpr float c_invSpacing = 1.0f / FORCE_BAKE_SAMPLE_SPACING;
    const uint32 numTeams = m_params.numTeams;
    const size_t vec4PerSample = (numTeams + 3u) / 4u; // mirrors force_bake.cs's team-sized stride
    const float gxf = pos.x * c_invSpacing;
    const float gzf = pos.z * c_invSpacing;
    const int gx0 = (int)std::floor(gxf), gz0 = (int)std::floor(gzf);
    const float fx = gxf - (float)gx0, fz = gzf - (float)gz0;
    // The 2x2 lattice corners around the point — ONE fetch serves the bilinear value, the owning
    // team AND the gradient. A corner in a missing chunk is ZERO field (outside every support).
    float corner[4][MAX_FORCE_TEAMS] = {};
    bool any = false;
    for (int c = 0; c < 4; ++c)
    {
        const int gx = gx0 + (c & 1), gz = gz0 + (c >> 1);
        const int bx = gx >= 0 ? gx / N : (gx - (N - 1)) / N; // floor division
        const int bz = gz >= 0 ? gz / N : (gz - (N - 1)) / N;
        const uint32 chunk = findBakeChunk(bx, bz);
        if (chunk == UINT32_MAX)
            continue;
        any = true;
        const int lx = gx - bx * N, lz = gz - bz * N;
        const glm::vec4* v = &m_bakeData[((size_t)chunk * (N * N) + (size_t)(lz * N + lx)) * vec4PerSample];
        for (uint32 t = 0; t < numTeams; ++t)
            corner[c][t] = v[t >> 2][t & 3];
    }
    if (!any)
        return s; // zero field here: valid, no force, not inside anything
    const float w[4] = { (1.0f - fx) * (1.0f - fz), fx * (1.0f - fz), (1.0f - fx) * fz, fx * fz };
    float phi[MAX_FORCE_TEAMS];
    for (uint32 t = 0; t < numTeams; ++t)
        phi[t] = corner[0][t] * w[0] + corner[1][t] * w[1] + corner[2][t] * w[2] + corner[3][t] * w[3];
    uint32 best = 0;
    for (uint32 t = 1; t < numTeams; ++t)
        if (phi[t] > phi[best])
            best = t;
    float second = 0.0f;
    for (uint32 t = 0; t < numTeams; ++t)
        if (t != best)
            second = glm::max(second, phi[t]);
    s.owningTeam = best;
    s.field = phi[best];
    s.inside = phi[best] > glm::max(m_params.isoThreshold, second); // hard-max bound (no junction
                                                                   // smoothing — rim-blur scale)
    // The opposing field (vs the SAMPLED team) per corner: bilinear value + the analytic gradient
    // of the bilinear patch — continuous inside a cell, gameplay-grade across them.
    float o[4];
    for (int c = 0; c < 4; ++c)
    {
        o[c] = 0.0f;
        for (uint32 t = 0; t < numTeams; ++t)
            if (t != team)
                o[c] = glm::max(o[c], corner[c][t]);
    }
    s.opposing = o[0] * w[0] + o[1] * w[1] + o[2] * w[2] + o[3] * w[3];
    s.opposingGradient = glm::vec3(
        ((o[1] - o[0]) * (1.0f - fz) + (o[3] - o[2]) * fz) * c_invSpacing,
        0.0f,
        ((o[2] - o[0]) * (1.0f - fx) + (o[3] - o[1]) * fx) * c_invSpacing);
    return s;
}

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
        const float cell = glm::max(2.0f * oc::bitCast<float>(m_maxJoinRadiusBits.load(oc::memory_order_relaxed)), 0.5f);
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
