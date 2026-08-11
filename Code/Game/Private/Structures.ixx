export module Game:Structures;

import Core;
import Core.glm;
import Entity;
import Force;

// The build/economy layer: resource nodes, the Minerals/Fuel stocks, the placeable structures and
// the power grids connecting them.
//  - A node produces nothing on its own: an EXTRACTOR must be built on it (one per node), and the
//    extractor must be cabled into a POWERED grid. Powered mineral extractors accrue Minerals
//    (global — the build currency). FUEL IS GRID-LOCAL: powered fuel extractors feed fuel into
//    their OWN grid's tanks (generators + Base, "Generator fuel tank" each); generators burn from
//    their grid's pool and stop when it runs dry. Fuel never teleports between grids — haul it by
//    cabling the fuel extractor's grid into the consumer grid.
//  - Power flows over MANUAL CABLES (the hotbar Cable tool, no auto-linking): ANY two structures
//    within cable range connect (two emitters relay power fine); transmitters are just the cheap
//    long-run piece, not a topological requirement. Cable-connected structures form a GRID
//    (union-find over cables) with an ENERGY economy: generators turn Fuel into energy/s, BATTERY
//    structures (and the Base, worth one battery) store it — charge lives ON the storage members,
//    so grid merges pool it, splits carry it, and a destroyed battery loses its charge. Consumers
//    (emitters, extractors) drain stored+generated energy in placement order — beyond it emitters
//    go dark and extractors stop harvesting. Generators only run against demand or an unfilled
//    battery (no idle Fuel burn); a storage-less grid still works as pure throughput.
//  - Every structure has health and drains while standing in ENEMY-owned territory (per-structure
//    ForceQuery); a friendly bubble overhead protects. Dead structures despawn (a dead extractor
//    frees its node; cables to a dead structure are pruned).
// Authority tick = main thread; this is the future server-side seam (queuePlaceRequest/
// queueCableRequest become client->server events validated before applying).
//  - The BASE exists from game start (spawnBase, never placeable): respawn anchor, always-on
//    emitter (self-powered), passive income equal to one extractor of EACH type, cable-connectable
//    like a building, and is invulnerable (a dead base would soft-lock the respawn).
export enum class EStructureType : uint8 { Emitter, Generator, Transmitter, Extractor, Battery, FuelTank, Base, Count };
export constexpr int NumPlaceableStructures = 6; // hotbar slots 0..5; Base is spawned, never placed

export enum class ENodeType : uint8 { Mineral, Fuel };

export const char* structureTypeName(EStructureType type);

export class StructureSystem final
{
public:
    void registerTweaks();
    void spawnNodes(); // authored whitebox layout around the map
    void spawnBase(const glm::vec3& groundPos); // game-start Base (free, one expected)
    void clear();      // drops every entity/query/emitter handle (before world teardown)

    // nodeIndex: Extractor only — the node it builds on (from findFreeNodeNear); -1 otherwise.
    void queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex = -1);
    // Cable tool: toggles the cable between two structures (by STABLE id) — creates it when the
    // pair validates (cableAllowed), removes it when it already exists.
    void queueCableRequest(uint32 idA, uint32 idB);
    // Delete mode: removes the structure (no refund). The Base is refused at apply time.
    void queueDemolishRequest(uint32 id);
    void tickAuthority(const glm::vec3& playerPos, float deltaSec);
    void drawDebug() const; // cable lines + node rings (windowed tick)

    // Extractor placement helper: the nearest node (XZ) without an extractor within maxDist of
    // groundPos, or -1. The ghost snaps to the returned node's ground position.
    int findFreeNodeNear(const glm::vec3& groundPos, float maxDist) const;
    glm::vec3 nodeGroundPos(int nodeIndex) const
    {
        const glm::vec3& p = m_nodes[nodeIndex].pos;
        return glm::vec3(p.x, 0.0f, p.z);
    }
    float extractorSnapRadius() const { return m_extractorSnapRadius; }

    // Cable-tool helpers (windowed aim). Indices are valid only within the frame (structures erase
    // on death) — persist selections as ids, not indices.
    int findConnectableNear(const glm::vec3& pos, float maxDist) const; // nearest structure, or -1
    int structureIndexById(uint32 id) const; // -1 when dead
    int structureCount() const { return (int)m_structures.size(); }
    uint32 structureId(int index) const { return m_structures[index].id; }
    glm::vec3 structurePos(int index) const { return m_structures[index].pos; }
    glm::vec3 structureLabelAnchor(int index) const; // world point just above the structure's top
    EStructureType structureType(int index) const { return m_structures[index].type; }
    float structureHealth(int index) const { return m_structures[index].health; }
    float structureHealthMax() const { return m_structureHealthMax; }
    bool structurePowered(int index) const { return m_structures[index].powered; }
    bool cableExists(uint32 idA, uint32 idB) const;
    // Valid NEW cable: distinct live endpoints within cable length (any structure types).
    bool cableAllowed(int indexA, int indexB) const;
    int cableCount() const { return (int)m_cables.size(); }

    float minerals() const { return m_minerals; }
    float fuel() const { return m_fuelTotal; } // total across all grid tanks (HUD)
    float gridEnergy() const { return m_gridEnergyTotal; }           // stored across all grids
    float gridEnergyCapacity() const { return m_gridCapacityTotal; } // batteries + Base storage
    float energyGenPerSec() const { return m_genRateTotal; }         // running generators' output
    float energyUsePerSec() const { return m_useRateTotal; }         // powered consumers' draw
    float structureCharge(int index) const { return m_structures[index].charge; }
    float structureCapacity(int index) const
    {
        const EStructureType t = m_structures[index].type;
        return t == EStructureType::Battery || t == EStructureType::Base ? m_batteryCapacity : 0.0f;
    }
    float structureFuel(int index) const { return m_structures[index].fuel; }
    float structureFuelCapacity(int index) const { return fuelCapacityOf(m_structures[index].type); }
    float fuelCapacityOf(EStructureType t) const
    {
        if (t == EStructureType::FuelTank)
            return m_fuelTankCapacity;
        return t == EStructureType::Generator || t == EStructureType::Base ? m_generatorFuelTank : 0.0f;
    }
    float mineralCost(EStructureType type) const { return m_costs[(int)type]; }
    int affordableCount(EStructureType type) const
    {
        const float cost = m_costs[(int)type];
        return cost > 0.0f ? (int)(m_minerals / cost) : 0;
    }
    float placeRange() const { return m_placeRange; }
    float cableRange() const { return m_cableRange; }
    float emitterFieldReach() const { return m_emitterReach; }
    float emitterFieldOutput() const { return m_emitterOutput; }

    // World-field suppression driver: sum over POWERED emitter pylons of a linear proximity weight
    // (1 at the objective, 0 at `radius`). Deliberately NOT the world emitter's getPressure() — its
    // 13-sample integral over a map-sized bubble is too sparse to see 12 m pylon bubbles reliably.
    float suppressionAt(const glm::vec3& objective, float radius) const
    {
        float sum = 0.0f;
        for (const Structure& s : m_structures)
            if (s.type == EStructureType::Emitter && s.powered)
                sum += glm::max(0.0f, 1.0f - glm::distance(glm::vec2(s.pos.x, s.pos.z),
                    glm::vec2(objective.x, objective.z)) / glm::max(radius, 1.0f));
        return sum;
    }

