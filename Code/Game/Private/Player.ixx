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

    // Camera-relative WASD velocity steering + LShift sprint + Space jump off a ground raycast
    // (port of the testbed's updateLocalPlayer). Windowed input only — no-ops without window focus.
    void tickMovement(const glm::vec3& cameraForwardPlanar, float deltaSec);
    // Shield shrink/regen from the shield emitter's pressure readback, push-out impulse, health
    // drain / death respawn.
    void tickShieldAndHealth(float deltaSec);

    Entity* entity() const { return m_entity.get(); }
    glm::vec3 interpolatedPos() const; // render-smooth body pose for the follow camera
    glm::vec3 bodyPos() const;         // current body position (authority logic)

    float health() const { return m_health; }
    float healthMax() const { return m_healthMax; }
    // Energy IS the shield resource: the bar drains/refills in its own units ("Energy regen/s",
    // "Energy drain/s @ pressure 1" tweaks); the emitter's field output is derived from the
    // energy fraction (full energy = "Max output").
    float energy() const { return m_energy; }
    float energyMax() const { return m_energyMax; }
    float shieldFrac() const { return m_energyMax > 0.0f ? m_energy / m_energyMax : 0.0f; }
    float shieldRadius() const; // pressure-aware equilibrium radius estimate, 0 = no bubble
    float pressure() const { return m_lastPressure; } // last readback, for the HUD (threshold tuning)
    float density() const { return m_lastDensity; }   // field density at the body (strongest team's
                                                      // field, the Density debug view's value)

private:
    EntityPtr m_entity;
    ForceQuery m_query; // point query at the body center — feeds the density readout
    glm::vec3 m_spawnPos{ 0.0f };
    bool m_jumpWasDown = false;

    float m_health = 100.0f;
    float m_energy = 100.0f;
    float m_lastPressure = 0.0f;
    float m_lastDensity = 0.0f;
    float m_graceTimer = 0.0f;      // seconds of post-spawn drain immunity (stale readbacks)
    bool m_shieldCollapsed = false; // latched at empty battery, cleared at "Reboot energy"

    // Tweaks ("Game/Player", "Game/Shield")
    float m_moveSpeed = 8.0f;
    float m_accel = 60.0f;
    float m_jumpSpeed = 6.0f;
    float m_sprintMult = 2.0f;
    float m_healthMax = 100.0f;
    float m_healthDrainRate = 15.0f;    // health/s while unshielded in enemy territory
    float m_shieldMaxOutput = 1.5f;     // field output while the battery holds ANY charge
    float m_energyMax = 100.0f;
    float m_energyRegenRate = 15.0f;    // energy/s refill (battery only — never grows the bubble)
    float m_energyDrainRate = 25.0f;    // energy/s drained per unit of pressure
    float m_rebootEnergy = 20.0f;       // collapsed shield restarts once the battery refills to this
    float m_coverDrainReduction = 0.75f; // drain reduction per unit of FRIENDLY field surplus over
                                         // the own output (standing inside a team emitter's bubble)
    float m_spawnGraceSec = 1.0f;        // no energy/health drain this long after (re)spawn — the
                                         // GPU readbacks still carry the death position for ~2 frames
    float m_damageRadius = 0.8f;        // metres: health drains once the equilibrium shield radius
                                        // squishes below this (capsule half-height is 0.8 world)
    float m_shieldPushGain = 10000.0f;  // applied-force -> impulse scale (testbed force-ball precedent)
};
