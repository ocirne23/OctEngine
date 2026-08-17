export module Force:System;

import Core;
import Core.glm;
import RendererVK;

// Forcefield bubble manager (Globals::forceSystem). Emitters project analytic influence-field
// "bubbles": same-team fields SUM (metaball merging), a point belongs to a team where that team's
// field beats the iso threshold and every other team's field, and the bubble surface is the
// equal-field equilibrium between teams — squish and focused-lobe "pierce" fall out of the math,
// no simulation. The renderer ray-marches the surface (ForceFieldPipeline); per-emitter applied
// force and point queries are computed on the GPU and read back ~2 frames latent.
// update() runs once per frame on the main thread (after the entity update, before present) and is
// the only place renderer emitter state is written; ForceEmitter::setTransform/setOutput/... only
// store into the instance's own slot, so they are safe from the parallel entity update pass (one
// writer per instance). create/destroy are main-thread.

export class ForceSystem;

// RAII handle to a live emitter. Move-only, like ParticleEffect/PhysicsBody.
export class ForceEmitter final
{
public:
    ForceEmitter() = default;
    ForceEmitter(ForceEmitter&& move) noexcept : m_handle(move.m_handle) { move.m_handle = 0; }
    ForceEmitter& operator=(ForceEmitter&& move) noexcept;
    ForceEmitter(const ForceEmitter&) = delete;
    ~ForceEmitter() { destroy(); }

    bool isValid() const { return m_handle != 0; }
    void destroy();

    // Safe from the parallel entity update (writes only this instance's own slot).
    void setTransform(const glm::vec3& pos, const glm::vec3& direction);
    void setPosition(const glm::vec3& pos);
    void setOutput(float output);
    void setReach(float reach);   // total extent: the bubble spans pos .. pos + dir * reach
    void setFocus(float focus);   // shape pinch [0,1]: 0.5 = sphere spanning the line, 0 = cone
                                  // pointed at the emitter, 1 = cone pointed at the target
    // Where the output density sits along the line [0,1] (0 = emitter end, 1 = target end), as a
    // smooth budget-conserving bump — Output stays the total; concentration comes from the rest.
    void setDistribution(float distribution);
    // Lateral scale, reach untouched: 1 = round (focus 0.5 = perfect sphere), < 1 pinches every
    // shape narrower (cones keep their straight taper at a sharper angle, spheres go prolate).
    void setWidth(float width);
    void setTeam(uint32 team); // main-thread (rare)
    // Shell rendering opacity [0,1]; 0 skips the ray-marched shell draw entirely (the field still
    // exists — it deforms other bubbles and produces force/pressure/query results). The intended
    // use is huge invisible fields whose proxy box would otherwise become a full-screen march.
    void setShellAlpha(float alpha);

    // GPU readback results, ~2 frames old (zero until the first readback lands). Force is the
    // opposing teams' field pressure integrated over this emitter's own bubble; pressure is the
    // mean opposing field strength (how hard the emitter is being pushed on overall).
    glm::vec3 getAppliedForce() const;
    float getPressure() const;
    // Estimated CURRENT bubble radius under external pressure: the closed-form iso profile at the
    // widest station with the threshold raised from iso to max(iso, pressure) — i.e. where the own
    // field meets the (assumed locally uniform) opposing level the pressure readback measured.
    // An average: the true equilibrium surface sits closer on the enemy-facing side. Inherits the
    // readback's ~2-frame latency; 0 = no bubble survives.
    float getEquilibriumRadius() const;
    // Field density at the bubble's CENTER per unit of Output — the budget fold times the
    // distribution gain at the center station (~1.11 for the default centered sphere: Output is a
    // total budget, and the distribution bump concentrates it mid-line). Divide a measured density
    // by this to read it in Output units.
    float getCenterDensityFactor() const;

    // Authored-field getters mirroring the setters above — read the live instance state (valid immediately,
    // unlike the GPU-latched applied force / pressure). Return the type's default for an invalid handle.
    float getOutput() const;
    float getReach() const;
    float getFocus() const;
    float getDistribution() const;
    float getWidth() const;
    uint32 getTeam() const;
    float getShellAlpha() const;

private:
    friend class ForceSystem;
    explicit ForceEmitter(uint64 handle) : m_handle(handle) {}
    uint64 m_handle = 0; // instance index | generation << 32; 0 = invalid (generations start at 1)
};

// RAII handle to a registered world-space point query: "which team's bubble (after deformation)
// contains this point?" Results are GPU-computed and land ~2 frames after the position is set.
export class ForceQuery final
{
public:
    ForceQuery() = default;
    ForceQuery(ForceQuery&& move) noexcept : m_handle(move.m_handle) { move.m_handle = 0; }
    ForceQuery& operator=(ForceQuery&& move) noexcept;
    ForceQuery(const ForceQuery&) = delete;
    ~ForceQuery() { destroy(); }

    bool isValid() const { return m_handle != 0; }
    void destroy();

    // Safe from the parallel entity update (writes only this instance's own slot).
    void setPosition(const glm::vec3& pos);

