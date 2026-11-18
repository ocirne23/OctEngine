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
export enum class EStructureType : uint8 { Emitter, Generator, Lumberyard, Battery, FuelTank, Solar, Fabricator, Bastion, Barracks, Wall, Turret, MineralSilo, Constructor, Base, CablePower, CablePipe, CableConveyor, CrossingPower, CrossingPipe, CrossingConveyor, House, MedicStation, Quarry, Bridge, Gate, Lens, Searchlight, Walkway, Armory, MageTower, ShipRail, Cannon, AmmoWorks, AmmoStore, WardResist, WardAbsorb, WardRetaliate, Count };

// THE WARDS: a 1x1 pylon with a light store that stands against a wall and strengthens the walls connected to it (a
// breadth-first search over the touching wall and gate segments, "Walls per ward" at most). One kind each: RESIST takes
// a share off every hit, ABSORB gives each segment a shield that takes hits first, RETALIATE sends a share of every
// bite back to the biter. Each pays for its work from its own light store (StructureSystem::tickWards, Wards.cpp).
export constexpr int WardKindCount = 3;
export constexpr bool isWardType(EStructureType t)
{
    return t == EStructureType::WardResist || t == EStructureType::WardAbsorb || t == EStructureType::WardRetaliate;
}
export constexpr int wardKindOf(EStructureType t) // 0 Resist, 1 Absorb, 2 Retaliate; -1 = not a ward
{
    return t == EStructureType::WardResist ? 0 : t == EStructureType::WardAbsorb ? 1 : t == EStructureType::WardRetaliate ? 2 : -1;
}

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
    return t == EStructureType::Bridge || t == EStructureType::Gate || t == EStructureType::Walkway;
}
// BREACHABLE by the units' flow field (Nav "Wall breach cost"): they walk into it and chew it down. The Walkway too:
// a line of it behind a wall must not close a base off.
export constexpr bool isBreachable(EStructureType t)
{
    return t == EStructureType::Wall || t == EStructureType::Gate || t == EStructureType::Walkway || t == EStructureType::ShipRail;
}

// THE HARVEST: a Lumberyard makes fuel (wood) from the trees around it, a Quarry minerals (stone) from the rocks. Each
// stands anywhere; its YIELD is what its radius holds (StructureSystem::harvestYield).
export constexpr bool isHarvestType(EStructureType t)
{
    return t == EStructureType::Lumberyard || t == EStructureType::Quarry;
}

