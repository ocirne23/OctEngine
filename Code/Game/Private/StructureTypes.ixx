export module Game:StructureTypes;

import Core;

// THE STRUCTURE TYPE VOCABULARY: the enum every structure carries plus the constexpr predicates
// that classify it (placeable, cable, crossing, emitter, barracks, ...). Nothing here touches an
// entity or a system — it is what StructureSystem (Game:Structures) and every other Game partition
// switch on. Re-exported through Game:Structures.
// SAVE FILES STORE Type AS AN INT: never remove or reorder values — new types APPEND before Count.
// Connector is a RETIRED slot (range links are gone; cables are physical now): its table entries
// remain, placement refuses it and loadFrom skips it. BarracksBrute/Runner/Spitter are RETIRED
// too (ONE barracks type now produces the unit type its owner picks): placement refuses them and
// loadFrom maps an old-save entry to a Barracks with that unit type preset.
export enum class EStructureType : uint8 { Emitter, Generator, Connector, Extractor, Battery, FuelTank, Solar, Fabricator, Bastion, Lance, Barracks, BarracksBrute, BarracksRunner, BarracksSpitter, Wall, Turret, MineralSilo, Constructor, Base, CablePower, CablePipe, CableConveyor, CrossingPower, CrossingPipe, CrossingConveyor, House, MedicStation, Count };

export constexpr bool isRetiredBarracksType(EStructureType t)
{
    return t == EStructureType::BarracksBrute || t == EStructureType::BarracksRunner
        || t == EStructureType::BarracksSpitter;
}
// Placeable = anything a player may build. The Base only enters through spawnBase; the Connector
// and the per-type barracks are retired.
export constexpr bool isPlaceableType(EStructureType t)
{
    return (int)t < (int)EStructureType::Count
        && t != EStructureType::Base && t != EStructureType::Connector && !isRetiredBarracksType(t);
}
// PHYSICAL CABLES: 1-cell grid segments, one type per medium. A contiguous same-medium run of
// BUILT segments is one transport RUN; the buildings it touches are its ports (see
// rebuildNetworks). A Crossing is a 1x3 oriented bridge OF ONE MEDIUM (one type per medium,
// like the cables): a perpendicular cable passes UNDER its middle cell, and it conducts only its
// own medium between its two ENDS — never whatever happens to touch them.
export constexpr bool isCableType(EStructureType t)
{
    return t == EStructureType::CablePower || t == EStructureType::CablePipe
        || t == EStructureType::CableConveyor;
}
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
// projectiles only), so they are no nav obstacle, never block a placement by a standing actor,
// and an RMB on one is a plain ground order. Cables, crossings and the flat Solar slab.
export constexpr bool isWalkThrough(EStructureType t)
{
    return isCableOrCrossing(t) || t == EStructureType::Solar;
}
export constexpr int cableMediumOf(EStructureType t) // 0 energy, 1 fuel, 2 minerals; -1 = not a cable
{
    return t == EStructureType::CablePower ? 0
         : t == EStructureType::CablePipe ? 1
         : t == EStructureType::CableConveyor ? 2 : -1;
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

// (No live structure ever carries a retired per-type barracks value — loadFrom converts them.)
export constexpr bool isBarracksType(EStructureType t)
{
    return t == EStructureType::Barracks;
}

// UNIT TYPES, indexed like Npc's ENpcType (Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant,
// Titan, Lobber — Npc.ixx static_asserts the count). Structures cannot import Npc, so the
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
// draw + pressure surcharge + unit siege drain, ramp/latch, outputFrac sync) — every emitter-union
// code path (tickPower, strainable, mirror, save/load) tests THIS, not isEmitterType.
export constexpr bool hasShieldEmitter(EStructureType t)
{
    return isEmitterType(t) || t == EStructureType::Base;
}

export enum class ENodeType : uint8 { Mineral, Fuel };

export constexpr int GameMaxTeams = 8;
