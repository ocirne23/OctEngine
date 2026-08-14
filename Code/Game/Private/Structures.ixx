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
export enum class EStructureType : uint8 { Emitter, Generator, Connector, Extractor, Battery, FuelTank, Solar, Fabricator, Bastion, Lance, Barracks, BarracksBrute, BarracksRunner, BarracksSpitter, Wall, Turret, MineralSilo, Constructor, Base, Count };
export constexpr int NumPlaceableStructures = 18; // everything but the Base (spawned, never placed)

export constexpr bool isBarracksType(EStructureType t)
{
    return t == EStructureType::Barracks || t == EStructureType::BarracksBrute
        || t == EStructureType::BarracksRunner || t == EStructureType::BarracksSpitter;
}

// The three emitter variants: Emitter = balanced sphere, Bastion = big expensive anchor bubble,
// Lance = focused cone aimed at placement (a directional push into the enemy field).
export constexpr bool isEmitterType(EStructureType t)
{
    return t == EStructureType::Emitter || t == EStructureType::Bastion || t == EStructureType::Lance;
}

export enum class ENodeType : uint8 { Mineral, Fuel };
// Basic/Heavy carry ENERGY, Pipe carries FUEL, Conveyor carries MINERALS. The tier rides
// GameStructureLink::cableTier; a link's medium decides which store pair it moves.
export enum class ECableType : uint8 { Basic, Heavy, Pipe, Conveyor, Count };
export constexpr int cableMedium(ECableType t) // 0 = energy, 1 = fuel, 2 = minerals
{
    return t == ECableType::Pipe ? 1 : t == ECableType::Conveyor ? 2 : 0;
}

export constexpr int GameMaxTeams = 8;

export const char* structureTypeName(EStructureType type);

export class StructureSystem final
{
public:
    // A per-frame view of one structure (index positions are valid for THIS frame only; anything
    // persistent goes by the stable id on the component).
    struct Ref
    {
        Entity* entity = nullptr;
        GameStructureComponent* state = nullptr;
        EStructureType type = EStructureType::Emitter;
        int nodeIndex = -1; // Extractor: the node under it (derived from position)
    };

    // CO-OP hooks: the server runs the real sim and notifies; clients mirror via the mirror* calls
    // + the periodic stat sync (mirrored emitters drive their LOCAL ForceComponent, so client-side
    // fields are real).
    std::function<void(int index)> onStructurePlaced;                       // server -> send GPl
    std::function<void(uint32 id)> onStructureRemoved;                      // server -> send GRm
    std::function<void(uint32, uint32, ECableType, bool removed)> onCableChanged; // server -> GCb
    std::function<void(uint32 id)> onRouteChanged;                          // server -> send GRt

