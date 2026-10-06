export module Settings.World;

import Core;

// SIM LOD ("Game/Sim LOD" tweaks, not Saved): which entities the World's update pass visits and how
// often their SIMULATION components tick, by the distance to the nearest FOCUS point (the
// players - the Game layer publishes them; the testbed uses the camera). The SpatialIndex stamps
// three UpdateTier passes (balls at radius[0..2] around every focus point); an entity's own
// stamps give its DISTANCE tier: 0 = every frame, 1/2 = time-based intervals, 3 = DORMANT
// (beyond radius[2]: not visited at all - no sim, no render push, subtree skipped). The visit
// set comes from one sphere query per focus point (radius[2] + queryMargin), run as a POST-UPDATE
// job for the NEXT pass (see World::computeSelection), plus the Global and freshly added roots; a
// visited parent emits only stamped children. The decision is PER ENTITY in the World's batch job:
// an entity is THROTTLED when it carries a following kind and no pinning kind (a kind with
// follow = false pins the entity to full rate while selected); the bubble gate and the dormant
// physics edge apply to every selected entity by distance. The entity itself only sees the delta
// it is handed (0 = skipped frame: sync + placement, no sim step). Full account in
// Code/Entity/CONTEXT.md.
export struct SimLodConfig
{
    bool enabled = true;
    bool horizontal = true;      // XZ distance (top-down game); off = full 3D distance
    float radius[3] = { 25.0f, 50.0f, 225.0f }; // tier t applies while dist < radius[t]; beyond radius[2] = dormant
    // Tick cadence for tier 1, tier 2, dormant: TIME-based (seconds between ticks; dormant 0 =
    // never) with a MINIMUM frame gap so a low frame rate still skips frames. The tick receives
    // the exact sim time it covers (World keeps a per-frame time ring; nothing on the entity).
    float intervalSec[3] = { 0.25f, 1.0f, 0.0f };
    int minFrames[3] = { 4, 16, 8 };
    float intervalJitter = 0.25f;  // per-entity +-fraction on the interval so a wave that entered a tier
                                   // together spreads out instead of ticking in lockstep
    bool dormantDisableBody = true; // dormant edge: DISABLE a throttled entity's physics body (out of the
                                    // broadphase + solver, pose kept - nothing can wake it) instead of
                                    // only parking it asleep; re-enabled on the wake edge either way
    int forceMaxTier = 1;          // a ForceComponent's bubble is ACTIVE only while its entity's tier
                                   // is <= this (3 = always); applies to every selected entity with a
                                   // bubble, throttled or not (structures included)
    int buoyancyMaxTier = 1;       // a dynamic PhysicsComponent body runs its buoyancy probes only
                                   // while its entity's tier is <= this (3 = always)
    int visibleMaxTier = 2;      // TICK-RATE floor for an in-view entity inside the outer radius (0 = full
                                 // rate on screen); never affects the bubble gate or dormancy, which go
                                 // by distance alone. 2 = distance rules everything (default)
    float queryMargin = 10.0f;   // the selection query reaches radius[2] + this, so an entity LEAVING the
                                 // outer tier is still visited once in the band with no tier stamp
                                 // (= dormant) and takes its dormancy edge (units park their body)
    // ZONES (World::setSimLodZones - the Game publishes the friendly forcefield bubbles): a zone stamps
    // tier 1 within its radius + zoneMargin and tier 2 over a further zoneTier2Band, never tier
    // 0, so a unit walking into a far base's field ticks (and is pushed) without a player near.
    float zoneMargin = 5.0f;
    float zoneTier2Band = 25.0f;
    // The selection job (the sphere queries + tier stamps) runs once this much SIM TIME has
    // passed since its last kick (frame-rate independent; at least one pass apart); in between
    // the last result is reused (dead roots dropped, new roots visited from the pending list).
    // NOT frame-sensitive: what the camera sees is selected every frame from the cull job's
    // frustum pass regardless. The query margin has to cover this much motion.
    float selectionIntervalSec = 0.05f;
    float maxCatchUpSec = 1.0f;  // cap in SECONDS (never frames: frame-rate bound) on the dt a throttled
                                 // tick receives - a tick gets its whole stretch, only a return from
                                 // dormancy is clipped
    bool units = true;           // GameUnitComponent follows the LOD
    bool structures = false;     // GameStructureComponent (barracks/turret clocks, flows)
    bool projectiles = false;    // GameProjectileComponent (lifetime, deflection)
    bool scripts = false;        // ScriptComponent Update
    bool animators = true;       // AnimatorComponent
};

export struct WorldSettings
{
    bool lightDebugGeometry = false; // "Editor": every LightComponent draws its debug wireframes
    SimLodConfig simLod;
    int simLodStats[4] = {};         // live readout: the last pass's per-tier entity counts (written by the World)
};

export namespace Settings
{
    void registerWorld(WorldSettings& s);
}
