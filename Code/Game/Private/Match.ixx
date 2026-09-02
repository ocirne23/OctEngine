export module Game:Match;

import Core;
import Core.glm;
import Core.Camera;
import Entity;
import Force;
import Input;
import Nav;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// The match orchestrator: owns the whitebox world (ground, objective, world-scale enemy emitter),
// the player, the structure/economy system and the follow camera. MUST be a stack local in main()
// (holds EntityPtrs and Force handles — a global would need an InitSeg slot).
//
// Two ticks, both main thread:
//  - update(dt): the AUTHORITY tick — placement drain, capture, grids/power, damage, player
//    movement + shield, world-field suppression, win check. Slots into the main loop between
//    networkManager.receive() and physics.update() (direct body setters sanctioned there); this is
//    the half that becomes the server tick in multiplayer.
//  - updateWindowed(camera, dt): camera overwrite, aim/placement input, ghost + debug draw, HUD.
//    Runs right after InputControls::applyPlayerCamera in the windowed block.
// The PvP ARENAS (the lobby's "Map" pick; the host's choice reaches clients through the GMp event
// exactly like the co-op seed). Every arena is impassable ROCK terrain on the shared cell grid
// (GameMatch::generatePvpGrid): a rock border ring replaced the old borderwall.pre fence.
export enum class EPvpMap : uint8
{
    Lane,        // the original corridor (x ±65, z ±20)
    WideLane,    // twice as wide (z ±40)
    Chokepoints, // the wide lane with a middle wall pierced by three 10 m openings
    Circle,      // a ring arena: central rock column inside a 65 m disc (a "0")
    Count,
};
export constexpr const char* pvpMapName(EPvpMap map)
{
    switch (map)
    {
    case EPvpMap::Lane:        return "Lane";
    case EPvpMap::WideLane:    return "Wide lane";
    case EPvpMap::Chokepoints: return "Chokepoints";
    case EPvpMap::Circle:      return "Circle";
    default:                   return "?";
    }
}

export class GameMatch final
{
public:
    // PvP (default): each client plays on its own Force team slot, everything a player builds
    // belongs to their team, and minerals/fuel are per-team.
    // CO-OP (`--game --coop`, clients pass both too — the world layout is built locally): its own
    // GENERATED world — no corridor, a much bigger square of seeded impassable rock terrain with
    // carved attack lanes and a player-only barrier ring at the edge (see CoopMap below). Every
    // player on team 0 around ONE central Base; the AI team (CoopAiTeam) has unleashed units
    // scattered over the reachable map and sends periodic SWARM waves (They-are-Billions style:
    // hundreds of cheap shield-less bodies, trickle-spawned) at the Base from beyond the barrier
    // on a random compass side. Authority-simulated; units reach clients through normal entity
    // replication.
    explicit GameMatch(bool enabled, bool coop = false);
    ~GameMatch();

    void spawnWorld();
    // Co-op map inputs chosen OUTSIDE (the lobby page's host settings): call before spawnWorld on
    // the authority. Writes the same variables the "Game/Coop" tweaks bind, so the panel shows
    // what generated; seed 0 still rolls a random map.
    void setMapSettings(uint32 seed, float fill, int lanes)
    {
        m_mapSeedTweak = (int)(seed & 0x7fffffffu);
        m_terrainFill = fill;
        m_terrainLanes = lanes;
    }
    // PvP team setup chosen OUTSIDE (the lobby): the host's team count (2..GameMaxTeams — one
    // Base per team, spread along the corridor) and the roster's picks (clientId, team; clientId
    // 0 = the server's own team). Call before spawnWorld on the authority. A client whose id is
    // not in the list (a late joiner) is seated on the least-populated team at join.
    void setLobbyTeams(uint8 numTeams, oc::span<const oc::pair<uint32, uint8>> picks);
    // The PvP arena (the lobby's "Map" pick): call before spawnWorld on the authority; clients
    // build whatever the server's GMp event names.
    void setPvpMap(EPvpMap map) { m_pvpMap = map; }
    // The PLAYER/CAMERA hot path, and nothing else: capsule adoption (client) + velocity steering +
    // the shield's body push — the direct body setters that must land BEFORE this frame's physics
    // step. Deliberately minimal so main reaches the spatial/begin-frame kicks as early as possible.
    void updatePlayer(float deltaSec);
    // The REST of the game tick, called AFTER the spatial/begin-frame joins (spawns/destroys and
    // spatial queries are legal again there; still before contact dispatch + the entity pass):
    // structures authority/mirror, materials, unit production, base healing, nav staging, net
    // flushes. Consequences of the placement: freshly spawned actors link into the spatial index
    // at the NEXT commit (the spawn guard keeps them visible), new bodies' velocities integrate on
    // the NEXT step, and feedNav's staging feeds the NEXT frame's NavSystem::update.
    void update(float deltaSec);
    void updateWindowed(Camera& camera, float deltaSec);