    void mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos, const glm::vec2& facingXZ,
        int nodeIndex, uint8 team, bool built);
    void mirrorRemove(uint32 id);
    void mirrorCable(uint32 idA, uint32 idB, ECableType type, bool removed);
    void mirrorStructureState(uint32 id, float healthFrac, float chargeFrac, float fuelFrac,
        float mineralFrac, float outputFrac, float utilFrac, bool powered, bool blueprint);
    void mirrorRoute(uint32 id, std::span<const glm::vec3> points);
    void mirrorTotals(std::span<const float> minerals, std::span<const float> fuel, float energyTotal,
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

    // Join replay iteration (server): every OWNED link as an id pair.
    int cableTotal() const;
    void cableAt(int i, uint32& idA, uint32& idB, ECableType& type) const;
    glm::vec2 structureFacing(int index) const; // from the entity's rotation (Lance replay)
    int structureNodeIndex(int index) const { return m_frame[index].nodeIndex; }
    float structureOutputFrac(int index) const // union: the emitter variant only
    {
        return isEmitterType(m_frame[index].type) ? m_frame[index].state->emitter.outputFrac : 0.0f;
    }

    // SAVE/LOAD (server): every structure + link into/from an AssetNode tree. loadFrom CLEARS the
    // current set first (removal hooks fire, so connected clients prune) and preserves ids.
    void saveTo(AssetNode& root) const;
    void loadFrom(const AssetNode& root);
    void clearAllStructures();

    void registerTweaks();
    void spawnNodes(); // the corridor arena's symmetric node set (deterministic on every instance)
    void spawnBase(const glm::vec3& groundPos, uint8 team = 0);
    void clear(); // drops every structure entity + node (before world teardown)

    // The per-frame REFRESH: one spatial query rebuilds the frame view + the id index. Runs at the
    // top of tickAuthority (server/single player) and tickMirror (clients); placements/removals
    // during the tick maintain it in place.
    void refresh();
    std::span<const Ref> structures() const { return m_frame; }

    // Client requests / local input (queued; validated + applied in tickAuthority — the MP seam).
    void queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
        const glm::vec3& facing, uint8 team);
    void queueCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team);
    void queueDemolishRequest(uint32 id, uint8 team);
    void queueRouteRequest(uint32 id, std::span<const glm::vec3> points, uint8 team);
    static constexpr int MaxRouteWaypoints = GameStructureComponent::MaxRoutePoints;
    std::span<const glm::vec3> structureRoute(int index) const // barracks only (empty elsewhere)
    {
        return isBarracksType(m_frame[index].type) ? m_frame[index].state->route
                                                   : std::span<const glm::vec3>{};
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
    uint8 structureTeam(int index) const { return (uint8)m_frame[index].state->team; }
    bool structureBlueprint(int index) const { return m_frame[index].state->blueprint; }
    bool structurePowered(int index) const { return m_frame[index].state->powered; }
    // A pair may hold ONE LINK PER MEDIUM (power + pipe + conveyor side by side); medium -1 = any.
    bool cableExists(uint32 idA, uint32 idB, int medium = -1) const;
    ECableType cableTypeBetween(uint32 idA, uint32 idB) const;
    // Valid NEW link of `type`: capacity in the medium on BOTH ends, within range (Connector ends
    // reach "Connector link range"), and a Connector carries exactly ONE medium across its links.
    bool cableAllowed(int indexA, int indexB, ECableType type, bool ignoreExisting = false) const;
    // Smart connect: the medium to auto-link a pair with (Count = none valid) — connector's
    // carried medium > the selected building's output > the first medium both hold.
    ECableType smartLinkTypeFor(int indexA, int indexB) const;

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
        case EStructureType::Connector:
        case EStructureType::Solar:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:
        case EStructureType::Turret:      return m_internalBuffer;
        // Barracks hold NO energy: they run purely on conveyor-fed minerals.
        case EStructureType::Generator:   return m_generatorBuffer;
        case EStructureType::Battery:     return m_batteryCapacity; // the Base stores NO energy
        default:                          return 0.0f;
        }
    }
    float structureFuel(int index) const { return m_frame[index].state->store[1]; }
    float structureFuelCapacity(int index) const { return fuelCapacityOf(m_frame[index].type); }
    float connectorUtilization(int index) const { return m_frame[index].state->flowUtil; }
    int connectorMedium(int index) const // the ONE medium this connector carries (-1 = no links)
    {
        const std::vector<GameStructureLink>& links = m_frame[index].state->links;
        return links.empty() ? -1 : (int)links.front().medium;
    }
    float structureMinerals(int index) const { return m_frame[index].state->store[2]; }
    float structureMineralCapacity(int index) const { return mineralCapacityOf(m_frame[index].type); }
    float fuelCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Generator:  return m_generatorFuelTank;
        case EStructureType::FuelTank:   return m_fuelTankCapacity;
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::Connector:  return m_internalBuffer;
        default:                         return 0.0f;
        }
    }
    float mineralCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::Connector:
        case EStructureType::Constructor:  return m_internalBuffer;
        case EStructureType::Barracks:     // conveyor-fed: units are SPAWNED from materials
        case EStructureType::BarracksBrute:
        case EStructureType::BarracksRunner:
        case EStructureType::BarracksSpitter: return m_barracksMineralCapacity;
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
             : t == EStructureType::Lance ? m_lanceReach : m_emitterReach;
    }
    float cableRange() const { return m_cableRange; }
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
        case EStructureType::MineralSilo: return 2;
        case EStructureType::Barracks:
        case EStructureType::BarracksBrute:
        case EStructureType::BarracksRunner:
        case EStructureType::BarracksSpitter:
        case EStructureType::Base:        return 3;
        default:                          return 1;
        }
    }
    static glm::vec3 snapToGrid(EStructureType type, const glm::vec3& groundPos);
    bool cellsFree(EStructureType type, const glm::vec3& snappedGroundPos) const;
    void setPlacementBounds(const glm::vec2& boundsMin, const glm::vec2& boundsMax)
    {
        m_boundsMin = boundsMin;
        m_boundsMax = boundsMax;
        m_hasBounds = true;
    }

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
    struct CableRequest
    {
        uint32 idA = 0, idB = 0;
        ECableType type = ECableType::Basic;
        uint8 team = 0;
    };
    struct RouteRequest
    {
        uint32 id = 0;
        std::vector<glm::vec3> points;
        uint8 team = 0;
    };

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
    void destroyStructureAt(size_t index); // unlink, free the node, remove the entity, fire GRm
    void applyStructureTint(const Ref& s);
    void applyCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team);
    void applyDemolishRequest(uint32 id, uint8 team);
    void stampTuning(const Ref& s); // capacity/band per medium (per tick — tweaks stay live)
    void tickProduction(float deltaSec); // income, fuel burn, consumer drain, emitter ramps
    void tickDamage(float deltaSec);     // death sweep + strainable marks
    void tickConstructors(float deltaSec);
    float emitterOutputOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionOutput
             : t == EStructureType::Lance ? m_lanceOutput : m_emitterOutput;
    }
    float emitterDrawOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionEnergyPerSec
             : t == EStructureType::Lance ? m_lanceEnergyPerSec : m_emitterEnergyPerSec;
    }

    std::vector<Ref> m_frame;                 // THIS FRAME's spatial query view (not owned state)
    std::unordered_map<uint32, int> m_byId;   // stable id -> frame index (rebuilt with it)
    std::vector<Node> m_nodes;
    std::vector<PlaceRequest> m_requests;
    std::vector<CableRequest> m_cableRequests;
    std::vector<std::pair<uint32, uint8>> m_demolishRequests;
    std::vector<RouteRequest> m_routeRequests;
    uint32 m_nextStructureId = 1; // 0 = invalid
    float m_time = 0.0f;
    bool m_wasFuelDry = false;
    glm::vec2 m_boundsMin{ 0.0f }, m_boundsMax{ 0.0f };
    bool m_hasBounds = false;

    float m_minerals[GameMaxTeams] = {};
    float m_fuelTotal[GameMaxTeams] = {};
    float m_gridEnergyTotal = 0.0f;
    float m_gridCapacityTotal = 0.0f;
    float m_genRateTotal = 0.0f;
    float m_useRateTotal = 0.0f;

    // Tweaks (all Synced — the server's values rule)
    float m_costs[18] = { // indexed by EStructureType (the 18 placeables; the Base is free)
        30.0f, // Emitter
        50.0f, // Generator
        20.0f, // Connector
        30.0f, // Extractor
        40.0f, // Battery
        30.0f, // FuelTank
        20.0f, // Solar
        60.0f, // Fabricator
        70.0f, // Bastion
        45.0f, // Lance
        150.0f, // Barracks
        200.0f, // BarracksBrute
        175.0f, // BarracksRunner
        200.0f, // BarracksSpitter
        5.0f,  // Wall (per segment)
        75.0f, // Turret
        30.0f, // MineralSilo
        50.0f, // Constructor
    };
    float m_startMinerals = 100.0f;
    float m_extractorSnapRadius = 6.0f;
    float m_mineralRate = 2.0f;
    float m_fuelRate = 1.5f;
    float m_baseIncomeMult = 0.25f;
    float m_cableRange = 18.0f;
    float m_connectorRange = 32.0f;
    float m_placeRange = 30.0f;
    float m_cableThroughput[(int)ECableType::Count] = { 
        5.0f,  // Basic
        20.0f, // Heavy
        4.0f,  // Pipe
        4.0f   // Conveyor
    };
    float m_internalBuffer = 10.0f;
    float m_generatorBuffer = 10.0f;
    float m_batteryCapacity = 100.0f;
    float m_generatorFuelTank = 10.0f;
    float m_fuelTankCapacity = 100.0f;
    float m_mineralSiloCapacity = 100.0f;
    float m_mineralBaseCapacity = 100.0f;
    float m_barracksMineralCapacity = 20.0f; // conveyor-fed spawn stock (own buffer, not the shared one)
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
    float m_emitterReach = 20.0f;

    float m_bastionEnergyPerSec = 5.0f;
    float m_bastionOutput = 2.6f;
    float m_bastionReach = 30.0f;

    float m_lanceEnergyPerSec = 3.0f;
    float m_lanceOutput = 0.08f;
    float m_lanceReach = 26.0f;

    float m_emitterPressureDraw = 3.0f;
    float m_emitterShrinkTime = 1.5f;
    float m_emitterGrowTime = 0.5f;
    float m_emitterRestartCharge = 6.0f;

    float m_structureHealthMax = 100.0f;
    float m_constructorRange = 18.0f;
    float m_constructorBuildRate = 4.0f;
    float m_constructorBoostRate = 0.5f;
    float m_constructorBoostMaterials = 1.0f;
    float m_constructorBoostEnergy = 1.0f;
    float m_waypointRadius = 3.0f;
    float m_projectileStructDamage = 20.0f;
    bool m_cheatInstantBuild = false;
    // MATERIALS a barracks pays per spawned unit, per unit type (Grunt/Brute/Runner/Spitter) —
    // stamped onto each barracks' component as its spawnCost.
    float m_spawnMaterials[4] = { 5.0f, 12.0f, 6.0f, 9.0f };
};
