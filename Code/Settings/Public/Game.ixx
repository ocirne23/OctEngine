export module Settings.Game;

import Core;

// The game's settings (Code/Game, plus the unit/structure component baselines Entity reads). Array sizes match
// Game's enums: EStructureType::Count and ENpcType::Count (static_asserts in Game).
export constexpr int GameStructureTypeCount = 27;
export constexpr int GameUnitTypeCount = 11;

// GameMatch: the co-op director, the material loop, base healing, melee, the nav feed and the labels.
export struct GameMatchSettings
{
    float waveFirstDelay = 40.0f;
    float waveInterval = 120.0f;
    // Waves are sized in BUDGET POINTS, not unit counts: each type has a cost, so a brute-heavy archetype fields far
    // fewer bodies than a swarm flood of the same budget.
    int waveBudget = 20;              // points in wave 1
    float waveBudgetGrowth = 40.0f;   // extra points per subsequent wave
    float waveGrowthGrowth = 5.0f;    // how much that per-wave growth itself climbs every wave
    float waveCost[GameUnitTypeCount] = { 3.0f, 25.0f, 2.0f, 10.0f, 1.0f,   // Grunt, Brute, Runner, Spitter, Swarm
                                          15.0f, 100.0f, 500.0f, 30.0f, 40.0f, // Elite, Giant, Titan, Lobber, Spawner
                                          5.0f };                              // Warrior
    int waveMaxAlive = 100000;        // total AI units cap (ambient + waves)
    float waveSpawnAreaPerUnit = 6.0f; // m² of blob per body (~2.8 m mean spacing at 6)
    int ambientBudget = 500000;       // POINTS of world-start scatter (same per-type costs as waves)
    float ambientSafeRadius = 45.0f;  // the scatter keeps clear of the Base (planar)
    int ambientRecipeWindow = 3;      // a group rolls recipes gated within this many bands below its depth band
    float ambientDepthScale = 0.9f;   // the depth fraction that already counts as the deepest band
    float ambientWanderInterval = 90.0f; // mean seconds between an idle AI unit's strolls (0 = off)
    float ambientWanderDistance = 12.0f; // stroll length (0.4-1x of it)
    float ambientWanderBaseBias = 0.5f;  // heading = random unit vector + bias * toward the Base
    float ambientWanderTimeout = 12.0f;  // a stroll that does not arrive gives up after this
    float labelMaxDistance = 120.0f;  // world labels beyond this are not built
    // Unit nav sources are culled to units with ANOTHER team's unit/player within this reach.
    float navUnitSourceReach = 64.0f;
    // A non-AI unit farther than this from every SIM LOD focus seeds a new cluster focus.
    float focusClusterRadius = 40.0f;
    int spawnsPerFrame = 100;         // trickle budget - a huge wave enters over seconds, not one hitch
    // Map generation inputs, read once at generation on the AUTHORITY (the lobby writes them too).
    int mapSeed = 0;                  // 0 = random each run
    float terrainFill = 0.3f;         // fraction of interior cells turned to rock (before carving)
    int terrainLanes = 6;             // carved attack lanes from the base ring to the map edge
    // The player-inventory material loop.
    float refillRadius = 6.0f;        // metres from a Silo/Base within which the inventory refills
    float refillRate = 15.0f;         // materials/s pulled from the stores
    float buildRadius = 6.0f;         // metres from a blueprint within which a player invests
    float playerBuildRate = 8.0f;     // materials/s a player invests into a blueprint
    // Health regen near an OWN-team Base (owner-computed against the local structure mirror).
    float baseHealRadius = 10.0f;
    float baseHealRate = 15.0f;       // health/s inside the radius
    float meleeDps = 10.0f;           // player melee aura: health/s to enemy units in melee range
    float meleeRadius = 2.5f;         // melee range (m, XZ from the capsule)
    float selectionClusterRadius = 12.0f; // link radius of the selection's cluster centroid
};

// GameCamera: the angled top-down follow camera. `distance` is also the live zoom (the camera writes it).
export struct GameCameraSettings
{
    float pitchDeg = 75.0f;
    float distance = 100.0f;
    float minDistance = 6.0f;
    float maxDistance = 250.0f;
    float zoomSpeed = 2.0f;
    float yawSpeed = 120.0f;
    float dragSensitivity = 0.25f;
    float aimHeight = 1.0f;
};