    struct Result
    {
        uint32 owningTeam = 0;      // strongest team at the point (only meaningful when inside)
        bool inside = false;        // inside owningTeam's bubble (field > iso and beats all others)
        float ownField = 0.0f;      // the STRONGEST team's field at the point — written even when
                                    // not inside any bubble (below iso), so it doubles as the
                                    // density readout; the debug density view heat-maps this value
        float opposingField = 0.0f; // best opposing team's field strength
        bool valid = false;         // false until the first readback for this slot lands

        // The field density at the point (the "Density" debug view's value): the strongest team's
        // field strength, meaningful inside AND outside bubbles.
        float density() const { return ownField; }
    };
    Result getResult() const; // latched by ForceSystem::update, ~2 frames old

private:
    friend class ForceSystem;
    explicit ForceQuery(uint64 handle) : m_handle(handle) {}
    uint64 m_handle = 0;
};

export class ForceSystem final
{
public:
    void initialize(); // registers the "Force" tweaks; call from main before world spawns
    // Pushes every live emitter's GPU config + the query positions to the renderer and latches the
    // GPU readbacks (applied forces, query results). Call once per frame from the main loop, after
    // world.update, before present.
    void update(Renderer& renderer, float deltaSec);

    // direction only matters with focus > 0 (focus 0 = spherical bubble). output must exceed the
    // iso threshold for a bubble to exist at all; reach is the field's hard falloff-to-zero radius
    // (the visible bubble is smaller: r = reach * sqrt(1 - sqrt(iso/output))).
    ForceEmitter createEmitter(uint32 team, const glm::vec3& pos, const glm::vec3& direction,
        float output, float reach, float focus = 0.5f, float distribution = 0.5f, float width = 1.0f);
    ForceQuery createQuery(const glm::vec3& pos);

    uint32 getNumEmitters() const { return m_numLiveEmitters; }
    const ForceFieldParams& getParams() const { return m_params; }

    // The GLOBAL AMBIENT FIELD: an analytic distance-based term one team projects everywhere —
    // zero within safeRadius of the planar center, +slope per metre beyond it, capped at
    // maxStrength. No emitter, never drawn; deforms bubbles and feeds every readback like any
    // field. slope <= 0 disables. Main thread (params push with update()).
    void setAmbientField(uint32 team, const glm::vec2& centerXZ, float safeRadius, float slope, float maxStrength)
    {
        m_params.ambientTeam = team;
        m_params.ambientCenter = centerXZ;
        m_params.ambientSafeRadius = safeRadius;
        m_params.ambientSlope = slope;
        m_params.ambientMaxStrength = maxStrength;
    }

private:
    friend class ForceEmitter;
    friend class ForceQuery;

    struct EmitterInstance
    {
        uint32 generation = 0; // 0 = free slot
        uint32 rendererSlot = UINT32_MAX;
        uint32 team = 0;
        float output = 1.0f;
        float reach = 1.0f;
        float focus = 0.0f;
        float distribution = 0.5f;
        float width = 1.0f;
        float shellAlpha = 1.0f;
        // Cached gain-weighted shape budget integral (1D quadrature; depends only on focus +
        // distribution, refreshed when either changes; width scales the total analytically).
        float distNormE = 1.0f;
        float distNormFocus = -1.0f;
        float distNormD = -1.0f;
        glm::vec3 pos{ 0.0f };
        glm::vec3 dir{ 0.0f, 1.0f, 0.0f };
        glm::vec3 appliedForce{ 0.0f }; // latched from the GPU readback
        float pressure = 0.0f;
    };
    struct QueryInstance
    {
        uint32 generation = 0; // 0 = free slot
        uint32 rendererSlot = UINT32_MAX;
        glm::vec3 pos{ 0.0f };
        ForceQuery::Result result;
    };

    // Refreshes the cached shape budget integral if focus/distribution changed and returns the
    // factor Output is multiplied by before upload: referenceBudget / (shapeBudget * width^2) —
    // TOTAL output is invariant across focus/distribution/width (reach still scales the total).
    float refreshDistributionScale(EmitterInstance& inst) const;
    EmitterInstance* resolveEmitter(uint64 handle);
    const EmitterInstance* resolveEmitter(uint64 handle) const;
    QueryInstance* resolveQuery(uint64 handle);
    void destroyEmitter(uint64 handle);
    void destroyQuery(uint64 handle);
    void debugDrawEmitter(Renderer& renderer, const EmitterInstance& inst) const;

    oc::vector<EmitterInstance> m_emitters; // indexed by handle low 32 bits; slots recycled by generation
    oc::vector<uint32> m_freeEmitters;
    oc::vector<QueryInstance> m_queries;
    oc::vector<uint32> m_freeQueries;
    uint32 m_numLiveEmitters = 0;
    uint32 m_generationCounter = 1;

    ForceFieldParams m_params; // owns the "Force" tweaks, pushed to the renderer every update
    bool m_debugDraw = false;
    bool m_debugDrawQueries = false;
};

export namespace Globals
{
    ForceSystem forceSystem;
}
