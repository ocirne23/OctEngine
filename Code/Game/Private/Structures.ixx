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
//  - ENERGY is a FLOW NETWORK over MANUAL CABLES (no auto-linking; any two structures within
//    cable range connect): EVERY structure holds LOCAL charge (consumers/transmitters a small
//    internal buffer, generators a production buffer, batteries/Base big storage). Each tick,
//    every cable moves energy toward EQUAL FILL FRACTIONS of its two endpoints, capped by the
//    cable TYPE's throughput (Basic/Heavy) — energy propagates hop by hop, so long thin lines
//    starve and local generation + storage wins. Generators produce into their OWN buffer only
//    while it has space (a full buffer = export-limited = no fuel burn); consumers drain their
//    internal battery and go unpowered when it empties. Charge dies with its structure.
//  - FUEL stays grid-pooled (union-find over cables): tanks on generators/fuel tanks/Base, fed by
//    powered fuel extractors of the same grid, burned by that grid's generators.
//  - Every structure has health and drains while standing in ENEMY-owned territory (per-structure
//    ForceQuery); a friendly bubble overhead protects. Dead structures despawn (a dead extractor
//    frees its node; cables to a dead structure are pruned).
// Authority tick = main thread; this is the future server-side seam (queuePlaceRequest/
// queueCableRequest become client->server events validated before applying).
//  - The BASE exists from game start (spawnBase, never placeable): respawn anchor, always-on
//    emitter (self-powered), passive income equal to one extractor of EACH type, cable-connectable
//    like a building, and is invulnerable (a dead base would soft-lock the respawn).
export enum class EStructureType : uint8 { Emitter, Generator, Transmitter, Extractor, Battery, FuelTank, Solar, Fabricator, Base, Count };
export constexpr int NumPlaceableStructures = 8; // everything but the Base (spawned, never placed)

