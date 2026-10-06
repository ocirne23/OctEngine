module Settings.Game;

import Core;
import Settings.Tweaks;

// Gameplay tweaks: the server's values overrule the clients' (the Synced blocks).

static void registerMatch(GameMatchSettings& s)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::floatVar("Game/Coop", "First wave delay (s)", &s.waveFirstDelay, 5.0f, 600.0f, 5.0f);
    Tweak::floatVar("Game/Coop", "Wave interval (s)", &s.waveInterval, 10.0f, 600.0f, 5.0f);
    Tweak::intVar("Game/Coop", "Wave budget", &s.waveBudget, 1, 5000, 10);
    Tweak::floatVar("Game/Coop", "Wave budget growth", &s.waveBudgetGrowth, 0.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Coop", "Wave growth growth", &s.waveGrowthGrowth, 0.0f, 100.0f, 0.5f);
    // waveCost is indexed by ENpcType.
    Tweak::floatVar("Game/Coop", "Cost grunt", &s.waveCost[0], 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost brute", &s.waveCost[1], 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost runner", &s.waveCost[2], 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost spitter", &s.waveCost[3], 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost swarm", &s.waveCost[4], 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost elite", &s.waveCost[5], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost giant", &s.waveCost[6], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost titan", &s.waveCost[7], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost lobber", &s.waveCost[8], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost spawner", &s.waveCost[9], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost warrior", &s.waveCost[10], 0.1f, 500.0f, 0.5f);
    Tweak::intVar("Game/Coop", "Max enemy units", &s.waveMaxAlive, 1, 100000, 50);
    Tweak::floatVar("Game/Coop", "Wave spawn area per unit", &s.waveSpawnAreaPerUnit, 1.0f, 60.0f, 0.5f);
    Tweak::intVar("Game/Coop", "Ambient budget", &s.ambientBudget, 0, 1000000, 10);
    Tweak::floatVar("Game/Coop", "Ambient safe radius", &s.ambientSafeRadius, 10.0f, 200.0f, 1.0f);
    Tweak::intVar("Game/Coop", "Ambient recipe window", &s.ambientRecipeWindow, 0, 20, 1);
    Tweak::floatVar("Game/Coop", "Ambient depth scale", &s.ambientDepthScale, 0.5f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Coop", "Ambient wander interval (s)", &s.ambientWanderInterval, 0.0f, 600.0f, 5.0f);
    Tweak::floatVar("Game/Coop", "Ambient wander distance", &s.ambientWanderDistance, 2.0f, 60.0f, 1.0f);
    Tweak::floatVar("Game/Coop", "Ambient wander base bias", &s.ambientWanderBaseBias, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Coop", "Ambient wander timeout (s)", &s.ambientWanderTimeout, 1.0f, 60.0f, 1.0f);
    Tweak::floatVar("HUD", "Label max distance", &s.labelMaxDistance, 10.0f, 2000.0f, 10.0f, {}, ETweakFlags::None);
    Tweak::floatVar("Game/Nav", "Nav unit source reach", &s.navUnitSourceReach, 8.0f, 400.0f, 4.0f);
    Tweak::floatVar("Game/Sim LOD", "Unit cluster focus radius", &s.focusClusterRadius, 5.0f, 200.0f, 1.0f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Coop", "Spawns per frame", &s.spawnsPerFrame, 1, 200, 1);
    // Map generation inputs, read once at generation on the AUTHORITY. Clients never read them: the values actually
    // used ride the GMp event (and the save) with the seed - a joiner's tweak sync lands after the world replay.
    Tweak::intVar("Game/Coop", "Map seed", &s.mapSeed, 0, 0x7fffffff, 1);
    Tweak::floatVar("Game/Coop", "Terrain fill", &s.terrainFill, 0.0f, 0.6f, 0.02f);
    Tweak::intVar("Game/Coop", "Terrain lanes", &s.terrainLanes, 2, 12, 1);
    Tweak::floatVar("Game/Construction", "Refill radius", &s.refillRadius, 1.0f, 30.0f, 0.25f);
    Tweak::floatVar("Game/Construction", "Refill rate", &s.refillRate, 0.5f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Construction", "Player build radius", &s.buildRadius, 1.0f, 30.0f, 0.25f);
    Tweak::floatVar("Game/Construction", "Player build rate", &s.playerBuildRate, 0.5f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Base heal radius", &s.baseHealRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Base heal/s", &s.baseHealRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Melee damage/s", &s.meleeDps, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Melee radius", &s.meleeRadius, 0.0f, 12.0f, 0.25f);
    Tweak::floatVar("Game/Nav", "Group cluster radius", &s.selectionClusterRadius, 2.0f, 60.0f, 0.5f);
}

static void registerCamera(GameCameraSettings& s)
{
    // Camera feel is a PERSONAL preference: never synced from the server.
    Tweak::floatVar("Game/Camera", "Pitch", &s.pitchDeg, 30.0f, 90.0f, 0.5f);
    Tweak::floatVar("Game/Camera", "Distance", &s.distance, 4.0f, 250.0f, 0.5f);
    Tweak::floatVar("Game/Camera", "Min distance", &s.minDistance, 2.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Camera", "Max distance", &s.maxDistance, 10.0f, 250.0f, 0.5f);
    Tweak::floatVar("Game/Camera", "Zoom speed", &s.zoomSpeed, 0.5f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Camera", "Yaw speed (deg/s)", &s.yawSpeed, 30.0f, 360.0f, 1.0f);
    Tweak::floatVar("Game/Camera", "Drag sensitivity", &s.dragSensitivity, 0.05f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Camera", "Aim height", &s.aimHeight, 0.0f, 4.0f, 0.05f);
}

static void registerPlayer(GamePlayerSettings& s)
{
    // View preferences like Game/Camera: OUTSIDE the Synced scope, so the server never pushes its own value onto a
    // client that is flying around.
    Tweak::boolean("Game/Player", "Detach camera (free fly)", &s.detachCamera);
    Tweak::boolean("Game/Player", "Detach focus point", &s.detachFocus); // only matters with the camera detached
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::floatVar("Game/Player", "Move speed", &s.moveSpeed, 0.5f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Accel", &s.accel, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Jump speed", &s.jumpSpeed, 0.5f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Sprint mult", &s.sprintMult, 1.0f, 5.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Sprint energy/s", &s.sprintEnergyPerSec, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Health max", &s.healthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Player", "Health drain/s", &s.healthDrainRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Max output", &s.shieldMaxOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Player", "Energy max", &s.energyMax, 1.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Player", "Energy regen/s", &s.energyRegenRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Energy drain/s @ pressure 1", &s.energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Reboot energy", &s.rebootEnergy, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Player", "Damage absorb (energy per hp)", &s.damageAbsorb, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Cover drain reduction", &s.coverDrainReduction, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Player", "Spawn grace (s)", &s.spawnGraceSec, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Player", "Materials max", &s.materialsMax, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Player", "Move arrive radius", &s.arriveRadius, 0.1f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Player", "Damage radius", &s.damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Player", "Push gain", &s.shieldPushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Player", "Surface tension", &s.shieldTension, 0.0f, 10.0f, 0.05f);
}

// costs is indexed by EStructureType, spawnEnergy / unitPopulation by ENpcType.
static void registerStructures(GameStructureSettings& s, GameStructureParams& sp)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::boolean("Game/Construction", "Free instant build", &s.cheatInstantBuild);
    Tweak::floatVar("Game/Structures", "Pressure draw tension", &s.pressureDrawTension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Start minerals", &s.startMinerals, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor snap radius", &s.extractorSnapRadius, 1.0f, 20.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Minerals/s per node", &s.mineralRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fuel/s per node", &s.fuelRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base income mult", &s.baseIncomeMult, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Emitter cost", &s.costs[0], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator cost", &s.costs[1], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Extractor cost", &s.costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Battery cost", &s.costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &s.costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Solar cost", &s.costs[6], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fabricator cost", &s.costs[7], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion cost", &s.costs[8], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lance cost", &s.costs[9], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Barracks cost", &s.costs[10], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "House cost", &s.costs[25], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Medic station cost", &s.costs[26], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo cost", &s.costs[16], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Constructor cost", &s.costs[17], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Power cable cost", &s.costs[19], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Pipeline cost", &s.costs[20], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Conveyor cost", &s.costs[21], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Power crossing cost", &s.costs[22], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Pipe crossing cost", &s.costs[23], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Conveyor crossing cost", &s.costs[24], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Cable health max", &s.cableHealthMax, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Constructor range", &s.constructorRange, 2.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Constructor build rate", &s.constructorBuildRate, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Structures", "Waypoint radius", &s.waypointRadius, 0.5f, 15.0f, 0.25f);
    Tweak::intVar("Game/Structures", "Wall breach cost", &s.wallBreachCost, 1, 254, 1);
    Tweak::floatVar("Game/Economy", "Mineral base capacity", &s.mineralBaseCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo capacity", &s.mineralSiloCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Barracks energy intake/s", &s.barracksEnergyIntake, 0.1f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Wall cost (per segment)", &s.costs[14], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Turret cost", &s.costs[15], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion energy/s", &s.bastionEnergyPerSec, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Lance energy/s", &s.lanceEnergyPerSec, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Solar energy/s", &s.solarEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator energy/s", &s.fabricatorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator fuel/s", &s.fabricatorFuelPerSec, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Fabricator minerals/s", &s.fabricatorMineralsPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fuel tank capacity", &s.fuelTankCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Fuel burn/s per generator", &s.fuelBurnRate, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Energy gen/s per generator", &s.genEnergyPerSec, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Emitter energy/s", &s.emitterEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Emitter energy/s @ pressure 1", &s.emitterPressureDraw, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base energy capacity", &s.baseEnergyCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Base energy gen/s", &s.baseEnergyGenPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base shield energy/s", &s.baseShieldEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield output", &s.baseShieldOutput, 0.1f, 8.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield reach", &s.baseShieldReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Extractor energy/s", &s.extractorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Battery capacity", &s.batteryCapacity, 10.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Internal buffer", &s.internalBuffer, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Emitter buffer", &s.emitterBuffer, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion buffer", &s.bastionBuffer, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lance buffer", &s.lanceBuffer, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator buffer", &s.generatorBuffer, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Cable throughput", &s.cableThroughput[0], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Pipeline throughput", &s.cableThroughput[1], 0.5f, 100.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Conveyor throughput", &s.cableThroughput[2], 0.5f, 100.0f, 0.25f);
    // The transport tick (rates/capacities re-stamp at the next network rebuild - StructureSystem listens).
    Tweak::floatVar("Game/Economy", "Transport tick rate (Hz)", &s.transportTickHz, 1.0f, 60.0f, 1.0f);
    Tweak::intVar("Game/Economy", "Transport substeps", &s.transportSubsteps, 1, 16, 1.0f);
    Tweak::intVar("Game/Economy", "Transport spread (groups)", &s.transportSpread, 1, 8, 1.0f);
    Tweak::intVar("Game/Economy", "Cable cells per segment", &s.cellsPerSegment[0], 1, 64, 1.0f);
    Tweak::intVar("Game/Economy", "Pipeline cells per segment", &s.cellsPerSegment[1], 1, 64, 1.0f);
    Tweak::intVar("Game/Economy", "Conveyor cells per segment", &s.cellsPerSegment[2], 1, 64, 1.0f);
    Tweak::floatVar("Game/Economy", "Storage pushes below (fill)", &s.storageLowMark, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Storage pulls above (fill)", &s.storageHighMark, 0.0f, 1.0f, 0.05f);
    Tweak::intVar("Game/Economy", "Transport nodes (stat)", &s.statTransportNodes, 0, 1000000);
    Tweak::intVar("Game/Economy", "Transport ticks (stat)", &s.statTransportTicks, 0, 1000000000);
    Tweak::floatVar("Game/Economy", "Generator fuel tank", &s.generatorFuelTank, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Place range", &s.placeRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter output", &s.emitterOutput, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter reach", &s.emitterReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Bastion output", &s.bastionOutput, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Bastion reach", &s.bastionReach, 2.0f, 46.0f, 0.5f);
    // The lance's Width 0.2 + Focus 0.85 concentrate a CONSERVED total (~25x+ local density), so its useful output
    // range sits far below the other emitters' - hence the tiny floor.
    Tweak::floatVar("Game/Structures", "Lance output", &s.lanceOutput, 0.01f, 8.0f, 0.01f);
    Tweak::floatVar("Game/Structures", "Lance reach", &s.lanceReach, 2.0f, 46.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter shrink time", &s.emitterShrinkTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter grow time", &s.emitterGrowTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter restart charge", &s.emitterRestartCharge, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Health max", &s.structureHealthMax, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &sp.fieldDamageRate, 0.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Friendlies", "Barracks population", &s.barracksPopulation, 0, 200, 1);
    Tweak::intVar("Game/Friendlies", "House population", &s.housePopulation, 0, 100, 1);
    Tweak::floatVar("Game/Friendlies", "House link radius", &s.houseLinkRadius, 2.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Medic energy/s", &s.medicEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Medic heal radius", &sp.medicRange, 2.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Medic heal/s", &sp.medicHealRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Grunt spawn energy", &s.spawnEnergy[0], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Brute spawn energy", &s.spawnEnergy[1], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Runner spawn energy", &s.spawnEnergy[2], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Spitter spawn energy", &s.spawnEnergy[3], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Swarm spawn energy", &s.spawnEnergy[4], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Warrior spawn energy", &s.spawnEnergy[10], 0.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Friendlies", "Grunt population", &s.unitPopulation[0], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Brute population", &s.unitPopulation[1], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Runner population", &s.unitPopulation[2], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Spitter population", &s.unitPopulation[3], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Swarm population", &s.unitPopulation[4], 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Warrior population", &s.unitPopulation[10], 0, 50, 1);
    Tweak::floatVar("Game/Friendlies", "Turret range", &sp.turretRange, 4.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret fire interval", &sp.turretFireInterval, 0.1f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Turret shot energy", &sp.turretShotEnergy, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Turret damage", &sp.turretDamage, 0.0f, 500.0f, 1.0f);
}

static void registerNpc(GameNpcSettings& s, GameUnitParams& up)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    // The shared unit-sim baseline (every unit of every team).
    Tweak::floatVar("Game/Enemies", "Unit energy drain/s @ pressure 1", &up.energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Push tension", &up.tension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field damage/s", &up.fieldDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Field damage mult", &up.fieldDpsMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push starts (x iso)", &up.fieldPushStart, 0.0f, 0.95f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain mult", &up.emitterDrainMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain range (m)", &up.strainRange, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Damage absorb (energy per hp)", &up.damageAbsorb, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Unit damage radius", &up.damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &up.pushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Enemies", "Retarget interval", &up.retargetInterval, 1.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Target search radius", &up.targetSearchRadius, 5.0f, 400.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Route engage radius", &up.routeEngageRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Order break radius", &up.orderBreakRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Unit max speed (m/s)", &up.maxSpeed, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Wander speed mult", &up.wanderSpeedMult, 0.05f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Enemies", "Wander speed max (m/s)", &up.wanderSpeedMax, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Far spread (deg)", &up.farSpreadDeg, 0.0f, 120.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Target track radius", &up.targetTrackRadius, 0.0f, 400.0f, 1.0f);
    Tweak::floatVar("Game/Enemies", "Nav follow radius", &up.navFollowRadius, 0.0f, 400.0f, 1.0f);
    Tweak::boolean("Game/Enemies", "Nav fields", &up.navEnabled);
    // Shared by units AND player capsules; a prefab's `HeightLimit` overrides it (< 0 = no ceiling, for flying units).
    Tweak::floatVar("Game/Nav", "Height limit (m)", &up.heightLimit, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Goal", &up.steerGoal, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Flow", &up.steerFlow, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Flow splat gain", &up.flowSplatGain, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Persist", &up.steerPersist, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Track goal", &up.steerTrackGoal, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Track flow mult", &up.trackFlowMult, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Pressure", &up.steerPressure, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Pressure knee", &up.pressureKnee, 0.01f, 3.0f, 0.01f);
    Tweak::floatVar("Game/Nav", "Flow knee", &up.flowKnee, 0.01f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Nav", "Presence pressure", &up.presencePressure, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Game/Nav", "Look-ahead", &up.steerLook, 2.0f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Wall push", &up.steerWall, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Wall keep (m)", &up.wallKeep, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Corner clip penalty", &up.steerCornerClip, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Nav", "Stuck pressure", &up.stuckPressure, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Nav", "Unstick after (s)", &up.unstickAfter, 0.5f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Nav", "Seed request interval (s)", &up.seedRequestInterval, 0.5f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Order lane speed", &s.orderLaneSpeed, 0.0f, 20.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Stuck lane speed", &s.stuckLaneSpeed, 0.0f, 20.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Lane width (m)", &s.laneWidth, 0.0f, 12.0f, 0.5f);
    Tweak::floatVar("Game/Nav", "Order flow blind (s)", &up.orderFlowBlind, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Friendlies", "Turret beam lifetime", &s.beamLifetime, 0.02f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Combat", "Melee hit lifetime", &s.hitLifetime, 0.02f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Combat", "Muzzle flash intensity", &s.flashIntensity, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Combat", "Hurt light intensity", &up.hurtLightIntensity, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Combat", "Hurt light decay (s)", &up.hurtLightDecay, 0.02f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Combat", "Light area (m)", &up.lightArea, 1.0f, 64.0f, 1.0f);
    Tweak::floatVar("Game/Combat", "Hurt flashes/s per area", &up.hurtFlashRate, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Lobber shot speed", &s.lobberShotSpeed, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Spitter shot speed", &s.spitterShotSpeed, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Sim LOD", "Far tick interval (s)", &s.farInterval, 0.05f, 5.0f, 0.05f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Sim LOD/Stats", "Far ticked", &s.farTicked, 0, 1 << 20, 0.0f, {}, ETweakFlags::None);
}

void Settings::registerGame(GameSettings& s)
{
    registerMatch(s.match);
    registerCamera(s.camera);
    registerPlayer(s.player);
    registerStructures(s.structures, s.structureParams);
    registerNpc(s.npc, s.unitParams);
}
