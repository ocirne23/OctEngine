export module Force:System;

import Core;
import Core.glm;
import RendererVK;
import Threading;

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
    // MERGING (on by default; false opts out): same-team mergeable emitters whose bubbles overlap are
    // carried by ONE group emitter on the GPU (see ForceSystem's "Force/Merge" tweaks). While merged
    // this emitter projects no field of its own — the group's covers it entirely — and it rejoins
    // the field as itself before its own bubble would leave the group's. Every setter/getter keeps
    // working; getAppliedForce/getPressure read either this emitter's own passive evaluation or the
    // group's shared readback ("Member readback").
    void setMergeable(bool mergeable);
    bool getMergeable() const;
    bool isMerged() const; // currently carried by a group emitter

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
    // The merge pass (updateMerging) runs as ONE job PIPELINED A FRAME: update() kicks it after the
    // upload, over this frame's post-sim emitter state, and joinMerge() (main loop top, right after
    // the UI join — before input/entity-change drains can create or destroy emitters) joins it, so
    // it overlaps present + the fence wait instead of anything main needs. The group spheres and
    // member transitions the next upload uses are therefore ONE FRAME behind the emitters — a few
    // cm at unit speeds, inside the cover margin, and every membership rule has hysteresis. The
    // job never touches the renderer: group slots are allocated/retired on main inside update().
    void joinMerge();

    // direction only matters with focus > 0 (focus 0 = spherical bubble). output must exceed the
    // iso threshold for a bubble to exist at all; reach is the field's hard falloff-to-zero radius
    // (the visible bubble is smaller: r = reach * sqrt(1 - sqrt(iso/output))).
    ForceEmitter createEmitter(uint32 team, const glm::vec3& pos, const glm::vec3& direction,
        float output, float reach, float focus = 0.5f, float distribution = 0.5f, float width = 1.0f);
    ForceQuery createQuery(const glm::vec3& pos);

    uint32 getNumEmitters() const { return m_numLiveEmitters; }
    uint32 getNumMergeGroups() const { return (uint32)m_statGroups; }
    uint32 getNumMergedEmitters() const { return (uint32)m_statMerged; }
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
        // Merging (updateMerging owns everything below; mergeable is the consumer's opt-OUT, default on).
        // Membership goes Own -> Joining -> Merged -> Leaving -> Own: while Joining/Leaving the
        // emitter uploads an ACTIVE sphere of its own output whose centre/visible radius lerp between
        // its own bubble and the group's displayed sphere (blend 0..1 over "Blend time"), so a unit
        // is covered by its own field until the group sphere has grown over it, and gets a field
        // back AT the group sphere before that shrinks away. Merged = no field (PASSIVE / skipped).
        enum class EMergeState : uint8 { Own, Joining, Merged, Leaving };
        bool mergeable = true;
        EMergeState mergeState = EMergeState::Own;
        uint32 group = 0;               // 1 + m_groups index while Joining/Merged, 0 = Own/Leaving
        float blend = 0.0f;             // transition progress
        glm::vec3 bubbleCenter{ 0.0f }; // bounding sphere of the uncontested iso bubble, refreshed
        float bubbleRadius = 0.0f;      // per update for mergeable emitters (0 = no bubble: never merges)
        glm::vec3 prevBubbleCenter{ 0.0f }; // last refresh's centre: the group sphere follows member motion
        bool bubbleValid = false;           // prevBubbleCenter holds a real previous refresh
        bool candidate = false;             // this frame: mergeable with a bubble (written by its own refresh)
        // Bubble radius cache: the 16-station profile only re-evaluates when a shape parameter
        // (or the iso threshold) changed — a moving unit just translates the centre.
        float boundsOutput = -1.0f, boundsReach = -1.0f, boundsFocus = -1.0f, boundsDist = -1.0f, boundsWidth = -1.0f, boundsIso = -1.0f;
        glm::vec3 blendFromCenter{ 0.0f }; // transition start sphere
        float blendFromRadius = 0.0f;
        glm::vec3 blendCenter{ 0.0f };     // the sphere uploaded last frame (a reversal restarts from it)
        float blendRadius = 0.0f;
    };
    // A merge group: one GPU sphere emitter covering every member's iso bubble (+ margin). Members
    // upload PASSIVE (own readback) or not at all (shared readback), per "Member readback". The
    // DISPLAYED sphere eases toward the TARGET (recomputeCover) over "Smooth time", floored by the
    // cover of the Merged members (they have no field of their own) at the displayed centre.
    struct MergeGroup
    {
        uint32 generation = 0; // 0 = free slot
        uint32 rendererSlot = UINT32_MAX;
        uint32 team = 0;
        oc::vector<uint32> members; // emitter indices; pruned lazily (destroyed / re-created slots)
        glm::vec3 targetCenter{ 0.0f };
        float targetRadius = 0.0f;
        float targetOutput = 0.0f;
        glm::vec3 center{ 0.0f };  // displayed
        float coverRadius = 0.0f;  // displayed iso radius of the group bubble
        float output = 0.0f;       // displayed (unfolded) output
        float reach = 0.0f;        // the uploaded sphere's reach (pos = center - up * reach/2)
        float sumOutput = 0.0f;    // members' summed output: the shared-readback force split
        float shellAlpha = 1.0f;
        glm::vec3 appliedForce{ 0.0f }; // latched readback (shared mode hands it to the members)
        float pressure = 0.0f;
        bool dissolve = false;          // set by the parallel cover pass, acted on serially (renderer slot)
    };
    struct MergeParams
    {
        bool enabled = true;
        float joinDistance = 0.5f;   // join when |ci - cj| < joinDistance * (ri + rj)
        float leaveDistance = 0.85f; // leave when no member is closer than leaveDistance * (ri + rj)
                                     // (< 1: the own bubble reappears while still overlapping the group)
        // Cover of one member from the group centre = spreadScale * |c - centre| + radiusScale * r;
        // the group radius = coverScale * max over members + coverMargin. 1/1/1 covers every member
        // bubble exactly; below 1 the group sphere hugs the crowd more tightly at the price of
        // members' own bubble rims sticking out of it near the edge (the tuned default: 1/1/0.85).
        float spreadScale = 1.0f;
        float radiusScale = 1.0f;
        float coverScale = 0.85f;
        float coverMargin = 0.2f;    // metres added around the members' cover
        float maxRadius = 4.0f;      // a group whose cover would exceed this refuses the member
        int maxMembers = 255;
        int minMembers = 2;          // smaller groups dissolve
        float sumFraction = 0.5f;    // group output = max(largest member, sum * fraction)
        bool memberReadback = true;  // members stay on the GPU as PASSIVE for their own force/pressure
        float smoothTime = 0.3f;     // group sphere easing time constant (s)
        float blendTime = 0.5f;      // member join/leave transition duration (s)
        float leaveFromGroup = 0.5f; // where a Leaving sphere starts: 0 = the own bubble (instant own
                                     // field, no ghost), 1 = a full copy of the group sphere shrinking
                                     // onto the unit (reads as an empty bubble left behind)
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

    // The merge job body: refresh bubble bounds, leave pass (members that drifted / changed team /
    // lost their bubble), candidate cells + neighbour pairs, union pass (new groups, joins,
    // group-group merges), cover recompute + dissolve of undersized groups. No renderer access.
    void updateMerging(float deltaSec);
    // A per-item pass: inline under minParallel items (a parallelFor's submit/wake/join costs more
    // than a handful of items), a parallelFor above it. fn(begin, end) either way.
    template<typename Func>
    void runPass(uint32 count, uint32 grain, uint32 minParallel, JobProfile profile, Func&& fn)
    {
        if (count < minParallel)
        {
            ProfileScope scope(profile.name, profile.category);
            fn(0u, count);
        }
        else
            Globals::jobSystem.parallelFor(0u, count, grain, profile, fn);
    }
    void refreshBubbleBounds(EmitterInstance& inst); // on jobs: writes inst + the PerWorker candidate staging
    uint32 createGroup(uint32 team);
    void dissolveGroup(uint32 groupIdx); // Merged members start Leaving from the group's displayed sphere
    void beginJoin(EmitterInstance& inst, uint32 groupIdx, const glm::vec3& fromCenter, float fromRadius);
    void beginLeave(EmitterInstance& inst, const glm::vec3& fromCenter, float fromRadius);
    bool recomputeCover(MergeGroup& group); // targets; false = fewer than minMembers live members remain
    void smoothGroup(MergeGroup& group, float deltaSec); // displayed <- target, floored by the Merged cover
    // Reach of a focus-0.5 sphere whose visible iso radius is `radius` at `output`; 0 = no bubble.
    float sphereReach(float radius, float output) const;
    // Radius a group sphere at `center` needs to cover this member (MergeParams scales, no margin).
    float memberCover(const EmitterInstance& m, const glm::vec3& center) const
    {
        return m_merge.spreadScale * glm::distance(m.bubbleCenter, center) + m_merge.radiusScale * m.bubbleRadius;
    }
    void debugDrawGroup(Renderer& renderer, const MergeGroup& group) const;

    oc::vector<EmitterInstance> m_emitters; // indexed by handle low 32 bits; slots recycled by generation
    oc::vector<uint32> m_freeEmitters;
    oc::vector<QueryInstance> m_queries;
    oc::vector<uint32> m_freeQueries;
    oc::vector<MergeGroup> m_groups;
    oc::vector<uint32> m_freeGroups;
    // Join-pass staging: the neighbour pass appends each candidate's pairs (i << 32 | j, i < j)
    // per worker; the serial union pass drains them.
    PerWorker<oc::vector<uint64>> m_pairStaging;
    // Candidate cell list: the bounds pass stages candidate indices per worker + CAS-maxes the
    // largest join radius; serially they become (cellKey, emitter) pairs sorted by key (cell =
    // 2 x that radius, so a 3x3x3 neighbourhood holds every possible partner), and the neighbour
    // pass binary-searches the 27 cells — a private structure over the candidates only, instead
    // of the entity SpatialIndex whose finest cells are full of render entries to filter.
    PerWorker<oc::vector<uint32>> m_candidateStaging;
    oc::vector<oc::pair<uint64, uint32>> m_cells;
    oc::atomic<uint32> m_maxJoinRadiusBits = 0; // float bits (positive floats order as uints)
    JobCounter m_mergeCounter; // the in-flight merge job (update kicks -> joinMerge joins next frame)
    bool m_mergeKicked = false;
    float m_mergeDeltaSec = 0.0f;
    oc::vector<uint32> m_retiredGroupSlots; // dissolved on the job; destroyed on main in update()
    uint32 m_numLiveEmitters = 0;
    uint32 m_generationCounter = 1;

    ForceFieldParams m_params; // owns the "Force" tweaks, pushed to the renderer every update
    MergeParams m_merge;       // the "Force/Merge" tweaks
    int m_statGroups = 0;      // read-only stats bound under Force/Merge
    int m_statMerged = 0;
    bool m_debugDraw = false;
    bool m_debugDrawQueries = false;
    bool m_debugDrawGroups = false;
};

export namespace Globals
{
    ForceSystem forceSystem;
}
