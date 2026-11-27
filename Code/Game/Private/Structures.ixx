export module Game:Structures;

import Core;
import Core.glm;
import Entity;
import File; // AssetNode (save/load)
import Force;
import Physics; // the gangways' colliders
import Threading; // the transport job
import Settings;
import :Ship; // the deck's tiles (the ship's structures)
export import :StructureTypes; // EStructureType + the type predicates, GameMaxTeams, ENodeType

static_assert((int)EStructureType::Count == GameStructureTypeCount); // the Settings.Game cost table

// The build/economy layer: every structure is an ENTITY whose GameStructureComponent holds its
// identity (stable id, team, health, blueprint) and its three float resource stores. Resources
// move between structures over the CABLE TRANSPORT this system owns (whole cells per segment, a
// fixed-rate job - see the block below and Transport.cpp). This system is the seam around them:
// placement (grid/nodes/requests - the MP validation point), the network rebuild, production
// (income, fuel burn, consumer drain, emitter ramps - over the roster), the death sweep, per-team
// totals, mirrors and save/load. The bodies are split over six implementation units by topic -
// the list is at the end of this file. The tuning is Globals::settings.game.structures (m_settings).

export const char* structureTypeName(EStructureType type);

export class StructureSystem final
{
public:
    StructureSystem();  // attaches the transport-tick listeners
    ~StructureSystem(); // drops them
    StructureSystem(const StructureSystem&) = delete;
    StructureSystem& operator=(const StructureSystem&) = delete;

    // One ROSTER entry (index positions are valid for THIS frame only - removals reindex; anything
    // persistent goes by the stable id on the component). `owner` is an OWNING ref: the raw
    // pointers can never dangle, and every way an entity leaves the world funnels through
    // World::removeRootEntity, whose callback deregisters the entry (see onWorldRootRemoved) - so
    // no per-frame world query is needed to revalidate the roster.
    struct Ref
    {
        EntityPtr owner;
        Entity* entity = nullptr;
        GameStructureComponent* state = nullptr;
        EStructureType type = EStructureType::Emitter;
        float yield = 1.0f; // Lumberyard / Quarry: its share of the full rate (harvestYield at placement, then resolveHarvestYields)
        bool harvestDirty = false; // a yard of its kind came or went: resolveHarvestYields shares its cluster again
        // RUBBLE (setRubble): the pile that shows in place of the building's own meshes (rubble1.pre / rubble2.pre, a
        // root entity of its own), and the pass mask the building's root mesh had before it was hidden. Mutable: the
        // roster hands out const Refs, and this is a visual.
        mutable EntityPtr rubbleVisual;
        mutable uint32 rubbleSavedPassMask = 0;
        // Cable segments: the four render-only arm child entities (+X, -X, +Z, -Z), cached at
        // spawn; rebuildNetworks enables the ones pointing at a connected neighbour. The owning
        // EntityPtr keeps the whole tree alive, so the raw pointers cannot dangle.
        Entity* arms[4] = {};
        // THE AIM HEAD (poseAimHeads, every instance, every frame): the render-only `Head` child that turns on the map - the
        // Searchlight's lamp (to GameStructureComponent::searchlight.yaw) and the Builder's hut's focus crystal (to headYaw).
        // Cached at spawn; the head's local +X is its forward.
        Entity* head = nullptr;
        // THE BUILDER'S HUT: its crystal's heading on the map (rad; direction = cos, 0, sin), turned at "Constructor turn
        // speed (deg/s)" toward buildTarget (the structure it builds or repairs: the authority's pick in tickConstructors, a
        // mirror's own from the mirrored roster; 0 = none). buildWorking: stone goes in now (the stream shows).
        float headYaw = 0.0f;
        uint32 buildTarget = 0;
        bool buildWorking = false;
        // WALKWAY (updateWalkway): the piece a body stands on - walkwayDeck.pre, or walkwayStairs.pre toward a plain
        // road piece. A root entity of its own with the collider, owned by the roster entry; a blueprint has none.
        // walkDir: -2 no piece, -1 the deck, 0..3 stairs down toward +X, -X, +Z, -Z.
        mutable EntityPtr walkPiece;
        mutable int8 walkDir = -2;
        mutable int8 walkWant = -2; // rebuildNetworks: this pass's pick (walkwayModeOf), read by the neighbours' poses
        // THE DECK'S TILT (solveWalkwayDecks -> updateWalkway): walkLift = the deck's origin over the Walkway's own,
        // walkSlope = its rise per m along X and Z; 0 for stairs or no piece. walkwayDeckY reads it. The *Want pair is
        // the solve's result; a change spawns the deck and its legs (walkLegs: upright, walkwayLegs.pre) again.
        mutable float walkLift = 0.0f;
        mutable glm::vec2 walkSlope{ 0.0f };
        mutable float walkLiftWant = 0.0f;
        mutable glm::vec2 walkSlopeWant{ 0.0f };
        mutable EntityPtr walkLegs;
        // PROBLEM BADGE (the world label's bubble): what GameMatch::structureWarning last found,
        // re-checked on a jittered ~1 s timer instead of every frame - it scans the links, and the
        // states it reports change on the timescale of a player's actions. Lives HERE, so it dies
        // with its structure (no id-keyed map).
        const char* warning = nullptr; // nullptr = nothing wrong
        glm::vec3 warningColor{ 1.0f };
        float warningTimer = 0.0f;     // seconds to the next check (0 = check on the next label build)
        // House: the barracks it feeds population to (0 = none in range), re-derived every
        // refresh() - nearest BUILT own-team barracks within "House link radius".
        uint32 linkedId = 0;
        // THE WALKERS' VIEW OF THIS BUILDING (authority only; Carriers.cpp). Counted from m_walkers at the top of every
        // tickCarriers, except sendReady (the ~1 s pass).
        uint8 walkers = 0;      // walkers whose transport job is this building
        uint8 walkersIdle = 0;  // ... of them: waiting at the pickup, or on their way to it
        uint8 guards = 0;       // an Armory: the walkers that are its guards, or on their way to it for a bow (counted fresh every tickCarriers)
        uint8 crew = 0;         // a Cannon: the walkers of its crew (on the way, at the gun or fetching munitions; counted fresh every tickCarriers)
        uint8 crewAtGun = 0;    // ... of them: at the gun now (it fires only with one)
        uint8 crewFetching = 0; // ... of them: on a munitions trip
        float alarm = 0.0f;     // an Armory / a Cannon: seconds its guards / crew still stay out (set while an enemy unit is in its range)
        glm::vec3 threat{ 0.0f }; // an Armory: the enemy unit nearest to it at the last pass (the posts are picked by it)
        uint8 waitRank = 0;     // scratch of the walker loop: the waiting walkers seen so far (the first one loads)
        uint8 homeWalkers = 0;  // House / Beacon: its workers that are out on the road
        bool sendReady = false; // it has a load to send now (a storage: and a consumer on its road wants it)
        bool buildReady = false; // it holds stone, and a blueprint on its road wants it (the ~1 s pass): work for a walker with no other
        bool repairReady = false; // the same for a damaged structure on its road, and only while the team has no build work
        bool transportReady = false; // a storage: another storage on its road is much emptier (a cart trip)
        bool transportUrgent = false; // ... by "Cart urgent fill gap" or more: the cart goes before the loads at the pickup
        bool ammoReachable = false;  // an alarmed Cannon: it holds a munition (or one is on its way), or a source of them is on its road (the ~1 s pass)
        float goodsIncoming[GameStructureComponent::NumStores] = {}; // per store index: the goods the walkers carry toward it now (counted fresh every tickCarriers)
        float cannonReload = 0.0f; // a Cannon: seconds to its next shot (tickCannons, authority)
        // A Cannon's GUN TURN: its aim from the placed facing (radians about +Y, within the arc). The entity's rotation is
        // the facing turned by it (cannonFacingRot undoes it). The authority turns it toward its target (tickCannons); a
        // client gets it in GSt's output byte.
        float cannonAim = 0.0f;
        float cannonAimGoal = 0.0f;           // where the gun turns to ("Turn rate (deg/s)"): the target's bearing (a client: GSt's)
        float cannonSeek = 0.0f;              // seconds to the next target search
        bool cannonHasTarget = false;
        glm::vec3 cannonTargetPos{ 0.0f };    // the target's place at the last search
        float buildIncoming = 0.0f; // a blueprint: the stone the walkers carry toward it now (counted fresh every tickCarriers)
        // THE LIGHT LINK (Light.cpp; derived on every instance about once a second): the HUB (a Crystal store, a Lens or
        // the Beacon) this structure sends its light to (a producer), takes it from (a consumer) or - a hub - its parent
        // in its hub network's tree. 0 = none. lightBlocked: a hub is in range but nothing has a clear line to it.
        uint32 lightId = 0;
        bool lightBlocked = false;
        float lightFlow = 0.0f; // light/s on the link, smoothed (tickLight; an estimate on a mirror): the beam shows while > 0
        mutable float beamLit = 0.0f; // 0..1: how much of its light the beam casts on surfaces (drawLightLinks fades it)
        // ON THE SHIP (see "The ship's structures" below): its ground point on the deck in SHIP space (y 0 = the deck
        // top; x, z snapped to the deck's tiles like snapToGrid). Its pose follows the ship. authoredInvulnerable: the
        // prefab's flag, restored when the ship lands (aloft every ship structure is invulnerable).
        bool onShip = false;
        glm::vec3 shipLocal{ 0.0f };
        int8 shipTurn = 0; // a rotated placement on the deck (the Cannon): eighths of a turn relative to the ship
        bool authoredInvulnerable = false;
        // A SHIP RAIL's collider (buildRailBody): the rail pieces on its open sides only, so the tile is walked on. Landed
        // and not rubble only (aloft the ship's body carries them). Shared: the roster hands out copies of a Ref.
        mutable oc::shared_ptr<PhysicsBody> railBody;
        // THE WARDS (Wards.cpp; derived on every instance by rebuildWardLinks). On a wall or gate segment: the ward of
        // each kind that covers it (0 = none), and the render-only pieces that show it (wardPieces: root entities of
        // their own, owned here; wardLook: what they show now, so they are spawned again only when it changes). On a
        // ward: the segments it covers, the segments its search reaches (wardReach: also the ones a nearer ward of its kind
        // covers), and the latch (wardDrained: its store ran empty, and it stays off until the store is back at
        // "Restart at"; updateWardLatches, every instance).
        uint32 wardOf[WardKindCount] = {};
        uint16 wardWalls = 0;
        uint16 wardReach = 0;
        bool wardDrained = false;
        mutable oc::vector<EntityPtr> wardPieces;
        mutable uint32 wardLook = 0;
        // RUBBLE: a living enemy unit in "Rubble safe radius (m)" at the last refreshRubbleSafety (a fresh pile counts as
        // unsafe until then: setRubble). rubbleBlocked reads it instead of a sphere query per call.
        mutable bool rubbleUnsafe = false;
    };