    bool enabled() const { return m_enabled; }

    // PROFILING SCENARIO (`--scenario <save>`, main calls it once at `--scenario-frame`): loads the
    // save (F10's path when empty), selects EVERY live own-team unit and orders them all to the
    // other team's Base — a repeatable crowd-pathing load without anyone at the keyboard.
    // Authority only (units simulate on the server). The load happens now; the select + order run
    // from the next update() (the loaded units' spatial entries link at the next commitFrame).
    bool runScenario(oc::string_view savePath);

    // MULTIPLAYER (windowed listen server + clients). The SERVER runs the whole sim; player-
    // structure state mirrors to clients over game events (GPl/GRm + periodic GSt stats; links
    // derive locally on every instance from the mirrored cable segments — no cable wire);
    // units and shots replicate as network entities (Component Network in their prefabs); each
    // client drives its own server-spawned capsule through the claim system and computes its own
    // shield locally (the mirrored emitter fields exist client-side, so readbacks are real).
    // Client build inputs travel as Gq* requests, validated server-side.
    void onClientJoined(uint32 clientId); // main.cpp routes the manager callbacks here (server)
    void onClientLeft(uint32 clientId);

    // Network game events (GPl/GSt/Gq*/...). main.cpp owns the ONE setOnGameEvent hook (it also
    // routes the lobby's "Lb*" traffic) and forwards everything else here — GameMatch must NOT
    // install its own hook: main's dispatcher constructs a lobby client's GameMatch from INSIDE a
    // dispatch, and replacing the oc::function there would destroy the lambda mid-execution.
    void handleNetEvent(oc::string_view name);

    // TRUE while Esc still has an in-game meaning (a pending two-click flow, an armed item, an
    // open hotbar page, or a non-Select mode — cancelOneLevel would consume it). main's ESCAPE
    // MENU only opens on an Esc press when this is false, so the cancel chain keeps first claim.
    bool escWouldCancel() const
    {
        return m_lanceAiming || m_wallPlacing || m_cablePainting
            || m_buildSelection >= 0 || m_buildCategory >= 0 || m_mode != EPlayerMode::Select;
    }

private:
    struct Aim
    {
        bool valid = false;
        bool affordable = false;
        glm::vec3 pos{ 0.0f };
        EStructureType type = EStructureType::Emitter;
        int nodeIndex = -1; // Extractor: the free node the ghost snapped to
    };
    // Interaction modes: Select is the neutral mode (click inspects, RMB routes/moves); Q/W/E open
    // the build categories (Combat/Production/Cables), X = Delete. Cables place like any other
    // item — the old two-click LINK tools are gone (connections derive from cable adjacency).
    enum class EPlayerMode : uint8 { Build, Delete, Select };

