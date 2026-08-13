export module Game:Structures;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import Force;

// The build/economy layer: resource nodes, the Minerals/Fuel stocks, the placeable structures and
// the power grids connecting them.
//  - A node produces nothing on its own: an EXTRACTOR must be built on it (one per node) and fed
//    energy. Powered mineral extractors accrue Minerals (global — the build currency). FUEL IS A
//    FLOW NETWORK like energy: powered fuel extractors fill their OWN small tank (full tank =
//    export-limited = production stalls), PIPELINE cables move fuel toward equal fill fractions
//    (capped by "Pipeline throughput"), and generators/fabricators burn from their OWN tanks.
//    Energy cables and pipes are separate media — a pipe carries no energy and vice versa.
//  - ENERGY is a FLOW NETWORK over MANUAL CABLES (no auto-linking; any two structures within
//    cable range connect): EVERY structure holds LOCAL charge (consumers/transmitters a small
//    internal buffer, generators a production buffer, batteries/Base big storage). Each tick,
//    every cable moves energy toward EQUAL FILL FRACTIONS of its two endpoints, capped by the
//    cable TYPE's throughput (Basic/Heavy) — energy propagates hop by hop, so long thin lines
//    starve and local generation + storage wins. Generators produce into their OWN buffer only
//    while it has space (a full buffer = export-limited = no fuel burn); consumers drain their
//    internal battery and go unpowered when it empties. Charge dies with its structure.
//  - Every structure has health and drains while standing in ENEMY-owned territory (per-structure
//    ForceQuery); a friendly bubble overhead protects. Dead structures despawn (a dead extractor
//    frees its node; cables to a dead structure are pruned).
// Authority tick = main thread; this is the future server-side seam (queuePlaceRequest/
// queueCableRequest become client->server events validated before applying).
//  - The BASE exists from game start (spawnBase, never placeable): respawn anchor, always-on
//    emitter (self-powered), passive income equal to one extractor of EACH type, cable-connectable
//    like a building, and is invulnerable (a dead base would soft-lock the respawn).
export enum class EStructureType : uint8 { Emitter, Generator, Connector, Extractor, Battery, FuelTank, Solar, Fabricator, Bastion, Lance, Barracks, BarracksBrute, BarracksRunner, BarracksSpitter, Wall, Turret, MineralSilo, Constructor, Base, Count };
export constexpr int NumPlaceableStructures = 18; // everything but the Base (spawned, never placed)

// One barracks variant per spawnable unit type (Grunt/Brute/Runner/Spitter — NpcSystem maps them).
export constexpr bool isBarracksType(EStructureType t)
{
    return t == EStructureType::Barracks || t == EStructureType::BarracksBrute
        || t == EStructureType::BarracksRunner || t == EStructureType::BarracksSpitter;
}

// The three emitter variants: Emitter = balanced sphere, Bastion = big expensive anchor bubble,
// Lance = focused cone auto-facing AWAY from the Base (a directional push into the enemy field).
export constexpr bool isEmitterType(EStructureType t)
{
    return t == EStructureType::Emitter || t == EStructureType::Bastion || t == EStructureType::Lance;
}

export enum class ENodeType : uint8 { Mineral, Fuel };
// Basic/Heavy carry ENERGY, Pipe carries FUEL, Conveyor carries MINERALS (extractor buffers ->
// silos/Base, where they become spendable). One link type per medium; a link's medium decides
// which per-structure store it equalizes.
export enum class ECableType : uint8 { Basic, Heavy, Pipe, Conveyor, Count };
export constexpr int cableMedium(ECableType t) // 0 = energy, 1 = fuel, 2 = minerals
{
    return t == ECableType::Pipe ? 1 : t == ECableType::Conveyor ? 2 : 0;
}

// PvP: structures/resources are per Force team (server = 0, client N = min(N, GameMaxTeams-1)).
// Single player / co-op leaves everything on team 0.
export constexpr int GameMaxTeams = 8;

export const char* structureTypeName(EStructureType type);

