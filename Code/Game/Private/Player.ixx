export module Game:Player;

import Core;
import Core.glm;
import Entity;
import Force;

// The player character: upright physics capsule (Entities/Game/player.pre) carrying a personal
// shield ForceEmitter (via its ForceComponent) and a health pool. BATTERY MODEL: while the Energy
// battery holds any charge the shield runs at constant "Max output" — its radius squishes with
// PRESSURE alone (field equilibrium), never with the battery level, so regen refills the bar
// without regrowing the bubble. Pressure drains the battery; at empty the shield COLLAPSES
// (latched) and restarts only once regen reaches "Reboot energy". The opposing field also
// physically pushes the capsule (applied-force readback -> impulse). Health drains while the
// equilibrium radius sits below "Damage radius" AND enemy pressure is actually present.
// All ticks run on the main thread before physics.update (direct body setters sanctioned).
export class GamePlayer final
{
public:
    void registerTweaks();
    void spawn(const glm::vec3& pos); // spawnAssetFile + addRootEntity; creates the territory query
    void despawn();
    // PvP: the player's Force team — the shield, projectiles and the cover check all follow it.
    // Applies to an already-spawned/adopted capsule too (the client learns its team at Welcome).
    void setTeam(uint32 team);
    uint32 team() const { return m_team; }
    // CO-OP CLIENT: the server spawned our player (Component Network + setOwner) — poll-adopt the
    // locally-owned capsule instead of spawning one. Movement/shield/health then run on the SAME
    // code paths (the owner simulates; the claim stream carries the body state to the server).
    void clientAdopt(const glm::vec3& respawnPos);
    float fireInterval() const { return m_fireInterval; }   // server-side rate limit for client
    float meleeInterval() const { return m_meleeInterval; } // combat requests
    // Server: adopt an externally spawned projectile (client combat requests) into the aging +
    // field-push loop, so it despawns and deflects like every other shot.
    void trackProjectile(EntityPtr projectile) { m_projectiles.push_back(Projectile{ std::move(projectile), 0.0f }); }

    // Camera-relative WASD velocity steering + LShift sprint + Space jump off a ground raycast
    // (port of the testbed's updateLocalPlayer). Windowed input only — no-ops without window focus.
    void tickMovement(const glm::vec3& cameraForwardPlanar, float deltaSec);
    // Shield shrink/regen from the shield emitter's pressure readback, push-out impulse, health
    // drain / death respawn.
    void tickShieldAndHealth(float deltaSec);
    // DIRECT damage (enemy unit melee, routed from the server's unit sim). The SHIELD BATTERY
    // absorbs it first ("Damage absorb" energy per hp) — only the overflow, or everything while
    // the shield is collapsed, reaches health. Spawn grace applies; death resolves through
    // tickShieldAndHealth's existing respawn path.
    void applyDamage(float amount);
    // Regeneration (standing near an own-team Base). Never revives past max; dead players are
    // handled by the respawn path, so this is a plain top-up.
    void heal(float amount) { m_health = glm::min(m_health + glm::max(amount, 0.0f), m_healthMax); }

    Entity* entity() const { return m_entity.get(); }
    glm::vec3 interpolatedPos() const; // render-smooth body pose for the follow camera
    glm::vec3 bodyPos() const;         // current body position (authority logic)

    // Combat (neutral mode: LMB shoots at the cursor, F melees). Inputs queue from the windowed
    // tick; tickCombat (authority, pre-physics) spawns/ages projectiles and applies the melee
    // shove. Projectiles carry a small team-0 field — enemy fields decelerate/deflect them via the
    // per-emitter applied-force readback (the testbed force-ball mechanism).
    void queueShot(const glm::vec3& dir) { m_queuedShotDir = dir; m_shotQueued = true; }
    void queueMelee(const glm::vec3& dirPlanar) { m_queuedMeleeDir = dirPlanar; m_meleeQueued = true; }
    void tickCombat(const glm::vec3& cameraForwardPlanar, float deltaSec);
    float meleeRange() const { return m_meleeRange; }
    float meleeFlash() const { return m_meleeFlash; } // seconds left of swing visual
    glm::vec3 meleeDir() const { return m_meleeDir; } // planar direction of the last swing
    bool meleeJustSwung() const { return m_meleeSwung; } // true for the tick the swing landed
                                                         // (GameMatch feeds it to NpcSystem damage)