    Aim computeAim(const Camera& camera, EStructureType type) const;
    bool aimGroundPoint(const Camera& camera, glm::vec3& outPos) const; // cursor ray vs colliders/ground plane
    void refreshBuildHotbar(); // repopulates slot labels/counts for the current grid level
    void updateModeSwitching();
    void updateBuildMode(const Camera& camera, bool confirmEdge, bool cancelEdge);
    void disarmBuild(); // drop the armed item + any half-finished two-click flow (RMB / Esc)
    void activateSlot(int slot); // grid hotkey OR click on the drawn slot: category / item / Delete / Cancel / Back
    void cancelOneLevel();       // C slot, Esc, Tab: two-click step -> armed item -> page/mode, one per press
    // Cable segments place by PAINTING: LMB press places a cell, holding + dragging keeps placing
    // the cells the cursor crosses (L-filled between samples so the run stays connected).
    void updateCablePlacement(const Camera& camera, EStructureType armed, bool confirmEdge);
    void placeCableLine(EStructureType armed, const glm::vec3& from, const glm::vec3& to, bool preview);
    void updateDeleteMode(const Camera& camera, bool confirmEdge);
    void updateSelectMode(const Camera& camera, bool confirmEdge, bool rmbEdge);
    // Shared click-to-select (Select mode, and Build mode wherever the click can't place).
    void updateSelectionClick(const Camera& camera, bool confirmEdge, bool allowPick);
    // Shared RMB handling (both modes): barracks route on ground (linking is the CONN tool now).
    void updateRightClickActions(const Camera& camera, bool rmbEdge);
    int hoveredStructure(const Camera& camera) const; // structure index under the cursor, or -1
    void setMode(EPlayerMode mode);
    void buildWorldLabels(const Camera& camera); // health bars + selected info over structures
    void updateHud();
    void tickBaseHealing(float deltaSec); // own player only — the owner computes its own health
    void tickPlayerMelee(float deltaSec); // AUTHORITY: every player capsule grinds adjacent enemy units
    void requestPlace(EStructureType type, const glm::vec3& pos, int nodeIndex, const glm::vec3& facing);
    void requestDemolish(uint32 id);
    void requestSetRoute(uint32 id, oc::span<const glm::vec3> points); // barracks waypoints
    void sendRoute(int index); // server: GRt broadcast (mirror + join replay)
    void requestSetUnitType(uint32 id, uint8 unitType); // barracks: the produced unit type (popup)
    void sendUnitType(int index); // server: GBu broadcast (mirror + join replay)
    void sendStructurePlaced(int index);
    void sendStats();
    // NAV: feed the flow-field service (authority only) — obstacles = rock terrain + every
    // structure footprint (change-detected inside Nav), sources = per team its structures + player
    // bodies (every frame). Units read the fields inside the entity pass.
    void feedNav();
    // SAVE/LOAD (F9/F10, server/single player only — clients refuse): structures/cables/units for
    // all teams to Assets/Local/gamesave.txt (players are NOT saved). Loading clears the current
    // set (removal hooks -> GRm prune connected clients) and re-broadcasts the loaded state.
    void saveGame();
    void loadGame(oc::string_view path = {}); // empty = the F10 path (Local/gamesave.txt)

    GamePlayer m_player;
    GameCamera m_camera;
    StructureSystem m_structures;
    NpcSystem m_npcs;

    EntityPtr m_ground;
    oc::vector<Nav::NavObstacle> m_wallObstacles; // rock terrain rects (static, both modes)
    oc::vector<Nav::NavObstacle> m_navObstacles;  // per-frame scratch: walls + structures
    oc::vector<Nav::NavSource> m_navSources[Nav::MaxTeams];
    // Lane seeding parameters live on NpcSystem (one set of tweaks); Match reads them for its own
    // route/order seeding.
    float laneSeedSpeed() const { return m_npcs.orderLaneSpeed(); }
    float laneSeedWidth() const { return m_npcs.laneWidth(); }