// GamePlayer: movement, health and the shield battery.
export struct GamePlayerSettings
{
    bool detachCamera = false;          // local-only (not Synced): free-fly view
    bool detachFocus = false;           // local-only: the scene focus follows the fly camera
    float moveSpeed = 4.0f;
    float accel = 30.0f;                // deliberately soft: steering force must lose against bubble push
    float jumpSpeed = 6.0f;
    float sprintMult = 2.0f;
    float sprintEnergyPerSec = 15.0f;   // sprint burns the shield battery; emptying it COLLAPSES the shield
    float healthMax = 100.0f;
    float healthDrainRate = 15.0f;      // health/s while unshielded in enemy territory
    float shieldMaxOutput = 1.5f;       // field output while the battery holds ANY charge
    float energyMax = 100.0f;
    float energyRegenRate = 10.0f;      // energy/s refill (battery only - never grows the bubble)
    float energyDrainRate = 50.0f;      // energy/s drained per unit of pressure
    float rebootEnergy = 20.0f;         // collapsed shield restarts once the battery refills to this
    float damageAbsorb = 2.0f;          // shield ENERGY spent per hp of direct damage absorbed (0 = none)
    float coverDrainReduction = 0.75f;  // drain reduction per unit of FRIENDLY field surplus over the own output
    float spawnGraceSec = 1.0f;         // no energy/health drain this long after (re)spawn
    float materialsMax = 50.0f;         // inventory size
    float arriveRadius = 0.8f;          // metres: a move order completes inside this ring
    float damageRadius = 1.0f;          // metres: health drains once the equilibrium shield radius squishes below this
    float shieldPushGain = 10000.0f;    // applied-force -> impulse scale
    float shieldTension = 1.5f;         // SURFACE TENSION: push AND energy drain scale by (1 + tension * pressure)
};

// StructureSystem: costs, the economy, the cable transport, the shield structures, barracks and houses.
export struct GameStructureSettings
{
    float costs[GameStructureTypeCount] = { // indexed by EStructureType (Base/Connector free)
        30.0f,  // Emitter
        40.0f,  // Generator
        0.0f,   // Connector (retired)
        25.0f,  // Extractor
        40.0f,  // Battery
        30.0f,  // FuelTank
        40.0f,  // Solar
        60.0f,  // Fabricator
        70.0f,  // Bastion
        45.0f,  // Lance
        150.0f, // Barracks
        0.0f,   // BarracksBrute (retired)
        0.0f,   // BarracksRunner (retired)
        0.0f,   // BarracksSpitter (retired)
        5.0f,   // Wall (per segment)
        75.0f,  // Turret
        25.0f,  // MineralSilo
        50.0f,  // Constructor
        100.0f, // Base (spawned, never placed - this entry only prices its REPAIRS)
        2.0f,   // CablePower (per segment)
        2.0f,   // CablePipe
        2.0f,   // CableConveyor
        6.0f,   // CrossingPower
        6.0f,   // CrossingPipe
        6.0f,   // CrossingConveyor
        40.0f,  // House
        50.0f,  // MedicStation
    };
    float startMinerals = 200.0f;
    float extractorSnapRadius = 6.0f;
    float mineralRate = 2.0f;
    float fuelRate = 4.0f;
    float baseIncomeMult = 0.25f;
    float placeRange = 30.0f;
    float cableThroughput[3] = { 20.0f, 4.0f, 4.0f }; // by MEDIUM (energy, fuel, minerals): a segment's OUT-RATE in cells/s
    // The transport tick: fixed rate, `substeps` stencil passes per tick, runs staggered over `spread` groups.
    float transportTickHz = 10.0f;
    int transportSubsteps = 4;
    int transportSpread = 4;
    int cellsPerSegment[3] = { 2, 1, 1 }; // soft capacity of one segment by MEDIUM
    float storageLowMark = 0.25f;     // storage pushes while its port node is at/below this fill
    float storageHighMark = 0.75f;    // ... and pulls while at/above this (hysteresis between)
    int statTransportNodes = 0;       // read-only stats, written by StructureSystem
    int statTransportTicks = 0;
    float cableHealthMax = 40.0f;     // segments/crossings are softer than buildings
    float internalBuffer = 10.0f;
    float emitterBuffer = 50.0f;      // the shield emitters' energy stores (their pressure draw spikes)
    float bastionBuffer = 150.0f;
    float lanceBuffer = 100.0f;
    float generatorBuffer = 10.0f;
    float batteryCapacity = 200.0f;
    float generatorFuelTank = 10.0f;
    float fuelTankCapacity = 200.0f;
    float mineralSiloCapacity = 200.0f;
    float mineralBaseCapacity = 200.0f;
    float medicEnergyPerSec = 1.5f;
    float barracksEnergyIntake = 2.0f; // energy/s a barracks' transport port takes at most: the BUILD RATE
    float genEnergyPerSec = 5.0f;
    float solarEnergyPerSec = 1.0f;
    float fuelBurnRate = 1.0f;
    float fabricatorMineralsPerSec = 0.5f;
    float fabricatorFuelPerSec = 1.0f;
    float fabricatorEnergyPerSec = 1.0f;
    float extractorEnergyPerSec = 1.0f;
    float pressureDrawTension = 1.5f;
    float emitterEnergyPerSec = 1.0f;
    float emitterOutput = 1.2f;
    float emitterReach = 27.0f;
    // The Base's shield: it pays for itself from the Base's own energy store.
    float baseEnergyCapacity = 100.0f;
    float baseEnergyGenPerSec = 2.0f; // free self-generation into its own store
    float baseShieldEnergyPerSec = 1.5f;
    float baseShieldOutput = 2.4f;
    float baseShieldReach = 27.0f;
    float bastionEnergyPerSec = 2.0f;
    float bastionOutput = 2.6f;
    float bastionReach = 45.0f;
    float lanceEnergyPerSec = 3.0f;
    float lanceOutput = 0.08f;
    float lanceReach = 26.0f;
    float emitterPressureDraw = 3.0f;
    float emitterShrinkTime = 1.5f;
    float emitterGrowTime = 0.5f;
    float emitterRestartCharge = 6.0f;
    float structureHealthMax = 100.0f;
    float constructorRange = 27.0f;
    float constructorBuildRate = 4.0f;
    float waypointRadius = 3.0f;
    int wallBreachCost = 10;          // a wall cell costs (1 + this) x 2 m of walking in the enemy fields
    bool cheatInstantBuild = false;
    // Per UNIT TYPE (ENpcType order): the ENERGY a barracks pays per spawned unit and the POPULATION the unit holds.
    float spawnEnergy[GameUnitTypeCount] = { 5.0f, 20.0f, 6.0f, 9.0f, 2.0f, 15.0f, 40.0f, 80.0f, 15.0f, 30.0f, 12.0f };
    int unitPopulation[GameUnitTypeCount] = { 2, 5, 2, 4, 1, 4, 8, 16, 4, 8, 3 };
    int barracksPopulation = 20;      // a barracks' own population cap
    int housePopulation = 10;         // added per linked house
    float houseLinkRadius = 25.0f;
};