    float health() const { return m_health; }
    float healthMax() const { return m_healthMax; }
    // Energy IS the shield resource: the bar drains/refills in its own units ("Energy regen/s",
    // "Energy drain/s @ pressure 1" tweaks); the emitter's field output is derived from the
    // energy fraction (full energy = "Max output").
    float energy() const { return m_energy; }
    float energyMax() const { return m_energyMax; }
    float shieldFrac() const { return m_energyMax > 0.0f ? m_energy / m_energyMax : 0.0f; }
    float shieldRadius() const; // pressure-aware equilibrium radius estimate, 0 = no bubble
    // MATERIALS inventory: refilled near own-team Silos/Base, invested into nearby blueprints.
    // Server-authoritative for every player (GameMatch runs the transfers); clients receive their
    // own value through the GSh mirror.
    float materials() const { return m_materials; }
    float materialsMax() const { return m_materialsMax; }
    void setMaterials(float value) { m_materials = glm::clamp(value, 0.0f, m_materialsMax); }
    float pressure() const { return m_lastPressure; } // last readback, for the HUD (threshold tuning)
    float density() const { return m_lastDensity; }   // field density at the body (strongest team's
                                                      // field, the Density debug view's value)
    bool shieldCollapsed() const { return m_shieldCollapsed; }
    // Collapse/reboot EDGE since the last poll — the co-op shield mirror flushes an event
    // immediately on it instead of waiting for the periodic send.
    bool takeShieldEdge() { const bool e = m_shieldEdge; m_shieldEdge = false; return e; }

private:
    struct Projectile
    {
        EntityPtr entity;
        float age = 0.0f;
    };

    EntityPtr m_entity;
    ForceQuery m_query; // point query at the body center — feeds the density readout
    std::vector<Projectile> m_projectiles;
    glm::vec3 m_spawnPos{ 0.0f };
    glm::vec3 m_queuedShotDir{ 0.0f };
    glm::vec3 m_queuedMeleeDir{ 0.0f };
    glm::vec3 m_meleeDir{ 0.0f, 0.0f, -1.0f };
    float m_fireCooldown = 0.0f;
    float m_meleeCooldown = 0.0f;
    float m_meleeFlash = 0.0f;
    bool m_shotQueued = false;
    bool m_meleeQueued = false;
    bool m_meleeSwung = false;
    bool m_jumpWasDown = false;

    uint32 m_team = 0;
    float m_health = 100.0f;
    float m_energy = 100.0f;
    float m_materials = 0.0f; // carried construction stock (see materials())
    float m_lastPressure = 0.0f;
    float m_lastDensity = 0.0f;
    float m_outputHistory[3] = { 1.5f, 1.5f, 1.5f }; // outputs of recent ticks — the applied-force
                                                     // readback is ~2 frames latent and scales with
                                                     // output, so the push normalizes by the output
                                                     // that PRODUCED it (shield-state-independent)
    float m_graceTimer = 0.0f;      // seconds of post-spawn drain immunity (stale readbacks)
    bool m_shieldCollapsed = false; // latched at empty battery, cleared at "Reboot energy"
    bool m_shieldEdge = false;      // collapse/reboot happened this tick (co-op mirror flush)

    // Tweaks ("Game/Player", "Game/Shield")
    float m_moveSpeed = 4.0f;
    float m_accel = 30.0f; // deliberately soft: steering force must lose against bubble push
    float m_jumpSpeed = 6.0f;
    float m_sprintMult = 2.0f;
    float m_healthMax = 100.0f;
    float m_healthDrainRate = 15.0f;    // health/s while unshielded in enemy territory
    float m_shieldMaxOutput = 1.5f;     // field output while the battery holds ANY charge
    float m_energyMax = 100.0f;
    float m_energyRegenRate = 10.0f;    // energy/s refill (battery only — never grows the bubble)
    float m_energyDrainRate = 50.0f;    // energy/s drained per unit of pressure
    float m_rebootEnergy = 20.0f;       // collapsed shield restarts once the battery refills to this
    float m_damageAbsorb = 2.0f;        // shield ENERGY spent per hp of direct damage absorbed
                                        // (0 = the shield absorbs nothing, hits go straight to hp)
    float m_coverDrainReduction = 0.75f; // drain reduction per unit of FRIENDLY field surplus over
                                         // the own output (standing inside a team emitter's bubble)
    float m_materialsMax = 50.0f;        // inventory size ("Game/Player/Materials max")
    float m_spawnGraceSec = 1.0f;        // no energy/health drain this long after (re)spawn — the
                                         // GPU readbacks still carry the death position for ~2 frames
    // Tweaks ("Game/Combat")
    float m_projSpeed = 30.0f;
    float m_projLifetime = 5.0f;
    float m_projPushGain = 10000.0f;     // field applied-force -> impulse (force-ball scale)
    float m_fireInterval = 0.25f;
    float m_meleeRange = 3.0f;
    float m_meleeImpulse = 600.0f;
    float m_meleeInterval = 0.5f;
    float m_damageRadius = 0.8f;        // metres: health drains once the equilibrium shield radius
                                        // squishes below this (capsule half-height is 0.8 world)
    float m_shieldPushGain = 10000.0f;  // applied-force -> impulse scale (testbed force-ball precedent)
    float m_shieldTension = 1.5f;       // SURFACE TENSION: push AND energy drain scale by
                                        // (1 + tension * pressure) — leaning deep into a bubble
                                        // stiffens superlinearly and burns both sides' batteries
};
