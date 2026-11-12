export module Game:Match;

import Core;
import Core.glm;
import Core.Camera;
import Core.Rect;  // the labels job's captured viewport
import Core.GameHud; // the labels job's kept scratch (HudWorldLabel, HudPopup)
import Threading;  // JobCounter (the labels job)
import Entity;
import Force;
import Input;
import Nav;
import RendererVK; // ShadowParams (the game's shadow preset)
import Settings;
import :GameCamera;
import :Faction;
import :Player;
import :Structures;
import :Npc;

// The match orchestrator: owns the whitebox world (ground, objective, world-scale enemy emitter),
// the player, the structure/economy system and the follow camera. MUST be a stack local in main()
// (holds EntityPtrs and Force handles - a global would need an InitSeg slot).
//
// Two ticks, both main thread:
//  - update(dt): the AUTHORITY tick - placement drain, capture, grids/power, damage, player
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
    // CO-OP (`--game --coop`, clients pass both too - the world layout is built locally): its own
    // GENERATED world - no corridor, a much bigger square of seeded impassable rock terrain with
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
    // the authority. Writes the "Game/Coop" settings, so the panel shows what generated; seed 0
    // still rolls a random map.
    void setMapSettings(uint32 seed, float fill, int lanes)
    {
        m_settings.mapSeed = (int)(seed & 0x7fffffffu);
        m_settings.terrainFill = fill;
        m_settings.terrainLanes = lanes;
    }
    // PvP team setup chosen OUTSIDE (the lobby): the host's team count (2..GameMaxTeams - one
    // Base per team, spread along the corridor) and the roster's picks (clientId, team; clientId
    // 0 = the server's own team). Call before spawnWorld on the authority. A client whose id is
    // not in the list (a late joiner) is seated on the least-populated team at join.
    void setLobbyTeams(uint8 numTeams, oc::span<const oc::pair<uint32, uint8>> picks);
    // The PvP arena (the lobby's "Map" pick): call before spawnWorld on the authority; clients
    // build whatever the server's GMp event names.
    void setPvpMap(EPvpMap map) { m_pvpMap = map; }
    // The PLAYER/CAMERA hot path, and nothing else: capsule adoption (client) + velocity steering +
    // the shield's body push - the direct body setters that must land BEFORE this frame's physics
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
    // "Game/Player/Detach camera" tweak: main runs the testbed fly camera INSTEAD of the follow
    // camera and updateWindowed leaves the frame camera alone and takes no game input (the fly
    // camera owns LMB-look and WASD, which are the game's place/select and grid hotkeys).
    bool cameraDetached() const { return m_player.cameraDetached(); }

    // CO-OP IN THE SANDBOX WORLD ("Game/Coop/Sandbox world", read at construction): the procedural terrain, trees and
    // ocean instead of the generated rock map, the third-person player (WASD, mouse look, crosshair aim), the grid
    // hotkeys on the number keys, and placement on the terrain (flat enough, dry, no tree or rock). The App switches
    // the terrain and ocean on for it (Session::startWorldAndGame).
    bool sandboxWorld() const { return m_sandboxWorld; }
    // Where the player starts (the App passes the sandbox camera's start and direction, or (0, 0) for a lobby-seeded
    // world): call before spawnWorld. The player spawns on the ground under it, the camera looks along `dir`, and the
    // Base site search starts a little ahead along it. y = the camera height to wait at while the terrain streams in.
    void setWorldAnchor(const glm::vec3& anchor, const glm::vec3& dir)
    {
        m_worldAnchor = anchor;
        m_worldAnchorDir = dir;
    }
    // The App holds the window in RELATIVE mouse mode while this is true (the third-person mouse look).
    // Not in the BUILD UI (the cursor is free there, to place anywhere on the screen) - except while the middle
    // button is held, which turns the camera.
    bool wantsMouseCapture() const
    {
        return m_enabled && m_sandboxWorld && !m_player.cameraDetached() && (!m_buildUi || m_mmbDown);
    }

    bool enabled() const { return m_enabled; }
    // THE PHYSICS FOCUS (the sandbox world): where the terrain / tree / rock colliders must exist - every player's body
    // (ours and the client twins') and the Base - whatever the camera does ("Detach camera": it flies away, and the
    // ground must stay under the bodies). Writes at most maxPoints; 0 outside the sandbox world (the App uses the camera).
    int physicsFocus(glm::vec3* out, int maxPoints) const;
    // THE COARSE GROUND (the sandbox world): the App's terrain height at any distance, for places with no collider - a
    // wave's spawn site, and the units that walk in by the far tick (GameUnitParams::farGround: they follow it).
    void setGroundSource(float (*source)(float x, float z, float fallback))
    {
        m_structures.setGroundSource(source);
        Globals::settings.game.unitParams.farGround = m_sandboxWorld ? source : nullptr;
    }
    // THE HARVEST SURVEY (the Lumberyard and the Quarry; see StructureSystem's THE HARVEST). The source is the App's:
    // the trees and the rocks in a circle, from the world's tree records. Those are readable at ONE point of the frame
    // only (after the procedural updates, before the tree world's bookkeeping job is kicked), so the ghost never asks
    // by itself: it leaves its position in m_harvestProbe, and main.cpp calls surveyHarvest at that point. The answer
    // is on the ghost one frame later.
    void setHarvestSource(StructureSystem::HarvestSource source) { m_structures.setHarvestSource(source); }
    void surveyHarvest();

    // PROFILING SCENARIO (`--scenario <save>`, main calls it once at `--scenario-frame`): loads the
    // save (F10's path when empty), selects EVERY live own-team unit and orders them all to the
    // other team's Base - a repeatable crowd-pathing load without anyone at the keyboard.
    // Authority only (units simulate on the server). The load happens now; the select + order run
    // from the next update() (the loaded units' spatial entries link at the next commitFrame).
    bool runScenario(oc::string_view savePath);

    // MULTIPLAYER (windowed listen server + clients). The SERVER runs the whole sim; player-
    // structure state mirrors to clients over game events (GPl/GRm + periodic GSt stats; links
    // derive locally on every instance from the mirrored cable segments - no cable wire);
    // units and shots replicate as network entities (Component Network in their prefabs); each
    // client drives its own server-spawned capsule through the claim system and computes its own
    // shield locally (the mirrored emitter fields exist client-side, so readbacks are real).
    // Client build inputs travel as Gq* requests, validated server-side.
    void onClientJoined(uint32 clientId); // main.cpp routes the manager callbacks here (server)
    void onClientLeft(uint32 clientId);

    // Network game events (GPl/GSt/Gq*/...). main.cpp owns the ONE setOnGameEvent hook (it also
    // routes the lobby's "Lb*" traffic) and forwards everything else here - GameMatch must NOT
    // install its own hook: main's dispatcher constructs a lobby client's GameMatch from INSIDE a
    // dispatch, and replacing the oc::function there would destroy the lambda mid-execution.
    void handleNetEvent(oc::string_view name);

    // SHARED PAUSE (the escape menu's "Pause game" / the paused box's "Resume"): ANY player may
    // pause or resume. The authority applies it (Time::setPaused - the sim clock stops, the
    // transport keeps running so the resume still arrives) and broadcasts GPz; a client sends a
    // GqZ request. Join replay carries the current state.
    void requestPause(bool paused);
    bool isPaused() const { return m_paused; }

    // TRUE while Esc still has an in-game meaning (a pending two-click flow, an armed item, an
    // open hotbar page, or a non-Select mode - cancelOneLevel would consume it). main's ESCAPE
    // MENU only opens on an Esc press when this is false, so the cancel chain keeps first claim.
    void applyPause(bool paused); // sets the sim clock + m_paused (authority and mirror alike)
    bool m_paused = false;

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
        glm::vec3 pos{ 0.0f }; // the snapped GROUND point (y = the height the structure stands on)
        EStructureType type = EStructureType::Emitter;
        float yield = 1.0f; // Lumberyard / Quarry: the survey's yield at pos (m_harvestProbe); affordable only once it is known
        StructureSystem::EGroundFit ground = StructureSystem::EGroundFit::Ok; // the sandbox world's ground test
    };
    // Interaction modes: Select is the neutral mode (click inspects, RMB routes/moves); Q/W open
    // the build categories (Combat/Production), A/S/D arm a cable straight from the root page,
    // X = Delete. Connections derive from cable adjacency (the old two-click LINK tools are gone).
    enum class EPlayerMode : uint8 { Build, Delete, Select };

    Aim computeAim(const Camera& camera, EStructureType type) const;
    // A ROAD cell over water is a BRIDGE: the piece the armed type becomes at a snapped cell. The road's ghost and
    // its paint stroke go through it, so a road painted across a river bridges it by itself.
    EStructureType roadPieceAt(EStructureType armed, const glm::vec3& snapped) const;
    // The armed Lumberyard / Quarry ghost's survey (surveyHarvest): asked for `pos`, answered one frame later.
    struct HarvestProbe
    {
        EStructureType type = EStructureType::Count; // Count = nothing asked
        glm::vec3 pos{ 0.0f };
        float yield = 0.0f;
        bool wanted = false; // a ghost asked this frame
        bool known = false;  // yield is the answer for (type, pos)
    };
    HarvestProbe m_harvestProbe;
    int m_frameYield = -2; // this frame's harvest ghost: percent, -1 = surveying, -2 = no such ghost
    int m_shownYield = -2; // what the HUD's "Yield" line shows
    glm::vec3 m_yieldGhostPos{ 0.0f }; // where that ghost stands: the labels pass writes the same yield over it
    void showYield(int percent);
    // The sandbox world's ground test on a snapped position (always true on the flat maps): p.y = the stand height.
    bool fitGround(EStructureType type, glm::vec3& p, const glm::quat& rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) const;
    StructureSystem::EGroundFit m_frameGroundFit = StructureSystem::EGroundFit::Ok; // this frame's refused ghost
    StructureSystem::EGroundFit m_shownGroundFit = StructureSystem::EGroundFit::Ok; // what the HUD line shows
    bool aimGroundPoint(const Camera& camera, glm::vec3& outPos) const; // cursor ray vs colliders/ground plane
    void refreshBuildHotbar(); // repopulates slot labels/counts/hover cards for the current grid level
    // The hotbar hover cards, one per structure type: rebuilt once a REAL second (the tweaks they
    // quote are live, but nobody drags one 60 times a second) - see refreshBuildHotbar.
    oc::string m_typeCards[(int)EStructureType::Count];
    double m_typeCardTime = -1.0; // real-clock stamp of the last rebuild (< 0 = never built)
    void updateModeSwitching();
    // The windowed tick's INPUT half: grid hotkeys, hotbar/popup clicks, the active mode's
    // clicks and the RMB move order. Skipped entirely while the camera is detached.
    void updateGameInput(const Camera& camera);
    void updateBuildMode(const Camera& camera, bool confirmEdge, bool cancelEdge);
    void disarmBuild(); // drop the armed item + any half-finished two-click flow (RMB / Esc)
    void activateSlot(int slot); // grid hotkey OR click on the drawn slot: category / item / Delete / Cancel / Back
    void cancelOneLevel();       // Esc, Tab: two-click step -> armed item -> page/mode, one per press
    // Cable segments place by PAINTING: LMB press starts a stroke, holding + dragging places the
    // cells the cursor crosses (L-filled between samples so the run stays connected). The newest
    // cell is HELD BACK one step: a plain cable of another medium ahead turns it into the near end
    // of a Crossing auto-placed over that cable along the stroke (see placeCableLine).
    void updateCablePlacement(const Camera& camera, EStructureType armed, bool confirmEdge);
    void placeCableLine(EStructureType armed, const glm::vec3& from, const glm::vec3& to);
    void finishCableStroke(EStructureType armed); // release / RMB cancel: lands the held cell, ends the stroke
    void updateDeleteMode(const Camera& camera, bool confirmEdge);
    void updateSelectMode(const Camera& camera, bool confirmEdge, bool rmbEdge);
    // Shared click-to-select (Select mode, and Build mode wherever the click can't place).
    void updateSelectionClick(const Camera& camera, bool confirmEdge, bool allowPick);
    // Shared RMB handling (both modes): barracks route on ground (linking is the CONN tool now).
    void updateRightClickActions(const Camera& camera, bool rmbEdge);
    int hoveredStructure(const Camera& camera) const; // structure index under the cursor, or -1
    EStructureType armedType() const; // the type on the build ghost; Count = nothing armed
    uint32 m_labelsHoverId = 0; // the structure under the aim this frame (0 = none): only it and the selected one show their name
    void setMode(EPlayerMode mode);
    // Health bars + selected info over structures/units: a JOB (submitWorldLabels at the end of the
    // game tick, joined by main before the widget pass is queued - see the definition).
    void buildWorldLabels();
    void submitWorldLabels(float deltaSec);
    float m_labelsDelta = 0.0f; // the frame delta the job ages the warning timers by
    // The problem badge over an own-team structure (nullptr = fine) - see the definition.
    const char* structureWarning(int index, glm::vec3& color) const;
    Camera m_labelsCamera;
    Rect m_labelsViewport;
    bool m_labelsCameraValid = false;
    JobCounter m_labelsCounter;
    oc::vector<Entity*> m_labelUnits; // the labels job's visible-unit scratch (one job in flight; never a thread_local - the job may park)
    oc::vector<HudWorldLabel> m_labelsScratch; // the labels job builds here, then SWAPS with GameHud's list (two vectors ping-pong)
    HudPopup m_labelsPopup;                    // same for the barracks popup (its buttons vector keeps its capacity)
