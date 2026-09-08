export module Force:ForceSystem;

import Core;
import Core.glm;
import RendererVK;
import Threading;
export import :Emitter; // the ForceEmitter handle (Emitter.ixx / Emitter.cpp)

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

// The CPU mirrors of the shader's field math, shared with the ForceEmitter handle's readbacks
// (Emitter.cpp). Module linkage, not exported; bodies in ForceSystem.cpp.
float forceDistributionGain(float t, float D); // the axial density bump (force_field.inc.glsl forceDistributionGain)
float forceReferenceBudget();                  // the plain sphere's budget integral every shape is folded onto

// CPU emitter instances — DELIBERATELY far above the renderer's MAX_FORCE_EMITTERS GPU slots.
// Every unit in a co-op map carries a bubble (the "Max enemy units" cap is 25000), but only the
// ones the SIM LOD keeps ACTIVE hold a renderer slot; the rest are instance-only and cost nothing
// on the GPU. m_emitters is RESERVED to this at initialize(), so it never reallocates under a
// lock-free resolveEmitter (32768 x ~192 B = ~6 MB).
export constexpr uint32 MAX_FORCE_INSTANCES = 32768;

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

    // ---- THE BAKED PRESSURE FIELD ("Force/Bake" tweaks) ------------------------------------
    // A sparse CPU-side sampling of EVERY team's field: update() selects 16 m XZ chunks from the
    // live emitters'/groups' support boxes, the GPU evaluates 16x16 samples per chunk at "Sample
    // height" (force_bake.cs), and update() republishes the paired readback copy — so ANY number
    // of consumers sample field force/exposure with plain bilinear taps and NO per-consumer GPU
    // slot (the swarm-unit replacement for per-unit ForceQueries).
    struct FieldSample
    {
        bool valid = false;   // false = bake disabled or nothing published yet (callers fall back)
        bool inside = false;  // inside owningTeam's bubble at the bake height
        uint32 owningTeam = 0;
        float field = 0.0f;                 // the STRONGEST team's field (owningTeam's), meaningful
                                            // outside bubbles too — the density readout
        float opposing = 0.0f;              // strongest field of any team != the sampled team
        glm::vec3 opposingGradient{ 0.0f }; // planar (XZ) gradient of that field
    };
    // Worker-safe between updates (the published containers only mutate in update(), after the
    // entity pass). A position outside every chunk reads as ZERO field — correct by construction,
    // the chunks cover every support box. ~3 frames latent end to end.
    FieldSample sampleBakedField(const glm::vec3& pos, uint32 team) const;

    uint32 getNumEmitters() const { return m_numLiveEmitters; }
    uint32 getNumMergeGroups() const { return (uint32)m_statGroups; }
    uint32 getNumMergedEmitters() const { return (uint32)m_statMerged; }
    const ForceFieldParams& getParams() const { return m_params; }

    // The LIVE team count (2..MAX_FORCE_TEAMS) — a GAME-MODE setting, not a tweak (co-op = 2):
    // the renderer recompiles the force shaders and remakes the team-sized bake volume/buffers
    // when the pushed params change (one device idle). Team values on emitters/queries clamp
    // below it. Call before the mode's world spawns (main thread).
    void setNumTeams(uint32 numTeams);
    uint32 numTeams() const { return m_params.numTeams; }
    // The proxy/interval draw-box shrink's iso reduction (packVisibleBounds in ForceSystem.cpp; read
    // from the upload workers, written only by the tweak panel between passes). 0 = off.
    float visibleBoundsIsoFrac() const { return m_visibleBoundsIsoFrac; }