// NpcSystem: the far tick, the seeded lanes, shot speeds and the strike visuals.
export struct GameNpcSettings
{
    float farInterval = 0.5f;         // SIM LOD far tick interval (s)
    int farTicked = 0;                // live readout: units moved by the last far tick (written by NpcSystem)
    // Seeded-lane strength: a player ORDER writes a strong, wide lane; a STUCK unit's request is a hint.
    float orderLaneSpeed = 10.0f;
    float stuckLaneSpeed = 10.0f;
    float laneWidth = 3.0f;           // metres PAINTED (0 = one cell)
    float spitterShotSpeed = 18.0f;
    float lobberShotSpeed = 14.0f;    // ShotKind 1: the slow splash shell
    float beamLifetime = 0.5f;        // turret lightning
    float hitLifetime = 0.25f;        // melee hit line + its flash
    float flashIntensity = 1.0f;      // multiplier on the turret muzzle light's peak
};

// The shared unit-sim baseline (GameUnitComponent::params refers to Globals::settings.game.unitParams). localTeam,
// huntSeedTeam, waypointRadius and voidY are no tweaks: the game sets the first two per match.
export struct GameUnitParams
{
    float energyDrainRate = 25.0f; // energy/s per unit of pressure (player shield rule, no regen)
    float tension = 1.5f;          // drain + push scale by (1 + tension*pressure)
    float fieldDps = 10.0f;        // health/s while squished below damageRadius under pressure
    float fieldDpsMult = 1.0f;
    float fieldPushStart = 0.7f;   // push ramp start as a fraction of iso: below it no shove, so
                                   // units reach the damage band instead of parking in the fringe
    float emitterDrainMult = 0.25f;
    float strainRange = 20.0f;     // planar reach (m) of the siege drain: the Bastion's bubble radius
    float damageAbsorb = 2.0f;     // shield energy per hp of direct damage absorbed before health
    float damageRadius = 0.6f;     // equilibrium radius below this + pressure = exposure damage
    float pushGain = 20000.0f;     // shared by the emitter readback path AND the shield-less path
    float retargetInterval = 5.0f;
    float wanderSpeedMult = 0.25f;
    float wanderSpeedMax = 0.75f;  // m/s
    int localTeam = -1;            // the VIEWER's team: its units tint green on every instance
    int huntSeedTeam = -1;         // the ONE team that also seeds lanes toward HUNTED targets (the
                                   // co-op AI); every other team seeds only routes and move orders
    float targetSearchRadius = 15.0f; // LOCAL harassment only: the barracks route does the delivery
    float maxSpeed = 12.0f;        // absolute m/s cap on every unit body, every tick, any cause
                                   // (field shoves included): ~1.5x the runner's 7.6
    float waypointRadius = 3.0f;
    float farSpreadDeg = 40.0f;    // far walk: persistent per-unit heading bias so a wave fans out
    float routeEngageRadius = 5.0f;  // marching a route: an enemy this near is engaged, then resumed
    float orderBreakRadius = 15.0f;  // a MOVE ORDER drops at the first enemy structure this near
    float voidY = -3.0f;           // checked by the full sim AND the far tick
    float heightLimit = 5.0f;      // world Y ceiling for units AND player capsules (GamePlayer too)
    bool navEnabled = true;
    float hurtLightIntensity = 8.0f;
    float hurtLightDecay = 0.25f;  // seconds from full to dark
    float lightArea = 8.0f;        // m: hurt-flash budget bucket size (lock-free hashed slots)
    float hurtFlashRate = 4.0f;    // flashes per second per area
    // steerHeading score: free * (Goal*dot(goal) + Flow*laneW*dot(lane) + Persist*dot(last))
    //   - Pressure*gpW*dot(gradP) + Wall*dot(wallAway) - clipped*CornerClip
    float steerGoal = 0.3f;
    float steerFlow = 1.0f;        // a lane is a proven route: outweighs walking straight at the goal
    float flowSplatGain = 0.5f;    // scale on the MEASURED velocity splatted into the lane (0 = no trail)
    float steerPersist = 0.4f;
    float targetTrackRadius = 5.0f; // geodesic m: closer = field-tracking beats the (laggy) seeded lane
    float steerTrackGoal = 1.5f;   // goal weight floor while tracking
    float trackFlowMult = 0.15f;   // lane weight multiplier while tracking
    float navFollowRadius = 40.0f; // geodesic m: no target, but walk the crowd lane if one is here
    float steerPressure = 0.5f;
    float pressureKnee = 0.23f;    // pressure gradient scoring 0.5 (x/(x+knee))
    float flowKnee = 0.15f;        // lane speed scoring 0.5, as a fraction of moveSpeed
    float orderFlowBlind = 0.0f;   // s a freshly ordered unit ignores the lane (so it can turn around)
    float seedRequestInterval = 1.0f;
    float unstickAfter = 1.0f;     // s of stall after which goal/persistence are dropped
    float presencePressure = 0.01f; // pressure every unit injects per tick (x60/s)
    float steerLook = 6.0f;        // metres of whisker (min; scales with speed)
    float steerCornerClip = 0.7f;
    float steerWall = 1.0f;
    float wallKeep = 0.9f;         // metres beyond the body radius the wall push reaches
    float stuckPressure = 0.5f;    // pressure a stalled unit injects per second (x stall, <= 1.5)
};

// The structures' machine baseline (GameStructureComponent::params refers to Globals::settings.game.structureParams).
export struct GameStructureParams
{
    float fieldDamageRate = 6.0f; // health/s while an enemy team's bubble owns the query point
    float turretRange = 18.0f;
    float turretFireInterval = 1.2f;
    float turretShotEnergy = 2.0f; // keep a WHOLE number: the cable transport delivers whole cells
                                   // into a store of this capacity, so 1.5 stalled one cell short
    float turretDamage = 25.0f;
    float medicRange = 12.0f;
    float medicHealRate = 4.0f;    // health/s AND battery energy/s per body (stations stack)
};

export struct GameSettings
{
    GameMatchSettings match;
    GameCameraSettings camera;
    GamePlayerSettings player;
    GameStructureSettings structures;
    GameNpcSettings npc;
    GameUnitParams unitParams;
    GameStructureParams structureParams;
};

// "Game/*" (plus "HUD/Label max distance"): registered in the order the old per-match registration ran - match,
// camera, player, structures (+ structureParams), npc (+ unitParams).
export namespace Settings
{
    void registerGame(GameSettings& s);
}