private:
    struct Node
    {
        EntityPtr entity;
        ENodeType type = ENodeType::Mineral;
        glm::vec3 pos{ 0.0f };
        bool extracted = false; // an extractor stands on this node
    };
    struct Structure
    {
        EntityPtr entity;
        ForceQuery query; // territory at the structure: enemy-owned = taking damage
        EStructureType type = EStructureType::Emitter;
        glm::vec3 pos{ 0.0f };
        float health = 100.0f;
        float charge = 0.0f; // stored energy (Battery/Base only) — rides the structure through
                             // grid merges/splits and dies with it
        float fuel = 0.0f;   // stored fuel (Generator/Base tanks only) — same contract as charge
        bool powered = false;
        uint16 gridId = 0;
        int nodeIndex = -1; // Extractor: the node it harvests (node indices are stable)
        uint32 id = 0;      // stable id — cables and UI selections survive vector erases
    };
    struct PlaceRequest
    {
        EStructureType type;
        glm::vec3 pos;
        int nodeIndex = -1;
    };
    struct Cable
    {
        uint32 idA = 0, idB = 0; // unordered pair of structure ids
    };

    void placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex);
    void applyCableRequest(uint32 idA, uint32 idB);
    void applyDemolishRequest(uint32 id);
    void destroyStructureAt(size_t index); // frees the node, removes the entity, erases
    void rebuildGrids(); // prune dead cables, union-find over the live ones, refill m_links
    void tickPower(float deltaSec);
    void tickDamage(float deltaSec);

    std::vector<Node> m_nodes;
    std::vector<Structure> m_structures; // placement order = power priority within a grid
    std::vector<PlaceRequest> m_requests;
    std::vector<Cable> m_cableRequests;
    std::vector<uint32> m_demolishRequests;
    std::vector<Cable> m_cables;
    std::vector<std::pair<uint16, uint16>> m_links; // index pairs of live cables (debug draw)
    uint32 m_nextStructureId = 1; // 0 = invalid
    float m_minerals = 0.0f;
    float m_fuelTotal = 0.0f;         // totals across grids, for the HUD
    float m_gridEnergyTotal = 0.0f;
    float m_gridCapacityTotal = 0.0f;
    float m_genRateTotal = 0.0f;
    float m_useRateTotal = 0.0f;

    // Tweaks ("Game/Economy", "Game/Structures")
    float m_startMinerals = 100.0f;
    float m_startFuel = 60.0f;
    float m_extractorSnapRadius = 5.0f; // aim within this of a free node snaps the extractor onto it
    float m_mineralRate = 5.0f;      // minerals/s per powered mineral extractor
    float m_fuelRate = 5.0f;         // fuel/s per powered fuel extractor
    float m_costs[6] = { 30.0f, 50.0f, 15.0f, 40.0f, 40.0f, 25.0f }; // Emitter, Generator, Transmitter, Extractor, Battery, FuelTank
    float m_fuelBurnRate = 1.0f;        // fuel/s per RUNNING generator
    float m_genEnergyPerSec = 10.0f;    // energy/s a running generator adds to its grid
    float m_emitterEnergyPerSec = 2.0f; // energy/s an emitter drains
    float m_extractorEnergyPerSec = 1.0f;
    float m_batteryCapacity = 100.0f;   // storage per Battery (the Base stores the same)
    float m_generatorFuelTank = 50.0f;  // fuel storage per Generator (the Base holds one tank too)
    float m_fuelTankCapacity = 150.0f;  // fuel storage per dedicated Fuel tank structure
    float m_cableRange = 18.0f;      // max cable length
    float m_placeRange = 20.0f;
    float m_emitterOutput = 1.2f;    // powered pylon field strength
    float m_emitterReach = 12.0f;    // keep under Force "Big reach threshold" (48)
    float m_structureHealthMax = 100.0f;
    float m_structureDamageRate = 8.0f; // health/s in enemy-owned territory
};
