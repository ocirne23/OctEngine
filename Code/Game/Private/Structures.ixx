export module Game:Structures;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import Force;

// The build/economy layer WITHOUT a structure roster: every structure is an ENTITY whose
// GameStructureComponent holds its identity (stable id, team, health, blueprint), its three
// resource stores and its LINKS to other structures — distribution runs per-entity over those
// links inside the engine's entity pass (see GameComponents.ixx). This system is the seam around
// them: placement (grid/nodes/requests — the MP validation point), link management, production
// (income, fuel burn, consumer drain, emitter ramps — iterating a PER-FRAME spatial query, the
// same no-lists pattern the units use), the death sweep, per-team totals, mirrors and save/load.
// SAVE FILES STORE Type AS AN INT: never remove or reorder values — new types APPEND before Count.
// Connector is a RETIRED slot (range links are gone; cables are physical now): its table entries
// remain, placement refuses it and loadFrom skips it. BarracksBrute/Runner/Spitter are RETIRED
// too (ONE barracks type now produces the unit type its owner picks): placement refuses them and
// loadFrom maps an old-save entry to a Barracks with that unit type preset.
export enum class EStructureType : uint8 { Emitter, Generator, Connector, Extractor, Battery, FuelTank, Solar, Fabricator, Bastion, Lance, Barracks, BarracksBrute, BarracksRunner, BarracksSpitter, Wall, Turret, MineralSilo, Constructor, Base, CablePower, CablePipe, CableConveyor, Crossing, House, Count };

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
// BUILT segments touching two buildings derives a GameStructureLink between them (see
// rebuildDerivedLinks). The Crossing is a 1x3 oriented bridge: a perpendicular cable passes UNDER
// its middle cell; it conducts whichever ONE medium its two ENDS resolve to.
export constexpr bool isCableType(EStructureType t)
{
    return t == EStructureType::CablePower || t == EStructureType::CablePipe
        || t == EStructureType::CableConveyor;
}
export constexpr bool isCableOrCrossing(EStructureType t)
{
    return isCableType(t) || t == EStructureType::Crossing;
}
export constexpr int cableMediumOf(EStructureType t) // 0 energy, 1 fuel, 2 minerals; -1 = not a cable
{
    return t == EStructureType::CablePower ? 0
         : t == EStructureType::CablePipe ? 1
         : t == EStructureType::CableConveyor ? 2 : -1;
}

// (No live structure ever carries a retired per-type barracks value — loadFrom converts them.)
export constexpr bool isBarracksType(EStructureType t)
{
    return t == EStructureType::Barracks;
}