    // CO-OP hooks: the server runs the real sim and notifies; clients mirror via the mirror* calls
    // + the periodic stat sync (mirrored emitters drive their LOCAL ForceComponent, so client-side
    // fields are real).
    oc::function<void(int index)> onStructurePlaced;                       // server -> send GPl
    oc::function<void(uint32 id)> onStructureRemoved;                      // server -> send GRm
    // A blueprint completed (investMaterials): the server re-fires GPl for it - cable segments are
    // excluded from GSt, so this is the only way a client learns a cable finished building.
    oc::function<void(uint32 id)> onStructureBuilt;
    oc::function<void(uint32 id)> onRouteChanged;                          // server -> send GRt
    oc::function<void(uint32 id)> onUnitTypeChanged;                       // server -> send GBu
    // Authority: re-push a changed route onto the barracks' live units. The unit roster lives in
    // NpcSystem (this partition cannot import it), so GameMatch wires the walk in.
    oc::function<void(uint32 id, oc::span<const glm::vec3> route)> onRouteLiveUnits;

    void mirrorPlace(uint32 id, EStructureType type, const glm::vec3& pos, const glm::vec2& facingXZ,
        float yield, uint8 team, bool built);
    void mirrorRemove(uint32 id);
    void mirrorStructureState(uint32 id, float healthFrac, float chargeFrac, float fuelFrac,
        float mineralFrac, float ammoFrac, float outputFrac, float utilFrac, bool powered, bool blueprint, bool rubble);
    // Cable/crossing BLUEPRINTS only (GCb): health = build progress, over the segment's own
    // healthMax. The built flip still arrives through the GPl re-send.
    void mirrorCableProgress(uint32 id, float healthFrac, bool rubble);
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

    glm::vec2 structureFacing(int index) const; // from the entity's rotation (Crossing replay)
    float structureYield(int index) const { return m_frame[index].yield; } // Lumberyard / Quarry (1 elsewhere)
    float structureCannonAim(int index) const { return m_frame[index].cannonAim; } // a Cannon's gun turn (radians from its facing)
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

    // THE FLAT MAPS' RESOURCE NODES: GameMatch places them one by one from the generated map (seeded - every instance
    // derives the identical set from the same seed, the corridor-set contract). A node is a DEPOSIT: a Quarry (mineral
    // node) or a Lumberyard (fuel node) with one in its harvest radius has a full yield. The sandbox world has none.
    void spawnNode(float x, float z, ENodeType type, float groundY = 0.0f); // one resource node entity + roster entry
    void clearNodes(); // co-op map regeneration: drop the node entities + roster (nothing else)
    void spawnBase(const glm::vec3& groundPos, uint8 team = 0);
    void clear(); // drops every structure entity + node (before world teardown)

    // The per-frame REFRESH: re-stamps live tuning (capacities/bands/throughputs) onto every roster
    // entry. The roster itself is maintained at the spawn/remove seams (spawnStructure,
    // destroyStructureAt, onWorldRootRemoved) - no world query. Runs at the top of tickAuthority
    // (server/single player) and tickMirror (clients).
    void refresh();
    // World::removeRootEntity notification (wired by GameMatch): deregisters the entry with full
    // bookkeeping (unlink, GRm hook) for ANY removal path - editor delete, script
    // destroy - not just the game's own. Must NOT call removeRootEntity (see World.ixx).
    void onWorldRootRemoved(const Entity* entity);
    oc::span<const Ref> structures() const { return m_frame; }