// Placeable = anything a player may build. The Base only enters through spawnBase, the ship's rails with the ship
// (StructureSystem::ensureShipRails).
export constexpr bool isPlaceableType(EStructureType t)
{
    return (int)t < (int)EStructureType::Count && t != EStructureType::Base && t != EStructureType::ShipRail;
}
// Stone builds and repairs it (investMaterials, the walkers' build and repair work, the Builder's hut): everything placeable,
// plus the two the game places itself - the Base and the ship's rail (rubble of a rail is built again like any blueprint).
export constexpr bool isRepairableType(EStructureType t)
{
    return isPlaceableType(t) || t == EStructureType::Base || t == EStructureType::ShipRail;
}
// PHYSICAL CABLES: 1-cell grid segments, one type per medium. A contiguous same-medium run of
// BUILT segments is one transport RUN; the buildings it touches are its ports (see
// rebuildNetworks). A Crossing is a 1x3 oriented bridge OF ONE MEDIUM (one type per medium,
// like the cables): a perpendicular cable passes UNDER its middle cell, and it conducts only its
// own medium between its two ENDS - never whatever happens to touch them.
export constexpr bool isCableType(EStructureType t)
{
    return t == EStructureType::CablePower || t == EStructureType::CablePipe
        || t == EStructureType::CableConveyor || t == EStructureType::Bridge || t == EStructureType::Walkway;
}
// THE BRIDGE IS A ROAD SEGMENT (a cable of the conveyor medium, cableMediumOf) that stands on water: it joins the
// road's runs, the carriers walk it, and it paints like a road. Unlike the other cable pieces its collider is real
// (bodies stand on the deck).
// THE WALKWAY IS A ROAD SEGMENT too, on a scaffold: its deck is StructureSystem::WalkwayDeckHeight over the ground, to
// stand behind a wall and see over it. Next to a plain road piece its deck is a flight of STAIRS down to the ground
// (StructureSystem::updateWalkway). It is NOT walk-through: a body cannot pass under the deck.
export constexpr bool isCrossingType(EStructureType t)
{
    return t == EStructureType::CrossingPower || t == EStructureType::CrossingPipe
        || t == EStructureType::CrossingConveyor;
}
export constexpr bool isCableOrCrossing(EStructureType t)
{
    return isCableType(t) || isCrossingType(t);
}
// What a player may build ON THE SHIP's deck: every placeable type but the road pieces (the deck's free tiles ARE road).
export constexpr bool isShipPlaceableType(EStructureType t)
{
    return isPlaceableType(t) && !isCableOrCrossing(t);
}
// WALK-THROUGH structures: players and units pass over them (the prefab's collider is Layer Cable,
// projectiles only - or, the Bridge, a deck they stand on), so they are no nav obstacle, never block a placement by a standing actor,
// and an RMB on one is a plain ground order. Cables, crossings and the flat Solar slab - never the Walkway.
export constexpr bool isWalkThrough(EStructureType t)
{
    return (isCableOrCrossing(t) && t != EStructureType::Walkway) || t == EStructureType::Solar || t == EStructureType::Cannon;
}
// THE CANNON stands on ONE cell that may also hold a road piece or a walkway deck (the piece stays the cell's primary
// occupant, the cannon rides `underId`), turned to a facing (eighths of a turn), and fires in an arc around it.
export constexpr bool isRotatedPlacement(EStructureType t)
{
    return t == EStructureType::Cannon;
}
// MUNITIONS: the fourth good (store index 3) - made by the Ammo works, banked by the Ammo store, used by the Cannon.
// They move ONLY by carrier (never on a cable).
export constexpr int GoodMunitions = 3;
export constexpr int cableMediumOf(EStructureType t) // 0 energy, 1 fuel, 2 minerals; -1 = not a cable
{
    return t == EStructureType::CablePower ? 0
         : t == EStructureType::CablePipe ? 1
         : t == EStructureType::CableConveyor || t == EStructureType::Bridge || t == EStructureType::Walkway ? 2 : -1;
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

// THE STORE PRIORITY (a player order, GameStructureComponent::storePriority): how a store trades with the OTHER stores -
// the light between linked hubs, the carts between storages. Its consumers it feeds the same in every setting.
export enum class EStorePriority : uint8 { Balance, Fill, Drain, Count };
export inline constexpr const char* c_storePriorityNames[] = { "Balance", "Fill", "Drain" };
export constexpr bool hasStorePriority(EStructureType t)
{
    return t == EStructureType::MineralSilo || t == EStructureType::Battery || t == EStructureType::Base
        || t == EStructureType::FuelTank || t == EStructureType::AmmoStore;
}
// A building with a player ORDER byte (the popup, GqU / GBu, the save): a barracks' unit type, a store's priority.
export constexpr bool hasOrderByte(EStructureType t)
{
    return isBarracksType(t) || hasStorePriority(t);
}

// UNIT TYPES, indexed like Npc's ENpcType (Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant,
// Titan, Lobber - Npc.ixx static_asserts the count). Structures cannot import Npc, so the
// per-type prices live there as plain arrays.
export constexpr int GameNumUnitTypes = 13; // (+ Spawner, Warrior, Bulwark, Aegis)
// What a barracks may be SET to produce: Grunt/Brute/Runner/Swarm/Warrior (10). The Spitter (3),
// the elite tier (5..8), the Spawner (9) and the shielded Bulwark / Aegis (11, 12) are enemy-only wave units.
// Requests/mirrors/loads for anything else fall back to the Grunt (0).
export constexpr bool isBarracksUnitType(int t)
{
    return t == 0 || t == 1 || t == 2 || t == 4 || t == 10;
}

// The two emitter variants: Emitter = balanced sphere, Bastion = big expensive anchor bubble.
export constexpr bool isEmitterType(EStructureType t)
{
    return t == EStructureType::Emitter || t == EStructureType::Bastion;
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
