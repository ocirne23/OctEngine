export module Force:Emitter;

import Core;
import Core.glm;

export class ForceSystem;

// RAII handle to a live emitter of the ForceSystem (see ForceSystem.ixx for the field model).
// Move-only, like ParticleEffect/PhysicsBody. The setters store into the instance's OWN slot, so
// they are safe from the parallel entity update pass (one writer per instance); create/destroy
// run under the system's create mutex.
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
    // ACTIVE gate (default on): off = the emitter keeps its instance and every parameter but
    // projects NO field this frame, and its GPU SLOT GOES BACK — skipped by the grid, the draw,
    // the compute and the bake, no readback (appliedForce/pressure read zero), evicted from and
    // never a candidate for merging. The first update() that sees it active again mints a new
    // slot. So a far unit's bubble costs one of MAX_FORCE_INSTANCES, never one of the much
    // scarcer MAX_FORCE_EMITTERS renderer slots. Pass-safe like setOutput (the slot churn itself
    // runs serially in update()). The World's SIM LOD drives it by tier.
    void setActive(bool active);
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
    // READBACK SOURCE for getAppliedForce / getPressure. Default OFF = the CPU pressure bake: a
    // ring of bilinear taps over the bubble at the bake height, no GPU work per emitter (and no
    // slot needed while merged). ON = the GPU integral (force_emitter.cs, ~2 frames latent) —
    // for emitters that leave the ground band, since the bake is planar: projectiles, lobs. The
    // bake being disabled or not yet published falls every emitter back to the GPU path.
    void setAnalyticReadback(bool analytic);
    // MERGING (on by default; false opts out): same-team mergeable emitters whose bubbles overlap are
    // carried by ONE group emitter on the GPU (see ForceSystem's "Force/Merge" tweaks). While merged
    // this emitter projects no field of its own — the group's covers it entirely — and it rejoins
    // the field as itself before its own bubble would leave the group's. Every setter/getter keeps
    // working; getAppliedForce/getPressure read either this emitter's own passive evaluation or the
    // group's shared readback ("Member readback").
    void setMergeable(bool mergeable);
    bool getMergeable() const;
    bool isMerged() const; // currently carried by a group emitter
    // Bounding sphere of the bubble this emitter is currently part of: the GROUP's displayed
    // sphere while merged (groupId = 1 + group index — the same for every member, a dedupe key),
    // else its own uncontested iso bubble (groupId 0). False = no bubble (invalid, gated off,
    // below iso). Main thread between joinMerge and update() — the group list is the merge job's.
    bool getBubbleBounds(glm::vec3& center, float& radius, uint32* groupId = nullptr) const;

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
