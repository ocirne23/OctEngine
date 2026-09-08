export module Game:Structures;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import Force;
import Threading; // the transport job

// The build/economy layer: every structure is an ENTITY whose GameStructureComponent holds its
// identity (stable id, team, health, blueprint) and its three float resource stores. Resources
// move between structures over the CABLE TRANSPORT this system owns (whole cells per segment, a
// fixed-rate job — see the block below and Transport.cpp). This system is the seam around them:
// placement (grid/nodes/requests — the MP validation point), the network rebuild, production
// (income, fuel burn, consumer drain, emitter ramps — over the roster), the death sweep, per-team
// totals, mirrors and save/load.
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
// Titan, Lobber — Npc.ixx static_asserts the count). This partition cannot import Npc, so the
// per-type prices live here as plain arrays.
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

export const char* structureTypeName(EStructureType type);

export class StructureSystem final
{
public:
    // One ROSTER entry (index positions are valid for THIS frame only — removals reindex; anything
    // persistent goes by the stable id on the component). `owner` is an OWNING ref: the raw
    // pointers can never dangle, and every way an entity leaves the world funnels through
    // World::removeRootEntity, whose callback deregisters the entry (see onWorldRootRemoved) — so
    // no per-frame world query is needed to revalidate the roster.
    struct Ref
    {
        EntityPtr owner;
        Entity* entity = nullptr;
        GameStructureComponent* state = nullptr;
        EStructureType type = EStructureType::Emitter;
        int nodeIndex = -1; // Extractor: the node under it
        // Cable segments: the four render-only arm child entities (+X, -X, +Z, -Z), cached at
        // spawn; rebuildNetworks enables the ones pointing at a connected neighbour. The owning
        // EntityPtr keeps the whole tree alive, so the raw pointers cannot dangle.
        Entity* arms[4] = {};
        // PROBLEM BADGE (the world label's bubble): what GameMatch::structureWarning last found,
        // re-checked on a jittered ~1 s timer instead of every frame — it scans the links, and the
        // states it reports change on the timescale of a player's actions. Lives HERE, so it dies
        // with its structure (no id-keyed map).
        const char* warning = nullptr; // nullptr = nothing wrong
        glm::vec3 warningColor{ 1.0f };
        float warningTimer = 0.0f;     // seconds to the next check (0 = check on the next label build)
        // House: the barracks it feeds population to (0 = none in range), re-derived every
        // refresh() — nearest BUILT own-team barracks within "House link radius".
        uint32 linkedId = 0;
    };

    // CO-OP hooks: the server runs the real sim and notifies; clients mirror via the mirror* calls
    // + the periodic stat sync (mirrored emitters drive their LOCAL ForceComponent, so client-side
    // fields are real).
    oc::function<void(int index)> onStructurePlaced;                       // server -> send GPl
    oc::function<void(uint32 id)> onStructureRemoved;                      // server -> send GRm
    // A blueprint completed (investMaterials): the server re-fires GPl for it — cable segments are
    // excluded from GSt, so this is the only way a client learns a cable finished building.
    oc::function<void(uint32 id)> onStructureBuilt;
    oc::function<void(uint32 id)> onRouteChanged;                          // server -> send GRt
    oc::function<void(uint32 id)> onUnitTypeChanged;                       // server -> send GBu
    // Authority: re-push a changed route onto the barracks' live units. The unit roster lives in
    // NpcSystem (this partition cannot import it), so GameMatch wires the walk in.
    oc::function<void(uint32 id, oc::span<const glm::vec3> route)> onRouteLiveUnits;