public:
    void joinWorldLabels(); // main.cpp, right before ui.update
private:
    void updateHud();
    void tickBaseHealing(float deltaSec); // own player only - the owner computes its own health
    void tickMedicHealing(float deltaSec); // medic stations: the own player (every instance); units heal in the station's component update
    void tickAmbientWander(float deltaSec); // co-op POST-UPDATE job: idle AI units stroll now and then, biased toward the Base (a steady per-frame budget)
    float m_wanderBudget = 0.0f;           // fractional strolls carried to the next frame
    float m_wanderSelectedFrac = 0.05f;    // smoothed share of the roster inside the SIM LOD selection (from the probes)
    std::mt19937 m_wanderRng{ 0x5eedu };   // the job's own generator (never the shared C rand from a worker)
    void tickPlayerMelee(float deltaSec); // AUTHORITY: every player capsule grinds adjacent enemy units
    void requestPlace(EStructureType type, const glm::vec3& pos, float yield, const glm::vec3& facing);
    void requestDemolish(uint32 id);
    void requestSetRoute(uint32 id, oc::span<const glm::vec3> points); // barracks waypoints
    void sendRoute(int index); // server: GRt broadcast (mirror + join replay)
    void requestSetUnitType(uint32 id, uint8 unitType); // barracks: the produced unit type (popup)
    void sendUnitType(int index); // server: GBu broadcast (mirror + join replay)
    void sendStructurePlaced(int index);
    void sendStats();
    // NAV: feed the flow-field service (authority only) - obstacles = rock terrain + every
    // structure footprint (change-detected inside Nav), sources = per team its structures + player
    // bodies (every frame). Units read the fields inside the entity pass. The whole feed - gather
    // AND the Nav setters - is a post-update job (submitted by update, joined at the top of the
    // next frame, ahead of that frame's NavSystem::update).
    void submitNavFeed(float deltaSec); // queues gatherNavFeed on the post-update batch
    void gatherNavFeed(float deltaSec); // one SLICE of the unit sweep a frame (a cycle = Nav's rebuild interval); publishes at the cycle end
    // SAVE/LOAD (F9/F10, server/single player only - clients refuse): structures/cables/units for
    // all teams to Assets/Local/gamesave.txt, plus the LOCAL player's position (`PlayerPos`; remote
    // players and all player STATE - health, energy, materials - are still not saved). Loading
    // clears the current set (removal hooks -> GRm prune connected clients), re-broadcasts the
    // loaded state and teleports the local capsule.
    void saveGame();
    void loadGame(oc::string_view path = {}); // empty = the F10 path (Local/gamesave.txt)
    // The co-op TRICKLE state (`WaveTrickle` / `AmbientTrickle`): points still queued to spawn plus
    // what the trickle needs to spawn them - so an F9 during a wave does not shrink it. loadTrickle
    // must run AFTER rebuildCoopMap, which voids the in-progress ambient group.
    void saveTrickle(AssetNode& root) const;
    void loadTrickle(const AssetNode& root);

    // The tuning: the co-op director, the material loop, base healing, melee, the nav feed and the
    // labels ("Game/Coop", "Game/Construction", ...). Global: the values persist across matches.
    GameMatchSettings& m_settings = Globals::settings.game.match;
    GamePlayer m_player;
    GameCamera m_camera;
    ShadowParams m_sandboxShadowParams; // the renderer's "Shadows" values before our preset (restored in ~GameMatch)
    glm::vec3 m_savedTeamColors[2]{};   // the "Force/Teams" colours before the sandbox world's palette (restored in ~GameMatch)
    float m_savedBakeHeight = Globals::settings.forceSystem.bakeSampleHeight; // "Force/Bake" "Sample height" before the Base's (tickBeacon)
    StructureSystem m_structures;
    NpcSystem m_npcs;

    EntityPtr m_ground;
    oc::vector<Nav::NavObstacle> m_wallObstacles; // rock terrain rects (static, both modes)
    oc::vector<Nav::NavObstacle> m_navObstacles;  // per-frame scratch: walls + structures
    oc::vector<Nav::NavSource> m_navSources[Nav::MaxTeams];
    // Unit sources are culled to units with ANOTHER team's unit/player within "Nav unit source
    // reach" (coarse: a cell hash of that size, 3x3 neighbourhood) - see gatherNavFeed.
    // Friendly unit CLUSTERS as extra SIM LOD focus points (see the focus block in update): a
    // non-AI unit farther than "Unit cluster focus radius" from every focus seeds a new cluster;
    // refreshed every 0.25 s.
    float m_focusClusterTimer = 0.0f;
    oc::vector<glm::vec3> m_focusClusters;
    // The shield structures' bubble spheres as SIM LOD ZONES (tier 1 + a tier 2 band, no tier 0):
    // an enemy walking into a far base's field ticks - and gets pushed - with no player near.
    // Refreshed on the cluster timer.
    oc::vector<glm::vec4> m_fieldZones;
    // Team bits per nav cell, as a FLAT open-addressing map: the hash map it replaced freed every
    // node on clear() and re-allocated them all the next cycle - most of the nav feed's memory churn.
    // clear() only resets the slots; the table keeps its size across cycles.
    struct NavCellTeamMap
    {
        struct Entry { uint64 key = 0; uint8 teams = 0; bool used = false; };
        oc::vector<Entry> table;
        uint32 count = 0;

        static uint32 slotOf(uint64 key, size_t size) // splitmix64 finalizer, masked (size is a power of two)
        {
            key ^= key >> 30; key *= 0xbf58'476d'1ce4'e5b9ull;
            key ^= key >> 27; key *= 0x94d0'49bb'1331'11ebull;
            return uint32(key ^ (key >> 31)) & uint32(size - 1);
        }
        uint8 find(uint64 key) const
        {
            if (table.empty())
                return 0;
            for (uint32 s = slotOf(key, table.size());; s = (s + 1) & uint32(table.size() - 1))
            {
                if (!table[s].used)
                    return 0;
                if (table[s].key == key)
                    return table[s].teams;
            }
        }
        void orTeams(uint64 key, uint8 bits)
        {
            if ((count + 1) * 2 > table.size())
                grow();
            for (uint32 s = slotOf(key, table.size());; s = (s + 1) & uint32(table.size() - 1))
            {
                Entry& e = table[s];
                if (!e.used)
                {
                    e = Entry{ key, bits, true };
                    ++count;
                    return;
                }
                if (e.key == key)
                {
                    e.teams |= bits;
                    return;
                }
            }
        }
        void grow()
        {
            oc::vector<Entry> old = oc::move(table);
            table.assign(glm::max<size_t>(old.size() * 2, 256), Entry{});
            count = 0;
            for (const Entry& e : old)
                if (e.used)
                    orTeams(e.key, e.teams);
        }
        void clear()
        {
            for (Entry& e : table)
                e = Entry{};
            count = 0;
        }
        void swap(NavCellTeamMap& other) { table.swap(other.table); oc::swap(count, other.count); }
    };
    NavCellTeamMap m_navCellTeams;     // the map the current cycle culls against (built by the previous cycle)
    NavCellTeamMap m_navCellTeamsNext; // being built by the current cycle's slices
    oc::vector<Nav::NavSource> m_navUnitSources[Nav::MaxTeams]; // the current cycle's accepted unit sources
    uint32 m_navFeedCursor = 0; // World root index the next slice starts at (0 = a cycle just published)
    // Lane seeding parameters live on NpcSystem (one set of tweaks); Match reads them for its own
    // route/order seeding.
    float laneSeedSpeed() const { return m_npcs.orderLaneSpeed(); }
    float laneSeedWidth() const { return m_npcs.laneWidth(); }

    MouseListenerHandle m_mouse; // caches window-space mouse pos, wheel + RMB drag accumulation
    glm::vec2 m_mousePos{ 0.0f }; // the sandbox world: always the viewport center (crosshair aim)
    glm::vec2 m_lookDelta{ 0.0f }; // the sandbox world: relative mouse motion since the last windowed frame
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
    bool m_boxSelectBlocked = false; // the held LMB press changed the mode (setMode): updateUnitSelection ignores it to its release
    void updateUnitSelection(const Camera& camera);
    bool orderSelectedUnits(const glm::vec3& target, bool freshOrder); // true = a lane plan was queued (fresh order + a raster to plan over; the A* runs on a job)
    bool moveOrderAt(const glm::vec3& worldPos, bool includePlayer = true); // THE RMB move order: player (unless CTRL: units only) + selected units walk there (fresh order, lane seeded); returns orderSelectedUnits'
    glm::vec3 pointOutsideFootprint(const glm::vec3& clicked, int structure) const; // a click on a building -> the reachable point at its face
    bool m_scenarioOrderPending = false; // runScenario loaded; issueScenarioOrder retries each update until units are queryable
    uint32 m_scenarioOrderTries = 0;
    void issueScenarioOrder();           // select ALL own units + move order on the other Base
    void seedRouteLane(uint32 structureId); // barracks route -> a planned lane in the flow field
    // (While an ordered group walks, its lane is kept fresh by the UNITS' own periodic plan
    // requests - Nav's proximity dedup makes the whole group cost one plan. See
    // GameUnitComponent's SeedRequest and NavSystem::requestSeedPath.)
    void pruneSelectedUnits();
    bool m_modeKeyWasDown[1] = {}; // Esc/Tab edge
    // THE SANDBOX WORLD'S BUILD UI. Two states: PLAY (the mouse looks, the crosshair aims, no bar) and the BUILD UI
    // (m_buildUi: the cursor is FREE, every ray leaves through it, the middle button held turns the camera, and the
    // build bar shows: three page tabs and the page's items, all clickable, with hover cards). Tab opens and closes
    // it; Q / E arm the previous / next item; X is Delete mode; the number row arms an item; each of them opens it.
    // Esc steps back (the two-click step, the armed item, the bar). The bar closes when the mode is back at Select.
    bool m_buildUi = false;
    bool m_crosshairShown = false;
    void armPageItem(int category, int selection);
    void toggleBuildUi();
    void stepSelection(int direction);
    void refreshBuildBar();        // the sandbox world's hotbar (refreshBuildHotbar's other half)
    int buildBarPage() const;      // the page the bar shows: the armed one, else the last used
    bool m_buildKeyWasDown[4] = {}; // Tab, Q, E, X edges
    bool m_flyKeyWasDown = false;   // F3 edge (the fly camera)
    int m_pageSelection[3] = {};    // the last armed item of each page
    int m_lastPage = 0;
    bool m_saveKeyWasDown = false; // F9/F10 edges (save/load game state)
    bool m_loadKeyWasDown = false;
    int m_buildCategory = -1;    // grid hotbar page: -1 = ROOT (categories), else index into the categories
                                 // (c_cableCategory = a cable armed FROM the root page - still drawn as root)
    bool isRootPage() const;     // the hotbar shows the root layout (categories, cables, Delete)
    int m_buildSelection = -1;   // armed item within the category (-1 = nothing armed, no ghost)
    bool m_gridKeyWasDown[12] = {}; // QWER/ASDF/ZXCV edges (polled - one per hotbar slot)
    bool m_lanceAiming = false;  // Lance two-click placement: first click anchored, awaiting facing
    glm::vec3 m_lancePendingPos{ 0.0f };
    bool m_wallPlacing = false;  // Wall drag placement: the press anchored the line start, release places
    glm::vec3 m_wallStart{ 0.0f };
    bool m_rmbUnitsOnly = false;   // CTRL was held on the RMB press: the move order skips the player
    bool m_cablePainting = false;  // LMB held: paint cells as the cursor crosses them
    glm::vec3 m_cablePaintLast{ 0.0f }; // the last cell the stroke reached (the next L-fill starts here)
    bool m_cablePendingValid = false;   // m_cablePending = the stroke's newest cell, held back one step
    glm::vec3 m_cablePending{ 0.0f };

    // TEAMS are SLOTS, never derived from the clientId: ids are minted monotonically and never
    // recycled (a reconnect or a failed first attempt burns one), so the second connection of the
    // same friend used to land on team 2 - a team with no Base. The map spawns one Base per
    // playable team (m_numTeams, see spawnWorld), and a player without a Base has no respawn
    // anchor, no mineral bank and no healing, so the slot pool is exactly that many.
    uint8 m_numTeams = 2;                                  // PvP playable teams (the lobby's count)
    oc::vector<oc::pair<uint32, uint8>> m_lobbyTeams;      // the lobby's picks (see setLobbyTeams)
    glm::vec3 baseGroundPos(uint8 team) const;             // PvP: that team's Base cell on the corridor
    uint8 allocateClientTeam(uint32 clientId) const;       // the lobby pick, else the least-populated team
    int clientTeam(uint32 clientId) const; // -1 = unknown client (no capsule yet); 0 = the server
    // The team a client's build/demolish/cable request acts as. Callers must reject unknown
    // senders first (handleNetEvent does) - this clamps rather than failing.
    uint8 requestTeam(uint32 clientId) const { return (uint8)glm::max(clientTeam(clientId), 0); }
    glm::vec3 teamStartPos(uint8 team) const; // spawn/respawn anchor beside that team's Base

    // ---- CO-OP (see the constructor comment) ----
    // The ambient/wave team. Team 1, NOT a high slot: co-op runs the Force system at TWO live
    // teams (setNumTeams(2) - the renderer's per-team costs shrink to fit), so every team index
    // must stay below 2. (< Nav::MaxTeams as well.)
    static constexpr uint8 CoopAiTeam = 1;
    // GENERATED MAP: impassable rock over a coarse cell grid (seeded noise + carved attack lanes),
    // a player-blocking barrier ring at the edge, and everything placed on it (resource nodes,
    // ambient spawns) guaranteed reachable from the Base by a flood fill that turns unreachable
    // open pockets into rock. The SERVER picks the seed (tweak "Map seed", 0 = random) and
    // broadcasts it as the "GMp" event (first thing in onClientJoined, before the structure
    // replay; also into F9 saves) - clients defer terrain + nodes until it arrives and build the
    // identical set locally from pure seeded math.
    // THE TERRAIN GRID serves BOTH modes: the co-op generator fills a 36² grid of 10 m cells, the
    // PvP arenas (generatePvpGrid) a per-arena rectangle of 5 m cells (rock.pre spawned at half
    // scale) - everything downstream (cell math, flood fill, rects, rock spawn, clampToOpenGround)
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
    // archetype, a disc around a reachable anchor) before rolling the next - loose blobs, not a
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
    void tickWaves(float deltaSec);  // authority: the wave clock
    void queueWave();                // pick a compass direction, size the swarm, seed its lane
    void tickCoopSpawns();           // trickle: wave + ambient spawns on a per-frame budget
    // COMPOSITION recipes, sampled per spawned unit. The wave's is rolled once per wave in
    // queueWave: an ARCHETYPE (single-type rush, screened siege, combined arms, ... - gated by the
    // wave index so early waves stay simple) with its weights jittered. The AMBIENT scatter picks
    // its spawn position FIRST and gates the archetype roll by DISTANCE from the Base - the same
    // minWave gate, driven by depth instead of time, so the deep map holds the heavy recipes and
    // the near ring stays swarm-grade. Authority-only (clients never spawn).
    struct WaveMixEntry { ENpcType type; float weight; };
    ENpcType sampleMix(const oc::fixed_vector<WaveMixEntry, 4>& mix) const; // weighted type roll
    oc::fixed_vector<WaveMixEntry, 4> m_waveMix;
    int m_lastArchetype = -1;    // never the same recipe twice in a row (when a choice exists)
    bool m_coop = false;
    float m_matchTime = 0.0f;    // authority sim seconds since the world spawned: the HUD "Time" clock, saved/restored
    float m_waveTimer = 0.0f;    // seconds to the next wave (armed in spawnWorld)
    int m_waveIndex = 0;         // waves launched so far
    // THE LIGHT RULES (the sandbox world): the waves come at night only - the first at dusk, then one every "Night wave
    // interval (s)" until dawn - sized and gated by the NIGHT number; daylight burns the AI team and scales the solar yield.
    bool m_night = false;        // the sun is down (the wave clock's edge detector), saved
    bool m_timeOfDayEdited = false; // the "Time of day (h)" tweak was set by hand since the last tickWaves (a Tweak::onChange listener): no night is counted for it
    int m_nightIndex = 0;        // nights started so far, saved
    int m_nightWave = 0;         // waves launched this night (the night's build-up)
    float m_waveScale = 1.0f;    // x the NEXT wave's budget, rolled ahead (tickWaves: the night's build-up, variation and surges)
    int waveLevel() const;       // what sizes a wave and gates its recipe, 0-based: the night (sandbox world) or the wave
    void stampLightRules();      // authority, per frame: the runtime unit / structure params the light rules own
    float daylight() const;      // 0 = night .. 1 = full day (a smooth step over the sun's height); 1 outside the sandbox world
    // THE BEACON (the sandbox world, every instance): the Base's power drives the cloud hole - the clouds' VORTEX, centred
    // on the Base ("Sky/Clouds/Vortex" keeps its shape tweaks) - the night's cloud coverage (the Shroud) and the beam.
    void tickBeacon(float deltaSec);
    void restoreBeacon();        // the cloud tweaks back to their values before the beacon took them
    // THE FOG EFFECTS (the sandbox world, every instance; per-frame fog volumes). A wave's spawn site holds a dark,
    // faintly glowing fog bank and a column up toward the clouds for "Spawn fog time (s)" (clients get the site with
    // GWv). Then the fields CLEAR the fog inside them: one clearing volume per field zone, added LAST.
    struct SpawnFog { glm::vec3 pos{ 0.0f }; float radius = 0.0f; float age = -1.0f; }; // age < 0 = a free slot
    static constexpr int c_spawnFogSites = 4;
    SpawnFog m_spawnFog[c_spawnFogSites];
    int m_spawnFogNext = 0;
    void addSpawnFog(const glm::vec3& pos, float radius);
    void tickFogEffects(float deltaSec);
    // THE FACTION LAYER (tickFaction, every instance off its own structure mirror; the sandbox world): whose land each
    // cell is. Its sources: the Base and the field bubbles (the players), a wave's spawn site while its fog stands (the enemy).
    FactionLayer m_faction;
    oc::vector<FactionLayer::Source> m_factionSources; // kept: refilled every frame
    void tickFaction(float deltaSec);
    // THE ENEMY OUTPOSTS (tickOutposts, every instance; the sandbox world): the ring of dark beacons around the Base.
    // The positions exist from the moment the Base does (the beam and the faction source need no more); the BUILDING
    // (enemyOutpost.pre) is spawned when the terrain collider reaches its site - only then has the site a ground height.
    struct EnemyOutpost { glm::vec3 pos{ 0.0f }; EntityPtr entity; };
    oc::vector<EnemyOutpost> m_outposts;
    float m_outpostGroundTimer = 0.0f; // s to the next ground probe of the outposts that have no building yet
    void tickOutposts(float deltaSec, const glm::vec3* basePos);
    // LIGHTNING (tickLightning, every instance, a look only): flashes in the clouds (the clouds' two flash slots), slot 0
    // may carry an arc to the ground. age < 0 = a free slot.
    struct Lightning { glm::vec3 pos{}, ground{}; float age = -1.0f, life = 0.0f; bool strike = false, fork = false; uint32 seed = 0; };
    Lightning m_lightning[2];
    float m_lightningTimer = 0.0f; // s to the next flash
    void tickLightning(float deltaSec);
    bool m_beaconSet = false;
    float m_beaconPower = 0.0f;  // smoothed 0..1: the Base's field output x its charge
    float m_beaconNightSec = 0.0f; // seconds of full night so far (0 by day)
    bool m_beaconLit = false;      // on from the ignite delay at dusk until the shroud is half open at dawn
    float m_beaconGlow = 0.0f;    // 0..1: the short beam on the Base brightens, before the climb
    float m_beaconClimb = 0.0f;   // 0..1: the beam's tip on its way from the Base to the cloud base
    float m_beaconIgnite = 0.0f;   // 0..1: the pierce, after the climb (the hole opens, the beam runs to its full height)
    struct BeaconSaved { bool vortexEnabled; float centreXKm, centreZKm, strength, spin, coverage, coverageVariation, macroVariation, eyeScale, lightIntensity; glm::vec3 lightColor; } m_beaconSaved{};
    float m_wavePendingBudget = 0.0f;    // POINTS of the current wave still to spawn (trickled):
                                         // each spawned unit spends its type's cost (waveCostOf)
    float m_ambientPendingBudget = 0.0f; // POINTS of world-start scatter still to spawn (same costs)
    glm::vec3 m_waveOrigin{ 0.0f }; // the wave's cluster center on the spawn ring
    // The blob's radius, sized ONCE per wave in queueWave so the AREA scales with the wave's
    // expected BODY COUNT (see waveSpawnRadius) - a big wave gets room instead of stacking bodies
    // on the same disc for physics to shove apart. Rides the save: a mid-wave load keeps the disc.
    float m_waveRadius = 8.0f;
    float waveSpawnRadius(float budget) const;
    // Spacing: the last few wave spawn points, so a new roll can reject a spot inside a body that
    // was just placed (bodies are parked at spawn - an overlap there resolves only when a player
    // comes near, as a push-out in the player's face)
    static constexpr int c_waveRecentSpawns = 32;
    glm::vec2 m_waveRecent[c_waveRecentSpawns]{};
    int m_waveRecentCount = 0, m_waveRecentNext = 0;
    glm::vec3 m_waveDest{ 0.0f };   // the Base's near face on the incoming side
    // Waves are sized in BUDGET POINTS ("Game/Coop" settings), each type at its own cost.
    float nextWaveBudget() const;     // the coming wave's points before the alive cap (queueWave + the HUD's "Next wave power")
    float nightAverageScale() const;  // the sandbox world: a night's average wave power over its base (the next night's base grows by it)
    float waveCostOf(ENpcType t) const { return glm::max(m_settings.waveCost[(int)t], 0.1f); }
    // Live units (every team): the alive cap in queueWave AND the HUD's "Enemies alive" -
    // GameUnitComponent's own live count (spawn/destroy edges), O(1), no roster and no walk.
    int aiAliveCount() const;

    // ---- THE SANDBOX WORLD (see sandboxWorld()) ----
    bool m_sandboxWorld = false;
    glm::vec3 m_worldAnchor{ 0.0f, 20.0f, 0.0f };    // setWorldAnchor: the player's start
    glm::vec3 m_worldAnchorDir{ 0.0f, 0.0f, -1.0f }; // ... and the start view direction
    // The AUTHORITY places the Base once the terrain collider has streamed in under the anchor: the search waits
    // until the ground has been there for a moment (the tree and rock colliders follow the terrain), then takes the
    // nearest site ahead of the anchor where the Base passes the ground test, spawns the player on the anchor and
    // scatters the nodes.
    bool m_siteFound = false;
    float m_siteGroundTimer = 0.0f;
    float m_siteWaitTimer = 0.0f;
    void tickBaseSite(float deltaSec);
    void spawnLocalPlayer(); // the authority's own capsule at m_playerStart (+ the server-primary flag)
    // The ground for something that only STANDS there (a spawn, a fog bank): the collider's exact height, else the coarse
    // terrain height, else the fallback.
    float groundY(float x, float z, float fallback) const { return m_structures.groundHeightAt(x, z, fallback, true); }
    // The "can't build here" HUD line, kept to the frames with a refused ghost.
    void showGroundReason(StructureSystem::EGroundFit fit);

    bool m_enabled = false;
    uint32 m_team = 0;       // OUR team: 0 on server/single player; on a client it follows the
                             // team the SERVER assigned our capsule (read off its puppet
                             // GameUnitComponent, which the snapshot game blob carries)
    bool m_isServer = false; // co-op roles, latched in spawnWorld
    bool m_isClient = false;
    float m_statTimer = 0.0f; // server: GSt cadence
    int m_cableSyncCursor = 0; // server: GCb rotation start when more blueprint segments exist than fit one event
    uint32 m_cableFillCursor = 0; // server: GCf rotation start over the transport's cable nodes
    oc::unordered_map<uint32, EntityPtr> m_clientPlayers;    // server: clientId -> their capsule
                                                              // (the ONLY per-client store - carried
                                                              // materials live on the twin's puppet
                                                              // component, damage owed in its inbox)
    float m_damageTimer = 0.0f; // server: GDm flush cadence

    // Corridor anchors (see the constructor): one Base at each end, clients spawn beside theirs.
    glm::vec3 m_basePos{ -55.0f, 0.0f, 0.0f };        // team 0's Base
    glm::vec3 m_playerStart{ -55.0f, 1.0f, -6.0f };   // just beside it = the respawn point
    glm::vec3 m_enemyBasePos{ 55.0f, 0.0f, 0.0f };    // team 1's Base; clients spawn beside it
};

