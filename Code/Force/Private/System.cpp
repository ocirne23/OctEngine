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
    Tweak::boolean("Force/Debug", "Draw emitters", &m_debugDraw);
    Tweak::boolean("Force/Debug", "Draw queries", &m_debugDrawQueries);
    Tweak::boolean("Force/Debug", "Density view", &m_params.densityView);
    Tweak::floatVar("Force/Debug", "Density range", &m_params.densityRange, 0.1f, 10.0f, 0.05f);
}

// outputScale = the distribution budget fold (refreshDistributionScale): the GPU sees Output
// pre-divided by the gain's field-weighted mean, so total emitted field stays exactly conserved.
static ForceEmitterGpu buildEmitterGpu(const glm::vec3& pos, const glm::vec3& dir, float output,
    float reach, float focus, uint32 team, float distribution, float width, float outputScale,
    float shellAlpha)
{
    ForceEmitterGpu gpu;
    gpu.posReach = glm::vec4(pos, glm::max(reach, 1e-3f));
    const glm::vec3 d = glm::dot(dir, dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0.0f, 1.0f, 0.0f);
    gpu.dirFocus = glm::vec4(d, glm::clamp(focus, 0.0f, 1.0f));
    gpu.outputParams = glm::vec4(glm::max(output, 0.0f) * outputScale, glm::clamp(shellAlpha, 0.0f, 1.0f),
        glm::clamp(distribution, 0.0f, 1.0f), glm::clamp(width, 0.05f, 4.0f));
    gpu.teamFlags = glm::uvec4(glm::min(team, MAX_FORCE_TEAMS - 1), FORCE_FLAG_ACTIVE, 0u, 0u);
    return gpu;
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

void ForceSystem::update(Renderer& renderer, float)
{
    ProfileScope profileScope("Force", EProfileCategory::Force);
    renderer.setForceFieldParams(m_params);
    for (EmitterInstance& inst : m_emitters)
    {
        if (inst.generation == 0)
            continue;
        renderer.updateForceEmitter(inst.rendererSlot,
            buildEmitterGpu(inst.pos, inst.dir, inst.output, inst.reach, inst.focus, inst.team,
                inst.distribution, inst.width, refreshDistributionScale(inst), inst.shellAlpha));
        // Latch the GPU force readback (slot-indexed, ~2 frames old; zero until the first lands).
        // "Force gain" applies HERE, not on the GPU: the compute writes the raw integral, so the
        // tweak takes effect instantly on the CPU read instead of riding the readback latency.
        const glm::vec4 readback = renderer.getForceEmitterReadback(inst.rendererSlot);
        inst.appliedForce = glm::vec3(readback) * m_params.forceGain;
        inst.pressure = readback.w;
        if (m_debugDraw)
            debugDrawEmitter(renderer, inst);
    }
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
    // Iso lateral half-width at axial station t, closed form (mirrors forceContribution):
    //   O' * (1 - u^2)^2 * g(t) = iso  ->  u^2 = 1 - sqrt(iso / (O' g))
    //   lat = (R/2) * sqrt((u^2 - X^2) * q^-m) where positive, X = 2t - 1, q = (1-X)/(1+X)
    const auto isoLateral = [&](float t) {
        const float g = forceDistributionGain(t, glm::clamp(inst.distribution, 0.0f, 1.0f));
        const float peak = foldedOutput * g;
        if (peak <= m_params.isoThreshold)
            return 0.0f; // density trough below iso: no surface at this station
        const float u2 = 1.0f - std::sqrt(m_params.isoThreshold / peak);
        const float X = t * 2.0f - 1.0f;
        const float y2 = u2 - X * X;
        if (y2 <= 0.0f)
            return 0.0f;
        const float q = glm::clamp((1.0f - X) / (1.0f + X), 1e-4f, 1e4f);
        return 0.5f * R * glm::clamp(inst.width, 0.05f, 4.0f) * std::sqrt(y2 * std::pow(q, -m));
    };

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