    // Client requests / local input (queued; validated + applied in tickAuthority - the MP seam).
    // yield: a Lumberyard's / Quarry's, from the requester's survey (harvestYield); ignored for every other type.
    void queuePlaceRequest(EStructureType type, const glm::vec3& groundPos, float yield,
        const glm::vec3& facing, uint8 team);
    void queueDemolishRequest(uint32 id, uint8 team);
    void queueRouteRequest(uint32 id, oc::span<const glm::vec3> points, uint8 team);
    // The ORDER byte (hasOrderByte): a barracks' unit type, a store's EStorePriority. One request / mirror / wire path.
    void queueUnitTypeRequest(uint32 id, uint8 unitType, uint8 team);
    static constexpr int MaxRouteWaypoints = GameStructureComponent::MaxRoutePoints;
    // The order byte (barracks: the union's barracks variant; a store: storePriority; 0 elsewhere).
    uint8 structureUnitType(int index) const
    {
        const Ref& ref = m_frame[index];
        return isBarracksType(ref.type) ? ref.state->barracks.unitType : hasStorePriority(ref.type) ? ref.state->storePriority : 0;
    }
    EStorePriority structurePriority(int index) const
    {
        return hasStorePriority(m_frame[index].type) ? (EStorePriority)m_frame[index].state->storePriority : EStorePriority::Balance;
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
    int structureWardWalls(int index) const { return m_frame[index].wardWalls; } // a ward: the wall segments it covers
    int structureWardReach(int index) const { return m_frame[index].wardReach; } // a ward: the wall segments its search reaches
    bool structureWardDrained(int index) const { return m_frame[index].wardDrained; } // a ward: off until its store refills
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
    float transportTickPeriod() const { return 1.0f / glm::max(m_settings.transportTickHz, 1.0f); }
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
        return m_settings.unitPopulation[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float unitSpawnEnergy(int unitType) const
    {
        return m_settings.spawnEnergy[glm::clamp(unitType, 0, GameNumUnitTypes - 1)];
    }
    float houseLinkRadius() const { return m_settings.houseLinkRadius; }
    // The medic's reach/rate live in GameStructureParams (the component's machine logic heals
    // units); these read them for the player heal, the ghost and the ring.
    float medicHealRadius() const { return Globals::settings.game.structureParams.medicRange; }
    float medicHealRate() const { return Globals::settings.game.structureParams.medicHealRate; }
    float armoryPostRange() const { return m_settings.armoryPostRange; } // the guards' / mages' posts lie this near their armory
    int housePopulation() const { return m_settings.housePopulation; }
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
    float waypointRadius() const { return m_settings.waypointRadius; }
    int wallBreachCost() const { return m_settings.wallBreachCost; } // Nav step multiplier through a built wall

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
    // The ground point under the structure's center (the entity sits half its box height above it): rings, routes
    // and other debug draws that hug the ground.
    glm::vec3 structureGroundPos(int index) const
    {
        return m_frame[index].entity->pos - glm::vec3(0.0f, spawnHeightOf(m_frame[index].type), 0.0f);
    }
    glm::vec3 structureLabelAnchor(int index) const;
    EStructureType structureType(int index) const { return m_frame[index].type; }
    float structureHealth(int index) const { return m_frame[index].state->health; }
    float healthMaxOf(EStructureType t) const { return glm::max(m_settings.health[(int)t], 1.0f); } // "Game/Health": one value per type
    float structureHealthMaxOf(int index) const { return m_frame[index].state->healthMax; }
    uint8 structureTeam(int index) const { return (uint8)m_frame[index].state->team; }
    bool structureBlueprint(int index) const { return m_frame[index].state->blueprint; }
    // RUBBLE: a built building the enemy destroyed (tickDamage) - a blueprint again, dark, walked over, no target.
    bool structureRubble(int index) const { return m_frame[index].state->rubble; }
    void setRubble(const Ref& s, bool rubble); // the flag + its tint + its collider (off while rubble)
    // Rubble with a body on the pile, or (not for a player) with a living enemy unit in "Rubble safe radius (m)": it is not built up.
    // The enemy part is Ref::rubbleUnsafe (refreshRubbleSafety, ~4 times a second); the body on the pile is checked live.
    bool rubbleBlocked(const Ref& s, bool byPlayer = false) const;
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
    void drawDebug(bool buildMode, EStructureType armed, uint32 selectedId) const; // see the definition for the inputs

    // ---- THE HARVEST (the Lumberyard and the Quarry) ----
    // A yard stands anywhere; what is within "Harvest radius" of it sets its YIELD (0..1, a share of the full rate),
    // fixed at placement. The sandbox world: the trees (Lumberyard) / the rocks (Quarry) of the world's records, through
    // the App's source. The flat maps: a resource node of its kind in the radius is a full yield.
    // The source answers what stands in a circle: wood = trees, stone = the rocks' summed size (m). false = it does not
    // know (the place is too far from this instance's camera, or its world is not generated yet).
    // It VISITS each tree and rock once, with its world position (the same in every survey), its wood and its stone, and a
    // weight (x its amount after the size exponent: a tree species' `Wood`, else 1).
    using HarvestVisitor = void (*)(void* user, float x, float z, float wood, float stone, float weight);
    using HarvestSource = bool (*)(float x, float z, float radius, HarvestVisitor visit, void* user);
    void setHarvestSource(HarvestSource source) { m_harvestSource = source; }
    float harvestRadius() const { return m_settings.harvestRadius; }
    // YARDS SHARE: a tree or a rock works for ONE yard. Yards of one kind whose circles overlap (a CLUSTER, joined
    // through any number of yards) divide what stands in the circles between them: the OLDER yard (the lower id) is
    // served first, and a younger one gets what is left - but an older yard takes its share from the trees the
    // younger one cannot reach first, so nothing that a yard could use stays unused (shareHarvest: a maximum flow,
    // one yard after the other; a yard's amount never drops when a younger one comes, and can rise when a yard goes).
    // The yield `type` would have at a ground point, next to the yards that stand; false = the source has no answer.
    // IT CALLS THE SOURCE, which reads the tree records: only from GameMatch::surveyHarvest (the App's survey point
    // in the frame).
    bool harvestYield(EStructureType type, const glm::vec3& groundPos, float& outYield) const;
    // The yields of the yards that stand, again from the world (every instance; from GameMatch::surveyHarvest too):
    // after a yard came or went (Ref::harvestDirty), each cluster with a dirty yard, about once a second until the
    // source answers for all of it. With no source (the flat maps, a headless server) a yard keeps its placement yield.
    void resolveHarvestYields();
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
        case EStructureType::Emitter:     return m_settings.emitterBuffer; // shield emitters hold a deeper charge:
        case EStructureType::Bastion:     return m_settings.bastionBuffer; // pressure draw spikes under a push
        // (The Lumberyard, the Quarry, the Fabricator and the Constructor hold no light: production runs on WORKERS, see staffing.)
        case EStructureType::Solar:
        case EStructureType::MedicStation:
        case EStructureType::Searchlight:
        case EStructureType::AmmoWorks:   // (its munitions are light charges: it takes light like a consumer)
        case EStructureType::Turret:      return m_settings.internalBuffer;
        case EStructureType::Barracks:    return 1.0f; // > 0 = power cables attach; the REAL capacity is
                                                       // stamped per instance (= its unit's cost, the build bar)
        case EStructureType::Generator:   return m_settings.generatorBuffer;
        case EStructureType::Battery:     return m_settings.batteryCapacity;
        case EStructureType::Lens:        return m_settings.lensBuffer;
        case EStructureType::Base:        return m_settings.baseEnergyCapacity; // feeds its always-on shield
        case EStructureType::WardResist:
        case EStructureType::WardAbsorb:
        case EStructureType::WardRetaliate: return m_settings.wardCapacity; // what it pays its walls' protection from
        default:                          return 0.0f;
        }
    }
    // ---- POPULATION: production needs WORKERS, not light ----
    // A team's workers = "Base workers" + "House workers" for each built House. Every built production building asks
    // for its workers (workersNeeded). With fewer workers than jobs, every production rate of the team scales by
    // staffing() = workers / jobs. Derived on every instance (updateStaffing: the authority's tick and the mirror's).
    int workersNeeded(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Lumberyard:
        case EStructureType::Quarry:      return m_settings.workersHarvest;
        case EStructureType::Fabricator:  return m_settings.workersFabricator;
        case EStructureType::Constructor: return m_settings.workersConstructor;
        case EStructureType::Generator:   return m_settings.workersGenerator;
        case EStructureType::AmmoWorks:   return m_settings.workersAmmo;
        default:                          return 0;
        }
    }
    // The light link of a roster entry (see Ref::lightId), for the badge and the ghost's rings.
    bool structureLightLinked(int index) const { return m_frame[index].lightId != 0; }
    bool structureLightBlocked(int index) const { return m_frame[index].lightBlocked; }
    bool lightBeams() const { return m_settings.lightBeams; }
    float lightHubReach() const { return m_settings.lightHubReach; }
    float mirrorBeamRange() const { return m_settings.mirrorBeamRange; }
    float lensRange() const { return m_settings.lensRange; }    int teamWorkers(uint8 team) const { return m_workers[team]; }
    int teamJobs(uint8 team) const { return m_jobs[team]; }
    int teamCarriers(uint8 team) const { return m_carrierCount[team]; } // workers out on the road (Carriers.cpp; the authority)
    bool structureOnRoad(int index) const { return touchesRoad(index); }
    float staffing(uint8 team) const { return m_jobs[team] > m_workers[team] ? (float)m_workers[team] / (float)m_jobs[team] : 1.0f; }
    void updateStaffing();
    float structureFuel(int index) const { return m_frame[index].state->store[1]; }
    float structureFuelCapacity(int index) const { return fuelCapacityOf(m_frame[index].type); }
    float structureFlowUtil(int index) const { return m_frame[index].state->flowUtil; }
    float structureMinerals(int index) const { return m_frame[index].state->store[2]; }
    float structureMineralCapacity(int index) const { return mineralCapacityOf(m_frame[index].type); }
    float structureMunitions(int index) const { return m_frame[index].state->store[GoodMunitions]; }
    float structureMunitionCapacity(int index) const { return munitionCapacityOf(m_frame[index].type); }
    float munitionCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Cannon:    return m_settings.cannonAmmoCapacity;
        case EStructureType::Ballista:  return m_settings.ballistaAmmoCapacity;
        case EStructureType::AmmoWorks: return m_settings.ammoWorksBuffer;
        case EStructureType::AmmoStore: return m_settings.ammoStoreCapacity;
        default:                        return 0.0f;
        }
    }
    float fuelCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Generator:  return m_settings.generatorFuelTank;
        case EStructureType::FuelTank:   return m_settings.fuelTankCapacity;
        // THE STOREHOUSE holds every good: stone (below) AND wood. The Fuel tank (the Woodshed) is off the hotbar.
        case EStructureType::MineralSilo: return m_settings.fuelTankCapacity;
        case EStructureType::Lumberyard:
        case EStructureType::Fabricator: return m_settings.internalBuffer;
        default:                         return 0.0f;
        }
    }
    float mineralCapacityOf(EStructureType t) const
    {
        switch (t)
        {
        case EStructureType::Quarry:
        case EStructureType::Fabricator:
        case EStructureType::Constructor:  return m_settings.internalBuffer;
        case EStructureType::AmmoWorks:    return m_settings.ammoWorksBuffer; // the stone it packs
        // Barracks hold NO minerals: units are paid from their energy store (no conveyor attaches).
        case EStructureType::MineralSilo:  return m_settings.mineralSiloCapacity;
        case EStructureType::Base:         return m_settings.mineralBaseCapacity;
        default:                           return 0.0f;
        }
    }
    float mineralCost(EStructureType type) const { return m_settings.costs[(int)type]; }
    // The build hotbar's hover card: one sentence + this type's exact per-second flows, one per
    // line, straight off the live tweaks (see the definition).
    oc::string describeType(EStructureType type) const;
    int affordableCount(EStructureType type, uint8 team = 0) const
    {
        const float cost = m_settings.costs[(int)type];
        return cost > 0.0f ? (int)(minerals(team) / cost) : 0;
    }
    float emitterReachOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_settings.bastionReach
             : t == EStructureType::Base ? m_settings.baseShieldReach : m_settings.emitterReach;
    }
    float placeRange() const { return m_settings.placeRange; }

    // ---- GRID PLACEMENT ----
    static constexpr float GridCellSize = 2.0f;
    static constexpr float WalkwayDeckHeight = 2.0f; // the Walkway's deck top over its ground (walkwayDeck.pre / walkwayStairs.pre)
    static constexpr int footprintCellsOf(EStructureType t)
    {
        switch (t)
        {
        case EStructureType::FuelTank:
        case EStructureType::Generator:
        case EStructureType::Battery:
        case EStructureType::Bastion:
        case EStructureType::Turret:
        case EStructureType::Searchlight:
        case EStructureType::Solar:
        case EStructureType::Lumberyard:
        case EStructureType::Quarry:
        case EStructureType::Fabricator:
        case EStructureType::Armory:
        case EStructureType::MageTower:
        case EStructureType::AmmoWorks:
        case EStructureType::House:
        case EStructureType::MedicStation:
        case EStructureType::MineralSilo: return 2;
        case EStructureType::Barracks:
        case EStructureType::Base:        return 3;
        default:                          return 1; // cables/crossing included (Crossing extends
        }                                           // along its facing - see footprintExtent)
    }
    // Cells covered along X and Z. Everything is square except the Crossing: 3x1 along its facing
    // (the entity rotation, quantized to an axis at placement).
    static glm::ivec2 footprintExtent(EStructureType t, const glm::quat& rot);
    static glm::vec3 snapToGrid(EStructureType type, const glm::vec3& groundPos);
    // ignoreCables: the unit-spawn probe - cables are walk-through, so a conveyor ring around a
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
    // redundant - the crossing's end conducts that medium anyway - and refusing it meant a stroke
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
    // A player capsule or unit standing on the footprint (spatial query - no rosters). Separate
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
    // - checked by cellsFree exactly like the arena bounds (so ghosts turn red and the unit spawn
    // probe refuses rock cells too). GameMatch rebuilds the list with the map.
    void setTerrainBlocked(oc::vector<glm::vec4> rects) { m_terrainBlocked = oc::move(rects); }

    // ---- THE SANDBOX WORLD'S GROUND (co-op on the procedural terrain) ----
    // Off (the flat maps): the ground is the y = 0 plane and every test below passes. On: heights are rays down onto
    // the terrain collider (layer "Terrain"), trees and rocks are their world colliders (layer "Vegetation"), water is
    // the physics water surface. The colliders exist only near this instance's camera (Terrain/Collision "Radius",
    // Trees/World colliders), which covers the place range around the player.
    void setTerrainGround(bool enabled) { m_terrainGround = enabled; }
    // THE COARSE GROUND (the sandbox world; the App's terrain height, any distance, a few metres off): what
    // groundHeightAt returns where its ray finds no collider - a place far from every player. Null = the fallback.
    using GroundSource = float (*)(float x, float z, float fallback);
    void setGroundSource(GroundSource source) { m_groundSource = source; }
    bool terrainGround() const { return m_terrainGround; }
    // The terrain height under (x, z); `fallback` where no collider is (always 0 on the flat maps).
    // coarse: where the ray finds no collider, the App's coarse terrain height (setGroundSource) before the fallback.
    float groundHeightAt(float x, float z, float fallback = 0.0f, bool coarse = false) const;
    enum class EGroundFit : uint8 { Ok, NoGround, Steep, Water, Blocked };
    // Can `type` stand at the snapped position? Samples the footprint's corners, edge midpoints and center: every
    // sample must hit the terrain, the highest minus the lowest may not pass "Max ground step (m)" (twice that for the
    // flat cable pieces), the ground may not sit under water, and no tree or rock collider may cut the box from the
    // ground to the structure's top. outY = the height the structure stands on: the LOWEST sample for a building (the
    // box sinks into the high side - it never floats over the low one), the center for a cable piece.
    EGroundFit groundFit(EStructureType type, const glm::vec3& snappedGroundPos, const glm::quat& rot, float& outY) const;
    static const char* groundFitReason(EGroundFit fit);

    // ---- CONSTRUCTION (server) ----
    float takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount);
    float fundNearbyBlueprint(const glm::vec3& pos, float radius, uint8 team, float amount,
        bool includeRepairs = false, float repairHealCap = FLT_MAX, bool byPlayer = false);
    // The structure fundNearbyBlueprint would pay: the nearest own blueprint (or damaged one, includeRepairs) in reach that
    // rubbleBlocked lets through. -1 = none.
    int findBuildTarget(const glm::vec3& pos, float radius, uint8 team, bool includeRepairs, bool byPlayer) const;
    // repairHealCap: the most health this call gives a BUILT structure (a repair); a blueprint has no cap.
    // byPlayer: a player pays - rubble takes it with enemies near too (rubbleBlocked).
    float investMaterials(const Ref& s, float amount, float repairHealCap = FLT_MAX, bool byPlayer = false);
    float constructorRange() const { return m_settings.constructorRange; }

    // A built Walkway DECK (not its stairs) in the grid cell under pos: outY = its walk height at pos (0.1 over the deck
    // top, with its tilt: walkwayDeckY).
    bool walkwayDeckTop(const glm::vec3& pos, float& outY) const;
    // The guards' shots of this tick (tickCarriers): the match draws one line each and clears the list.
    struct GuardShot { glm::vec3 from, to; float arc; }; // arc: the arrow's height over the straight line at its middle (m)
    oc::vector<GuardShot>& guardShots() { return m_guardShots; }
    // THE CANNONS (tickCannons, authority, after production): a built Cannon with a munition and its reload done fires
    // at the nearest solid enemy unit in its arc ("Arc (deg)" around its facing) between "Min range" and "Range". A
    // shot is a launch (the muzzle, the ballistic velocity): the match spawns the shell (NpcSystem::fireCannonShell)
    // and clears the list. The shell does the damage (cannonShell.pre: its splash). A BALLISTA's shot is a bolt (bolt:
    // ballistaBolt.pre, NpcSystem::fireBallistaBolt); a MAGE's (tickCarriers) a fireball (mageFireball.pre).
    enum class EShotKind : uint8 { Shell, Bolt, Fireball };
    struct CannonShot { glm::vec3 from, velocity; uint8 team; EShotKind kind = EShotKind::Shell; };
    oc::vector<CannonShot>& cannonShots() { return m_cannonShots; }
    void tickCannons(float deltaSec);
    // A gun's placed facing: its entity's rotation without the gun's turn (Ref::cannonAim; identity turn elsewhere).
    static glm::quat cannonFacingRot(const Ref& r)
    {
        return isGunType(r.type) ? r.entity->rot * glm::angleAxis(-r.cannonAim, glm::vec3(0.0f, 1.0f, 0.0f)) : r.entity->rot;
    }
    // Turns the gun toward Ref::cannonAimGoal at "Turn rate (deg/s)": the entity's rotation follows. Authority and mirror.
    void turnCannon(Ref& r, float deltaSec) const
    {
        const float step = glm::radians(glm::max(m_settings.cannonTurnRate, 1.0f)) * deltaSec;
        const float aim = r.cannonAim + glm::clamp(r.cannonAimGoal - r.cannonAim, -step, step);
        if (aim == r.cannonAim)
            return;
        const glm::quat facing = cannonFacingRot(r);
        r.cannonAim = aim;
        r.entity->rot = facing * glm::angleAxis(aim, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // THE AMMO STORE'S BLAST (tickDamage): where one blew up this tick and how big - the match shows the flash and clears
    // the list. The damage is done already.
    struct Blast { glm::vec3 pos; float radius; };
    oc::vector<Blast>& blasts() { return m_blasts; }
    // A Cannon's stand: the top of the road piece or the walkway deck in its cell, else nothing (false: the ground).
    bool cannonStandY(const glm::vec3& snappedGround, float& outY) const;

    // ---- THE SHIP'S STRUCTURES (Docs/Context/Game/Ship.md "Building on the ship") ----
    // A structure ON THE SHIP keeps its place on the deck in SHIP space (Ref::shipLocal). GameMatch hands in the
    // ship's pose every tick (setShipPose, before tickShipStructures). FLYING it rides the ship and is OUT OF THE WORLD:
    // no grid cells, its own collider off (the ship's body carries its box: shipColliderBoxes), invulnerable (no unit
    // bites it, no shot or field hurts it, no Nav obstacle). LANDED, the ship sits on the world grid (a quarter-turn
    // yaw, its centre on a cell corner), so every deck tile IS a world cell: the structures join the world like any
    // other (cells, collider, damage, Nav, the light links), and the deck's FREE tiles are ROAD (roadAt). The
    // authority only: clients see no ship and get none of these (no GPl).
    void setShipPose(bool exists, const glm::vec3& pos, const glm::quat& rot, bool landed);
    void tickShipStructures(); // the poses, and the join / leave edges of a landing / take-off
    // Changes whenever the boxes shipColliderBoxes gives change (a ship structure came, went or became rubble, the
    // ship landed or took off): GameMatch rebuilds the ship's body then.
    uint32 shipRevision() const { return m_shipRevision; }
    void shipColliderBoxes(oc::vector<GameShip::Box>& out) const; // empty while landed
    bool structureOnShip(int index) const { return m_frame[index].onShip; }
    bool shipJoined() const { return m_shipJoined; } // the ship's structures are in the world (it has landed)
    // A landed ship rail's open sides in WORLD axes (bit 0 +X, 1 -X, 2 +Z, 3 -Z): its rail pieces, which no path crosses.
    uint8 railWorldSides(const Ref& s) const;
    // A ship-local ground point snapped to the deck's tiles for `type` (snapToGrid's rule, in ship space).
    static glm::vec3 snapToDeck(EStructureType type, const glm::vec3& local) { return snapToGrid(type, local); }
    // Can `type` stand at the snapped ship-local point: every footprint tile on the deck, off the helm, free of the
    // other ship structures.
    bool deckCellsFree(EStructureType type, const glm::vec3& snappedLocal) const;
    // turn: a rotated placement's facing on the deck, in eighths of a turn relative to the ship (the Cannon).
    void queueShipPlaceRequest(EStructureType type, const glm::vec3& snappedLocal, float yield, uint8 team, int turn = 0);
    // The ship's world pose of a ship-local point (setShipPose's pose).
    glm::vec3 shipToWorld(const glm::vec3& local) const { return m_shipPos + m_shipRot * local; }
    // The Beacon on the deck (the start) and the rails on the edge tiles that have none yet (the start, after a load).
    void spawnShipBase(uint8 team = 0);
    void ensureShipRails(uint8 team = 0);
    // A LANDING SITE: can the ship land with its centre at `centre` (a tile corner) and `turns` quarter turns? Free
    // of the other structures' cells. The ground, the water and the trees are the match's tests.
    bool shipLandingClear(const glm::vec3& centre, int turns) const;

private:
    // The ship-local tile of a world cell (landed: the inverse of the quarter-turn pose); false = no deck tile there.
    bool worldCellOnDeck(int cx, int cz) const;
    void placeShipStructure(EStructureType type, const glm::vec3& local, float yield, uint8 team, int turn); // the ship request's validation + spawn
    int spawnShipStructure(uint32 id, EStructureType type, const glm::vec3& local, uint8 team, bool built, float yield = 1.0f, int turn = 0);
    // A ship structure's world rotation: the ship's, turned by its own shipTurn (a Cannon).
    glm::quat shipStructureRot(const Ref& r) const
    {
        return m_shipRot * glm::angleAxis((float)r.shipTurn * glm::quarter_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    void shipJoin(const Ref& s);  // into the world (landed)
    void shipLeave(const Ref& s); // out of it (aloft)
    void applyRailSides(const Ref& s); // a ShipRail's rail pieces: the sides of its tile that face off the deck
    void buildRailBody(const Ref& s);  // a ShipRail's collider (none aloft or as rubble)
    // The rail pieces of a rail tile in ship space, from the tile's centre on the deck top: (centre, half) per open side.
    static int railPieces(const glm::vec3& tileLocal, glm::vec3 (&centres)[4], glm::vec3 (&halves)[4]);
    // THE GANGWAYS (landed): stairs out of every ENTRANCE down to the ground - render-only steps (gangwayStep.pre), one
    // sloped collider, and their cells as ROAD (m_gangwayRoad: cell -> the road height), so the walkers go down them.
    // Built at the landing edge, gone at the take-off.
    struct Gangway
    {
        oc::vector<EntityPtr> steps;
        PhysicsBody body;
    };
    Gangway m_gangways[8]; // one per entry of GameShip::entrances
    oc::unordered_map<uint64, float> m_gangwayRoad;
    // > 0: a flight found NO GROUND COLLIDER under it (the terrain collider there is not built yet - a load right after
    // the start): tickShipStructures builds the stairs again when this counts down to 0 (a tick each).
    int m_gangwayRetry = 0;
    void buildGangways();
    void clearGangways();
    bool m_shipExists = false;
    bool m_shipLanded = false;
    bool m_shipJoined = false; // the ship structures are in the world (the last tickShipStructures saw it landed)
    // The landed deck's world cells (inclusive, one cell of margin): the deck / rail checks of the road search and roadAt
    // answer "no" outside it with two compares instead of hash lookups and the pose math (set at the landing edge).
    glm::ivec2 m_deckCellMin{ 0 }, m_deckCellMax{ -1 };
    bool nearDeck(int cx, int cz) const
    {
        return cx >= m_deckCellMin.x && cx <= m_deckCellMax.x && cz >= m_deckCellMin.y && cz <= m_deckCellMax.y;
    }
    glm::vec3 m_shipPos{ 0.0f };
    glm::quat m_shipRot{ 1.0f, 0.0f, 0.0f, 0.0f };
    uint32 m_shipRevision = 0;

    oc::vector<GuardShot> m_guardShots;
    oc::vector<CannonShot> m_cannonShots;
    oc::vector<Blast> m_blasts;
    void ammoBlast(const Ref& s); // tickDamage: an Ammo store dies with munitions in it
    struct Node
    {
        EntityPtr entity;
        glm::vec3 pos{ 0.0f };
        ENodeType type = ENodeType::Mineral;
    };
    struct PlaceRequest
    {
        EStructureType type;
        glm::vec3 pos;
        glm::vec3 facing{ 0.0f };
        float yield = 1.0f;
        uint8 team = 0;
        bool ship = false; // pos is a ship-local ground point on the deck
        int turn = 0;      // ship: a rotated placement's turn on the deck (eighths, relative to the ship)
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
    // linked houses' bonus. Derived every refresh() on every instance (clients too - positions
    // are mirrored, so the same derivation yields the same caps).
    void linkHouses();

    GameStructureComponent* stateById(uint32 id)
    {
        const int index = structureIndexById(id);
        return index >= 0 ? m_frame[index].state : nullptr;
    }
    void placeStructure(EStructureType type, const glm::vec3& groundPos, float yield,
        const glm::vec3& facing, uint8 team);
    void spendMinerals(uint8 team, float amount); // drains the team's Silos first, the Base last
    // The one placement path: spawns the prefab, stamps the component, registers into the frame.
    // Returns the frame index or -1. rot = authored orientation (a crossing's facing).
    int spawnStructure(uint32 id, EStructureType type, const glm::vec3& pos, const glm::quat& rot,
        uint8 team, bool built, float yield = 1.0f, bool insert = true, bool onShip = false); // insert: into the cell hash (a ship structure aloft: no)
    void destroyStructureAt(size_t index); // deregister (removeStructureBookkeeping) + drop the world's ref
    void removeStructureBookkeeping(size_t index); // unlink, erase + reindex, fire GRm
    // Its footprint's tops changed (spawned, destroyed, to rubble and back): the snow site map traces them again; hide = the
    // snow caps there go at once (Renderer::invalidateSnowSite). Not on the ship (its structures are movers: not in the map).
    void invalidateSnowSite(const Ref& s, bool hide) const;
    void applyStructureTint(const Ref& s);
    void applyDemolishRequest(uint32 id, uint8 team);
    void stampTuning(const Ref& s); // capacities + machine variant (per tick - tweaks stay live)
    void tickProduction(float deltaSec); // income, fuel burn, consumer drain, emitter ramps
    void tickDamage(float deltaSec);     // death sweep + strainable marks
    void tickConstructors(float deltaSec);
    // The Builder's huts on a mirror: the same pick and turn from the mirrored roster (the stone is the authority's).
    void mirrorConstructors(float deltaSec);
    // Turns the hut's crystal toward its target at "Constructor turn speed (deg/s)"; true = it points at it.
    bool turnBuilderHead(Ref& hut, const glm::vec3& target, float deltaSec) const;
    // Every instance, every frame: the aim heads (Ref::head) and the Builder's huts' stone streams.
    void poseAimHeads();
    void drawBuilderStreams() const;
    // Every c_rubbleCheckPeriod: Ref::rubbleUnsafe of each rubble pile, ONE sphere query each (the searches for build
    // work ran one per pile per searcher per frame, which a late game with much rubble and a large wave made very slow).
    void refreshRubbleSafety(float deltaSec);
    float m_rubbleCheckTimer = 0.0f;
    // ---- LIGHT ON BEAMS (Light.cpp; "Game/Light" "Beams") ----
    // Light does not ride the cable transport: the power cable is off the hotbar. HUBS are the Crystal stores and the
    // Beacon. A MIRROR sends its light on one beam to the nearest hub within "Mirror beam range" that it has a clear
    // line to; a Hearth to the nearest hub in the hub reach. A hub FEEDS every consumer in "Hub reach" with a clear
    // line. THE HUBS form NETWORKS: a spanning tree over the hub pairs in "Hub link range" (a Lens end: "Lens range")
    // with a clear line, the shortest first, so every hub that can reach another one by a chain of links is in one
    // network; each tree link is the child's lightId to its parent. A network levels as ONE POOL (levelLightNet).
    // rebuildLightLinks derives the links (every instance); tickLight moves the light along them (authority);
    // drawLightLinks draws the beams.
    void rebuildLightLinks();
    void tickLight(float deltaSec);
    // One network, m_lightNet[begin, end): every hub moves toward its share of the network's light (the store priority:
    // Fill hubs fill first, then Balance, then Drain; the Beacon keeps its reserve), each at its own rate; the light on
    // each tree link (the flow out of the child's subtree) goes to m_lightPoolMoved for the beams.
    void levelLightNet(size_t begin, size_t end, float deltaSec);
    struct LightNetNode { uint32 id; int parent; }; // parent: an index into m_lightNet, -1 = a network's root
    oc::vector<LightNetNode> m_lightNet; // every network's hubs, each network a tree in breadth-first order from its root
    oc::vector<float> m_lightPoolMoved;  // per roster index, this tick: the light on the hub's link to its parent
    // drawLightLinks: every moving link's glow as a beam card, the light on surfaces for the "Lit beams" best (see Light.cpp).
    void drawLightLinks(float deltaSec) const;
    struct BeamCandidate { const Ref* ref; glm::vec3 a, b; float power, score, wasLit; };
    mutable oc::vector<BeamCandidate> m_beamScratch; // drawLightLinks' per-frame list (kept for its capacity)
    // THE SEARCHLIGHTS (every instance, each frame): the shining ones' cones into GameUnitParams::lightCones (the
    // units' reveal + burn) and one SpotLight each. The aim and the draw are the component's (EMachineKind::Searchlight).
    void publishSearchlights() const;
    bool lightLineClear(const Ref& from, const Ref& to) const;
    glm::vec3 beamPointOf(const Ref& ref) const; // a beam's end on a building: the centre of its crystal
    float m_lightLinkTimer = 0.0f;
    size_t m_lightLinkCount = 0; // the roster size of the last derivation: a change re-derives at once
    // ---- THE WARDS (Wards.cpp) ----
    // rebuildWardLinks: every built ward runs ONE breadth-first search from the wall and gate segments that touch its
    // cell over the connected built segments of its team (the 8 cells around each), "Walls per ward" segments at most,
    // and covers the ones no earlier ward of its kind covers (Ref::wardOf). Every instance, about twice a second and at
    // once when the roster size changes. tickWards (authority): pays each ward for what its walls did since the last
    // tick, grows the Absorb shields back (not within "Absorb regen delay" of a hit), and stamps each segment's shares
    // (GameStructureComponent::wall), 0 for a ward that is not active. updateWardLatches (every instance): an emptied
    // ward stays off until its store is back at "Restart at". updateWardLooks (every instance): the pieces on the open
    // sides of each covered segment.
    void rebuildWardLinks();
    void updateWardLatches();
    void tickWards(float deltaSec);
    void updateWardLooks(bool authority);
    void tickWardLinks(float deltaSec, bool authority); // the timers of the two derivations above (the authority's tick and the mirror's)
    static void clearWardPieces(const Ref& s);
    bool wardActive(uint32 wardId) const; // the ward stands built, is not latched off and its store holds light
    float m_wardLinkTimer = 0.0f;
    float m_wardLookTimer = 0.0f;
    size_t m_wardLinkCount = 0;
    // ---- THE CARRIERS (Carriers.cpp, authority) ----
    // Wood and stone do not ride the cable transport ("Game/Carriers" "Enabled"): WORKERS carry them over the ROAD
    // (built conveyor segments and conveyor crossings). A walker is one FREE worker of its team (workers - production
    // jobs), and it comes out of a House (or the Beacon) that touches a road. It takes a TRANSPORT JOB = a building
    // that sends goods: it walks to that building's road cell (the pickup) and stays there - load, deliver to the
    // nearest taker, come back, wait. A second walker joins a job only while a load is ready and nobody waits for it;
    // a walker that waits behind another one looks for other work on its road network.
    struct Walker
    {
        EntityPtr entity;            // carrier.pre
        oc::vector<glm::vec3> path;  // the current leg: road cell centres, in walking order
        float along = 0.0f;          // metres walked on the leg
        float load = 0.0f;           // goods carried
        float wait = 0.0f;           // seconds to the next search (waiting)
        float idle = 0.0f;           // seconds waited with nothing to carry
        uint32 homeId = 0;           // its House / Beacon
        uint32 jobId = 0;            // the building it carries FROM
        uint32 targetId = 0;         // the building the load goes to
        uint8 good = 1;              // the store index it carries: 1 = wood, 2 = stone
        uint8 state = 0;             // EWalkerState (Carriers.cpp)
        uint8 work = 0;              // ECarrierWork: what its load is for
        bool dismissed = false;      // its worker is needed elsewhere: it walks home when it is next idle
        bool guard = false;          // its entity is guard.pre / mage.pre (it is armed): setGuardLook
        bool mage = false;           // its job is a Mage tower: armed with a staff, not a bow
        uint8 team = 0;              // its house's team
    };
    oc::vector<Walker> m_walkers;
    // THE ARMORY'S GUARDS: walkers too. While an enemy unit is in an Armory's range (Ref::alarm), free workers walk to
    // it for a bow (the houses send them, and waiting walkers go), then to a POST - a free Walkway deck cell in the
    // Armory's range, else a road cell, nearest to the threat - and shoot from there. Nothing hurts them. See
    // "The Armory" in Docs/Context/Game/Economy.md.
    GameUnitComponent* nearestEnemyUnit(const glm::vec3& pos, float radius, uint32 team, glm::vec3& outPos,
        const glm::vec3* sightFrom = nullptr, Entity** outEntity = nullptr) const;
    // THE ALARMS: whether any living enemy unit is within `radius` (flat); the first one found ends the search. outPos: its place.
    bool anyEnemyUnit(const glm::vec3& pos, float radius, uint32 team, glm::vec3* outPos = nullptr) const;
    // THE GUARDS' ARROWS in flight (authority): the hit is a hitscan, but it LANDS when the drawn arrow arrives (the
    // flight time: the distance / "Arrow speed (m/s)"). The victim is held, so a dead or removed one is simply skipped.
    struct ArrowHit { EntityPtr victim; float damage; float timeLeft; };
    oc::vector<ArrowHit> m_arrowHits;
    void tickArrowHits(float deltaSec);
    float arrowArc(const glm::vec3& from, const glm::vec3& to) const; // the arrow's drawn arc: over a wall in the way
public:
    // An arrow's flight time from -> to on its arc (arrowArc: the height over the straight line at the middle): the
    // length of the drawn parabola (8 chords) / "Arrow speed (m/s)", and at least the time GRAVITY takes over that arc -
    // a body thrown up and falling back rises arc = g T^2 / 8, so T = sqrt(8 arc / g) - so a high arc flies like a lob
    // (the drawing is linear in time along the chord plus that parabola: a ballistic flight). The gravity is x "Arrow arc
    // gravity (x)": the real one made a high arc too slow. The SAME on every role (NpcSystem draws the arrow for that
    // long; the authority lands the hit after it).
    static float arrowFlightTime(const glm::vec3& from, const glm::vec3& to, float arc = 0.0f)
    {
        constexpr int c_chords = 8;
        const glm::vec3 span = to - from;
        float length = 0.0f;
        glm::vec3 prev = from;
        for (int s = 1; s <= c_chords; ++s)
        {
            const float t = (float)s / (float)c_chords;
            const glm::vec3 next = from + span * t + glm::vec3(0.0f, 4.0f * arc * t * (1.0f - t), 0.0f);
            length += glm::distance(prev, next);
            prev = next;
        }
        const float gravity = glm::max(-Globals::settings.physics.gravity.y * Globals::settings.game.structures.guardArrowGravity, 0.1f);
        return glm::max(glm::max(length / glm::max(Globals::settings.game.structures.guardArrowSpeed, 1.0f),
                                 std::sqrt(8.0f * glm::max(arc, 0.0f) / gravity)), 0.05f);
    }
private:
    bool walkwayDeckAt(int cx, int cz) const;
    bool guardDeckAt(int cx, int cz) const; // a Walkway deck, a built ward's top, or a built rail tile of the landed ship: no clear line needed
    bool wardAt(int cx, int cz) const; // a built walkable tower (a ward, the Watch post): its top is road (roadAt), a high place for guards
    bool watchPostAt(int cx, int cz) const; // a built Watch post: the guards' first post, and a longer reach from it
    // A ward's top in roadAt's terms (a deck origin's height: 0.1 over the top, which is at a deck's height).
    // A walkable tower's top as a deck ORIGIN height (0.1 over the walk surface): every tower's top is 0.25 over its origin
    // (a ward: 2 m over the ground, a deck's height; the Watch post: 4 m).
    float wardTopY(const Ref& w) const { return w.entity->pos.y + 0.25f + (spawnHeightOf(EStructureType::Walkway) - WalkwayDeckHeight); }
    bool railEdgeBetween(int ax, int az, int bx, int bz) const; // a landed ship's rail piece between two neighbour cells
    bool guardWanted(int index, uint8 team) const;
    // THE CANNON'S CREW: walkers too. While an enemy unit is within "Crew call range" of a Cannon (Ref::alarm), free
    // workers walk to it and MAN it (it fires only then). One of them keeps it armed: a trip to the nearest Ammo store /
    // Ammo works for munitions while another one stays at the gun (a lone one goes when the gun is empty). See "The
    // Cannon and munitions" in Docs/Context/Game/Economy.md.
    bool crewWanted(int index, uint8 team) const;
    int crewAmmoSource(int fromStructure, glm::ivec2 fromCell, uint8 team, oc::vector<glm::vec3>& outPath) const;
    bool guardPost(glm::ivec2 fromCell, int armoryIdx, size_t self, oc::vector<glm::vec3>& outPath) const;
    void setGuardLook(Walker& w, bool guard);
    int m_carrierCount[GameMaxTeams] = {}; // walkers on the road per team (the HUD)
    float m_carrierPassTimer = 0.0f;       // the ~1 s pass: sendReady, and the houses send out / call back workers
    void tickCarriers(float deltaSec);
    void carrierHousePass();
    // throughGates: a built GATE with road on two opposite sides is road too (the walkers pass it); off for the test of
    // those sides, so two gates side by side do not ask each other.
    bool roadAt(int cx, int cz, float& outY, bool throughGates = true) const;
    bool touchesRoad(int index) const;
    // WHAT A LOAD IS FOR, in the order a walker takes work: Goods = the buildings that take goods (never a blueprint);
    // Build = ONLY the blueprints that still need stone (blueprintNeed; rubble too) - the walkers BUILD along the road,
    // the road's own pieces too; Repair = ONLY the damaged built structures (repairNeed), the last work of all.
    // Transport = a CART load ("Cart load") from one STORAGE to another one of the same good (Storehouse, Wood store,
    // Beacon - transportRoleOf Storage on both ends, nothing else): it evens out the fill of the stores of a base.
    enum ECarrierWork : uint8 { CarrierWork_Goods, CarrierWork_Build, CarrierWork_Repair, CarrierWork_Transport };
    // The cart load of `good` that goes from storage sourceIdx to storage targetIdx: what brings the two to the same
    // fill, at most a cart; 0 = no trip (not two storages of the good, fills too close, or too little to move).
    float transportAmount(int sourceIdx, int targetIdx, int good) const;
    bool carrierAccepts(int sourceIdx, int targetIdx, int good, bool consumersOnly, int work, bool& isConsumer) const;
    // The stone a blueprint still needs to be built / a damaged structure to be whole, less what the walkers already
    // carry to it.
    float blueprintNeed(const Ref& ref) const;
    float repairNeed(const Ref& ref) const;
    // ONE breadth-first search over the road cells, from a structure's footprint (fromStructure >= 0) or from one road
    // cell. goal(roster index) of every building a visited cell touches: 2 = take it (the search ends), 1 = keep the
    // first such as the fallback, 0 = no. Returns the roster index (-1 = none) and the walking path to its road cell.
    // Both walk THE ROAD GRAPH (ensureRoadGraph), with the visited marks of their depth (a search inside a search's cost: two).
    int roadSearch(int fromStructure, glm::ivec2 fromCell, const oc::function<int(int)>& goal, oc::vector<glm::vec3>& outPath) const;
    // The same search, but it takes the building of the LOWEST cost: cost(roster index, road steps to it) >= the steps, or
    // < 0 = no. It stops once the steps alone pass the best cost (and at maxSteps).
    int roadSearchBest(int fromStructure, glm::ivec2 fromCell, const oc::function<float(int, int)>& cost, oc::vector<glm::vec3>& outPath,
        int maxSteps = INT_MAX) const;
    // Where a load of `good` from sourceIdx goes: the NEAREST taker by road, a consumer or a storage alike.
    int findRoadTarget(int sourceIdx, int fromStructure, glm::ivec2 fromCell, int good, bool consumersOnly, int work,
        oc::vector<glm::vec3>& outPath) const;
    // Another storage of the team that holds a load of `good` within maxSteps of the consumer by road (findRoadTarget).
    // From m_nearStores: one search per consumer and good a tick, not one per question.
    bool nearerStorage(int consumerIdx, int sourceIdx, int good, int maxSteps) const;
    bool carrierSendGood(int index, int good, bool& consumersOnly) const; // does it have a load of `good` to send?
    bool carrierHoldsStone(int index) const;                              // stone a walker can take to a blueprint
    // THE JOB PICK: the building as a walker's job, as road cells added to the walk there (-1 = no job for a walker).
    float carrierJobCost(int index, uint8 team) const;

    // ---- The road graph (the carriers' searches) ----
    // Every road cell (roadAt) as a node with its four links, built again on the first search after the LAYOUT changed
    // (m_layoutVersion: every markLinksDirty, and the end of a network rebuild - the walkways' pieces). A search walks the
    // links: no hash lookup per cell. A blueprint a walker finishes is road for its next search at once (investMaterials
    // marks the layout).
    struct RoadNode
    {
        int cx, cz;
        float y;           // roadAt's height
        int next[4];       // the road node through each side (the carriers' c_neighbours order; -1 = none, or a ship's rail between)
        int touches[4][2]; // roster indices of the structures in that neighbour cell, occupant and under-cable (-1 = none)
    };
    struct RoadVisit { int node, parent, steps; }; // node -1: the search's own start cell (a guard's Watch post: no road)
    struct RoadSearchScratch
    {
        oc::vector<uint32> stamp; // == gen: visited in this search
        uint32 gen = 0;
        oc::vector<RoadVisit> open;
    };
    void ensureRoadGraph() const;
    void roadLinks(int cx, int cz, RoadNode& node) const; // node.next / touches of the cell (m_roadNodeAt must be complete)
    mutable oc::vector<RoadNode> m_roadNodes;
    mutable oc::unordered_map<uint64, int> m_roadNodeAt; // cellKey -> road node
    mutable uint64 m_roadGraphVersion = UINT64_MAX;
    mutable RoadSearchScratch m_roadScratch[2];
    mutable int m_roadSearchDepth = 0;
    uint64 m_layoutVersion = 0;
    void markLinksDirty() { m_linksDirty = true; ++m_layoutVersion; } // a structure came, went, was built or fell: the networks and the road
    // The two nearest storages of a good with a load to send, by road from a consumer (index -1 = none), for this tick
    // (m_carrierTick). Slot = consumer roster index x NumStores + good.
    struct NearStores { uint32 tick = 0; int index[2] = { -1, -1 }; int steps[2] = {}; };
    mutable oc::vector<NearStores> m_nearStores;
    uint32 m_carrierTick = 0;

    // ---- CELL OCCUPANCY + DERIVED LINKS ----
    // One entry per occupied grid cell. `id` is the primary occupant; `underId` is set only on a
    // Crossing's MIDDLE cell - the perpendicular cable passing under it.
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
    // co-op rock rects and the resource nodes' own ground. Shared by cellsFree and
    // planCrossing, which only differ in what they make of an OCCUPIED cell.
    bool footprintAreaClear(EStructureType type, const glm::vec3& p, const glm::quat& rot) const;
    // (cx, cz) is the raised MIDDLE cell of the crossing at frame index `index`.
    bool isCrossingCenter(int index, int cx, int cz) const;
    // The occupant at frame index `index` may pass UNDER a crossing's middle at (cx, cz): a plain
    // cable, or a crossing whose END cell this is - an end counts as cable for bridging.
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
    void updateWalkway(const Ref& s); // a Walkway's deck or stairs piece, by its neighbours (and the deck's tilt)
    int8 walkwayModeOf(const Ref& s) const; // -2 no piece (a blueprint), -1 the deck, 0..3 stairs toward +X, -X, +Z, -Z
    void solveWalkwayDecks(); // every deck's tilt (Ref::walkLiftWant / walkSlopeWant), after the walkWant pass
    // A Walkway deck's walk height at a point of its cell: its origin's (0.1 over the deck top) with the tilt.
    static float walkwayDeckY(const Ref& r, const glm::vec3& at)
    {
        return r.entity->pos.y + r.walkLift + glm::dot(r.walkSlope, glm::vec2(at.x - r.entity->pos.x, at.z - r.entity->pos.z));
    }

    // ---- THE CABLE TRANSPORT (Transport.cpp) ----
    // Resources move over the cables as WHOLE CELLS. Every built cable segment and conducting
    // crossing is a NODE with an integer fill and an out-rate; a built building that holds a
    // medium is a JUNCTION node (its port: every segment touching it is a neighbour, so a line
    // through an emitter carries through), and the building's SLOT hangs on that node. Each
    // transport tick (fixed rate, runs staggered over frames) the game INJECTS every slot's
    // supply/demand from the building's float store, and a JOB ticks the due runs IN PARALLEL:
    // per run two BFS fields (hops to the nearest wanting port, hops to the nearest pushing
    // port), then `Substeps` of owner-only stencil passes - OFFER: a node serves its slots'
    // demand first, then FORWARDS into neighbours with free space that are downhill toward
    // demand, else uphill away from supply (filling up), then takes slot supply into its free
    // space, all within its out-rate; APPLY: fill' = fill - out + in - and the join hands the
    // cells to the stores. No atomics, no entity walk, no TLS, the result is independent of
    // scheduling, and a run's bottleneck is simply its slowest segment.
    // Node adjacency is CSR (a junction has any number of neighbours); each adjacency slot knows
    // the reverse slot so APPLY reads its inflow from the neighbours' own out-slots.
    struct TransportNode
    {
        uint32 structureId = 0;    // the cable/crossing this node is - or the bridged building's id
        uint32 adjFirst = 0, adjCount = 0;
        uint32 slotFirst = 0, slotCount = 0; // the building slots on this (junction) node
        uint32 rateFp = 0;         // out budget per SUB-STEP, 1/1024 cell (junctions: x degree)
        uint32 carryFp = 0;        // fractional budget carried between sub-steps
        uint16 fill = 0;           // cells held - may transiently exceed the segment capacity (soft)
        uint16 moved = 0;          // cells that left this node last tick (gauge)
        float movedAvg = 0.0f;     // cells/s, EMA over ~2 s of `moved` (the label's throughput)
        uint16 run = 0;
        uint8 medium = 0;
        uint8 junction = 0;        // a building's port node: never saved or mirrored
        uint8 rotate = 0;          // integer-remainder rotation counter
        uint16 dist = 0xFFFF;      // THE DEMAND FIELD: hops (through cables) to the nearest port that
                                   // wants cells this tick; 0xFFFF = none. Cells move strictly
                                   // DOWNHILL on it - toward demand - first
        uint16 sdist = 0xFFFF;     // THE SUPPLY FIELD: hops to the nearest port pushing cells this
                                   // tick. With no demand outlet, cells move strictly UPHILL on it -
                                   // away from supply - so cables fill up outward from producers
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
        int8 storageMode = 0;         // storage: +1 pulling, -1 pushing, 0 undecided - flips only at the
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
                                     // its queue (owner-only, so runs tick in parallel - no TLS)
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
    // A store's rank in the trade between stores (EStorePriority): Drain 0 < Balance 1 < Fill 2. Equal ranks level
    // their fills; otherwise the goods go one way only, from the lower rank to the higher one.
    static int storeRank(const Ref& ref)
    {
        if (!hasStorePriority(ref.type))
            return 1;
        const EStorePriority p = (EStorePriority)ref.state->storePriority;
        return p == EStorePriority::Fill ? 2 : p == EStorePriority::Drain ? 0 : 1;
    }

    oc::unordered_map<uint64, CellEntry> m_cells;
    bool m_linksDirty = true;
    float emitterOutputOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_settings.bastionOutput
             : t == EStructureType::Base ? m_settings.baseShieldOutput : m_settings.emitterOutput;
    }
    float emitterDrawOf(EStructureType t) const
    {
        return t == EStructureType::Bastion ? m_settings.bastionEnergyPerSec
             : t == EStructureType::Base ? m_settings.baseShieldEnergyPerSec : m_settings.emitterEnergyPerSec;
    }

    oc::vector<Ref> m_frame;                 // the persistent roster (owning refs - see Ref)
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
    bool m_terrainGround = false;           // the sandbox world (setTerrainGround)
    GroundSource m_groundSource = nullptr;  // setGroundSource
    HarvestSource m_harvestSource = nullptr; // setHarvestSource
    int m_harvestRetry = 0;                  // resolveHarvestYields: calls until the next try
    static constexpr int MaxHarvestCluster = 64; // yards in one cluster that share (a bit each); the rest of it gets nothing more
    // The amount (trees, or m of rock) each yard of a cluster gets, in the order given = the order they are served.
    // false = the source has no answer for a circle.
    bool shareHarvest(bool lumber, oc::span<const glm::vec2> yards, oc::span<float> got) const;
    // The yards of `type` that share with yard `seed` of `centres` (its cluster), in the order of `centres`.
    static void harvestCluster(oc::span<const glm::vec2> centres, int seed, float radius, oc::vector<int>& out);
    float harvestFull(bool lumber) const { return glm::max(lumber ? m_settings.harvestTreesFull : m_settings.harvestStoneFull, 1.0f); }
    bool harvestNodeNear(bool lumber, glm::vec2 pos) const; // the flat maps: a deposit of the kind in the radius (a full yield, not shared)
    static constexpr float c_nodeHeight = 0.8f; // a resource node entity's center above its ground

    int m_workers[GameMaxTeams] = {}; // see staffing()
    int m_jobs[GameMaxTeams] = {};
    float m_minerals[GameMaxTeams] = {};
    float m_fuelTotal[GameMaxTeams] = {};
    float m_gridEnergyTotal = 0.0f;
    float m_gridCapacityTotal = 0.0f;
    float m_genRateTotal = 0.0f;
    float m_useRateTotal = 0.0f;

    // The tuning: costs, the economy, the cable transport (the tick rate/substeps/spread re-dirty the links
    // through the listeners StructureSystem() attaches), the shield structures, barracks and houses. The
    // MEDIC STATION's reach/rate are GameStructureParams (its component update heals own-team units).
    GameStructureSettings& m_settings = Globals::settings.game.structures;
    int cellsPerSegmentOf(int medium) const { return glm::max(m_settings.cellsPerSegment[glm::clamp(medium, 0, 2)], 1); }
    float m_projectileStructDamage = 20.0f;
};

// Debug-line helpers shared by the Game implementation units (Structures, Npc, the Match*.cpp
// files). Module linkage, NOT exported; bodies in Structures.cpp.
uint32 packColor(const glm::vec3& c);
// A field structure's shell alpha from its energy charge (0..1): steady above "Low charge warning", below it a flicker
// that gets deeper and faster as the store empties. `id` offsets the phase, so two shells never flicker in step.
float emitterShellAlpha(float charge, uint32 id, float warnCharge);
void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments = 32);

// StructureSystem's bodies, all `module Game;` implementation units (Match.ixx lists its own the
// same way):
//   Structures.cpp          - the type tables, describeType, the settings listeners, refresh / stampTuning,
//                             clear, the nodes, request queueing, tickAuthority / tickMirror
//   StructuresPlacement.cpp - the grid (snap, footprints, the cell hash, cellsFree / planCrossing),
//                             spawnStructure / destroy / placeStructure / spawnBase / demolish
//   StructuresNetwork.cpp   - rebuildNetworks (union-find, crossings, bridging, the transport
//                             graph) + updateArms
//   StructuresEconomy.cpp   - tickProduction, the death sweep, constructors, materials / repairs,
//                             the tint, drawDebug
//   Carriers.cpp            - the walkers that move wood and stone on the road (tickCarriers, findRoadTarget)
//   Light.cpp               - light on beams: the derived links to the hubs, their flow, their lines
//   Wards.cpp               - the wards: their wall links, what they pay, the pieces on the walls
//   StructuresSync.cpp      - the mirror* appliers, saveTo / loadFrom / clearAllStructures
//   StructuresShip.cpp      - the structures on the ship's deck (their pose, the landing edges, the deck placement, the rails)
//   Transport.cpp           - the cable transport tick (see the block above)
