export module Game:StructureTypes;

import Core;

// THE STRUCTURE TYPE VOCABULARY: the enum every structure carries plus the constexpr predicates
// that classify it (placeable, cable, crossing, emitter, barracks, ...). Nothing here touches an
// entity or a system - it is what StructureSystem (Game:Structures) and every other Game partition
// switch on. Re-exported through Game:Structures.
// A save file stores Type as this enum's INT, and NO save compatibility is kept: a type that is no longer used is
// REMOVED here and in every table indexed by the enum (the names and spawn heights in Structures.cpp, the short
// names in Match.ixx, the prefabs in StructuresPlacement.cpp, the costs in Settings.Game), and an old save or
// scenario file then loads wrong.
export enum class EStructureType : uint8 { Emitter, Generator, Lumberyard, Battery, FuelTank, Solar, Fabricator, Bastion, Lance, Barracks, Wall, Turret, MineralSilo, Constructor, Base, CablePower, CablePipe, CableConveyor, CrossingPower, CrossingPipe, CrossingConveyor, House, MedicStation, Quarry, Bridge, Gate, Lens, Searchlight, Count };

// LINE types: 1-cell segments placed as a drag LINE (press, drag, release). The Wall and the Gate (a wall the PLAYERS
// walk through: its collider is Layer Gate, which leaves the Player layer out).
export constexpr bool isLineType(EStructureType t)
{
    return t == EStructureType::Wall || t == EStructureType::Gate;
}
// STANDS ON WATER: where water lies over the terrain, the water SURFACE is the ground the structure stands on
// (StructureSystem::groundFit), and its prefab reaches down to the bed. Every other type refuses water.
export constexpr bool standsOnWater(EStructureType t)
{
    return t == EStructureType::Bridge || t == EStructureType::Gate;
}
// BREACHABLE by the units' flow field (Nav "Wall breach cost"): they walk into it and chew it down.
export constexpr bool isBreachable(EStructureType t)
{
    return t == EStructureType::Wall || t == EStructureType::Gate;
}

// THE HARVEST: a Lumberyard makes fuel (wood) from the trees around it, a Quarry minerals (stone) from the rocks. Each
// stands anywhere; its YIELD is what its radius holds (StructureSystem::harvestYield).
export constexpr bool isHarvestType(EStructureType t)
{
    return t == EStructureType::Lumberyard || t == EStructureType::Quarry;
}

// Placeable = anything a player may build. The Base only enters through spawnBase.
export constexpr bool isPlaceableType(EStructureType t)
{
    return (int)t < (int)EStructureType::Count && t != EStructureType::Base;
}
// PHYSICAL CABLES: 1-cell grid segments, one type per medium. A contiguous same-medium run of
// BUILT segments is one transport RUN; the buildings it touches are its ports (see
// rebuildNetworks). A Crossing is a 1x3 oriented bridge OF ONE MEDIUM (one type per medium,
// like the cables): a perpendicular cable passes UNDER its middle cell, and it conducts only its
// own medium between its two ENDS - never whatever happens to touch them.
export constexpr bool isCableType(EStructureType t)
{
    return t == EStructureType::CablePower || t == EStructureType::CablePipe
        || t == EStructureType::CableConveyor || t == EStructureType::Bridge;
}
// THE BRIDGE IS A ROAD SEGMENT (a cable of the conveyor medium, cableMediumOf) that stands on water: it joins the
// road's runs, the carriers walk it, and it paints like a road. Unlike the other cable pieces its collider is real
// (bodies stand on the deck).
export constexpr bool isCrossingType(EStructureType t)
{
    return t == EStructureType::CrossingPower || t == EStructureType::CrossingPipe
        || t == EStructureType::CrossingConveyor;
}
export constexpr bool isCableOrCrossing(EStructureType t)
{
    return isCableType(t) || isCrossingType(t);
}
// WALK-THROUGH structures: players and units pass over them (the prefab's collider is Layer Cable,
// projectiles only - or, the Bridge, a deck they stand on), so they are no nav obstacle, never block a placement by a standing actor,
// and an RMB on one is a plain ground order. Cables, crossings and the flat Solar slab.
export constexpr bool isWalkThrough(EStructureType t)
{
    return isCableOrCrossing(t) || t == EStructureType::Solar;
}
export constexpr int cableMediumOf(EStructureType t) // 0 energy, 1 fuel, 2 minerals; -1 = not a cable
{
    return t == EStructureType::CablePower ? 0
         : t == EStructureType::CablePipe ? 1
         : t == EStructureType::CableConveyor || t == EStructureType::Bridge ? 2 : -1;
}
export constexpr int crossingMediumOf(EStructureType t) // same scale; -1 = not a crossing
{
    return t == EStructureType::CrossingPower ? 0
         : t == EStructureType::CrossingPipe ? 1
         : t == EStructureType::CrossingConveyor ? 2 : -1;
}
export constexpr EStructureType crossingForMedium(int medium)
{
    return medium == 1 ? EStructureType::CrossingPipe
         : medium == 2 ? EStructureType::CrossingConveyor : EStructureType::CrossingPower;
}
export constexpr int conduitMediumOf(EStructureType t) // cable OR crossing medium; -1 = neither
{
    return isCableType(t) ? cableMediumOf(t) : crossingMediumOf(t);
}

export constexpr bool isBarracksType(EStructureType t)
{
    return t == EStructureType::Barracks;
}

// UNIT TYPES, indexed like Npc's ENpcType (Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant,
// Titan, Lobber - Npc.ixx static_asserts the count). Structures cannot import Npc, so the
// per-type prices live there as plain arrays.
export constexpr int GameNumUnitTypes = 11; // (+ Spawner, Warrior)
// What a barracks may be SET to produce: Grunt/Brute/Runner/Swarm/Warrior (10). The Spitter (3),
// the elite tier (5..8) and the Spawner (9) are enemy-only wave units. Requests/mirrors/loads for
// anything else fall back to the Grunt (0).
export constexpr bool isBarracksUnitType(int t)
{
    return t == 0 || t == 1 || t == 2 || t == 4 || t == 10;
}

// The three emitter variants: Emitter = balanced sphere, Bastion = big expensive anchor bubble,
// Lance = focused cone aimed at placement (a directional push into the enemy field).
export constexpr bool isEmitterType(EStructureType t)
{
    return t == EStructureType::Emitter || t == EStructureType::Bastion || t == EStructureType::Lance;
}
// The Base's always-on bubble now runs on the SAME shield rules as the placeable emitters (energy
// draw + pressure surcharge + unit siege drain, ramp/latch, outputFrac sync) - every emitter-union
// code path (tickPower, strainable, mirror, save/load) tests THIS, not isEmitterType.
export constexpr bool hasShieldEmitter(EStructureType t)
{
    return isEmitterType(t) || t == EStructureType::Base;
}

export enum class ENodeType : uint8 { Mineral, Fuel };

export constexpr int GameMaxTeams = 8;