    void mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos, const glm::vec2& facingXZ,
        int nodeIndex, uint8 team, bool built);
    void mirrorRemove(uint32 id);
    void mirrorStructureState(uint32 id, float healthFrac, float chargeFrac, float fuelFrac,
        float mineralFrac, float outputFrac, float utilFrac, bool powered, bool blueprint);
    // Cable/crossing BLUEPRINTS only (GCb): health = build progress, over the segment's own
    // healthMax. The built flip still arrives through the GPl re-send.
    void mirrorCableProgress(uint32 id, float healthFrac);
    void mirrorRoute(uint32 id, oc::span<const glm::vec3> points);
    void mirrorUnitType(uint32 id, uint8 unitType);
    void mirrorTotals(oc::span<const float> minerals, oc::span<const float> fuel, float energyTotal,
        float energyCap, float genRate, float useRate)
    {
        for (int t = 0; t < GameMaxTeams; ++t)
        {
            m_minerals[t] = t < (int)minerals.size() ? minerals[t] : 0.0f;
            m_fuelTotal[t] = t < (int)fuel.size() ? fuel[t] : 0.0f;
        }
        m_gridEnergyTotal = energyTotal;
        m_gridCapacityTotal = energyCap;
        m_genRateTotal = genRate;
        m_useRateTotal = useRate;
    }
    void tickMirror(float deltaSec); // client per-frame: refresh + ease emitter outputs

    glm::vec2 structureFacing(int index) const; // from the entity's rotation (Lance/Crossing replay)
    int structureNodeIndex(int index) const { return m_frame[index].nodeIndex; }
    float structureOutputFrac(int index) const // union: the emitter variant (Base included)
    {
        return hasShieldEmitter(m_frame[index].type) ? m_frame[index].state->emitter.outputFrac : 0.0f;
    }
    // The bubble spheres (xyz center, w radius) the shield structures currently project: one per
    // merge GROUP where members merged (friendly units in the group ride along), else the
    // structure's own bubble; structures with no bubble (blueprints, unpowered) contribute none.
    // Appends at most maxCount; the SIM LOD zones (GameMatch::update). Main thread.
    void collectShieldBubbles(oc::vector<glm::vec4>& out, uint32 maxCount) const;

    // SAVE/LOAD (server): every structure + link into/from an AssetNode tree. loadFrom CLEARS the
    // current set first (removal hooks fire, so connected clients prune) and preserves ids.
    void saveTo(AssetNode& root) const;
    void loadFrom(const AssetNode& root);
    void clearAllStructures();

    void registerTweaks();
    // CO-OP: GameMatch places nodes one by one from the generated map (seeded — every instance
    // derives the identical set from the same seed, the corridor-set contract).
    void spawnNode(float x, float z, ENodeType type); // one resource node entity + roster entry
    void clearNodes(); // co-op map regeneration: drop the node entities + roster (nothing else)
    void spawnBase(const glm::vec3& groundPos, uint8 team = 0);
    void clear(); // drops every structure entity + node (before world teardown)

    // The per-frame REFRESH: re-stamps live tuning (capacities/bands/throughputs) onto every roster
    // entry. The roster itself is maintained at the spawn/remove seams (spawnStructure,
    // destroyStructureAt, onWorldRootRemoved) — no world query. Runs at the top of tickAuthority
    // (server/single player) and tickMirror (clients).
    void refresh();
    // World::removeRootEntity notification (wired by GameMatch): deregisters the entry with full
    // bookkeeping (unlink, node free, GRm hook) for ANY removal path — editor delete, script
    // destroy — not just the game's own. Must NOT call removeRootEntity (see World.ixx).
    void onWorldRootRemoved(const Entity* entity);
    oc::span<const Ref> structures() const { return m_frame; }

    // Client requests / local input (queued; validated + applied in tickAuthority — the MP seam).
    void queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
        const glm::vec3& facing, uint8 team);
    void queueDemolishRequest(uint32 id, uint8 team);
    void queueRouteRequest(uint32 id, oc::span<const glm::vec3> points, uint8 team);
    void queueUnitTypeRequest(uint32 id, uint8 unitType, uint8 team); // barracks: what it produces
    static constexpr int MaxRouteWaypoints = GameStructureComponent::MaxRoutePoints;
    // BARRACKS readouts (union: barracks variant — 0 elsewhere).
    uint8 structureUnitType(int index) const
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->barracks.unitType : 0;
    }
    int structurePopulation(int index) const
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->barracks.population : 0;
    }
    int structurePopCap(int index) const
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->barracks.popCap : 0;
    }
    int structureHouses(int index) const
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->barracks.houses : 0;
    }
    uint32 structureLinkedId(int index) const { return m_frame[index].linkedId; } // House -> barracks
    // A cable segment / crossing's transport readout (the selected-cable label): its own fill
    // against the segment capacity, the cells leaving it as a rate averaged over ~2 s, the segment's
    // out-rate, and its run's total fill / capacity / segment count. false = not conducting (a
    // blueprint, or a segment on no run). Clients read mirrored fills, no rates.
    struct CableInfo
    {
        int medium = 0;
        int fill = 0, capacity = 0;
        float movedPerSec = 0.0f, ratePerSec = 0.0f;
        int runFill = 0, runCapacity = 0, runSegments = 0;
    };
    bool cableInfo(int index, CableInfo& out) const;
    // MIRROR of the cable fills (server -> GCf, rotating through the cable nodes from `cursor`):
    // one byte per segment; clients apply by id. Junction nodes are never sent.
    struct CableMirror { uint32 id; uint8 fill; uint8 util; }; // util = the ~2 s throughput / rate, x255
    void collectCableFills(oc::vector<CableMirror>& out, uint32& cursor, int maxRecords) const;
    void mirrorCableFill(uint32 id, uint8 fill, uint8 util);
    float transportTickPeriod() const { return 1.0f / glm::max(m_transportTickHz, 1.0f); }
    // The problem badge cached on the roster entry (see Ref::warning): GameMatch's world-labels
    // job re-checks each structure on its own jittered timer and stamps the result here.
    const char* structureWarning(int index) const { return m_frame[index].warning; }
    const glm::vec3& structureWarningColor(int index) const { return m_frame[index].warningColor; }
    float& structureWarningTimer(int index) { return m_frame[index].warningTimer; }
    void setStructureWarning(int index, const char* text, const glm::vec3& color)
    {
        m_frame[index].warning = text;
        m_frame[index].warningColor = color;
    }
    int unitPopulation(int unitType) const
    {
        return m_unitPopulation[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float unitSpawnEnergy(int unitType) const
    {
        return m_spawnEnergy[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float houseLinkRadius() const { return m_houseLinkRadius; }
    // The medic's reach/rate live in GameStructureParams (the component's machine logic heals
    // units); these read them for the player heal, the ghost and the ring.
    float medicHealRadius() const { return GameStructureComponent::params.medicRange; }
    float medicHealRate() const { return GameStructureComponent::params.medicHealRate; }
    int housePopulation() const { return m_housePopulation; }
    oc::span<const glm::vec3> structureRoute(int index) const // barracks only (empty elsewhere)
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->route
                                                   : oc::span<const glm::vec3>{};
    }
    const GameStructureComponent* structureStateById(uint32 id) const
    {
        const int index = structureIndexById(id);
        return index >= 0 ? m_frame[index].state : nullptr;
    }
    GameStructureComponent* structureStateById(uint32 id)
    {
        const int index = structureIndexById(id);
        return index >= 0 ? m_frame[index].state : nullptr;
    }
    float waypointRadius() const { return m_waypointRadius; }
    int wallBreachCost() const { return m_wallBreachCost; } // Nav step multiplier through a built wall

    int structureIndexById(uint32 id) const
    {
        const auto it = m_byId.find(id);
        return it != m_byId.end() ? it->second : -1;
    }
    int structureIndexByEntity(const Entity* entity) const
    {
        for (int i = 0; i < (int)m_frame.size(); ++i)
            if (m_frame[i].entity == entity)
                return i;
        return -1;
    }
    int structureCount() const { return (int)m_frame.size(); }
    uint32 structureId(int index) const { return m_frame[index].state->structureId; }
    glm::vec3 structurePos(int index) const { return m_frame[index].entity->pos; }
    glm::vec3 structureLabelAnchor(int index) const;
    EStructureType structureType(int index) const { return m_frame[index].type; }
    float structureHealth(int index) const { return m_frame[index].state->health; }
    float structureHealthMax() const { return m_structureHealthMax; }
    float structureHealthMaxOf(int index) const { return m_frame[index].state->healthMax; } // cables are softer
    uint8 structureTeam(int index) const { return (uint8)m_frame[index].state->team; }
    bool structureBlueprint(int index) const { return m_frame[index].state->blueprint; }
    bool structurePowered(int index) const { return m_frame[index].state->powered; }

    uint32 randomTargetStructureId(const glm::vec3& nearPos, uint8 attackerTeam) const;
    void damageStructure(uint32 id, float amount);
    bool trySpendEnergy(uint32 id, float energy)
    {
        GameStructureComponent* c = stateById(id);
        if (!c || c->store[0] < energy)
            return false;
        c->store[0] -= energy;
        return true;
    }
    bool trySpendStoredMinerals(uint32 id, float amount)
    {
        GameStructureComponent* c = stateById(id);
        if (!c || c->store[2] < amount)
            return false;
        c->store[2] -= amount;
        return true;
    }

    void tickAuthority(const glm::vec3& playerPos, float deltaSec);
    void drawDebug() const;

    int findFreeNodeNear(const glm::vec3& groundPos, float maxDist) const;
    glm::vec3 nodeGroundPos(int nodeIndex) const
    {
        const glm::vec3& p = m_nodes[nodeIndex].pos;
        return glm::vec3(p.x, 0.0f, p.z);
    }
    float extractorSnapRadius() const { return m_extractorSnapRadius; }
    int nodeCount() const { return (int)m_nodes.size(); }
    glm::vec3 nodePos(int index) const { return m_nodes[index].pos; }
    ENodeType nodeType(int index) const { return m_nodes[index].type; }
    bool nodeExtracted(int index) const { return m_nodes[index].extracted; }
    // The nearest structure the cursor can select within maxDist (planar).
    int findConnectableNear(const glm::vec3& pos, float maxDist) const;

    float minerals(uint8 team = 0) const { return m_minerals[glm::min((int)team, GameMaxTeams - 1)]; }
    float fuel(uint8 team = 0) const { return m_fuelTotal[glm::min((int)team, GameMaxTeams - 1)]; }
    float gridEnergy() const { return m_gridEnergyTotal; }
    float gridEnergyCapacity() const { return m_gridCapacityTotal; }
    float energyGenPerSec() const { return m_genRateTotal; }
    float energyUsePerSec() const { return m_useRateTotal; }
    float structureCharge(int index) const { return m_frame[index].state->store[0]; }
    float structureCapacity(int index) const { return m_frame[index].state->capacity[0]; } // per-instance (barracks: its unit's cost)
    float energyCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Emitter:     return m_emitterBuffer; // shield emitters hold a deeper charge:
        case EStructureType::Bastion:     return m_bastionBuffer; // pressure draw spikes under a push
        case EStructureType::Lance:       return m_lanceBuffer;
        case EStructureType::Extractor:
        case EStructureType::Solar:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:
        case EStructureType::MedicStation:
        case EStructureType::Turret:      return m_internalBuffer;
        case EStructureType::Barracks:    return 1.0f; // > 0 = power cables attach; the REAL capacity is
                                                       // stamped per instance (= its unit's cost, the build bar)
        case EStructureType::Generator:   return m_generatorBuffer;
        case EStructureType::Battery:     return m_batteryCapacity;
        case EStructureType::Base:        return m_baseEnergyCapacity; // feeds its always-on shield
        default:                          return 0.0f;
        }
    }
    float structureFuel(int index) const { return m_frame[index].state->store[1]; }
    float structureFuelCapacity(int index) const { return fuelCapacityOf(m_frame[index].type); }
    float structureFlowUtil(int index) const { return m_frame[index].state->flowUtil; }
    float structureMinerals(int index) const { return m_frame[index].state->store[2]; }
    float structureMineralCapacity(int index) const { return mineralCapacityOf(m_frame[index].type); }
    float fuelCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Generator:  return m_generatorFuelTank;
        case EStructureType::FuelTank:   return m_fuelTankCapacity;
        case EStructureType::Extractor:
        case EStructureType::Fabricator: return m_internalBuffer;
        default:                         return 0.0f;
        }
    }
    float mineralCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:  return m_internalBuffer;
        // Barracks hold NO minerals: units are paid from their energy store (no conveyor attaches).
        case EStructureType::MineralSilo:  return m_mineralSiloCapacity;
        case EStructureType::Base:         return m_mineralBaseCapacity;
        default:                           return 0.0f;
        }
    }
    float mineralCost(EStructureType type) const { return m_costs[(int)type]; }
    // The build hotbar's hover card: one sentence + this type's exact per-second flows, one per
    // line, straight off the live tweaks (see the definition).
    oc::string describeType(EStructureType type) const;
    int affordableCount(EStructureType type, uint8 team = 0) const
    {
        const float cost = m_costs[(int)type];
        return cost > 0.0f ? (int)(minerals(team) / cost) : 0;
    }
    float emitterReachOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionReach
             : t == EStructureType::Lance ? m_lanceReach
             : t == EStructureType::Base ? m_baseShieldReach : m_emitterReach;
    }
    float placeRange() const { return m_placeRange; }

    // ---- GRID PLACEMENT --------------------------------------------------------------------
    static constexpr float GridCellSize = 2.0f;
    static constexpr int footprintCellsOf(EStructureType t)
    {
        switch (t)
        {
        case EStructureType::FuelTank:
        case EStructureType::Bastion:
        case EStructureType::Turret:
        case EStructureType::Solar:
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::House:
        case EStructureType::MedicStation:
        case EStructureType::MineralSilo: return 2;
        case EStructureType::Barracks:
        case EStructureType::Base:        return 3;
        default:                          return 1; // cables/crossing included (Crossing extends
        }                                           // along its facing — see footprintExtent)
    }
    // Cells covered along X and Z. Everything is square except the Crossing: 3x1 along its facing
    // (the entity rotation, quantized to an axis at placement).
    static glm::ivec2 footprintExtent(EStructureType t, const glm::quat& rot);
    static glm::vec3 snapToGrid(EStructureType type, const glm::vec3& groundPos);
    // ignoreCables: the unit-spawn probe — cables are walk-through, so a conveyor ring around a
    // barracks must not block its spawn points.
    bool cellsFree(EStructureType type, const glm::vec3& snappedGroundPos,
        const glm::quat& rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f), bool ignoreCables = false) const;
    // The SOLE occupant of a 1-cell snapped position that a crossing's middle may bridge: a plain
    // cable segment, or another crossing's END cell (-1 = empty, a building, a crossing middle, or
    // already bridged). The paint stroke's auto-crossing probe: such a cell of another medium gets
    // the stroke's crossing placed over it.
    int bridgeableAt(const glm::vec3& snappedGroundPos) const;
    // A crossing placement, validated with ONE relaxation `cellsFree` does not make: an END cell
    // may hold a plain cable of the crossing's OWN medium, which is REPLACED. Such a segment is
    // redundant — the crossing's end conducts that medium anyway — and refusing it meant a stroke
    // could not cross a foreign line wherever its own run already stood. Everything else (another
    // medium, a building, a second crossing) blocks exactly as before. `replace` names the
    // segments to demolish first; own team only, so a stroke never eats an enemy's cable.
    struct CrossingPlan
    {
        bool valid = false;
        uint32 replace[2] = {}; // the two END cells' own-medium cables (0 = that end was empty)
    };
    CrossingPlan planCrossing(EStructureType type, const glm::vec3& snappedGroundPos,
        const glm::quat& rot, uint8 team) const;
    static float spawnHeightOf(EStructureType type); // the prefab box's HALF height (ghost preview)
    // A player capsule or unit standing on the footprint (spatial query — no rosters). Separate
    // from cellsFree on purpose: that one also probes unit SPAWN points, which must not refuse a
    // cell just because units stand nearby.
    static bool actorInFootprint(EStructureType type, const glm::vec3& snappedGroundPos);
    void setPlacementBounds(const glm::vec2& boundsMin, const glm::vec2& boundsMax)
    {
        m_boundsMin = boundsMin;
        m_boundsMax = boundsMax;
        m_hasBounds = true;
    }
    // CO-OP impassable terrain: world-space rects (minX, minZ, maxX, maxZ) no footprint may enter
    // — checked by cellsFree exactly like the arena bounds (so ghosts turn red and the unit spawn
    // probe refuses rock cells too). GameMatch rebuilds the list with the map.
    void setTerrainBlocked(oc::vector<glm::vec4> rects) { m_terrainBlocked = oc::move(rects); }

    // ---- CONSTRUCTION (server) -------------------------------------------------------------
    float takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount);
    float fundNearbyBlueprint(const glm::vec3& pos, float radius, uint8 team, float amount,
        bool includeRepairs = false);
    float investMaterials(const Ref& s, float amount);
    float constructorRange() const { return m_constructorRange; }