private:
    friend class ForceEmitter;
    friend class ForceQuery;

    struct EmitterInstance
    {
        uint32 generation = 0; // 0 = free slot
        uint32 rendererSlot = UINT32_MAX; // ONLY while active: minted/retired by update(), never at create
        uint32 team = 0;
        bool active = true; // see ForceEmitter::setActive
        bool analyticReadback = false; // see ForceEmitter::setAnalyticReadback
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
        float maxRadius = 8.0f;      // a group whose cover would exceed this refuses the member
        int maxMembers = 255;
        int minMembers = 2;          // smaller groups dissolve
        float sumFraction = 0.2f;    // group output = max(largest member, sum * fraction)
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
    // One emitter's GPU upload: transition sphere, merge flags, the readback latch and the debug
    // rings. Runs on the upload jobs (own instance + own renderer slot only) and serially in
    // update() for an emitter that just took a slot back. The caller guarantees a valid slot.
    void uploadEmitter(Renderer& renderer, EmitterInstance& inst, float blendStep);
    // The bake-tap readback (the force_emitter.cs integral mirrored on the CPU): centre + a ring
    // of taps at 0.35 x reach over the uploaded shape `gpu`, each weighted by the emitter's own
    // normalized field there, reading the PUBLISHED pressure bake (last frame's). Worker-safe.
    void bakedReadback(EmitterInstance& inst, const RendererVKLayout::ForceEmitterGpu& gpu) const;

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
    // OWNER-SLICED staging for a pass: one slot per chunk the pass hands out (the inline path is
    // one chunk; JobSystem::numChunks above it), so a pass's staging memory scales with its item
    // count, not with the scheduler's context count as a PerWorker would. fn reads its slot at
    // `slots[begin / grain]`; the serial drain walks exactly passSlots() of them.
    static uint32 passSlots(uint32 count, uint32 grain, uint32 minParallel)
    {
        return count < minParallel ? 1u : JobSystem::numChunks(count, grain); // inline calls fn(0, count) even at 0
    }
    template<typename T>
    static void prepareSlots(oc::vector<T>& slots, uint32 n) // grow (capacity kept) + clear the n in use
    {
        if (slots.size() < n)
            slots.resize(n);
        for (uint32 i = 0; i < n; ++i)
            slots[i].clear();
    }
    void refreshBubbleBounds(EmitterInstance& inst, oc::vector<uint32>& candidates); // on jobs: writes inst + the chunk's candidate slot
    uint32 createGroup(uint32 team);
    void dissolveGroup(uint32 groupIdx); // Merged members start Leaving from the group's displayed sphere
    void beginJoin(EmitterInstance& inst, uint32 groupIdx, const glm::vec3& fromCenter, float fromRadius);
    void beginLeave(EmitterInstance& inst, const glm::vec3& fromCenter, float fromRadius);
    bool recomputeCover(MergeGroup& group); // targets; false = fewer than minMembers live members remain
    void smoothGroup(MergeGroup& group, float deltaSec); // displayed <- target, floored by the Merged cover
    // Reach of a focus-0.5 sphere whose visible iso radius is `radius` at `output`; 0 = no bubble.
    float sphereReach(float radius, float output) const;
    // Baked pressure field: chunk set from the emitter/group support boxes -> renderer upload,
    // then the paired readback republished for the samplers. Both main-thread inside update().
    void buildBakeChunks(Renderer& renderer);
    void publishBake(Renderer& renderer);
    // Radius a group sphere at `center` needs to cover this member (MergeParams scales, no margin).
    float memberCover(const EmitterInstance& m, const glm::vec3& center) const
    {
        return m_merge.spreadScale * glm::distance(m.bubbleCenter, center) + m_merge.radiusScale * m.bubbleRadius;
    }
    void debugDrawGroup(Renderer& renderer, const MergeGroup& group) const;

    // Parallel entity spawning: create/destroy of emitters and queries run concurrently from spawn
    // jobs — the free lists and generation counter serialize here. m_emitters is RESERVED to
    // MAX_FORCE_INSTANCES and m_queries to the renderer's MAX at initialize(), so growth never
    // reallocates under a concurrent resolveEmitter/resolveQuery (the setters stay lock-free, same
    // as the parallel-pass contract).
    std::mutex m_createMutex;
    oc::vector<EmitterInstance> m_emitters; // indexed by handle low 32 bits; slots recycled by generation
    oc::vector<uint32> m_freeEmitters;
    oc::vector<QueryInstance> m_queries;
    oc::vector<uint32> m_freeQueries;
    oc::vector<MergeGroup> m_groups;
    oc::vector<uint32> m_freeGroups;
    // Join-pass staging: the neighbour pass appends each candidate's pairs (i << 32 | j, i < j)
    // into its CHUNK's slot (owner-sliced, see passSlots); the serial union pass drains them.
    oc::vector<oc::vector<uint64>> m_pairStaging;
    // Candidate cell list: the bounds pass stages candidate indices per chunk + CAS-maxes the
    // largest join radius; serially they become (cellKey, emitter) pairs sorted by key (cell =
    // 2 x that radius, so a 3x3x3 neighbourhood holds every possible partner), and the neighbour
    // pass binary-searches the 27 cells — a private structure over the candidates only, instead
    // of the entity SpatialIndex whose finest cells are full of render entries to filter.
    oc::vector<oc::vector<uint32>> m_candidateStaging;
    // Renderer-slot churn staging: the upload jobs see the ACTIVE gate flip and stage the emitter
    // index in their chunk's slot; update() mints/retires the slots SERIALLY right after, since
    // the renderer's create/destroy grow vectors those same jobs are indexing.
    struct SlotChurn
    {
        oc::vector<uint32> acquire;
        oc::vector<uint32> release;
        void clear() { acquire.clear(); release.clear(); }
    };
    static constexpr uint32 c_uploadGrain = 64; // the "Force upload" parallelFor's grain (slots index by it)
    oc::vector<SlotChurn> m_slotChurn;
    oc::vector<oc::pair<uint64, uint32>> m_cells;
    oc::atomic<uint32> m_maxJoinRadiusBits = 0; // float bits (positive floats order as uints)
    JobCounter m_mergeCounter; // the in-flight merge job (update kicks -> joinMerge joins next frame)
    bool m_mergeKicked = false;
    float m_mergeDeltaSec = 0.0f;
    oc::vector<uint32> m_retiredGroupSlots; // dissolved on the job; destroyed on main in update()
    uint32 m_numLiveEmitters = 0;
    uint32 m_numSlottedEmitters = 0; // of those, the ACTIVE ones holding a renderer slot
    uint32 m_generationCounter = 1;
    bool m_slotCapWarned = false;     // printed once per starvation stretch, cleared when it ends
    bool m_instanceCapWarned = false; // once
    int m_statEmitters = 0;           // read-only stats bound under Force
    int m_statSlots = 0;

    float m_visibleBoundsIsoFrac = 1.0f; // draw-box shrink (packVisibleBounds); 0 = full boxes

    // Baked pressure field state (see sampleBakedField): scratch this frame, published last copy.
    bool m_bakeEnabled = true;
    float m_bakeSampleHeight = 1.0f; // world y the field is evaluated at (where bodies live)
    int m_statBakeChunks = 0;
    bool m_bakePublished = false;
    bool m_bakeCapWarned = false;
    oc::vector<glm::ivec4> m_bakeChunkScratch;
    // Chunk-key set for the selection: a STAMP-cleared open-addressing table (no per-frame clear,
    // no sort) whose first-seen keys land in `unique`. One per worker for the boxes pass, one
    // for the serial merge of the workers' unique lists. Past the probe limit a key is appended
    // unchecked — the cap step dedups the (rare) survivors. Deliberately still a PerWorker (not
    // owner-sliced like the vector stagings): a slot is a fixed 4096-entry table, and its dedupe
    // pays off with FEW, FULL slots — one per 256-emitter chunk would cost more memory on a big
    // map than one per context, and hand the serial merge many more near-duplicate lists.
    struct BakeKeySet
    {
        static constexpr uint32 SIZE = 4096; // power of two; ~8x the chunk cap
        oc::vector<uint64> keys;
        oc::vector<uint32> stamps;
        uint32 stamp = 0;
        oc::vector<uint64> unique;
        bool dirty = false; // an over-probed append happened: `unique` may hold duplicates
        void begin();
        void insert(uint64 key);
    };
    PerWorker<BakeKeySet> m_bakeKeyStaging;
    BakeKeySet m_bakeMerge;
    // Published chunk lookup: a DENSE uint16 grid over the chunk coords' bounding box when it fits
    // (the normal case — O(1), two subtractions and a multiply per corner), else the sorted key
    // list and a binary search.
    oc::vector<uint16> m_bakeGrid;                  // chunk index or UINT16_MAX
    glm::ivec2 m_bakeGridLo{ 0 };
    glm::ivec2 m_bakeGridSize{ 0 };                 // 0 = the sorted fallback is live
    oc::vector<oc::pair<uint64, uint32>> m_bakeSorted; // (key, chunk index), by key
    static constexpr uint32 MAX_BAKE_GRID_CELLS = 32768; // 64 KB of uint16
    uint32 findBakeChunk(int bx, int bz) const;      // UINT32_MAX = no chunk
    oc::vector<glm::vec4> m_bakeData;               // published readback copy, sized ONCE to the cap
                                                    // (vec4PerChunk x MAX_FORCE_BAKE_CHUNKS): never resized

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