    MouseListenerHandle m_mouse; // caches window-space mouse pos, wheel + RMB drag accumulation
    glm::vec2 m_mousePos{ 0.0f };
    float m_dragDeltaX = 0.0f; // MIDDLE-held horizontal pixels this frame (consumed by the camera)
    float m_wheelAccum = 0.0f;
    bool m_mmbDown = false;
    bool m_rmbDown = false;
    bool m_rmbConsumed = false;  // an RMB action (cancel/connect/route) ate this frame's click, so
                                 // it must NOT also become a move order
    bool m_rmbMoveDrag = false;  // this RMB hold started as a move order: keep steering at the
                                 // cursor until the button comes up
    bool m_placeClicked = false; // LMB edge inside the viewport (consumed by the active mode)
    bool m_rmbClicked = false;   // RMB edge inside the viewport (route waypoint / move order)
    EPlayerMode m_mode = EPlayerMode::Select; // Select IS the neutral mode (player combat removed)
    uint32 m_selectedId = 0;     // Select mode: highlighted structure (stable id; 0 = none)
    // UNIT SELECTION (RTS box drag in Select mode AND in Build with nothing armed): owning handles to own-team units; RMB move
    // orders go to them together with the player. Authority only (units simulate on the server).
    oc::vector<EntityPtr> m_selectedUnits;
    glm::vec2 m_lmbDownPos{ 0.0f };
    bool m_lmbDown = false;
    bool m_lmbReleased = false;  // release edge (consumed by updateWindowed)
    void updateUnitSelection(const Camera& camera);
    bool orderSelectedUnits(const glm::vec3& target, bool freshOrder); // true = a lane was seeded (fresh order + A* found a route)
    bool moveOrderAt(const glm::vec3& worldPos); // THE RMB move order: player + selected units walk there (fresh order, lane seeded); returns orderSelectedUnits'
    glm::vec3 pointOutsideFootprint(const glm::vec3& clicked, int structure) const; // a click on a building -> the reachable point at its face
    bool m_scenarioOrderPending = false; // runScenario loaded; issueScenarioOrder retries each update until units are queryable
    uint32 m_scenarioOrderTries = 0;
    void issueScenarioOrder();           // select ALL own units + move order on the other Base
    void seedRouteLane(uint32 structureId); // barracks route -> a planned lane in the flow field
    // (While an ordered group walks, its lane is kept fresh by the UNITS' own periodic plan
    // requests — Nav's proximity dedup makes the whole group cost one plan. See
    // GameUnitComponent's SeedRequest and NavSystem::requestSeedPath.)
    float m_selectionClusterRadius = 12.0f; // link radius of the selection's cluster centroid
    void pruneSelectedUnits();
    bool m_modeKeyWasDown[1] = {}; // Esc/Tab edge
    bool m_saveKeyWasDown = false; // F9/F10 edges (save/load game state)
    bool m_loadKeyWasDown = false;
    int m_buildCategory = -1;    // grid hotbar page: -1 = ROOT (categories), else index into the categories
    int m_buildSelection = -1;   // armed item within the category (-1 = nothing armed, no ghost)
    bool m_gridKeyWasDown[12] = {}; // QWER/ASDF/ZXCV edges (polled — one per hotbar slot)
    bool m_lanceAiming = false;  // Lance two-click placement: first click anchored, awaiting facing
    glm::vec3 m_lancePendingPos{ 0.0f };
    bool m_wallPlacing = false;  // Wall drag placement: the press anchored the line start, release places
    glm::vec3 m_wallStart{ 0.0f };
    // Crossing orientation: the long axis follows the CAMERA facing (quantized to ±X/±Z);
    // pressing/clicking the armed CRSS slot again rotates it 90°.
    bool m_crossingRotated = false;
    bool m_cablePainting = false;  // LMB held: paint cells as the cursor crosses them
    glm::vec3 m_cablePaintLast{ 0.0f };