// ---- Shared by the Match*.cpp implementation units (module linkage, NOT exported) ----
// GameMatch's bodies are split by topic:
//   Match.cpp        construction, spawnWorld, the three frame entry points, the nav feed, the
//                    player ticks (melee, healing), the HUD, the sandbox world's Base site + nodes
//   MatchMap.cpp     the generated terrain: the co-op grid, the PvP arenas, rocks/nodes, the
//                    per-team Base anchors
//   MatchCoop.cpp    the co-op director: waves, the spawn trickle, the ambient wander, the trickle
//                    save state
//   MatchNet.cpp     teams, the join replay, every game event (send + handle), the requests
//   MatchSave.cpp    F9/F10 save/load + the profiling scenario
//   MatchInput.cpp   the hotbar, the interaction modes, placement, selection, orders
//   MatchLabels.cpp  the world-label job
// Everything more than one of them reads is declared here.
inline constexpr float c_corridorHalfLength = 65.0f; // Lane/Wide/Chokepoints x extent of the play area
inline constexpr float c_corridorHalfWidth = 20.0f;  // Lane z extent
// The CO-OP map: a big square centred on the shared Base - impassable rock terrain over a coarse
// cell grid plus a player-blocking barrier ring at ±c_coopHalfSize (see MatchMap.cpp). The ground
// plane (ground.pre) is 600 m, so ±300 is the hard edge; waves spawn in the open ring BETWEEN the
// barrier and the ground edge and walk in through it (the barrier's collider only matches the
// Player layer).
inline constexpr float c_coopHalfSize = 270.0f;
inline constexpr float c_coopGroundEdge = 296.0f;    // spawn clamp just inside the 600 m ground
// The 12 hotbar slots' key captions: QWER / ASDF / ZXCV, row-major (see MatchInput.cpp).
inline constexpr oc::string_view c_gridKeyLabels[12] = { "Q", "W", "E", "R", "A", "S", "D", "F", "Z", "X", "C", "V" };
// The same slots on the number row (the sandbox world: WASD walk the third-person player).
inline constexpr oc::string_view c_gridNumberKeyLabels[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
// The sandbox world's BUILD BAR (one row): three page tabs (no key), then nine item slots on the number row.
inline constexpr int c_buildBarTabs = 3;
inline constexpr oc::string_view c_buildBarKeyLabels[12] = { "", "", "", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
// 3-5 char shorthands, indexed by EStructureType - the SAME vocabulary the hotbar slot and the
// world tag over a building use, so a slot and the thing it builds read identically.
inline constexpr const char* c_structureShortNames[] = { "LNTN", "HRTH", "LMBR", "CRYS",
    "WOOD", "MIRR", "STNW", "GLNT", "LNC", "BRK", "WALL", "TOWR", "STOR",
    "BUIL", "BEACN", "LIGHT", "TR-W", "ROAD", "CRS-L", "CRS-W", "CRS-R", "HOUS", "HEAL", "QRRY", "BRDG", "GATE", "LENS", "SRCH" };
static_assert(oc::size(c_structureShortNames) == (size_t)EStructureType::Count);
// The barracks' unit-type popup captions, in ENpcType order (the same order the price tables use).
inline constexpr const char* c_unitTypeNames[] = { "Grunt", "Brute", "Runner", "Spitter", "Swarm",
    "Elite", "Giant", "Titan", "Lobber", "Spawner", "Warrior" };
static_assert(oc::size(c_unitTypeNames) == (size_t)ENpcType::Count);
static_assert((int)ENpcType::Count == GameUnitTypeCount); // GameMatchSettings::waveCost
// The popup's buttons, in order: the producible types (isBarracksUnitType - no Spitter).
inline constexpr uint8 c_barracksMenu[] = { (uint8)ENpcType::Grunt, (uint8)ENpcType::Warrior,
    (uint8)ENpcType::Brute, (uint8)ENpcType::Runner, (uint8)ENpcType::Swarm };
// (packColor / drawCircle come from Structures.ixx.)
// GHOST: the exact box the structure will occupy - footprint square × the prefab's height, drawn
// as a wireframe at the snapped position (every whitebox building IS a box, so this is the real
// shape, not an approximation), plus the interior cell lines so the grid it takes is unambiguous.
void drawStructureGhost(EStructureType type, const glm::vec3& groundPos, uint32 color);
