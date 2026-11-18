module Settings.Game;

import Core;
import Settings.Tweaks;

// Gameplay tweaks: the server's values overrule the clients' (the Synced blocks).

static void registerMatch(GameMatchSettings& s)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    // THE WAVES: wave k of night n = "Wave budget" x "Night growth (x)"^n x (1 + "Wave growth per wave" x k), +- "Wave variation".
    Tweak::boolean("Game/Coop", "Spawn enemies", &s.coopEnemies);
    Tweak::intVar("Game/Coop", "Wave budget", &s.waveBudget, 1, 5000, 10);
    Tweak::floatVar("Game/Coop", "Night growth (x)", &s.nightGrowth, 1.0f, 20.0f, 0.1f);
    Tweak::intVar("Game/Coop", "Waves per night", &s.nightWaves, 1, 100, 1);
    Tweak::floatVar("Game/Coop", "Wave growth per wave", &s.waveGrowthPerWave, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Coop", "Wave variation", &s.waveVariation, 0.0f, 0.9f, 0.05f);
    Tweak::floatVar("Game/Coop", "Wave distance (m)", &s.sandboxWaveDistance, 20.0f, 90.0f, 1.0f);
    Tweak::floatVar("Game/Coop", "Wave max distance (m)", &s.sandboxWaveMaxDistance, 20.0f, 4000.0f, 5.0f);
    Tweak::floatVar("Game/Coop", "Wave spawn area per unit", &s.waveSpawnAreaPerUnit, 1.0f, 60.0f, 0.5f);
    Tweak::intVar("Game/Coop", "Max enemy units", &s.waveMaxAlive, 1, 100000, 50);
    Tweak::intVar("Game/Coop", "Wave max bodies", &s.waveMaxBodies, 50, 100000, 50);
    Tweak::intVar("Game/Coop", "Target enemies alive", &s.waveTargetAlive, 100, 100000, 100);
    Tweak::intVar("Game/Coop", "Spawns per frame", &s.spawnsPerFrame, 1, 200, 1);
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
    Tweak::floatVar("Game/Coop", "Cost bulwark", &s.waveCost[11], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Cost aegis", &s.waveCost[12], 0.1f, 500.0f, 0.5f);
    Tweak::floatVar("HUD", "Label max distance", &s.labelMaxDistance, 10.0f, 2000.0f, 10.0f, {}, ETweakFlags::None);
    Tweak::floatVar("HUD", "Building bar distance (m)", &s.labelBarDistance, 0.0f, 2000.0f, 5.0f, {}, ETweakFlags::None);
    Tweak::floatVar("HUD", "Unit bar strength (x common enemy)", &s.labelUnitStrength, 0.0f, 10.0f, 0.1f, {}, ETweakFlags::None);
    Tweak::floatVar("Game/Nav", "Nav unit source reach", &s.navUnitSourceReach, 8.0f, 400.0f, 4.0f);
    Tweak::floatVar("Game/Sim LOD", "Unit cluster focus radius", &s.focusClusterRadius, 5.0f, 200.0f, 1.0f, {}, ETweakFlags::None);
    Tweak::floatVar("Game/Coop", "Start time of day (h)", &s.startTimeOfDay, -1.0f, 24.0f, 0.25f);
    Tweak::floatVar("Game/Coop", "Sunlight damage/s", &s.sunlightDps, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Coop", "Sunlight damage (share of health/s)", &s.sunlightShare, 0.0f, 1.0f, 0.005f);
    Tweak::boolean("Game/Coop", "Fields block demons", &s.fieldsBlockDemons);
    Tweak::boolean("Game/Coop", "Demon shadow form", &s.demonShadowForm);
    Tweak::floatVar("Game/Coop", "Staff light radius (m)", &s.staffLightRadius, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Coop", "Demon speed (x)", &s.demonSpeed, 0.05f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Coop", "Demon speed in a field (x)", &s.demonFieldSpeed, 0.05f, 1.0f, 0.05f);
    Tweak::floatVar("Game/Fog", "Spawn fog density (1/m)", &s.spawnFogDensity, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Game/Fog", "Spawn fog glow", &s.spawnFogGlow, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Fog", "Spawn fog time (s)", &s.spawnFogSeconds, 1.0f, 600.0f, 1.0f);
    Tweak::floatVar("Game/Fog", "Spawn signal beam glow", &s.spawnBeamGlow, 0.0f, 200.0f, 0.25f);
    Tweak::floatVar("Game/Fog", "Spawn signal beam radius (m)", &s.spawnBeamRadius, 0.05f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Fog", "Field fog clearing", &s.fieldFogClear, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Game/Beacon", "Enabled", &s.beaconEnabled);
    Tweak::floatVar("Game/Beacon", "Shroud coverage", &s.shroudCoverage, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Shroud full at sun height (dusk)", &s.shroudDuskSunHeight, -0.2f, 0.3f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Day hole strength", &s.beaconDayStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Ignite delay (s)", &s.beaconIgniteDelay, 0.0f, 120.0f, 0.5f);
    Tweak::floatVar("Game/Beacon", "Beam glow time (s)", &s.beaconGlowSeconds, 0.01f, 10.0f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Beam climb time (s)", &s.beaconClimbSeconds, 0.01f, 10.0f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Pierce time (s)", &s.beaconPierceSeconds, 0.1f, 60.0f, 0.1f);
    Tweak::floatVar("Game/Beacon", "Light off at shroud", &s.beaconOffShroud, 0.0f, 0.99f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Beam fade out time (s)", &s.beaconFadeSeconds, 0.01f, 60.0f, 0.1f);
    Tweak::floatVar("Game/Beacon", "Hole close time (s)", &s.beaconCloseSeconds, 0.1f, 300.0f, 0.5f);
    Tweak::floatVar("Game/Beacon", "Calm spin (deg/min)", &s.beaconCalmSpin, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Beacon", "Pressure spin (deg/min)", &s.beaconPressureSpin, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Beacon", "Beam height (m)", &s.beaconBeamHeight, 50.0f, 80000.0f, 50.0f);
    Tweak::floatVar("Game/Beacon", "Beam top fade", &s.beaconBeamTipFade, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Beacon", "Beam light intensity", &s.beaconLightIntensity, 0.0f, 1000.0f, 0.1f);
    Tweak::floatVar("Game/Beacon", "Beam light range (m)", &s.beaconLightRange, 1.0f, 150.0f, 1.0f); // the light grid's memory grows with range^3
    Tweak::floatVar("Game/Beacon", "Beam radius (m)", &s.beaconBeamRadius, 0.05f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Beacon", "Beam light on clouds", &s.beaconCloudLight, 0.0f, 100.0f, 0.01f);
    Tweak::boolean("Game/Lightning", "Enabled", &s.lightningEnabled);
    Tweak::floatVar("Game/Lightning", "Flashes per minute", &s.lightningPerMinute, 0.0f, 240.0f, 0.5f);
    Tweak::floatVar("Game/Lightning", "Arc share", &s.lightningStrikeShare, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Lightning", "Min distance (m)", &s.lightningMinDistance, 0.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Game/Lightning", "Max distance (m)", &s.lightningMaxDistance, 50.0f, 50000.0f, 10.0f);
    Tweak::floatVar("Game/Lightning", "Vortex wall share", &s.lightningWallShare, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Lightning", "Flash time (s)", &s.lightningDuration, 0.05f, 5.0f, 0.01f);
    Tweak::color3("Game/Lightning", "Colour", &s.lightningColor);
    Tweak::floatVar("Game/Lightning", "Cloud light", &s.lightningCloudLight, 0.0f, 1000.0f, 0.1f);
    Tweak::floatVar("Game/Lightning", "Cloud light radius (m)", &s.lightningCloudRadius, 10.0f, 5000.0f, 10.0f);
    Tweak::floatVar("Game/Lightning", "Arc intensity", &s.lightningArcIntensity, 0.0f, 10000.0f, 0.5f);
    Tweak::floatVar("Game/Lightning", "Arc radius (m)", &s.lightningArcRadius, 0.05f, 50.0f, 0.05f);
    Tweak::floatVar("Game/Lightning", "Arc zigzag", &s.lightningArcJitter, 0.0f, 0.5f, 0.005f);
    Tweak::floatVar("Game/Lightning", "Fork share", &s.lightningForkShare, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Construction", "Refill radius", &s.refillRadius, 1.0f, 30.0f, 0.25f);
    Tweak::floatVar("Game/Construction", "Refill rate", &s.refillRate, 0.5f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Construction", "Player build radius", &s.buildRadius, 1.0f, 30.0f, 0.25f);
    Tweak::floatVar("Game/Construction", "Player build rate", &s.playerBuildRate, 0.5f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Base heal radius", &s.baseHealRadius, 0.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Base heal/s", &s.baseHealRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Melee damage/s", &s.meleeDps, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Melee radius", &s.meleeRadius, 0.0f, 12.0f, 0.25f);
    Tweak::floatVar("Game/Player", "Staff beam range (m)", &s.staffBeamRange, 2.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Staff beam half angle (deg)", &s.staffBeamHalfAngleDeg, 0.5f, 45.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Staff beam damage/s", &s.staffBeamDps, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Player", "Staff beam light/s", &s.staffBeamLightPerSec, 0.0f, 100.0f, 0.5f);    Tweak::floatVar("Game/Friendlies", "Follow radius (m)", &s.followRadius, 1.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Follow leash (m)", &s.followLeash, 5.0f, 200.0f, 1.0f);    Tweak::floatVar("Game/Nav", "Group cluster radius", &s.selectionClusterRadius, 2.0f, 60.0f, 0.5f);
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
    Tweak::floatVar("Game/Camera", "Third person distance", &s.thirdDistance, 1.0f, 120.0f, 0.1f);
    Tweak::floatVar("Game/Camera", "Third person min distance", &s.thirdMinDistance, 0.5f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Camera", "Third person max distance", &s.thirdMaxDistance, 2.0f, 120.0f, 0.5f);
    Tweak::floatVar("Game/Camera", "Mouse sensitivity", &s.thirdSensitivity, 0.0002f, 0.02f, 0.0001f);
    Tweak::floatVar("Game/Camera", "Third person min height", &s.thirdMinHeight, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Camera", "Third person eye height", &s.thirdEyeHeight, 0.0f, 6.0f, 0.05f);
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
    Tweak::floatVar("Game/Construction", "Max ground step (m)", &s.maxGroundStep, 0.0f, 4.0f, 0.05f); // 0 = no limit
    Tweak::floatVar("Game/Structures", "Pressure draw tension", &s.pressureDrawTension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Start minerals", &s.startMinerals, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Harvest radius", &s.harvestRadius, 4.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Trees for full lumberyard yield", &s.harvestTreesFull, 1.0f, 200.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Rocks for full quarry yield", &s.harvestStoneFull, 0.5f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Harvest size exponent", &s.harvestSizeExponent, 0.0f, 4.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Harvest reference rock (m)", &s.harvestRockRef, 0.5f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Harvest minimum yield", &s.harvestMinYield, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Economy", "Quarry minerals/s", &s.mineralRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Lumberyard fuel/s", &s.fuelRate, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base income mult", &s.baseIncomeMult, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Emitter cost", &s.costs[0], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Generator cost", &s.costs[1], 0.0f, 500.0f, 1.0f);
    // The indices are EStructureType's (Game:StructureTypes): Emitter 0, Generator 1, Lumberyard 2, Battery 3, FuelTank 4,
    // Solar 5, Fabricator 6, Bastion 7, Barracks 8, Wall 9, Turret 10, MineralSilo 11, Constructor 12, Base 13,
    // CablePower 14, CablePipe 15, CableConveyor 16, CrossingPower 17, CrossingPipe 18, CrossingConveyor 19, House 20,
    // MedicStation 21, Quarry 22, Bridge 23, Gate 24, Lens 25, Searchlight 26, Walkway 27, Armory 28, MageTower 29, ShipRail 30,
    // Cannon 31, AmmoWorks 32, AmmoStore 33, WardResist 34, WardAbsorb 35, WardRetaliate 36.
    Tweak::floatVar("Game/Economy", "Ship rail repair cost", &s.costs[30], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Resist ward cost", &s.costs[34], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Absorb ward cost", &s.costs[35], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Retaliate ward cost", &s.costs[36], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Ward", "Light store", &s.wardCapacity, 1.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Ward", "Refill (light/s)", &s.wardIntake, 0.0f, 100.0f, 0.1f);
    Tweak::floatVar("Game/Ward", "Restart at (share of store)", &s.wardRestart, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Game/Ward", "Walls per ward", &s.wardWalls, 1, 64, 1);
    Tweak::floatVar("Game/Ward", "Resist share", &s.wardResist, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Ward", "Resist light per damage", &s.wardResistCost, 0.0f, 5.0f, 0.005f);
    Tweak::floatVar("Game/Ward", "Absorb shield (health)", &s.wardAbsorbShield, 0.0f, 10000.0f, 5.0f);
    Tweak::floatVar("Game/Ward", "Absorb regen (health/s)", &s.wardAbsorbRegen, 0.0f, 1000.0f, 0.5f);
    Tweak::floatVar("Game/Ward", "Absorb regen delay (s)", &s.wardAbsorbDelay, 0.0f, 60.0f, 0.1f);
    Tweak::floatVar("Game/Ward", "Absorb light per health", &s.wardAbsorbCost, 0.0f, 5.0f, 0.005f);
    Tweak::floatVar("Game/Ward", "Retaliate share", &s.wardRetaliate, 0.0f, 5.0f, 0.01f);
    Tweak::floatVar("Game/Ward", "Retaliate light per damage", &s.wardRetaliateCost, 0.0f, 5.0f, 0.005f);
    Tweak::floatVar("Game/Economy", "Cannon cost", &s.costs[31], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Ammo works cost", &s.costs[32], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Ammo store cost", &s.costs[33], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Range (m)", &s.cannonRange, 5.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Min range (m)", &s.cannonMinRange, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Cannon", "Arc (deg)", &s.cannonArcDeg, 10.0f, 360.0f, 5.0f);
    Tweak::floatVar("Game/Cannon", "Fire interval (s)", &s.cannonFireInterval, 0.2f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Cannon", "Shot speed (m/s)", &s.cannonShotSpeed, 5.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Cannon", "Munitions held", &s.cannonAmmoCapacity, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Turn rate (deg/s)", &s.cannonTurnRate, 5.0f, 720.0f, 5.0f);
    Tweak::intVar("Game/Cannon", "Crew", &s.cannonCrew, 1, 6, 1);
    Tweak::floatVar("Game/Cannon", "Crew call range (m)", &s.cannonCallRange, 5.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Crew stand down (s)", &s.cannonStandDown, 0.0f, 120.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Ammo works munitions/s", &s.ammoRate, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Cannon", "Stone per munition", &s.ammoStonePer, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Cannon", "Light per munition", &s.ammoLightPer, 0.0f, 50.0f, 0.1f);
    Tweak::intVar("Game/Cannon", "Ammo works workers", &s.workersAmmo, 0, 20, 1);
    Tweak::floatVar("Game/Cannon", "Ammo works buffer", &s.ammoWorksBuffer, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Cannon", "Ammo store capacity", &s.ammoStoreCapacity, 1.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Game/Cannon", "Blast radius (m)", &s.ammoBlastRadius, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Cannon", "Blast damage", &s.ammoBlastDamage, 0.0f, 5000.0f, 10.0f);
    Tweak::floatVar("Game/Economy", "Armory cost", &s.costs[28], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Mage tower cost", &s.costs[29], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Searchlight cost", &s.costs[26], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Walkway cost (per segment)", &s.costs[27], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Lens cost", &s.costs[25], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Lumberyard cost", &s.costs[2], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Quarry cost", &s.costs[22], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bridge cost (per segment)", &s.costs[23], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Gate cost (per segment)", &s.costs[24], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Battery cost", &s.costs[3], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fuel tank cost", &s.costs[4], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Solar cost", &s.costs[5], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Fabricator cost", &s.costs[6], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion cost", &s.costs[7], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Barracks cost", &s.costs[8], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "House cost", &s.costs[20], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Medic station cost", &s.costs[21], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo cost", &s.costs[11], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Constructor cost", &s.costs[12], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Power cable cost", &s.costs[14], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Pipeline cost", &s.costs[15], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Conveyor cost", &s.costs[16], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Power crossing cost", &s.costs[17], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Pipe crossing cost", &s.costs[18], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Conveyor crossing cost", &s.costs[19], 0.0f, 100.0f, 0.5f);
    {
        // Health per type, in EStructureType order (the player-facing names where the game has one).
        static constexpr const char* c_healthNames[GameStructureTypeCount] = { "Lantern health", "Hearth health", "Lumberyard health",
            "Crystal store health", "Woodshed health", "Mirror health", "Stoneworks health", "Great lantern health",
            "Barracks health", "Wall health", "Light tower health", "Storehouse health", "Builder's hut health", "Beacon health",
            "Light conduit health", "Wood track health", "Road health", "Conduit crossing health", "Wood crossing health",
            "Road crossing health", "House health", "Healer's hut health", "Quarry health", "Bridge health", "Gate health",
            "Lens health", "Searchlight health", "Walkway health", "Armory health", "Mage tower health", "Ship rail health",
            "Cannon health", "Ammo works health", "Ammo store health", "Resist ward health", "Absorb ward health",
            "Retaliate ward health" };
        for (int i = 0; i < GameStructureTypeCount; ++i)
            Tweak::floatVar("Game/Health", c_healthNames[i], &s.health[i], 1.0f, 10000.0f, 5.0f);
    }
    Tweak::boolean("Game/Light", "Beams", &s.lightBeams);
    Tweak::floatVar("Game/Light", "Mirror beam range (m)", &s.mirrorBeamRange, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Light", "Hub reach (m)", &s.lightHubReach, 5.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Light", "Hub link range (m)", &s.lightHubLinkRange, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Light", "Lens range (m)", &s.lensRange, 4.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Light", "Lens buffer", &s.lensBuffer, 1.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Light", "Lens rate (light/s)", &s.lensRate, 0.5f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Light", "Beam rate (light/s)", &s.lightBeamRate, 0.5f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Light", "Feed rate (light/s)", &s.lightFeedRate, 0.5f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Light", "Share rate (light/s)", &s.lightShareRate, 0.5f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Light", "Mirror glitter (sparks/s)", &s.mirrorGlitterRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Light", "Beam glow", &s.lightBeamGlow, 0.0f, 100.0f, 0.1f);
    Tweak::intVar("Game/Light", "Lit beams", &s.lightBeamLitCount, 0, 64, 1);
    Tweak::floatVar("Game/Light", "Beam full flow (light/s)", &s.lightBeamFullFlow, 0.1f, 500.0f, 0.1f);
    Tweak::floatVar("Game/Light", "Beam radius (m)", &s.lightBeamRadius, 0.02f, 5.0f, 0.01f);
    Tweak::floatVar("Game/Light", "Beam glow range (m)", &s.lightBeamGlowRange, 0.5f, 100.0f, 0.5f);
    Tweak::boolean("Game/Carriers", "Enabled", &s.carriers);
    Tweak::floatVar("Game/Carriers", "Load", &s.carrierLoad, 1.0f, 100.0f, 1.0f);
    Tweak::floatVar("Game/Carriers", "Cart load", &s.carrierCartLoad, 1.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Game/Carriers", "Walk speed (m/s)", &s.carrierSpeed, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Carriers", "Build rate (stone/s)", &s.carrierBuildRate, 0.1f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Armory", "Range (m)", &s.armoryRange, 5.0f, 300.0f, 1.0f);
    Tweak::floatVar("Game/Armory", "Post range (m)", &s.armoryPostRange, 2.0f, 300.0f, 1.0f);
    Tweak::intVar("Game/Armory", "Guards", &s.armoryGuards, 0, 64, 1);
    Tweak::floatVar("Game/Armory", "Stand down after (s)", &s.armoryStandDown, 0.0f, 120.0f, 1.0f);
    Tweak::floatVar("Game/Armory", "Guard range (m)", &s.guardRange, 2.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Armory", "Guard damage", &s.guardDamage, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Armory", "Guard shot interval (s)", &s.guardInterval, 0.1f, 10.0f, 0.05f);
    Tweak::intVar("Game/Armory", "Mages per tower", &s.mageCount, 0, 32, 1);
    Tweak::floatVar("Game/Armory", "Mage range (m)", &s.mageRange, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Armory", "Mage cone half angle (deg)", &s.mageHalfAngleDeg, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Armory", "Mage damage/s", &s.mageDps, 0.0f, 200.0f, 0.1f);
    Tweak::floatVar("Game/Armory", "Mage light brightness (x)", &s.mageLightIntensity, 0.0f, 5.0f, 0.05f);
    Tweak::intVar("Game/Carriers", "Max walkers per building", &s.carriersPerBuilding, 1, 32, 1);
    Tweak::floatVar("Game/Structures", "Constructor range", &s.constructorRange, 2.0f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Constructor build rate", &s.constructorBuildRate, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Structures", "Constructor repair health/s", &s.constructorRepairRate, 1.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Constructor turn speed (deg/s)", &s.constructorTurnRate, 5.0f, 720.0f, 5.0f);
    Tweak::floatVar("Game/Structures", "Constructor stones/s", &s.constructorStreamRate, 0.0f, 100.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Waypoint radius", &s.waypointRadius, 0.5f, 15.0f, 0.25f);
    Tweak::intVar("Game/Structures", "Wall breach cost", &s.wallBreachCost, 1, 254, 1);
    Tweak::floatVar("Game/Economy", "Mineral base capacity", &s.mineralBaseCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Mineral silo capacity", &s.mineralSiloCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Barracks energy intake/s", &s.barracksEnergyIntake, 0.1f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Wall cost (per segment)", &s.costs[9], 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Turret cost", &s.costs[10], 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion energy/s", &s.bastionEnergyPerSec, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Solar energy/s", &s.solarEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator energy/s", &s.fabricatorEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fabricator fuel/s", &s.fabricatorFuelPerSec, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Fabricator minerals/s", &s.fabricatorMineralsPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Fuel tank capacity", &s.fuelTankCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Fuel burn/s per generator", &s.fuelBurnRate, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Economy", "Energy gen/s per generator", &s.genEnergyPerSec, 0.5f, 50.0f, 0.25f);
    Tweak::floatVar("Game/Economy", "Emitter energy/s", &s.emitterEnergyPerSec, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Emitter energy/s @ pressure 1", &s.emitterPressureDraw, 0.0f, 50.0f, 0.1f);
    Tweak::floatVar("Game/Economy", "Base energy capacity", &s.baseEnergyCapacity, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Game/Economy", "Base shield energy/s", &s.baseShieldEnergyPerSec, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield output", &s.baseShieldOutput, 0.1f, 8.0f, 0.1f);
    Tweak::floatVar("Game/Structures", "Base shield reach", &s.baseShieldReach, 2.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Battery capacity", &s.batteryCapacity, 10.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Game/Economy", "Internal buffer", &s.internalBuffer, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Economy", "Emitter buffer", &s.emitterBuffer, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Economy", "Bastion buffer", &s.bastionBuffer, 1.0f, 500.0f, 1.0f);
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
    Tweak::floatVar("Game/Structures", "Emitter reach", &s.emitterReach, 2.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Bastion output", &s.bastionOutput, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Bastion reach", &s.bastionReach, 2.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Emitter shrink time", &s.emitterShrinkTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter grow time", &s.emitterGrowTime, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Emitter restart charge", &s.emitterRestartCharge, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Structures", "Low charge warning (fill)", &s.emitterWarnCharge, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Structures", "Sleeping field alpha", &s.fieldDayAlpha, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Structures", "Rubble rebuild cost (x)", &s.rubbleCost, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Game/Structures", "Rubble safe radius (m)", &s.rubbleSafeRadius, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Structures", "Damage/s in enemy field", &sp.fieldDamageRate, 0.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Population", "Base workers", &s.baseWorkers, 0, 200, 1);
    Tweak::intVar("Game/Population", "House workers", &s.houseWorkers, 0, 200, 1);
    Tweak::intVar("Game/Population", "Workers: lumberyard, quarry", &s.workersHarvest, 0, 50, 1);
    Tweak::intVar("Game/Population", "Workers: fabricator", &s.workersFabricator, 0, 50, 1);
    Tweak::intVar("Game/Population", "Workers: constructor", &s.workersConstructor, 0, 50, 1);
    Tweak::intVar("Game/Population", "Workers: generator", &s.workersGenerator, 0, 50, 1);
    Tweak::intVar("Game/Friendlies", "Barracks population", &s.barracksPopulation, 0, 200, 1);
    Tweak::floatVar("Game/Friendlies", "Barracks unit health (x)", &s.barracksUnitHealthMult, 0.1f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Barracks unit damage (x)", &s.barracksUnitDamageMult, 0.0f, 100.0f, 0.5f);
    Tweak::intVar("Game/Friendlies", "House population", &s.housePopulation, 0, 100, 1);
    Tweak::floatVar("Game/Friendlies", "House link radius", &s.houseLinkRadius, 2.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Medic heal radius", &sp.medicRange, 2.0f, 60.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Medic heal/s", &sp.medicHealRate, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Medic full heal cost (x unit cost)", &sp.medicHealCost, 0.0f, 5.0f, 0.05f);
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
    Tweak::floatVar("Game/Friendlies", "Turret damage", &sp.turretDamage, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Searchlight range", &sp.searchlightRange, 4.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Searchlight cone half angle (deg)", &sp.searchlightHalfAngleDeg, 2.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Searchlight damage/s", &sp.searchlightDps, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Searchlight light/s", &sp.searchlightEnergyPerSec, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Friendlies", "Searchlight turn speed (deg/s)", &sp.searchlightTurnDegPerSec, 5.0f, 720.0f, 5.0f);
    Tweak::floatVar("Game/Friendlies", "Searchlight brightness", &sp.searchlightIntensity, 0.0f, 50000.0f, 50.0f);}

static void registerNpc(GameNpcSettings& s, GameUnitParams& up)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    // The shared unit-sim baseline (every unit of every team).
    Tweak::floatVar("Game/Enemies", "Unit energy drain/s @ pressure 1", &up.energyDrainRate, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Shield drain (battery share/s at iso)", &up.shieldDrainShare, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Game/Enemies", "Push tension", &up.tension, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field damage/s", &up.fieldDps, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Field damage mult", &up.fieldDpsMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push starts (x iso)", &up.fieldPushStart, 0.0f, 0.95f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain mult", &up.emitterDrainMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Bite damage on structures mult", &up.structureBiteMult, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Emitter drain range (m)", &up.strainRange, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Game/Enemies", "Damage absorb (energy per hp)", &up.damageAbsorb, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Enemies", "Unit damage radius", &up.damageRadius, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Enemies", "Field push gain", &up.pushGain, 0.0f, 100000.0f, 100.0f);
    Tweak::floatVar("Game/Enemies", "Push on shielded units (x)", &up.shieldedPushMult, 0.0f, 1.0f, 0.01f);
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
    Tweak::floatVar("Game/Friendlies", "Turret beam glow", &s.beamGlow, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Friendlies", "Turret beam radius (m)", &s.beamRadius, 0.02f, 5.0f, 0.01f);
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

static void registerFaction(GameFactionSettings& s)
{
    Tweak::boolean("Game/Faction", "Enabled", &s.enabled);
    Tweak::floatVar("Game/Faction", "Cell size (m)", &s.cellSize, 1.0f, 32.0f, 0.5f); // read when the grid is made (the match start)
    Tweak::floatVar("Game/Faction", "Sweep time (s)", &s.sweepSeconds, 0.05f, 30.0f, 0.05f);
    Tweak::floatVar("Game/Faction", "Source time (s)", &s.sourceSeconds, 0.5f, 3600.0f, 0.5f);
    Tweak::floatVar("Game/Faction", "Diffusion (1/s)", &s.diffusion, 0.0f, 1.0f, 0.001f);
    Tweak::intVar("Game/Faction", "Start value (0 enemy, 128 neutral, 255 player)", &s.startValue, 0, 255, 1); // read when the grid is made
    Tweak::floatVar("Game/Faction", "Base radius (m)", &s.baseRadius, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Game/Faction", "Field radius (x bubble)", &s.fieldRadiusScale, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Faction", "Enemy spawn site radius (m)", &s.enemySiteRadius, 0.0f, 2000.0f, 5.0f);
    Tweak::boolean("Game/Enemy outposts", "Enabled", &s.outpostsEnabled);
    Tweak::intVar("Game/Enemy outposts", "Count", &s.outpostCount, 1, 16, 1);                                 // read when the ring is made
    Tweak::floatVar("Game/Enemy outposts", "Distance from the Base (m)", &s.outpostDistance, 50.0f, 7500.0f, 10.0f); // read when the ring is made
    Tweak::floatVar("Game/Enemy outposts", "Faction radius (m)", &s.outpostRadius, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Game/Enemy outposts", "Beam height (m)", &s.outpostBeamHeight, 10.0f, 80000.0f, 10.0f);
    Tweak::floatVar("Game/Enemy outposts", "Beam radius (m)", &s.outpostBeamRadius, 0.05f, 20.0f, 0.05f);
    Tweak::floatVar("Game/Enemy outposts", "Beam top fade", &s.outpostBeamTipFade, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Enemy outposts", "Beam intensity", &s.outpostBeamIntensity, 0.0f, 1000.0f, 0.1f);
    Tweak::floatVar("Game/Enemy outposts", "Beam halo range (m)", &s.outpostBeamRange, 1.0f, 500.0f, 1.0f);
}

static void registerSound(GameSoundSettings& s)
{
    Tweak::boolean("Game/Sound", "Enabled", &s.enabled);
    Tweak::floatVar("Game/Sound", "Effects volume", &s.effectsVolume, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Sound", "Structure volume", &s.structureVolume, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Sound", "Ambience volume", &s.ambienceVolume, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Sound", "Crowd volume", &s.crowdVolume, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Sound", "Interface volume", &s.interfaceVolume, 0.0f, 2.0f, 0.01f);
    Tweak::intVar("Game/Sound/Limits", "Max one-shot voices", &s.maxVoices, 1, 48, 1.0f);
    Tweak::intVar("Game/Sound/Limits", "Max building loops", &s.maxLoops, 0, 12, 1.0f);
    Tweak::floatVar("Game/Sound", "Height in the panning", &s.panHeight, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Sound/Distance", "Reverb send", &s.reverbSend, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Game/Sound/Distance", "Air distance (m)", &s.airDistance, 10.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Game/Sound/Limits", "Full volume distance (m)", &s.fullVolumeDistance, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Game/Sound/Limits", "Hearing range (m)", &s.hearingRange, 10.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Sound/Crowd", "Cues/s for a full battle bed", &s.crowdFull, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/Sound/Crowd", "Battle focus", &s.battleFocus, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Sound/Crowd", "Units for a full horde bed", &s.hordeFull, 1.0f, 5000.0f, 1.0f);
    Tweak::floatVar("Game/Sound/Crowd", "Horde radius (m)", &s.hordeRadius, 5.0f, 300.0f, 1.0f);
    // In EGameSound's order (Game:Sound), after "-": the pick plays that sound once. Every pick is another variant.
    static constexpr oc::string_view SOUND_NAMES[] = { "-", "Melee hit", "Unit reveal", "Unit death", "Shot fired", "Lob fired",
        "Shot impact", "Splash impact", "Turret fire", "Structure placed", "Structure built", "Structure collapse",
        "Bird song", "Cricket bout", "Owl call", "Wind gust", "Thunder", "Thunder crack",
        "Alarm", "Player hurt", "Loop: wind", "Loop: day", "Loop: night", "Loop: battle", "Loop: horde",
        "Loop: hum", "Loop: fire", "Loop: works" };
    Tweak::enumVar("Game/Sound", "Play a sound", &s.audition, SOUND_NAMES, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Game/Sound/Ambience", "Enemies that silence the animals", &s.fearUnits, 0.5f, 200.0f, 0.5f);
    Tweak::floatVar("Game/Sound/Ambience", "Animals return (s)", &s.fearReturn, 1.0f, 300.0f, 1.0f);
    Tweak::floatVar("Game/Sound/Limits", "Loudness limit", &s.loudnessLimit, 0.1f, 8.0f, 0.05f);
    Tweak::floatVar("Game/Sound/Stats", "Loudness (before the limit)", &s.loudness, 0.0f, 100.0f, 0.0f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Sound/Stats", "Voices playing", &s.voicesPlaying, 0, 1 << 20, 0.0f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Sound/Stats", "Cues in/s", &s.cuesPerSecond, 0, 1 << 30, 0.0f, {}, ETweakFlags::None);
    Tweak::intVar("Game/Sound/Stats", "One-shots started/s", &s.cuesPlayed, 0, 1 << 20, 0.0f, {}, ETweakFlags::None);
}

static void registerShip(GameShipSettings& s)
{
    const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
    Tweak::boolean("Game/Ship", "Enabled", &s.enabled);
    Tweak::floatVar("Game/Ship", "Thrust (m/s2)", &s.thrust, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Throttle rate (/s)", &s.throttleRate, 0.05f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Max speed (m/s)", &s.maxSpeed, 1.0f, 80.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Side drag (/s)", &s.sideDrag, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Turn rate (deg/s)", &s.turnRate, 1.0f, 90.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Turn accel (deg/s2)", &s.turnAccel, 0.5f, 90.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Turn drag (/s)", &s.turnDrag, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Min height (m)", &s.minHeight, 0.5f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Max height (m)", &s.maxHeight, 1.0f, 300.0f, 1.0f);
    Tweak::floatVar("Game/Ship", "Climb rate (m/s)", &s.climbRate, 0.5f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Vertical stiffness", &s.verticalStiffness, 0.05f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Vertical damping", &s.verticalDamping, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Max vertical accel (m/s2)", &s.maxVerticalAccel, 0.1f, 30.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Look ahead (s)", &s.lookAhead, 0.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Deep water (m)", &s.deepWater, 0.5f, 50.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Sea speed (x)", &s.seaSpeedMult, 0.05f, 2.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Freeboard (m)", &s.freeboard, -5.0f, 10.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Sea stiffness", &s.seaStiffness, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Sea damping", &s.seaDamping, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Game/Ship", "Wave follow (x)", &s.waveFollow, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Bank per turn", &s.bankPerTurn, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Game/Ship", "Max bank (deg)", &s.maxBank, 0.0f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Pitch per accel", &s.pitchPerAccel, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Max pitch (deg)", &s.maxPitch, 0.0f, 20.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Bob height (m)", &s.bobHeight, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Game/Ship", "Bob period (s)", &s.bobPeriod, 0.5f, 30.0f, 0.5f);
    Tweak::floatVar("Game/Ship", "Camera distance (m)", &s.cameraDistance, 5.0f, 200.0f, 1.0f, {}, ETweakFlags::None);
}

void Settings::registerGame(GameSettings& s)
{
    registerShip(s.ship);
    registerSound(s.sound);
    registerMatch(s.match);
    registerFaction(s.faction);
    registerCamera(s.camera);
    registerPlayer(s.player);
    registerStructures(s.structures, s.structureParams);
    registerNpc(s.npc, s.unitParams);
}
