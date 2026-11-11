export module Settings.Game;

import Core;
import Core.glm;

// The game's settings (Code/Game, plus the unit/structure component baselines Entity reads). Array sizes match
// Game's enums: EStructureType::Count and ENpcType::Count (static_asserts in Game).
export constexpr int GameStructureTypeCount = 23;
export constexpr int GameUnitTypeCount = 11;

// GameMatch: the co-op director, the material loop, base healing, melee, the nav feed and the labels.
export struct GameMatchSettings
{
    float waveFirstDelay = 40.0f;
    float waveInterval = 120.0f;
    // Waves are sized in BUDGET POINTS, not unit counts: each type has a cost, so a brute-heavy archetype fields far
    // fewer bodies than a swarm flood of the same budget.
    int waveBudget = 60;             // points in wave 1
    float waveBudgetGrowth = 60.0f;   // extra points per subsequent wave
    float waveGrowthGrowth = 30.0f;   // how much that per-wave growth itself climbs every wave
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
    // Co-op in the SANDBOX WORLD (terrain, trees, ocean) with the third-person player; off = the generated rock map
    // with the top-down camera. Read when the match is constructed.
    bool coopSandboxWorld = true;
    bool coopEnemies = true;          // waves + the ambient scatter (off: a peaceful building game)
    float sandboxWaveDistance = 70.0f; // sandbox world: the NEAREST a wave spawns to the Base (m); it spawns on the first enemy land from there out
    float sandboxWaveMaxDistance = 400.0f; // ... and the farthest, where the players' land reaches past it
    // THE LIGHT RULES (the sandbox world): the waves come at NIGHT only, daylight burns the demons, and a field does
    // not push them - they walk in and burn.
    float startTimeOfDay = 7.0f;      // h: the authority sets the sun cycle's time at the match start (< 0 = keep it)
    float nightWaveInterval = 45.0f;  // s between the waves of one night (the first one launches at dusk)
    float waveSizeMult = 1.0f;        // x every wave's budget: the one knob for "how many enemies"
    float nightWaveRamp = 0.25f;     // each later wave of a night is this much bigger (x the night's base size, added per wave)
    float nightWaveVariation = 0.4f;  // random +- on a night wave's size and on the time to the next one
    int   nightSurgeEvery = 4;        // every Nth wave of a night is a surge (0 = none)
    float nightSurgeSize = 2.0f;      // x the size of a surge wave
    float sunlightDps = 20.0f;        // health/s a demon loses in full daylight
    bool fieldsBlockDemons = false;   // on = the fields push every demon, as on the flat maps
    bool demonShadowForm = true;      // a demon outside a player field (and outside daylight) takes no direct damage
    float staffLightRadius = 0.0f;  // m around a player in which a demon is solid (0 = the fields alone)
    // The fields do not push a demon, so the demons are slower: everywhere, and more inside a player field.
    float demonSpeed = 0.7f;          // x every demon's MoveSpeed
    float demonFieldSpeed = 0.5f;     // x that again inside a player field
    // THE BEACON (the sandbox world): the Base's power holds a hole in the clouds (the clouds' vortex, centred on the
    // Base) and sends a beam up; at night the cloud coverage rises to the Shroud's.
    bool beaconEnabled = true;
    float shroudCoverage = 1.0f;     // the cloud coverage at night (the day's is the "Sky/Clouds" value at the match start)
    float shroudDuskSunHeight = 0.0f; // at dusk the clouds are closed at this sun height (the sine of its elevation; they start 0.2 above it)
    float beaconDayStrength = 0.0f; // x the hole's strength in full daylight
    float beaconIgniteDelay = 8.0f;   // s of full night (the clouds closed) before the Beacon turns on
    float beaconGlowSeconds = 1.5f;   // s: a short beam on the Base brightens
    float beaconClimbSeconds = 1.5f;  // s: the beam's tip travels from the Base up to the cloud base (slow near the ground, faster in the sky)
    float beaconPierceSeconds = 5.0f; // s: then the beam tears through the clouds (the first quarter) and the hole grows to its size
    float beaconOffShroud = 0.5f;     // at dawn the light goes off when the shroud (1 = closed, 0 = the day's clouds) is down to this
    float beaconFadeSeconds = 8.0f;   // s: the beam fades out over this at dawn
    float beaconCloseSeconds = 40.0f; // s: at dawn, after the light is off, the hole closes over this (calm)
    float beaconCalmSpin = 10.0f;   // deg/min: the vortex's spin with a full Base store
    float beaconPressureSpin = 60.0f; // deg/min: at night with an empty Base store
    float beaconBeamHeight = 80000.0f; // m above the Base
    float beaconBeamTipFade = 0.5f;    // the share of the beam's height over which its glow fades out toward the top (0 = a hard end)
    float beaconLightIntensity = 1.0f; // the beam light's intensity (the column's glow and the light on the ground), x the power
    float beaconLightRange = 80.0f; // m from the beam it lights surfaces (and its halo in the air ends)
    float beaconBeamRadius = 0.75f;   // m: the column's core
    float beaconCloudLight = 0.33f;   // the beam's light on the clouds around the hole (the vortex's eye light), x the power
    // LIGHTNING (the sandbox world, at night): flashes of light inside the clouds around the player, a share of them
    // with an ARC down to the ground. A look only: no damage, no light on surfaces.
    bool lightningEnabled = true;
    float lightningPerMinute = 10.0f;      // flashes per minute at full night (the gaps are random)
    float lightningStrikeShare = 0.33f;    // the share of the flashes with an arc to the ground
    float lightningMinDistance = 100.0f;   // m from the player: the nearest flash
    float lightningMaxDistance = 4000.0f;  // m: the farthest
    float lightningWallShare = 0.2f;      // while the vortex is open: the share of the flashes placed on its eyewall, high up (none is ever in the hole)
    float lightningDuration = 0.6f;       // s: the time scale of a flash (its 1 to 6 strokes: 0.6 = one lasts ~0.15 s, a long one ~2 s)
    glm::vec3 lightningColor = glm::vec3(0.75f, 0.85f, 1.0f);
    float lightningCloudLight = 8.0f;      // the flash's radiance on the clouds at its centre
    float lightningCloudRadius = 600.0f;   // m: the flash's size in the clouds (1/4 of the light at this distance)
    float lightningArcIntensity = 60.0f;   // the arc's glow
    float lightningArcRadius = 1.0f;       // m: the arc's core
    float lightningArcJitter = 0.1f;       // how far the arc zigzags sideways, as a share of its length
    float lightningForkShare = 0.6f;       // the share of the arcs with a side fork
    // THE FOG EFFECTS (the sandbox world): a wave's spawn site, and the fields' clearings.
    float spawnFogDensity = 0.05f;    // 1/m: the bank's extinction at a spawn site (0 = no spawn fog)
    float spawnFogGlow = 0.6f;        // its own glow (the night gives fog no light)
    float spawnFogSeconds = 40.0f;    // how long a site holds its fog
    float fieldFogClear = 1.0f;       // 0..1: the share of the fog a field removes inside its bubble (0 = off)
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
    // The THIRD-PERSON camera (co-op in the sandbox world): mouse look, the wheel zooms `thirdDistance`.
    float thirdDistance = 4.0f;
    float thirdMinDistance = 1.5f;
    float thirdMaxDistance = 20.0f;
    float thirdSensitivity = 0.0025f; // radians per mouse count
    float thirdMinHeight = 1.0f;      // m: the eye stays this far over the ground under it (0 = off)
    float thirdEyeHeight = 1.4f;  // m above the capsule center: the orbit pivot
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
    float costs[GameStructureTypeCount] = { // indexed by EStructureType
        30.0f,  // Emitter
        40.0f,  // Generator
        25.0f,  // Extractor
        40.0f,  // Battery
        30.0f,  // FuelTank
        40.0f,  // Solar
        60.0f,  // Fabricator
        70.0f,  // Bastion
        45.0f,  // Lance
        150.0f, // Barracks
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
    float fuelRate = 1.0f;
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
    // LIGHT ON BEAMS: light moves between the buildings and their HUBS (the Crystal stores and the Beacon), not as
    // cells on the power cables.
    bool lightBeams = true;           // off = the old cell flow on power cables
    float mirrorBeamRange = 60.0f;    // m: a Mirror reaches a hub this far away
    float lightHubReach = 30.0f;      // m: a hub feeds the consumers this near (and a Hearth reaches a hub this far)
    float lightHubLinkRange = 60.0f;  // m: two hubs this near share their light
    float lightBeamRate = 20.0f;      // light/s a producer sends
    float lightFeedRate = 10.0f;      // light/s a consumer takes (the barracks and the turret keep their own intake)
    float lightShareRate = 10.0f;     // light/s between two hubs
    // The look of a link's beam (the nearest few are BeamLights): its glow, its core, how far it lights surfaces.
    float lightBeamGlow = 1.5f;       // at the full flow
    float lightBeamFullFlow = 1.0f;   // light/s at which a beam has its full glow and radius (less light = less beam):
                                      // a Lantern's draw - a Mirror's 0.21 is a thin beam, a Hearth's 0.45 a half one
    float lightBeamRadius = 0.35f;    // m, at the full flow (half of it at no flow)
    float lightBeamGlowRange = 8.0f;  // m
    // THE CARRIERS: wood and stone move by walkers on the road (the conveyor segments), not as cells on the cables.
    bool carriers = true;             // off = the old cell flow on pipelines and conveyors
    float carrierLoad = 5.0f;         // goods a walker carries
    float carrierSpeed = 3.5f;        // m/s
    int   carriersPerBuilding = 6;    // walkers one sending building can have at most
    float cableHealthMax = 40.0f;     // segments/crossings are softer than buildings
    float internalBuffer = 10.0f;
    float emitterBuffer = 50.0f;      // the shield emitters' energy stores (their pressure draw spikes)
    float bastionBuffer = 150.0f;
    float lanceBuffer = 100.0f;
    float generatorBuffer = 10.0f;
    // THE CRYSTAL STORE: one full store runs TWO field generators through one night. A night is ~640 s (the sun cycle
    // at 60x, the default latitude and day), a generator draws 1 light/s awake: 2 x 640 = 1280, plus room for the extra
    // draw under pressure. Filled by day: a Mirror makes ~780 light in one day, so two Mirrors fill one store.
    float batteryCapacity = 1500.0f;
    float generatorFuelTank = 10.0f;
    float fuelTankCapacity = 200.0f;
    float mineralSiloCapacity = 200.0f;
    float mineralBaseCapacity = 200.0f;
    float medicEnergyPerSec = 1.5f;
    float barracksEnergyIntake = 2.0f; // energy/s a barracks' transport port takes at most: the BUILD RATE
    // THE LIGHT BALANCE (the default sun cycle: a day gives a Mirror ~780 s of full daylight, a night keeps a field
    // awake ~640 s; all with enough Crystal stores between them). A Lantern draws 1 light/s awake, a Great lantern 3.
    //   4 Mirrors carry 1 Lantern through the night:  4 x 0.21 x 780 = 655 >= 640.  12 Mirrors: 1 Great lantern.
    //   1 Hearth, burning day and night (1440 s), carries 1 Lantern:  0.45 x 1440 = 648 >= 640.  3 Hearths: 1 Great.
    float genEnergyPerSec = 0.45f;    // the Hearth: an early, inefficient source (1 wood/s for this much light)
    float solarEnergyPerSec = 0.21f;
    float solarDaylight = 1.0f;       // runtime (GameMatch): x the solar yield - the daylight in the sandbox world, else 1
    float fuelBurnRate = 0.5f;
    float fabricatorMineralsPerSec = 0.5f;
    float fabricatorFuelPerSec = 1.0f;
    float fabricatorEnergyPerSec = 1.0f;
    float extractorEnergyPerSec = 1.0f;
    float pressureDrawTension = 1.5f;
    float emitterEnergyPerSec = 1.0f;
    float emitterOutput = 1.2f;
    float emitterReach = 54.0f;
    // The Base's shield: it pays for itself from the Base's own energy store.
    float baseEnergyCapacity = 100.0f;
    float baseShieldEnergyPerSec = 1.5f; // the field's standing draw AND the Base's own generation: its idle field is free
    float baseShieldOutput = 2.4f;
    float baseShieldReach = 54.0f;
    float bastionEnergyPerSec = 3.0f; // 3 Lanterns' worth (see THE LIGHT BALANCE)
    float bastionOutput = 2.6f;
    float bastionReach = 90.0f;
    float lanceEnergyPerSec = 3.0f;
    float lanceOutput = 0.08f;
    float lanceReach = 52.0f;
    float emitterPressureDraw = 3.0f;
    float emitterShrinkTime = 1.5f;
    float emitterGrowTime = 0.5f;
    float emitterRestartCharge = 6.0f;
    float emitterWarnCharge = 0.35f;  // store fraction below which a field's shell flickers (0 = never)
    // FIELDS SLEEP BY DAY (the sandbox world): with no darkness and no demon on it, a field draws no light and its
    // shell is faint - it still shows its area. fieldNight is runtime (GameMatch): 0 = full day .. 1 = night; 1 in
    // every other mode, where a field never sleeps.
    float fieldNight = 1.0f;
    float fieldDayAlpha = 0.15f;      // x the shell alpha of a sleeping field (the shell's glow and its light follow it)
    float structureHealthMax = 100.0f;
    float constructorRange = 27.0f;
    float constructorBuildRate = 4.0f;
    float waypointRadius = 3.0f;
    int wallBreachCost = 10;          // a wall cell costs (1 + this) x 2 m of walking in the enemy fields
    bool cheatInstantBuild = false;
    // The sandbox world's ground test: the highest minus the lowest terrain sample under a footprint may not pass this.
    // 0 = no slope limit.
    float maxGroundStep = 0.0f;
    // Per UNIT TYPE (ENpcType order): the ENERGY a barracks pays per spawned unit and the POPULATION the unit holds.
    float spawnEnergy[GameUnitTypeCount] = { 5.0f, 20.0f, 6.0f, 9.0f, 2.0f, 15.0f, 40.0f, 80.0f, 15.0f, 30.0f, 12.0f };
    int unitPopulation[GameUnitTypeCount] = { 2, 5, 2, 4, 1, 4, 8, 16, 4, 8, 3 };
    // WORKERS: production runs on population, not on light. A team has baseWorkers + houseWorkers for each House; every
    // production building asks for its workers; short of workers, all production of the team slows by workers / jobs.
    int baseWorkers = 6;
    int houseWorkers = 4;
    int workersExtractor = 2;
    int workersFabricator = 3;
    int workersConstructor = 2;
    int workersGenerator = 1;
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
    float fieldDps = 2.0f;        // health/s while squished below damageRadius under pressure
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
    bool flatGround = true;        // runtime (GameMatch): false = the sandbox terrain world - no height ceiling, no
                                   // void floor and no bite height window, which all assume the y = 0 plane
    // Runtime (GameMatch, the co-op sandbox world's light rules; the other modes keep the defaults):
    bool fieldPush = true;         // false = an enemy field does not push a unit (it walks in and burns); a prefab's
                                   // `FieldBlocked true` keeps the push for that type
    int shadowTeam = -1;           // the team that is SHADOW outside an opposing field: no direct damage, no turret
                                   // lock (-1 = none); daylight makes it solid
    // THE PLAYERS' STAFF LIGHT (runtime, GameMatch): a shadow-team unit this near a player is solid too, so a player
    // can always fight what attacks next to it. Per point: x, y, z of the player, then the radius.
    // THE FAR TICK's ground (runtime, set by the game in the sandbox world; null = a far unit keeps its height): the
    // terrain height at a point, or the fallback where there is none. A far unit walks farLift over it, so it is over
    // the true ground when it comes near and its body goes live.
    float (*farGround)(float x, float z, float fallback) = nullptr;
    float farLift = 2.5f;
    float lightPoints[8][4]{};
    int lightPointCount = 0;
    // THE FIELD ZONES (runtime, GameMatch, the terrain world): the bubbles of fieldZoneTeam's field buildings as
    // spheres (x, y, z, radius). A unit of another team inside one, measured FLAT on the map, is in the field
    // whatever the force bake's height slice reads (GameUnitComponent::tickField).
    float fieldZones[64][4]{};
    int fieldZoneCount = 0;
    int fieldZoneTeam = -1;
    int slowTeam = -1;             // the team whose units walk slower (runtime; -1 = none): x slowMult, and
    float slowMult = 1.0f;         // x fieldSlowMult more while a unit stands in an opposing field
    float fieldSlowMult = 1.0f;
    int sunBurnTeam = -1;          // the team daylight burns (-1 = none)
    float sunDps = 0.0f;           // health/s that team loses now: "Sunlight damage/s" x the daylight
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

// THE FACTION LAYER (Game:Faction, the sandbox world): whose land each cell is, and how that spreads.
export struct GameFactionSettings
{
    bool enabled = true;
    float cellSize = 4.0f;         // m; x 4096 cells = the grid's extent (16 km). Read when the grid is made
    // What all land starts as: 0 = the enemy's, 128 = neutral, 255 = the players'. Read when the grid is made. 127 = one step UNDER neutral: no
    // tint by day, and the NIGHT takes all land under neutral to the full enemy look (Renderer::setFactionNight).
    int startValue = 127;
    float sweepSeconds = 2.0f;     // one pass of the spread over the ACTIVE area (around the sources) takes this long
    float sourceSeconds = 45.0f;   // land at a source's centre goes to its side over about this long
    float diffusion = 0.02f;       // per second: how fast a cell follows its neighbours (the value bleeds past a source's radius)
    float baseRadius = 150.0f;     // m: the Base's player source
    float fieldRadiusScale = 2.0f; // x a field bubble's radius: its player source
    float enemySiteRadius = 120.0f; // m: the enemy source at a wave's spawn site, while its fog stands
    // THE ENEMY OUTPOSTS (GameMatch::tickOutposts): a ring of dark beacons around the Base, each an enemy source.
    bool outpostsEnabled = true;
    int outpostCount = 8;            // on a regular polygon (8 = the octagon). Read when the ring is made
    float outpostDistance = 600.0f;  // m from the Base. Read when the ring is made
    float outpostRadius = 250.0f;    // m: an outpost's enemy source
    float outpostBeamHeight = 300.0f;    // m
    float outpostBeamRadius = 0.4f;      // m: the beam's core
    float outpostBeamTipFade = 0.6f;     // the share of the beam's height over which it fades out toward the top (0 = a hard end)
    float outpostBeamIntensity = 2.0f;   // the beam's glow in the air
    float outpostBeamRange = 60.0f;      // m: where the beam's halo ends
};

export struct GameSettings
{
    GameFactionSettings faction;
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
