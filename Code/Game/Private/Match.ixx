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
    explicit GameMatch(bool enabled);
    ~GameMatch();

    void spawnWorld();
    void update(float deltaSec);
    void updateWindowed(Camera& camera, float deltaSec);

    bool enabled() const { return m_enabled; }

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
    // B = Build (hotbar placement + cable tool), X = Delete, V = Select. Pressing the active
    // mode's key returns to neutral; the hotbar is visible only in Build mode (= mode indicator).
    enum class EPlayerMode : uint8 { None, Build, Delete, Select };

    Aim computeAim(const Camera& camera) const;
    bool aimGroundPoint(const Camera& camera, glm::vec3& outPos) const; // cursor ray vs colliders/ground plane
    void updateModeSwitching();
    void updateBuildMode(const Camera& camera, bool confirmEdge);
    void updateCableTool(const Camera& camera, bool confirmEdge, ECableType type);
    void updateDeleteMode(const Camera& camera, bool confirmEdge);
    void updateSelectMode(const Camera& camera, bool confirmEdge);
    int hoveredStructure(const Camera& camera) const; // structure index under the cursor, or -1
    void setMode(EPlayerMode mode);
    void buildWorldLabels(const Camera& camera); // health bars + selected info over structures
    void updateHud();
    void drawWorldFieldBoundary() const;

    GamePlayer m_player;
    GameCamera m_camera;
    StructureSystem m_structures;
    NpcSystem m_npcs;

    EntityPtr m_ground;

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
    bool m_modeKeyWasDown[3] = { false, false, false }; // B, X, V edges

    bool m_enabled = false;

    // Tweaks ("Game/World"). The ambient enemy field surrounds the player: zero within "Safe
    // radius" of the BASE, growing at "Field gradient" per metre beyond it — weakest at home,
    // strongest far out. The radius is a plain live tweak (no dynamics); local ground is won with
    // emitter bubbles, not by moving the frontier.
    glm::vec3 m_basePos{ 0.0f, 0.0f, 146.0f };     // game-start Base; its bubble covers the spawn
    glm::vec3 m_playerStart{ 0.0f, 1.0f, 140.0f }; // just in front of the Base = the respawn point
    float m_safeRadius = 30.0f;
    float m_fieldSlope = 0.04f;     // ambient strength per metre beyond the safe radius
    float m_fieldMaxStrength = 2.0f;
};
