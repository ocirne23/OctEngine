export module Game:Match;

import Core;
import Core.glm;
import Core.Camera;
import Entity;
import Force;
import Input;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// One entry of the RTS grid hotbar (Build mode): the top level shows the CATEGORIES (keys 1..3);
// picking one repopulates the hotbar with its items (keys 1..N arm the ghost/tool, key 0 = Back).
// Partition-scope so Match.cpp's category tables can be plain file statics.
struct BuildItem
{
    const char* label;
    bool isCable;
    EStructureType structure;
    ECableType cable;
};

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
    // pvp: PvP TEST mode — no ambient enemy field, no enemy waves/camps; each client plays on its
    // own Force team (server = 0, client N = min(N, GameMaxTeams-1)), everything a player builds
    // belongs to their team, and minerals/fuel are per-team.
    explicit GameMatch(bool enabled, bool pvp = false);
    ~GameMatch();

    void spawnWorld();
    void update(float deltaSec);
    void updateWindowed(Camera& camera, float deltaSec);

    bool enabled() const { return m_enabled; }

    // CO-OP (windowed listen server + clients). The SERVER runs the whole sim; player-structure
    // state mirrors to clients over game events (GPl/GRm/GCb + periodic GSt stats); units, shots
    // and camp structures replicate as network entities (Component Network in their prefabs);
    // each client drives its own server-spawned capsule through the claim system and computes its
    // own shield locally (the mirrored emitter fields exist client-side, so readbacks are real).
    // Client build/combat inputs travel as GqP/GqC/GqD/GqS/GqM requests, validated server-side.
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
    enum class EPlayerMode : uint8 { None, Build, Delete, Select };

    Aim computeAim(const Camera& camera, EStructureType type) const;
    bool aimGroundPoint(const Camera& camera, glm::vec3& outPos) const; // cursor ray vs colliders/ground plane
    void refreshBuildHotbar(); // repopulates slot labels/counts for the current grid level
    void updateModeSwitching();
    void updateBuildMode(const Camera& camera, bool confirmEdge);
    void updateCableTool(const Camera& camera, bool confirmEdge, ECableType type);
    void updateDeleteMode(const Camera& camera, bool confirmEdge);
    void updateSelectMode(const Camera& camera, bool confirmEdge);
    int hoveredStructure(const Camera& camera) const; // structure index under the cursor, or -1
    void setMode(EPlayerMode mode);
    void buildWorldLabels(const Camera& camera); // health bars + selected info over structures
    void updateHud();
    void handleNetEvent(std::string_view name); // NetworkManager::setOnGameEvent target
    void requestPlace(EStructureType type, const glm::vec3& pos, int nodeIndex, const glm::vec3& facing);
    void requestCable(uint32 idA, uint32 idB, ECableType type);
    void requestDemolish(uint32 id);
    void sendStructurePlaced(int index);
    void sendCableChanged(uint32 idA, uint32 idB, ECableType type, bool removed);
    void sendStats();
    void sendShieldStates(); // server: netId-keyed shield mirror ("GSh", ~10 Hz + collapse edges)
    void sendShieldReport(); // client: own locally computed shield -> server ("GqE")
    void serverMeleeFrom(const glm::vec3& pos, const glm::vec3& dirPlanar, uint8 team);
    void drawWorldFieldBoundary() const;
    void spawnCorridorWalls(); // PvP arena border (deterministic local spawn on every instance)

    GamePlayer m_player;
    GameCamera m_camera;
    StructureSystem m_structures;
    NpcSystem m_npcs;

    EntityPtr m_ground; // PvP: the corridor border segments live under it as children

    MouseListenerHandle m_mouse; // caches window-space mouse pos, wheel + RMB drag accumulation
    glm::vec2 m_mousePos{ 0.0f };
    float m_dragDeltaX = 0.0f; // RMB-held horizontal pixels this frame (consumed by the camera)
    float m_wheelAccum = 0.0f;
    bool m_rmbDown = false;
    bool m_placeClicked = false; // LMB edge inside the viewport (consumed by the active mode)
    bool m_placeKeyWasDown = false;
    uint32 m_cablePendingId = 0; // cable tool: first selected endpoint (stable id; 0 = none)
    EPlayerMode m_mode = EPlayerMode::None;
    uint32 m_selectedId = 0;     // Select mode: highlighted structure (stable id; 0 = none)
    bool m_modeKeyWasDown[5] = {}; // B, V, C (categories), X, Tab edges
    int m_buildCategory = -1;    // grid hotbar: -1 = category level, else index into the categories
    int m_buildSelection = -1;   // armed item within the category (-1 = nothing armed, no ghost)
    bool m_numKeyWasDown[10] = {}; // 1..9,0 edges (polled — the grid logic reads raw keys)
    bool m_lanceAiming = false;  // Lance two-click placement: first click anchored, awaiting facing
    glm::vec3 m_lancePendingPos{ 0.0f };
    bool m_wallPlacing = false;  // Wall two-click placement: first click anchored the line start
    glm::vec3 m_wallStart{ 0.0f };

    static uint32 teamOfClient(uint32 clientId) { return glm::min(clientId, uint32(GameMaxTeams - 1)); }
    // The team a client's build/demolish/cable request acts as: per-client in PvP, all team 0 in co-op.
    uint8 requestTeam(uint32 clientId) const { return m_pvp ? (uint8)teamOfClient(clientId) : 0; }

    bool m_enabled = false;
    bool m_pvp = false;      // see the constructor note
    uint32 m_team = 0;       // OUR team: 0 on server/single player; latched from the Welcome's
                             // clientId on a client (0 until welcomed)
    bool m_isServer = false; // co-op roles, latched in spawnWorld
    bool m_isClient = false;
    float m_statTimer = 0.0f; // server: GSt cadence
    std::unordered_map<uint32, EntityPtr> m_clientPlayers;    // server: clientId -> their capsule
    std::unordered_map<uint32, float> m_clientFireCooldown;   // server: combat request rate limits
    std::unordered_map<uint32, float> m_clientMeleeCooldown;
    std::unordered_map<uint32, float> m_clientMaterials;      // server: per-client carried materials
                                                              // (authoritative; mirrored via GSh)
    // Shield mirror ("GSh"): netId-keyed health/battery/output for actors this instance does NOT
    // simulate. A client holds every unit + other player (bubble output applied + overhead bars);
    // the server holds the client players, as last self-reported through "GqE" (each owner
    // computes its shield locally — its readbacks run there).
    struct RemoteShield
    {
        float healthFrac = 1.0f;
        float energyFrac = 1.0f;
        float output = 0.0f;
        bool collapsed = false;
        uint8 kind = 0; // ShieldNetState kinds: 0 enemy unit, 1 friendly unit, 2 player
        uint8 team = 0; // bars color own-team green / everything else red
    };
    std::unordered_map<uint32, RemoteShield> m_remoteShields;
    float m_shieldTimer = 0.0f; // server: GSh cadence (collapse edges flush immediately)
    float m_reportTimer = 0.0f; // client: GqE cadence (collapse edges flush immediately)

    // Tweaks ("Game/World"). The ambient enemy field surrounds the player: zero within "Safe
    // radius" of the BASE, growing at "Field gradient" per metre beyond it — weakest at home,
    // strongest far out. The radius is a plain live tweak (no dynamics); local ground is won with
    // emitter bubbles, not by moving the frontier.
    glm::vec3 m_basePos{ 0.0f, 0.0f, 0.0f };      // game-start Base at the MAP CENTER; the ambient
                                                  // enemy field surrounds it on all sides
    glm::vec3 m_playerStart{ 0.0f, 1.0f, -6.0f }; // just beside the Base = the respawn point
    glm::vec3 m_pvpEnemyBasePos{ 90.0f, 0.0f, 0.0f }; // PvP: team 1's Base; clients spawn beside it.
                                                      // PvP relocates m_basePos/m_playerStart to the
                                                      // corridor's other end (see the constructor)
    float m_safeRadius = 30.0f;
    float m_fieldSlope = 0.04f;     // ambient strength per metre beyond the safe radius
    float m_fieldMaxStrength = 2.0f;
    // Tweaks ("Game/Construction"): the player-inventory material loop.
    float m_refillRadius = 6.0f;    // metres from a Silo/Base within which the inventory refills
    float m_refillRate = 12.0f;     // materials/s pulled from the stores
    float m_buildRadius = 6.0f;     // metres from a blueprint within which a player invests
    float m_playerBuildRate = 8.0f; // materials/s a player invests into a blueprint
};