    // TEAMS are SLOTS, never derived from the clientId: ids are minted monotonically and never
    // recycled (a reconnect or a failed first attempt burns one), so the second connection of the
    // same friend used to land on team 2 — a team with no Base. The map spawns one Base per
    // playable team (m_numTeams, see spawnWorld), and a player without a Base has no respawn
    // anchor, no mineral bank and no healing, so the slot pool is exactly that many.
    uint8 m_numTeams = 2;                                  // PvP playable teams (the lobby's count)
    oc::vector<oc::pair<uint32, uint8>> m_lobbyTeams;      // the lobby's picks (see setLobbyTeams)
    glm::vec3 baseGroundPos(uint8 team) const;             // PvP: that team's Base cell on the corridor
    uint8 allocateClientTeam(uint32 clientId) const;       // the lobby pick, else the least-populated team
    int clientTeam(uint32 clientId) const; // -1 = unknown client (no capsule yet); 0 = the server
    // The team a client's build/demolish/cable request acts as. Callers must reject unknown
    // senders first (handleNetEvent does) — this clamps rather than failing.
    uint8 requestTeam(uint32 clientId) const { return (uint8)glm::max(clientTeam(clientId), 0); }
    glm::vec3 teamStartPos(uint8 team) const; // spawn/respawn anchor beside that team's Base