export enum class ENodeType : uint8 { Mineral, Fuel };
export enum class ECableType : uint8 { Basic, Heavy, Count };

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
    // pair validates (cableAllowed), removes it when it already exists with the SAME type, and
    // RETYPES it when it exists with a different type (upgrade/downgrade in place).
    void queueCableRequest(uint32 idA, uint32 idB, ECableType type);
    // Delete mode: removes the structure (no refund). The Base is refused at apply time.
    void queueDemolishRequest(uint32 id);
    // NPC attack surface: a random pick among the 4 CLOSEST damageable structures to nearPos
    // (Base excluded; 0 = none exist) — units harass their neighbourhood instead of marching
    // across the map — and direct damage by stable id (main thread; death cleanup in the next
    // tickDamage sweep).
    uint32 randomTargetStructureId(const glm::vec3& nearPos) const;
    void damageStructure(uint32 id, float amount);
    // Enemy units pressing on emitter bubbles cost energy: deposits the FULL energyPerSec onto the
    // NEAREST ACTIVE emitter within `radius` (flat — in range is in range) — a unit physically
    // leans on one bubble, and dark emitters (no bubble) are pressed by nothing. The next tickPower
    // adds it to that emitter's draw. A CPU term on purpose — the 13-sample pressure integral is
    // too sparse to feel small unit bubbles reliably.
    void addEmitterLoad(const glm::vec3& pos, float radius, float energyPerSec)
    {
        int best = -1;
        float bestDist = radius;
        for (int i = 0; i < (int)m_structures.size(); ++i)
        {
            const Structure& s = m_structures[i];
            if (s.type != EStructureType::Emitter || s.outputFrac <= 0.05f)
                continue; // only a live bubble takes the strain
            const float d = glm::distance(glm::vec2(s.pos.x, s.pos.z), glm::vec2(pos.x, pos.z));
            if (d < bestDist)
            {
                bestDist = d;
                best = i;
            }
        }
        if (best >= 0)
            m_structures[best].unitLoad += energyPerSec;
    }
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
    int cableCount(ECableType type) const
    {
        int n = 0;
        for (const Cable& c : m_cables)
            if (c.type == type)
                ++n;
        return n;
    }

    float minerals() const { return m_minerals; }
    float fuel() const { return m_fuelTotal; } // total across all grid tanks (HUD)
    float gridEnergy() const { return m_gridEnergyTotal; }           // stored across all grids
    float gridEnergyCapacity() const { return m_gridCapacityTotal; } // batteries + Base storage
    float energyGenPerSec() const { return m_genRateTotal; }         // running generators' output
    float energyUsePerSec() const { return m_useRateTotal; }         // powered consumers' draw
    float structureCharge(int index) const { return m_structures[index].charge; }
    float structureCapacity(int index) const { return energyCapacityOf(m_structures[index].type); }
    // Every structure stores energy locally now: consumers/transmitters a small internal buffer,
    // generators a production buffer, batteries/Base the big storage.
    float energyCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Emitter:
        case EStructureType::Extractor:
        case EStructureType::Transmitter:
        case EStructureType::Solar:
        case EStructureType::Fabricator:  return m_internalBuffer;
        case EStructureType::Generator:   return m_generatorBuffer;
        case EStructureType::Battery:
        case EStructureType::Base:        return m_batteryCapacity;
        default:                          return 0.0f;
        }
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
        float outputFrac = 0.0f;      // Emitter: smoothed output fraction — the bubble shrinks over
                                      // "Emitter shrink time" on power loss, regrows on restart
        float unitLoad = 0.0f;        // Emitter: extra energy/s deposited by enemy units pressing
                                      // on it (addEmitterLoad) — consumed and cleared by tickPower
        bool emitterDown = false;     // Emitter: latched off until the internal battery refills to
                                      // "Emitter restart charge" (no per-tick flicker at empty)
        bool lastHitByAttack = false; // diagnostic: which source landed the last damage (log on death)
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
        ECableType type = ECableType::Basic;
        float lastFlowRate = 0.0f; // energy/s moved last tick, signed A->B (drives the flow visual)
    };
    struct Link // per-tick resolved cable (indices valid until the next structure erase)
    {
        uint16 a = 0, b = 0;
        uint16 cableIndex = 0;
    };

    void placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex);
    void applyCableRequest(uint32 idA, uint32 idB, ECableType type);
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
    std::vector<Link> m_links; // per-tick resolved cables (flow sim + debug draw)
    float m_time = 0.0f;       // drives the flow-pulse visual
    uint32 m_nextStructureId = 1; // 0 = invalid
    float m_minerals = 0.0f;
    float m_fuelTotal = 0.0f;         // totals across grids, for the HUD
    bool m_wasFuelDry = false;        // edge detector for the fuel-collapse log
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
    float m_costs[8] = { 30.0f, 50.0f, 15.0f, 40.0f, 40.0f, 25.0f, 20.0f, 60.0f }; // Emitter, Generator,
                        // Transmitter, Extractor, Battery, FuelTank, Solar, Fabricator
    float m_fuelBurnRate = 1.0f;        // fuel/s per RUNNING generator
    float m_genEnergyPerSec = 10.0f;    // energy/s a running generator adds to its grid
    float m_solarEnergyPerSec = 1.5f;   // energy/s a solar panel trickles into its buffer (no fuel)
    float m_fabricatorEnergyPerSec = 4.0f;  // energy/s a fabricator drains while producing
    float m_fabricatorMineralsPerSec = 2.0f; // minerals/s a POWERED fabricator produces
    float m_emitterEnergyPerSec = 1.0f; // energy/s an emitter drains at rest
    float m_emitterPressureDraw = 3.0f; // EXTRA energy/s per unit of pressure on the emitter's
                                        // field — contested emitters cost more to hold
    float m_extractorEnergyPerSec = 1.0f;
    float m_batteryCapacity = 100.0f;   // storage per Battery (the Base stores the same)
    float m_internalBuffer = 10.0f;     // emitter/extractor/transmitter local energy buffer
    float m_generatorBuffer = 20.0f;    // generator production buffer (full = production throttles)
    float m_cableThroughput[2] = { 5.0f, 20.0f }; // energy/s per cable type (Basic, Heavy)
    float m_generatorFuelTank = 50.0f;  // fuel storage per Generator (the Base holds one tank too)
    float m_fuelTankCapacity = 150.0f;  // fuel storage per dedicated Fuel tank structure
    float m_cableRange = 18.0f;      // max cable length
    float m_placeRange = 20.0f;
    float m_emitterOutput = 2.0f;    // powered pylon field strength
    float m_emitterReach = 25.0f;    // keep under Force "Big reach threshold" (48)
    float m_emitterShrinkTime = 1.5f; // seconds for a starved emitter's bubble to fade out
    float m_emitterGrowTime = 0.5f;   // seconds to regrow after a restart
    float m_emitterRestartCharge = 5.0f; // internal battery level that clears the down-latch
    float m_structureHealthMax = 100.0f;
    float m_structureDamageRate = 8.0f; // health/s in enemy-owned territory
};
