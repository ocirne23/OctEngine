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

// One entry of the RTS grid hotbar (Build mode): the top level shows the CATEGORIES (keys 1..3);
// picking one repopulates the hotbar with its items (keys 1..N arm the ghost/tool, key 0 = Back).
// Partition-scope so Match.cpp's category tables can be plain file statics.
// The two-click LINK tools, armed straight off the root hotbar page (Link mode): click one
// structure, then the other. Connect creates (medium inferred by smartLinkTypeFor), Disconnect
// removes, Upgrade retypes Basic -> Heavy.
enum class EBuildTool : uint8 { Connect, Disconnect, Upgrade };

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
export class GameMatch final
{
public:
    // PvP only: each client plays on its own Force team (server = 0, client N =
    // min(N, GameMaxTeams-1)), everything a player builds belongs to their team, and
    // minerals/fuel are per-team.
    explicit GameMatch(bool enabled);
    ~GameMatch();

    void spawnWorld();
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
    // structure state mirrors to clients over game events (GPl/GRm/GCb + periodic GSt stats);
    // units and shots replicate as network entities (Component Network in their prefabs); each
    // client drives its own server-spawned capsule through the claim system and computes its own
    // shield locally (the mirrored emitter fields exist client-side, so readbacks are real).
    // Client build inputs travel as Gq* requests, validated server-side.
    void onClientJoined(uint32 clientId); // main.cpp routes the manager callbacks here (server)
    void onClientLeft(uint32 clientId);

private:
    struct Aim
    {
        bool valid = false;
        bool affordable = false;
        glm::vec3 pos{ 0.0f };
        EStructureType type = EStructureType::Emitter;
        int nodeIndex = -1; // Extractor: the free node the ghost snapped to
    };
    // Interaction modes: neutral by default — clicking does NOTHING until a mode key is pressed.
    // B/V/C jump STRAIGHT into Build mode with that category (Emitters/Production/Distribution) —
    // b->1->LMB, v->3->LMB style; pressing the ACTIVE category's key exits to neutral. X = Delete,
    // Tab = Select. The hotbar (visible only in Build) always shows the current category's items:
    // keys 1..N arm, 0 disarms.
    // Select is the neutral mode. Link = a two-click cable TOOL armed straight off the root
    // hotbar page (Connect on E, Disconnect on D, Upgrade on F) — no category page involved.
    enum class EPlayerMode : uint8 { Link, Build, Delete, Select };

    Aim computeAim(const Camera& camera, EStructureType type) const;
    bool aimGroundPoint(const Camera& camera, glm::vec3& outPos) const; // cursor ray vs colliders/ground plane
    void refreshBuildHotbar(); // repopulates slot labels/counts for the current grid level
    void updateModeSwitching();
    void updateBuildMode(const Camera& camera, bool confirmEdge, bool cancelEdge);
    void disarmBuild(); // drop the armed item + any half-finished two-click flow (RMB / Esc)
    void activateSlot(int slot); // grid hotkey OR click on the drawn slot: category / item / Delete / Cancel / Back
    void cancelOneLevel();       // C slot, Esc, Tab: two-click step -> armed item -> page/mode, one per press
    void updateLinkTool(const Camera& camera, bool confirmEdge, bool cancelEdge, EBuildTool tool);
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
    void handleNetEvent(oc::string_view name); // NetworkManager::setOnGameEvent target
    void requestPlace(EStructureType type, const glm::vec3& pos, int nodeIndex, const glm::vec3& facing);
    void requestCable(uint32 idA, uint32 idB, ECableType type);
    void requestDemolish(uint32 id);
    void requestSetRoute(uint32 id, oc::span<const glm::vec3> points); // barracks waypoints
    void sendRoute(int index); // server: GRt broadcast (mirror + join replay)
    void sendStructurePlaced(int index);
    void sendCableChanged(uint32 idA, uint32 idB, ECableType type, bool removed);
    void sendStats();
    void spawnCorridorWalls(); // arena border (deterministic local spawn on every instance)
    // NAV: feed the flow-field service (authority only) — obstacles = border walls + every
    // structure footprint (change-detected inside Nav), sources = per team its structures + player
    // bodies (every frame). Units read the fields inside the entity pass.
    void feedNav(float deltaSec);
    // SAVE/LOAD (F9/F10, server/single player only — clients refuse): structures/cables/units for
    // all teams to Assets/Local/gamesave.txt (players are NOT saved). Loading clears the current
    // set (removal hooks -> GRm prune connected clients) and re-broadcasts the loaded state.
    void saveGame();
    void loadGame(oc::string_view path = {}); // empty = the F10 path (Local/gamesave.txt)

    GamePlayer m_player;
    GameCamera m_camera;
    StructureSystem m_structures;
    NpcSystem m_npcs;

    EntityPtr m_ground; // the corridor border segments live under it as children
    oc::vector<Nav::NavObstacle> m_wallObstacles; // border collider footprints (static)
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
    uint32 m_cablePendingId = 0; // cable tool: first selected endpoint (stable id; 0 = none)
    EPlayerMode m_mode = EPlayerMode::Select; // Select IS the neutral mode (player combat removed)
    EBuildTool m_linkTool = EBuildTool::Connect; // which tool Link mode runs (root slot E, D or F)
    uint32 m_selectedId = 0;     // Select mode: highlighted structure (stable id; 0 = none)
    // UNIT SELECTION (RTS box drag in Select mode): owning handles to own-team units; RMB move
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
    bool m_wallPlacing = false;  // Wall two-click placement: first click anchored the line start
    glm::vec3 m_wallStart{ 0.0f };

    // TEAMS are SLOTS, never derived from the clientId: ids are minted monotonically and never
    // recycled (a reconnect or a failed first attempt burns one), so the second connection of the
    // same friend used to land on team 2 — a team with no Base. The map spawns one Base per
    // playable team (see spawnWorld), and a player without a Base has no respawn anchor, no
    // mineral bank and no healing, so the slot pool is exactly that many.
    static constexpr uint8 PlayableTeams = 2;
    uint8 allocateClientTeam() const;      // lowest free slot; extras double up on the last one
    int clientTeam(uint32 clientId) const; // -1 = unknown client (no capsule yet); 0 = the server
    // The team a client's build/demolish/cable request acts as. Callers must reject unknown
    // senders first (handleNetEvent does) — this clamps rather than failing.
    uint8 requestTeam(uint32 clientId) const { return (uint8)glm::max(clientTeam(clientId), 0); }
    glm::vec3 teamStartPos(uint8 team) const; // spawn/respawn anchor beside that team's Base

    bool m_enabled = false;
    uint32 m_team = 0;       // OUR team: 0 on server/single player; on a client it follows the
                             // team the SERVER assigned our capsule (read off its puppet
                             // GameUnitComponent, which the snapshot game blob carries)
    bool m_isServer = false; // co-op roles, latched in spawnWorld
    bool m_isClient = false;
    float m_statTimer = 0.0f; // server: GSt cadence
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
};