    // ---- CO-OP (see the constructor comment) ----
    // The ambient/wave team. Team 1, NOT a high slot: co-op runs the Force system at TWO live
    // teams (setNumTeams(2) — the renderer's per-team costs shrink to fit), so every team index
    // must stay below 2. (< Nav::MaxTeams as well.)
    static constexpr uint8 CoopAiTeam = 1;
    // GENERATED MAP: impassable rock over a coarse cell grid (seeded noise + carved attack lanes),
    // a player-blocking barrier ring at the edge, and everything placed on it (resource nodes,
    // ambient spawns) guaranteed reachable from the Base by a flood fill that turns unreachable
    // open pockets into rock. The SERVER picks the seed (tweak "Map seed", 0 = random) and
    // broadcasts it as the "GMp" event (first thing in onClientJoined, before the structure
    // replay; also into F9 saves) — clients defer terrain + nodes until it arrives and build the
    // identical set locally from pure seeded math.
    // THE TERRAIN GRID serves BOTH modes: the co-op generator fills a 36² grid of 10 m cells, the
    // PvP arenas (generatePvpGrid) a per-arena rectangle of 5 m cells (rock.pre spawned at half
    // scale) — everything downstream (cell math, flood fill, rects, rock spawn, clampToOpenGround)
    // reads the grid's own dimensions.
    struct CoopMap
    {
        oc::vector<uint8> blocked;  // one per cell, row-major z * cellsX + x (1 = rock)
        oc::vector<uint16> depth;   // BFS steps from the Base over open cells (0xFFFF unreachable)
        oc::vector<int> reachable;  // open + reachable cell indices (node/ambient placement pool)
        int cellsX = 0, cellsZ = 0; // grid size
        float cellSize = 10.0f;     // one rock block (co-op 10 m, PvP 5 m)
        glm::vec2 origin{ 0.0f };   // world (x, z) of cell (0, 0)'s min corner
        uint16 maxDepth = 1;
        bool pvp = false;           // which generator built it (the no-op tests compare per mode)
        EPvpMap pvpMap = EPvpMap::Lane;
        uint32 seed = 0;
        float fill = 0.3f;          // generation inputs as USED (ride GMp + the save with the seed:
        int lanes = 6;              //  a joiner's tweak sync lands after the replay, too late)
        bool built = false;
    };
    // Both roles; no-op when already built from those exact inputs.
    void rebuildCoopMap(uint32 seed, float fill, int lanes);
    void rebuildPvpMap(EPvpMap map);
    void generateCoopGrid();             // the pure-math half: blocked cells, flood fill, rects
    void generatePvpGrid();              // the PvP arena layouts (+ rock border), same outputs
    void finishGrid(oc::span<const int> seedCells); // BFS from the seeds, seal pockets, merged rects
    void spawnTerrain();                 // rocks (+ the co-op barrier) under m_terrainRoot, nodes
    void spawnPvpNodes();                // the arena's node table (skips cells that landed in rock)
    void sendMapSeed();                  // server: the GMp broadcast (co-op inputs / the PvP arena)
    glm::vec3 coopCellCenter(int index) const;
    int coopCellAt(const glm::vec3& pos) const; // -1 outside the map square
    // A move destination inside a rock is unreachable (the A* and every unit plan to it fail):
    // clamp it to the nearest open cell center. Identity when the map is absent or the cell open.
    glm::vec3 clampToOpenGround(const glm::vec3& pos) const;
    glm::vec3 ambientPointNear(const glm::vec3& center, float radius) const; // ambient body spot on open ground
    void drawCoopBarrier(); // pulsing energy lines strung between the barrier posts
    // The AMBIENT scatter spawns in small groups: the trickle fills the current group (one
    // archetype, a disc around a reachable anchor) before rolling the next — loose blobs, not a
    // lattice.
    struct AmbientSpawn
    {
        glm::vec3 center{ 0.0f };
        float radius = 6.0f;
        int remaining = 0;  // bodies still to place in this group (0 = roll a new one)
        int archetype = 0;  // index into c_waveArchetypes
    };
    AmbientSpawn m_ambientSpawn;
    CoopMap m_coopMap;
    EntityPtr m_terrainRoot; // rocks + barrier segments live under it; removed on regeneration
    oc::vector<glm::vec4> m_terrainRects; // merged blocked runs (minX, minZ, maxX, maxZ)
    EPvpMap m_pvpMap = EPvpMap::Lane; // PvP arena (setPvpMap; clients follow GMp)
    glm::vec2 m_pvpInterior{ 65.0f, 20.0f }; // the arena's open half-extents (placement bounds)
    int m_mapSeedTweak = 0;       // "Game/Coop/Map seed": 0 = random each run (authority only)
    float m_terrainFill = 0.3f;   // fraction of interior cells turned to rock (before carving)
    int m_terrainLanes = 6;       // carved attack lanes from the base ring to the map edge
    void tickWaves(float deltaSec);  // authority: the wave clock
    void queueWave();                // pick a compass direction, size the swarm, seed its lane
    void tickCoopSpawns();           // trickle: wave + ambient spawns on a per-frame budget
    // COMPOSITION recipes, sampled per spawned unit. The wave's is rolled once per wave in
    // queueWave: an ARCHETYPE (single-type rush, screened siege, combined arms, ... — gated by the
    // wave index so early waves stay simple) with its weights jittered. The AMBIENT scatter picks
    // its spawn position FIRST and gates the archetype roll by DISTANCE from the Base — the same
    // minWave gate, driven by depth instead of time, so the deep map holds the heavy recipes and
    // the near ring stays swarm-grade. Authority-only (clients never spawn).
    struct WaveMixEntry { ENpcType type; float weight; };
    ENpcType sampleMix(const oc::fixed_vector<WaveMixEntry, 4>& mix) const; // weighted type roll
    oc::fixed_vector<WaveMixEntry, 4> m_waveMix;
    int m_lastArchetype = -1;    // never the same recipe twice in a row (when a choice exists)
    bool m_coop = false;
    float m_waveTimer = 0.0f;    // seconds to the next wave (armed in spawnWorld)
    int m_waveIndex = 0;         // waves launched so far
    float m_wavePendingBudget = 0.0f;    // POINTS of the current wave still to spawn (trickled):
                                         // each spawned unit spends its type's cost (m_waveCost)
    float m_ambientPendingBudget = 0.0f; // POINTS of world-start scatter still to spawn (same costs)
    glm::vec3 m_waveOrigin{ 0.0f }; // the wave's cluster center on the spawn ring
    // Spacing: the last few wave spawn points, so a new roll can reject a spot inside a body that
    // was just placed (bodies are parked at spawn — an overlap there resolves only when a player
    // comes near, as a push-out in the player's face)
    static constexpr int c_waveRecentSpawns = 32;
    glm::vec2 m_waveRecent[c_waveRecentSpawns]{};
    int m_waveRecentCount = 0, m_waveRecentNext = 0;
    glm::vec3 m_waveDest{ 0.0f };   // the Base's near face on the incoming side
    // Tweaks ("Game/Coop", Synced):
    float m_waveFirstDelay = 30.0f;
    float m_waveInterval = 90.0f;
    // Waves are sized in BUDGET POINTS, not unit counts: each type has a cost (tweaks), so a
    // brute-heavy archetype fields far fewer bodies than a swarm flood of the same budget.
    int m_waveBudget = 20;           // points in wave 1 (swarm costs 1 = the old unit count)
    float m_waveBudgetGrowth = 40.0f; // extra points per subsequent wave
    float m_waveCost[(int)ENpcType::Count] = { 3.0f, 10.0f, 2.0f, 5.0f, 1.0f }; // Grunt, Brute, Runner, Spitter, Swarm
    float waveCostOf(ENpcType t) const { return glm::max(m_waveCost[(int)t], 0.1f); }
    int m_waveMaxAlive = 15000;    // total AI units cap (ambient + waves)
    int m_ambientBudget = 20000;    // POINTS of world-start scatter (same per-type costs as waves)
    float m_ambientSafeRadius = 45.0f; // the scatter keeps clear of the Base
    int m_spawnsPerFrame = 100;    // trickle budget — a huge wave enters over seconds, not one hitch