// UNIT TYPES, indexed like Npc's ENpcType (Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant,
// Titan, Lobber — Npc.ixx static_asserts the count). This partition cannot import Npc, so the
// per-type prices live here as plain arrays.
export constexpr int GameNumUnitTypes = 10; // (+ Spawner)
// What a barracks may be SET to produce: Grunt/Brute/Runner/Swarm. The Spitter (3) and the elite
// tier (5..8) are enemy-only wave units. Requests/mirrors/loads for anything else fall back to
// the Grunt (0).
export constexpr bool isBarracksUnitType(int t)
{
    return t == 0 || t == 1 || t == 2 || t == 4;
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
        // spawn; rebuildDerivedLinks enables the ones pointing at a connected neighbour. The owning
        // EntityPtr keeps the whole tree alive, so the raw pointers cannot dangle.
        Entity* arms[4] = {};
        // Crossing: the medium it currently conducts (-1 = inert), stamped by rebuildDerivedLinks;
        // drives the tint (power/pipe/conveyor hue, authored gray while inert).
        int8 conductMedium = -1;
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
    int unitPopulation(int unitType) const
    {
        return m_unitPopulation[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float unitSpawnEnergy(int unitType) const
    {
        return m_spawnEnergy[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float houseLinkRadius() const { return m_houseLinkRadius; }
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
    float structureCapacity(int index) const { return energyCapacityOf(m_frame[index].type); }
    float energyCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Emitter:
        case EStructureType::Bastion:
        case EStructureType::Lance:
        case EStructureType::Extractor:
        case EStructureType::Solar:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:
        case EStructureType::Turret:      return m_internalBuffer;
        case EStructureType::Barracks:    return m_barracksEnergyCapacity; // cable-fed: units are SPAWNED from energy
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
    void stampTuning(const Ref& s); // capacity/band per medium (per tick — tweaks stay live)
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
    // The BUILT cable segment occupying a cell: the primary occupant, or the under-cable when a
    // crossing sits on top (which is what keeps a crossing from unioning with the cable under it).
    int cableSegmentAt(int cx, int cz, bool builtOnly = true) const;
    // Rebuild every GameStructureLink from cable adjacency (dirty-gated; main thread, game tick):
    // union-find over built segments, crossing conduction, building attachment, then a diff
    // against the live links (owner = lower structureId, so a rebuild never flips flow state).
    // Also refreshes the run table (draw) and the segments' arm visuals.
    void rebuildDerivedLinks();
    void updateArms(const Ref& s); // enable the arm children pointing at connected neighbours

    struct CableRun // persistent between rebuilds for drawDebug (ids — indices go stale)
    {
        uint8 medium = 0;
        oc::vector<uint32> segmentIds;  // cable segments + conducting crossings of the run
        oc::vector<uint32> buildingIds; // attached structures (capacity in the medium)
    };
    oc::unordered_map<uint64, CellEntry> m_cells;
    oc::vector<CableRun> m_runs;
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
        20.0f, // Solar
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
        6.0f,  // Crossing
        40.0f, // House
    };
    float m_startMinerals = 100.0f;
    float m_extractorSnapRadius = 6.0f;
    float m_mineralRate = 2.0f;
    float m_fuelRate = 4.0f;
    float m_baseIncomeMult = 0.25f;
    float m_placeRange = 30.0f;
    float m_cableThroughput[3] = { // by MEDIUM (no tiers any more)
        10.0f,  // energy
        4.0f,  // fuel
        4.0f   // minerals
    };
    float m_cableHealthMax = 40.0f; // segments/crossings are softer than buildings
    float m_internalBuffer = 10.0f;
    float m_generatorBuffer = 10.0f;
    float m_batteryCapacity = 100.0f;
    float m_generatorFuelTank = 10.0f;
    float m_fuelTankCapacity = 100.0f;
    float m_mineralSiloCapacity = 100.0f;
    float m_mineralBaseCapacity = 100.0f;
    float m_barracksEnergyCapacity = 20.0f; // cable-fed spawn stock (own buffer, filled like any consumer)
    float m_genEnergyPerSec = 7.5f;
    float m_solarEnergyPerSec = 1.0f;
    float m_fuelBurnRate = 1.0f;
    float m_fabricatorMineralsPerSec = 1.5f;
    float m_fabricatorFuelPerSec = 1.0f;
    float m_fabricatorEnergyPerSec = 2.0f;
    float m_extractorEnergyPerSec = 1.5f;
    float m_pressureDrawTension = 1.5f;

    float m_emitterEnergyPerSec = 1.5f;
    float m_emitterOutput = 1.2f;
    float m_emitterReach = 27.0f;
    // The Base's shield (the values base.pre used to author; the shield now pays for itself from
    // the Base's own energy store — feed it power cables or it goes dark like any emitter).
    float m_baseEnergyCapacity = 100.0f;
    float m_baseEnergyGenPerSec = 2.0f; // free self-generation (solar-style trickle into its own store)
    float m_baseShieldEnergyPerSec = 1.5f;
    float m_baseShieldOutput = 2.4f;
    float m_baseShieldReach = 27.0f;

    float m_bastionEnergyPerSec = 5.0f;
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
    float m_spawnEnergy[GameNumUnitTypes] = { 5.0f, 20.0f, 6.0f, 9.0f, 2.0f, 15.0f, 40.0f, 80.0f, 15.0f, 30.0f };
    int m_unitPopulation[GameNumUnitTypes] = { 2, 5, 2, 4, 1, 4, 8, 16, 4, 8 };
    int m_barracksPopulation = 20; // a barracks' own population cap
    int m_housePopulation = 10;    // added per linked house
    float m_houseLinkRadius = 25.0f;
};