export class StructureSystem final
{
public:
    // CO-OP: the server runs the real sim and notifies through the hooks below; clients run a
    // MIRROR — no tick logic, structures/cables applied from the server's events (mirror* calls),
    // volatile state (health/charge/fuel/outputFrac/powered + the resource totals) refreshed by
    // the periodic stat sync. Mirrored emitters drive their ForceComponent from the synced
    // outputFrac, so the FIELDS are correct client-side (the client player's shield readbacks
    // press against real local fields).
    std::function<void(int index)> onStructurePlaced;                       // server -> send GPl
    std::function<void(uint32 id)> onStructureRemoved;                      // server -> send GRm
    std::function<void(uint32, uint32, ECableType, bool removed)> onCableChanged; // server -> GCb

    void mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos, const glm::vec2& facingXZ,
        int nodeIndex, uint8 team, bool built);
    void mirrorRemove(uint32 id);
    void mirrorCable(uint32 idA, uint32 idB, ECableType type, bool removed);
    void mirrorStructureState(uint32 id, float healthFrac, float chargeFrac, float fuelFrac,
        float mineralFrac, float outputFrac, float utilFrac, bool powered, bool blueprint);
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
    // Client per-frame: resolves the cable draw list + eases emitter outputs toward the 5 Hz
    // synced targets (stepping them straight in made shrink/grow ramps stutter).
    void tickMirror(float deltaSec);

    // Join replay iteration (server): every cable as id pairs.
    int cableTotal() const { return (int)m_cables.size(); }
    void cableAt(int i, uint32& idA, uint32& idB, ECableType& type) const
    {
        idA = m_cables[i].idA;
        idB = m_cables[i].idB;
        type = m_cables[i].type;
    }
    glm::vec2 structureFacing(int index) const { return m_structures[index].facingXZ; }
    int structureNodeIndex(int index) const { return m_structures[index].nodeIndex; }
    float structureOutputFrac(int index) const { return m_structures[index].outputFrac; }

    // SAVE/LOAD (server): every structure (id/type/pos/facing/node/team/blueprint + stores +
    // route) and cable into/from an AssetNode tree. loadFrom CLEARS the current set first
    // (removal hooks fire, so connected clients prune) and preserves ids.
    void saveTo(AssetNode& root) const;
    void loadFrom(const AssetNode& root);
    void clearAllStructures();

    void registerTweaks();
    // Authored whitebox layout (same on every instance — no sync needed): the corridor arena's
    // symmetric node set (starters at each end, contested middle).
    void spawnNodes();
    void spawnBase(const glm::vec3& groundPos, uint8 team = 0); // game-start Base (free; PvP spawns
                                                                // one per playing team)
    void clear();      // drops every entity/query/emitter handle (before world teardown)

    // nodeIndex: Extractor only — the node it builds on (from findFreeNodeNear); -1 otherwise.
    // facing: planar direction override (Lance's aimed second click); zero = auto (away from Base).
    // team: the requester's team — pays that team's minerals, owns the structure's fields.
    void queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, int nodeIndex = -1,
        const glm::vec3& facing = glm::vec3(0.0f), uint8 team = 0);
    // Cable tool: toggles the cable between two structures (by STABLE id) — creates it when the
    // pair validates (cableAllowed), removes it when it already exists with the SAME type, and
    // RETYPES it when it exists with a different type (upgrade/downgrade in place). Both endpoints
    // must belong to the requester's team — grids never bridge teams.
    void queueCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team = 0);
    // Delete mode: removes the structure (no refund). The Base and other teams' structures are
    // refused at apply time.
    void queueDemolishRequest(uint32 id, uint8 team = 0);
    // BARRACKS ROUTES: replace the barracks' waypoint list (validated at apply: own-team barracks
    // only, clamped to MaxRouteWaypoints). Spawned units walk the route, advancing when they touch
    // each waypoint's "destination circle" (waypointRadius); combat AI resumes after the last.
    static constexpr int MaxRouteWaypoints = 6; // also bounds the GqW request under the 64B cap
    void queueRouteRequest(uint32 id, std::span<const glm::vec3> points, uint8 team = 0);
    std::function<void(uint32 id)> onRouteChanged; // server -> send GRt (mirror + join replay)
    void mirrorRoute(uint32 id, std::span<const glm::vec3> points);
    const std::vector<glm::vec3>& structureRoute(int index) const { return m_structures[index].route; }
    const std::vector<glm::vec3>* structureRouteById(uint32 id) const
    {
        const int index = structureIndexById(id);
        return index >= 0 ? &m_structures[index].route : nullptr;
    }
    float waypointRadius() const { return m_waypointRadius; }
    // SMART CONNECT: the link type to auto-create between two structures — a Connector's already-
    // carried medium wins, else A's (the selected building's) resource OUTPUT, else B's, else the
    // first medium BOTH hold. Count = no sensible/valid link.
    ECableType smartLinkTypeFor(int indexA, int indexB) const;
    // NPC attack surface: a random pick among the 4 CLOSEST damageable structures to nearPos
    // (Base excluded; 0 = none exist) — units harass their neighbourhood instead of marching
    // across the map — and direct damage by stable id (main thread; death cleanup in the next
    // tickDamage sweep).
    // attackerTeam: structures of that team are never candidates (PvE units pass 1 — every player
    // structure qualifies; PvP units pass their own team so they only hunt enemy buildings).
    uint32 randomTargetStructureId(const glm::vec3& nearPos, uint8 attackerTeam) const;
    void damageStructure(uint32 id, float amount);
    // Spends `energy` from a structure's internal store if present — barracks spawns and turret
    // shots pay through this, so both are gated by the supply network.
    bool trySpendEnergy(uint32 id, float energy)
    {
        const int index = structureIndexById(id);
        if (index < 0 || m_structures[index].charge < energy)
            return false;
        m_structures[index].charge -= energy;
        return true;
    }
    // Extra spawn speed on a barracks from idle Constructors in range (0 = none, 1 = double speed
    // ...): the Npc barracks timers tick at (1 + boost). Rebuilt every authority tick.
    float barracksSpawnBoost(uint32 barracksId) const
    {
        const auto it = m_barracksBoost.find(barracksId);
        return it != m_barracksBoost.end() ? it->second : 0.0f;
    }
    // Same contract for the mineral store — barracks pay MATERIALS per spawned unit.
    bool trySpendStoredMinerals(uint32 id, float amount)
    {
        const int index = structureIndexById(id);
        if (index < 0 || m_structures[index].mineralStore < amount)
            return false;
        m_structures[index].mineralStore -= amount;
        return true;
    }

    // Enemy units pressing on emitter bubbles cost energy: deposits the FULL energyPerSec onto the
    // NEAREST ACTIVE emitter within `radius` (flat — in range is in range) — a unit physically
    // leans on one bubble, and dark emitters (no bubble) are pressed by nothing. The next tickPower
    // adds it to that emitter's draw. A CPU term on purpose — the 13-sample pressure integral is
    // too sparse to feel small unit bubbles reliably.
    void addEmitterLoad(const glm::vec3& pos, float radius, float energyPerSec, uint8 attackerTeam = 1)
    {
        int best = -1;
        float bestDist = radius;
        for (int i = 0; i < (int)m_structures.size(); ++i)
        {
            const Structure& s = m_structures[i];
            if (!isEmitterType(s.type) || s.outputFrac <= 0.05f)
                continue; // only a live bubble takes the strain
            if (s.team == attackerTeam)
                continue; // own-team bubbles are never strained (PvP)
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
    // Pointer-value comparison only (safe on a stale pointer): resolves contact-hook victims.
    int structureIndexByEntity(const Entity* entity) const
    {
        for (int i = 0; i < (int)m_structures.size(); ++i)
            if (m_structures[i].entity.get() == entity)
                return i;
        return -1;
    }
    int structureCount() const { return (int)m_structures.size(); }
    uint32 structureId(int index) const { return m_structures[index].id; }
    glm::vec3 structurePos(int index) const { return m_structures[index].pos; }
    glm::vec3 structureLabelAnchor(int index) const; // world point just above the structure's top
    EStructureType structureType(int index) const { return m_structures[index].type; }
    float structureHealth(int index) const { return m_structures[index].health; }
    float structureHealthMax() const { return m_structureHealthMax; }
    bool structurePowered(int index) const { return m_structures[index].powered; }
    bool cableExists(uint32 idA, uint32 idB) const;
    // The existing link's type between a pair (Count = none) — the Disconnect/Upgrade tools.
    ECableType cableTypeBetween(uint32 idA, uint32 idB) const
    {
        for (const Cable& c : m_cables)
            if ((c.idA == idA && c.idB == idB) || (c.idA == idB && c.idB == idA))
                return c.type;
        return ECableType::Count;
    }
    // Valid NEW link of `type`: distinct live endpoints within cable length, AT LEAST ONE endpoint
    // a Connector (links never run building-to-building), and every Connector endpoint carries
    // only ONE medium (power / fuel / minerals) across all its links. ignoreCableIndex skips one
    // existing cable in the medium check (the retype-in-place path re-validates itself).
    bool cableAllowed(int indexA, int indexB, ECableType type, int ignoreCableIndex = -1) const;
    int cableCount(ECableType type) const
    {
        int n = 0;
        for (const Cable& c : m_cables)
            if (c.type == type)
                ++n;
        return n;
    }

    // SPENDABLE minerals: the sum of the team's Silo + Base mineral STORES (recomputed each
    // authority tick; mirror-set on clients). Extractor/fabricator buffers do NOT count — a
    // Conveyor link has to bring the minerals home first.
    float minerals(uint8 team = 0) const { return m_minerals[glm::min((int)team, GameMaxTeams - 1)]; }
    float fuel(uint8 team = 0) const { return m_fuelTotal[glm::min((int)team, GameMaxTeams - 1)]; } // that team's tanks (HUD)
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
        case EStructureType::Bastion:
        case EStructureType::Lance:
        case EStructureType::Extractor:
        case EStructureType::Connector:
        case EStructureType::Solar:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:
        case EStructureType::Turret:      return m_internalBuffer;
        // Barracks hold NO energy: they run purely on conveyor-fed minerals, so an energy buffer
        // would only show an idle bar and let a useless power cable attach.
        case EStructureType::Generator:   return m_generatorBuffer;
        case EStructureType::Battery:     return m_batteryCapacity; // the Base stores NO energy
        default:                          return 0.0f;
        }
    }
    float structureFuel(int index) const { return m_structures[index].fuel; }
    float structureFuelCapacity(int index) const { return fuelCapacityOf(m_structures[index].type); }
    // Connector gauge: EMA-smoothed peak utilization of the links touching it (computed in
    // tickPower; the raw per-tick equalizing flow jitters). GSt-synced, so clients read it too.
    float connectorUtilization(int index) const { return m_structures[index].flowUtil; }
    int connectorMedium(int index) const
    {
        const uint32 id = m_structures[index].id;
        for (const Cable& c : m_cables)
            if (c.idA == id || c.idB == id)
                return cableMedium(c.type);
        return -1;
    }
    float structureMinerals(int index) const { return m_structures[index].mineralStore; }
    float structureMineralCapacity(int index) const { return mineralCapacityOf(m_structures[index].type); }
    // Per-structure mineral storage: extractors/fabricators a small output buffer (full = stalls),
    // silos and the Base the big stores that make minerals spendable, connectors a relay buffer.
    float mineralCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::Connector:
        case EStructureType::Barracks:     // conveyor-fed: units are SPAWNED from materials
        case EStructureType::BarracksBrute:
        case EStructureType::BarracksRunner:
        case EStructureType::BarracksSpitter:
        case EStructureType::Constructor:  return m_internalBuffer; // conveyor-fed build stock
        case EStructureType::MineralSilo:  return m_mineralSiloCapacity;
        case EStructureType::Base:         return m_mineralBaseCapacity;
        default:                           return 0.0f;
        }
    }
    float fuelCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::FuelTank:   return m_fuelTankCapacity;
        case EStructureType::Generator:  return m_generatorFuelTank; // the Base stores NO fuel
        case EStructureType::Extractor:  // fuel extractors export from here; mineral ones never fill it
        case EStructureType::Fabricator:
        case EStructureType::Connector:  return m_internalBuffer; // flows RELAY through connectors,
                                         // so they need a small store in every medium they can carry
        default:                         return 0.0f;
        }
    }
    float mineralCost(EStructureType type) const { return m_costs[(int)type]; }
    int affordableCount(EStructureType type, uint8 team = 0) const
    {
        const float cost = m_costs[(int)type];
        return cost > 0.0f ? (int)(minerals(team) / cost) : 0;
    }
    uint8 structureTeam(int index) const { return m_structures[index].team; }
    bool structureBlueprint(int index) const { return m_structures[index].blueprint; }

    // ---- GRID PLACEMENT ----------------------------------------------------------------------
    // Buildings snap to a world-aligned grid and occupy footprint x footprint CELLS; occupied
    // cells refuse new placements. Odd footprints center ON a cell, even ones on a cell corner.
    static constexpr float GridCellSize = 2.0f;
    static int footprintCellsOf(EStructureType t)
    {
        switch (t)
        {
        case EStructureType::FuelTank:
        case EStructureType::Bastion:
        case EStructureType::Turret:
        case EStructureType::Solar:      // flat panel: 4x4 m but only 2 m tall
        case EStructureType::Extractor:
        case EStructureType::Fabricator:
        case EStructureType::MineralSilo: return 2; // 4 m boxes
        case EStructureType::Barracks:
        case EStructureType::BarracksBrute:
        case EStructureType::BarracksRunner:
        case EStructureType::BarracksSpitter:
        case EStructureType::Base:        return 3; // 6 m boxes
        default:                          return 1;
        }
    }
    static glm::vec3 snapToGrid(EStructureType t, const glm::vec3& groundPos)
    {
        const bool odd = (footprintCellsOf(t) & 1) != 0;
        const auto snapAxis = [odd](float v)
        {
            return odd ? (std::floor(v / GridCellSize) + 0.5f) * GridCellSize
                       : std::round(v / GridCellSize) * GridCellSize;
        };
        return glm::vec3(snapAxis(groundPos.x), 0.0f, snapAxis(groundPos.z));
    }
    // Optional placement bounds (the PvP corridor interior): footprints must lie fully inside —
    // this is what keeps buildings out of the arena border walls (plain entities, not structures).
    void setPlacementBounds(const glm::vec2& minXZ, const glm::vec2& maxXZ)
    {
        m_boundsMin = minXZ;
        m_boundsMax = maxXZ;
        m_hasBounds = true;
    }
    // Free = inside the bounds AND the footprint overlaps no existing structure's (blueprints
    // included) AND no free resource node's RESERVED extractor slot — a building can neither
    // block an extractor's future spot nor overlap one placed later. Also used as a probe for
    // unit spawn points (a 1x1 footprint).
    bool cellsFree(EStructureType t, const glm::vec3& snappedPos) const
    {
        const float half = footprintCellsOf(t) * GridCellSize * 0.5f;
        if (m_hasBounds && (snappedPos.x - half < m_boundsMin.x || snappedPos.x + half > m_boundsMax.x
            || snappedPos.z - half < m_boundsMin.y || snappedPos.z + half > m_boundsMax.y))
            return false;
        for (const Structure& s : m_structures)
        {
            const float limit = half + footprintCellsOf(s.type) * GridCellSize * 0.5f - 0.01f;
            if (glm::abs(s.pos.x - snappedPos.x) < limit && glm::abs(s.pos.z - snappedPos.z) < limit)
                return false;
        }
        if (t != EStructureType::Extractor) // the extractor itself is what the slot is FOR
        {
            const float slotHalf = footprintCellsOf(EStructureType::Extractor) * GridCellSize * 0.5f;
            for (const Node& node : m_nodes)
            {
                if (node.extracted)
                    continue; // the standing extractor already occupies these cells
                const glm::vec3 slot = snapToGrid(EStructureType::Extractor, node.pos);
                const float limit = half + slotHalf - 0.01f;
                if (glm::abs(slot.x - snappedPos.x) < limit && glm::abs(slot.z - snappedPos.z) < limit)
                    return false;
            }
        }
        return true;
    }

    // ---- CONSTRUCTION (server) -------------------------------------------------------------
    // Withdraw up to `amount` stored minerals from the team's Silos/Base within `radius` of pos
    // (the player-inventory refill). Returns what was actually taken.
    float takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount);
    // Invest up to `amount` materials into the NEAREST own-team blueprint within `radius` —
    // includeRepairs additionally accepts DAMAGED built structures (the Constructor's repair role;
    // same materials-per-hp price as construction). Returns what was actually spent.
    float fundNearbyBlueprint(const glm::vec3& pos, float radius, uint8 team, float amount,
        bool includeRepairs = false);
    float placeRange() const { return m_placeRange; }
    float cableRange() const { return m_cableRange; }
    // Per-variant emitter tuning (Emitter / Bastion / Lance).
    float emitterOutputOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionOutput
             : t == EStructureType::Lance ? m_lanceOutput : m_emitterOutput;
    }
    float emitterReachOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionReach
             : t == EStructureType::Lance ? m_lanceReach : m_emitterReach;
    }
    float emitterDrawOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_bastionEnergyPerSec
             : t == EStructureType::Lance ? m_lanceEnergyPerSec : m_emitterEnergyPerSec;
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
        float mineralStore = 0.0f; // stored minerals (extractor/fabricator buffer, silo/Base bank)
        float flowUtil = 0.0f;     // Connector: EMA-smoothed peak link utilization (GSt-synced)
        float outputFracTarget = 0.0f; // CLIENT mirror: the last synced outputFrac — tickMirror
                                       // eases the live fraction toward it (5 Hz steps stutter)
        float outputFrac = 0.0f;      // Emitter: smoothed output fraction — the bubble shrinks over
                                      // "Emitter shrink time" on power loss, regrows on restart
        float unitLoad = 0.0f;        // Emitter: extra energy/s deposited by enemy units pressing
                                      // on it (addEmitterLoad) — consumed and cleared by tickPower
        bool emitterDown = false;     // Emitter: latched off until the internal battery refills to
                                      // "Emitter restart charge" (no per-tick flicker at empty)
        bool lastHitByAttack = false; // diagnostic: which source landed the last damage (log on death)
        bool powered = false;
        int nodeIndex = -1; // Extractor: the node it harvests (node indices are stable)
        uint32 id = 0;      // stable id — cables and UI selections survive vector erases
        glm::vec2 facingXZ{ 0.0f }; // Lance: the aimed facing (kept for the co-op join replay)
        uint8 team = 0;     // owning Force team — fields, income and demolish rights follow it
        bool blueprint = false;     // CONSTRUCTION: placed free as a ghost — inert (no power, no
                                    // fields, no income). HEALTH IS THE PROGRESS: materials heal
                                    // it at cost/healthMax per hp; full health = built. Damage
                                    // during construction literally undoes the work.
        std::vector<glm::vec3> route; // BARRACKS: spawn waypoints — units walk them in order
                                      // before their combat AI takes over (dies with the barracks)
    };
    struct PlaceRequest
    {
        EStructureType type;
        glm::vec3 pos;
        glm::vec3 facing{ 0.0f }; // planar facing override (zero = auto)
        int nodeIndex = -1;
        uint8 team = 0;
    };
    struct Cable
    {
        uint32 idA = 0, idB = 0; // unordered pair of structure ids
        ECableType type = ECableType::Basic;
        float lastFlowRate = 0.0f; // energy/s moved last tick, signed A->B (drives the flow visual)
        uint8 team = 0;            // request-only: the requester's team (validated at apply)
    };
    struct Link // per-tick resolved cable (indices valid until the next structure erase)
    {
        uint16 a = 0, b = 0;
        uint16 cableIndex = 0;
    };

    void placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
        const glm::vec3& facing, uint8 team);
    void spendMinerals(uint8 team, float amount); // drains the team's Silos first, the Base last
    void applyStructureTint(Structure& s);        // blueprint gray vs the prefab's authored color
    float investMaterials(Structure& s, float amount); // heals at cost/healthMax per hp; returns
                                                       // materials spent; full health = built
    void applyCableRequest(uint32 idA, uint32 idB, ECableType type, uint8 team);
    void applyDemolishRequest(uint32 id, uint8 team);
    void destroyStructureAt(size_t index); // frees the node, removes the entity, erases
    void rebuildLinks(); // prune cables whose endpoint died, resolve the rest to index pairs
    void tickPower(float deltaSec);
    void tickDamage(float deltaSec);

    std::vector<Node> m_nodes;
    std::vector<Structure> m_structures; // placement order = power priority within a grid
    std::vector<PlaceRequest> m_requests;
    std::vector<Cable> m_cableRequests;
    std::vector<std::pair<uint32, uint8>> m_demolishRequests; // id + requester team
    struct RouteRequest
    {
        uint32 id = 0;
        uint8 team = 0;
        std::vector<glm::vec3> points;
    };
    std::vector<RouteRequest> m_routeRequests;
    std::vector<Cable> m_cables;
    std::vector<Link> m_links; // per-tick resolved cables (flow sim + debug draw)
    float m_time = 0.0f;       // drives the flow-pulse visual
    glm::vec2 m_boundsMin{ 0.0f }, m_boundsMax{ 0.0f }; // see setPlacementBounds
    bool m_hasBounds = false;
    uint32 m_nextStructureId = 1; // 0 = invalid
    float m_minerals[GameMaxTeams] = {};  // per-team build currency
    float m_fuelTotal[GameMaxTeams] = {}; // per-team tank totals, for the HUD
    bool m_wasFuelDry = false;        // edge detector for the fuel-collapse log
    float m_gridEnergyTotal = 0.0f;
    float m_gridCapacityTotal = 0.0f;
    float m_genRateTotal = 0.0f;
    float m_useRateTotal = 0.0f;

    // Tweaks ("Game/Economy", "Game/Structures")
    float m_startMinerals = 100.0f;
    float m_extractorSnapRadius = 5.0f; // aim within this of a free node snaps the extractor onto it
    float m_mineralRate = 1.0f;      // minerals/s per powered mineral extractor
    float m_fuelRate = 4.0f;         // fuel/s per powered fuel extractor
    float m_baseIncomeMult = 0.25f;  // the Base's passive income as a fraction of an extractor —
                                     // a trickle to bootstrap, not a substitute for map control
    float m_costs[18] = { 
        40.0f,  // Emitter,
        30.0f,  // Generator
        10.0f,  // Connector
        40.0f,  // Extractor
		40.0f,  // Battery
		40.0f,  // FuelTank
		30.0f,  // Solar
		60.0f,  // Fabricator
		70.0f,  // Bastion
		45.0f,  // Lance
		100.0f,  // Barracks
		200.0f,  // BarracksBrute
		75.0f,  // BarracksRunner
		150.0f,  // BarracksSpitter
		5.0f,   // Wall (per segment)
		50.0f,  // Turret
		40.0f,  // MineralSilo
		40.0f };// Constructor
                        // Generator, Connector, Extractor, Battery, FuelTank, Solar, Fabricator,
                        // Bastion, Lance, Barracks(+Brute/Runner/Spitter), Wall (per segment),
                        // Turret, MineralSilo, Constructor
    float m_mineralBaseCapacity = 100.0f; // per-team mineral cap without silos
    float m_mineralSiloCapacity = 100.0f; // extra cap per Mineral silo
    float m_constructorRange = 14.0f;     // Constructor: builds blueprints within this
    float m_constructorBuildRate = 6.0f;  // materials/s a powered Constructor invests
    float m_waypointRadius = 3.0f;            // a route waypoint counts as reached inside this
    float m_constructorBoostRate = 1.0f;      // spawn-speed ADDED to the nearest barracks (1 = 2x)
    float m_constructorBoostMaterials = 2.0f; // materials/s the boost burns
    float m_constructorBoostEnergy = 2.0f;    // energy/s the boost burns (from the internal buffer)
    std::unordered_map<uint32, float> m_barracksBoost; // per-barracks summed boost, rebuilt per tick
    float m_fuelBurnRate = 1.0f;        // fuel/s per RUNNING generator
    float m_genEnergyPerSec = 10.0f;    // energy/s a running generator adds to its grid
    float m_solarEnergyPerSec = 1.5f;   // energy/s a solar panel trickles into its buffer (no fuel)
    float m_fabricatorEnergyPerSec = 5.0f;  // energy/s a fabricator drains while producing
    float m_fabricatorFuelPerSec = 1.0f;    // fuel/s it burns alongside (piped into its own tank)
    float m_fabricatorMineralsPerSec = 0.5f; // minerals/s a POWERED fabricator produces
    float m_emitterEnergyPerSec = 1.0f; // energy/s an emitter drains at rest
    float m_emitterPressureDraw = 3.0f; // EXTRA energy/s per unit of pressure on the emitter's
                                        // field — contested emitters cost more to hold
    float m_pressureDrawTension = 1.0f; // surcharge tension: draw scales by (1 + tension * pressure)
                                        // — the defender's half of the surface-tension rule
    float m_extractorEnergyPerSec = 1.0f;
    float m_batteryCapacity = 100.0f;   // storage per Battery (the Base stores the same)
    float m_internalBuffer = 10.0f;     // emitter/extractor/transmitter local energy buffer
    float m_generatorBuffer = 20.0f;    // generator production buffer (full = production throttles)
    float m_cableThroughput[4] = { 5.0f, 20.0f, 4.0f, 6.0f }; // per-type transfer/s: Basic + Heavy
                                                              // carry ENERGY, Pipe FUEL, Conveyor MINERALS
    float m_generatorFuelTank = 10.0f;  // fuel storage per Generator (the Base holds one tank too)
    float m_fuelTankCapacity = 100.0f;  // fuel storage per dedicated Fuel tank structure
    float m_cableRange = 18.0f;      // max building-to-building link length
    float m_connectorRange = 32.0f;  // max link length when either endpoint is a Connector
    float m_placeRange = 20.0f;
    float m_emitterOutput = 2.0f;    // powered pylon field strength
    float m_emitterReach = 25.0f;    // keep under Force "Big reach threshold" (48)
    float m_bastionOutput = 3.2f;    // the anchor variant: bigger, stronger, hungry
    float m_bastionReach = 36.0f;
    float m_bastionEnergyPerSec = 5.0f;
    float m_lanceOutput = 0.08f;     // the push variant: focused cone — its Width/Focus DENSIFY a
                                     // conserved total ~25x+, so this is deliberately tiny (peak
                                     // density lands ~2x an Emitter's)
    float m_lanceReach = 40.0f;
    float m_lanceEnergyPerSec = 3.0f;
    float m_emitterShrinkTime = 1.5f; // seconds for a starved emitter's bubble to fade out
    float m_emitterGrowTime = 0.5f;   // seconds to regrow after a restart
    float m_emitterRestartCharge = 5.0f; // internal battery level that clears the down-latch
    float m_structureHealthMax = 100.0f;
    float m_structureDamageRate = 8.0f; // health/s in enemy-owned territory
    float m_projectileStructDamage = 20.0f; // health per enemy-team projectile hit
    bool m_cheatInstantBuild = false; // "Game/Cheats/Free instant build": placements spawn BUILT
                                      // at full health — no blueprint, no material investment
};