    bool m_enabled = false;
    uint32 m_team = 0;       // OUR team: 0 on server/single player; on a client it follows the
                             // team the SERVER assigned our capsule (read off its puppet
                             // GameUnitComponent, which the snapshot game blob carries)
    bool m_isServer = false; // co-op roles, latched in spawnWorld
    bool m_isClient = false;
    float m_statTimer = 0.0f; // server: GSt cadence
    int m_cableSyncCursor = 0; // server: GCb rotation start when more blueprint segments exist than fit one event
    oc::unordered_map<uint32, EntityPtr> m_clientPlayers;    // server: clientId -> their capsule
                                                              // (the ONLY per-client store — carried
                                                              // materials live on the twin's puppet
                                                              // component, damage owed in its inbox)
    float m_damageTimer = 0.0f; // server: GDm flush cadence

    // Corridor anchors (see the constructor): one Base at each end, clients spawn beside theirs.
    glm::vec3 m_basePos{ -55.0f, 0.0f, 0.0f };        // team 0's Base
    glm::vec3 m_playerStart{ -55.0f, 1.0f, -6.0f };   // just beside it = the respawn point
    glm::vec3 m_enemyBasePos{ 55.0f, 0.0f, 0.0f };    // team 1's Base; clients spawn beside it
    // Tweaks ("Game/Construction"): the player-inventory material loop.
    float m_refillRadius = 6.0f;    // metres from a Silo/Base within which the inventory refills
    float m_refillRate = 15.0f;     // materials/s pulled from the stores
    float m_buildRadius = 6.0f;     // metres from a blueprint within which a player invests
    float m_playerBuildRate = 8.0f; // materials/s a player invests into a blueprint
    // Tweaks ("Game/Player"): health regen while standing near an OWN-team Base. Computed by each
    // player's OWNER (health is owner-computed) against its local structure mirror — no sync.
    float m_baseHealRadius = 10.0f;
    float m_baseHealRate = 15.0f;   // health/s inside the radius
    float m_meleeDps = 10.0f;       // player melee aura: health/s to enemy units in melee range
    float m_meleeRadius = 2.5f;     // melee range (m, XZ from the capsule)
};