private:
    struct Node
    {
        EntityPtr entity;
        glm::vec3 pos{ 0.0f };
        ENodeType type = ENodeType::Mineral;
        bool extracted = false;
    };
    struct PlaceRequest
    {
        EStructureType type;
        glm::vec3 pos;
        glm::vec3 facing{ 0.0f };
        int nodeIndex = -1;
        uint8 team = 0;
    };
    struct RouteRequest
    {
        uint32 id = 0;
        oc::vector<glm::vec3> points;
        uint8 team = 0;
    };
    struct UnitTypeRequest
    {
        uint32 id = 0;
        uint8 unitType = 0;
        uint8 team = 0;
    };
    // HOUSES: each built house links to the nearest built own-team barracks within the link
    // radius (one barracks per house); a barracks' population cap = its own allowance + its
    // linked houses' bonus. Derived every refresh() on every instance (clients too — positions
    // are mirrored, so the same derivation yields the same caps).
    void linkHouses();

    GameStructureComponent* stateById(uint32 id)
    {
        const int index = structureIndexById(id);
        return index >= 0 ? m_frame[index].state : nullptr;
    }
    void placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
        const glm::vec3& facing, uint8 team);
    void spendMinerals(uint8 team, float amount); // drains the team's Silos first, the Base last
    // The one placement path: spawns the prefab, stamps the component, registers into the frame.
    // Returns the frame index or -1. rot = authored orientation (Lance facing).
    int spawnStructure(uint32 id, EStructureType type, const glm::vec3& pos, const glm::quat& rot,
        uint8 team, bool built, int nodeIndex);
    void destroyStructureAt(size_t index); // deregister (removeStructureBookkeeping) + drop the world's ref
    void removeStructureBookkeeping(size_t index); // unlink, free the node, erase + reindex, fire GRm
    void applyStructureTint(const Ref& s);
    void applyDemolishRequest(uint32 id, uint8 team);
    void stampTuning(const Ref& s); // capacities + machine variant (per tick — tweaks stay live)
    void tickProduction(float deltaSec); // income, fuel burn, consumer drain, emitter ramps
    void tickDamage(float deltaSec);     // death sweep + strainable marks
    void tickConstructors(float deltaSec);

    // ---- CELL OCCUPANCY + DERIVED LINKS ----------------------------------------------------
    // One entry per occupied grid cell. `id` is the primary occupant; `underId` is set only on a
    // Crossing's MIDDLE cell — the perpendicular cable passing under it.
    struct CellEntry
    {
        uint32 id = 0;
        uint32 underId = 0;
    };
    static uint64 cellKey(int cx, int cz) { return (uint64)(uint32)cx << 32 | (uint32)cz; }
    // Visit every (cx, cz) cell of a footprint at the snapped position.
    template <typename Fn>
    static void forEachFootprintCell(EStructureType type, const glm::vec3& pos, const glm::quat& rot,
        Fn&& fn)
    {
        const glm::ivec2 ext = footprintExtent(type, rot);
        const int bx = (int)std::lround(pos.x / GridCellSize - (float)ext.x * 0.5f);
        const int bz = (int)std::lround(pos.z / GridCellSize - (float)ext.y * 0.5f);
        for (int dz = 0; dz < ext.y; ++dz)
            for (int dx = 0; dx < ext.x; ++dx)
                fn(bx + dx, bz + dz);
    }
    void insertCells(const Ref& s);   // spawn seam (also demotes an under-cable below a crossing)
    void eraseCells(const Ref& s);    // remove seam (promotes the under-cable back to primary)
    // Everything a footprint must clear BEFORE the per-cell occupancy walk: the arena bounds, the
    // co-op rock rects and the free nodes' reserved extractor footprints. Shared by cellsFree and
    // planCrossing, which only differ in what they make of an OCCUPIED cell.
    bool footprintAreaClear(EStructureType type, const glm::vec3& p, const glm::quat& rot) const;
    // (cx, cz) is the raised MIDDLE cell of the crossing at frame index `index`.
    bool isCrossingCenter(int index, int cx, int cz) const;
    // The occupant at frame index `index` may pass UNDER a crossing's middle at (cx, cz): a plain
    // cable, or a crossing whose END cell this is — an end counts as cable for bridging.
    bool isBridgeable(int index, int cx, int cz) const;
    // The BUILT cable segment occupying a cell: the primary occupant, or the under-cable when a
    // crossing sits on top (which is what keeps a crossing from unioning with the cable under it).
    int cableSegmentAt(int cx, int cz, bool builtOnly = true) const;
    // Rebuild the CABLE TRANSPORT NETWORKS from cable adjacency (dirty-gated; main thread, game
    // tick): union-find over built segments, crossing conduction, building attachment and
    // bridging, then the flat node graph the transport job runs on (below). Also refreshes the
    // segments' arm visuals. Cable fills survive a rebuild by structure id.
    void rebuildNetworks();
    void updateArms(const Ref& s); // enable the arm children pointing at connected neighbours

    // ---- THE CABLE TRANSPORT (Transport.cpp) ----------------------------------------------
    // Resources move over the cables as WHOLE CELLS. Every built cable segment and conducting
    // crossing is a NODE with an integer fill and an out-rate; a built building that holds a
    // medium is a JUNCTION node (its port: every segment touching it is a neighbour, so a line
    // through an emitter carries through), and the building's SLOT hangs on that node. Each
    // transport tick (fixed rate, runs staggered over frames) the game INJECTS every slot's
    // supply/demand from the building's float store, and a JOB ticks the due runs IN PARALLEL:
    // per run two BFS fields (hops to the nearest wanting port, hops to the nearest pushing
    // port), then `Substeps` of owner-only stencil passes — OFFER: a node serves its slots'
    // demand first, then FORWARDS into neighbours with free space that are downhill toward
    // demand, else uphill away from supply (filling up), then takes slot supply into its free
    // space, all within its out-rate; APPLY: fill' = fill - out + in — and the join hands the
    // cells to the stores. No atomics, no entity walk, no TLS, the result is independent of
    // scheduling, and a run's bottleneck is simply its slowest segment.
    // Node adjacency is CSR (a junction has any number of neighbours); each adjacency slot knows
    // the reverse slot so APPLY reads its inflow from the neighbours' own out-slots.
    struct TransportNode
    {
        uint32 structureId = 0;    // the cable/crossing this node is — or the bridged building's id
        uint32 adjFirst = 0, adjCount = 0;
        uint32 slotFirst = 0, slotCount = 0; // the building slots on this (junction) node
        uint32 rateFp = 0;         // out budget per SUB-STEP, 1/1024 cell (junctions: x degree)
        uint32 carryFp = 0;        // fractional budget carried between sub-steps
        uint16 fill = 0;           // cells held — may transiently exceed the segment capacity (soft)
        uint16 moved = 0;          // cells that left this node last tick (gauge)
        float movedAvg = 0.0f;     // cells/s, EMA over ~2 s of `moved` (the label's throughput)
        uint16 run = 0;
        uint8 medium = 0;
        uint8 junction = 0;        // a building's port node: never saved or mirrored
        uint8 rotate = 0;          // integer-remainder rotation counter
        uint16 dist = 0xFFFF;      // THE DEMAND FIELD: hops (through cables) to the nearest port that
                                   // wants cells this tick; 0xFFFF = none. Cells move strictly
                                   // DOWNHILL on it — toward demand — first
        uint16 sdist = 0xFFFF;     // THE SUPPLY FIELD: hops to the nearest port pushing cells this
                                   // tick. With no demand outlet, cells move strictly UPHILL on it —
                                   // away from supply — so cables fill up outward from producers
    };
    struct TransportAdj { uint32 node; uint32 reverse; }; // neighbour + its slot pointing back here
    enum class ETransportRole : uint8 { Consumer, Producer, Storage };
    struct TransportSlot // one (building, medium) port
    {
        GameStructureComponent* state = nullptr;
        uint32 node = 0;           // the junction it hangs on
        uint8 medium = 0;
        ETransportRole role = ETransportRole::Consumer;
        int32 supply = 0, demand = 0; // cells offered / wanted this tick (inject); served down by the job
        int32 taken = 0, given = 0;   // cells the network took from / delivered to this port this tick
        int32 reserved = 0;           // cells reserved OUT of the store at inject (refund = reserved - taken)
        float intakeCarry = 0.0f;     // metered machines: fractional per-tick intake carry
        float intakePerSec = 0.0f;    // 0 = unmetered
        int8 storageMode = 0;         // storage: +1 pulling, -1 pushing, 0 undecided — flips only at the
                                      // OPPOSITE mark (true hysteresis; a per-tick band idled every
                                      // other tick on small segments)
    };
    struct TransportRun { uint32 firstNode = 0, numNode = 0; uint8 medium = 0; uint8 group = 0; };
    struct TransportNet
    {
        oc::vector<TransportNode> nodes;
        oc::vector<TransportAdj> adj;
        oc::vector<TransportSlot> slots;
        oc::vector<TransportRun> runs;
        oc::unordered_map<uint32, uint32> nodeById; // cable/crossing id -> node (label, mirror, save)
        // job scratch, owner-only writes
        oc::vector<uint16> outAdj;  // per adjacency slot: cells sent to that neighbour this sub-step
        oc::vector<uint16> outSlot; // per port slot: cells delivered this sub-step
        oc::vector<uint16> inSlot;  // per port slot: cells taken from the port this sub-step
        oc::vector<uint32> dueRuns; // the runs this job ticks
        oc::vector<uint32> bfsQueue; // nodes.size() entries: each run's BFS uses ITS node range as
                                     // its queue (owner-only, so runs tick in parallel — no TLS)
    };
    TransportNet m_net;
    oc::unordered_map<uint32, uint16> m_savedFills; // fills of cables that left the graph (rebuild carry-over)
    JobCounter m_transportCounter;
    bool m_transportKicked = false;
    float m_transportGroupNext[8] = {};  // sim time each stagger group next ticks
    float m_transportTime = 0.0f;
    uint32 m_transportTickIndex = 0;
    // Inject the due runs' slots + kick the job (end of tickAuthority); join it + hand the cells
    // to the stores (top of tickAuthority). The job overlaps present and the next frame's front.
    void kickTransport();
    void joinTransport();
    void transportTick(); // the job body: substeps x (offer, apply) over m_net.dueRuns
    void transportApplyBoundary();
    void transportInject(const TransportRun& run);
    void addTransportSlot(int buildingIdx, int medium, uint32 node);
    static ETransportRole transportRoleOf(EStructureType t, int medium);

    oc::unordered_map<uint64, CellEntry> m_cells;
    bool m_linksDirty = true;
    float emitterOutputOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionOutput
             : t == EStructureType::Lance ? m_lanceOutput
             : t == EStructureType::Base ? m_baseShieldOutput : m_emitterOutput;
    }
    float emitterDrawOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionEnergyPerSec
             : t == EStructureType::Lance ? m_lanceEnergyPerSec
             : t == EStructureType::Base ? m_baseShieldEnergyPerSec : m_emitterEnergyPerSec;
    }

    oc::vector<Ref> m_frame;                 // the persistent roster (owning refs — see Ref)
    oc::unordered_map<uint32, int> m_byId;   // stable id -> roster index (maintained with it)
    oc::vector<Node> m_nodes;
    oc::vector<PlaceRequest> m_requests;
    oc::vector<oc::pair<uint32, uint8>> m_demolishRequests;
    oc::vector<RouteRequest> m_routeRequests;
    oc::vector<UnitTypeRequest> m_unitTypeRequests;
    uint32 m_nextStructureId = 1; // 0 = invalid
    float m_time = 0.0f;
    glm::vec2 m_boundsMin{ 0.0f }, m_boundsMax{ 0.0f };
    bool m_hasBounds = false;
    oc::vector<glm::vec4> m_terrainBlocked; // co-op rock rects (minX, minZ, maxX, maxZ)

    float m_minerals[GameMaxTeams] = {};
    float m_fuelTotal[GameMaxTeams] = {};
    float m_gridEnergyTotal = 0.0f;
    float m_gridCapacityTotal = 0.0f;
    float m_genRateTotal = 0.0f;
    float m_useRateTotal = 0.0f;

    // Tweaks (all Synced — the server's values rule)
    float m_costs[(int)EStructureType::Count] = { // indexed by EStructureType (Base/Connector free)
        30.0f, // Emitter
        40.0f, // Generator
        0.0f,  // Connector (retired)
        25.0f, // Extractor
        40.0f, // Battery
        30.0f, // FuelTank
        40.0f, // Solar
        60.0f, // Fabricator
        70.0f, // Bastion
        45.0f, // Lance
        150.0f, // Barracks
        0.0f,  // BarracksBrute (retired)
        0.0f,  // BarracksRunner (retired)
        0.0f,  // BarracksSpitter (retired)
        5.0f,  // Wall (per segment)
        75.0f, // Turret
        25.0f, // MineralSilo
        50.0f, // Constructor
        100.0f, // Base (spawned, never placed — this entry only prices its REPAIRS)
        2.0f,  // CablePower (per segment)
        2.0f,  // CablePipe
        2.0f,  // CableConveyor
        6.0f,  // CrossingPower
        6.0f,  // CrossingPipe
        6.0f,  // CrossingConveyor
        40.0f, // House
        50.0f, // MedicStation
    };
    float m_startMinerals = 200.0f;
    float m_extractorSnapRadius = 6.0f;
    float m_mineralRate = 2.0f;
    float m_fuelRate = 4.0f;
    float m_baseIncomeMult = 0.25f;
    float m_placeRange = 30.0f;
    float m_cableThroughput[3] = { // by MEDIUM: a segment's OUT-RATE in cells/s (the bottleneck unit)
        20.0f,  // energy
        4.0f,  // fuel
        4.0f   // minerals
    };
    // The transport tick: fixed rate, `Substeps` stencil passes per tick (an empty line fills at
    // Substeps segments per tick; a full one moves at the segment rate), runs staggered over
    // `Spread` groups so a large base's work lands on several frames.
    float m_transportTickHz = 10.0f;
    int m_transportSubsteps = 4;
    int m_transportSpread = 4;
    int m_cellsPerSegment[3] = { 2, 1, 1 }; // soft capacity of one segment by MEDIUM (a run buffers
                                            // segments x this): energy holds a burst, the slower
                                            // pipe/conveyor are pure transport
    int cellsPerSegmentOf(int medium) const { return glm::max(m_cellsPerSegment[glm::clamp(medium, 0, 2)], 1); }
    float m_storageLowMark = 0.25f;   // storage pushes while its port node is at/below this fill
    float m_storageHighMark = 0.75f;  // ... and pulls while at/above this (hysteresis between)
    int m_statTransportNodes = 0;     // read-only stats under Game/Economy
    int m_statTransportTicks = 0;
    float m_cableHealthMax = 40.0f; // segments/crossings are softer than buildings
    float m_internalBuffer = 10.0f;
    float m_emitterBuffer = 50.0f;  // the shield emitters' energy stores (their pressure draw spikes)
    float m_bastionBuffer = 150.0f;
    float m_lanceBuffer = 100.0f;
    float m_generatorBuffer = 10.0f;
    float m_batteryCapacity = 200.0f;
    float m_generatorFuelTank = 10.0f;
    float m_fuelTankCapacity = 200.0f;
    float m_mineralSiloCapacity = 200.0f;
    float m_mineralBaseCapacity = 200.0f;
    // MEDIC STATION: a plain powered consumer; while powered its component update heals own-team
    // units in reach (GameStructureParams::medicRange/medicHealRate) and GameMatch heals the own
    // player (tickMedicHealing).
    float m_medicEnergyPerSec = 1.5f;
    float m_barracksEnergyIntake = 2.0f; // energy/s a barracks' transport port takes at most: the BUILD RATE
                                         // (build time = unit cost / this — Grunt 5 -> 2.5 s, Brute 20 -> 10 s)
    float m_genEnergyPerSec = 5.0f;
    float m_solarEnergyPerSec = 1.0f;
    float m_fuelBurnRate = 1.0f;
    float m_fabricatorMineralsPerSec = 0.5f;
    float m_fabricatorFuelPerSec = 1.0f;
    float m_fabricatorEnergyPerSec = 1.0f;
    float m_extractorEnergyPerSec = 1.0f;
    float m_pressureDrawTension = 1.5f;

    float m_emitterEnergyPerSec = 1.0f;
    float m_emitterOutput = 1.2f;
    float m_emitterReach = 27.0f;
    // The Base's shield (the values base.pre used to author; the shield now pays for itself from
    // the Base's own energy store — feed it power cables or it goes dark like any emitter).
    float m_baseEnergyCapacity = 100.0f;
    float m_baseEnergyGenPerSec = 2.0f; // free self-generation (solar-style trickle into its own store)
    float m_baseShieldEnergyPerSec = 1.5f;
    float m_baseShieldOutput = 2.4f;
    float m_baseShieldReach = 27.0f;

    float m_bastionEnergyPerSec = 2.0f;
    float m_bastionOutput = 2.6f;
    float m_bastionReach = 45.0f;

    float m_lanceEnergyPerSec = 3.0f;
    float m_lanceOutput = 0.08f;
    float m_lanceReach = 26.0f;

    float m_emitterPressureDraw = 3.0f;
    float m_emitterShrinkTime = 1.5f;
    float m_emitterGrowTime = 0.5f;
    float m_emitterRestartCharge = 6.0f;

    float m_structureHealthMax = 100.0f;
    float m_constructorRange = 27.0f;
    float m_constructorBuildRate = 4.0f;
    float m_waypointRadius = 3.0f;
    int m_wallBreachCost = 10; // a wall cell costs (1 + this) x 2 m of walking in the enemy fields
    float m_projectileStructDamage = 20.0f;
    bool m_cheatInstantBuild = false;
    // Per UNIT TYPE (Grunt/Brute/Runner/Spitter/Swarm — ENpcType order): the ENERGY a barracks
    // pays per spawned unit and the POPULATION the unit holds, stamped onto each barracks'
    // component (spawnCost/spawnPop) from its selected type.
    float m_spawnEnergy[GameNumUnitTypes] = { 5.0f, 20.0f, 6.0f, 9.0f, 2.0f, 15.0f, 40.0f, 80.0f, 15.0f, 30.0f, 12.0f };
    int m_unitPopulation[GameNumUnitTypes] = { 2, 5, 2, 4, 1, 4, 8, 16, 4, 8, 3 };
    int m_barracksPopulation = 20; // a barracks' own population cap
    int m_housePopulation = 10;    // added per linked house
    float m_houseLinkRadius = 25.0f;
};

// Debug-line helpers shared by the Game implementation units (Structures, Npc, the Match*.cpp
// files). Module linkage, NOT exported; bodies in Structures.cpp.
uint32 packColor(const glm::vec3& c);
void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments = 32);
